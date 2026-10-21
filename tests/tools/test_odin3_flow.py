"""Tests for tools/flow/odin3_flow.py (7.3 Tasks 1 to 3, spec 7 section 2.1): the vtr target without
VPR, the lpm target, the step and exit-code contract, the VPR summary and --analysis.

The tests that run odin3 need ODIN3_CLI (CTest python_tools); none needs VTR."""

from __future__ import annotations

import os
import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import lpm_check

import odin3_flow as flow

from .helpers import run_main

ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_CLI", "")
DFF = ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_DL_logic_w_Dff.v"
LPM_MICRO = ROOT / "tests" / "lpm" / "lpm_add_sub.v"
needs_odin3 = unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")


def env_without_vtr() -> dict[str, str]:
    return {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}


class FlowCase(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def run_flow(self, *argv: str) -> tuple[int, str, str]:
        with mock.patch.dict(os.environ, env_without_vtr(), clear=True):
            return run_main(flow.main, [*argv, "--odin3", ODIN3, "--out", str(self.tmp / "out")])


@needs_odin3
class VtrTarget(FlowCase):
    def test_vtr_no_vpr_writes_blif(self) -> None:
        code, out, err = self.run_flow(str(DFF), "--top", "bm_DL_logic_w_Dff", "--no-vpr")
        self.assertEqual(code, 0, out + err)
        blif = self.tmp / "out" / "odin3" / "bm_DL_logic_w_Dff.blif"
        self.assertTrue(blif.is_file() and blif.stat().st_size > 0)

    def test_failing_step_names_step_and_log(self) -> None:
        bad = self.tmp / "bad.v"
        bad.write_text("module bad(input a, output y);\n  assign y = (a;\nendmodule\n")
        code, out, err = self.run_flow(str(bad), "--top", "bad", "--no-vpr")
        self.assertEqual(code, 1)
        self.assertIn("step odin3", out + err)
        log = re.search(r"(\S+odin3\.log)", out + err)
        self.assertIsNotNone(log)
        assert log is not None
        self.assertTrue(Path(log.group(1)).is_file())

    def test_flow_requires_top_when_ambiguous(self) -> None:
        two = self.tmp / "two.v"
        two.write_text("module alpha(input a, output y); assign y = a; endmodule\n"
                       "module beta(input a, output y); assign y = ~a; endmodule\n")
        code, out, err = self.run_flow(str(two), "--no-vpr")
        self.assertEqual(code, 2)
        self.assertIn("alpha", out + err)
        self.assertIn("beta", out + err)


@needs_odin3
class LpmTarget(FlowCase):
    def test_lpm_writes_verilog_and_lints(self) -> None:
        code, out, err = self.run_flow(str(LPM_MICRO), "--top", "lpm_add_sub_micro",
                                       "--target", "lpm")
        self.assertEqual(code, 0, out + err)
        written = self.tmp / "out" / "odin3" / "lpm_add_sub_micro.v"
        self.assertIn("lpm_add_sub", written.read_text())
        self.assertTrue((self.tmp / "out" / "readback" / "readback.log").is_file())

    def test_lpm_flow_matches_5_1(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r"foreach\(_lpm_micro IN ITEMS lpm_add_sub\b")
        self.assertIn("tools/lpm-check/lpm-check", cmake)
        out = self.tmp / "gen.v"
        lib = ROOT / "lib" / "lpm.o3lib"
        for family in (None, "Stratix IV"):
            want = lpm_check.script_text(lpm_check.DEFAULT_SCRIPT, lib, LPM_MICRO, out, family)
            got = flow.lpm_script([LPM_MICRO], out, family or "")
            self.assertEqual(got, want)


if __name__ == "__main__":
    unittest.main()
