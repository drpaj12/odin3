#!/usr/bin/env bash
# check-symbols.sh — link-level backstop for spec §15.1: no exit()/abort() outside src/cli/.
#
# usage: tools/check-symbols.sh LIB.so...
#        tools/check-symbols.sh --exports odin3.h LIB.so
#        tools/check-symbols.sh --only F1,F2,... --forbid S1,S2,... LIB.so
# Fails if any given shared object (libodin3, plugins — never the CLI) imports a process-ending
# function. Catches what source checks cannot: macros, token pasting, function pointers.
# Run by CTest as `symbol_rules`. When src/ir/check*.c lands (Phase 1), its debug-only abort()
# will need an explicit allowance here.
#
# --exports mode: the function symbols libodin3 exports (nm -D, types T/W/D/B) must equal the
# functions declared between ODIN3_CDEF_BEGIN and ODIN3_CDEF_END in odin3.h (typedefs excluded).
#
# --only/--forbid mode (3D spec §4.2, the linked-ABC adapter): the symbols LIB.so defines in its
# dynamic table (nm -D --defined-only, any type) must equal the --only list, and it must import
# (nm -D --undefined-only) nothing in the --forbid list. Versions (@GLIBC_…) are ignored. One line
# per violation on stderr, exit 1; exit 2 on a usage error or an unreadable LIB.so.
set -euo pipefail

# words <comma list>: one word per line, sorted, without empty entries.
words() {
    tr ',' '\n' <<<"$1" | sed '/^$/d' | sort -u
}

if [[ ${1:-} == --only ]]; then
    usage_only="usage: tools/check-symbols.sh --only F1,F2,... --forbid S1,S2,... LIB.so"
    if [[ $# -ne 5 || $3 != --forbid || -z $(words "$2") || -z $(words "$4") ]]; then
        echo "$usage_only" >&2
        exit 2
    fi
    lib=$5
    only=$(words "$2")
    forbid=$(words "$4")
    if ! exported=$(nm -D --defined-only "$lib" | awk 'NF >= 3 {print $3}' | sed -E 's/@.*//' |
        sort -u) || ! imported=$(nm -D --undefined-only "$lib" | awk '{print $NF}' |
        sed -E 's/@.*//' | sort -u); then
        echo "check-symbols: cannot read the dynamic symbols of $lib" >&2
        exit 2
    fi
    rc=0
    while read -r name; do
        [[ -n $name ]] && { echo "check-symbols: $lib exports $name, which is not in --only" >&2; rc=1; }
    done < <(comm -13 <(echo "$only") <(echo "$exported"))
    while read -r name; do
        [[ -n $name ]] && { echo "check-symbols: $lib does not export $name" >&2; rc=1; }
    done < <(comm -23 <(echo "$only") <(echo "$exported"))
    while read -r name; do
        [[ -n $name ]] && { echo "check-symbols: $lib imports forbidden $name" >&2; rc=1; }
    done < <(comm -12 <(echo "$forbid") <(echo "$imported"))
    [[ $rc -eq 0 ]] && echo "check-symbols: $lib exports exactly --only and imports nothing in --forbid"
    exit $rc
fi

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
