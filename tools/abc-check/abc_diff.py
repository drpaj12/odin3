"""abc-diff: the BLIF exchange against the in-memory exchange, design by design (3D Task 21; spec
13.5, I8).

Usage::

    abc-diff --odin3 O --equiv-check E [--lut K] [--timeout S] [--expect-fail F] [--keep DIR]
             <file or dir>...

Every ``.blif`` under the directories (and the files) given is one design; all are sorted together
by size, smallest first (ties by path).  For each design the tool runs odin3 twice::

    O -p "read_techlib lib/vtr.o3lib; read_blif X; write_blif before.blif;
          abc --lut K --via-blif <dir>; write_blif after.blif"          (the BLIF exchange)
    ODIN3_ABC_EXCHANGE=aig O -p "...; abc --lut K; write_blif after.blif"   (the in-memory one)

in ``blif/`` and ``aig/`` under the design's directory in ``--keep DIR`` (default: a fresh
directory under ``~/odin3-ws/work/abc-diff/``), then runs ``equiv-check --blackbox`` on the input
against each result and on the two results against each other.  One line per design::

    <design> luts <b> <a> <a/b> ffs <b> <a> <a/b> depth <b> <a> <a/b> <verdict>

``b`` is the BLIF exchange's count, ``a`` the in-memory exchange's, the ratio ``a/b`` has three
decimals.  LUTs are the ``.names`` with an input, FFs the ``.latch`` lines, depth the longest chain
of LUTs between inputs, flip-flop outputs, hard-block outputs and outputs, flip-flop inputs.  The
verdict is ``equivalent``; ``explain`` when the results are equivalent but some ratio is more than
5% from 1 (PHASE3 records the reason, I8); or the first failure: ``differs``, ``timeout``,
``views``, ``multiclock``, ``error`` (equiv-check), ``odin3-error`` (a run did not finish; then the
line is ``<design> odin3-error <detail>`` without counts).  ``--expect-fail`` takes the lines of
abc-equiv's file (``<path> <status> <reason>``): a listed design passes only with its pinned
status.  A path under ``~/odin3-ws/golden`` first needs ``qor_heavy.require_heavy()`` (point
(m)): run through ``tools/heavy-run``.  Nothing is deleted.

Exit codes (the project's):

- 0: every design passes (``equivalent``, ``explain`` or a listed class).
- 1: a design ``differs``, or a listed design passes (XPASS).
- 2: a tool error (usage, a run that did not finish, ``error``, ``views`` or ``multiclock``
  unlisted).
- 4: only ``unproven`` or ``timeout`` designs failed (equiv-check's exit 4 is ``unproven``; only a
  listing of ``unproven`` absorbs it).
- 77: ABC is not available (``ODIN3_ABC`` names a missing file, or odin3 says the linked ABC is
  missing) and every design was skipped.

The plan said 0, 1, 2; exit code 3 (a crash) is not produced: a crashing odin3 is ``odin3-error``.
All three pairs (input~blif, input~aig, blif~aig) always run; the verdict is the worst of them
(differs, timeout, unproven, error, multiclock), so a listed ``error`` on the input pairs cannot
hide a ``differs`` of the two results.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import os
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _sub in ("tools/abc-check", "tools/blif"):  # also under python -I, which adds no script dir
    if str(REPO_ROOT / _sub) not in sys.path:
        sys.path.insert(0, str(REPO_ROOT / _sub))
import abc_equiv  # noqa: E402
import blif  # noqa: E402

TOOL = "abc-diff"
EXIT_OK, EXIT_DIFFERS, EXIT_ERROR, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 4, 77
STATUS_ORDER = ("differs", "timeout", "unproven", "error", "multiclock")  # worst first
NO_ABC_TEXT = "linked ABC is not available"
WORK_ROOT = Path.home() / "odin3-ws" / "work" / "abc-diff"
EXPLAIN_NUM, EXPLAIN_DEN = 1, 20  # a ratio more than 1/20 (5%) from 1 needs an explanation
PASSING = ("equivalent", "explain")
NO_DEPTH = -1  # a combinational cycle of LUTs: the depth is not defined
ODIN3_FAILED = "odin3-error"
# the statuses a listed design may be pinned to
PINNED = (*abc_equiv.PINNED, "differs", "timeout", "unproven")


@dataclass(frozen=True)
class Counts:
    luts: int
    ffs: int
    depth: int


@dataclass(frozen=True)
class Verdict:
    status: str
    secs: float
    counts: tuple[Counts, Counts] | None = None  # (BLIF exchange, in-memory exchange)
    detail: str = ""


def ratio_text(blif_n: int, aig_n: int) -> str:
    """``a/b`` to three decimals; ``1.000`` for 0/0, ``inf`` for a/0 and ``n/a`` for no depth."""
    if NO_DEPTH in (blif_n, aig_n):
        return "n/a"
    if blif_n == 0:
        return "1.000" if aig_n == 0 else "inf"
    return f"{aig_n / blif_n:.3f}"


def needs_explanation(blif_n: int, aig_n: int) -> bool:
    """True when ``aig_n / blif_n`` is more than 5% from 1 (exact integer arithmetic)."""
    if NO_DEPTH in (blif_n, aig_n):
        return False
    if blif_n == 0:
        return aig_n != 0
    return abs(aig_n - blif_n) * EXPLAIN_DEN > blif_n * EXPLAIN_NUM


def top_cells(path: Path) -> tuple[list[tuple[list[str], str]], int]:
    """The top model's ``.names`` (inputs, output) with at least one input, and its ``.latch``
    count.  Read line by line: a hard block's ``.subckt`` needs no ``.model`` here."""
    names: list[tuple[list[str], str]] = []
    latches = 0
    seen_models = 0
    for _, words in blif.logical_lines(path.read_text(encoding="utf-8")):
        if words[0] == ".model":
            seen_models += 1
        if seen_models > 1:
            break
        if words[0] == ".names" and len(words) > 2:
            names.append((words[1:-1], words[-1]))
        elif words[0] == ".latch":
            latches += 1
    return names, latches


