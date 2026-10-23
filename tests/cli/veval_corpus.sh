#!/usr/bin/env bash
# veval_corpus.sh — the pinned VerilogEval coverage corpus (v1.1.0, DESIGN §4.2): every
# tests/cli/veval/refs/*_ref.sv runs through the SystemVerilog front end and the gate flow
# (read_techlib; read_verilog; proc; opt; lower; abc --gates cmos6; dfflibmap cmos6;
# write_blif); tests/cli/veval/manifest.tsv pins each design's expected outcome. An ok design
# that regresses to a refusal or a crash fails, and a refused design must keep its refusal class
# and pinned message (tools/veval/veval-coverage --manifest).
#
# With ODIN3_YOSYS set, the smallest VVEVAL_EQUIV_SAMPLE (default 20) ok designs are also
# equivalent to Yosys's read_verilog -sv + techmap BLIF, as tests/cli/slang_flow.sh does.
#
# usage: tests/cli/veval_corpus.sh ODIN3 SOURCE_DIR
set -euo pipefail

[[ $# -eq 2 ]] || { echo "usage: veval_corpus.sh ODIN3 SOURCE_DIR" >&2; exit 2; }
odin3=$1
src=$2
corpus=$src/tests/cli/veval

work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-veval.XXXXXX")
trap 'rm -rf "$work"' EXIT

args=(--odin3 "$odin3" --refs "$corpus/refs" --lib "$src/lib/cmos6.o3lib" --work "$work"
      --manifest "$corpus/manifest.tsv")
if [[ -n ${ODIN3_YOSYS:-} ]]; then
    args+=(--equiv-sample "${VVEVAL_EQUIV_SAMPLE:-20}" --yosys "$ODIN3_YOSYS")
fi
"$src/tools/veval/veval-coverage" "${args[@]}"
