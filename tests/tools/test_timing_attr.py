"""Tests for tools/flow/timing_report.py and timing_attr.py (v1.1.1, docs/V1.1.1.md): the VPR
setup-report parser, the BLIF join, the delay model, the aggregation, the outputs and the command
line, on small committed fixtures (a VPR report plus the matching Odin III files of two micros), so
no VPR is needed. One integration test runs the real flow and skips without VTR_ROOT and
ODIN3_CLI."""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import odin3_flow as flow
import qor_legs
import timing_attr as ta
import timing_report as tr

from .helpers import run_main, scratch

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures" / "timing_attr"
FIXTURES_V2 = HERE / "fixtures" / "timing_attr_v2"
GOLDEN_V1 = HERE / "data" / "timing_attr_v1"
ROOT = HERE.parents[1]
ODIN3 = os.environ.get("ODIN3_CLI", "")
VTR_ROOT = os.environ.get("VTR_ROOT", "")
ADDER, DFF = "bm_DL_BCD_adder", "bm_DL_logic_w_Dff"
MICRO_DFF = ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_DL_logic_w_Dff.v"
EPS = 1e-6


VPR_PYTHON = Path(VTR_ROOT) / ".venv" / "bin" / "python" if VTR_ROOT else Path()


def path_text(index: int, start: str = "g.Q[0] (.latch clocked by Clock)",
              end: str = "out:g.outpad[0] (.output clocked by Clock)", kind: str = "setup",
              rows: bool = True, slack: str | None = "slack (VIOLATED)  -1.451",
              launch_clock: str = "Clock") -> list[str]:
    """One path section in the layout of tatum's TimingReporter::report_timing_path
    (libs/EXTERNAL/libtatum/libtatum/tatum/TimingReporter.cpp, read in VTR 9.x)."""
    lines = [f"#Path {index}", f"Startpoint: {start}", f"Endpoint  : {end}",
             f"Path Type : {kind}", "", "Point      Incr      Path", "-----",
             f"clock {launch_clock} (rise edge)   0.000     0.000",
             "clock source latency   0.000     0.000"]
    if rows:
        lines += ["g.clk[0] (.latch)   0.042     0.042",
                  "g.Q[0] (.latch) [clock-to-output]   0.124     0.166",
                  "out:g.outpad[0] (.output)   1.285     1.451"]
    lines += ["data arrival time    1.451", "", "clock Clock (rise edge)   0.000     0.000",
              "data required time    0.000", "-----", "data required time    0.000",
              "data arrival time    -1.451", "-----"]
    if slack:
        lines.append(slack)
    return lines + [""]


def copy_fixture(case: unittest.TestCase, name: str) -> Path:
    """A writable copy of fixture ``name`` (a flow output directory)."""
    dest = scratch(case, "ot-tattr-") / name
    shutil.copytree(FIXTURES / name, dest)
    return dest


def copy_fixture_v2(case: unittest.TestCase, name: str) -> Path:
    """A writable copy of version 2 fixture ``name``."""
    dest = scratch(case, "ot-tattr2-") / name
    shutil.copytree(FIXTURES_V2 / name, dest)
    return dest


class ReportParser(unittest.TestCase):
    def test_adder_report(self) -> None:
        paths = tr.parse_report(FIXTURES / ADDER / "vpr" / "run" / "report_timing.setup.rpt")
        self.assertEqual(len(paths), 10)
        first = paths[0]
        self.assertEqual((first.index, first.path_type, first.slack_state), (1, "setup",
                                                                              "VIOLATED"))
        self.assertAlmostEqual(first.slack_ns or 0, -4.619, places=3)
        self.assertAlmostEqual(first.arrival_ns or 0, 4.619, places=3)
        self.assertEqual(first.startpoint, "X[0].inpad[0] (.input clocked by virtual_io_clock)")
        # a combinational path starts at the input pad after the input delay row
        self.assertEqual((first.rows[0].atom, first.rows[0].block_type), ("X[0]", ".input"))
        self.assertEqual(first.rows[-1].atom, "out:Cout")
        self.assertEqual(first.rows[-1].port, "outpad")

    def test_flop_path_starts_at_clock_to_output(self) -> None:
        paths = tr.parse_report(FIXTURES / DFF / "vpr" / "run" / "report_timing.setup.rpt")
        first = paths[0]
        self.assertEqual([r.atom for r in first.rows], ["g", "out:g"])
        self.assertEqual(first.rows[0].note, "clock-to-output")
        self.assertAlmostEqual(first.rows[0].incr, 0.124)  # the clock pin row is skipped
        self.assertAlmostEqual(first.rows[1].incr, 1.285)

    def test_final_cpd(self) -> None:
        text = (FIXTURES / DFF / "vpr" / "run" / "vpr.crit_path.out").read_text()
        self.assertEqual(tr.final_cpd([text]), (1.45102, 689.17))
        self.assertIsNone(tr.final_cpd(["nothing here"]))
        self.assertEqual(tr.final_cpd(["no", "Final critical path delay (least slack): 3 ns"]),
                         (3.0, None))

    def test_missing_and_foreign_files(self) -> None:
        tmp = scratch(self, "ot-trep-")
        with self.assertRaises(tr.ReportError):
            tr.parse_report(tmp / "none.rpt")
        (tmp / "x.rpt").write_text("hello\n")
        with self.assertRaises(tr.ReportError):
            tr.parse_report(tmp / "x.rpt")

    def test_atom_names_with_dots_and_brackets(self) -> None:
        lines = [
            "#Path 1", "Startpoint: a", "Endpoint  : b", "Path Type : setup", "",
            "Point                  Incr      Path", "-----",
            "input external delay   0.000     0.000",
            "u1.x[3].inpad[0] (.input)   0.000     0.000",
            "u1.y.in[2] (.names)         0.500     0.500",
            "u1.y.out[0] (.names)        0.200     0.700",
            "data arrival time                     0.700", "",
            "data required time                    0.000",
            "slack (VIOLATED)                     -0.700",
        ]
        rows = tr.parse_lines(lines)[0].rows
        self.assertEqual([(r.atom, r.port, r.bit) for r in rows],
                         [("u1.x[3]", "inpad", 0), ("u1.y", "in", 2), ("u1.y", "out", 0)])


def parse(*paths: list[str]) -> tr.Report:
    """read_report on the inline path sections ``paths``."""
    tmp = Path(tempfile.mkdtemp(prefix="ot-trep-"))
    try:
        (tmp / "r.rpt").write_text("\n".join(["#Timing report of worst 9 path(s)", ""]
                                              + [ln for p in paths for ln in p]
                                              + ["#End of timing report", ""]))
        return tr.read_report(tmp / "r.rpt")
    finally:
        shutil.rmtree(tmp, True)


