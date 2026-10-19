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
#include <sys/xattr.h>
#include <sys/random.h>
#include <stdatomic.h>

#define MAX_DISKS 16
#define CHUNK_SIZE (4U * 1024U * 1024U)
struct map_entry {
    uint64_t offset;
    unsigned char disk;
    unsigned char reserved[7];
};
static char roots[MAX_DISKS][PATH_MAX];
static unsigned weights[MAX_DISKS];
static int root_count;
static char mount_path[PATH_MAX];
static dev_t root_devices[MAX_DISKS];
/* Namespace operations are serialized; open file descriptors can read in parallel. */
static pthread_rwlock_t namespace_lock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_int active_handles;

struct sf_handle {
    int fd;
    int map_fd;
    int segment_fds[MAX_DISKS];
    bool striped;
    char id[33];
    pthread_mutex_t prefetch_lock;
    uint64_t last_end;
    uint64_t prefetched_through;
    bool prefetched_any;
};

static int internal_path(int disk, const char *kind, const char *id,
                         const char *name, char out[PATH_MAX]) {
    const char *parts[] = {roots[disk], "/.stripefs/", kind, "/", id,
                           name ? "/" : "", name ? name : ""};
    size_t used = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        size_t len = strlen(parts[i]);
        if (len >= PATH_MAX - used) return -ENAMETOOLONG;
        memcpy(out + used, parts[i], len);
        used += len;
    }
    out[used] = '\0';
    return 0;
}

static int layout_dir(int disk, const char *kind, char out[PATH_MAX]) {
    int rc = internal_path(disk, kind, "", NULL, out);
    if (!rc) out[strlen(out) - 1] = '\0';
    return rc;
}

static int file_id(const char *path, char id[33]) {
    ssize_t n = lgetxattr(path, "user.stripefs.id", id, 32);
    if (n < 0 && errno == ENODATA) return 0;
    if (n < 0) return -errno;
    if (n != 32) return -EIO;
    for (int i = 0; i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return -EIO;
    id[32] = '\0';
    return 1;
}

static int read_all(int fd, void *buf, size_t size, off_t off) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, (char *)buf + done, size - done, off + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        done += (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t size, off_t off) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, (const char *)buf + done, size - done, off + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        done += (size_t)n;
    }
    return 0;
}

static int fsync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -errno;
    int rc = fsync(fd) ? -errno : 0;
    close(fd); return rc;
}

static int striped_read(int map_fd, const int segment_fds[MAX_DISKS], char *buf,
                        size_t size, off_t off) {
    size_t done = 0;
    while (done < size) {
        uint64_t index = (uint64_t)(off + (off_t)done) / CHUNK_SIZE;
        size_t chunk_off = (size_t)(off + (off_t)done) % CHUNK_SIZE;
        size_t amount = CHUNK_SIZE - chunk_off;
        if (amount > size - done) amount = size - done;
        struct map_entry entry;
        if (read_all(map_fd, &entry, sizeof(entry), (off_t)(index * sizeof(entry)))) return -EIO;
        if (!entry.disk || entry.disk > root_count) return -EIO;
        struct stat disk_stat;
        if (stat(roots[entry.disk - 1], &disk_stat) ||
            disk_stat.st_dev != root_devices[entry.disk - 1]) return -EIO;
        int fd = segment_fds[entry.disk - 1];
        if (fd < 0) return -EIO;
        int rc = read_all(fd, buf + done, amount, (off_t)entry.offset + (off_t)chunk_off);
        if (rc) return -EIO;
        done += amount;
    }
    return (int)done;
}

static void prefetch_stripes(struct sf_handle *h, size_t size, off_t off) {
    if (off < 0) return;
    uint64_t index = (uint64_t)off / CHUNK_SIZE;
    pthread_mutex_lock(&h->prefetch_lock);
    bool sequential = off == 0 || ((uint64_t)off >= h->last_end &&
                      (uint64_t)off - h->last_end <= CHUNK_SIZE);
    if ((uint64_t)off + size > h->last_end) h->last_end = (uint64_t)off + size;
    if (sequential && (!h->prefetched_any || index + 3 >= h->prefetched_through)) {
        struct stat st;
        if (!fstat(h->map_fd, &st)) {
            uint64_t count = (uint64_t)st.st_size / sizeof(struct map_entry);
            uint64_t from = h->prefetched_any ? h->prefetched_through + 1 : index;
            uint64_t end = index + 8;
            if (end >= count) end = count ? count - 1 : 0;
            for (uint64_t i = from; count && i <= end; i++) {
                struct map_entry entry;
                if (read_all(h->map_fd, &entry, sizeof(entry), (off_t)(i * sizeof(entry)))) break;
                if (!entry.disk || entry.disk > root_count) break;
                int fd = h->segment_fds[entry.disk - 1];
                if (fd >= 0) readahead(fd, (off_t)entry.offset, CHUNK_SIZE);
            }
            h->prefetched_any = true;
            h->prefetched_through = end;
        }
    }
    pthread_mutex_unlock(&h->prefetch_lock);
}

