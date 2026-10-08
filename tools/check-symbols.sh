#!/usr/bin/env bash
# check-symbols.sh — link-level backstop for spec §15.1: no exit()/abort() outside src/cli/.
#
# usage: tools/check-symbols.sh LIB.so...
# Fails if any given shared object (libodin3, plugins — never the CLI) imports a process-ending
# function. Catches what source checks cannot: macros, token pasting, function pointers.
# Run by CTest as `symbol_rules`. When src/ir/check*.c lands (Phase 1), its debug-only abort()
# will need an explicit allowance here.
set -euo pipefail
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
