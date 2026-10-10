CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

.PHONY: all clean test install

all: cearch

cearch: src/cearch.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: cearch
	sh tests/smoke.sh ./cearch

install: cearch
	install -Dm755 cearch $(DESTDIR)/usr/local/bin/cearch
	install -Dm644 docs/USAGE.md $(DESTDIR)/usr/local/share/doc/cearch/USAGE.md
	install -Dm644 docs/ARCHITECTURE.md $(DESTDIR)/usr/local/share/doc/cearch/ARCHITECTURE.md

clean:
	rm -f cearch