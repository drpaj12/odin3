"""Tests for tools/opt-check/bench_opt_report.py (2F spec §19, plan Task 19): the line parser, the
targets, the miss report, the elab-sweep log reader and the smoke check."""

from __future__ import annotations

import contextlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import bench_opt_report as bor
import bench_proc_report
import memcap

# A real bench_opt line (diffeq1.v, Release, 2026-10-13), the columns' ground truth.
DIFFEQ1 = (
    "bench_opt file=diffeq1.v cells0=25 nets0=841 pins0=1779 cellsp=51 netsp=1226 pinsp=3342 "
    "cells1=42 nets1=845 pins1=2196 iters=4 t_total=0.002 t_expr=0.001 t_merge=0.000 "
    "t_muxtree=0.000 t_reduce=0.000 t_share=0.000 t_dff=0.001 t_clean=0.000 t_setup=0.000 "
    "setup_rounds=11 rw_rewritten=2 t_compact=0.000 dead_nodes=47 dead_nets=783 dead_wires=0 "
    "full_iters=1 full_t_total=0.000 full_t_expr=0.000 full_t_merge=0.000 full_t_muxtree=0.000 "
    "full_t_reduce=0.000 full_t_share=0.000 full_t_dff=0.000 full_t_clean=0.000 "
    "full_t_setup=0.000 rss_proc_kib=4316 rss_proc_cur_kib=4316 rss_opt_kib=4316 "
    "rss_opt_growth_pct=0.0 rss_compact_kib=4320 rss_full_kib=4320 rss_peak_kib=4320 "
    "rss_src=hwm"
)


def row(**over: str) -> dict[str, str]:
    base = bor.parse_line(DIFFEQ1)
    base.update(over)
    return base


class ParseTest(unittest.TestCase):
    def test_real_line_has_every_column(self) -> None:
        self.assertEqual(bor.missing_columns(bor.parse_line(DIFFEQ1)), [])

    def test_a_missing_column_is_named(self) -> None:
        line = row()
        del line["full_t_total"]
        self.assertEqual(bor.missing_columns(line), ["full_t_total"])

    def test_columns_are_unique(self) -> None:
        self.assertEqual(len(set(bor.OPT_COLUMNS)), len(bor.OPT_COLUMNS))

    def test_a_word_that_is_not_key_value(self) -> None:
        with self.assertRaises(ValueError):
            bor.parse_line("bench_opt a=1 b")


class TargetsTest(unittest.TestCase):
    def verdicts(self, rows: dict[str, dict[str, str]],
                 expected: frozenset[str] = frozenset()) -> dict[str, str]:
        return {t.name: t.verdict for t in bor.opt_targets(rows, expected)}

    def test_empty_run_is_all_missed_unless_listed(self) -> None:
        self.assertEqual(set(self.verdicts({}).values()), {"MISSED"})
        listed = frozenset({"mcml.v", "koios/lenet.v"})
        verdicts = self.verdicts({}, listed)
        self.assertEqual(verdicts["lenet.v, Phase 2 opt passes"], "n/a")
        self.assertEqual(verdicts["mcml.v, opt -full"], "n/a")

    def test_mcml_limits(self) -> None:
        ok = self.verdicts({"mcml.v": row(t_total="0.999", full_t_total="0.299")})
        self.assertEqual(ok["mcml.v, Phase 2 opt passes"], "ok")
        self.assertEqual(ok["mcml.v, opt -full"], "ok")
        slow = self.verdicts({"mcml.v": row(t_total="1.001", full_t_total="0.301")})
        self.assertEqual(slow["mcml.v, Phase 2 opt passes"], "MISSED")
        self.assertEqual(slow["mcml.v, opt -full"], "MISSED")

    def test_lenet_time_and_rss_growth(self) -> None:
        ok = self.verdicts({"koios/lenet.v": row(t_total="14.9", rss_opt_growth_pct="24.9")})
        self.assertEqual(ok["lenet.v, Phase 2 opt passes"], "ok")
        self.assertEqual(ok["lenet.v, peak RSS growth over the post-proc IR"], "ok")
        bad = self.verdicts({"koios/lenet.v": row(t_total="15.1", rss_opt_growth_pct="25.1")})
        self.assertEqual(bad["lenet.v, Phase 2 opt passes"], "MISSED")
        self.assertEqual(bad["lenet.v, peak RSS growth over the post-proc IR"], "MISSED")

    def test_a_lower_bound_rss_never_passes(self) -> None:
        verdicts = self.verdicts({"koios/lenet.v": row(rss_opt_growth_pct="1", rss_src="cur")})
        self.assertEqual(verdicts["lenet.v, peak RSS growth over the post-proc IR"], "MISSED")

    def test_the_vtr_set_needs_every_file(self) -> None:
        name = f"the {bor.VTR_FILES} VTR files, total Phase 2 opt passes"
        vtr = {f"v{i}.v": row(t_total="0.05") for i in range(bor.VTR_FILES)}
        self.assertEqual(self.verdicts(vtr)[name], "ok")
        vtr["v0.v"] = row(t_total="2.0")
        self.assertEqual(self.verdicts(vtr)[name], "MISSED")
        del vtr["v0.v"]
        self.assertEqual(self.verdicts(vtr)[name], "MISSED")
        self.assertEqual(self.verdicts(vtr, frozenset({"v0.v"}))[name], "ok")