def netlist_counts(path: Path) -> Counts:
    """LUTs, FFs and depth of the top model of a BLIF file (one linear pass, no recursion)."""
    luts, ffs = top_cells(path)
    outputs = {out for _, out in luts}
    pending: collections.Counter[str] = collections.Counter()
    readers: dict[str, list[int]] = collections.defaultdict(list)
    for number, (ins, out) in enumerate(luts):
        for net in set(ins):
            if net in outputs:
                pending[out] += 1
                readers[net].append(number)
    level: dict[str, int] = {}
    ready = collections.deque(n for n, (_, out) in enumerate(luts) if pending[out] == 0)
    depth = 0
    while ready:
        ins, out = luts[ready.popleft()]
        level[out] = 1 + max((level.get(net, 0) for net in ins), default=0)
        depth = max(depth, level[out])
        for reader in readers.get(out, ()):
            pending[luts[reader][1]] -= 1
            if pending[luts[reader][1]] == 0:
                ready.append(reader)
    released = sum(1 for _, out in luts if out in level)
    return Counts(len(luts), ffs, depth if released == len(luts) else NO_DEPTH)


def line_text(design: Path, verdict: Verdict) -> str:
    """The design's output line."""
    if verdict.counts is None:
        extra = f" ({verdict.detail})" if verdict.detail else ""
        return f"{design} {verdict.status}{extra}"
    left, right = verdict.counts
    fields = []
    for name, base, new in (("luts", left.luts, right.luts), ("ffs", left.ffs, right.ffs),
                            ("depth", left.depth, right.depth)):
        shown = (("n/a" if n == NO_DEPTH else str(n)) for n in (base, new))
        fields.append(f"{name} {' '.join(shown)} {ratio_text(base, new)}")
    extra = f" ({verdict.detail})" if verdict.detail else ""
    return f"{design} {' '.join(fields)} {verdict.status}{extra}"


