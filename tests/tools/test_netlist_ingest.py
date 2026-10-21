"""Tests for tools/netlist-ingest (Phase 6.1): the name extraction from a Verilog netlist and a
BLIF, and the gates failing on a planted difference (a NAND2 turned into a NOR2 in a copy of the
fixtures). The planted-difference tests need ``ODIN3_CLI`` (CTest python_tools)."""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import shutil
import unittest
from collections.abc import Callable, Iterator
from pathlib import Path
from unittest import mock

import netlist_ingest as ni

from .helpers import REPO_ROOT, scratch

ODIN3 = os.environ.get("ODIN3_CLI")
VERILOG = """module m(a, y);
  wire _00_;
  wire [1:0] \\uut.x ;
  input a;
  output [1:0] y;
  NAND2 _05_ (
    .A(a),
    .Y(_00_)
  );
  assign y = _00_;
  OTHER _06_ (
endmodule
"""
BLIF = """# c
.model top
.inputs a[0] b
.outputs y
.subckt NAND2 A=a[0] B=b Y=n1
.names n1 y
1 1
.latch y q re clk 3
.end
.model other
.inputs zz
.end
"""


class NameTests(unittest.TestCase):
    def test_verilog_names(self) -> None:
        tmp = scratch(self, "ni-")
        (tmp / "m.v").write_text(VERILOG)
        with mock.patch.object(ni, "ROOT", tmp):
            wires, instances, types = ni.verilog_names(Path("m.v"))
        self.assertEqual(wires, {"_00_", "uut.x", "a", "y"})
        self.assertEqual(instances, {"_05_"})  # OTHER is not a cell of cmos.v
        self.assertEqual(types, {"NAND2"})

    def test_blif_nets_first_model_only(self) -> None:
        tmp = scratch(self, "ni-")
        (tmp / "m.blif").write_text(BLIF)
        with mock.patch.object(ni, "ROOT", tmp):
            nets = ni.blif_nets(Path("m.blif"))
        self.assertEqual(nets, {"a[0]", "b", "y", "n1", "q", "clk"})

    def test_unescape(self) -> None:
        self.assertEqual(ni.unescape("\\a.b"), "a.b")
        self.assertEqual(ni.unescape("a"), "a")

    def test_missing(self) -> None:
        rows = [("t", "wire", "a", ""), ("t", "net", "b", ""), ("u", "wire", "c", "")]
        self.assertEqual(ni.missing({"a", "b", "c"}, rows, "t", ("wire",)), ["b", "c"])


