"""Tests for tools/pp-diff: normalization, tokenization, the Yosys dump cut, classification and
the suite's input set, on fixed strings (no oracle is run)."""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import pp_diff
from pp_diff import Expect, Kind, Outcome, Run

from . import helpers


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
        self.assertEqual(pp_diff.tokenize(text), ["wire", "a", ";", "assign", "a", "=", "b", ";"])

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
            [
                "a",
                "<<<",
                "=",
                "b",
                ">>>",
                "c",
                "===",
                "d",
                "!==",
                "e",
                "**",
                "f",
                "&&",
                "g",
                "||",
                "h",
                "~&",
                "i",
                "~^",
                "j",
                "^~",
                "k",
                "+:",
                "l",
                "-:",
                "m",
                "->",
                "n",
                "<=",
                "o",
            ],
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


_DECLARED = Expect(declared="directive inside macro text")


class ClassifyTest(unittest.TestCase):
    def test_all_agree(self) -> None:
        kind, _ = pp_diff.classify(_ok("a b"), _ok("a  b"), _ok("a\nb"))
        self.assertIs(kind, Kind.AGREE)

    def test_all_reject(self) -> None:
        kind, _ = pp_diff.classify(_err("x"), _err("y"), _err("z"))
        self.assertIs(kind, Kind.AGREE)

    def test_odin3_differs_from_agreeing_oracles(self) -> None:
        kind, detail = pp_diff.classify(_ok("a c"), _ok("a b"), _ok("a b"))
        self.assertIs(kind, Kind.DIFFER)
        self.assertIn("token 1", detail)
        kind, detail = pp_diff.classify(_err("boom"), _ok("a"), _ok("a"))
        self.assertIs(kind, Kind.DIFFER)
        self.assertIn("boom", detail)
        kind, _ = pp_diff.classify(_ok("a"), _err("x"), _err("y"))
        self.assertIs(kind, Kind.DIFFER)

    def test_crash_is_never_a_rejection(self) -> None:
        # review finding 1: a crash on a file every tool rejects must not read "all reject"
        kind, detail = pp_diff.classify(Outcome.crash("killed by signal 11"), _err("x"), _err("y"))
        self.assertIs(kind, Kind.CRASH)
        self.assertIn("odin3: killed by signal 11", detail)
        kind, detail = pp_diff.classify(_err("x"), Outcome.crash("timeout"), _err("y"))
        self.assertIs(kind, Kind.CRASH)
        self.assertIn("icarus", detail)
        kind, _ = pp_diff.classify(_ok("a"), _ok("a"), Outcome.crash("killed by signal 6"))
        self.assertIs(kind, Kind.CRASH)
        self.assertFalse(Outcome.crash("x").same(Outcome.crash("x")))

    def test_oracles_disagree_needs_the_expected_side(self) -> None:
        # review finding 2: the side Odin III matches is enforced against the table
        yosys = Expect(side=pp_diff.YOSYS)
        kind, detail = pp_diff.classify(_err("u"), _ok("a"), _err("y"), yosys)
        self.assertIs(kind, Kind.ORACLES_DISAGREE)
        self.assertIn("icarus accepts", detail)
        self.assertIn("odin3 = yosys", detail)
        kind, detail = pp_diff.classify(_ok("a"), _ok("a"), _err("y"), yosys)
        self.assertIs(kind, Kind.DISAGREE_UNEXPECTED)
        self.assertIn("odin3 = icarus, expected yosys", detail)
        kind, detail = pp_diff.classify(_ok("a"), _ok("a b"), _ok("a c"), Expect())
        self.assertIs(kind, Kind.DISAGREE_UNEXPECTED)
        self.assertIn("no ORACLES_DISAGREE row", detail)
        kind, _ = pp_diff.classify(_ok("a c"), _ok("a b"), _ok("a c"), yosys)
        self.assertIs(kind, Kind.ORACLES_DISAGREE)

    def test_disagree_table(self) -> None:
        for key, (side, reason) in pp_diff.ORACLES_DISAGREE.items():
            with self.subTest(key=key):
                self.assertIn(side, (pp_diff.ICARUS, pp_diff.YOSYS))
                self.assertTrue(reason)
                self.assertTrue((helpers.REPO_ROOT / key).is_file())
        for table in (pp_diff.DECLARED, pp_diff.ODIN3_ONLY):
            for key in table:
                self.assertTrue((helpers.REPO_ROOT / key).is_file(), key)

    def test_expectation(self) -> None:
        probes = helpers.REPO_ROOT / pp_diff.PROBES
        self.assertEqual(pp_diff.expectation(probes / "t9.v"), Expect(side=pp_diff.ICARUS))
        self.assertEqual(
            pp_diff.expectation(probes / "t5.v"),
            Expect(declared="syntax: directive inside macro text"),
        )
        self.assertEqual(pp_diff.expectation(probes / "t1.v"), Expect())

    def test_declared(self) -> None:
        msg = "t5.v:2:26: error: syntax: directive inside macro text"
        kind, _ = pp_diff.classify(_err(msg), _ok("a"), _ok("a"), _DECLARED)
        self.assertIs(kind, Kind.DECLARED)

    def test_declared_unmet(self) -> None:
        kind, _ = pp_diff.classify(_ok("a"), _ok("a"), _ok("a"), _DECLARED)
        self.assertIs(kind, Kind.DECLARED_UNMET)
        kind, _ = pp_diff.classify(_err("other"), _ok("a"), _ok("a"), _DECLARED)
        self.assertIs(kind, Kind.DECLARED_UNMET)

    def test_declared_needs_both_oracles_to_accept_alike(self) -> None:
        # review minor 7: the premise of a declared divergence is checked
        msg = "error: syntax: directive inside macro text"
        for icarus, yosys in ((_err("x"), _err("y")), (_ok("a"), _err("y")), (_ok("a"), _ok("b"))):
            with self.subTest(icarus=icarus, yosys=yosys):
                kind, detail = pp_diff.classify(_err(msg), icarus, yosys, _DECLARED)
                self.assertIs(kind, Kind.DECLARED_UNMET)
                self.assertIn("no longer both accept", detail)

    def test_odin3_only(self) -> None:
        # review minor 9: rec.v runs Odin III alone
        expected = "syntax: recursive macro 'R'"
        msg = "rec.v:2:26: error: syntax: recursive macro 'R'"
        self.assertIs(pp_diff.classify_odin3_only(_err(msg), expected)[0], Kind.DECLARED)
        self.assertIs(pp_diff.classify_odin3_only(_ok("a"), expected)[0], Kind.DECLARED_UNMET)
        crash = Outcome.crash("timeout after 120 s")
        self.assertIs(pp_diff.classify_odin3_only(crash, expected)[0], Kind.CRASH)

    def test_first_difference(self) -> None:
        self.assertEqual(pp_diff.first_difference(("a", "b"), ("a", "b")), None)
        self.assertEqual(pp_diff.first_difference(("a",), ("a", "b")), 1)
        self.assertEqual(pp_diff.first_difference(("x",), ("a",)), 0)


