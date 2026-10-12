"""Tests for tools/vtr-qor/qor_parse.py (3H plan Task 3, T1).

The fixtures under tests/tools/fixtures/vtr_qor are real VTR output (logs trimmed to the lines
the parse configs match, plus the ``time -v`` lines); ``expected.json`` records what VTR's
``parse_vtr_flow.py`` printed for the untrimmed directory. Nothing here runs VTR.
"""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import tempfile
import unittest
from pathlib import Path

import qor_parse
from qor_heavy import HEAVY_DIR, HeavyError
from qor_parse import STAGE_GLOBS, StageCost, TimeV

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "vtr_qor"
V = Path("/v")
PY = V / ".venv" / "bin" / "python"
LIBS = "/v/vtr_flow/scripts/python_libs"
ARCH = V / "vtr_flow" / "arch" / "timing" / "k6_frac_N10_frac_chain_mem32K_40nm.xml"
TIME_V = (
    "\tCommand being timed: \"x\"\n"
    "\tElapsed (wall clock) time (h:mm:ss or m:ss): {elapsed}\n"
    "\tMaximum resident set size (kbytes): {kib}\n"
)


def run_fixtures() -> list[Path]:
    return sorted(p.parent for p in FIXTURES.glob("*/expected.json"))


def read_results(fixture: Path, name: str) -> dict[str, str]:
    return qor_parse.read_parse_results(fixture / name, not_before=0.0, label="P x")


class TempTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-parse-test-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def write(self, name: str, text: str) -> Path:
        path = self.tmp / name
        path.write_text(text, encoding="utf-8")
        return path

    def copy_fixture(self, name: str) -> Path:
        dst = self.tmp / name
        shutil.copytree(FIXTURES / name, dst)
        return dst


class FixtureTest(unittest.TestCase):
    def test_fixtures_exist(self) -> None:
        names = [p.name for p in run_fixtures()]
        self.assertIn("blink_P", names)
        self.assertTrue((FIXTURES / "frontend" / "frontend.time").is_file())

    def test_fixture_values(self) -> None:
        for fixture in run_fixtures():
            with self.subTest(fixture=fixture.name):
                expected = json.loads((fixture / "expected.json").read_text(encoding="utf-8"))
                parse = read_results(fixture, "parse_results.txt")
                qor = read_results(fixture, "qor_results.txt")
                # The committed rows are what VTR printed (untrimmed).
                for key, raw in expected["vtr_parse"].items():
                    self.assertEqual(parse.get(key, qor.get(key)), raw, key)
                self.assertEqual(qor_parse.metrics_from_parse(parse, qor), expected["metrics"])
                got = qor_parse.vpr_time_s(fixture)
                self.assertIsNotNone(got)
                self.assertAlmostEqual(got or 0.0, expected["vpr_time_s"], places=9)

    def test_fixture_stage_costs(self) -> None:
        for fixture in run_fixtures():
            with self.subTest(fixture=fixture.name):
                text = (fixture / "expected.json").read_text(encoding="utf-8")
                expected = json.loads(text)["stage"]
                cost = qor_parse.stage_costs(fixture, STAGE_GLOBS, "P x")
                self.assertAlmostEqual(cost.wall_s, expected["wall_s"], places=9)
                self.assertEqual(cost.peak_rss_mib, expected["peak_rss_mib"])
                self.assertEqual(cost.complete, expected["complete"])
                self.assertEqual(cost.warnings, ())

    def test_parse_rows_shape(self) -> None:
        parse = read_results(FIXTURES / "blink_P", "parse_results.txt")
        self.assertEqual(len(parse), 104)
        self.assertNotIn("", parse)
        self.assertEqual(parse["error"], "")
        self.assertEqual(parse["arch"], "EArch.xml")
        self.assertEqual(parse["hostname"], "CEC-JAMIESPA701")
        qor = read_results(FIXTURES / "blink_P", "qor_results.txt")
        self.assertEqual(list(qor), ["arch", "circuit", "script_params", "vpr_status",
                                     "total_wirelength", "total_runtime", "num_clb",
                                     "min_chan_width", "crit_path_delay"])

    def test_frontend_time(self) -> None:
        self.assertEqual(qor_parse.read_time_v(FIXTURES / "frontend" / "frontend.time"),
                         TimeV(0.0, 20))