static int random_id(char id[33]) {
    unsigned char bytes[16];
    if (getrandom(bytes, sizeof(bytes), 0) != sizeof(bytes)) return -EIO;
    for (int i = 0; i < 16; i++) sprintf(id + i * 2, "%02x", bytes[i]);
    return 0;
}

static int copy_xattrs(const char *source, int dest_fd) {
    ssize_t len = llistxattr(source, NULL, 0);
    if (len < 0) return -errno;
    if (!len) return 0;
    char *names = malloc((size_t)len);
    if (!names) return -ENOMEM;
    if (llistxattr(source, names, (size_t)len) != len) { free(names); return -EIO; }
    int rc = 0;
    for (char *name = names; name < names + len; name += strlen(name) + 1) {
        if (!strcmp(name, "user.stripefs.id")) continue;
        ssize_t size = lgetxattr(source, name, NULL, 0);
        if (size < 0) { rc = -errno; break; }
        void *value = malloc((size_t)size + 1);
        if (!value) { rc = -ENOMEM; break; }
        if (lgetxattr(source, name, value, (size_t)size) != size ||
            fsetxattr(dest_fd, name, value, (size_t)size, 0)) rc = -errno;
        free(value);
        if (rc) break;
    }
    free(names); return rc;
}

static bool within(const char *path, const char *directory) {
    size_t n = strlen(directory);
    return !strncmp(path, directory, n) &&
           (directory[n - 1] == '/' || path[n] == '/' || path[n] == '\0');
}

static int backing(int disk, const char *path, char out[PATH_MAX]) {
    if (!strcmp(path, "/.stripefs") || !strncmp(path, "/.stripefs/", 11))
        return -ENOENT;
    if (path[0] != '/' || strstr(path, "/../") ||
        strcmp(path, "/..") == 0 || strstr(path, "/./") ||
        strcmp(path, "/.") == 0) return -EINVAL;
    if (snprintf(out, PATH_MAX, "%s%s", roots[disk], path) >= PATH_MAX)
        return -ENAMETOOLONG;
    return 0;
}

static int check_roots(void) {
    for (int i = 0; i < root_count; i++) {
        struct stat st;
        if (stat(roots[i], &st) || st.st_dev != root_devices[i] || !S_ISDIR(st.st_mode))
            return -EIO;
    }
    return 0;
}

static int locate(const char *path, char out[PATH_MAX], struct stat *st) {
    int health = check_roots();
    if (health) return health;
    for (int d = 0; d < root_count; d++) {
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
    uint64_t total = 0;
    for (int i = 0; i < root_count; i++) total += weights[i];
    uint64_t position = path_hash(path) % total;
    int preferred = 0;
    for (int i = 0; i < root_count; i++) {
        if (position < weights[i]) { preferred = i; break; }
        position -= weights[i];
    }
    struct statvfs v;
    if (!statvfs(roots[preferred], &v) && v.f_bavail * v.f_frsize > 64ULL * 1024 * 1024)
        return preferred;
    uint64_t best_free = 0; int best = preferred;
    for (int i = 0; i < root_count; i++) {
        if (!statvfs(roots[i], &v)) {
            uint64_t free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
            if (free_bytes > best_free) { best_free = free_bytes; best = i; }
        }
    }
    return best;
}

static void cleanup_staging(const char id[33]) {
    for (int i = 0; i < root_count; i++) {
        char dir_path[PATH_MAX];
        if (internal_path(i, "chunks", id, NULL, dir_path)) continue;
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                if (entry->d_name[0] == '.') continue;
                char chunk[PATH_MAX];
                if (!internal_path(i, "chunks", id, entry->d_name, chunk)) unlink(chunk);
            }
            closedir(dir);
            rmdir(dir_path);
        }
    }
    char path[PATH_MAX];
    if (!internal_path(0, "maps", id, NULL, path)) unlink(path);
    for (int i = 0; i < root_count; i++)
        if (!internal_path(i, "tmp", id, NULL, path)) unlink(path);
    if (!internal_path(0, "transactions", id, NULL, path)) unlink(path);
}

