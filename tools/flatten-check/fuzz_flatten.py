"""fuzz_flatten: random hierarchies through `flatten` (3A spec 14.5).

Usage::

    fuzz_flatten --cases N --seed S --odin3 O [--yosys Y] [--equiv] [--repro-dir DIR]

Case ``i`` is the hierarchy ``verilog_gen --flatten`` writes for seed ``S + i`` (top ``top``).
Legs:

1. ``flatten``: ``read_verilog; hierarchy; proc; flatten; check; write_rtlil`` must succeed (Debug
   builds run ``check`` full around the pass);
2. ``sim`` (spec 14.4): ``sim`` before and after ``flatten`` on the same vectors must be equal.
   The unflattened design is simulated first; only a design the simulator cannot run for a known
   reason (word flip-flops before ``lower``, tristate buffers) is ``skipped``, counted, never a
   pass. Any other failure of that run is a failure, and so is any failure of the ``flatten`` run
   once the unflattened design simulated (a design that simulated before must simulate after);
3. ``yosys`` (with ``--yosys``, spec 14.3): the flat module's public wire names and widths, public
   cell names and cell-type multiset against Yosys 0.55's ``flatten`` (``flatten_diff.compare``);
   with ``--equiv`` also Yosys's equivalence proof, except for a case that holds ``keep_hierarchy``
   (two renamed black boxes cannot be proved equal) or an ``inout`` tristate buffer (no SAT model).

A failing case is copied to ``DIR`` (default ``tests/fuzz/flatten``) as ``seed<N>.v`` with a
``// leg: ...`` first-line note and its path is printed: a reproducer, kept as a fixture once the
bug is fixed. Exit 0 every case ok, 1 a case failed, 2 usage or tool error, 77 without a memory
cap.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import flatten_common as fc
import flatten_diff as fd
import flatten_profile
import memcap

TOP = "top"
SIM_CYCLES = 16
SIM_SEED = 1
# The known reasons the unflattened design cannot be simulated: a word-level flip-flop or latch
# (`lower` comes later) or a tristate buffer. Anything else the simulator rejects is a failure.
SKIP_REASON = re.compile(
    r"cannot simulate `\$(?:\w*dff\w*|\w*latch\w*|tribuf|_TBUF_)`"
)


@dataclass
class Tally:
    """Counts over the cases of a run."""

    cases: int = 0
    sim_equal: int = 0
    sim_skipped: int = 0
    equiv_skipped: int = 0
    equiv_unproven: int = 0
    failures: list[str] = field(default_factory=list)


def odin_run(odin3: str, script: str) -> tuple[int, str]:
    """(exit code, stderr + stdout) of one capped Odin III run with the top given."""
    done = fc.run_capped([odin3, "--top", TOP, "-p", script])
    return done.returncode, (done.stderr or "") + (done.stdout or "")


def leg_flatten(odin3: str, source: Path, work: Path, sim_after: Path | None) -> str | None:
    """None when leg 1 passes, else the failure text. With ``sim_after`` the flat design is also
    simulated (the vectors go to that file) in the same run, so a design that simulated before
    flatten and fails to after fails here."""
    script = (
        f"read_verilog --nolatches {source}; hierarchy --top {TOP}; proc; flatten; check; "
        f"write_rtlil {work / 'flat.il'}"
    )
    if sim_after is not None:
        script += f"; sim {SIM_CYCLES} {SIM_SEED} {sim_after}"
    code, log = odin_run(odin3, script)
    return None if code == 0 else f"exit {code}: {log.strip()[-300:]}"


def leg_sim_before(odin3: str, source: Path, before: Path) -> str:
    """``ok`` (the vectors are in ``before``), ``skipped`` (a known reason the unflattened design
    cannot be simulated) or ``FAIL: ...``."""
    script = (
        f"read_verilog --nolatches {source}; hierarchy --top {TOP}; proc; "
        f"sim {SIM_CYCLES} {SIM_SEED} {before}"
    )
    code, log = odin_run(odin3, script)
    if code == 0:
        return "ok"
    if SKIP_REASON.search(log):
        return "skipped"
    return f"FAIL: the unflattened design failed to simulate (exit {code}): {log.strip()[-300:]}"


def leg_yosys(
    tools: fc.Tools, source: Path, work: Path, mode: tuple[bool, bool]
) -> str | None:
    """None when the names, cells and (with ``equiv``) the proof agree with Yosys. ``mode`` is
    (equiv, tristate): a tristate buffer is Yosys's ``$mux`` with a ``z`` input, so Odin III's
    ``$tribuf`` counts as a ``$mux``."""
    equiv, tristate = mode
    fold = {"$tribuf": "$mux"} if tristate else None
    result = fd.diff_in(work, source, TOP, tools, equiv, fold)
    if isinstance(result, str):
        return f"error: {result}"
    return "; ".join(result) if result else None


def yosys_failure(found: str | None, tally: Tally) -> str | None:
    """A proof that times out or aborts is no counterexample: it is counted and not a failure."""
    if found is not None and found.startswith("equiv: unproven"):
        tally.equiv_unproven += 1
        return None
    return found


def run_case(seed: int, args: argparse.Namespace, work: Path, tally: Tally) -> str | None:
    """Runs the legs of one seed; the failure text, or None. Updates ``tally``."""
    profile = flatten_profile.generate(seed)
    source = work / f"seed{seed}.v"
    source.write_text(profile.text, encoding="utf-8")
    before, after = work / "before.txt", work / "after.txt"
    verdict = leg_sim_before(args.odin3, source, before)
    if verdict.startswith("FAIL"):
        return f"sim: {verdict}"
    failure = leg_flatten(args.odin3, source, work, after if verdict == "ok" else None)
    if failure is not None:
        return f"flatten: {failure}"
    if verdict == "ok" and not (after.exists() and before.read_text() == after.read_text()):
        return "sim: FAIL: the simulation differs after flatten"
    tally.sim_equal += verdict == "ok"
    tally.sim_skipped += verdict == "skipped"
    if args.yosys:
        # Yosys cannot prove a design equal when it holds kept instances (two renamed black boxes)
        # or tristate buffers (no SAT model).
        unprovable = bool(profile.features & {"keep_module", "keep_inst", "inout"})
        tally.equiv_skipped += bool(args.equiv and unprovable)
        mode = (args.equiv and not unprovable, "inout" in profile.features)
        tools = fc.Tools(args.odin3, args.yosys)
        found = yosys_failure(leg_yosys(tools, source, work, mode), tally)
        if found is not None:
            return f"yosys: {found}"
    return None


def save_reproducer(seed: int, failure: str, repro_dir: Path) -> Path:
    """Copies the seed's case to the reproducer directory with the failure as a comment."""
    repro_dir.mkdir(parents=True, exist_ok=True)
    path = repro_dir / f"seed{seed}.v"
    note = failure.replace("\n", " ")[:300]
    path.write_text(f"// leg: {note}\n" + flatten_profile.generate(seed).text, encoding="utf-8")
    return path


