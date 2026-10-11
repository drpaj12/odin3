#!/usr/bin/env python3
"""solved-conflicts: the solved-conflict allowlist of a Bison grammar (2C spec §3.1, VP-11).

"Zero conflicts" proves nothing on its own: a conflict Bison resolves by precedence or
associativity is silent. This tool runs `bison --report=solved` on the grammar and prints every
"Conflict between rule N and token T resolved as R." line normalised to

    <rule text> :: <token> :: <resolution>

with the rule text taken from the report's Grammar section (left side included), so renumbering
the rules changes nothing; the lines are sorted and de-duplicated (one conflict shows in several
states). With --expect it compares them with a committed file instead.

Usage: solved-conflicts [--bison B] --grammar G (--expect F | --print)
Exit codes: 0 equal (or printed), 1 different (a unified diff on stderr), 2 usage/input error.
"""

from __future__ import annotations

import argparse
import difflib
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

_RULE = re.compile(r"^\s+(\d+)\s+(?:(\S+):|\|)\s*(.*?)\s*$")
_SOLVED = re.compile(r"^\s*Conflict between rule (\d+) and token (.+) resolved as (.+)\.\s*$")


class ToolError(Exception):
    """A usage or input error (exit 2)."""


def rule_texts(report: str) -> dict[int, str]:
    """Rule number -> "lhs: rhs" from the Grammar section of a Bison report."""
    texts: dict[int, str] = {}
    lhs = ""
    in_grammar = False
    for line in report.splitlines():
        if line == "Grammar":
            in_grammar = True
            continue
        if in_grammar and line and not line[0].isspace():
            break  # the next section
        match = _RULE.match(line) if in_grammar else None
        if match is None:
            continue
        number, new_lhs, rhs = match.groups()
        lhs = new_lhs if new_lhs is not None else lhs
        texts[int(number)] = f"{lhs}: {rhs}"
    return texts


def normalise(report: str) -> list[str]:
    """The report's solved conflicts as sorted, unique "<rule> :: <token> :: <resolution>"."""
    texts = rule_texts(report)
    lines: set[str] = set()
    for line in report.splitlines():
        match = _SOLVED.match(line)
        if match is None:
            continue
        number, token, resolution = match.groups()
        if int(number) not in texts:
            raise ToolError(f"rule {number} is not in the report's Grammar section")
        lines.add(f"{texts[int(number)]} :: {token} :: {resolution}")
    return sorted(lines)


def bison_report(bison: str, grammar: Path) -> str:
    """Runs bison --report=solved on grammar into a temporary directory; returns the report."""
    if not grammar.is_file():
        raise ToolError(f"{grammar}: no such grammar file")
    with tempfile.TemporaryDirectory(prefix="solved-conflicts-") as tmp:
        report = Path(tmp) / "grammar.output"
        cmd = [
            bison,
            "--report=solved",
            f"--report-file={report}",
            "-o",
            str(Path(tmp) / "grammar.c"),
            str(grammar),
        ]
        try:
            done = subprocess.run(cmd, capture_output=True, text=True, check=False)
        except OSError as exc:
            raise ToolError(f"cannot run {bison}: {exc}") from exc
        if done.returncode != 0:
            raise ToolError(f"bison failed on {grammar}:\n{done.stderr.rstrip()}")
        return report.read_text(encoding="utf-8")


def compare(actual: list[str], expect: Path) -> int:
    """0 when the expected file holds exactly these lines, else 1 with a unified diff."""
    try:
        wanted = expect.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ToolError(f"{expect}: {exc.strerror}") from exc
    if wanted == actual:
        return 0
    diff = difflib.unified_diff(
        [line + "\n" for line in wanted],
        [line + "\n" for line in actual],
        fromfile=str(expect),
        tofile="bison --report=solved",
    )
    sys.stderr.write("".join(diff))
    return 1


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(prog="solved-conflicts", description=__doc__.splitlines()[0])
    parser.add_argument("--bison", default="bison", help="the bison executable (default: bison)")
    parser.add_argument("--grammar", required=True, type=Path, help="the .y file")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--expect", type=Path, help="the committed allowlist to compare with")
    mode.add_argument("--print", action="store_true", help="print the normalised lines")
    args = parser.parse_args(argv)
    try:
        lines = normalise(bison_report(args.bison, args.grammar))
        if args.print:
            sys.stdout.write("".join(line + "\n" for line in lines))
            return 0
        return compare(lines, args.expect)
    except ToolError as exc:
        print(f"solved-conflicts: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
