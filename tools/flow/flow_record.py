"""The Mimir run record of one odin3-flow run (`mimir-run.json` in `--out`).

Built from what the run itself produced: the commands it ran (`FlowResult.runs`), the logs VPR
wrote (its own echo of the effective options and its `time -v` figures), the binaries and source
trees it used. Nothing is reconstructed later. Format and rules: `runrecord.py`.
"""

from __future__ import annotations

import hashlib
import os
import re
import shlex
import sys
from pathlib import Path
from typing import TYPE_CHECKING

import runrecord as rr

if TYPE_CHECKING:
    from odin3_flow import FlowArgs, FlowResult, StepResult

SOURCE = "odin3-flow"
SEED_FILE = "vpr/src/base/read_options.cpp"
# options that take a value in the flow's odin3 script; any other `-x` is a flag
VALUE_OPTIONS = frozenset((
    "--lut", "--lut-k", "--name-style", "--prov-ast", "--stop-after", "-encoding", "--neg-edge",
    "--async", "--max-mem-bits", "--report", "--separator", "--script", "--gates", "--via-blif",
    "--log", "--prov", "--name-map", "--lower-snapshot"))
# pass -> (file, text) of the line that initialises its options: an option not given keeps that
# initial value (zero, NULL or the listed default)
INIT_LINES = {
    "read_verilog": ("src/frontends/verilog/read_verilog.c",
                     "rv_args parsed = {.parse_only = false, .elab = {0}};"),
    "proc": ("src/passes/proc/proc_pass.c", "memset(&opts, 0, sizeof opts);"),
    "fsm": ("src/passes/fsm/register.c", "memset(&opts, 0, sizeof opts);"),
    "opt": ("src/passes/opt/register.c", "memset(out, 0, sizeof *out);"),
    "flatten": ("src/passes/flatten/register.c", "opts->separator = NULL;"),
    "lower": ("src/passes/lower/register.c", "memset(opts, 0, sizeof *opts);"),
    "abc": ("src/passes/abc/args.c", "*out = (odin3_abc_opts){0};"),
    "write_blif": ("src/passes/builtin.c", "ODIN3_BLIF_DIALECT_PLAIN, ODIN3_BLIF_NAMES_SHORT"),
}
_ABC_LINE = re.compile(r"linked ABC ([0-9a-f]{40})")
_TIME_V_PREFIX = "/usr/bin/env time -v "


def split_script(script: str) -> list[list[str]]:
    """The commands of an odin3 `-p` script: split at ';' outside double quotes, then into words."""
    parts: list[str] = []
    cur: list[str] = []
    quoted = False
    for ch in script:
        if ch == '"':
            quoted = not quoted
        if ch == ";" and not quoted:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return [shlex.split(p) for p in parts if p.strip()]


def script_options(words: list[str]) -> list[tuple[str, str]]:
    """(option, value) of one command's words after the pass name; a flag has the value ''."""
    out: list[tuple[str, str]] = []
    i = 1
    while i < len(words):
        w = words[i]
        if w.startswith("-") and len(w) > 1:
            if w in VALUE_OPTIONS and i + 1 < len(words):
                out.append((w, words[i + 1]))
                i += 1
            else:
                out.append((w, ""))
        i += 1
    return out


def add_script_params(rec: rr.RunRecorder, script: str, tree: Path | None, commit: str) -> None:
    """Every option of every pass on the command line (`given`), and for each pass the initial
    value of the options it was not given, cited `file:line` at the tree's commit."""
    seen: dict[str, int] = {}
    cmds = split_script(script)
    rec.param("odin3.script_passes", " ".join(c[0] for c in cmds), "given", "command line -p")
    for words in cmds:
        name = words[0]
        seen[name] = seen.get(name, 0) + 1
        tag = name if seen[name] == 1 else f"{name}#{seen[name]}"
        opts = script_options(words)
        for opt, val in opts:
            rec.param(f"odin3.{tag}.{opt.lstrip('-')}", val or "(flag)", "given",
                      "command line -p script")
        if not opts:
            rec.param(f"odin3.{tag}.options", "(none)", "given", "command line -p script")
        if seen[name] == 1 and name in INIT_LINES:
            rel, needle = INIT_LINES[name]
            n = rr.line_of(rr.git_show(tree, commit, rel), needle)
            if n:
                rec.param(f"odin3.{name}.unset_options", "initial value of the parser", "default",
                          f"odin3-tree {commit[:8]} {rel}:{n}")
            else:
                rec.param(f"odin3.{name}.unset_options", "", "default-unrecorded",
                          "odin3 source tree or the initialising line not available")


