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


def cells(*rows: tuple[str, str, str]) -> list[lc.Obj]:
    """Nodes with a cell type: (type, name, src)."""
    return [lc.Obj("m", "node", name, src, ctype) for ctype, name, src in rows]


class ParseTests(unittest.TestCase):
    def test_dump_fields_and_escapes(self) -> None:
        text = "m|node|$c1|a.v:3.1-4.10|b.v:2\nm\\|x|wire|a\\\\b\\nc|\nm|net|$n1|\n"
        rows = lc.parse_dump(text)
        self.assertEqual(rows[0], lc.Obj("m", "node", "$c1", "a.v:3.1-4.10|b.v:2"))
        self.assertEqual(rows[1], lc.Obj("m|x", "wire", "a\\b\nc", ""))
        self.assertEqual(rows[2].src, "")

    def test_dump_cell_types(self) -> None:
        rows = lc.parse_dump(
            "m|node:$_AND_|$c1|a.v:3\nm|node:|$c2|\nm|node:a\\|b|$c3|\nm|net|$n1|\n"
        )
        self.assertEqual((rows[0].kind, rows[0].ctype), ("node", "$_AND_"))
        self.assertEqual((rows[1].kind, rows[1].ctype), ("node", ""))
        self.assertEqual(rows[2].ctype, "a|b")
        self.assertEqual((rows[3].kind, rows[3].ctype), ("net", ""))

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

    def test_cell_class_is_by_type(self) -> None:
        # a constant driver: X-9; a port cell: X-8 in the BLIF dialects only; any other cell lost
        # in a lossless format has no class, whatever the written files hold
        before = cells(("$_CONST1_", "$c1", A), ("$port_in", "$c2", B), ("$and", "$c3", C))
        after = cells(("$_CONST1_", "$c1", ""), ("$port_in", "$c2", ""), ("$and", "$c3", ""))
        t = lc.compare("json", before, after)["node"]
        self.assertEqual((t.lost, t.unclassified), ({"X-9": 1}, 2))
        t = lc.compare("plain", before, after)["node"]
        self.assertEqual((t.lost, t.unclassified), ({"X-8": 1, "X-9": 1}, 1))

    def test_dropped_cell_with_a_wire_location_is_not_a_port(self) -> None:
        # the old rule took a lost cell with the location of a wire for a port cell
        before = dump(("wire", "x", A)) + cells(("$and", "$c1", A))
        after = dump(("wire", "x", A)) + cells(("$and", "$c1", ""))
        t = lc.compare("json", before, after)["node"]
        self.assertEqual((t.lost, t.unclassified), ({}, 1))

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

    def test_buffer_lost_in_verilog_is_x9_only_as_an_alias(self) -> None:
        before = cells(("$lut", "$c1", A), ("$lut", "$c2", "a.v:5.2-5.9"))
        after = cells(("$lut", "$c1", A), ("$lut", "$c2", ""))
        folded = {"m": lc.Carrier(frozenset({"$c2"}), frozenset({("a.v", 5)}))}
        t = lc.compare("verilog", before, after, folded)["node"]
        self.assertEqual((t.lost, t.unclassified), ({"X-9": 1}, 0))
        # the buffer is in the JSON file, but the Verilog file has no alias for it: a real loss
        kept = {"m": lc.Carrier(frozenset({"$c2"}), frozenset())}
        t = lc.compare("verilog", before, after, kept)["node"]
        self.assertEqual((t.lost, t.unclassified), ({}, 1))
        # an alias at that line is no excuse for a cell that is not a buffer
        other = {"m": lc.Carrier(frozenset(), frozenset({("a.v", 5)}))}
        self.assertEqual(lc.compare("verilog", before, after, other)["node"].unclassified, 1)
        t = lc.compare("json", before, after, folded)["node"]  # JSON keeps the buffer
        self.assertEqual((t.lost, t.unclassified), ({}, 1))

    def test_buffer_cells(self) -> None:
        lut = {"type": "$lut", "parameters": {"WIDTH": "0" * 31 + "1", "LUT": "10"}}
        inverter = {"type": "$lut", "parameters": {"WIDTH": "0" * 31 + "1", "LUT": "01"}}
        self.assertTrue(lc.is_buffer(lut))
        self.assertFalse(lc.is_buffer(inverter))
        self.assertTrue(lc.is_buffer({"type": "$_BUF_"}))

    def test_net_with_a_wire_location_is_inherited(self) -> None:
        # D-2, D-3: a net takes the location of its netname or declaration
        before = dump(("wire", "x", A), ("net", "$n1", ""), ("net", "$n2", ""))
        after = dump(("wire", "x", A), ("net", "$n1", A), ("net", "$n2", B))
        t = lc.compare("json", before, after)["net"]
        # $n1 takes the location of a wire (one net had none before); $n2 has no wire's location
        self.assertEqual((t.inherited, t.gained), (1, 1))
        more = lc.compare(
            "json",
            dump(("wire", "x", A), ("net", "$n1", A)),
            dump(("net", "$n1", A), ("net", "$n2", A)),
        )
        self.assertEqual(
            (more["net"].inherited, more["net"].gained), (0, 1)
        )  # no net to give it to
        named = lc.compare("json", dump(("wire", "n", A), ("net", "n", "")), dump(("net", "n", A)))
        self.assertEqual((named["net"].inherited, named["net"].gained), (1, 0))
        # a net named for another wire, or a generated name no wire can be told from: gained
        other = lc.compare("json", dump(("wire", "x", A), ("net", "n", "")), dump(("net", "n", B)))
        self.assertEqual((other["net"].inherited, other["net"].gained), (0, 1))

    def test_module_location_is_modgained(self) -> None:
        # a constant driver the reader makes takes the module's location (X-9, reported)
        before = dump(("module", "m", A), ("node", "$c1", ""), ("node", "$c2", ""))
        after = dump(("module", "m", A), ("node", "$c1", A), ("node", "$c2", B))
        t = lc.compare("verilog", before, after)["node"]
        self.assertEqual((t.modgained, t.gained), (1, 1))
        tally = {"node": lc.Tally(had=1, modgained=1)}
        self.assertEqual(lc.check_tallies("verilog", tally, lc.Allow(modgained=True)), [])
        problems = lc.check_tallies("verilog", tally)  # not listed: a failure
        self.assertTrue(any("module's location" in p for p in problems), problems)
        self.assertEqual(lc.check_tallies("verilog", {"node": lc.Tally(had=1)}), [])

    def test_new_objects_are_gained(self) -> None:
        before = dump(("wire", "x", A))
        after = dump(("wire", "x", A), ("wire", "$n7", B))
        self.assertEqual(lc.compare("verilog", before, after)["wire"].gained, 1)

    def test_every_surplus_is_gained(self) -> None:
        # one object with no location before, three located after: none is capped by the first
        before = dump(("node", "$c1", A), ("node", "$c2", ""))
        after = dump(("node", "$c1", A), ("node", "$c2", B), ("node", "$c3", B), ("node", "$c4", C))
        t = lc.compare("json", before, after)["node"]
        self.assertEqual((t.same, t.gained), (1, 3))

    def test_verilog_split_keeps_a_line_from_before(self) -> None:
        # the reader splits an assign into cells with its line: not gained; a line nothing had is
        before = dump(("node", "$c1", A))
        after = dump(("node", "$c1", "a.v:3"), ("node", "$c2", "a.v:3"), ("node", "$c3", "a.v:99"))
        t = lc.compare("verilog", before, after)["node"]
        self.assertEqual((t.line, t.split, t.gained), (1, 1, 1))
        t = lc.compare("json", before, after)["node"]  # no splitting in JSON
        self.assertEqual((t.split, t.gained), (0, 3))

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
        self.assertTrue(any("wire" in p and "different location" in p for p in problems), problems)

    def test_wrong_location_is_never_classified(self) -> None:
        # present but different: a failure in every format, whatever class the loss would have
        before = dump(("net", "n", A), ("node", "$c1", B)) + cells(("$_CONST0_", "n0", C))
        after = dump(("net", "n", "a.v:99.1-99.2")) + cells(("$_CONST0_", "n0", "a.v:98"))
        for fmt in ("json", "verilog", "blif", "plain"):
            t = lc.compare(fmt, before, after)
            self.assertEqual(t["net"].wrong, 1, fmt)
            self.assertEqual(t["net"].lost, {}, fmt)
            if fmt != "blif":  # the VTR dialect names no cell: it pools them
                self.assertEqual(t["node"].wrong, 1, fmt)
                self.assertEqual(t["node"].lost.get("X-9", 0), 0, fmt)
            problems = lc.check_tallies(fmt, t)
            self.assertTrue(any("different location" in p for p in problems), (fmt, problems))


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
        table = lc.parse_expect(
            "# c\nlut x.v error:script no lower for it\n\n"
            "absent lut x.v verilog sub not instantiated\n"
            "modgained verilog a literal 1'b1 driver\n"
        )
        self.assertEqual(table.designs, {("lut", "x.v"): ("error:script", "no lower for it")})
        self.assertEqual(table.allow("lut", "x.v", "verilog"), lc.Allow(frozenset({"sub"}), True))
        self.assertEqual(table.allow("lut", "x.v", "json"), lc.Allow())
        self.assertEqual(table.allow("lut", "y.v", "verilog"), lc.Allow(frozenset(), True))
        for bad in (
            "lut x.v\n",
            "lut x.v bogus why\n",
            "absent lut x.v json sub no module-less json\n",  # JSON always has every module
            "absent lut x.v plain sub nor plain BLIF\n",
            "modgained blah why\n",
            "modgained verilog\n",
            "absent lut x.v verilog sub\n",  # no reason
        ):
            with self.assertRaises(lc.ToolError, msg=bad):
                lc.parse_expect(bad)

    def test_level_script(self) -> None:
        text = lc.level_script(
            "# c\n@TECHLIB@\nread_verilog @DESIGN@\nproc\n", {"@DESIGN@": "d.v", "@TECHLIB@": ""}
        )
        self.assertEqual(text, "read_verilog d.v; proc")


