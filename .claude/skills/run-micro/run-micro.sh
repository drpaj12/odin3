#!/usr/bin/env bash
# run-micro.sh: the micro sweeps of the current build, through CTest, as one pass/fail table.
#
# The micro comparisons are CTest tests with expect files (hardmap-diff and pm-diff-micro against the
# Parmys goldens, the LPM, FSM and loc micros, tri-state): this script runs those, it does not
# re-implement them. No ODIN3_MICRO_CMD is needed any more.
#
# Env: PRESET (default debug; the binary is build/$PRESET/odin3), MICRO_REGEX (extra ctest -R
#      alternative), JOBS (ctest -j, default 2), GOLDEN / VTR_ROOT as the tests read them.
# Exit: 0 every micro test passed, 1 any failed, 3 nothing could be run (no binary, no tests, or
#       every test skipped for a missing VTR_ROOT / golden tree): claim nothing then.
set -uo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$repo" || exit 2
preset=${PRESET:-debug}
bin=build/$preset/odin3
export VTR_ROOT=${VTR_ROOT:-$HOME/odin3-ws/external/vtr-verilog-to-routing}

if [ ! -x "$bin" ]; then
    echo "no Odin III binary at $bin (cmake --preset $preset && tools/build-locked --preset $preset): nothing run, nothing passed"
    exit 3
fi
if [ ! -d "$HOME/odin3-ws/golden" ]; then
    echo "note: no golden tree at ~/odin3-ws/golden: the Parmys comparisons will be skipped (run /oracle)"
fi

# Tests labelled micro, plus the golden sweeps over the micros by name.
regex='^(hardmap_diff_micro|pm_diff_micro_.*|lpm_check_.*|fsm_(equiv|diff)_micro|tristate_micro'
regex+="${MICRO_REGEX:+|$MICRO_REGEX})\$"
log=$(mktemp)
trap 'rm -f "$log"' EXIT
ctest --preset "$preset" -R "$regex" -j"${JOBS:-2}" --no-tests=ignore >"$log" 2>&1
rc=$?

printf '%-44s %s\n' test result
pass=0 fail=0 skip=0
while IFS= read -r line; do
    case "$line" in
    *"Test"*"#"*)
        name=$(sed -E 's/.*Test +#[0-9]+: ([^ ]+) .*/\1/' <<<"$line")
        case "$name" in test_tmp_*) continue ;; esac # CTest fixtures, not micros
        case "$line" in
        *Passed*) res=pass pass=$((pass + 1)) ;;
        *"Skipped"* | *"Not Run"*) res=skipped skip=$((skip + 1)) ;;
        *) res=FAIL fail=$((fail + 1)) ;;
        esac
        printf '%-44s %s\n' "$name" "$res"
        ;;
    esac
done <"$log"
echo "totals: $pass passed, $fail failed, $skip skipped"
if [ $fail -ne 0 ]; then
    echo "--- ctest output of the failures (tail) ---"
    tail -n 60 "$log"
    exit 1
fi
if [ $pass -eq 0 ]; then
    echo "no micro test ran (all skipped or none matched): nothing passed"
    exit 3
fi
[ $rc -eq 0 ] || { tail -n 20 "$log"; exit 1; }
exit 0
