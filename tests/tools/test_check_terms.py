"""Tests for docs/paper/check_terms.py (stdlib only).

They build a small paper in a temporary directory; no LaTeX is needed."""

import tempfile
import unittest
from pathlib import Path

import check_terms as ct

MAIN = r"""\nociteall{keyone,
  gizmo}
\begin{abstract}
The LUT is used here, which is exempt.
\end{abstract}
\section{Intro}
The CLB holds many things. % a comment that mentions a gizmo
We place the netlist.
\input{body}
\input{IMAGE/fig}
"""
BODY = r"""A LUT is a look-up table.
A CLB is a block of LUTs.
The netlist is a list of gates.
"""
TERMS = """| term | definition | defined at | first used at | depends on | search |
|---|---|---|---|---|---|
| LUT | a table | body.tex:1 | - | - | `\\bLUTs?\\b` |
| CLB | a block | body.tex:2 | - | LUT | `\\bCLBs?\\b` |
| netlist | gates | body.tex:3 | - | - | |
| gizmo | never in text | none | - | - | |
| ghost | not at the line | body.tex:1 | - | - | |
"""


class CheckTerms(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "main.tex").write_text(MAIN, encoding="utf-8")
        (self.dir / "body.tex").write_text(BODY, encoding="utf-8")
        (self.dir / "IMAGE").mkdir()
        (self.dir / "IMAGE" / "fig.tex").write_text("the CLB figure\n", encoding="utf-8")
        (self.dir / "terms.md").write_text(TERMS, encoding="utf-8")
        self.lines = ct.flatten(self.dir / "main.tex")
        self.terms = ct.parse_terms(self.dir / "terms.md")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_flatten_order_and_flags(self) -> None:
        names = [f"{line.file}:{line.number}" for line in self.lines]
        self.assertLess(names.index("main.tex:6"), names.index("body.tex:1"))
        self.assertTrue(self.lines[1].in_abstract)
        self.assertNotIn("IMAGE/fig.tex:1", names)

    def test_comment_is_stripped(self) -> None:
        self.assertFalse(any("gizmo" in line.text for line in self.lines))

    def test_nociteall_block_is_skipped(self) -> None:
        self.assertFalse(any("keyone" in line.text for line in self.lines))

    def test_parse_terms(self) -> None:
        self.assertEqual([t.name for t in self.terms], ["LUT", "CLB", "netlist", "gizmo", "ghost"])
        self.assertEqual(self.terms[1].depends_on, ["LUT"])

    def test_alternation_in_search_is_unescaped(self) -> None:
        text = "| a | b | none | - | - | `x\\|y` |\n"
        path = self.dir / "alt.md"
        path.write_text(text, encoding="utf-8")
        term = ct.parse_terms(path)[0]
        self.assertEqual(term.search, "x|y")

    def test_abstract_use_is_exempt_and_use_before_definition_found(self) -> None:
        rep = ct.check(self.lines, self.terms)
        names = {t.name for t, _, _, _ in rep.used_before_defined}
        self.assertIn("CLB", names)  # used at main.tex:5, defined body.tex:2
        self.assertNotIn("LUT", names)  # the abstract use is exempt; body.tex:1 defines it
        self.assertIn("netlist", names)

    def test_bad_definition_and_failure_count(self) -> None:
        rep = ct.check(self.lines, self.terms)
        self.assertEqual([t.name for t, _ in rep.bad_definition], ["ghost"])
        self.assertGreaterEqual(rep.failures(), 3)

    def test_never_defined(self) -> None:
        terms = [ct.Term("CLB", "", "none", "", [], r"\bCLBs?\b")]
        rep = ct.check(self.lines, terms)
        self.assertEqual(len(rep.never_defined), 1)
        self.assertEqual(rep.never_defined[0][1], 2)

    def test_depends_order(self) -> None:
        terms = [
            ct.Term("CLB", "", "body.tex:2", "", ["LUT"], r"\bCLBs?\b"),
            ct.Term("LUT", "", "body.tex:3", "", [], r"\bLUTs?\b"),
        ]
        rep = ct.check(self.lines, terms)
        self.assertTrue(any("before its dependency" in why for _, why in rep.depends_order))

    def test_main_exit_status(self) -> None:
        args = ["--tex", str(self.dir / "main.tex"), "--terms", str(self.dir / "terms.md")]
        self.assertEqual(ct.main(args), 1)
        self.assertEqual(ct.main([*args, "--report-only"]), 0)


if __name__ == "__main__":
    unittest.main()
