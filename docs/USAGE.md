# Usage

## Build

```sh
make
make test
```

## Index snapshot

```sh
cearch index --db DB ROOT [ROOT ...]
cearch search --db DB [--limit N] [--path] QUERY
cearch content --db DB [--limit N] QUERY
cearch stats --db DB
```

The indexer recursively walks each root without following directory symlinks. Hidden files are included. Traversal failures fail the indexing operation so an incomplete scan is not silently presented as a complete snapshot. The snapshot is written to a temporary file, synced, atomically renamed into place, and the containing directory is synced before indexing reports success.

Name search matches case-insensitively against the basename by default. Use `--path` to match the full path. Short queries use a scan; longer queries use trigram postings to reduce candidate verification. Results are newline-delimited by default; pass `--null` to CLI or daemon-client `search`/`content` commands for NUL-delimited output that safely represents filenames containing newlines.

Content search reads indexed regular files afresh and verifies literal matches against current file bytes. Files containing NUL bytes are skipped. This is not a persistent content inverted index.

## Daemon

Start the daemon in the foreground:

```sh
cearchd serve --db DB [--socket PATH] [--root ROOT ...]
```

Example:

```sh
Root="$HOME/Documents"
Db="$HOME/.local/state/cearch/index.db"
mkdir -p "$(dirname "$Db")"
cearch index --db "$Db" "$Root"
cearchd serve --db "$Db" --root "$Root"
```

The default socket is `$XDG_RUNTIME_DIR/cearch.sock`. The socket is created with owner-only permissions. If `XDG_RUNTIME_DIR` is unset, pass an explicit `--socket PATH`. The daemon does not detach; manage it with a terminal, service manager, or supervisor.

With one or more `--root` arguments, the daemon installs recursive inotify watches. Events mark the snapshot dirty and trigger a full scan; a periodic full reconciliation runs every 30 seconds as a backstop. If the inotify queue overflows, reconciliation is forced. If a scan fails, the active in-memory snapshot is retained. Without `--root`, the daemon serves the snapshot read-only and does not automatically refresh it.

Run clients from another terminal:

```sh
cearchd client search notes
cearchd client search --path /home/me/Documents
cearchd client content 'literal phrase'
cearchd client --limit 50 search report
cearchd client stats
cearchd client ping
cearchd client shutdown
```

When using a custom socket, provide the same path to each client:

```sh
cearchd client --socket /tmp/cearch.sock search notes
```

The protocol is local to the host and versioned. Request lengths and result payload sizes are bounded. The current daemon processes one connection at a time; expensive content searches can delay other clients.

## Exit status

- `0`: the command succeeded. A search with no matches also returns zero.
- `1`: runtime, database, connection, or query error.
- `2`: usage or argument error.

## Database and recovery

The snapshot is a versioned binary file containing a fixed-size metadata record table and a NUL-terminated path string table. It is written to a temporary file, synced, and atomically renamed into place. Keep the database in a private state directory, preferably outside roots being indexed. If an index or daemon is interrupted, rerun `cearch index` or restart the daemon with the same database and roots.