#!/bin/sh
# run_hard4.sh <odin3> <vtr.o3lib>: 4-state simulation of hard cells interpreted from their `fn`
# lines (adder). With an unknown input bit the output bits that no completion changes stay known,
# as in the gate netlist: the `sim_tb` output of `hardmap; lower` must equal that of `lower`.
set -u
odin3=$1
lib=$2
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-hard4.XXXXXX")
cd "$here/hard4" || exit 2
fail=0
for tb in *_tb.v; do
    n=${tb%_tb.v}
    for flow in lower hardmap; do
        pre=""
        [ "$flow" = hardmap ] && pre="hardmap;"
        script="read_techlib $lib; read_verilog $n.v; proc; opt; $pre lower --keep-sel; sim_tb --out $work/$n.$flow.out $tb"
        if ! "$odin3" -p "$script" >"$work/$n.$flow.log" 2>&1; then
            echo "FAIL $n [$flow] (odin3 error)"; tail -3 "$work/$n.$flow.log"; fail=1
        fi
    done
    if cmp -s "$work/$n.lower.out" "$work/$n.hardmap.out"; then
        echo "PASS $n"
    else
        echo "FAIL $n (hardmap differs from lower)"; diff "$work/$n.lower.out" "$work/$n.hardmap.out"; fail=1
    fi
done
rm -r "$work"
exit $fail
