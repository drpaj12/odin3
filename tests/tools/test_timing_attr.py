"""Tests for tools/flow/timing_report.py and timing_attr.py (v1.1.1, docs/V1.1.1.md): the VPR
setup-report parser, the BLIF join, the delay model, the aggregation, the outputs and the command
line, on small committed fixtures (a VPR report plus the matching Odin III files of two micros), so
no VPR is needed. One integration test runs the real flow and skips without VTR_ROOT and
ODIN3_CLI."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
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
ROOT = HERE.parents[1]
ODIN3 = os.environ.get("ODIN3_CLI", "")
VTR_ROOT = os.environ.get("VTR_ROOT", "")
ADDER, DFF = "bm_DL_BCD_adder", "bm_DL_logic_w_Dff"
EPS = 1e-6


def copy_fixture(case: unittest.TestCase, name: str) -> Path:
    """A writable copy of fixture ``name`` (a flow output directory)."""
    dest = scratch(case, "ot-tattr-") / name
    shutil.copytree(FIXTURES / name, dest)
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

    def test_source_text(self) -> None:
        tmp = scratch(self, "ot-tsrc-")
        (tmp / "a.v").write_text("module a;\n   x <= y;\nendmodule\n")
        cache: dict[str, list[str]] = {}
        self.assertEqual(ta.source_text(str(tmp / "a.v"), 2, cache), "x <= y;")
        self.assertIsNone(ta.source_text(str(tmp / "a.v"), 9, cache))
        self.assertIsNone(ta.source_text(str(tmp / "none.v"), 1, cache))


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
        self.assertEqual(spans[15]["assigned_word"], None)  # text is not readable: path is relative
        self.assertEqual(report["modules"][0]["module"], ADDER)
        words = {(w["word"]) for w in report["words"]}
        self.assertTrue({"X", "Cout"} <= words)
        shares = [s["critical_share"] for s in report["spans"]]
        self.assertLessEqual(sum(shares), 1.0 + EPS)

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
        self.assertEqual((report["timing"]["paths_parsed"], len(report["paths"])), (3, 3))

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
        self.assertEqual(set(report["paths"][0]["nodes"][0]),
                         {"atom", "block_type", "pins", "cell_delay_ns", "net_delay_in_ns",
                          "delay_ns", "category", "span", "module", "word", "bit"})
        self.assertEqual(set(report["spans"][0]),
                         {"id", "file", "line", "text", "modules", "words", "assigned_word",
                          "paths_through", "worst_slack_ns", "nodes", "critical_delay_ns",
                          "critical_share"})


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

    def test_timing_script(self) -> None:
        text = flow.timing_script(6, Path("/o/timing"))
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


class PythonApi(unittest.TestCase):
    def test_timing_attribution(self) -> None:
        try:
            import odin3
        except ImportError as exc:  # cffi missing
            self.skipTest(str(exc))
        report = odin3.timing_attribution(FIXTURES / DFF)
        self.assertEqual(report, ta.attribute_flow_dir(FIXTURES / DFF))
        with self.assertRaises(RuntimeError):
            odin3.timing_attribution(FIXTURES)


@unittest.skipUnless(ODIN3 and VTR_ROOT and (Path(VTR_ROOT) / "build" / "vpr" / "vpr").is_file(),
                     "needs ODIN3_CLI and a built VTR_ROOT")
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
        self.run_flow(tmp / "attr", "--timing-attr")
        # the extra writes and the report do not change what VPR reads or what it makes
        self.assertEqual((tmp / "plain" / "odin3" / "bm_DL_BCD_adder.blif").read_bytes(),
                         (tmp / "attr" / "odin3" / "bm_DL_BCD_adder.blif").read_bytes())
        a = json.loads((tmp / "plain" / "summary.json").read_text())
        b = json.loads((tmp / "attr" / "summary.json").read_text())
        for key in ("luts", "clbs", "min_chan_width", "crit_path_ns", "wirelength"):
            self.assertEqual(a[key], b[key], key)
        report = json.loads((tmp / "attr" / "timing" / "attribution.json").read_text())
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["coverage"]["unmatched"], 0)
        self.assertIn("bm_DL_BCD_adder.v:15",
                      (tmp / "attr" / "timing" / "summary.txt").read_text())
        # the standalone command gives the same data
        again = ta.attribute_flow_dir(tmp / "attr")
        self.assertEqual(again["critical_path"]["spans"], report["critical_path"]["spans"])
        proc = subprocess.run([str(ROOT / "tools" / "flow" / "odin3-timing"), str(tmp / "attr")],
                              capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Fmax", proc.stdout)


if __name__ == "__main__":
    unittest.main()
