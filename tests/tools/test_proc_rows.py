"""Tests for tests/proc/rows (2E spec 18.2, plan Task 11) and tools/proc-check/proc_rows.py.

The rows themselves are checked against Odin III and Yosys by the CTests proc_rows,
proc_diff_rows and proc_bridge_rows; here the files are checked for shape, and the CTest driver's
helpers for what they accept and reject.
"""

from __future__ import annotations

import contextlib
import io
import re
import tempfile
import unittest
from pathlib import Path

import proc_rows

REPO_ROOT = Path(__file__).resolve().parents[2]
PROC = REPO_ROOT / "tests" / "proc"
ROWS = PROC / "rows"

ERROR_ROWS = {"err_two_clocks", "err_second_root_if", "err_reset_untested", "err_dual_edge",
              "err_nonconst_reset"}
ROW_COUNT = 48
YOSYS_V_RE = re.compile(r"^// yosys -V: Yosys 0\.55 \(")


def _rows() -> list[proc_rows.Row]:
    return proc_rows.parse_rows((ROWS / "rows.txt").read_text(encoding="utf-8"))


class RowsShapeTest(unittest.TestCase):
    def test_rows_txt_lists_the_48_slugs_once_each(self) -> None:
        slugs = [r.slug for r in _rows()]
        self.assertEqual(len(slugs), ROW_COUNT)
        self.assertEqual(len(set(slugs)), ROW_COUNT)

    def test_exactly_the_five_error_rows_are_flagged(self) -> None:
        self.assertEqual({r.slug for r in _rows() if r.err}, ERROR_ROWS)

    def test_no_stray_files(self) -> None:
        expected = {"rows.txt"}
        for row in _rows():
            expected.add(f"{row.slug}.v")
            if row.err:
                expected.add(f"{row.slug}.err")
                continue
            expected.add(f"{row.slug}.il")
            if row.comb:
                expected.add(f"{row.slug}.nolatches.il")
        self.assertEqual({p.name for p in ROWS.iterdir()}, expected)

    def test_every_source_has_its_header(self) -> None:
        for row in _rows():
            lines = (ROWS / f"{row.slug}.v").read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines[0].startswith(f"// {row.slug}: "), row.slug)
            self.assertRegex(lines[1], YOSYS_V_RE, row.slug)

    def test_every_non_error_row_has_its_golden(self) -> None:
        for row in _rows():
            self.assertEqual((ROWS / f"{row.slug}.il").is_file(), not row.err, row.slug)

    def test_every_comb_row_has_a_nolatches_golden(self) -> None:
        for row in _rows():
            want = row.comb and not row.err
            self.assertEqual((ROWS / f"{row.slug}.nolatches.il").is_file(), want, row.slug)

    def test_exactly_the_error_rows_have_err_files(self) -> None:
        have = {p.stem for p in ROWS.glob("*.err")}
        self.assertEqual(have, ERROR_ROWS)

    def test_no_golden_holds_a_process(self) -> None:
        for path in ROWS.glob("*.il"):
            self.assertNotIn("process", path.read_text(encoding="utf-8"), path.name)

    def test_err_files_are_located_error_lines_at_the_always(self) -> None:
        for slug in ERROR_ROWS:
            source = (ROWS / f"{slug}.v").read_text(encoding="utf-8").splitlines()
            text = (ROWS / f"{slug}.err").read_text(encoding="utf-8")
            match = re.fullmatch(rf"{slug}\.v:(\d+):(\d+): error: .*\n", text)
            self.assertIsNotNone(match, slug)
            assert match is not None
            line, col = int(match[1]), int(match[2])
            self.assertTrue(source[line - 1][col - 1:].startswith("always"), slug)

    def test_diff_expect_has_no_entries(self) -> None:
        lines = (PROC / "diff_expect.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual([x for x in lines if x.strip() and not x.startswith("#")], [])

    def test_case_default_golden_has_one_pmux_of_width_8(self) -> None:
        text = (ROWS / "case_default.il").read_text(encoding="utf-8")
        self.assertEqual(proc_rows.pmux_widths(text), [8])
        self.assertIn("output reg [7:0] y",
                      (ROWS / "case_default.v").read_text(encoding="utf-8"))

    def test_bridge_expect_names_only_rows_and_has_all_three_fields(self) -> None:
        expect = proc_rows.parse_bridge_expect(
            (PROC / "bridge_expect.txt").read_text(encoding="utf-8"))
        self.assertLessEqual(set(expect), {r.slug for r in _rows() if not r.err})
        self.assertTrue(expect)


class HelperTest(unittest.TestCase):
    def test_parse_rows_rejects_a_bad_line(self) -> None:
        with self.assertRaises(proc_rows.RowsError):
            proc_rows.parse_rows("a comb=2 err=0\n")
        self.assertEqual(proc_rows.parse_rows("a comb=1 err=0\n"),
                         [proc_rows.Row("a", True, False)])

    def test_error_lines_keep_located_errors_only(self) -> None:
        stderr = ("odin3: info: x\n"
                  "odin3: error: e.v:5:3: error: 'q' is clocked by both 'a' and 'b'\n"
                  "odin3: warning: e.v:1:1: warning: w\n"
                  "odin3: error: pass proc: failed: ODIN3_ERR_PARSE\n"
                  "odin3: error: e.v:6:3: error: second\n")
        self.assertEqual(proc_rows.error_lines(stderr),
                         "e.v:5:3: error: 'q' is clocked by both 'a' and 'b'\n"
                         "e.v:6:3: error: second\n")
        self.assertEqual(proc_rows.error_lines("odin3: info: only\n"), "")

    def test_pmux_widths(self) -> None:
        text = ("  cell $pmux $c1\n    parameter \\WIDTH 8\n    parameter \\S_WIDTH 3\n  end\n"
                "  cell $mux $c2\n    parameter \\WIDTH 4\n  end\n"
                "  cell $pmux $c3\n    parameter \\WIDTH 2\n  end\n")
        self.assertEqual(proc_rows.pmux_widths(text), [8, 2])
        self.assertEqual(proc_rows.pmux_widths("cell $mux $c\n parameter \\WIDTH 8\n end\n"), [])

    def test_parse_bridge_expect(self) -> None:
        got = proc_rows.parse_bridge_expect("# c\n\na|text with spaces|why not\n")
        self.assertEqual(got, {"a": ("text with spaces", "why not")})
        with self.assertRaises(proc_rows.RowsError):
            proc_rows.parse_bridge_expect("a|only two\n")
        with self.assertRaises(proc_rows.RowsError):
            proc_rows.parse_bridge_expect("a||reason\n")

    def test_rows_load_smallest_file_first(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "rows.txt").write_text("big comb=0 err=0\nsmall comb=0 err=0\n",
                                           encoding="utf-8")
            (root / "big.v").write_text("x" * 100, encoding="utf-8")
            (root / "small.v").write_text("x", encoding="utf-8")
            self.assertEqual([r.slug for r in proc_rows.load_rows(root)], ["small", "big"])

    def test_goldens_notice_a_changed_golden_and_a_changed_err(self) -> None:
        """The checkers fail on a one-byte change: fake odin3 writes the row's golden text."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "rows"
            root.mkdir()
            fake = Path(tmp) / "odin3"
            fake.write_text(
                "#!/usr/bin/env python3\nimport sys\n"
                "script = sys.argv[sys.argv.index('-p') + 1]\n"
                "if 'bad.v' in script:\n"
                "    sys.stderr.write('odin3: error: bad.v:5:3: error: boom\\n'\n"
                "                     'odin3: error: pass proc: failed: ODIN3_ERR_PARSE\\n')\n"
                "    sys.exit(1)\n"
                "sys.stdout.write('module x\\n')\n", encoding="utf-8")
            fake.chmod(0o755)
            (root / "ok.v").write_text("m", encoding="utf-8")
            (root / "bad.v").write_text("m", encoding="utf-8")
            (root / "ok.il").write_text("module x\n", encoding="utf-8")
            (root / "bad.err").write_text("bad.v:5:3: error: boom\n", encoding="utf-8")
            ctx = proc_rows.Context(root, str(fake))
            row_ok = proc_rows.Row("ok", False, False)
            row_bad = proc_rows.Row("bad", False, True)
            proc_rows.check_lowering(ctx, row_ok, False)
            proc_rows.check_error_row(ctx, row_bad)
            self.assertEqual(ctx.failures, [])
            (root / "ok.il").write_text("module y\n", encoding="utf-8")
            (root / "bad.err").write_text("bad.v:5:3: error: bam\n", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                proc_rows.check_lowering(ctx, row_ok, False)
                proc_rows.check_error_row(ctx, row_bad)
            self.assertEqual(len(ctx.failures), 2)


if __name__ == "__main__":
    unittest.main()
