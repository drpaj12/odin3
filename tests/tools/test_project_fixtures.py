"""Tests for tools/project-fixtures: the reference project parsers, the source scan and the
corpus check (which is also how CI checks tests/golden/projects)."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any

import project_fixtures as pf

from . import helpers

CORPUS = helpers.REPO_ROOT / "tests" / "golden" / "projects"


class _TmpCase(unittest.TestCase):
    """A scratch case directory with helpers to write files and parse a format."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def put(self, rel: str, text: str = "") -> Path:
        path = self.dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def load(self, fmt: str, entry: str) -> pf.Result:
        return pf.load_project(self.dir, fmt, self.dir / entry)

    def record(self, fmt: str, entry: str) -> dict[str, Any]:
        """The parsed record (resolution errors after parsing do not matter here)."""
        res = self.load(fmt, entry)
        self.assertIsNotNone(res.record, str(res.error))
        assert res.record is not None
        return res.record

    def error(self, fmt: str, entry: str) -> pf.ProjectError:
        res = self.load(fmt, entry)
        self.assertIsNotNone(res.error, "expected an error")
        assert res.error is not None
        return res.error


MOD = "module {name} (input wire a, output wire y);\n{body}endmodule\n"


def module(name: str, *insts: str) -> str:
    body = "".join(f"    {i} u_{k} (.a(a), .y(y));\n" for k, i in enumerate(insts))
    return MOD.format(name=name, body=body or "    assign y = a;\n")


class O3projTest(_TmpCase):
    def test_every_key(self) -> None:
        self.put("src/top.v", module("top"))
        self.put("inc/x.vh")
        self.put("lib/p.v", module("p"))
        self.put("cells/.keep")
        self.put("a.o3lib")
        self.put("p.o3lib")
        self.put("f.o3")
        self.put("p.o3proj", """# comment
file verilog src/top.v   # trailing comment
file vhdl "src/top.v" library mylib
libfile verilog lib/p.v
libdir cells
libext .v .sv
incdir inc
define A
define "B=x y"
top top
param top.W 3
arch a.o3lib
device "Cyclone V" 5CSEMA5F31C6
map top.u_* $mul to multiply,mult_27 min_width 9 max_width 36
map * $mem keep
limit multiply 40
available multiply 112
hide dual_port_ram
cell m27 from multiply param A=27
patterns p.o3lib
flow f.o3
""")
        rec = self.record("o3proj", "p.o3proj")
        self.assertEqual(rec["files"][1], {"path": "src/top.v", "language": "vhdl",
                                           "library": "mylib"})
        self.assertEqual(rec["libfiles"], [{"path": "lib/p.v", "language": "verilog"}])
        self.assertEqual((rec["libdirs"], rec["libext"], rec["incdirs"]),
                         (["cells"], [".v", ".sv"], ["inc"]))
        self.assertEqual(rec["defines"], [{"name": "A", "value": None},
                                          {"name": "B", "value": "x y"}])
        self.assertEqual(rec["params"], [{"module": "top", "name": "W", "value": "3"}])
        self.assertEqual(rec["arch"][1], {"kind": "device", "family": "Cyclone V",
                                          "device": "5CSEMA5F31C6"})
        self.assertEqual(rec["rules"][0], {"rule": "map", "scope": "top.u_*", "subject": "$mul",
                                           "action": "to", "cells": ["multiply", "mult_27"],
                                           "min_width": 9, "max_width": 36})
        self.assertEqual(rec["rules"][1]["action"], "keep")
        self.assertEqual(rec["rules"][3], {"rule": "patterns", "path": "p.o3lib"})
        self.assertEqual(rec["overrides"][1]["params"], {"A": "27"})
        self.assertEqual(rec["flow"], "f.o3")

    def test_unknown_key_is_located(self) -> None:
        self.put("p.o3proj", "\n# x\nmaps * $mul soft\n")
        err = self.error("o3proj", "p.o3proj")
        self.assertEqual(err.as_json(), {"kind": "unknown_key", "at": "p.o3proj:3",
                                         "names": ["maps"]})

    def test_syntax_errors(self) -> None:
        for line in ("map * $mul to a b", "map * $mul soft min_width", "map * $mul soft x 3",
                     "map * $mul fast", "param W 3", "limit m lots", "top", "file cobol x.v",
                     'define "unterminated', "cell m from x param", "define 9X"):
            with self.subTest(line=line):
                self.put("p.o3proj", line + "\n")
                self.assertEqual(self.error("o3proj", "p.o3proj").kind, "syntax")

    def test_paths_resolve_against_the_project_file(self) -> None:
        self.put("src/top.v", module("top"))
        self.put("proj/p.o3proj", "file verilog ../src/top.v\n")
        rec = self.record("o3proj", "proj/p.o3proj")
        self.assertEqual(rec["files"][0]["path"], "src/top.v")

    def test_missing_file_and_dir(self) -> None:
        self.put("p.o3proj", "incdir nope\n")
        err = self.error("o3proj", "p.o3proj")
        self.assertEqual((err.kind, err.at, err.names), ("missing_file", "p.o3proj:1", ["nope"]))


