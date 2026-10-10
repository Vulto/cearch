# Usage

## Commands

```sh
cearch index --db DB ROOT [ROOT ...]
cearch search --db DB [--limit N] [--path] QUERY
cearch content --db DB [--limit N] QUERY
cearch stats --db DB
cearch help
```

The index command recursively walks each root without following directory symlinks. It records regular files and directories and skips unreadable entries while continuing the scan. Hidden files are included. Use explicit roots to keep indexing predictable.

Search matches query text case-insensitively against the basename by default. Add `--path` to match against the full path. Results are newline-delimited paths and are suitable for shell pipelines, but paths containing literal newline characters cannot be represented unambiguously in this initial text output mode.

Content search reads indexed regular files fresh and reports paths containing the literal query. It is not yet a persistent content inverted index; the architecture document describes the planned indexed implementation. NUL-containing files are skipped.

## Exit status

- `0`: command succeeded (search may return zero matches).
- `1`: usage, filesystem, database, or runtime error.

## Database

The database is a versioned text snapshot. Re-running `index` rebuilds it atomically using a temporary file and rename, so interruption does not intentionally replace a good prior snapshot with a partial one. Use a private directory such as `~/.local/state/cearch`.

## Service operation

The initial release does not run a background daemon. It intentionally avoids pretending a one-shot index is continuously live. Daemon/client, inotify reconciliation, and persistent trigram/content postings are follow-on milestones.