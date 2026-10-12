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
# the same seed gives the same file; without a file a summary is logged), project_dump
# (--project and -f with --dump-project print the record; a --project without a script runs
# read_project alone and exits with its status), project_top (--top replaces the project's top
# with an info line and counts as applied), project_revision (--revision reaches read_project),
# project_diag_paths (--diag-paths given|resolved names the file of a located error),
# project_preprocess (preprocess --out d --segments writes 0.v, 0.segments.json and
# files-read.txt; without --out the text goes to stdout), parse_only (--parse-only a.v b.v is
# -p "read_verilog --parse-only a.v b.v"; with --project or -f it reads the record's files; a
# syntax error fails), parse_only_with_script (--parse-only with -p or a .o3 script exits 2:
# use the pass option), abc_script_missing (--abc-script naming no file exits 2),
# abc_script_project (--abc-script replaces the project's abc_script with an info line; --help
# shows the option). The project cases run from the project's directory (tests/cli/project), so
# the paths given are relative.
set -euo pipefail

[[ $# -eq 4 ]] || { echo "usage: test_cli.sh CASE ODIN3 NETLIST_COMPARE FIXTURES_DIR" >&2; exit 2; }
case_name=$1
odin3=$2
compare=$3
fixtures=$4
fixture=$fixtures/hand_body.blif

work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT

project=$fixtures/../../cli/project
corpus=$fixtures/../projects

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
project_dump)
    cd "$project"
    "$odin3" --project p.o3proj --dump-project >"$work/out" 2>"$work/err" ||
        fail "--project --dump-project failed"
    grep -qF '"top": "top",' "$work/out" || fail "the record lacks its top"
    grep -qF "\"files\": [{\"path\": \"$(pwd -P)/src/top.v\", \"language\": \"verilog\"" \
        "$work/out" || fail "the record lacks the resolved file"
    "$odin3" -f files.f --dump-project >"$work/out_f" 2>"$work/err" || fail "-f --dump-project failed"
    grep -qF '"top": null,' "$work/out_f" || fail "a file list has no top"
    grep -qF '"defines": [{"name": "WIDTH", "value": "4"}],' "$work/out_f" ||
        fail "the list's define is missing"
    "$odin3" --project p.o3proj >"$work/out" 2>"$work/err" || fail "--project alone failed"
    grep -qF "pass read_project: " "$work/err" || fail "read_project did not run"
    [[ ! -s $work/out ]] || fail "--project alone printed something"
    expect_fail "odin3: error: missing_file: project not found: 'none.o3proj'" --project none.o3proj
    expect_fail "dump_project: no project was read" -p "check" --dump-project
    ;;
project_top)
    cd "$project"
    "$odin3" --top other --project p.o3proj --dump-project >"$work/out" 2>"$work/err" ||
        fail "--top --project failed"
    grep -qF "info: p.o3proj: --top other replaces the project top top" "$work/err" ||
        fail "no replacement info line"
    grep -qF '"top": "other",' "$work/out" || fail "--top did not replace the top"
    ! grep -qF "was not applied" "$work/err" || fail "spurious --top warning"
    "$odin3" --top other -p "read_project p.o3proj" 2>"$work/err" || fail "-p read_project failed"
    ! grep -qF "was not applied" "$work/err" || fail "spurious --top warning (script)"
    "$odin3" --top other -p "stats" 2>"$work/err" || fail "stats failed"
    grep -qF "warning: --top other was not applied" "$work/err" || fail "no unapplied warning"
    ! grep -qF "error" "$work/err" || fail "an error was logged"
    ;;
project_revision)
    cd "$corpus/qpf_revisions"
    "$odin3" --revision fast -p "read_project proj.qpf; dump_project" >"$work/out" \
        2>"$work/err" || fail "--revision fast failed"
    grep -qF '"value": "2"' "$work/out" || fail "revision fast's parameter is missing"
    expect_fail "ambiguous_revision" -p "read_project proj.qpf"
    ;;
