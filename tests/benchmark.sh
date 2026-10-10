#!/bin/sh
set -eu

Program=${1:-./cearch}
FileCount=${CEARCH_BENCH_FILES:-10000}
case "$FileCount" in
    ''|*[!0-9]*) echo "CEARCH_BENCH_FILES must be a positive integer" >&2; exit 2 ;;
esac
if [ "$FileCount" -lt 1 ]; then
    echo "CEARCH_BENCH_FILES must be a positive integer" >&2
    exit 2
fi

TempDir=$(mktemp -d)
trap 'rm -rf "$TempDir"' EXIT HUP INT TERM
Root="$TempDir/root"
Db="$TempDir/index.db"
mkdir -p "$Root"

Index=1
while [ "$Index" -le "$FileCount" ]; do
    Directory=$(printf '%03d' $(((Index - 1) % 100)))
    mkdir -p "$Root/$Directory"
    printf 'benchmark payload file=%08d searchable_token_%03d\n' "$Index" "$((Index % 997))" > "$Root/$Directory/document-$Index.txt"
    Index=$((Index + 1))
done

elapsed_ms() {
    Start=$(date +%s%N)
    "$@"
    End=$(date +%s%N)
    printf '%s' "$(((End - Start) / 1000000))"
}

printf 'cearch benchmark (warm-cache query measurements)\n'
printf 'date: %s\n' "$(date -Is)"
printf 'system: %s\n' "$(uname -a)"
printf 'compiler: '
${CC:-cc} --version | sed -n '1p'
printf 'files: %s\n' "$FileCount"
printf 'dataset: generated text files in up to 100 directories; each file has one deterministic payload line\n'

printf 'index_ms: '
IndexMs=$(elapsed_ms "$Program" index --db "$Db" "$Root")
printf '%s\n' "$IndexMs"

printf 'name_search_ms: '
SearchMs=$(elapsed_ms "$Program" search --db "$Db" "document-$FileCount")
printf '%s\n' "$SearchMs"

printf 'content_scan_ms: '
ContentMs=$(elapsed_ms "$Program" content --db "$Db" "searchable_token_997")
printf '%s\n' "$ContentMs"

printf 'Note: timings are single-run wall-clock measurements, not statistically robust; query runs follow indexing and may use warm filesystem cache. Repeat runs and record machine/cache conditions before comparing changes.\n'
