"""vtr-qor: the command line of the 3H QoR harness (3H spec QH-9, §5.2, §6, §12; plan Tasks 6, 9).

Subcommands here: ``doctor``, ``run``, ``status``, ``clean``, ``table``, ``gate`` and ``publish``;
Tasks 10-15 add the rest of spec §9. The common options are §9's: ``--vtr-root`` (default
``$VTR_ROOT``, else ``~/odin3-ws/external/vtr-verilog-to-routing``), ``--odin3`` (default
``<repo>/build/release/src/cli/odin3``), ``--out`` (default ``$ODIN3_QOR_DIR``, else
``~/odin3-ws/work/vtr-qor``; it must lie under ``~/odin3-ws/work`` or ``/tmp/claude-1000``, point
(m)), ``--arch k6|earch``, ``--no-wait``, ``--allow-vtr-drift`` and ``-v``.

- ``doctor`` (not heavy) checks the machine read-only and prints ``ok|FAIL  <item>: <detail>``
  per check; exit 0 when every check passes, else 1. The heavy lock is probed through
  ``/proc/locks``, so the probe never takes it.
- ``run`` checks VTR (E-QH1, E-QH2), the designs and legs (E-QH7) and the passes the legs need
  (E-QH5), and plans. ``--dry-run`` prints ``<leg> <design> <key12> cached|run`` per job and
  stops (not heavy). Otherwise it becomes a heavy run (``qor_heavy.reexec_under_lock``: through
  ``tools/heavy-run``, or in place when already inside one) and runs the jobs one at a time
  (``qor_sched.run_jobs``). ``finish_job`` builds each job's ``metrics.json`` record, and
  ``prepare_job``/``finish_job`` write ``job.json``. Only after the runner has recorded every
  job's final status (its ``after_run`` hook, before the ``done:`` line) does ``latest`` move:
  to a leg key whose own planned set (``planned.json``, every design ever planned under it) is
  complete; it is retracted when this run's key is no longer complete. ``--allow-vtr-drift``
  waives only a VTR at another commit (report-only: its own ``<vtr12>``, never ``latest``); a
  modified tracked tree is always refused.
- ``status`` prints one line per (arch, leg) of the current VTR commit:
  ``<arch> <leg> latest <key12|-> ok <n> failed <m> age <d>d``.
- ``clean [--keep N]`` runs retention (spec §5.4, ``qor_sched.prune``) over every
  ``<vtr12>/<arch>/<leg>``, each key judged by its own planned set.
- ``table --legs L,… [--keys L=key12,…] --label TEXT [--set S,…]`` (not heavy; it runs
  nothing) reads one key per leg (``--keys`` or ``latest``) under the pin's ``<vtr12>`` (the
  VTR checkout's with ``--allow-vtr-drift``), keeps the designs of ``--set`` (default
  ``gate,extra``), gates them (``tests/vtr/qor``'s ``bands.json`` and TSVs; a TSV that does not
  exist yet is empty) over ``gate_designs``: on k6 decision 3 needs all 19 (a table narrowed by
  ``--set`` is "not gated" and INCOMPLETE, ruling N1); on EArch the smoke set, an agent default
  never titled decision 3 (none: not gated). It reads VTR's golden rows of the table's arch
  for the cross-check when P is in the table, and writes
  ``<out>/tables/<YYYYMMDD-HHMM of the header date>-<label>/`` (``qor.md``, ``runs.csv``,
  ``ratios.csv``, ``runs.json``), printing the directory.
- ``gate <table dir>`` prints ``Gate (decision 3): PASS|FAIL|INCOMPLETE`` (an EArch table's
  title says it is not decision 3's), then the violations, from the table's ``runs.json``;
  exit 0, 1 or 3 (2 when the table is not gated).
- ``publish <table dir>`` (the controller's) needs ``ODIN3_QOR_PUBLISH=1`` (E-QH15). It copies
  the four files to ``~/odin3-ws/golden/qor/<vtr12>/<label>/`` (``~`` is ``$HOME``) through
  ``.<label>.tmp`` and one rename, refuses an existing label unless ``--force``, and prints the
  ``git -C ~/odin3-ws/golden add`` and ``commit`` lines to run.

The harness must be ``tools/heavy-run``'s direct child (``heavy-run tools/vtr-qor/vtr-qor run
…``, or ``ODIN3_HEAVY_OK=1 tools/vtr-qor/vtr-qor run …``, which re-runs itself that way): one job
runs at a time, so the runner refuses to start a command while any other process sits in
heavy-run's scope, and ``heavy-run sh -c 'vtr-qor run … | tee log'`` exits 2 with ``one job at a
time: … still holds process <sh pid>``. Redirect in the outer shell (``> log 2>&1``) instead.

``VTR_ROOT`` and ``ODIN3_QOR_DIR`` are the spec's configuration (§3, §5.1). No production path
reads a test hook from the environment: tests replace the module seams (``MEMINFO``, ``CPUINFO``,
``PROC_LOCKS``, ``SYSTEMD_PROBE`` here; ``qor_heavy``'s, ``qor_legs``' and ``qor_sched``'s).
"""

from __future__ import annotations

import argparse
import dataclasses
import functools
import json
import math
import os
import pwd
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Collection, Mapping, Sequence
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from types import MappingProxyType

# Point (u): ``python3 -I`` (3.11+) does not put the script's own directory on sys.path.
_HERE = str(Path(__file__).resolve().parent)
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import qor_heavy  # noqa: E402
import qor_legs  # noqa: E402
import qor_netstats  # noqa: E402
import qor_parse  # noqa: E402
import qor_sched  # noqa: E402
import qor_table  # noqa: E402
from qor_heavy import PREFIX, HeavyError  # noqa: E402
from qor_sched import Job, PlanRequest, RunOptions, Sets  # noqa: E402

PROG = "vtr-qor"
SCRIPT = Path(__file__).resolve()
SCHEMA = 1
DEFAULT_VTR_REL = Path("odin3-ws") / "external" / "vtr-verilog-to-routing"
DEFAULT_OUT_REL = Path("odin3-ws") / "work" / "vtr-qor"
ODIN3_REL = Path("build") / "release" / "src" / "cli" / "odin3"
VTR_PATHS_REL = Path("vtr_flow") / "scripts" / "python_libs" / "vtr" / "paths.py"
ARCHES = ("k6", "earch")
EARCH = "earch"
# Machine readers (module seams for the tests).
MEMINFO = Path("/proc/meminfo")
CPUINFO = Path("/proc/cpuinfo")
PROC_LOCKS = Path("/proc/locks")
# doctor's probe that a capped child scope can be made in the heavy slice (the brief's argv).
SYSTEMD_PROBE: tuple[str, ...] = (
    "systemd-run", "--user", "--scope", "-q", "--slice=odin3-heavy.slice", "true")
# docs/ORACLES.md pins VTR's Yosys (the Parmys host) at this version.
YOSYS_PIN = "Yosys 0.55"
# tools/heavy-run must keep the scope alive past an OOM kill (PR #58), or a capped job takes the
# harness down with it and is never recorded.
OOM_POLICY = "OOMPolicy=continue"
PROBE_TIMEOUT_S = 60
GIT_TIMEOUT_S = 60
PARSE_TIMEOUT_S = 600
SECONDS_PER_DAY = 86400
KIB_PER_GIB = 1024**2
MEM_DECIMALS = 10
EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
# The legs whose ABC depth and exchange come from the ``# abc:`` header of <J>/<d>.blif (point
# (e)); the others take VTR's own ``abc_depth`` (abc0.out ``lev = N``).
ABC_HEADER_LEGS = frozenset({"O", "Pprime", "Oprov"})
# The legs that run VTR's ABC stage after odin3 (-start abc): their front-end cost adds it.
ABC_STAGE_LEGS = frozenset({"Oprime", "R"})
ABC_STAGE_GLOBS = tuple(g for g in qor_parse.STAGE_GLOBS if g != "parmys.out")
NOT_OK = frozenset({qor_sched.FAILED, qor_sched.CAPPED, qor_sched.TIMEOUT, qor_sched.ABORTED})
KEY12_TEXT = re.compile(r"[0-9a-f]{12}")
LATEST = "latest"
HEAVY_SLICES = ("odin3-heavy.slice", qor_heavy.XL_SLICE)
VERDICTS_SKIPPED = MappingProxyType({"fcheck": "skipped", "equiv": "skipped", "rerun": "skipped"})
EXIT_INCOMPLETE = 3
GATE_EXIT = MappingProxyType({qor_table.PASS: EXIT_OK, qor_table.FAIL: EXIT_FAIL,
                              qor_table.INCOMPLETE: EXIT_INCOMPLETE})
