"""Tests for tools/flow/odin3_flow.py (7.3 Tasks 1 to 3, spec 7 section 2.1): the vtr target without
VPR, the lpm target, the step and exit-code contract, the VPR summary and --analysis.

The tests that run odin3 need ODIN3_CLI (CTest python_tools); none needs VTR."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import lpm_check

import odin3_flow as flow
import odin3_graph
import qor_parse

from .helpers import run_main

ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_CLI", "")
DFF = ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_DL_logic_w_Dff.v"
LPM_MICRO = ROOT / "tests" / "lpm" / "lpm_add_sub.v"
VPR_FIXTURE = Path(__file__).resolve().parent / "fixtures" / "vtr_qor" / "blink_P"
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

    def test_no_vpr_without_vtr_root(self) -> None:
        code, out, err = self.run_flow(str(DFF), "--top", "bm_DL_logic_w_Dff")
        self.assertEqual(code, 0, out + err)
        self.assertIn("NOTE", out + err)
        self.assertFalse((self.tmp / "out" / "vpr").exists())


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
            self.assertEqual(got.replace('"', ""), want.replace('"', ""))  # paths are quoted


class SummaryFromLogs(unittest.TestCase):
    def results(self, patch: dict[str, str] | None = None) -> tuple[dict[str, str], dict[str, str]]:
        parse = qor_parse.read_parse_results(VPR_FIXTURE / "parse_results.txt",
                                             not_before=0.0, label="t")
        qor = qor_parse.read_parse_results(VPR_FIXTURE / "qor_results.txt",
                                           not_before=0.0, label="t")
        return {**parse, **(patch or {})}, qor

    def test_summary_from_vpr_logs(self) -> None:
        parse, qor = self.results({"min_chan_width": "34", "critical_path_delay": "2.5",
                                   "crit_path_routed_wirelength": "77"})
        text = (VPR_FIXTURE / "vpr.out").read_text()
        got = flow.summary_from_vpr(parse, qor, text, luts=3)
        self.assertEqual(got["luts"], 3)
        self.assertEqual(got["clbs"], 1)
        self.assertEqual(got["hard_blocks"], {"mult_36": 0, "memory": 0})
        self.assertEqual(got["min_chan_width"], 34)
        self.assertEqual(got["crit_path_ns"], 2.5)
        self.assertEqual(got["wirelength"], 77)

    def test_summary_missing_values_are_null(self) -> None:
        parse, qor = self.results()
        got = flow.summary_from_vpr(parse, qor, (VPR_FIXTURE / "vpr.out").read_text(), luts=0)
        self.assertIsNone(got["min_chan_width"])
        self.assertIsNone(got["crit_path_ns"])
        json.dumps(got)


@needs_odin3
class Analysis(FlowCase):
    def test_analysis_outputs(self) -> None:
        code, out, err = self.run_flow(str(DFF), "--top", "bm_DL_logic_w_Dff", "--no-vpr",
                                       "--analysis")
        self.assertEqual(code, 0, out + err)
        names = ("bm_DL_logic_w_Dff.dot", "bm_DL_logic_w_Dff.odin3.json", "levelize.txt",
                 "locs.txt")
        for name in names:
            path = self.tmp / "out" / "analysis" / name
            self.assertTrue(path.is_file() and path.stat().st_size > 0, name)
        nodes, edges = odin3_graph.parse_graph(self.tmp / "out" / "analysis" / names[1])
        self.assertGreater(len(nodes), 0)
        self.assertGreater(len(edges), 0)

    def test_analysis_with_lpm_is_usage_error(self) -> None:
        code, out, err = self.run_flow(str(LPM_MICRO), "--top", "lpm_add_sub_micro",
                                       "--target", "lpm", "--analysis")
        self.assertEqual(code, 2, out + err)


SHARE_FILES = ("lib/lpm.o3lib", "lib/vtr.o3lib", "lib/abc/vtr.abc", "tools/flow/lpm.script",
               "tools/flow/odin3_flow.py")


def share_tree(dest: Path) -> Path:
    """A copy of what the install puts under share/odin3: lib/, tools/flow, tools/vtr-qor."""
    for rel in SHARE_FILES:
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / rel, dest / rel)
    shutil.copytree(ROOT / "tools" / "vtr-qor", dest / "tools" / "vtr-qor",
                    ignore=shutil.ignore_patterns("__pycache__", "offload", "systemd"))
    return dest


class HelpText(unittest.TestCase):
    def test_help_states_the_contract(self) -> None:
        code, out, err = run_main(flow.main, ["--help"])
        self.assertEqual(code, 0, err)
        flat = " ".join(out.split())
        for text in ("vtr", "lpm", "--family", "default", "--analysis", "--target vtr",
                     "exit", "0", "1", "2"):
            self.assertIn(text, flat)
        for option in ("--top", "--arch", "--vpr", "--odin3", "--out", "designs"):
            self.assertRegex(out, rf"{option}\s+\S")  # every option has a help text


class Luts(unittest.TestCase):
    def test_luts_count_only_nontrivial_names(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            blif = Path(tmp) / "t.blif"
            blif.write_text(".model t\n.names gnd\n.names a b y\n11 1\n.names a buf\n1 1\n"
                            ".names a b c z\n111 1\n.end\n")
            self.assertEqual(flow.count_luts(blif), 2)


class HardBlocks(unittest.TestCase):
    def test_other_hard_types_and_pads(self) -> None:
        text = ("Netlist clb blocks: 4.\nNetlist EArch_multiply blocks: 2.\n"
                "Netlist inpad blocks: 3.\nNetlist outpad blocks: 1.\nNetlist io blocks: 4.\n"
                "Netlist EMPTY blocks: 0.\nNetlist dsp_top blocks: 5.\n")
        parse = {"num_clb": "4"}
        with mock.patch.object(qor_parse, "metrics_from_parse",
                               return_value={"packed": {"num_clb": 4}, "routed": {
                                   "min_chan_width": None, "critical_path_delay_ns": None,
                                   "crit_path_routed_wirelength": None}}):
            got = flow.summary_from_vpr(parse, {}, text, luts=1)
        self.assertEqual(got["hard_blocks"], {"EArch_multiply": 2, "dsp_top": 5})


class Robustness(FlowCase):
    def test_missing_odin3_is_usage_error_before_any_step(self) -> None:
        with mock.patch.dict(os.environ, env_without_vtr(), clear=True):
            code, out, err = run_main(flow.main, [str(DFF), "--top", "bm_DL_logic_w_Dff",
                                                  "--no-vpr", "--odin3", str(self.tmp / "nope"),
                                                  "--out", str(self.tmp / "out")])
        self.assertEqual(code, 2, out + err)
        self.assertIn("odin3", err)
        self.assertFalse((self.tmp / "out" / "odin3").exists())

    def test_arch_without_vtr_root_warns(self) -> None:
        arch = self.tmp / "a.xml"
        arch.write_text("<architecture/>")
        with mock.patch.dict(os.environ, env_without_vtr(), clear=True):
            code, out, err = run_main(flow.main, [str(DFF), "--top", "bm_DL_logic_w_Dff",
                                                  "--no-vpr", "--arch", str(arch), "--odin3",
                                                  ODIN3 or "/bin/true",
                                                  "--out", str(self.tmp / "out")])
        self.assertIn("WARNING", err)
        self.assertIn("K = 6", err)


@needs_odin3
class Installed(FlowCase):
    def test_share_layout_run_from_elsewhere(self) -> None:
        share = share_tree(self.tmp / "inst" / "share" / "odin3")
        env = env_without_vtr()
        env["ODIN3_SHARE"] = str(share)
        env["PYTHONPATH"] = str(share / "tools" / "vtr-qor")
        for target, micro, top in (("vtr", DFF, "bm_DL_logic_w_Dff"),
                                   ("lpm", LPM_MICRO, "lpm_add_sub_micro")):
            out = self.tmp / f"out_{target}"
            proc = subprocess.run(
                [sys.executable, str(share / "tools" / "flow" / "odin3_flow.py"), str(micro),
                 "--top", top, "--target", target, "--no-vpr", "--odin3", ODIN3,
                 "--out", str(out)], cwd=self.tmp, env=env, capture_output=True, text=True,
                check=False)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_resource_root_env_then_source_tree(self) -> None:
        with mock.patch.dict(os.environ, {"ODIN3_SHARE": str(self.tmp)}):
            self.assertEqual(flow.resource_root(), self.tmp)
        with mock.patch.dict(os.environ, env_without_vtr(), clear=True):
            self.assertEqual(flow.resource_root(), ROOT)

    def test_design_path_with_space(self) -> None:
        spaced = self.tmp / "my designs"
        spaced.mkdir()
        design = spaced / "d ff.v"
        shutil.copy(DFF, design)
        code, out, err = self.run_flow(str(design), "--top", "bm_DL_logic_w_Dff", "--no-vpr")
        self.assertEqual(code, 0, out + err)
        lpm = spaced / "add sub.v"
        shutil.copy(LPM_MICRO, lpm)
        code, out, err = self.run_flow(str(lpm), "--top", "lpm_add_sub_micro", "--target", "lpm")
        self.assertEqual(code, 0, out + err)
        summary = json.loads((self.tmp / "out" / "summary.json").read_text())
        self.assertIn("seconds", summary)

    def test_summary_names_what_luts_counts(self) -> None:
        code, out, err = self.run_flow(str(DFF), "--top", "bm_DL_logic_w_Dff", "--no-vpr")
        self.assertEqual(code, 0, out + err)
        summary = json.loads((self.tmp / "out" / "summary.json").read_text())
        self.assertIn("luts", summary)
        self.assertIn("luts_note", summary)


if __name__ == "__main__":
    unittest.main()
