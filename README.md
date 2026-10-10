# cearch

**cearch** is a Linux-first file index and search utility written in C23. It provides a CLI indexer, a persistent mmap snapshot, and an optional local daemon/client interface.

## Build and test

Requirements: Linux, GCC or Clang with C2x support, and Make.

```sh
make
make test
```

## Quick start

```sh
Root="$HOME/Documents"
Db="$HOME/.local/state/cearch/index.db"

# Build or refresh the index snapshot
mkdir -p "$Root" "$(dirname "$Db")"
./cearch index --db "$Db" "$Root"

# Start the daemon in the foreground and keep this root reconciled
./cearchd serve --db "$Db" --root "$Root"

# In another terminal
./cearchd client search notes
./cearchd client content 'literal text'
./cearchd client stats
```

The default socket is `$XDG_RUNTIME_DIR/cearch.sock`. It is restricted to the current user. Pass `--socket PATH` to both daemon and client to use a custom path.

Use [docs/USAGE.md](docs/USAGE.md) for all options and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for implementation boundaries.

## Design

- C23-compatible, Linux/POSIX APIs, no mandatory third-party libraries.
- Dense numeric file IDs, delta-coded trigram postings, and skip checkpoints stored in the mmap snapshot.
- Atomic, versioned binary snapshots with bounded mmap validation.
- The daemon keeps a warm index in memory and uses Unix-domain sockets.
- Recursive inotify watches mark the index dirty; event overflow and periodic full reconciliation are handled by rescanning roots and atomically replacing the snapshot.
- Content matches are verified by reading current file contents; the content search is not yet a persistent inverted content index.

## Current scope

The program has one-shot indexing/search and a foreground daemon with name/content queries, statistics, ping, shutdown, and optional live reconciliation. Persistent trigram postings are stored in the snapshot; content search remains a scan of current regular-file contents. An advanced query AST, fuzzy scoring, parallel client handling, and reproducible performance benchmarks remain future engineering work.