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

    def load(self, fmt: str, entry: str, env: dict[str, str] | None = None,
             options: dict[str, str] | None = None) -> pf.Result:
        return pf.load_project(self.dir, fmt, self.dir / entry, env, options)

    def record(self, fmt: str, entry: str, env: dict[str, str] | None = None,
               options: dict[str, str] | None = None) -> dict[str, Any]:
        """The parsed record (resolution errors after parsing do not matter here)."""
        res = self.load(fmt, entry, env, options)
        self.assertIsNotNone(res.record, str(res.error))
        assert res.record is not None
        return res.record

    def error(self, fmt: str, entry: str, env: dict[str, str] | None = None,
              options: dict[str, str] | None = None) -> pf.ProjectError:
        res = self.load(fmt, entry, env, options)
        self.assertIsNotNone(res.error, "expected an error")
        assert res.error is not None
        return res.error


MOD = "module {name} (input wire a, output wire y);\n{body}endmodule\n"


def module(name: str, *insts: str) -> str:
    body = "".join(f"    {i} u_{k} (.a(a), .y(y));\n" for k, i in enumerate(insts))
    return MOD.format(name=name, body=body or "    assign y = a;\n")


class WordsTest(unittest.TestCase):
    def test_words(self) -> None:
        self.assertEqual(pf.split_words('a "b c" "d\\"e\\\\" # x', "p:1"),
                         [("a", False), ("b c", True), ('d"e\\', True)])
        self.assertEqual(pf.split_words("a // b", "p:1", file_list=True), [("a", False)])
        self.assertEqual(pf.split_words("+define+D=#1 #c", "p:1"), [("+define+D=#1", False)])
        self.assertEqual(pf.split_words("C:\\rtl\\a.v", "p:1"), [("C:\\rtl\\a.v", False)])
        self.assertEqual(pf.split_words('+define+TAG="AB"', "p:1", file_list=True),
                         [('+define+TAG="AB"', False)])

    def test_word_errors(self) -> None:
        for line in ('"a', '""', '"a"b', 'a"b"', '"a\\n"', '"a"#c'):
            with self.subTest(line=line), self.assertRaises(pf.ProjectError) as cm:
                pf.split_words(line, "p:1")
            self.assertEqual(cm.exception.kind, "syntax")


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
file verilog src/top.v
libfile verilog lib/p.v
libdir cells
libext .v .sv
incdir inc
define A
define "B=x y"
top top
param top.W 3
param top.u_x.S "text"
param top.H 8'hFF
arch a.o3lib
device "Cyclone V" 5CSEMA5F31C6
map top.u_* $mul to multiply,mult_27 min_width 9 max_width 36
map * $mem keep
split $mem width depth 64
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
        self.assertEqual((len(rec["files"]), rec["duplicates"]), (2, ["src/top.v"]))
        self.assertEqual(rec["libfiles"], [{"path": "lib/p.v", "language": "verilog"}])
        self.assertEqual((rec["libdirs"], rec["libext"], rec["incdirs"]),
                         (["cells"], [".v", ".sv"], ["inc"]))
        self.assertEqual(rec["defines"], [{"name": "A", "value": None},
                                          {"name": "B", "value": "x y"}])
        self.assertEqual(rec["top"], "top")
        self.assertEqual(rec["params"], [
            {"scope": "top", "name": "W", "value": "3", "type": "number"},
            {"scope": "top.u_x", "name": "S", "value": "text", "type": "string"},
            {"scope": "top", "name": "H", "value": "8'hFF", "type": "number"}])
        self.assertEqual(rec["arch"][1], {"kind": "device", "family": "Cyclone V",
                                          "device": "5CSEMA5F31C6"})
        self.assertEqual(rec["rules"][0], {"rule": "map", "scope": "top.u_*", "subject": "$mul",
                                           "action": "to", "cells": ["multiply", "mult_27"],
                                           "min_width": 9, "max_width": 36})
        self.assertEqual(rec["rules"][2], {"rule": "split", "subject": "$mem", "width": True,
                                           "depth": "64"})
        self.assertEqual(rec["rules"][4], {"rule": "patterns", "path": "p.o3lib"})
        self.assertEqual(rec["overrides"][1]["params"], {"A": "27"})
        self.assertEqual(rec["flow"], "f.o3")

    def test_unknown_key_is_located(self) -> None:
        self.put("p.o3proj", "\n# x\nmaps * $mul soft\n")
        err = self.error("o3proj", "p.o3proj")
        self.assertEqual(err.as_json(), {"kind": "unknown_key", "at": "p.o3proj:3",
                                         "names": ["maps"]})

    def test_syntax_errors(self) -> None:
        for text in ("map * $mul to a b", "map * $mul soft min_width", "map * $mul soft x 3",
                     "map * $mul fast", "param W 3", "limit m lots", "top", "file cobol x.v",
                     'define "unterminated', "cell m from x param", "define 9X",
                     "top a\ntop b", "top t\nparam other.W 3", "param t.W 3",
                     "top t\nparam t.W wide", "split $mem", "split $mem depth deep",
                     "top a.b.c", 'file verilog "a"b'):
            with self.subTest(text=text):
                self.put("p.o3proj", text + "\n")
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

    def test_unknown_types(self) -> None:
        for text in ("libext .txt", "arch a.json", "import x.txt"):
            with self.subTest(text=text):
                self.put("p.o3proj", text + "\n")
                self.assertEqual(self.error("o3proj", "p.o3proj").kind, "unknown_file_type")

    def test_import_quartus(self) -> None:
        self.put("a.v", module("a"))
        self.put("p.qpf", 'PROJECT_REVISION = "r1"\nPROJECT_REVISION = "r2"\n')
        self.put("r2.qsf", "set_global_assignment -name VERILOG_FILE a.v\n"
                           "set_global_assignment -name TOP_LEVEL_ENTITY a\n")
        self.put("p.o3proj", "define X\nimport p.qpf revision r2\n")
        rec = self.record("o3proj", "p.o3proj")
        self.assertEqual((rec["top"], rec["files"][0]["path"]), ("a", "a.v"))
        self.put("p.o3proj", "import p.qpf\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").kind, "ambiguous_revision")
        self.put("p.o3proj", "top a\nimport p.qpf revision r2\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").kind, "syntax")


