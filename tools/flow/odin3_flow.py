"""odin3_flow: take a design through Odin III, and optionally VPR, in one command (7.3, spec 7
section 2.1).

    odin3_flow.py design.v [more.v ...] --top TOP [--target vtr|lpm] [--arch ARCH.xml]
        [--family NAME] [--vpr | --no-vpr] [--analysis] [--prov] [--prov-cut] [--prov-level L]
        [--prov-budget SIZE] [--timing-attr] [--timing-paths N] [--timing-legacy]
        [--hints FILE | --two-pass [--hint-seeds N] [--hint-cut exact|all] [--hint-max-cells N]]
        [--odin3 PATH] [--out DIR]
        [--target silicon --tools DIR --pdk DIR --steps a,b --clock PORT]

Target ``vtr``: the Odin III script of the vtr-qor O leg (imported from tools/vtr-qor, never
copied) writes ``odin3/<top>.blif``. Target ``lpm``: the 5.1 LPM script (tools/flow/lpm.script)
writes ``odin3/<top>.v`` and a read-back lint (``read_verilog -icells``) follows. Every step runs in
its own subdirectory of ``--out`` with its log. Exit codes: 0 ok, 1 a step failed, 2 usage error.
The module is importable with tools/vtr-qor on PYTHONPATH (the wrapper sets it):
``run_flow(FlowArgs) -> FlowResult``.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

import prov_steps
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
TARGETS = ("vtr", "lpm", "silicon")
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
    prov: bool = False  # --prov: the BLIF has hash names and a version 2 name map (v1.2.0)
    timing_legacy: bool = False  # --timing-legacy: the v1.1.1 route, a second provenance BLIF
    keep_results: bool = False  # --keep-results: keep VPR's .net, .place and .route (prov_post)
    tools: Path | None = None  # --tools: user-space tools directory of --target silicon
    pdk: Path | None = None  # --pdk: the sky130A directory
    steps: str = ""  # --steps: comma list of silicon steps (default all)
    clock: str = ""  # --clock: clock port of the silicon design (enables CTS)
    incdirs: tuple[str, ...] = ()  # -I / +incdir+: include directories (absolute)
    defines: tuple[str, ...] = ()  # -D / +define+: NAME or NAME=VALUE
    read_opts: tuple[str, ...] = ()  # --read-opt: extra read_verilog options
    strict_tristate: bool = False  # --strict-tristate: a tri-state conversion is an error
    prov_cut: bool = False  # --prov-cut: abc keeps the exact cut of each LUT (needs --prov)
    prov_level: str = ""  # --prov-level 0..4|auto: the provenance depth (odin3 --prov-level)
    prov_budget: str = ""  # --prov-budget SIZE: the most provenance bytes (odin3 --prov-budget)
    hints: Path | None = None  # --hints FILE: read_hints before abc (docs/user/timing-hints.md)
    net_timing: bool = False  # VPR also writes report_net_timing.csv (pass 1 of --two-pass)
    vpr_seed: int = 0  # VPR --seed (0: VPR's default, which is 1)
    two_pass: bool = False  # --two-pass: pass 1 to routing, hint file, pass 2 with it
    hint_seeds: int = 1  # --hint-seeds N: hints from the worst over VPR seeds 1..N
    hint_cut: str = "exact"  # --hint-cut exact|all
    hint_max_cells: int = 8  # --hint-max-cells N (0: no limit)


@dataclass(frozen=True)
class StepResult:
    name: str
    ok: bool
    log: Path
    seconds: float
    argv: tuple[str, ...] = ()  # the command of a tool step (empty for a check made in-process)
    exit_code: int | None = None
    rss_mib: float | None = None  # peak RSS of the tool and its children (wait4)


@dataclass
class FlowResult:
    ok: bool
    steps: list[StepResult] = field(default_factory=list)
    runs: list[StepResult] = field(default_factory=list)  # every tool run, retries included
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


def design_root(designs: Sequence[Path]) -> Path:
    """The common parent directory of the (absolute) ``designs``: the working directory of the
    odin3 step, so that the file names in hash names and provenance are relative to it and do not
    depend on where the checkout lives."""
    return Path(os.path.commonpath([str(d.parent) for d in designs]))


def relative_designs(designs: Sequence[Path]) -> list[Path]:
    """Each design as a path relative to :func:`design_root`."""
    root = design_root(designs)
    return [Path(os.path.relpath(d, root)) for d in designs]


def root_warning(designs: Sequence[Path]) -> str:
    """A WARNING line when the design root is the filesystem root (designs under unrelated
    directories: names then carry the path minus its leading slash), else an empty string."""
    if design_root(designs) != Path(designs[0].anchor):
        return ""
    return ("WARNING: the designs share no parent directory below the filesystem root: names in "
            "hash names, the name map and provenance carry the path minus its leading slash, so "
            "they depend on where the files live; put the designs under one directory for "
            "location-independent names")


def name_root_note(designs: Sequence[Path]) -> str:
    """The run-log line that records the name root: relative names only, never an absolute path."""
    return "name_root=. designs=" + " ".join(os.fspath(d) for d in relative_designs(designs))


def run_step(name: str, argv: Sequence[str], step_dir: Path, env: dict[str, str] | None = None,
             timeout: int = STEP_TIMEOUT_S, cwd: Path | None = None,
             note: str = "") -> StepResult:
    """Run ``argv`` in ``cwd`` (default ``step_dir``); stdout and stderr go to the log in
    ``step_dir``, after ``note`` when given. A timeout is a failure."""
    step_dir.mkdir(parents=True, exist_ok=True)
    log = step_dir / f"{name}.log"
    start = time.monotonic()
    code: int | None = None
    rss: float | None = None
    try:
        with log.open("w") as handle:
            handle.write("$ " + " ".join(argv) + "\n")
            if note:
                handle.write(note + "\n")
            handle.flush()
            proc = subprocess.Popen(list(argv), cwd=cwd or step_dir, stdout=handle,
                                    stderr=subprocess.STDOUT, env=env)
            expired: list[bool] = []

            def expire() -> None:
                expired.append(True)
                proc.kill()

            timer = threading.Timer(timeout, expire)
            timer.start()
            try:
                _, status, usage = os.wait4(proc.pid, 0)
            finally:
                timer.cancel()
            code = os.waitstatus_to_exitcode(status)
            proc.returncode = code
            rss = usage.ru_maxrss / 1024.0  # KiB on Linux
            if expired:
                handle.write(f"\ntimeout after {timeout} s\n")
        ok = code == 0 and not expired
    except OSError as exc:
        log.write_text(f"{log.read_text()}\n{exc}\n")
        ok = False
    return StepResult(name, ok, log, time.monotonic() - start, tuple(argv), code, rss)


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


PROJECT_REFUSED = ("libfile", "libdir", "libext", "param")  # keys that change the read


def expand_project(path: Path) -> tuple[list[Path], list[str], list[str], str | None]:
    """The design files, include directories, defines and top module of an ``.o3proj`` (paths
    relative to the project file). ``file <language> <path> [library <name>]``, ``incdir``,
    ``define`` and ``top [<library>.]<module>`` are read; the library of a file is not used. A key
    that would change the read and that the flow cannot carry (``libfile``, ``libdir``, ``libext``,
    ``param``) is a UsageError; any other key (``arch``, ``flow``, ``tool`` ...) is not used."""
    path = path.resolve()
    if not path.is_file():
        raise UsageError(f"no such project file: {path}")
    files: list[Path] = []
    incdirs: list[str] = []
    defines: list[str] = []
    top: str | None = None
    for num, line in enumerate(path.read_text().splitlines(), 1):
        try:
            words = shlex.split(line, comments=True)
        except ValueError as exc:
            raise UsageError(f"{path}:{num}: {exc}") from exc
        if not words:
            continue
        key, args = words[0], words[1:]
        if key in PROJECT_REFUSED:
            raise UsageError(f"{path}:{num}: '{key}' is not carried by odin3-flow; "
                             f"read the project with odin3 itself")
        if key == "file" and len(args) >= 2:
            if args[0] not in ("verilog", "systemverilog"):
                raise UsageError(f"{path}:{num}: file language '{args[0]}' is not Verilog")
            files.append((path.parent / args[1]).resolve())
        elif key == "incdir" and len(args) == 1:
            incdirs.append(str((path.parent / args[0]).resolve()))
        elif key == "define" and len(args) == 1:
            defines.append(args[0])
        elif key == "top" and len(args) == 1:
            top = args[0].split(".", 1)[-1]
        elif key in ("file", "incdir", "define", "top"):
            raise UsageError(f"{path}:{num}: bad '{key}' line")
    return files, incdirs, defines, top


def expand_filelist(path: Path, seen: tuple[Path, ...] = ()) \
        -> tuple[list[Path], list[str], list[str]]:
    """The designs, include directories and defines of a Verilog file list: one item per line or
    blank-separated; ``//`` and ``#`` start a comment; ``+incdir+a+b``, ``-I dir``,
    ``+define+X=1``, ``-D X=1``, ``-f other`` (paths relative to the working directory),
    ``-F other`` (paths relative to that list); an environment variable ``$VAR`` or ``${VAR}``
    is expanded. Any other option is a UsageError (it would be dropped silently)."""
    path = path.resolve()
    if path in seen:
        raise UsageError(f"{path}: file list includes itself")
    if not path.is_file():
        raise UsageError(f"no such file list: {path}")
    files: list[Path] = []
    incdirs: list[str] = []
    defines: list[str] = []
    tokens: list[str] = []
    for line in path.read_text().splitlines():
        line = re.sub(r"//.*|#.*", "", line)
        tokens += os.path.expandvars(line).split()
    base = Path.cwd()
    it = iter(tokens)
    for tok in it:
        if tok.startswith("+incdir+"):
            incdirs += [str((base / d).resolve()) for d in tok[8:].split("+") if d]
        elif tok.startswith("+define+"):
            defines += [d for d in tok[8:].split("+") if d]
        elif tok in ("-I", "-D", "-f", "-F"):
            arg = next(it, None)
            if arg is None:
                raise UsageError(f"{path}: {tok} needs an argument")
            if tok == "-I":
                incdirs.append(str((base / arg).resolve()))
            elif tok == "-D":
                defines.append(arg)
            else:
                sub = (path.parent if tok == "-F" else base) / arg
                f, i, d = expand_filelist(sub, seen + (path,))
                files, incdirs, defines = files + f, incdirs + i, defines + d
        elif tok.startswith("-I") and len(tok) > 2:
            incdirs.append(str((base / tok[2:]).resolve()))
        elif tok.startswith("-D") and len(tok) > 2:
            defines.append(tok[2:])
        elif tok.startswith(("-", "+")):
            raise UsageError(f"{path}: unsupported file list option '{tok}' "
                             f"(supported: +incdir+ +define+ -I -D -f -F)")
        else:
            files.append((base / tok).resolve())
    return files, incdirs, defines


def project_text(args: FlowArgs) -> str:
    """The .o3proj that carries the include directories and defines to ``read_verilog``."""
    lines = [f"incdir {quote(d)}" for d in args.incdirs]
    lines += [f"define {quote(d)}" for d in args.defines]
    return "\n".join(lines) + "\n"


def with_read_options(args: FlowArgs, script: str, step_dir: Path) -> str:
    """``script`` with ``read_project <flow.o3proj>;`` first when there are include directories or
    defines, and the ``--read-opt`` options after its first ``read_verilog``."""
    if args.read_opts:
        if "read_verilog " not in script:
            raise UsageError("internal: the odin3 script has no read_verilog")
        opts = " ".join(args.read_opts)
        script = script.replace("read_verilog ", f"read_verilog {opts} ", 1)
    if args.incdirs or args.defines:
        step_dir.mkdir(parents=True, exist_ok=True)
        proj = step_dir / "flow.o3proj"
        proj.write_text(project_text(args))
        script = f"read_project {quote(proj)}; {script}"
    return script


def tristate_script(script: str, strict: bool) -> str:
    """``script`` with the ``tristate`` pass after its ``flatten``: internal tri-states become logic
    and a top inout port is split, one WARNING each (``strict``: any conversion is an error,
    the behaviour before the pass). A script without ``flatten`` is returned as it is."""
    name = "tristate --strict" if strict else "tristate"
    return script.replace("flatten; ", f"flatten; {name}; ", 1)


def odin3_command(args: FlowArgs, top: str, script: str) -> list[str]:
    depth = (["--prov-level", args.prov_level] if args.prov_level else []) + \
        (["--prov-budget", args.prov_budget] if args.prov_budget else [])
    return [str(args.odin3), "--diag-paths", "resolved", *depth, "--top", top, "-p", script]


DEPTH_RE = re.compile(r"proc: provenance depth (\d) ")


def resolved_depth(odin3_log: Path) -> int | None:
    """The provenance depth odin3 used (its ``proc`` INFO line), None when it printed none."""
    match = DEPTH_RE.search(odin3_log.read_text(errors="replace")) if odin3_log.is_file() else None
    return int(match.group(1)) if match else None


SIZE_RE = re.compile(r"([0-9]+)([KMGkmg]?)")
SIZE_SHIFT = {"": 0, "K": 10, "M": 20, "G": 30}
BYTES_RE = re.compile(r"proc: provenance depth \d \(\w+\): estimated \d+ records, \d+ edges, "
                      r"\d+ bytes \(name map (\d+), step files (\d+)\)")


def parse_size(text: str) -> int | None:
    """``--prov-budget`` text as bytes (digits, then an optional K, M or G of either case, powers
    of 1024), None when it is anything else or zero (as ``odin3 --prov-budget`` reads it)."""
    match = SIZE_RE.fullmatch(text)
    if match is None:
        return None
    size = int(match.group(1)) << SIZE_SHIFT[match.group(2).upper()]
    return size if 0 < size < 1 << 64 else None


def steps_over_budget(odin3_log: Path, budget: str) -> tuple[int, int, int] | None:
    """(name map bytes, step file bytes, budget bytes) when the depth 4 estimate of the odin3 log
    puts the step files over the budget (odin3 itself counts the name map only), else None."""
    limit = parse_size(budget) if budget else None
    match = BYTES_RE.search(odin3_log.read_text(errors="replace")) if odin3_log.is_file() else None
    if limit is None or match is None:
        return None
    map_bytes, step_bytes = int(match.group(1)), int(match.group(2))
    return (map_bytes, step_bytes, limit) if map_bytes + step_bytes > limit else None


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
    if args.keep_results:  # odin3-prov-post reads the packed netlist and the placement
        argv = [a for a in argv if a != "-delete_result_files"]
    if args.timing_attr and args.timing_paths != VPR_DEFAULT_PATHS:
        argv += ["--timing_report_npaths", str(args.timing_paths)]
    if args.net_timing:
        argv += ["--generate_net_timing_report", "on"]
    if args.vpr_seed:
        argv += ["--seed", str(args.vpr_seed)]
    started = time.time()
    step = run_step("vpr", argv, job_dir, {**os.environ, **dict(cmd.env)},
                    timeout if timeout is not None else qor_legs.stage_timeout("A") + 60)
    return ctx, step, started


def promote_retry_run(job_dir: Path, retry: dict[str, int], result: FlowResult) -> None:
    """With ``--keep-results`` after a route retry, ``vpr/run`` holds the failed first attempt.
    Its ``.net``, ``.place`` and ``.route`` are not one routed result (``vpr --analysis`` cannot
    use them) and ``odin3-prov-post`` finds two run directories. The failed attempt moves to
    ``vpr/run-failed-W<failed width>`` and the routed retry's run directory becomes ``vpr/run``."""
    failed = job_dir / f"run-failed-W{retry['failed_width']}"
    used = job_dir / f"retry-{retry['used_width']}" / "run"
    (job_dir / "run").rename(failed)
    used.rename(job_dir / "run")
    result.vpr_run_dir = job_dir / "run"
    print(f"NOTE: --keep-results: the routed run (W={retry['used_width']}) is vpr/run; the failed "
          f"one is {failed.name}")


