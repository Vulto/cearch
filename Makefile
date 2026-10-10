CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=gnu2x -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

.PHONY: all clean test install

all: cearch cearchd

cearch: src/cearch.c src/index.c src/indexer.c src/indexer.h src/index.h src/store.c src/store.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/cearch.c src/index.c src/indexer.c src/store.c

cearchd: src/daemon.c src/index.c src/indexer.c src/indexer.h src/store.c src/protocol.h src/index.h src/store.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/daemon.c src/index.c src/indexer.c src/store.c

test: cearch cearchd
	$(CC) $(CFLAGS) $(LDFLAGS) -o cearch-testquery src/testquery.c src/index.c
	./cearch-testquery
	rm -f cearch-testquery
	sh tests/smoke.sh ./cearch && sh tests/daemon.sh ./cearchd

install: cearch cearchd
	install -Dm755 cearch $(DESTDIR)/usr/local/bin/cearch
	install -Dm755 cearchd $(DESTDIR)/usr/local/bin/cearchd
	install -Dm644 docs/USAGE.md $(DESTDIR)/usr/local/share/doc/cearch/USAGE.md
	install -Dm644 docs/ARCHITECTURE.md $(DESTDIR)/usr/local/share/doc/cearch/ARCHITECTURE.md

clean:
	rm -f cearch cearchd cearch-testquery