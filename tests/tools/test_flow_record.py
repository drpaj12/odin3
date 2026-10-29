"""Tests for tools/flow/runrecord.py and flow_record.py: the Mimir run record of odin3-flow.

The tests that run odin3 need ODIN3_CLI (CTest python_tools); none needs VTR."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import flow_record
import odin3_flow as flow
import runrecord as rr

from .helpers import run_main

ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_CLI", "")
DFF = ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_DL_logic_w_Dff.v"
needs_odin3 = unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")


class TmpCase(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)


class ScriptParsing(unittest.TestCase):
    def test_split_keeps_quoted_semicolons(self) -> None:
        cmds = flow_record.split_script('read_techlib "a;b c"; proc; abc --lut 6')
        self.assertEqual(cmds, [["read_techlib", "a;b c"], ["proc"], ["abc", "--lut", "6"]])

    def test_options_value_and_flag(self) -> None:
        words = ["write_blif", "--vtr", "--lut-k", "6", "out.blif"]
        self.assertEqual(flow_record.script_options(words), [("--vtr", ""), ("--lut-k", "6")])


class Placeholders(TmpCase):
    def rec(self, top: str = "mydesign") -> rr.RunRecorder:
        return rr.RunRecorder("s", "d", "a", 0, "arch", {"out": self.tmp}, top=top)

    def test_paths_and_top(self) -> None:
        rec = self.rec()
        self.assertEqual(rec.ph(f"{self.tmp}/odin3/mydesign.blif"), "<out>/odin3/<top>.blif")
        self.assertEqual(rec.ph("/elsewhere/x.v"), "<abs>/x.v")
        self.assertEqual(rec.ph("mydesign"), "<top>")
        self.assertEqual(rec.ph("--top=mydesign"), "--top=<top>")
        self.assertEqual(rec.ph("a/mydesign_more.v"), "a/mydesign_more.v")

    def test_idempotent(self) -> None:
        rec = self.rec()
        once = rec.ph(f"x {self.tmp}/vpr/run")
        self.assertEqual(rec.ph(once), once)


class HostAlias(TmpCase):
    def test_no_map_is_unknown(self) -> None:
        self.assertEqual(rr.host_alias("some-host", self.tmp / "none.toml"), "unknown")

    def test_map_numbers_a_new_name_and_reads_it_back(self) -> None:
        path = self.tmp / "hosts.toml"
        path.write_text('[hosts]\n"alpha" = "host-1"\n', encoding="utf-8")
        self.assertEqual(rr.host_alias("Alpha", path), "host-1")
        self.assertEqual(rr.host_alias("beta", path), "host-2")
        self.assertEqual(rr.host_alias("beta", path), "host-2")


class Privacy(TmpCase):
    def test_hits_name_the_rule_and_key_not_the_value(self) -> None:
        rec = {"a": {"b": "/home/someone/x"}, "c": ["ok", "x=/abs/path"], "d": "vpr/src/a.cpp:3"}
        hits = rr.privacy_hits(rec)
        self.assertIn("home-path at record.a.b", hits)
        self.assertIn("absolute-path at record.c[1]", hits)
        self.assertFalse(any("record.d" in h for h in hits))
        self.assertFalse(any("someone" in h for h in hits))

    def test_user_name_is_refused(self) -> None:
        with mock.patch("getpass.getuser", return_value="zzuser"):
            hits = rr.privacy_hits({"k": "run by ZZUSER"})
        self.assertEqual(hits, ["user-name at record.k"])


@needs_odin3
class FlowRecord(TmpCase):
    def run_flow(self, *extra: str) -> dict[str, object]:
        env = {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}
        env["MIMIR_HOST_MAP"] = str(self.tmp / "missing.toml")
        out = self.tmp / "out"
        with mock.patch.dict(os.environ, env, clear=True):
            code, so, se = run_main(flow.main, [str(DFF), *extra, "--odin3", ODIN3, "--out",
                                                str(out)])
        self.assertEqual(code, 0, so + se)
        data: dict[str, object] = json.loads((out / "mimir-run.json").read_text(encoding="utf-8"))
        return data

    def test_record_shape(self) -> None:
        rec = self.run_flow()
        self.assertEqual(rec["kind"], "odin3-flow-run")
        self.assertEqual(rec["source"], "odin3-flow")
        self.assertEqual(rec["host"], "unknown")
        self.assertTrue(rec["ok"])
        steps = rec["steps"]
        assert isinstance(steps, list)
        self.assertEqual([s["name"] for s in steps], ["odin3"])
        self.assertIsInstance(steps[0]["peak_rss_mib"], float)
        inputs = rec["inputs"]
        assert isinstance(inputs, dict)
        self.assertEqual(len(inputs["netlist"]["sha256"]), 64)
        self.assertEqual(rr.privacy_hits(rec), [])

    def test_params_cite_their_sources(self) -> None:
        params = self.run_flow()["params"]
        assert isinstance(params, dict)
        self.assertEqual(params["odin3.abc.lut"]["conf"], "given")
        self.assertEqual(params["odin3.abc.unset_options"]["conf"], "default")
        self.assertRegex(params["odin3.abc.unset_options"]["source"], r"args\.c:\d+$")
        self.assertEqual(params["tool.abc_commit"]["conf"], "given")
        self.assertEqual(params["run.seed"]["value"], "none")
        self.assertNotIn("odin3.cmd.top", params)

    def test_command_text_does_not_depend_on_the_design(self) -> None:
        rec = self.run_flow()
        steps = rec["steps"]
        assert isinstance(steps, list)
        self.assertIn("<top>", steps[0]["argv"])
        self.assertNotIn("bm_DL_logic_w_Dff", json.dumps(steps[0]["argv"]).replace(
            "<designs>", ""))