def run_pair(cfg: abc_equiv.Config, design_name: str, pair: tuple[Path, Path]) -> abc_equiv.Outcome:
    """equiv-check --blackbox on two files; the outcome as abc-equiv reports it."""
    start = time.monotonic()
    extra = ["--allow-undriven"] if design_name in abc_equiv.ALLOW_UNDRIVEN else []
    argv = [cfg.equiv_check, "--blackbox", abc_equiv.BLACKBOX_MODELS, *extra, "--timeout",
            str(cfg.timeout), str(pair[0]), str(pair[1])]
    try:
        proc = subprocess.run(argv, cwd=abc_equiv.REPO_ROOT, env=abc_equiv.child_env(cfg, False),
                              capture_output=True, text=True,
                              timeout=cfg.timeout + abc_equiv.EQUIV_SLACK, check=False)
    except subprocess.TimeoutExpired:
        return abc_equiv.Outcome("timeout", time.monotonic() - start, "equiv-check timed out")
    except OSError as exc:
        return abc_equiv.Outcome("error", time.monotonic() - start,
                                 f"cannot run {cfg.equiv_check}: {exc}")
    secs = time.monotonic() - start
    log = proc.stdout + proc.stderr
    if abc_equiv.MULTICLOCK_TEXT in log:
        return abc_equiv.Outcome("multiclock", secs, abc_equiv.MULTICLOCK_TEXT)
    status = {0: "equivalent", 1: "differs", 4: "unproven"}.get(proc.returncode, "error")
    if status == "differs" and "NOT equivalent" not in log:
        status = "error"
    return abc_equiv.Outcome(status, secs, (log.strip().splitlines() or [""])[-1])


def exchange_runs(cfg: abc_equiv.Config, design: Path,
                  out: Path) -> tuple[Path, Path] | abc_equiv.Outcome:
    """Both odin3 runs; the two ``after.blif`` files (BLIF, in-memory), or the failing outcome."""
    blif_dir, aig_dir = out / "blif", out / "aig"
    blif_cfg = dataclasses.replace(cfg, via_blif=str(out / "blif-files"), exchange=None)
    aig_cfg = dataclasses.replace(cfg, via_blif=None, exchange="aig")
    for label, run_cfg, where in (("blif", blif_cfg, blif_dir), ("aig", aig_cfg, aig_dir)):
        outcome = abc_equiv.run_odin3(run_cfg, design, where)
        if outcome.status != "mapped":
            status = outcome.status if outcome.status in ("timeout", "views") else ODIN3_FAILED
            status = "skip" if NO_ABC_TEXT in outcome.detail else status
            return abc_equiv.Outcome(status, outcome.secs, f"{label} exchange: {outcome.detail}")
    return blif_dir / "after.blif", aig_dir / "after.blif"


def judge_design(cfg: abc_equiv.Config, design: Path, out: Path) -> Verdict:
    """Run both exchanges, compare them with the input and each other, count."""
    start = time.monotonic()
    ran = exchange_runs(cfg, design, out)
    if isinstance(ran, abc_equiv.Outcome):
        return Verdict(ran.status, time.monotonic() - start, None, ran.detail)
    after_blif, after_aig = ran
    before = out / "blif" / "before.blif"
    pairs = (("input~blif", (before, after_blif)), ("input~aig", (before, after_aig)),
             ("blif~aig", (after_blif, after_aig)))
    bad: list[tuple[str, abc_equiv.Outcome]] = []
    for label, files in pairs:  # all three, whatever the first says
        outcome = run_pair(cfg, design.stem, files)
        if outcome.status != "equivalent":
            bad.append((label, outcome))
    status = "equivalent"
    if bad:
        status = min((o.status for _, o in bad),
                     key=lambda name: STATUS_ORDER.index(name) if name in STATUS_ORDER else 99)
    detail = "; ".join(f"{label}: {o.detail}" for label, o in bad)
    counts = (netlist_counts(after_blif), netlist_counts(after_aig))
    if status == "equivalent" and any(
            needs_explanation(getattr(counts[0], f), getattr(counts[1], f))
            for f in ("luts", "ffs", "depth")):
        status = "explain"
    return Verdict(status, time.monotonic() - start, counts, detail)


def read_expect(path: str | None) -> dict[str, str]:
    """``--expect-fail``: abc-equiv's ``<path> <status> <reason>`` lines, with ``differs`` and
    ``timeout`` allowed as statuses too."""
    entries: dict[str, str] = {}
    if path is None:
        return entries
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise abc_equiv.UsageError(f"{path}: cannot read: {exc}") from exc
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(" ")
        if not reason.strip() or reason.split()[0] not in PINNED:
            raise abc_equiv.UsageError(f"{path}:{number}: expected '<path> <status> <reason>' "
                                       f"with status one of {', '.join(PINNED)}")
        entries[name] = reason.strip()
    return entries