class FileListTest(_TmpCase):
    def test_plus_options_and_nesting(self) -> None:
        self.put("src/a.v", module("a"))
        self.put("src/sub/b.sv", "module b; endmodule\n")
        self.put("inc/.keep")
        self.put("src/sub/inner.f", "src/sub/b.sv // -f: relative to the outermost list\n")
        self.put("src/sub/own.F", "b.sv\n")
        self.put("files.f", "+define+X+Y=2 +incdir+inc\n# comment\n-f src/sub/inner.f\n"
                            "-F src/sub/own.F\nsrc/a.v\n+libext+.v+.sv\n")
        rec = self.record("f", "files.f")
        self.assertEqual([f["path"] for f in rec["files"]], ["src/sub/b.sv", "src/a.v"])
        self.assertEqual(rec["duplicates"], ["src/sub/b.sv"])
        self.assertEqual(rec["files"][0]["language"], "systemverilog")
        self.assertEqual(rec["defines"], [{"name": "X", "value": None},
                                          {"name": "Y", "value": "2"}])
        self.assertEqual((rec["incdirs"], rec["libext"]), (["inc"], [".v", ".sv"]))

    def test_top_sv_vars_and_crlf(self) -> None:
        self.put("rtl/a.v", module("a"))
        self.put("rtl/b.v", module("b", "a"))
        (self.dir / "files.f").write_bytes(b"-top b\r\n$D/a.v\r\n-sv\r\n${D}/b.v\r\n")
        rec = self.record("f", "files.f", env={"D": "rtl"})
        self.assertEqual(rec["top"], "b")
        self.assertEqual([f["language"] for f in rec["files"]], ["verilog", "systemverilog"])
        err = self.error("f", "files.f")
        self.assertEqual(err.as_json(), {"kind": "undefined_variable", "at": "files.f:2",
                                         "names": ["D"]})
        self.put("in.f", "-sv\nrtl/b.v\n")
        self.put("files.f", "-f in.f\nrtl/a.v\n")
        rec = self.record("f", "files.f")  # -sv ends with the list that says it
        self.assertEqual([f["language"] for f in rec["files"]], ["systemverilog", "verilog"])
        self.put("files.f", "--top-module a\n-top b\n")
        self.assertEqual(self.error("f", "files.f").kind, "syntax")

    def test_cycle(self) -> None:
        self.put("a.f", "-f b.f\n")
        self.put("b.f", "\n-f a.f\n")
        err = self.error("f", "a.f")
        self.assertEqual(err.as_json(), {"kind": "f_cycle", "at": "b.f:2",
                                         "names": ["a.f", "b.f", "a.f"]})

    def test_errors(self) -> None:
        self.put("x.txt")
        for text, kind in (("-bogus x\n", "unknown_option"), ("+foo+1\n", "unknown_option"),
                           ("-v\n", "syntax"), ("+incdir+\n", "syntax"),
                           ("x.txt\n", "unknown_file_type"), ("+libext+.txt\n",
                                                              "unknown_file_type"),
                           ("-y nodir\n", "missing_file"), ("$\n", "syntax")):
            with self.subTest(text=text):
                self.put("files.f", text)
                self.assertEqual(self.error("f", "files.f").kind, kind)