class FileListTest(_TmpCase):
    def test_plus_options_and_nesting(self) -> None:
        self.put("src/a.v", module("a"))
        self.put("src/sub/b.sv", "module b; endmodule\n")
        self.put("inc/.keep")
        self.put("src/sub/inner.f", "b.sv // comment\n")
        self.put("files.f", "+define+X+Y=2 +incdir+inc\n# comment\n-f src/sub/inner.f\n"
                            "src/a.v\n+libext+.v+.sv\n")
        rec = self.record("f", "files.f")
        self.assertEqual([f["path"] for f in rec["files"]], ["src/sub/b.sv", "src/a.v"])
        self.assertEqual(rec["files"][0]["language"], "systemverilog")
        self.assertEqual(rec["defines"], [{"name": "X", "value": None},
                                          {"name": "Y", "value": "2"}])
        self.assertEqual((rec["incdirs"], rec["libext"]), (["inc"], [".v", ".sv"]))

    def test_cycle(self) -> None:
        self.put("a.f", "-f b.f\n")
        self.put("b.f", "\n-F a.f\n")
        err = self.error("f", "a.f")
        self.assertEqual(err.as_json(), {"kind": "f_cycle", "at": "b.f:2",
                                         "names": ["a.f", "b.f", "a.f"]})

    def test_errors(self) -> None:
        self.put("x.txt")
        for text, kind in (("-top x\n", "unknown_option"), ("+foo+1\n", "unknown_option"),
                           ("-v\n", "syntax"), ("+incdir+\n", "syntax"),
                           ("x.txt\n", "unknown_file_type"), ("-y nodir\n", "missing_file")):
            with self.subTest(text=text):
                self.put("files.f", text)
                self.assertEqual(self.error("f", "files.f").kind, kind)