class OutcomeTest(unittest.TestCase):
    LOCATED = "odin3: error: /x/a.v:3:9: error: syntax: stray backtick\n"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.stream = Path(self._tmp.name) / "0.v"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_odin3_accepts_with_a_stream(self) -> None:
        self.stream.write_text("module m; endmodule\n")
        outcome = pp_diff.odin3_outcome(Run(0, "", ""), self.stream)
        self.assertEqual(outcome, Outcome.accepted(["module", "m", ";", "endmodule"]))

    def test_odin3_rejects_only_by_exit_1_with_a_located_error(self) -> None:
        outcome = pp_diff.odin3_outcome(Run(1, "", self.LOCATED), self.stream)
        self.assertEqual(outcome, Outcome.rejected(self.LOCATED.strip()))
        unlocated = "odin3: error: include_not_found: 'x.vh'\n"
        self.assertFalse(pp_diff.odin3_outcome(Run(1, "", unlocated), self.stream).crashed)

    def test_odin3_abnormal_ends_are_crashes(self) -> None:
        # review finding 1
        runs = {
            "killed by signal 11": Run(-11, "", ""),
            "timeout after": Run(None, "", ""),
            "exit 3": Run(3, "", self.LOCATED),
            "exit 1, no located error": Run(1, "", "odin3: error: -p: command 1: failed\n"),
            "no stream": Run(0, "", ""),
            "exit 134": Run(134, "", "ERROR: AddressSanitizer: heap-use-after-free\n"),
        }
        for expected, run in runs.items():
            with self.subTest(expected=expected):
                outcome = pp_diff.odin3_outcome(run, self.stream)
                self.assertTrue(outcome.crashed)
                self.assertIn(expected, outcome.message)

    def test_odin3_stream_holding_translate_off_is_a_defect(self) -> None:
        # review minor 6: normalization would otherwise hide a PP-10 bug
        self.stream.write_text("a\n// synopsys translate_off\nb\n// synopsys translate_on\n")
        outcome = pp_diff.odin3_outcome(Run(0, "", ""), self.stream)
        self.assertTrue(outcome.crashed)
        self.assertIn("translate_off", outcome.message)

    def test_icarus_error_on_stderr_with_exit_0_is_a_rejection(self) -> None:
        # review minor 5
        self.stream.write_text("garbled\n")
        run = Run(0, "", "/x/u1.v:1: Unterminated comment in define\n")
        outcome = pp_diff.icarus_outcome(run, self.stream)
        self.assertFalse(outcome.ok)
        self.assertFalse(outcome.crashed)
        self.assertIn("Unterminated comment", outcome.message)

    def test_icarus_warning_is_not_a_rejection(self) -> None:
        self.stream.write_text("a\n")
        run = Run(0, "", "/x/t8.v:2: warning: macro UNDEF undefined (and assumed null)\n")
        self.assertEqual(pp_diff.icarus_outcome(run, self.stream), Outcome.accepted(["a"]))

    def test_icarus_signal_is_a_crash(self) -> None:
        self.assertTrue(pp_diff.icarus_outcome(Run(-6, "", ""), self.stream).crashed)
        run = Run(1, "", "x.v:1: Include file y not found\n")
        outcome = pp_diff.icarus_outcome(run, self.stream)
        self.assertEqual(outcome.message, "x.v:1: Include file y not found")