project_diag_paths)
    cd "$project"
    expect_fail "bad.o3proj:1:1: error: unknown_key: unknown key: 'nonsense'" --project bad.o3proj
    grep -qF "/bad.o3proj:1:1:" "$work/err" && fail "given style printed an absolute path"
    expect_fail "$(pwd -P)/bad.o3proj:1:1: error: unknown_key: unknown key: 'nonsense'" \
        --diag-paths resolved --project bad.o3proj
    expect_fail "bad.o3proj:1:1: error: unknown_key" --diag-paths given --project bad.o3proj
    set +e
    "$odin3" --diag-paths sideways --project p.o3proj 2>"$work/err"
    rc=$?
    set -e
    [[ $rc -eq 2 ]] || fail "--diag-paths sideways: exit code $rc, expected 2"
    ;;
project_preprocess)
    cd "$project"
    "$odin3" --project p.o3proj -p "preprocess --out $work/d --segments" 2>"$work/err" ||
        fail "preprocess --out --segments failed"
    [[ -s $work/d/0.v && -s $work/d/0.segments.json && -s $work/d/files-read.txt ]] ||
        fail "missing preprocess outputs"
    grep -qF "assign y = ~(a);" "$work/d/0.v" || fail "0.v is not expanded"
    grep -qF '"buffer_kind": "macro_arg", "macro": "INV"' "$work/d/0.segments.json" ||
        fail "0.segments.json lacks the argument segment"
    # The spelling (spelling steps chased to the FILE buffer, 2B §7.8) of a body segment and of
    # a project define's body, which the fuzzers check from the outside (§10.5).
    grep -qF '"macro": "INV", "spell_file": "defs.vh", "spell_line": 1, "spell_col": 16}' \
        "$work/d/0.segments.json" || fail "0.segments.json lacks the body's spelling"
    grep -qF '"spell_file": "<command line>", "spell_line": 1, "spell_col": 15}' \
        "$work/d/0.segments.json" || fail "0.segments.json lacks the define's spelling"
    printf '%s\n' "$(pwd -P)/src/top.v" "$(pwd -P)/inc/defs.vh" >"$work/want"
    diff "$work/want" "$work/d/files-read.txt" >/dev/null || fail "files-read.txt differs"
    "$odin3" -f files.f -p preprocess >"$work/out" 2>"$work/err" || fail "preprocess failed"
    cmp -s "$work/out" "$work/d/0.v" || fail "stdout differs from 0.v"
    expect_fail "include_not_found" --project "$corpus/neg_include_not_found/project.o3proj" \
        -p preprocess
    ;;
parse_only)
    printf 'module a (input wire i, output wire o);\n    b u_b (.i(i), .o(o));\nendmodule\n' \
        >"$work/a.v"
    printf 'module b (input wire i, output wire o);\n    assign o = i;\nendmodule\n' >"$work/b.v"
    # odin3 --parse-only a.v b.v is -p "read_verilog --parse-only a.v b.v": the same summary.
    "$odin3" --parse-only "$work/a.v" "$work/b.v" 2>"$work/err" || fail "--parse-only failed"
    grep -o 'read_verilog: 2 units (0 library), [0-9]* nodes, [0-9]* comments, [0-9]* bytes' \
        "$work/err" >"$work/sugar" || fail "no summary line for --parse-only"
    "$odin3" -p "read_verilog --parse-only $work/a.v $work/b.v" 2>"$work/err" ||
        fail "-p read_verilog --parse-only failed"
    grep -o 'read_verilog: 2 units (0 library), [0-9]* nodes, [0-9]* comments, [0-9]* bytes' \
        "$work/err" >"$work/script" || fail "no summary line for the pass"
    cmp -s "$work/sugar" "$work/script" || fail "--parse-only differs from the pass"
    # With --project or -f: read_project, then read_verilog --parse-only (the record's files).
    cd "$project"
    "$odin3" --project p.o3proj --parse-only 2>"$work/err" || fail "--project --parse-only failed"
    grep -qF 'read_verilog: 1 units (0 library)' "$work/err" || fail "--project: no summary"
    "$odin3" -f files.f --parse-only 2>"$work/err" || fail "-f --parse-only failed"
    grep -qF 'read_verilog: 1 units (0 library)' "$work/err" || fail "-f: no summary"
    # A located syntax error fails the run.
    printf 'module c;\n    wire a b;\nendmodule\n' >"$work/c.v"
    expect_fail "c.v:2:12: error: syntax: unexpected identifier" --parse-only "$work/c.v"
    ;;
