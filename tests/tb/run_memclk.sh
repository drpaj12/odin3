#!/bin/sh
# run_memclk.sh <odin3> [iverilog-bin]: memories written on several clocks (tests/tb/memclk/<n>.v with
# <n>_tb.v). `lower` refuses write ports on different clocks, so only the simulator's memory cell
# (`lower --keep-mem-cells`) runs them: its `sim_tb` output, 4-state and 2-state, must equal iverilog
# + vvp line for line.
set -u
odin3=$1
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-memclk.XXXXXX")
fail=0
cd "$here/memclk" || exit 2
for tb in *_tb.v; do
    n=${tb%_tb.v}
    if ! iverilog -g2005 -I . -o "$work/$n.vvp" "$tb" "$n.v" 2>"$work/$n.iverr" || ! vvp "$work/$n.vvp" >"$work/$n.ref" 2>&1; then
        echo "FAIL $n (iverilog)"; fail=1; continue
    fi
    sed -i '/\$finish called/d' "$work/$n.ref"
    for state in --4state --2state; do
        if ! "$odin3" -p "read_verilog $n.v; proc; opt; lower --keep-mem-cells; sim_tb $state -I . --out $work/$n.out $tb" >"$work/$n.log" 2>&1; then
            echo "FAIL $n $state (odin3 error)"; tail -3 "$work/$n.log"; fail=1; continue
        fi
        if diff -u "$work/$n.ref" "$work/$n.out" >"$work/$n.diff"; then
            echo "PASS $n $state ($(wc -l <"$work/$n.out") lines)"
        else
            echo "FAIL $n $state"; head -12 "$work/$n.diff"; fail=1
        fi
    done
done
exit $fail
