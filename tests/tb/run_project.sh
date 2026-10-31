#!/bin/sh
# run_project.sh <odin3>: the golden project tests/tb/project (spec
# docs/specs/2026-10-31-project-testbench-design.md section 6b). `run_testbenches` runs four
# testbenches: good (pass), bad (a deliberately wrong expect file: fail, mismatch reported), both
# (rtl vs netlist) and err (an unsupported construct: error). The good expect file is also checked
# against iverilog (the oracle). Prints PASS/FAIL per check.
set -u
odin3=$1
here=$(cd "$(dirname "$0")" && pwd)/project
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tbproj.XXXXXX")
fail=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }
prep='read_project project.o3proj; read_verilog; proc; opt; lower'
cd "$here" || exit 2

# the oracle: iverilog on the same testbench and DUT gives the expect file
if command -v iverilog >/dev/null 2>&1 && command -v vvp >/dev/null 2>&1; then
    iverilog -g2005 -o "$work/good.vvp" tb_good.v counter.v && vvp "$work/good.vvp" >"$work/good.ref" 2>&1
    sed -i '/\$finish called/d' "$work/good.ref"
    if diff -u good.expect "$work/good.ref" >"$work/oracle.diff"; then ok oracle; else bad oracle; head -8 "$work/oracle.diff"; fi
fi

# the whole project: nonzero exit (bad fails, err errors), one report line each
"$odin3" -p "$prep; run_testbenches --json $work/r.json --report $work/r.txt" >"$work/all.log" 2>&1
[ $? -ne 0 ] && ok exit_nonzero || bad exit_nonzero
grep -q '^good pass rtl' "$work/all.log" && ok good_pass || bad good_pass
grep -q '^bad fail rtl' "$work/all.log" && ok bad_fail || bad bad_fail
grep -q 'mismatch: expect line 5 time 4 field 1: want "4 9 0" got "4 5 0"' "$work/all.log" && ok mismatch || bad mismatch
grep -q '^both pass both' "$work/all.log" && ok both_pass || bad both_pass
grep -q '^err error rtl' "$work/all.log" && ok err_error || bad err_error
grep -q 'run_testbenches: 2 passed, 1 failed, 1 errors' "$work/all.log" && ok totals || bad totals
cmp -s "$work/r.txt" "$work/all.log" || grep -q '^good pass rtl' "$work/r.txt" && ok report_file || bad report_file
python3 - "$work/r.json" <<'PY' && ok json || bad json
import json, sys
d = json.load(open(sys.argv[1]))
by = {t["name"]: t for t in d["testbenches"]}
assert [t["name"] for t in d["testbenches"]] == ["good", "bad", "both", "err"]
assert (d["passed"], d["failed"], d["errors"]) == (2, 1, 1)
assert by["good"]["status"] == "pass" and by["good"]["mismatch"] is None and by["good"]["lines"] == 20
m = by["bad"]["mismatch"]
assert by["bad"]["status"] == "fail" and m["compared"] == "expect" and m["line"] == 5
assert m["time"] == 4 and m["field"] == 1 and m["want"] == "4 9 0" and m["got"] == "4 5 0"
assert by["both"]["on"] == "both" and by["both"]["status"] == "pass"
assert by["err"]["status"] == "error" and by["err"]["error"].startswith("run (rtl)")
for t in d["testbenches"]:
    assert t["wall_ms"] >= 0 and "end_time" in t
PY

# --only
"$odin3" -p "$prep; run_testbenches --only good" >"$work/one.log" 2>&1 && ok only_good || bad only_good
"$odin3" -p "$prep; run_testbenches --only both" >"$work/one.log" 2>&1 && ok only_both || bad only_both
"$odin3" -p "$prep; run_testbenches --only bad" >"$work/one.log" 2>&1 && bad only_bad_fails || ok only_bad_fails
"$odin3" -p "$prep; run_testbenches --only nosuch" >"$work/one.log" 2>&1 && bad only_unknown || ok only_unknown

# on both against a synthesised netlist: rtl vs the design in hand after abc
"$odin3" -p "$prep; abc --lut 6; run_testbenches --only both" >"$work/abc.log" 2>&1 && ok both_abc || { bad both_abc; tail -5 "$work/abc.log"; }

# no testbench in the project: prints that and passes
"$odin3" -p "read_project none.o3proj; run_testbenches" >"$work/none.log" 2>&1 && grep -q 'defines no testbenches' "$work/none.log" && ok none || bad none
# no project read: an error
"$odin3" -p "run_testbenches" >"$work/np.log" 2>&1 && bad no_project || ok no_project
rm -rf "$work"
exit $fail
