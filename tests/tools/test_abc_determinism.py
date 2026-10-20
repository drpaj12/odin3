"""Tests for tools/abc-check/abc-determinism (3D Task 22), with a fake odin3."""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import abc_determinism
import abc_equiv

from . import helpers

# The fake odin3 copies the design it reads to write_blif's target (so a design with three equal
# models comes out with three equal models) and, with --via-blif D, writes D/abc0.in.blif, .cmd
# (which names D), .out.blif and .log.  The design's name says what goes wrong:
#   rand: write_blif differs between runs      cmd: abc0.cmd differs between runs
#   logonly: only abc0.log and the ABC time stamp of abc0.out.blif differ (not compared)
#   outbody: abc0.out.blif differs after its time stamp    third: the third model of a copy differs
#   debug: the Debug binary (named odin3-debug) writes another write_blif
#   scriptdiff: write_blif differs between runs only with --script (the child route)
#   nofiles: with --via-blif no exchange file is written
#   fail: odin3 exits 1
FAKE_ODIN3 = """#!{python}
import json, os, re, sys, time
argv = sys.argv
script = argv[2]
with open(os.environ["FAKE_LOG"], "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"bin": os.path.basename(argv[0]), "script": script,
                         "env": os.environ.get("ODIN3_ABC_EXCHANGE")}}) + "\\n")
design = re.search(r"read_blif (\\S+);", script).group(1)
target = re.search(r"write_blif (\\S+)$", script).group(1)
via = re.search(r"--via-blif ([^;\\s]+)", script)
name = os.path.basename(design)
if "fail" in name:
    sys.stderr.write("abc: it broke\\n")
    sys.exit(1)
text = open(design, encoding="utf-8").read()
tick = str(time.time_ns())
if "rand" in name:
    text += "# " + tick + "\\n"
if "scriptdiff" in name and "--script" in script:
    text += "# " + tick + "\\n"
if "debug" in name and os.path.basename(argv[0]) == "odin3-debug":
    text += "# debug build\\n"
if "third" in name:
    parts = text.split(".model")
    if len(parts) > 3:
        parts[3] += "# only the third\\n"
    text = ".model".join(parts)
if via and "nofiles" not in name:
    os.makedirs(via.group(1), exist_ok=True)
    base = via.group(1)
    open(base + "/abc0.in.blif", "w").write("in\\n")
    open(base + "/abc0.cmd", "w").write("read " + base + "/abc0.in.blif\\n"
                                       + ("# " + tick + "\\n" if "cmd" in name else ""))
    stamp = "# Benchmark \\"o3abc\\" written by ABC on " + tick + "\\n"
    open(base + "/abc0.out.blif", "w").write(stamp + "out\\n"
                                            + ("# " + tick + "\\n" if "outbody" in name else ""))
    open(base + "/abc0.log", "w").write("time " + tick + "\\n")
open(target, "w", encoding="utf-8").write(text)
"""

DESIGN = ".model t\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n"


class AbcDeterminismTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.odin3 = self.script("odin3", FAKE_ODIN3)
        self.dbg = self.script("odin3-debug", FAKE_ODIN3)
        self.log = self.root / "fake.log"
        patcher = mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.keep = self.root / "keep"
        self.sleeps: list[float] = []

    def script(self, name: str, text: str) -> str:
        path = self.root / "bin" / name
        path.parent.mkdir(exist_ok=True)
        path.write_text(text.format(python=sys.executable), encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def design(self, rel: str, text: str = DESIGN) -> str:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return str(path)

    def calls(self) -> list[dict[str, str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def run_tool(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", self.odin3, "--keep", str(self.keep), *extra]
        return helpers.run_main(lambda a: abc_determinism.main(a, self.sleeps.append), argv)

    def test_two_equal_processes_are_same(self) -> None:
        design = self.design("d/ok.blif")
        code, out, err = self.run_tool(design)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(out.splitlines()[0], f"{design} same")
        self.assertIn("1 designs: 1 same, 0 differs, 0 error", out)
        calls = self.calls()
        self.assertEqual(len(calls), 4)  # the default script twice, the child route twice
        self.assertNotIn("--script", calls[0]["script"])
        self.assertIn("--script", calls[2]["script"])
        self.assertIn("--via-blif", calls[2]["script"])
        self.assertEqual(self.sleeps, [])

    def test_the_child_route_row_finds_a_difference_only_it_shows(self) -> None:
        design = self.design("d/scriptdiff.blif")
        code, out, _ = self.run_tool(design)
        self.assertEqual(code, 1)
        self.assertIn(f"{design} differs a:child:write_blif", out)

    def test_via_blif_with_no_exchange_files_at_all_is_an_error(self) -> None:
        design = self.design("d/nofiles.blif")
        code, out, err = self.run_tool("--via-blif", design)
        self.assertEqual(code, 2)
        self.assertIn(f"{design} same (no exchange files)", out)
        self.assertIn("nothing was compared", err)

    def test_a_design_without_exchange_files_among_others_is_noted(self) -> None:
        empty = self.design("d/nofiles.blif")
        ok = self.design("d/ok.blif")
        code, out, err = self.run_tool("--via-blif", empty, ok)
        self.assertEqual(code, 0, out + err)
        self.assertIn(f"{empty} same (no exchange files)", out)
        self.assertIn(f"{ok} same\n", out)

    def test_only_a_first_line_time_stamp_is_ignored(self) -> None:
        left, right = self.root / "l.blif", self.root / "r.blif"
        left.write_text('# Benchmark "a" written by ABC on Mon\nbody\n', encoding="utf-8")
        right.write_text('# Benchmark "b" written by ABC on Tue\nbody\n', encoding="utf-8")
        self.assertTrue(abc_determinism.same_bytes(left, right))
        left.write_text('body\n# Benchmark "a" written by ABC on Mon\n', encoding="utf-8")
        right.write_text('body\n# Benchmark "b" written by ABC on Tue\n', encoding="utf-8")
        self.assertFalse(abc_determinism.same_bytes(left, right))

    def test_mode_c_compares_the_exchange_files_across_the_gap(self) -> None:
        design = self.design("d/cmd.blif")
        code, out, _ = self.run_tool("--mode", "c", "--via-blif", design)
        self.assertEqual(code, 1)
        self.assertIn(f"{design} differs c:abc0.cmd", out)
        self.assertEqual(self.sleeps, [60])

    def test_a_different_write_blif_differs(self) -> None:
        design = self.design("d/rand.blif")
        code, out, _ = self.run_tool(design)
        self.assertEqual(code, 1)
        self.assertEqual(out.splitlines()[0], f"{design} differs a:write_blif")

    def test_via_blif_compares_the_exchange_files_but_never_the_log(self) -> None:
        design = self.design("d/logonly.blif")
        code, out, err = self.run_tool("--via-blif", design)
        self.assertEqual(code, 0, out + err)
        self.assertIn(f"{design} same", out)
        for call in self.calls():
            self.assertIn("--via-blif", call["script"])
        cmd = self.design("d/cmd.blif")
        code, out, _ = self.run_tool("--via-blif", cmd)
        self.assertEqual(code, 1)
        self.assertIn(f"{cmd} differs a:abc0.cmd", out)

    def test_a_different_out_blif_body_differs(self) -> None:
        design = self.design("d/outbody.blif")
        code, out, _ = self.run_tool("--via-blif", design)
        self.assertEqual(code, 1)
        self.assertIn(f"{design} differs a:abc0.out.blif", out)

    def test_the_via_blif_directory_is_the_same_path_in_both_runs(self) -> None:
        design = self.design("d/ok.blif")
        self.run_tool("--via-blif", design)
        paths = {
            c["script"].split("--via-blif ")[1].split(" ")[0].split(";")[0] for c in self.calls()
        }
        self.assertEqual(len(paths), 2)  # the default row's and the child row's, each shared

    def test_without_via_blif_the_environment_is_passed_through(self) -> None:
        design = self.design("d/ok.blif")
        with mock.patch.dict(os.environ, {"ODIN3_ABC_EXCHANGE": "aig"}):
            self.run_tool(design)
        self.assertEqual({c["env"] for c in self.calls()}, {"aig"})

    def test_mode_b_three_copies_in_one_process(self) -> None:
        design = self.design("d/ok.blif")
        code, out, err = self.run_tool("--mode", "b", design)
        self.assertEqual(code, 0, out + err)
        calls = self.calls()
        self.assertEqual(len(calls), 2)  # the default script and the fraig one, one process each
        self.assertNotIn("--script", calls[0]["script"])
        self.assertIn("--script", calls[1]["script"])
        fraig = (self.keep / "0000-ok" / "b" / "fraig.abc").read_text(encoding="utf-8")
        self.assertEqual(fraig, "strash\nfraig\nif -K 6\n")
        three = (self.keep / "0000-ok" / "b" / "ok-default.in.blif").read_text(encoding="utf-8")
        self.assertEqual(three.count(".model"), 3)
        self.assertIn(".model t__o3det2", three)

    def test_mode_b_finds_a_copy_that_differs(self) -> None:
        design = self.design("d/third.blif")
        code, out, _ = self.run_tool("--mode", "b", design)
        self.assertEqual(code, 1)
        self.assertIn(f"{design} differs b:third-default.out.blif:t__o3det3", out)

    def test_mode_c_sleeps_sixty_seconds_between_the_runs(self) -> None:
        design = self.design("d/ok.blif")
        code, out, err = self.run_tool("--mode", "c", design)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(self.sleeps, [60])
        rand = self.design("d/rand.blif")
        code, out, _ = self.run_tool("--mode", "c", rand)
        self.assertEqual(code, 1)
        self.assertIn(f"{rand} differs c:write_blif", out)

    def test_mode_d_runs_release_then_debug(self) -> None:
        design = self.design("d/ok.blif")
        code, out, err = self.run_tool("--mode", "d", "--odin3-debug", self.dbg, design)
        self.assertEqual(code, 0, out + err)
        self.assertEqual([c["bin"] for c in self.calls()], ["odin3", "odin3-debug"])
        bad = self.design("d/debug.blif")
        code, out, _ = self.run_tool("--mode", "d", "--odin3-debug", self.dbg, bad)
        self.assertEqual(code, 1)
        self.assertIn(f"{bad} differs d:write_blif", out)

    def test_mode_d_without_the_debug_binary_is_exit_two(self) -> None:
        design = self.design("d/ok.blif")
        code, _, err = self.run_tool("--mode", "d", design)
        self.assertEqual(code, 2)
        self.assertIn("--odin3-debug", err)
        self.assertEqual(self.calls(), [])

    def test_mode_d_refuses_a_vtr_golden(self) -> None:
        golden = self.root / "golden" / "arch"
        design = self.design("golden/arch/vtr/x/x.parmys.blif")
        with (
            mock.patch.object(abc_equiv, "GOLDEN_ROOT", self.root / "golden"),
            mock.patch("abc_equiv.qor_heavy.require_heavy"),
        ):
            code, _, err = self.run_tool("--mode", "d", "--odin3-debug", self.dbg, design)
        self.assertEqual(code, 2)
        self.assertIn("micros", err)
        self.assertTrue(golden.exists())

    def test_a_golden_path_needs_a_heavy_run(self) -> None:
        design = self.design("golden/arch/vtr/x/x.parmys.blif")
        with mock.patch.object(abc_equiv, "GOLDEN_ROOT", self.root / "golden"):
            code, _, err = self.run_tool(design)
        self.assertEqual(code, 2)
        self.assertIn("heavy", err.lower())

    def test_an_odin3_failure_is_exit_two(self) -> None:
        design = self.design("d/fail.blif")
        code, out, _ = self.run_tool(design)
        self.assertEqual(code, 2)
        self.assertIn(f"{design} error odin3 failed: abc: it broke", out)

    def test_designs_run_smallest_first_and_listed_ones_are_skipped(self) -> None:
        big = self.design("d/big.blif", DESIGN + "# padding\n" * 20)
        small = self.design("d/small.blif")
        listed = self.design("d/listed.blif", DESIGN + "# x\n")
        expect = self.root / "expect.txt"
        expect.write_text("d/listed.blif error a reason\n", encoding="utf-8")
        code, out, err = self.run_tool("--expect-fail", str(expect), str(self.root / "d"))
        self.assertEqual(code, 0, out + err)
        lines = [line.split(" ")[0] for line in out.splitlines()[:2]]
        self.assertEqual(lines, [small, big])
        self.assertNotIn(listed, out)

    def test_a_missing_design_is_exit_two(self) -> None:
        code, _, err = self.run_tool(str(self.root / "missing.blif"))
        self.assertEqual(code, 2)
        self.assertIn("no such file", err)


if __name__ == "__main__":
    unittest.main()