class CommandTest(unittest.TestCase):
    def test_parse_commands(self) -> None:
        run_dir = Path("/w/j/run")
        cmds = qor_parse.parse_commands(V, PY, run_dir, ARCH, "sha.v")
        self.assertEqual(len(cmds), 2)
        common = [f"arch={ARCH.name}", "circuit=sha.v", "script_params=common"]
        script = f"{LIBS}/vtr/parse_vtr_flow.py"
        self.assertEqual(list(cmds[0].argv), [
            "/v/.venv/bin/python", script, "/w/j/run",
            "/v/vtr_flow/parse/parse_config/vpr_standard.txt", *common])
        self.assertEqual(list(cmds[1].argv), [
            "/v/.venv/bin/python", script, "/w/j/run",
            "/v/vtr_flow/parse/qor_config/qor_standard.txt", *common])
        self.assertEqual(cmds[0].stdout, run_dir / "parse_results.txt")
        self.assertEqual(cmds[1].stdout, run_dir / "qor_results.txt")
        for cmd in cmds:
            self.assertEqual(cmd.env, (("PYTHONPATH", LIBS),))
            self.assertEqual(cmd.cwd, run_dir)

    def test_arch_name_is_the_xml_file(self) -> None:
        cmds = qor_parse.parse_commands(V, PY, Path("/r"), Path("/a/b/EArch.xml"), "x.blif")
        self.assertIn("arch=EArch.xml", cmds[0].argv)
        self.assertIn("circuit=x.blif", cmds[0].argv)


class ReadParseResultsTest(TempTest):
    ROW = "arch\tcircuit\terror\tnum_clb\t\nEArch.xml\tsha.v\t\t7\t\n"

    def test_stale_parse(self) -> None:
        path = self.write("parse_results.txt", self.ROW)
        not_before = 1_000_000.0
        os.utime(path, (not_before - 1, not_before - 1))
        with self.assertRaises(HeavyError) as caught:
            qor_parse.read_parse_results(path, not_before=not_before, label="P sha")
        self.assertEqual(caught.exception.code, 1)
        self.assertEqual(caught.exception.message,
                         "P sha: stale parse_results.txt from an earlier attempt; job failed")

    def test_stale_names_the_file(self) -> None:
        path = self.write("qor_results.txt", self.ROW)
        os.utime(path, (10.0, 10.0))
        with self.assertRaises(HeavyError) as caught:
            qor_parse.read_parse_results(path, not_before=11.0, label="O sha")
        self.assertEqual(caught.exception.message,
                         "O sha: stale qor_results.txt from an earlier attempt; job failed")

    def test_fresh_and_boundary(self) -> None:
        path = self.write("parse_results.txt", self.ROW)
        os.utime(path, (500.0, 500.0))
        for not_before in (500.0, 499.0):
            got = qor_parse.read_parse_results(path, not_before=not_before, label="P sha")
            self.assertEqual(got, {"arch": "EArch.xml", "circuit": "sha.v", "error": "",
                                   "num_clb": "7"})

    def test_missing_file(self) -> None:
        with self.assertRaises(HeavyError) as caught:
            qor_parse.read_parse_results(self.tmp / "nope.txt", not_before=0.0, label="P sha")
        self.assertEqual(caught.exception.code, 1)
        self.assertEqual(caught.exception.message,
                         "P sha: cannot read nope.txt: No such file or directory")

    def test_malformed(self) -> None:
        for name, text in (
            ("empty", ""),
            ("header_only", "a\tb\t\n"),
            ("three_lines", "a\tb\t\n1\t2\t\n3\t4\t\n"),
            ("short_row", "a\tb\tc\t\n1\t2\t\n"),
            ("long_row", "a\tb\t\n1\t2\t3\t\n"),
            ("duplicate_name", "a\ta\t\n1\t2\t\n"),
        ):
            with self.subTest(case=name):
                path = self.write(f"{name}.txt", text)
                with self.assertRaises(HeavyError) as caught:
                    qor_parse.read_parse_results(path, not_before=0.0, label="P sha")
                self.assertEqual(caught.exception.code, 1)
                self.assertEqual(caught.exception.message,
                                 f"P sha: malformed {name}.txt; job failed")

    def test_trailing_blank_lines_and_crlf_tolerated(self) -> None:
        path = self.write("p.txt", "a\tb\t\r\n1\t\t\r\n\n\n")
        self.assertEqual(qor_parse.read_parse_results(path, not_before=0.0, label="x"),
                         {"a": "1", "b": ""})