LABEL_TEXT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
DEFAULT_TABLE_SETS = "gate,extra"
TSV_INPUTS = ("explanations.tsv", "divergences.tsv", "equiv-exceptions.tsv")
PUBLISH_ENV = "ODIN3_QOR_PUBLISH"
GOLDEN_REL = Path("odin3-ws") / "golden"
E_QH15 = "publish: set ODIN3_QOR_PUBLISH=1 (controller only; agents never write ~/odin3-ws/golden)"

# Read VTR's own vtr/paths.py by file (not ``import vtr``, whose package imports the whole flow),
# without writing bytecode into VTR's read-only tree.
_PATHS_SNIPPET = (
    "import importlib.util, sys\n"
    "spec = importlib.util.spec_from_file_location('vtr_paths', sys.argv[1])\n"
    "mod = importlib.util.module_from_spec(spec)\n"
    "spec.loader.exec_module(mod)\n"
    "print(mod.vpr_exe_path)\n"
    "print(mod.abc_exe_path)\n"
    "print(mod.yosys_exe_path)\n"
)
VTR_EXES = ("vpr", "abc", "yosys")
RUN_EPILOG = """\
A real run is tools/heavy-run's direct child: ODIN3_HEAVY_OK=1 tools/vtr-qor/vtr-qor run ...
(it re-runs itself through tools/heavy-run), or tools/heavy-run tools/vtr-qor/vtr-qor run ...
Never heavy-run sh -c '... | tee log': the runner refuses to start a job while another
process sits in heavy-run's scope (one job at a time). Redirect in the outer shell
(> log 2>&1). Started under nohup, a hangup is ignored (SIGHUP stays SIG_IGN)."""


# -- configuration -----------------------------------------------------------------------------


def _home() -> Path:
    """The account's home from the password database (``$HOME`` can leak)."""
    return Path(pwd.getpwuid(os.getuid()).pw_dir)


def _vtr_root(args: argparse.Namespace) -> Path:
    given = args.vtr_root or os.environ.get("VTR_ROOT") or _home() / DEFAULT_VTR_REL
    return Path(os.path.abspath(given))


def _odin3(args: argparse.Namespace) -> Path:
    return Path(os.path.abspath(args.odin3 or qor_legs.REPO_ROOT / ODIN3_REL))


def _out(args: argparse.Namespace) -> Path:
    """``--out`` resolved and checked (point (m), E-QH12)."""
    given = args.out or os.environ.get("ODIN3_QOR_DIR") or _home() / DEFAULT_OUT_REL
    return qor_heavy.check_out_dir(Path(given), _home())


