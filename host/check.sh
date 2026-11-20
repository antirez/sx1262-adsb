#!/bin/sh
# Each row contains the five replay inputs followed by a reference word or "-".
# Give only the first five columns to the decoder. Compare afterwards so that
# a missing packet, an extra one, or a single wrong bit all fail the test.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
replay=${1:-"$here/build/replay"}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for set_name in garden-and-bench bench-test morning techo-bench; do
    gzip -dc "$here/fixtures/$set_name.tsv.gz" > "$tmp/rows"
    cut -f1-5 "$tmp/rows" > "$tmp/input"
    awk -F '\t' '$6 != "-" { print $1, $6 }' "$tmp/rows" > "$tmp/expected"
    "$replay" --bench < "$tmp/input" > "$tmp/results"
    cut -d' ' -f1,2 "$tmp/results" > "$tmp/actual"
    diff -u "$tmp/expected" "$tmp/actual"
    "$replay" --bench --busy < "$tmp/input" > "$tmp/filtered"
    diff -u "$tmp/results" "$tmp/filtered"
    bench=$(wc -l < "$tmp/actual")
    # Outside bench mode the same captures yield every word except TEST.
    # TEST words are DF2: their first byte is 0x10-0x17 (00010xxx).
    awk '$2 !~ /^1[0-7]/' "$tmp/expected" > "$tmp/aircraft"
    "$replay" < "$tmp/input" | cut -d' ' -f1,2 > "$tmp/actual"
    diff -u "$tmp/aircraft" "$tmp/actual"
    printf '%s: %d captures, %d packets in bench mode, %d without\n' "$set_name" "$(wc -l < "$tmp/rows")" "$bench" "$(wc -l < "$tmp/actual")"
done

# The illustrated position pair is also the plain-text example for replay.
"$replay" < "$here/../docs/4baa79-position.tsv" > "$tmp/results"
cut -d' ' -f1,2 "$tmp/results" > "$tmp/actual"
cat > "$tmp/expected" <<'END'
233323 8d4baa79580db4913fe0a3be306b
233353 8d4baa79580db0fbd3f6267aeda9
END
diff -u "$tmp/expected" "$tmp/actual"
printf 'illustrated position pair: 2 captures, 2 packets\n'

# This timed coax packet first failed on the T-Echo. The input contains RSSI
# only. Its independent RTL-SDR word belongs on the comparison side.
"$replay" < "$here/../docs/1090-block-871.tsv" > "$tmp/results"
cut -d' ' -f1,2 "$tmp/results" > "$tmp/actual"
echo '871 8d000000c8540afe9ffe693c94ea' > "$tmp/expected"
diff -u "$tmp/expected" "$tmp/actual"

# All sixteen arrivals have an independent transmitted word. Future decoders
# may recover more of them. Check every returned word, and keep the ten that
# motivated the whole-reply timing pass as a regression floor.
gzip -dc "$here/fixtures/techo-1090-quiet.tsv.gz" > "$tmp/rows"
cut -f1-5 "$tmp/rows" > "$tmp/input"
"$replay" < "$tmp/input" > "$tmp/results"
awk 'NR==FNR { expected[$1]=$6; next }
     !($1 in expected) || expected[$1]!=$2 || seen[$1]++ { bad=1 }
     END {
         n=split("1 3 5 6 7 8 9 12 13 14",required," ")
         for (i=1;i<=n;i++) if (!seen[required[i]]) bad=1
         exit bad
     }' "$tmp/rows" "$tmp/results"
