#!/usr/bin/env bash
# check-symbols.sh — link-level backstop for spec §15.1: no exit()/abort() outside src/cli/.
#
# usage: tools/check-symbols.sh LIB.so...
#        tools/check-symbols.sh --exports odin3.h LIB.so
# Fails if any given shared object (libodin3, plugins — never the CLI) imports a process-ending
# function. Catches what source checks cannot: macros, token pasting, function pointers.
# Run by CTest as `symbol_rules`. When src/ir/check*.c lands (Phase 1), its debug-only abort()
# will need an explicit allowance here.
#
# --exports mode: the function symbols libodin3 exports (nm -D, types T/W/D/B) must equal the
# functions declared between ODIN3_CDEF_BEGIN and ODIN3_CDEF_END in odin3.h (typedefs excluded).
set -euo pipefail

if [[ ${1:-} == --exports ]]; then
    [[ $# -eq 3 ]] || { echo "usage: tools/check-symbols.sh --exports odin3.h LIB.so" >&2; exit 2; }
    header=$2
    lib=$3
    declared=$(sed -n '/ODIN3_CDEF_BEGIN/,/ODIN3_CDEF_END/p' "$header" |
        grep -E '^[A-Za-z]' | grep -vE '^typedef' |
        grep -oE 'odin3_[A-Za-z0-9_]+[[:space:]]*\(' | sed -E 's/[[:space:]]*\($//' | sort -u)
    exported=$(nm -D --defined-only "$lib" | awk '$2 ~ /^[TWDB]$/ && $3 ~ /^odin3_/ {print $3}' |
        sed -E 's/@.*//' | sort -u)
    # A sanitized build must not hide toolchain symbols: show every non-odin3_ export too.
    other=$(nm -D --defined-only "$lib" | awk '$2 ~ /^[TWDB]$/ && $3 !~ /^odin3_/ {print $3}' |
        sed -E 's/@.*//' | sort -u)
    rc=0
    while read -r name; do
        [[ -n $name ]] && { echo "check-symbols: exported but not declared in $header: $name" >&2; rc=1; }
    done < <(comm -13 <(echo "$declared") <(echo "$exported"))
    while read -r name; do
        [[ -n $name ]] && { echo "check-symbols: declared in $header but not exported: $name" >&2; rc=1; }
    done < <(comm -23 <(echo "$declared") <(echo "$exported"))
    while read -r name; do
        [[ -n $name ]] && { echo "check-symbols: unexpected non-odin3_ export: $name" >&2; rc=1; }
    done <<<"$other"
    [[ $rc -eq 0 ]] && echo "check-symbols: exports OK ($(echo "$declared" | wc -l) functions)"
    exit $rc
fi

[[ $# -gt 0 ]] || { echo "usage: tools/check-symbols.sh LIB.so..." >&2; exit 2; }
banned='^(exit|_exit|_Exit|quick_exit|abort)(@.*)?$'
rc=0
for lib in "$@"; do
    while read -r sym; do
        if [[ $sym =~ $banned ]]; then
            echo "check-symbols: $lib imports ${sym%%@*} (only src/cli/ may end the process)" >&2
            rc=1
        fi
    done < <(nm -D --undefined-only "$lib" | awk '{print $NF}')
done
[[ $rc -eq 0 ]] && echo "check-symbols: OK ($# objects)"
exit $rc