static int optimize_file(const char *logical) {
    char source[PATH_MAX], id[33], map_path[PATH_MAX], stub_path[PATH_MAX];
    struct stat before, after;
    int disk = locate(logical, source, &before);
    if (disk < 0) return disk;
    if (!S_ISREG(before.st_mode) || before.st_nlink != 1) return -EOPNOTSUPP;
    int existing = file_id(source, id);
    if (existing < 0) return existing;
    if (existing) return 0;
    if (atomic_load(&active_handles)) return -EBUSY;
    int rc = random_id(id);
    if (rc) return rc;
    int src_fd = open(source, O_RDONLY);
    if (src_fd < 0) return -errno;
    int map_fd = -1, stub_fd = -1;
    int segments[MAX_DISKS];
    for (int i = 0; i < MAX_DISKS; i++) segments[i] = -1;
    char *buffer = NULL, *check = NULL;
    uint64_t free_bytes[MAX_DISKS], assigned[MAX_DISKS] = {0};
    for (int i = 0; i < root_count; i++) {
        struct statvfs v;
        if (statvfs(roots[i], &v)) { rc = -errno; goto finish; }
        free_bytes[i] = (uint64_t)v.f_bavail * v.f_frsize;
        char dir[PATH_MAX];
        rc = internal_path(i, "chunks", id, NULL, dir);
        if (rc || mkdir(dir, 0700)) { if (!rc) rc = -errno; goto finish; }
        char data[PATH_MAX];
        rc = internal_path(i, "chunks", id, "data", data);
        if (rc) goto finish;
        segments[i] = open(data, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (segments[i] < 0) { rc = -errno; goto finish; }
    }
    rc = internal_path(0, "maps", id, NULL, map_path);
    if (rc) goto finish;
    map_fd = open(map_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (map_fd < 0) { rc = -errno; goto finish; }
    buffer = malloc(CHUNK_SIZE); check = malloc(CHUNK_SIZE);
    if (!buffer || !check) { rc = -ENOMEM; goto finish; }
    uint64_t chunks = ((uint64_t)before.st_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    for (uint64_t index = 0; index < chunks; index++) {
        off_t off = (off_t)(index * CHUNK_SIZE);
        size_t len = (size_t)(before.st_size - off);
        if (len > CHUNK_SIZE) len = CHUNK_SIZE;
        rc = read_all(src_fd, buffer, len, off);
        if (rc) goto finish;
        int chosen = -1;
        for (int i = 0; i < root_count; i++) {
            if (free_bytes[i] < len + 64ULL * 1024 * 1024) continue;
            if (chosen < 0 ||
                (long double)assigned[i] / weights[i] < (long double)assigned[chosen] / weights[chosen] ||
                (assigned[i] == assigned[chosen] && weights[i] > weights[chosen])) chosen = i;
        }
        if (chosen < 0) { rc = -ENOSPC; goto finish; }
        rc = write_all(segments[chosen], buffer, len, (off_t)assigned[chosen]);
        if (rc) goto finish;
        struct map_entry entry = {.offset = assigned[chosen], .disk = (unsigned char)(chosen + 1)};
        rc = write_all(map_fd, &entry, sizeof(entry), (off_t)(index * sizeof(entry)));
        if (rc) goto finish;
        assigned[chosen] += len; free_bytes[chosen] -= len;
    }
    for (int i = 0; i < root_count; i++)
        if (fsync(segments[i])) { rc = -errno; goto finish; }
    if (fsync(map_fd)) { rc = -errno; goto finish; }
    for (int i = 0; i < root_count; i++) {
        char dir[PATH_MAX]; internal_path(i, "chunks", id, NULL, dir);
        rc = fsync_dir(dir);
        if (rc) goto finish;
    }
    /* Full byte comparison detects transfer faults before changing the visible file. */
    for (uint64_t index = 0; index < chunks; index++) {
        off_t off = (off_t)(index * CHUNK_SIZE);
        size_t len = (size_t)(before.st_size - off);
        if (len > CHUNK_SIZE) len = CHUNK_SIZE;
        rc = read_all(src_fd, buffer, len, off);
        if (rc) goto finish;
        int n = striped_read(map_fd, segments, check, len, off);
        if (n < 0 || (size_t)n != len || memcmp(buffer, check, len)) {
            rc = -EIO; goto finish;
        }
    }
    if (fstat(src_fd, &after) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        rc = -ESTALE; goto finish;
    }
    rc = internal_path(disk, "tmp", id, NULL, stub_path);
    if (rc) goto finish;
    stub_fd = open(stub_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (stub_fd < 0) { rc = -errno; goto finish; }
    if (ftruncate(stub_fd, before.st_size) || fchmod(stub_fd, before.st_mode & 07777) ||
        fchown(stub_fd, before.st_uid, before.st_gid)) {
        rc = -errno; goto finish;
    }
    struct timespec times[2] = {before.st_atim, before.st_mtim};
    if (futimens(stub_fd, times)) { rc = -errno; goto finish; }
    rc = copy_xattrs(source, stub_fd);
    if (rc) goto finish;
    if (fsetxattr(stub_fd, "user.stripefs.id", id, 32, 0) || fsync(stub_fd)) {
        rc = -errno; goto finish;
    }
    char transaction[PATH_MAX], backup[PATH_MAX];
    internal_path(0, "transactions", id, NULL, transaction);
    internal_path(disk, "backups", id, NULL, backup);
    int tx = open(transaction, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (tx < 0) { rc = -errno; goto finish; }
    char record[PATH_MAX + 32];
    int record_len = snprintf(record, sizeof(record), "%d\n%s\n", disk, logical);
    rc = write_all(tx, record, (size_t)record_len, 0);
    if (!rc && fsync(tx)) rc = -errno;
    close(tx);
    if (rc) goto finish;
    char tx_dir[PATH_MAX];
    rc = layout_dir(0, "transactions", tx_dir);
    if (rc) goto finish;
    rc = fsync_dir(tx_dir);
    if (rc) goto finish;
    if (rename(source, backup)) { rc = -errno; goto finish; }
    if (rename(stub_path, source)) {
        rc = -errno;
        rename(backup, source);
        goto finish;
    }
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", source);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; fsync_dir(parent); }
    unlink(transaction);
    fsync_dir(tx_dir);
    rc = 0;
finish:
    if (stub_fd >= 0) close(stub_fd);
    if (map_fd >= 0) close(map_fd);
    for (int i = 0; i < root_count; i++) if (segments[i] >= 0) close(segments[i]);
    close(src_fd);
    free(buffer); free(check);
    if (rc) {
        char backup[PATH_MAX], current_id[33];
        internal_path(disk, "backups", id, NULL, backup);
        struct stat st;
        if (!lstat(source, &st) && file_id(source, current_id) == 0 &&
            lstat(backup, &st) && errno == ENOENT) cleanup_staging(id);
    }
    return rc;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0700) && errno != EEXIST) return -errno;
    struct stat st;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode)) return -ENOTDIR;
    return 0;
}

static int setup_layout(void) {
    for (int i = 0; i < root_count; i++) {
        char p[PATH_MAX];
        if (snprintf(p, sizeof(p), "%s/.stripefs", roots[i]) >= (int)sizeof(p)) return -ENAMETOOLONG;
        int rc = ensure_dir(p); if (rc) return rc;
        const char *kinds[] = {"chunks", "tmp", "backups", "maps", "transactions"};
        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            if (i && k >= 3) continue;
            if (snprintf(p, sizeof(p), "%s/.stripefs/%s", roots[i], kinds[k]) >= (int)sizeof(p))
                return -ENAMETOOLONG;
            rc = ensure_dir(p); if (rc) return rc;
        }
    }
    return 0;
}

