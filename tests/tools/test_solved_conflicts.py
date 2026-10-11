"""Tests for tools/parse-check/solved-conflicts (2C spec §3.1: the solved-conflict allowlist)."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

import solved_conflicts

from . import helpers

SOLVED = helpers.FIXTURES / "solved"
BISON = shutil.which("bison")


@unittest.skipIf(BISON is None, "bison not found")
class SolvedConflictsTest(unittest.TestCase):
    def run_tool(self, *argv: str) -> tuple[int, str, str]:
        return helpers.run_main(solved_conflicts.main, list(argv))

    def test_print_normalises_the_report(self) -> None:
        code, out, err = self.run_tool("--grammar", str(SOLVED / "expr.y"), "--print")
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(out, (SOLVED / "expr.expected").read_text())
        self.assertEqual(len(out.splitlines()), 4)

    def test_rule_order_does_not_matter(self) -> None:
        code, out, _ = self.run_tool("--grammar", str(SOLVED / "expr_reordered.y"), "--print")
        self.assertEqual(code, 0)
        self.assertEqual(out, (SOLVED / "expr.expected").read_text())

    def test_expect_equal_exits_0(self) -> None:
        code, out, err = self.run_tool(
            "--grammar", str(SOLVED / "expr.y"), "--expect", str(SOLVED / "expr.expected")
        )
        self.assertEqual((code, out, err), (0, "", ""))

    def test_missing_expected_line_exits_1_with_diff(self) -> None:
        lines = (SOLVED / "expr.expected").read_text().splitlines(keepends=True)
        with tempfile.TemporaryDirectory() as tmp:
            expect = Path(tmp) / "short.txt"
            expect.write_text("".join(lines[:3]))
            code, out, err = self.run_tool(
                "--bison", str(BISON), "--grammar", str(SOLVED / "expr.y"), "--expect", str(expect)
            )
        self.assertEqual((code, out), (1, ""))
        self.assertIn("+" + lines[3], err)
        self.assertIn("---", err)

    def test_missing_grammar_exits_2(self) -> None:
        code, _, err = self.run_tool("--grammar", str(SOLVED / "absent.y"), "--print")
        self.assertEqual(code, 2)
        self.assertIn("absent.y", err)

    def test_bison_failure_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            broken = Path(tmp) / "broken.y"
            broken.write_text("%%\ne: e '+' e | N ;\n")  # N is undeclared; e never terminates
            code, _, err = self.run_tool("--grammar", str(broken), "--print")
        self.assertEqual(code, 2)
        self.assertIn("bison", err)

    def test_empty_grammar_report_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plain = Path(tmp) / "plain.y"
            plain.write_text("%token N\n%%\ne: N | %empty ;\n")
            empty = Path(tmp) / "empty.txt"
            empty.write_text("")
            code, out, err = self.run_tool("--grammar", str(plain), "--expect", str(empty))
        self.assertEqual((code, out, err), (0, "", ""))

    def test_expect_and_print_are_exclusive(self) -> None:
        code, _, _ = self.run_tool("--grammar", str(SOLVED / "expr.y"))
        self.assertEqual(code, 2)


class NormaliseTest(unittest.TestCase):
    REPORT = """Grammar

    0 $accept: s $end

    1 s: s '+' s
    2  | %empty

    3 t: N


State 4

    Conflict between rule 1 and token '+' resolved as reduce (%left '+').
    Conflict between rule 1 and token '+' resolved as reduce (%left '+').
    Conflict between rule 3 and token "else" resolved as shift (LOWER_THAN_ELSE < "else").
"""

    def test_rule_texts_carry_their_left_side(self) -> None:
        self.assertEqual(
            solved_conflicts.rule_texts(self.REPORT),
            {0: "$accept: s $end", 1: "s: s '+' s", 2: "s: %empty", 3: "t: N"},
        )

    def test_lines_are_sorted_and_deduplicated(self) -> None:
        self.assertEqual(
            solved_conflicts.normalise(self.REPORT),
            [
                "s: s '+' s :: '+' :: reduce (%left '+')",
                't: N :: "else" :: shift (LOWER_THAN_ELSE < "else")',
            ],
        )


if __name__ == "__main__":
    unittest.main()
