"""flatten-sim: Odin III's simulator before and after `flatten` on the same random vectors
(3A spec 14.4). Classes: equal, differs, skipped (the simulator cannot run the design: never a
pass). Exit 0 when every file is equal, 1 when one differs, 2 on an error, 77 without a cap.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import flatten_common as fc
import memcap

DEFAULT_VECTORS = 64
DEFAULT_SEED = 1


def sim_vectors(
    source: Path, top: str, odin3: str, flat: bool, cycles: int, seed: int
) -> str | None:
    """The vector file text of one simulation, None when the simulator cannot run it."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "v.txt"
        steps = f"read_verilog --nolatches {source}; hierarchy --top {top}; proc; "
        steps += "flatten; " if flat else ""
        run = fc.run_capped([odin3, "-p", f"{steps}sim {cycles} {seed} {out}"])
        return out.read_text() if run.returncode == 0 and out.exists() else None


def classify(source: Path, top: str, odin3: str, cycles: int, seed: int) -> str:
    before = sim_vectors(source, top, odin3, False, cycles, seed)
    after = sim_vectors(source, top, odin3, True, cycles, seed)
    if before is None or after is None:
        return "skipped:sim-failed"
    return "equal" if before == after else "differs"


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(prog="flatten-sim", description=__doc__)
    parser.add_argument("--top", default="top")
    parser.add_argument("--odin3", default="build/debug/odin3")
    parser.add_argument("--vectors", type=int, default=DEFAULT_VECTORS)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args(argv)
    worst = fc.EXIT_SAME
    for source in args.files:
        try:
            verdict = classify(source, args.top, args.odin3, args.vectors, args.seed)
        except memcap.CapUnavailable as exc:
            print(f"flatten-sim: skipped: {exc}")
            return fc.EXIT_SKIP
        print(f"{source.name}: {verdict}")
        if verdict == "differs":
            worst = max(worst, fc.EXIT_DIFF)
        elif verdict != "equal":
            worst = max(worst, fc.EXIT_ERROR)
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
