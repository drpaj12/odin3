#!/usr/bin/env python3
"""Report every term a LaTeX paper uses before its definition.

Usage:
    python3 docs/paper/check_terms.py --tex docs/paper/<paper>.tex [--terms docs/paper/terms.md]
                                      [--top N] [--report-only]

terms.md holds one table row per term:

    | term | definition | defined at | first used at | depends on | search |

``defined at`` is ``file:line`` (relative to the .tex directory) or ``none``. ``search`` is an
optional regular expression (case-insensitive) that finds the term in the text; the default is
the term itself. A literal ``|`` inside a cell is written ``\\|``. The checker expands
``\\input`` in reading order, strips comments, then reports

  * USED-BEFORE-DEFINED: first use precedes the defining line;
  * NEVER-DEFINED: the term occurs but has no definition (``none``);
  * BAD-DEFINITION: the ``defined at`` line does not contain the term;
  * DEPENDS-ORDER: a term is defined before a term it depends on;
  * STALE: ``first used at`` in terms.md differs from the computed first use (informational).

Front matter is exempt: everything before the first ``\\section`` (title, abstract, keywords; an
abstract may use a term before the body defines it).

Exit status is 1 when any of the first four kinds is found, unless ``--report-only`` is given.
Standard library only.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

INPUT_RE = re.compile(r"\\input\{([^}]+)\}")
STRIP_RE = re.compile(
    r"\\(?:cite|ref|label|eqref|url|includegraphics|input|bibliography)\w*(?:\[[^\]]*\])?\{[^}]*\}"
)


@dataclass(frozen=True)
class Line:
    """One text line of the flattened paper."""

    file: str
    number: int
    text: str
    in_abstract: bool = False


@dataclass
class Term:
    """One row of terms.md."""

    name: str
    definition: str
    defined_at: str
    first_used_at: str
    depends_on: list[str]
    search: str

    def pattern(self) -> re.Pattern[str]:
        """Compiled search pattern."""
        expr = self.search if self.search else re.escape(self.name)
        return re.compile(expr, re.IGNORECASE)


@dataclass
class Report:
    """Result of one check."""

    used_before_defined: list[tuple[Term, int, Line, Line | None]] = field(default_factory=list)
    never_defined: list[tuple[Term, int, Line]] = field(default_factory=list)
    bad_definition: list[tuple[Term, str]] = field(default_factory=list)
    depends_order: list[tuple[Term, str]] = field(default_factory=list)
    stale: list[tuple[Term, str]] = field(default_factory=list)

    def failures(self) -> int:
        """Count of findings that fail the check."""
        return (
            len(self.used_before_defined)
            + len(self.never_defined)
            + len(self.bad_definition)
            + len(self.depends_order)
        )


def strip_comment(text: str) -> str:
    """Remove an unescaped LaTeX comment from one line."""
    return re.sub(r"(?<!\\)%.*$", "", text)


def flatten(tex: Path) -> list[Line]:
    """Expand \\input recursively, in reading order, with comments stripped."""
    base = tex.parent
    out: list[Line] = []
    state = {"abstract": True, "nocite": False}

    def walk(path: Path) -> None:
        rel = path.relative_to(base).as_posix()
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            text = strip_comment(raw)
            if "\\nociteall{" in text or "\\nocite{" in text:
                state["nocite"] = "}" not in text
                continue
            if state["nocite"]:
                state["nocite"] = "}" not in text
                continue
            if "\\section{" in text or "\\section*{" in text:
                state["abstract"] = False
            match = INPUT_RE.search(text)
            if match:
                name = match.group(1)
                target = base / (name if name.endswith(".tex") else name + ".tex")
                if target.is_file() and not name.startswith("IMAGE/"):
                    walk(target)
                    continue
                if name.startswith("IMAGE/"):
                    continue
            out.append(Line(rel, number, STRIP_RE.sub("", text), state["abstract"]))

    walk(tex)
    return out


def parse_terms(md: Path) -> list[Term]:
    """Read the term table rows (lines starting with '|', header and rule rows skipped)."""
    terms: list[Term] = []
    for raw in md.read_text(encoding="utf-8").splitlines():
        if not raw.startswith("|"):
            continue
        cells = [
            c.strip().replace("\\|", "|") for c in re.split(r"(?<!\\)\|", raw.strip().strip("|"))
        ]
        if len(cells) < 5 or cells[0].lower() == "term" or set(cells[0]) <= {"-", ":", " "}:
            continue
        deps = [d.strip() for d in cells[4].split(";") if d.strip() and d.strip() != "-"]
        search = cells[5].strip("`") if len(cells) > 5 else ""
        terms.append(
            Term(cells[0], cells[1], cells[2].strip("`"), cells[3].strip("`"), deps, search)
        )
    return terms


def locate(lines: list[Line], where: str) -> int | None:
    """Index in the flattened text of a ``file:line`` reference, or None."""
    if ":" not in where:
        return None
    name, _, num = where.rpartition(":")
    if not num.isdigit():
        return None
    for index, line in enumerate(lines):
        if line.file == name and line.number == int(num):
            return index
    return None


def first_use(lines: list[Line], term: Term) -> int | None:
    """Index of the first flattened line that matches the term (abstract lines are exempt)."""
    pat = term.pattern()
    for index, line in enumerate(lines):
        if not line.in_abstract and pat.search(line.text):
            return index
    return None


def count_uses(lines: list[Line], term: Term) -> int:
    """Number of flattened lines that match the term."""
    pat = term.pattern()
    return sum(1 for line in lines if not line.in_abstract and pat.search(line.text))


def check(lines: list[Line], terms: list[Term]) -> Report:
    """Compare first uses with definitions."""
    rep = Report()
    def_index: dict[str, int | None] = {}
    for term in terms:
        defined = None if term.defined_at.lower() == "none" else locate(lines, term.defined_at)
        def_index[term.name] = defined
        used = first_use(lines, term)
        if used is not None:
            here = f"{lines[used].file}:{lines[used].number}"
            if term.first_used_at and term.first_used_at != here:
                rep.stale.append((term, here))
        if term.defined_at.lower() == "none":
            if used is not None:
                rep.never_defined.append((term, count_uses(lines, term), lines[used]))
            continue
        if defined is None:
            rep.bad_definition.append((term, f"{term.defined_at} is not a line of the paper"))
            continue
        if not term.pattern().search(lines[defined].text):
            rep.bad_definition.append((term, f"{term.defined_at} does not contain the term"))
        if used is not None and used < defined:
            rep.used_before_defined.append((term, defined - used, lines[used], lines[defined]))
    by_name = {t.name: t for t in terms}
    for term in terms:
        mine = def_index.get(term.name)
        for dep in term.depends_on:
            if dep not in by_name:
                rep.depends_order.append((term, f"depends on unknown term '{dep}'"))
                continue
            theirs = def_index.get(dep)
            if theirs is None:
                rep.depends_order.append((term, f"its dependency '{dep}' has no definition"))
            elif mine is not None and theirs > mine:
                rep.depends_order.append((term, f"defined before its dependency '{dep}'"))
    rep.used_before_defined.sort(key=lambda r: -r[1])
    rep.never_defined.sort(key=lambda r: -r[1])
    return rep


def render(rep: Report, top: int) -> str:
    """Human-readable report."""
    out: list[str] = []
    out.append(f"USED-BEFORE-DEFINED: {len(rep.used_before_defined)}")
    for term, gap, used, defined in rep.used_before_defined[:top]:
        where = f"{defined.file}:{defined.number}" if defined else "?"
        out.append(
            f"  {term.name}: first used {used.file}:{used.number}, "
            f"defined {where} ({gap} lines later)"
        )
    out.append(f"NEVER-DEFINED (used, no definition): {len(rep.never_defined)}")
    for term, uses, used in rep.never_defined[:top]:
        out.append(f"  {term.name}: first used {used.file}:{used.number}, {uses} lines use it")
    out.append(f"BAD-DEFINITION: {len(rep.bad_definition)}")
    for term, why in rep.bad_definition:
        out.append(f"  {term.name}: {why}")
    out.append(f"DEPENDS-ORDER: {len(rep.depends_order)}")
    for term, why in rep.depends_order[:top]:
        out.append(f"  {term.name}: {why}")
    out.append(f"STALE first-used-at (informational): {len(rep.stale)}")
    for term, here in rep.stale[:top]:
        out.append(f"  {term.name}: terms.md says {term.first_used_at or '-'}, text says {here}")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    """Command-line entry."""
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--tex", type=Path, required=True)
    parser.add_argument("--terms", type=Path, default=here / "terms.md")
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args(argv)
    lines = flatten(args.tex.resolve())
    rep = check(lines, parse_terms(args.terms))
    print(render(rep, args.top))
    return 0 if args.report_only or rep.failures() == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
