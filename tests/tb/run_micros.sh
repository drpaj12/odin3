#!/bin/sh
# run_micros.sh <odin3> [iverilog-bin]: for each tests/tb/micro/<n>_tb.v (and misc_tb.v, no DUT) the
# $display output of `odin3 sim_tb` must equal iverilog + vvp line for line (spec section 7a).
set -u
odin3=$1
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tb.XXXXXX")
fail=0
cd "$here/micro" || exit 2
for tb in *_tb.v; do
    n=${tb%_tb.v}
    dut=""
    [ -f "$n.v" ] && dut="$n.v"
    if iverilog -g2005 -o "$work/$n.vvp" "$tb" $dut 2>"$work/$n.iverr" && vvp "$work/$n.vvp" >"$work/$n.ref" 2>&1; then
        sed -i '/\$finish called/d' "$work/$n.ref"
    else
        echo "SKIP $n (iverilog rejects)"; continue
    fi
    if [ -n "$dut" ]; then
        script="read_verilog $dut; proc; opt; lower; sim_tb --out $work/$n.out $tb"
    else
        script="sim_tb --out $work/$n.out $tb"
    fi
    if ! "$odin3" -p "$script" >"$work/$n.log" 2>&1; then
        echo "FAIL $n (odin3 error)"; tail -3 "$work/$n.log"; fail=1; continue
    fi
    if diff -u "$work/$n.ref" "$work/$n.out" >"$work/$n.diff"; then
        echo "PASS $n ($(wc -l <"$work/$n.out") lines)"
    else
        echo "FAIL $n"; head -12 "$work/$n.diff"; fail=1
    fi
done
exit $fail
