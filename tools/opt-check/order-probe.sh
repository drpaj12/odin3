#!/usr/bin/env bash
# order-probe.sh: the pass-order evidence of 2F spec section 3 (OP-3; plan Task 17, point (o)).
#
# usage: order-probe.sh --yosys Y [--netlist-compare N] [--primitives P] [--variants "R R0 R1 C"]
#                       [--limit N] [--mem SIZE] [--work DIR] [--verbose] <dir or .v file>...
#
# Each variant is the rest of synthesis.tcl (spec section 3's table) inside Yosys 0.55, without the
# `parmys` plugin pass, written to BLIF and compared with `netlist-compare --implicit-models`
# against Parmys's own order (the reference):
#
#   ref  opt_expr; opt_clean; opt -nodffe -nosdff; proc; fsm; opt; wreduce; ...   (Parmys's order)
#   R    proc; opt_expr; opt_clean; opt -nodffe -nosdff; fsm; ...                 (v1's "mode R")
#   R0   proc; fsm; ...                                   (nothing between proc and the rest)
#   R1   proc; opt_expr; opt_clean; fsm; ...              (only opt_expr; opt_clean)
#   C    the reference with the main `opt` moved before `fsm`
#
# Every .v under the directories is a design, smallest first (ties by path); --limit N keeps the
# first N. The reads are `read_verilog -sv -nolatches` (after `read_verilog -nomem2reg P` with
# --primitives) and `hierarchy -check -auto-top -purge_lib`. A design Yosys rejects, or whose
# reference run fails, counts as `error` for every variant. The output is a line per design and
# variant (`identical`, `differs` with both `.names` counts, or `error`), then one total per
# variant. This is evidence, not a gate: the exit code is 0 unless the arguments are wrong (2), no
# Yosys 0.55 is found (2) or no memory cap can be applied (77).
#
# Every Yosys child runs under a cgroup memory cap (systemd-run --user --scope, MemoryMax, no
# swap; inside tools/heavy-run, in its slice). The work directory is a fresh mktemp -d, removed
# on exit, unless --work DIR names one (kept).
set -euo pipefail

here="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
yosys=""
compare="$here/../netlist-compare/netlist-compare"
primitives=""
variants="R R0 R1 C"
limit=0
mem="2G"
work=""
verbose=0
inputs=()

usage() {
    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//' >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
    --yosys) yosys="$2"; shift 2 ;;
    --netlist-compare) compare="$2"; shift 2 ;;
    --primitives) primitives="$2"; shift 2 ;;
    --variants) variants="$2"; shift 2 ;;
    --limit) limit="$2"; shift 2 ;;
    --mem) mem="$2"; shift 2 ;;
    --work) work="$2"; shift 2 ;;
    --verbose) verbose=1; shift ;;
    -h | --help) usage ;;
    -*) echo "order-probe: unknown option $1" >&2; exit 2 ;;
    *) inputs+=("$1"); shift ;;
    esac
done
[ -n "$yosys" ] && [ ${#inputs[@]} -gt 0 ] || usage
for v in $variants; do
    case "$v" in R | R0 | R1 | C) ;; *) echo "order-probe: unknown variant $v" >&2; exit 2 ;; esac
done
case "$limit" in '' | *[!0-9]*) echo "order-probe: --limit wants a number" >&2; exit 2 ;; esac
"$yosys" -V 2>/dev/null | grep -q 'Yosys 0\.55' ||
    { echo "order-probe: $yosys is not Yosys 0.55" >&2; exit 2; }

# ---- the scripts --------------------------------------------------------------------------------

trio="opt_expr; opt_clean; opt -nodffe -nosdff"
# Parmys's lines 5-13 without the `parmys` pass (the shared tail); $1 is "fsm; opt" or "opt; fsm".
tail_of() {
    echo "$1; wreduce; peepopt; opt_clean; share; opt -full; memory -nomap; flatten; opt -full;" \
        "techmap -map +/parmys/adff2dff.v; techmap -map +/parmys/adffe2dff.v;" \
        "techmap -map +/parmys/aldff2dff.v; techmap -map +/parmys/aldffe2dff.v; opt -full;" \
        "opt -full; techmap; opt -fast; dffunmap; opt -fast -noff;" \
        "hierarchy -check -auto-top -purge_lib"
}

