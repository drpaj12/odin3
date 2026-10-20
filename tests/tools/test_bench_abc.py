"""Tests for tests/bench/bench_abc.c (3D Task 22) and tools/abc-check/bench-abc-set.

The binary tests need a Release build: CTest's bench_abc_smoke sets ODIN3_BENCH_ABC to it (and
ODIN3_ABC to the pinned executable); without it they skip.  bench-abc-set is tested with a fake.
"""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SET_TOOL = REPO_ROOT / "tools" / "abc-check" / "bench-abc-set"
FF = REPO_ROOT / "tests" / "golden" / "blif" / "ff.parmys.blif"
COLUMNS = [
    "design",
    "module",
    "domain",
    "exchange",
    "cells",
    "ffs",
    "luts",
    "depth",
    "classify_ms",
    "aig_ms",
    "check_ms",
    "splice_ms",
    "prov_ms",
    "outside_ms",
    "abc_ms",
    "namemap_ms",
    "namemap_cov_pct",
    "edges",
    "rss_before_mb",
    "rss_peak_mb",
    "abc_nodes",
    "abc_latches",
    "abc_levels",
    "aig_objs",
    "bytes_per_aig_obj",
]
TIME_COLUMNS = COLUMNS[8:16]
BENCH = os.environ.get("ODIN3_BENCH_ABC")


def run(argv: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv, cwd=REPO_ROOT, env=env, capture_output=True, text=True, timeout=600, check=False
    )


def rows(out: str) -> list[dict[str, str]]:
    lines = [line.split("\t") for line in out.splitlines() if line]
    return [dict(zip(lines[0], line, strict=True)) for line in lines[1:]]


