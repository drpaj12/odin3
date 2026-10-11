"""Tests for tools/pp-diff: normalization, tokenization, the Yosys dump cut, classification and
the suite's input set, on fixed strings (no oracle is run)."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import pp_diff
from pp_diff import Kind, Outcome


def _ok(text: str) -> Outcome:
    return Outcome.accepted(pp_diff.tokenize(pp_diff.normalize(text)))


def _err(message: str) -> Outcome:
    return Outcome.rejected(message)


class NormalizeTest(unittest.TestCase):
    def test_pass_through_and_marker_lines_are_stripped(self) -> None:
        text = (
            '`file_push "a.v"\n'
            "`timescale 1ns / 1ps\n"
            "  `default_nettype none\n"
            '`line 3 "a.v" 0\n'
            "module m; endmodule\n"
            "`file_pop\n"
        )
        self.assertEqual(
            pp_diff.tokenize(pp_diff.normalize(text)), ["module", "m", ";", "endmodule"]
        )

    def test_directive_line_keeps_its_newline(self) -> None:
        self.assertEqual(pp_diff.normalize("`resetall x\ny\n"), "\ny\n")

    def test_every_pass_through_name_is_stripped(self) -> None:
        for name in pp_diff.STRIPPED:
            with self.subTest(name=name):
                self.assertEqual(pp_diff.normalize(f"`{name} 1 2\nz"), "\nz")

    def test_other_backtick_words_stay(self) -> None:
        self.assertEqual(pp_diff.normalize("a `UNDEF b\n"), "a `UNDEF b\n")

    def test_translate_regions_are_removed(self) -> None:
        text = (
            "a\n"
            "// synopsys translate_off\n"
            "b `timescale 1ns/1ps\n"
            "/* synthesis translate_on */ c\n"
            "//pragma translate_off \r\n"
            "d\n"
            "// pragma translate_on\n"
            "e\n"
        )
        self.assertEqual(pp_diff.tokenize(pp_diff.normalize(text)), ["a", "c", "e"])

    def test_translate_comment_must_be_exact(self) -> None:
        for comment in (
            "// Synopsys translate_off",
            "// synopsys translate_off now",
            "// translate_off",
            "/* synopsys translate_off",
        ):
            with self.subTest(comment=comment):
                text = comment + "\nx\n"
                expected = [] if comment.startswith("/*") else ["x"]
                self.assertEqual(pp_diff.tokenize(pp_diff.normalize(text)), expected)
        self.assertEqual(pp_diff.tokenize(pp_diff.normalize("/* synopsys translate_off */ x")), [])

    def test_translate_marker_inside_a_string_is_text(self) -> None:
        text = 'a "// synopsys translate_off" b\n'
        self.assertEqual(
            pp_diff.tokenize(pp_diff.normalize(text)), ["a", '"// synopsys translate_off"', "b"]
        )

    def test_unterminated_region_runs_to_the_end(self) -> None:
        self.assertEqual(
            pp_diff.tokenize(pp_diff.normalize("a // synopsys translate_off\nb\n")), ["a"]
        )

    def test_stray_translate_on_is_ignored(self) -> None:
        text = "a // synopsys translate_on\nb\n"
        self.assertEqual(pp_diff.tokenize(pp_diff.normalize(text)), ["a", "b"])


class TokenizeTest(unittest.TestCase):
    def test_whitespace_and_comments_dropped(self) -> None:
        text = "wire  /* c */ a ; // tail\n\tassign\r\na=b;"
        self.assertEqual(
            pp_diff.tokenize(text), ["wire", "a", ";", "assign", "a", "=", "b", ";"]
        )

    def test_numbers_and_based_literals(self) -> None:
        self.assertEqual(
            pp_diff.tokenize("8'hFF 8 'h ff 4'sb1_0x? 1.5e-3 12_3 'd7"),
            ["8", "'hFF", "8", "'hff", "4", "'sb1_0x?", "1.5e-3", "12_3", "'d7"],
        )

    def test_identifiers_escaped_system_and_backtick(self) -> None:
        self.assertEqual(
            pp_diff.tokenize("a$1 _b \\x+y  $display `M"),
            ["a$1", "_b", "\\x+y", "$display", "`M"],
        )

    def test_strings_are_one_token(self) -> None:
        self.assertEqual(pp_diff.tokenize('"a // b \\" c" d'), ['"a // b \\" c"', "d"])

    def test_operators_longest_match(self) -> None:
        self.assertEqual(
            pp_diff.tokenize("a<<<=b>>>c===d!==e**f&&g||h~&i~^j^~k+:l-:m->n<=o"),
            ["a", "<<<", "=", "b", ">>>", "c", "===", "d", "!==", "e", "**", "f", "&&", "g",
             "||", "h", "~&", "i", "~^", "j", "^~", "k", "+:", "l", "-:", "m", "->", "n",
             "<=", "o"],
        )

    def test_single_characters(self) -> None:
        self.assertEqual(pp_diff.tokenize("({[#@]})"), list("({[#@]})"))

    def test_unterminated_comment_and_string(self) -> None:
        self.assertEqual(pp_diff.tokenize("a /* b"), ["a"])
        self.assertEqual(pp_diff.tokenize('a "b\nc'), ["a", '"b', "c"])


class YosysDumpTest(unittest.TestCase):
    LOG = (
        "1. Executing Verilog-2005 frontend: a.v\n"
        "Parsing Verilog input from `a.v' to AST representation.\n"
        "-- Verilog code after preprocessor --\n"
        '`file_push "a.v"\n'
        "module m; endmodule\n"
        "`file_pop\n"
        "-- END OF DUMP --\n"
        "a.v:9: ERROR: syntax error\n"
    )

    def test_dump_is_cut(self) -> None:
        self.assertEqual(
            pp_diff.yosys_dump(self.LOG), '`file_push "a.v"\nmodule m; endmodule\n`file_pop\n'
        )

    def test_missing_dump(self) -> None:
        self.assertIsNone(pp_diff.yosys_dump("ERROR: include file not found\n"))

    def test_undefined_macro_left_in_the_dump_is_an_error(self) -> None:
        log = self.LOG.replace("module m;", "module m; wire w = `UNDEF;")
        outcome = pp_diff.yosys_outcome(log, "")
        self.assertFalse(outcome.ok)
        self.assertIn("`UNDEF", outcome.message)

    def test_dump_after_parse_error_is_accepted(self) -> None:
        outcome = pp_diff.yosys_outcome(self.LOG, "a.v:9: ERROR: syntax error")
        self.assertTrue(outcome.ok)
        self.assertEqual(outcome.tokens, ("module", "m", ";", "endmodule"))

    def test_no_dump_takes_the_error_line(self) -> None:
        outcome = pp_diff.yosys_outcome("", "noise\na.v:1: ERROR: Can't open include file\n")
        self.assertFalse(outcome.ok)
        self.assertEqual(outcome.message, "a.v:1: ERROR: Can't open include file")


class ClassifyTest(unittest.TestCase):
    def test_all_agree(self) -> None:
        kind, _ = pp_diff.classify(_ok("a b"), _ok("a  b"), _ok("a\nb"), None)
        self.assertIs(kind, Kind.AGREE)

    def test_all_reject(self) -> None:
        kind, _ = pp_diff.classify(_err("x"), _err("y"), _err("z"), None)
        self.assertIs(kind, Kind.AGREE)

    def test_odin3_differs_from_agreeing_oracles(self) -> None:
        kind, detail = pp_diff.classify(_ok("a c"), _ok("a b"), _ok("a b"), None)
        self.assertIs(kind, Kind.DIFFER)
        self.assertIn("token 1", detail)
        kind, detail = pp_diff.classify(_err("boom"), _ok("a"), _ok("a"), None)
        self.assertIs(kind, Kind.DIFFER)
        self.assertIn("boom", detail)
        kind, _ = pp_diff.classify(_ok("a"), _err("x"), _err("y"), None)
        self.assertIs(kind, Kind.DIFFER)

    def test_oracles_disagree(self) -> None:
        kind, detail = pp_diff.classify(_err("u"), _ok("a"), _err("y"), None)
        self.assertIs(kind, Kind.ORACLES_DISAGREE)
        self.assertIn("icarus accepts", detail)
        self.assertIn("odin3 = yosys", detail)
        kind, _ = pp_diff.classify(_ok("a"), _ok("a b"), _ok("a c"), None)
        self.assertIs(kind, Kind.ORACLES_DISAGREE)

    def test_declared(self) -> None:
        kind, _ = pp_diff.classify(
            _err("t5.v:2:26: error: syntax: directive inside macro text"),
            _ok("a"),
            _ok("a"),
            "directive inside macro text",
        )
        self.assertIs(kind, Kind.DECLARED)

    def test_declared_unmet(self) -> None:
        kind, _ = pp_diff.classify(_ok("a"), _ok("a"), _ok("a"), "directive inside macro text")
        self.assertIs(kind, Kind.DECLARED_UNMET)
        kind, _ = pp_diff.classify(_err("other"), _ok("a"), _ok("a"), "recursive macro")
        self.assertIs(kind, Kind.DECLARED_UNMET)

    def test_first_difference(self) -> None:
        self.assertEqual(pp_diff.first_difference(("a", "b"), ("a", "b")), None)
        self.assertEqual(pp_diff.first_difference(("a",), ("a", "b")), 1)
        self.assertEqual(pp_diff.first_difference(("x",), ("a",)), 0)


class SuiteTest(unittest.TestCase):
    def test_includes_are_covered_by_their_includer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "k").mkdir()
            (root / "g").mkdir()
            (root / "k" / "top.vh").write_text('`define W 3\n`include "../g/body.v"\n')
            (root / "g" / "body.v").write_text("wire [`W:0] a;\n")
            (root / "plain.v").write_text("module m; endmodule\n")
            (root / "c.v").write_text('// `include "plain.v"\n`define X\n')
            roots, covered = pp_diff.backtick_roots(root)
            self.assertEqual(roots, [root / "c.v", root / "k" / "top.vh"])
            self.assertEqual(covered, {root / "g" / "body.v": root / "k" / "top.vh"})

    def test_order_is_smallest_first_mcml_last(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "mcml.v").write_text("x")
            (root / "big.v").write_text("x" * 50)
            (root / "small.v").write_text("x" * 5)
            ordered = pp_diff.smallest_first([root / "mcml.v", root / "big.v", root / "small.v"])
            self.assertEqual([p.name for p in ordered], ["small.v", "big.v", "mcml.v"])


if __name__ == "__main__":
    unittest.main()
