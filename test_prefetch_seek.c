#define main stripefsd_main
#define readahead recorded_readahead
#include "stripefsd.c"
#undef main
#undef readahead
#include <assert.h>

static off64_t prefetched[32];
static size_t prefetch_count;

ssize_t recorded_readahead(int fd, off64_t offset, size_t count) {
    (void)fd;
    assert(count == CHUNK_SIZE);
    assert(prefetch_count < sizeof(prefetched) / sizeof(prefetched[0]));
    prefetched[prefetch_count++] = offset;
    return 0;
}

int main(void) {
    char name[] = "/tmp/stripefs-prefetch-XXXXXX";
    int map_fd = mkstemp(name);
    assert(map_fd >= 0);
    assert(unlink(name) == 0);
    root_count = 1;
    for (uint64_t i = 0; i < 300; i++) {
        struct map_entry entry = {.offset = i * CHUNK_SIZE, .disk = 1};
        assert(write_all(map_fd, &entry, sizeof(entry), (off_t)(i * sizeof(entry))) == 0);
    }

    struct sf_handle handle = {.map_fd = map_fd};
    handle.segment_fds[0] = 0;
    assert(pthread_mutex_init(&handle.prefetch_lock, NULL) == 0);

    prefetch_stripes(&handle, 1U << 20, 1U << 20);
    assert(prefetch_count == 9);
    assert(prefetched[0] == 0 && prefetched[8] == 8 * CHUNK_SIZE);

    prefetch_stripes(&handle, 1U << 20, 250U * CHUNK_SIZE);
    assert(prefetch_count == 9);
    prefetch_stripes(&handle, 1U << 20, 250U * CHUNK_SIZE + (1U << 20));
    assert(prefetch_count == 18);
    assert(prefetched[9] == 250 * CHUNK_SIZE && prefetched[17] == 258 * CHUNK_SIZE);

    prefetch_stripes(&handle, 1U << 20, 25U * CHUNK_SIZE);
    assert(prefetch_count == 18);
    prefetch_stripes(&handle, 1U << 20, 25U * CHUNK_SIZE + (1U << 20));
    assert(prefetch_count == 27);
    assert(prefetched[18] == 25 * CHUNK_SIZE && prefetched[26] == 33 * CHUNK_SIZE);

    assert(pthread_mutex_destroy(&handle.prefetch_lock) == 0);
    assert(close(map_fd) == 0);
    puts("PASS: prefetch skips forward and backward seek gaps");
    return 0;
}
