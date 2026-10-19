#define FUSE_USE_VERSION 35
#define _GNU_SOURCE
#include <fuse3/fuse.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static char roots[2][PATH_MAX];
/* Namespace operations are serialized; open file descriptors can read in parallel. */
static pthread_rwlock_t namespace_lock = PTHREAD_RWLOCK_INITIALIZER;

static bool within(const char *path, const char *directory) {
    size_t n = strlen(directory);
    return !strncmp(path, directory, n) &&
           (directory[n - 1] == '/' || path[n] == '/' || path[n] == '\0');
}

static int backing(int disk, const char *path, char out[PATH_MAX]) {
    if (path[0] != '/' || strstr(path, "/../") ||
        strcmp(path, "/..") == 0 || strstr(path, "/./") ||
        strcmp(path, "/.") == 0) return -EINVAL;
    if (snprintf(out, PATH_MAX, "%s%s", roots[disk], path) >= PATH_MAX)
        return -ENAMETOOLONG;
    return 0;
}

static int locate(const char *path, char out[PATH_MAX], struct stat *st) {
    for (int d = 0; d < 2; d++) {
        int rc = backing(d, path, out);
        if (rc) return rc;
        struct stat tmp;
        if (lstat(out, st ? st : &tmp) == 0) return d;
        if (errno != ENOENT && errno != ENOTDIR) return -errno;
    }
    return -ENOENT;
}

static int dir_exists(const char *path) {
    char p[PATH_MAX]; struct stat st;
    int d = locate(path, p, &st);
    if (d < 0) return d;
    return S_ISDIR(st.st_mode) ? 0 : -ENOTDIR;
}

static uint64_t path_hash(const char *path) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        h = (h ^ *p) * UINT64_C(1099511628211);
    return h;
}

static int choose_disk(const char *path) {
    int preferred = (int)(path_hash(path) & 1);
    struct statvfs v;
    if (statvfs(roots[preferred], &v) == 0 && v.f_bavail > 0)
        return preferred;
    return 1 - preferred;
}

static int ds_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    if (fi && fi->fh) return fstat((int)fi->fh, st) == -1 ? -errno : 0;
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, st);
    pthread_rwlock_unlock(&namespace_lock);
    return d < 0 ? d : 0;
}

static int ds_access(const char *path, int mask) {
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    int rc = d < 0 ? d : (access(p, mask) ? -errno : 0);
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    pthread_rwlock_rdlock(&namespace_lock);
    int rc = dir_exists(path);
    if (rc) goto done;
    for (int d = 0; d < 2; d++) {
        char p[PATH_MAX]; rc = backing(d, path, p);
        if (rc) goto done;
        DIR *dir = opendir(p);
        if (!dir) { rc = -errno; goto done; }
        struct dirent *e;
        while (1) {
            errno = 0;
            e = readdir(dir);
            if (!e) { if (errno && !rc) rc = -errno; break; }
            if (d == 1 && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                char other[PATH_MAX];
                if (snprintf(other, sizeof(other), "%s%s/%s", roots[0],
                             strcmp(path, "/") ? path : "", e->d_name) >= (int)sizeof(other)) {
                    rc = -ENAMETOOLONG; break;
                }
                struct stat st;
                if (lstat(other, &st) == 0) continue;
            }
            if (d == 1 && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) continue;
            if (filler(buf, e->d_name, NULL, 0, 0)) break;
        }
        closedir(dir);
        if (rc) goto done;
    }
done:
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_open(const char *path, struct fuse_file_info *fi) {
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    int fd = d < 0 ? -1 : open(p, fi->flags);
    int rc = d < 0 ? d : (fd < 0 ? -errno : 0);
    if (!rc) fi->fh = (uint64_t)fd;
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; int existing = locate(path, p, NULL);
    int rc;
    if (existing >= 0) rc = -EEXIST;
    else if (existing != -ENOENT) rc = existing;
    else {
        int d = choose_disk(path);
        rc = backing(d, path, p);
        if (!rc) {
            int fd = open(p, fi->flags | O_CREAT | O_EXCL, mode);
            rc = fd < 0 ? -errno : 0;
            if (!rc) fi->fh = (uint64_t)fd;
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)path; ssize_t n = pread((int)fi->fh, buf, size, off);
    return n < 0 ? -errno : (int)n;
}
static int ds_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)path; ssize_t n = pwrite((int)fi->fh, buf, size, off);
    return n < 0 ? -errno : (int)n;
}
static int ds_release(const char *path, struct fuse_file_info *fi) {
    (void)path; return close((int)fi->fh) ? -errno : 0;
}
static int ds_flush(const char *path, struct fuse_file_info *fi) {
    (void)path; int fd = dup((int)fi->fh);
    if (fd < 0) return -errno;
    return close(fd) ? -errno : 0;
}
static int ds_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path; int rc = datasync ? fdatasync((int)fi->fh) : fsync((int)fi->fh);
    return rc ? -errno : 0;
}

