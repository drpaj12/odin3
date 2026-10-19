"""Tests for the ``--lower`` generator profile and tools/lower-check/lower-gen-diff (3B Task 9,
spec 14.5). ``lower_gen_diff.run_tool`` is replaced by a scripted fake, so no tool runs."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from unittest import mock

import lower_equiv
import lower_gen_diff as lgd
import lower_profile
import verilog_gen

from . import helpers

SEEDS = range(1, 201)


class ProfileTest(unittest.TestCase):
    def test_deterministic_per_seed(self) -> None:
        for seed in (1, 7, 49, 200):
            self.assertEqual(lower_profile.generate(seed), lower_profile.generate(seed))
        self.assertNotEqual(lower_profile.generate(1).text, lower_profile.generate(3).text)

    def test_ten_consecutive_seeds_hold_every_kind(self) -> None:
        for first in (1, 5, 133):
            kinds = {lower_profile.generate(s).kind for s in range(first, first + 10)}
            self.assertEqual(kinds, set(lower_profile.KINDS), first)

    def test_every_feature_appears_in_the_200_seeds(self) -> None:
        seen: set[str] = set()
        for seed in SEEDS:
            seen |= lower_profile.generate(seed).features
        self.assertEqual(sorted(set(lower_profile.FEATURES) - seen), [])

    def test_header_names_the_kind_and_the_real_features(self) -> None:
        for seed in range(1, 61):
            profile = lower_profile.generate(seed)
            lines = profile.text.splitlines()
            self.assertEqual(lines[0], f"// kind: {profile.kind}")
            self.assertEqual(lines[1], "// features: " + " ".join(sorted(profile.features)))
            self.assertLessEqual(profile.features, set(lower_profile.FEATURES))

    def test_text_shows_each_cell_class(self) -> None:
        text = "".join(lower_profile.generate(s).text for s in SEEDS)
        for needle in (" * ", " / ", " % ", " << s", " >>> ", "$signed(", "$unsigned(", "case (s)",
                       "[s +: ", "negedge clk", "posedge rst", "if (!en)", "reg [", "<<<"):
            self.assertIn(needle, text, needle)
        for op in ("==", "!=", "<=", ">=", "&&", "||", "~^", "-("):
            self.assertIn(op, text, op)

    def test_widths_and_signs_are_mixed(self) -> None:
        text = lower_profile.generate(1).text
        self.assertIn("input [7:0] a", text)
        self.assertIn("input signed [6:0] c", text)
        self.assertIn("input signed [3:0] d", text)

    def test_every_output_is_driven_and_declared_once(self) -> None:
        for seed in range(1, 61):
            text = lower_profile.generate(seed).text
            header = text.split("module t (", 1)[1].split(");", 1)[0]
            names = [part.split()[-1] for part in header.split(", ")]
            self.assertEqual(len(names), len(set(names)), seed)

    def test_cli_writes_the_profile_and_no_ast(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "g.v"
            self.assertEqual(verilog_gen.main(["--lower", "--seed", "5", "--out", str(out)]), 0)
            self.assertEqual(out.read_text(encoding="utf-8"), lower_profile.generate(5).text)
            self.assertFalse(out.with_suffix(".ast").exists())

    def test_lower_is_exclusive_with_the_other_profiles_and_mutate(self) -> None:
        for other in ("--proc", "--elab", "--opt", "--flatten", "--mutate"):
            code, _, _ = helpers.run_main(verilog_gen.main, ["--lower", other, "--seed", "1",
                                                              "--out", "/dev/null"])
            self.assertEqual(code, 2, other)

    def test_parses_with_yosys_055(self) -> None:
        yosys = os.environ.get("ODIN3_YOSYS055")
        if not yosys:
            self.skipTest("ODIN3_YOSYS055 is not set (no Yosys 0.55)")
        with tempfile.TemporaryDirectory() as tmp:
            for seed in SEEDS:
                path = Path(tmp) / f"s{seed}.v"
                path.write_text(lower_profile.generate(seed).text, encoding="utf-8")
                done = subprocess.run([yosys, "-q", "-p", f"read_verilog -sv {path}; proc; opt"],
                                      capture_output=True, text=True, check=False)
                self.assertEqual(done.returncode, 0, f"seed {seed}: {done.stderr[-300:]}")


class Scripted:
    """``run_tool`` returning, per tool name, a (code, output) or a function of the seed."""

    def __init__(self) -> None:
        self.diff: tuple[int, str] = (0, "lower-diff: seed: ok (3 cells)\n")
        self.equiv: tuple[int, str] = (0, "lower-equiv: seed: ok (3 cells)\n")
        self.calls: list[list[str]] = []

    def __call__(self, argv: Sequence[str], cwd: Path) -> tuple[int, str]:
        self.calls.append(list(argv))
        return self.diff if "lower-diff" in argv[0] else self.equiv


class DriverTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.work = Path(self._tmp.name) / "work"
        self.fake = Scripted()
        patch = mock.patch.object(lgd, "run_tool", self.fake)
        patch.start()
        self.addCleanup(patch.stop)

    def run_tool(self, *args: str) -> tuple[int, str, str]:
        argv = ["--odin3", "/bin/odin3", "--yosys", "/bin/yosys55", "--parmys-techlibs", "/x",
                "--work", str(self.work), *args]
        return helpers.run_main(lgd.main, argv)

    def test_clean_seeds_pass_and_write_their_files(self) -> None:
        code, out, _ = self.run_tool("--seeds", "1..10")
        self.assertEqual(code, 0, out)
        self.assertIn("seeds 1..10: 10 ok, 0 structural, 0 unproven, 0 empty, 0 differ, 0 BUG; "
                      "compared 10 of 10", out)
        text = (self.work / "seed2" / "seed2.v").read_text(encoding="utf-8")
        self.assertEqual(text, lower_profile.generate(2).text)

    def test_each_kind_gets_its_family_and_latches_its_flag(self) -> None:
        self.run_tool("--seeds", "1..10")
        diffs = [c for c in self.fake.calls if "lower-diff" in c[0]]
        families = {c[c.index("--family") + 1] for c in diffs}
        self.assertEqual(families, {"arith", "ff", "latch", "mem"})
        for call in self.fake.calls:
            if "lower-equiv" in call[0]:
                kind = Path(call[-1]).read_text(encoding="utf-8").splitlines()[0]
                self.assertEqual("--latches" in call, kind == "// kind: latch")

    def test_not_a_refinement_is_a_bug_and_exits_1(self) -> None:
        self.fake.equiv = (1, "lower-equiv: s.v: NOT A REFINEMENT: trace on y\n")
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 1, out)
        self.assertIn("seed 1 [comb]: BUG", out)
        self.assertIn("kept: ", out)
        self.assertIn("1 BUG", out)

    def test_a_lower_diff_difference_fails_the_seed(self) -> None:
        self.fake.diff = (1, "lower-diff: s.v [arith/T]: histogram: DIFFERS: x\n")
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 1, out)
        self.assertIn("seed 1 [comb]: FAIL", out)

    def test_unproven_is_counted_not_failed_unless_strict(self) -> None:
        self.fake.equiv = (4, "lower-equiv: s.v: UNPROVEN: SAT timeout\n")
        code, out, _ = self.run_tool("--seeds", "1..2", "--min-compared", "0")
        self.assertEqual(code, 0, out)
        self.assertIn("0 ok, 0 structural, 2 unproven", out)
        self.assertIn("UNPROVEN: SAT timeout", out)
        code, _, _ = self.run_tool("--seeds", "1..2", "--strict-unproven", "--min-compared", "0")
        self.assertEqual(code, 4)

    def test_a_lower_diff_unproven_is_a_measured_fact_not_a_difference(self) -> None:
        self.fake.diff = (1, "lower-diff: s.v [ff/D]: equiv: UNPROVEN: equiv-check: unsupported\n")
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 0, out)
        self.assertIn("seed 1 [comb]: unproven (diff unproven, equiv ok)", out)

    def test_empty_seeds_do_not_count_as_compared(self) -> None:
        self.fake.diff = (1, "lower-diff: s.v: compared: NOTHING TO COMPARE\n")
        self.fake.equiv = (1, "lower-equiv: s.v: NOTHING TO COMPARE: folded\n")
        code, out, _ = self.run_tool("--seeds", "1..3", "--min-compared", "0")
        self.assertEqual(code, 0, out)
        self.assertIn("3 empty", out)
        code, _, err = self.run_tool("--seeds", "1..3", "--min-compared", "1")
        self.assertEqual(code, 2)
        self.assertIn("below --min-compared", err)

    def test_a_crash_is_hard_and_stops_the_run(self) -> None:
        self.fake.equiv = (3, "lower-equiv: HARD: killed\n")
        code, out, err = self.run_tool("--seeds", "1..5")
        self.assertEqual(code, 3, out)
        self.assertIn("HARD", err)
        self.assertNotIn("seed 2", out)

    def test_bad_seed_range_is_a_usage_error(self) -> None:
        for text in ("0..3", "5..2", "x"):
            code, _, err = self.run_tool("--seeds", text)
            self.assertEqual(code, 2, text)
            self.assertIn("expected A..B", err)

    HIST = ("lower-diff: seed1.v [arith/T]: histogram: DIFFERS: module t: odin3 $_AND_ 10, "
            "$_OR_ 5; yosys $_AND_ 11, $_OR_ 4\n")

    def expect_file(self, text: str) -> Path:
        path = Path(self._tmp.name) / "expect.txt"
        path.write_text(text, encoding="utf-8")
        return path

    def test_a_histogram_only_difference_fails_unless_listed_within_the_limit(self) -> None:
        self.fake.diff = (1, self.HIST)
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 1, out)
        self.assertIn("not listed", out)
        listed = self.expect_file("seed1/seed1.v arith histogram structural\n")
        code, out, _ = self.run_tool("--seeds", "1..1", "--expect", str(listed))
        self.assertEqual(code, 0, out)
        self.assertIn("1 ok", out)

    def test_a_listed_seed_with_a_big_delta_or_no_difference_fails(self) -> None:
        listed = self.expect_file("seed1/seed1.v arith histogram structural\n")
        self.fake.diff = (1, self.HIST.replace("_AND_ 10", "_AND_ 30"))
        code, out, _ = self.run_tool("--seeds", "1..1", "--expect", str(listed))
        self.assertEqual(code, 1, out)
        self.assertIn("above the listed limit", out)
        self.fake.diff = (0, "lower-diff: seed1.v: ok (3 cells)\n")
        code, out, _ = self.run_tool("--seeds", "1..1", "--expect", str(listed))
        self.assertEqual(code, 1, out)
        self.assertIn("no longer differs", out)

    def test_the_list_never_absorbs_an_equiv_or_other_step(self) -> None:
        listed = self.expect_file("seed1/seed1.v arith histogram structural\n")
        self.fake.diff = (1, self.HIST + "lower-diff: seed1.v [arith/T]: equiv: DIFFERS: x\n")
        code, out, _ = self.run_tool("--seeds", "1..1", "--expect", str(listed))
        self.assertEqual(code, 1, out)
        self.assertIn("seed 1 [comb]: FAIL", out)

    def test_report_mode_counts_a_histogram_difference_but_not_an_equiv_one(self) -> None:
        self.fake.diff = (1, self.HIST)
        code, out, _ = self.run_tool("--seeds", "1..2", "--tier-t", "report")
        self.assertEqual(code, 0, out)
        self.assertIn("0 ok, 2 structural", out)
        self.fake.diff = (1, self.HIST + "lower-diff: s [arith/T]: equiv: DIFFERS: x\n")
        code, _, _ = self.run_tool("--seeds", "1..1", "--tier-t", "report")
        self.assertEqual(code, 1)

    def test_a_stale_line_beside_unproven_is_not_softened(self) -> None:
        self.fake.diff = (1, "lower-diff: s: equiv: UNPROVEN: x\n"
                             "lower-diff: s: histogram: stale entry in the expect list (r)\n")
        code, out, _ = self.run_tool("--seeds", "1..1")
        self.assertEqual(code, 1, out)

    def test_a_child_exit_2_or_77_ends_the_run_with_that_code(self) -> None:
        self.fake.diff = (2, "lower-diff: error: map adff2dff.v not found\n")
        code, out, err = self.run_tool("--seeds", "1..5")
        self.assertEqual(code, 2, out)
        self.assertNotIn("seed 2", out)
        self.fake.diff = (0, "ok\n")
        self.fake.equiv = (77, "skipped\n")
        code, _, _ = self.run_tool("--seeds", "1..5")
        self.assertEqual(code, 77)

    def test_no_vtr_root_and_no_techlibs_is_a_skip(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("VTR_ROOT", None)
            code, _, err = helpers.run_main(lgd.main, ["--odin3", "o", "--yosys", "y",
                                                       "--seeds", "1..1"])
        self.assertEqual(code, 77)
        self.assertIn("VTR_ROOT", err)

    def test_a_bad_expect_line_is_a_usage_error(self) -> None:
        bad = self.expect_file("seed1/seed1.v arith equiv nope\n")
        code, _, err = self.run_tool("--seeds", "1..1", "--expect", str(bad))
        self.assertEqual(code, 2)
        self.assertIn("want '<seedN/seedN.v> arith histogram", err)


class LatchesFlagTest(unittest.TestCase):
    def test_snapshot_script_keeps_latches_when_asked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            keep = lower_equiv.snapshot_script(["x.v"], None, None, ["proc"], "", run, False)
            drop = lower_equiv.snapshot_script(["x.v"], None, None, ["proc"], "", run)
        self.assertNotIn("--nolatches", keep)
        self.assertIn("--nolatches", drop)


if __name__ == "__main__":
    unittest.main()