static int recover_transactions(void) {
    char dir_path[PATH_MAX];
    int path_rc = layout_dir(0, "transactions", dir_path);
    if (path_rc) return path_rc;
    DIR *dir = opendir(dir_path);
    if (!dir) return -errno;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (strlen(entry->d_name) != 32) { rc = -EIO; break; }
        char transaction[PATH_MAX], backup[PATH_MAX], source[PATH_MAX];
        internal_path(0, "transactions", entry->d_name, NULL, transaction);
        FILE *f = fopen(transaction, "r");
        if (!f) { rc = -errno; break; }
        int disk = -1; char logical[PATH_MAX];
        if (fscanf(f, "%d\n", &disk) != 1 ||
            !fgets(logical, sizeof(logical), f) || disk < 0 || disk >= root_count) {
            fclose(f); rc = -EIO; break;
        }
        fclose(f);
        logical[strcspn(logical, "\n")] = '\0';
        rc = backing(disk, logical, source);
        if (rc) break;
        internal_path(disk, "backups", entry->d_name, NULL, backup);
        struct stat st;
        if (lstat(source, &st)) {
            if (errno == ENOENT && !lstat(backup, &st)) {
                if (rename(backup, source)) { rc = -errno; break; }
            } else { rc = -EIO; break; }
        }
        if (unlink(transaction)) { rc = -errno; break; }
    }
    closedir(dir);
    return rc;
}

static void cleanup_chunks(const char id[33], int map_fd) {
    (void)map_fd;
    for (int i = 0; i < root_count; i++) {
        char dir[PATH_MAX], data[PATH_MAX];
        if (!internal_path(i, "chunks", id, "data", data)) unlink(data);
        if (!internal_path(i, "chunks", id, NULL, dir)) rmdir(dir);
    }
    char map[PATH_MAX];
    if (!internal_path(0, "maps", id, NULL, map)) unlink(map);
}

