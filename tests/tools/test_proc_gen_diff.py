"""Tests for the ``--proc`` generator profile and tools/proc-check/proc-gen-diff (2E spec §18.6,
plan Task 12), over fake ``proc-diff`` / ``elab-equiv`` / ``odin3`` scripts that log their argv."""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import proc_gen_diff
import proc_profile
import verilog_gen

from . import helpers

# The fake proc-diff exits $FAKE_PD_RC (default 0), or $FAKE_PD_RC_069 when its --yosys ends in
# "yosys069"; the fake elab-equiv exits $FAKE_EE_RC; the fake odin3 exits $FAKE_OPT_RC.
FAKE_TOOL = """#!{python}
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps([{name!r}, *sys.argv[1:]]) + "\\n")
argv = sys.argv[1:]
code = int(os.environ.get({var!r}, "0"))
if {name!r} == "proc-diff" and any(a.endswith("yosys069") for a in argv):
    code = int(os.environ.get("FAKE_PD_RC_069", str(code)))
print("fake " + {name!r} + " exit " + str(code))
sys.exit(code)
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def seed_with(latch: bool) -> int:
    return next(s for s in range(1, 200)
                if ("latch" in proc_profile.generate(s).features) == latch)


class ProfileTest(unittest.TestCase):
    def test_deterministic_per_seed(self) -> None:
        for seed in (1, 7, 49):
            self.assertEqual(proc_profile.generate(seed), proc_profile.generate(seed))
        self.assertNotEqual(proc_profile.generate(1).text, proc_profile.generate(2).text)

    def test_every_feature_appears_in_seeds_1_to_50(self) -> None:
        seen: set[str] = set()
        for seed in range(1, 51):
            seen |= proc_profile.generate(seed).features
        self.assertEqual(sorted(set(proc_profile.FEATURES) - seen), [])

    def test_features_are_named_in_the_file_and_real(self) -> None:
        for seed in range(1, 51):
            profile = proc_profile.generate(seed)
            first = profile.text.splitlines()[0]
            self.assertEqual(first, "// features: " + " ".join(sorted(profile.features)))
            self.assertLessEqual(profile.features, set(proc_profile.FEATURES))

    def test_text_shows_each_construct(self) -> None:
        text = "".join(proc_profile.generate(s).text for s in range(1, 51))
        for needle in ("casez (", "casex (", "// synopsys full_case", "parallel_case",
                       "negedge", "3'b", "?", "or posedge r", "or negedge r", " [0:3];"):
            self.assertIn(needle, text, needle)

    def test_cli_writes_the_profile_and_no_ast(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "g.v"
            code = verilog_gen.main(["--proc", "--seed", "5", "--out", str(out)])
            self.assertEqual(code, 0)
            self.assertEqual(out.read_text(encoding="utf-8"), proc_profile.generate(5).text)
            self.assertFalse(out.with_suffix(".ast").exists())

    def test_proc_and_mutate_are_exclusive(self) -> None:
        code, _, _ = helpers.run_main(verilog_gen.main, ["--proc", "--mutate", "--seed", "1",
                                                          "--out", "/dev/null"])
        self.assertEqual(code, 2)

    def test_parses_with_yosys_055(self) -> None:
        yosys = os.environ.get("ODIN3_YOSYS055")
        if not yosys:
            self.skipTest("ODIN3_YOSYS055 is not set (no Yosys 0.55)")
        with tempfile.TemporaryDirectory() as tmp:
            for seed in range(1, 51):
                path = Path(tmp) / f"s{seed}.v"
                path.write_text(proc_profile.generate(seed).text, encoding="utf-8")
                done = subprocess.run([yosys, "-q", "-p", f"read_verilog -sv {path}"],
                                      capture_output=True, text=True, check=False)
                self.assertEqual(done.returncode, 0, f"seed {seed}: {done.stderr[-300:]}")


class ProcGenDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.log = self.dir / "argv.log"
        python = sys.executable
        self.proc_diff = _exe(self.dir / "proc-diff", FAKE_TOOL.replace("{python}", python)
                              .replace("{name!r}", "'proc-diff'")
                              .replace("{var!r}", "'FAKE_PD_RC'"))
        self.elab_equiv = _exe(self.dir / "elab-equiv", FAKE_TOOL.replace("{python}", python)
                               .replace("{name!r}", "'elab-equiv'")
                               .replace("{var!r}", "'FAKE_EE_RC'"))
        self.odin3 = _exe(self.dir / "odin3", FAKE_TOOL.replace("{python}", python)
                          .replace("{name!r}", "'odin3'").replace("{var!r}", "'FAKE_OPT_RC'"))
        self.yosys = _exe(self.dir / "yosys", "#!/bin/sh\nexit 0\n")
        self.yosys069 = _exe(self.dir / "yosys069", "#!/bin/sh\nexit 0\n")
        self.equiv_check = _exe(self.dir / "equiv-check", "#!/bin/sh\nexit 0\n")
        self.work = self.dir / "work"
        for name, value in (("PROC_DIFF", self.proc_diff), ("ELAB_EQUIV", self.elab_equiv)):
            patch = mock.patch.object(proc_gen_diff, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        env_patch = mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)})
        env_patch.start()
        self.addCleanup(env_patch.stop)

    def run_tool(self, *args: str, env: dict[str, str] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--equiv-check",
                str(self.equiv_check), "--work", str(self.work), *args]
        with mock.patch.dict(os.environ, env or {}):
            return helpers.run_main(proc_gen_diff.main, argv)

    def calls(self, name: str) -> list[list[str]]:
        if not self.log.exists():
            return []
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        return [row[1:] for row in rows if row[0] == name]

    def test_a_clean_seed_runs_both_latch_modes_and_the_bridge(self) -> None:
        seed = seed_with(latch=False)
        code, out, err = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio")
        self.assertEqual(code, 0, out + err)
        diffs = self.calls("proc-diff")
        self.assertEqual(len(diffs), 2, diffs)
        self.assertEqual(sum("--nolatches" in argv for argv in diffs), 1)
        self.assertTrue(all("--no-trio" in argv for argv in diffs))
        bridges = self.calls("elab-equiv")
        self.assertEqual(len(bridges), 1)
        for flag in ("--pass", "proc", "--unmap-enables", "--async2sync", "--no-trio"):
            self.assertIn(flag, bridges[0])
        self.assertTrue((self.work / f"seed{seed}" / f"seed{seed}.v").is_file())
        self.assertIn(f"seed {seed}: ok", out)

    def test_a_latch_seed_runs_the_default_mode_only(self) -> None:
        seed = seed_with(latch=True)
        code, out, err = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio")
        self.assertEqual(code, 0, out + err)
        diffs = self.calls("proc-diff")
        self.assertEqual(len(diffs), 1)
        self.assertNotIn("--nolatches", diffs[0])
        self.assertEqual(self.calls("elab-equiv"), [])

    def test_a_range_runs_every_seed_in_order(self) -> None:
        code, out, _ = self.run_tool("--seeds", "3..6", "--no-trio")
        self.assertEqual(code, 0)
        order = [int(line.split()[1].rstrip(":")) for line in out.splitlines()
                 if line.startswith("seed ")]
        self.assertEqual(order, [3, 4, 5, 6])

    def test_a_bad_range_is_a_usage_error(self) -> None:
        for bad in ("5", "6..5", "a..b", "0..3"):
            code, _, err = self.run_tool("--seeds", bad)
            self.assertEqual(code, 2, bad)
            self.assertIn("--seeds", err)

    def test_a_difference_fails_and_keeps_the_seed_and_file(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio",
                                     env={"FAKE_PD_RC": "1"})
        self.assertEqual(code, 1)
        self.assertIn(f"seed {seed}: FAIL", out)
        kept = self.work / f"seed{seed}" / f"seed{seed}.v"
        self.assertIn(str(kept), out)
        self.assertTrue(kept.is_file())

    def test_an_error_exit_is_a_failure_too(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio",
                                     env={"FAKE_EE_RC": "2"})
        self.assertEqual(code, 1)
        self.assertIn("bridge", out)

    def test_drift_between_055_and_069_is_classified_not_failed(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio",
                                     "--yosys-069", str(self.yosys069),
                                     env={"FAKE_PD_RC_069": "1"})
        self.assertEqual(code, 0, out)
        self.assertIn("oracle drift", out)
        self.assertNotIn("FAIL", out)
        runs = [argv for argv in self.calls("proc-diff") if str(self.yosys069) in argv]
        self.assertTrue(runs)
        self.assertTrue(all("--allow-version" in argv for argv in runs))

    def test_a_055_difference_is_a_failure_even_when_069_agrees(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio",
                                     "--yosys-069", str(self.yosys069),
                                     env={"FAKE_PD_RC": "1"})
        self.assertEqual(code, 1)
        self.assertIn("FAIL", out)
        self.assertNotIn("oracle drift:", out)

    def test_an_oracle_crash_is_counted_and_not_a_failure(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio",
                                     env={"FAKE_EE_RC": "3"})
        self.assertEqual(code, 0, out)
        self.assertIn("oracle crash", out)
        self.assertIn("1 oracle crashes", out)

    def test_a_compare_tie_is_counted_and_not_a_failure(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", "--no-trio",
                                     env={"FAKE_PD_RC": "3"})
        self.assertEqual(code, 0, out)
        self.assertIn("compare tie", out)
        self.assertNotIn("FAIL", out)

    def test_the_trio_is_probed_with_the_trio_flags(self) -> None:
        seed = seed_with(latch=False)
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}")
        self.assertEqual(code, 0, out)
        probes = self.calls("odin3")
        self.assertEqual(probes, [["-p", "opt -nodffe -nosdff"]])
        self.assertTrue(all("--no-trio" not in argv for argv in self.calls("proc-diff")))
        # opt missing (exit 1): the trio is skipped, and the notice says so
        self.log.write_text("", encoding="utf-8")
        code, out, _ = self.run_tool("--seeds", f"{seed}..{seed}", env={"FAKE_OPT_RC": "1"})
        self.assertEqual(code, 0, out)
        self.assertIn("trio: skipped", out)
        self.assertTrue(all("--no-trio" in argv for argv in self.calls("proc-diff")))


if __name__ == "__main__":
    unittest.main()
