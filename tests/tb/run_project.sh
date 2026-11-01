#!/bin/sh
# run_project.sh <odin3>: the golden project tests/tb/project (spec
# docs/specs/2026-10-31-project-testbench-design.md section 6b). `run_testbenches` runs four
# testbenches: good (pass), bad (a deliberately wrong expect file: fail, mismatch reported), both
# (rtl vs netlist) and err (an unsupported construct: error). The good expect file is also checked
# against iverilog (the oracle), on the RTL and on the abc netlist (written with write_verilog), so
# `on netlist` and `on both` are judged by iverilog too. project2.o3proj covers `on netlist`, the dut
# check, the time limits (a hang fails, with the default and with max-time), a stale file and a
# long name; project.qsf the qsf import end to end (a data file listed before the testbench, a
# hyphenated section id, NativeLink's choice, a time-boxed run). Prints PASS/FAIL/SKIP per check.
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
have_iv=0
command -v iverilog >/dev/null 2>&1 && command -v vvp >/dev/null 2>&1 && have_iv=1
if [ $have_iv -eq 1 ]; then
    iverilog -g2005 -o "$work/good.vvp" tb_good.v counter.v && vvp "$work/good.vvp" >"$work/good.ref" 2>&1
    sed -i '/\$finish called/d' "$work/good.ref"
    if diff -u good.expect "$work/good.ref" >"$work/oracle.diff"; then ok oracle; else bad oracle; head -8 "$work/oracle.diff"; fi
    # the netlist side: the abc netlist as Verilog, same testbench, same expect file
    if "$odin3" -p "$prep; abc --lut 6; write_verilog $work/net.v" >"$work/wv.log" 2>&1 \
        && iverilog -g2005 -o "$work/net.vvp" tb_good.v "$work/net.v" 2>"$work/net.err" \
        && vvp "$work/net.vvp" >"$work/net.ref" 2>&1; then
        sed -i '/\$finish called/d' "$work/net.ref"
        if diff -u good.expect "$work/net.ref" >"$work/net.diff"; then ok oracle_netlist; else bad oracle_netlist; head -8 "$work/net.diff"; fi
    else
        bad oracle_netlist; head -3 "$work/wv.log" "$work/net.err"
    fi
else
    echo "SKIP oracle (no iverilog/vvp on PATH)"
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
if cmp -s "$work/r.txt" "$work/all.log" || grep -q '^good pass rtl' "$work/r.txt"; then ok report_file; else bad report_file; fi
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
if "$odin3" -p "$prep; run_testbenches --only nosuch" >"$work/one.log" 2>&1; then bad only_unknown
elif grep -q 'no testbench named `nosuch`' "$work/one.log"; then ok only_unknown; else bad only_unknown; fi

# on both against a synthesised netlist: rtl vs the design in hand after abc
"$odin3" -p "$prep; abc --lut 6; run_testbenches --only both" >"$work/abc.log" 2>&1 && ok both_abc || { bad both_abc; tail -5 "$work/abc.log"; }

# project2: each testbench alone (--only)
p2='read_project project2.o3proj; read_verilog; proc; opt; lower; abc --lut 6'
# run_only <name> <expected exit 0|1> <grep pattern in the output> <check name>
run_only() {
    "$odin3" -p "$p2; run_testbenches --only $1" >"$work/p2.log" 2>&1
    code=$?
    want_ok=0
    if [ "$2" = 0 ]; then [ $code -eq 0 ] && want_ok=1; else [ $code -ne 0 ] && want_ok=1; fi
    if [ $want_ok -eq 1 ] && grep -q "$3" "$work/p2.log"; then
        ok "$4"
    else
        bad "$4"; tail -6 "$work/p2.log"
    fi
}
run_only netonly 0 '^netonly pass netlist' netlist_only
run_only nodut 1 'error: run (rtl): ODIN3_ERR_INVALID_ARG: run_testbenches: dut `nosuch` is not instantiated' dut_missing_instance
run_only hang_default 1 'timeout: no \$finish by time 10000000 (default limit' hang_default_fails
run_only hang_max 1 'timeout: no \$finish by time 1000 (max-time limit' hang_max_fails
run_only stale 1 'error: run (rtl): ODIN3_ERR_IO: run_testbenches: cannot read' stale_file_errors
grep -q "file not found: '.*gone.v'" "$work/p2.log" && ok stale_file_warned_at_read || bad stale_file_warned_at_read
long=$(printf "tb_with_a_very_long_name_%.0s" 1 2 3 4 5 6 7 8 9 10)
run_only "$long" 0 "^$long pass rtl" long_name_only
"$odin3" -p "$p2; run_testbenches --json $work/p2.json" >"$work/p2.log" 2>&1
python3 - "$work/p2.json" <<'PY' && ok json_timeout || bad json_timeout
import json, sys
by = {t["name"]: t for t in json.load(open(sys.argv[1]))["testbenches"]}
assert by["hang_default"]["status"] == "fail" and by["hang_default"]["timeout"] is True
assert by["netonly"]["timeout"] is False and by["netonly"]["status"] == "pass"
assert by["nodut"]["status"] == "error" and "nosuch" in by["nodut"]["error"]
PY
# the scratch RTL design is built once per run: the project is read twice (the script and the
# scratch design), not once per rtl testbench, so the stale-file WARNING appears twice
n=$(grep -c "file not found: '.*gone.v'" "$work/p2.log")
[ "$n" -eq 2 ] && ok rtl_built_once || { bad rtl_built_once; echo "$n warnings"; }

# the qsf import end to end: NativeLink picks qgood (the data file TRACEFILE.txt before
# tb_good.v is skipped with a WARNING), Testbench-hang is disabled until named with --only, and
# a qsf RUN_SIM_FOR run is time-boxed: stopping at the limit passes
pq='read_project --format qsf project.qsf; read_verilog; proc; opt; lower'
"$odin3" -p "$pq; run_testbenches" >"$work/q.log" 2>&1 && grep -q '^qgood pass rtl' "$work/q.log" \
    && grep -q '1 testbenches disabled by the project not run' "$work/q.log" \
    && grep -q 'not a Verilog file.*TRACEFILE.txt' "$work/q.log" && ok qsf_default || { bad qsf_default; tail -6 "$work/q.log"; }
"$odin3" -p "$pq; run_testbenches --only Testbench-hang" >"$work/q.log" 2>&1 && grep -q '^Testbench-hang pass rtl' "$work/q.log" \
    && ok qsf_timeboxed_passes || { bad qsf_timeboxed_passes; tail -6 "$work/q.log"; }

# no testbench in the project: prints that and passes
"$odin3" -p "read_project none.o3proj; run_testbenches" >"$work/none.log" 2>&1 && grep -q 'defines no testbenches' "$work/none.log" && ok none || bad none
# --only on a project without testbenches: an unknown name is an error
if "$odin3" -p "read_project none.o3proj; run_testbenches --only zz" >"$work/nz.log" 2>&1; then bad only_zz_no_tb
elif grep -q 'no testbench named `zz`' "$work/nz.log"; then ok only_zz_no_tb; else bad only_zz_no_tb; fi
# no project read: an error
"$odin3" -p "run_testbenches" >"$work/np.log" 2>&1 && bad no_project || ok no_project
rm -rf "$work"
exit $fail