class AbsentModuleTests(unittest.TestCase):
    """The objects of a module the read-back lacks are X-1 only where the expect file says so."""

    BEFORE = [
        lc.Obj("top", "module", "top", A),
        lc.Obj("top", "wire", "x", A),
        lc.Obj("sub", "module", "sub", B),
        lc.Obj("sub", "wire", "y", B),
    ]
    AFTER = [lc.Obj("top", "module", "top", A), lc.Obj("top", "wire", "x", A)]

    def test_missing_module_fails_in_every_format(self) -> None:
        for fmt in ("json", "verilog", "blif", "plain"):
            t = lc.compare(fmt, self.BEFORE, self.AFTER)
            problems = lc.check_tallies(fmt, t)
            self.assertTrue(any("module sub is absent" in p for p in problems), (fmt, problems))

    def test_listed_module_is_x1(self) -> None:
        allow = lc.Allow(absent=frozenset({"sub"}))
        t = lc.compare("verilog", self.BEFORE, self.AFTER, None, allow)
        self.assertEqual(lc.check_tallies("verilog", t, allow), [])
        self.assertEqual(t["wire"].lost, {"X-1": 1})

    def test_listed_module_that_is_instantiated_still_fails(self) -> None:
        before = self.BEFORE + [lc.Obj("top", "node", "$c1", A, "sub")]
        after = self.AFTER + [lc.Obj("top", "node", "$c1", A, "")]
        allow = lc.Allow(absent=frozenset({"sub"}))
        problems = lc.check_tallies(
            "verilog", lc.compare("verilog", before, after, None, allow), allow
        )
        self.assertTrue(any("instantiates it" in p for p in problems), problems)

    def test_stale_entry_fails(self) -> None:
        allow = lc.Allow(absent=frozenset({"top"}))
        t = lc.compare("verilog", self.BEFORE, self.BEFORE, None, allow)
        problems = lc.check_tallies("verilog", t, allow)
        self.assertTrue(any("STALE absent entry for module top" in p for p in problems), problems)

    def test_unlisted_module_objects_get_no_x1(self) -> None:
        t = lc.compare("json", self.BEFORE, self.AFTER)
        self.assertEqual(t["wire"].unclassified, 1)  # JSON wires are lossless
        self.assertNotIn("X-1", t["wire"].lost)


