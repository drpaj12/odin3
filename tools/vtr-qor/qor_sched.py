"""qor_sched: vtr-qor's jobs, keys, cache, planner and runner (3H spec QH-2, §5.3, §7; plan
Tasks 2 and 5).

**Order.** The planner is the one source of the job order:
- designs follow ``sets.json``'s ``order`` (ascending Parmys golden size, smallest first), group A
  before group B, whatever order they were asked in;
- jobs are design-major and leg-minor, legs in ``PLAN_LEG_ORDER`` (P, Pprime, Oprime, O, R,
  Oprov), so the smallest designs finish every leg first.

The runner keeps that order with one VTR or Verilator job at a time (``MAX_JOBS``, point (w)), so
each fcheck runs right after its own VPR job. The nightly's group B rotation and its 06:00
admission deadline select and admit jobs from this list (plan Task 13); they never reorder it.

**Keys** (point (f)). The *job key* is §5.3's SHA-256 over every key input and decides cache reuse
(``metrics.json`` ``key``). The *leg key* leaves out the design and its source SHA-256; its first
12 hex digits name the ``<key12>`` directory shared by every design of one leg configuration. Job
directories are ``<out>/<vtr12>/<arch base>/<leg>/<key12>/<d>``.

**The runner** (``run_jobs``; spec §7.2-7.6, plan point (w)) is the part that starts VTR:
- it refuses to start anything unless ``qor_heavy.require_heavy`` finds ``tools/heavy-run``'s
  real nesting evidence (the cgroup under the slice and descriptor 9 on the lock) and the cap,
  before the run and again before every job and command; an environment variable alone is never
  evidence, and there is no fallback;
- one job at a time (``MAX_JOBS``), group A then group B in plan order, each fcheck right after
  its own job and before the next one starts;
- every attempt starts from an empty job directory (``fresh_attempt``), each command runs in its
  own session under the job's wall timeout, and its whole process group is killed when it ends,
  so nothing of one job outlives it;
- admission (``admit``) holds jobs while ``C:`` or ``MemAvailable`` is short, stops after
  ``PAUSE_LIMIT_S``, and aborts a running job below ``DISK_FLOOR_GIB``.

No production path reads a test hook from the environment (controller ruling, 3H Task 1): tests
replace the module seams ``LDD_CMD`` and ``run_fcheck`` here, ``REPO_ROOT``/``GOLDEN_ROOT`` in
``qor_legs``, and ``SELF_CGROUP``/``CGROUP_ROOT``/``FD9``/``HEAVY_LOCK``/``DF_CMD`` in
``qor_heavy``.
"""

from __future__ import annotations

import dataclasses
import fnmatch
import gzip
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from contextlib import ExitStack, contextmanager, suppress
from dataclasses import dataclass, field
from pathlib import Path
from types import FrameType, MappingProxyType
from typing import IO, Any, NoReturn

import qor_heavy
import qor_legs
from qor_heavy import CAP_BYTES, GIB, MSG_NOT_INSIDE, PREFIX, HeavyError, ScopeInfo
from qor_legs import Command, LegContext

SCHEMA = 1
# One VTR or Verilator job at a time (ruling C1, point (w)).
MAX_JOBS = 1
PLAN_LEG_ORDER = ("P", "Pprime", "Oprime", "O", "R", "Oprov")
GOLDEN_LEGS = frozenset({"Pprime", "R"})
SET_NAMES = ("gate", "extra", "smoke")
GROUPS = ("A", "B")
DESIGN_FIELDS = frozenset({"rank", "group", "golden_bytes", "vtr_flow_time_s"})
# Design names become path components (<J>/<d>, <d>.v): no separators, no dots.
DESIGN_NAME = re.compile(r"[A-Za-z0-9_]+")
SETS_JSON = qor_legs.REPO_ROOT / "tests" / "vtr" / "qor" / "sets.json"
# ldd by absolute path when present; a module seam the tests replace (point (q)).
LDD_BIN = "/usr/bin/ldd" if Path("/usr/bin/ldd").exists() else "ldd"
LDD_CMD: tuple[str, ...] = (LDD_BIN,)
# The source SHA-256 of a Pprime/R job whose golden is absent; Task 10 skips it with E-QH6.
GOLDEN_MISSING = "missing"
KEY_HEX = 64
KEY12 = 12
_CHUNK = 1 << 20
_LDD_LINE = re.compile(r"^\s*(libodin3\S*?\.so(?:\.\d+)*)\s+=>\s+(\S+)")


# -- sets.json ---------------------------------------------------------------------------------


@dataclass(frozen=True)
class DesignInfo:
    """Spec §2's first five columns: the gate rank (None for the extras), group, golden size."""

    rank: int | None
    group: str
    golden_bytes: int
    vtr_flow_time_s: float


@dataclass(frozen=True)
class Sets:
    order: tuple[str, ...]
    gate: tuple[str, ...]
    extra: tuple[str, ...]
    smoke: tuple[str, ...]
    group_b: tuple[str, ...]
    designs: dict[str, DesignInfo]


def _bad(path: Path, what: str) -> HeavyError:
    return HeavyError(2, f"{path}: {what}")


def _names(data: Mapping[str, Any], field: str, path: Path, *, empty: bool = False
           ) -> tuple[str, ...]:
    value = data.get(field)
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise _bad(path, f"'{field}' is not a list of design names")
    if not value and not empty:
        raise _bad(path, f"'{field}' is empty")
    if len(set(value)) != len(value):
        raise _bad(path, f"'{field}' repeats a design")
    for name in value:
        if not DESIGN_NAME.fullmatch(name):
            raise _bad(path, f"'{field}' has a bad design name '{name}'")
    return tuple(value)


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _design_info(name: str, raw: object, path: Path) -> DesignInfo:
    if not isinstance(raw, dict) or set(raw) != DESIGN_FIELDS:
        raise _bad(path, f"design '{name}' needs exactly {', '.join(sorted(DESIGN_FIELDS))}")
    rank, group = raw["rank"], raw["group"]
    size, secs = raw["golden_bytes"], raw["vtr_flow_time_s"]
    if rank is not None and not _is_int(rank):
        raise _bad(path, f"design '{name}': rank is not an integer or null")
    if group not in GROUPS:
        raise _bad(path, f"design '{name}': group is not A or B")
    if not _is_int(size) or size <= 0:
        raise _bad(path, f"design '{name}': golden_bytes is not a positive integer")
    if not (_is_int(secs) or isinstance(secs, float)) or not math.isfinite(secs) or secs <= 0:
        raise _bad(path, f"design '{name}': vtr_flow_time_s is not a positive finite number")
    return DesignInfo(rank, str(group), size, float(secs))


def _subsequence(part: tuple[str, ...], order: tuple[str, ...]) -> bool:
    members = set(part)
    return part == tuple(d for d in order if d in members)