def vpr_seed_run(args: FlowArgs, blif: Path, job_dir: Path) -> tuple[Path | None, float | None]:
    """One more VPR run of ``blif`` with ``args.vpr_seed`` (``--two-pass`` with ``--hint-seeds N``).
    It follows the procedure of seed 1 (``vpr_step``: minimum width, relaxed-width route, route
    retries) in ``job_dir / "vpr"``. Returns its run directory and routed CPD in ns, None where
    it failed. ``args.net_timing`` must be set for the net timing report to be written."""
    vtr_root = vtr_root_from_env()
    if vtr_root is None:
        raise UsageError("--vpr needs VTR_ROOT")
    job_dir.mkdir(parents=True, exist_ok=True)
    k = lut_k(args, vtr_root, job_dir / "k.log")
    scratch = FlowResult(ok=False)
    vpr_step(dataclasses.replace(args, out=job_dir), blif, k, scratch)
    cpd = scratch.summary.get("crit_path_ns")
    if scratch.vpr_run_dir is None or not isinstance(cpd, (int, float)):
        return None, None
    return scratch.vpr_run_dir, float(cpd)


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
    result.runs.append(step)
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
            result.runs.append(rstep)
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
        if args.keep_results:
            promote_retry_run(job_dir, retry, result)


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


