CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=gnu2x -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

.PHONY: all clean test install

all: cearch

cearch: src/cearch.c src/index.c src/index.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/cearch.c src/index.c

test: cearch
	sh tests/smoke.sh ./cearch

install: cearch
	install -Dm755 cearch $(DESTDIR)/usr/local/bin/cearch
	install -Dm644 docs/USAGE.md $(DESTDIR)/usr/local/share/doc/cearch/USAGE.md
	install -Dm644 docs/ARCHITECTURE.md $(DESTDIR)/usr/local/share/doc/cearch/ARCHITECTURE.md

clean:
	rm -f cearch