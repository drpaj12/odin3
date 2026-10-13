"""Tests for tools/opt-check/opt-run (2F Task 18, spec 20.4): the Phase 2 script on one design.
``memcap.run`` is replaced by a scripted child, so no Odin III runs."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import memcap
import opt_equiv
import opt_run

from . import helpers

OPT_H = Path(__file__).resolve().parents[2] / "src" / "passes" / "opt" / "opt.h"


def child(code: int, text: str) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess([], code, "", text)


class JudgeTest(unittest.TestCase):
    def test_the_cap_matches_opt_h(self) -> None:
        text = OPT_H.read_text(encoding="utf-8")
        self.assertIn(f"ODIN3_OPT_MAX_ITERATIONS = {opt_run.MAX_ITERATIONS},", text)

    def test_the_largest_count_is_reported(self) -> None:
        log = ("odin3: info: opt: fixed point after 3 iterations\n"
               "odin3: info: opt: fixed point after 9 iterations\n")
        self.assertEqual(opt_run.judge(log), (0, "ok: iterations max=9 opt-runs=2"))

    def test_no_fixed_point_fails(self) -> None:
        log = ("odin3: warn: opt: module 'm': no fixed point after 10000 iterations; stopping\n"
               "odin3: info: opt: fixed point after 3 iterations\n")
        code, line = opt_run.judge(log)
        self.assertEqual(code, 1)
        self.assertIn("no fixed point after 10000 iterations", line)

    def test_a_count_at_the_cap_and_a_missing_line_fail(self) -> None:
        self.assertEqual(opt_run.judge("opt: fixed point after 10000 iterations\n")[0], 1)
        self.assertEqual(opt_run.judge("opt: fixed point after 9999 iterations\n")[0], 0)
        code, line = opt_run.judge("no opt line\n")
        self.assertEqual(code, 1)
        self.assertIn("no `opt: fixed point` line", line)


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.file = Path(self._tmp.name) / "d.v"
        self.file.write_text("module t; endmodule\n", encoding="utf-8")

    def run_tool(self, result: subprocess.CompletedProcess[str], *args: str
                 ) -> tuple[int, str, list[list[str]]]:
        calls: list[list[str]] = []

        def fake(argv: list[str], cap: int, **_: object) -> subprocess.CompletedProcess[str]:
            calls.append([*argv, f"cap={cap}"])
            return result

        with mock.patch.object(memcap, "run", fake):
            code, out, _ = helpers.run_main(
                opt_run.main, ["--odin3", "odin3", *args, str(self.file)])
        return code, out, calls

    def test_the_script_is_the_read_hierarchy_phase2_and_check_under_check(self) -> None:
        code, out, calls = self.run_tool(
            child(0, "opt: fixed point after 2 iterations\n"), "--top", "t", "--techlib", "L.o3lib",
            "--nolatches", "--mem-cap", "3G")
        self.assertEqual(code, 0, out)
        self.assertIn("opt-run: ok: iterations max=2 opt-runs=1", out)
        argv = calls[0]
        self.assertEqual(argv[:4], ["odin3", "--check", "--top", "t"])
        self.assertEqual(argv[-1], f"cap={3 << 30}")
        script = argv[argv.index("-p") + 1]
        commands = script.split("; ")
        self.assertTrue(commands[0].startswith("read_techlib "))
        self.assertTrue(commands[1].startswith("read_verilog --nolatches "))
        self.assertEqual(commands[2], "hierarchy --top t")
        self.assertEqual(commands[3:-1], opt_equiv.phase2_commands())
        self.assertEqual(commands[-1], "check")

    def test_a_failing_child_is_reported_with_its_error(self) -> None:
        code, out, _ = self.run_tool(child(1, "odin3: error: d.v:1: syntax: bad\n"))
        self.assertEqual(code, 1)
        self.assertIn("opt-run: FAIL: odin3 exit 1: odin3: error: d.v:1: syntax: bad", out)

    def test_a_killed_child_is_named(self) -> None:
        code, out, _ = self.run_tool(child(-9, ""))
        self.assertEqual(code, 1)
        self.assertIn("killed by signal 9", out)

    def test_a_missing_cap_is_a_skip(self) -> None:
        with mock.patch.object(memcap, "run", side_effect=memcap.CapUnavailable("no systemd")):
            code, _, _ = helpers.run_main(opt_run.main, ["--odin3", "odin3", str(self.file)])
        self.assertEqual(code, 77)


if __name__ == "__main__":
    unittest.main()
