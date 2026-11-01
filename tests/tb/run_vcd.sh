#!/bin/sh
# run_vcd.sh <odin3> <python3>: for each tests/tb/micro/<n>_tb.v with a DUT (<n>.v), the VCD of
# `odin3 sim_tb --vcd` must equal iverilog's $dumpvars(0, <n>_tb) VCD signal by signal
# (tools/vcd-compare): same value at every time either file changes it. A `timescale is put in
# front of the testbench of both runs (the micros have none; iverilog defaults to 1s).
set -u
odin3=$1
py=${2:-python3}
here=$(cd "$(dirname "$0")" && pwd)
tool="$here/../../tools/vcd-compare/vcd-compare"
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-vcd.XXXXXX")
fail=0
cd "$here/micro" || exit 2
for tb in *_tb.v; do
    n=${tb%_tb.v}
    [ -f "$n.v" ] || continue
    { echo '`timescale 1ns/1ps'; cat "$tb"; } >"$work/$n.tb.v"
    printf 'module dumper;\n  initial begin $dumpfile("%s/%s.ref.vcd"); $dumpvars(0, %s_tb); end\nendmodule\n' \
        "$work" "$n" "$n" >"$work/dumper.v"
    if ! iverilog -g2005 -s "${n}_tb" -s dumper -o "$work/$n.vvp" "$work/$n.tb.v" "$n.v" "$work/dumper.v" 2>"$work/$n.iverr" \
        || ! vvp "$work/$n.vvp" >"$work/$n.ref" 2>&1; then
        echo "SKIP $n (iverilog rejects)"; continue
    fi
    script="read_verilog $n.v; proc; opt; lower; sim_tb --out $work/$n.out --vcd $work/$n.vcd $work/$n.tb.v"
    if ! "$odin3" -p "$script" >"$work/$n.log" 2>&1; then
        echo "FAIL $n (odin3 error)"; tail -3 "$work/$n.log"; fail=1; continue
    fi
    if "$py" "$tool" "$work/$n.vcd" "$work/$n.ref.vcd" >"$work/$n.cmp" 2>&1; then
        echo "PASS $n ($(cat "$work/$n.cmp"))"
    else
        echo "FAIL $n"; head -12 "$work/$n.cmp"; fail=1
    fi
done
exit $fail
