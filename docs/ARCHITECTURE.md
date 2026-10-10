# Architecture

## Goals

cearch is a Linux-first, pure-C23, pipe-friendly file search utility. It separates file metadata from query logic and uses numeric IDs for index references. The first release establishes the data model and correctness rules before adding daemon concurrency and advanced indexes.

## Data-oriented storage

The persistent baseline stores newline-delimited, escaped path records. The in-memory query representation is a compact set of records containing a path, basename range, size, modification time, device/inode identity, and file type. This baseline format is intentionally easy to inspect and recover; it is not presented as the final compact mmap format.

The target storage format is a versioned snapshot with:
- a dense FileId table;
- a shared path/name string blob;
- parent IDs and name offsets;
- sorted trigram -> FileId posting lists, delta-encoded;
- metadata columns and a journal;
- atomic snapshots plus append-only deltas.

Full paths should eventually be reconstructed from parent/name data so sibling paths do not repeatedly store the same prefix.

## Search

For names at least three bytes long, generate unique, lowercase byte trigrams from the query. Intersect candidate postings beginning with the shortest posting list, then verify the original query against candidate names/paths. Short queries fall back to a bounded scan because they provide too little trigram selectivity. Fuzzy ranking must be applied after candidate reduction, not across the whole filesystem.

The first baseline uses a compact sorted record collection and substring verification. The target trigram posting index can be added without changing CLI semantics.

## Content correctness

Content search reads current file bytes and checks them before reporting matches. A persistent content index, when introduced, will be a candidate generator only: metadata validation and a fresh read verify hits. Binary files and unreadable files are skipped by default, with errors optionally reported to stderr.

## Watcher correctness

inotify is not a lossless journal: queues can overflow, watches are not inherently recursive, and changes can race with watch installation. Events only mark entries/roots dirty and accelerate re-indexing. An overflow marks affected roots dirty and triggers reconciliation. Periodic or on-demand reconciliation remains the correctness backstop.

## Daemon target

The planned daemon owns the current index and serves bounded binary requests over a Unix-domain socket. The protocol must validate all lengths, enforce per-client limits, and avoid trusting paths or offsets received from clients. Systemd user units and OpenRC examples will be optional packaging aids. The first release is a one-shot CLI so it can be compiled and tested without a service manager.

## Query language target

Future grammar:

- `ext:c`, `type:file`, `type:dir`
- `size:>10mb`, `mtime:<7d`, `path:/home/me`
- `in:/some/root`, `exact:`, `prefix:`, `suffix:`, `fuzzy:`, `regex:`
- Boolean `AND`, `OR`, `NOT`, parentheses, and exclusion terms.

Parsing must reject malformed expressions rather than silently changing their meaning.

## Non-goals for the baseline

No GUI, terminal UI dependency, root requirement, kernel module, or mandatory database engine. Performance claims require repeatable benchmark results on recorded hardware and datasets.