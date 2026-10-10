# Quality Audit

**Scope:** repository source and tests on `main` at audit time. This is a source-level audit; findings are not claims that every defect has been reproduced at runtime.

## Executive summary

Cearch is a Linux-first C23 command-line indexer/searcher. The current implementation is a single-process, one-shot tool backed by a memory-mapped, versioned binary snapshot. It is not yet the daemon-backed, continuously reconciled, persistent inverted-index search service described by the target architecture.

The existing smoke test covers a happy-path temporary tree, basic case-insensitive name search, path search, content search, stats, and a changed file after indexing. It does not currently establish malformed-database safety, atomic-write failure behavior, filesystem race behavior, content chunk-boundary behavior, limits, symlink policy, or performance.

## Confirmed implementation facts

- `src/cearch.c` performs a recursive walk, records metadata and paths, and loads the full snapshot into heap-allocated entries for each query.
- `src/index.c` builds an in-memory trigram posting index for each name search. It is rebuilt from all indexed paths per invocation; it is not persisted.
- `src/store.c` writes a native-layout binary header and records, then uses a temporary file, `fsync`, and `rename`. The format currently has no explicit byte order or ABI-independent field encoding.
- Content search reads current regular-file contents. It is a fresh scan, not a persistent content index.
- The CLI emits newline-delimited paths. A filename containing a newline is therefore ambiguous in output.
- The only existing integration suite is `tests/smoke.sh`; CI previously built and ran that suite with GCC only.
- The architecture documents describe the daemon, watcher/reconciliation, persistent postings, advanced query language, and scalable snapshot format as future work, not implemented features.

## Findings and priorities

### P0 — Snapshot path terminator validation can read beyond the mapped string table

In `CearchStorePath`, the bounds condition permits `PathOffset + PathLength == StringsSize`, then reads `Strings[StringsSize]`. That byte is outside the mapped string table and can be outside the mapping entirely. The writer produces NUL-terminated paths, but a malformed or hostile database can exercise this reader. Validation must require the terminator position to be strictly inside the string table, and the loader should validate every path before exposing the snapshot.

### P1 — Snapshot durability is not fully crash-durable

The snapshot file is synced before rename, but the containing directory is not synced after rename. A successful return therefore does not guarantee that the renamed directory entry survives sudden power loss on all filesystems. The temporary filename is PID-based and opened with `O_EXCL`; a stale temporary file can make indexing fail. These are reliability limitations, not evidence that ordinary rename replacement is non-atomic.

### P1 — Filesystem traversal errors can produce an apparently successful incomplete index

Stat/open/readdir failures are printed but traversal continues and can still return success. This behavior may be useful for best-effort indexing, but callers cannot distinguish a complete scan from a partial scan using the exit code alone. The CLI needs an explicit and documented policy: fail on traversal errors or return a distinct partial-success status/report.

### P1 — The in-memory index representation uses incompatible struct types

`src/cearch.c` casts an array of private `FileEntry` objects to `CearchFileEntry *` before the index code reads it. The current fields are laid out identically, but the types are distinct; accessing an object through an incompatible struct type is not a sound C aliasing contract. Move the shared entry type into a header and use that exact type in both modules.

### P1 — Content scan has underspecified error and binary semantics

Unreadable files and read errors are silently treated as non-matches. A NUL byte anywhere in a read chunk causes the file to be skipped, even if a matching literal occurs earlier in that same chunk; this is consistent with a conservative text-only policy but should be explicit and tested. A read error after partial data is currently indistinguishable from EOF.

### P2 — Search performance is not yet the target architecture

Each search loads all records, allocates path copies, builds all path trigrams, sorts all trigram pairs, and then scans entries. The trigram structure reduces some verification work but is rebuilt on every invocation and candidate checks are performed per entry. This should not be described as a persistent index or as low-latency search over millions of paths without benchmarks.

### P2 — Snapshot format is native-layout, not portable

The file stores C structs directly, including host endianness and compiler ABI layout. Versioning detects some incompatibility but does not make the format portable across architectures/ABIs. Define field widths, byte order, alignment, and reserved-field validation before promising portable durable databases.

### P2 — Output and command contracts need more tests

Newline-delimited paths cannot unambiguously represent every valid Linux filename. CLI parsing, zero/large limits, empty queries, overlapping roots, symlink roots, unreadable entries, malformed snapshots, and interrupted writes need regression coverage.

## Required remediation order

1. Fix snapshot string bounds and validate malformed snapshots.
2. Share the file-entry type across modules to remove the incompatible-struct cast.
3. Add focused unit tests for trigram candidates and snapshot validation, plus integration regressions for limits, symlinks, chunk boundaries, and corrupted database files.
4. Make traversal and content-read failure semantics explicit and testable.
5. Run GCC and Clang builds, unit/integration tests, sanitizers, and static analysis in CI.
6. Benchmark representative small, medium, and large trees before changing data structures. Preserve CLI semantics with regression tests as the persistent index is introduced.

## Acceptance criteria

- Every CI job fails on compile warnings promoted to errors, test failures, sanitizer findings, or static-analysis failures.
- Unit tests exercise index/store logic without requiring a user filesystem or external dependencies.
- Integration tests use isolated temporary directories and assert exact output/exit status where output is part of the contract.
- Documentation labels implemented, partially implemented, and planned behavior separately.
- Performance claims include dataset shape, hardware, command, warm/cold-cache conditions, and repeatable measurements.
- CI passing is evidence for the checks actually run; it is not proof of daemon, watcher, million-file performance, or filesystem-race correctness.
