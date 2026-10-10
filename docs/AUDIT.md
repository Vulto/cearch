# Quality Audit

**Scope:** current Cearch implementation, including snapshot version 4, persistent filename/content postings, fuzzy/regex queries, and bounded daemon workers. Findings below distinguish verified implementation behavior from remaining risks.

## Executive summary

Cearch is a Linux-first C23 file index/search utility with a CLI and foreground daemon. The versioned mmap snapshot persists filename trigram postings and a bounded content trigram candidate index. The daemon reconciles watched roots, verifies content matches against current bytes, and serves clients concurrently through a capped worker set.

The code now has meaningful unit and integration coverage, GCC/Clang warning-as-error builds, GCC static analysis, and ASan/UBSan in CI. The benchmark harness reports repeated query timings, indexing time, snapshot size, and peak indexing RSS. CI does not yet prove performance or correctness at million-file scale, under hostile filesystem races, or under every storage failure.

## Resolved baseline findings

- **Path terminator bounds:** snapshot loading validates string-table bounds and in-table NUL terminators before exposing entries.
- **Snapshot durability:** unique temporary files, file sync, atomic rename, and parent-directory sync are used for snapshot replacement.
- **Incomplete scans:** traversal errors prevent replacing the active snapshot.
- **Entry type aliasing:** indexing and query code use the shared CearchFileEntry type.
- **Per-query name-index rebuild:** filename trigram postings, delta-coded IDs, and skip checkpoints are persisted and attached from the mmap snapshot.
- **Content scan cost:** snapshot v4 includes bounded content trigram postings. Current bytes are verified and stale/unindexed files are scanned as fallback.
- **Single-client blocking:** daemon requests run in detached worker threads, capped at 64 active clients, with socket I/O timeouts and a read/write lock around live snapshot access.
- **CI coverage:** GCC and Clang warning-as-error builds, unit/integration tests, GCC static analysis, benchmark smoke test, and ASan/UBSan are required.

## Open findings and risks

### P1 — Content index coverage is intentionally bounded

Only regular text files up to 512 KiB each and within a 16 MiB aggregate indexing budget receive persistent content postings. Larger, over-budget, unreadable, or stale files use a correctness fallback scan. This avoids unbounded index-construction memory but can make content queries slow on large trees. Benchmark the coverage/latency tradeoff before raising limits or changing the index representation.

### P1 — Snapshot encoding is native-layout

The snapshot writes fixed-width C records in host byte order and native ABI layout. Version checks reject incompatible snapshots but do not make them portable across architectures, compiler ABIs, or endianness. Snapshot v4 also requires older database files to be rebuilt with cearch index.

### P1 — Content freshness relies on filesystem metadata

Freshness checks compare size, device/inode, and nanosecond mtime/ctime. This detects normal edits and replacements, but does not provide a cryptographic content identity if an external process can restore metadata or if a filesystem lacks sufficient timestamp precision. A content hash would improve certainty at additional indexing and storage cost.

### P2 — CLI searches still load path metadata into heap memory

The posting lists are mapped, but one-shot CLI invocations load entry metadata and allocate path copies before querying. Memory use therefore still grows with indexed entry count. Measure resident memory at representative scales before claiming suitability for tens of millions of files.

### P2 — Complex and fuzzy queries scan the entry set

Simple filename terms use persisted postings. Boolean expressions, metadata predicates, regex, and fuzzy terms scan entries; fuzzy ranking uses a bounded top-K heap, so result storage is bounded by the requested limit, but CPU cost remains linear in the index size.

### P2 — Content read errors are not surfaced as distinct results

Unreadable files and read errors do not become matches, but the CLI does not provide a per-file error report. Files containing NUL bytes are deliberately excluded from content search. This policy is documented, but additional tests should cover read failures and chunk boundaries.

### P2 — Newline output is ambiguous for valid filenames

Default output is newline-delimited. The --null option provides an unambiguous machine-readable path stream, but the default remains intended for interactive use and there is no JSON output format.

### P2 — Worker saturation and cancellation need deeper testing

The daemon caps active workers at 64 and sets 10-second socket I/O timeouts. Concurrent requests and partial-request isolation are covered by integration tests. Sustained saturation, slow readers, shutdown under load, and cancellation of expensive search work still need stress testing. The state read lock is acquired only after the full request payload is received, so a partial request does not block reconciliation; long-running searches can still delay a snapshot swap.

### P2 — Performance evidence is still incomplete

The benchmark harness is repeatable and reports samples/median, indexing time, snapshot size, and peak RSS. CI runs only a small smoke dataset. No trustworthy million-file benchmark report has yet been recorded with hardware, filesystem, compiler flags, and cache conditions.

### P2 — Failure injection and filesystem race coverage are incomplete

Dedicated tests are still needed for write/fsync/rename failures, stale temporary files, overlapping roots, symlink policy, permission changes, rapid replace/rename races, malformed snapshot fuzzing, and worker saturation.

## Required next work

1. Record repeatable small/medium/large benchmark results, including indexing time, peak RSS, snapshot bytes, name-query latency, content-query latency, and fuzzy/regex costs.
2. Tune content-index coverage based on measurements; preserve fallback correctness.
3. Add failure injection for snapshot writes and fuzz malformed snapshot inputs.
4. Stress daemon worker saturation, shutdown, and reconciliation while clients are active.
5. Decide whether portable on-disk encoding and structured output are requirements for the intended release.
6. Revisit the append-only update journal only if full reconciliation costs justify the complexity.

## Acceptance criteria

- CI fails on warnings promoted to errors, unit/integration failures, sanitizer findings, or static-analysis failures.
- Unit tests cover index and snapshot invariants without relying on the user's filesystem.
- Integration tests use temporary trees and verify observable output/exit behavior.
- Every documented limitation is either tested or explicitly marked as unverified.
- Performance claims include dataset shape, hardware, compiler flags, filesystem, cache state, and repeatable measurements.
- Passing CI is evidence only for the checks it runs; it is not proof of large-scale performance or complete filesystem-race safety.