def _check_split(sets: Sets, path: Path) -> None:
    if set(sets.designs) != set(sets.order):
        raise _bad(path, "'designs' and 'order' name different designs")
    if set(sets.gate) & set(sets.extra) or set(sets.gate) | set(sets.extra) != set(sets.order):
        raise _bad(path, "'gate' and 'extra' must split 'order'")
    if not _subsequence(sets.gate, sets.order) or not _subsequence(sets.extra, sets.order):
        raise _bad(path, "'gate' and 'extra' must follow 'order'")
    if sets.smoke != sets.gate[:len(sets.smoke)]:
        raise _bad(path, "'smoke' must be the first designs of 'gate'")


def _check_ranks(sets: Sets, path: Path) -> None:
    for i, design in enumerate(sets.gate, start=1):
        if sets.designs[design].rank != i:
            raise _bad(path, f"design '{design}': rank is not {i} (its place in 'gate')")
    for design in sets.extra:
        if sets.designs[design].rank is not None:
            raise _bad(path, f"design '{design}': an extra has rank null")


def _check_order(sets: Sets, path: Path) -> None:
    in_b = tuple(d for d in sets.order if sets.designs[d].group == "B")
    if sets.group_b != in_b or sets.order[len(sets.order) - len(in_b):] != in_b:
        raise _bad(path, "'group_b' must be the designs of group B, at the end of 'order'")
    sizes = [sets.designs[d].golden_bytes for d in sets.order]
    if sizes != sorted(sizes):
        raise _bad(path, "'order' is not by ascending golden size")


def load_sets(path: Path = SETS_JSON) -> Sets:
    """Read and check ``tests/vtr/qor/sets.json`` (spec §2, point (i)). Failure: HeavyError(2)."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise _bad(path, f"cannot read: {exc}") from exc
    if not isinstance(data, dict) or not _is_int(data.get("schema")) \
            or data["schema"] != SCHEMA:
        raise _bad(path, f"not a schema {SCHEMA} sets file")
    raw = data.get("designs")
    if not isinstance(raw, dict):
        raise _bad(path, "'designs' is not an object")
    sets = Sets(
        order=_names(data, "order", path), gate=_names(data, "gate", path),
        extra=_names(data, "extra", path, empty=True), smoke=_names(data, "smoke", path),
        group_b=_names(data, "group_b", path, empty=True),
        designs={name: _design_info(name, info, path) for name, info in raw.items()},
    )
    _check_split(sets, path)
    _check_ranks(sets, path)
    _check_order(sets, path)
    return sets


def _check_designs(sets: Sets, designs: Iterable[str]) -> None:
    for design in designs:
        if design not in sets.designs:
            raise HeavyError(2, f"unknown design '{design}' (see tests/vtr/qor/sets.json)")


def _in_order(sets: Sets, chosen: Iterable[str]) -> tuple[str, ...]:
    """§2 order: group A then group B, each smallest first."""
    wanted = set(chosen)
    rank = {d: i for i, d in enumerate(sets.order)}
    return tuple(sorted(wanted, key=lambda d: (sets.designs[d].group == "B", rank[d])))


def select_designs(sets: Sets, set_names: Sequence[str], designs: Sequence[str]) -> tuple[str, ...]:
    """The union of the named sets and designs, in §2 order whatever the input order (E-QH7)."""
    by_name = {"gate": sets.gate, "extra": sets.extra, "smoke": sets.smoke}
    chosen: set[str] = set()
    for name in set_names:
        if name not in by_name:
            raise HeavyError(2, f"unknown set '{name}' ({', '.join(SET_NAMES)})")
        chosen.update(by_name[name])
    _check_designs(sets, designs)
    chosen.update(designs)
    return _in_order(sets, chosen)


# -- keys --------------------------------------------------------------------------------------


@dataclass(frozen=True)
class KeyInputs:
    """§5.3's key inputs. The odin3 fields (``odin3_build``, ``script``, ``lib_sha256``,
    ``abc_script_sha256``) are None for P, which runs no odin3."""

    leg: str
    design: str
    arch_base: str
    arch_sha256: str
    source_sha256: str
    vtr_commit: str
    params: tuple[str, ...]
    odin3_build: str | None
    script: str | None
    lib_sha256: str | None
    abc_script_sha256: str | None


def _digest(fields: dict[str, Any]) -> str:
    fields["schema"] = SCHEMA
    text = json.dumps(fields, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def job_key(k: KeyInputs) -> str:
    """The job key: SHA-256 of the canonical JSON of every field plus ``schema``."""
    return _digest(dataclasses.asdict(k))


def leg_key(k: KeyInputs) -> str:
    """The leg key: the job key without ``design`` and ``source_sha256`` (point (f))."""
    fields = dataclasses.asdict(k)
    del fields["design"], fields["source_sha256"]
    return _digest(fields)


def file_sha256(path: Path) -> str:
    """The SHA-256 of a file, streamed. Failure: HeavyError(2)."""
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(_CHUNK), b""):
                digest.update(chunk)
    except OSError as exc:
        raise HeavyError(2, f"cannot read {path}: {exc.strerror}") from exc
    return digest.hexdigest()


def _sha_or(path: Path, absent: str | None) -> str | None:
    return file_sha256(path) if path.is_file() else absent


def _ldd_libs(odin3: Path, env: Mapping[str, str] | None) -> list[tuple[str, Path]]:
    argv = [*LDD_CMD, str(odin3)]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, check=False,
                              env=None if env is None else dict(env))
    except OSError as exc:
        raise HeavyError(2, f"cannot run {argv[0]}: {exc.strerror}") from exc
    if proc.returncode != 0:
        if "not a dynamic executable" in proc.stdout + proc.stderr:
            return []
        raise HeavyError(2, f"cannot list the libraries of {odin3} ({argv[0]} exited "
                            f"{proc.returncode})")
    libs = []
    for line in proc.stdout.splitlines():
        match = _LDD_LINE.match(line)
        if match is None:
            continue
        if match.group(2) == "not":
            raise HeavyError(2, f"{odin3}: {argv[0]} cannot find {match.group(1)}")
        libs.append((match.group(1), Path(match.group(2))))
    return sorted(libs)


def odin3_build_id(odin3: Path, env: Mapping[str, str] | None = None) -> str:
    """The odin3 build id (point (q)): SHA-256 over the ``odin3`` binary, then every
    ``libodin3*.so`` that ldd resolves for it (``libodin3_abc.so`` included), in basename order.

    ``env`` is the environment ldd runs in (``LD_LIBRARY_PATH`` decides what resolves); None is
    this process's. The ldd argv is ``LDD_CMD`` (a module seam, never an environment hook).
    """
    digest = hashlib.sha256()
    digest.update(f"odin3\0{file_sha256(odin3)}\n".encode())
    for name, path in _ldd_libs(odin3, env):
        digest.update(f"{name}\0{file_sha256(path)}\n".encode())
    return digest.hexdigest()


# -- the planner -------------------------------------------------------------------------------


@dataclass(frozen=True)
class Job:
    """One (leg, design) job. ``key`` is the job key, ``key12`` the leg key's first 12 hex
    digits (the directory); ``job_dir`` is ``<out>/<vtr12>/<arch base>/<leg>/<key12>/<d>``."""

    leg: str
    design: str
    arch: str
    group: str
    gating: bool
    key: str
    key12: str
    job_dir: Path
    commands: tuple[Command, ...]
    cached: bool


@dataclass(frozen=True)
class PlanRequest:
    legs: tuple[str, ...]
    designs: tuple[str, ...]
    arch: str
    out: Path
    vtr_root: Path
    odin3: Path
    rerun: bool
    seed: int = 1


@dataclass(frozen=True)
class _Common:
    """The key inputs shared by every job of one plan, computed once."""

    arch: str
    arch_xml: Path
    arch_sha256: str
    vtr_commit: str
    python: Path
    repo: Path
    k: int
    odin3_build: str | None
    lib_sha256: str | None
    abc_script_sha256: str | None
    hashes: dict[Path, str] = dataclasses.field(default_factory=dict)


def _ordered_legs(legs: Iterable[str]) -> tuple[str, ...]:
    wanted = {qor_legs.normalise_leg(leg) for leg in legs}
    return tuple(leg for leg in PLAN_LEG_ORDER if leg in wanted)


def _common(req: PlanRequest, legs: tuple[str, ...]) -> _Common:
    arch = qor_legs.arch_base(req.arch)
    xml = qor_legs.arch_xml_path(req.vtr_root, arch)
    arch_sha = file_sha256(xml)
    commit = qor_legs.vtr_commit(req.vtr_root)
    python = req.vtr_root / ".venv" / "bin" / "python"
    repo = qor_legs.REPO_ROOT
    if not any(leg != "P" for leg in legs):
        return _Common(arch, xml, arch_sha, commit, python, repo, 0, None, None, None)
    return _Common(
        arch, xml, arch_sha, commit, python, repo,
        k=qor_legs.lut_size(req.vtr_root, python, xml),
        odin3_build=odin3_build_id(req.odin3),
        lib_sha256=_sha_or(repo / qor_legs.LIB_REL, None),
        abc_script_sha256=_sha_or(repo / qor_legs.ABC_SCRIPT_REL, None),
    )


def _key_inputs(leg: str, design: str, source_sha: str, req: PlanRequest,
                com: _Common) -> KeyInputs:
    odin3 = leg != "P"
    return KeyInputs(
        leg=leg, design=design, arch_base=com.arch, arch_sha256=com.arch_sha256,
        source_sha256=source_sha, vtr_commit=com.vtr_commit,
        params=tuple(qor_legs.key_params(req.seed)),
        odin3_build=com.odin3_build if odin3 else None,
        script=qor_legs.key_script(leg, com.k, qor_legs.leg_name_style(leg)) if odin3 else None,
        lib_sha256=com.lib_sha256 if odin3 else None,
        abc_script_sha256=com.abc_script_sha256 if odin3 else None,
    )


def is_cached(job_dir: Path, key: str) -> bool:
    """True when ``<job_dir>/metrics.json`` holds ``status ok`` under this job key."""
    try:
        metrics = json.loads((job_dir / "metrics.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return isinstance(metrics, dict) and metrics.get("status") == "ok" \
        and metrics.get("key") == key


def _source_sha256(path: Path, com: _Common, *, may_miss: bool) -> str:
    """A design source's SHA-256, hashed once per plan (a golden is read by Pprime and R, a
    ``.v`` by up to four legs). A missing golden is ``GOLDEN_MISSING``."""
    if path not in com.hashes:
        if may_miss and not path.is_file():
            return GOLDEN_MISSING
        com.hashes[path] = file_sha256(path)
    return com.hashes[path]


def _plan_job(leg: str, design: str, req: PlanRequest, sets: Sets, com: _Common) -> Job:
    golden = qor_legs.golden_blif(com.arch, design) if leg in GOLDEN_LEGS else None
    if golden is not None:
        source_sha = _source_sha256(golden, com, may_miss=True)
    else:
        source_sha = _source_sha256(qor_legs.design_verilog(req.vtr_root, design), com,
                                    may_miss=False)
    inputs = _key_inputs(leg, design, source_sha, req, com)
    key, key12 = job_key(inputs), leg_key(inputs)[:KEY12]
    job_dir = req.out / com.vtr_commit[:KEY12] / com.arch / leg / key12 / design
    group = sets.designs[design].group
    ctx = LegContext(
        vtr_root=req.vtr_root, python=com.python, arch_xml=com.arch_xml, odin3=req.odin3,
        repo=com.repo, job_dir=job_dir, k=com.k, group=group, seed=req.seed, golden=golden,
    )
    return Job(
        leg=leg, design=design, arch=com.arch, group=group,
        gating=com.arch == qor_legs.ARCH_BASES["k6"] and design in sets.gate,
        key=key, key12=key12, job_dir=job_dir,
        commands=tuple(qor_legs.build_commands(leg, design, ctx)),
        cached=not req.rerun and is_cached(job_dir, key),
    )


def plan_jobs(req: PlanRequest, sets: Sets) -> list[Job]:
    """Every job of a run, in run order: designs in §2 order (group A, then group B, smallest
    first), each design's legs in ``PLAN_LEG_ORDER``. Unknown legs or designs: E-QH7."""
    legs = _ordered_legs(req.legs)
    _check_designs(sets, req.designs)
    designs = _in_order(sets, req.designs)
    if not legs or not designs:
        return []
    com = _common(req, legs)
    return [_plan_job(leg, design, req, sets, com) for design in designs for leg in legs]


