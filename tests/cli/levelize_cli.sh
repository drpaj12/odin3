#!/usr/bin/env bash
# levelize_cli.sh ODIN3 BLIF OUT_DIR [refuse]: read_blif, levelize, write_json through the CLI.
# Without "refuse" the JSON must carry the odin3_level attribute on cells and nets and a second
# run must give the same file; with it levelize must fail on the word-level cell and write nothing.
set -euo pipefail
[[ $# -ge 3 ]] || { echo "usage: levelize_cli.sh ODIN3 BLIF OUT_DIR [refuse]" >&2; exit 2; }
odin3=$1
blif=$2
out=$3
mkdir -p "$out"
name=$(basename "$blif" .blif)
lib=$(dirname "$0")/../../lib/vtr.o3lib
if [[ ${4:-} == refuse ]]; then
    if "$odin3" -p "read_techlib $lib; read_blif $blif; levelize" >"$out/$name.log" 2>&1; then
        echo "levelize must refuse a word-level cell" >&2
        exit 1
    fi
    grep -q "word-level cell" "$out/$name.log" || { echo "no refusal text" >&2; exit 1; }
    exit 0
fi
"$odin3" -p "read_techlib $lib; read_blif $blif; levelize; write_json $out/$name.1.json" \
    >"$out/$name.log" 2>&1
grep -q "levelize: 1 modules" "$out/$name.log" || { echo "no summary line" >&2; exit 1; }
grep -q '"odin3_level"' "$out/$name.1.json" || { echo "no odin3_level in the JSON" >&2; exit 1; }
"$odin3" -p "read_techlib $lib; read_blif $blif; levelize; levelize; write_json $out/$name.2.json" \
    >/dev/null 2>&1
cmp -s "$out/$name.1.json" "$out/$name.2.json" || { echo "second run differs" >&2; exit 1; }
