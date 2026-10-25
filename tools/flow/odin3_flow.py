"""odin3_flow: take a design through Odin III, and optionally VPR, in one command (7.3, spec 7
section 2.1).

    odin3_flow.py design.v [more.v ...] --top TOP [--target vtr|lpm] [--arch ARCH.xml]
        [--family NAME] [--vpr | --no-vpr] [--analysis] [--timing-attr] [--timing-paths N]
        [--odin3 PATH] [--out DIR]

Target ``vtr``: the Odin III script of the vtr-qor O leg (imported from tools/vtr-qor, never
copied) writes ``odin3/<top>.blif``. Target ``lpm``: the 5.1 LPM script (tools/flow/lpm.script)
writes ``odin3/<top>.v`` and a read-back lint (``read_verilog -icells``) follows. Every step runs in
its own subdirectory of ``--out`` with its log. Exit codes: 0 ok, 1 a step failed, 2 usage error.
The module is importable with tools/vtr-qor on PYTHONPATH (the wrapper sets it):
``run_flow(FlowArgs) -> FlowResult``.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

import qor_legs
import qor_parse
import timing_attr
from qor_heavy import HeavyError

HERE = Path(__file__).resolve().parent
SHARE_ENV = "ODIN3_SHARE"
LPM_SCRIPT_REL = Path("tools") / "flow" / "lpm.script"
LPM_LIB_REL = Path("lib") / "lpm.o3lib"
DEFAULT_ARCH = "k6"
DEFAULT_K = 6
STEP_TIMEOUT_S = 3600
DOT_MAX_NODES = 100000
NOT_HARD = ("clb", "io", "EMPTY", "inpad", "outpad")  # packer block types that are not hard blocks
TARGETS = ("vtr", "lpm")
VPR_DEFAULT_PATHS = timing_attr.DEFAULT_PATHS  # --timing_report_npaths when VPR is not told
_MODULE = re.compile(r"^\s*module\s+\\?([A-Za-z_][A-Za-z0-9_$]*)", re.M)
_HARD_BLOCKS = re.compile(r"^Netlist (\w+) blocks:\s*(\d+)", re.M)
_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


class UsageError(Exception):
    """A bad command line or input: exit 2."""


@dataclass(frozen=True)
class FlowArgs:
    designs: list[Path]
    top: str | None
    target: str
    arch: Path | None
    family: str
    vpr: bool
    analysis: bool
    odin3: Path
    out: Path
    timing_attr: bool = False  # --timing-attr: attribute the critical path to the HDL
    timing_paths: int = VPR_DEFAULT_PATHS  # setup paths VPR reports (and the tool attributes)


@dataclass(frozen=True)
class StepResult:
    name: str
    ok: bool
    log: Path
    seconds: float


@dataclass
class FlowResult:
    ok: bool
    steps: list[StepResult] = field(default_factory=list)
    summary: dict[str, object] = field(default_factory=dict)
    vpr_run_dir: Path | None = None  # the VPR run directory of the routed result


def find_tops(designs: Sequence[Path]) -> list[str]:
    """The modules of ``designs`` that no text of the files mentions but their own header."""
    text = "\n".join(_COMMENT.sub("", p.read_text(errors="replace")) for p in designs)
    names = list(dict.fromkeys(_MODULE.findall(text)))
    def mentions(name: str) -> int:
        pattern = rf"(?<![A-Za-z0-9_$]){re.escape(name)}(?![A-Za-z0-9_$])"
        return len(re.findall(pattern, text))

    return [n for n in names if mentions(n) == 1]


def resolve_top(designs: Sequence[Path], top: str | None) -> str:
    """``top`` when given, else the one uninstantiated module; UsageError naming the candidates."""
    if top is not None:
        return top
    tops = find_tops(designs)
    if len(tops) == 1:
        return tops[0]
    if not tops:
        raise UsageError("no top module found: give --top")
    raise UsageError(f"more than one top module ({', '.join(tops)}): give --top")


def _has_data(root: Path) -> bool:
    return (root / LPM_LIB_REL).is_file() and (root / LPM_SCRIPT_REL).is_file()


def resource_root() -> Path:
    """The tree holding the data the flow needs (``lib/*.o3lib``, ``lib/abc/vtr.abc``,
    ``tools/flow/lpm.script``, ``tools/vtr-qor``): ``$ODIN3_SHARE``, else the source tree (two
    levels above this script), else ``<script dir>/../../share/odin3`` (the install layout:
    ``share/odin3/{lib,tools/flow,tools/vtr-qor}``). UsageError when none holds the data."""
    given = os.environ.get(SHARE_ENV)
    if given:
        return Path(given)
    for root in (HERE.parents[1], (HERE / ".." / ".." / "share" / "odin3").resolve()):
        if _has_data(root):
            return root
    raise UsageError(f"cannot find the Odin III data files (lib/, tools/flow/lpm.script): "
                     f"set {SHARE_ENV} to the share/odin3 directory")


def quote(path: str | Path) -> str:
    """``path`` as one script argument: double-quoted (a path may hold spaces)."""
    text = os.fspath(path)
    if '"' in text:
        raise UsageError(f"{text}: a path cannot hold a double quote")
    return f'"{text}"'


def lpm_script(designs: Sequence[Path], out: Path, family: str) -> str:
    """The text of tools/flow/lpm.script with its placeholders filled, as lpm-check fills them
    (each path is quoted)."""
    root = resource_root()
    lines = [ln for ln in (root / LPM_SCRIPT_REL).read_text().splitlines()
             if ln.strip() and not ln.startswith("#")]
    fam = f'--family "{family}"' if family else ""
    text = lines[0].replace("@LIB@", quote(root / LPM_LIB_REL))
    text = text.replace("@DESIGN@", " ".join(quote(d) for d in designs))
    return text.replace("@FAMILY@", fam).replace("@OUT@", quote(out)).replace("  ", " ")


def run_step(name: str, argv: Sequence[str], step_dir: Path, env: dict[str, str] | None = None,
             timeout: int = STEP_TIMEOUT_S) -> StepResult:
    """Run ``argv`` in ``step_dir``; stdout and stderr go to its log. A timeout is a failure."""
    step_dir.mkdir(parents=True, exist_ok=True)
    log = step_dir / f"{name}.log"
    start = time.monotonic()
    try:
        with log.open("w") as handle:
            handle.write("$ " + " ".join(argv) + "\n")
            handle.flush()
            proc = subprocess.run(list(argv), cwd=step_dir, stdout=handle, stderr=subprocess.STDOUT,
                                  env=env, timeout=timeout, check=False)
        ok = proc.returncode == 0
    except (OSError, subprocess.TimeoutExpired) as exc:
        log.write_text(f"{log.read_text()}\n{exc}\n")
        ok = False
    return StepResult(name, ok, log, time.monotonic() - start)


def lut_k(args: FlowArgs, vtr_root: Path | None, log: Path) -> int:
    """K from the architecture through VTR's own reader; 6 (stated in ``log``) without VTR."""
    arch = args.arch
    if arch is None and vtr_root is not None:
        arch = qor_legs.arch_xml_path(vtr_root, DEFAULT_ARCH)
    if arch is None or vtr_root is None:
        if args.arch is not None:
            print(f"WARNING: no VTR_ROOT: K = {DEFAULT_K} assumed; the LUT size of "
                  f"{args.arch} is not read", file=sys.stderr)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(f"NOTE: no VTR_ROOT or arch: LUT size K = {DEFAULT_K}\n")
        return DEFAULT_K
    python = vtr_root / ".venv" / "bin" / "python"
    return int(qor_legs.lut_size(vtr_root, python, arch))


def vtr_root_from_env() -> Path | None:
    value = os.environ.get("VTR_ROOT")
    return Path(value) if value else None


def odin3_command(args: FlowArgs, top: str, script: str) -> list[str]:
    return [str(args.odin3), "--diag-paths", "resolved", "--top", top, "-p", script]


LUTS_NOTE = ".names with two or more inputs (constants and one-input buffers or inverters excluded)"


def count_luts(blif: Path) -> int:
    """The ``.names`` of a written BLIF with at least two inputs (``.names a b y`` has two)."""
    return sum(1 for line in blif.read_text(errors="replace").splitlines()
               if line.startswith(".names") and len(line.split()) >= 4)


def summary_from_vpr(parse: Mapping[str, str], qor: Mapping[str, str], vpr_out: str,
                     luts: int) -> dict[str, object]:
    """The VPR keys of ``summary.json`` from VTR's parse results and ``vpr.out``.

    A figure VTR did not report is None. ``hard_blocks`` maps each ``Netlist <type> blocks``
    line of the packer (other than ``clb``, ``io`` and ``EMPTY``) to its count."""
    metrics = qor_parse.metrics_from_parse(parse, qor)
    packed, routed = metrics["packed"], metrics["routed"]
    hard = {m.group(1): int(m.group(2)) for m in _HARD_BLOCKS.finditer(vpr_out)
            if m.group(1) not in NOT_HARD}
    return {
        "luts": luts,
        "luts_note": LUTS_NOTE,
        "clbs": packed["num_clb"],
        "hard_blocks": hard,
        "min_chan_width": routed["min_chan_width"],
        "crit_path_ns": routed["critical_path_delay_ns"],
        "wirelength": routed["crit_path_routed_wirelength"],
    }


def parse_vpr(ctx: qor_legs.LegContext, blif: Path, started: float) -> dict[str, object]:
    """Run VTR's two parse scripts over the VPR run directory and summarise them."""
    run_dir = ctx.job_dir / "run"
    for cmd in qor_parse.parse_commands(ctx.vtr_root, ctx.python, run_dir, ctx.arch_xml,
                                        blif.name):
        env = {**os.environ, **dict(cmd.env)}
        err = cmd.stderr or Path(os.devnull)
        with cmd.stdout.open("w") as out, err.open("w") as eh:
            proc = subprocess.run(list(cmd.argv), cwd=cmd.cwd, stdout=out, stderr=eh, env=env,
                                  check=False)
        if proc.returncode != 0:
            raise HeavyError(1, f"parse failed: {cmd.argv[1]} exit {proc.returncode}")
    results = {name: qor_parse.read_parse_results(run_dir / name, not_before=started,
                                                  label="vpr")
               for name in (qor_parse.PARSE_RESULTS, qor_parse.QOR_RESULTS)}
    vpr_out = (run_dir / "vpr.out").read_text(errors="replace")
    return summary_from_vpr(results[qor_parse.PARSE_RESULTS], results[qor_parse.QOR_RESULTS],
                            vpr_out, count_luts(blif))


_MIN_W = re.compile(r"^\s*Best routing used a channel width factor of (\d+)\.", re.M)
_UNROUTABLE = re.compile(r"Circuit is unroutable with a channel width factor of (\d+)\.")
_FINAL_CPD = re.compile(r"^Final critical path delay \(least slack\): ([-\d.e+]+) ns", re.M)
_TOTAL_WL = re.compile(r"^\s*Total wirelength: (\d+)", re.M)
_RELAX_FACTOR = 1.3  # run_vtr_flow.py's -relax_w_factor default


def relax_w(minimum: int) -> int:
    """VTR's relaxed width: ``2 * round(minimum * 1.3 / 2)`` (vtr/util.py ``relax_w``; Python's
    round, half to even)."""
    return 2 * round(minimum * _RELAX_FACTOR / 2)


def even_up(width: float) -> int:
    """``width`` rounded up to the next even integer."""
    whole = math.ceil(width)
    return whole + whole % 2


def route_failure(run_dir: Path) -> tuple[int, int] | None:
    """(minimum, relaxed) channel width of a VTR run whose final route failed, else None.

    The minimum is the last "Best routing used a channel width factor of N" of ``vpr.out`` (the
    binary search finished); the relaxed width is the one the final route reports as unroutable
    in ``vpr.crit_path.out`` (``relax_w`` of the minimum when that line is missing). None when
    the search did not finish or the final route did not fail on the route."""
    def read(name: str) -> str:
        path = run_dir / name
        return path.read_text(errors="replace") if path.is_file() else ""
    found = _MIN_W.findall(read("vpr.out"))
    if not found:
        return None
    crit = _UNROUTABLE.findall(read("vpr.crit_path.out"))
    if not crit:
        return None  # the step failed on something other than the final route
    minimum = int(found[-1])
    return minimum, int(crit[-1]) if crit else relax_w(minimum)


def retry_widths(minimum: int, relaxed: int) -> list[int]:
    """The widths to try after the relaxed one failed: relaxed + 2, relaxed + 4, 2 x minimum."""
    widths: list[int] = []
    for width in (relaxed + 2, relaxed + 4, even_up(2 * minimum)):
        if width not in widths and width != relaxed:
            widths.append(width)
    return widths


def vpr_run(args: FlowArgs, vtr_root: Path, arch: Path, k: int, blif: Path, job_dir: Path,
            width: int | None, timeout: int | None = None
            ) -> tuple[qor_legs.LegContext, StepResult, float]:
    """One ``run_vtr_flow.py -start vpr`` in ``job_dir``; ``width`` fixes ``--route_chan_width``
    (VTR then skips the minimum-width search). ``timeout`` defaults to the stage timeout + 60 s.
    ``--timing-paths N`` adds ``--timing_report_npaths N`` (a report option) when N is not VPR's
    default."""
    ctx = qor_legs.LegContext(vtr_root, vtr_root / ".venv" / "bin" / "python", arch, args.odin3,
                              resource_root(), job_dir, k, "A")
    job_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(blif, job_dir / blif.name)  # build_commands reads <job_dir>/<design>.blif
    cmd = qor_legs.build_commands("O", blif.stem, ctx)[1]
    argv = [*cmd.argv, "--route_chan_width", str(width)] if width is not None else list(cmd.argv)
    if args.timing_attr and args.timing_paths != VPR_DEFAULT_PATHS:
        argv += ["--timing_report_npaths", str(args.timing_paths)]
    started = time.time()
    step = run_step("vpr", argv, job_dir, {**os.environ, **dict(cmd.env)},
                    timeout if timeout is not None else qor_legs.stage_timeout("A") + 60)
    return ctx, step, started


def vpr_step(args: FlowArgs, blif: Path, k: int, result: FlowResult) -> None:
    """Pack, place and route ``blif`` at the minimum channel width with vtr-qor's options.

    VTR routes once at 1.3 x the minimum width, and on a tiny device that width can be
    unroutable while its neighbours route. When the search finished and only that final route
    failed, the route is retried at fixed wider widths (``route_retry`` in ``summary.json``)."""
    vtr_root = vtr_root_from_env()
    if vtr_root is None:
        raise UsageError("--vpr needs VTR_ROOT")
    arch = args.arch or qor_legs.arch_xml_path(vtr_root, DEFAULT_ARCH)
    job_dir = args.out / "vpr"
    ctx, step, started = vpr_run(args, vtr_root, arch, k, blif, job_dir, None)
    result.steps.append(step)
    retry: dict[str, int] | None = None
    if not step.ok:
        failure = route_failure(job_dir / "run")
        if failure is None:
            return
        minimum, relaxed = failure
        tried = [relaxed]
        widths = retry_widths(minimum, relaxed)
        deadline = time.monotonic() + qor_legs.stage_timeout("A")  # shared by all retries
        skipped: list[int] = []
        for index, width in enumerate(widths):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                skipped = widths[index:]
                break
            tried.append(width)
            rctx, rstep, rstarted = vpr_run(args, vtr_root, arch, k, blif,
                                            job_dir / f"retry-{width}", width, math.ceil(remaining))
            if rstep.ok:
                print(f"NOTE: route at W={relaxed} failed (VPR); routed at W={width}")
                ctx, step, started = rctx, rstep, rstarted
                result.steps[-1] = step
                retry = {"min_chan_width": minimum, "failed_width": relaxed, "used_width": width}
                break
        if retry is None:
            note = (f"\nroute failed at every width tried: "
                    f"{', '.join(str(w) for w in tried)} (min {minimum}); retry "
                    f"logs in {job_dir}/retry-<W>/\n")
            if skipped:
                note += (f"retry time budget spent; widths not tried: "
                         f"{', '.join(str(w) for w in skipped)}\n")
            step.log.write_text(step.log.read_text() + note)
            return
    result.vpr_run_dir = ctx.job_dir / "run"
    try:
        result.summary.update(parse_vpr(ctx, blif, started))
    except (HeavyError, OSError) as exc:
        step.log.write_text(f"{step.log.read_text()}\nsummary: {exc}\n")
        result.steps[-1] = StepResult("vpr", False, step.log, step.seconds)
        return
    if retry is not None:
        # A fixed-width run reports its route under the min-width columns of VTR's parse, so the
        # critical-path figures come from the VPR log.
        text = (ctx.job_dir / "run" / "vpr.out").read_text(errors="replace")
        cpd, wirelength = _FINAL_CPD.findall(text), _TOTAL_WL.findall(text)
        if result.summary.get("crit_path_ns") is None and cpd:
            result.summary["crit_path_ns"] = float(cpd[-1])
        if result.summary.get("wirelength") is None and wirelength:
            result.summary["wirelength"] = int(wirelength[-1])
        result.summary["min_chan_width"] = retry["min_chan_width"]
        result.summary["route_retry"] = retry


def analysis_script(top: str, adir: Path) -> str:
    """The analysis passes, appended to the script (the netlist view is in memory there):
    ``levelize`` (its info line goes to the log), ``write_dot --color-by --locs``,
    ``write_json --odin3`` and ``dump_locs --types``."""
    return (f"; levelize; write_dot --color-by @type --locs --max-nodes {DOT_MAX_NODES} "
            f"{quote(adir / (top + '.dot'))}; "
            f"write_json --odin3 {quote(adir / (top + '.odin3.json'))}; "
            f"dump_locs --types {quote(adir / 'locs.txt')}")


def analysis_step(top: str, adir: Path, odin3_log: Path, result: FlowResult) -> None:
    """Make ``levelize.txt`` from the levelize info line of the odin3 log and check that the four
    analysis files exist and are not empty."""
    start = time.monotonic()
    text = odin3_log.read_text(errors="replace")
    lines = [ln for ln in text.splitlines() if "info: levelize:" in ln]
    (adir / "levelize.txt").write_text("\n".join(lines) + ("\n" if lines else ""))
    files = [adir / name for name in (f"{top}.dot", f"{top}.odin3.json", "levelize.txt",
                                      "locs.txt")]
    log = adir / "analysis.log"
    bad = [f.name for f in files if not f.is_file() or f.stat().st_size == 0]
    log.write_text("".join(f"missing or empty: {name}\n" for name in bad) or "ok\n")
    result.steps.append(StepResult("analysis", not bad, log, time.monotonic() - start))


def timing_script(k: int, tdir: Path) -> str:
    """The writes that give the timing attribution its provenance, appended to the script: the
    same netlist as BLIF with provenance names (same blocks, same order as the BLIF VPR reads) and
    ``write_json --odin3``. Both only read the netlist: the BLIF VPR reads is unchanged."""
    return (f"; write_blif --vtr --lut-k {k} --name-style provenance "
            f"{quote(tdir / timing_attr.PROV_BLIF_NAME)}; "
            f"write_json --odin3 {quote(tdir / timing_attr.NETLIST_JSON_NAME)}")


def timing_step(args: FlowArgs, top: str, blif: Path, run_dir: Path, result: FlowResult) -> None:
    """Attribute the critical path of the VPR run in ``run_dir``: ``timing/inputs.json``,
    ``attribution.json`` and ``summary.txt``. A failure is a failed step."""
    start = time.monotonic()
    tdir = args.out / timing_attr.TIMING_DIR
    log = tdir / "timing.log"
    inputs = {"top": top, "blif": os.path.relpath(blif, args.out),
              "prov_blif": f"{timing_attr.TIMING_DIR}/{timing_attr.PROV_BLIF_NAME}",
              "netlist_json": f"{timing_attr.TIMING_DIR}/{timing_attr.NETLIST_JSON_NAME}",
              "vpr_run_dir": os.path.relpath(run_dir, args.out),
              "paths_requested": args.timing_paths,
              # where the provenance file names point: odin3 ran in odin3/ with these designs
              "source_cwd": os.path.relpath(args.out / "odin3", args.out),
              "designs": [str(d) for d in args.designs]}
    (tdir / timing_attr.INPUTS_NAME).write_text(json.dumps(inputs, indent=1) + "\n")
    try:
        report = timing_attr.attribute_flow_dir(args.out, args.timing_paths)
        timing_attr.write_outputs(report, tdir)
        log.write_text("ok\n")
        cov, crit = report["coverage"], report["critical_path"]
        result.summary["timing_attr"] = {
            "paths": report["timing"]["paths_parsed"], "matched_share": cov["matched_share"],
            "critical_spans": [s["span"] for s in crit["spans"] if s["span"]],
        }
        ok = True
    except (timing_attr.AttributionError, OSError, ValueError) as exc:
        log.write_text(f"{exc}\n")
        ok = False
    result.steps.append(StepResult("timing", ok, log, time.monotonic() - start))


def vtr_steps(args: FlowArgs, top: str, result: FlowResult) -> Path | None:
    """Target ``vtr``: the odin3 step, then VPR when asked. The BLIF, or None on a failure."""
    step_dir = args.out / "odin3"
    vtr_root = vtr_root_from_env()
    k = lut_k(args, vtr_root, step_dir / "k.log")
    blif = step_dir / f"{top}.blif"
    design = " ".join(quote(d) for d in args.designs)
    script = qor_legs.odin3_script("O", design, quote(blif), k, "short")
    adir = args.out / "analysis"
    if args.analysis:
        adir.mkdir(parents=True, exist_ok=True)
        script += analysis_script(top, adir)
    if args.timing_attr:
        (args.out / timing_attr.TIMING_DIR).mkdir(parents=True, exist_ok=True)
        script += timing_script(k, args.out / timing_attr.TIMING_DIR)
    step = run_step("odin3", odin3_command(args, top, script), step_dir)
    result.steps.append(step)
    if not step.ok:
        return None
    result.summary["luts"] = count_luts(blif)
    result.summary["luts_note"] = LUTS_NOTE
    if args.analysis:
        analysis_step(top, adir, step.log, result)
        if not result.steps[-1].ok:
            return None
    if args.vpr:
        vpr_step(args, blif, k, result)
        if args.timing_attr and result.steps[-1].ok and result.vpr_run_dir is not None:
            timing_step(args, top, blif, result.vpr_run_dir, result)
    return blif if result.steps[-1].ok else None


def lpm_steps(args: FlowArgs, top: str, result: FlowResult) -> Path | None:
    """The odin3 step and the read-back lint of target ``lpm``."""
    step_dir = args.out / "odin3"
    written = step_dir / f"{top}.v"
    script = lpm_script(args.designs, written, args.family)
    step = run_step("odin3", odin3_command(args, top, script), step_dir)
    result.steps.append(step)
    if not step.ok:
        return None
    back = f"read_techlib {quote(resource_root() / LPM_LIB_REL)}; " \
           f"read_verilog -icells {quote(written)}"
    lint = run_step("readback", [str(args.odin3), "--check", "-p", back], args.out / "readback")
    result.steps.append(lint)
    return written if lint.ok else None


def run_flow(args: FlowArgs) -> FlowResult:
    """Run every step; stop at the first that fails. UsageError for a bad input."""
    if args.target not in TARGETS:
        raise UsageError(f"unknown target '{args.target}' ({', '.join(TARGETS)})")
    for design in args.designs:
        if not design.is_file():
            raise UsageError(f"no such design file: {design}")
    if args.analysis and args.target != "vtr":
        raise UsageError("--analysis applies to --target vtr only (levelize needs bits)")
    if args.timing_attr and (args.target != "vtr" or not args.vpr):
        raise UsageError("--timing-attr needs --target vtr and VPR (--vpr, VTR_ROOT)")
    if args.timing_paths < 1:
        raise UsageError("--timing-paths needs a positive number")
    if not args.odin3.is_file():
        raise UsageError(f"no odin3 binary at {args.odin3}: build it, put it on PATH or give "
                         f"--odin3")
    top = resolve_top(args.designs, args.top)
    qor_legs.REPO_ROOT = resource_root()  # the module seam: where $LIB (lib/vtr.o3lib) is
    result = FlowResult(ok=False)
    args.out.mkdir(parents=True, exist_ok=True)
    step_fn = vtr_steps if args.target == "vtr" else lpm_steps
    result.ok = step_fn(args, top, result) is not None
    result.summary["seconds"] = {st.name: round(st.seconds, 3) for st in result.steps}
    (args.out / "summary.json").write_text(json.dumps(result.summary, indent=2) + "\n")
    return result


def print_summary(summary: Mapping[str, object]) -> None:
    """The summary as a table, one key per line."""
    for key, value in summary.items():
        print(f"  {key:<16} {value}")


def resolve_vpr(flag: bool | None, target: str) -> bool:
    """``--vpr``/``--no-vpr``; the default is on when VTR_ROOT is set (NOTE when it is not)."""
    if target != "vtr":
        if flag:
            raise UsageError("--vpr applies to --target vtr only")
        return False
    if flag is None:
        if vtr_root_from_env() is None:
            print("NOTE: VTR_ROOT is not set: VPR skipped (use --no-vpr to silence this)")
            return False
        return True
    if flag and vtr_root_from_env() is None:
        raise UsageError("--vpr needs VTR_ROOT")
    return flag


def default_odin3() -> Path:
    """The CLI: the one bundled in the wheel (odin3/bin/odin3 beside odin3/data), else odin3 on
    PATH, else build/release/odin3."""
    bundled = HERE.parents[2] / "bin" / "odin3"
    if HERE.parents[1].name == "data" and bundled.is_file():
        return bundled
    found = shutil.which("odin3")
    return Path(found) if found else HERE.parents[1] / "build" / "release" / "odin3"


def parse_args(argv: Sequence[str]) -> FlowArgs:
    ap = argparse.ArgumentParser(
        prog="odin3_flow", description=__doc__.split("\n")[0],
        epilog="Targets: vtr (BLIF for VPR, the default) and lpm (Verilog with LPM cells). "
               "Exit codes: 0 ok, 1 a step failed, 2 usage error.")
    ap.add_argument("designs", nargs="+", type=Path, help="Verilog design file(s)")
    ap.add_argument("--top", help="top module (default: the one uninstantiated module)")
    ap.add_argument("--target", default="vtr", choices=TARGETS,
                    help="vtr: BLIF (and VPR); lpm: LPM-cell Verilog plus a read-back lint "
                         "(default: vtr)")
    ap.add_argument("--arch", type=Path,
                    help=f"VTR architecture XML (default: {DEFAULT_ARCH}); needs VTR_ROOT, "
                         f"without it K = {DEFAULT_K} is assumed with a WARNING")
    ap.add_argument("--family", default="",
                    help="LPM device family for --target lpm (default: none, empty)")
    ap.add_argument("--vpr", action=argparse.BooleanOptionalAction, default=None,
                    help="run VPR after odin3 (default: on when VTR_ROOT is set); "
                         "--target vtr only")
    ap.add_argument("--analysis", action="store_true",
                    help="also write the dot, JSON, levelize and locs files; needs --target vtr")
    ap.add_argument("--timing-attr", action="store_true",
                    help="attribute the critical path to HDL statements, modules and words "
                         "(timing/attribution.json and summary.txt); needs VPR")
    ap.add_argument("--timing-paths", type=int, default=None, metavar="N",
                    help=f"setup paths to report and attribute (default {VPR_DEFAULT_PATHS}); "
                         f"implies --timing-attr")
    ap.add_argument("--odin3", type=Path, default=None,
                    help="odin3 binary (default: odin3 on PATH, else build/release/odin3)")
    ap.add_argument("--out", type=Path, help="output directory (default: odin3_out/<top>)")
    ns = ap.parse_args(argv)
    out = ns.out
    if out is None:
        for design in ns.designs:
            if not design.is_file():
                raise UsageError(f"no such design file: {design}")
        out = Path("odin3_out") / resolve_top(ns.designs, ns.top)
    vpr = resolve_vpr(ns.vpr, ns.target)
    arch = ns.arch.resolve() if ns.arch else None
    attr = ns.timing_attr or ns.timing_paths is not None
    paths = ns.timing_paths if ns.timing_paths is not None else VPR_DEFAULT_PATHS
    return FlowArgs([d.resolve() for d in ns.designs], ns.top, ns.target, arch, ns.family, vpr,
                    ns.analysis, (ns.odin3 or default_odin3()).resolve(), out.resolve(),
                    attr, paths)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
        result = run_flow(args)
    except UsageError as exc:
        print(f"odin3_flow: {exc}", file=sys.stderr)
        return 2
    for step in result.steps:
        state = "ok" if step.ok else "FAILED"
        print(f"step {step.name}: {state} ({step.seconds:.1f} s) log {step.log}")
    print_summary(result.summary)
    for step in result.steps:
        if not step.ok:
            print(f"odin3_flow: step {step.name} failed; see {step.log}", file=sys.stderr)
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