class TclTest(unittest.TestCase):
    def cmds(self, text: str, qip: bool = False) -> list[tuple[list[str], int]]:
        qip_dir = Path("/q") if qip else None
        return list(pf.TclReader(text, lambda n: f"q:{n}", qip_dir).commands())

    def test_words(self) -> None:
        text = ('# c\nset_a -name X "a \\"b\\""; set_b {x {y} z}\n'
                "set_c one \\\n  two\n  # a comment where a command may start\nset_d a\"b\n")
        self.assertEqual(self.cmds(text), [(["set_a", "-name", "X", 'a "b"'], 2),
                                           (["set_b", "x {y} z"], 2),
                                           (["set_c", "one", "two"], 3), (["set_d", 'a"b'], 6)])

    def test_qip_idiom(self) -> None:
        text = 'set -name F [file join $::quartus(qip_path) "x.v"]\nset [file join ' \
               "$::quartus(qip_path) y.v]\n"
        self.assertEqual([w[-1] for w, _ in self.cmds(text, qip=True)], ["/q/x.v", "/q/y.v"])

    def test_brackets_inside_words(self) -> None:
        text = 'set_location_assignment PIN_A -to LEDR[0]\nset -to "SW[1]" {x[2]}\n'
        self.assertEqual([w[-1] for w, _ in self.cmds(text)], ["LEDR[0]", "x[2]"])
        self.assertEqual(self.cmds(text)[1][0][2], "SW[1]")

    def test_errors(self) -> None:
        for text in ('a "x"y', "a {x}y", 'a "\\n"', "a $x", "a [b]", 'a "x', "a {x",
                     "a b\\c"):
            with self.subTest(text=text), self.assertRaises(pf.ProjectError) as cm:
                self.cmds(text)
            self.assertEqual(cm.exception.kind, "syntax")
        with self.assertRaises(pf.ProjectError):
            self.cmds('a [file join $::quartus(qip_path) "x.v"]', qip=False)


