"""Tests for tools/lower-check/lower-run (3B Task 9, spec 14.5): the corpus smoke run.

A fake ``odin3`` shell script stands in for the real one and prints what a case needs; one test
per exit class (0 ok, 1 fail, 3 crash, 5 cap) and the vacuous-pass guard of the 3B-4 review H3: a
log without any `lower:` line is a failure, never ok. The work directories are removed
when each test ends.
"""

from __future__ import annotations

import subprocess
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import lower_run as lr
import memcap

from . import helpers

LOWERED = "lower: module 'm': lowered 3 word cells to 17 bit cells\n"
NOTHING = "lower: nothing to lower\n"
CAP = "lower: module 'm': node limit: would create 99 nodes (cap 10)\n"


def say(text: str) -> str:
    """A shell line that prints ``text`` exactly."""
    return f"cat <<'END'\n{text}END\n"


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(
        list(argv), capture_output=True, text=True, check=False, cwd=kwargs.get("cwd")
    )


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = helpers.scratch(self, "lower_run_test_")
        (self.root / "m.v").write_text("module m(input a, output y); endmodule\n")

    def run_fake(self, body: str) -> tuple[int, str, str]:
        fake = self.root / "odin3"
        fake.write_text("#!/usr/bin/env bash\n" + body)
        fake.chmod(0o755)
        argv = ["--odin3", str(fake), "--work", str(self.root), str(self.root / "m.v")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(lr.main, argv)

    def test_the_script_is_the_phase_3_script_up_to_compact(self) -> None:
        script = lr.script_for(["a.v"], None, None, "", Path("/tmp/o.blif"))
        self.assertTrue(
            script.endswith("proc; opt; lower; opt -fast; compact; check; write_blif /tmp/o.blif"),
            script,
        )

    def test_a_lowered_design_is_ok_and_counted(self) -> None:
        code, out, err = self.run_fake(say(LOWERED))
        self.assertEqual(code, 0, out + err)
        self.assertIn("lower-run: ok: lower: module 'm': lowered 3 word cells", out)
        self.assertIn("lower-run: compared 1 of 1 designs", out)

    def test_nothing_to_lower_is_ok_but_not_counted(self) -> None:
        code, out, _ = self.run_fake(say(NOTHING))
        self.assertEqual(code, 0)
        self.assertIn("lower-run: compared 0 of 1 designs", out)

    def test_a_log_with_no_lower_line_is_a_failure_not_ok(self) -> None:
        code, out, _ = self.run_fake("echo 'check: ok'\n")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: no lower line in the log", out)
        self.assertNotIn("ok:", out)
        self.assertEqual(lr.judge("")[0], 1)

    def test_a_failing_command_is_exit_1(self) -> None:
        code, out, _ = self.run_fake("echo 'odin3: error: check failed: dangling net'\nexit 1\n")
        self.assertEqual(code, 1)
        self.assertIn("lower-run: FAIL: odin3 exit 1", out)

    def test_a_cap_is_exit_5(self) -> None:
        code, out, _ = self.run_fake(say(CAP) + "exit 1\n")
        self.assertEqual(code, lr.EXIT_CAP)
        self.assertEqual(lr.EXIT_CAP, 5)
        self.assertIn("lower-run: cap: lower: module 'm'", out)

    def test_a_signal_is_exit_3(self) -> None:
        code, out, _ = self.run_fake("kill -SEGV $$\n")
        self.assertEqual(code, lr.EXIT_CRASH)
        self.assertIn("CRASH: odin3 killed by signal 11", out)

    def test_a_timeout_is_exit_3(self) -> None:
        with mock.patch.object(lr, "TIMEOUT_SECONDS", 0.2):

            def slow(
                argv: Sequence[str], cap: int, **kwargs: Any
            ) -> subprocess.CompletedProcess[str]:
                raise subprocess.TimeoutExpired(list(argv), kwargs.get("timeout", 0.0))

            with mock.patch.object(memcap, "run", slow):
                code, out, _ = helpers.run_main(lr.main, ["--odin3", "x", str(self.root / "m.v")])
        self.assertEqual(code, lr.EXIT_CRASH)
        self.assertIn("timed out", out)


if __name__ == "__main__":
    unittest.main()