def _run_quiet(argv: Sequence[str], timeout: float) -> subprocess.CompletedProcess[str] | None:
    """Run a probe (stdin closed, output captured); None when it cannot run or times out."""
    try:
        return subprocess.run(list(argv), stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, check=False, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None


def _git(repo: Path, *args: str) -> subprocess.CompletedProcess[str] | None:
    """git on a read-only tree: ``--no-optional-locks`` keeps ``status`` from rewriting the
    index."""
    return _run_quiet(["git", "--no-optional-locks", "-C", str(repo), *args], GIT_TIMEOUT_S)


# -- VTR (E-QH1, E-QH2) ------------------------------------------------------------------------


def _vtr_state(vtr_root: Path) -> tuple[str, int]:
    """VTR's full commit and its number of modified tracked files (E-QH1 when it is not a VTR
    checkout)."""
    if not vtr_root.is_dir():
        raise HeavyError(EXIT_USAGE, f"VTR not found at {vtr_root} (set VTR_ROOT or --vtr-root)")
    commit = qor_legs.vtr_commit(vtr_root)
    proc = _git(vtr_root, "status", "--porcelain", "--untracked-files=no")
    if proc is None or proc.returncode != 0:
        raise HeavyError(EXIT_USAGE, f"cannot read the state of VTR at {vtr_root} "
                                     "(git status failed)")
    return commit, sum(1 for line in proc.stdout.splitlines() if line.strip())


def _check_vtr(vtr_root: Path, allow_drift: bool) -> tuple[str, bool]:
    """The commit and whether it drifts from the pin. Another commit is E-QH2 unless
    ``allow_drift`` (a report-only run, point (p)): its own ``<vtr12>`` and keys. Modified
    tracked files are always E-QH2: the keys hold the commit only, so a modified tree would
    cache its results under the clean tree's keys (Task 6 ruling I1)."""
    commit, modified = _vtr_state(vtr_root)
    pin = qor_legs.VTR_PIN
    if modified > 0:
        raise HeavyError(EXIT_USAGE, f"VTR at {vtr_root} has {modified} modified tracked files")
    drift = commit != pin
    if drift and not allow_drift:
        raise HeavyError(EXIT_USAGE, f"VTR at {vtr_root} is {commit[:12]}, ORACLES.md pins "
                                     f"{pin[:12]} (use --allow-vtr-drift for a report-only run)")
    return commit, drift


def check_vtr(vtr_root: Path, allow_drift: bool) -> str:
    """VTR's full commit, after E-QH1 (no checkout at ``vtr_root``) and E-QH2: modified tracked
    files (``git status --porcelain --untracked-files=no``) always; another commit than the pin
    unless ``allow_drift`` (a report-only run)."""
    return _check_vtr(vtr_root, allow_drift)[0]


# -- designs and legs (E-QH5, E-QH7) -----------------------------------------------------------


def _split(text: str) -> list[str]:
    return [word.strip() for word in text.split(",") if word.strip()]


def _legs(text: str) -> tuple[str, ...]:
    names = _split(text)
    if not names:
        raise HeavyError(EXIT_USAGE, "--leg names no leg")
    return tuple(dict.fromkeys(qor_legs.normalise_leg(name) for name in names))


def odin3_passes(odin3: Path) -> frozenset[str]:
    """The passes ``<odin3> --help`` lists (point (t))."""
    proc = _run_quiet([str(odin3), "--help"], PROBE_TIMEOUT_S)
    if proc is None:
        raise HeavyError(EXIT_USAGE, f"cannot run {odin3} --help")
    return qor_legs.registered_passes(proc.stdout + "\n" + proc.stderr)


def check_legs(legs: Sequence[str], odin3: Path) -> None:
    """E-QH5: every pass of every odin3 leg's script is registered."""
    needing = [leg for leg in legs if qor_legs.LEG_PASSES[leg]]
    if not needing:
        return
    passes = odin3_passes(odin3)
    for leg in needing:
        missing = qor_legs.leg_available(leg, passes)
        if missing is not None:
            raise HeavyError(EXIT_USAGE, f"leg {leg} needs pass '{missing}', which {odin3} does "
                                         "not register")


# -- job.json and metrics.json -----------------------------------------------------------------


@dataclass(frozen=True)
class RunContext:
    """What ``prepare_job`` and ``finish_job`` need beyond the job: VTR, the arch, whether VTR
    drifts from the pin, the odin3 repo's commit (None for P-only runs) and the runner's
    options (the parse commands run through ``qor_sched.run_command``, inside the heavy run)."""

    vtr_root: Path
    arch_xml: Path
    vtr_commit: str
    vtr_drift: bool
    odin3_commit: str | None
    opts: RunOptions

    @property
    def python(self) -> Path:
        return self.vtr_root / ".venv" / "bin" / "python"


def _utc(when: float) -> str:
    return datetime.fromtimestamp(when, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _label(job: Job) -> str:
    return f"{job.leg} {job.design}"


def parse_commands(job: Job, ctx: RunContext) -> list[qor_legs.Command]:
    """Spec §4.2's two parse commands over ``<J>/run`` (``circuit=<d>.v`` for P, else
    ``<d>.blif``)."""
    circuit = f"{job.design}.v" if job.leg == "P" else f"{job.design}.blif"
    return qor_parse.parse_commands(ctx.vtr_root, ctx.python, job.job_dir / "run",
                                    ctx.arch_xml, circuit)


def _command_record(cmd: qor_legs.Command) -> dict[str, object]:
    return {"argv": list(cmd.argv), "cwd": str(cmd.cwd), "stdout": str(cmd.stdout),
            "stderr": None if cmd.stderr is None else str(cmd.stderr), "env": dict(cmd.env)}


def _host() -> dict[str, object]:
    """``{cpu, mem_gib}`` from ``/proc/cpuinfo`` and ``/proc/meminfo`` (None when unreadable)."""
    cpu: str | None = None
    mem: float | None = None
    try:
        for line in CPUINFO.read_text(encoding="utf-8", errors="replace").splitlines():
            name, _, value = line.partition(":")
            if name.strip() == "model name":
                cpu = value.strip()
                break
    except OSError:
        pass
    try:
        for line in MEMINFO.read_text(encoding="utf-8").splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[0] == "MemTotal:" and fields[1].isdigit():
                mem = round(int(fields[1]) / KIB_PER_GIB, 1)
    except OSError:
        pass
    return {"cpu": cpu, "mem_gib": mem}


def job_record(job: Job, ctx: RunContext, attempt_start: float) -> dict[str, object]:
    """``job.json`` as an attempt starts: the job, every key input, every command line (the
    job's and the parse's), ``PATH`` and ``ODIN3_*``, the UTC start, the host, the slice and
    ``vtr_drift``; ``ended`` (and P's point (l) fields) are filled in by ``finish_job``."""
    scope = ctx.opts.scope
    record: dict[str, object] = {
        "schema": SCHEMA, "leg": job.leg, "design": job.design, "arch": job.arch,
        "group": job.group, "gating": job.gating, "key": job.key, "key12": job.key12,
        "key_inputs": None if job.inputs is None else dataclasses.asdict(job.inputs),
        "commands": [_command_record(cmd) for cmd in (*job.commands,
                                                      *parse_commands(job, ctx))],
        "env": {name: value for name, value in sorted(ctx.opts.env.items())
                if name == "PATH" or name.startswith("ODIN3_")},
        "started": _utc(attempt_start), "ended": None, "host": _host(),
        "slice": None if scope is None else scope.path.name,
        "memory_max": None if scope is None else scope.memory_max,
        "vtr_root": str(ctx.vtr_root), "vtr_commit": ctx.vtr_commit,
        "vtr_drift": ctx.vtr_drift, "cost_warnings": [],
    }
    if job.leg == "P":
        record.update(parmys_blif_sha256=None, parmys_equals_golden=None)
    return record


def _write_json(path: Path, data: Mapping[str, object]) -> None:
    """JSON with sorted keys and indent 1 (plan, Determinism), written by atomic rename."""
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(json.dumps(data, sort_keys=True, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def prepare_job(job: Job, attempt_start: float, *, ctx: RunContext) -> None:
    """The runner's hook after each fresh attempt: ``job.json``, so failed jobs keep it."""
    _write_json(job.job_dir / "job.json", job_record(job, ctx, attempt_start))


def _run_parse(job: Job, ctx: RunContext) -> None:
    """VTR's parse (spec §4.2), each command through the runner (guarded, in the job cgroup)."""
    for cmd in parse_commands(job, ctx):
        outcome = qor_sched.run_command(cmd, ctx.opts,
                                        deadline=ctx.opts.clock() + PARSE_TIMEOUT_S)
        if outcome.kind != "exit" or outcome.returncode != 0:
            what = outcome.error or (f"exited {outcome.returncode}" if outcome.kind == "exit"
                                     else outcome.kind)
            raise HeavyError(EXIT_FAIL, f"{_label(job)}: VTR's parse {what}; see {cmd.stderr}")


def _abc_fields(job: Job, parse: Mapping[str, str]) -> tuple[int | None, str | None]:
    """``abc_depth`` and ``abc_exchange`` (point (e)): the last ``# abc:`` line of
    ``<J>/<d>.blif`` for O, Pprime and Oprov; VTR's ``abc_depth`` for the others."""
    if job.leg in ABC_HEADER_LEGS:
        try:
            header = qor_netstats.abc_header(job.job_dir / f"{job.design}.blif")
        except (OSError, ValueError) as exc:
            raise HeavyError(EXIT_FAIL, f"{_label(job)}: {exc}") from exc
        return (None, None) if header is None else (header.depth, header.exchange)
    value = qor_parse.present(parse.get("abc_depth"))
    return (int(value) if value is not None and value.is_integer() else None), None


def _netlist(job: Job, parse: Mapping[str, str], attempt_start: float) -> dict[str, object]:
    """Spec §4.1 from ``<J>/run/<d>.pre-vpr.blif`` (this attempt's), plus point (e)."""
    pre = job.job_dir / "run" / f"{job.design}.pre-vpr.blif"
    try:
        fresh = pre.stat().st_mtime >= attempt_start
        stats = qor_netstats.netstats(pre)
    except (OSError, ValueError) as exc:
        raise HeavyError(EXIT_FAIL, f"{_label(job)}: cannot read {pre}: {exc}") from exc
    if not fresh:
        raise HeavyError(EXIT_FAIL, f"{_label(job)}: stale {pre.name} from an earlier attempt; "
                                    "job failed")
    depth, exchange = _abc_fields(job, parse)
    return {"luts": stats.luts, "ffs": stats.ffs, "adders": stats.adders, "mults": stats.mults,
            "ram_slices": stats.ram_slices, "depth": stats.depth, "abc_depth": depth,
            "blif_sha256": stats.blif_sha256, "abc_exchange": exchange}


def _frontend_cost(job: Job) -> tuple[float | None, int | None, list[str]]:
    """Spec §4.3-4.4: P's front end is the parmys and ABC stages; an odin3 leg's is
    ``frontend.time``, plus VTR's ABC stage for Oprime and R. (Pprime's Parmys part, point (o),
    is plan Task 10's.) A cost with a log short of ``time -v`` lines gets a warning."""
    run = job.job_dir / "run"
    label = _label(job)
    if job.leg == "P":
        stage = qor_parse.stage_costs(run, qor_parse.STAGE_GLOBS, label)
        return round(stage.wall_s, 2), stage.peak_rss_mib, list(stage.warnings)
    timing = qor_parse.read_time_v(job.job_dir / "frontend.time")
    warnings = []
    if timing.wall_s is None or timing.peak_rss_mib is None:
        warnings.append(f"{PREFIX}{label}: no time -v in frontend.time; stage cost incomplete")
    wall, peak = timing.wall_s, timing.peak_rss_mib
    if job.leg in ABC_STAGE_LEGS:
        stage = qor_parse.stage_costs(run, ABC_STAGE_GLOBS, label)
        warnings += stage.warnings
        wall = (wall or 0.0) + stage.wall_s
        peak = max(peak or 0, stage.peak_rss_mib)
    return (None if wall is None else round(wall, 2)), peak, warnings


def _sha_or_none(path: Path) -> str | None:
    try:
        return qor_sched.file_sha256(path) if path.is_file() else None
    except HeavyError:
        return None


def _parmys_fields(job: Job) -> dict[str, object]:
    """Point (l), report-only: the SHA-256 of ``run/<d>.parmys.blif`` when VTR left it, and
    whether it equals the Parmys golden (None when either is missing)."""
    mine = _sha_or_none(job.job_dir / "run" / f"{job.design}.parmys.blif")
    golden = _sha_or_none(qor_legs.golden_blif(job.arch, job.design))
    equal = None if mine is None or golden is None else mine == golden
    return {"parmys_blif_sha256": mine, "parmys_equals_golden": equal}


def parmys_stage_wall(job: Job) -> float | None:
    """Point (o): the wall seconds of ``parmys.out`` in the latest P job of the same VTR commit,
    arch and design (``<vtr12>/<arch>/P/latest``); None when there is none."""
    p_leg = job.job_dir.parents[1].parent / "P"
    key12 = read_latest(p_leg)
    if key12 is None:
        return None
    return qor_parse.read_time_v(p_leg / key12 / job.design / "run" / "parmys.out").wall_s


def _warn(ctx: RunContext, lines: Sequence[str]) -> None:
    for line in lines:
        if ctx.opts.log is not None:
            ctx.opts.log.emit(line.removeprefix(PREFIX))
        else:
            print(line, file=sys.stderr, flush=True)


def finish_job(job: Job, attempt_start: float, *, ctx: RunContext) -> dict[str, object]:
    """One completed job's ``metrics.json`` record (spec §5.2, schema 1, plus
    ``netlist.abc_exchange``): VTR's parse, ``netstats`` of the pre-VPR netlist, the ``# abc:``
    header or VTR's ``abc_depth``, and the front-end cost. It also completes ``job.json``
    (``ended``, the cost warnings, P's point (l)). The runner writes ``metrics.json`` itself,
    after cleanup, with the key, the status and the elapsed time; any failure here is a
    HeavyError (E-QH8), and the job fails."""
    label = _label(job)
    _run_parse(job, ctx)
    run = job.job_dir / "run"
    parse = qor_parse.read_parse_results(run / qor_parse.PARSE_RESULTS,
                                         not_before=attempt_start, label=label)
    qor = qor_parse.read_parse_results(run / qor_parse.QOR_RESULTS,
                                       not_before=attempt_start, label=label)
    vtr = qor_parse.metrics_from_parse(parse, qor)
    netlist = _netlist(job, parse, attempt_start)
    wall, peak, warnings = _frontend_cost(job)
    parmys_wall = parmys_stage_wall(job) if job.leg == "Pprime" else None
    if job.leg == "Pprime" and parmys_wall is None:
        warnings.append(f"{PREFIX}Pprime {job.design}: no P job for the Parmys stage cost")
    _warn(ctx, warnings)
    inputs = job.inputs
    odin3 = job.leg != "P"
    metrics: dict[str, object] = {
        "schema": SCHEMA, "leg": job.leg, "design": job.design, "arch": job.arch,
        "key": job.key, "status": qor_sched.OK, "gating": job.gating,
        "vtr_commit": ctx.vtr_commit, "odin3_commit": ctx.odin3_commit if odin3 else None,
        "odin3_build": None if inputs is None else inputs.odin3_build,
        "abc_script_sha256": None if inputs is None else inputs.abc_script_sha256,
        "started": _utc(attempt_start), "elapsed_s": 0.0,
        "netlist": netlist, "packed": vtr["packed"], "routed": vtr["routed"],
        "cost": {"frontend_wall_s": wall, "frontend_peak_rss_mib": peak,
                 "vpr_time_s": qor_parse.vpr_time_s(run),
                 "vpr_total_runtime_s": vtr["cost"]["vpr_total_runtime_s"],
                 "max_vpr_mem_mib": vtr["cost"]["max_vpr_mem_mib"]},
        "verdicts": dict(VERDICTS_SKIPPED), "error": "",
    }
    record = job_record(job, ctx, attempt_start)
    record.update(ended=_utc(time.time()), cost_warnings=[w.removeprefix(PREFIX)
                                                          for w in warnings])
    if job.leg == "P":
        record.update(_parmys_fields(job))
    if job.leg == "Pprime":
        record["parmys_stage_wall_s"] = None if parmys_wall is None else round(parmys_wall, 2)
    _write_json(job.job_dir / "job.json", record)
    return metrics


# -- latest ------------------------------------------------------------------------------------


def _refuse_symlink(path: Path) -> None:
    if path.is_symlink():
        raise HeavyError(EXIT_USAGE, f"refusing to write {path}: {path} is a symlink")


def write_latest(leg_dir: Path, key12: str) -> None:
    """``<vtr12>/<arch>/<leg>/latest``: a file (no symlinks, plan Files), by atomic rename."""
    _refuse_symlink(leg_dir)
    latest = leg_dir / LATEST
    _refuse_symlink(latest)
    tmp = leg_dir / f".{LATEST}.tmp"
    tmp.write_text(f"{key12}\n", encoding="utf-8")
    os.replace(tmp, latest)


def read_latest(leg_dir: Path) -> str | None:
    """The key12 ``latest`` names; None when it is missing, a symlink, or not a key12."""
    latest = leg_dir / LATEST
    try:
        if latest.is_symlink() or not latest.is_file():
            return None
        word = latest.read_text(encoding="utf-8", errors="replace").strip()
    except FileNotFoundError:
        return None
    return word if KEY12_TEXT.fullmatch(word) else None


def _leg_complete(req: PlanRequest, sets: Sets, leg: str, key_dir: Path) -> bool:
    """Point (f) as ruled (Task 6 fix round 1): every design of the leg key's own planned set
    (``planned.json``) has a complete job under its *planned* job key."""
    planned = qor_sched.read_planned(key_dir)
    if not planned:
        return False
    jobs = qor_sched.plan_jobs(dataclasses.replace(req, legs=(leg,), designs=tuple(planned),
                                                   rerun=False), sets)
    return all(qor_sched.job_complete(job) for job in jobs)


def update_latest(req: PlanRequest, sets: Sets, *, drift: bool) -> list[str]:
    """After the run (every job's status recorded), per leg of ``req``: a leg key that now holds
    its planned set complete becomes ``latest``; a ``latest`` naming this run's key that is no
    longer complete is retracted (``latest`` is the newest *complete* key, spec §5.2). A drift
    run (VTR at another commit) is report-only and never moves ``latest``. A key's planned set
    grows only after ``latest`` is retracted from it (``qor_sched.write_planned``). A transient
    read error makes a job look incomplete here, so it may retract ``latest`` until the next run
    of that key (review N3; accepted). Returns the lines to log."""
    lines = []
    for leg in dict.fromkeys(qor_legs.normalise_leg(name) for name in req.legs):
        jobs = qor_sched.plan_jobs(dataclasses.replace(req, legs=(leg,)), sets)
        if not jobs:
            continue
        key_dir, key12 = jobs[0].job_dir.parent, jobs[0].key12
        leg_dir = key_dir.parent
        complete = _leg_complete(req, sets, leg, key_dir)
        current = read_latest(leg_dir)
        if complete and not drift and current != key12:
            write_latest(leg_dir, key12)
            lines.append(f"latest: {leg} {key12}")
        elif not complete and current == key12:
            _refuse_symlink(leg_dir / LATEST)
            qor_heavy.safe_rmtree(leg_dir / LATEST, req.out)
            lines.append(f"latest: {leg} {key12} retracted (no longer complete)")
    return lines


def _end_job_records(results: Sequence[qor_sched.JobResult]) -> None:
    """``job.json``'s ``ended`` for the jobs that ended without metrics (``finish_job`` sets it
    for ok ones): the time their ``status`` was written."""
    for result in results:
        record_path, status = result.job.job_dir / "job.json", result.job.job_dir / "status"
        try:
            record = json.loads(record_path.read_text(encoding="utf-8"))
            ended = status.stat().st_mtime
        except (OSError, ValueError):
            continue
        if isinstance(record, dict) and record.get("ended") is None:
            record["ended"] = _utc(ended)
            _write_json(record_path, record)


def after_run(results: Sequence[qor_sched.JobResult], *, req: PlanRequest, sets: Sets,
              ctx: RunContext) -> None:
    """The runner's last step before its ``done:`` line: ``job.json`` ends, then ``latest``
    (logged to ``harness.log``)."""
    _end_job_records(results)
    for line in update_latest(req, sets, drift=ctx.vtr_drift):
        _warn(ctx, [f"{PREFIX}{line}"])


# -- run ---------------------------------------------------------------------------------------


def _with_stage_timeout(job: Job, seconds: int) -> Job:
    """``--stage-timeout``: VTR's ``-timeout`` (housekeeping, outside the keys) replaced."""
    commands = []
    for cmd in job.commands:
        argv = list(cmd.argv)
        if "-timeout" in argv[:-1]:
            argv[argv.index("-timeout") + 1] = str(seconds)
        commands.append(dataclasses.replace(cmd, argv=tuple(argv)))
    return dataclasses.replace(job, commands=tuple(commands))


def _select(sets: Sets, args: argparse.Namespace) -> tuple[str, ...]:
    set_names, designs = _split(args.sets), _split(args.designs)
    if not set_names and not designs:
        raise HeavyError(EXIT_USAGE, "run needs --set or --designs")
    return qor_sched.select_designs(sets, set_names, designs)


def _check_run_options(args: argparse.Namespace, opts: RunOptions) -> None:
    qor_sched._check_jobs_option(opts)  # E-QH9 before anything is planned (point (w))
    if args.xl and (args.sets or not args.designs):
        raise HeavyError(EXIT_USAGE, "--xl is only for run --designs <capped designs> "
                                     "(the controller's, point (d))")


def odin3_commit(legs: Sequence[str], odin3: Path) -> str | None:
    """The commit of the checkout ``odin3`` was built in, when that is this repo (``--odin3``
    under ``REPO_ROOT``), with ``-dirty`` when tracked files are modified; else None (P-only
    runs too). ``odin3_build`` stays the binary's identity."""
    repo = Path(os.path.realpath(qor_legs.REPO_ROOT))
    if all(leg == "P" for leg in legs) or repo not in Path(os.path.realpath(odin3)).parents:
        return None
    head = _git(repo, "rev-parse", "HEAD")
    text = head.stdout.strip() if head is not None and head.returncode == 0 else ""
    if not re.fullmatch(r"[0-9a-f]{40}", text):
        return None
    status = _git(repo, "status", "--porcelain", "--untracked-files=no")
    dirty = status is None or status.returncode != 0 or bool(status.stdout.strip())
    return f"{text}-dirty" if dirty else text


def _plan_lines(jobs: Sequence[Job]) -> list[str]:
    def state(job: Job) -> str:
        if job.skip_reason is not None:
            return "skipped"
        return "cached" if job.cached and qor_sched.job_complete(job) else "run"
    return [f"{job.leg} {job.design} {job.key12} {state(job)}" for job in jobs]


def cmd_run(args: argparse.Namespace, argv: Sequence[str]) -> int:
    out = _out(args)
    timeouts = (qor_sched.JOB_TIMEOUT_S if args.job_timeout is None else
                MappingProxyType({group: args.job_timeout for group in qor_sched.GROUPS}))
    opts = RunOptions(out=out, jobs=args.jobs, job_timeout_s=timeouts, fcheck=args.fcheck,
                      meminfo=MEMINFO)
    _check_run_options(args, opts)
    vtr_root = _vtr_root(args)
    commit, drift = _check_vtr(vtr_root, args.allow_vtr_drift)
    sets = qor_sched.load_sets()
    designs = _select(sets, args)
    legs = _legs(args.leg)
    odin3 = _odin3(args)
    check_legs(legs, odin3)
    req = PlanRequest(legs=legs, designs=designs, arch=args.arch, out=out, vtr_root=vtr_root,
                      odin3=odin3, rerun=args.rerun)
    jobs = qor_sched.plan_jobs(req, sets)
    if args.stage_timeout is not None:
        jobs = [_with_stage_timeout(job, args.stage_timeout) for job in jobs]
    if args.dry_run:
        for line in _plan_lines(jobs):
            print(line)
        return EXIT_OK
    opts.scope = qor_heavy.reexec_under_lock(list(argv), script=SCRIPT, xl=args.xl,
                                             no_wait=args.no_wait, lock=qor_heavy.HEAVY_LOCK)
    if args.verbose:
        for line in _plan_lines(jobs):
            print(f"{PREFIX}plan: {line}", file=sys.stderr)
    opts.keep_netlists = frozenset(legs) if args.keep_netlists else frozenset()
    opts.retain_planned = True
    ctx = RunContext(vtr_root=vtr_root, arch_xml=qor_legs.arch_xml_path(vtr_root, args.arch),
                     vtr_commit=commit, vtr_drift=drift, odin3_commit=odin3_commit(legs, odin3),
                     opts=opts)
    opts.prepare = functools.partial(prepare_job, ctx=ctx)
    opts.after_run = functools.partial(after_run, req=req, sets=sets, ctx=ctx)
    summary = qor_sched.run_jobs(jobs, opts, functools.partial(finish_job, ctx=ctx))
    return summary.exit_code


# -- status and clean --------------------------------------------------------------------------


def _subdirs(directory: Path) -> list[Path]:
    """The directories in ``directory``, symlinks skipped (never followed). A live run creates
    and deletes entries meanwhile: one that vanishes is skipped, and so is a vanished
    ``directory``."""
    try:
        if directory.is_symlink() or not directory.is_dir():
            return []
        return sorted(p for p in directory.iterdir() if p.is_dir() and not p.is_symlink())
    except FileNotFoundError:
        return []


def _job_statuses(key_dirs: Sequence[Path]) -> list[tuple[str, float]]:
    """(status word, mtime) of every job; a file that vanishes while it is read is skipped."""
    found = []
    for key_dir in key_dirs:
        for job_dir in _subdirs(key_dir):
            status = job_dir / "status"
            try:
                if status.is_symlink():
                    continue
                when = status.stat().st_mtime
                word = status.read_text(encoding="utf-8", errors="replace").strip()
            except FileNotFoundError:
                continue
            found.append((word, when))
    return found


def status_line(arch: str, leg: str, leg_dir: Path, now: float) -> str:
    """``<arch> <leg> latest <key12|-> ok <n> failed <m> age <d>d``: the counts and the age
    (since the newest ``status``) over the ``latest`` key, or over every key when there is no
    ``latest``. ``failed`` counts every job that ended without a result (failed, capped,
    timeout, aborted)."""
    latest = read_latest(leg_dir)
    keys = _subdirs(leg_dir)
    if latest is not None and (leg_dir / latest) in keys:
        keys = [leg_dir / latest]
    found = _job_statuses(keys)
    ok = sum(word == qor_sched.OK for word, _ in found)
    failed = sum(word in NOT_OK for word, _ in found)
    newest = max((when for _, when in found), default=None)
    age = "-" if newest is None else f"{max(0, math.floor((now - newest) / SECONDS_PER_DAY))}d"
    return f"{arch} {leg} latest {latest or '-'} ok {ok} failed {failed} age {age}"


def cmd_status(args: argparse.Namespace, argv: Sequence[str]) -> int:
    out = _out(args)
    vtr12 = qor_legs.vtr_commit(_vtr_root(args))[:qor_sched.KEY12]
    now = time.time()
    lines = []
    for arch_dir in _subdirs(out / vtr12):
        for leg in qor_sched.PLAN_LEG_ORDER:
            leg_dir = arch_dir / leg
            if leg_dir.is_dir() and not leg_dir.is_symlink():
                lines.append(status_line(arch_dir.name, leg, leg_dir, now))
    if not lines:
        print(f"{PREFIX}status: no jobs under {out / vtr12}", file=sys.stderr)
    for line in lines:
        print(line)
    return EXIT_OK


def _failed_runs(leg_dir: Path) -> int:
    """The run directories kept for diagnosis (jobs that ended without a result)."""
    count = 0
    for key in _subdirs(leg_dir):
        for job in _subdirs(key):
            status = job / "status"
            word = status.read_text(encoding="utf-8", errors="replace").strip() \
                if status.is_file() and not status.is_symlink() else ""
            count += word in NOT_OK and (job / "run").is_dir()
    return count


def cmd_clean(args: argparse.Namespace, argv: Sequence[str]) -> int:
    """Retention (spec §5.4) over every ``<vtr12>/<arch>/<leg>`` under ``--out``; a key is
    complete when its own planned set is (``planned.json``)."""
    out = _out(args)
    for vtr_dir in _subdirs(out):
        if not KEY12_TEXT.fullmatch(vtr_dir.name):
            continue
        for arch_dir in _subdirs(vtr_dir):
            for leg in qor_sched.PLAN_LEG_ORDER:
                leg_dir = arch_dir / leg
                if not leg_dir.is_dir():
                    continue
                keys, runs = len(_subdirs(leg_dir)), _failed_runs(leg_dir)
                qor_sched.prune(out, vtr_dir.name, arch_dir.name, leg, planned=True,
                                keep_keys=args.keep)
                print(f"{PREFIX}clean: {leg_dir}: {keys - len(_subdirs(leg_dir))} keys and "
                      f"{runs - _failed_runs(leg_dir)} failed run directories removed",
                      file=sys.stderr)
    return EXIT_OK


# -- table, gate, publish (spec §6, §9) ------------------------------------------------------


def _keys(text: str, legs: Sequence[str]) -> dict[str, str]:
    keys = {}
    for item in _split(text):
        leg, sep, key12 = item.partition("=")
        if not sep or not leg.strip() or not key12.strip():
            raise HeavyError(EXIT_USAGE, f"--keys expects L=key12,...: '{item}'")
        name = qor_legs.normalise_leg(leg.strip())
        if name not in legs:
            raise HeavyError(EXIT_USAGE, f"--keys names leg {name}, which --legs does not")
        keys[name] = key12.strip()
    return keys


def _label_arg(text: str) -> str:
    if not LABEL_TEXT.fullmatch(text):
        raise HeavyError(EXIT_USAGE, f"--label '{text}' is not a name (letters, digits, '.', "
                                     "'_' and '-', starting with a letter or digit)")
    return text


def _tsv_rows(name: str) -> list[tuple[str, str, str]]:
    """A ``tests/vtr/qor`` TSV's rows; one that does not exist yet (Tasks 11, 12) is empty."""
    path = qor_table.REPO_INPUTS / name
    return qor_table.load_tsv(path) if path.exists() else []


def _table_vtr12(args: argparse.Namespace) -> str:
    if args.allow_vtr_drift:
        return qor_legs.vtr_commit(_vtr_root(args))[:qor_sched.KEY12]
    return qor_legs.VTR_PIN[:qor_sched.KEY12]


def _golden(args: argparse.Namespace, arch: str) -> qor_table.GoldenXcheck:
    vtr_root = _vtr_root(args)
    try:
        return qor_table.load_golden_xcheck(vtr_root, arch=arch)
    except (OSError, ValueError) as exc:
        raise HeavyError(EXIT_USAGE, f"table: cannot read VTR's golden results under "
                                     f"{vtr_root}: {exc}") from exc


def gate_designs(sets: Sets, arch_key: str, selected: Collection[str]) -> tuple[str, ...]:
    """The gate designs a table holds: on k6 §6.2's 19 that its ``--set`` includes (decision 3
    gates only when all 19 are there, ruling N1); on EArch the smoke set, its only set (§2; an
    agent default, never decision 3's gate). Empty on EArch: the table is not gated."""
    base = sets.smoke if arch_key == EARCH else sets.gate
    return tuple(design for design in base if design in selected)


def cmd_table(args: argparse.Namespace, argv: Sequence[str]) -> int:
    """Spec §6: the four files of one table; prints its directory."""
    out = _out(args)
    label = _label_arg(args.label)
    legs = _legs(args.legs)
    keys = _keys(args.keys, legs)
    sets = qor_sched.load_sets()
    designs = set(qor_sched.select_designs(sets, _split(args.sets), []))
    arch, vtr12 = qor_legs.ARCH_BASES[args.arch], _table_vtr12(args)
    runs = [run for run in qor_table.load_runs(out, vtr12, arch, keys, legs=legs)
            if run.design in designs]
    try:
        bands = qor_table.load_bands(qor_table.BANDS_JSON)
        explanations, divergences, exceptions = (_tsv_rows(name) for name in TSV_INPUTS)
    except (OSError, ValueError) as exc:
        raise HeavyError(EXIT_USAGE, f"table: {exc}") from exc
    gate_set = gate_designs(sets, args.arch, designs)
    narrowed = len(gate_set) if args.arch != EARCH and len(gate_set) != len(sets.gate) else None
    if narrowed is not None:
        gate_set = sets.gate  # GEOMEAN_GATE stays over the 19; n shows how many are there
        verdict: qor_table.Verdict | None = qor_table.narrowed_verdict(narrowed)
    else:
        verdict = qor_table.gate(qor_table.GateInputs(
            runs=runs, bands=bands, gate_set=gate_set, explanations=explanations,
            divergences=divergences, equiv_exceptions=exceptions)) if gate_set else None
    golden = _golden(args, arch) if "P" in legs else None
    header = qor_table.table_header(runs, arch=arch, label=label, vtr12=vtr12)
    table_dir = out / "tables" / f"{qor_table.table_stamp(header)}-{label}"
    table_dir.mkdir(parents=True, exist_ok=True)
    qor_table.write_md(runs, verdict, header, table_dir / "qor.md", golden=golden, bands=bands,
                       divergences=divergences, equiv_exceptions=exceptions, gate_set=gate_set,
                       narrowed=narrowed)
    qor_table.write_csv(runs, table_dir / "runs.csv", table_dir / "ratios.csv", bands=bands,
                        gate_set=gate_set)
    qor_table.write_json(runs, verdict, header, table_dir / "runs.json", bands=bands,
                         gate_set=gate_set)
    print(table_dir)
    return EXIT_OK


def _table_json(table_dir: Path, what: str) -> dict[str, object]:
    """A table directory's ``runs.json`` (its header and gate object checked)."""
    path = Path(os.path.abspath(table_dir)) / "runs.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise HeavyError(EXIT_USAGE, f"{what}: {path}: not a table ({exc})") from exc
    if not isinstance(data, dict) or not isinstance(data.get("header"), dict) \
            or data.get("schema") != qor_table.TABLE_SCHEMA:
        raise HeavyError(EXIT_USAGE, f"{what}: {path}: not a table (no schema 1 header)")
    return data


def cmd_gate(args: argparse.Namespace, argv: Sequence[str]) -> int:
    """§6.2's verdict as the table recorded it: the verdict line, then each violation."""
    data = _table_json(args.table_dir, "gate")
    gate_object = data.get("gate")
    if not isinstance(gate_object, dict):
        raise HeavyError(EXIT_USAGE, f"gate: {os.path.abspath(args.table_dir)}: the table is "
                                     "not gated (no gate design in it)")
    state = qor_table.verdict_state(gate_object)
    header = data["header"]
    arch = header.get("arch") if isinstance(header, dict) else None
    print(f"{qor_table.gate_title(arch if isinstance(arch, str) else '')}: {state}")
    violations = gate_object.get("violations")
    for line in violations if isinstance(violations, list) else []:
        print(line)
    return GATE_EXIT[state]


def _publish_target(header: Mapping[str, object]) -> tuple[str, str]:
    vtr12, label = header.get("vtr12"), header.get("label")
    if not isinstance(vtr12, str) or not KEY12_TEXT.fullmatch(vtr12) \
            or not isinstance(label, str) or not LABEL_TEXT.fullmatch(label):
        raise HeavyError(EXIT_USAGE, "publish: the table header has no valid vtr12 and label")
    return vtr12, label


def _check_table_dir(path: Path) -> None:
    """Refuse a symlink, or a directory holding anything but the four table files (absent is
    fine): only such a directory is ever replaced or removed by ``publish``."""
    if path.is_symlink():
        raise HeavyError(EXIT_USAGE, f"publish: refusing {path}: it is a symlink")
    if not path.exists():
        return
    others = sorted(p.name for p in path.iterdir() if p.name not in qor_table.TABLE_FILES)
    if others:
        raise HeavyError(EXIT_USAGE, f"publish: refusing to replace or remove {path}: it holds "
                                     f"{', '.join(others)}")


def _remove_table_dir(path: Path) -> None:
    """Remove a published table directory left by ``publish`` itself (its four files, then the
    directory), after ``_check_table_dir``: nothing else in odin3-golden is ever deleted."""
    _check_table_dir(path)
    if not path.exists():
        return
    for name in qor_table.TABLE_FILES:
        (path / name).unlink(missing_ok=True)
    path.rmdir()


def cmd_publish(args: argparse.Namespace, argv: Sequence[str]) -> int:
    """Copy a table's four files to odin3-golden (the controller's step, E-QH15): into
    ``.<label>.tmp``, then renamed into place, so a label holds one whole table. An existing
    label is refused unless ``--force``."""
    if os.environ.get(PUBLISH_ENV) != "1":
        raise HeavyError(EXIT_USAGE, E_QH15)
    source = Path(os.path.abspath(args.table_dir))
    header = _table_json(source, "publish")["header"]
    assert isinstance(header, dict)
    vtr12, label = _publish_target(header)
    missing = [name for name in qor_table.TABLE_FILES
               if not (source / name).is_file() or (source / name).is_symlink()]
    if missing:
        raise HeavyError(EXIT_USAGE, f"publish: {source} lacks {', '.join(missing)}")
    parent = Path.home() / GOLDEN_REL / "qor" / vtr12
    target = parent / label
    if target.is_symlink():
        raise HeavyError(EXIT_USAGE, f"publish: refusing {target}: it is a symlink")
    if target.exists() and not args.force:
        raise HeavyError(EXIT_USAGE, f"publish: {target} exists (--force replaces it)")
    _check_table_dir(target)  # review N2: refuse before any rename, never strand a file
    parent.mkdir(parents=True, exist_ok=True)
    tmp, old = parent / f".{label}.tmp", parent / f".{label}.old"
    _remove_table_dir(tmp)
    _remove_table_dir(old)
    tmp.mkdir()
    for name in qor_table.TABLE_FILES:
        shutil.copyfile(source / name, tmp / name)
    if target.exists():
        os.rename(target, old)
    os.rename(tmp, target)
    _remove_table_dir(old)
    print(f"git -C ~/odin3-ws/golden add qor/{vtr12}/{label}")
    print(f'git -C ~/odin3-ws/golden commit -m "qor: {label} ({vtr12})"')
    return EXIT_OK


# -- doctor ------------------------------------------------------------------------------------

CheckResult = tuple[bool, str]


def _vtr_item(vtr_root: Path) -> CheckResult:
    commit = check_vtr(vtr_root, allow_drift=False)
    return True, f"{vtr_root} at {commit[:12]} (the pin), no modified tracked files"


def vtr_exes(vtr_root: Path) -> dict[str, Path]:
    """``vpr``, ``abc`` and ``yosys`` as VTR's own ``vtr/paths.py`` names them."""
    paths_py = vtr_root / VTR_PATHS_REL
    if not paths_py.is_file():
        raise HeavyError(EXIT_USAGE, f"{paths_py} missing")
    proc = _run_quiet([sys.executable, "-I", "-B", "-c", _PATHS_SNIPPET, str(paths_py)],
                      PROBE_TIMEOUT_S)
    lines = [] if proc is None or proc.returncode != 0 else proc.stdout.splitlines()
    if len(lines) != len(VTR_EXES):
        raise HeavyError(EXIT_USAGE, f"cannot read {paths_py}")
    return {name: Path(line) for name, line in zip(VTR_EXES, lines, strict=True)}


def _executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def _exe_item(exes: Callable[[], dict[str, Path]], name: str) -> CheckResult:
    path = exes()[name]
    return (True, str(path)) if _executable(path) else (False, f"{path} missing")


def _first_line(proc: subprocess.CompletedProcess[str] | None) -> str:
    if proc is None:
        return "did not run"
    lines = (proc.stdout + proc.stderr).strip().splitlines()
    return lines[0].strip() if lines else f"exit {proc.returncode}"


def _yosys_item(exes: Callable[[], dict[str, Path]]) -> CheckResult:
    """VTR's Yosys (``run_random_check``'s ``resolve_yosys`` finds the same one) at the
    version ORACLES.md pins."""
    path = exes()["yosys"]
    if not _executable(path):
        return False, f"{path} missing"
    version = _first_line(_run_quiet([str(path), "-V"], PROBE_TIMEOUT_S))
    if version == YOSYS_PIN or version.startswith(YOSYS_PIN + " "):
        return True, f"{path} ({version})"
    return False, f"{path} is {version}, ORACLES.md pins {YOSYS_PIN}"


def _verilator_item() -> CheckResult:
    found = shutil.which("verilator")
    if found is None:
        return False, "verilator is not on PATH"
    return True, f"{found} ({_first_line(_run_quiet([found, '--version'], PROBE_TIMEOUT_S))})"


def _venv_item(vtr_root: Path) -> CheckResult:
    python = vtr_root / ".venv" / "bin" / "python"
    proc = _run_quiet([str(python), "-I", "-c", "import sys; print(sys.version.split()[0])"],
                      PROBE_TIMEOUT_S)
    if proc is None or proc.returncode != 0:
        return False, f"{python} does not run"
    return True, f"{python} (Python {proc.stdout.strip()})"


def _time_item() -> CheckResult:
    argv = [qor_legs.TIME_BIN, "-v", "true"]
    proc = _run_quiet(argv, PROBE_TIMEOUT_S)
    shown = " ".join(argv)
    if proc is None or proc.returncode != 0 or "Maximum resident set size" not in proc.stderr:
        return False, f"{shown} does not report time -v figures"
    return True, shown


def _heavy_run_item() -> CheckResult:
    path = qor_heavy.HEAVY_RUN
    if not path.is_file():
        return False, "tools/heavy-run missing (2E-1 not merged)"
    if not os.access(path, os.X_OK):
        return False, f"{path} is not executable"
    code = [line for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
            if not line.lstrip().startswith("#")]
    if not any(OOM_POLICY in line for line in code):
        return False, f"{path} does not pass -p {OOM_POLICY} (PR #58 not merged)"
    return True, f"{path} ({OOM_POLICY})"


def _systemd_item() -> CheckResult:
    shown = " ".join(SYSTEMD_PROBE)
    proc = _run_quiet(SYSTEMD_PROBE, PROBE_TIMEOUT_S)
    if proc is None or proc.returncode != 0:
        return False, f"{shown} failed ({_first_line(proc)})"
    return True, shown


def _disk_item() -> CheckResult:
    free = qor_heavy.disk_free_gib()
    if free < qor_sched.MIN_DISK_FREE_GIB:
        return False, f"C: has {free} GiB free (< {qor_sched.MIN_DISK_FREE_GIB} GiB)"
    return True, f"C: has {free} GiB free"


def _memory_item() -> CheckResult:
    mem = qor_heavy.mem_available_gib(MEMINFO)
    shown = f"MemAvailable {math.floor(mem * MEM_DECIMALS) / MEM_DECIMALS:.1f} GiB"
    if mem < qor_sched.MIN_MEM_AVAILABLE_GIB:
        return False, f"{shown} (< {qor_sched.MIN_MEM_AVAILABLE_GIB})"
    return True, shown


def lock_holders(lock: Path) -> list[tuple[str, str]]:
    """The ``(READ|WRITE, pid)`` of every lock held on ``lock``'s inode, read from
    ``/proc/locks``: a probe that never takes the lock (a waiter's ``->`` line is skipped)."""
    try:
        info = os.stat(lock)
    except FileNotFoundError:
        return []
    want = (os.major(info.st_dev), os.minor(info.st_dev), info.st_ino)
    holders = []
    for line in PROC_LOCKS.read_text(encoding="ascii", errors="replace").splitlines():
        fields = line.split()
        if len(fields) < 6 or "->" in fields:
            continue
        where = fields[5].split(":")
        try:
            found = (int(where[0], 16), int(where[1], 16), int(where[2]))
        except (IndexError, ValueError):
            continue
        if found == want:
            holders.append((fields[3], fields[4]))
    return holders


def heavy_run_holding(lock: Path) -> str | None:
    """The heavy slice this process runs in when it holds ``lock`` as ``tools/heavy-run``'s
    child does: its cgroup lies in a heavy slice and the inherited descriptor 9 is the lock.
    Decided from the cgroup and the descriptor, never from an environment variable."""
    try:
        text = qor_heavy.SELF_CGROUP.read_text(encoding="utf-8")
        target = os.readlink(qor_heavy.FD9)
    except OSError:
        return None
    if os.path.realpath(target) != os.path.realpath(lock):
        return None
    cgroup = next((line[3:] for line in text.splitlines() if line.startswith("0::")), "")
    for heavy_slice in HEAVY_SLICES:
        if f"/{heavy_slice}/" in cgroup:
            return heavy_slice
    return None


def _lock_item() -> CheckResult:
    """Free, or held by this very heavy run (doctor inside the nightly), is ok; a build's shared
    lock is ok too (``heavy-run`` waits for it, writer first); another heavy run is a FAIL."""
    lock = qor_heavy.HEAVY_LOCK
    own = heavy_run_holding(lock)
    if own is not None:
        return True, f"{lock} held by this heavy run ({own})"
    holders = lock_holders(lock)
    writers = [(mode, pid) for mode, pid in holders if mode != "READ"]
    if writers:
        mode, pid = writers[0]
        return False, f"{lock} is busy ({mode} lock held by pid {pid}: another heavy run)"
    if holders:
        return True, (f"{lock} shared by a build (READ lock held by pid {holders[0][1]}); "
                      "heavy-run waits for it")
    return True, f"{lock} free"


def _legs_item(odin3: Path) -> CheckResult:
    """Which legs ``odin3`` can run (informational: a leg waiting for its passes is not a
    fault of the machine)."""
    if not _executable(odin3):
        return True, f"P; {odin3} missing, so no odin3 leg"
    passes = odin3_passes(odin3)
    available, missing = [], []
    for leg in qor_sched.PLAN_LEG_ORDER:
        lacking = qor_legs.leg_available(leg, passes)
        if lacking is None:
            available.append(leg)
        else:
            missing.append(f"; {leg} missing '{lacking}'")
    return True, " ".join(available) + "".join(missing)


def doctor_checks(vtr_root: Path, odin3: Path) -> list[tuple[str, Callable[[], CheckResult]]]:
    exes = functools.cache(lambda: vtr_exes(vtr_root))
    return [
        ("vtr", lambda: _vtr_item(vtr_root)),
        ("vpr", lambda: _exe_item(exes, "vpr")),
        ("abc", lambda: _exe_item(exes, "abc")),
        ("yosys", lambda: _yosys_item(exes)),
        ("verilator", _verilator_item),
        ("venv", lambda: _venv_item(vtr_root)),
        ("time", _time_item),
        ("heavy-run", _heavy_run_item),
        ("systemd-run", _systemd_item),
        ("disk", _disk_item),
        ("memory", _memory_item),
        ("lock", _lock_item),
        ("legs", lambda: _legs_item(odin3)),
    ]


def cmd_doctor(args: argparse.Namespace, argv: Sequence[str]) -> int:
    """The read-only checklist (spec §9, §12 step 1): one line per check, exit 0 or 1."""
    failed = 0
    for item, check in doctor_checks(_vtr_root(args), _odin3(args)):
        try:
            ok, detail = check()
        except HeavyError as exc:
            ok, detail = False, exc.message
        except OSError as exc:
            ok, detail = False, str(exc)
        print(f"{'ok' if ok else 'FAIL'}  {item}: {detail}", flush=True)
        failed += not ok
    return EXIT_OK if failed == 0 else EXIT_FAIL


# -- main --------------------------------------------------------------------------------------


def _positive(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        value = 0
    if value <= 0:
        raise argparse.ArgumentTypeError(f"'{text}' is not a positive integer")
    return value


def _parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--vtr-root", type=Path, metavar="DIR",
                        help="VTR (default $VTR_ROOT, else ~/odin3-ws/external/vtr-...)")
    common.add_argument("--odin3", type=Path, metavar="PATH",
                        help="the odin3 binary (default <repo>/build/release/src/cli/odin3)")
    common.add_argument("--out", type=Path, metavar="DIR",
                        help="the work tree (default $ODIN3_QOR_DIR, else ~/odin3-ws/work/vtr-qor)")
    common.add_argument("--arch", choices=ARCHES, default="k6")
    common.add_argument("--no-wait", action="store_true", help="exit 4 if the heavy lock is busy")
    common.add_argument("--allow-vtr-drift", action="store_true",
                        help="run on a VTR other than the pin (report-only)")
    common.add_argument("-v", "--verbose", action="store_true")
    parser = argparse.ArgumentParser(prog=PROG, description="the 3H QoR harness (VTR flow runs "
                                     "of Parmys and Odin III on the VTR benchmarks)")
    sub = parser.add_subparsers(dest="command", required=True, metavar="COMMAND")
    sub.add_parser("doctor", parents=[common], help="check the machine (read-only)")
    run = sub.add_parser(
        "run", parents=[common], help="run legs through VTR (heavy)", epilog=RUN_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    run.add_argument("--leg", required=True, metavar="L[,L...]",
                     help="P, O, Oprime (O'), Pprime (P'), R, Oprov")
    run.add_argument("--set", dest="sets", default="", metavar="S[,S...]",
                     help="gate, extra, smoke")
    run.add_argument("--designs", default="", metavar="D[,D...]")
    run.add_argument("-j", dest="jobs", type=int, default=1, metavar="N")
    run.add_argument("--rerun", action="store_true", help="ignore the job cache")
    run.add_argument("--fcheck", action="store_true", help="the Verilator check after each job")
    run.add_argument("--keep-netlists", action="store_true")
    run.add_argument("--job-timeout", type=_positive, metavar="S")
    run.add_argument("--stage-timeout", type=_positive, metavar="S")
    run.add_argument("--dry-run", action="store_true", help="print the jobs; run nothing")
    run.add_argument("--xl", action="store_true",
                     help="the 9 GiB slice, for capped --designs only (the controller's)")
    sub.add_parser("status", parents=[common], help="latest key and counts per arch and leg")
    clean = sub.add_parser("clean", parents=[common], help="retention (spec §5.4)")
    clean.add_argument("--keep", type=_positive, default=qor_sched.KEEP_KEYS, metavar="N",
                       help="complete keys kept per arch and leg")
    table = sub.add_parser("table", parents=[common], help="write a QoR table (spec §6)")
    table.add_argument("--legs", required=True, metavar="L[,L...]",
                       help="P, O, Oprime (O'), Pprime (P'), R, Oprov")
    table.add_argument("--keys", default="", metavar="L=key12[,...]",
                       help="a key per leg (default: the leg's latest)")
    table.add_argument("--label", required=True, metavar="TEXT")
    table.add_argument("--set", dest="sets", default=DEFAULT_TABLE_SETS, metavar="S[,S...]",
                       help="gate, extra, smoke (default gate,extra)")
    gate = sub.add_parser("gate", parents=[common], help="print a table's verdict")
    gate.add_argument("table_dir", type=Path, metavar="TABLE_DIR")
    publish = sub.add_parser("publish", parents=[common],
                             help="copy a table to odin3-golden (controller only)")
    publish.add_argument("table_dir", type=Path, metavar="TABLE_DIR")
    publish.add_argument("--force", action="store_true", help="replace a published label")
    return parser


COMMANDS: dict[str, Callable[[argparse.Namespace, Sequence[str]], int]] = {
    "doctor": cmd_doctor, "run": cmd_run, "status": cmd_status, "clean": cmd_clean,
    "table": cmd_table, "gate": cmd_gate, "publish": cmd_publish,
}


def main(argv: Sequence[str] | None = None) -> int:
    """The command line; a HeavyError prints ``vtr-qor: <message>`` and gives its exit code."""
    args_list = list(sys.argv[1:] if argv is None else argv)
    args = _parser().parse_args(args_list)
    try:
        return COMMANDS[args.command](args, args_list)
    except HeavyError as exc:
        print(f"{PREFIX}{exc.message}", file=sys.stderr, flush=True)
        return exc.code
    except OSError as exc:  # a file system error outside the runner's own handling
        print(f"{PREFIX}{exc}", file=sys.stderr, flush=True)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