class QuartusTest(_TmpCase):
    def test_assignments(self) -> None:
        self.put("src/a.vhd", "entity r is end entity;\n")
        self.put("src/n.vqm", module("n"))
        self.put("lib1/.keep")
        self.put("lib2/.keep")
        self.put("p.qpf", 'QUARTUS_VERSION = "23.1"\n\nPROJECT_REVISION = "r"\n')
        self.put("r.qsf", """set_global_assignment -name VHDL_FILE src/a.vhd -library lib1
set_global_assignment -name vqm_file src/n.vqm
set_global_assignment -name TOP_LEVEL_ENTITY first
set_global_assignment -name TOP_LEVEL_ENTITY r
set_global_assignment -name SEARCH_PATH "lib1;lib2"
set_global_assignment -name USER_LIBRARIES lib2
set_parameter -name W 4
set_parameter -entity r -name D {"txt"}
set_parameter -to "r|sub:u_s" -name E 2
set_global_assignment -name AUTO_DSP_RECOGNITION off
set_global_assignment -name AUTO_RAM_RECOGNITION ON
set_instance_assignment -name RAMSTYLE LOGIC -to "|r|ram:u_m"
set_global_assignment -name MULTSTYLE LOGIC -entity sub
set_instance_assignment -name MULTSTYLE DSP -to u_x
set_location_assignment PIN_A1 -to a
set_location_assignment PIN_A2 -to b
source other.tcl
""")
        rec = self.record("qsf", "p.qpf")
        self.assertEqual(rec["top"], "r")
        self.assertEqual(rec["files"], [
            {"path": "src/a.vhd", "language": "vhdl", "library": "lib1"},
            {"path": "src/n.vqm", "language": "vqm", "library": "work"}])
        self.assertEqual((rec["incdirs"], rec["libdirs"]), (["lib1", "lib2"],
                                                           ["lib1", "lib2", "lib2"]))
        self.assertEqual(rec["params"], [
            {"scope": "r", "name": "W", "value": "4", "type": "number"},
            {"scope": "r", "name": "D", "value": "txt", "type": "string"},
            {"scope": "r.u_s", "name": "E", "value": "2", "type": "number"}])
        self.assertEqual([(r["scope"], r["subject"]) for r in rec["rules"]],
                         [("*", "$mul"), ("r.u_m", "$mem"), ("sub", "$mul")])
        self.assertEqual(rec["ignored"], ["LOCATION", "MULTSTYLE", "source"])

    def test_option_and_value_errors(self) -> None:
        self.put("p.qpf", "")
        for line, kind in (("set_global_assignment -name TOP_LEVEL_ENTITY t -to x",
                            "unknown_option"),
                           ("set_global_assignment -name X v -bogus 1", "unknown_option"),
                           ("set_global_assignment -name X a b", "syntax"),
                           ("set_global_assignment -name X", "syntax"),
                           ("set_global_assignment X", "syntax"),
                           ("set_parameter -entity other -name W 1", "syntax"),
                           ("set_parameter W 1", "syntax")):
            with self.subTest(line=line):
                self.put("p.qsf", line + "\n")
                self.assertEqual(self.error("qsf", "p.qpf").kind, kind)

    def test_revisions(self) -> None:
        self.put("p.qpf", '\nPROJECT_REVISION = "a"\nPROJECT_REVISION = "b"\n')
        self.put("a.qsf", "set_global_assignment -name TOP_LEVEL_ENTITY ta\n")
        self.put("b.qsf", "")
        self.assertEqual(self.error("qsf", "p.qpf").as_json(),
                         {"kind": "ambiguous_revision", "at": "p.qpf:2", "names": ["a", "b"]})
        self.assertEqual(self.record("qsf", "p.qpf", options={"revision": "b"})["top"], "b")
        self.assertEqual(self.error("qsf", "p.qpf", options={"revision": "c"}).as_json(),
                         {"kind": "unknown_revision", "at": None, "names": ["c"]})
        self.put("i.o3proj", "\nimport p.qpf revision c\n")
        self.assertEqual(self.error("o3proj", "i.o3proj").at, "i.o3proj:2")
        self.assertEqual(self.record("qsf", "a.qsf")["top"], "ta")  # a .qsf as the entry
        self.assertEqual(self.error("qsf", "a.qsf", options={"revision": "b"}).kind,
                         "unknown_revision")

    def test_qpf_without_revision_uses_its_name(self) -> None:
        self.put("proj.qpf", "# nothing\n")
        self.put("proj.qsf", "")
        self.assertEqual(self.record("qsf", "proj.qpf")["top"], "proj")

    def test_missing_revision_qsf_and_bad_qpf(self) -> None:
        self.put("p.qpf", '\nPROJECT_REVISION = "gone"\n')
        err = self.error("qsf", "p.qpf")
        self.assertEqual((err.kind, err.names), ("missing_file", ["gone.qsf"]))
        self.put("p.qpf", "PROJECT_REVISION r\n")
        self.assertEqual(self.error("qsf", "p.qpf").at, "p.qpf:1")

    def test_qip_nesting_and_cycle(self) -> None:
        self.put("ip/x.v", module("x"))
        self.put("ip/a.qip", "set_global_assignment -name QIP_FILE "
                             '[file join $::quartus(qip_path) "b.qip"]\n')
        self.put("ip/b.qip", "# plain paths: relative to the project\n"
                             "set_global_assignment -name VERILOG_FILE ip/x.v\n")
        self.put("t.qsf", "set_global_assignment -name QIP_FILE ip/a.qip\n")
        self.assertEqual(self.record("qsf", "t.qsf")["files"][0]["path"], "ip/x.v")
        self.put("ip/b.qip", "set_global_assignment -name QIP_FILE ip/a.qip\n")
        err = self.error("qsf", "t.qsf")
        self.assertEqual(err.as_json(), {"kind": "qip_cycle", "at": "ip/b.qip:1",
                                         "names": ["ip/a.qip", "ip/b.qip", "ip/a.qip"]})

    def test_anchored_scopes(self) -> None:
        self.assertEqual(pf.anchored(None, None, "t"), "*")
        self.assertEqual(pf.anchored("m", None, "t"), "m")
        self.assertEqual(pf.anchored(None, "t|mul:u_small", "t"), "t.u_small")
        self.assertEqual(pf.anchored(None, "u_x", "lib.t"), "t.u_x")
        self.assertEqual(pf.anchored("m", "a:b|c", "t"), "m.b.c")


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

    def test_optimizations(self) -> None:
        self.put("o.xml", '<config><optimizations><multiply size="9" fixed="1"/>'
                          '<adder size="2" threshold_size="4"/><memory split_memory_width="1"/>'
                          '<mix_soft_hard_blocks/></optimizations></config>')
        rec = self.record("odin2", "o.xml")
        self.assertEqual([(r["rule"], r.get("subject"), r.get("min_width")) for r in rec["rules"]],
                         [("map", "$mul", 9), ("map", "$add", 4), ("split", "$mem", None)])
        self.assertEqual(rec["ignored"], ["optimizations/adder@size",
                                          "optimizations/mix_soft_hard_blocks",
                                          "optimizations/multiply@fixed"])

    def test_errors(self) -> None:
        self.put("bad.xml", "<config>\n<inputs>\n</config>\n")
        self.assertEqual(self.error("odin2", "bad.xml").at, "bad.xml:3")
        self.put("root.xml", "<odin/>")
        self.assertEqual(self.error("odin2", "root.xml").kind, "syntax")
        self.put("type.xml", "<config><inputs>\n<input_type>systemverilog</input_type>"
                             "</inputs></config>")
        self.assertEqual(self.error("odin2", "type.xml").as_json(),
                         {"kind": "unsupported_input_type", "at": "type.xml:2",
                          "names": ["systemverilog"]})
        self.put("two.xml", "<config><inputs><input_type>blif</input_type>\n"
                            "<input_type>blif</input_type></inputs></config>")
        self.assertEqual(self.error("odin2", "two.xml").at, "two.xml:2")
        self.put("mem.xml", '<config><optimizations><memory split_memory_depth="deep"/>'
                            "</optimizations></config>")
        self.assertEqual(self.error("odin2", "mem.xml").kind, "syntax")


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
        self.assertEqual((res["top"], res["units"]), ("work.top", ["work.leaf", "work.top"]))

    def test_include_cycle(self) -> None:
        self.put("a.vh", '`include "b.vh"\n')
        self.put("b.vh", '\n`include "a.vh"\n')
        self.put("t.v", '`include "a.vh"\n' + module("t"))
        self.put("p.o3proj", "file verilog t.v\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").as_json(),
                         {"kind": "include_cycle", "at": "b.vh:2", "names": ["a.vh"]})

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
                         {"kind": "ambiguous_top", "at": None, "names": ["work.a", "work.b"]})
        self.put("p.o3proj", "file verilog a.v\ntop nope\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").as_json(),
                         {"kind": "unknown_top", "at": "p.o3proj:2", "names": ["nope"]})
        self.put("p.o3proj", "file verilog a.v library l1\nfile verilog a.v library l2\ntop a\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").as_json(),
                         {"kind": "ambiguous_top", "at": "p.o3proj:3", "names": ["l1.a", "l2.a"]})
        res = self.resolved("file verilog a.v library l1\nfile verilog a.v library l2\n"
                            "top l2.a\n")
        self.assertEqual(res["top"], "l2.a")
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

    def test_library_search_order(self) -> None:
        self.put("top.v", module("top", "leafcell", "other"))
        self.put("l1/leafcell.sv", module("leafcell"))
        self.put("l2/leafcell.v", "garbage that is never read")
        self.put("l2/other.v", module("other"))
        self.put("l1/unused.sv", module("unused"))
        res = self.resolved("file verilog top.v\nlibdir l1\nlibdir l2\nlibext .sv .v\n")
        self.assertEqual(res["read"], ["l1/leafcell.sv", "l2/other.v", "top.v"])
        self.put("t2.v", module("t2", "other"))
        self.put("l3/other.v", module("other"))
        res = self.resolved("file verilog t2.v\nlibdir l3\nlibdir l2\n")  # no libext: .v
        self.assertEqual(res["read"], ["l3/other.v", "t2.v"])  # the first directory wins

    def test_design_shadows_library(self) -> None:
        self.put("top.v", module("top", "cell1") + module("cell1"))
        self.put("lib/cell1.v", "not read")
        res = self.resolved("file verilog top.v\nlibdir lib\n")
        self.assertEqual(res["read"], ["top.v"])

    def test_vhdl_names_are_case_insensitive_across_languages(self) -> None:
        self.put("e.vhd", "-- c\nENTITY Leaf IS\nEND ENTITY Leaf;\n"
                          "architecture rtl of LEAF is begin end architecture;\n")
        self.put("top.v", module("top", "leaf"))
        res = self.resolved("file vhdl e.vhd\nfile verilog top.v\n")
        self.assertEqual(res["units"], ["work.leaf", "work.top"])

    def test_vhdl_library_qualified_entities_and_order(self) -> None:
        self.put("x.vhd", "entity core is end entity;\narchitecture a of core is begin end;\n")
        self.put("top.vhd", "use work.p.all;\nentity top is end entity;\n"
                            "architecture a of top is begin\n"
                            "  u1 : entity l1.core;\n  u2 : entity work.helper port map (x);\n"
                            "  u3 : component core;\nend architecture;\n")
        self.put("h.vhd", "entity helper is end entity helper;\n")
        self.put("p.vhd", "package p is end package;\n")
        res = self.resolved("file vhdl top.vhd\nfile vhdl x.vhd library l1\n"
                            "file vhdl x.vhd library l2\nfile vhdl h.vhd\nfile vhdl p.vhd\n")
        self.assertEqual(res["top"], "work.top")
        self.assertIn("l1.core", res["units"])
        self.assertEqual(res["order"], ["x.vhd", "x.vhd", "h.vhd", "p.vhd", "top.vhd"])

    def test_vhdl_secondary_units_in_other_files(self) -> None:
        self.put("arch.vhd", "architecture a of e is begin\n  u : entity work.leaf;\nend;\n")
        self.put("body.vhd", "package body p is end package body;\n")
        self.put("ent.vhd", "entity e is end entity;\n")
        self.put("pkg.vhd", "package p is end package;\n")
        self.put("leaf.vhd", "entity leaf is end entity;\n")
        res = self.resolved("file vhdl arch.vhd\nfile vhdl body.vhd\nfile vhdl ent.vhd\n"
                            "file vhdl pkg.vhd\nfile vhdl leaf.vhd\n")
        self.assertEqual(res["order"], ["ent.vhd", "pkg.vhd", "body.vhd", "leaf.vhd",
                                        "arch.vhd"])
        self.assertEqual(res["units"], ["work.e", "work.leaf"])  # arch.vhd's instance counts

    def test_vhdl_dependency_cycle(self) -> None:
        self.put("a.vhd", "use work.pb.all;\npackage pa is end package;\n")
        self.put("b.vhd", "use work.pa.all;\npackage pb is end package;\n")
        self.put("p.o3proj", "file vhdl a.vhd\nfile vhdl b.vhd\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").kind, "dependency_cycle")

    def test_blif_first_model_is_top(self) -> None:
        self.put("n.blif", ".model b\n.subckt a x=x\n.end\n.model a\n.end\n.model c\n.end\n")
        res = self.resolved("file blif n.blif\n")
        self.assertEqual(res["top"], "work.b")
        self.assertEqual(res["units"], ["work.a", "work.b"])

    def test_vqm_primitives_and_edif(self) -> None:
        self.put("n.vqm", module("n", "cyclonev_lcell_comb"))
        self.assertEqual(self.resolved("file vqm n.vqm\n")["units"], ["work.n"])
        self.put("n.vqm", module("n", "made_up_cell"))
        self.put("p.o3proj", "file vqm n.vqm\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").kind, "unresolved_module")
        self.put("r.v", module("r", "altsyncram", "lpm_mult"))  # megafunctions in RTL
        self.assertEqual(self.resolved("file verilog r.v\n")["units"], ["work.r"])
        self.put("n.edf", "(edif n)")
        self.put("p.o3proj", "file edif n.edf\n")
        self.assertEqual(self.error("o3proj", "p.o3proj").kind, "unsupported_language")

    def test_sv_package_function_is_not_an_instance(self) -> None:
        self.put("p.sv", "package p;\n  function automatic word_t f (input x);\n"
                         "  endfunction\nendpackage\n")
        self.put("t.sv", "module t;\n  word_t w [1:0];\n  p::word_t v;\nendmodule\n")
        res = self.resolved("file systemverilog p.sv\nfile systemverilog t.sv\n")
        self.assertEqual(res["units"], ["work.t"])