class PresentTest(unittest.TestCase):
    def test_absent(self) -> None:
        for text in ("-1", "", "nan", "inf", None, "-1.0", " -1 ", "NaN", "-inf", "Infinity",
                     "abc", "1_000", "0x10", "1,5", "1.2.3", "--1", "1e999", " "):
            with self.subTest(text=text):
                self.assertIsNone(qor_parse.present(text))

    def test_numbers(self) -> None:
        for text, value in (("0", 0.0), ("1.934", 1.934), (" 2 ", 2.0), ("1e3", 1000.0),
                            ("-0.5", -0.5), ("+3", 3.0), (".5", 0.5), ("7.", 7.0),
                            ("-1.5", -1.5), ("-2", -2.0)):
            with self.subTest(text=text):
                self.assertEqual(qor_parse.present(text), value)

    def test_zero_is_a_value(self) -> None:
        got = qor_parse.present("0")
        self.assertIsNotNone(got)
        self.assertEqual(got, 0.0)


class TimeVTest(TempTest):
    def read(self, elapsed: str, kib: int = 70000) -> TimeV:
        path = self.write("t.time", TIME_V.format(elapsed=elapsed, kib=kib))
        return qor_parse.read_time_v(path)

    def test_time_v(self) -> None:
        self.assertEqual(self.read("0:14.23"), TimeV(14.23, 68))
        self.assertEqual(self.read("1:02:03.5").wall_s, 3723.5)
        self.assertEqual(self.read("1:02:03").wall_s, 3723.0)
        self.assertEqual(self.read("12:34.5").wall_s, 754.5)
        self.assertEqual(self.read("0:00.00").wall_s, 0.0)

    def test_rss_rounds_half_up(self) -> None:
        for kib, mib in ((70000, 68), (511, 0), (512, 1), (1023, 1), (1024, 1), (1535, 1),
                         (1536, 2), (0, 0)):
            with self.subTest(kib=kib):
                self.assertEqual(self.read("0:01.00", kib).peak_rss_mib, mib)

    def test_no_lines(self) -> None:
        path = self.write("e.time", "nothing here\nElapsed time: 5\n")
        self.assertEqual(qor_parse.read_time_v(path), TimeV(None, None))
        self.assertEqual(qor_parse.read_time_v(self.write("empty.time", "")), TimeV(None, None))
        self.assertEqual(qor_parse.read_time_v(self.tmp / "absent"), TimeV(None, None))

    def test_one_line_only(self) -> None:
        only_wall = self.write("w", "\tElapsed (wall clock) time (h:mm:ss or m:ss): 0:02.50\n")
        self.assertEqual(qor_parse.read_time_v(only_wall), TimeV(2.5, None))
        only_rss = self.write("r", "\tMaximum resident set size (kbytes): 2048\n")
        self.assertEqual(qor_parse.read_time_v(only_rss), TimeV(None, 2))

    def test_unparseable_elapsed_is_none(self) -> None:
        self.assertEqual(self.read("garbage"), TimeV(None, 68))

    def test_blocks_add_wall_and_take_peak(self) -> None:
        text = (TIME_V.format(elapsed="0:10.00", kib=2048)
                + "log text\n" + TIME_V.format(elapsed="0:05.50", kib=1024))
        self.assertEqual(qor_parse.read_time_v(self.write("m", text)), TimeV(15.5, 2))

    def test_lines_amid_program_output(self) -> None:
        text = "some output\n" + TIME_V.format(elapsed="2:00.00", kib=3072) + "\tExit status: 0\n"
        self.assertEqual(qor_parse.read_time_v(self.write("o", text)), TimeV(120.0, 3))


