#!/usr/bin/env bash
# slang_flow.sh — the v1.1.0 SystemVerilog end-to-end test (DESIGN §4.2): each .sv design is read
# by the slang adapter, elaborated, run through the gate flow (proc; opt; lower; abc --gates;
# dfflibmap) and written as a gate-mapped structural Verilog netlist; the netlist is read back by
# the owned Verilog front end and the two BLIFs are equivalent (tools/equiv-check). With Yosys on
# PATH the RTL's BLIF is also equivalent to Yosys's read_verilog -sv + techmap BLIF.
#
# usage: tests/cli/slang_flow.sh ODIN3 EQUIV_CHECK SOURCE_DIR
set -euo pipefail

[[ $# -eq 3 ]] || { echo "usage: slang_flow.sh ODIN3 EQUIV_CHECK SOURCE_DIR" >&2; exit 2; }
odin3=$1
equiv=$2
src=$3
lib=$src/lib/cmos6.o3lib
data=$src/tests/unit/data/slang
golden=$src/tests/golden/slang

work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-slang.XXXXXX")
trap 'rm -rf "$work"' EXIT

fail() { echo "slang_flow: $*" >&2; exit 1; }

# One design: forward gate flow, structural Verilog, read-back and equivalence.
flow() { # name top sources...
    local name=$1 top=$2
    shift 2
    local sources=("$@")
    "$odin3" --top "$top" -p \
        "read_techlib $lib; read_verilog ${sources[*]}; proc; opt; lower; abc --gates cmos6; dfflibmap cmos6; write_verilog $work/$name.v; write_blif --gates-as-names $work/$name.blif" \
        >"$work/$name.fwd.log" 2>&1 || { cat "$work/$name.fwd.log" >&2; fail "$name: forward flow failed"; }
    # A purely wiring design (a concatenation) maps to no gate: its structural Verilog is an
    # assign; a design with logic must hold library gate instances.
    if ! grep -Eq '^  (BUF|INV|NAND2|NOR2|DFF) ' "$work/$name.v" &&
        ! grep -q '  assign ' "$work/$name.v"; then
        fail "$name: no gate cell instance or assign in the structural Verilog"
    fi
    "$odin3" -p "read_techlib $lib; read_verilog $work/$name.v; write_blif --gates-as-names $work/$name.read.blif" \
        >"$work/$name.back.log" 2>&1 || { cat "$work/$name.back.log" >&2; fail "$name: read-back failed"; }
    local text
    text=$("$equiv" "$work/$name.blif" "$work/$name.read.blif" 2>&1) ||
        fail "$name: roundtrip not equivalent: $text"
    case "$text" in
    *equivalent*) ;;
    *) fail "$name: roundtrip not equivalent: $text" ;;
    esac
    echo "slang_flow: $name: gate flow and structural-Verilog roundtrip equivalent"
}

# The five verilog-eval references (committed under tests/unit/data/slang/verilog-eval).
for ref in Prob004_vector2 Prob055_conditional Prob058_alwaysblock2 Prob100_fsm3comb \
    Prob023_vector100r; do
    flow "$ref" RefModule "$data/verilog-eval/${ref}_ref.sv"
done
# Packages, wildcard imports, enums, a struct, a memory and always_comb/ff/latch.
flow sv_types dut "$data/sv_types.sv"
# An interface instance and an interface port (flattened), packed struct via a modport.
flow sv_iface top "$data/sv_iface.sv"
# The golden project's package/alu/top in compilation order.
flow sv_package top "$src/tests/golden/projects/sv_package/src/pkg/alu_pkg.sv" \
    "$src/tests/golden/projects/sv_package/src/alu.sv" \
    "$src/tests/golden/projects/sv_package/src/top.sv"

grep -lEq '^  (BUF|INV|NAND2|NOR2|DFF) ' "$work"/*.v >/dev/null ||
    fail "no design produced a library gate instance"
# Golden: the gate-mapped BLIF of alwaysblock2 is byte-identical to the committed golden.
if [[ -f $golden/alwaysblock2.blif ]]; then
    "$src/tools/netlist-compare/netlist-compare" "$work/Prob058_alwaysblock2.blif" \
        "$golden/alwaysblock2.blif" >/dev/null ||
        fail "alwaysblock2: the gate BLIF differs from tests/golden/slang/alwaysblock2.blif"
    echo "slang_flow: alwaysblock2: gate BLIF equals the golden"
fi

# Yosys oracle: the RTL BLIF is equivalent to Yosys 0.55's techmapped BLIF for the five refs.
if command -v yosys >/dev/null 2>&1; then
    for ref in Prob004_vector2 Prob055_conditional Prob058_alwaysblock2 Prob100_fsm3comb \
        Prob023_vector100r; do
        "$odin3" --top RefModule -p \
            "read_verilog $data/verilog-eval/${ref}_ref.sv; proc; opt; lower; write_blif --gates-as-names $work/$ref.rtl.blif" \
            >"$work/$ref.rtl.log" 2>&1 || { cat "$work/$ref.rtl.log" >&2; fail "$ref: RTL BLIF failed"; }
        yosys -q -p "read_verilog -sv $data/verilog-eval/${ref}_ref.sv; hierarchy -top RefModule; proc; opt; techmap; opt; write_blif $work/$ref.yosys.blif" \
            >"$work/$ref.yosys.log" 2>&1 || { cat "$work/$ref.yosys.log" >&2; fail "$ref: Yosys failed"; }
        text2=$("$equiv" "$work/$ref.rtl.blif" "$work/$ref.yosys.blif" 2>&1) ||
            fail "$ref: differs from Yosys: $local_text"
        case "$text2" in
        *equivalent*) ;;
        *) fail "$ref: differs from Yosys: $local_text" ;;
        esac
        echo "slang_flow: $ref: equivalent to Yosys"
    done
else
    echo "slang_flow: yosys not found; the Yosys comparison is skipped"
fi
