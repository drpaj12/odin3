"""flatten-golden: the flat top module of Odin III's `flatten` against committed expected files
(3A spec 14.2) and the writers' printing of its aliased nets (spec 16, item 2).

For each fixture ``tests/flatten/NAME.v`` (or ``.blif``) Odin III runs ``read_*; [hierarchy --top T;
proc;] flatten; check`` and writes RTLIL (not for a BLIF fixture: a ``.names`` cover has no RTLIL
form), BLIF and Verilog. The top module of each file is compared with
``tests/flatten/expected/NAME.{il,blif,v}`` (the unused modules ``flatten`` leaves in the design are
not compared); a second run must give byte-identical files. ``--write`` regenerates the expected
files: review them (against Yosys with ``flatten-diff``) before committing. Exit 0 equal, 1 a file
differs, 2 error, 77 without a memory cap.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import flatten_common as fc
import memcap

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = Path("tests/flatten/expected")
TOP_RTLIL = re.compile(r"^module \\(\S+)$")


FORMATS_TAG = re.compile(r"golden-formats:\s*([a-z ]+)")


def formats(source: Path) -> tuple[str, ...]:
    """The output suffixes for a fixture: a BLIF fixture has no RTLIL (``$sop``); a fixture says
    ``golden-formats: il v`` in a comment when a writer cannot print it (a ``$mem`` has no BLIF
    form before ``lower``)."""
    tag = FORMATS_TAG.search((ROOT / source).read_text())
    if tag is not None:
        return tuple(tag.group(1).split())
    return ("blif", "v") if source.suffix == ".blif" else ("il", "blif", "v")


def script(source: Path, top: str, out: Path) -> str:
    """The Odin III script of one fixture, writing ``out.<suffix>`` for each format."""
    if source.suffix == ".blif":
        read = f"read_blif {source}; flatten"
    else:
        read = f"read_verilog --nolatches {source}; hierarchy --top {top}; proc; flatten"
    writers = {"il": "write_rtlil", "blif": "write_blif", "v": "write_verilog"}
    writes = "; ".join(f"{writers[s]} {out}.{s}" for s in formats(source))
    return f"{read}; check; {writes}"


def top_part(text: str, suffix: str, top: str) -> str:
    """The lines of module ``top`` in a written file (``""`` when absent)."""
    start, stop = {
        "il": (f"module \\{top}", "end"),
        "blif": (f".model {top}", ".end"),
        "v": (f"module {top} (", "endmodule"),
    }[suffix]
    lines = text.splitlines()
    for first, line in enumerate(lines):
        if line == start or (suffix == "v" and line.startswith(start)):
            for last in range(first, len(lines)):
                if lines[last] == stop:
                    return "\n".join(lines[first : last + 1]) + "\n"
    return ""


def run_script(odin3: str, source: Path, top: str, work: Path, tag: str) -> dict[str, str] | str:
    """The written files by suffix, or an error text."""
    out = work / tag
    done = fc.run_capped([odin3, "-p", script(source, top, out)])
    if done.returncode != 0:
        return f"odin3 exit {done.returncode}: {(done.stderr or done.stdout).strip()[-300:]}"
    return {s: Path(f"{out}.{s}").read_text() for s in formats(source)}


def check_one(odin3: str, source: Path, top: str, write: bool) -> list[str] | str:
    """The problems of one fixture (empty: all equal), or an error text."""
    with tempfile.TemporaryDirectory() as tmp:
        first = run_script(odin3, source, top, Path(tmp), "one")
        second = run_script(odin3, source, top, Path(tmp), "two")
    if isinstance(first, str) or isinstance(second, str):
        return first if isinstance(first, str) else str(second)
    problems = [f"{s}: two runs differ" for s in first if first[s] != second[s]]
    for suffix, text in first.items():
        part = top_part(text, suffix, top)
        want = ROOT / EXPECTED / f"{source.stem}.{suffix}"
        if not part:
            problems.append(f"{suffix}: no module {top} in the output")
        elif write:
            want.parent.mkdir(parents=True, exist_ok=True)
            want.write_text(part)
        elif not want.is_file():
            problems.append(f"{suffix}: no expected file {want.name} (run with --write)")
        elif want.read_text() != part:
            problems.append(f"{suffix}: differs from {want.name}")
    return problems


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(prog="flatten-golden", description=__doc__)
    parser.add_argument("--top", default="top")
    parser.add_argument("--odin3", default="build/debug/odin3")
    parser.add_argument("--write", action="store_true", help="regenerate the expected files")
    parser.add_argument("files", nargs="+", type=Path, help="fixtures, relative to the repo root")
    args = parser.parse_args(argv)
    worst = fc.EXIT_SAME
    for source in args.files:
        try:
            result = check_one(str(Path(args.odin3).resolve()), source, args.top, args.write)
        except memcap.CapUnavailable as exc:
            print(f"flatten-golden: skipped: {exc}")
            return fc.EXIT_SKIP
        if isinstance(result, str):
            print(f"{source.name}: error: {result}")
            worst = max(worst, fc.EXIT_ERROR)
        elif result:
            print(f"{source.name}: DIFFERS")
            for problem in result:
                print(f"  {problem}")
            worst = max(worst, fc.EXIT_DIFF)
        else:
            print(f"{source.name}: {'written' if args.write else 'equal'}")
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
