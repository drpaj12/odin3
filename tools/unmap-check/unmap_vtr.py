"""unmap-vtr: the VTR sweep of the unmap pass (v1.0.4, spec T-7).

Usage::

    unmap-vtr --odin3 BIN [--arch XML] [--expect FILE] [--timeout S] DESIGN.v...

Per design, in the order given (the sweep passes the 19 designs of
``tests/hardmap/vtr_designs.txt``, smallest first), Odin III runs the Phase 3 script of
``tools/hardmap-check/phase3_pre_abc.script`` up to ``hardmap`` (the arch is read by
``read_arch`` when ``--arch`` is given, else ``lib/vtr.o3lib``) and then ``unmap``. The tool
prints one row per design::

    name  hard_adder hard_mul hard_ram  add sub mul mem  declined cands

``hard_*`` are the hard cells unmap saw (the `unmap:` INFO line of P-4), the next four the word
cells it made, ``declined`` the groups left with a note and ``cands`` the candidate sets.
Equivalence is not checked (the Phase 3 exit rule: big designs are not proven equal).

A design must finish, and no group may be declined with reason ``cycle`` or ``cin_carry`` unless
the ``--expect`` file lists it: a line is ``<design> <reason> <why>``. A listed line that does not
occur is stale and fails (the lists shrink).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

TOOL = "unmap-vtr"
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SCRIPT = REPO / "tools" / "hardmap-check" / "phase3_pre_abc.script"
ADDER_LIB = REPO / "tests" / "hardmap" / "adder_lib.v"
TECHLIB = REPO / "lib" / "vtr.o3lib"
EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
DEFAULT_TIMEOUT = 7200.0
WATCHED_REASONS = ("cycle", "cin_carry")

INFO_LINE = re.compile(
    r"unmap: (\d+) adder cells to (\d+) \$add \+ (\d+) \$sub, (\d+) multiply to (\d+) \$mul "
    r"\((\d+) regrouped\), (\d+) RAM cells to (\d+) \$mem, (\d+) groups left"
)
DECLINED = re.compile(r"unmap: module '[^']*': (\w+) at '[^']*' not recovered: (\w+)")
CANDIDATE = re.compile(r"\(candidate set [^)]*\)")


@dataclass
class Row:
    """What one design gave: the P-4 counts and the reasons of the groups left."""

    name: str
    adder_cells: int = 0
    add: int = 0
    sub: int = 0
    multiply: int = 0
    mul: int = 0
    regrouped: int = 0
    ram_cells: int = 0
    mem: int = 0
    left: int = 0
    reasons: Counter[str] = field(default_factory=Counter)
    cands: int = 0
    error: str = ""


def parse_log(name: str, log: str) -> Row:
    """The row of a design from the text Odin III logged (the last unmap INFO line counts)."""
    row = Row(name)
    for match in INFO_LINE.finditer(log):
        numbers = [int(group) for group in match.groups()]
        (row.adder_cells, row.add, row.sub, row.multiply, row.mul, row.regrouped, row.ram_cells,
         row.mem, row.left) = numbers
    for match in DECLINED.finditer(log):
        row.reasons[match.group(2)] += 1
    row.cands = len(CANDIDATE.findall(log))
    return row


def script_text(design: Path, arch: Path | None) -> str:
    """The Phase 3 script up to ``hardmap``, then ``unmap``, as one ``-p`` script."""
    lines = [ln.strip() for ln in SCRIPT.read_text().splitlines()]
    commands = [ln for ln in lines if ln and not ln.startswith("#")]
    commands = commands[: commands.index("hardmap") + 1] + ["unmap"]
    text = ";".join(commands)
    if arch is not None:
        text = text.replace("read_techlib ", "read_arch ", 1)
    lib = arch if arch is not None else TECHLIB
    return (text.replace("@LIB@", str(lib)).replace("@ADDERLIB@", str(ADDER_LIB))
            .replace("@DESIGN@", str(design)))


def run_design(odin3: str, design: Path, arch: Path | None, timeout: float) -> Row:
    """Runs one design; a failed run is a row with ``error`` set."""
    try:
        proc = subprocess.run(
            [odin3, "-p", script_text(design, arch)],
            capture_output=True, text=True, timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        row = Row(design.stem)
        row.error = f"timed out after {timeout:.0f} s"
        return row
    row = parse_log(design.stem, proc.stderr)
    if proc.returncode != 0:
        row.error = f"odin3 exited {proc.returncode}: {proc.stderr.strip().splitlines()[-1:]}"
    return row


def format_row(row: Row) -> str:
    return (f"{row.name:<20} {row.adder_cells:>9} {row.multiply:>7} {row.ram_cells:>7}  "
            f"{row.add:>6} {row.sub:>5} {row.mul:>5} {row.mem:>5}  {row.left:>8} {row.cands:>5}")


def read_expect(path: Path | None) -> list[tuple[str, str]]:
    """The ``(design, reason)`` pairs of an expect file."""
    pairs: list[tuple[str, str]] = []
    if path is None:
        return pairs
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3:
            raise ValueError(f"{path}:{number}: need `<design> <reason> <why>`")
        pairs.append((parts[0], parts[1]))
    return pairs


def judge(rows: Sequence[Row], listed: Sequence[tuple[str, str]]) -> list[str]:
    """The failures: a design that failed, an unlisted watched reason, a stale listing."""
    failures: list[str] = []
    seen: set[tuple[str, str]] = set()
    for row in rows:
        if row.error:
            failures.append(f"{row.name}: {row.error}")
        for reason in WATCHED_REASONS:
            if row.reasons[reason] and (row.name, reason) not in listed:
                failures.append(f"{row.name}: {row.reasons[reason]} groups declined: {reason}")
            elif row.reasons[reason]:
                seen.add((row.name, reason))
    names = {row.name for row in rows}
    failures.extend(f"stale expect line: {name} {reason}" for name, reason in listed
                    if name in names and (name, reason) not in seen)
    return failures


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    parser.add_argument("--odin3", required=True)
    parser.add_argument("--arch", type=Path)
    parser.add_argument("--expect", type=Path)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("designs", nargs="+", type=Path)
    return parser.parse_args(list(argv))


def main(argv: Sequence[str]) -> int:
    try:
        args = parse_args(argv)
        listed = read_expect(args.expect)
    except (SystemExit, ValueError, OSError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    print(f"{'design':<20} {'hard_add':>9} {'hard_mul':>7} {'hard_ram':>7}  "
          f"{'add':>6} {'sub':>5} {'mul':>5} {'mem':>5}  {'declined':>8} {'cands':>5}", flush=True)
    rows: list[Row] = []
    for design in args.designs:
        row = run_design(args.odin3, design, args.arch, args.timeout)
        rows.append(row)
        print(format_row(row), flush=True)
    failures = judge(rows, listed)
    for failure in failures:
        print(f"{TOOL}: {failure}", file=sys.stderr)
    return EXIT_FAIL if failures else EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