def prov_script(script: str, k: int) -> str:
    """The odin3 script of the v1.2.0 route: the same script, with the elaboration keeping the
    constructs (``read_verilog --prov-ast refs``) and the BLIF VPR reads written with hash names
    and the version 2 name map next to it (``write_blif --name-style hash --prov-map``). The
    netlist is the same; only its names change (docs/V1.2.0.md 6.1). UsageError when the script
    does not have the two places."""
    read, write = "read_verilog --nolatches --vtr-compat ", f"write_blif --vtr --lut-k {k}"
    if script.count(read) != 1 or script.count(write) != 1:
        raise UsageError("internal: the odin3 script has no read_verilog / write_blif to extend")
    return script.replace(read, "read_verilog --nolatches --vtr-compat --prov-ast refs ").replace(
        write, f"{write} --name-style hash --prov-map")


def timing_script(k: int, tdir: Path, legacy: bool = False) -> str:
    """The writes that give the timing attribution its provenance, appended to the script. The
    default (v1.2.0): ``write_json --odin3`` only, because the BLIF VPR reads carries hash names
    and its name map (``prov_script``), so no second BLIF is written. ``legacy`` (v1.1.1): also
    the same netlist as BLIF with provenance names (same blocks, same order as the BLIF VPR
    reads). The writes only read the netlist: the BLIF VPR reads is unchanged."""
    json_write = f"write_json --odin3 {quote(tdir / timing_attr.NETLIST_JSON_NAME)}"
    if not legacy:
        return f"; {json_write}"
    return (f"; write_blif --vtr --lut-k {k} --name-style provenance "
            f"{quote(tdir / timing_attr.PROV_BLIF_NAME)}; {json_write}")