class ReportSections(unittest.TestCase):
    """The shapes of a report that are not a clean setup path (formats checked against
    TimingReporter.cpp: report_timing_path prints Startpoint/Endpoint/Path Type, the Point table,
    then the slack line; only constrained paths are reported, the unconstrained ones go to
    report_unconstrained_*, a separate file)."""

    def test_clean_report_has_nothing_skipped(self) -> None:
        report = parse(path_text(1), path_text(2))
        self.assertEqual((len(report.paths), report.skipped_total), (2, 0))

    def test_truncated_last_path_is_counted(self) -> None:
        cut = path_text(2)[:12]  # the file ends in the middle of the data rows
        report = parse(path_text(1), cut)
        self.assertEqual(len(report.paths), 1)
        self.assertEqual(report.skipped, {"no_slack": 1})

    def test_header_only_report_is_an_error(self) -> None:
        tmp = scratch(self, "ot-trep-")
        (tmp / "r.rpt").write_text("#Timing report of worst 0 path(s)\n# Unit scale: 1e-09\n"
                                   "\n#End of timing report\n")
        with self.assertRaises(tr.ReportError) as ctx:
            tr.read_report(tmp / "r.rpt")
        self.assertIn("no '#Path'", str(ctx.exception))

    def test_no_slack_line_anywhere_is_an_error(self) -> None:
        tmp = scratch(self, "ot-trep-")
        (tmp / "r.rpt").write_text("\n".join(path_text(1, slack=None)))
        with self.assertRaises(tr.ReportError) as ctx:
            tr.read_report(tmp / "r.rpt")
        self.assertIn("no 'slack' line", str(ctx.exception))

    def test_path_without_data_rows_is_counted(self) -> None:
        report = parse(path_text(1), path_text(2, rows=False))
        self.assertEqual((len(report.paths), report.skipped), (1, {"no_rows": 1}))

    def test_unconstrained_entries_are_not_paths(self) -> None:
        # VPR writes the unconstrained start/endpoints to their own file as "<id> <type> <name>"
        # lines with no '#Path': a setup report that holds only those is not a setup report
        tmp = scratch(self, "ot-trep-")
        (tmp / "u.rpt").write_text("#Unconstrained setup startpoint/endpoint report\n\n"
                                   "12 SOURCE some_input\n13 SINK some_output\n"
                                   "#End of unconstrained setup startpoint/endpoint report\n")
        with self.assertRaises(tr.ReportError):
            tr.read_report(tmp / "u.rpt")
        # and a path section whose slack line is missing is skipped, not attributed as zero
        report = parse(path_text(1), path_text(2, slack=None))
        self.assertEqual(report.skipped, {"no_slack": 1})

    def test_hold_path_in_setup_report_is_skipped_and_counted(self) -> None:
        report = parse(path_text(1), path_text(2, kind="hold", slack="slack (MET)  0.050"))
        self.assertEqual([p.index for p in report.paths], [1])
        self.assertEqual(report.skipped, {"hold": 1})

    def test_only_hold_paths_is_an_error(self) -> None:
        with self.assertRaises(tr.ReportError) as ctx:
            parse(path_text(1, kind="hold", slack="slack (MET)  0.050"))
        self.assertIn("1 hold", str(ctx.exception))

    def test_multi_clock_startpoints(self) -> None:
        report = parse(
            path_text(1, "a.Q[0] (.latch clocked by clkA)", "b.D[0] (.latch clocked by clkB)",
                      launch_clock="clkA"),
            path_text(2, "c.Q[0] (.latch clocked by clkB)", "d.D[0] (.latch clocked by clkA)",
                      launch_clock="clkB"),
            path_text(3, "x[0].inpad[0] (.input clocked by virtual_io_clock)",
                      "out:y.outpad[0] (.output clocked by clkA)"))
        self.assertEqual(len(report.paths), 3)
        self.assertTrue(report.paths[0].startpoint.endswith("clocked by clkA)"))
        self.assertTrue(report.paths[1].endpoint.endswith("clocked by clkA)"))
        self.assertIn("virtual_io_clock", report.paths[2].startpoint)
        for path in report.paths:  # the clock network rows are not data rows
            self.assertEqual([r.atom for r in path.rows], ["g", "out:g"])

    def test_skipped_are_reported_in_the_attribution(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        rpt = flow_dir / "vpr" / "run" / "report_timing.setup.rpt"
        rpt.write_text(rpt.read_text() + "\n".join(path_text(9, kind="hold")) + "\n"
                       + "\n".join(path_text(10, slack=None)) + "\n")
        report = ta.attribute_flow_dir(flow_dir)
        t = report["timing"]
        self.assertEqual((t["paths_skipped"], t["paths_skipped_reasons"]),
                         (2, {"hold": 1, "no_slack": 1}))
        self.assertIn("Skipped: 2 report paths", ta.summary_text(report))


class Hops(unittest.TestCase):
    def test_delays_add_up(self) -> None:
        for name in (ADDER, DFF):
            paths = tr.parse_report(FIXTURES / name / "vpr" / "run" / "report_timing.setup.rpt")
            for path in paths:
                hops = ta.path_hops(path)
                total = sum(h.delay_ns(hops[i + 1] if i + 1 < len(hops) else None)
                            for i, h in enumerate(hops))
                self.assertAlmostEqual(total, sum(r.incr for r in path.rows), places=6)

    def test_cell_and_net_delay(self) -> None:
        path = tr.parse_report(FIXTURES / DFF / "vpr" / "run" / "report_timing.setup.rpt")[0]
        hops = ta.path_hops(path)
        self.assertEqual(len(hops), 2)
        self.assertAlmostEqual(hops[0].cell_ns, 0.124)  # clock to output
        self.assertAlmostEqual(hops[1].net_in_ns, 1.285)  # the net the flop drives
        self.assertAlmostEqual(hops[0].delay_ns(hops[1]), 1.409)

    def test_flop_feeding_itself_is_two_hops(self) -> None:
        launch = tr.Row("f", "Q", 0, ".latch", "clock-to-output", 0.1, 0.1)
        capture = tr.Row("f", "D", 0, ".latch", "", 0.3, 0.4)
        path = tr.TimingPath(1, rows=[launch, capture])
        hops = ta.path_hops(path)
        self.assertEqual([h.atom for h in hops], ["f", "f"])
        self.assertAlmostEqual(hops[1].net_in_ns, 0.3)
        self.assertAlmostEqual(hops[1].cell_ns, 0.0)


class Joining(unittest.TestCase):
    def test_name_map(self) -> None:
        names = ta.build_name_map(FIXTURES / DFF / "odin3" / f"{DFF}.blif",
                                  FIXTURES / DFF / "timing" / "prov.blif")
        self.assertEqual(names.names["$n7"], (DFF, "$n7", "micro/bm_DL_logic_w_Dff.v", 10))
        self.assertEqual(names.names["$n8"][3], 11)
        self.assertEqual(names.blocks, 4)
        self.assertIn("Clock", names.inputs)
        self.assertEqual(names.outputs, {"f", "g"})

    def test_misaligned_files_are_refused(self) -> None:
        tmp = scratch(self, "ot-tmap-")
        short = FIXTURES / DFF / "odin3" / f"{DFF}.blif"
        prov = (FIXTURES / DFF / "timing" / "prov.blif").read_text().splitlines()
        (tmp / "short.blif").write_text(prov[0] + "\n")  # too few lines
        with self.assertRaises(ta.AttributionError):
            ta.build_name_map(tmp / "short.blif", FIXTURES / DFF / "timing" / "prov.blif")
        (tmp / "cut.blif").write_text("\n".join(prov[:-3]) + "\n")  # fewer lines than short
        with self.assertRaises(ta.AttributionError):
            ta.build_name_map(short, tmp / "cut.blif")

    def test_two_names_for_one_net_are_refused(self) -> None:
        tmp = scratch(self, "ot-tdup-")
        (tmp / "s.blif").write_text(".names a b\n.names a c\n")
        (tmp / "p.blif").write_text(".names a t/b@f.v:1\n.names a t/c@f.v:2\n")
        names = ta.build_name_map(tmp / "s.blif", tmp / "p.blif")
        self.assertEqual(names.names["b"], ("t", "b", "f.v", 1))
        (tmp / "s2.blif").write_text(".names x b\n.names y b\n")
        (tmp / "p2.blif").write_text(".names x t/b@f.v:1\n.names y t/b@f.v:2\n")
        with self.assertRaises(ta.AttributionError):
            ta.build_name_map(tmp / "s2.blif", tmp / "p2.blif")

    def test_helpers(self) -> None:
        self.assertEqual(ta.split_word("S[3]"), ("S", 3))
        self.assertEqual(ta.split_word("clk"), ("clk", None))
        self.assertEqual(ta.split_word("$n12"), (None, None))
        self.assertEqual(ta.net_of("cin=$n5"), "$n5")
        self.assertEqual(ta.assigned_word("  x[3:0] <= a + b;"), "x")
        self.assertEqual(ta.assigned_word("assign y = a & b;"), "y")
        self.assertEqual(ta.assigned_word("if (a == b)"), None)
        self.assertEqual(ta.assigned_word("always @(posedge clk)"), None)
        self.assertIsNone(ta.assigned_word(None))

    def test_word_of_generated_net_from_json(self) -> None:
        tmp = scratch(self, "ot-tjson-")
        module = {"netnames": {"count": {"bits": [5, 6, 7]}, "ok": {"bits": [9]},
                               "$auto": {"bits": [11]}},
                  "cells": {"$c1": {"attributes": {"src": "f.v:3.1-3.9"},
                                    "port_directions": {"Y": "output"},
                                    "connections": {"Y": [6]}}}}
        (tmp / "n.json").write_text(json.dumps({"modules": {"top": module}}))
        index = ta.load_json_index(tmp / "n.json", "top")
        assert index is not None
        self.assertEqual(index.net_word("$n6"), ("count", 2))  # bit id 7 is net 6, third bit
        self.assertEqual(index.net_word("$n8"), ("ok", None))
        self.assertIsNone(index.net_word("$n10"))  # only a generated name: no word
        self.assertIsNone(index.net_word("count"))
        self.assertEqual(index.lookup("count[0]"), None)  # bit 5 has no driver cell
        self.assertEqual(index.lookup("count[1]"), ("f.v:3.1-3.9", "top"))
        self.assertIsNone(ta.load_json_index(tmp / "none.json", "top"))

    def test_source_files(self) -> None:
        tmp = scratch(self, "ot-tsrc-")
        (tmp / "a.v").write_text("module a;\n   x <= y;\nendmodule\n")
        sources = ta.SourceFiles()
        self.assertEqual(sources.text(str(tmp / "a.v"), 2), "x <= y;")
        self.assertIsNone(sources.text(str(tmp / "a.v"), 9))
        self.assertEqual(sources.unreadable, set())  # a line out of range is not an unread file
        self.assertIsNone(sources.text(str(tmp / "none.v"), 1))
        self.assertEqual(sources.unreadable, {str(tmp / "none.v")})

    def test_relative_source_names_use_the_recorded_roots_not_the_cwd(self) -> None:
        tmp = scratch(self, "ot-tsrc-")
        (tmp / "src").mkdir()
        (tmp / "src" / "a.v").write_text("module a;\n   x <= y;\nendmodule\n")
        (tmp / "run").mkdir()
        (tmp / "run" / "b.v").write_text("one\ntwo\n")
        sources = ta.SourceFiles([tmp / "run"], [tmp / "src" / "a.v"])
        self.assertEqual(sources.text("b.v", 2), "two")  # under a root
        self.assertEqual(sources.text("src/a.v", 2), "x <= y;")  # by suffix of a design path
        self.assertEqual(sources.text("elsewhere/a.v", 1), "module a;")  # one design of that name
        self.assertEqual(sources.text(str(tmp / "gone" / "a.v"), 1), "module a;")  # moved tree
        self.assertIsNone(sources.text("nope.v", 1))
        self.assertEqual(sources.unreadable, {"nope.v"})
        here = Path.cwd()
        os.chdir(tmp / "run")  # the process directory is not consulted
        try:
            self.assertIsNone(ta.SourceFiles().text("b.v", 1))
        finally:
            os.chdir(here)

    def test_json_offsets_and_aliases(self) -> None:
        tmp = scratch(self, "ot-tjson-")
        cells = {f"$c{i}": {"attributes": {"src": f"f.v:{i}.1-{i}.9"},
                            "port_directions": {"Y": "output"}, "connections": {"Y": [code]}}
                 for i, code in enumerate((20, 21, 22, 30, 31), 1)}
        module = {"netnames": {
            "up": {"bits": [20, 21, 22], "offset": 4},               # up[4]=20 up[5]=21 up[6]=22
            "down": {"bits": [30, 31], "offset": 2, "upto": 1},      # down[3]=30 down[2]=31
            "z_long_alias": {"bits": [20]}, "a": {"bits": [21]}}, "cells": cells}
        (tmp / "n.json").write_text(json.dumps({"modules": {"top": module}}))
        index = ta.load_json_index(tmp / "n.json", "top")
        assert index is not None
        self.assertEqual(index.lookup("up[4]"), ("f.v:1.1-1.9", "top"))
        self.assertEqual(index.lookup("up[6]"), ("f.v:3.1-3.9", "top"))
        self.assertEqual(index.lookup("down[3]"), ("f.v:4.1-4.9", "top"))  # upto: msb first
        self.assertEqual(index.lookup("down[2]"), ("f.v:5.1-5.9", "top"))
        self.assertIsNone(index.lookup("up[0]"))  # below the offset
        self.assertIsNone(index.lookup("up[7]"))  # above the word
        self.assertIsNone(index.lookup("up"))  # several bits, which one is unknown
        # one net, several names: the shortest name wins, then the smaller name
        self.assertEqual(index.bit_names[20], ("up", 4))  # 'up' is shorter than 'z_long_alias'
        self.assertEqual(index.bit_names[21], ("a", None))  # 'a' is shorter than 'up'
        self.assertEqual(index.bit_names[22], ("up", 6))
        self.assertEqual(index.bit_names[30], ("down", 3))

class Attribution(unittest.TestCase):
    def test_adder_attribution(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / ADDER)
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["timing"]["cpd_ns"], 4.61929)
        self.assertEqual(report["timing"]["paths_parsed"], 10)
        crit = report["critical_path"]
        self.assertEqual(crit["startpoint"].split()[0], "X[0].inpad[0]")
        lines = [s["line"] for s in crit["spans"] if s["kind"] == "matched"]
        self.assertEqual(lines, [15, 19, 16])  # Z = X+Y+Cin; {Cout,S} = Z+6; if (...)
        self.assertAlmostEqual(sum(s["delay_ns"] for s in crit["spans"]), crit["data_delay_ns"],
                               places=5)
        self.assertAlmostEqual(crit["data_delay_ns"], 4.619, places=3)
        self.assertAlmostEqual(sum(s["share"] for s in crit["spans"]), 1.0, places=4)
        # the LUT that drives the port Cout has a port-named net: found through the JSON
        last = [n for n in report["paths"][0]["nodes"] if n["atom"] == "Cout"][0]
        self.assertEqual((last["category"], last["word"], last["span"].endswith(":16")),
                         ("matched", "Cout", True))

    def test_coverage_adds_up(self) -> None:
        for name in (ADDER, DFF):
            cov = ta.attribute_flow_dir(FIXTURES / name)["coverage"]
            self.assertEqual(cov["matched"] + cov["port"] + cov["unmatched"], cov["nodes"])
            self.assertAlmostEqual(cov["matched_share"] + cov["port_share"]
                                   + cov["unmatched_share"], 1.0, places=5)
            self.assertEqual(cov["unmatched"], 0)

    def test_aggregates_over_paths(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / ADDER)
        spans = {s["line"]: s for s in report["spans"]}
        self.assertEqual(spans[15]["paths_through"], 10)  # every path goes through Z = X+Y+Cin
        self.assertAlmostEqual(spans[15]["worst_slack_ns"], report["timing"]["worst_slack_ns"])
        # the fixture's provenance names a relative path that exists nowhere here: the text is
        # null AND the unreadable file is counted and shown, never silent
        self.assertIsNone(spans[15]["text"])
        cov = report["coverage"]
        self.assertEqual((cov["source_files"], cov["source_files_unreadable"]), (1, 1))
        self.assertEqual(cov["source_files_unreadable_list"], ["micro/bm_DL_BCD_adder.v"])
        self.assertIn("Source text unavailable: 1 of 1", ta.summary_text(report))
        self.assertEqual(report["modules"][0]["module"], ADDER)
        words = {(w["word"]) for w in report["words"]}
        self.assertTrue({"X", "Cout"} <= words)
        shares = [s["critical_share"] for s in report["spans"]]
        self.assertLessEqual(sum(shares), 1.0 + EPS)

    def test_source_resolved_from_the_inputs_from_any_cwd(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        (flow_dir / "odin3" / "micro").mkdir()
        (flow_dir / "odin3" / "micro" / "bm_DL_logic_w_Dff.v").write_text(
            "\n" * 7 + "always @(posedge Clock)\n\n  f <= x1 & x2;\n  g <= f | x3;\n")
        inputs = flow_dir / "timing" / "inputs.json"
        data = json.loads(inputs.read_text())
        data["source_cwd"] = "odin3"  # where odin3 ran: the provenance path is relative to it
        inputs.write_text(json.dumps(data))
        here, other = Path.cwd(), scratch(self, "ot-tcwd-")
        os.chdir(other)
        try:
            report = ta.attribute_flow_dir(flow_dir)
        finally:
            os.chdir(here)
        texts = {s["line"]: (s["text"], s["assigned_word"]) for s in report["spans"]}
        self.assertEqual(texts[10], ("f <= x1 & x2;", "f"))
        cov = report["coverage"]
        self.assertEqual((cov["source_files"], cov["source_files_unreadable"]), (1, 0))
        # the same flow dir, moved, with only the designs recorded by name
        data["source_cwd"], data["designs"] = None, [str(flow_dir / "odin3" / "micro" /
                                                         "bm_DL_logic_w_Dff.v")]
        inputs.write_text(json.dumps(data))
        again = ta.attribute_flow_dir(flow_dir)
        self.assertEqual(again["coverage"]["source_files_unreadable"], 0)

    def test_word_via_says_how_the_word_was_found(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / ADDER)
        nodes = [n for p in report["paths"] for n in p["nodes"]]
        vias = {(n["category"], n["word_via"]) for n in nodes if n["word"]}
        self.assertIn(("port", "pad"), vias)
        self.assertTrue({v for _, v in vias} <= {"name", "json-id", "pad"})
        self.assertTrue(all(n["word_via"] is None for n in nodes if not n["word"]))
        last = [n for n in report["paths"][0]["nodes"] if n["atom"] == "Cout"][0]
        self.assertEqual(last["word_via"], "name")
        spans = {s["line"]: s for s in report["spans"]}
        self.assertEqual(spans[16]["words_via"]["Cout"], ["name"])
        for crit in report["critical_path"]["spans"]:
            self.assertEqual(set(crit["words_via"]), set(crit["words"]))
        for row in report["words"]:
            self.assertTrue(row["word_via"])

    def test_json_id_join_is_marked(self) -> None:
        tmp = scratch(self, "ot-tjid-")
        module = {"netnames": {"count": {"bits": [5, 6, 7]}}, "cells": {}}
        (tmp / "n.json").write_text(json.dumps({"modules": {"top": module}}))
        names = ta.NameMap({"$n6": ("top", "$n6", "f.v", 3), "$n1": ("top", "$n1", "f.v", 4),
                            "q": ("top", "q[2]", "f.v", 5)})
        resolver = ta.Resolver(names, ta.load_json_index(tmp / "n.json", "top"), "top")
        joined = resolver.resolve("$n6", ".names")
        self.assertEqual((joined.word, joined.bit, joined.word_via), ("count", 2, "json-id"))
        self.assertEqual(resolver.resolve("$n1", ".names").word_via, "")  # no word, no method
        self.assertEqual(resolver.resolve("q", ".names").word_via, "name")
        self.assertEqual(resolver.resolve("out:q", ".output").word_via, "pad")

    def test_pads_are_flagged_and_kept_apart_in_words(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / ADDER)
        cov = report["coverage"]
        self.assertAlmostEqual(cov["matched_share_excl_port"],
                               cov["matched"] / (cov["matched"] + cov["unmatched"]))
        self.assertGreater(cov["port"], 0)
        cout = [w for w in report["words"] if w["word"] == "Cout"]
        self.assertEqual({w["port"] for w in cout}, {True, False})  # the pad and the LUT net
        top = report["modules"][0]
        self.assertGreater(top["port_nodes"], 0)
        self.assertLess(top["port_nodes"], top["nodes"])

    def test_flop_design(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / DFF)
        self.assertEqual(report["timing"]["fmax_mhz"], 689.17)
        spans = report["critical_path"]["spans"]
        self.assertEqual(spans[0]["line"], 8)  # the always block: flop g and its output net
        self.assertEqual(spans[0]["words"], ["g"])
        self.assertAlmostEqual(spans[0]["delay_ns"], 1.409)
        self.assertEqual(spans[-1]["kind"], "port")

    def test_paths_limit(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / ADDER, paths=3)
        t = report["timing"]
        self.assertEqual((t["paths_parsed"], len(report["paths"])), (3, 3))
        self.assertEqual((t["paths_kept"], t["paths_limit"], t["paths_requested"]), (3, 3, 100))
        self.assertEqual((t["paths_in_report"], t["paths_skipped"]), (10, 0))
        full = ta.attribute_flow_dir(FIXTURES / ADDER)["timing"]
        self.assertEqual((full["paths_kept"], full["paths_limit"]), (10, None))

    def test_paths_below_one_are_refused(self) -> None:
        for bad in (0, -1):
            with self.assertRaises(ta.AttributionError):
                ta.attribute_flow_dir(FIXTURES / DFF, paths=bad)

    def test_unreadable_inputs_are_attribution_errors(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        (flow_dir / "timing" / "inputs.json").write_text("{not json")
        with self.assertRaises(ta.AttributionError) as ctx:
            ta.attribute_flow_dir(flow_dir)
        self.assertIn("cannot read an input", str(ctx.exception))
        flow_dir = copy_fixture(self, DFF)
        (flow_dir / "timing" / "netlist.odin3.json").write_text("[")
        with self.assertRaises(ta.AttributionError):
            ta.attribute_flow_dir(flow_dir)
        flow_dir = copy_fixture(self, DFF)
        (flow_dir / "timing" / "prov.blif").write_bytes(b"\xff\xfe\x00")  # not text
        with self.assertRaises(ta.AttributionError):
            ta.attribute_flow_dir(flow_dir)
        flow_dir = copy_fixture(self, DFF)
        rpt = flow_dir / "vpr" / "run" / "report_timing.setup.rpt"
        rpt.unlink()
        rpt.mkdir()  # a directory where the report should be: OSError, not a traceback
        with self.assertRaises(ta.AttributionError):
            ta.attribute_flow_dir(flow_dir)

    def test_unmatched_nodes_are_listed_never_dropped(self) -> None:
        flow_dir = copy_fixture(self, ADDER)
        prov = flow_dir / "timing" / "prov.blif"
        prov.write_text(prov.read_text().replace("@micro/bm_DL_BCD_adder.v:15", ""))
        (flow_dir / "timing" / "netlist.odin3.json").unlink()
        report = ta.attribute_flow_dir(flow_dir)
        cov = report["coverage"]
        self.assertGreater(cov["unmatched"], 0)
        self.assertEqual(cov["unmatched_atoms_total"], len(cov["unmatched_atoms"]))
        self.assertEqual(cov["matched"] + cov["port"] + cov["unmatched"], cov["nodes"])
        listed = sum(a["nodes"] for a in cov["unmatched_atoms"])
        self.assertEqual(listed, cov["unmatched"])
        kinds = {s["kind"] for s in report["critical_path"]["spans"]}
        self.assertIn("unmatched", kinds)
        self.assertIn("Unmatched:", ta.summary_text(report))

    def test_missing_inputs(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        (flow_dir / "timing" / "prov.blif").unlink()
        with self.assertRaises(ta.AttributionError) as ctx:
            ta.attribute_flow_dir(flow_dir)
        self.assertIn("missing input", str(ctx.exception))
        (flow_dir / "timing" / "inputs.json").unlink()
        with self.assertRaises(ta.AttributionError) as ctx:
            ta.attribute_flow_dir(flow_dir)
        self.assertIn("--timing-attr", str(ctx.exception))

    def test_missing_report(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        (flow_dir / "vpr" / "run" / "report_timing.setup.rpt").unlink()
        with self.assertRaises(ta.AttributionError):
            ta.attribute_flow_dir(flow_dir)


class Outputs(unittest.TestCase):
    def test_write_outputs_and_summary(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / ADDER)
        out = scratch(self, "ot-tout-")
        js, txt = ta.write_outputs(report, out / "timing")
        self.assertEqual(json.loads(js.read_text()), json.loads(json.dumps(report)))
        text = txt.read_text()
        self.assertIn("Fmax 216.48 MHz   CPD 4.619 ns", text)
        self.assertIn("These statements set your Fmax", text)
        self.assertIn("micro/bm_DL_BCD_adder.v:15", text)
        self.assertIn("module bm_DL_BCD_adder", text)
        self.assertLess(text.index(":15 "), text.index(":16 "))

    def test_summary_shows_source_text(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        src = flow_dir / "bm_DL_logic_w_Dff.v"
        src.write_text("\n" * 7 + "always @(posedge Clock)\n\n  f <= x1 & x2;\n  g <= f | x3;\n")
        for name in ("prov.blif", "netlist.odin3.json"):  # point the spans at the real file
            path = flow_dir / "timing" / name
            path.write_text(path.read_text().replace("micro/bm_DL_logic_w_Dff.v", str(src)))
        report = ta.attribute_flow_dir(flow_dir)
        texts = {s["line"]: (s["text"], s["assigned_word"]) for s in report["spans"]}
        self.assertEqual(texts[8], ("always @(posedge Clock)", None))
        self.assertEqual(texts[10], ("f <= x1 & x2;", "f"))
        self.assertIn("| always @(posedge Clock)", ta.summary_text(report))
        self.assertIn("word ~f", ta.summary_text(report))

    def test_json_schema_keys(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES / DFF)
        self.assertEqual(set(report), {"schema_version", "design", "timing", "coverage",
                                       "critical_path", "spans", "modules", "words", "paths"})
        self.assertEqual(set(report["paths"][0]),
                         {"index", "slack_ns", "arrival_ns", "required_ns", "startpoint",
                          "endpoint", "nodes"})
        self.assertEqual(set(report["timing"]),
                         {"cpd_ns", "fmax_mhz", "paths_requested", "paths_limit",
                          "paths_in_report", "paths_kept", "paths_parsed", "paths_skipped",
                          "paths_skipped_reasons", "worst_slack_ns", "critical_data_delay_ns"})
        self.assertEqual(set(report["paths"][0]["nodes"][0]),
                         {"atom", "block_type", "pins", "cell_delay_ns", "net_delay_in_ns",
                          "delay_ns", "category", "span", "module", "word", "bit", "word_via"})
        self.assertEqual(set(report["spans"][0]),
                         {"id", "file", "line", "text", "modules", "words", "assigned_word",
                          "paths_through", "worst_slack_ns", "nodes", "critical_delay_ns",
                          "critical_share", "words_via"})
        self.assertEqual(set(report["words"][0]),
                         {"module", "word", "port", "bits", "word_via", "paths_through",
                          "worst_slack_ns", "nodes", "critical_delay_ns", "critical_share"})
        self.assertEqual(set(report["modules"][0]),
                         {"module", "words", "port_nodes", "paths_through", "worst_slack_ns",
                          "nodes", "critical_delay_ns", "critical_share"})


class NameMapAttribution(unittest.TestCase):
    """The version 2 route (V1.2.0 Task 7): the join is by atom name through the map next to the
    hash-name BLIF, with the full span, the construct chain and the DECLARATOR word. The fixtures
    are the two micros of the older ones re-run with `--prov` (VPR report translated to the hash
    names by pairing the two BLIFs) and `decl`, a design with a declarator initialiser."""

    def paths(self, name: str) -> list[dict[str, object]]:
        return ta.attribute_flow_dir(FIXTURES_V2 / name)["paths"]  # type: ignore[no-any-return]

    def test_full_span_and_chain_of_a_statement(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES_V2 / DFF)
        node = next(n for n in report["paths"][0]["nodes"] if n["atom"] == "g")
        self.assertEqual(node["span"], "tests/micro/verilog/micro/bm_DL_logic_w_Dff.v:8")
        self.assertTrue(node["loc"].endswith("bm_DL_logic_w_Dff.v:8.2-12.5"), node["loc"])
        self.assertEqual(
            node["chain_text"],
            "block ALWAYS 8.2-12.5 < module MODULE bm_DL_logic_w_Dff 3.1-14.10")
        first = report["critical_path"]["spans"][0]
        self.assertEqual(first["loc"], node["loc"])
        self.assertEqual([c["level"] for c in first["chain"]], ["block", "module"])
        self.assertEqual(report["design"]["name_map"].rsplit("/", 1)[1], f"{DFF}.blif.names")
        self.assertIsNone(report["design"]["provenance_blif"])

    def test_same_spans_modules_and_words_as_the_older_route(self) -> None:
        for name in (ADDER, DFF):
            new = ta.attribute_flow_dir(FIXTURES_V2 / name)
            old = ta.attribute_flow_dir(FIXTURES / name)
            for key in ("matched", "port", "unmatched", "nodes"):
                self.assertEqual(new["coverage"][key], old["coverage"][key], key)
            for a, b in zip(new["paths"], old["paths"], strict=True):
                for x, y in zip(a["nodes"], b["nodes"], strict=True):
                    for key in ("category", "module", "word", "bit"):
                        self.assertEqual(x[key], y[key], (name, x["atom"], key))
                    # the files differ only in how the design path was spelled; line is the same
                    self.assertEqual((x["span"] or "").rsplit(":", 1)[-1],
                                     (y["span"] or "").rsplit(":", 1)[-1])

    def test_generated_net_gets_the_word_of_its_declarator(self) -> None:
        report = ta.attribute_flow_dir(FIXTURES_V2 / "decl")
        nodes = {n["atom"]: n for n in report["paths"][0]["nodes"]}
        adder = nodes["hrbr7iafsqp"]  # a generated net ($nNN) inside `wire [3:0] t = a + b`
        self.assertEqual((adder["word"], adder["bit"], adder["word_via"]),
                         ("t", None, "declarator"))
        self.assertTrue(adder["loc"].endswith("decl.v:4.17-4.22"), adder["loc"])
        self.assertEqual(adder["chain_text"], "expr BINARY 4.17-4.22 < stmt DECLARATOR t "
                         "4.13-4.22 < stmt NET_DECL 4.2-4.23 < module MODULE decl 1.1-6.10")
        self.assertEqual(nodes["h5r4pj6r3jg"]["word_via"], "name")  # a source net: t[1]
        self.assertEqual((nodes["y[1]"]["word"], nodes["y[1]"]["bit"]), ("y", 1))
        cov = report["coverage"]
        self.assertEqual((cov["generated_nodes"], cov["generated_word_nodes"],
                          cov["generated_word_share"]), (2, 2, 1.0))
        self.assertEqual((cov["net_nodes"], cov["span_share"], cov["loc_share"],
                          cov["chain_share"]), (4, 1.0, 1.0, 1.0))

    def test_generated_nets_without_a_declarator_have_no_word(self) -> None:
        cov = ta.attribute_flow_dir(FIXTURES_V2 / ADDER)["coverage"]
        self.assertGreater(cov["generated_nodes"], 0)
        self.assertEqual(cov["generated_word_nodes"], 0)
        self.assertEqual(cov["chain_share"], 1.0)

    def test_atom_not_in_the_map_is_unmatched_and_listed(self) -> None:
        flow_dir = copy_fixture_v2(self, DFF)
        report = (flow_dir / "vpr/run/report_timing.setup.rpt")
        report.write_text(report.read_text().replace("hvihy24qg2f", "zzunknown"))
        cov = ta.attribute_flow_dir(flow_dir)["coverage"]
        self.assertEqual([e["atom"] for e in cov["unmatched_atoms"]], ["zzunknown"])

    def test_version_1_map_and_missing_map_are_refused(self) -> None:
        flow_dir = copy_fixture_v2(self, DFF)
        mp = flow_dir / "odin3" / f"{DFF}.blif.names"
        mp.write_text("# odin3-names 1 hash=sha256 width=10\n")
        with self.assertRaisesRegex(ta.AttributionError, "version 1"):
            ta.attribute_flow_dir(flow_dir)
        mp.with_name("gone").write_text("x")
        inputs = json.loads((flow_dir / "timing/inputs.json").read_text())
        inputs["name_map"] = "odin3/gone.names"
        (flow_dir / "timing/inputs.json").write_text(json.dumps(inputs))
        with self.assertRaisesRegex(ta.AttributionError, "missing input"):
            ta.attribute_flow_dir(flow_dir)

    def test_summary_shows_the_chain(self) -> None:
        text = ta.summary_text(ta.attribute_flow_dir(FIXTURES_V2 / "decl"))
        self.assertIn("      chain: expr BINARY 4.17-4.22 < stmt DECLARATOR t", text)

    def test_schema_version_and_old_keys_stay(self) -> None:
        new = ta.attribute_flow_dir(FIXTURES_V2 / DFF)
        old = ta.attribute_flow_dir(FIXTURES / DFF)
        self.assertEqual(new["schema_version"], 1)
        for section in ("design", "timing", "coverage"):
            self.assertLessEqual(set(old[section]), set(new[section]), section)
        self.assertLessEqual(set(old["paths"][0]["nodes"][0]), set(new["paths"][0]["nodes"][0]))


class OlderFlowDirectory(unittest.TestCase):
    """A flow directory made by v1.1.1 (a second provenance BLIF, no map) takes the older route:
    its attribution.json and summary.txt are byte for byte what main (v1.1.1) wrote. The golden
    files were made by main's tools/flow/timing_attr.py on these fixtures; the flow directory
    path is replaced by <FLOW>."""

    def test_byte_identical_to_main(self) -> None:
        for name in (ADDER, DFF):
            flow_dir = (FIXTURES / name).resolve()
            report = ta.attribute_flow_dir(flow_dir)
            js = (json.dumps(report, indent=1) + "\n").replace(str(flow_dir), "<FLOW>")
            text = ta.summary_text(report).replace(str(flow_dir), "<FLOW>")
            self.assertEqual(js, (GOLDEN_V1 / f"{name}.attribution.json").read_text(), name)
            self.assertEqual(text, (GOLDEN_V1 / f"{name}.summary.txt").read_text(), name)
            self.assertNotIn("loc", report["paths"][0]["nodes"][0])
            self.assertNotIn("name_map", report["design"])


class CommandLine(unittest.TestCase):
    def test_ok(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        code, out, err = run_main(ta.main, [str(flow_dir)])
        self.assertEqual((code, err), (0, ""))
        self.assertIn("These statements set your Fmax", out)
        self.assertTrue((flow_dir / "timing" / "attribution.json").is_file())
        self.assertTrue((flow_dir / "timing" / "summary.txt").is_file())

    def test_out_and_paths(self) -> None:
        flow_dir = copy_fixture(self, ADDER)
        dest = scratch(self, "ot-tcli-") / "o"
        code, _, _ = run_main(ta.main, [str(flow_dir), "--paths", "2", "--out", str(dest)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads((dest / "attribution.json").read_text())["timing"]
                         ["paths_parsed"], 2)

    def test_failure_is_exit_1(self) -> None:
        empty = scratch(self, "ot-tcli-")
        code, _, err = run_main(ta.main, [str(empty)])
        self.assertEqual(code, 1)
        self.assertIn("--timing-attr", err)

    def test_unwritable_out_is_exit_1_not_a_traceback(self) -> None:
        flow_dir = copy_fixture(self, DFF)
        blocker = scratch(self, "ot-tcli-") / "file"
        blocker.write_text("x")
        code, _, err = run_main(ta.main, [str(flow_dir), "--out", str(blocker / "sub")])
        self.assertEqual(code, 1)
        self.assertIn("odin3-timing:", err)

    def test_usage_errors_are_exit_2(self) -> None:
        self.assertEqual(run_main(ta.main, ["/nonexistent/flow"])[0], 2)
        self.assertEqual(run_main(ta.main, [str(FIXTURES / DFF), "--paths", "0"])[0], 2)
        self.assertEqual(run_main(ta.main, [])[0], 2)


class FlowOptions(unittest.TestCase):
    def parse(self, *extra: str) -> flow.FlowArgs:
        with mock.patch.dict(os.environ, {"VTR_ROOT": "/nonexistent-vtr"}):
            return flow.parse_args([str(ROOT / "tests" / "micro" / "verilog" / "micro" /
                                        "bm_DL_logic_w_Dff.v"), "--out", "/tmp/ot-never", *extra])

    def test_defaults(self) -> None:
        args = self.parse("--vpr")
        self.assertEqual((args.timing_attr, args.timing_paths), (False, 100))

    def test_timing_attr(self) -> None:
        args = self.parse("--vpr", "--timing-attr")
        self.assertEqual((args.timing_attr, args.timing_paths), (True, 100))

    def test_timing_paths_implies_attr(self) -> None:
        args = self.parse("--vpr", "--timing-paths", "250")
        self.assertEqual((args.timing_attr, args.timing_paths), (True, 250))

    def test_needs_vtr_target_and_vpr(self) -> None:
        bad = flow.FlowArgs([Path("x.v")], None, "vtr", None, "", False, False, Path("odin3"),
                            Path("/tmp/ot-never"), timing_attr=True)
        with self.assertRaises(flow.UsageError):
            flow.run_flow(bad)
        zero = flow.FlowArgs([Path("x.v")], None, "vtr", None, "", True, False, Path("odin3"),
                             Path("/tmp/ot-never"), timing_attr=True, timing_paths=0)
        with self.assertRaises(flow.UsageError):
            flow.run_flow(zero)

    def test_timing_script_v2_writes_no_second_blif(self) -> None:
        text = flow.timing_script(6, Path("/o/timing"))
        self.assertEqual(text, '; write_json --odin3 "/o/timing/netlist.odin3.json"')
        self.assertNotIn("provenance", text)

    def test_prov_script_names_the_blif_with_hashes_and_a_map(self) -> None:
        base = qor_legs.odin3_script("O", "d.v", "o.blif", 6, "short")
        text = flow.prov_script(base, 6)
        self.assertIn("read_verilog --nolatches --vtr-compat --prov-ast refs d.v;", text)
        self.assertIn("write_blif --vtr --lut-k 6 --name-style hash --prov-map o.blif", text)
        self.assertEqual(text.count("write_blif"), 1)
        with self.assertRaises(flow.UsageError):
            flow.prov_script("read_blif x; write_blif y", 6)

    def test_prov_options(self) -> None:
        self.assertEqual(self.parse("--vpr", "--prov").prov, True)
        self.assertEqual(self.parse("--vpr").prov, False)
        v2 = self.parse("--vpr", "--timing-attr")
        self.assertEqual((v2.prov, v2.timing_legacy), (True, False))  # --timing-attr uses the map
        old = self.parse("--vpr", "--timing-attr", "--timing-legacy")
        self.assertEqual((old.prov, old.timing_legacy), (False, True))
        for bad in (flow.FlowArgs([MICRO_DFF], None, "lpm", None, "", False, False,
                                  Path("odin3"), Path("/tmp/ot-never"), prov=True),
                    flow.FlowArgs([MICRO_DFF], None, "vtr", None, "", True, False,
                                  Path("odin3"), Path("/tmp/ot-never"), timing_attr=True,
                                  prov=True, timing_legacy=True),
                    flow.FlowArgs([MICRO_DFF], None, "vtr", None, "", True, False,
                                  Path("odin3"), Path("/tmp/ot-never"), timing_legacy=True)):
            with self.assertRaises(flow.UsageError) as caught:
                flow.run_flow(bad)
            self.assertNotIn("no such design", str(caught.exception))

    def test_timing_script(self) -> None:
        text = flow.timing_script(6, Path("/o/timing"), legacy=True)
        self.assertIn('write_blif --vtr --lut-k 6 --name-style provenance "/o/timing/prov.blif"',
                      text)
        self.assertIn('write_json --odin3 "/o/timing/netlist.odin3.json"', text)
        self.assertTrue(text.startswith("; "))

    def test_vpr_report_option_only_when_not_default(self) -> None:
        seen: list[list[str]] = []

        def fake_step(name: str, argv: list[str], *_a: object, **_k: object) -> flow.StepResult:
            seen.append(list(argv))
            return flow.StepResult(name, False, Path("/dev/null"), 0.0)

        blif = scratch(self, "ot-tvpr-") / "d.blif"
        blif.write_text(".model d\n.end\n")
        for paths, expect in ((100, False), (250, True)):
            args = flow.FlowArgs([Path("x.v")], None, "vtr", None, "", True, False, Path("odin3"),
                                 blif.parent, True, paths)
            with mock.patch.object(flow, "run_step", fake_step), \
                    mock.patch.object(qor_legs, "stage_timeout", return_value=10):
                flow.vpr_run(args, Path("/vtr"), Path("/a.xml"), 6, blif, blif.parent / "j", None)
            self.assertEqual("--timing_report_npaths" in seen[-1], expect)
            if expect:
                self.assertEqual(seen[-1][-2:], ["--timing_report_npaths", "250"])


@unittest.skipUnless(ODIN3, "needs ODIN3_CLI")
class ProvFlow(unittest.TestCase):
    """`odin3-flow --prov --no-vpr` through the real CLI: hash names and a version 2 map."""

    def test_prov_writes_the_hash_blif_and_a_version_2_map(self) -> None:
        tmp = scratch(self, "ot-tprov-")
        env = {k: v for k, v in os.environ.items() if k != "LD_PRELOAD"}
        with mock.patch.dict(os.environ, env, clear=True):
            code, text, err = run_main(flow.main, [str(MICRO_DFF), "--odin3", ODIN3, "--no-vpr",
                                                   "--prov", "--out", str(tmp / "o")])
        self.assertEqual(code, 0, text + err)
        blif = tmp / "o" / "odin3" / f"{DFF}.blif"
        names = blif.with_name(blif.name + ".names")
        self.assertTrue(names.read_text().startswith("# odin3-names 2 "))
        self.assertIn("step prov: ok", text)
        self.assertRegex(blif.read_text(), r"\bh[0-9a-z]{10}\b")
        self.assertNotIn("$n", blif.read_text())
        self.assertFalse((tmp / "o" / "timing").exists())  # no second BLIF, no timing step

    def test_prov_blif_equals_the_short_blif_up_to_names(self) -> None:
        tmp = scratch(self, "ot-tprov-")
        env = {k: v for k, v in os.environ.items() if k != "LD_PRELOAD"}
        with mock.patch.dict(os.environ, env, clear=True):
            for sub, extra in (("plain", []), ("prov", ["--prov"])):
                code, text, err = run_main(flow.main, [str(MICRO_DFF), "--odin3", ODIN3,
                                                       "--no-vpr", "--out", str(tmp / sub),
                                                       *extra])
                self.assertEqual(code, 0, text + err)
        short = [ln.split() for ln in ta.logical_lines(tmp / "plain/odin3" / f"{DFF}.blif")][2:]
        hashed = [ln.split() for ln in ta.logical_lines(tmp / "prov/odin3" / f"{DFF}.blif")][2:]
        self.assertEqual([len(x) for x in short], [len(x) for x in hashed])
        self.assertEqual([x[0] for x in short], [x[0] for x in hashed])


class PythonApi(unittest.TestCase):
    """odin3.timing_attribution reads files only; it needs the module, not the shared library, so
    the test stubs ``cffi`` when it is not installed."""

    def load(self) -> object:
        path = ROOT / "plugins" / "python" / "odin3.py"
        stub = {} if "cffi" in sys.modules or importlib.util.find_spec("cffi") else {
            "cffi": mock.MagicMock()}
        with mock.patch.dict(sys.modules, stub):
            spec = importlib.util.spec_from_file_location("odin3_under_test", path)
            assert spec is not None and spec.loader is not None
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module  # dataclasses look the module up by name
            self.addCleanup(sys.modules.pop, spec.name, None)
            spec.loader.exec_module(module)
        return module

    def test_timing_attribution(self) -> None:
        odin3 = self.load()
        report = odin3.timing_attribution(FIXTURES / DFF)  # type: ignore[attr-defined]
        self.assertEqual(report, ta.attribute_flow_dir(FIXTURES / DFF))
        with self.assertRaises(RuntimeError):
            odin3.timing_attribution(FIXTURES)  # type: ignore[attr-defined]
        with self.assertRaises(RuntimeError):  # a bad input is an AttributionError too
            odin3.timing_attribution(FIXTURES / DFF, 0)  # type: ignore[attr-defined]


@unittest.skipUnless(
    ODIN3 and VTR_ROOT and (Path(VTR_ROOT) / "build" / "vpr" / "vpr").is_file()
    and VPR_PYTHON.is_file(), "needs ODIN3_CLI and a built VTR_ROOT with .venv/bin/python")
class Integration(unittest.TestCase):
    MICRO = ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_DL_BCD_adder.v"

    def run_flow(self, out: Path, *extra: str) -> int:
        code, text, err = run_main(flow.main, [str(self.MICRO), "--odin3", ODIN3, "--out",
                                               str(out), "--vpr", *extra])
        self.assertEqual(code, 0, text + err)
        return code

    def test_real_flow_and_report_only(self) -> None:
        tmp = scratch(self, "ot-tint-")
        self.run_flow(tmp / "plain")
        self.run_flow(tmp / "attr", "--timing-attr", "--timing-legacy")
        self.run_flow(tmp / "map", "--timing-attr")
        blif = "bm_DL_BCD_adder.blif"
        # the v1.1.1 route: the extra writes and the report do not change what VPR reads
        self.assertEqual((tmp / "plain" / "odin3" / blif).read_bytes(),
                         (tmp / "attr" / "odin3" / blif).read_bytes())
        self.assertTrue((tmp / "attr" / "timing" / "prov.blif").is_file())
        # the v1.2.0 route: hash names and a map, no second BLIF, the same VPR figures
        self.assertFalse((tmp / "map" / "timing" / "prov.blif").exists())
        self.assertTrue((tmp / "map" / "odin3" / (blif + ".names")).is_file())
        a = json.loads((tmp / "plain" / "summary.json").read_text())
        b = json.loads((tmp / "attr" / "summary.json").read_text())
        c = json.loads((tmp / "map" / "summary.json").read_text())
        for key in ("luts", "clbs", "min_chan_width", "crit_path_ns", "wirelength"):
            self.assertEqual(a[key], b[key], key)
            self.assertEqual(a[key], c[key], key)
        report = json.loads((tmp / "attr" / "timing" / "attribution.json").read_text())
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["coverage"]["unmatched"], 0)
        self.assertIn("bm_DL_BCD_adder.v:15",
                      (tmp / "attr" / "timing" / "summary.txt").read_text())
        mapped = json.loads((tmp / "map" / "timing" / "attribution.json").read_text())
        self.assertEqual(mapped["schema_version"], 1)
        self.assertEqual(mapped["coverage"]["unmatched"], 0)
        self.assertEqual(mapped["coverage"]["chain_share"], 1.0)
        self.assertEqual([s["span"] for s in mapped["critical_path"]["spans"]],
                         [s["span"] for s in report["critical_path"]["spans"]])
        # the standalone command gives the same data
        again = ta.attribute_flow_dir(tmp / "attr")
        self.assertEqual(again["critical_path"]["spans"], report["critical_path"]["spans"])
        proc = subprocess.run([str(ROOT / "tools" / "flow" / "odin3-timing"), str(tmp / "map")],
                              capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Fmax", proc.stdout)


if __name__ == "__main__":
    unittest.main()