def inner_vpr_runs(run: StepResult) -> list[tuple[str, list[str], float | None, float | None]]:
    """The VPR command lines `run_vtr_flow` ran, from the first line of VPR's own logs, with the
    wall time and peak RSS of the `time -v` block at their end."""
    found = []
    for name in ("vpr.out", "vpr.crit_path.out"):
        log = run.log.parent / "run" / name
        if not log.is_file():
            continue
        text = log.read_text(encoding="utf-8", errors="replace")
        first = text.split("\n", 1)[0]
        if first.startswith(_TIME_V_PREFIX):
            secs, rss = rr.parse_time_v(text)
            found.append((name, shlex.split(first[len(_TIME_V_PREFIX):]), secs, rss))
    return found


def add_steps(rec: rr.RunRecorder, result: FlowResult) -> None:
    for run in result.runs:
        name = "run_vtr_flow" if run.name == "vpr" else run.name
        code = -1 if run.exit_code is None else run.exit_code
        rec.add_step(name, run.argv, code, run.seconds, run.rss_mib)
        if run.name == "vpr":
            for _, argv, secs, rss in inner_vpr_runs(run):
                rec.add_step("vpr", argv, 0, secs, rss, shared=True)


def add_vpr_params(rec: rr.RunRecorder, result: FlowResult, vtr_tree: Path | None,
                   commit: str) -> int:
    """VPR's echo, the run_vtr_flow and final-route flags, the seed. Returns the default seed."""
    runs = [r for r in result.runs if r.name == "vpr"]
    if not runs:
        return 0
    last = runs[-1]
    rec.add_vpr_echo(last.log.parent / "run" / "vpr.out", "vpr")
    for key, vals in rr.cmd_options(list(last.argv[4:])):
        rec.param(f"vpr_flow.cmd.{key}", " ".join(rec.ph(v) for v in vals) or "(flag)", "given",
                  "command line of run_vtr_flow.py")
    inner = inner_vpr_runs(last)
    if len(inner) > 1:  # the final route; its channel width is a result of the first, not a setup
        opts = rr.cmd_options(inner[1][1][3:])
        for key, vals in opts:
            if key not in ("route_chan_width", "circuit_file"):
                rec.param(f"vpr_final.cmd.{key}", " ".join(vals) or "(flag)", "given",
                          "vpr.crit_path.out line 1 (command line)")
    lines = rr.git_show(vtr_tree, commit, SEED_FILE)
    found = rr.option_default(lines or [], "--seed")
    if found:
        rec.param("run.seed", found[0], "default", f"vtr-tree {commit[:8]} {SEED_FILE}:{found[1]}")
        return int(found[0])
    rec.param("run.seed", "", "default-unrecorded", "VPR source tree or commit not available")
    return 0


def combined_netlist(designs: list[Path]) -> str:
    digest = hashlib.sha256()
    for d in designs:
        digest.update(d.read_bytes())
    return digest.hexdigest()


