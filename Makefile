CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror

dualstripe: dualstripe.c
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 $< -o $@ $(shell pkg-config --cflags --libs fuse3) -pthread

.PHONY: clean
clean:
	rm -f dualstripe
