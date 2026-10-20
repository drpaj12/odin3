"""Tests for tools/elab-check/bench_report.py (2D Task 27; spec §15): the bench_elab line parser,
the §15 targets and the smoke check, over fake bench scripts; plus elab-sweep's --bench-report."""

from __future__ import annotations

import contextlib
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import bench_report
import memcap

from . import helpers

FULL = " ".join(f"{name}=1" for name in bench_report.ELAB_COLUMNS)


def row(**over: object) -> dict[str, str]:
    """A full bench_elab line as a row, with some columns replaced."""
    parsed = bench_report.parse_line(FULL)
    parsed.update({k: str(v) for k, v in over.items()})
    return parsed


def rows_31(per_file_ms: float) -> dict[str, dict[str, str]]:
    out = {f"f{i}.v": row(elab_ms=per_file_ms) for i in range(31)}
    out["mcml.v"] = row(elab_ms=per_file_ms)
    del out["f0.v"]  # mcml.v is one of the 31
    return out


class ParseTest(unittest.TestCase):
    def test_parse_line_splits_key_value_columns(self) -> None:
        parsed = bench_report.parse_line("file=a.v bytes=12 elab_ms=1.5\n")
        self.assertEqual(parsed, {"file": "a.v", "bytes": "12", "elab_ms": "1.5"})

    def test_parse_line_rejects_a_word_without_equals(self) -> None:
        with self.assertRaises(ValueError):
            bench_report.parse_line("file=a.v oops")

    def test_missing_columns_names_each_absent_column(self) -> None:
        parsed = bench_report.parse_line("file=a.v bytes=1")
        missing = bench_report.missing_columns(parsed)
        self.assertIn("est_total_B", missing)
        self.assertIn("rss_kib", missing)
        self.assertNotIn("bytes", missing)
        self.assertEqual(bench_report.missing_columns(row()), [])

    def test_the_columns_are_the_brief_s_list(self) -> None:
        names = set(bench_report.ELAB_COLUMNS)
        for needed in (
            "bytes",
            "parse_ms",
            "stage1_ms",
            "bind_ms",
            "stage2_ms",
            "parsed_nodes",
            "parsed_bytes",
            "elab_nodes",
            "elab_bytes",
            "modules",
            "specs",
            "cells",
            "procs",
            "mems",
            "nets",
            "wires",
            "pins",
            "est_pins_B",
            "est_nets_B",
            "est_nodes_B",
            "est_wires_B",
            "est_bodies_B",
            "est_elab_B",
            "est_total_B",
            "rss_kib",
        ):
            self.assertIn(needed, names)


class TargetTest(unittest.TestCase):
    def verdicts(self, rows: dict[str, dict[str, str]]) -> dict[str, str]:
        return {t.name: t.verdict for t in bench_report.elab_targets(rows)}

    def test_mcml_within_and_over(self) -> None:
        ok = self.verdicts({"mcml.v": row(elab_ms=499.9, rss_kib=195000)})
        self.assertEqual(ok["mcml.v stage 1 + stage 2"], "ok")
        self.assertEqual(ok["mcml.v peak RSS"], "ok")
        bad = self.verdicts({"mcml.v": row(elab_ms=500.1, rss_kib=200000)})
        self.assertEqual(bad["mcml.v stage 1 + stage 2"], "MISSED")
        self.assertEqual(bad["mcml.v peak RSS"], "MISSED")  # 200000 KiB > 200 MB (10^6 B)

    def test_lenet_within_and_over(self) -> None:
        ok = self.verdicts({"koios/lenet.v": row(elab_ms=9999, rss_kib=1900000)})
        self.assertEqual(ok["lenet.v stage 1 + stage 2"], "ok")
        self.assertEqual(ok["lenet.v peak RSS"], "ok")
        bad = self.verdicts({"koios/lenet.v": row(elab_ms=10001, rss_kib=2000000)})
        self.assertEqual(bad["lenet.v stage 1 + stage 2"], "MISSED")
        self.assertEqual(bad["lenet.v peak RSS"], "MISSED")  # 2,000,000 KiB > 2.0 GB (10^9 B)

    def test_absent_files_are_not_applicable_not_missed(self) -> None:
        got = self.verdicts({})
        self.assertEqual(set(got.values()), {"n/a"})

    def test_the_31_files_sum(self) -> None:
        under = self.verdicts(rows_31(90.0))  # 31 x 90 ms = 2.79 s
        self.assertEqual(under["31 verilog/*.v files, total elaboration"], "ok")
        over = self.verdicts(rows_31(100.0))  # 3.1 s
        self.assertEqual(over["31 verilog/*.v files, total elaboration"], "MISSED")

    def test_koios_files_do_not_count_among_the_31(self) -> None:
        rows = rows_31(1.0)
        rows["koios/big.v"] = row(elab_ms=10**6)
        got = self.verdicts(rows)
        self.assertEqual(got["31 verilog/*.v files, total elaboration"], "ok")

    def test_a_partial_set_of_the_31_is_not_applicable(self) -> None:
        rows = rows_31(1.0)
        del rows["f1.v"]
        got = self.verdicts(rows)
        self.assertEqual(got["31 verilog/*.v files, total elaboration"], "n/a")

    def test_report_lists_estimate_terms_and_flags_a_miss(self) -> None:
        rows = {"mcml.v": row(elab_ms=900, rss_kib=1, est_pins_B=7, est_total_B=99)}
        lines, missed = bench_report.report("elab", rows)
        self.assertTrue(missed)
        text = "\n".join(lines)
        self.assertIn("MISSED", text)
        self.assertIn("est_pins_B=7", text)  # a miss is explained with the estimate terms
        self.assertIn("est_total_B=99", text)
        _, missed_none = bench_report.report("elab", {"mcml.v": row(elab_ms=1, rss_kib=1)})
        self.assertFalse(missed_none)

    def test_unknown_profile(self) -> None:
        with self.assertRaises(ValueError):
            bench_report.report("nope", {})

    def test_proc_profile_is_registered_and_is_2es_report(self) -> None:
        # 2E's review: bench_proc_report's profile belongs in PROFILES, so that
        # `elab-sweep --bench bench_proc --bench-report proc` reads the same rows.
        self.assertIn("proc", bench_report.PROFILES)
        lines, missed = bench_report.report("proc", {})
        self.assertTrue(missed)  # a run with no mcml.v row never passes
        self.assertEqual(lines[0], "bench targets (spec 19):")
        names = [t.name for t in bench_report.proc_targets({})]
        self.assertTrue(any("mcml.v" in n for n in names), names)
        # the expect-fail paths reach it: a listed file is not MISSED for being absent
        _, missed_listed = bench_report.report("proc", {}, frozenset({"mcml.v"}))
        self.assertTrue(missed_listed)  # lenet.v and the sets are still absent


