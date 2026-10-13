"""Tests for the ``--elab`` generator profile, tools/elab-check/elab-diff, const-diff and the
fuzzer's seed script (2D spec 14.5, plan Task 28), over fake ``elab-equiv`` / driver scripts that
log their argv."""

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

import const_diff
import elab_diff
import elab_fuzz_seeds
import elab_profile
import gen_const_table
import verilog_gen

from . import helpers

# The fake elab-equiv exits $FAKE_EE_RC (default 0), or $FAKE_EE_RC_069 when its --yosys ends in
# "yosys069"; it logs its argv as JSON to $FAKE_LOG.
FAKE_EQUIV = """#!{python}
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
code = int(os.environ.get("FAKE_EE_RC", "0"))
if any(a.endswith("yosys069") for a in sys.argv[1:]):
    code = int(os.environ.get("FAKE_EE_RC_069", str(code)))
print("fake elab-equiv exit " + str(code))
if os.environ.get("FAKE_UNDECIDED") and code == 2:
    print("Networks are UNDECIDED.   Time =     0.04 sec")
sys.exit(code)
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class ProfileTest(unittest.TestCase):
    def test_deterministic_per_seed(self) -> None:
        for seed in (1, 7, 49):
            self.assertEqual(elab_profile.generate(seed), elab_profile.generate(seed))
        self.assertNotEqual(elab_profile.generate(1).text, elab_profile.generate(2).text)

    def test_every_trap_feature_appears_in_seeds_1_to_50(self) -> None:
        seen: set[str] = set()
        for seed in range(1, 51):
            seen |= set(elab_profile.generate(seed).features)
        self.assertEqual(sorted(set(elab_profile.FEATURES) - seen), [])

    def test_the_first_line_lists_the_features_used(self) -> None:
        for seed in range(1, 51):
            made = elab_profile.generate(seed)
            self.assertEqual(made.text.splitlines()[0],
                             "// features: " + ", ".join(made.features))
            self.assertLessEqual(set(made.features), set(elab_profile.FEATURES))

    def test_text_shows_each_construct(self) -> None:
        text = "".join(elab_profile.generate(s).text for s in range(1, 51))
        for needle in ("===", "!==", "defparam", "#(", "genvar", "generate", "casez (",
                       "casex (", "$signed", "'sd", "+:", "-:", " <= ", "tk(", "reg [3:0] mem ["):
            self.assertIn(needle, text, needle)

    def test_cli_writes_the_profile_and_no_ast(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "g.v"
            code = verilog_gen.main(["--elab", "--seed", "5", "--out", str(out)])
            self.assertEqual(code, 0)
            self.assertEqual(out.read_text(encoding="utf-8"), elab_profile.generate(5).text)
            self.assertFalse(out.with_suffix(".ast").exists())

    def test_elab_proc_and_mutate_are_exclusive(self) -> None:
        for flags in (["--elab", "--mutate"], ["--elab", "--proc"]):
            code, _, _ = helpers.run_main(verilog_gen.main,
                                          [*flags, "--seed", "1", "--out", "/dev/null"])
            self.assertEqual(code, 2, flags)

    def test_parses_with_yosys_055(self) -> None:
        yosys = os.environ.get("ODIN3_YOSYS055")
        if not yosys:
            self.skipTest("ODIN3_YOSYS055 is not set (no Yosys 0.55)")
        with tempfile.TemporaryDirectory() as tmp:
            for seed in range(1, 51):
                path = Path(tmp) / f"s{seed}.v"
                path.write_text(elab_profile.generate(seed).text, encoding="utf-8")
                done = subprocess.run([yosys, "-q", "-p", f"read_verilog -sv {path}"],
                                      capture_output=True, text=True, check=False)
                self.assertEqual(done.returncode, 0, f"seed {seed}: {done.stderr[-300:]}")


class SeedScriptTest(unittest.TestCase):
    def test_seeds_are_a_mode_byte_then_the_file_smallest_first(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src"
            src.mkdir()
            (src / "big.v").write_text("module big; endmodule // padding\n", encoding="utf-8")
            (src / "small.v").write_text("module s;endmodule\n", encoding="utf-8")
            out = Path(tmp) / "out"
            code, _, _ = helpers.run_main(elab_fuzz_seeds.main,
                                           [str(out), str(src), "--generated", "2"])
            self.assertEqual(code, 0)
            seeds = [p.read_bytes() for p in sorted(out.glob("seed_*"))]
            self.assertEqual(len(seeds), 4)
            self.assertEqual(seeds[0], b"\x01module s;endmodule\n")
            self.assertTrue(seeds[1].startswith(b"\x01module big"))
            self.assertEqual(seeds[2][0], 0)
            self.assertEqual(seeds[3][0], 3)
            self.assertIn(b"module gtop", seeds[2])


class ElabDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.log = self.dir / "argv.log"
        self.equiv = _exe(self.dir / "elab-equiv", FAKE_EQUIV.replace("{python}", sys.executable))
        self.odin3 = _exe(self.dir / "odin3", "#!/bin/sh\nexit 0\n")
        self.yosys = _exe(self.dir / "yosys", "#!/bin/sh\nexit 0\n")
        self.yosys069 = _exe(self.dir / "yosys069", "#!/bin/sh\nexit 0\n")
        self.equiv_check = _exe(self.dir / "equiv-check", "#!/bin/sh\nexit 0\n")
        self.work = self.dir / "work"
        patch = mock.patch.object(elab_diff, "ELAB_EQUIV", self.equiv)
        patch.start()
        self.addCleanup(patch.stop)
        env_patch = mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)})
        env_patch.start()
        self.addCleanup(env_patch.stop)

    def run_tool(self, *args: str, env: dict[str, str] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--equiv-check",
                str(self.equiv_check), "--work", str(self.work), *args]
        with mock.patch.dict(os.environ, env or {}):
            return helpers.run_main(elab_diff.main, argv)

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_a_clean_seed_runs_the_bridge_on_gtop(self) -> None:
        code, out, err = self.run_tool("--seeds", "3..3")
        self.assertEqual(code, 0, out + err)
        runs = self.calls()
        self.assertEqual(len(runs), 1)
        for flag in ("--top", "gtop", "--unmap-enables", "--mem-cap"):
            self.assertIn(flag, runs[0])
        self.assertTrue((self.work / "seed3" / "seed3.v").is_file())
        self.assertIn("seed 3: ok", out)

    def test_the_file_is_the_profile_of_the_seed(self) -> None:
        self.run_tool("--seeds", "4..4")
        text = (self.work / "seed4" / "seed4.v").read_text(encoding="utf-8")
        self.assertEqual(text, elab_profile.generate(4).text)

    def test_a_range_runs_every_seed_in_order(self) -> None:
        code, out, _ = self.run_tool("--seeds", "3..6")
        self.assertEqual(code, 0)
        order = [int(line.split()[1].rstrip(":")) for line in out.splitlines()
                 if line.startswith("seed ")]
        self.assertEqual(order, [3, 4, 5, 6])

    def test_a_bad_range_is_a_usage_error(self) -> None:
        for bad in ("5", "6..5", "a..b", "0..3"):
            code, _, err = self.run_tool("--seeds", bad)
            self.assertEqual(code, 2, bad)
            self.assertIn("--seeds", err)

    def test_a_non_equivalence_fails_and_keeps_the_seed_and_file(self) -> None:
        code, out, _ = self.run_tool("--seeds", "2..2", env={"FAKE_EE_RC": "1"})
        self.assertEqual(code, 1)
        self.assertIn("seed 2: FAIL", out)
        kept = self.work / "seed2" / "seed2.v"
        self.assertIn(str(kept), out)
        self.assertTrue(kept.is_file())

    def test_an_error_exit_is_a_failure_too(self) -> None:
        code, out, _ = self.run_tool("--seeds", "2..2", env={"FAKE_EE_RC": "2"})
        self.assertEqual(code, 1)
        self.assertIn("seed 2: FAIL", out)

    def test_drift_between_055_and_069_is_classified_not_failed(self) -> None:
        code, out, _ = self.run_tool("--seeds", "2..2", "--yosys-069", str(self.yosys069),
                                     env={"FAKE_EE_RC_069": "1"})
        self.assertEqual(code, 0, out)
        self.assertIn("oracle drift", out)
        self.assertNotIn("FAIL", out)
        runs = [argv for argv in self.calls() if str(self.yosys069) in argv]
        self.assertEqual(len(runs), 1)
        self.assertIn("--allow-version", runs[0])
        self.assertIn("0.69", runs[0])
        self.assertIn("1 oracle drift", out)

    def test_a_failure_at_055_is_a_failure_whatever_069_says(self) -> None:
        code, out, _ = self.run_tool("--seeds", "2..2", "--yosys-069", str(self.yosys069),
                                     env={"FAKE_EE_RC": "1", "FAKE_EE_RC_069": "0"})
        self.assertEqual(code, 1)
        self.assertEqual(len(self.calls()), 1)
        self.assertIn("FAIL", out)

    def test_an_oracle_crash_is_listed_not_failed(self) -> None:
        code, out, _ = self.run_tool("--seeds", "2..2", env={"FAKE_EE_RC": "3"})
        self.assertIn("seed 2: oracle crash", out)
        self.assertNotIn("seed 2: FAIL", out)
        self.assertIn("1 oracle crashes", out)
        self.assertEqual(code, 1)  # a run that verified nothing never passes

    def test_a_few_crashes_among_many_seeds_pass(self) -> None:
        script = FAKE_EQUIV.replace("{python}", sys.executable).replace(
            'code = int(os.environ.get("FAKE_EE_RC", "0"))',
            'code = 3 if sys.argv[-1].endswith("seed5.v") else 0')
        _exe(self.equiv, script)
        code, out, _ = self.run_tool("--seeds", "1..20")
        self.assertEqual(code, 0, out)
        self.assertIn("seed 5: oracle crash", out)
        self.assertIn("1 oracle crashes", out)

    def test_an_undecided_miter_is_unverified_not_failed(self) -> None:
        code, out, _ = self.run_tool("--seeds", "2..2",
                                     env={"FAKE_EE_RC": "2", "FAKE_UNDECIDED": "1"})
        self.assertIn("seed 2: undecided", out)
        self.assertNotIn("FAIL seed", out)
        self.assertNotIn("seed 2: FAIL", out)
        self.assertEqual(code, 1)  # all of one seed unverified: the run proved nothing

    def test_expect_fail_turns_a_listed_failure_into_an_expected_one(self) -> None:
        listed = self.dir / "xfail.txt"
        listed.write_text("# known\n2: lost write  # why\n", encoding="utf-8")
        code, out, _ = self.run_tool("--seeds", "2..3", "--expect-fail", str(listed),
                                     env={"FAKE_EE_RC": "1"})
        self.assertIn("seed 2: expected failure: lost write", out)
        self.assertIn("seed 3: FAIL", out)
        self.assertIn("1 expected failures", out)
        self.assertEqual(code, 1)  # seed 3 is not listed
        code, out, _ = self.run_tool("--seeds", "2..2", "--expect-fail", str(listed),
                                     env={"FAKE_EE_RC": "1"})
        self.assertEqual(code, 0, out)

    def test_a_listed_seed_that_passes_is_a_failure(self) -> None:
        listed = self.dir / "xfail.txt"
        listed.write_text("2: stale\n", encoding="utf-8")
        code, out, _ = self.run_tool("--seeds", "2..2", "--expect-fail", str(listed))
        self.assertEqual(code, 1)
        self.assertIn("unexpectedly passes", out)

    def test_a_bad_expect_fail_line_is_a_usage_error(self) -> None:
        listed = self.dir / "xfail.txt"
        listed.write_text("two: nope\n", encoding="utf-8")
        code, _, err = self.run_tool("--seeds", "2..2", "--expect-fail", str(listed))
        self.assertEqual(code, 2)
        self.assertIn("expected 'SEED: reason'", err)

    def test_too_many_unverified_seeds_fail_the_run(self) -> None:
        self.assertFalse(elab_diff.too_many_unverified(0, 10))
        self.assertFalse(elab_diff.too_many_unverified(1, 10))
        self.assertTrue(elab_diff.too_many_unverified(2, 10))
        self.assertTrue(elab_diff.too_many_unverified(1, 1))


class ConstDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()

    def run_tool(self, driver_rc: int, *args: str) -> tuple[int, str, str]:
        replay = _exe(self.dir / "const-replay",
                      f"#!/bin/sh\necho 'const-replay: 3 cases, {driver_rc} wrong'\n"
                      f"[ {driver_rc} -ne 0 ] && echo 'const_table: ADD 1 0 1 1 0  got 0'\n"
                      f"exit {driver_rc}\n")
        argv = ["--yosys", "/nonexistent", "--replay", str(replay), "--count", "10", *args]
        canned = mock.patch.object(gen_const_table, "generate", return_value="ADD 1 0 1 1 0\n")
        with canned:
            return helpers.run_main(const_diff.main, argv)

    def test_split_count_gives_half_each(self) -> None:
        self.assertEqual(const_diff.split_count(10), (5, 5))
        self.assertEqual(const_diff.split_count(11), (6, 5))
        self.assertEqual(const_diff.split_count(1), (1, 0))

    def test_agreement_passes(self) -> None:
        code, out, _ = self.run_tool(0)
        self.assertEqual(code, 0, out)
        self.assertIn("10 expressions, all agree", out)

    def test_a_disagreement_fails_and_names_the_case(self) -> None:
        code, out, _ = self.run_tool(1)
        self.assertEqual(code, 1)
        self.assertIn("const_table: ADD 1 0 1 1 0  got 0", out)

    def test_a_refused_yosys_is_a_usage_error(self) -> None:
        refused = mock.patch.object(gen_const_table, "generate",
                                    side_effect=gen_const_table.YosysRefused("not 0.55"))
        with refused:
            code, _, err = helpers.run_main(
                const_diff.main, ["--yosys", "y", "--replay", "r", "--count", "4"])
        self.assertEqual(code, 2)
        self.assertIn("not 0.55", err)

    def test_the_seed_picks_distinct_seeds_for_the_two_tables(self) -> None:
        seeds: list[int] = []

        def fake_generate(yosys: str, seed: int, count: int, mixed: bool = False) -> str:
            del yosys, count, mixed
            seeds.append(seed)
            return "ADD 1 0 1 1 0\n"

        replay = _exe(self.dir / "r", "#!/bin/sh\necho 'const-replay: 1 cases, 0 wrong'\n")
        with mock.patch.object(gen_const_table, "generate", fake_generate):
            code, _, _ = helpers.run_main(const_diff.main, [
                "--yosys", "y", "--replay", str(replay), "--count", "4", "--seed", "9"])
        self.assertEqual(code, 0)
        self.assertEqual(seeds, [18, 19])

    def test_against_the_real_yosys_when_present(self) -> None:
        yosys = os.environ.get("ODIN3_YOSYS055")
        replay = os.environ.get("ODIN3_CONST_REPLAY")
        if not yosys or not replay:
            self.skipTest("ODIN3_YOSYS055 / ODIN3_CONST_REPLAY are not set")
        code, out, err = helpers.run_main(const_diff.main, [
            "--yosys", yosys, "--replay", replay, "--count", "200", "--seed", "5"])
        self.assertEqual(code, 0, out + err)


if __name__ == "__main__":
    unittest.main()
