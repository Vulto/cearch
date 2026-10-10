#!/bin/sh
set -eu

Program=${1:-./cearch}
TempDir=$(mktemp -d)
trap 'rm -rf "$TempDir"' EXIT HUP INT TERM

mkdir -p "$TempDir/root/sub" "$TempDir/dbdir" "$TempDir/root/folder"
printf 'hello from cearch\nsecond line\n' > "$TempDir/root/needle.txt"
printf 'unique_content_token\n' > "$TempDir/root/sub/readme.md"
printf 'nothing here\n' > "$TempDir/root/other.txt"

"$Program" index --db "$TempDir/dbdir/index.db" "$TempDir/root"
"$Program" stats --db "$TempDir/dbdir/index.db" > "$TempDir/stats"
grep -q '^files: 3$' "$TempDir/stats"
grep -q '^directories: 3$' "$TempDir/stats"

"$Program" search --db "$TempDir/dbdir/index.db" needle > "$TempDir/name"
grep -q '/needle.txt$' "$TempDir/name"

"$Program" search --db "$TempDir/dbdir/index.db" --path sub > "$TempDir/path"
grep -q '/sub/readme.md$' "$TempDir/path"

"$Program" content --db "$TempDir/dbdir/index.db" unique_content_token > "$TempDir/content"
grep -q '/sub/readme.md$' "$TempDir/content"

"$Program" search --db "$TempDir/dbdir/index.db" absent > "$TempDir/missing"
test ! -s "$TempDir/missing"

printf 'fresh content token\n' >> "$TempDir/root/other.txt"
"$Program" content --db "$TempDir/dbdir/index.db" 'fresh content token' > "$TempDir/fresh"
grep -q '/other.txt$' "$TempDir/fresh"

echo 'cearch smoke tests passed'
