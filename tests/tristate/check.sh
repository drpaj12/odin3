#!/usr/bin/env bash
# check.sh — the `tristate` pass (brief odin3-tristate-pass). Run by CTest as tristate_micro.
#
# usage: check.sh ODIN3 DIR
#
# For each case DIR/<n>.v: odin3 reads it, `proc; opt; flatten; tristate; opt`, writes Verilog; the
# WARNING lines are checked; iverilog runs DIR/<n>.tb.v on the RTL and (-DSPLIT for inout cases)
# on the written netlist and the printed traces must be equal. Exit 0 pass, 1 fail, 77 no iverilog.
set -uo pipefail
odin3=$1 dir=$2
command -v iverilog >/dev/null || { echo "tristate_micro: no iverilog: skipped"; exit 77; }
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tristate.XXXXXX")
trap 'rm -rf "$work"' EXIT
fails=0
fail() { echo "tristate_micro: FAIL: $*" >&2; fails=$((fails + 1)); }

# case | extra define | required WARNING text (one per line, ';' separated)
run_case() {
    local n=$1 def=$2 want=$3
    "$odin3" --top top -p "read_verilog $dir/$n.v; proc; opt; flatten; tristate; opt; write_verilog $work/$n.out.v" \
        >"$work/$n.log" 2>&1 || { fail "$n: odin3 failed: $(tail -3 "$work/$n.log")"; return; }
    local IFS=';' w
    for w in $want; do
        grep -qF -- "$w" "$work/$n.log" || fail "$n: no WARNING containing '$w'"
    done
    iverilog -g2005 -o "$work/$n.rtl.vvp" "$dir/$n.v" "$dir/$n.tb.v" 2>"$work/$n.iv1" ||
        { fail "$n: iverilog (RTL): $(head -3 "$work/$n.iv1")"; return; }
    iverilog -g2005 ${def:+-D$def} -o "$work/$n.out.vvp" "$work/$n.out.v" "$dir/$n.tb.v" 2>"$work/$n.iv2" ||
        { fail "$n: iverilog (netlist): $(head -3 "$work/$n.iv2")"; return; }
    vvp "$work/$n.rtl.vvp" >"$work/$n.rtl.txt"
    vvp "$work/$n.out.vvp" >"$work/$n.out.txt"
    [[ -s $work/$n.rtl.txt ]] || { fail "$n: empty trace"; return; }
    cmp -s "$work/$n.rtl.txt" "$work/$n.out.txt" ||
        fail "$n: traces differ: $(diff "$work/$n.rtl.txt" "$work/$n.out.txt" | head -4 | tr '\n' '|')"
}

run_case single_inout SPLIT "inout: port p;split into p_i/p_o/p_oe"
run_case inout_read SPLIT "inout: port p;split into p_i"
run_case excl_bus "" "internal net 'b' converted to logic"
run_case sub_bus "" "tristate: bus;2 drivers converted to logic"
run_case sub_inout SPLIT "inout: port sda;split into sda_i/sda_o/sda_oe"
run_case bus8 SPLIT "inout: port db;split into db_i/db_o/db_oe"
run_case repl_inout SPLIT "inout: port dq;split into dq_i/dq_o/dq_oe"

# a conflict-capable bus: converted with the bus conflict said, no simulation compare
"$odin3" --top top -p "read_verilog $dir/sub_bus.v; proc; opt; flatten; tristate" >"$work/c.log" 2>&1 ||
    fail "conflict: odin3 failed"
grep -qF "bus conflict" "$work/c.log" || fail "conflict: the WARNING does not say bus conflict"

# --strict refuses
"$odin3" --top top -p "read_verilog $dir/sub_bus.v; proc; opt; flatten; tristate --strict" >"$work/s.log" 2>&1 &&
    fail "strict: accepted a tri-state net"

[[ $fails -eq 0 ]] && echo "tristate_micro: ok" && exit 0
exit 1
