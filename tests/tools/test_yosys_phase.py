"""Tests for tools/proc-check/yosys-phase (2E plan point (a)) over a fake Yosys that logs its argv:
the exact scripts of --upto and --passes, and the 0.55 version guard."""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import yosys_phase

from . import helpers

ORACLE = "Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0 -fPIC -O3)"

FAKE_YOSYS = """#!{python}
import json, os, sys
with open(os.environ["FAKE_YOSYS_LOG"], "a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
if sys.argv[1:] == ["-V"]:
    print(os.environ.get("FAKE_YOSYS_VERSION", "{oracle}"))
    sys.exit(0)
print("module \\\\m")
print("end")
sys.exit(int(os.environ.get("FAKE_YOSYS_RC", "0")))
"""


class YosysPhaseTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.yosys = self.dir / "yosys"
        self.yosys.write_text(FAKE_YOSYS.format(python=sys.executable, oracle=ORACLE))
        self.yosys.chmod(self.yosys.stat().st_mode | stat.S_IXUSR)
        self.log = self.dir / "argv.log"
        env = {"FAKE_YOSYS_LOG": str(self.log)}
        self._env = mock.patch.dict(os.environ, env)
        self._env.start()

    def tearDown(self) -> None:
        self._env.stop()
        self._tmp.cleanup()

    def run_tool(self, *args: str) -> tuple[int, str, str]:
        return helpers.run_main(yosys_phase.main, ["--yosys", str(self.yosys), *args])

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def script_of_last_run(self) -> str:
        last = self.calls()[-1]
        self.assertEqual(last[:2], ["-q", "-p"])
        return last[2]

    def test_upto_arst_runs_exactly_the_list(self) -> None:
        code, out, _ = self.run_tool("--upto", "arst", "t.il")
        self.assertEqual(code, 0)
        self.assertEqual(out, "module \\m\nend\n")
        self.assertEqual(self.calls()[0], ["-V"])
        self.assertEqual(
            self.script_of_last_run(),
            "read_rtlil t.il; proc_clean; proc_rmdead; proc_prune; proc_init; proc_arst; "
            "write_rtlil -",
        )

    def test_upto_dff_ends_with_memwr_and_clean(self) -> None:
        code, _, _ = self.run_tool("--upto", "dff", "t.il")
        self.assertEqual(code, 0)
        self.assertTrue(
            self.script_of_last_run().endswith(
                "proc_mux; proc_dlatch; proc_dff; proc_memwr; proc_clean; write_rtlil -"
            )
        )

    def test_every_phase_list(self) -> None:
        self.assertEqual(yosys_phase.passes_upto("clean"), ["proc_clean"])
        self.assertEqual(yosys_phase.passes_upto("rmdead"), ["proc_clean", "proc_rmdead"])
        self.assertEqual(yosys_phase.passes_upto("prune")[-1], "proc_prune")
        self.assertEqual(yosys_phase.passes_upto("mux")[-2:], ["proc_arst", "proc_mux"])
        self.assertEqual(yosys_phase.passes_upto("dlatch")[-1], "proc_dlatch")
        self.assertNotIn("proc_rom", yosys_phase.passes_upto("dff"))

    def test_passes_runs_only_them(self) -> None:
        code, _, _ = self.run_tool("--passes", "proc_mux", "t.il")
        self.assertEqual(code, 0)
        self.assertEqual(self.script_of_last_run(), "read_rtlil t.il; proc_mux; write_rtlil -")
        code, _, _ = self.run_tool("--passes", " proc_clean ;proc_rmdead; ", "t.il")
        self.assertEqual(code, 0)
        self.assertEqual(
            self.script_of_last_run(), "read_rtlil t.il; proc_clean; proc_rmdead; write_rtlil -"
        )

    def test_another_version_is_refused_unless_allowed(self) -> None:
        with mock.patch.dict(os.environ, {"FAKE_YOSYS_VERSION": "Yosys 0.69+270 (git sha1 x)"}):
            code, _, err = self.run_tool("--upto", "clean", "t.il")
            self.assertEqual(code, 2)
            self.assertIn("not Yosys 0.55", err)
            self.assertEqual(self.calls(), [["-V"]])
            code, _, _ = self.run_tool("--allow-version", "0.69", "--upto", "clean", "t.il")
            self.assertEqual(code, 0)
            self.assertEqual(
                self.script_of_last_run(), "read_rtlil t.il; proc_clean; write_rtlil -"
            )

    def test_0_550_is_not_0_55(self) -> None:
        self.assertFalse(yosys_phase.version_ok("Yosys 0.550 (git sha1 x)", None))
        self.assertTrue(yosys_phase.version_ok("Yosys 0.55", None))

    def test_yosys_failure_exits_1(self) -> None:
        with mock.patch.dict(os.environ, {"FAKE_YOSYS_RC": "3"}):
            code, _, err = self.run_tool("--upto", "clean", "t.il")
        self.assertEqual(code, 1)
        self.assertIn("Yosys exited 3", err)

    def test_usage_errors_exit_2(self) -> None:
        code, _, _ = self.run_tool("t.il")
        self.assertEqual(code, 2)
        code, _, _ = self.run_tool("--upto", "proc_rom", "t.il")
        self.assertEqual(code, 2)
        code, _, _ = self.run_tool("--passes", " ; ", "t.il")
        self.assertEqual(code, 2)
        code, _, _ = helpers.run_main(
            yosys_phase.main, ["--yosys", str(self.dir / "none"), "--upto", "clean", "t.il"]
        )
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