def judge(verdict: Verdict, reason: str | None) -> tuple[bool, str]:
    """(passes, note): a listed design passes only with its pinned status."""
    if reason is None:
        return verdict.status in PASSING, ""
    if verdict.status in PASSING:
        return False, f"XPASS: listed ({reason}) but {verdict.status}"
    pinned = reason.split()[0]
    if verdict.status != pinned:
        return False, f"UNEXPECTED: listed as {pinned} but {verdict.status}"
    return True, f"listed: {reason}"


def run_all(cfg: abc_equiv.Config, designs: Sequence[Path], entries: dict[str, str]) -> int:
    counts: dict[str, int] = {}
    codes: list[int] = []
    failed = 0
    for index, design in enumerate(designs):
        verdict = judge_design(cfg, design, abc_equiv.design_dir(cfg, index, design))
        ok, note = judge(verdict, abc_equiv.listed_reason(design, entries))
        counts[verdict.status] = counts.get(verdict.status, 0) + 1
        failed += 0 if ok else 1
        codes.append(design_code(verdict.status, ok))
        detail = " ".join(x for x in (note, verdict.detail) if x)
        shown = dataclasses.replace(verdict, detail=detail)
        print(line_text(design, shown), flush=True)
    shown_counts = ", ".join(f"{name} {count}" for name, count in sorted(counts.items()))
    print(f"{TOOL}: {len(designs)} designs: {len(designs) - failed} passed, {failed} failed"
          + (f" ({shown_counts})" if shown_counts else ""))
    return exit_code(codes, len(designs))


def exit_code(codes: Sequence[int], total: int) -> int:
    """The project exit code of the per-design codes (0 for a pass)."""
    if total and codes.count(EXIT_SKIP) == total:
        return EXIT_SKIP
    for code in (EXIT_DIFFERS, EXIT_ERROR, EXIT_UNPROVEN):
        if code in codes:
            return code
    return EXIT_SKIP if EXIT_SKIP in codes else EXIT_OK


def design_code(status: str, ok: bool) -> int:
    """The exit code a design contributes."""
    if ok:
        return EXIT_OK
    if status in PASSING or status == "differs":
        return EXIT_DIFFERS  # a listed design that passes (XPASS), or a difference
    if status in ("unproven", "timeout"):
        return EXIT_UNPROVEN
    return EXIT_SKIP if status == "skip" else EXIT_ERROR


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog=TOOL, description="BLIF exchange against in-memory.")
    ap.add_argument("paths", nargs="+", metavar="FILE_OR_DIR")
    ap.add_argument("--odin3", required=True, metavar="O", help="the odin3 binary")
    ap.add_argument("--equiv-check", required=True, metavar="E", help="the equiv-check tool")
    ap.add_argument("--lut", type=int, default=abc_equiv.DEFAULT_LUT, metavar="K")
    ap.add_argument("--timeout", type=int, default=abc_equiv.DEFAULT_TIMEOUT, metavar="S")
    ap.add_argument("--expect-fail", metavar="F", help="listed designs that must not pass")
    ap.add_argument("--keep", metavar="DIR", help="where the runs' files are kept")
    return ap


def _keep_dir(arg: str | None) -> Path:
    if arg:
        return Path(arg)
    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    keep = Path(tempfile.mkdtemp(prefix="run.", dir=WORK_ROOT))
    print(f"{TOOL}: files in {keep}", file=sys.stderr)
    return keep


def no_abc() -> bool:
    """True when ``ODIN3_ABC`` names a file that does not exist (ABC is not available)."""
    path = os.environ.get("ODIN3_ABC")
    return path is not None and not Path(path).exists()


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        abc_equiv.check_golden(args.paths)
        cfg = abc_equiv.Config(args.odin3, args.equiv_check, "abc", args.lut, None, None, False,
                               _keep_dir(args.keep), args.timeout)
        entries = read_expect(args.expect_fail)
        designs = abc_equiv.collect(args.paths)
    except abc_equiv.UsageError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_ERROR
    if no_abc():
        print(f"{TOOL}: ODIN3_ABC names a missing file; skipped", file=sys.stderr)
        return EXIT_SKIP
    return run_all(cfg, designs, entries)


if __name__ == "__main__":
    sys.exit(main())
