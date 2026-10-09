#!/usr/bin/env bash
# test_cli.sh — end-to-end tests of odin3 pass scripts (run by CTest as cli_<case>).
#
# usage: tests/cli/test_cli.sh CASE ODIN3 NETLIST_COMPARE FIXTURES_DIR
# Cases: script_roundtrip, p_roundtrip (read, check, write; netlist-compare identical),
# unknown_pass_p, unknown_pass_script (located error, nonzero exit, nothing written), top (--top
# wins over the BLIF first model), check_flag (--check accepted), usage (bad arguments exit 2).
set -euo pipefail

[[ $# -eq 4 ]] || { echo "usage: test_cli.sh CASE ODIN3 NETLIST_COMPARE FIXTURES_DIR" >&2; exit 2; }
case_name=$1
odin3=$2
compare=$3
fixtures=$4
fixture=$fixtures/hand_body.blif

work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT

fail() {
    echo "test_cli $case_name: FAIL: $*" >&2
    [[ -f $work/err ]] && sed 's/^/    stderr: /' "$work/err" >&2
    exit 1
}

# expect_fail <needle> <odin3 args...>: odin3 must exit nonzero and print needle on stderr.
expect_fail() {
    local needle=$1
    shift
    if "$odin3" "$@" >"$work/out" 2>"$work/err"; then
        fail "odin3 $* succeeded"
    fi
    grep -qF -- "$needle" "$work/err" || fail "stderr lacks: $needle"
}

case $case_name in
script_roundtrip)
    printf '# round trip\nread_blif %s\ncheck\nwrite_blif %s\n' "$fixture" "$work/out.blif" \
        >"$work/rt.o3"
    "$odin3" "$work/rt.o3" 2>"$work/err" || fail "odin3 script failed"
    "$compare" "$fixture" "$work/out.blif" || fail "netlist-compare: not identical"
    ;;
p_roundtrip)
    "$odin3" -p "read_blif $fixture; check; write_blif $work/out.blif" 2>"$work/err" ||
        fail "odin3 -p failed"
    "$compare" "$fixture" "$work/out.blif" || fail "netlist-compare: not identical"
    ;;
unknown_pass_p)
    expect_fail "-p: command 2: unknown pass 'nope'" \
        -p "read_blif $fixture; nope x; write_blif $work/out.blif"
    [[ ! -e $work/out.blif ]] || fail "a pass ran before the script was rejected"
    ;;
unknown_pass_script)
    printf 'read_blif %s\n# comment\ncheck; nope\nwrite_blif %s\n' "$fixture" "$work/out.blif" \
        >"$work/bad.o3"
    expect_fail "$work/bad.o3:3: unknown pass 'nope'" "$work/bad.o3"
    [[ ! -e $work/out.blif ]] || fail "a pass ran before the script was rejected"
    ;;
top)
    "$odin3" --top sub -p "read_blif $fixture; stats" 2>"$work/err" || fail "odin3 --top failed"
    grep -qF "stats: design: modules 2, top sub" "$work/err" || fail "top is not sub"
    expect_fail "hierarchy: no module named 'nope'" --top nope -p "read_blif $fixture"
    ;;
check_flag)
    "$odin3" --check -p "read_blif $fixture; check --fast" 2>"$work/err" || fail "--check failed"
    ;;
usage)
    set +e
    "$odin3" -p 2>"$work/err"
    rc=$?
    "$odin3" --bogus 2>>"$work/err"
    rc2=$?
    set -e
    [[ $rc -eq 2 && $rc2 -eq 2 ]] || fail "exit codes $rc $rc2, expected 2 2"
    expect_fail "cannot read the script" "$work/missing.o3"
    ;;
*)
    echo "test_cli.sh: unknown case $case_name" >&2
    exit 2
    ;;
esac
echo "test_cli $case_name: OK"
