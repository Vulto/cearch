# Implementation status

## Implemented and verified in the current repository

- Linux-first C23-compatible CLI and foreground daemon/client.
- Recursive filesystem indexing without following directory symlinks; scan failures prevent replacing the active snapshot.
- Versioned, validated snapshot with atomic replacement, file sync, rename, and parent-directory sync.
- Persisted trigram metadata, delta-coded posting IDs, skip checkpoints, and mapped posting lookup for filename/path searches.
- Case-insensitive basename/full-path substring search with literal verification.
- Bounded query AST with quoted phrases, boolean `NOT`/`AND`/`OR`, parentheses, exact/prefix/suffix modes, and metadata filters for extension, type, size, modification age, path substring, and root containment.
- Fresh literal content scans of indexed regular files; content reads are not backed by a persistent content index.
- Unix-domain socket protocol with bounded request/response sizes, owner-only socket permissions, search/content/stats/ping/shutdown commands.
- Optional recursive inotify watches, event-triggered full scans, queue-overflow handling, and periodic reconciliation.
- Unit tests for index postings and snapshot validation, plus CLI and daemon integration tests.
- GCC and Clang CI builds with warnings as errors, GCC static analysis, and ASan/UBSan tests.

## Remaining target work

- Persistent content candidate index with current-content verification and update/recovery semantics.
- Regex and fuzzy ranking. Compound boolean queries and metadata filters currently scan paths; acceleration and broader query-language tests remain needed.
- Bounded parallel client processing, timeouts/cancellation, and load testing.
- Append-only update journal and snapshot compaction; establish whether benchmarks justify this complexity.
- Repeatable benchmark datasets and reports, protocol fuzzing, filesystem race tests, and broader failure-injection coverage.
- Structured output formats beyond newline-delimited and NUL-delimited paths.
- Portable on-disk encoding is not a promise of the current native-layout snapshot.
- Optional service-manager units and runtime packaging polish.

## Acceptance gate

Do not call the project complete until every target feature is either implemented and tested or explicitly removed from the agreed scope. Required checks are `make clean all`, `make test`, `make analyze`, and `make sanitize`; CI must enforce the compiler, test, analyzer, and sanitizer checks. Performance claims require recorded dataset shape, hardware, command, cache conditions, and repeatable measurements. Passing CI validates only the checks actually run.