class ReportTest(unittest.TestCase):
    def test_a_miss_prints_times_and_memory(self) -> None:
        lines, missed = bor.report({"mcml.v": row(t_total="1.5", t_dff="0.7")})
        text = "\n".join(lines)
        self.assertTrue(missed)
        self.assertIn("times of mcml.v: ", text)
        self.assertIn("t_dff=0.7", text)
        self.assertIn("memory of mcml.v: ", text)
        self.assertIn("slowest file: mcml.v 1.500 s", text)

    def test_a_complete_run_passes(self) -> None:
        rows = {"mcml.v": row(t_total="0.8", full_t_total="0.1"),
                "koios/lenet.v": row(t_total="9", rss_opt_growth_pct="20")}
        rows |= {f"v{i}.v": row(t_total="0.05") for i in range(bor.VTR_FILES - 1)}
        lines, missed = bor.report(rows)
        self.assertFalse(missed, lines)
        self.assertNotIn("times of mcml.v", "\n".join(lines))
        self.assertIn("times of koios/lenet.v", "\n".join(lines))

    def test_a_short_row_fails(self) -> None:
        short = row()
        del short["rss_src"]
        lines, missed = bor.report({"mcml.v": short})
        self.assertTrue(missed)
        self.assertIn("missing columns: rss_src", "\n".join(lines))

    def test_main_reads_a_sweep_log_and_the_expect_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "log"
            log.write_text("ok    mcml.v (1 B, 0.1 s)\nbench: " + DIFFEQ1 + "\n", "utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(bor.main(["report", str(log)]), 1)
            self.assertIn("MISSED", out.getvalue())
            expect = Path(tmp) / "expect.txt"
            expect.write_text("# c\nkoios/lenet.v [n] why\n", "utf-8")
            self.assertEqual(bench_proc_report.expect_paths([str(expect)]), {"koios/lenet.v"})


class SweepLogTest(unittest.TestCase):
    def test_rows_follow_their_verdict(self) -> None:
        log = (
            "ok    a.v (10 B, 0.1 s)\n"
            "bench: bench_opt file=a.v t_total=1\n"
            "xfail koios/h.v (3 B, 0.0 s): no module\n"
            "ok    koios/lenet.v (11 B, 9.0 s)\n"
            "bench: bench_opt file=lenet.v t_total=2\n"
            "summary: 2 ok\n"
        )
        rows = bor.parse_sweep_log(log)
        self.assertEqual(set(rows), {"a.v", "koios/lenet.v"})
        self.assertEqual(rows["koios/lenet.v"]["t_total"], "2")


class SmokeTest(unittest.TestCase):
    def run_smoke(self, code: int, out: str) -> int:
        done = subprocess.CompletedProcess([], code, out, "boom")
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(memcap, "run", return_value=done))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            return bor.smoke("bench_opt", ["a.v"], None, 1 << 30)

    def test_a_complete_line_passes(self) -> None:
        self.assertEqual(self.run_smoke(0, DIFFEQ1 + "\n"), 0)

    def test_a_failed_child_fails(self) -> None:
        self.assertEqual(self.run_smoke(1, ""), 1)

    def test_a_short_line_fails(self) -> None:
        self.assertEqual(self.run_smoke(0, "bench_opt file=a.v t_total=1\n"), 1)


if __name__ == "__main__":
    unittest.main()
