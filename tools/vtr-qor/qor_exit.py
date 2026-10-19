"""qor_exit: the steps of ``vtr-qor exit-run`` (3H spec §9, plan Task 15, M3).

``exit-run --label TEXT [--budget H]`` is started by the controller inside ``tools/heavy-run``
(as the nightly is, point (c)); ``vtr_qor.cmd_exit_run`` builds the context and calls
``run_exit``. This module decides the eight steps and their argv and holds the resume state, so
all of it is testable with a fake ``run_inner``:

1. ``run --leg P,Oprime,Pprime,O --set gate,extra --fcheck --keep-netlists`` (the Verilator check
   of P and O right after their own job);
2. ``fcheck --leg P --set gate,extra --resume`` (the P check from the kept netlists, for any P job
   step 1 did not check);
3. ``equiv`` on the designs ``EQUIV_DESIGNS`` names;
4. ``rerun-check --leg O --set gate``;
5. ``run --arch earch --set smoke --leg P,Pprime,Oprime,O``;
6. the seed reruns (§6.3): ``run --leg P,O --designs <candidates> --seed N`` for N in 2 and 3;
7. ``table --legs P,Oprime,Pprime,O --label <label>`` (with ``--seeds 2,3`` after step 6 ran);
8. ``gate <table dir>``.

A step that ended with every job run (exit 0 or 1) is recorded in
``<out>/exit-run/<label>/state.json`` and not repeated when ``exit-run`` is started again with
the label; the job cache and each held verdict (``--resume``) make a repeated step cheap anyway.
A step that stopped short (exit 3 capped, timeout or the deadline; 5 the disk) is repeated.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

import qor_sched
from qor_sched import Sets

STEPS = 8
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_INCOMPLETE, EXIT_LOCK, EXIT_DISK = 0, 1, 2, 3, 4, 5
STATE_FILE = "state.json"
STEP_NAMES = ("run", "P fcheck", "equiv", "rerun-check", "EArch smoke", "seed reruns", "table",
              "gate")
GATE_LEGS = ("P", "Oprime", "Pprime", "O")
SMOKE_LEGS = ("P", "Pprime", "Oprime", "O")
# Step 3: PHASE3 #16's required designs among the gate, plus the one the plan measures (§8.2).
EQUIV_EXTRA = ("stereovision0",)
SEEDS = (2, 3)
SEED_LEGS = ("P", "O")
SEED_DESIGNS_NONE = "no design is near a bound"


@dataclass
class ExitContext:
    """What ``run_exit`` needs. ``common`` is the option list every inner command takes
    (``--vtr-root``, ``--odin3``, ``--out``, ...); ``legs`` the legs ``odin3`` can run (P always)
    and ``lacks`` what each other leg is missing; ``candidates`` the seed-rerun designs from the
    step 1 results (None: not decidable yet); ``run_inner`` runs one vtr-qor subcommand in
    process and ``emit`` writes a ``vtr-qor: `` line."""

    out: Path
    label: str
    sets: Sets
    legs: tuple[str, ...]
    lacks: dict[str, str]
    common: tuple[str, ...]
    run_inner: Callable[[Sequence[str]], int]
    emit: Callable[[str], None]
    candidates: Callable[[], list[str] | None]
    keys: Callable[[], dict[str, str]]
    fingerprint: str = ""


def state_path(out: Path, label: str) -> Path:
    return out / "exit-run" / label / STATE_FILE


def load_done(out: Path, label: str, fingerprint: str = "") -> dict[int, int]:
    """The recorded steps (number to exit code); unreadable, malformed, or recorded for another
    fingerprint (a rebuilt odin3, a moved VTR, an edited input): none."""
    try:
        data = json.loads(state_path(out, label).read_text(encoding="utf-8"))
        if data.get("fingerprint", "") != fingerprint:
            return {}
        return {int(k): int(v) for k, v in data["done"].items()}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def save_done(out: Path, label: str, done: dict[int, int], fingerprint: str = "") -> None:
    path = state_path(out, label)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    body = {"label": label, "fingerprint": fingerprint,
            "done": {str(k): done[k] for k in sorted(done)}}
    tmp.write_text(json.dumps(body, sort_keys=True, indent=1) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _legs_arg(wanted: Sequence[str], available: Sequence[str]) -> str:
    return ",".join(leg for leg in wanted if leg in available)


def equiv_designs(sets: Sets) -> list[str]:
    """Step 3's designs, in §2 order: the gate designs PHASE3 #16 requires ``equiv`` on, and
    ``EQUIV_EXTRA`` (measured on the first O run)."""
    return [d for d in sets.order if (d in sets.gate and d in qor_sched.EQUIV_REQUIRED)
            or d in EQUIV_EXTRA]


def skip_lines(ctx: ExitContext) -> list[str]:
    """``<L> skipped: needs pass '<p>'`` for every leg of steps 1, 5 and 7 that cannot run."""
    wanted = dict.fromkeys((*GATE_LEGS, *SMOKE_LEGS))
    return [f"{leg} skipped: needs pass '{ctx.lacks[leg]}'" for leg in wanted if leg in ctx.lacks]


def step_argvs(number: int, ctx: ExitContext, designs: Sequence[str] = (),
               table_dir: Path | None = None) -> list[list[str]]:
    """The argv of each inner command of step ``number`` (1 to 8). ``designs`` are step 6's
    seed-rerun designs and, in step 7, a non-empty list adds ``--seeds 2,3``; ``table_dir`` is
    step 8's table."""
    common = list(ctx.common)
    if number == 1:
        return [["run", *common, "--leg", _legs_arg(GATE_LEGS, ctx.legs), "--set", "gate,extra",
                 "--fcheck", "--keep-netlists"]]
    if number == 2:
        return [["fcheck", *common, "--leg", "P", "--set", "gate,extra", "--resume"]]
    if number == 3:
        return [["equiv", *common, "--designs", ",".join(equiv_designs(ctx.sets)), "--resume"]]
    if number == 4:
        return [["rerun-check", *common, "--leg", "O", "--set", "gate", "--resume"]]
    if number == 5:
        return [["run", *common, "--arch", "earch", "--set", "smoke", "--leg",
                 _legs_arg(SMOKE_LEGS, ctx.legs)]]
    if number == 6:
        return [["run", *common, "--leg", ",".join(SEED_LEGS), "--designs", ",".join(designs),
                 "--seed", str(seed)] for seed in SEEDS] if designs else []
    if number == 7:
        seeds = ["--seeds", ",".join(str(seed) for seed in SEEDS)] if designs else []
        keys = ctx.keys()
        pinned = ["--keys", ",".join(f"{leg}={keys[leg]}" for leg in GATE_LEGS if leg in keys)]
        return [["table", *common, "--legs", _legs_arg(GATE_LEGS, ctx.legs), *pinned,
                 "--label", ctx.label, *seeds]]
    return [["gate", str(table_dir)]] if table_dir is not None else []