# -- the runner (plan Task 5; spec §7.2-7.6) ---------------------------------------------------

# The scheduler caps (plan, Global Constraints; spec §11 as made exact by point (w)).
MIN_MEM_AVAILABLE_GIB = 7
MIN_DISK_FREE_GIB = 20
DISK_FLOOR_GIB = 10
PAUSE_LIMIT_S = 7200
POLL_S = 60
JOB_TIMEOUT_S: Mapping[str, int] = MappingProxyType({"A": 14400, "B": 21600})
KILL_GRACE_S = 30.0
CAPPED_RETRIES = 0
KEEP_KEYS = 3
KEEP_FAILED = 5
KEEP_FAILED_BYTES = 2 * GIB
# Verilator's -j for each group's fcheck (decision 16; plan Task 11 runs it).
FCHECK_J = MappingProxyType({"A": 2, "B": 1})

# Spec §7.5: what an ok job keeps of <J>/run (names and globs).
KEEP_RUN_FILES = (
    "vtr_flow.out", "output.txt", "parse_results.txt", "qor_results.txt", "vpr.out",
    "vpr.crit_path.out", "parmys.out", "abc*.out", "*_blackboxing_latch.out",
    "restore_latch*.out", "report_clocks.abc.out",
)
# Point (k): the stage logs E-QH10 names (the newest by mtime), in <J>/run and in <J>.
STAGE_LOGS_RUN = ("parmys.out", "abc*.out", "*_blackboxing_latch.out", "restore_latch*.out",
                  "vpr*.out")
