"""Tests for the ``--opt`` generator profile and tools/opt-check/opt-gen-diff (2F Task 18, spec
20.4). ``opt_diff.Diff`` is replaced by a fake that returns scripted results, so no tool runs."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import opt_diff
import opt_equiv
import opt_gen_diff
import opt_profile
import verilog_gen

from . import helpers


class ProfileTest(unittest.TestCase):
    def test_deterministic_per_seed(self) -> None:
        for seed in (1, 7, 49):
            self.assertEqual(opt_profile.generate(seed), opt_profile.generate(seed))
        self.assertNotEqual(opt_profile.generate(1).text, opt_profile.generate(2).text)

    def test_every_feature_appears_in_seeds_1_to_50(self) -> None:
        seen: set[str] = set()
        for seed in range(1, 51):
            seen |= opt_profile.generate(seed).features
        self.assertEqual(sorted(set(opt_profile.FEATURES) - seen), [])

    def test_features_are_named_in_the_file_and_real(self) -> None:
        for seed in range(1, 51):
            profile = opt_profile.generate(seed)
            self.assertEqual(
                profile.text.splitlines()[0], "// features: " + " ".join(sorted(profile.features))
            )
            self.assertLessEqual(profile.features, set(opt_profile.FEATURES))

    def test_text_shows_each_construct(self) -> None:
        text = "".join(opt_profile.generate(s).text for s in range(1, 51))
        for needle in (
            "default:",
            "endcase",
            "<<",
            ">>>",
            " * ",
            "4'b",
            "if (en)",
            "posedge rst",
            "if (rst)",
        ):
            self.assertIn(needle, text, needle)
        self.assertRegex(text, r"4'b[01]*x[01]*")  # an x bit in an operand

    def test_every_case_without_default_covers_the_selector(self) -> None:
        for seed in range(1, 51):
            text = opt_profile.generate(seed).text
            if "case_no_default" in opt_profile.generate(seed).features:
                self.assertIn("2'd3:", text)

    def test_cli_writes_the_profile_and_no_ast(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "g.v"
            self.assertEqual(verilog_gen.main(["--opt", "--seed", "5", "--out", str(out)]), 0)
            self.assertEqual(out.read_text(encoding="utf-8"), opt_profile.generate(5).text)
            self.assertFalse(out.with_suffix(".ast").exists())

    def test_opt_is_exclusive_with_proc_elab_and_mutate(self) -> None:
        for other in ("--proc", "--elab", "--mutate"):
            code, _, _ = helpers.run_main(
                verilog_gen.main, ["--opt", other, "--seed", "1", "--out", "/dev/null"]
            )
            self.assertEqual(code, 2, other)

    def test_parses_with_yosys_055(self) -> None:
        yosys = os.environ.get("ODIN3_YOSYS055")
        if not yosys:
            self.skipTest("ODIN3_YOSYS055 is not set (no Yosys 0.55)")
        with tempfile.TemporaryDirectory() as tmp:
            for seed in range(1, 51):
                path = Path(tmp) / f"s{seed}.v"
                path.write_text(opt_profile.generate(seed).text, encoding="utf-8")
                done = subprocess.run(
                    [yosys, "-q", "-p", f"read_verilog -sv {path}"],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(done.returncode, 0, f"seed {seed}: {done.stderr[-300:]}")


class FakeChecker:
    """The reverse refinement: ``back`` says whether odin.il refines yosys.il."""

    def __init__(self, back: bool, crash: bool = False) -> None:
        self.back = back
        self.crash = crash
        self.calls: list[str] = []

    def refines(self, gold: Path, gate: Path, tag: str) -> tuple[bool, str]:
        self.calls.append(f"{gold.name}:{gate.name}:{tag}")
        if FakeDiff.back_abort:
            raise opt_equiv.RefineAbort("yosys-refine exit 1: ERROR: Parser error")
        if self.crash:
            raise opt_equiv.CrashError("yosys-refine", "killed (exit -9): boom")
        return self.back, ""


class FakeDiff:
    """Scripted ``opt_diff.Diff``: ``plan`` maps (yosys, script slug) to the differing steps, or
    to an exception to raise."""

    plan: dict[tuple[str, str], object] = {}
    back = True
    crash_back = False
    back_abort = False
    checkers: list[FakeChecker] = []

    def __init__(self, yosys: str) -> None:
        self.yosys = yosys
        self.checker = FakeChecker(FakeDiff.back, FakeDiff.crash_back)
        FakeDiff.checkers.append(self.checker)

    def run(self, script: opt_diff.Script, files: list[str]) -> opt_diff.Result:
        what = FakeDiff.plan.get((self.yosys, script.slug), ())
        if isinstance(what, Exception):
            raise what
        assert isinstance(what, tuple)
        result = opt_diff.Result(script)
        result.differences = {step: f"{step} differs" for step in what}
        result.odin_counters = {"constshift": 1}
        return result


class GenDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.work = Path(self._tmp.name) / "work"
        FakeDiff.plan = {}
        FakeDiff.back = True
        FakeDiff.crash_back = False
        FakeDiff.back_abort = False
        FakeDiff.checkers = []
        patch = mock.patch.object(
            opt_gen_diff, "make_diff", lambda args, yosys, work: FakeDiff(Path(yosys).name)
        )
        patch.start()
        self.addCleanup(patch.stop)

    def run_tool(self, *args: str) -> tuple[int, str, str]:
        argv = ["--odin3", "/bin/odin3", "--yosys", "/bin/yosys55", "--work", str(self.work), *args]
        return helpers.run_main(opt_gen_diff.main, argv)

    def test_clean_seeds_pass_and_write_their_files(self) -> None:
        code, out, _ = self.run_tool("--seeds", "1..3")
        self.assertEqual(code, 0, out)
        self.assertIn("3 seeds, 6 runs, 0 seeds failed, 0 structural", out)
        text = (self.work / "seed2" / "seed2.v").read_text(encoding="utf-8")
        self.assertEqual(text, opt_profile.generate(2).text)

    def test_a_refine_difference_fails_and_names_the_kept_file(self) -> None:
        FakeDiff.plan = {("yosys55", "phase2"): ("refine",)}
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 1, out)
        self.assertIn("seed 1 [phase2]: FAIL refine: refine differs; file kept: ", out)
        self.assertIn("seed1.v", out)
        self.assertIn("[full]: ok", out)

    def test_histogram_and_identity_differences_are_classified_by_the_reverse_check(self) -> None:
        FakeDiff.plan = {("yosys55", "phase2"): ("histogram", "identity")}
        code, out, _ = self.run_tool(
            "--seeds", "1..10", "--script", "phase2", "--script", "opt -full"
        )
        # 10 phase2 runs are structural, each proved in both directions: not counted against the
        # fallback gate (review 7), however many there are
        self.assertEqual(code, 0, out)
        self.assertIn("histogram: structural; identity: structural", out)
        self.assertIn("0 seeds failed, 10 structural (10 proved both ways, 0 not)", out)
        self.assertNotIn("(the limit is", out)
        self.assertTrue(all("odin.il:yosys.il" in c for k in FakeDiff.checkers for c in k.calls))

    def test_structural_pairs_not_proved_both_ways_are_gated(self) -> None:
        FakeDiff.back = False
        FakeDiff.plan = {("yosys55", "phase2"): ("identity",)}
        code, out, _ = self.run_tool("--seeds", "1..10")
        self.assertEqual(code, 1, out)
        self.assertIn("10 structural (0 proved both ways, 10 not)", out)
        self.assertIn(
            "10 of 20 runs are structural and not proved both ways (the limit is 5%)", out
        )
        FakeDiff.plan = {("yosys55", "phase2"): ("identity",)}
        code, out, _ = self.run_tool(
            "--seeds", "1..1", "--script", "phase2", *("--script", "opt -full") * 39
        )
        self.assertEqual(code, 0, out)  # 1 of 40 pairs: under the 5% limit

    def test_a_one_way_refinement_is_named(self) -> None:
        FakeDiff.back = False
        FakeDiff.plan = {("yosys55", "phase2"): ("identity",)}
        code, out, _ = self.run_tool(
            "--seeds",
            "1..1",
            "--script",
            "phase2",
            "--script",
            "opt -full",
            "--script",
            "opt -fast",
            "--script",
            "opt -fast -noff",
            "--expect-structural",
            self.expect_file("1 phase2 identity | listed"),
        )
        self.assertEqual(code, 0, out)
        self.assertIn("seed 1 [phase2]: identity: refines only", out)

    def test_a_yosys_crash_is_an_oracle_crash_and_an_odin3_error_fails(self) -> None:
        FakeDiff.plan = {
            ("yosys55", "phase2"): opt_equiv.CrashError("yosys", "killed: boom"),
            ("yosys55", "full"): opt_equiv.ToolError("odin3 exit 1: bad"),
        }
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 1, out)
        self.assertIn("seed 1 [phase2]: oracle crash: yosys: killed: boom", out)
        self.assertIn("seed 1 [full]: FAIL error: odin3 exit 1: bad", out)

    def test_only_a_crash_of_the_oracle_step_is_oracle_trouble(self) -> None:
        """Review I2: these are failures of ours or of the tools, never tolerated oracle crashes."""
        ours = [
            opt_equiv.CrashError("odin3", "timed out after 600 s"),
            opt_equiv.CrashError("netlist-compare", "timed out after 600 s"),
            opt_equiv.CrashError("yosys (odin lowering)", "killed (exit -11): boom"),
            opt_equiv.ToolError("yosys exit 1: ERROR: read_rtlil in.il"),
            opt_equiv.ToolError("yosys (odin lowering) exit 1: ERROR: bad odin.il"),
            opt_equiv.ToolError("yosys exit 1: timed out"),
        ]
        for exc in ours:
            with self.subTest(exc=str(exc)):
                FakeDiff.plan = {("yosys55", "phase2"): exc}
                code, out, _ = self.run_tool("--seeds", "1..1", "--script", "phase2")
                self.assertIn("seed 1 [phase2]: FAIL error:", out)
                self.assertIn("0 oracle crashes", out)
                self.assertEqual(code, 3 if isinstance(exc, opt_equiv.CrashError) else 1, out)
        self.assertTrue(opt_gen_diff.oracle_trouble(opt_equiv.CrashError("yosys", "killed")))

    def test_a_crash_while_classifying_is_a_failure_not_an_abort(self) -> None:
        FakeDiff.plan = {("yosys55", "phase2"): ("identity",)}
        FakeDiff.crash_back = True
        code, out, _ = self.run_tool("--seeds", "1..2", "--script", "phase2")
        self.assertEqual(code, 3, out)
        self.assertIn("seed 1 [phase2]: FAIL error: yosys-refine: killed", out)
        self.assertIn("seed 2 [phase2]: FAIL error", out)

    def test_every_yosys_child_crashing_fails(self) -> None:
        boom = opt_equiv.CrashError("yosys", "killed: boom")
        FakeDiff.plan = {("yosys55", "phase2"): boom, ("yosys55", "full"): boom}
        code, out, _ = self.run_tool("--seeds", "1..3")
        self.assertEqual(code, 1, out)
        self.assertIn("0 seeds failed", out)
        self.assertIn("6 of 6 runs had a Yosys crash (the limit is 10%)", out)

    def test_a_crash_of_our_own_refinement_check_is_hard_not_an_oracle_crash(self) -> None:
        """Review N1: only Yosys dying on the source it should accept is an oracle crash."""
        FakeDiff.plan = {("yosys55", "phase2"): opt_equiv.CrashError("yosys-refine", "killed")}
        code, out, _ = self.run_tool(
            "--seeds", "1..1", "--script", "phase2", *("--script", "opt -full") * 39
        )
        self.assertEqual(code, 3, out)
        self.assertIn("0 oracle crashes", out)
        self.assertFalse(opt_gen_diff.oracle_trouble(opt_equiv.CrashError("yosys-refine", "x")))

    def test_hard_wins_over_the_crash_and_unproved_limits(self) -> None:
        """Review N2: exit 3 is never lost to an exit-1 gate."""
        FakeDiff.plan = {
            ("yosys55", "phase2"): opt_equiv.CrashError("yosys", "killed"),
            ("yosys55", "full"): opt_equiv.CrashError("odin3", "killed"),
        }
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 3, out)
        FakeDiff.back = False
        FakeDiff.plan = {
            ("yosys55", "phase2"): ("identity",),
            ("yosys55", "full"): opt_equiv.CrashError("odin3", "killed"),
        }
        self.assertEqual(self.run_tool("--seeds", "1..1")[0], 3)

    def test_a_yosys_error_abort_of_our_refinement_check_is_exit_2(self) -> None:
        """Review I3: the forward check and the back-proof share the rule."""
        FakeDiff.plan = {("yosys55", "phase2"): opt_equiv.RefineAbort("yosys-refine exit 1: x")}
        self.assertEqual(self.run_tool("--seeds", "1..1")[0], 2)
        FakeDiff.plan = {("yosys55", "phase2"): ("identity",)}
        FakeDiff.back_abort = True
        self.assertEqual(self.run_tool("--seeds", "1..1")[0], 2)

    def test_a_few_crashes_under_the_limit_pass(self) -> None:
        FakeDiff.plan = {("yosys55", "phase2"): opt_equiv.CrashError("yosys", "timed out")}
        code, out, _ = self.run_tool(
            "--seeds", "1..1", "--script", "phase2", *("--script", "opt -full") * 9
        )
        self.assertEqual(code, 0, out)  # 10 pairs, 1 crash: exactly the 10% limit
        self.assertIn("1 oracle crashes", out)
        self.assertNotIn("had a Yosys crash", out)

    def expect_file(self, *lines: str) -> str:
        path = Path(self._tmp.name) / "expect.txt"
        path.write_text("# header\n" + "\n".join(lines) + "\n", encoding="utf-8")
        return str(path)

    def test_an_unlisted_structural_pair_fails_and_a_listed_one_passes(self) -> None:
        FakeDiff.plan = {("yosys55", "phase2"): ("identity",)}
        listed = self.expect_file("1 phase2 identity | cmpzero odin3 0 yosys 2")
        code, out, _ = self.run_tool("--seeds", "1..2", "--expect-structural", listed)
        self.assertEqual(code, 1, out)
        self.assertNotIn("seed 1 [phase2]: FAIL", out)
        self.assertIn("seed 2 [phase2]: FAIL unexpected structural identity", out)
        self.assertNotIn("(the limit is", out)

    def test_a_listed_pair_that_is_gone_is_stale_and_a_bad_list_is_a_usage_error(self) -> None:
        stale = self.expect_file("1 opt -full identity | gone")
        code, out, _ = self.run_tool("--seeds", "1..1", "--expect-structural", stale)
        self.assertEqual(code, 0, out)
        self.assertIn("stale: seed 1 [full] identity", out)
        bad = self.expect_file("1 phase2 identity")
        code, _, err = self.run_tool("--seeds", "1..1", "--expect-structural", bad)
        self.assertEqual(code, 2)
        self.assertIn("expected `SEED SCRIPT STEP | reason`", err)

    def test_the_checked_in_list_parses(self) -> None:
        table = opt_gen_diff.load_expected(
            Path(__file__).resolve().parents[1] / "opt" / "gen_structural_expect.txt"
        )
        self.assertIn((153, "phase2", "histogram"), table)
        self.assertIn((100, "phase2", "histogram"), table)

    def test_drift_is_a_difference_between_0_55_and_0_69(self) -> None:
        FakeDiff.plan = {
            ("yosys069", "phase2"): ("identity",),
            ("yosys55", "full"): ("identity",),
            ("yosys069", "full"): ("identity",),
        }
        code, out, _ = self.run_tool(
            "--seeds",
            "1..1",
            "--yosys-069",
            "/bin/yosys069",
            "--script",
            "phase2",
            "--script",
            "opt -full",
            "--script",
            "opt -fast",
            "--script",
            "opt -fast -noff",
            "--expect-structural",
            self.expect_file("1 opt -full identity | listed"),
        )
        self.assertIn(
            "seed 1 [phase2]: oracle drift: 0.55 differs in nothing, 0.69 in ['identity']", out
        )
        self.assertIn("seed 1 [full]: identity: structural", out)  # alike on both: no drift
        self.assertNotIn("[full]: identity: structural; oracle drift", out)
        self.assertIn("1 oracle drift", out)
        self.assertEqual(code, 0, out)

    def test_seed_ranges(self) -> None:
        self.assertEqual(list(opt_gen_diff.parse_seeds("3..5")), [3, 4, 5])
        for bad in ("5..3", "0..2", "x", "1-3"):
            with self.assertRaises(opt_gen_diff.GenDiffError):
                opt_gen_diff.parse_seeds(bad)
        code, _, err = self.run_tool("--seeds", "9..2")
        self.assertEqual(code, 2)
        self.assertIn("expected A..B", err)


if __name__ == "__main__":
    unittest.main()
