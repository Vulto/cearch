# Architecture

## Goals

cearch is a Linux-first, pure-C23, pipe-friendly file index and search utility. The data model uses dense file IDs and compact posting lists where they reduce candidate work. The repository intentionally has no required database engine or GUI dependency.

## Current snapshot

The current binary snapshot contains a versioned native-layout header, fixed-size metadata records, and a NUL-terminated path string table. It is written through a temporary file, synced, and atomically renamed. Readers validate header sizes, offsets, record types, string bounds, terminators, and embedded NUL bytes before exposing entries. The format is currently host-endian/native-layout and is not promised to be portable between different ABIs or architectures.

The shared `CearchBuildSnapshot` path is used by the CLI index command and daemon reconciliation, keeping the traversal and serialization policy consistent. Failed scans do not replace the previous snapshot. The daemon mmaps the snapshot and references paths in the mapped string table; its metadata records are represented by compact in-memory entries.

## Name search

For query strings at least three bytes long, cearch produces unique ASCII-lowercased byte trigrams, finds postings, and uses posting membership to reject candidates before literal case-insensitive verification. Short queries use a scan. The index stores postings for all indexed paths, so basename matches remain candidates; the original query always verifies the final result.

The snapshot stores sorted trigram metadata, delta-coded numeric file IDs, and skip checkpoints. Queries attach these arrays directly from the read-only mapping and decode candidate IDs without rebuilding the full posting index. The daemon retains the mapped snapshot across requests. The content index is not persistent: content queries read the current contents of indexed regular files.

## Daemon and protocol

`cearchd` serves bounded, versioned request/response messages over a Unix-domain stream socket. The socket is owner-only, and clients cannot send arbitrary path offsets or pointers. The daemon supports name search, content search, stats, ping, and shutdown. It is currently a single-threaded foreground process; a long-running content search can delay subsequent clients. Multi-client concurrency and load testing remain future work.

## Filesystem reconciliation

When started with `--root ROOT` arguments, the daemon adds recursive inotify watches. Events do not directly mutate metadata; they mark the snapshot dirty and trigger a full rescan. Queue overflow also marks the index dirty. A periodic full reconciliation every 30 seconds is the correctness backstop for events missed during watch setup/reset. A successful rebuild atomically replaces the file and the daemon swaps to a freshly loaded mapping/index. If the scan or reload fails, the current in-memory snapshot remains in service.

Without explicit roots, the daemon is read-only and does not infer root ownership from indexed paths.

## Query semantics and planned grammar

Name queries support case-insensitive substring terms, quoted phrases, `exact:`, `prefix:`, `suffix:`, POSIX extended `regex:` terms, metadata filters `ext:`, `type:`, `size:`, `mtime:`, `path:`, `in:`, boolean `NOT`/`AND`/`OR`, and parentheses. Precedence is `NOT`, then `AND`, then `OR`; operators are explicit. Simple terms use persisted trigram postings. Compound expressions and metadata filters currently scan paths and evaluate the AST, so complex-query performance requires benchmarking. Content queries remain literal, case-sensitive scans.

Fuzzy ranking remains planned. Regex terms are compiled once per query and malformed patterns are rejected. The parser rejects malformed expressions and bounds query length/token count rather than silently reinterpreting invalid input.

## Next architecture stages

1. Establish repeatable benchmark baselines and profile indexing, name search, and content search before making performance claims.
2. Add a persistent content candidate index with correct handling for edits, deletes, binary files, and interrupted updates.
3. Add bounded multi-client processing, deadlines/cancellation, protocol fuzzing, and load tests.
4. Evaluate an append-only update journal and snapshot compaction against measured update costs; keep full reconciliation as the correctness fallback.
5. Expand filesystem race tests, failure injection for snapshot writes, and malformed database fuzzing.
6. Evaluate portable on-disk encoding and packaging/service integration only after compatibility and deployment requirements are defined.

## Non-goals

No GUI, terminal UI dependency, root requirement, kernel module, or mandatory third-party database. Performance claims require recorded hardware, a reproducible dataset, warm/cold cache context, and benchmark results.