"""qor_nightly: the decisions of ``vtr-qor nightly`` (3H spec §13, plan Task 13, point (c)).

The nightly is started by the controller inside ``tools/heavy-run``; ``vtr_qor.cmd_nightly``
runs the steps in process. This module holds everything that can be decided and tested without a
VTR run:
- the deadline (the next 06:00 local, or the start plus ``--budget``) and a job's expected
  minutes (the last measured elapsed time of the same (arch, leg, design), else VTR's flow time
  x 1.5, plus the leg's last front-end time when it runs ``odin3``);
- the group B ``ROTATION`` of eight slots, its state file, the starvation guard and the
  sweep-night rules (3E sweeps when the BLIF writer or reader changed or on a Saturday, 3D sweeps
  when ``src/passes/abc`` or ``adapters/abc`` changed or on a Sunday; either skips step 4);
- the commands of the 3E and 3D sweeps (each started with descriptor 9 passed, so it is inside
  the held lock);
- the diff of two nightly tables (``summary.md``).
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
from collections.abc import Callable, Collection, Mapping, Sequence
from contextlib import suppress
from dataclasses import dataclass
from datetime import date, datetime, time, timedelta
from pathlib import Path

import qor_legs
import qor_sched
import qor_table
from qor_sched import Job, Sets
from qor_table import Run

DEADLINE_HOUR = 6
NIGHTLY_BUDGET_H = 5.0  # plan point (c) and Task 13: the default budget is 5 h
SECONDS_PER_MINUTE = 60.0
SECONDS_PER_HOUR = 3600.0
FLOW_TIME_FACTOR = 1.5  # VTR's runner times x 1.5 (spec §2)
SATURDAY, SUNDAY = 5, 6
STARVED_NIGHTS = 2
MOVE_LIMIT = 0.02
VERDICT_FIELDS = ("fcheck", "equiv")  # a changed verdict is listed like a moved metric
SWEEP_3E_PATHS = ("src/backends/blif", "src/frontends/blif")
SWEEP_3D_PATHS = ("src/passes/abc", "adapters/abc")
BIG_DESIGNS = frozenset({"mcml", "LU32PEEng"})
EQUIV_REQUIRED = qor_sched.EQUIV_REQUIRED
PRIME = "′"
STATE_FILE = "rotation.json"
GIT_TIMEOUT_S = 60
EXIT_DISK = qor_sched.EXIT_DISK


@dataclass(frozen=True)
class Slot:
    """One group B rotation slot: a design, its legs and the checks that follow."""

    number: int
    design: str
    legs: tuple[str, ...]
    fcheck: bool = False
    equiv: bool = False


# Spec §13 step 4 / plan Task 13: the eight (design, legs) slots of the 8-night cycle.
ROTATION = (
    Slot(1, "LU8PEEng", ("O", "Oprime", "Pprime"), fcheck=True),
    Slot(2, "bgm", ("O", "Oprime", "Pprime"), fcheck=True),  # no equiv: PHASE3 #16 (bgm is big)
    Slot(3, "mcml", ("O",)),
    Slot(4, "mcml", ("Oprime",)),
    Slot(5, "mcml", ("Pprime",)),
    Slot(6, "LU32PEEng", ("O",)),
    Slot(7, "LU32PEEng", ("Oprime",)),
    Slot(8, "LU32PEEng", ("Pprime",)),
)


def display_leg(leg: str) -> str:
    return {"Oprime": f"O{PRIME}", "Pprime": f"P{PRIME}"}.get(leg, leg)


def slot_text(slot: Slot) -> str:
    """``mcml O'``: the design and its legs as the summary names them."""
    return f"{slot.design} {' '.join(display_leg(leg) for leg in slot.legs)}"


# -- the deadline and the expected duration ----------------------------------------------------


def nightly_deadline(start: datetime, budget_h: float) -> datetime:
    """The earlier of the next 06:00 local after ``start`` and ``start`` plus ``budget_h``."""
    six = datetime.combine(start.date(), time(DEADLINE_HOUR), tzinfo=start.tzinfo)
    if six <= start:
        six += timedelta(days=1)
    return min(six, start + timedelta(hours=budget_h))