STAGE_LOGS_JOB = ("frontend.log", "fcheck.log")
FRONTEND_LOG = "frontend.log"
VTR_FLOW_SCRIPT = "run_vtr_flow.py"

OK, FAILED, CAPPED, TIMEOUT, SKIPPED, ABORTED = (
    "ok", "failed", "capped", "timeout", "skipped", "aborted")
STATUSES = (OK, FAILED, CAPPED, TIMEOUT, SKIPPED, ABORTED)
# Statuses whose job keeps <J>/run whole for diagnosis (spec §5.4, §7.5).
KEPT_FOR_DIAGNOSIS = frozenset({FAILED, CAPPED, TIMEOUT, ABORTED})
GO, WAIT, STOP = "go", "wait", "stop"
EXIT_OK, EXIT_FAILED, EXIT_USAGE, EXIT_INCOMPLETE, EXIT_DISK = 0, 1, 2, 3, 5
# <out>/<vtr12>/<arch>/<leg>/<key12>/<d>: the job directory is this deep under --out.
JOB_DEPTH = 5
# How a running command is watched: exit checks back off from WAIT_FIRST_S to WAIT_MAX_S.
WAIT_FIRST_S = 0.01
WAIT_MAX_S = 1.0
GRACE_POLL_S = 0.05
# tools/heavy-run's lock descriptor, passed on to every job so the lock outlives no job.
LOCK_FD = 9
PROC_SELF_FD = Path("/proc/self/fd")
SECONDS_PER_MINUTE = 60
SECONDS_PER_HOUR = 3600
MEM_DECIMALS = 10
HINT_COMPACT = ("free C: by compacting the WSL disk (wsl --shutdown, then Optimize-VHD or "
                "diskpart compact vdisk; Peter's hands)")
# Signals that become SystemExit while jobs run, so the job's process group is killed first.
EXIT_SIGNALS = (signal.SIGTERM, signal.SIGHUP)
_DEFERRED_SIGNALS = frozenset({signal.SIGTERM, signal.SIGHUP, signal.SIGINT})


@dataclass(frozen=True)
class JobResult:
    """How one job ended: ``status`` is one of ``STATUSES``; ``error`` is the message (without
    the ``vtr-qor: `` prefix), empty for ok."""

    job: Job
    status: str
    elapsed_s: float
    error: str


@dataclass(frozen=True)
class RunSummary:
    results: tuple[JobResult, ...]
    stopped_for_disk: bool
    exit_code: int


@dataclass(frozen=True)
class CommandOutcome:
    """One command's end: ``kind`` is ``exit`` (with ``returncode``), ``timeout``, ``aborted``
    (the disk floor, with ``error``) or ``error`` (it could not start, with ``error``)."""

    kind: str
    returncode: int | None
    error: str


class HarnessLog:
    """Spec §7.6: appends each line to ``<out>/harness.log`` and echoes it to stderr."""

    def __init__(self, out: Path) -> None:
        out.mkdir(parents=True, exist_ok=True)
        self.path = out / "harness.log"

    def emit(self, message: str) -> None:
        line = f"{PREFIX}{message}"
        print(line, file=sys.stderr, flush=True)
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")

    def job(self, i: int, n: int, result: JobResult) -> None:
        """``[<i>/<n>] <leg> <design> <status> <elapsed> s (<group>, j1)``: one worker."""
        job = result.job
        self.emit(f"[{i}/{n}] {job.leg} {job.design} {result.status} "
                  f"{result.elapsed_s:.1f} s ({job.group}, j{MAX_JOBS})")

    def done(self, results: Sequence[JobResult], elapsed_s: float) -> None:
        """The run's last line. ``aborted`` jobs (a disk-floor stop) are added only when there
        are any, so a normal run's line is exactly the spec's."""
        count = {status: sum(r.status == status for r in results) for status in STATUSES}
        aborted = f", {count[ABORTED]} aborted" if count[ABORTED] else ""
        self.emit(f"done: {count[OK]} ok, {count[FAILED]} failed, {count[CAPPED]} capped, "
                  f"{count[TIMEOUT]} timeout, {count[SKIPPED]} skipped{aborted} "
                  f"in {hms(elapsed_s)}")


def hms(seconds: float) -> str:
    """``h:mm:ss``, rounded to the second."""
    total = max(0, round(seconds))
    hours, rest = divmod(total, SECONDS_PER_HOUR)
    minutes, secs = divmod(rest, SECONDS_PER_MINUTE)
    return f"{hours}:{minutes:02d}:{secs:02d}"


@dataclass
class _Admission:
    """Admission state across polls: when ``C:`` went short, and whether memory is awaited."""

    paused_since: float | None = None
    mem_waiting: bool = False


def _environ() -> Mapping[str, str]:
    return os.environ


@dataclass(kw_only=True)
class RunOptions:
    """How ``run_jobs`` runs. ``scope`` is what ``reexec_under_lock`` returned; ``clock`` and
    ``sleep`` drive admission and the job timeouts; ``env`` is the jobs' base environment;
    ``deadline`` and ``expected_s`` gate the nightly's starts (plan Task 13)."""

    jobs: int = 1
    job_timeout_s: Mapping[str, int] = JOB_TIMEOUT_S
    keep_netlists: frozenset[str] = frozenset()
    fcheck: bool = False
    deadline: float | None = None
    scope: ScopeInfo | None = None
    clock: Callable[[], float] = time.time
    sleep: Callable[[float], None] = time.sleep
    env: Mapping[str, str] = field(default_factory=_environ)
    meminfo: Path = Path("/proc/meminfo")
    out: Path
    expected_s: Callable[[Job], float] | None = None
    admission: _Admission = field(default_factory=_Admission, init=False, repr=False,
                                  compare=False)
    log: HarnessLog | None = field(default=None, init=False, repr=False, compare=False)


Finish = Callable[[Job, float], dict[str, object]]


def _emit(opts: RunOptions, message: str) -> None:
    if opts.log is not None:
        opts.log.emit(message)
    else:
        print(f"{PREFIX}{message}", file=sys.stderr, flush=True)


def _label(job: Job) -> str:
    return f"{job.leg} {job.design}"


# -- the heavy-run guard -----------------------------------------------------------------------


def _heavy_scope(opts: RunOptions) -> ScopeInfo:
    """The verified cap, from the real nesting evidence (``qor_heavy.require_heavy``), never
    from the environment alone. ``opts.scope`` must be set (by ``reexec_under_lock``); its cap
    says whether this is an ``--xl`` run."""
    if opts.scope is None:
        raise HeavyError(EXIT_USAGE, MSG_NOT_INSIDE)
    return qor_heavy.require_heavy(xl=opts.scope.memory_max > CAP_BYTES)


