#!/bin/sh
# run.sh <odin3> <upstream-dir> <program.vmh> <cycles> <work-dir> [mem-address-bits]
# One program: the iverilog reference and Odin III sim_tb on the RTL (proc; opt; lower) and on the
# netlist (the same design written by write_blif and read back by a fresh run). One result line
# per run: wall time, peak memory,
# first mismatch. Exit 77 (skip) without the upstream tree, iverilog or vvp; 1 on a mismatch.
set -u
odin3=$1; up=$2; prog=$3; cycles=$4; work=$5; bits=${6:-}
here=$(cd "$(dirname "$0")" && pwd)
[ -d "$up/quartus_project_revert_timer" ] || { echo "SKIP: $up not found"; exit 77; }
command -v iverilog >/dev/null && command -v vvp >/dev/null || { echo "SKIP: no iverilog"; exit 77; }
d=$work/${prog%.vmh}${bits:+_m$bits}
files=$("$here/prepare.sh" "$up" "$d" "$prog" $bits | tail -1) || exit 2
cd "$d" || exit 2
iverilog -g2005 -P mcu_tb.CYCLES="$cycles" -o ref.vvp mcu_tb.v $files || exit 2
timed() { # label, outfile, command...
    label=$1; out=$2; shift 2
    /usr/bin/time -f "%e s %M KB" -o "$out.time" "$@" >"$out.log" 2>&1 || { echo "$label: ERROR"; tail -3 "$out.log"; return 2; }
}
timed iverilog ref vvp ref.vvp && grep '^[0-9]' ref.log >ref.txt
echo "iverilog: $(cat ref.time)"
status=0
run_odin() { # label, script, optional second script (a fresh run reading what the first wrote)
    if [ -n "${3:-}" ]; then
        "$odin3" --top single_cycle_BRAM_top -p "$2" >"$1.build.log" 2>&1 || { echo "$1: build ERROR"; status=1; return; }
        set -- "$1" "$3"
    fi
    timed "$1" "$1.txt" "$odin3" --top single_cycle_BRAM_top -p "$2; sim_tb --out $1.txt mcu_tb.v" || { status=1; return; }
    printf '%s: %s; ' "$1" "$(cat "$1.txt.time")"
    python3 "$here/compare.py" ref.txt "$1.txt" | tr '\n' ' '; echo
    python3 "$here/compare.py" ref.txt "$1.txt" >/dev/null || status=1
}
run_odin rtl "read_verilog $files; proc; opt; lower"
run_odin netlist "read_verilog $files; proc; opt; lower; write_blif nl.blif" "read_blif nl.blif"
exit $status
