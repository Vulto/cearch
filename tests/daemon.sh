#!/bin/sh
set -eu
Daemon=${1:-./cearchd}
TempDir=$(mktemp -d)
DaemonPid=
cleanup() {
    if [ -n "$DaemonPid" ]; then kill "$DaemonPid" 2>/dev/null || true; wait "$DaemonPid" 2>/dev/null || true; fi
    rm -rf "$TempDir"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TempDir/root/sub" "$TempDir/run" "$TempDir/state"
printf 'daemon_query_marker\n' > "$TempDir/root/needle.txt"
printf 'other contents\n' > "$TempDir/root/sub/readme.txt"
./cearch index --db "$TempDir/state/index.db" "$TempDir/root"
"$Daemon" serve --db "$TempDir/state/index.db" --socket "$TempDir/run/cearch.sock" --root "$TempDir/root" &
DaemonPid=$!
Attempts=0
until "$Daemon" client --socket "$TempDir/run/cearch.sock" ping >/dev/null 2>&1; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 50 ]; then echo 'daemon failed to start' >&2; exit 1; fi
    sleep 0.1
done
"$Daemon" client --socket "$TempDir/run/cearch.sock" search needle > "$TempDir/search"
grep -q '/needle.txt$' "$TempDir/search"
"$Daemon" client --socket "$TempDir/run/cearch.sock" content daemon_query_marker > "$TempDir/content"
grep -q '/needle.txt$' "$TempDir/content"
"$Daemon" client --socket "$TempDir/run/cearch.sock" stats > "$TempDir/stats"
grep -q '^entries: 4"$Daemon" client --socket "$TempDir/run/cearch.sock" shutdown >/dev/null
wait "$DaemonPid"
DaemonPid=
echo 'cearch daemon tests passed'
 "$TempDir/stats"

printf 'live_reconciliation_marker\n' > "$TempDir/root/new-arrival.txt"
Attempts=0
until "$Daemon" client --socket "$TempDir/run/cearch.sock" search new-arrival > "$TempDir/live-search" &&
      grep -q '/new-arrival.txt"$Daemon" client --socket "$TempDir/run/cearch.sock" shutdown >/dev/null
wait "$DaemonPid"
DaemonPid=
echo 'cearch daemon tests passed'
 "$TempDir/live-search"; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 100 ]; then
        echo 'daemon did not reconcile a new file' >&2
        exit 1
    fi
    sleep 0.1
done
"$Daemon" client --socket "$TempDir/run/cearch.sock" content live_reconciliation_marker > "$TempDir/live-content"
grep -q '/new-arrival.txt"$Daemon" client --socket "$TempDir/run/cearch.sock" shutdown >/dev/null
wait "$DaemonPid"
DaemonPid=
echo 'cearch daemon tests passed'
 "$TempDir/live-content"
"$Daemon" client --socket "$TempDir/run/cearch.sock" shutdown >/dev/null
wait "$DaemonPid"
DaemonPid=
echo 'cearch daemon tests passed'