def _check_jobs_option(opts: RunOptions) -> None:
    """E-QH9 (point (w)): one VTR or Verilator job at a time."""
    if opts.jobs > MAX_JOBS:
        raise HeavyError(EXIT_USAGE, f"-j {opts.jobs} exceeds {MAX_JOBS} (the heavy-run rule)")
    if opts.jobs < 1:
        raise HeavyError(EXIT_USAGE, f"-j {opts.jobs} is not a job count")


def job_out_root(job: Job) -> Path:
    """The ``--out`` holding the job: every deletion of the job's files is confined to it."""
    return job.job_dir.parents[JOB_DEPTH - 1]


def _check_layout(jobs: Sequence[Job], opts: RunOptions) -> None:
    """Every job directory is ``JOB_DEPTH`` below ``opts.out`` (deletions are confined to it),
    and every job's group is known and has a wall timeout."""
    for job in jobs:
        if len(job.job_dir.parents) < JOB_DEPTH or job_out_root(job) != opts.out:
            raise HeavyError(EXIT_USAGE, f"{job.job_dir}: not a job directory under {opts.out}")
        if job.group not in GROUPS or job.group not in opts.job_timeout_s:
            raise HeavyError(EXIT_USAGE, f"{_label(job)}: unknown group '{job.group}'")


# -- files -------------------------------------------------------------------------------------


def _write_atomic(path: Path, text: str) -> None:
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def _write_status(job: Job, status: str) -> None:
    """``<J>/status``: one word (spec §5.2)."""
    _write_atomic(job.job_dir / "status", f"{status}\n")


def fresh_attempt(job: Job) -> float:
    """Start an attempt from nothing (spec §7.4; the Task 2 handoff): the whole job directory
    (``metrics.json``, ``status``, ``run/``, the netlists, ``job.json``) goes through
    ``safe_rmtree``, and ``<J>/run`` is recreated. Returns the attempt's start time on the file
    system's clock (no later than the new ``run/``'s mtime), the bound below which a log or parse
    file is stale."""
    qor_heavy.safe_rmtree(job.job_dir, job_out_root(job))
    run = job.job_dir / "run"
    run.mkdir(parents=True)
    return min(time.time(), run.stat().st_mtime)


def job_complete(job: Job) -> bool:
    """True when the job's ``metrics.json`` is ``ok`` under the planned job key (never an older
    key's ``ok`` left in the same ``<key12>`` directory)."""
    return is_cached(job.job_dir, job.key)


# -- running one command -----------------------------------------------------------------------


def _job_env(cmd: Command, opts: RunOptions) -> dict[str, str]:
    """The base environment, the command's overrides, and no bytecode written into VTR's
    read-only tree."""
    env = dict(opts.env)
    env.update(dict(cmd.env))
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def _lock_fds() -> tuple[int, ...]:
    """Descriptor 9 when it really is the heavy lock (as ``tools/heavy-run`` passes it): a job
    that holds it keeps the lock taken even if this process dies first."""
    try:
        target = os.readlink(PROC_SELF_FD / str(LOCK_FD))
    except OSError:
        return ()
    if os.path.realpath(target) != os.path.realpath(qor_heavy.HEAVY_LOCK):
        return ()
    return (LOCK_FD,)


def _start(cmd: Command, opts: RunOptions) -> subprocess.Popen[bytes]:
    """Start a command in its own session, stdout to ``cmd.stdout`` and stderr to
    ``cmd.stderr`` (or with stdout when unset), both written afresh."""
    with ExitStack() as stack:
        out = stack.enter_context(cmd.stdout.open("wb"))
        err: int | IO[bytes] = subprocess.STDOUT
        if cmd.stderr is not None:
            err = stack.enter_context(cmd.stderr.open("wb"))
        return subprocess.Popen(
            list(cmd.argv), cwd=cmd.cwd, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
            env=_job_env(cmd, opts), start_new_session=True, pass_fds=_lock_fds(),
        )


def _exited(pid: int) -> bool:
    """True once the child has exited; it is not reaped, so its process group id stays
    reserved until ``_end_group`` has signalled the group."""
    return os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT) is not None


def _signal_group(pgid: int, sig: signal.Signals) -> None:
    with suppress(ProcessLookupError):  # the group has already gone
        os.killpg(pgid, sig)


def _end_group(proc: subprocess.Popen[bytes], *, terminate: bool) -> int:
    """End the command's process group and reap its leader. With ``terminate``, SIGTERM first
    and SIGKILL after ``KILL_GRACE_S`` (spec §7.4); the group always gets SIGKILL at the end, so
    no process of this job survives into the next one (one job at a time). Termination signals
    are held until it is done."""
    held = signal.pthread_sigmask(signal.SIG_BLOCK, _DEFERRED_SIGNALS)
    try:
        if terminate:
            _signal_group(proc.pid, signal.SIGTERM)
            limit = time.monotonic() + KILL_GRACE_S
            while not _exited(proc.pid) and time.monotonic() < limit:
                time.sleep(GRACE_POLL_S)
        _signal_group(proc.pid, signal.SIGKILL)
        return proc.wait()
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, held)


def floor_message(running: int) -> str:
    """E-QH11's floor text."""
    return f"C: fell below {DISK_FLOOR_GIB} GiB free; terminating {running} running jobs"


def _watch(proc: subprocess.Popen[bytes], opts: RunOptions, deadline: float) -> str:
    """Wait for the command: ``exit``, or ``timeout`` at ``deadline``, or ``aborted`` when a
    check every ``POLL_S`` finds ``C:`` below the floor."""
    pause = WAIT_FIRST_S
    next_disk = opts.clock() + POLL_S
    while not _exited(proc.pid):
        now = opts.clock()
        if now >= deadline:
            return TIMEOUT
        if now >= next_disk:
            next_disk = now + POLL_S
            if admit(opts, running=1) == STOP:
                return ABORTED
        time.sleep(pause)
        pause = min(pause * 2, WAIT_MAX_S)
    return "exit"


def run_command(cmd: Command, opts: RunOptions, *, deadline: float) -> CommandOutcome:
    """Run one command inside the verified heavy run (refused outside), under ``deadline`` (a
    time on ``opts.clock``) and the disk floor; the whole process group ends with it."""
    _heavy_scope(opts)
    try:
        proc = _start(cmd, opts)
    except OSError as exc:
        return CommandOutcome("error", None, f"cannot run {cmd.argv[0]}: {exc.strerror or exc}")
    try:
        kind = _watch(proc, opts, deadline)
    except HeavyError as exc:  # C: unreadable while the job runs: stop it as at the floor
        _end_group(proc, terminate=True)
        return CommandOutcome(ABORTED, None, exc.message)
    except BaseException:
        _end_group(proc, terminate=True)
        raise
    returncode = _end_group(proc, terminate=kind != "exit")
    if kind == ABORTED:
        return CommandOutcome(ABORTED, None, floor_message(1))
    return CommandOutcome(kind, returncode if kind == "exit" else None, "")


# -- running one job ---------------------------------------------------------------------------