class SuiteTest(unittest.TestCase):
    def test_includes_are_found_outside_comments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "k").mkdir()
            (root / "g").mkdir()
            (root / "k" / "top.vh").write_text('`define W 3\n`include "../g/body.v"\n')
            (root / "g" / "body.v").write_text("wire [`W:0] a;\n")
            (root / "plain.v").write_text("module m; endmodule\n")
            (root / "c.v").write_text('// `include "d.v"\n/* `include "d.v" */\n`define X\n')
            (root / "d.v").write_text("`define Y\n")
            roots, included = pp_diff.backtick_roots(root)
            self.assertEqual(roots, [root / "c.v", root / "d.v", root / "k" / "top.vh"])
            self.assertEqual(included, {root / "g" / "body.v": [root / "k" / "top.vh"]})

    def test_coverage_comes_from_files_read(self) -> None:
        # review finding 3: an include in an untaken branch is not "compared through" it
        top, other, bad = Path("/m/top.v"), Path("/m/other.v"), Path("/m/bad.v")
        a_and, a_or, b = Path("/m/top/a_and.v"), Path("/m/top/a_or.v"), Path("/m/bad/b.v")
        included = {a_and: [top], a_or: [top], b: [bad]}
        reads: dict[Path, list[Path] | None] = {other: [other], top: [top, a_and], bad: None}
        rows = dict(pp_diff.coverage(included, reads))
        self.assertEqual(rows[a_and], "included by /m/top.v; compared through it")
        self.assertEqual(
            rows[a_or], "a variant that its includer's conditional does not select (/m/top.v)"
        )
        self.assertIn("rejected by odin3", rows[b])

    def test_order_is_smallest_first_mcml_last_without_duplicates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "mcml.v").write_text("x")
            (root / "big.v").write_text("x" * 50)
            (root / "small.v").write_text("x" * 5)
            paths = [root / "mcml.v", root / "big.v", root / "small.v", root / "big.v"]
            ordered = pp_diff.smallest_first(paths)
            self.assertEqual([p.name for p in ordered], ["small.v", "big.v", "mcml.v"])


_FAKE_ODIN3 = """#!/usr/bin/env bash
out="${4#preprocess --out }"
case "$FAKE_ODIN3" in
  segv) kill -SEGV $$ ;;
  reject) echo "odin3: error: /x.v:1:1: error: syntax: stray backtick" >&2; exit 1 ;;
esac
mkdir -p "$out"
printf 'module m; endmodule\\n' > "$out/0.v"
printf '%s\\n' "$(grep '^file' "$2" | cut -d' ' -f3)" > "$out/files-read.txt"
cp "$2" "$FAKE_LOG.o3proj"
"""
_FAKE_ICARUS = """#!/usr/bin/env bash
printf '%s\\n' "$@" > "$FAKE_LOG.icarus"
ls -A > "$FAKE_LOG.icarus-cwd"
while [ "$1" != "-o" ]; do shift; done
[ "$FAKE_ORACLES" = reject ] && { echo "x.v:1: syntax error" >&2; exit 1; }
printf 'module m; endmodule\\n' > "$2"
"""
_FAKE_YOSYS = """#!/usr/bin/env bash
printf '%s\\n' "$@" > "$FAKE_LOG.yosys"
ls -A > "$FAKE_LOG.yosys-cwd"
[ "$FAKE_ORACLES" = reject ] && { echo "ERROR: x" >&2; exit 1; }
printf -- '-- Verilog code after preprocessor --\\nmodule m; endmodule\\n-- END OF DUMP --\\n'
"""


