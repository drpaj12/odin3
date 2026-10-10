#!/usr/bin/env bash
# test_cli.sh — end-to-end tests of odin3 pass scripts (run by CTest as cli_<case>).
#
# usage: tests/cli/test_cli.sh CASE ODIN3 NETLIST_COMPARE FIXTURES_DIR
# Cases: script_roundtrip, p_roundtrip (read, check, write; netlist-compare identical),
# unknown_pass_p, unknown_pass_script, unknown_pass_later_script (located error, nonzero exit,
# nothing written, even when the typo is in a later -p), top (--top wins over the BLIF first
# model), top_written_first (write_blif puts the top first, so it reads back as the top),
# top_unapplied (an unused --top is a warning; one a later
# `hierarchy --top` overrode is not), quoted (quoted paths; an unterminated quote is a
# located error), help_runs_nothing, check_flag (--check accepted; its Release semantics are unit
# tested), usage (bad arguments exit 2), techlib (read_techlib, a BLIF using its multiply, write),
# plugin (the example plugin's pass and cell type from a script; $ODIN3_EXAMPLE_PLUGIN names it;
# the load notice is logged on stderr, stdout stays empty), writers (write_json, write_verilog and
# write_dot from one script, their options and refusals), sim (the sim pass's vectors equal
# odin3-sim-vectors' for the same file and seed, title line aside; $ODIN3_SIM_VECTORS names it;
# the same seed gives the same file; without a file a summary is logged).
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
    expect_fail "read_blif: --top: no module named 'nope'" --top nope -p "read_blif $fixture"
    ;;
unknown_pass_later_script)
    expect_fail "-p: command 1: unknown pass 'nope'" \
        -p "read_blif $fixture; write_blif $work/out.blif" -p "nope"
    [[ ! -e $work/out.blif ]] || fail "a pass ran before the later script was rejected"
    ;;
top_written_first)
    "$odin3" --top sub -p "read_blif $fixture; write_blif $work/out.blif" 2>"$work/err" ||
        fail "odin3 --top sub write failed"
    "$odin3" -p "read_blif $work/out.blif; stats" 2>"$work/err" || fail "read back failed"
    grep -qF "stats: design: modules 2, top sub" "$work/err" || fail "read-back top is not sub"
    ;;
top_unapplied)
    "$odin3" --top zzz -p "stats" 2>"$work/err" || fail "odin3 failed"
    grep -qF "warning: --top zzz was not applied" "$work/err" || fail "no warning"
    "$odin3" --top sub -p "read_blif $fixture; hierarchy --top top; stats" 2>"$work/err" ||
        fail "odin3 --top with hierarchy --top failed"
    grep -qF "stats: design: modules 2, top top" "$work/err" || fail "hierarchy --top lost"
    ! grep -qF "was not applied" "$work/err" || fail "spurious --top warning"
    ;;
quoted)
    "$odin3" -p "read_blif \"$fixture\"; write_blif \"$work/out dir;#.blif\" # done" \
        2>"$work/err" || fail "quoted paths failed"
    "$compare" "$fixture" "$work/out dir;#.blif" || fail "netlist-compare: not identical"
    expect_fail "-p: command 2: unterminated quote" -p "read_blif $fixture; write_blif \"x"
    ;;
help_runs_nothing)
    "$odin3" --help -p "read_blif $fixture; write_blif $work/out.blif" >"$work/out" 2>"$work/err" ||
        fail "odin3 --help failed"
    grep -qF "hierarchy [--top <name>]" "$work/out" || fail "help lacks the pass list"
    [[ ! -e $work/out.blif ]] || fail "--help ran the script"
    ;;
techlib)
    # read_techlib, then a BLIF instantiating the library's multiply, then write: the round trip
    # is identical, and a missing library is a nonzero exit.
    lib=$fixtures/../../../lib/vtr.o3lib
    mult=$fixtures/../techlib/multiply.parmys.01.blif
    "$odin3" -p "read_techlib $lib; read_blif $mult; check; stats; write_blif $work/out.blif" \
        2>"$work/err" || fail "odin3 read_techlib script failed"
    grep -qF "pass read_techlib: " "$work/err" || fail "read_techlib did not run"
    grep -qF "multiply 1" "$work/err" || fail "stats lacks the multiply cell"
    "$compare" "$mult" "$work/out.blif" || fail "netlist-compare: not identical"
    expect_fail "cannot open" -p "read_techlib $work/none.o3lib"
    ;;
