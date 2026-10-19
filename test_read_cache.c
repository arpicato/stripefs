/* Build as test_cache_mode.c; no mount required. */
#define main stripefs_main
#include "stripefsd.c"
#undef main
#include <assert.h>

int main(void) {
    FILE *f = tmpfile();
    assert(f);
    char source[8192], actual[8192];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (char)i;
    assert(fwrite(source, 1, sizeof(source), f) == sizeof(source));
    assert(fflush(f) == 0);
    assert(read_backing(fileno(f), actual, 4096, 37, true) == 0);
    assert(!memcmp(actual, source + 37, 4096));
    assert(read_backing(fileno(f), actual, sizeof(actual), 0, false) == 0);
    assert(!memcmp(actual, source, sizeof(source)));
    assert(read_backing(fileno(f), actual, sizeof(actual), 1, true) == -EIO);
    assert(read_backing(-1, actual, 16, 0, true) == -EBADF);
    fclose(f);
    puts("PASS: small/large backing reads, unaligned offsets, EOF and errors");
    return 0;
}