variant_script() {
    case "$1" in
    ref) echo "$trio; proc; $(tail_of 'fsm; opt')" ;;
    R) echo "proc; $trio; $(tail_of 'fsm; opt')" ;;
    R0) echo "proc; $(tail_of 'fsm; opt')" ;;
    R1) echo "proc; opt_expr; opt_clean; $(tail_of 'fsm; opt')" ;;
    C) echo "$trio; proc; $(tail_of 'opt; fsm')" ;;
    esac
}

# ---- running ------------------------------------------------------------------------------------

capped() {
    local slice=()
    if [ -n "${ODIN3_HEAVY_SLICE:-}" ]; then slice=(--slice="$ODIN3_HEAVY_SLICE"); fi
    systemd-run --user --scope -q "${slice[@]}" -p MemoryMax="$mem" -p MemorySwapMax=0 "$@"
}

systemd-run --user --scope -q -p MemoryMax="$mem" true 2>/dev/null ||
    { echo "order-probe: skipped: no memory cap (systemd-run --user --scope failed)" >&2; exit 77; }

made=0
if [ -z "$work" ]; then
    work="$(mktemp -d "${TMPDIR:-/tmp}/order-probe.XXXXXX")"
    made=1
    trap 'rm -rf "$work"' EXIT
fi
mkdir -p "$work"

# run_variant NAME FILE -> $work/NAME.blif, 0 on success
run_variant() {
    local name="$1" file="$2" read=""
    if [ -n "$primitives" ]; then read="read_verilog -nomem2reg $primitives; "; fi
    local script="${read}read_verilog -sv -nolatches $file; hierarchy -check -auto-top -purge_lib;"
    script="$script $(variant_script "$name"); write_blif -true + vcc -false + gnd -undef + unconn"
    script="$script -blackbox $work/$name.blif"
    capped "$yosys" -q -p "$script" >"$work/$name.log" 2>&1
}

names_in() { grep -c '^\.names' "$1" || true; }

# designs: every .v under the inputs, smallest first
designs=()
while IFS= read -r line; do designs+=("${line#* }"); done < <(
    for item in "${inputs[@]}"; do
        if [ -d "$item" ]; then find "$item" -name '*.v' -printf '%s %p\n'
        else printf '%s %s\n' "$(stat -c %s "$item")" "$item"; fi
    done | sort -k1,1n -k2,2)
if [ "$limit" -gt 0 ] && [ ${#designs[@]} -gt "$limit" ]; then
    designs=("${designs[@]:0:$limit}")
fi

declare -A same differ failed
for v in $variants; do same[$v]=0; differ[$v]=0; failed[$v]=0; done

for design in "${designs[@]}"; do
    if ! run_variant ref "$design"; then
        for v in $variants; do
            failed[$v]=$((failed[$v] + 1))
            echo "order-probe: $design [$v]: error (reference run failed)"
        done
        continue
    fi
    for v in $variants; do
        if ! run_variant "$v" "$design"; then
            failed[$v]=$((failed[$v] + 1))
            echo "order-probe: $design [$v]: error"
        elif "$compare" --quiet --implicit-models "$work/ref.blif" "$work/$v.blif"; then
            same[$v]=$((same[$v] + 1))
            if [ "$verbose" -eq 1 ]; then echo "order-probe: $design [$v]: identical"; fi
        else
            differ[$v]=$((differ[$v] + 1))
            echo "order-probe: $design [$v]: differs (.names $(names_in "$work/ref.blif")" \
                "vs $(names_in "$work/$v.blif"))"
        fi
    done
done

echo "order-probe: ${#designs[@]} designs"
for v in $variants; do
    echo "order-probe: variant $v: ${same[$v]} identical, ${differ[$v]} differ, ${failed[$v]} error"
done
[ "$made" -eq 1 ] || echo "order-probe: work directory kept: $work"
exit 0