class BlifDialectTests(unittest.TestCase):
    def test_blif_wire_with_a_location_fails(self) -> None:
        rows = dump(("module", "m", A), ("wire", "x", A))
        for fmt in ("blif", "plain"):
            problems = lc.check_tallies(fmt, lc.compare(fmt, rows, rows))
            self.assertTrue(any("out of date" in p for p in problems), (fmt, problems))

    def test_blif_wire_without_one_is_fine(self) -> None:
        before = dump(("wire", "x", A), ("net", "$n1", A), ("node", "$c1", A))
        after = dump(("wire", "x", ""), ("net", "$n1", ""), ("node", "$c1", A))
        for fmt in ("blif", "plain"):
            problems = lc.check_tallies(fmt, lc.compare(fmt, before, after))
            self.assertFalse(any("out of date" in p for p in problems), (fmt, problems))


class PinTests(unittest.TestCase):
    PINS = lc.parse_pins("# c\nmicros lut json node X-9 5\nmicros lut json net X-1 7\n")

    def sums(self, nodes: int, nets: int) -> dict[tuple[str, str], list[dict[str, lc.Tally]]]:
        run = {
            "node": lc.Tally(lost={"X-9": nodes} if nodes else {}),
            "net": lc.Tally(lost={"X-1": nets}),
        }
        return {("lut", "json"): [run]}

    def test_at_the_pin(self) -> None:
        self.assertEqual(lc.pin_problems(self.PINS, "micros", self.sums(5, 7)), ([], []))

    def test_above_fails_below_notes(self) -> None:
        fails, notes = lc.pin_problems(self.PINS, "micros", self.sums(6, 6))
        self.assertEqual(len(fails), 1)
        self.assertIn("lut json node X-9: 6 lost, pinned 5", fails[0])
        self.assertEqual(len(notes), 1)
        self.assertIn("improved, update the pin", notes[0])

    def test_a_class_with_no_pin_is_pinned_at_zero(self) -> None:
        sums = self.sums(5, 7)
        sums[("lut", "json")][0]["wire"] = lc.Tally(lost={"X-8": 1})
        fails, _ = lc.pin_problems(self.PINS, "micros", sums)
        self.assertEqual(len(fails), 1)
        self.assertIn("wire X-8", fails[0])

    def test_other_sets_are_not_looked_at(self) -> None:
        self.assertEqual(lc.pin_problems(self.PINS, "vtr14", self.sums(0, 0)), ([], []))

    def test_pin_lines_round_trip(self) -> None:
        run = {"node": lc.Tally(objects=40, had=30, lost={"X-9": 5})}
        lines = lc.pin_lines("micros", "lut", "json", run)
        self.assertEqual(
            lc.parse_pins("\n".join(lines)), {("micros", "lut", "json", "node", "X-9"): 5}
        )
        self.assertIn("# of 40 objects, 30 with a location", lines[0])

    def test_modgained_is_pinned(self) -> None:
        pins = lc.parse_pins("micros lut verilog node modgained 3\n")
        run = {"node": lc.Tally(modgained=4)}
        fails, _ = lc.pin_problems(pins, "micros", {("lut", "verilog"): [run]})
        self.assertEqual(len(fails), 1)
        self.assertIn("modgained", fails[0])
        lines = lc.pin_lines("micros", "lut", "verilog", run)
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("micros lut verilog node modgained 4  # of "))

    def test_bad_pins(self) -> None:
        for bad in ("micros lut json node X-9\n", "micros lut json node lost 5\n"):
            with self.assertRaises(lc.ToolError):
                lc.parse_pins(bad)