class VprTimeTest(TempTest):
    LINE = "The entire flow of VPR took {} seconds (max_rss 62.1 MiB)\n"

    def test_sum_of_both_logs(self) -> None:
        (self.tmp / "vpr.out").write_text("x\n" + self.LINE.format("10.5"), encoding="utf-8")
        (self.tmp / "vpr.crit_path.out").write_text(self.LINE.format("2.25"), encoding="utf-8")
        self.assertEqual(qor_parse.vpr_time_s(self.tmp), 12.75)

    def test_every_line_counts(self) -> None:
        (self.tmp / "vpr.out").write_text(self.LINE.format("1") + self.LINE.format("2"),
                                          encoding="utf-8")
        self.assertEqual(qor_parse.vpr_time_s(self.tmp), 3.0)

    def test_one_log(self) -> None:
        (self.tmp / "vpr.crit_path.out").write_text(self.LINE.format("4"), encoding="utf-8")
        self.assertEqual(qor_parse.vpr_time_s(self.tmp), 4.0)

    def test_other_logs_ignored(self) -> None:
        (self.tmp / "vpr.out").write_text(self.LINE.format("1"), encoding="utf-8")
        (self.tmp / "parmys.out").write_text(self.LINE.format("100"), encoding="utf-8")
        self.assertEqual(qor_parse.vpr_time_s(self.tmp), 1.0)

    def test_none_when_absent(self) -> None:
        self.assertIsNone(qor_parse.vpr_time_s(self.tmp))
        (self.tmp / "vpr.out").write_text("no such line\n", encoding="utf-8")
        self.assertIsNone(qor_parse.vpr_time_s(self.tmp))

    def test_none_when_a_log_cannot_be_read(self) -> None:
        (self.tmp / "vpr.out").write_text(self.LINE.format("1"), encoding="utf-8")
        (self.tmp / "vpr.crit_path.out").mkdir()
        self.assertIsNone(qor_parse.vpr_time_s(self.tmp))

    def test_other_wording_is_not_a_match(self) -> None:
        (self.tmp / "vpr.out").write_text("The entire flow of VPR took 5 secs\n", encoding="utf-8")
        self.assertIsNone(qor_parse.vpr_time_s(self.tmp))

    def test_none_when_unreadable_number(self) -> None:
        (self.tmp / "vpr.out").write_text(self.LINE.format("1"), encoding="utf-8")
        (self.tmp / "vpr.crit_path.out").write_text(self.LINE.format("soon"), encoding="utf-8")
        self.assertIsNone(qor_parse.vpr_time_s(self.tmp))


