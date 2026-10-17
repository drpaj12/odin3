#!/usr/bin/env bash
# check.sh — PHASE2 decision 28: under --vtr-compat a lone tri-state driver of a top-level output
# loses its enable, as Parmys does. Run by CTest as vtr_tristate_out.
#
# usage: check.sh ODIN3 YOSYS SP_SPLICE NETLIST_COMPARE ARCH DIR
#
# 1. odin3 prints the WARNING under --vtr-compat and none without the option.
# 2. sp-splice (Odin III's front end with --vtr-compat, then Yosys 0.55 with Parmys) writes a
#    netlist identical to the Parmys golden DIR/tri_out.parmys.blif: four buffers q = d, `en`
#    unused (no tri-state cell).
# Exit 0 pass, 1 fail, 77 skipped (no arch file, or no memory cap possible for an ASan child).
set -uo pipefail
odin3=$1 yosys=$2 splice=$3 compare=$4 arch=$5 dir=$6
[[ -f $arch ]] || { echo "vtr_tristate_out: no arch $arch: skipped"; exit 77; }
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tristate.XXXXXX")
trap 'rm -rf "$work"' EXIT
fail() { echo "vtr_tristate_out: FAIL: $*" >&2; exit 1; }

warn="tri_out.v:5:10: warning: tri-state driver of output port 'q': --vtr-compat drops the enable"
"$odin3" -p "read_verilog --vtr-compat $dir/tri_out.v" 2>"$work/compat.log" ||
    fail "odin3 --vtr-compat failed"
grep -qF -- "$warn" "$work/compat.log" || fail "no WARNING '$warn'"
"$odin3" -p "read_verilog $dir/tri_out.v" 2>"$work/plain.log" || fail "odin3 failed"
grep -qF -- "drops the enable" "$work/plain.log" && fail "the WARNING is printed without the option"

"$splice" --odin3 "$odin3" --yosys "$yosys" --arch "$arch" --top tri_out --keep "$work/sp" \
    "$dir/tri_out.v" >"$work/sp.out" 2>"$work/sp.err"
rc=$?
[[ $rc -eq 77 ]] && { echo "vtr_tristate_out: sp-splice skipped"; exit 77; }
[[ $rc -eq 0 ]] || fail "sp-splice exit $rc: $(tail -3 "$work/sp.err")"
"$compare" "$work/sp/out.blif" "$dir/tri_out.parmys.blif" ||
    fail "netlist-compare: the netlist is not the Parmys golden (enable not dropped?)"
echo "vtr_tristate_out: ok"
