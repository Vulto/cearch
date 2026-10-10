#!/bin/sh
set -eu

Program=${1:-./cearch}
FileCount=${CEARCH_BENCH_FILES:-10000}
Repeats=${CEARCH_BENCH_REPEATS:-5}
for Value in "$FileCount" "$Repeats"; do
    case "$Value" in
        ''|*[!0-9]*) echo "benchmark counts must be positive integers" >&2; exit 2 ;;
    esac
    if [ "$Value" -lt 1 ]; then echo "benchmark counts must be positive integers" >&2; exit 2; fi
done

TempDir=$(mktemp -d)
trap 'rm -rf "$TempDir"' EXIT HUP INT TERM
Root="$TempDir/root"
Db="$TempDir/index.db"
mkdir -p "$Root"

Index=1
while [ "$Index" -le "$FileCount" ]; do
    Directory=$(printf '%03d' $(((Index - 1) % 100)))
    mkdir -p "$Root/$Directory"
    printf 'benchmark payload file=%08d searchable_token_%03d repeated content phrase\n' \
        "$Index" "$((Index % 997))" > "$Root/$Directory/document-$Index.txt"
    Index=$((Index + 1))
done

measure_ms() {
    Start=$(date +%s%N)
    "$@" >/dev/null
    End=$(date +%s%N)
    printf '%s' "$(((End - Start) / 1000000))"
}

measure_repeated() {
    Label=$1
    shift
    "$@" >/dev/null
    Samples=""
    Index=1
    while [ "$Index" -le "$Repeats" ]; do
        Value=$(measure_ms "$@")
        if [ -n "$Samples" ]; then Samples="$Samples $Value"; else Samples=$Value; fi
        Index=$((Index + 1))
    done
    Sorted=$(printf '%s\n' "$Samples" | tr ' ' '\n' | sort -n)
    Median=$(printf '%s\n' "$Sorted" | awk '
        { Values[NR] = $1 }
        END {
            if (NR % 2) print Values[(NR + 1) / 2];
            else printf "%.1f\n", (Values[NR / 2] + Values[NR / 2 + 1]) / 2;
        }')
    printf '%s_ms_median: %s\n' "$Label" "$Median"
    printf '%s_ms_samples: %s\n' "$Label" "$(printf '%s\n' "$Samples" | tr ' ' ',')"
}

printf 'cearch benchmark (warm-cache repeated measurements)\n'
printf 'date: %s\n' "$(date -Is)"
printf 'system: %s\n' "$(uname -a)"
printf 'cpu_count: %s\n' "$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf unknown)"
printf 'compiler: '
${CC:-cc} --version | sed -n '1p'
printf 'files: %s\n' "$FileCount"
printf 'query_repeats: %s\n' "$Repeats"
printf 'dataset: deterministic small text files across up to 100 directories\n'

Start=$(date +%s%N)
if [ -x /usr/bin/time ]; then
    /usr/bin/time -f '%M' -o "$TempDir/index-rss-kb" "$Program" index --db "$Db" "$Root"
    PeakRss=$(cat "$TempDir/index-rss-kb")
else
    "$Program" index --db "$Db" "$Root"
    PeakRss=unavailable
fi
End=$(date +%s%N)
printf 'index_ms: %s\n' "$(((End - Start) / 1000000))"
printf 'index_peak_rss_kb: %s\n' "$PeakRss"
printf 'snapshot_bytes: %s\n' "$(wc -c < "$Db" | tr -d ' ')"

measure_repeated name_search "$Program" search --db "$Db" "document-$FileCount"
measure_repeated regex_search "$Program" search --db "$Db" 'regex:^document-[0-9]+[.]txt$'
measure_repeated fuzzy_search "$Program" search --db "$Db" fuzzy:documemt
measure_repeated content_index_search "$Program" content --db "$Db" searchable_token_001
measure_repeated content_short_scan "$Program" content --db "$Db" xy

printf 'content_index: bounded persistent trigram candidate index; stale/unindexed files use correctness fallback\n'
printf 'Note: timings are warm-cache samples. Compare only runs with the same dataset, compiler flags, machine, and filesystem conditions. Increase CEARCH_BENCH_FILES to test larger trees; large runs can take substantial time and disk space.\n'