static int unstripe_file(const char *logical) {
    char source[PATH_MAX], id[33]; struct stat before;
    int disk = locate(logical, source, &before);
    if (disk < 0) return disk;
    int kind = file_id(source, id);
    if (kind < 0) return kind;
    if (!kind) return 0;
    if (atomic_load(&active_handles)) return -EBUSY;
    char map[PATH_MAX], temporary[PATH_MAX], temp_name[40];
    internal_path(0, "maps", id, NULL, map);
    int map_fd = open(map, O_RDONLY);
    if (map_fd < 0) return -EIO;
    snprintf(temp_name, sizeof(temp_name), "%s.plain", id);
    int rc = internal_path(disk, "tmp", temp_name, NULL, temporary);
    if (rc) { close(map_fd); return rc; }
    int temp_fd = open(temporary, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (temp_fd < 0) { close(map_fd); return -errno; }
    char *buffer = NULL, *check = NULL;
    int segments[MAX_DISKS];
    for (int i = 0; i < MAX_DISKS; i++) segments[i] = -1;
    for (int i = 0; i < root_count; i++) {
        char data[PATH_MAX]; internal_path(i, "chunks", id, "data", data);
        segments[i] = open(data, O_RDONLY);
        if (segments[i] < 0) { rc = -EIO; goto finish; }
    }
    buffer = malloc(CHUNK_SIZE); check = malloc(CHUNK_SIZE);
    if (!buffer || !check) { rc = -ENOMEM; goto finish; }
    for (off_t off = 0; off < before.st_size; off += CHUNK_SIZE) {
        size_t len = (size_t)(before.st_size - off);
        if (len > CHUNK_SIZE) len = CHUNK_SIZE;
        int n = striped_read(map_fd, segments, buffer, len, off);
        if (n < 0 || (size_t)n != len) { rc = -EIO; goto finish; }
        rc = write_all(temp_fd, buffer, len, off);
        if (rc) goto finish;
    }
    if (fchmod(temp_fd, before.st_mode & 07777) ||
        fchown(temp_fd, before.st_uid, before.st_gid)) { rc = -errno; goto finish; }
    struct timespec times[2] = {before.st_atim, before.st_mtim};
    if (futimens(temp_fd, times)) { rc = -errno; goto finish; }
    rc = copy_xattrs(source, temp_fd);
    if (rc) goto finish;
    if (fsync(temp_fd)) { rc = -errno; goto finish; }
    for (off_t off = 0; off < before.st_size; off += CHUNK_SIZE) {
        size_t len = (size_t)(before.st_size - off);
        if (len > CHUNK_SIZE) len = CHUNK_SIZE;
        int n = striped_read(map_fd, segments, buffer, len, off);
        if (n < 0 || (size_t)n != len || read_all(temp_fd, check, len, off) ||
            memcmp(buffer, check, len)) { rc = -EIO; goto finish; }
    }
    if (renameat2(AT_FDCWD, temporary, AT_FDCWD, source, RENAME_EXCHANGE)) {
        rc = -errno; goto finish;
    }
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", source);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; fsync_dir(parent); }
    /* A crash after exchange leaves an extra old stub/chunks, not lost data. */
    unlink(temporary);
    cleanup_chunks(id, map_fd);
    rc = 0;
finish:
    free(buffer); free(check);
    close(temp_fd); close(map_fd);
    for (int i = 0; i < root_count; i++) if (segments[i] >= 0) close(segments[i]);
    if (rc) unlink(temporary);
    return rc;
}

static int ds_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    if (fi && fi->fh) {
        struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
        return fstat(h->fd, st) == -1 ? -errno : 0;
    }
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
    for (int d = 0; d < root_count; d++) {
        char p[PATH_MAX]; rc = backing(d, path, p);
        if (rc) goto done;
        DIR *dir = opendir(p);
        if (!dir) { rc = -errno; goto done; }
        struct dirent *e;
        while (1) {
            errno = 0;
            e = readdir(dir);
            if (!e) { if (errno && !rc) rc = -errno; break; }
            if (!strcmp(e->d_name, ".stripefs")) continue;
            if (d && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) continue;
            bool duplicate = false;
            for (int prior = 0; prior < d; prior++) {
                char other[PATH_MAX];
                if (snprintf(other, sizeof(other), "%s%s/%s", roots[prior],
                             strcmp(path, "/") ? path : "", e->d_name) >= (int)sizeof(other)) {
                    rc = -ENAMETOOLONG; break;
                }
                struct stat st;
                if (!lstat(other, &st)) { duplicate = true; break; }
            }
            if (rc) break;
            if (duplicate) continue;
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
    char id[33]; int kind = d < 0 ? d : file_id(p, id);
    int rc = kind < 0 ? kind : 0;
    if (!rc && kind && (fi->flags & O_ACCMODE) != O_RDONLY) rc = -EROFS;
    struct sf_handle *h = NULL;
    if (!rc) {
        h = calloc(1, sizeof(*h));
        if (!h) rc = -ENOMEM;
        else {
            h->fd = -1; h->map_fd = -1;
            for (int i = 0; i < MAX_DISKS; i++) h->segment_fds[i] = -1;
            pthread_mutex_init(&h->prefetch_lock, NULL);
        }
    }
    if (!rc) {
        h->fd = open(p, kind ? O_RDONLY : fi->flags);
        h->map_fd = -1;
        if (h->fd < 0) rc = -errno;
        if (!rc && kind) {
            char map[PATH_MAX]; internal_path(0, "maps", id, NULL, map);
            h->map_fd = open(map, O_RDONLY);
            if (h->map_fd < 0) rc = -EIO;
            if (!rc) for (int i = 0; i < root_count; i++) {
                char data[PATH_MAX]; internal_path(i, "chunks", id, "data", data);
                h->segment_fds[i] = open(data, O_RDONLY);
                if (h->segment_fds[i] < 0) { rc = -EIO; break; }
                posix_fadvise(h->segment_fds[i], 0, 0, POSIX_FADV_SEQUENTIAL);
            }
            if (!rc) {
                h->striped = true;
                memcpy(h->id, id, sizeof(h->id));
            }
        }
    }
    if (!rc) {
        fi->fh = (uint64_t)(uintptr_t)h;
        atomic_fetch_add(&active_handles, 1);
        if (kind) fi->keep_cache = 1;
    } else if (h) {
        if (h->fd >= 0) close(h->fd);
        if (h->map_fd >= 0) close(h->map_fd);
        for (int i = 0; i < root_count; i++) if (h->segment_fds[i] >= 0) close(h->segment_fds[i]);
        pthread_mutex_destroy(&h->prefetch_lock);
        free(h);
    }
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
            if (!rc) {
                struct sf_handle *h = calloc(1, sizeof(*h));
                if (!h) { close(fd); unlink(p); rc = -ENOMEM; }
                else {
                    h->fd = fd; h->map_fd = -1;
                    for (int i = 0; i < MAX_DISKS; i++) h->segment_fds[i] = -1;
                    pthread_mutex_init(&h->prefetch_lock, NULL);
                    fi->fh = (uint64_t)(uintptr_t)h;
                    atomic_fetch_add(&active_handles, 1);
                }
            }
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)path;
    struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
    if (h->striped) {
        struct stat st;
        if (fstat(h->fd, &st)) return -errno;
        if (off >= st.st_size) return 0;
        if (size > (size_t)(st.st_size - off)) size = (size_t)(st.st_size - off);
        prefetch_stripes(h, size, off);
        return striped_read(h->map_fd, h->segment_fds, buf, size, off);
    }
    ssize_t n = pread(h->fd, buf, size, off);
    return n < 0 ? -errno : (int)n;
}
static int ds_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)path;
    struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
    if (h->striped) return -EROFS;
    ssize_t n = pwrite(h->fd, buf, size, off);
    return n < 0 ? -errno : (int)n;
}
static int ds_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
    int rc = close(h->fd) ? -errno : 0;
    if (h->map_fd >= 0) close(h->map_fd);
    for (int i = 0; i < root_count; i++) if (h->segment_fds[i] >= 0) close(h->segment_fds[i]);
    pthread_mutex_destroy(&h->prefetch_lock);
    free(h);
    atomic_fetch_sub(&active_handles, 1);
    return rc;
}
static int ds_flush(const char *path, struct fuse_file_info *fi) {
    (void)path;
    struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
    int fd = dup(h->fd);
    if (fd < 0) return -errno;
    return close(fd) ? -errno : 0;
}
static int ds_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path;
    struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
    int rc = datasync ? fdatasync(h->fd) : fsync(h->fd);
    return rc ? -errno : 0;
}

