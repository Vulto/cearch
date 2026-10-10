CC ?= cc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=gnu2x -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -D_FILE_OFFSET_BITS=64
LDFLAGS ?=

.PHONY: all clean test unit-test integration-test sanitize analyze install bench

all: cearch cearchd

cearch: src/cearch.c src/index.c src/indexer.c src/indexer.h src/index.h src/store.c src/store.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/cearch.c src/index.c src/indexer.c src/store.c

cearchd: src/daemon.c src/index.c src/indexer.c src/indexer.h src/protocol.h src/index.h src/store.c src/store.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/daemon.c src/index.c src/indexer.c src/store.c

tests/index_test: tests/index_test.c src/index.c src/index.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/index_test.c src/index.c

tests/store_test: tests/store_test.c src/store.c src/store.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/store_test.c src/store.c

unit-test: tests/index_test tests/store_test
	./tests/index_test
	./tests/store_test

integration-test: cearch cearchd
	sh tests/smoke.sh ./cearch
	sh tests/daemon.sh ./cearchd

test: unit-test integration-test

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -std=gnu2x -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -D_FILE_OFFSET_BITS=64 -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"

analyze:
	$(CC) $(CFLAGS) -fanalyzer -fsyntax-only src/cearch.c src/index.c src/store.c src/indexer.c src/daemon.c

bench: cearch
	sh tests/benchmark.sh ./cearch

install: cearch cearchd
	install -Dm755 cearch $(DESTDIR)/usr/local/bin/cearch
	install -Dm755 cearchd $(DESTDIR)/usr/local/bin/cearchd
	install -Dm644 docs/USAGE.md $(DESTDIR)/usr/local/share/doc/cearch/USAGE.md
	install -Dm644 docs/ARCHITECTURE.md $(DESTDIR)/usr/local/share/doc/cearch/ARCHITECTURE.md
	install -Dm644 docs/AUDIT.md $(DESTDIR)/usr/local/share/doc/cearch/AUDIT.md
	install -Dm644 docs/TESTING.md $(DESTDIR)/usr/local/share/doc/cearch/TESTING.md

clean:
	rm -f cearch cearchd tests/index_test tests/store_test