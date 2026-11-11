#!/bin/sh
# run_memx.sh <odin3>: x and z inputs on memories (tests/tb/memx/<n>.v with <n>_tb.v). The output of
# `sim_tb --4state` with every `$mem` lowered to gates must equal the output with the `$mem`
# simulated as a cell (`lower --keep-mem-cells`), line for line. iverilog is not the reference here:
# an x write address makes the words it may reach x (as the lowered netlist does), where
# iverilog drops the write. memory_merge turns a registered read into a synchronous read port.
set -u
odin3=$1
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-memx.XXXXXX")
fail=0
cd "$here/memx" || exit 2
for tb in *_tb.v; do
    n=${tb%_tb.v}
    for merge in "" "memory_merge;"; do
        for state in --4state --2state; do
            "$odin3" -p "read_verilog $n.v; proc; opt; $merge lower; sim_tb $state -I . --out $work/$n.gates $tb" >"$work/$n.log1" 2>&1 || { echo "FAIL $n (lower)"; tail -3 "$work/$n.log1"; fail=1; continue; }
            "$odin3" -p "read_verilog $n.v; proc; opt; $merge lower --keep-mem-cells; sim_tb $state -I . --out $work/$n.cell $tb" >"$work/$n.log2" 2>&1 || { echo "FAIL $n (memory cell)"; tail -3 "$work/$n.log2"; fail=1; continue; }
            if diff -u "$work/$n.gates" "$work/$n.cell" >"$work/$n.diff"; then
                echo "PASS $n $merge $state ($(wc -l <"$work/$n.cell") lines)"
            else
                echo "FAIL $n $merge $state"; head -12 "$work/$n.diff"; fail=1
            fi
        done
    done
done
exit $fail