def worst(codes: Sequence[int]) -> int:
    """The code a step with several commands ends with: the disk (5) first, then usage and lock
    (2, 4), then a failed job (1), then an incomplete one (3), else 0."""
    for wanted in (EXIT_DISK, EXIT_USAGE, EXIT_LOCK, EXIT_FAIL, EXIT_INCOMPLETE):
        if wanted in codes:
            return wanted
    return EXIT_OK


def _run_step(number: int, ctx: ExitContext, designs: Sequence[str],
              table_dir: Path | None = None) -> tuple[int, str]:
    """Run the commands of one step; returns the step's code and the last line of the stdout of
    the last command (the table directory, for step 7). A disk stop, a usage error or a busy lock
    ends the step at once."""
    codes, last = [], ""
    for argv in step_argvs(number, ctx, designs, table_dir):
        text = io.StringIO()
        with contextlib.redirect_stdout(text):
            code = ctx.run_inner(argv)
        lines = text.getvalue().splitlines()
        last = lines[-1] if lines else ""
        codes.append(code)
        if code in (EXIT_DISK, EXIT_USAGE, EXIT_LOCK):
            break
    return worst(codes), last


def _stopped_short(code: int) -> bool:
    """A step that did not run every job: exit 3 (capped, timeout, deadline), 5 (disk), or an
    option or lock error."""
    return code in (EXIT_INCOMPLETE, EXIT_DISK, EXIT_USAGE, EXIT_LOCK)


def _step_line(ctx: ExitContext, number: int) -> None:
    ctx.emit(f"exit-run: step {number}/{STEPS} {STEP_NAMES[number - 1]}")


def run_exit(ctx: ExitContext) -> int:
    """The eight steps. Steps 1 to 6 run in order (a step recorded done is skipped); a disk stop
    ends the run at once. Steps 7 and 8 run only when no earlier step stopped short, since the
    table of a partial run has no verdict worth recording; the exit code is then ``gate``'s (a
    failed job already shows in the table). When a step stopped short the code is the worst of
    those steps (5, then 2, 4, then 3)."""
    for line in skip_lines(ctx):
        ctx.emit(line)
    done = load_done(ctx.out, ctx.label, ctx.fingerprint)
    if not done and state_path(ctx.out, ctx.label).is_file():
        ctx.emit(f"exit-run: {state_path(ctx.out, ctx.label)} is for another build or input set; "
                 "every step starts again (the job cache keeps what still holds)")
    short: list[int] = []
    for number in range(1, STEPS - 1):
        if number in done:
            ctx.emit(f"exit-run: step {number}/{STEPS} {STEP_NAMES[number - 1]} already done "
                     f"(exit {done[number]})")
            continue
        _step_line(ctx, number)
        designs: list[str] = []
        if number == 6:
            found = ctx.candidates()
            if found is None:
                ctx.emit("exit-run: seed reruns wait for a complete O and P")
                short.append(EXIT_INCOMPLETE)
                continue
            designs = found
            if not designs:
                ctx.emit(f"exit-run: seed reruns: {SEED_DESIGNS_NONE}")
        code, _ = _run_step(number, ctx, designs)
        if code == EXIT_DISK:
            return EXIT_DISK
        if _stopped_short(code):
            short.append(code)
        else:
            done[number] = code
            save_done(ctx.out, ctx.label, done, ctx.fingerprint)
    failed = sorted(n for n, c in done.items() if c == EXIT_FAIL)
    if failed:
        ctx.emit(f"exit-run: steps {failed} had failed jobs or checks (not retried on resume; "
                 "rerun them with run --rerun --designs ...)")
    if short:
        ctx.emit(f"exit-run: steps 7 and 8 wait for the unfinished steps; run exit-run --label "
                 f"{ctx.label} again")
        return worst([*short, *([EXIT_FAIL] if failed else [])])
    _step_line(ctx, STEPS - 1)
    code, last = _run_step(STEPS - 1, ctx, ctx.candidates() or [])
    if code not in (EXIT_OK, EXIT_FAIL, EXIT_INCOMPLETE) or not last:
        return code or EXIT_FAIL
    _step_line(ctx, STEPS)
    verdict = _run_step(STEPS, ctx, (), Path(last))[0]
    # a failed job anywhere (the EArch smoke and the seed reruns are outside the gate) fails
    # the exit run, whatever the k6 gate says
    return EXIT_FAIL if failed and verdict in (EXIT_OK, EXIT_INCOMPLETE) else verdict
