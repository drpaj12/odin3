#!/usr/bin/env bash
# run-micro.sh: compare Odin III output on tests/micro against Parmys goldens.
# Env: ODIN3_MICRO_CMD (template with {design} {arch} {out}), GOLDEN (default ~/odin3-ws/golden).
# Exit: 0 all pass, 1 any fail, 2 a comparison tool errored (no fail), 3 nothing could be run
# (Phase 0/1). Errors are never counted as passes or fails.
set -uo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$repo" || exit 2
golden=${GOLDEN:-$HOME/odin3-ws/golden}
bin=build/debug/odin3
archs=(EArch k6_frac_N10_frac_chain_mem32K_40nm)
vtr=${VTR_ROOT:-$HOME/odin3-ws/external/vtr-verilog-to-routing}/vtr_flow/arch/timing

mapfile -t designs < <(find tests/micro -name '*.v' | sort)
if [ "${#designs[@]}" -eq 0 ]; then
    echo "no micro designs in tests/micro yet (Phase 0): nothing run, nothing passed"
    exit 3
fi
if [ ! -x "$bin" ]; then
    echo "no Odin III binary at $bin (build the debug preset first): nothing run, nothing passed"
    exit 3
fi
if [ -z "${ODIN3_MICRO_CMD:-}" ]; then
    echo "ODIN3_MICRO_CMD not set (template with {design} {arch} {out}); ask the human for the CLI invocation"
    exit 3
fi

fail=0
err=0
printf '%-28s %-36s %-10s %-10s %s\n' design arch identical equivalent note
for d in "${designs[@]}"; do
    leaf=$(basename "$d" .v)
    for a in "${archs[@]}"; do
        gold="$golden/$a/micro/$leaf/$leaf.parmys.blif"
        out="build/micro-out/$a/$leaf.blif"
        ident=skipped equiv=skipped note=""
        mkdir -p "$(dirname "$out")"
        rm -f "$out"
        if [ ! -f "$gold" ]; then
            note="no golden (run /oracle)"
        else
            cmd=${ODIN3_MICRO_CMD//\{design\}/$d}
            cmd=${cmd//\{arch\}/$vtr/$a.xml}
            cmd=${cmd//\{out\}/$out}
            if ! bash -c "$cmd" >"build/micro-out/$a/$leaf.log" 2>&1 || [ ! -f "$out" ]; then
                ident=no equiv=no note="odin3 failed: build/micro-out/$a/$leaf.log"
                fail=1
            else
                python3 tools/netlist-compare/netlist_compare.py --quiet "$out" "$gold"
                rc=$?
                if [ $rc -eq 0 ]; then
                    ident=yes equiv=yes
                else
                    if [ $rc -ne 1 ]; then
                    ident=error note="netlist-compare exit $rc" err=1
                else
                    ident=no
                    python3 tools/equiv-check/equiv_check.py "$out" "$gold" >"build/micro-out/$a/$leaf.equiv.log" 2>&1
                    case $? in
                    0) equiv=yes ;;
                    1) equiv=no fail=1 ;;
                    *) equiv=error err=1 note="equiv-check error: build/micro-out/$a/$leaf.equiv.log" ;;
                    esac
                fi
                fi
            fi
        fi
        printf '%-28s %-36s %-10s %-10s %s\n' "$leaf" "$a" "$ident" "$equiv" "$note"
    done
done
if [ $fail -ne 0 ]; then
    exit 1
fi
exit $((err ? 2 : 0))