class QuartusTest(_TmpCase):
    def test_tcl_words(self) -> None:
        text = ('# c\nset_a -name X "a \\"b\\""; set_b {x {y} z}\n'
                "set_c one \\\n  two\n  # not a comment start? yes it is\nset_d\n")
        cmds = list(pf.TclReader(text).commands())
        self.assertEqual(cmds, [(["set_a", "-name", "X", 'a "b"'], 2),
                                (["set_b", "x {y} z"], 2), (["set_c", "one", "two"], 3),
                                (["set_d"], 6)])

    def test_unterminated_quote(self) -> None:
        self.put("p.qpf", 'PROJECT_REVISION = "r"\n')
        self.put("r.qsf", 'set_global_assignment -name X "oops\n')
        self.assertEqual(self.error("qsf", "p.qpf").kind, "syntax")

    def test_revision_default_top_params_and_rules(self) -> None:
        self.put("src/a.vhd", "entity r is end entity;\n")
        self.put("p.qpf", 'QUARTUS_VERSION = "23.1"\n\nPROJECT_REVISION = "r"\n'
                          'PROJECT_REVISION = "other"\n')
        self.put("r.qsf", """set_global_assignment -name VHDL_FILE src/a.vhd -library lib1
set_parameter -name W 4
set_parameter -entity sub -name D 2
set_global_assignment -name AUTO_DSP_RECOGNITION OFF
set_global_assignment -name AUTO_RAM_RECOGNITION ON
set_instance_assignment -name RAMSTYLE LOGIC -to "|r|ram:u_m"
set_instance_assignment -name MULTSTYLE DSP -to u_x
set_location_assignment PIN_A1 -to a
source other.tcl
""")
        rec = self.record("qsf", "p.qpf")
        self.assertEqual(rec["top"], ["r"])
        self.assertEqual(rec["files"], [{"path": "src/a.vhd", "language": "vhdl",
                                         "library": "lib1"}])
        self.assertEqual(rec["params"], [{"module": "r", "name": "W", "value": "4"},
                                         {"module": "sub", "name": "D", "value": "2"}])
        self.assertEqual([(r["scope"], r["subject"]) for r in rec["rules"]],
                         [("*", "$mul"), ("r.u_m", "$mem")])
        self.assertEqual(rec["ignored"], ["LOCATION", "MULTSTYLE", "source"])

    def test_qpf_without_revision_uses_its_name(self) -> None:
        self.put("proj.qpf", "# nothing\n")
        self.put("proj.qsf", "set_global_assignment -name TOP_LEVEL_ENTITY t\n")
        self.assertEqual(self.record("qsf", "proj.qpf")["top"], ["t"])

    def test_missing_revision_qsf(self) -> None:
        self.put("p.qpf", '\nPROJECT_REVISION = "gone"\n')
        err = self.error("qsf", "p.qpf")
        self.assertEqual((err.kind, err.at, err.names), ("missing_file", "p.qpf:2", ["gone.qsf"]))

    def test_bad_qpf_line(self) -> None:
        self.put("p.qpf", "PROJECT_REVISION r\n")
        self.assertEqual(self.error("qsf", "p.qpf").at, "p.qpf:1")

    def test_quartus_scope(self) -> None:
        self.assertEqual(pf.quartus_scope(None), "*")
        self.assertEqual(pf.quartus_scope("top|mul:u_small"), "top.u_small")
        self.assertEqual(pf.quartus_scope("|a:b|c"), "b.c")


class Odin2Test(_TmpCase):
    def test_legacy_and_inputs_forms(self) -> None:
        self.put("a.v", module("a"))
        self.put("n.blif", ".model n\n.inputs x\n.outputs y\n.names x y\n1 1\n.end\n")
        self.put("arch.xml", "<architecture/>")
        self.put("legacy.xml", "<config><verilog_files><verilog_file> a.v </verilog_file>"
                               "</verilog_files><output><target><arch_file>arch.xml</arch_file>"
                               "</target></output><debug_outputs/></config>")
        rec = self.record("odin2", "legacy.xml")
        self.assertEqual(rec["files"][0]["path"], "a.v")
        self.assertEqual(rec["arch"], [{"kind": "vpr_xml", "path": "arch.xml"}])
        self.assertEqual((rec["output"], rec["ignored"]), (None, ["debug_outputs"]))
        self.put("inputs.xml", "<config><inputs><input_path_and_name>n.blif"
                               "</input_path_and_name><input_type>BLIF</input_type></inputs>"
                               "<output><output_path_and_name>o/x.blif</output_path_and_name>"
                               "</output></config>")
        rec = self.record("odin2", "inputs.xml")
        self.assertEqual(rec["files"], [{"path": "n.blif", "language": "blif",
                                         "library": "work"}])
        self.assertEqual(rec["output"], {"path": "o/x.blif", "format": None})

    def test_errors(self) -> None:
        self.put("bad.xml", "<config>\n<inputs>\n</config>\n")
        self.assertEqual(self.error("odin2", "bad.xml").at, "bad.xml:3")
        self.put("root.xml", "<odin/>")
        self.assertEqual(self.error("odin2", "root.xml").kind, "syntax")
        self.put("type.xml", "<config><inputs>\n<input_type>vhdl</input_type></inputs></config>")
        self.assertEqual(self.error("odin2", "type.xml").at, "type.xml:2")