Rows = list[tuple[str, str, str, str]]


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
class PlantedTests(unittest.TestCase):
    def copy_fixtures(self) -> Path:
        tmp = scratch(self, "ni-")
        shutil.copytree(REPO_ROOT / "tests/netlist", tmp / "tests/netlist")
        return tmp

    def args(self, **extra: str) -> argparse.Namespace:
        equiv = str(REPO_ROOT / "tools/equiv-check/equiv-check")
        return argparse.Namespace(odin3=ODIN3, equiv_check=equiv, **extra)

    def run_check(self, check: Callable[[argparse.Namespace], int], root: Path | None = None,
                  **extra: str) -> tuple[int, str]:
        """(exit code, output) of a check, with ROOT optionally a fixture copy."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            if root is None:
                code = check(self.args(**extra))
            else:
                with mock.patch.object(ni, "ROOT", root):
                    code = check(self.args(**extra))
        return code, out.getvalue()

    @contextlib.contextmanager
    def edit_dump(self, keep: Callable[[str, Rows], Rows]) -> Iterator[None]:
        """Patch dump_lines: ``keep(read script, rows)`` gives the rows the check sees."""
        real = ni.dump_lines

        def planted(binary: str, read: str) -> tuple[Rows, str]:
            rows, why = real(binary, read)
            return keep(read, rows), why

        with mock.patch.object(ni, "dump_lines", planted):
            yield

    def plant_equiv(self, name: str, old: str, new: str, form: str) -> tuple[int, str]:
        if not os.environ.get("ODIN3_ABC"):
            self.skipTest("ODIN3_ABC not set (the equivalence leg maps to LUTs with ABC)")
        tmp = self.copy_fixtures()
        path = tmp / "tests/netlist/cone" / name
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1))
        return self.run_check(ni.check_equiv, tmp, micro="cone", form=form)

    def test_equivalent_fixture_passes(self) -> None:
        if not os.environ.get("ODIN3_ABC"):
            self.skipTest("ODIN3_ABC not set (the equivalence leg maps to LUTs with ABC)")
        for form in ni.FORMS:
            code, text = self.run_check(ni.check_equiv, self.copy_fixtures(), micro="cone",
                                        form=form)
            self.assertEqual(code, 0, text)
            self.assertNotIn("NOT equivalent", text)

    def test_planted_difference_fails_verilog(self) -> None:
        code, text = self.plant_equiv("mapped.v", "NAND2 ", "NOR2 ", "verilog")
        self.assertEqual(code, 1, text)
        self.assertIn("NOT equivalent", text)

    def test_planted_difference_fails_blif(self) -> None:
        code, text = self.plant_equiv("mapped.blif", "$_NAND_", "$_NOR_", "blif")
        self.assertEqual(code, 1, text)
        self.assertIn("NOT equivalent", text)

    def test_missing_name_fails(self) -> None:
        with self.edit_dump(lambda _, rows: [r for r in rows if r[2:3] != ("a",)
                                             or r[1] != "wire"]):
            code, text = self.run_check(ni.check_names, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("FAIL missing wire a", text)

    def test_missing_blif_net_fails(self) -> None:
        def drop(read: str, rows: Rows) -> Rows:
            if "read_blif" not in read:
                return rows
            return [r for r in rows if r[1] not in ("net", "wire") or r[2] != "a"]

        with self.edit_dump(drop):
            code, text = self.run_check(ni.check_names, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("FAIL missing blif net", text)

    def test_names_fail_when_nothing_matches(self) -> None:
        tmp = self.copy_fixtures()
        (tmp / "tests/netlist/cone/mapped.v").write_text("module cone;\nendmodule\n")
        code, text = self.run_check(ni.check_names, tmp, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("no wires found", text)

    def test_missing_cell_module_fails(self) -> None:
        with self.edit_dump(lambda _, rows: [r for r in rows
                                             if not (r[1] == "module" and r[2] == "NAND2")]):
            code, text = self.run_check(ni.check_names, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("FAIL missing cell module NAND2", text)

    def test_prov_missing_location_fails(self) -> None:
        def strip(read: str, rows: Rows) -> Rows:
            if "read_blif" in read:
                return rows
            return [(m, k, n, "" if k == "node" else loc) for m, k, n, loc in rows]

        with self.edit_dump(strip):
            code, text = self.run_check(ni.check_prov, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("no location in", text)

    def test_prov_blif_location_fails(self) -> None:
        def locate(read: str, rows: Rows) -> Rows:
            if "read_blif" not in read:
                return rows
            return [(m, k, n, "rtl.v:1.1-2.2") for m, k, n, _ in rows]

        with self.edit_dump(locate):
            code, text = self.run_check(ni.check_prov, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("has a location", text)

    def test_prov_fails_when_nothing_to_check(self) -> None:
        with self.edit_dump(lambda _, rows: [r for r in rows if r[1] == "module"]):
            code, text = self.run_check(ni.check_prov, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("vacuously", text)

    def test_src_unrestored_object_fails(self) -> None:
        dropped: Rows = []

        def lose_one(_: str, rows: Rows) -> Rows:
            for row in rows:
                if row[1] == "wire" and row[3].startswith("rtl.v:") and not dropped:
                    dropped.append(row)
            return [r for r in rows if r not in dropped]

        with self.edit_dump(lose_one):
            code, text = self.run_check(ni.check_src, micro="cone")
        self.assertEqual(code, 1, text)
        self.assertIn("src attributes exist", text)

    def test_src_passes_on_the_fixture(self) -> None:
        code, text = self.run_check(ni.check_src, micro="cone")
        self.assertEqual(code, 0, text)

    def test_unknown_cell_accepted_fails(self) -> None:
        with mock.patch.object(ni, "odin3", lambda *_: (0, "")):
            code, text = self.run_check(ni.check_unknown)
        self.assertEqual(code, 1, text)
        self.assertIn("expected a located unresolved_module error", text)

    def test_unknown_blackbox_refused_fails(self) -> None:
        real = ni.odin3

        def refuse(binary: str, script: str) -> tuple[int, str]:
            if "--auto-blackbox" in script:
                return 1, "odin3: error: refused"
            return real(binary, script)

        with mock.patch.object(ni, "odin3", refuse):
            code, text = self.run_check(ni.check_unknown)
        self.assertEqual(code, 1, text)
        self.assertIn("with --auto-blackbox", text)

    def test_cells_broken_library_fails(self) -> None:
        tmp = self.copy_fixtures()
        cells = tmp / "tests/netlist/cells/cmos.v"
        cells.write_text(cells.read_text() + "\nmodule broken (\n")
        code, text = self.run_check(ni.check_cells, tmp)
        self.assertEqual(code, 1, text)
        self.assertIn("FAIL", text)


if __name__ == "__main__":
    unittest.main()