def _newest_stage(job_dir: Path) -> str | None:
    """Point (k): the newest stage log by mtime, or None."""
    found: list[tuple[float, str]] = []
    for directory, globs in ((job_dir / "run", STAGE_LOGS_RUN), (job_dir, STAGE_LOGS_JOB)):
        for pattern in globs:
            for path in directory.glob(pattern):
                try:
                    found.append((path.lstat().st_mtime, path.name))
                except OSError:
                    continue
    return max(found)[1] if found else None


def _capped_message(job: Job, scope: ScopeInfo) -> str:
    """E-QH10 (point (k)): the stage and the job's leaf-scope peak, else ``?``."""
    stage = _newest_stage(job.job_dir) or "?"
    peak = qor_heavy.scope_peak_mib(scope)
    shown = "?" if peak is None else str(peak)
    return (f"{_label(job)}: capped: stage {stage} exceeded the {scope.memory_max // GIB} GiB "
            f"scope (peak ≥ {shown} MiB at the kill)")


def _ended(returncode: int) -> str:
    return f"exited {returncode}" if returncode >= 0 else f"was killed by signal {-returncode}"


def _failure_message(job: Job, cmd: Command, returncode: int) -> str:
    """E-QH8's front-end text, or the same shape for VTR and any other command."""
    if cmd.stdout == job.job_dir / FRONTEND_LOG:
        what = "front end"
    elif len(cmd.argv) > 1 and Path(cmd.argv[1]).name == VTR_FLOW_SCRIPT:
        what = "VTR"
    else:
        what = Path(cmd.argv[0]).name
    return f"{_label(job)}: {what} {_ended(returncode)}; see {cmd.stdout}"


def _classify(job: Job, cmd: Command, outcome: CommandOutcome, opts: RunOptions,
              scope: ScopeInfo, oom_before: int) -> tuple[str, str]:
    """A command's outcome as a job status and message (spec §7.4): a failure while the scope's
    ``oom_kill`` counter rose is ``capped``."""
    if outcome.kind == TIMEOUT:
        return TIMEOUT, f"{_label(job)}: timeout after {opts.job_timeout_s[job.group]} s"
    if outcome.kind == ABORTED:
        return ABORTED, outcome.error
    if outcome.kind == "error" or outcome.returncode is None:
        return FAILED, f"{_label(job)}: {outcome.error}"
    if outcome.returncode == 0:
        return OK, ""
    if qor_heavy.oom_kills(scope) > oom_before:
        return CAPPED, _capped_message(job, scope)
    return FAILED, _failure_message(job, cmd, outcome.returncode)


def _vpr_status(metrics: Mapping[str, object]) -> str | None:
    routed = metrics.get("routed")
    status = routed.get("vpr_status") if isinstance(routed, Mapping) else None
    return status if isinstance(status, str) else None


def _finish(job: Job, finish: Finish, attempt_start: float
            ) -> tuple[str, str, dict[str, object] | None]:
    """Call ``finish``; a parse or metrics failure fails the job (E-QH8), and so does any
    ``vpr_status`` other than ``success``."""
    try:
        metrics = finish(job, attempt_start)
        json.dumps(metrics)  # metrics.json must be writable
    except HeavyError as exc:
        return FAILED, exc.message, None
    except Exception as exc:  # a broken finish must not stop an unattended run
        return FAILED, f"{_label(job)}: cannot finish the job: {exc}", None
    status = _vpr_status(metrics)
    if status != "success":
        log = job.job_dir / "run" / "vtr_flow.out"
        return FAILED, f"{_label(job)}: vpr_status={status or 'missing'}; see {log}", metrics
    return OK, "", metrics


def _record(job: Job, status: str, error: str, metrics: dict[str, object] | None,
            elapsed_s: float) -> None:
    """``metrics.json`` (with the planned key and this attempt's status) when there are metrics;
    otherwise none may survive, so a failed attempt is never read as ``ok``."""
    path = job.job_dir / "metrics.json"
    if metrics is None:
        qor_heavy.safe_rmtree(path, job_out_root(job))
    else:
        record = dict(metrics)
        record.update(key=job.key, status=status, error=error, elapsed_s=round(elapsed_s, 1))
        _write_atomic(path, json.dumps(record, sort_keys=True, indent=1) + "\n")
    _write_status(job, status)


def run_one(job: Job, opts: RunOptions, finish: Finish) -> JobResult:
    """Run one job alone from a fresh attempt, inside the verified heavy run (spec §7.4)."""
    scope = _heavy_scope(opts)
    began = opts.clock()
    deadline = began + opts.job_timeout_s[job.group]
    attempt_start = fresh_attempt(job)
    oom_before = qor_heavy.oom_kills(scope)
    for cmd in job.commands:
        outcome = run_command(cmd, opts, deadline=deadline)
        status, error = _classify(job, cmd, outcome, opts, scope, oom_before)
        if status != OK:
            elapsed = opts.clock() - began
            _record(job, status, error, None, elapsed)
            return JobResult(job, status, elapsed, error)
    status, error, metrics = _finish(job, finish, attempt_start)
    elapsed = opts.clock() - began
    _record(job, status, error, metrics, elapsed)
    return JobResult(job, status, elapsed, error)


# -- admission ---------------------------------------------------------------------------------


def admit(opts: RunOptions, *, running: int) -> str:
    """Spec §7.3. With jobs running, only the floor: ``stop`` below ``DISK_FLOOR_GIB``. Before
    a job: ``wait`` while ``C:`` is under ``MIN_DISK_FREE_GIB`` (``stop`` once that has lasted
    ``PAUSE_LIMIT_S``) or ``MemAvailable`` under ``MIN_MEM_AVAILABLE_GIB``, else ``go``. Prints
    the paused, resumed and waiting lines once per episode. HeavyError when a reader fails."""
    free = qor_heavy.disk_free_gib()
    if running > 0:
        return STOP if free < DISK_FLOOR_GIB else GO
    state = opts.admission
    if free < MIN_DISK_FREE_GIB:
        now = opts.clock()
        if state.paused_since is None:
            state.paused_since = now
            _emit(opts, f"paused: C: has {free} GiB free (< {MIN_DISK_FREE_GIB} GiB); "
                        f"checking every {POLL_S} s")
        elif now - state.paused_since >= PAUSE_LIMIT_S:
            return STOP
        return WAIT
    if state.paused_since is not None:
        state.paused_since = None
        _emit(opts, f"resumed: C: has {free} GiB free")
    mem = qor_heavy.mem_available_gib(opts.meminfo)
    if mem < MIN_MEM_AVAILABLE_GIB:
        if not state.mem_waiting:
            state.mem_waiting = True
            shown = math.floor(mem * MEM_DECIMALS) / MEM_DECIMALS
            _emit(opts, f"waiting: {shown:.1f} GiB available (< {MIN_MEM_AVAILABLE_GIB})")
        return WAIT
    state.mem_waiting = False
    return GO


def _fits_deadline(job: Job, opts: RunOptions) -> bool:
    if opts.deadline is None:
        return True
    expected = opts.expected_s(job) if opts.expected_s is not None else 0.0
    return opts.clock() + expected <= opts.deadline


