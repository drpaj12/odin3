#!/usr/bin/env bash
# corpus.sh ODIN3: imports every VTR architecture file with read_arch (spec T-5). Exits 77 (skip)
# without $VTR_ROOT. Every file must import (exit 0); the files whose models have non-identifier
# names (spec M-5) warn, and their number is printed and checked against the expected count.
# This is a parse-coverage and crash test pinned to VTR 3c9a4d23: the totals (300 files, 27 with
# the warning) change with a VTR update, and the failure says so. Per-arch facts (models, leaves,
# K) of the three sweep archs are checked here from the INFO line, in depth in test_arch_vtr.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: corpus.sh ODIN3" >&2; exit 2; }
odin3=$1
[[ -n "${VTR_ROOT:-}" && -d "$VTR_ROOT/vtr_flow/arch" ]] || { echo "skip: VTR_ROOT not set"; exit 77; }
expect_warned=27   # VTR 3c9a4d23 (docs/ORACLES.md): titan, noc, stratixiv and multi_die archs
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-arch-corpus.XXXXXX")
trap 'rm -rf "$work"' EXIT
total=0
warned=0
slowest=0
slowest_file=
while IFS= read -r file; do
    grep -q '<architecture' "$file" || continue
    total=$((total + 1))
    start=$(date +%s.%N)
    if ! "$odin3" -p "read_arch $file" >"$work/out" 2>"$work/err"; then
        echo "FAIL: $file" >&2
        cat "$work/err" >&2
        exit 1
    fi
    elapsed=$(echo "$(date +%s.%N) - $start" | bc)
    if (($(echo "$elapsed > $slowest" | bc))); then
        slowest=$elapsed
        slowest_file=$file
    fi
    case "$file" in
        */timing/k6_frac_N10_frac_chain_mem32K_40nm.xml) want="4 models (4 registered), .* leaves, K=6" ;;
        */COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml) want="4 models (4 registered), .* leaves, K=6" ;;
        */timing/k4_N4_90nm.xml) want="0 models (0 registered), .* leaves, K=4" ;;
        *) want= ;;
    esac
    if [[ -n "$want" ]] && ! grep -q "read_arch: .*: $want" "$work/err"; then
        echo "FAIL: $file: expected '$want'" >&2
        cat "$work/err" >&2
        exit 1
    fi
    if grep -q 'blif_model="\.\(names\|subckt\)' "$file" && grep -q ', 0 leaves,' "$work/err"; then
        echo "FAIL: $file has pb_type leaves but the import found none" >&2
        exit 1
    fi
    if grep -q 'cannot be .o3lib names' "$work/err"; then
        warned=$((warned + 1))
    fi
done < <(find "$VTR_ROOT/vtr_flow/arch" -name '*.xml' | sort)
echo "imported $total architectures; $warned with the non-identifier warning; slowest ${slowest}s ($slowest_file)"
[[ $total -eq 300 ]] || { echo "expected 300 architectures (VTR 3c9a4d23), found $total" >&2; exit 1; }
if [[ $warned -ne $expect_warned ]]; then
    echo "expected $expect_warned archs with the M-5 warning, got $warned" >&2
    exit 1
fi
