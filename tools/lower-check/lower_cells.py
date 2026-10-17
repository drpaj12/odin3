"""lower-cells: the per-cell goldens of `lower` (3B spec 14.2; plan Task 3 step 3).

Usage::

    lower-cells --odin3 O [--netlist-compare N] [--update] [--mem-cap C] [--work DIR] DIR_OR_FILE...

For every ``X.v`` (a directory contributes its ``*.v`` files, smallest first): Odin III runs
``read_verilog --nolatches X.v; hierarchy; proc; write_rtlil in.il; lower; write_blif out.blif``
(no ``opt``: the cell under test must survive; the flip-flop fixtures run ``proc; opt``, which
infers the enable and synchronous reset). The committed ``X.il`` (the input of ``lower``,
which ``lower-diff`` hands to Yosys) must equal ``in.il`` byte for byte, and the committed
``X.blif`` must be ``netlist-compare``-identical to ``out.blif``. ``--update`` writes both next to
the ``.v`` instead (overwriting, never removing); review the diff against Yosys with ``lower-diff``
before committing. Exit codes as the other lower-check tools: 0 every golden matches (or was
written), 1 one differs or is missing, 2 usage or a tool error, 3 hard (a child crashed, was killed
or timed out), 77 skipped. The work directory is made under ``$ODIN3_LOWER_WORK`` (else
``$TMPDIR``) and is never removed.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

import lower_common as lc
import memcap

TOOL = "lower-cells"
HERE = Path(__file__).resolve().parent
NETLIST_COMPARE = HERE.parent / "netlist-compare" / "netlist-compare"


# The fixtures that need ``opt`` after ``proc``: a flip-flop's enable and synchronous reset are
# folded into the cell by ``opt`` (``$dffe``, ``$sdff``, ...); ``proc`` alone leaves ``$dff`` and
# muxes.
FF_STEMS = frozenset({"dff", "dffe", "sdff", "sdffe", "sdffce", "adff", "adffe", "aldff", "aldffe",
                      "dffsr", "dffsre"})


def pre_of(source: Path) -> str:
    """The Odin III commands between ``hierarchy`` and ``lower``: ``proc``, plus ``opt`` for a
    flip-flop."""
    return "proc; opt" if source.stem.split("_")[0] in FF_STEMS else "proc"


def odin_script(source: Path, work: Path) -> str:
    return "; ".join([f"read_verilog --nolatches {lc.quote(str(source))}", "hierarchy",
                      pre_of(source),
                      f"write_rtlil {work / 'in.il'}", "lower", f"write_blif {work / 'out.blif'}"])


def collect(paths: Sequence[str]) -> list[Path]:
    """The ``.v`` files named, a directory's ``*.v`` included, smallest first (then by name)."""
    found: list[Path] = []
    for name in paths:
        path = Path(name).resolve()
        found += sorted(path.glob("*.v")) if path.is_dir() else [path]
    return sorted(found, key=lambda p: (p.stat().st_size, p.name))


class Cells:
    def __init__(self, args: argparse.Namespace, work: Path) -> None:
        self.args = args
        self.work = work

    def run_odin(self, source: Path, run_dir: Path) -> None:
        done = lc.capped("odin3", [lc.exe(self.args.odin3), "-p", odin_script(source, run_dir)],
                         self.args.mem_cap, cwd=run_dir)
        if done.returncode != 0:
            raise lc.ToolError(f"odin3 exit {done.returncode} on {source.name}: "
                               f"{lc.why((done.stdout or '') + (done.stderr or ''))}")

    def compare(self, source: Path, run_dir: Path) -> list[str]:
        """The differences of one fixture from its committed goldens (empty: it matches)."""
        problems: list[str] = []
        golden_il, golden_blif = source.with_suffix(".il"), source.with_suffix(".blif")
        if not golden_il.exists() or not golden_blif.exists():
            return [f"{source.name}: no committed .il/.blif (run with --update)"]
        if golden_il.read_text(encoding="utf-8") != (run_dir / "in.il").read_text(encoding="utf-8"):
            problems.append(f"{source.name}: in.il differs from {golden_il.name}")
        argv = [self.args.netlist_compare, "--quiet", "--implicit-models", str(golden_blif),
                str(run_dir / "out.blif")]
        done = lc.capped("netlist-compare", argv, self.args.mem_cap, cwd=run_dir)
        if done.returncode == 1:
            problems.append(f"{source.name}: out.blif is not identical to {golden_blif.name}")
        elif done.returncode != 0:
            raise lc.ToolError(f"netlist-compare exit {done.returncode} on {source.name}: "
                               f"{lc.why((done.stdout or '') + (done.stderr or ''))}")
        return problems

    def one(self, source: Path, number: int) -> list[str]:
        run_dir = self.work / f"c{number:04d}"
        run_dir.mkdir(exist_ok=True)
        self.run_odin(source, run_dir)
        if self.args.update:
            shutil.copyfile(run_dir / "in.il", source.with_suffix(".il"))
            shutil.copyfile(run_dir / "out.blif", source.with_suffix(".blif"))
            return []
        return self.compare(source, run_dir)


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--netlist-compare", default=str(NETLIST_COMPARE), metavar="N")
    ap.add_argument("--update", action="store_true", help="write the .il and .blif goldens")
    ap.add_argument("--mem-cap", type=lc.parse_size, default=lc.DEFAULT_CAP, metavar="SIZE")
    ap.add_argument("--work", type=Path, help="the work directory")
    ap.add_argument("paths", nargs="+", help="fixture .v files or directories of them")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    sources = collect(args.paths)
    if not sources:
        raise lc.ToolError("no .v fixture found")
    cells = Cells(args, work)
    problems: list[str] = []
    for number, source in enumerate(sources):
        problems += cells.one(source, number)
    for line in problems:
        print(f"{TOOL}: {line}")
    verb = "wrote goldens for" if args.update else "checked"
    print(f"{TOOL}: {verb} {len(sources)} fixtures, {len(problems)} problems")
    return lc.EXIT_FAIL if problems else lc.EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    work = lc.make_work(args.work, "lower-cells-")
    try:
        return execute(args, work)
    except lc.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        return lc.EXIT_CRASH
    except (OSError, lc.ToolError, ValueError, subprocess.SubprocessError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return lc.EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
