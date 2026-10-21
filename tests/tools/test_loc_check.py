"""Tests for tools/loc-check/loc_check.py (5.4 spec T-3, section 5): the dump parser, the location
text, the comparison of a hand-made before and after pair (same, line, inherited, lost with a
class, gained), the failure rules (zero objects, a lost object with no class, gained, a line count
where the format is lossless), the specificity numbers, the expect file and the exit codes, with
the Odin III runs replaced by fixed dumps.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import loc_check as lc

from .helpers import run_main

A = "a.v:3.1-4.10"
B = "a.v:5.2-5.9"
C = "a.v:7.1-7.4|b.v:2.1-2.2"


def dump(*rows: tuple[str, str, str]) -> list[lc.Obj]:
    return [lc.Obj("m", kind, name, src) for kind, name, src in rows]


class ParseTests(unittest.TestCase):
    def test_dump_fields_and_escapes(self) -> None:
        text = "m|node|$c1|a.v:3.1-4.10|b.v:2\nm\\|x|wire|a\\\\b\\nc|\nm|net|$n1|\n"
        rows = lc.parse_dump(text)
        self.assertEqual(rows[0], lc.Obj("m", "node", "$c1", "a.v:3.1-4.10|b.v:2"))
        self.assertEqual(rows[1], lc.Obj("m|x", "wire", "a\\b\nc", ""))
        self.assertEqual(rows[2].src, "")

    def test_bad_dump_line(self) -> None:
        with self.assertRaises(lc.ToolError):
            lc.parse_dump("m|node|only three\n")

    def test_locations(self) -> None:
        locs = lc.parse_locs(C)
        self.assertEqual([(x.file, x.line) for x in locs], [("a.v", 7), ("b.v", 2)])
        self.assertEqual(lc.parse_locs("a.v:12")[0].line, 12)
        self.assertEqual(lc.parse_locs("C:\\x\\a.v:3.1-4.2")[0].file, "C:\\x\\a.v")
        self.assertEqual(lc.parse_locs(""), [])

    def test_generated_names(self) -> None:
        self.assertTrue(lc.is_generated("$c12"))
        self.assertTrue(lc.is_generated("$n1"))
        self.assertFalse(lc.is_generated("x1"))
        self.assertFalse(lc.is_generated("$c7.and0"))


class CompareTests(unittest.TestCase):
    def test_same_line_lost_gained_named(self) -> None:
        before = dump(
            ("module", "m", A),
            ("wire", "x", A),
            ("wire", "y", A),
            ("wire", "z", A),
            ("wire", "w", ""),
        )
        after = dump(
            ("module", "m", A),
            ("wire", "x", A),
            ("wire", "y", "a.v:3.9-9.9"),  # same file and line, other columns
            ("wire", "z", ""),
            ("wire", "w", B),  # gained
        )
        t = lc.compare("verilog", before, after)["wire"]
        self.assertEqual((t.objects, t.had, t.same, t.line, t.gained), (4, 3, 1, 1, 1))
        self.assertEqual(t.lost, {})
        self.assertEqual(t.unclassified, 1)  # a wire has a carrier in Verilog: z is a bug
        self.assertEqual(lc.compare("verilog", before, after)["module"].same, 1)

    def test_pool_matching_of_generated_names(self) -> None:
        before = dump(
            ("node", "$c1", A), ("node", "$c2", A), ("node", "$c3", B), ("node", "$c4", "")
        )
        after = dump(
            ("node", "$c9", B), ("node", "$c8", A), ("node", "$c7", A), ("node", "$c6", "")
        )
        t = lc.compare("plain", before, after)["node"]
        self.assertEqual((t.had, t.same, t.line, t.unclassified, t.gained), (3, 3, 0, 0, 0))

    def test_lost_gets_a_class_by_rule(self) -> None:
        before = dump(("net", "$n1", A), ("net", "$n2", B), ("node", "$c1", A))
        after = dump(("net", "$n1", ""), ("net", "$n2", ""), ("node", "$c1", ""))
        t = lc.compare("json", before, after)
        self.assertEqual(t["net"].lost, {"X-1": 2})
        self.assertEqual(t["node"].lost, {})
        self.assertEqual(t["node"].unclassified, 1)  # a JSON cell is lossless: no class applies
        self.assertEqual(lc.compare("blif", before, after)["node"].lost, {"X-6": 1})
        self.assertEqual(lc.compare("plain", before, after)["net"].lost, {"X-1": 2})

    def test_cell_without_a_json_entry(self) -> None:
        # a constant driver the JSON file has no cell for: X-9; a port cell (the location of a
        # declared wire): X-8; a cell the file has but the read-back lost: no class
        before = dump(("wire", "x", A), ("node", "$c1", A), ("node", "$c2", B), ("node", "$c3", C))
        after = dump(
            ("wire", "x", A), ("node", "$c1", ""), ("node", "$c2", ""), ("node", "$c3", "")
        )
        written = {"m": lc.CellSet(frozenset({"$c3"}), frozenset())}
        t = lc.compare("json", before, after, written)["node"]
        self.assertEqual((t.lost, t.unclassified), ({"X-8": 1, "X-9": 1}, 1))
        t = lc.compare("json", before, after)["node"]  # no JSON file known: all are written
        self.assertEqual((t.lost, t.unclassified), ({}, 3))

    def test_named_net_class(self) -> None:
        before = dump(("net", "n", A))
        after = dump(("net", "n", ""))
        self.assertEqual(lc.compare("plain", before, after)["net"].lost, {"X-7": 1})
        self.assertEqual(lc.compare("blif", before, after)["net"].unclassified, 1)

    def test_blif_cells_are_inherited(self) -> None:
        before = dump(
            ("node", "$c1", A), ("node", "$c2", B), ("node", "$c3", C), ("node", "$c4", "")
        )
        after = dump(
            ("node", "$c5", "a.v:3"), ("node", "$c6", B), ("node", "$c7", ""), ("node", "$c8", A)
        )
        t = lc.compare("blif", before, after)["node"]
        # inherited: every located cell after (the dialect writes no cell location); lost: the
        # had cell nothing matches
        self.assertEqual((t.had, t.inherited, t.same, t.line), (3, 3, 0, 0))
        self.assertEqual((t.lost, t.gained, t.unclassified), ({"X-6": 1}, 0, 0))

    def test_gained_in_a_pool(self) -> None:
        before = dump(("node", "$c1", A), ("node", "$c2", ""))
        after = dump(("node", "$c1", A), ("node", "$c2", B))
        t = lc.compare("json", before, after)["node"]
        self.assertEqual((t.same, t.gained), (1, 1))

    def test_buffer_lost_in_verilog_is_x9(self) -> None:
        before = dump(("node", "$c1", A), ("node", "$c2", B))
        after = dump(("node", "$c1", A), ("node", "$c2", ""))
        written = {"m": lc.CellSet(frozenset({"$c1", "$c2"}), frozenset({"$c2"}))}
        t = lc.compare("verilog", before, after, written)["node"]
        self.assertEqual((t.lost, t.unclassified), ({"X-9": 1}, 0))
        t = lc.compare("json", before, after, written)["node"]  # JSON keeps the buffer
        self.assertEqual((t.lost, t.unclassified), ({}, 1))

    def test_buffer_cells(self) -> None:
        lut = {"type": "$lut", "parameters": {"WIDTH": "0" * 31 + "1", "LUT": "10"}}
        inverter = {"type": "$lut", "parameters": {"WIDTH": "0" * 31 + "1", "LUT": "01"}}
        self.assertTrue(lc.is_buffer(lut))
        self.assertFalse(lc.is_buffer(inverter))
        self.assertTrue(lc.is_buffer({"type": "$_BUF_"}))
        self.assertEqual(lc.cell_set({"a": lut, "b": inverter}).buffers, frozenset({"a"}))

    def test_net_with_a_wire_location_is_inherited(self) -> None:
        # D-2, D-3: a net takes the location of its netname or declaration
        before = dump(("wire", "x", A), ("net", "$n1", ""), ("net", "$n2", ""))
        after = dump(("wire", "x", A), ("net", "$n1", A), ("net", "$n2", B))
        t = lc.compare("json", before, after)["net"]
        self.assertEqual((t.inherited, t.gained), (1, 1))
        named = lc.compare("json", dump(("wire", "x", A), ("net", "n", "")), dump(("net", "n", A)))
        self.assertEqual((named["net"].inherited, named["net"].gained), (1, 0))

    def test_module_location_is_modgained(self) -> None:
        # a constant driver the reader makes takes the module's location (X-9, reported)
        before = dump(("module", "m", A), ("node", "$c1", ""), ("node", "$c2", ""))
        after = dump(("module", "m", A), ("node", "$c1", A), ("node", "$c2", B))
        t = lc.compare("verilog", before, after)["node"]
        self.assertEqual((t.modgained, t.gained), (1, 1))
        problems = lc.check_tallies("verilog", {"node": lc.Tally(had=1, modgained=1)})
        self.assertEqual(problems, [])

    def test_new_objects_are_not_gained(self) -> None:
        before = dump(("wire", "x", A))
        after = dump(("wire", "x", A), ("wire", "$n7", B))
        self.assertEqual(lc.compare("verilog", before, after)["wire"].gained, 0)

    def test_line_only_cells_in_verilog(self) -> None:
        before = dump(("node", "$c1", A), ("node", "$c2", C))
        after = dump(("node", "$c1", "a.v:3"), ("node", "$c2", "a.v:7"))
        t = lc.compare("verilog", before, after)["node"]
        self.assertEqual((t.same, t.line, t.unclassified), (0, 2, 0))
        self.assertEqual(lc.check_tallies("verilog", {"node": t}), [])

    def test_mutated_readback_is_reported(self) -> None:
        before = dump(("module", "m", A), ("wire", "x", A), ("wire", "y", B))
        good = dump(("module", "m", A), ("wire", "x", A), ("wire", "y", B))
        bad = dump(("module", "m", A), ("wire", "x", A), ("wire", "y", "a.v:99.1-99.2"))
        self.assertEqual(lc.check_tallies("json", lc.compare("json", before, good)), [])
        problems = lc.check_tallies("json", lc.compare("json", before, bad))
        self.assertTrue(any("wire" in p and "no class" in p for p in problems), problems)


class CheckTests(unittest.TestCase):
    def test_zero_objects_fail(self) -> None:
        problems = lc.check_tallies("json", lc.compare("json", [], []))
        self.assertTrue(any("matched no objects" in p for p in problems), problems)

    def test_no_located_object_fails(self) -> None:
        rows = dump(("wire", "x", ""))
        problems = lc.check_tallies("json", lc.compare("json", rows, rows))
        self.assertTrue(any("matched no objects" in p for p in problems), problems)

    def test_gained_fails(self) -> None:
        before = dump(("wire", "x", A), ("wire", "y", ""))
        after = dump(("wire", "x", A), ("wire", "y", B))
        problems = lc.check_tallies("json", lc.compare("json", before, after))
        self.assertTrue(any("gained" in p for p in problems), problems)

    def test_line_where_lossless_fails(self) -> None:
        before = dump(("wire", "x", A))
        after = dump(("wire", "x", "a.v:3"))
        for fmt in ("json", "verilog", "plain"):
            problems = lc.check_tallies(fmt, lc.compare(fmt, before, after))
            self.assertTrue(any("line-only" in p for p in problems), (fmt, problems))

    def test_pool_pairs_by_line_only_where_the_format_does(self) -> None:
        before = dump(("node", "$c1", A))
        after = dump(("node", "$c1", "a.v:3"))
        self.assertEqual(lc.compare("verilog", before, after)["node"].line, 1)
        t = lc.compare("json", before, after)["node"]
        self.assertEqual((t.line, t.unclassified), (0, 1))

    def test_clean_pair(self) -> None:
        rows = dump(("module", "m", A), ("wire", "x", A), ("node", "$c1", B))
        self.assertEqual(lc.check_tallies("json", lc.compare("json", rows, rows)), [])


class NumbersTests(unittest.TestCase):
    def test_specificity(self) -> None:
        self.assertEqual(lc.specificity([]), (0.0, 0.0))
        self.assertEqual(lc.specificity([1, 1, 3]), (1.0, 3.0))
        self.assertEqual(lc.specificity([4, 1, 2, 3]), (2.5, 4.0))
        values = list(range(1, 11))
        self.assertEqual(lc.specificity(values), (5.5, 9.0))

    def test_specificity_columns(self) -> None:
        before = dump(("node", "$c1", C), ("node", "$c2", A), ("node", "$c3", A))
        after = dump(("node", "$c1", C), ("node", "$c2", A), ("node", "$c3", ""))
        t = lc.compare("json", before, after)["node"]
        self.assertEqual(sorted(t.lines_before), [1, 1, 2])
        self.assertEqual(sorted(t.lines_after), [1, 2])

    def test_cap_count(self) -> None:
        many = "|".join(f"a.v:{n}" for n in range(1, 9))
        t = lc.compare("json", dump(("node", "$c1", many)), dump(("node", "$c1", many)))["node"]
        self.assertEqual(t.capped, 1)

    def test_total_row(self) -> None:
        one = lc.compare("json", dump(("wire", "x", A)), dump(("wire", "x", A)))
        two = lc.compare("json", dump(("wire", "y", A)), dump(("wire", "y", "")))
        total = lc.merge_tallies([one, two])["wire"]
        self.assertEqual((total.objects, total.had, total.same), (2, 2, 1))
        self.assertEqual(total.unclassified, 1)


class ExpectTests(unittest.TestCase):
    def test_parse(self) -> None:
        table = lc.parse_expect("# c\nlut x.v error:script no lower for it\n\n")
        self.assertEqual(table, {("lut", "x.v"): ("error:script", "no lower for it")})
        with self.assertRaises(lc.ToolError):
            lc.parse_expect("lut x.v\n")
        with self.assertRaises(lc.ToolError):
            lc.parse_expect("lut x.v bogus why\n")

    def test_level_script(self) -> None:
        text = lc.level_script(
            "# c\n@TECHLIB@\nread_verilog @DESIGN@\nproc\n", {"@DESIGN@": "d.v", "@TECHLIB@": ""}
        )
        self.assertEqual(text, "read_verilog d.v; proc")


class MainTests(unittest.TestCase):
    def test_usage_error(self) -> None:
        code, _, err = run_main(lc.main, ["--level", "nope", "x.v"])
        self.assertEqual(code, 2)
        self.assertIn("unknown level", err)

    def test_no_designs(self) -> None:
        code, _, err = run_main(lc.main, ["--level", "word"])
        self.assertEqual(code, 2)
        self.assertIn("no designs", err)

    def test_run_with_fixed_dumps(self) -> None:
        rows = [lc.Obj("m", "module", "m", A), lc.Obj("m", "wire", "x", A)]
        runs = {fmt: lc.Pair(rows, rows) for fmt in ("json", "verilog")}
        with tempfile.TemporaryDirectory() as tmp:
            design = Path(tmp) / "d.v"
            design.write_text("module m; endmodule\n", encoding="utf-8")
            with mock.patch.object(lc, "run_design", return_value=runs):
                code, out, _ = run_main(lc.main, ["--level", "word", str(design)])
        self.assertEqual(code, 0, out)
        self.assertIn("loc-check: compared 1 of 1", out)
        self.assertIn("TOTAL", out)

    def test_run_with_a_gained_location(self) -> None:
        before = [lc.Obj("m", "wire", "x", A), lc.Obj("m", "wire", "y", "")]
        after = [lc.Obj("m", "wire", "x", A), lc.Obj("m", "wire", "y", B)]
        with tempfile.TemporaryDirectory() as tmp:
            design = Path(tmp) / "d.v"
            design.write_text("module m; endmodule\n", encoding="utf-8")
            with mock.patch.object(lc, "run_design", return_value={"json": lc.Pair(before, after)}):
                code, out, _ = run_main(lc.main, ["--level", "word", str(design)])
        self.assertEqual(code, 1)
        self.assertIn("gained", out)

    def test_stale_expect(self) -> None:
        rows = [lc.Obj("m", "wire", "x", A)]
        with tempfile.TemporaryDirectory() as tmp:
            design = Path(tmp) / "d.v"
            design.write_text("module m; endmodule\n", encoding="utf-8")
            expect = Path(tmp) / "expect.txt"
            expect.write_text("word d.v error:script was broken\n", encoding="utf-8")
            with mock.patch.object(lc, "run_design", return_value={"json": lc.Pair(rows, rows)}):
                code, out, _ = run_main(
                    lc.main, ["--level", "word", "--expect", str(expect), str(design)]
                )
        self.assertEqual(code, 1)
        self.assertIn("STALE", out)


if __name__ == "__main__":
    unittest.main()
