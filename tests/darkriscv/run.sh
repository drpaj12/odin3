#!/bin/sh
# run.sh <odin3> <upstream-dir> <cycles> <work-dir>
# The iverilog reference and Odin III sim_tb on the RTL (proc; opt; lower) and on the netlist (the
# same design written by write_blif and read back by a fresh run). One result line per run: wall
# time, peak memory, masked fields, first mismatch. Exit 77 (skip) without the upstream tree,
# iverilog or vvp; 1 on a mismatch.
set -u
odin3=$1; up=$2; cycles=$3; work=$4
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$up/src/darksocv.mem" ] || { echo "SKIP: $up not found"; exit 77; }
command -v iverilog >/dev/null && command -v vvp >/dev/null || { echo "SKIP: no iverilog"; exit 77; }
# No pipe: a pipeline would report tail's status, not prepare.sh's (dash has no pipefail).
listing=$("$here/prepare.sh" "$up" "$work") || exit 2
files=$(printf '%s\n' "$listing" | tail -n 1)
cd "$work/rtl" || exit 2
sed -i "s/parameter CYCLES = [0-9]*/parameter CYCLES = $cycles/" dark_tb.v
cp dark_tb.v ../iv/rtl/dark_tb.v
(cd ../iv/rtl && iverilog -g2005 -o ../ref.vvp dark_tb.v $files) || exit 2
timed() { # label, outfile, command...
    label=$1; out=$2; shift 2
    /usr/bin/time -f "%e s %M KB" -o "$out.time" "$@" >"$out.log" 2>&1 || { echo "$label: ERROR"; tail -3 "$out.log"; return 2; }
}
timed iverilog ../ref sh -c "cd ../iv/rtl && vvp ../ref.vvp" || exit 2
grep '^[0-9]' ../ref.log >../ref.txt
echo "iverilog: $(cat ../ref.time)"
status=0
run_odin() { # label, script, optional second script (a fresh run reading what the first wrote)
    if [ -n "${3:-}" ]; then
        "$odin3" --top darksocv -p "$2" >"../$1.build.log" 2>&1 || { echo "$1: build ERROR"; status=1; return; }
        set -- "$1" "$3"
    fi
    timed "$1" "../$1.txt" "$odin3" --top darksocv -p "$2; sim_tb --out ../$1.txt dark_tb.v" || { status=1; return; }
    printf '%s: %s; ' "$1" "$(cat "../$1.txt.time")"
    python3 "$here/../mcu/compare.py" ../ref.txt "../$1.txt" "$cycles" | tr '\n' ' '; echo
    python3 "$here/../mcu/compare.py" ../ref.txt "../$1.txt" "$cycles" >/dev/null || status=1
}
run_odin rtl "read_verilog $files; proc; opt; lower"
run_odin netlist "read_verilog $files; proc; opt; lower; write_blif nl.blif" "read_blif nl.blif"
exit $status
