# Implementation status

## Implemented and tested

- C23-compatible Linux CLI and foreground daemon.
- Recursive non-symlink-following filesystem scan shared by initial indexing and reconciliation.
- Versioned binary snapshot with atomic replacement and mmap validation.
- Bounds, type, NUL-terminator, and embedded-NUL path validation.
- Case-insensitive basename/full-path name queries using in-memory trigram postings with literal verification.
- Fresh literal content scans of indexed regular files.
- Unix-domain socket protocol with bounded requests/responses, owner-only socket permissions, stats, ping, and shutdown.
- Optional recursive inotify watches, queue-overflow handling, event-triggered full scans, and periodic full reconciliation.
- Unit tests for trigram candidates and snapshot round-trip/corrupt input, plus CLI and daemon integration tests.
- GCC/Clang CI with warnings as errors and sanitizer coverage.

## Still required for the target architecture

- Persistent on-disk trigram posting lists and direct mmap queries.
- Delta-coded/compressed postings and compact parent/name records.
- Persistent content candidate index.
- Append-only update journal and snapshot compaction.
- Query AST, boolean operators, metadata filters, exact/prefix/suffix modes, regex, fuzzy ranking.
- Bounded parallel client processing and cancellation.
- Benchmarks over representative datasets, protocol fuzzing, and filesystem race tests.
- Optional service-manager units and install/runtime packaging polish.

CI passing validates only the checks it runs. Do not claim millisecond queries over millions of paths until reproducible benchmarks support the claim.