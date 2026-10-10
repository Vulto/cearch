# cearch

**cearch** is a pure CLI Linux file-search utility written in C23. It is designed around a daemon/client architecture, compact integer file IDs, a trigram name index, and filesystem reconciliation.

This repository starts with a working baseline and grows incrementally. Correctness and buildability are prioritized over unmeasured performance claims.

## Build

Requirements: a C23-capable compiler (recent GCC or Clang), GNU Make, and Linux.

```sh
make
make test
```

The project aims to avoid mandatory third-party dependencies. The current baseline uses libc and Linux/POSIX APIs.

## Quick start

```sh
# Index a tree into a local database
./cearch index --db ~/.local/state/cearch/index /home/me

# Search indexed paths
./cearch search --db ~/.local/state/cearch/index notes

# Search exact case-sensitive text in file contents (freshly reads candidate files)
./cearch content --db ~/.local/state/cearch/index 'needle'
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/USAGE.md](docs/USAGE.md).

## Design constraints

- C23, CLI-first, suitable for Gentoo and Arch Linux.
- Dense file IDs and offsets instead of per-entry heap object graphs.
- Name search via trigram postings and exact verification.
- Filesystem notifications are hints, not a source of truth. Reconciliation recovers missed events.
- Content hits are verified against current file contents before being returned.
- No unsupported claim that an index is perfectly current merely because inotify is enabled.

## Status

The initial implementation is deliberately conservative: one-shot index/search commands, durable portable records, trigram candidate generation, filters, content verification, and tests. A long-running daemon, concurrent query workers, durable inverted content index, compressed mmap snapshots, and robust watch management are follow-on work and are documented as such.