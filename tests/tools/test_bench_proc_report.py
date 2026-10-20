"""Tests for tools/proc-check/bench_proc_report.py (2E spec §19, plan Task 13): the line parser,
the targets, the miss report and the elab-sweep log reader."""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

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
    def verdicts(
        self, rows: dict[str, dict[str, str]], expected: frozenset[str] = frozenset()
    ) -> dict[str, str]:
        return {t.name: t.verdict for t in bpr.proc_targets(rows, expected)}

    def test_empty_run_is_all_missed(self) -> None:
        self.assertEqual(set(self.verdicts({}).values()), {"MISSED"})
        listed = frozenset({"mcml.v", "koios/lenet.v"})
        self.assertEqual(self.verdicts({}, listed)["lenet.v proc --noopt"], "n/a")

    def test_mcml_limit(self) -> None:
        self.assertEqual(
            self.verdicts({"mcml.v": row(noopt_ms="199.9")})["mcml.v proc --noopt"], "ok"
        )
        self.assertEqual(
            self.verdicts({"mcml.v": row(noopt_ms="200.1")})["mcml.v proc --noopt"], "MISSED"
        )

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
        self.assertEqual(
            self.verdicts(vtr)[f"the {bpr.VTR_FILES} VTR files, total proc --noopt"], "MISSED"
        )
        del vtr["v0.v"]
        self.assertEqual(
            self.verdicts(vtr)[f"the {bpr.VTR_FILES} VTR files, total proc --noopt"], "MISSED"
        )

    def test_koios_set(self) -> None:
        koios = {f"koios/k{i}.v": row(noopt_ms="100") for i in range(bpr.KOIOS_FILES - 3)}
        listed = frozenset(f"koios/x{i}.v" for i in range(3))
        name = next(t.name for t in bpr.proc_targets(koios, listed) if t.name.startswith("the 33"))
        self.assertEqual(self.verdicts(koios, listed)[name], "ok")
        # The same 30 rows with nothing listed is a short set, not a pass.
        self.assertEqual(self.verdicts(koios)[name], "MISSED")
        koios["koios/k0.v"] = row(noopt_ms="5200")
        self.assertEqual(self.verdicts(koios, listed)[name], "MISSED")


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
        rows = {"mcml.v": row(noopt_ms="1"), "koios/lenet.v": row(noopt_ms="1")}
        rows |= {f"v{i}.v": row(noopt_ms="1") for i in range(bpr.VTR_FILES - 1)}
        rows |= {f"koios/k{i}.v": row(noopt_ms="1") for i in range(bpr.KOIOS_FILES - 1)}
        lines, missed = bpr.report("proc", rows)
        self.assertFalse(missed)
        self.assertNotIn("estimate terms of mcml.v", "\n".join(lines))

    def test_partial_data_fails(self) -> None:
        _, missed = bpr.report("proc", {"mcml.v": row(noopt_ms="1")})
        self.assertTrue(missed)  # lenet.v and both sets are absent

    def test_short_row_fails(self) -> None:
        short = row(noopt_ms="1")
        del short["rss_kib"]
        lines, missed = bpr.report("proc", {"mcml.v": short})
        self.assertTrue(missed)
        self.assertIn("missing columns: rss_kib", "\n".join(lines))

    def test_main_exit_code_and_expect_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "log"
            log.write_text(
                "ok    mcml.v (1 B, 0.1 s)\nbench: "
                + " ".join(f"{k}={v}" for k, v in row(noopt_ms="1").items())
                + "\n",
                "utf-8",
            )
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bpr.main(["report", str(log)]), 1)
            expect = Path(tmp) / "expect.txt"
            expect.write_text("# c\nkoios/lenet.v [n] why\n", "utf-8")
            self.assertEqual(bpr.expect_paths([str(expect)]), {"koios/lenet.v"})

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
        done = subprocess.run(
            [exe, "--one", "/dev/null"], capture_output=True, text=True, check=False, timeout=60
        )
        if "needs a Release build" not in done.stderr:
            self.skipTest("this bench_proc is a Release build")
        self.assertEqual(done.returncode, 2)
        self.assertEqual(done.stdout, "")


if __name__ == "__main__":
    unittest.main()
