/* cc with the same libfuse flags as stripefsd.c; no mount needed. */
#define main stripefs_main
#include "stripefsd.c"
#undef main
#include <assert.h>

int main(void) {
    struct fuse_conn_info conn = {0};
    struct fuse_config cfg = {0};
    ds_init(&conn, &cfg);
    assert(!direct_reads);
    conn.capable = FUSE_CAP_DIRECT_IO_ALLOW_MMAP;
    ds_init(&conn, &cfg);
    assert(direct_reads);
    assert(conn.want & FUSE_CAP_DIRECT_IO_ALLOW_MMAP);
    puts("PASS: direct reads require mmap capability; older kernels retain cache");
    return 0;
}