class RealQsfTest(_TmpCase):
    """A Quartus project from the wild (Yosys's DE2i-150 example, read only), if present."""

    def test_de2i_parses(self) -> None:
        src = helpers.REPO_ROOT.parent / "external/yosys/examples/intel/DE2i-150/quartus_compile"
        if not (src / "de2i.qsf").is_file():
            self.skipTest(f"{src} not present")
        (self.dir / "proj").mkdir()
        for name in ("de2i.qsf", "de2i.qpf"):
            shutil.copy(src / name, self.dir / "proj" / name)
        self.put("top.vqm", module("top"))  # the .qsf names ../top.vqm
        rec = self.record("qsf", "proj/de2i.qpf")
        self.assertEqual((rec["top"], rec["files"][0]["language"]), ("top", "vqm"))
        self.assertIn("LOCATION", rec["ignored"])
        self.assertEqual(rec["arch"][0]["device"], "EP4CGX150DF31C7")


class CorpusTest(unittest.TestCase):
    def test_committed_corpus_checks(self) -> None:
        code, out, err = helpers.run_main(pf.main, ["check", "--root", str(CORPUS)])
        self.assertEqual(code, 0, out + err)
        self.assertIn("elaborate in Phase 2:", out)
        self.assertIn("cases ok", out)

    def test_corpus_covers_every_format_and_error_kind(self) -> None:
        fmts: set[str] = set()
        kinds: set[str] = set()
        for exp_path in CORPUS.glob("*/expected.json"):
            exp = json.loads(exp_path.read_text())
            fmts |= set(exp["formats"])
            if "error" in exp:
                kinds.add(exp["error"]["kind"])
        self.assertEqual(fmts, set(pf.FORMATS))
        self.assertGreaterEqual(len(kinds), 14)


