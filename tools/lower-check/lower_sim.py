"""lower-sim: Odin III's simulator before and after `lower` on the same random vectors (3B spec
14.3; plan Task 2).

Usage::

    lower-sim --odin3 O [--top T] [--vectors N] [--seed S] [--pre CMD]... [--lower-args A]
              [--techlib L] [--mem-cap C] [--work DIR] FILE...

Per design (the FILEs together): ``read_verilog --nolatches FILE...; hierarchy; <pre>`` (``pre``
defaults to ``proc; opt``) is simulated for N cycles from seed S twice with the 1E ``sim`` pass:
as it is (the word cells run through the registry's simulate hooks) and after ``lower`` (the bit
cells). The vector files are compared; they hold every input and output bit of every cycle, which
is the sample point: the outputs after each cycle's active edge has settled (the declared
divergence of decision 19, a reset's mid-cycle timing, is not sampled). Per cell type of the
design the tool prints ``pass`` (the two runs agree) or ``skipped (reason)``. **A skip is never a
pass.** A design is skipped, and every type in it listed as skipped, when it holds a type without a
simulate hook (``$shl $shr $sshr $shiftx $div $mod $pow $tribuf``, ``$mem $memrd $memwr``, the word
flip-flops and latches); those are covered by ``lower-diff`` and ``lower-equiv``, by the reference
stepper of the unit tests and by the C array model (3B spec 14.3 table). A ``.blif`` FILE is
``read_blif``. A design with no word cell (``opt`` folded it away, or only bit cells) compared
nothing: it prints ``NOTHING TO COMPARE`` and exits 1 unless ``--allow-empty`` is given.

Exit codes (one convention across the opt tools): 0 no difference (skips are listed); 1 a
difference (the after run's vectors differ, or the lowered design does not simulate) or nothing
to compare; 2 usage or a
tool error (Odin III exited with an error, or the before run cannot be simulated for a reason that
is not a known skip); 3 hard: a child crashed, was killed by a signal or its memory cap, or timed
out; 77 skipped (an ASan child and no systemd-run). The work directory is made under
``$ODIN3_LOWER_WORK`` (else ``$TMPDIR``) and is never removed.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

import lower_common as lc
import memcap

TOOL = "lower-sim"
DEFAULT_VECTORS = 64
DEFAULT_SEED = 1
DEFAULT_PRE = ("proc", "opt")
NO_HOOK = "no simulate hook"
FF_REASON = "no simulate hook (IR change D); checked against the reference stepper"
MEM_REASON = "no simulate hook; covered by lower-equiv and the C array model"
SKIP_REASONS = {
    "$shl": f"{NO_HOOK}; covered by lower-diff (tier T) and the unit tests' C model",
    "$shr": f"{NO_HOOK}; covered by lower-diff (tier T) and the unit tests' C model",
    "$sshr": f"{NO_HOOK}; covered by lower-diff (tier T) and the unit tests' C model",
    "$shiftx": f"{NO_HOOK}; x outputs: covered by lower-equiv",
    "$div": f"{NO_HOOK}; covered by lower-diff (tier T)",
    "$mod": f"{NO_HOOK}; covered by lower-diff (tier T)",
    "$pow": f"{NO_HOOK}; lower has no rule for it",
    "$tribuf": f"{NO_HOOK}; lower has no rule for it",
    "$mem": MEM_REASON,
    "$memrd": MEM_REASON,
    "$memwr": MEM_REASON,
    **dict.fromkeys(("$dff", "$dffe", "$adff", "$sdff", "$adffe", "$sdffe", "$sdffce", "$aldff",
                     "$aldffe", "$dffsr", "$dffsre", "$dlatch", "$adlatch", "$dlatchsr", "$sr"),
                    FF_REASON),
}
MUTANTS = ("invert-first",)


def odin_script(files: Sequence[str], args: argparse.Namespace, work: Path, lowered: bool,
                vectors: Path) -> str:
    """The Odin III ``-p`` script of one run."""
    front = lc.read_front(files, args.top, str(Path(args.techlib).resolve()) if args.techlib
                          else None)
    pre = list(args.pre or DEFAULT_PRE) if len(files) != 1 or not files[0].endswith(".blif") else []
    steps = [*front, *pre]
    if not lowered:
        steps.append(f"write_rtlil {work / 'before.il'}")
    else:
        steps.append(f"lower {args.lower_args}".strip())
        if args.mutant:
            steps += [f"write_blif {work / 'lowered.blif'}"]
    return "; ".join([*steps, f"sim {args.vectors} {args.seed} {vectors}"])


def mutate(blif: str, how: str) -> str:
    """The lowered BLIF with one injected fault (a test hook). ``invert-first``: every cover row
    of the first ``.names`` block gets the opposite output value, so its function is inverted."""
    if how != "invert-first":
        raise lc.ToolError(f"unknown mutant '{how}': one of {MUTANTS}")
    out: list[str] = []
    in_first = False
    seen = False
    for line in blif.splitlines():
        if line.startswith(".names"):
            in_first = not seen
            seen = True
        elif line.startswith("."):
            in_first = False
        elif in_first and line.strip() and line.rstrip()[-1] in "01":
            flipped = "0" if line.rstrip()[-1] == "1" else "1"
            line = line.rstrip()[:-1] + flipped
        out.append(line)
    if not seen:
        raise lc.ToolError("the lowered BLIF has no .names block to mutate")
    return "\n".join(out) + "\n"


class Sim:
    """One design before and after."""

    def __init__(self, args: argparse.Namespace, work: Path) -> None:
        self.args = args
        self.work = work
        self.cap: int = args.mem_cap

    def odin(self, script: str, run_dir: Path) -> tuple[int, str]:
        done = lc.capped("odin3", [lc.exe(self.args.odin3), *lc.odin_args(self.args.top), "-p",
                                   script], self.cap, cwd=run_dir)
        return done.returncode, (done.stdout or "") + (done.stderr or "")

    def before(self, files: Sequence[str], run_dir: Path) -> tuple[set[str], str | None]:
        """(the cell types of the design, the vector text or None when it cannot be simulated)."""
        vec = run_dir / "before.vec"
        script = odin_script(files, self.args, run_dir, False, vec)
        code, log = self.odin(script, run_dir)
        il = run_dir / "before.il"
        types = lc.cell_types(il.read_text(encoding="utf-8")) if il.exists() else set()
        skipped = sorted(types & SKIP_REASONS.keys())
        if code != 0 and skipped:
            return types, None
        if code != 0:
            raise lc.ToolError(f"odin3 (before) exit {code}: {lc.why(log)}")
        if skipped:  # simulates (the word cell is a black box that happens to work): no verdict
            return types, None
        return types, vec.read_text(encoding="utf-8")

    def after(self, files: Sequence[str], run_dir: Path) -> str:
        """The vector text of the lowered design, '' with a reason when it does not simulate."""
        vec = run_dir / "after.vec"
        code, log = self.odin(odin_script(files, self.args, run_dir, True, vec), run_dir)
        if code != 0:
            raise AfterError(f"the lowered design does not simulate: {lc.why(log)}")
        if self.args.mutant:
            blif = run_dir / "lowered.blif"
            mutated = run_dir / "mutated.blif"
            mutated.write_text(mutate(blif.read_text(encoding="utf-8"), self.args.mutant),
                               encoding="utf-8")
            code, log = self.odin(f"read_blif {mutated}; sim {self.args.vectors} {self.args.seed} "
                                  f"{vec}", run_dir)
            if code != 0:
                raise AfterError(f"the mutated design does not simulate: {lc.why(log)}")
        return vec.read_text(encoding="utf-8")


class AfterError(Exception):
    """The lowered design cannot be simulated although the original can: a difference."""


def first_difference(before: str, after: str) -> str:
    """Where two vector texts first differ, for the message."""
    left, right = before.splitlines(), after.splitlines()
    for number, (lhs, rhs) in enumerate(zip(left, right, strict=False), 1):
        if lhs != rhs:
            return f"line {number}: before '{lhs}', after '{rhs}'"
    return f"the files have {len(left)} and {len(right)} lines"


def classify(sim: Sim, files: Sequence[str], run_dir: Path) -> tuple[list[str], str]:
    """(the output lines of one design, its class: ``equal``, ``differs``, ``skipped`` or ``empty``:
    no word cell, so nothing was compared)."""
    name = Path(files[0]).name
    types, before = sim.before(files, run_dir)
    skipped = sorted(types & SKIP_REASONS.keys())
    if before is None:
        lines = [f"{TOOL}: {name}: {t}: skipped ({SKIP_REASONS.get(t, 'design skipped')})"
                 for t in sorted(types)]
        if lines:
            return lines, "skipped"
        return [f"{TOOL}: {name}: NOTHING TO COMPARE: no cell to judge"], "empty"
    if not any(t.startswith("$") and not t.startswith("$_") for t in types):
        return [f"{TOOL}: {name}: NOTHING TO COMPARE: no word cell left after "
                f"'{'; '.join(sim.args.pre or DEFAULT_PRE)}' (folded away?)"], "empty"
    try:
        after = sim.after(files, run_dir)
    except AfterError as exc:
        return [f"{TOOL}: {name}: DIFFERS: {exc}"], "differs"
    if before != after:
        return [f"{TOOL}: {name}: DIFFERS: {first_difference(before, after)}"], "differs"
    assert not skipped
    cycles = f"{sim.args.vectors} cycles, seed {sim.args.seed}"
    return [f"{TOOL}: {name}: {t}: pass" for t in sorted(types)] + [
        f"{TOOL}: {name}: sample points: outputs after each cycle's active edge has settled "
        f"({cycles})"], "equal"


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--vectors", type=int, default=DEFAULT_VECTORS, help="cycles (default 64)")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--pre", action="append", metavar="CMD",
                    help="a command between hierarchy and the simulation (default proc, opt)")
    ap.add_argument("--lower-args", default="", metavar="A", help="arguments of the lower pass")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first")
    ap.add_argument("--mem-cap", type=lc.parse_size, default=lc.DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 3G)")
    ap.add_argument("--work", type=Path, help="the work directory")
    ap.add_argument("--mutant", choices=MUTANTS, help=argparse.SUPPRESS)
    ap.add_argument("--allow-empty", action="store_true",
                    help="a design with no word cell to compare is not a failure")
    ap.add_argument("--each", action="store_true", help="every FILE is its own design")
    ap.add_argument("files", nargs="+", help="the Verilog (or BLIF) files")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    designs = [[f] for f in files] if args.each else [files]
    sim = Sim(args, work)
    worst = lc.EXIT_OK
    for number, design in enumerate(designs):
        run_dir = work / f"d{number:04d}"
        run_dir.mkdir(exist_ok=True)
        lines, kind = classify(sim, design, run_dir)
        for line in lines:
            print(line, flush=True)
        empty_fails = kind == "empty" and not args.allow_empty
        worst = lc.EXIT_FAIL if kind == "differs" or empty_fails else worst
    return worst


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    work = lc.make_work(args.work, "lower-sim-")
    try:
        return execute(args, work)
    except lc.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
        return lc.EXIT_CRASH
    except (OSError, lc.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return lc.EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
