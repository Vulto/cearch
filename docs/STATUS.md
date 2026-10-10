# Implementation status

## Implemented and CI-tested

- C23-compatible Linux CLI (build flag `-std=gnu2x` for GCC 13 compatibility).
- Recursive non-symlink-following index scan over one or multiple roots.
- Atomic text snapshot replacement with file flush/fsync and rename.
- Escaped path serialization and record format versioning.
- Case-insensitive basename search and optional full-path search.
- Fresh literal content scan of indexed regular files.
- File/directory statistics.
- Smoke tests in GitHub Actions.
- No mandatory third-party library.

## Not yet the complete target architecture

This snapshot is a foundational baseline, not the fully specified high-performance search daemon. It does not yet implement:

- daemon/client Unix socket protocol;
- inotify watcher with overflow recovery and reconciliation loop;
- memory-mapped binary snapshot and WAL/base-delta compaction;
- filename/path trigram inverted index;
- persistent content token/trigram inverted index;
- fuzzy scoring or typo-tolerant edit distance;
- query AST/Boolean operators/regex or metadata filters;
- multi-threaded query service and load testing.

The next engineering stage should replace the O(N) scan and text database format with a versioned mmap snapshot and compact posting lists, while retaining the existing CLI contract and regression tests. Do not describe this baseline as meeting millisecond queries over millions of files until benchmarks prove it.