class CheckTest(unittest.TestCase):
    """The checker must notice a corpus that drifted."""

    CASES = ("multi_dir", "neg_missing_file", "lib_v_y", "nested_f")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        for name in self.CASES:
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
        for case in self.CASES:
            code, out = self.check(case)
            self.assertEqual(code, 0, out)

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

    def test_unused_files_and_oracle_files(self) -> None:
        (self.root / "multi_dir" / "src" / "stray.v").write_text("")
        (self.root / "multi_dir" / "stray.f").write_text("")
        (self.root / "multi_dir" / "ref.blif").unlink()
        code, out = self.check("multi_dir")
        self.assertEqual(code, 1)
        self.assertIn("src/stray.v is not used", out)
        self.assertIn("stray.f is not used", out)  # an unreferenced project file too
        self.assertIn("ref.blif missing", out)

    def test_unused_list_is_checked(self) -> None:
        self.edit("lib_v_y", {"unused": ["lib/cells/and2.v"]})
        out = self.check("lib_v_y")[1]
        self.assertIn("lib/cells/or2.v is not used", out)
        self.assertIn("listed as unused but a format used it", out)

    def test_unreferenced_nested_list(self) -> None:
        f = self.root / "nested_f" / "files.f"
        f.write_text(f.read_text().replace("-f src/core/core.f\n", "") +
                     "src/common/dffr.v\nsrc/core/alu.v\nsrc/core/regs.v\n+incdir+src/include\n")
        self.assertIn("src/core/core.f is not used", self.check("nested_f")[1])

    def test_stray_project_file(self) -> None:
        (self.root / "lib_v_y" / "odin2.xml").write_text("<config/>")
        self.assertIn("odin2 is not in formats", self.check("lib_v_y")[1])

    def test_schema(self) -> None:
        self.edit("multi_dir", {"formats": ["o3proj", "verilator"], "extra": 1, "phase": 3,
                                "format_overrides": {"o3proj": {"top": "x"}}})
        out = self.check("multi_dir")[1]
        self.assertIn("unknown key 'extra'", out)
        self.assertIn("bad formats", out)
        self.assertIn("phase must be", out)
        self.assertIn("format_overrides for 'o3proj'", out)

    def test_format_phase_required_for_odin2(self) -> None:
        self.edit("multi_dir", {"format_phase": {}})
        self.assertIn("format_phase", self.check("multi_dir")[1])

    def test_ref_cmd_header(self) -> None:
        cmd = self.root / "multi_dir" / "ref.cmd"
        cmd.write_text("\n".join(ln for ln in cmd.read_text().splitlines()
                                 if not ln.startswith("# versions:")) + "\n")
        self.assertIn("'# versions:' line", self.check("multi_dir")[1])

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
