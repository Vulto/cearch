#!/bin/sh
set -eu
Daemon=${1:-./cearchd}
TempDir=$(mktemp -d)
DaemonPid=
SlowPid=
cleanup() {
    if [ -n "$SlowPid" ]; then kill "$SlowPid" 2>/dev/null || true; wait "$SlowPid" 2>/dev/null || true; fi
    if [ -n "$DaemonPid" ]; then kill "$DaemonPid" 2>/dev/null || true; wait "$DaemonPid" 2>/dev/null || true; fi
    rm -rf "$TempDir"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$TempDir/root/sub" "$TempDir/run" "$TempDir/state"
printf 'daemon_query_marker\n' > "$TempDir/root/needle.txt"
printf 'other contents\n' > "$TempDir/root/sub/readme.txt"
OddPath="$TempDir/root/odd
name.txt"
printf 'daemon_newline_payload\n' > "$OddPath"
printf 'exact fuzzy result\n' > "$TempDir/root/report.txt"
printf 'typo fuzzy result\n' > "$TempDir/root/reprot.txt"
printf 'large_fallback_token\n' > "$TempDir/root/large.txt"
yes 'padding' 2>/dev/null | head -c 600000 >> "$TempDir/root/large.txt"
./cearch index --db "$TempDir/state/index.db" "$TempDir/root"
"$Daemon" serve --db "$TempDir/state/index.db" --socket "$TempDir/run/cearch.sock" --root "$TempDir/root" &
DaemonPid=$!
Attempts=0
until "$Daemon" client --socket "$TempDir/run/cearch.sock" ping >/dev/null 2>&1; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 50 ]; then echo 'daemon failed to start' >&2; exit 1; fi
    sleep 0.1
done
./tests/slow_client "$TempDir/run/cearch.sock" "$TempDir/slow-ready" &
SlowPid=$!
Attempts=0
until [ -f "$TempDir/slow-ready" ]; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 50 ]; then echo 'slow client failed to connect' >&2; exit 1; fi
    sleep 0.02
done
timeout 1 "$Daemon" client --socket "$TempDir/run/cearch.sock" ping > /dev/null
printf 'during_slow_client_marker\n' > "$TempDir/root/during-slow.txt"
Attempts=0
until timeout 0.25s "$Daemon" client --socket "$TempDir/run/cearch.sock" search during-slow > "$TempDir/during-slow-search" &&
      grep -q '/during-slow.txt$' "$TempDir/during-slow-search"; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 20 ]; then echo 'reconciliation blocked by a partial client request' >&2; exit 1; fi
    sleep 0.1
done
wait "$SlowPid"
SlowPid=
ClientPids=""
Index=1
while [ "$Index" -le 16 ]; do
    "$Daemon" client --socket "$TempDir/run/cearch.sock" search needle > "$TempDir/concurrent-$Index" &
    ClientPids="$ClientPids $!"
    Index=$((Index + 1))
done
for ClientPid in $ClientPids; do wait "$ClientPid"; done
Index=1
while [ "$Index" -le 16 ]; do
    grep -q '/needle.txt$' "$TempDir/concurrent-$Index"
    Index=$((Index + 1))
done
"$Daemon" client --socket "$TempDir/run/cearch.sock" search needle > "$TempDir/search"
grep -q '/needle.txt$' "$TempDir/search"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search 'prefix:needle' > "$TempDir/prefix"
grep -q '/needle.txt$' "$TempDir/prefix"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search 'regex:^needle[.]txt$' > "$TempDir/regex"
grep -q '/needle.txt$' "$TempDir/regex"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search fuzzy:report > "$TempDir/fuzzy"
test "$(wc -l < "$TempDir/fuzzy")" -eq 2
test "$(sed -n '1p' "$TempDir/fuzzy")" = "$TempDir/root/report.txt"
test "$(sed -n '2p' "$TempDir/fuzzy")" = "$TempDir/root/reprot.txt"
"$Daemon" client --socket "$TempDir/run/cearch.sock" --limit 1 search fuzzy:report > "$TempDir/fuzzy-limit"
test "$(wc -l < "$TempDir/fuzzy-limit")" -eq 1
test "$(cat "$TempDir/fuzzy-limit")" = "$TempDir/root/report.txt"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search ext:txt > "$TempDir/ext"
grep -q '/needle.txt$' "$TempDir/ext"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search "in:$TempDir/root/sub" > "$TempDir/in-filter"
grep -q '/sub/readme.txt$' "$TempDir/in-filter"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search 'needle AND txt' > "$TempDir/and"
grep -q '/needle.txt$' "$TempDir/and"
printf '%s\0' "$OddPath" > "$TempDir/expected-null"
"$Daemon" client --socket "$TempDir/run/cearch.sock" search --null odd > "$TempDir/actual-null"
cmp "$TempDir/expected-null" "$TempDir/actual-null"
"$Daemon" client --socket "$TempDir/run/cearch.sock" content daemon_query_marker > "$TempDir/content"
grep -q '/needle.txt$' "$TempDir/content"
"$Daemon" client --socket "$TempDir/run/cearch.sock" content large_fallback_token > "$TempDir/large-content"
grep -q '/large.txt$' "$TempDir/large-content"
"$Daemon" client --socket "$TempDir/run/cearch.sock" stats > "$TempDir/stats"
grep -q '^entries: 9$' "$TempDir/stats"
printf 'live_reconciliation_marker\n' > "$TempDir/root/new-arrival.txt"
Attempts=0
until "$Daemon" client --socket "$TempDir/run/cearch.sock" search new-arrival > "$TempDir/live-search" &&
      grep -q '/new-arrival.txt$' "$TempDir/live-search"; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 100 ]; then echo 'daemon did not reconcile a new file' >&2; exit 1; fi
    sleep 0.1
done
"$Daemon" client --socket "$TempDir/run/cearch.sock" content live_reconciliation_marker > "$TempDir/live-content"
grep -q '/new-arrival.txt$' "$TempDir/live-content"
"$Daemon" client --socket "$TempDir/run/cearch.sock" stats > "$TempDir/live-stats"
grep -q '^entries: 10$' "$TempDir/live-stats"
rm "$TempDir/root/needle.txt"
Attempts=0
until "$Daemon" client --socket "$TempDir/run/cearch.sock" search needle > "$TempDir/removed-search" &&
      test ! -s "$TempDir/removed-search"; do
    Attempts=$((Attempts + 1))
    if [ "$Attempts" -ge 100 ]; then echo 'daemon did not reconcile a deleted file' >&2; exit 1; fi
    sleep 0.1
done
"$Daemon" client --socket "$TempDir/run/cearch.sock" stats > "$TempDir/removed-stats"
grep -q '^entries: 9$' "$TempDir/removed-stats"
"$Daemon" client --socket "$TempDir/run/cearch.sock" shutdown >/dev/null
wait "$DaemonPid"
DaemonPid=
echo 'cearch daemon tests passed'