def _deadline_line(job: Job, opts: RunOptions) -> str:
    """E-QH10's nightly text (plan Task 13 owns the nightly)."""
    expected = opts.expected_s(job) if opts.expected_s is not None else 0.0
    minutes = math.ceil(expected / SECONDS_PER_MINUTE)
    when = time.strftime("%H:%M", time.localtime(opts.deadline or 0.0))
    return f"nightly: {_label(job)} not started: expected {minutes} min would pass {when}"


def _admit_loop(job: Job, opts: RunOptions, *, deadline: bool) -> str:
    """Poll ``admit`` every ``POLL_S`` until ``go`` or ``stop``; with ``deadline``, ``skipped``
    once the job no longer fits before ``opts.deadline``."""
    while True:
        if deadline and not _fits_deadline(job, opts):
            return SKIPPED
        verdict = admit(opts, running=0)
        if verdict != WAIT:
            return verdict
        opts.sleep(POLL_S)


# -- cleanup and retention ---------------------------------------------------------------------


def _kept(name: str) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in KEEP_RUN_FILES)


def _gzip(src: Path, dst: Path) -> None:
    """A reproducible gzip (no name, mtime 0), written by atomic rename."""
    tmp = dst.with_name(f".{dst.name}.tmp")
    with src.open("rb") as fin, tmp.open("wb") as raw, \
            gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as fout:
        shutil.copyfileobj(fin, fout, _CHUNK)
    os.replace(tmp, dst)


def _regular(path: Path) -> bool:
    return path.is_file() and not path.is_symlink()


def cleanup_job(job: Job, result: JobResult, keep_netlists: frozenset[str]) -> None:
    """Spec §7.5, after an ``ok`` job (any other status keeps ``run/`` whole): ``<J>/run`` keeps
    ``KEEP_RUN_FILES``; P always keeps ``<d>.pre-vpr.blif`` gzipped; the other legs lose the
    pre-VPR netlist and keep ``<J>/<d>.blif`` only as its ``.sha256``, unless the leg is in
    ``keep_netlists``. Deletions go through ``safe_rmtree`` (E-QH12)."""
    if result.status != OK:
        return
    root = job_out_root(job)
    run = job.job_dir / "run"
    pre = run / f"{job.design}.pre-vpr.blif"
    keep = {pre.name} if job.leg != "P" and job.leg in keep_netlists else set()
    if job.leg == "P" and _regular(pre):
        _gzip(pre, pre.with_name(pre.name + ".gz"))
        keep.add(pre.name + ".gz")
    if run.is_dir():
        for entry in sorted(run.iterdir()):
            if entry.name not in keep and not _kept(entry.name):
                qor_heavy.safe_rmtree(entry, root)
    blif = job.job_dir / f"{job.design}.blif"
    if job.leg != "P" and job.leg not in keep_netlists and _regular(blif):
        _write_atomic(blif.with_name(blif.name + ".sha256"), file_sha256(blif) + "\n")
        qor_heavy.safe_rmtree(blif, root)


@dataclass(frozen=True)
class _JobDir:
    path: Path
    status: str | None
    metrics_ok: bool
    when: float


def _symlink_refusal(path: Path) -> HeavyError:
    return HeavyError(EXIT_USAGE, f"refusing to delete {path}: {path} is a symlink")


def _entries(directory: Path) -> list[Path]:
    """The entries of a directory, refusing any symlink rather than following it (E-QH12)."""
    entries = sorted(directory.iterdir())
    for entry in entries:
        if entry.is_symlink():
            raise _symlink_refusal(entry)
    return entries


