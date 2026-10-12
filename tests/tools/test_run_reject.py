"""Tests for tools/parse-check/run-reject (2C spec §7.3, plan Task 9) over a fake odin3.

The fake odin3 records its arguments and working directory in <case>.argv and prints <case>.fake:
its first line is the exit status, the rest the output.
"""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

import run_reject

from . import helpers

FAKE = """#!{python}
import json, os, re, sys
args = sys.argv[1:]
name = re.search(r"(\\w+)\\.v", " ".join(args)).group(1)
with open(name + ".argv", "w") as out:
    json.dump({{"args": args, "cwd": os.getcwd()}}, out)
lines = open(name + ".fake").read().split("\\n", 1)
sys.stdout.write(lines[1] if len(lines) > 1 else "")
sys.exit(int(lines[0]))
"""

STUB_TEXT = "elaboration is not available yet (2D)"
STUB = "odin3: error: " + STUB_TEXT + "\n"
INVALID = "odin3: error: pass read_verilog: failed: ODIN3_ERR_INVALID_ARG\n"


class RunRejectTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.cases = self.root / "cases"
        self.cases.mkdir()
        self.odin3 = self.root / "odin3"
        self.odin3.write_text(FAKE.format(python=sys.executable))
        self.odin3.chmod(self.odin3.stat().st_mode | stat.S_IXUSR)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def case(self, name: str, expect: str, code: int, output: str, suffix: str = ".err") -> None:
        (self.cases / f"{name}.v").write_text("module m; endmodule\n")
        (self.cases / f"{name}{suffix}").write_text(expect)
        (self.cases / f"{name}.fake").write_text(f"{code}\n{output}")

    def run_tool(self, *extra: str) -> tuple[int, str, str]:
        argv = [*extra, str(self.odin3), str(self.cases)]
        return helpers.run_main(run_reject.main, argv)

    def test_prefix_and_substring_on_one_line_pass(self) -> None:
        self.case("c", "c.v:3:5: error:\nboom\n", 1, "odin3: error: c.v:3:5: error: a boom here\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0)
        self.assertIn("pass 1, pending 0, fail 0", out)

    def test_runs_parse_only_in_the_case_directory(self) -> None:
        self.case("c", "c.v:3:5: error:\nboom\n", 1, "odin3: error: c.v:3:5: error: boom\n")
        self.assertEqual(self.run_tool()[0], 0)
        record = json.loads((self.cases / "c.argv").read_text())
        self.assertEqual(record["args"], ["--parse-only", "c.v"])
        self.assertEqual(os.path.realpath(record["cwd"]), os.path.realpath(self.cases))

    def test_wrong_location_fails(self) -> None:
        self.case("c", "c.v:3:5: error:\nboom\n", 1, "odin3: error: c.v:3:6: error: boom\n")
        code, _, err = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("FAIL c.v: no line", err)

    def test_substring_must_be_on_the_located_line(self) -> None:
        output = "odin3: error: c.v:3:5: error: other\nodin3: error: boom\n"
        self.case("c", "c.v:3:5: error:\nboom\n", 1, output)
        self.assertEqual(self.run_tool()[0], 1)

    def test_parse_only_matches_the_first_error(self) -> None:
        output = "odin3: error: c.v:2:1: error: spurious\nodin3: error: c.v:3:5: error: boom\n"
        self.case("c", "c.v:3:5: error:\nboom\n", 1, output)
        self.assertEqual(self.run_tool()[0], 1)
        output = "odin3: warning: c.v:2:1: warning: w\nodin3: error: c.v:3:5: error: boom\n"
        self.case("c", "c.v:3:5: error:\nboom\n", 1, output + "odin3: error: c.v:4:1: error: x\n")
        self.assertEqual(self.run_tool()[0], 0)

    def test_elaborate_matches_any_error_line(self) -> None:
        output = "odin3: error: c.v:2:1: error: other row\nodin3: error: c.v:3:5: error: boom\n"
        self.case("c", "c.v:3:5: error:\nboom\n", 1, output)
        self.assertEqual(self.run_tool("--elaborate")[0], 0)

    def test_error_needs_a_nonzero_exit(self) -> None:
        self.case("c", "c.v:3:5: error:\nboom\n", 0, "odin3: error: c.v:3:5: error: boom\n")
        code, _, err = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("exit status 0", err)

    def test_invalid_arg_guard(self) -> None:
        output = "odin3: error: c.v:3:5: error: boom\n" + INVALID
        self.case("c", "c.v:3:5: error:\nboom\n", 1, output)
        code, _, err = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("forbidden line", err)

    def test_check_failure_guard(self) -> None:
        output = "odin3: error: c.v:3:5: error: boom\nodin3: error: ast check: rule 1: node 3: x\n"
        self.case("c", "c.v:3:5: error:\nboom\n", 1, output)
        self.assertEqual(self.run_tool()[0], 1)

    def test_elaborate_runs_read_verilog_without_parse_only(self) -> None:
        self.case("c", "c.v:3:5: error:\nboom\n", 1, "odin3: error: c.v:3:5: error: boom\n")
        self.assertEqual(self.run_tool("--elaborate")[0], 0)
        record = json.loads((self.cases / "c.argv").read_text())
        self.assertEqual(record["args"], ["-p", "read_verilog c.v"])

    def test_elaborate_stub_is_pending_and_all_pending_skips(self) -> None:
        self.case("a", "a.v:2:1: error:\nx\n", 1, STUB + INVALID)
        self.case("b", "b.v:2:1: warning:\ny\n", 1, STUB + INVALID, suffix=".warn")
        code, out, _ = self.run_tool("--elaborate")
        self.assertEqual(code, 77)
        self.assertIn("pass 0, pending 2, fail 0", out)

    def test_stub_without_elaborate_fails(self) -> None:
        self.case("a", "a.v:2:1: error:\nx\n", 1, STUB)
        self.assertEqual(self.run_tool()[0], 1)

    def test_elaborate_pass_and_pending_exit_0(self) -> None:
        self.case("a", "a.v:2:1: error:\nx\n", 1, "odin3: error: a.v:2:1: error: x\n")
        self.case("b", "b.v:2:1: error:\ny\n", 1, STUB)
        code, out, _ = self.run_tool("--elaborate")
        self.assertEqual(code, 0)
        self.assertIn("pass 1, pending 1, fail 0", out)

    def test_elaborate_located_pending_line_is_pending(self) -> None:
        located = "odin3: error: a.v:2:3: error: " + STUB_TEXT + ": 'ALWAYS'\n"
        self.case("a", "a.v:4:1: error:\nx\n", 1, located)
        self.case("b", "b.v:2:1: error:\ny\n", 1, "odin3: error: b.v:2:1: error: y\n")
        code, out, err = self.run_tool("--elaborate")
        self.assertEqual(code, 0)
        self.assertEqual(out, "run-reject: pass 1, pending 1, fail 0\n")
        self.assertIn("PENDING a.v: 2D: not available yet", err)

    def test_elaborate_all_pending_located_or_not_exits_77(self) -> None:
        located = "odin3: error: a.v:2:3: error: " + STUB_TEXT + ": 'NET_DECL'\n"
        self.case("a", "a.v:4:1: error:\nx\n", 1, located)
        self.case("b", "b.v:2:1: warning:\ny\n", 1, STUB, suffix=".warn")
        code, out, _ = self.run_tool("--elaborate")
        self.assertEqual(code, 77)
        self.assertEqual(out, "run-reject: pass 0, pending 2, fail 0\n")

    def test_elaborate_missing_line_without_pending_text_fails(self) -> None:
        self.case("a", "a.v:2:1: error:\nx\n", 1, "odin3: error: a.v:9:9: error: other\n")
        code, out, err = self.run_tool("--elaborate")
        self.assertEqual(code, 1)
        self.assertEqual(out, "run-reject: pass 0, pending 0, fail 1\n")
        self.assertIn("FAIL a.v: no line 'a.v:2:1: error: ...x...'", err)

    def test_elaborate_fail_exits_1(self) -> None:
        self.case("a", "a.v:2:1: error:\nx\n", 1, "odin3: error: a.v:2:1: error: y\n")
        self.case("b", "b.v:2:1: error:\ny\n", 1, STUB)
        self.assertEqual(self.run_tool("--elaborate")[0], 1)

    def test_warning_case_matches_a_warning_line_with_exit_0(self) -> None:
        output = "odin3: warning: w.v:2:3: warning: delay is ignored\n"
        self.case("w", "w.v:2:3: warning:\ndelay is ignored\n", 0, output, suffix=".warn")
        self.assertEqual(self.run_tool("--elaborate")[0], 0)
        self.case("w", "w.v:2:3: warning:\ndelay is ignored\n", 1, output, suffix=".warn")
        self.assertEqual(self.run_tool("--elaborate")[0], 1)

    def test_error_expectation_does_not_match_a_warning(self) -> None:
        self.case("c", "c.v:3:5: error:\nboom\n", 1, "odin3: warning: c.v:3:5: warning: boom\n")
        self.assertEqual(self.run_tool()[0], 1)

    def test_missing_expectation_is_a_usage_error(self) -> None:
        (self.cases / "c.v").write_text("module m; endmodule\n")
        self.assertEqual(self.run_tool()[0], 2)

    def test_malformed_expectation_is_a_usage_error(self) -> None:
        self.case("c", "c.v:3:5:\nboom\n", 1, "")
        self.assertEqual(self.run_tool()[0], 2)

    def test_empty_directory_is_a_usage_error(self) -> None:
        self.assertEqual(self.run_tool()[0], 2)

    def test_cases_run_smallest_first(self) -> None:
        (self.cases / "big.v").write_text("x" * 100)
        (self.cases / "small.v").write_text("x")
        self.assertEqual([p.name for p in run_reject.cases_of(self.cases)], ["small.v", "big.v"])


if __name__ == "__main__":
    unittest.main()