plugin)
    "$odin3" --plugin "${ODIN3_EXAMPLE_PLUGIN:?}" \
        -p "read_blif $fixtures/../../cli/example_plugin.blif; check; example_count" \
        2>"$work/err" || fail "odin3 with the example plugin failed"
    grep -qF "example_count: 2 example_and cells" "$work/err" || fail "no example_count line"
    "$odin3" --plugin "$ODIN3_EXAMPLE_PLUGIN" >"$work/out" 2>"$work/err" || fail "load failed"
    grep -qF "info: loaded plugin $ODIN3_EXAMPLE_PLUGIN" "$work/err" || fail "no load notice"
    [[ ! -s $work/out ]] || fail "stdout is not empty"
    ;;
writers)
    "$odin3" -p "read_blif $fixture; write_json $work/a.json; write_verilog $work/a.v;
        write_verilog $work/p.v --name-style provenance; write_dot $work/a.dot --max-nodes 100;
        write_dot $work/f.dot --focus top/u_sub" 2>"$work/err" || fail "writer script failed"
    grep -qF '"creator": "Odin III"' "$work/a.json" || fail "a.json is not Odin III JSON"
    grep -qF "module top" "$work/a.v" || fail "a.v lacks module top"
    grep -qF "@" "$work/p.v" || fail "p.v has no provenance names"
    head -1 "$work/a.dot" | grep -qF "digraph" || fail "a.dot is not a digraph"
    [[ -s $work/f.dot ]] || fail "focused dot is empty"
    expect_fail "dot: refusing to draw" -p "read_blif $fixture; write_dot $work/b.dot --max-nodes 2"
    expect_fail "--name-style expects short or provenance" \
        -p "read_blif $fixture; write_verilog $work/b.v --name-style long"
    expect_fail "write_json: expects one path" -p "read_blif $fixture; write_json"
    ;;
sim)
    ff=$fixtures/ff.parmys.blif
    "$odin3" -p "read_blif $ff; sim 32 5 $work/a.vec; sim 32 5 $work/b.vec; sim 32 5" \
        2>"$work/err" || fail "sim script failed"
    cmp -s "$work/a.vec" "$work/b.vec" || fail "same seed, different vectors"
    head -1 "$work/a.vec" | grep -qxF "# odin3 sim dff seed 5 cycles 32" || fail "bad title line"
    "${ODIN3_SIM_VECTORS:?}" "$ff" --seed 5 --cycles 32 >"$work/ref.vec" 2>>"$work/err" ||
        fail "odin3-sim-vectors failed"
    diff <(tail -n +2 "$work/a.vec") <(tail -n +2 "$work/ref.vec") >/dev/null ||
        fail "sim vectors differ from odin3-sim-vectors"
    last=$(tail -1 "$work/a.vec")
    grep -qF "sim: dff: 32 cycles, seed 5; last cycle: $last" "$work/err" || fail "no summary"
    expect_fail "sim: expects <cycles> <seed>" -p "read_blif $ff; sim x 1"
    expect_fail "sim: the design has no top module" -p "sim 4 1"
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
keep_ast)
    "$odin3" --keep-ast -p "read_blif $fixture; check" 2>"$work/err" || fail "--keep-ast failed"
    "$odin3" --keep-ast=all -p "check" 2>"$work/err" || fail "--keep-ast=all failed"
    "$odin3" --help >"$work/out" 2>"$work/err" || fail "--help failed"
    grep -qF -- "--keep-ast=all" "$work/out" || fail "--help lacks --keep-ast=all"
    grep -qF -- "write_verilog_ast <file>" "$work/out" || fail "--help lacks write_verilog_ast"
    set +e
    "$odin3" --keep-ast=bogus -p "check" 2>"$work/err"
    rc=$?
    set -e
    [[ $rc -eq 2 ]] || fail "--keep-ast=bogus: exit code $rc, expected 2"
    expect_fail "write_verilog_ast: the design holds no AST" \
        --keep-ast -p "read_blif $fixture; write_verilog_ast $work/out.v"
    ;;
*)
    echo "test_cli.sh: unknown case $case_name" >&2
    exit 2
    ;;
esac
echo "test_cli $case_name: OK"
