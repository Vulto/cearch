# Cearch Engineering Contract

This file is authoritative for coding agents working in this repository. If source code, old comments, or older documentation conflict with verified behavior, update the documentation and tests; do not silently assume the target design is already implemented.

## Product intent

Cearch is a Linux-first C23 command-line file index/search utility. Preserve the CLI-first, dependency-light approach. Prioritize correctness and measurable performance over claims or speculative architecture.

## Current reality

- The application is a one-shot CLI, not a daemon/client service.
- Indexing walks the filesystem and writes a versioned binary snapshot.
- The snapshot uses native C struct layout and host endianness; it is not a portable interchange format.
- Name-search trigram postings are built in memory for each invocation; they are not persisted.
- Content search reads current regular-file bytes; it is not backed by a persistent content index.
- There is no recursive inotify service or reconciliation loop.
- Newline-delimited output cannot unambiguously represent filenames containing newlines.
- Check `docs/STATUS.md` and `docs/AUDIT.md` before planning work. Planned features must remain labelled planned until implemented and tested.

## Architecture rules

1. Use C23 and Linux/POSIX interfaces; do not add mandatory third-party runtime dependencies without a documented reason.
2. Keep shared data types in headers and use the same declared type across translation units. Do not cast between structurally similar but incompatible structs.
3. Check arithmetic before allocation, offset computation, and file-size calculations.
4. Treat the database as untrusted input. Validate the entire header, record bounds, string terminators, record types, and all referenced offsets before use.
5. Preserve atomic snapshot replacement. Document durability guarantees precisely; syncing the file alone does not make the directory entry crash-durable.
6. Filesystem notifications, when implemented, are hints. Reconciliation must be the correctness mechanism.
7. Trigram matches are candidates only. Verify the actual query against the target name/path before reporting.
8. Keep output and exit-status semantics documented and covered by integration tests.
9. Do not make performance claims without repeatable benchmarks with recorded datasets and conditions.

## Required verification

Before considering a change complete, run:

```sh
make clean all
make test
make analyze
make sanitize
```

CI must pass GCC and Clang builds with warnings as errors, all unit/integration tests, and AddressSanitizer/UndefinedBehaviorSanitizer. Do not dismiss a failure as infrastructure without inspecting the failed step and its logs.

Every bug fix must include a regression test when reasonably testable. New algorithms and persistent-format invariants need focused unit tests; CLI-visible behavior needs integration coverage. Tests must be deterministic, isolated, and offline.

## Scope honesty

A passing build is not proof of correct indexing under concurrent filesystem mutation, portable database compatibility, daemon correctness, continuous freshness, or million-file performance. Mark any untested behavior as unknown. Never weaken tests merely to obtain a green status.
