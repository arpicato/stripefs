CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

stripefsd: stripefsd.c
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 $< -o $@ $(shell pkg-config --cflags --libs fuse3) -pthread

test_prefetch_seek: test_prefetch_seek.c stripefsd.c
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 $< -o $@ $(shell pkg-config --cflags --libs fuse3) -pthread

.PHONY: clean test-prefetch
test-prefetch: test_prefetch_seek
	./test_prefetch_seek

clean:
	rm -f stripefsd test_prefetch_seek
