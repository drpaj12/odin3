#!/usr/bin/env bash
# corpus.sh ODIN3: imports every VTR architecture file with read_arch (spec T-5). Exits 77 (skip)
# without $VTR_ROOT. Every file must import (exit 0); the files whose models have non-identifier
# names (spec M-5) warn, and their number is printed and checked against the expected count.
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
    if grep -q 'not .o3lib identifiers' "$work/err"; then
        warned=$((warned + 1))
    fi
done < <(find "$VTR_ROOT/vtr_flow/arch" -name '*.xml' | sort)
echo "imported $total architectures; $warned with the non-identifier warning; slowest ${slowest}s ($slowest_file)"
[[ $total -ge 1 ]] || { echo "no architecture found" >&2; exit 1; }
if [[ $warned -ne $expect_warned ]]; then
    echo "expected $expect_warned archs with the M-5 warning, got $warned" >&2
    exit 1
fi