class StageCostTest(TempTest):
    def two_clock(self) -> Path:
        """A real single-clock run with a second ABC domain: ``abc1.out`` is ``abc0.out`` with its
        two ``time -v`` figures edited, since no real two-clock log is committed yet."""
        run = self.copy_fixture("blink_P")
        text = (run / "abc0.out").read_text(encoding="utf-8")
        text = re.sub(r"(Elapsed [^\n]*): [^\n]*", r"\1: 0:01.50", text)
        text = re.sub(r"(Maximum resident set size \(kbytes\)): \d+", r"\1: 50000", text)
        (run / "abc1.out").write_text(text, encoding="utf-8")
        return run

    def test_stage_costs_two_clock(self) -> None:
        run = self.two_clock()
        cost = qor_parse.stage_costs(run, STAGE_GLOBS, "P stereovision3")
        # parmys 0.04 + abc0 0.03 + abc1 1.50 + three helpers 0.00.
        self.assertAlmostEqual(cost.wall_s, 1.57, places=9)
        self.assertEqual(cost.peak_rss_mib, 49)
        self.assertTrue(cost.complete)
        self.assertEqual(cost.warnings, ())
        text = (run / "abc1.out").read_text(encoding="utf-8")
        stripped = "".join(line for line in text.splitlines(keepends=True)
                           if "Elapsed" not in line and "Maximum resident" not in line)
        (run / "abc1.out").write_text(stripped, encoding="utf-8")
        cost = qor_parse.stage_costs(run, STAGE_GLOBS, "P stereovision3")
        self.assertFalse(cost.complete)
        self.assertEqual(cost.warnings, (
            "vtr-qor: P stereovision3: no time -v in abc1.out; stage cost incomplete",))
        # The log that has no lines adds nothing; the others still count.
        self.assertAlmostEqual(cost.wall_s, 0.07, places=9)
        self.assertEqual(cost.peak_rss_mib, 34)

    def test_every_incomplete_log_is_named(self) -> None:
        run = self.copy_fixture("blink_P")
        (run / "parmys.out").write_text("none\n", encoding="utf-8")
        (run / "restore_latch0.out").write_text("none\n", encoding="utf-8")
        cost = qor_parse.stage_costs(run, STAGE_GLOBS, "O' x")
        self.assertFalse(cost.complete)
        self.assertEqual(cost.warnings, (
            "vtr-qor: O' x: no time -v in parmys.out; stage cost incomplete",
            "vtr-qor: O' x: no time -v in restore_latch0.out; stage cost incomplete"))

    def test_half_a_block_is_incomplete(self) -> None:
        run = self.copy_fixture("blink_P")
        (run / "abc0.out").write_text(
            "\tElapsed (wall clock) time (h:mm:ss or m:ss): 0:03.00\n", encoding="utf-8")
        cost = qor_parse.stage_costs(run, ("abc*.out",), "P x")
        self.assertFalse(cost.complete)
        self.assertEqual(len(cost.warnings), 1)

    def test_only_rss_line_is_incomplete(self) -> None:
        run = self.copy_fixture("blink_P")
        (run / "abc0.out").write_text(
            "\tMaximum resident set size (kbytes): 2048\n", encoding="utf-8")
        cost = qor_parse.stage_costs(run, ("abc*.out",), "P x")
        self.assertFalse(cost.complete)
        self.assertEqual(cost.warnings, (
            "vtr-qor: P x: no time -v in abc0.out; stage cost incomplete",))
        self.assertEqual(cost.peak_rss_mib, 2)

    def test_directories_are_not_logs(self) -> None:
        run = self.copy_fixture("blink_P")
        (run / "abc9.out").mkdir()
        cost = qor_parse.stage_costs(run, STAGE_GLOBS, "P x")
        self.assertTrue(cost.complete)
        self.assertEqual(cost.warnings, ())

    def test_only_matched_logs_and_each_once(self) -> None:
        run = self.copy_fixture("blink_P")
        # vpr.out has a time -v block too but is not a stage log.
        once = qor_parse.stage_costs(run, ("parmys.out",), "P x")
        twice = qor_parse.stage_costs(run, ("parmys.out", "par*.out"), "P x")
        self.assertEqual(once, twice)
        self.assertAlmostEqual(once.wall_s, 0.04, places=9)
        self.assertEqual(once.peak_rss_mib, 26)
        (run / "vpr.out").write_text(TIME_V.format(elapsed="9:00.00", kib=9_000_000),
                                     encoding="utf-8")
        self.assertEqual(qor_parse.stage_costs(run, ("parmys.out",), "P x"), once)

    def test_no_logs_is_incomplete(self) -> None:
        cost = qor_parse.stage_costs(self.tmp, STAGE_GLOBS, "P x")
        self.assertEqual(cost, StageCost(0.0, 0, False, (
            f"vtr-qor: P x: no stage logs in {self.tmp}; stage cost incomplete",)))

    def test_stage_globs(self) -> None:
        self.assertEqual(STAGE_GLOBS, ("parmys.out", "abc*.out", "*_blackboxing_latch.out",
                                       "restore_latch*.out", "report_clocks.abc.out"))

    def test_wall_sums_exactly_the_matched_files(self) -> None:
        for name, wall in (("parmys.out", "0:01.00"), ("abc0.out", "0:02.00"),
                           ("abc10.out", "0:04.00"), ("0_blackboxing_latch.out", "0:08.00"),
                           ("restore_latch0.out", "0:16.00"), ("report_clocks.abc.out", "0:32.00"),
                           ("vpr.out", "1:04.00")):
            (self.tmp / name).write_text(TIME_V.format(elapsed=wall, kib=1024), encoding="utf-8")
        cost = qor_parse.stage_costs(self.tmp, STAGE_GLOBS, "P x")
        self.assertEqual(cost, StageCost(63.0, 1, True, ()))