def _read_word(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return None


def _job_dir_info(path: Path) -> _JobDir:
    for name in ("status", "metrics.json", "run"):
        if (path / name).is_symlink():
            raise _symlink_refusal(path / name)
    status_file = path / "status"
    try:
        when = status_file.lstat().st_mtime
    except OSError:
        when = path.lstat().st_mtime
    try:
        metrics = json.loads((path / "metrics.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        metrics = None
    metrics_ok = isinstance(metrics, dict) and metrics.get("status") == OK
    return _JobDir(path, _read_word(status_file), metrics_ok, when)


def _tree_bytes(path: Path) -> int:
    """The apparent size of a tree (``st_size``, symlinks not followed): sparse files count in
    full, so the cap errs towards pruning."""
    total = 0
    for top, dirs, files in os.walk(path, followlinks=False):
        for name in (*dirs, *files):
            total += os.lstat(Path(top) / name).st_size
    return total


def _scan_leg(leg_dir: Path) -> dict[Path, list[_JobDir]]:
    keys: dict[Path, list[_JobDir]] = {}
    for key_dir in _entries(leg_dir):
        if key_dir.is_dir():
            keys[key_dir] = [_job_dir_info(d) for d in _entries(key_dir) if d.is_dir()]
    return keys


def _prune_keys(keys: Mapping[Path, list[_JobDir]], latest: str | None, root: Path,
                keep_keys: int) -> None:
    """Spec §5.4: the ``keep_keys`` newest complete keys stay (and ``latest`` always)."""
    complete = [(max(j.when for j in jobs), key.name, key) for key, jobs in keys.items()
                if jobs and all(j.status == OK and j.metrics_ok for j in jobs)]
    complete.sort(reverse=True)
    for _, name, key in complete[keep_keys:]:
        if name != latest:
            qor_heavy.safe_rmtree(key, root)


def _prune_failed(keys: Mapping[Path, list[_JobDir]], root: Path) -> None:
    """Spec §5.4: at most ``KEEP_FAILED`` failed run directories and ``KEEP_FAILED_BYTES`` in
    all per leg; the oldest lose ``run/`` first (their ``status`` stays)."""
    failed = sorted((j.when, str(j.path), j.path / "run") for jobs in keys.values()
                    for j in jobs if j.status in KEPT_FOR_DIAGNOSIS)
    runs = [(run, _tree_bytes(run)) for _, _, run in failed if run.is_dir()]
    count, total = len(runs), sum(size for _, size in runs)
    for run, size in runs:
        if count <= KEEP_FAILED and total <= KEEP_FAILED_BYTES:
            break
        qor_heavy.safe_rmtree(run, root)
        count -= 1
        total -= size


def prune(out: Path, vtr12: str, arch: str, leg: str, *, keep_keys: int = KEEP_KEYS) -> None:
    """Retention for ``<out>/<vtr12>/<arch>/<leg>`` (spec §5.4). A symlink anywhere on the way
    is refused (E-QH12), never followed; deletions go through ``safe_rmtree`` under ``out``."""
    leg_dir = out / vtr12 / arch / leg
    current = out
    for part in (vtr12, arch, leg):
        current = current / part
        if current.is_symlink():
            raise _symlink_refusal(current)
    if not leg_dir.is_dir():
        return
    if (leg_dir / "latest").is_symlink():
        raise _symlink_refusal(leg_dir / "latest")
    keys = _scan_leg(leg_dir)
    _prune_keys(keys, _read_word(leg_dir / "latest"), out, keep_keys)
    remaining = {key: jobs for key, jobs in keys.items() if key.exists()}
    _prune_failed(remaining, out)


def _prune_after(jobs: Sequence[Job], opts: RunOptions) -> None:
    seen: set[Path] = set()
    for job in jobs:
        leg_dir = job.job_dir.parents[1]
        if leg_dir in seen:
            continue
        seen.add(leg_dir)
        try:
            prune(opts.out, leg_dir.parents[1].name, leg_dir.parent.name, leg_dir.name)
        except (HeavyError, OSError) as exc:
            _emit(opts, exc.message if isinstance(exc, HeavyError) else f"prune: {exc}")


# -- the run -----------------------------------------------------------------------------------


def run_fcheck(job: Job, opts: RunOptions, verilator_j: int) -> str:
    """The Verilator random check of one finished job (plan Task 11 implements it here); until
    then every verdict is ``skipped``."""
    return SKIPPED


def _grouped(jobs: Sequence[Job]) -> list[Job]:
    """Group A then group B, each in plan order."""
    return [job for group in GROUPS for job in jobs if job.group == group]


@contextmanager
def _signals_as_exit() -> Iterator[None]:
    """SIGTERM and SIGHUP raise SystemExit while jobs run, so the running job's group is killed
    before this process goes (the main thread only; elsewhere the handlers stay)."""
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    previous = {sig: signal.signal(sig, _raise_exit) for sig in EXIT_SIGNALS}
    try:
        yield
    finally:
        for sig, handler in previous.items():
            if handler is not None:
                signal.signal(sig, handler)


def _raise_exit(signum: int, frame: FrameType | None) -> NoReturn:
    raise SystemExit(128 + signum)


@dataclass
class _Run:
    """One ``run_jobs`` call's progress."""

    jobs: list[Job]
    opts: RunOptions
    finish: Finish
    log: HarnessLog
    results: list[JobResult] = field(default_factory=list)
    stop_code: int = EXIT_OK

    def stop(self, code: int, start: int) -> None:
        """Stop with ``code``; the jobs from index ``start`` on are not started."""
        self.stop_code = code
        for job in self.jobs[start:]:
            cached = job.cached and job_complete(job)
            self.results.append(JobResult(job, OK if cached else SKIPPED, 0.0,
                                          "" if cached else "not started"))

    def not_started(self, start: int) -> int:
        return sum(not (job.cached and job_complete(job)) for job in self.jobs[start:])


def _after_ok(run: _Run, job: Job, result: JobResult) -> bool:
    """Cleanup, then the job's own fcheck before the next job; False when admission stopped the
    run."""
    try:
        cleanup_job(job, result, run.opts.keep_netlists)
    except (HeavyError, OSError) as exc:
        run.log.emit(exc.message if isinstance(exc, HeavyError) else
                     f"{_label(job)}: cleanup failed: {exc}")
    if not run.opts.fcheck:
        return True
    if _admit_loop(job, run.opts, deadline=False) == STOP:
        return False
    run_fcheck(job, run.opts, FCHECK_J[job.group])
    return True


def _stop_for_pause(run: _Run, index: int) -> None:
    hours = PAUSE_LIMIT_S // SECONDS_PER_HOUR
    run.log.emit(f"C: stayed below {MIN_DISK_FREE_GIB} GiB free for {hours} h; stopping "
                 f"({run.not_started(index)} jobs not started)")
    run.log.emit(HINT_COMPACT)
    run.stop(EXIT_DISK, index)


def _step(run: _Run, index: int) -> bool:
    """Run ``run.jobs[index]``; False when the run stops."""
    job, n = run.jobs[index], len(run.jobs)
    if job.cached and job_complete(job):
        run.results.append(JobResult(job, OK, 0.0, ""))
        run.log.emit(f"[{index + 1}/{n}] {job.leg} {job.design} cached")
        return True
    gate = _admit_loop(job, run.opts, deadline=True)
    if gate == SKIPPED:
        run.log.emit(_deadline_line(job, run.opts))
        result = JobResult(job, SKIPPED, 0.0, "not started before the deadline")
        run.results.append(result)
        run.log.job(index + 1, n, result)
        return True
    if gate == STOP:
        _stop_for_pause(run, index)
        return False
    result = run_one(job, run.opts, run.finish)
    if result.error:
        run.log.emit(result.error)
    if result.status == CAPPED:
        run.log.emit(f"{_label(job)}: no verdict")
    run.results.append(result)
    run.log.job(index + 1, n, result)
    if result.status == ABORTED:
        run.stop(EXIT_DISK, index + 1)
        return False
    if result.status == OK and not _after_ok(run, job, result):
        _stop_for_pause(run, index + 1)
        return False
    return True


def _exit_code(results: Sequence[JobResult], stop_code: int) -> int:
    """Plan, Global Constraints: 5 stopped for disk (or the stopping error's code), 1 a failure,
    3 capped or timeout without a failure, else 0."""
    if stop_code != EXIT_OK:
        return stop_code
    statuses = {result.status for result in results}
    if FAILED in statuses or ABORTED in statuses:
        return EXIT_FAILED
    if CAPPED in statuses or TIMEOUT in statuses:
        return EXIT_INCOMPLETE
    return EXIT_OK


def run_jobs(jobs: Sequence[Job], opts: RunOptions, finish: Finish) -> RunSummary:
    """Run every job, one at a time (point (w)): group A then group B in plan order, each
    admitted first (spec §7.3), each fcheck right after its own job; retention after the run.
    Refused (HeavyError) above ``MAX_JOBS`` (E-QH9) or outside a verified heavy run.

    ``finish(job, attempt_start)`` builds the job's metrics (plan Task 6) once all its commands
    exited 0; its ``routed.vpr_status`` must be ``success``. The runner writes ``metrics.json``
    with the planned key and the status, and ``<J>/status``."""
    _check_jobs_option(opts)
    _check_layout(jobs, opts)
    ordered = _grouped(jobs)
    _heavy_scope(opts)
    log = HarnessLog(opts.out)
    opts.log = log
    began = opts.clock()
    run = _Run(ordered, opts, finish, log)
    with _signals_as_exit():
        index = 0
        while index < len(ordered):
            try:
                going = _step(run, index)
            except HeavyError as exc:  # a reader failed between jobs (E-QH11 or meminfo)
                log.emit(exc.message)
                run.stop(exc.code, len(run.results))
                going = False
            except OSError as exc:  # no job runs here: stop cleanly rather than crash
                log.emit(f"stopping: {exc}")
                run.stop(EXIT_USAGE, len(run.results))
                going = False
            if not going:
                break
            index += 1
    _prune_after(ordered, opts)
    log.done(run.results, opts.clock() - began)
    return RunSummary(tuple(run.results), run.stop_code == EXIT_DISK,
                      _exit_code(run.results, run.stop_code))
