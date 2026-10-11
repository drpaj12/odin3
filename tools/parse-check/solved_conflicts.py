#!/usr/bin/env python3
"""solved-conflicts: the solved-conflict allowlist of a Bison grammar (2C spec §3.1, VP-11).

"Zero conflicts" proves nothing on its own: a conflict Bison resolves by precedence or
associativity is silent. This tool runs `bison --report=solved` on the grammar and prints every
"Conflict between rule N and token T resolved as R." line normalised to

    <rule text> :: <token> :: <resolution>

with the rule text taken from the report's Grammar section (left side included), so renumbering
the rules changes nothing; the lines are sorted and de-duplicated (one conflict shows in several
states). With --expect it compares them with a committed file instead.

The lines alone cannot see a new context in which an operator token follows an operand: once
every operator rule meets every operator token, a rule such as `s: '#' e '*' ';'` resolves its
conflicts with lines already listed. So with --operand E (the operand nonterminal, whose
operator rules are `E: E <op> ... E`) the tool also requires that no precedence token (one of
a %left, %right or %nonassoc line) can follow E anywhere outside its operator rules: neither
FIRST of what follows it in a rule, nor, when that is nullable, the LALR lookaheads of the rule's
reduction (Bison's --report=lookaheads). Each violation is printed as
`<rule> :: <token> :: operator token after E` and fails the run.

Usage: solved-conflicts [--bison B] --grammar G [--operand E] (--expect F | --print)
Exit codes: 0 equal (or printed) and no violation, 1 different (a unified diff on stderr) or a
violation, 2 usage/input error.
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
_REDUCE_ITEM = re.compile(r"^\s+(\d+)\s+(?:\S+:|\|).*\u2022\s*\[(.*)\]\s*$")
_SYMBOL = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|[^\s,\[\]]+')
_PRECEDENCE = re.compile(r"^%(?:left|right|nonassoc)\b(.*)$", re.MULTILINE)
_TOKEN_DECL = re.compile(r"^%token\b(.*)$", re.MULTILINE)
_ALIAS = re.compile(r'([A-Za-z_]\w*)\s+("(?:[^"\\]|\\.)*")')
_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_EMPTY = ("ε", "%empty")
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


def precedence_tokens(grammar_text: str) -> set[str]:
    """The tokens of the %left/%right/%nonassoc lines, spelled as Bison's report spells them
    (a string alias for a named token that has one)."""
    text = _COMMENT.sub(" ", grammar_text)
    aliases: dict[str, str] = {}
    for decl in _TOKEN_DECL.finditer(text):
        aliases.update(_ALIAS.findall(decl.group(1)))
    tokens: set[str] = set()
    for line in _PRECEDENCE.finditer(text):
        for symbol in _SYMBOL.findall(line.group(1)):
            if not symbol.startswith("<"):
                tokens.add(aliases.get(symbol, symbol))
    return tokens


def _rules(report: str) -> dict[int, tuple[str, list[str]]]:
    """Rule number -> (lhs, rhs symbols) from the report's Grammar section."""
    rules: dict[int, tuple[str, list[str]]] = {}
    for number, text in rule_texts(report).items():
        lhs, _, rhs = text.partition(": ")
        rules[number] = (lhs, [s for s in _SYMBOL.findall(rhs) if s not in _EMPTY])
    return rules


def _first_sets(
    rules: dict[int, tuple[str, list[str]]],
) -> tuple[dict[str, set[str]], set[str]]:
    """FIRST of every nonterminal and the set of nullable ones (a fixpoint)."""
    first: dict[str, set[str]] = {lhs: set() for lhs, _ in rules.values()}
    nullable: set[str] = set()
    changed = True
    while changed:
        changed = False
        for lhs, rhs in rules.values():
            heads, empty = _first_of(rhs, first, nullable)
            if not heads <= first[lhs]:
                first[lhs] |= heads
                changed = True
            if empty and lhs not in nullable:
                nullable.add(lhs)
                changed = True
    return first, nullable


def _first_of(
    symbols: list[str], first: dict[str, set[str]], nullable: set[str]
) -> tuple[set[str], bool]:
    """FIRST of a symbol string and whether it derives the empty string."""
    heads: set[str] = set()
    for symbol in symbols:
        if symbol not in first:  # a terminal
            heads.add(symbol)
            return heads, False
        heads |= first[symbol]
        if symbol not in nullable:
            return heads, False
    return heads, True


def _reduce_lookaheads(report: str) -> dict[int, set[str]]:
    """Rule number -> the union of its reductions' LALR lookaheads in every state."""
    lookaheads: dict[int, set[str]] = {}
    for line in report.splitlines():
        match = _REDUCE_ITEM.match(line)
        if match is not None:
            tokens = set(_SYMBOL.findall(match.group(2)))
            lookaheads.setdefault(int(match.group(1)), set()).update(tokens)
    return lookaheads


def operator_violations(report: str, operand: str, operators: set[str]) -> list[str]:
    """Where an operator token can follow operand outside operand's own operator rules."""
    rules = _rules(report)
    first, nullable = _first_sets(rules)
    lookaheads = _reduce_lookaheads(report)
    texts = rule_texts(report)
    found: set[str] = set()
    for number, (lhs, rhs) in rules.items():
        is_operator = lhs == operand and len(rhs) > 1 and rhs[0] == operand
        for i, symbol in enumerate(rhs):
            if symbol != operand or (is_operator and i in (0, len(rhs) - 1)):
                continue
            after, empty = _first_of(rhs[i + 1 :], first, nullable)
            if empty:
                after = after | lookaheads.get(number, set())
            for token in sorted(after & operators):
                found.add(f"{texts[number]} :: {token} :: operator token after {operand}")
    return sorted(found)


def bison_report(bison: str, grammar: Path) -> str:
    """Runs bison --report=solved on grammar into a temporary directory; returns the report."""
    if not grammar.is_file():
        raise ToolError(f"{grammar}: no such grammar file")
    with tempfile.TemporaryDirectory(prefix="solved-conflicts-") as tmp:
        report = Path(tmp) / "grammar.output"
        cmd = [
            bison,
            "--report=solved,lookaheads",
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
    parser.add_argument(
        "--operand", help="the operand nonterminal whose context is guarded (see above)"
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--expect", type=Path, help="the committed allowlist to compare with")
    mode.add_argument("--print", action="store_true", help="print the normalised lines")
    args = parser.parse_args(argv)
    try:
        report = bison_report(args.bison, args.grammar)
        lines = normalise(report)
        violations: list[str] = []
        if args.operand is not None:
            operators = precedence_tokens(args.grammar.read_text(encoding="utf-8"))
            violations = operator_violations(report, args.operand, operators)
        sys.stderr.write("".join(line + "\n" for line in violations))
        if args.print:
            sys.stdout.write("".join(line + "\n" for line in lines))
            return 1 if violations else 0
        return max(compare(lines, args.expect), 1 if violations else 0)
    except ToolError as exc:
        print(f"solved-conflicts: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