static int ds_mkdir(const char *path, mode_t mode) {
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; int exists = locate(path, p, NULL);
    int rc = exists >= 0 ? -EEXIST : (exists == -ENOENT ? 0 : exists);
    if (!rc) {
        rc = backing(0, path, p);
        if (!rc && mkdir(p, mode)) rc = -errno;
        if (!rc) {
            char q[PATH_MAX]; rc = backing(1, path, q);
            if (!rc && mkdir(q, mode)) rc = -errno;
            if (rc) rmdir(p);
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_unlink(const char *path) {
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : (S_ISDIR(st.st_mode) ? -EISDIR : (unlink(p) ? -errno : 0));
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_rmdir(const char *path) {
    pthread_rwlock_wrlock(&namespace_lock);
    int rc = dir_exists(path);
    if (!rc) {
        /* Check both halves before removing either half. */
        for (int d = 0; d < 2; d++) {
            char p[PATH_MAX]; rc = backing(d, path, p);
            if (rc) break;
            DIR *dir = opendir(p);
            if (!dir) { rc = -errno; break; }
            struct dirent *e;
            while ((e = readdir(dir))) {
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                    rc = -ENOTEMPTY; break;
                }
            }
            closedir(dir);
            if (rc) break;
        }
        if (!rc) {
            char p[PATH_MAX], q[PATH_MAX];
            backing(0, path, p); backing(1, path, q);
            struct stat st; lstat(p, &st);
            if (rmdir(p)) rc = -errno;
            else if (rmdir(q)) {
                rc = -errno;
                mkdir(p, st.st_mode & 07777);
            }
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    if (!strcmp(from, to)) return 0;
    pthread_rwlock_wrlock(&namespace_lock);
    char src[PATH_MAX], dst[PATH_MAX], other[PATH_MAX];
    struct stat st; int d = locate(from, src, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) {
        int target = locate(to, other, NULL);
        if (target < 0 && target != -ENOENT) rc = target;
        else if (S_ISDIR(st.st_mode)) {
            /* Directory trees are mirrored; replacement is deliberately refused. */
            if (target >= 0) rc = -EEXIST;
            for (int i = 0; i < 2 && !rc; i++) {
                char a[PATH_MAX], b[PATH_MAX];
                rc = backing(i, from, a);
                if (!rc) rc = backing(i, to, b);
                if (!rc && rename(a, b)) rc = -errno;
                if (rc && i == 1) {
                    char old[PATH_MAX], newp[PATH_MAX];
                    backing(0, from, old); backing(0, to, newp);
                    rename(newp, old);
                }
            }
        } else if (target >= 0 && target != d) rc = -EXDEV;
        else {
            rc = backing(d, to, dst);
            if (!rc && rename(src, dst)) rc = -errno;
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    if (fi) return ftruncate((int)fi->fh, size) ? -errno : 0;
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    int rc = d < 0 ? d : (truncate(p, size) ? -errno : 0);
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    if (fi) return fchmod((int)fi->fh, mode) ? -errno : 0;
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? 2 : 1); i++) {
        if (S_ISDIR(st.st_mode)) backing(i, path, p);
        if (chmod(p, mode)) { rc = -errno; break; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    if (fi) return fchown((int)fi->fh, uid, gid) ? -errno : 0;
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? 2 : 1); i++) {
        if (S_ISDIR(st.st_mode)) backing(i, path, p);
        if (lchown(p, uid, gid)) { rc = -errno; break; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi) {
    if (fi) return futimens((int)fi->fh, ts) ? -errno : 0;
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? 2 : 1); i++) {
        if (S_ISDIR(st.st_mode)) backing(i, path, p);
        if (utimensat(AT_FDCWD, p, ts, AT_SYMLINK_NOFOLLOW)) { rc = -errno; break; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_symlink(const char *target, const char *path) {
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    int rc = d >= 0 ? -EEXIST : (d == -ENOENT ? 0 : d);
    if (!rc) {
        rc = backing(choose_disk(path), path, p);
        if (!rc && symlink(target, p)) rc = -errno;
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_readlink(const char *path, char *buf, size_t size) {
    if (!size) return -EINVAL;
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    ssize_t n = d < 0 ? -1 : readlink(p, buf, size - 1);
    int rc = d < 0 ? d : (n < 0 ? -errno : 0);
    if (!rc) buf[n] = '\0';
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_link(const char *from, const char *to) {
    pthread_rwlock_wrlock(&namespace_lock);
    char a[PATH_MAX], b[PATH_MAX]; int d = locate(from, a, NULL);
    int rc = d < 0 ? d : backing(d, to, b);
    if (!rc) {
        int exists = locate(to, b, NULL);
        if (exists >= 0) rc = -EEXIST;
        else if (exists != -ENOENT) rc = exists;
        else { backing(d, to, b); if (link(a, b)) rc = -errno; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_statfs(const char *path, struct statvfs *out) {
    (void)path; struct statvfs a, b;
    if (statvfs(roots[0], &a) || statvfs(roots[1], &b)) return -errno;
    *out = a;
    if (a.f_frsize == b.f_frsize) {
        out->f_blocks += b.f_blocks; out->f_bfree += b.f_bfree;
        out->f_bavail += b.f_bavail; out->f_files += b.f_files;
        out->f_ffree += b.f_ffree;
    }
    return 0;
}

static const struct fuse_operations ops = {
    .getattr = ds_getattr, .access = ds_access, .readdir = ds_readdir,
    .open = ds_open, .create = ds_create, .read = ds_read,
    .write = ds_write, .release = ds_release, .flush = ds_flush,
    .fsync = ds_fsync, .mkdir = ds_mkdir, .unlink = ds_unlink,
    .rmdir = ds_rmdir, .rename = ds_rename, .truncate = ds_truncate,
    .chmod = ds_chmod, .chown = ds_chown, .utimens = ds_utimens,
    .symlink = ds_symlink, .readlink = ds_readlink, .link = ds_link,
    .statfs = ds_statfs,
};

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s BACKING0 BACKING1 MOUNTPOINT [FUSE options]\n", argv[0]);
        return 2;
    }
    struct stat a, b, m;
    for (int i = 0; i < 2; i++) {
        if (!realpath(argv[i + 1], roots[i]) || stat(roots[i], i ? &b : &a)) {
            perror(argv[i + 1]); return 1;
        }
    }
    char mountpoint[PATH_MAX];
    if (!realpath(argv[3], mountpoint) || stat(mountpoint, &m)) {
        perror(argv[3]); return 1;
    }
    if (!S_ISDIR(a.st_mode) || !S_ISDIR(b.st_mode) || !S_ISDIR(m.st_mode) ||
        a.st_dev == b.st_dev ||
        within(mountpoint, roots[0]) || within(mountpoint, roots[1]) ||
        within(roots[0], mountpoint) || within(roots[1], mountpoint)) {
        fprintf(stderr, "backing directories must be on different devices; mountpoint must be outside them\n");
        return 2;
    }
    char **args = calloc((size_t)argc - 1, sizeof(char *));
    if (!args) return 1;
    args[0] = argv[0]; args[1] = argv[3];
    for (int i = 4; i < argc; i++) args[i - 2] = argv[i];
    int rc = fuse_main(argc - 2, args, &ops, NULL);
    free(args); return rc;
}
