#!/bin/sh
# run_micros.sh <odin3> [iverilog-bin]: for each tests/tb/micro/<n>_tb.v (and misc_tb.v, no DUT; an optional <n>_lib.v is a second testbench file, -I . for `include) the
# $display output of `odin3 sim_tb` must equal iverilog + vvp line for line (spec section 7a).
# An optional <n>.rflags file holds extra arguments of `read_verilog` (e.g. --case-xz-never).
# An optional <n>.flags file holds extra arguments of `lower` for the DUT (e.g. --keep-sel, the
# flow for x-bearing benches: if/case selects keep their RTL meaning). SIM_TB_FLAGS (environment)
# is added to every sim_tb command; with --2state in it the 4-state micros (x*_tb.v) are skipped; the x micros get --4state,
# since 2-state mode cannot print x like iverilog.
set -u
odin3=$1
flags=${SIM_TB_FLAGS:-}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tb.XXXXXX")
fail=0
cd "$here/micro" || exit 2
for tb in *_tb.v; do
    n=${tb%_tb.v}
    case "$flags" in *--2state*) case "$n" in x*) continue ;; esac ;; esac
    mflags=""
    case "$n" in x*) mflags="--4state" ;; esac
    lflags=""
    [ -f "$n.flags" ] && lflags=$(cat "$n.flags")
    rflags=""
    [ -f "$n.rflags" ] && rflags=$(cat "$n.rflags")
    dut=""
    [ -f "$n.v" ] && dut="$n.v"
    extra=""
    [ -f "${n}_lib.v" ] && extra="${n}_lib.v"
    if iverilog -g2005 -I . -o "$work/$n.vvp" "$tb" $extra $dut 2>"$work/$n.iverr" && vvp "$work/$n.vvp" >"$work/$n.ref" 2>&1; then
        sed -i '/\$finish called/d' "$work/$n.ref"
    else
        echo "SKIP $n (iverilog rejects)"; continue
    fi
    if [ -n "$dut" ]; then
        script="read_verilog $rflags $dut; proc; opt; lower $lflags; sim_tb $flags $mflags -I . --out $work/$n.out $tb $extra"
    else
        script="sim_tb $flags $mflags -I . --out $work/$n.out $tb $extra"
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
