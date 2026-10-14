"""Tests for tools/flatten-check/fuzz_flatten (3A Task 5, spec 14.5): the legs, the tally and the
reproducer, with a fake Odin III that plants a bug (the simulation differs, or flatten fails)."""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from unittest import mock

import flatten_common as fc
import flatten_diff as fd
import fuzz_flatten as ff


class FakeOdin:
    """Stands in for ``fc.run_capped``: scripted by ``mode`` ("good", "break-sim",
    "break-flatten", "skip-sim")."""

    def __init__(self, mode: str) -> None:
        self.mode = mode
        self.calls: list[str] = []

    def __call__(self, argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
        script = argv[argv.index("-p") + 1]
        self.calls.append(script)
        code, text = 0, ""
        if "sim " in script:
            files = re.findall(r"sim \d+ \d+ ([^;\s]+)", script)
            if self.mode == "skip-sim":
                return subprocess.CompletedProcess(argv, 1, "", "error: cannot simulate `$dff`")
            Path(files[0]).write_text("v1\n")
            Path(files[1]).write_text("v2\n" if self.mode == "break-sim" else "v1\n")
        elif self.mode == "break-flatten":
            code, text = 1, "error: flatten: boom"
        return subprocess.CompletedProcess(argv, code, "", text)


def args(cases: int, repro: Path) -> argparse.Namespace:
    return argparse.Namespace(
        cases=cases, seed=10, odin3="odin3", yosys=None, equiv=False, repro_dir=repro
    )


class FuzzTests(unittest.TestCase):
    def _run(self, mode: str, cases: int = 3) -> tuple[ff.Tally, FakeOdin, Path]:
        tmp = Path(tempfile.mkdtemp())
        fake = FakeOdin(mode)
        with mock.patch.object(fc, "run_capped", fake), mock.patch("builtins.print"):
            tally = ff.fuzz(args(cases, tmp / "repro"))
        return tally, fake, tmp / "repro"

    def test_good_cases_pass(self) -> None:
        tally, fake, repro = self._run("good")
        self.assertEqual((tally.cases, tally.failures, tally.sim_equal), (3, [], 3))
        self.assertFalse(repro.exists())
        self.assertEqual(len(fake.calls), 6)  # two Odin III runs per case

    def test_planted_sim_bug_is_reported_and_written(self) -> None:
        tally, _, repro = self._run("break-sim", 2)
        self.assertEqual(len(tally.failures), 2)
        self.assertIn("the simulation differs after flatten", tally.failures[0])
        written = sorted(repro.glob("seed*.v"))
        self.assertEqual([p.name for p in written], ["seed10.v", "seed11.v"])
        first = written[0].read_text()
        self.assertTrue(first.startswith("// leg: sim: FAIL"))
        self.assertIn("module top ", first)

    def test_flatten_failure_stops_the_case(self) -> None:
        tally, fake, _ = self._run("break-flatten", 1)
        self.assertEqual(len(tally.failures), 1)
        self.assertIn("flatten: exit 1", tally.failures[0])
        self.assertEqual(len(fake.calls), 1)  # the sim leg did not run

    def test_unsimulable_design_is_skipped_not_passed(self) -> None:
        tally, _, _ = self._run("skip-sim")
        self.assertEqual((tally.sim_equal, tally.sim_skipped, tally.failures), (0, 3, []))

    def test_main_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch("builtins.print"):
            base = ["--cases", "1", "--odin3", "odin3", "--repro-dir", str(Path(tmp) / "r")]
            with mock.patch.object(fc, "run_capped", FakeOdin("good")):
                self.assertEqual(ff.main(base), fc.EXIT_SAME)
            with mock.patch.object(fc, "run_capped", FakeOdin("break-sim")):
                self.assertEqual(ff.main(base), fc.EXIT_DIFF)

    def test_yosys_leg_reports_a_difference(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch("builtins.print"):
            ns = args(1, Path(tmp) / "r")
            ns.yosys = "yosys"
            with (
                mock.patch.object(fc, "run_capped", FakeOdin("good")),
                mock.patch.object(fd, "diff_in", return_value=["wire a: odin3 1 yosys None"]),
            ):
                tally = ff.fuzz(ns)
        self.assertEqual(len(tally.failures), 1)
        self.assertIn("yosys: wire a", tally.failures[0])

    def test_unproven_equivalence_is_counted_not_failed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch("builtins.print"):
            ns = args(2, Path(tmp) / "r")
            ns.yosys = "yosys"
            with (
                mock.patch.object(fc, "run_capped", FakeOdin("good")),
                mock.patch.object(fd, "diff_in", return_value=["equiv: unproven (timeout)"]),
            ):
                tally = ff.fuzz(ns)
        self.assertEqual((tally.failures, tally.equiv_unproven), ([], 2))

    def test_equivalence_is_skipped_for_kept_hierarchy_and_tristate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch("builtins.print"):
            ns = args(60, Path(tmp) / "r")
            ns.yosys, ns.equiv = "yosys", True
            seen: list[bool] = []

            def fake_diff(*call: object, **_: object) -> list[str]:
                seen.append(bool(call[4]))
                return []

            with (
                mock.patch.object(fc, "run_capped", FakeOdin("good")),
                mock.patch.object(fd, "diff_in", side_effect=fake_diff),
            ):
                tally = ff.fuzz(ns)
        self.assertEqual(len(seen), 60)
        self.assertEqual(tally.equiv_skipped, seen.count(False))
        self.assertTrue(any(seen) and not all(seen))


if __name__ == "__main__":
    unittest.main()
