"""Tests for tests/verilog/coverage: one source per coverage-audit row (2C spec §7.2).

The row IDs come from the §4 tables of the coverage audit; rows.txt maps each to its file and to
the 2C task whose grammar completes it; the precedence- and slot-sensitive rows carry hand-written
`.ast` goldens; the forms of the 2C spec §8 review focus 4 are each present.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
COVERAGE = REPO_ROOT / "tests" / "verilog" / "coverage"
AUDIT = REPO_ROOT / "docs" / "specs" / "2026-10-09-2C-verilog-coverage.md"

ROW_COUNT = 159
GRAMMAR_TASKS = {4, 5, 6, 7}
PREPROCESSOR_TASK = 9
PREPROCESSOR_ROWS = {"L13", "L14", "L15", "L16", "L17"}


def _span(prefix: str, first: int, last: int) -> set[str]:
    return {f"{prefix}{i}" for i in range(first, last + 1)}


HAND_GOLDEN_ROWS = (
    _span("E", 1, 23)
    | _span("C", 4, 7)
    | {"F1"}
    | _span("G", 2, 6)
    | _span("K", 1, 3)
    | {"T1", "T2", "T11", "I1"}
    | _span("M", 1, 2)
    | _span("N", 7, 10)
)
HAND_MARK = "# hand-written"

# Review focus 4: each form a grammar could silently drop, as (row, substring).
FORMS = [
    ("M4", "localparam LW"),  # localparam in a parameter port list
    ("C7", "default y = b;"),  # default without a colon
    ("T18", "(* keep *) reg u;"),  # an attribute before a block-item declaration
    ("I6", "u[3:0] ("),  # an instance array
    ("T4", "@clk"),  # @ident without parentheses
    ("T11", "#(a+b)"),  # a parenthesised delay expression
    ("L22", 'endmodule\n`begin_keywords "1364-2001"'),  # a set opened inside another
]
KEYWORD_VERSIONS = ['"1364-1995"', '"1364-2001"', '"1364-2001-noconfig"', '"1364-2005"']

_STRIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\\\S+', re.S)
_UNIT_WORD = re.compile(r"\b(module|macromodule|endmodule)\b")
_FILE_REF = re.compile(r"@([^\s:]+):\d+:\d+")


def audit_rows() -> list[str]:
    """The row IDs of the audit's §4 construct tables, in table order."""
    text = AUDIT.read_text(encoding="utf-8")
    section = text.split("\n## 4. ", 1)[1].split("\n## 5. ", 1)[0]
    return re.findall(r"^\| ([A-Z]\d+) \|", section, re.M)


def read_rows() -> list[tuple[str, str, int]]:
    out = []
    for line in (COVERAGE / "rows.txt").read_text(encoding="utf-8").splitlines():
        row, name, task = line.split()
        out.append((row, name, int(task)))
    return out


def module_depths(source: str) -> list[int]:
    """The module nesting depth after each module/macromodule/endmodule word, outside comments,
    strings and escaped identifiers."""
    depth, out = 0, []
    for word in _UNIT_WORD.findall(_STRIP.sub(" ", source)):
        depth += -1 if word == "endmodule" else 1
        out.append(depth)
    return out


class CoverageRowsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rows = read_rows()
        self.by_row = {row: (name, task) for row, name, task in self.rows}

    def test_audit_has_159_rows(self) -> None:
        ids = audit_rows()
        self.assertEqual(len(ids), ROW_COUNT)
        self.assertEqual(len(set(ids)), ROW_COUNT)

    def test_rows_txt_lists_each_audit_row_once(self) -> None:
        listed = [row for row, _, _ in self.rows]
        self.assertEqual(sorted(listed), sorted(audit_rows()))
        self.assertEqual(len(set(name for _, name, _ in self.rows)), ROW_COUNT)

    def test_file_names_and_tasks(self) -> None:
        for row, name, task in self.rows:
            with self.subTest(row=row):
                self.assertRegex(name, rf"^{row}_[a-z0-9_]+\.v$")
                if row in PREPROCESSOR_ROWS:
                    self.assertEqual(task, PREPROCESSOR_TASK)
                else:
                    self.assertIn(task, GRAMMAR_TASKS)

    def test_every_source_is_listed(self) -> None:
        listed = {name for _, name, _ in self.rows}
        on_disk = {p.name for p in COVERAGE.glob("*.v")}
        self.assertEqual(on_disk, listed)

    def test_sources_open_with_header_and_balance_modules(self) -> None:
        for row, name, _ in self.rows:
            with self.subTest(row=row):
                path = COVERAGE / name
                self.assertTrue(path.is_file(), name)
                source = path.read_text(encoding="utf-8")
                first = source.split("\n", 1)[0]
                self.assertTrue(first.startswith(f"// {row}: "), first)
                self.assertIn(" — variants: ", first)
                depths = module_depths(source)
                self.assertTrue(depths, "no module")
                self.assertTrue(all(d in (0, 1) for d in depths), depths)
                self.assertEqual(depths[-1], 0)

    def test_hand_goldens_are_exactly_the_sensitive_rows(self) -> None:
        marked = set()
        for path in COVERAGE.glob("*.ast"):
            row = path.name.split("_", 1)[0]
            self.assertIn(row, self.by_row, path.name)
            self.assertEqual(path.with_suffix(".v").name, self.by_row[row][0])
            lines = path.read_text(encoding="utf-8").splitlines()
            if lines and lines[0] == HAND_MARK:
                marked.add(row)
        self.assertEqual(marked, HAND_GOLDEN_ROWS)
        self.assertEqual(len(marked), 46)

    def test_ast_references_name_their_own_source(self) -> None:
        for path in COVERAGE.glob("*.ast"):
            with self.subTest(ast=path.name):
                refs = set(_FILE_REF.findall(path.read_text(encoding="utf-8")))
                self.assertEqual(refs, {path.with_suffix(".v").name})

    def test_review_focus_forms_are_present(self) -> None:
        for row, form in FORMS:
            with self.subTest(row=row, form=form):
                source = (COVERAGE / self.by_row[row][0]).read_text(encoding="utf-8")
                self.assertIn(form, source)

    def test_begin_keywords_versions_and_nesting(self) -> None:
        source = (COVERAGE / self.by_row["L22"][0]).read_text(encoding="utf-8")
        for version in KEYWORD_VERSIONS:
            self.assertIn(f"`begin_keywords {version}", source)
        depth, deepest = 0, 0
        for word in re.findall(r"`(begin_keywords|end_keywords)\b", _STRIP.sub(" ", source)):
            depth += 1 if word == "begin_keywords" else -1
            deepest = max(deepest, depth)
            self.assertGreaterEqual(depth, 0)
        self.assertEqual(depth, 0)
        self.assertGreaterEqual(deepest, 2)


if __name__ == "__main__":
    unittest.main()