class WrittenTests(unittest.TestCase):
    def test_verilog_aliases(self) -> None:
        text = (
            '(* src = "a.v:1.1-9.9" *)\nmodule top (  // a.v:1\n  input a\n);\n'
            "  assign y = a;  // /w/a.v:5\n"
            "  assign z[2] = a[1];  // /w/a.v:6\n"
            "  assign q = a == b;  // /w/a.v:7\n"
            "  assign r = ~a;  // /w/a.v:8\n"
            "  assign s = a;\n"
            "endmodule\nmodule \\odd.name  (\n);\n  assign u = v;  // /w/b.v:2\nendmodule\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "W.v"
            path.write_text(text, encoding="utf-8")
            got = lc.verilog_aliases(path)
        self.assertEqual(got["top"], frozenset({("/w/a.v", 5), ("/w/a.v", 6)}))
        self.assertEqual(got["odd.name"], frozenset({("/w/b.v", 2)}))

    def test_written_facts(self) -> None:
        lut = {"type": "$lut", "parameters": {"WIDTH": "0" * 31 + "1", "LUT": "10"}}
        inv = {"type": "$lut", "parameters": {"WIDTH": "0" * 31 + "1", "LUT": "01"}}
        doc = {"modules": {"top": {"cells": {"b": lut, "i": inv}}}}
        with tempfile.TemporaryDirectory() as tmp:
            j, v = Path(tmp) / "W.json", Path(tmp) / "W.v"
            j.write_text(__import__("json").dumps(doc), encoding="utf-8")
            v.write_text(
                "module top (\n);\n  assign y = a;  // a.v:4\nendmodule\n", encoding="utf-8"
            )
            got = lc.written_facts(j, v)
        self.assertEqual(got["top"], lc.Carrier(frozenset({"b"}), frozenset({("a.v", 4)})))


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