def build_record(args: FlowArgs, result: FlowResult, top: str, share: Path,
                 vtr_root: Path | None) -> rr.RunRecorder:
    designs_root = Path(os.path.commonpath([str(d.parent) for d in args.designs]))
    roots = {"out": args.out, "designs": designs_root, "share": share}
    if vtr_root is not None:
        roots["vtr"] = vtr_root
    uses_vpr = any(r.name == "vpr" for r in result.runs)
    flags = [n for n, on in (("vpr", uses_vpr), ("prov", args.prov), ("analysis", args.analysis),
                             ("timing", args.timing_attr)) if on]
    arm = "+".join([args.target, *flags])
    vpr_argv = next((r.argv for r in result.runs if r.name == "vpr"), ())
    arch_file = Path(vpr_argv[3]) if len(vpr_argv) > 3 else args.arch
    rec = rr.RunRecorder(SOURCE, top, arm, 0, arch_file.stem if arch_file else "none", roots,
                         {args.odin3: "<odin3>"}, top)
    add_steps(rec, result)
    otree = share if (share / ".git").exists() else None
    ocommit = rr.git(otree, "rev-parse", "HEAD") if otree else ""
    entry = rr.tool_entry(otree, args.odin3)
    rec.add_tool("odin3", entry)
    abc = _ABC_LINE.search(str(entry["binary_version"]))
    rec.add_linked_tool("abc", abc.group(1) if abc else "",
                        "odin3 --version, line 'linked ABC'")
    vtr_commit = ""
    if uses_vpr and vtr_root is not None:
        ventry = rr.tool_entry(vtr_root, vtr_root / "vpr" / "vpr")
        rec.add_tool("vpr", ventry)
        vtr_commit = str(ventry["commit"])
    odin_run = next((r for r in result.runs if r.name == "odin3"), None)
    if odin_run is not None:
        argv = list(odin_run.argv)
        for key, vals in rr.cmd_options(argv[1:argv.index("-p")]):
            if key != "top":  # the design's top module is the record's `design`
                rec.param(f"odin3.cmd.{key}", " ".join(vals) or "(flag)", "given", "command line")
        add_script_params(rec, argv[argv.index("-p") + 1], otree, ocommit)
    rec.param("flow.target", args.target, "given", "command line --target")
    for flag in ("prov", "analysis", "timing_attr", "timing_legacy", "keep_results"):
        rec.param(f"flow.{flag}", str(bool(getattr(args, flag))).lower(), "given",
                  f"command line --{flag.replace('_', '-')} (off when absent)")
    if args.timing_attr:
        rec.param("flow.timing_paths", str(args.timing_paths), "given", "command line or default")
    seed = add_vpr_params(rec, result, vtr_root, vtr_commit)
    if not uses_vpr:
        rec.param("run.seed", "none", "given", "odin3-flow passes no seed (the flow has no seed)")
    rec.rec["seed"] = seed
    add_files(rec, args, result, top, vpr_argv)
    return rec


def add_files(rec: rr.RunRecorder, args: FlowArgs, result: FlowResult, top: str,
              vpr_argv: tuple[str, ...]) -> None:
    for i, d in enumerate(args.designs):
        rec.add_input(f"design_{i}", d)
    if len(args.designs) == 1:
        rec.add_input("netlist", args.designs[0])
    else:
        rec.add_file_entry("inputs", "netlist", rec.ph(str(args.designs[0])),
                           combined_netlist(args.designs))
        rec.param("inputs.netlist_files", str(len(args.designs)), "given",
                  "netlist sha256 is over the design files concatenated in command-line order")
    if len(vpr_argv) > 3:
        rec.add_input("arch", Path(vpr_argv[3]))
    elif args.arch is not None:
        rec.add_input("arch", args.arch)
    for role, rel in (("blif", f"odin3/{top}.blif"), ("name_map", f"odin3/{top}.blif.names"),
                      ("lpm_verilog", f"odin3/{top}.v"), ("odin3_log", "odin3/odin3.log"),
                      ("vpr_log", "vpr/vpr.log"), ("summary", "summary.json")):
        if (args.out / rel).is_file():
            rec.add_artifact(role, args.out / rel)


def write_record(args: FlowArgs, result: FlowResult, top: str, share: Path,
                 vtr_root: Path | None) -> Path | None:
    """Write `<out>/mimir-run.json`; a privacy hit is logged (rule and key path) and no record is
    written, the flow's own result is not changed."""
    path = args.out / rr.FILE_NAME
    try:
        build_record(args, result, top, share, vtr_root).write(path)
    except rr.RecordRefused as exc:
        print(f"odin3_flow: no run record written: {exc}", file=sys.stderr)
        return None
    return path
