"""qor_sched: vtr-qor's jobs, keys, cache and planner (3H spec QH-2, §5.3, §7.2; plan Task 2).

This is the planning half; plan Task 5 adds the runner.

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

No production path reads a test hook from the environment (controller ruling, 3H Task 1): tests
replace the module seams ``LDD_CMD`` here and ``REPO_ROOT``/``GOLDEN_ROOT`` in ``qor_legs``.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import re
import subprocess
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import qor_legs
from qor_heavy import HeavyError
from qor_legs import Command, LegContext

SCHEMA = 1
# One VTR or Verilator job at a time (ruling C1, point (w)).
MAX_JOBS = 1
PLAN_LEG_ORDER = ("P", "Pprime", "Oprime", "O", "R", "Oprov")
GOLDEN_LEGS = frozenset({"Pprime", "R"})
SET_NAMES = ("gate", "extra", "smoke")
GROUPS = ("A", "B")
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


def _names(data: Mapping[str, Any], field: str, path: Path) -> tuple[str, ...]:
    value = data.get(field)
    if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
        raise _bad(path, f"'{field}' is not a list of design names")
    if len(set(value)) != len(value):
        raise _bad(path, f"'{field}' repeats a design")
    return tuple(value)


def _design_info(name: str, raw: object, path: Path) -> DesignInfo:
    if not isinstance(raw, dict):
        raise _bad(path, f"design '{name}' is not an object")
    rank, group = raw.get("rank"), raw.get("group")
    size, secs = raw.get("golden_bytes"), raw.get("vtr_flow_time_s")
    if (rank is not None and (not isinstance(rank, int) or isinstance(rank, bool))) \
            or group not in GROUPS or not isinstance(size, int) or isinstance(size, bool) \
            or not isinstance(secs, (int, float)) or isinstance(secs, bool):
        raise _bad(path, f"design '{name}' needs rank, group (A|B), golden_bytes, vtr_flow_time_s")
    return DesignInfo(rank, str(group), size, float(secs))


def _check_sets(sets: Sets, path: Path) -> None:
    if set(sets.designs) != set(sets.order):
        raise _bad(path, "'designs' and 'order' name different designs")
    if set(sets.gate) & set(sets.extra) or set(sets.gate) | set(sets.extra) != set(sets.order):
        raise _bad(path, "'gate' and 'extra' must split 'order'")
    if not set(sets.smoke) <= set(sets.gate):
        raise _bad(path, "'smoke' must lie in 'gate'")
    in_b = {d for d, info in sets.designs.items() if info.group == "B"}
    if set(sets.group_b) != in_b:
        raise _bad(path, "'group_b' must be the designs of group B")
    sizes = [sets.designs[d].golden_bytes for d in sets.order]
    if sizes != sorted(sizes):
        raise _bad(path, "'order' is not by ascending golden size")


def load_sets(path: Path = SETS_JSON) -> Sets:
    """Read and check ``tests/vtr/qor/sets.json`` (spec §2, point (i)). Failure: HeavyError(2)."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise _bad(path, f"cannot read: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise _bad(path, f"not a schema {SCHEMA} sets file")
    raw = data.get("designs")
    if not isinstance(raw, dict):
        raise _bad(path, "'designs' is not an object")
    sets = Sets(
        order=_names(data, "order", path), gate=_names(data, "gate", path),
        extra=_names(data, "extra", path), smoke=_names(data, "smoke", path),
        group_b=_names(data, "group_b", path),
        designs={name: _design_info(name, info, path) for name, info in raw.items()},
    )
    _check_sets(sets, path)
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


def _ordered_legs(legs: Iterable[str]) -> tuple[str, ...]:
    wanted = {qor_legs.normalise_leg(leg) for leg in legs}
    return tuple(leg for leg in PLAN_LEG_ORDER if leg in wanted)


def _common(req: PlanRequest, legs: tuple[str, ...]) -> _Common:
    arch = qor_legs.arch_base(req.arch)
    xml = qor_legs.arch_xml(req.vtr_root, arch)
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


def _plan_job(leg: str, design: str, req: PlanRequest, sets: Sets, com: _Common) -> Job:
    golden = qor_legs.golden_blif(com.arch, design) if leg in GOLDEN_LEGS else None
    if golden is not None:
        source_sha = _sha_or(golden, GOLDEN_MISSING) or GOLDEN_MISSING
    else:
        source_sha = file_sha256(qor_legs.design_verilog(req.vtr_root, design))
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
