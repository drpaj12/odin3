#!/usr/bin/env bash
# graphlets_cli.sh ODIN3 BLIF OUT_DIR: read_blif, graphlets through the CLI (spec
# docs/specs/2026-10-30-graphlets-design.md). Checks the summary line, the JSON file (deterministic
# apart from the "ms" figures), a --cells partition, a --focus path, --directed, and --compare of a
# file with itself (agreement 1; uncalibrated, then calibrated with the test calibration).
set -euo pipefail
[[ $# -ge 3 ]] || { echo "usage: graphlets_cli.sh ODIN3 BLIF OUT_DIR" >&2; exit 2; }
odin3=$1
blif=$2
out=$3
mkdir -p "$out"
name=$(basename "$blif" .blif)
here=$(dirname "$0")
lib=$here/../../lib/vtr.o3lib
cal=$here/graphlet_calibration_test.json
run() { "$odin3" -p "read_techlib $lib; read_blif $blif; $1" >"$out/$name.log" 2>&1; }
run "graphlets --out $out/$name.1.json --per-node"
grep -q "graphlets: 1 groups" "$out/$name.log" || { echo "no summary line" >&2; exit 1; }
grep -q '"schema": "odin3-graphlets"' "$out/$name.1.json" || { echo "no schema" >&2; exit 1; }
run "graphlets --out $out/$name.2.json --per-node"
cmp -s <(sed 's/"us": [0-9]*,//' "$out/$name.1.json") <(sed 's/"us": [0-9]*,//' "$out/$name.2.json") ||
    { echo "second run differs" >&2; exit 1; }
run "graphlets --directed --out $out/$name.d.json"
grep -q '"mode": "directed-4"' "$out/$name.d.json" || { echo "no directed mode" >&2; exit 1; }
mod=$(sed -n 's/.*"module": "\([^"]*\)".*/\1/p' "$out/$name.1.json" | head -1)
run "graphlets --focus $mod --out $out/$name.f.json"
grep -q '"source": "focus"' "$out/$name.f.json" || { echo "no focus group" >&2; exit 1; }
printf '@pair\n1\n2\n@rest\n3\n' >"$out/$name.cells"
run "graphlets --cells $out/$name.cells --halo 1 --out $out/$name.c.json"
grep -q '"name": "pair"' "$out/$name.c.json" || { echo "no cells group" >&2; exit 1; }
grep -q 'compare' "$out/$name.c.json" || { echo "no compare block" >&2; exit 1; }
run "graphlets --compare \"$out/$name.c.json#pair\" \"$out/$name.c.json#rest\" --out $out/$name.cmp3.json"
grep -q '"gdd_agreement"' "$out/$name.cmp3.json" || { echo "compare of two groups of one file failed" >&2; exit 1; }
run "graphlets --compare $out/$name.1.json $out/$name.1.json --out $out/$name.cmp.json"
grep -q '"gdd_agreement": 1.000000' "$out/$name.cmp.json" || { echo "self agreement is not 1" >&2; exit 1; }
grep -q "uncalibrated" "$out/$name.log" || { echo "no uncalibrated note" >&2; exit 1; }
run "graphlets --compare $out/$name.1.json $out/$name.1.json --calibration $cal --out $out/$name.cmp2.json"
grep -q '"posterior_gdd_agreement"' "$out/$name.cmp2.json" || { echo "no posterior" >&2; exit 1; }
if run "graphlets --max-nodes 1 --out $out/$name.x.json"; then
    echo "--max-nodes 1 must refuse" >&2
    exit 1
fi
grep -q "more than --max-nodes" "$out/$name.log" || { echo "no refusal text" >&2; exit 1; }