class ResolveTest(_TmpCase):
    def resolved(self, project: str) -> dict[str, Any]:
        self.put("p.o3proj", project)
        res = self.load("o3proj", "p.o3proj")
        self.assertIsNone(res.error, str(res.error))
        assert res.resolved is not None
        return res.resolved

    def test_include_search_order_and_ifdef(self) -> None:
        self.put("src/x.vh", "`define FROM_SRC\n")
        self.put("inc/x.vh", "`define FROM_INC\n")
        self.put("src/top.v", '`include "x.vh"\n`ifdef FROM_SRC\n' + module("top", "leaf")
                 + "`else\n" + module("top", "other") + "`endif\n")
        self.put("src/leaf.v", "/* block\n comment */ " + module("leaf"))
        self.put("src/other.v", module("other"))
        res = self.resolved("incdir inc\nfile verilog src/top.v\nfile verilog src/leaf.v\n")
        self.assertEqual(res["read"], ["src/leaf.v", "src/top.v", "src/x.vh"])
        self.assertEqual(res["units"], ["work.leaf", "work.top"])

    def test_global_define_selects_branch(self) -> None:
        self.put("top.v", "`ifndef ALT\n" + module("top", "a") + "`elsif ALT\n"
                 + module("top", "b") + "`endif\n")
        self.put("a.v", module("a"))
        self.put("b.v", module("b"))
        res = self.resolved("define ALT=1\nfile verilog top.v\nfile verilog a.v\n"
                            "file verilog b.v\ntop top\n")
        self.assertEqual(res["units"], ["work.b", "work.top"])

    def test_top_errors(self) -> None:
        self.put("a.v", module("a"))
        self.put("b.v", module("b"))
        self.put("p.o3proj", "file verilog a.v\nfile verilog b.v\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").as_json(),
                         {"kind": "ambiguous_top", "at": None, "names": ["a", "b"]})
        self.put("p.o3proj", "file verilog a.v\ntop nope\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").kind, "unknown_top")
        self.put("c.v", module("c", "d"))
        self.put("p.o3proj", "file verilog c.v\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").as_json(),
                         {"kind": "unresolved_module", "at": "c.v:2", "names": ["d"]})

    def test_duplicate_is_located_at_the_second(self) -> None:
        self.put("a.v", module("m"))
        self.put("b.v", "\n" + module("m"))
        self.put("p.o3proj", "file verilog a.v\nfile verilog b.v\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").as_json(),
                         {"kind": "duplicate_module", "at": "b.v:2", "names": ["m"]})

    def test_libdir_uses_libext_on_demand(self) -> None:
        self.put("top.v", module("top", "leafcell"))
        self.put("lib/leafcell.sv", module("leafcell"))
        self.put("lib/leafcell", "not verilog")
        self.put("lib/unused.sv", module("unused"))
        res = self.resolved("file verilog top.v\nlibdir lib\nlibext .v .sv\n")
        self.assertEqual(res["read"], ["lib/leafcell.sv", "top.v"])

    def test_vhdl_names_are_case_insensitive_across_languages(self) -> None:
        self.put("e.vhd", "-- c\nENTITY Leaf IS\nEND ENTITY Leaf;\n"
                          "architecture rtl of LEAF is begin end architecture;\n")
        self.put("top.v", module("top", "leaf"))
        res = self.resolved("file vhdl e.vhd\nfile verilog top.v\n")
        self.assertEqual(res["units"], ["work.leaf", "work.top"])

    def test_vhdl_library_qualified_entities(self) -> None:
        self.put("x.vhd", "entity core is end entity;\narchitecture a of core is begin end;\n")
        self.put("top.vhd", "entity top is end entity;\narchitecture a of top is begin\n"
                            "  u1 : entity l1.core;\n  u2 : entity work.helper port map (x);\n"
                            "  u3 : component core;\nend architecture;\n")
        self.put("h.vhd", "entity helper is end entity helper;\n")
        res = self.resolved("file vhdl x.vhd library l1\nfile vhdl x.vhd library l2\n"
                            "file vhdl h.vhd\nfile vhdl top.vhd\n")
        self.assertEqual(res["top"], ["top"])
        self.assertIn("l1.core", res["units"])
        self.assertIn("work.helper", res["units"])

    def test_blif_first_model_is_top(self) -> None:
        self.put("n.blif", ".model b\n.subckt a x=x\n.end\n.model a\n.end\n.model c\n.end\n")
        res = self.resolved("file blif n.blif\n")
        self.assertEqual(res["top"], ["b"])
        self.assertEqual(res["units"], ["work.a", "work.b"])

    def test_sv_package_function_is_not_an_instance(self) -> None:
        self.put("p.sv", "package p;\n  function automatic word_t f (input x);\n"
                         "  endfunction\nendpackage\n")
        self.put("t.sv", "module t;\n  word_t w [1:0];\n  p::word_t v;\nendmodule\n")
        res = self.resolved("file systemverilog p.sv\nfile systemverilog t.sv\n")
        self.assertEqual(res["units"], ["work.t"])