class FakeToolsTest(unittest.TestCase):
    """main() end to end with fake odin3, iverilog and yosys binaries."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.bins: dict[str, str] = {}
        fakes = (("odin3", _FAKE_ODIN3), ("iverilog", _FAKE_ICARUS), ("yosys", _FAKE_YOSYS))
        for name, text in fakes:
            path = self.dir / name
            path.write_text(text)
            path.chmod(0o755)
            self.bins[name] = str(path)
        self.source = self.dir / "a.v"
        self.source.write_text("module m; endmodule\n")
        self.log = self.dir / "log"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _main(self, env: dict[str, str], *extra: str) -> tuple[int, str]:
        argv = ["--odin3", self.bins["odin3"], "--iverilog", self.bins["iverilog"]]
        argv += ["--yosys", self.bins["yosys"], "--work", str(self.dir), *extra, str(self.source)]
        with mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log), **env}):
            code, out, _ = helpers.run_main(pp_diff.main, argv)
        return code, out

    def test_agree(self) -> None:
        code, out = self._main({})
        self.assertEqual(code, 0, out)
        self.assertIn("agree 1,", out)

    def test_odin3_crash_on_a_file_all_reject_fails(self) -> None:
        # review finding 1, end to end
        code, out = self._main({"FAKE_ODIN3": "segv", "FAKE_ORACLES": "reject"})
        self.assertEqual(code, 1, out)
        self.assertIn("CRASH", out)
        self.assertIn("killed by signal 11", out)
        code, out = self._main({"FAKE_ODIN3": "reject", "FAKE_ORACLES": "reject"})
        self.assertEqual(code, 0, out)

    def test_relative_incdir_is_absolute_for_every_tool(self) -> None:
        # review finding 4
        cwd = os.getcwd()
        os.chdir(self.dir)
        try:
            code, out = self._main({}, "-I", "inc", "-D", "W=3")
        finally:
            os.chdir(cwd)
        self.assertEqual(code, 0, out)
        absolute = str(self.dir / "inc")
        self.assertIn(f"-I{absolute}", Path(f"{self.log}.icarus").read_text().splitlines())
        self.assertIn(f"-I{absolute}", Path(f"{self.log}.yosys").read_text())
        self.assertIn(f"incdir {absolute}", Path(f"{self.log}.o3proj").read_text())
        self.assertIn("-DW=3", Path(f"{self.log}.icarus").read_text().splitlines())

    def test_oracles_run_from_an_empty_directory(self) -> None:
        # review minor 8
        code, out = self._main({})
        self.assertEqual(code, 0, out)
        self.assertEqual(Path(f"{self.log}.icarus-cwd").read_text(), "")
        self.assertEqual(Path(f"{self.log}.yosys-cwd").read_text(), "")

    def test_double_quote_in_a_path_is_a_usage_error(self) -> None:
        # final review parked minor 9: neither a Yosys script nor an .o3proj word can carry it
        with self.assertRaises(pp_diff.UsageError):
            pp_diff._word('/x/a"b.v')
        self.assertEqual(pp_diff._word("/x/a b.v"), '"/x/a b.v"')
        quoted = self.dir / 'q"uote.v'
        quoted.write_text("module m; endmodule\n")
        self.source = quoted
        code, _ = self._main({})
        self.assertEqual(code, pp_diff.EXIT_USAGE)
        self.assertFalse(Path(f"{self.log}.o3proj").exists())  # nothing ran
        self.source = self.dir / "a.v"
        code, _ = self._main({}, "-I", str(self.dir / 'i"nc'))
        self.assertEqual(code, pp_diff.EXIT_USAGE)

    def test_missing_odin3_is_an_error_missing_oracle_a_skip(self) -> None:
        # review minor 10
        self.bins["odin3"] = str(self.dir / "nope")
        self.assertEqual(self._main({})[0], pp_diff.EXIT_USAGE)
        self.bins["odin3"] = str(self.dir / "odin3")
        self.bins["yosys"] = str(self.dir / "nope")
        self.assertEqual(self._main({})[0], pp_diff.EXIT_SKIP)


if __name__ == "__main__":
    unittest.main()