def _measured(history: Path, arch: str, leg: str, design: str, field: str) -> float | None:
    """The newest ``ok`` ``metrics.json`` of (arch, leg, design) under any key and VTR commit:
    ``elapsed_s`` (field ``elapsed_s``) or ``cost.frontend_wall_s``."""
    best: tuple[float, float] | None = None
    for path in history.glob(f"*/{arch}/{leg}/*/{design}/metrics.json"):
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
            when = path.stat().st_mtime
        except (OSError, ValueError):
            continue
        if not isinstance(record, dict) or record.get("status") != qor_sched.OK:
            continue
        value = record.get("cost", {}).get(field.split(".")[-1]) if "." in field \
            else record.get(field)
        if isinstance(value, (int, float)) and value > 0 and (best is None or when > best[0]):
            best = (when, float(value))
    return None if best is None else best[1]


def expected_minutes(job: Job, history: Path, sets: Sets) -> float:
    """The last measured ``elapsed_s`` of the same (arch, leg, design) under any key, in minutes;
    else ``vtr_flow_time_s`` x 1.5, plus the leg's last ``frontend_wall_s`` when the leg runs
    ``odin3`` (spec §13)."""
    elapsed = _measured(history, job.arch, job.leg, job.design, "elapsed_s")
    if elapsed is not None:
        return elapsed / SECONDS_PER_MINUTE
    seconds = sets.designs[job.design].vtr_flow_time_s * FLOW_TIME_FACTOR
    if job.leg != "P":
        seconds += _measured(history, job.arch, job.leg, job.design, "cost.frontend_wall_s") or 0
    return seconds / SECONDS_PER_MINUTE


# -- rotation state ----------------------------------------------------------------------------


def default_state() -> dict[str, object]:
    return {"next_slot": 1, "last_admitted": "", "starved": {}, "last_sweep_commit": "",
            "last_sweep_date": "", "last_3d_commit": ""}


def load_state(path: Path) -> dict[str, object]:
    """``rotation.json``, defaults filled in; an unreadable or malformed file is the default."""
    state = default_state()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return state
    if isinstance(data, dict):
        state.update({k: v for k, v in data.items() if k in state})
    slot = state["next_slot"]
    if not isinstance(slot, int) or not 1 <= slot <= len(ROTATION):
        state["next_slot"] = 1
    if not isinstance(state["starved"], dict):
        state["starved"] = {}
    return state


