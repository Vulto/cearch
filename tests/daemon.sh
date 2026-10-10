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
"$Daemon" serve --db "$TempDir/state/index.db" --socket "$TempDir/run/cearch.sock" &
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
grep -q '^entries: 4$' "$TempDir/stats"
"$Daemon" client --socket "$TempDir/run/cearch.sock" shutdown >/dev/null
wait "$DaemonPid"
DaemonPid=
echo 'cearch daemon tests passed'