class MetricsTest(unittest.TestCase):
    PARSE = {
        "num_clb": "104", "num_memories": "2", "num_mult": "3", "device_width": "12",
        "device_height": "11", "vpr_status": "success", "min_chan_width": "34",
        "critical_path_delay": "2.5", "crit_path_routed_wirelength": "12345",
        "max_vpr_mem": "63568",
    }
    QOR = {"total_runtime": "12.75"}

    def test_full(self) -> None:
        self.assertEqual(qor_parse.metrics_from_parse(self.PARSE, self.QOR), {
            "packed": {"num_clb": 104, "num_memories": 2, "num_mult": 3, "device_width": 12,
                       "device_height": 11},
            "routed": {"vpr_status": "success", "min_chan_width": 34,
                       "critical_path_delay_ns": 2.5, "fmax_mhz": 400.0,
                       "crit_path_routed_wirelength": 12345},
            "cost": {"max_vpr_mem_mib": 62, "vpr_total_runtime_s": 12.75},
        })

    def test_types(self) -> None:
        got = qor_parse.metrics_from_parse(self.PARSE, self.QOR)
        for key in ("num_clb", "device_width"):
            self.assertIs(type(got["packed"][key]), int)
        self.assertIs(type(got["routed"]["min_chan_width"]), int)
        self.assertIs(type(got["cost"]["max_vpr_mem_mib"]), int)
        self.assertIs(type(got["routed"]["fmax_mhz"]), float)

    def test_fmax(self) -> None:
        parse = dict(self.PARSE, critical_path_delay="1.934")
        fmax = qor_parse.metrics_from_parse(parse, self.QOR)["routed"]["fmax_mhz"]
        self.assertIsInstance(fmax, float)
        self.assertTrue(math.isclose(float(fmax or 0.0), 1000 / 1.934, rel_tol=1e-12))
        for delay in ("0", "-2.5", "-1", "", "nan", "inf"):
            with self.subTest(delay=delay):
                got = qor_parse.metrics_from_parse(dict(self.PARSE, critical_path_delay=delay),
                                                   self.QOR)
                self.assertIsNone(got["routed"]["fmax_mhz"])

    def test_mem_rounds_half_up(self) -> None:
        for kib, mib in (("0", 0), ("511", 0), ("512", 1), ("1023", 1), ("1024", 1),
                         ("1535", 1), ("1536", 2), ("63568", 62), ("6291456", 6144)):
            with self.subTest(kib=kib):
                got = qor_parse.metrics_from_parse(dict(self.PARSE, max_vpr_mem=kib), self.QOR)
                self.assertEqual(got["cost"]["max_vpr_mem_mib"], mib)

    def test_missing_key(self) -> None:
        parse = dict(self.PARSE)
        del parse["min_chan_width"]
        got = qor_parse.metrics_from_parse(parse, self.QOR)
        self.assertIsNone(got["routed"]["min_chan_width"])
        self.assertEqual(got["packed"]["num_clb"], 104)

    def test_missing_key_in_fixture_copy(self) -> None:
        parse = read_results(FIXTURES / "blink_P", "parse_results.txt")
        parse["min_chan_width"] = "34"
        qor = read_results(FIXTURES / "blink_P", "qor_results.txt")
        self.assertEqual(qor_parse.metrics_from_parse(parse, qor)["routed"]["min_chan_width"], 34)
        del parse["min_chan_width"]
        self.assertIsNone(qor_parse.metrics_from_parse(parse, qor)["routed"]["min_chan_width"])

    def test_nothing_reads_minus_one(self) -> None:
        keys = list(self.PARSE) + ["total_runtime"]
        minus = {k: "-1" for k in keys}
        blank = {k: "" for k in keys}
        nan = {k: "nan" for k in keys if k != "vpr_status"}
        inf = {"total_runtime": "inf"}
        for parse, qor in (({}, {}), (minus, minus), (blank, blank), (nan, inf)):
            got = qor_parse.metrics_from_parse(parse, qor)
            for group in got.values():
                for key, value in group.items():
                    self.assertIsNone(value, key)

    def test_each_key_independent(self) -> None:
        for key in self.PARSE:
            with self.subTest(key=key):
                parse = dict(self.PARSE)
                del parse[key]
                got = qor_parse.metrics_from_parse(parse, self.QOR)
                full = qor_parse.metrics_from_parse(self.PARSE, self.QOR)
                changed = [(g, k) for g in got for k in got[g] if got[g][k] != full[g][k]]
                expected = {
                    "critical_path_delay": [("routed", "critical_path_delay_ns"),
                                            ("routed", "fmax_mhz")],
                    "max_vpr_mem": [("cost", "max_vpr_mem_mib")],
                    "num_clb": [("packed", "num_clb")],
                    "num_memories": [("packed", "num_memories")],
                    "num_mult": [("packed", "num_mult")],
                    "device_width": [("packed", "device_width")],
                    "device_height": [("packed", "device_height")],
                    "vpr_status": [("routed", "vpr_status")],
                    "min_chan_width": [("routed", "min_chan_width")],
                    "crit_path_routed_wirelength": [("routed", "crit_path_routed_wirelength")],
                }[key]
                self.assertEqual(sorted(changed), sorted(expected))
                self.assertTrue(all(got[g][k] is None for g, k in changed))

    def test_total_runtime_comes_from_qor(self) -> None:
        got = qor_parse.metrics_from_parse(self.PARSE, {})
        self.assertIsNone(got["cost"]["vpr_total_runtime_s"])
        got = qor_parse.metrics_from_parse(dict(self.PARSE, total_runtime="99"), {})
        self.assertIsNone(got["cost"]["vpr_total_runtime_s"])

    def test_vpr_status_is_text(self) -> None:
        for status in ("success", "failed_place", "unknown_error"):
            got = qor_parse.metrics_from_parse(dict(self.PARSE, vpr_status=status), self.QOR)
            self.assertEqual(got["routed"]["vpr_status"], status)

    def test_fractional_counts_are_unreadable(self) -> None:
        got = qor_parse.metrics_from_parse(dict(self.PARSE, num_clb="3.5"), self.QOR)
        self.assertIsNone(got["packed"]["num_clb"])


class SourceTest(unittest.TestCase):
    def test_no_environment_hooks(self) -> None:
        source = Path(qor_parse.__file__).read_text(encoding="utf-8")
        self.assertNotIn("environ", source)
        self.assertNotIn("getenv", source)


if __name__ == "__main__":
    unittest.main()