@unittest.skipUnless(BENCH, "needs ODIN3_BENCH_ABC (a Release bench_abc, CTest bench_abc_smoke)")
class BenchAbcTest(unittest.TestCase):
    def test_one_prints_every_column_and_the_times_are_numbers(self) -> None:
        proc = run([str(BENCH), "--one", str(FF), "--header"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        header = proc.stdout.splitlines()[0].split("\t")
        self.assertEqual(header, COLUMNS)
        found = rows(proc.stdout)
        self.assertEqual(len(found), 1)
        row = found[0]
        self.assertEqual(len(proc.stdout.splitlines()[1].split("\t")), len(COLUMNS))
        self.assertEqual((row["design"], row["module"]), ("ff", "dff"))
        for name in TIME_COLUMNS:
            self.assertGreaterEqual(float(row[name]), 0.0, name)
        self.assertGreater(float(row["rss_peak_mb"]), 0.0)
        self.assertEqual(row["luts"], "1")
        outside = sum(float(row[n]) for n in TIME_COLUMNS[:5])
        self.assertAlmostEqual(float(row["outside_ms"]), outside, delta=0.01)

    def test_the_exchange_follows_the_environment(self) -> None:
        env = dict(os.environ, ODIN3_ABC_EXCHANGE="aig")
        row = rows(run([str(BENCH), "--one", str(FF), "--header"], env).stdout)[0]
        self.assertEqual(row["exchange"], "aig")
        self.assertGreater(int(row["aig_objs"]), 0)
        self.assertNotEqual(row["bytes_per_aig_obj"], "-")
        self.assertNotEqual(row["namemap_cov_pct"], "-")

    def test_exe_adds_a_row_with_equal_counts(self) -> None:
        exe = os.environ.get("ODIN3_ABC")
        if not exe:
            self.skipTest("needs ODIN3_ABC")
        with tempfile.TemporaryDirectory() as tmp:
            proc = run([str(BENCH), "--one", str(FF), "--header", "--via-blif", tmp, "--exe", exe])
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertTrue((Path(tmp) / "abc0.exe.out.blif").is_file())
        linked, pinned = rows(proc.stdout)
        self.assertEqual((linked["exchange"], pinned["exchange"]), ("blif", "exe"))
        self.assertEqual(len(proc.stdout.splitlines()[2].split("\t")), len(COLUMNS))
        for column in ("abc_nodes", "abc_latches", "abc_levels"):
            self.assertEqual(linked[column], pinned[column], column)
        self.assertGreater(float(pinned["abc_ms"]), 0.0)
        if pinned["rss_peak_mb"] != "-":
            self.assertGreater(float(pinned["rss_peak_mb"]), 0.0)

    def test_a_childs_peak_is_not_inflated_by_the_parent(self) -> None:
        sleeper = "/bin/sleep"
        if not os.access(sleeper, os.X_OK):
            self.skipTest("needs /bin/sleep")
        proc = run([str(BENCH), "--selftest-rss", "200", "--exe", sleeper])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        fields = proc.stdout.split()
        self.assertEqual(
            (fields[0], fields[1], fields[3]), ("selftest_rss", "parent_mb", "child_peak_mb")
        )
        parent, child = float(fields[2]), float(fields[4])
        self.assertGreaterEqual(parent, 200.0)
        self.assertGreater(child, 0.0)
        self.assertLess(child, 50.0)  # ru_maxrss would read at least `parent`

    def test_the_peak_column_is_a_dash_or_a_number(self) -> None:
        row = rows(run([str(BENCH), "--one", str(FF), "--header"]).stdout)[0]
        peak = row["rss_peak_mb"]
        self.assertTrue(peak == "-" or float(peak) >= float(row["rss_before_mb"]), peak)

    def test_micro_prints_the_two_fixed_costs_as_numbers(self) -> None:
        proc = run([str(BENCH), "--micro"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        words = proc.stdout.strip().split("\t")
        self.assertEqual(
            [words[i] for i in (0, 1, 3, 5)],
            ["micro", "open_close_ms", "checker_ms", "checker_bytes"],
        )
        self.assertGreater(float(words[2]), 0.0)
        self.assertGreater(float(words[4]), 0.0)
        self.assertGreater(int(words[6]), 60000)  # a 64 KiB script

    def test_usage_errors_are_exit_two(self) -> None:
        for argv in (
            [],
            ["--one"],
            ["--one", str(FF), "--lut", "1"],
            ["--one", str(FF), "--exe", "/bin/true"],
            ["--bogus"],
            ["--micro", "--one", str(FF)],
        ):
            self.assertEqual(run([str(BENCH), *argv]).returncode, 2, argv)

    def test_a_missing_file_is_exit_one(self) -> None:
        proc = run([str(BENCH), "--one", "/nonexistent/x.blif"])
        self.assertEqual(proc.returncode, 1)


class BenchSetToolTest(unittest.TestCase):
    """bench-abc-set with a fake bench_abc that records its arguments."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.log = self.root / "calls"
        fake = self.root / "bench"
        fake.write_text(
            '#!/bin/sh\necho "$ODIN3_ABC_EXCHANGE|$*" >> "$FAKE_LOG"\n'
            '[ "${FAKE_FAIL:-}" != "$2" ] || exit 1\necho "ok $2"\n'
        )
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        self.bench = str(fake)
        self.env = dict(os.environ, FAKE_LOG=str(self.log))
        self.env.pop("ODIN3_ABC_EXCHANGE", None)

    def blif(self, name: str, size: int) -> str:
        path = self.root / name
        path.write_text("x" * size)
        return str(path)

    def calls(self) -> list[str]:
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_runs_smallest_first_with_one_header_and_a_directory_per_design(self) -> None:
        big, small = self.blif("big.parmys.blif", 50), self.blif("small.parmys.blif", 5)
        proc = run(
            [
                str(SET_TOOL),
                "--bench",
                self.bench,
                "--exe",
                "/x/abc",
                "--keep",
                str(self.root / "keep"),
                big,
                small,
            ],
            self.env,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        first, second = self.calls()
        self.assertIn(f"--one {small} --lut 6 --header --via-blif {self.root}/keep/small", first)
        self.assertIn("--exe /x/abc", first)
        self.assertIn(f"--one {big} --lut 6 --via-blif {self.root}/keep/big", second)
        self.assertNotIn("--header", second)

    def test_the_in_memory_exchange_sets_the_environment_and_no_exe(self) -> None:
        one = self.blif("a.blif", 1)
        proc = run([str(SET_TOOL), "--bench", self.bench, "--exchange", "aig", one], self.env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.calls(), [f"aig|--one {one} --lut 6 --header"])

    def test_a_failed_run_is_exit_one_and_the_others_still_run(self) -> None:
        bad, good = self.blif("bad.blif", 1), self.blif("good.blif", 2)
        env = dict(self.env, FAKE_FAIL=bad)
        proc = run([str(SET_TOOL), "--bench", self.bench, "--exchange", "aig", bad, good], env)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(len(self.calls()), 2)
        self.assertIn("failed", proc.stderr)

    def test_usage_errors_are_exit_two(self) -> None:
        one = self.blif("a.blif", 1)
        for argv in (
            [],
            ["--bench", self.bench],
            ["--bench", self.bench, "--exe", "/x", one],
            ["--bench", self.bench, "--exchange", "xyz", one],
        ):
            self.assertEqual(run([str(SET_TOOL), *argv], self.env).returncode, 2, argv)


if __name__ == "__main__":
    unittest.main()
