"""v1.4: tools/flow/prov_steps.py (the step-file writer, the FPGA converter) and
tools/silicon/schema_check.py. Tests that need a name map use ODIN3_CLI (CTest python_tools)."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import schema_check

import name_map_v2
import prov_steps as ps

from .helpers import REPO_ROOT
from .test_names_v2 import ODIN3, write_map

SCHEMAS = REPO_ROOT / "docs" / "schemas"


def tiny_run(out: Path) -> Path:
    """A two-stage run written by hand."""
    w = ps.StepWriter(out, "demo", "d.v", "top", "1.4.0")
    w.add_tool("odin3", "1.4.0")
    src = out / "d.v"
    src.write_text("module top; endmodule\n")
    w.add_design_file("d.v", src)
    hdl = [ps.obj("hdl", "d.v:1.1-1.20", "stmt", "m", "hdl",
                  src=[{"file": "d.v", "span": "1.1-1.20"}])]
    w.add_step("read", "hdl", {"id": "odin3", "version": "1.4.0"}, hdl)
    cells = [ps.obj("ir", "c1", "cell", "c1", "hdl", [ps.link("hdl:d.v:1.1-1.20", "fine")],
                    type="$and"),
             ps.obj("ir", "c2", "cell", "c2", "physical:tap", [])]
    w.add_step("elab", "ir", {"id": "odin3", "version": "1.4.0"}, cells, needs=["read"])
    w.add_step("equiv", "ir", {"id": "equiv-check", "version": ""}, None, needs=["elab"],
               check={"name": "equiv-check", "result": "equivalent", "detail": ""})
    return w.finish()


class StepWriterTest(unittest.TestCase):
    def test_files_index_and_schema(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            index = tiny_run(Path(tmp))
            self.assertEqual(index, Path(tmp) / "prov" / "index.json")
            names = sorted(p.name for p in index.parent.iterdir())
            self.assertEqual(names, ["01-read.json", "01-read.objects.jsonl", "02-elab.json",
                                     "02-elab.objects.jsonl", "03-equiv.json", "index.json"])
            data = json.loads(index.read_text())
            self.assertEqual([s["id"] for s in data["steps"]], ["read", "elab", "equiv"])
            self.assertEqual(data["steps"][1]["count"], 2)
            self.assertIsNone(data["steps"][2]["objects"])
            head = json.loads((index.parent / "02-elab.json").read_text())
            self.assertEqual(head["counts"]["by_origin"], {"hdl": 1, "physical:tap": 1})
            self.assertEqual(head["counts"]["by_link_kind"], {"fine": 1})
            self.assertEqual(schema_check.check_dir(index.parent, SCHEMAS), [])

    def test_schema_check_catches_problems(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            index = tiny_run(Path(tmp))
            objs = index.parent / "02-elab.objects.jsonl"
            objs.write_text(objs.read_text().replace('"fine"', '"weird"'))
            errs = schema_check.check_dir(index.parent, SCHEMAS)
            self.assertTrue(any("sha256 differs" in e for e in errs))
            self.assertTrue(any("weird" in e for e in errs))

    def test_origin_hdl_needs_a_link(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            w = ps.StepWriter(Path(tmp), "demo", "d.v", "top", "1")
            w.add_step("elab", "ir", {"id": "odin3", "version": ""},
                       [ps.obj("ir", "c", "cell", "c", "hdl", [])])
            w.finish()
            errs = schema_check.check_dir(Path(tmp) / "prov", SCHEMAS)
            self.assertTrue(any("origin hdl without a link" in e for e in errs))

    def test_privacy_rule(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            w = ps.StepWriter(Path(tmp), "demo", "d.v", "top", "1")
            bad = ps.obj("ir", "c", "cell", "c", "hdl", [],
                         src=[{"file": "/home" + "/u/x.v", "span": "1.1-1.2"}])
            with self.assertRaises(ps.PrivacyError):
                w.add_step("elab", "ir", {"id": "odin3", "version": ""}, [bad])
            with self.assertRaises(ps.PrivacyError):
                w.add_step("elab", "ir", {"id": "odin3", "version": ""}, None,
                           inputs=[{"role": "design", "path": "/tmp/x.v"}])

    def test_windows_path_is_private(self) -> None:
        with self.assertRaises(ps.PrivacyError):
            ps._check_private("C:" + chr(92) + "x", "t")
        with self.assertRaises(ps.PrivacyError):
            ps._check_private({"a": ["see D:" + chr(92) + "y.v"]}, "t")

    def test_unknown_names_are_refused(self) -> None:
        with self.assertRaises(ValueError):
            ps.link("a:b", "nonsense")
        with self.assertRaises(ValueError):
            ps.link("a:b", "name", "certain")
        with self.assertRaises(ValueError):
            ps.obj("nowhere", "x", "cell", "x", "hdl")


FIXTURE = REPO_ROOT / "tests" / "tools" / "fixtures" / "prov_post" / "bm_DL_logic_w_Dff"


class AbsolutePathMapTest(unittest.TestCase):
    """Name-map F lines may hold absolute paths (the stored fixtures do)."""

    def copy_run(self, work: Path) -> Path:
        run = work / "run"
        (run / "odin3").mkdir(parents=True)
        for p in (FIXTURE / "odin3").iterdir():
            (run / "odin3" / p.name).write_bytes(p.read_bytes())
        return run

    def test_main_relativizes_absolute_sources(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run = self.copy_run(Path(tmp))
            names = next((run / "odin3").glob("*.names"))
            self.assertRegex(names.read_text(), r"\nF\tf1\t/")
            self.assertEqual(ps.main([str(run)]), 0)
            text = "".join(p.read_text() for p in (run / "prov").iterdir())
            self.assertNotIn("/tmp/", text)
            self.assertIn("bm_DL_logic_w_Dff.v", text)
            self.assertEqual(schema_check.check_dir(run / "prov", SCHEMAS), [])

    def test_main_reports_privacy_error(self) -> None:
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            run = self.copy_run(Path(tmp))
            with mock.patch.object(ps, "fpga_steps", side_effect=ps.PrivacyError("bad path")):
                self.assertEqual(ps.main([str(run)]), 2)


class LongChainTest(unittest.TestCase):
    """fpga_steps must not hold one dict per link of every object: a real CPU has objects whose
    chain is most of the design (a-z80-modified: 2515 objects x up to 30582 constructs)."""

    DEPTH = 1500
    OBJECTS = 300

    def synthetic_map(self) -> name_map_v2.NameMap:
        nm = name_map_v2.NameMap(version=2, width=8, ast="full")
        nm.files = ["a.v"]
        for n in range(1, self.DEPTH + 1):  # one chain: construct n has parent n - 1
            nm.constructs.append(name_map_v2.MapConstruct(
                n, "stmt", "stmt", n - 1, 1, f"{n}.1-{n}.9", f"s{n}", ""))
        nm.records.append(name_map_v2.MapRecord(
            1, "S", "stmt", "origin", "t1.1", (), ("f1:1.1-1.9",), (self.DEPTH,), (), ""))
        for k in range(self.OBJECTS):
            nm.objects.append(name_map_v2.MapObject(
                "N", "top", "net", f"n{k}", "", "", "", 0, (1,), "stmt", ""))
        return nm

    def test_long_chains_stream_and_match_the_dict_form(self) -> None:
        import tracemalloc
        from unittest import mock

        nm = self.synthetic_map()
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(name_map_v2, "read_name_map", return_value=nm):
            out = Path(tmp)
            (out / "t.blif.names").write_text("x\n")
            tracemalloc.start()
            ps.fpga_steps(out / "t.blif.names", out, "t", "t", "1.5.1", "t.blif")
            peak = tracemalloc.get_traced_memory()[1]
            tracemalloc.stop()
            # one dict per link would be 450000 dicts (over 100 MB); streaming stays far below
            self.assertLess(peak, 30_000_000)
            lines = (out / "prov" / "02-map.objects.jsonl").read_text().splitlines()
            self.assertEqual(len(lines), self.OBJECTS)
            row = json.loads(lines[0])
            self.assertEqual(len(row["links"]), self.DEPTH)
            self.assertEqual(row["links"][0], ps.link(f"hdl:c{self.DEPTH}", "fine"))
            self.assertEqual(lines[0], json.dumps(row, sort_keys=True, separators=(",", ":")))
            header = json.loads((out / "prov" / "02-map.json").read_text())
            self.assertEqual(header["counts"]["by_link_kind"], {"fine": self.DEPTH * self.OBJECTS})
            self.assertEqual(header["counts"]["objects"], self.OBJECTS)


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
class FpgaStepsTest(unittest.TestCase):
    def test_map_to_step_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            map_path = write_map(work, "ch_intrinsics", "blif", 2)
            blif = Path(str(map_path)[:-6])
            index = ps.fpga_steps(map_path, work, "ch_intrinsics", "ch_intrinsics", "1.4.0",
                                  blif.name)
            data = json.loads(index.read_text())
            self.assertEqual([s["stage"] for s in data["steps"]], ["hdl", "luts"])
            self.assertGreater(data["steps"][0]["count"], 0)
            self.assertGreater(data["steps"][1]["count"], 0)
            self.assertEqual(schema_check.check_dir(index.parent, SCHEMAS), [])
            text = (index.parent / data["steps"][1]["objects"]).read_text()
            rows = [json.loads(x) for x in text.splitlines()]
            linked = [r for r in rows if r["origin"] == "hdl"]
            self.assertTrue(linked)
            self.assertTrue(all(r["links"] and r["recs"] for r in linked))
            self.assertTrue(all(r["links"][0]["to"].startswith("hdl:c") for r in linked))

    def test_activity_attributes_on_luts_objects(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            map_path = write_map(work, "ch_intrinsics", "blif", 2)
            blif = Path(str(map_path)[:-6])
            plain = ps.fpga_steps(map_path, work, "t", "t", "1.6.0", blif.name)
            first = (plain.parent / "02-map.objects.jsonl").read_text().splitlines()
            rows = [json.loads(x) for x in first]
            self.assertTrue(all("act_p1" not in r["attrs"] for r in rows))
            name = rows[0]["name"]
            out = work / "act"
            out.mkdir()
            index = ps.fpga_steps(map_path, out, "t", "t", "1.6.0", blif.name,
                                  activity={name: (0.25, 0.125)})
            lines = (index.parent / "02-map.objects.jsonl").read_text().splitlines()
            got = [json.loads(x) for x in lines]
            hit = [r for r in got if r["name"] == name]
            self.assertTrue(hit)
            for row in hit:
                self.assertEqual(row["attrs"]["act_p1"], 0.25)
                self.assertEqual(row["attrs"]["act_density"], 0.125)
                self.assertEqual(row["attrs"]["act_source"], "odin3-vectorless")
            self.assertEqual(sum("act_p1" in r["attrs"] for r in got), len(hit))
            self.assertEqual(schema_check.check_dir(index.parent, SCHEMAS), [])

    def test_read_act(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.act"
            path.write_text("n1 0.5 0.2\n\nn2 0.25 0.125\n")
            self.assertEqual(ps.read_act(path), {"n1": (0.5, 0.2), "n2": (0.25, 0.125)})


if __name__ == "__main__":
    unittest.main()