FAKE_OK = """#!{python}
import sys
print({line!r})
"""


class SmokeTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        meminfo = self.dir / "meminfo"
        meminfo.write_text("MemTotal:       33554432 kB\n", encoding="ascii")
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(meminfo)))
        stack.enter_context(mock.patch.object(memcap, "SYSTEMD_RUN", "no-such-systemd-run"))
        self.addCleanup(stack.close)

    def fake(self, line: str) -> Path:
        path = self.dir / "bench_elab"
        path.write_text(
            FAKE_OK.replace("{python}", sys.executable).replace("{line!r}", repr(line)),
            encoding="utf-8",
        )
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def test_smoke_passes_when_every_column_is_present(self) -> None:
        bench = self.fake(FULL)
        src = self.dir / "a.v"
        src.write_text("module m; endmodule\n", encoding="utf-8")
        code, out, err = helpers.run_main(
            bench_report.main, ["smoke", "--bench", str(bench), str(src), str(src)]
        )
        self.assertEqual(code, 0, err)
        self.assertIn("2 files", out)

    def test_smoke_fails_naming_the_missing_column(self) -> None:
        bench = self.fake(FULL.replace("est_total_B=1", "est_total=1"))
        src = self.dir / "a.v"
        src.write_text("module m; endmodule\n", encoding="utf-8")
        code, _, err = helpers.run_main(
            bench_report.main, ["smoke", "--bench", str(bench), str(src)]
        )
        self.assertEqual(code, 1)
        self.assertIn("est_total_B", err)

    def test_smoke_passes_the_techlib_through(self) -> None:
        bench = self.dir / "bench_elab"
        bench.write_text(
            f"#!{sys.executable}\nimport sys\n"
            "assert sys.argv[1] == '--one' and sys.argv[3:] == ['--techlib', 'T.o3lib'], sys.argv\n"
            f"print({FULL!r})\n",
            encoding="utf-8",
        )
        bench.chmod(bench.stat().st_mode | stat.S_IXUSR)
        src = self.dir / "a.v"
        src.write_text("x", encoding="utf-8")
        code, _, err = helpers.run_main(
            bench_report.main, ["smoke", "--bench", str(bench), "--techlib", "T.o3lib", str(src)]
        )
        self.assertEqual(code, 0, err)

    def test_smoke_fails_when_the_bench_fails(self) -> None:
        bench = self.dir / "bench_elab"
        bench.write_text(f"#!{sys.executable}\nimport sys\nsys.exit(1)\n", encoding="utf-8")
        bench.chmod(bench.stat().st_mode | stat.S_IXUSR)
        src = self.dir / "a.v"
        src.write_text("x", encoding="utf-8")
        code, _, err = helpers.run_main(
            bench_report.main, ["smoke", "--bench", str(bench), str(src)]
        )
        self.assertEqual(code, 1)
        self.assertIn("exit 1", err)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONHASHSEED", "0")
    unittest.main()