def save_state(path: Path, state: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(json.dumps(state, sort_keys=True, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def starved_count(state: Mapping[str, object], slot: Slot) -> int:
    starved = state["starved"]
    value = starved.get(str(slot.number), 0) if isinstance(starved, dict) else 0
    return value if isinstance(value, int) else 0


def settle_slot(state: dict[str, object], slot: Slot, *, ran: bool, today: date) -> None:
    """A slot advances only when it ran; a refused one counts a starved night."""
    starved = state["starved"]
    assert isinstance(starved, dict)
    if ran:
        state["next_slot"] = slot.number % len(ROTATION) + 1
        state["last_admitted"] = today.isoformat()
        starved.pop(str(slot.number), None)
    else:
        starved[str(slot.number)] = starved_count(state, slot) + 1


def starved_line(slot: Slot, nights: int) -> str:
    return (f"vtr-qor: nightly: group B slot {slot.number} ({slot_text(slot)}) "
            f"starved for {nights} nights")


def available_slot(slot: Slot, available: Collection[str]) -> Slot | None:
    """The slot restricted to the legs ``available``; None when none is."""
    legs = tuple(leg for leg in slot.legs if leg in available)
    return None if not legs else Slot(slot.number, slot.design, legs, slot.fcheck, slot.equiv)


# -- the night's plan --------------------------------------------------------------------------


def head_commit(repo: Path) -> str:
    proc = _git(repo, "rev-parse", "HEAD")
    return proc.stdout.strip() if proc is not None and proc.returncode == 0 else ""


def _git(repo: Path, *args: str) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                              check=False, timeout=GIT_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired):
        return None


def changed_since(repo: Path, commit: str, paths: Sequence[str]) -> bool:
    """True when ``git log <commit>..HEAD -- <paths>`` is non-empty. No recorded commit means
    nothing to compare with: False (the night records HEAD). A commit git cannot resolve: True."""
    if not commit:
        return False
    proc = _git(repo, "log", "--format=%H", f"{commit}..HEAD", "--", *paths)
    if proc is None or proc.returncode != 0:
        return True
    return bool(proc.stdout.strip())


@dataclass(frozen=True)
class NightPlan:
    """What tonight runs: the rotation slot (None on a sweep night), whether step 4 goes before
    step 3 (the slot was refused last night), the sweep steps, and the slot's starved nights."""

    slot: Slot | None
    starved_first: bool
    sweep_3e: bool
    sweep_3d: bool
    starved_nights: int


def plan_night(state: Mapping[str, object], repo: Path, today: date) -> NightPlan:
    """Step 4's slot and the sweep nights (spec §13 steps 4-6). A sweep night skips step 4 and
    leaves the rotation where it is."""
    sweep_3e = changed_since(repo, str(state["last_sweep_commit"]), SWEEP_3E_PATHS) \
        or today.weekday() == SATURDAY
    sweep_3d = changed_since(repo, str(state["last_3d_commit"]), SWEEP_3D_PATHS) \
        or today.weekday() == SUNDAY
    slot = ROTATION[int(str(state["next_slot"])) - 1]
    nights = starved_count(state, slot)
    if sweep_3e or sweep_3d:
        return NightPlan(None, False, sweep_3e, sweep_3d, nights)
    return NightPlan(slot, nights >= 1, False, False, nights)


# -- the sweeps (steps 5 and 6) ----------------------------------------------------------------

LOCK_FD = 9


TOOL_KILL_GRACE_S = 30.0
EXIT_TOOL_TIMEOUT = 3  # a tool stopped at the night's deadline is incomplete (exit 3)


def _stop_group(proc: subprocess.Popen[bytes]) -> None:
    """SIGTERM to the tool's process group, SIGKILL to it after ``TOOL_KILL_GRACE_S``."""
    for sig, wait in ((signal.SIGTERM, TOOL_KILL_GRACE_S), (signal.SIGKILL, TOOL_KILL_GRACE_S)):
        with suppress(ProcessLookupError, PermissionError):
            os.killpg(proc.pid, sig)
        try:
            proc.wait(timeout=wait)
            return
        except subprocess.TimeoutExpired:
            continue


def run_tool(argv: Sequence[str], *, cwd: Path | None = None,
             timeout_s: float | None = None) -> int:
    """A heavy child tool, started with descriptor 9 (the held lock) passed, so it is inside the
    run and does not re-execute (plan Task 13). It runs in its own process group; after
    ``timeout_s`` (the night's deadline, review H3) the group gets SIGTERM, then SIGKILL, and the
    result is ``EXIT_TOOL_TIMEOUT``."""
    fds = qor_sched.lock_fds()
    with subprocess.Popen(list(argv), cwd=cwd, stdin=subprocess.DEVNULL, pass_fds=fds,
                          start_new_session=True) as proc:
        try:
            return proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            _stop_group(proc)
            return EXIT_TOOL_TIMEOUT


def sweep_3e_commands(repo: Path, odin3: Path, vtr_root: Path, golden_k6: Path, work: Path
                      ) -> list[list[str]]:
    """3E-4's sweeps on the k6 golden tree, as its CTest runs them but with the harness's own
    ``--odin3`` and ``--vtr-root`` (review M5): ``blif-roundtrip --vtr`` and ``vtr-read-check``,
    their reports under ``work``."""
    return [
        [str(repo / "tools" / "blif-roundtrip" / "blif-roundtrip"), "--vtr",
         "--rt", str(odin3.parent / "odin3-blif-rt"), "--techlib", str(repo / qor_legs.LIB_REL),
         "--out", str(work / "blif-roundtrip-vtr"), str(golden_k6)],
        [str(repo / "tools" / "vtr-read-check" / "vtr-read-check"),
         "--odin3", str(odin3), "--golden-root", str(golden_k6),
         "--arch", str(qor_legs.arch_xml_path(vtr_root, "k6")),
         "--vpr", str(vtr_root / "vpr" / "vpr"), "--work", str(work / "vtr-read-check")],
    ]


ABC_LUT = "6"
ABC_TIMEOUT_S = "3000"
DEBUG_ODIN3_REL = Path("build") / "debug" / "odin3"
EXPECT_FAIL_REL = Path("tests") / "abc" / "equiv_expect_fail.txt"


def sweep_3d_commands(repo: Path, odin3: Path, golden_k6: Path, sets: Sets, work: Path
                      ) -> list[list[str]]:
    """3D's sweeps in order (3D §13.4, §13.5, §13.8), with the options of the CTest sweep
    ``abc_equiv_golden`` and the 3D plan's signatures (review H2): ``abc-equiv`` and ``abc-diff``
    with ``--equiv-check``, ``--lut 6``, ``--timeout 3000`` and ``--expect-fail``; ``abc-equiv``
    keeps its files under ``work``; ``abc-determinism --mode a`` and ``--mode c`` on the k6
    goldens except ``mcml`` and ``LU32PEEng`` (smallest first), then ``--mode d`` (Debug against
    Release, ``--odin3-debug``) on the micros only: the committed BLIFs and the k6
    ``regression/`` goldens."""
    tools = repo / "tools" / "abc-check"
    files = [str(golden_k6 / "vtr" / d / f"{d}.parmys.blif") for d in sets.order
             if d in sets.gate and d not in BIG_DESIGNS
             and (golden_k6 / "vtr" / d / f"{d}.parmys.blif").is_file()]
    base = ["--odin3", str(odin3)]
    equiv = ["--equiv-check", str(repo / "tools" / "equiv-check" / "equiv-check"),
             "--lut", ABC_LUT, "--timeout", ABC_TIMEOUT_S,
             "--expect-fail", str(repo / EXPECT_FAIL_REL)]
    micros = [str(repo / "tests" / "golden" / "blif"), str(golden_k6 / "regression")]
    debug = repo / DEBUG_ODIN3_REL
    return [
        [str(tools / "abc-equiv"), *base, *equiv, "--keep", str(work / "abc-equiv-golden"),
         *files],
        [str(tools / "abc-diff"), *base, *equiv, *files],
        [str(tools / "abc-determinism"), *base, "--mode", "a", *files],
        [str(tools / "abc-determinism"), *base, "--mode", "c", *files],
        [str(tools / "abc-determinism"), *base, "--odin3-debug", str(debug), "--mode", "d",
         *micros],
    ]


def run_sweeps(commands: Sequence[Sequence[str]], emit: Callable[[str], None], what: str,
               runner: Callable[..., int] | None = None,
               remaining: Callable[[], float | None] | None = None) -> list[str]:
    """Run each command in order; a tool that is not installed is skipped with the line
    ``vtr-qor: nightly: <what> skipped (<tool> absent)``. ``remaining`` is the seconds left to
    the night's deadline: a command is not started when none is left (``not started``), and one
    that runs is stopped at the deadline (review H3). Returns the summary line of each."""
    run = runner or run_tool
    lines = []
    for argv in commands:
        tool = Path(argv[0])
        if not tool.is_file():
            emit(f"nightly: {what} skipped ({tool.parent.name}/{tool.name} absent)")
            lines.append(f"{tool.name}: skipped (absent)")
            continue
        left = None if remaining is None else remaining()
        if left is not None and left <= 0:
            emit(f"nightly: {what}: {tool.name} not started before the deadline")
            lines.append(f"{tool.name}: not started (deadline)")
            continue
        code = run(argv) if left is None else run(argv, timeout_s=left)
        lines.append(f"{tool.name}: exit {code}")
        emit(f"nightly: {what}: {tool.name} exit {code}")
    return lines


def sweep_ran(lines: Sequence[str]) -> bool:
    """True when at least one tool of a sweep ran (not all skipped absent or not started)."""
    return any(": exit " in line for line in lines)


# -- the summary -------------------------------------------------------------------------------


def runs_from_json(path: Path) -> list[Run]:
    """The runs of a table's ``runs.json`` (metrics objects with ``key12`` and ``status``)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    runs = []
    for item in data["runs"]:
        runs.append(Run(leg=item["leg"], design=item["design"], arch=item.get("arch", ""),
                        key12=item.get("key12", ""), status=item["status"],
                        gating=bool(item.get("gating")), metrics=item))
    return runs


def _pct(old: float, new: float) -> float:
    return (new - old) / old if old else (0.0 if new == old else float("inf"))


def diff_runs(previous: Sequence[Run], current: Sequence[Run], metrics: Sequence[str]) -> list[str]:
    """Every (leg, design, metric) that moved by more than 2 % between two nightly tables, and
    every changed status, in (design, leg, metric) order."""
    before = {(r.leg, r.design): r for r in previous}
    lines = []
    for run in sorted(current, key=lambda r: (r.design, r.leg)):
        old = before.get((run.leg, run.design))
        if old is None:
            continue
        label = f"{display_leg(run.leg)} {run.design}"
        if old.status != run.status:
            lines.append(f"{label}: status {old.status} -> {run.status}")
        for verdict in VERDICT_FIELDS:
            before_v, after_v = qor_table.text_field(old, verdict), qor_table.text_field(
                run, verdict)
            if before_v != after_v:
                lines.append(f"{label}: {verdict} {before_v} -> {after_v}")
        for name in metrics:
            a, b = qor_table.metric(old, name), qor_table.metric(run, name)
            if a is None or b is None or abs(_pct(a, b)) <= MOVE_LIMIT:
                continue
            lines.append(f"{label}: {name} {a:g} -> {b:g} ({_pct(a, b):+.1%})")
    return lines


def previous_table(tables: Path, label_prefix: str, current: Path) -> Path | None:
    """The newest nightly table directory before ``current`` (names sort by their stamp)."""
    names = sorted(p.name for p in tables.glob(f"*-{label_prefix}*") if p.is_dir()
                   and p.name < current.name)
    return tables / names[-1] if names else None


def summary_text(starved: Sequence[str], steps: Sequence[str], moved: Sequence[str]) -> str:
    """``summary.md``: the starved line first when there is one, then every skipped or
    not-started step (L3), the steps, then the movers."""
    skipped = [f"vtr-qor: nightly: {s}" for s in steps if "skipped" in s or "not started" in s]
    parts = [*starved, *skipped, "# vtr-qor nightly", "", "## Steps", "",
             *[f"- {s}" for s in steps], "",
             "## Moved more than 2 % or changed status", ""]
    parts += [f"- {m}" for m in moved] if moved else ["None."]
    return "\n".join(parts) + "\n"


# -- check-lib and crosscheck ------------------------------------------------------------------

MODELS_REL = Path("tests") / "vtr" / "arch" / "k6_frac_N10_frac_chain_mem32K_40nm.models.json"
TIME_SUFFIXES = ("_time", "_runtime")
TIME_PREFIX = "max_vpr_mem"


def check_lib_command(repo: Path, vtr_root: Path) -> list[str]:
    """3E §15.2: ``vtr-models check-lib lib/vtr.o3lib --models <k6 models> --vtr-root <V>``."""
    return [str(repo / "tools" / "vtr-models" / "vtr-models"), "check-lib",
            str(repo / qor_legs.LIB_REL), "--models", str(repo / MODELS_REL),
            "--vtr-root", str(vtr_root)]


# 3F's task for ``crosscheck`` (PHASE3 decision 29, ruling 2): the three smallest designs, P's flow.
DEFAULT_TASK = qor_legs.REPO_ROOT / "tests" / "vtr" / "tasks" / "qor_p"


def crosscheck_command(vtr_root: Path, task: Path, out: Path) -> list[str]:
    """``run_vtr_task.py <abs task> -temp_dir <out>/crosscheck -j 1`` (spec §9)."""
    python = vtr_root / ".venv" / "bin" / "python"
    return [str(python), str(vtr_root / "vtr_flow" / "scripts" / "run_vtr_task.py"),
            str(task.resolve()), "-temp_dir", str(out / "crosscheck"), "-j", "1"]


def parse_rows(path: Path) -> dict[str, dict[str, str]]:
    """A ``parse_results.txt`` (a header line, then one tab-separated row per circuit) keyed by
    the circuit's name without ``.v``/``.blif``."""
    lines = [line for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(lines) < 2:
        return {}
    names = lines[0].rstrip("\t").split("\t")
    rows = {}
    for line in lines[1:]:
        fields = line.rstrip("\t").split("\t")
        row = dict(zip(names, fields, strict=False))
        circuit = row.get("circuit", "")
        rows[circuit.rsplit(".", 1)[0] if "." in circuit else circuit] = row
    return rows


def is_time_key(name: str) -> bool:
    return name.endswith(TIME_SUFFIXES) or name.startswith(TIME_PREFIX)


def crosscheck_lines(design: str, task: Mapping[str, str], harness: Mapping[str, str]
                     ) -> list[str]:
    """``vtr-qor: crosscheck: <d> <key>: task <a>, harness <b>`` for every non-time key of the
    task's row (but ``circuit``, which names the row) that the harness's row lacks or holds
    differently."""
    lines = []
    for key in sorted(task):
        if is_time_key(key) or key == "circuit":
            continue
        a, b = task[key], harness.get(key, "(absent)")
        if a != b:
            lines.append(f"vtr-qor: crosscheck: {design} {key}: task {a}, harness {b}")
    return lines


# -- the night ---------------------------------------------------------------------------------


@dataclass
class NightContext:
    """What ``run_night`` needs. ``run_inner`` runs one vtr-qor subcommand in process;
    ``legs`` are the legs this odin3 can run (P always); ``refused`` counts the deadline refusals
    in ``harness.log`` so far."""

    out: Path
    repo: Path
    odin3: Path
    vtr_root: Path
    golden_k6: Path
    sets: Sets
    legs: tuple[str, ...]
    common: tuple[str, ...]
    today: date
    run_inner: Callable[[Sequence[str]], int]
    emit: Callable[[str], None]
    refused: Callable[[], int]
    remaining: Callable[[], float | None] | None = None  # seconds to the deadline (None: no cap)


def _legs_arg(legs: Sequence[str]) -> str:
    return ",".join(legs)


class Night:
    """One night's steps; ``results`` collects ``(step, exit code)``. After a step exits 5 (the
    disk floor, review M3) no later step or sweep runs; the night writes its summary and exits 5.
    ``swept`` says which sweeps ran at least one tool (L2)."""

    def __init__(self, ctx: NightContext) -> None:
        self.ctx = ctx
        self.results: list[tuple[str, int]] = []
        self.disk_stop = False
        self.swept = {"3e": False, "3d": False}

    def _stopped(self, name: str) -> bool:
        if self.disk_stop:
            self.results.append((f"{name} (skipped: disk floor)", 0))
        return self.disk_stop

    def inner(self, name: str, argv: Sequence[str]) -> int:
        if self._stopped(name):
            return EXIT_DISK
        code = self.ctx.run_inner(argv)
        self.results.append((name, code))
        self.disk_stop = code == EXIT_DISK
        return code

    def tool(self, name: str, argv: Sequence[str]) -> int:
        tool = Path(argv[0])
        if self._stopped(name):
            return EXIT_DISK
        if not tool.is_file():
            self.ctx.emit(f"nightly: {name} skipped ({tool.parent.name}/{tool.name} absent)")
            self.results.append((name + " (skipped, absent)", 0))
            return 0
        left = None if self.ctx.remaining is None else self.ctx.remaining()
        if left is not None and left <= 0:
            self.ctx.emit(f"nightly: {name} not started before the deadline")
            self.results.append((name + " (not started: deadline)", EXIT_TOOL_TIMEOUT))
            return EXIT_TOOL_TIMEOUT
        code = run_tool(argv, timeout_s=left)
        self.results.append((name, code))
        return code

    def step1(self) -> None:
        ctx = self.ctx
        self.inner("1 doctor", ["doctor", *ctx.common])
        models = ctx.repo / "tools" / "vtr-models" / "vtr-models"
        arch_xml = qor_legs.arch_xml_path(ctx.vtr_root, "k6")
        self.tool("1 vtr-models check", [str(models), "check", str(ctx.repo / MODELS_REL),
                                         "--arch", str(arch_xml)])
        self.inner("1 check-lib", ["check-lib", *ctx.common])

    def step2(self) -> None:
        ctx = self.ctx
        self.inner("2 EArch smoke", ["run", *ctx.common, "--arch", "earch", "--set", "smoke",
                                     "--leg", _legs_arg(ctx.legs)])

    def step3(self, *, checks: bool) -> None:
        ctx = self.ctx
        group_a = [d for d in ctx.sets.order if ctx.sets.designs[d].group == "A"]
        designs = ",".join(group_a)
        plain = [leg for leg in ctx.legs if leg not in ("O", "Oprime")]
        run = ["run", *ctx.common, "--designs", designs]
        self.inner("3 group A", [*run, "--leg", _legs_arg(plain)])
        if "Oprime" in ctx.legs:
            self.inner("3 group A O'", [*run, "--leg", "Oprime", "--keep-netlists"])
        if "O" in ctx.legs:
            self.inner("3 group A O", [*run, "--leg", "O", "--keep-netlists", "--fcheck"])
        if checks:
            small = ",".join(d for d in group_a if d in EQUIV_REQUIRED and d in ctx.sets.gate)
            self.inner("3 equiv", ["equiv", *ctx.common, "--designs", small])
            if "O" in ctx.legs:
                self.inner("3 rerun-check", ["rerun-check", *ctx.common, "--designs", designs])

    def step4(self, slot: Slot) -> bool:
        """Run the slot's jobs; True when none was refused by the deadline."""
        ctx = self.ctx
        before = ctx.refused()
        run = ["run", *ctx.common, "--designs", slot.design]
        keep = ["--keep-netlists"] if slot.equiv else []
        others = [leg for leg in slot.legs if leg != "O"]
        if others:
            self.inner(f"4 slot {slot.number}", [*run, "--leg", _legs_arg(others), *keep])
        if "O" in slot.legs:
            fcheck = ["--fcheck", "--keep-netlists"] if slot.fcheck else keep
            self.inner(f"4 slot {slot.number} O", [*run, "--leg", "O", *fcheck])
        if slot.equiv:
            self.inner(f"4 slot {slot.number} equiv", ["equiv", *ctx.common,
                                                       "--designs", slot.design])
        return ctx.refused() == before

    def sweeps(self, plan: NightPlan, sets: Sets) -> list[str]:
        ctx, lines = self.ctx, []
        work = ctx.out / "nightly" / f"{ctx.today:%Y%m%d}"
        if self.disk_stop:
            return ["sweeps: skipped (disk floor)"] if plan.sweep_3e or plan.sweep_3d else []
        if plan.sweep_3e:
            got = run_sweeps(sweep_3e_commands(ctx.repo, ctx.odin3, ctx.vtr_root, ctx.golden_k6,
                                               work), ctx.emit, "3E sweeps",
                             remaining=ctx.remaining)
            self.swept["3e"] = sweep_ran(got)
            lines += got
        if plan.sweep_3d:
            got = run_sweeps(sweep_3d_commands(ctx.repo, ctx.odin3, ctx.golden_k6, sets, work),
                             ctx.emit, "3D sweeps", remaining=ctx.remaining)
            self.swept["3d"] = sweep_ran(got)
            lines += got
        return lines

    def exit_code(self) -> int:
        codes = {code for _, code in self.results}
        if self.disk_stop:
            return EXIT_DISK
        if codes - {0, 3}:
            return 1
        return 3 if 3 in codes else 0
