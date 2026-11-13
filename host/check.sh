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
    "$replay" < "$tmp/input" > "$tmp/results"
    cut -d' ' -f1,2 "$tmp/results" > "$tmp/actual"
    diff -u "$tmp/expected" "$tmp/actual"
    "$replay" --busy < "$tmp/input" > "$tmp/filtered"
    diff -u "$tmp/results" "$tmp/filtered"
    printf '%s: %d captures, %d packets\n' "$set_name" "$(wc -l < "$tmp/rows")" "$(wc -l < "$tmp/actual")"
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
