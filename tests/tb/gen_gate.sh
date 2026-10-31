#!/bin/sh
# gen_gate.sh <odin3> <cycles> <design.v>...: testbench_gen on each design's lowered netlist, then
# the same generated testbench on the RTL under iverilog and on the netlist under sim_tb must print
# the same lines (spec section 7b). Prints PASS/FAIL/SKIP/XREF per design (XREF: the iverilog
# output has x, the 2-state simulator cannot match it).
set -u
odin3=$1; cycles=$2; shift 2
work=${GATE_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/odin3-gen.XXXXXX")}
mkdir -p "$work"
PRIMS=""; LIB=""
if [ -n "${VTR_PRIMS:-}" ]; then PRIMS=$VTR_PRIMS; LIB="read_verilog --vtr-compat --lib $VTR_PRIMS;"; fi   # RAM models
for v in "$@"; do
    n=$(basename "$v" .v)
    d="$work/$n"; mkdir -p "$d"
    if ! "$odin3" -p "$LIB read_verilog --vtr-compat $v; proc; opt; lower; testbench_gen --out $d/tb.v --cycles $cycles" >"$d/gen.log" 2>&1; then
        echo "SKIP $n (generator: $(grep -m1 error "$d/gen.log" | cut -c1-110))"; continue
    fi
    if ! iverilog -g2005 -o "$d/ref.vvp" "$d/tb.v" "$v" $PRIMS 2>"$d/iv.err"; then
        echo "SKIP $n (iverilog rejects: $(head -1 "$d/iv.err" | cut -c1-90))"; continue
    fi
    timeout 600 vvp "$d/ref.vvp" 2>&1 | grep -v 'finish called' >"$d/ref.out"
    if ! "$odin3" -p "$LIB read_verilog --vtr-compat $v; proc; opt; lower; sim_tb --out $d/odin.out $d/tb.v" >"$d/sim.log" 2>&1; then
        if grep -q unsupported "$d/sim.log"; then echo "UNSUP $n ($(grep -m1 unsupported "$d/sim.log" | cut -c1-110))"; else echo "FAIL $n (sim_tb: $(grep -m1 error "$d/sim.log" | cut -c1-110))"; fi; continue
    fi
    res=$(python3 "$(dirname "$0")/cmp_x.py" "$d/ref.out" "$d/odin.out")
    case "$res" in
    same) echo "PASS $n ($(wc -l <"$d/odin.out") lines)" ;;
    masked*) echo "PASS-X $n ($(wc -l <"$d/odin.out") lines, equal apart from x digits in iverilog: $res)" ;;
    *) echo "FAIL $n ($res)" ;;
    esac
done
