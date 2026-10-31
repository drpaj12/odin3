#!/bin/sh
# run_errors.sh <odin3>: each tests/tb/errors/*_tb.v must make `sim_tb` fail (non-zero exit, no hang)
# with the message on its "// EXPECT:" line. Prints PASS/FAIL per file.
set -u
odin3=$1
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tberr.XXXXXX")
fail=0
for tb in "$here"/errors/*_tb.v; do
    n=$(basename "$tb" .v)
    want=$(sed -n 's|^// EXPECT: ||p' "$tb")
    if timeout 60 "$odin3" -p "sim_tb --out $work/$n.out $tb" >"$work/$n.log" 2>&1; then
        echo "FAIL $n (run succeeded)"; fail=1
    elif grep -qF -- "$want" "$work/$n.log"; then
        echo "PASS $n"
    else
        echo "FAIL $n (no message \"$want\")"; head -3 "$work/$n.log"; fail=1
    fi
done
exit $fail