def hints_script(script: str, hints: Path, k: int) -> str:
    """``script`` with ``read_hints <file>`` just before its abc pass."""
    abc = f"abc --lut {k}"
    if script.count(abc) != 1:
        raise UsageError("internal: the odin3 script has no single abc pass to put hints before")
    return script.replace(abc, f"read_hints {quote(hints)}; {abc}")


def map_path(blif: Path) -> Path:
    """The version 2 name map the writer puts next to the BLIF (``<blif>.names``)."""
    return blif.with_name(blif.name + ".names")


def prov_step(blif: Path, result: FlowResult) -> None:
    """Check the version 2 map next to the BLIF: it exists and has the version 2 header."""
    start = time.monotonic()
    path = map_path(blif)
    log = path.with_name(path.name + ".log")
    head = path.read_text(errors="replace").split("\n", 1)[0] if path.is_file() else ""
    ok = head.startswith("# odin3-names 2 ")
    log.write_text("ok\n" if ok else f"missing or not a version 2 name map: {path}\n")
    result.steps.append(StepResult("prov", ok, log, time.monotonic() - start))


def timing_step(args: FlowArgs, top: str, blif: Path, run_dir: Path, result: FlowResult) -> None:
    """Attribute the critical path of the VPR run in ``run_dir``: ``timing/inputs.json``,
    ``attribution.json`` and ``summary.txt``. A failure is a failed step."""
    start = time.monotonic()
    tdir = args.out / timing_attr.TIMING_DIR
    log = tdir / "timing.log"
    inputs = {"top": top, "blif": os.path.relpath(blif, args.out),
              **({"prov_blif": f"{timing_attr.TIMING_DIR}/{timing_attr.PROV_BLIF_NAME}"}
                 if args.timing_legacy
                 else {timing_attr.NAME_MAP_KEY: os.path.relpath(map_path(blif), args.out)}),
              "netlist_json": f"{timing_attr.TIMING_DIR}/{timing_attr.NETLIST_JSON_NAME}",
              "vpr_run_dir": os.path.relpath(run_dir, args.out),
              "paths_requested": args.timing_paths,
              # where the provenance file names point: odin3 ran in odin3/ with these designs
              "source_cwd": os.path.relpath(design_root(args.designs), args.out),
              # fallback when the flow dir moved without the design tree
              "source_cwd_abs": str(design_root(args.designs)),
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
    design = " ".join(quote(d) for d in relative_designs(args.designs))
    script = qor_legs.odin3_script("O", design, quote(blif), k, "short")
    if args.prov:
        script = prov_script(script, k)
    # a depth that resolves above 0 makes write_blif use hash names and write the map by itself
    # (--name-style and --prov-map come from it), so the BLIF never has hash names without a map
    if args.prov_cut:
        script = script.replace(f"abc --lut {k}", f"abc --lut {k} --prov-cut on")
    script = with_read_options(args, script, step_dir)
    script = tristate_script(script, args.strict_tristate)
    if args.hints is not None:
        script = hints_script(script, args.hints, k)
    adir = args.out / "analysis"
    if args.analysis:
        adir.mkdir(parents=True, exist_ok=True)
        script += analysis_script(top, adir)
    if args.timing_attr:
        (args.out / timing_attr.TIMING_DIR).mkdir(parents=True, exist_ok=True)
        script += timing_script(k, args.out / timing_attr.TIMING_DIR, args.timing_legacy)
    step = run_step("odin3", odin3_command(args, top, script), step_dir,
                    cwd=design_root(args.designs), note=name_root_note(args.designs))
    result.steps.append(step)
    result.runs.append(step)
    if not step.ok:
        return None
    result.summary["luts"] = count_luts(blif)
    result.summary["luts_note"] = LUTS_NOTE
    depth = resolved_depth(step.log)
    names = args.prov or (map_path(blif).is_file() and depth is not None and depth > 0)
    if names:
        prov_step(blif, result)
        if not result.steps[-1].ok:
            return None
    if args.analysis:
        analysis_step(top, adir, step.log, result)
        if not result.steps[-1].ok:
            return None
    if args.vpr:
        vpr_step(args, blif, k, result)
        if args.timing_attr and result.steps[-1].ok and result.vpr_run_dir is not None:
            timing_step(args, top, blif, result.vpr_run_dir, result)
    steps = args.prov or (names and depth == 4)  # the step files are depth 4 (or --prov)
    over = None if args.prov or not steps else steps_over_budget(step.log, args.prov_budget)
    if over is not None:
        print(f"WARNING: the prov/ step files are skipped: estimated at {over[1]} bytes, which "
              f"with the name map ({over[0]} bytes) is over the --prov-budget of {over[2]} bytes",
              file=sys.stderr)
        steps = False
    if steps and result.steps[-1].ok:
        prov_steps.fpga_steps(map_path(blif), args.out, top, top,
                              prov_steps.odin3_version(args.odin3), blif.name,
                              root=design_root(args.designs))
        result.summary["prov_steps"] = "prov/index.json"
    return blif if result.steps[-1].ok else None


LPM_NOTE = ("Only the netlist goes to Quartus. The sim file holds empty stubs of the LPM and "
            "black-box modules, for the read-back and iverilog only: a user module with a "
            "library name would override the megafunction in Quartus.")
LPM_INSTANCE_RE = re.compile(
    r"^\s*(?:\(\*[^\n]*?\*\)\s*)?(lpm_add_sub|lpm_mult|lpm_compare|altsyncram)\s+#\(", re.M)
LPM_ALWAYS_RE = re.compile(r"^\s*always\s+@", re.M)
LPM_REG_ARRAY_RE = re.compile(r"^\s*reg\s+(?:\[[^\]]*\]\s*)?\S+\s+\[[^\]]*\]\s*;", re.M)


def lpm_cell_counts(text: str) -> dict[str, int]:
    """Instances of each LPM type, ``always`` blocks (flip-flops and latches) and reg arrays
    (memories left to Quartus inference) in a netlist written by ``write_verilog --quartus``."""
    counts = {kind: 0 for kind in ("lpm_add_sub", "lpm_mult", "lpm_compare", "altsyncram")}
    for match in LPM_INSTANCE_RE.finditer(text):
        counts[match.group(1)] += 1
    counts["always_blocks"] = len(LPM_ALWAYS_RE.findall(text))
    counts["reg_arrays"] = len(LPM_REG_ARRAY_RE.findall(text))
    return counts


def lpm_steps(args: FlowArgs, top: str, result: FlowResult) -> Path | None:
    """The odin3 step and the read-back lint of target ``lpm``."""
    step_dir = args.out / "odin3"
    written = step_dir / f"{top}.v"
    script = lpm_script(relative_designs(args.designs), written, args.family)
    script = with_read_options(args, script, step_dir)
    step = run_step("odin3", odin3_command(args, top, script), step_dir,
                    cwd=design_root(args.designs), note=name_root_note(args.designs))
    result.steps.append(step)
    result.runs.append(step)
    if not step.ok:
        return None
    sim = step_dir / f"{top}_lpm_sim.v"
    back = f"read_techlib {quote(resource_root() / LPM_LIB_REL)}; " \
           f"read_verilog -icells {quote(written)} {quote(sim)}; proc"
    lint = run_step("readback", [str(args.odin3), "--check", "-p", back], args.out / "readback")
    result.steps.append(lint)
    result.runs.append(lint)
    result.summary["lpm_netlist"] = str(written)
    result.summary["lpm_sim_file"] = str(sim)
    result.summary["lpm_note"] = LPM_NOTE
    if written.is_file():
        result.summary["lpm_cells"] = lpm_cell_counts(written.read_text())
    return written if lint.ok else None


def silicon_run(args: FlowArgs, top: str, result: FlowResult) -> Path | None:
    """Target ``silicon``: tools/silicon/silicon_flow.py (steps, step files, optional tools)."""
    sys.path.insert(0, str(HERE.parent / "silicon"))
    import silicon_flow

    steps = tuple(x for x in args.steps.split(",") if x) or silicon_flow.TOP_STEPS
    tools = silicon_flow.discover(args.odin3, args.tools, args.pdk)
    gone = silicon_flow.missing(tools, steps)
    if gone:
        raise UsageError("silicon steps need tools that are not installed:\n  "
                         + "\n  ".join(gone) + "\n(see docs/user/silicon-flow.md; "
                         "--steps read,elab,map,equiv,netlist needs none)")
    run = silicon_flow.Run(args.designs[0], top, args.out, silicon_flow.default_lib(), tools,
                           args.clock, steps, design_root(args.designs),
                           strict_tristate=args.strict_tristate)
    start = time.monotonic()
    code = silicon_flow.run(run)
    log = args.out / "silicon.log"
    log.write_text("\n".join(run.log) + "\n")
    result.steps.append(StepResult("silicon", code == 0, log, time.monotonic() - start))
    result.summary["silicon_steps"] = run.log
    result.summary["silicon_seconds"] = run.seconds
    result.summary["prov_steps"] = "prov/index.json"
    return args.out if code == 0 else None


def check_hint_args(args: FlowArgs) -> None:
    """The usage errors of --hints and --two-pass."""
    if args.hints is not None and not args.hints.is_file():
        raise UsageError(f"no such hint file: {args.hints}")
    if (args.hints is not None or args.two_pass) and args.target != "vtr":
        raise UsageError("--hints and --two-pass apply to --target vtr only")
    if args.two_pass and not args.vpr:
        raise UsageError("--two-pass needs VPR (--vpr, VTR_ROOT): pass 1 goes to routing")
    if args.two_pass and args.hints is not None:
        raise UsageError("--two-pass writes its own hint file: not with --hints")
    if args.two_pass and (args.prov or args.timing_attr):
        raise UsageError("--two-pass sets the provenance depth itself: not with --prov or "
                         "--timing-attr")


def run_flow(args: FlowArgs) -> FlowResult:
    """Run every step; stop at the first that fails. UsageError for a bad input."""
    if args.target not in TARGETS:
        raise UsageError(f"unknown target '{args.target}' ({', '.join(TARGETS)})")
    for design in args.designs:
        if not design.is_file():
            raise UsageError(f"no such design file: {design}")
    if args.target == "silicon" and (args.incdirs or args.defines or args.read_opts):
        raise UsageError("-I, -D, -f and --read-opt are not supported with --target silicon yet")
    if args.target == "silicon" and len(args.designs) != 1:
        raise UsageError("--target silicon takes exactly one design file")
    if args.analysis and args.target != "vtr":
        raise UsageError("--analysis applies to --target vtr only (levelize needs bits)")
    if args.timing_attr and (args.target != "vtr" or not args.vpr):
        raise UsageError("--timing-attr needs --target vtr and VPR (--vpr, VTR_ROOT)")
    if args.timing_paths < 1:
        raise UsageError("--timing-paths needs a positive number")
    if args.prov and args.target != "vtr":
        raise UsageError("--prov applies to --target vtr only (silicon always writes prov/)")
    if args.timing_legacy and not args.timing_attr:
        raise UsageError("--timing-legacy needs --timing-attr")
    if args.timing_legacy and args.prov:
        raise UsageError("--timing-legacy is the route without the name map: not with --prov")
    check_hint_args(args)
    if not args.odin3.is_file():
        raise UsageError(f"no odin3 binary at {args.odin3}: build it, put it on PATH or give "
                         f"--odin3")
    top = resolve_top(args.designs, args.top)
    warning = root_warning(args.designs)
    if warning:
        print(warning, file=sys.stderr)
    qor_legs.REPO_ROOT = resource_root()  # the module seam: where $LIB (lib/vtr.o3lib) is
    result = FlowResult(ok=False)
    args.out.mkdir(parents=True, exist_ok=True)
    if args.two_pass:
        import two_pass

        done: FlowResult = two_pass.run(args, top, run_flow, vpr_seed_run)
        return done
    step_fn = {"vtr": vtr_steps, "lpm": lpm_steps, "silicon": silicon_run}[args.target]
    result.ok = step_fn(args, top, result) is not None
    result.summary["seconds"] = {st.name: round(st.seconds, 3) for st in result.steps}
    (args.out / "summary.json").write_text(json.dumps(result.summary, indent=2) + "\n")
    import flow_record  # the run record (mimir-run.json); after summary.json, which it hashes

    flow_record.write_record(args, result, top, qor_legs.REPO_ROOT, vtr_root_from_env())
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
    ap.add_argument("designs", nargs="*", type=Path,
                    help="Verilog design file(s) (or give -f)")
    ap.add_argument("-I", "--incdir", action="append", default=[], metavar="DIR",
                    help="include directory for `include and $readmemh/b (repeatable)")
    ap.add_argument("-D", "--define", action="append", default=[], metavar="NAME[=VALUE]",
                    help="preprocessor define (repeatable)")
    ap.add_argument("-f", "--file-list", action="append", default=[], type=Path, metavar="LIST",
                    help="Verilog file list: files, +incdir+, +define+, -I, -D, -f, -F "
                         "(repeatable; its files follow the designs given)")
    ap.add_argument("--read-opt", action="append", default=[], metavar="OPT",
                    help="an extra read_verilog option, written with an equals sign, e.g. "
                         "--read-opt=--auto-blackbox or --read-opt=--nomem2reg (repeatable)")
    ap.add_argument("--project", type=Path, metavar="X.o3proj",
                    help="an .o3proj: its files, include directories, defines and `top` "
                         "(files and options given as well are added; a different --top is an "
                         "error)")
    ap.add_argument("--top", help="top module (default: the project's, else the one "
                                  "uninstantiated module)")
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
    ap.add_argument("--prov", action="store_true",
                    help="write the BLIF with hash names and the version 2 name map "
                         "(<top>.blif.names: records, spans, constructs); implied by "
                         "--timing-attr; needs --target vtr")
    ap.add_argument("--prov-cut", action="store_true",
                    help="with --prov: give each LUT the record of the gates its function covers "
                         "(abc --prov-cut on; the netlist is the same); needs --target vtr")
    ap.add_argument("--prov-level", default="", metavar="L",
                    help="provenance depth for odin3: 0 off, 1 statement, 2 ancestry, 3 cut, "
                         "4 full (hash names, name map and, at 4, the prov/ step files), or auto "
                         "(the highest that fits --prov-budget); --prov and --prov-cut still "
                         "override; needs --target vtr")
    ap.add_argument("--prov-budget", default="", metavar="SIZE",
                    help="the most provenance bytes (K, M, G, either case): a level over it "
                         "drops to the highest that fits, with a WARNING in odin3/odin3.log; "
                         "the last --prov-level or --prov-budget given is used")
    ap.add_argument("--timing-attr", action="store_true",
                    help="attribute the critical path to HDL statements, modules and words "
                         "(timing/attribution.json and summary.txt); needs VPR")
    ap.add_argument("--timing-paths", type=int, default=None, metavar="N",
                    help=f"setup paths to report and attribute (default {VPR_DEFAULT_PATHS}); "
                         f"implies --timing-attr")
    ap.add_argument("--timing-legacy", action="store_true",
                    help="with --timing-attr: the v1.1.1 route (short-name BLIF plus a second "
                         "BLIF with provenance names, line-level spans) instead of the name map")
    ap.add_argument("--keep-results", action="store_true",
                    help="keep VPR's .net, .place and .route in vpr/run (odin3-prov-post reads "
                         "them); needs --vpr")
    ap.add_argument("--tools", type=Path, help="--target silicon: the user-space tools directory "
                    "(OpenROAD, Magic, Netgen, KLayout, PDK; or set ODIN3_SILICON_TOOLS)")
    ap.add_argument("--pdk", type=Path, help="--target silicon: the sky130A directory of the PDK")
    ap.add_argument("--steps", default="", help="--target silicon: comma list of steps (read,"
                    "elab,map,equiv,netlist,place,route,gds,extract,lvs,recover; default all)")
    ap.add_argument("--clock", default="", help="--target silicon: the clock port (enables CTS)")
    ap.add_argument("--strict-tristate", action="store_true",
                    help="make a tri-state conversion an error (default: the tristate pass runs "
                         "after flatten and warns once per conversion)")
    ap.add_argument("--hints", type=Path, metavar="FILE",
                    help="a timing hint file for the abc pass (read_hints before abc); "
                         "needs --target vtr")
    ap.add_argument("--two-pass", action="store_true",
                    help="pass 1 through routing, back-annotate the routed timing into a hint "
                         "file, pass 2 with it; writes pass1/, pass2/, hints.tsv and "
                         "two-pass.json; needs VPR")
    ap.add_argument("--hint-seeds", type=int, default=1, metavar="N",
                    help="with --two-pass: hints from VPR seed 1 (default) or the worst over "
                         "seeds 1..N")
    ap.add_argument("--hint-cut", choices=("exact", "all"), default="exact",
                    help="with --two-pass: LUTs with an exact cut only (default) or also "
                         "approximate cuts")
    ap.add_argument("--hint-max-cells", type=int, default=8, metavar="N",
                    help="with --two-pass: skip a LUT whose cut covers more than N word cells "
                         "(default 8; 0: no limit)")
    ap.add_argument("--odin3", type=Path, default=None,
                    help="odin3 binary (default: odin3 on PATH, else build/release/odin3)")
    ap.add_argument("--out", type=Path, help="output directory (default: odin3_out/<top>)")
    ns = ap.parse_args(argv)
    designs = list(ns.designs)
    incdirs = [str(Path(d).resolve()) for d in ns.incdir]
    defines = list(ns.define)
    for lst in ns.file_list:
        files, inc, dfn = expand_filelist(lst)
        designs += files
        incdirs += inc
        defines += dfn
    if ns.project is not None:
        files, inc, dfn, ptop = expand_project(ns.project)
        designs = files + designs
        incdirs += inc
        defines += dfn
        if ptop is not None and ns.top not in (None, ptop):
            raise UsageError(f"--top {ns.top} conflicts with the top of {ns.project}: {ptop}")
        ns.top = ns.top or ptop
    if not designs:
        ap.error("no design files: give files, -f LIST or --project X.o3proj")
    ns.designs = designs
    out = ns.out
    if out is None:
        for design in ns.designs:
            if not design.is_file():
                raise UsageError(f"no such design file: {design}")
        out = Path("odin3_out") / resolve_top(ns.designs, ns.top)
    vpr = resolve_vpr(ns.vpr, ns.target)
    if ns.prov_cut and not (ns.prov or ns.timing_attr or ns.timing_paths is not None):
        ap.error("--prov-cut needs --prov (or --timing-attr)")
    if ns.prov_level and ns.prov_level not in ("0", "1", "2", "3", "4", "auto"):
        ap.error("--prov-level expects 0, 1, 2, 3, 4 or auto")
    if ns.prov_budget and parse_size(ns.prov_budget) is None:
        ap.error("--prov-budget expects a positive size with an optional K, M or G (1M, 500k)")
    if (ns.prov_level or ns.prov_budget) and ns.target != "vtr":
        ap.error("--prov-level and --prov-budget need --target vtr")
    if ns.keep_results and not vpr:
        ap.error("--keep-results needs VPR (--vpr)")
    if ns.hint_seeds < 1:
        ap.error("--hint-seeds needs a positive number")
    arch = ns.arch.resolve() if ns.arch else None
    attr = ns.timing_attr or ns.timing_paths is not None
    prov = ns.prov or (attr and not ns.timing_legacy)
    if prov:  # an explicit older option wins: --prov (also from --timing-attr) runs as before
        ns.prov_level, ns.prov_budget = prov_depth_ignored(ns.prov_level, ns.prov_budget)
    paths = ns.timing_paths if ns.timing_paths is not None else VPR_DEFAULT_PATHS
    return FlowArgs([d.resolve() for d in ns.designs], ns.top, ns.target, arch, ns.family, vpr,
                    ns.analysis, (ns.odin3 or default_odin3()).resolve(), out.resolve(),
                    attr, paths, prov, ns.timing_legacy, ns.keep_results,
                    ns.tools.resolve() if ns.tools else None,
                    ns.pdk.resolve() if ns.pdk else None, ns.steps, ns.clock,
                    tuple(dict.fromkeys(incdirs)), tuple(defines), tuple(ns.read_opt),
                    ns.strict_tristate, ns.prov_cut, ns.prov_level, ns.prov_budget,
                    ns.hints.resolve() if ns.hints else None, two_pass=ns.two_pass,
                    hint_seeds=ns.hint_seeds, hint_cut=ns.hint_cut,
                    hint_max_cells=ns.hint_max_cells)


def prov_depth_ignored(level: str, budget: str) -> tuple[str, str]:
    """``--prov`` keeps its own behaviour (hash names, the map, the step files, abc's default edge
    budget): the depth options given with it are dropped, one WARNING naming them."""
    ignored = [f"{opt} {val}" for opt, val in (("--prov-level", level), ("--prov-budget", budget))
               if val]
    if ignored:
        print(f"WARNING: {' and '.join(ignored)} ignored: --prov (or --timing-attr, which implies "
              f"it) keeps its own provenance behaviour", file=sys.stderr)
    return "", ""


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