def fuzz(args: argparse.Namespace) -> Tally:
    tally = Tally()
    for index in range(args.cases):
        seed = args.seed + index
        with tempfile.TemporaryDirectory() as tmp:
            failure = run_case(seed, args, Path(tmp), tally)
        tally.cases += 1
        if failure is not None:
            path = save_reproducer(seed, failure, args.repro_dir)
            tally.failures.append(f"seed {seed}: {failure} ({path})")
            print(tally.failures[-1])
    return tally


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(prog="fuzz_flatten", description=__doc__)
    parser.add_argument("--cases", type=int, default=100)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--odin3", default="build/debug/odin3")
    parser.add_argument("--yosys", help="the Yosys 0.55 binary (enables the yosys leg)")
    parser.add_argument("--equiv", action="store_true", help="with --yosys: the equivalence proof")
    parser.add_argument("--repro-dir", type=Path, default=Path("tests/fuzz/flatten"))
    args = parser.parse_args(argv)
    try:
        tally = fuzz(args)
    except memcap.CapUnavailable as exc:
        print(f"fuzz_flatten: skipped: {exc}")
        return fc.EXIT_SKIP
    print(
        f"fuzz_flatten: {tally.cases} cases, {len(tally.failures)} failed; sim equal "
        f"{tally.sim_equal}, sim skipped {tally.sim_skipped}"
        + (
            f", equivalence skipped {tally.equiv_skipped} (keep_hierarchy, inout), "
            f"unproven {tally.equiv_unproven}"
            if args.equiv
            else ""
        )
    )
    return fc.EXIT_DIFF if tally.failures else fc.EXIT_SAME


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