static int ds_mkdir(const char *path, mode_t mode) {
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; int exists = locate(path, p, NULL);
    int rc = exists >= 0 ? -EEXIST : (exists == -ENOENT ? 0 : exists);
    if (!rc) {
        for (int i = 0; i < root_count && !rc; i++) {
            rc = backing(i, path, p);
            if (!rc && mkdir(p, mode)) rc = -errno;
            if (rc) for (int prior = i - 1; prior >= 0; prior--) {
                backing(prior, path, p); rmdir(p);
            }
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_unlink(const char *path) {
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : (S_ISDIR(st.st_mode) ? -EISDIR : 0);
    char id[33]; int kind = rc ? 0 : file_id(p, id);
    if (!rc && kind < 0) rc = kind;
    if (!rc && kind && atomic_load(&active_handles)) rc = -EBUSY;
    int map_fd = -1;
    if (!rc && kind) {
        char map[PATH_MAX]; internal_path(0, "maps", id, NULL, map);
        map_fd = open(map, O_RDONLY);
        if (map_fd < 0) rc = -EIO;
    }
    if (!rc && unlink(p)) rc = -errno;
    if (!rc && map_fd >= 0) cleanup_chunks(id, map_fd);
    if (map_fd >= 0) close(map_fd);
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_rmdir(const char *path) {
    pthread_rwlock_wrlock(&namespace_lock);
    int rc = dir_exists(path);
    if (!rc) {
        /* Check both halves before removing either half. */
        for (int d = 0; d < root_count; d++) {
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
            for (int i = 0; i < root_count && !rc; i++) {
                char p[PATH_MAX]; backing(i, path, p);
                struct stat st; lstat(p, &st);
                if (rmdir(p)) {
                    rc = -errno;
                    for (int prior = i - 1; prior >= 0; prior--) {
                        backing(prior, path, p); mkdir(p, st.st_mode & 07777);
                    }
                }
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
            for (int i = 0; i < root_count && !rc; i++) {
                char a[PATH_MAX], b[PATH_MAX];
                rc = backing(i, from, a);
                if (!rc) rc = backing(i, to, b);
                if (!rc && rename(a, b)) rc = -errno;
                if (rc) for (int prior = i - 1; prior >= 0; prior--) {
                    char old[PATH_MAX], newp[PATH_MAX];
                    backing(prior, from, old); backing(prior, to, newp);
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
    if (fi) {
        struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
        if (h->striped) return -EROFS;
        return ftruncate(h->fd, size) ? -errno : 0;
    }
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    char id[33]; int kind = d < 0 ? d : file_id(p, id);
    int rc = kind < 0 ? kind : (kind ? -EROFS : (truncate(p, size) ? -errno : 0));
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    if (fi) {
        struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
        return fchmod(h->fd, mode) ? -errno : 0;
    }
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? root_count : 1); i++) {
        if (S_ISDIR(st.st_mode)) backing(i, path, p);
        if (chmod(p, mode)) { rc = -errno; break; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    if (fi) {
        struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
        return fchown(h->fd, uid, gid) ? -errno : 0;
    }
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? root_count : 1); i++) {
        if (S_ISDIR(st.st_mode)) backing(i, path, p);
        if (lchown(p, uid, gid)) { rc = -errno; break; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi) {
    if (fi) {
        struct sf_handle *h = (struct sf_handle *)(uintptr_t)fi->fh;
        return futimens(h->fd, ts) ? -errno : 0;
    }
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? root_count : 1); i++) {
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
    char id[33]; int kind = d < 0 ? d : file_id(a, id);
    int rc = kind < 0 ? kind : (kind ? -EOPNOTSUPP : backing(d, to, b));
    if (!rc) {
        int exists = locate(to, b, NULL);
        if (exists >= 0) rc = -EEXIST;
        else if (exists != -ENOENT) rc = exists;
        else { backing(d, to, b); if (link(a, b)) rc = -errno; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_getxattr(const char *path, const char *name, char *value, size_t size) {
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    int rc;
    if (d < 0) rc = d;
    else if (!strcmp(name, "user.stripefs.state")) {
        char id[33]; int kind = file_id(p, id);
        if (kind < 0) rc = kind;
        else {
            const char *state = kind ? "striped" : "plain";
            size_t needed = strlen(state);
            if (!size) rc = (int)needed;
            else if (size < needed) rc = -ERANGE;
            else { memcpy(value, state, needed); rc = (int)needed; }
        }
    } else {
        ssize_t n = lgetxattr(p, name, value, size);
        rc = n < 0 ? -errno : (int)n;
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_listxattr(const char *path, char *list, size_t size) {
    pthread_rwlock_rdlock(&namespace_lock);
    char p[PATH_MAX]; int d = locate(path, p, NULL);
    ssize_t n = d < 0 ? -1 : llistxattr(p, list, size);
    int rc = d < 0 ? d : (n < 0 ? -errno : (int)n);
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_setxattr(const char *path, const char *name, const char *value,
                       size_t size, int flags) {
    pthread_rwlock_wrlock(&namespace_lock);
    int rc;
    if (!strcmp(name, "user.stripefs.optimize")) {
        rc = (size == 1 && value[0] == '1' && !flags) ? optimize_file(path) : -EINVAL;
    } else if (!strcmp(name, "user.stripefs.unstripe")) {
        rc = (size == 1 && value[0] == '1' && !flags) ? unstripe_file(path) : -EINVAL;
    } else if (!strcmp(name, "user.stripefs.id") || !strcmp(name, "user.stripefs.state")) {
        rc = -EPERM;
    } else {
        char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
        rc = d < 0 ? d : 0;
        if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? root_count : 1); i++) {
            if (S_ISDIR(st.st_mode)) backing(i, path, p);
            if (lsetxattr(p, name, value, size, flags)) { rc = -errno; break; }
        }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}

static int ds_removexattr(const char *path, const char *name) {
    if (!strncmp(name, "user.stripefs.", 14)) return -EPERM;
    pthread_rwlock_wrlock(&namespace_lock);
    char p[PATH_MAX]; struct stat st; int d = locate(path, p, &st);
    int rc = d < 0 ? d : 0;
    if (!rc) for (int i = 0; i < (S_ISDIR(st.st_mode) ? root_count : 1); i++) {
        if (S_ISDIR(st.st_mode)) backing(i, path, p);
        if (lremovexattr(p, name)) { rc = -errno; break; }
    }
    pthread_rwlock_unlock(&namespace_lock); return rc;
}
static int ds_statfs(const char *path, struct statvfs *out) {
    (void)path;
    int health = check_roots();
    if (health) return health;
    if (statvfs(roots[0], out)) return -errno;
    for (int i = 1; i < root_count; i++) {
        struct statvfs v;
        if (statvfs(roots[i], &v)) return -errno;
        if (out->f_frsize == v.f_frsize) {
            out->f_blocks += v.f_blocks; out->f_bfree += v.f_bfree;
            out->f_bavail += v.f_bavail; out->f_files += v.f_files;
            out->f_ffree += v.f_ffree;
        }
    }
    return 0;
}

static void *ds_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    conn->max_background = 64;
    conn->max_readahead = 16U * 1024U * 1024U;
    conn->max_read = 1024U * 1024U;
    conn->want |= FUSE_CAP_ASYNC_READ;
    cfg->auto_cache = 1;
    cfg->attr_timeout = 0.25;
    cfg->entry_timeout = 0.25;
    cfg->negative_timeout = 0;
    return NULL;
}

static const struct fuse_operations ops = {
    .init = ds_init,
    .getattr = ds_getattr, .access = ds_access, .readdir = ds_readdir,
    .open = ds_open, .create = ds_create, .read = ds_read,
    .write = ds_write, .release = ds_release, .flush = ds_flush,
    .fsync = ds_fsync, .mkdir = ds_mkdir, .unlink = ds_unlink,
    .rmdir = ds_rmdir, .rename = ds_rename, .truncate = ds_truncate,
    .chmod = ds_chmod, .chown = ds_chown, .utimens = ds_utimens,
    .symlink = ds_symlink, .readlink = ds_readlink, .link = ds_link,
    .statfs = ds_statfs, .getxattr = ds_getxattr, .setxattr = ds_setxattr,
    .listxattr = ds_listxattr, .removexattr = ds_removexattr,
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s CONFIG [FUSE options]\n", argv[0]);
        return 2;
    }
    FILE *config = fopen(argv[1], "r");
    if (!config) { perror(argv[1]); return 1; }
    char line[PATH_MAX + 128], raw_mount[PATH_MAX] = "";
    while (fgets(line, sizeof(line), config)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strncmp(line, "mount\t", 6)) {
            if (snprintf(raw_mount, sizeof(raw_mount), "%s", line + 6) >= (int)sizeof(raw_mount))
                return 2;
        } else if (!strncmp(line, "root\t", 5)) {
            if (root_count == MAX_DISKS) { fprintf(stderr, "too many disks\n"); return 2; }
            char *tab = strchr(line + 5, '\t');
            if (!tab) { fprintf(stderr, "invalid root line\n"); return 2; }
            *tab = '\0';
            char *end;
            unsigned long weight = strtoul(line + 5, &end, 10);
            if (*end || !weight || weight > 1000000UL || !tab[1]) {
                fprintf(stderr, "invalid disk weight or path\n"); return 2;
            }
            weights[root_count] = (unsigned)weight;
            if (!realpath(tab + 1, roots[root_count])) { perror(tab + 1); return 1; }
            root_count++;
        } else if (line[0]) {
            fprintf(stderr, "invalid config line\n"); return 2;
        }
    }
    fclose(config);
    if (root_count < 2 || !raw_mount[0]) {
        fprintf(stderr, "configure a mountpoint and at least two disks\n"); return 2;
    }
    if (!realpath(raw_mount, mount_path)) { perror(raw_mount); return 1; }
    struct stat m;
    if (stat(mount_path, &m) || !S_ISDIR(m.st_mode)) {
        perror(mount_path); return 1;
    }
    for (int i = 0; i < root_count; i++) {
        struct stat st;
        if (stat(roots[i], &st) || !S_ISDIR(st.st_mode) ||
            within(mount_path, roots[i]) || within(roots[i], mount_path)) {
            fprintf(stderr, "invalid backing directory or mountpoint overlap: %s\n", roots[i]); return 2;
        }
        for (int j = 0; j < i; j++) {
            struct stat prior;
            stat(roots[j], &prior);
            if (st.st_dev == prior.st_dev) {
                fprintf(stderr, "backing directories must be on distinct devices\n"); return 2;
            }
        }
        root_devices[i] = st.st_dev;
    }
    int layout_rc = setup_layout();
    if (!layout_rc) layout_rc = recover_transactions();
    if (layout_rc) {
        fprintf(stderr, "StripeFS layout or recovery failed: %s\n", strerror(-layout_rc));
        return 1;
    }
    char **args = calloc((size_t)argc + 3, sizeof(char *));
    if (!args) return 1;
    args[0] = argv[0]; args[1] = mount_path;
    args[2] = "-o"; args[3] = "max_read=1048576";
    for (int i = 2; i < argc; i++) args[i + 2] = argv[i];
    int rc = fuse_main(argc + 2, args, &ops, NULL);
    free(args); return rc;
}