parse_only_with_script)
    printf 'module b;\nendmodule\n' >"$work/b.v"
    set +e
    "$odin3" --parse-only -p "stats" "$work/b.v" 2>"$work/err"
    rc=$?
    set -e
    [[ $rc -eq 2 ]] || fail "--parse-only with -p: exit code $rc, expected 2"
    grep -qF -- "--parse-only cannot be combined with a script: use the pass option" \
        "$work/err" || fail "stderr lacks the message"
    # A script file (.o3) under --parse-only is refused the same way, not parsed as Verilog.
    printf 'stats\n' >"$work/flow.o3"
    set +e
    "$odin3" --parse-only "$work/flow.o3" 2>"$work/err"
    rc=$?
    set -e
    [[ $rc -eq 2 ]] || fail "--parse-only with a script file: exit code $rc, expected 2"
    grep -qF -- "use the pass option" "$work/err" || fail "script file: stderr lacks the message"
    ;;
abc_script_missing)
    cd "$work"
    set +e
    "$odin3" --abc-script nope.abc -p stats 2>"$work/err"
    rc=$?
    set -e
    [[ $rc -eq 2 ]] || fail "--abc-script nope.abc: exit code $rc, expected 2"
    grep -qxF "odin3: --abc-script nope.abc: no such file" "$work/err" ||
        fail "stderr lacks the message"
    mkdir "$work/dir.abc"
    set +e
    "$odin3" --abc-script dir.abc -p stats 2>"$work/err"
    rc=$?
    set -e
    [[ $rc -eq 2 ]] || fail "--abc-script on a directory: exit code $rc, expected 2"
    grep -qxF "odin3: --abc-script dir.abc: no such file" "$work/err" ||
        fail "a directory: stderr lacks the message"
    ;;
abc_script_project)
    cp -R "$corpus/o3proj_abc_script" "$work/p"
    mv "$work/p/project.o3proj" "$work/p/p.o3proj"
    printf 'strash\n' >"$work/p/cli.abc"
    cd "$work/p"
    "$odin3" --abc-script cli.abc --project p.o3proj -p dump_project >"$work/out" \
        2>"$work/err" || fail "--abc-script --project failed"
    grep -qF "\"abc_script\": \"$(pwd -P)/cli.abc\"," "$work/out" ||
        fail "the dump's abc_script is not cli.abc's resolved path"
    grep -qF "info: p.o3proj: --abc-script cli.abc replaces the project abc_script abc/map.abc" \
        "$work/err" || fail "no replacement info line"
    "$odin3" --project p.o3proj -p dump_project >"$work/out" 2>"$work/err" ||
        fail "--project alone failed"
    grep -qF "\"abc_script\": \"$(pwd -P)/abc/map.abc\"," "$work/out" ||
        fail "without --abc-script the project's script is not kept"
    ! grep -qF "replaces the project abc_script" "$work/err" || fail "spurious info line"
    "$odin3" --help >"$work/out" 2>"$work/err" || fail "--help failed"
    grep -qxF "  --abc-script FILE the ABC script of every abc pass (below abc --script)" \
        "$work/out" || fail "--help lacks the --abc-script line"
    ;;
*)
    echo "test_cli.sh: unknown case $case_name" >&2
    exit 2
    ;;
esac
echo "test_cli $case_name: OK"
