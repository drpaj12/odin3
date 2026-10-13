"""Tests for tools/proc-check/bench_proc_report.py (2E spec §19, plan Task 13): the line parser,
the targets, the miss report and the elab-sweep log reader."""

from __future__ import annotations

import os
import subprocess
import unittest

import bench_proc_report as bpr


def row(**over: str) -> dict[str, str]:
    base = {name: "0" for name in bpr.PROC_COLUMNS}
    base.update(file="x.v", trio="full")
    base.update(over)
    return base


class ParseTest(unittest.TestCase):
    def test_parse_line(self) -> None:
        self.assertEqual(bpr.parse_line("a=1 b=x.v"), {"a": "1", "b": "x.v"})
        with self.assertRaises(ValueError):
            bpr.parse_line("a=1 b")

    def test_missing_columns(self) -> None:
        line = row()
        self.assertEqual(bpr.missing_columns(line), [])
        del line["f_ms"]
        self.assertEqual(bpr.missing_columns(line), ["f_ms"])

    def test_columns_are_unique(self) -> None:
        self.assertEqual(len(set(bpr.PROC_COLUMNS)), len(bpr.PROC_COLUMNS))


class TargetsTest(unittest.TestCase):
    def verdicts(self, rows: dict[str, dict[str, str]]) -> dict[str, str]:
        return {t.name: t.verdict for t in bpr.proc_targets(rows)}

    def test_empty_run_is_all_na(self) -> None:
        self.assertEqual(set(self.verdicts({}).values()), {"n/a"})

    def test_mcml_limit(self) -> None:
        self.assertEqual(self.verdicts({"mcml.v": row(noopt_ms="199.9")})["mcml.v proc --noopt"],
                         "ok")
        self.assertEqual(self.verdicts({"mcml.v": row(noopt_ms="200.1")})["mcml.v proc --noopt"],
                         "MISSED")

    def test_lenet_time_and_rss(self) -> None:
        ok = self.verdicts({"koios/lenet.v": row(noopt_ms="4999", rss_kib=str(2_900_000))})
        self.assertEqual(ok["lenet.v proc --noopt"], "ok")
        self.assertEqual(ok["lenet.v, through proc, peak RSS"], "ok")
        # 3.0 GB decimal: 2,929,688 KiB is 3.0000 x 10^9 B and 2,930,000 KiB is over.
        slow = self.verdicts({"koios/lenet.v": row(noopt_ms="5001", rss_kib="2930000")})
        self.assertEqual(slow["lenet.v proc --noopt"], "MISSED")
        self.assertEqual(slow["lenet.v, through proc, peak RSS"], "MISSED")

    def test_set_targets_need_every_file(self) -> None:
        vtr = {f"v{i}.v": row(noopt_ms="10") for i in range(bpr.VTR_FILES)}
        verdicts = self.verdicts(vtr)
        self.assertEqual(verdicts[f"the {bpr.VTR_FILES} VTR files, total proc --noopt"], "ok")
        vtr["v0.v"] = row(noopt_ms="500")
        self.assertEqual(self.verdicts(vtr)[f"the {bpr.VTR_FILES} VTR files, total proc --noopt"],
                         "MISSED")
        del vtr["v0.v"]
        self.assertEqual(self.verdicts(vtr)[f"the {bpr.VTR_FILES} VTR files, total proc --noopt"],
                         "n/a")

    def test_koios_set(self) -> None:
        count = bpr.KOIOS_FILES - bpr.KOIOS_NO_MODULE
        koios = {f"koios/k{i}.v": row(noopt_ms="100") for i in range(count)}
        name = next(t.name for t in bpr.proc_targets(koios) if t.name.startswith("the 33"))
        self.assertEqual(self.verdicts(koios)[name], "ok")
        koios["koios/k0.v"] = row(noopt_ms="5200")
        self.assertEqual(self.verdicts(koios)[name], "MISSED")


class ReportTest(unittest.TestCase):
    def test_miss_prints_terms(self) -> None:
        rows = {"mcml.v": row(noopt_ms="300", est_total_B="123", budget_ms="30")}
        lines, missed = bpr.report("proc", rows)
        self.assertTrue(missed)
        text = "\n".join(lines)
        self.assertIn("MISSED", text)
        self.assertIn("estimate terms of mcml.v: ", text)
        self.assertIn("est_total_B=123", text)
        self.assertIn("largest byte-budget share of noopt_ms: mcml.v 10.0%", text)

    def test_pass_has_no_terms_for_mcml(self) -> None:
        lines, missed = bpr.report("proc", {"mcml.v": row(noopt_ms="1")})
        self.assertFalse(missed)
        self.assertNotIn("estimate terms of mcml.v", "\n".join(lines))

    def test_unknown_profile(self) -> None:
        with self.assertRaises(ValueError):
            bpr.report("elab", {})


class SweepLogTest(unittest.TestCase):
    def test_rows_follow_their_verdict(self) -> None:
        log = (
            "ok    a.v (10 B, 0.1 s)\n"
            "bench: file=a.v noopt_ms=1\n"
            "xfail koios/h.v (3 B, 0.0 s): no module\n"
            "ok    koios/lenet.v (11 B, 9.0 s)\n"
            "bench: file=lenet.v noopt_ms=2\n"
            "summary: 2 ok\n"
        )
        rows = bpr.parse_sweep_log(log)
        self.assertEqual(set(rows), {"a.v", "koios/lenet.v"})
        self.assertEqual(rows["koios/lenet.v"]["noopt_ms"], "2")


class DebugGuardTest(unittest.TestCase):
    """A Debug or sanitized bench_proc refuses to run: its times are not the targets' (exit 2)."""

    def test_unoptimized_bench_refuses(self) -> None:
        exe = os.environ.get("ODIN3_BENCH_PROC")
        if not exe:
            self.skipTest("ODIN3_BENCH_PROC is not set (run through ctest)")
        done = subprocess.run([exe, "--one", "/dev/null"], capture_output=True, text=True,
                              check=False, timeout=60)
        if "needs a Release build" not in done.stderr:
            self.skipTest("this bench_proc is a Release build")
        self.assertEqual(done.returncode, 2)
        self.assertEqual(done.stdout, "")


if __name__ == "__main__":
    unittest.main()