class CorpusTest(unittest.TestCase):
    def test_committed_corpus_checks(self) -> None:
        code, out, err = helpers.run_main(pf.main, ["check", "--root", str(CORPUS)])
        self.assertEqual(code, 0, out + err)
        self.assertIn("cases ok", out)

    def test_corpus_covers_every_format(self) -> None:
        seen: set[str] = set()
        for exp_path in CORPUS.glob("*/expected.json"):
            seen |= set(json.loads(exp_path.read_text())["formats"])
        self.assertEqual(seen, set(pf.FORMATS))


class CheckTest(unittest.TestCase):
    """The checker must notice a corpus that drifted."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        for name in ("multi_dir", "neg_missing_file", "lib_v_y"):
            shutil.copytree(CORPUS / name, self.root / name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def check(self, case: str) -> tuple[int, str]:
        code, out, err = helpers.run_main(pf.main, ["check", "--root", str(self.root),
                                                    "--case", case])
        return code, out + err

    def edit(self, case: str, change: dict[str, Any]) -> None:
        path = self.root / case / "expected.json"
        exp = json.loads(path.read_text())
        exp.update(change)
        path.write_text(json.dumps(exp))

    def test_copies_pass(self) -> None:
        for case in ("multi_dir", "neg_missing_file", "lib_v_y"):
            self.assertEqual(self.check(case)[0], 0, self.check(case)[1])

    def test_record_mismatch(self) -> None:
        exp = json.loads((self.root / "multi_dir" / "expected.json").read_text())
        exp["project"]["files"].reverse()
        self.edit("multi_dir", {"project": exp["project"]})
        code, out = self.check("multi_dir")
        self.assertEqual(code, 1)
        self.assertIn("record files", out)

    def test_error_mismatch(self) -> None:
        self.edit("neg_missing_file", {"error": {"kind": "missing_file", "at": "x:1",
                                                 "names": ["src/missing.v"]}})
        self.assertIn("expected error", self.check("neg_missing_file")[1])

    def test_unused_file_and_missing_oracle(self) -> None:
        (self.root / "multi_dir" / "src" / "stray.v").write_text("")
        (self.root / "multi_dir" / "ref.blif").unlink()
        code, out = self.check("multi_dir")
        self.assertEqual(code, 1)
        self.assertIn("src/stray.v is not used", out)
        self.assertIn("ref.blif missing", out)

    def test_unused_list_is_checked(self) -> None:
        self.edit("lib_v_y", {"unused": ["lib/cells/and2.v"]})
        out = self.check("lib_v_y")[1]
        self.assertIn("lib/cells/or2.v is not used", out)
        self.assertIn("listed as unused but a format used it", out)

    def test_stray_project_file(self) -> None:
        (self.root / "lib_v_y" / "odin2.xml").write_text("<config/>")
        self.assertIn("odin2 is not in formats", self.check("lib_v_y")[1])

    def test_schema(self) -> None:
        self.edit("multi_dir", {"formats": ["o3proj", "verilator"], "extra": 1})
        out = self.check("multi_dir")[1]
        self.assertIn("unknown key 'extra'", out)
        self.assertIn("bad formats", out)

    def test_usage_errors(self) -> None:
        self.assertEqual(self.check("nope")[0], 2)
        code, _, err = helpers.run_main(pf.main, ["check", "--root", str(self.root / "x")])
        self.assertEqual(code, 2)
        self.assertIn("not a directory", err)

    def test_parse_command(self) -> None:
        code, out, _ = helpers.run_main(pf.main, ["parse", str(self.root / "neg_missing_file"),
                                                  "f"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["error"]["at"], "files.f:2")


if __name__ == "__main__":
    unittest.main()
