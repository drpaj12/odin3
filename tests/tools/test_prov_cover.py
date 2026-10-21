"""Tests for tools/prov-cover/prov_cover.py (6.5b, spec 6 section 5, P-1 to P-8): the class of every
node of a typed dump, the exceptions P-3 names, the strict figure, the written-file cross-check,
the specificity numbers, the `.place` check and the exit status. Pure Python: hand-made dumps and
netlist facts, no odin3 binary.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from unittest import mock

import legs
import prov_cover as pc

from .helpers import run_main

A = "a.v:3.1-4.10"
B = "a.v:5.2-5.9"


def dump(*rows: tuple[str, str, str]) -> list[pc.Obj]:
    """Typed node rows: (type, name, src)."""
    return pc.parse_dump("".join(f"m|node:{t}|{n}|{s}\n" for t, n, s in rows))


def cell(
    ctype: str, ins: tuple[str, ...], outs: tuple[str, ...], identity: bool = False
) -> pc.Cell:
    return pc.Cell(ctype, ins, outs, identity)


def facts(
    cells: dict[str, pc.Cell],
    port_in: tuple[str, ...] = (),
    port_out: tuple[str, ...] = (),
) -> pc.Facts:
    return {"m": pc.Mod(cells, frozenset(port_in), frozenset(port_out))}


class AllowTests(unittest.TestCase):
    def test_default_enables_inherited_and_pad(self) -> None:
        self.assertEqual(pc.parse_allow(None), frozenset({"inherited", "pad"}))
        self.assertEqual(pc.DEFAULT_ALLOW, frozenset({"inherited", "pad"}))

    def test_allow_can_narrow(self) -> None:
        self.assertEqual(pc.parse_allow("pad"), frozenset({"pad"}))
        self.assertEqual(pc.parse_allow(""), frozenset())
        with self.assertRaises(pc.ToolError):
            pc.parse_allow("inherited,bogus")


class ClassTests(unittest.TestCase):
    def test_p1_classes_by_type(self) -> None:
        rows = dump(
            ("$lut", "$c1", A),
            ("$_DFF_P_", "$c2", A),
            ("multiply", "$c3", A),
            ("lpm_add_sub", "$c4", B),
            ("altsyncram", "$c5", B),
            ("$port_in", "$c6", ""),
        )
        rep = pc.classify(rows, {}, allow=frozenset())
        self.assertEqual(rep.objects, {"lut": 1, "latch": 1, "hard": 1, "lpm": 2})
        self.assertEqual(rep.resolved, rep.objects)
        self.assertTrue(rep.ok)
        self.assertEqual(rep.coverage, 1.0)

    def test_unresolved_plain_lut_fails_the_gate(self) -> None:
        rows = dump(("$lut", "$c1", A), ("$lut", "$c2", ""))
        f = facts({"$c2": cell("$lut", ("x", "y"), ("z",))})
        rep = pc.classify(rows, f, allow=pc.DEFAULT_ALLOW)
        self.assertFalse(rep.ok)
        self.assertEqual([d.name for d in rep.defects], ["$c2"])
        self.assertEqual(rep.coverage, 0.5)

    def test_defect_on_an_output_port_net_is_noted(self) -> None:
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""))
        f = facts(
            {
                "$c1": cell("$sop", ("a", "b"), ("o",)),
                "$c2": cell("$sop", ("a", "b"), ("n",)),
            },
            port_out=("o",),
        )
        rep = pc.classify(rows, f, allow=pc.DEFAULT_ALLOW)
        self.assertEqual([d.note for d in rep.defects], ["drives-output-port", ""])
        self.assertIn(
            "defects by note: drives-output-port 1, other 1", pc.render_table([("d", "b", rep)])
        )

    def test_every_class_listed_zero_included(self) -> None:
        rep = pc.classify(dump(("$lut", "$c1", A)), {}, allow=pc.DEFAULT_ALLOW)
        self.assertEqual(set(rep.exceptions), set(pc.EXCEPTIONS))
        self.assertEqual(sum(rep.exceptions.values()), 0)
        text = pc.render_table([("d", "a", rep)])
        for name in pc.EXCEPTIONS:
            self.assertIn(name, text)
        self.assertIn("strict", text)

    def test_const_driver(self) -> None:
        rows = dump(("$_CONST0_", "$c1", ""), ("$_CONST1_", "$c2", A), ("$lut", "$c3", A))
        rep = pc.classify(rows, {}, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["const-driver"], 1)
        self.assertTrue(rep.ok)

    def test_const_table(self) -> None:
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""), ("$lut", "$c3", A))
        f = facts({"$c1": cell("$sop", (), ("gnd",)), "$c2": cell("$sop", (), ("n9",))})
        rep = pc.classify(rows, f, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["const-table"], 1)
        self.assertEqual([d.name for d in rep.defects], ["$c2"])

    def test_const_table_folded_by_write_json(self) -> None:
        # write_json folds a zero-input table into a constant: no cell, a gnd net of constant bits
        rows = dump(("$sop", "$c1", ""))
        mod = pc.Mod({}, generators=frozenset({"gnd"}))
        rep = pc.classify(rows, {"m": mod}, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["const-table"], 1)
        rep = pc.classify(rows, {"m": pc.Mod({})}, allow=frozenset(), readback=True)
        self.assertEqual(len(rep.defects), 1)

    def test_port_buffer_output_and_input_side(self) -> None:
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""), ("$sop", "$c3", ""))
        f = facts(
            {
                "$c1": cell("$sop", ("n1",), ("o",), True),
                "$c2": cell("$sop", ("i",), ("n2",), True),
                "$c3": cell("$sop", ("n3",), ("n4",), True),
            },
            port_in=("i",),
            port_out=("o",),
        )
        rep = pc.classify(rows, f, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["port-buffer"], 2)
        self.assertEqual([d.name for d in rep.defects], ["$c3"])

    def test_port_buffer_same_net_or_non_identity_is_no_exception(self) -> None:
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""))
        f = facts(
            {
                "$c1": cell("$sop", ("o",), ("o",), True),
                "$c2": cell("$sop", ("n1",), ("o",), False),
            },
            port_out=("o",),
        )
        rep = pc.classify(rows, f, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["port-buffer"], 0)
        self.assertEqual(len(rep.defects), 2)

    def test_port_buffer_with_located_leg_a_driver_is_a_lost_location(self) -> None:
        rows = dump(("$sop", "$c1", ""))
        f = facts({"$c1": cell("$sop", ("n1",), ("o",), True)}, port_out=("o",))
        rep = pc.classify(rows, f, allow=frozenset(), readback=True, located={("m", "o")})
        self.assertEqual(rep.exceptions["port-buffer"], 0)
        self.assertEqual(rep.defects[0].kind, "lost-location")
        self.assertFalse(rep.ok)

    def test_not_helper(self) -> None:
        rows = dump(
            ("$sop", "$c1", A), ("$_NOT_", "$c2", ""), ("$_NOT_", "$c3", ""), ("$lut", "$c4", A)
        )
        f = facts(
            {
                "$c1": cell("$sop", ("a",), ("n1",)),
                "$c2": cell("$_NOT_", ("n1",), ("n2",)),
                "$c3": cell("$_NOT_", ("a",), ("n3",)),
            }
        )
        rep = pc.classify(rows, f, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["not-helper"], 1)
        self.assertEqual([d.name for d in rep.defects], ["$c3"])

    def test_inherited_counted_both_ways(self) -> None:
        rows = dump(("$sop", "$c1", A), ("$sop", "$c2", B), ("multiply", "$c3", A))
        on = pc.classify(rows, {}, allow=pc.DEFAULT_ALLOW, inherits=True)
        self.assertEqual(on.exceptions["inherited"], 3)
        self.assertEqual(on.coverage, 1.0)
        self.assertEqual(on.strict, 0.0)
        off = pc.classify(rows, {}, allow=frozenset(), inherits=True)
        self.assertEqual(off.exceptions["inherited"], 3)
        self.assertEqual(off.coverage, 0.0)
        self.assertFalse(off.ok)
        self.assertEqual(len(off.defects), 3)

    def test_other_unlocated_node_is_a_defect(self) -> None:
        rep = pc.classify(dump(("$_AND_", "$c1", "")), {}, allow=pc.DEFAULT_ALLOW)
        self.assertFalse(rep.ok)
        self.assertEqual(rep.defects[0].kind, "unresolved")

    def test_strict_figure_excludes_exceptions(self) -> None:
        rows = dump(("$lut", "$c1", A), ("$sop", "$c2", ""), ("$lut", "$c3", A))
        f = facts({"$c2": cell("$sop", (), ("vcc",))})
        rep = pc.classify(rows, f, allow=pc.DEFAULT_ALLOW, readback=True)
        self.assertEqual(rep.coverage, 1.0)  # 2 of 2 once the const table is removed
        self.assertAlmostEqual(rep.strict or 0.0, 2 / 3)

    def test_first_ten_defects_named(self) -> None:
        rows = dump(*[("$lut", f"$c{i}", "") for i in range(15)])
        f = facts({f"$c{i}": cell("$lut", ("a",), (f"n{i}",)) for i in range(15)})
        rep = pc.classify(rows, f, allow=pc.DEFAULT_ALLOW)
        self.assertEqual(len(rep.defects), 15)
        self.assertEqual(len(rep.named_defects()), 10)


class CrossCheckTests(unittest.TestCase):
    def _rep(self, written: dict[str, int]) -> pc.LegReport:
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""))
        f = facts(
            {
                "$c1": cell("$sop", (), ("gnd",)),
                "$c2": cell("$sop", ("n",), ("o",), True),
            },
            port_out=("o",),
        )
        return pc.classify(rows, f, allow=frozenset(), readback=True, written=written)

    def test_equal_counts_pass(self) -> None:
        rep = self._rep({"const-table": 1, "port-buffer": 1, "not-helper": 0})
        self.assertEqual(rep.mismatches, [])
        self.assertTrue(rep.ok)

    def test_mismatch_fails_and_is_printed(self) -> None:
        rep = self._rep({"const-table": 1, "port-buffer": 2, "not-helper": 0})
        self.assertFalse(rep.ok)
        self.assertEqual(len(rep.mismatches), 1)
        self.assertIn("port-buffer", rep.mismatches[0])
        self.assertIn("port-buffer", pc.render_table([("d", "b", rep)]))

    def test_missing_written_counts_are_not_checked(self) -> None:
        self.assertEqual(self._rep({}).mismatches, [])


class WrittenBlifTests(unittest.TestCase):
    BLIF = """.model top
.inputs a[0] b
.outputs o p q
.names gnd
.names vcc
1
.names a[0] n1@a.v:3
1 1
.names n1@a.v:3 o
1 1
.names a[0] b n2@a.v:4
11 1
.names n2@a.v:4 p
1 0
.names b q
1 1
.latch n1@a.v:3 r 0
.end
"""

    def test_counts(self) -> None:
        got = pc.written_counts(self.BLIF)
        self.assertEqual(got["const-table"], 2)
        # a[0] -> n1@ (output net located: the cell inherits, no exception); n1 -> o and b -> q
        # (output is a port bit, no location)
        self.assertEqual(got["port-buffer"], 2)
        self.assertEqual(got["not-helper"], 0)  # the reader makes no $not (reader.c end_names)
        self.assertEqual(got["offset-cover"], 1)
        self.assertEqual(got["lut"], 5)  # the 7 .names minus the 2 constant generators
        self.assertEqual(got["latch"], 1)


JSON = json.dumps(
    {
        "modules": {
            "m": {
                "ports": {
                    "x": {"direction": "input", "bits": [2]},
                    "o": {"direction": "output", "bits": [4]},
                },
                "cells": {
                    "$c1": {
                        "type": "$lut",
                        "parameters": {"LUT": "10", "WIDTH": "00000000000000000000000000000001"},
                        "port_directions": {"A": "input", "Y": "output"},
                        "connections": {"A": [2], "Y": [3]},
                    },
                    "$c2": {
                        "type": "$lut",
                        "parameters": {"LUT": "01"},
                        "port_directions": {"A": "input", "Y": "output"},
                        "connections": {"A": [3], "Y": [4]},
                    },
                    "$c3": {
                        "type": "$lut",
                        "parameters": {"LUT": "1"},
                        "port_directions": {"Y": "output"},
                        "connections": {"Y": ["0"]},
                    },
                },
                "netnames": {
                    "gnd": {"hide_name": 0, "bits": ["0"]},
                    "x": {"hide_name": 0, "bits": [2]},
                    "$n1": {"hide_name": 1, "bits": [3]},
                    "o": {"hide_name": 0, "bits": [4]},
                },
            }
        }
    }
)


class FactsTests(unittest.TestCase):
    def test_facts_from_json(self) -> None:
        mod = legs.facts_from_json(JSON)["m"]
        self.assertEqual(mod.cells["$c1"], pc.Cell("$lut", ("2",), ("3",), True))
        self.assertFalse(mod.cells["$c2"].identity)  # an inverter
        self.assertEqual(mod.cells["$c3"].ins, ())
        self.assertEqual((mod.port_in, mod.port_out), (frozenset({"2"}), frozenset({"4"})))
        self.assertEqual(mod.net_name("4"), "o")
        self.assertEqual(mod.generators, frozenset({"gnd"}))

    def test_json_facts_feed_the_rules(self) -> None:
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""))
        rep = pc.classify(rows, legs.facts_from_json(JSON), allow=frozenset(), readback=True)
        # $c1 copies input x to an internal net: the input side is a port bit
        self.assertEqual(rep.exceptions["port-buffer"], 1)
        self.assertEqual([d.name for d in rep.defects], ["$c2"])

    def test_located_nets_of_leg_a(self) -> None:
        rows = dump(("$lut", "$c1", A), ("$lut", "$c2", ""))
        found = legs.located_nets(rows, legs.facts_from_json(JSON))
        self.assertEqual(found, {("m", "$n1")})


class SpecificityTests(unittest.TestCase):
    def test_median_and_p90(self) -> None:
        self.assertEqual(pc.specificity([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]), (5.5, 9.0))
        self.assertEqual(pc.specificity([4]), (4.0, 4.0))
        self.assertEqual(pc.specificity([]), (0.0, 0.0))

    def test_distinct_lines_and_capped(self) -> None:
        many = "|".join(f"a.v:{n}.1-{n}.2" for n in range(1, 9))
        same = "a.v:3.1-3.4|a.v:3.9-3.12"
        rows = dump(("$lut", "$c1", many), ("$lut", "$c2", same), ("$lut", "$c3", B))
        rep = pc.classify(rows, {}, allow=frozenset())
        self.assertEqual(rep.capped, 1)
        self.assertEqual(rep.lines, [8, 1, 1])
        self.assertEqual((rep.median, rep.p90), (1.0, 8.0))


class PlaceTests(unittest.TestCase):
    PLACE = """Netlist_File: x.net Netlist_ID: 1
Array size: 4 x 4 logic blocks

#block name\tx\ty\tsubblk\tblock number
#----------\t--\t--\t------\t------------
in[0]\t0\t1\t0\t#0
out:o\t0\t2\t0\t#1
n1@a.v:3\t1\t1\t0\t#2
$abc$7\t1\t2\t0\t#3
"""

    def test_pad_resolved_block_and_unresolved_block(self) -> None:
        ports = frozenset({"in[0]", "o"})
        rep = pc.place_check(self.PLACE, set(), ports, allow=pc.DEFAULT_ALLOW)
        self.assertEqual(rep.exceptions["pad"], 2)
        self.assertEqual([d.name for d in rep.defects], ["$abc$7"])
        self.assertEqual(rep.objects, {"block": 2})
        self.assertEqual(rep.blocks, 4)

    def test_pad_not_allowed_is_a_defect(self) -> None:
        ports = frozenset({"in[0]", "o"})
        rep = pc.place_check(self.PLACE, set(), ports, allow=frozenset())
        self.assertEqual(len(rep.defects), 3)

    def test_block_resolved_by_mapping(self) -> None:
        rep = pc.place_check(
            self.PLACE, {"$abc$7"}, frozenset({"in[0]", "o"}), allow=pc.DEFAULT_ALLOW
        )
        self.assertTrue(rep.ok)


def fake_leg(*items: tuple[str, str, pc.LegReport]) -> mock._patch[mock.MagicMock]:
    return mock.patch("prov_cover.run_designs", return_value=list(items))


class CliTests(unittest.TestCase):
    def test_exit_status_follows_the_gate(self) -> None:
        good = pc.classify(dump(("$lut", "$c1", A)), {}, allow=pc.DEFAULT_ALLOW)
        bad = pc.classify(dump(("$_AND_", "$c1", "")), {}, allow=pc.DEFAULT_ALLOW)
        with fake_leg(("d", "a", good)):
            code, out, _ = run_main(pc.main, ["--legs", "a", "x.v"])
        self.assertEqual(code, pc.EXIT_OK)
        self.assertIn("coverage", out)
        with fake_leg(("d", "a", good), ("e", "b", bad)):
            code, out, _ = run_main(pc.main, ["--legs", "a", "x.v"])
        self.assertEqual(code, pc.EXIT_FAIL)
        self.assertIn("$c1", out)

    def test_report_json_written(self) -> None:
        import tempfile

        good = pc.classify(dump(("$lut", "$c1", A)), {}, allow=pc.DEFAULT_ALLOW)
        with tempfile.TemporaryDirectory() as tmp, fake_leg(("d", "a", good)):
            path = Path(tmp) / "report.json"
            code, _, _ = run_main(pc.main, ["--legs", "a", "--report", str(path), "x.v"])
            data = json.loads(path.read_text())
        self.assertEqual(code, pc.EXIT_OK)
        self.assertEqual(data["designs"][0]["design"], "d")
        self.assertIn("strict", data["designs"][0]["legs"]["a"])
        self.assertEqual(set(data["designs"][0]["legs"]["a"]["exceptions"]), set(pc.EXCEPTIONS))

    def test_expect_file_excepts_a_known_defect(self) -> None:
        import tempfile

        good = pc.classify(dump(("$lut", "$c1", A)), {}, allow=pc.DEFAULT_ALLOW)
        bad = pc.classify(dump(("$_AND_", "$c1", "")), {}, allow=pc.DEFAULT_ALLOW)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "expect.txt"
            path.write_text("# known\nb e unresolved/$_AND_ 1 port bits carry no location\n")
            args = ["--legs", "a", "--expect", str(path), "x.v"]
            with fake_leg(("d", "a", good), ("e", "b", bad)):
                code, out, _ = run_main(pc.main, args)
            self.assertEqual(code, pc.EXIT_OK)
            self.assertIn("1 excepted", out)
            with fake_leg(("d", "a", good), ("e", "b", good)):  # listed but passes: stale
                code, out, _ = run_main(pc.main, args)
            self.assertEqual(code, pc.EXIT_FAIL)
            self.assertIn("stale expect line: b e", out)
            with fake_leg(("d", "a", bad), ("e", "b", bad)):  # an unlisted failure still fails
                code, _, _ = run_main(pc.main, args)
            self.assertEqual(code, pc.EXIT_FAIL)

    def test_expect_line_needs_id_count_and_reason(self) -> None:
        for text in ("b only_two\n", "b d id 1\n", "b d id many reason\n"):
            with self.assertRaises(pc.ToolError, msg=text):
                pc.parse_expect(text)
        table = pc.parse_expect("b d x/y 3 why not\nb d x/z 1 other\n")
        self.assertEqual(table[("d", "b")], {"x/y": (3, "why not"), "x/z": (1, "other")})

    def test_flow_error_is_never_excepted(self) -> None:
        with self.assertRaises(pc.ToolError):
            pc.parse_expect("a d flow-error 1 tool broke\n")
        rep = pc.LegReport()
        rep.defects.append(pc.Defect("flow-error", "", "d", "boom"))
        bad, excepted, _ = pc.judge([("d", "a", rep)], {})
        self.assertEqual((bad, excepted), (1, 0))


def lut_rep(n: int, note: str = "") -> pc.LegReport:
    """A leg report with n unlocated LUT defects of one note."""
    rep = pc.LegReport()
    rep.defects += [pc.Defect("unresolved", "m", f"$c{i}", "$sop", note) for i in range(n)]
    return rep


class JudgeTests(unittest.TestCase):
    D1 = "unresolved/lut/drives-output-port"

    def test_listed_defect_alone_is_excepted(self) -> None:
        expect = pc.parse_expect(f"b d {self.D1} 2 D1\n")
        bad, excepted, problems = pc.judge([("d", "b", lut_rep(2, "drives-output-port"))], expect)
        self.assertEqual((bad, excepted, problems), (0, 1, []))

    def test_extra_unlisted_loss_on_a_listed_leg_fails(self) -> None:
        expect = pc.parse_expect(f"b d {self.D1} 2 D1\n")
        rep = lut_rep(2, "drives-output-port")
        rep.defects.append(pc.Defect("unresolved", "m", "$x", "single_port_ram"))
        self.assertEqual(pc.judge([("d", "b", rep)], expect)[:2], (1, 0))

    def test_more_of_a_listed_defect_fails_and_fewer_is_stale(self) -> None:
        expect = pc.parse_expect(f"b d {self.D1} 2 D1\n")
        self.assertEqual(
            pc.judge([("d", "b", lut_rep(3, "drives-output-port"))], expect)[:2], (1, 0)
        )
        bad, _, problems = pc.judge([("d", "b", lut_rep(1, "drives-output-port"))], expect)
        self.assertEqual(bad, 0)
        self.assertTrue(any("lists 2, found 1" in p for p in problems))

    def test_a_listed_leg_that_did_not_run_is_stale(self) -> None:
        expect = pc.parse_expect(f"b d {self.D1} 2 D1\n")
        good = pc.classify(dump(("$lut", "$c1", A)), {}, allow=pc.DEFAULT_ALLOW)
        _, _, problems = pc.judge([("d", "a", good)], expect)
        self.assertEqual(problems, ["stale expect line: b d did not run"])

    def test_mismatch_is_never_excepted(self) -> None:
        expect = pc.parse_expect(f"b d {self.D1} 1 D1\n")
        rep = lut_rep(1, "drives-output-port")
        rep.mismatches.append("port-buffer: 1 in the dump, 2 in the written file")
        self.assertEqual(pc.judge([("d", "b", rep)], expect)[:2], (1, 0))


class ReadBackOnlyTests(unittest.TestCase):
    """P-3: the exception rules apply to a read-back dump only; in memory every unlocated object is
    a loss."""

    def test_unlocated_identity_lut_at_a_port_bit_fails_leg_a(self) -> None:
        rows = dump(("$lut", "$c1", ""))
        f = facts({"$c1": cell("$lut", ("n1",), ("o",), True)}, port_out=("o",))
        rep = pc.classify(rows, f, allow=pc.DEFAULT_ALLOW)
        self.assertFalse(rep.ok)
        self.assertEqual(rep.exceptions["port-buffer"], 0)
        again = pc.classify(rows, f, allow=pc.DEFAULT_ALLOW, readback=True)
        self.assertTrue(again.ok)

    def test_unlocated_const_and_not_helper_fail_in_memory(self) -> None:
        rows = dump(("$_CONST0_", "$c1", ""), ("$sop", "$c2", ""))
        f = facts({"$c2": cell("$sop", (), ("gnd",))})
        rep = pc.classify(rows, f, allow=frozenset())
        self.assertEqual(len(rep.defects), 2)
        self.assertEqual(sum(rep.exceptions.values()), 0)
        rep = pc.classify(rows, f, allow=frozenset(), readback=True)
        self.assertTrue(rep.ok)


class ConstTableTests(unittest.TestCase):
    def test_dropped_real_table_is_not_a_const_table(self) -> None:
        # two tables folded away, one generator net: only one can be the generator's
        rows = dump(("$sop", "$c1", ""), ("$sop", "$c2", ""))
        mod = pc.Mod({}, generators=frozenset({"gnd"}))
        rep = pc.classify(rows, {"m": mod}, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["const-table"], 1)
        self.assertEqual(len(rep.defects), 1)

    def test_table_with_inputs_is_not_a_const_table(self) -> None:
        rows = dump(("$sop", "$c1", ""))
        f = facts({"$c1": cell("$sop", ("a",), ("gnd",))})
        rep = pc.classify(rows, f, allow=frozenset(), readback=True)
        self.assertEqual(rep.exceptions["const-table"], 0)
        self.assertEqual(len(rep.defects), 1)


class VersusTests(unittest.TestCase):
    def _pair(self, after: tuple[tuple[str, str, str], ...]) -> pc.LegReport:
        first = pc.classify(
            dump(("$lut", "$c1", A), ("$lut", "$c2", A), ("$_DFF_P_", "$c3", A)),
            {},
            allow=frozenset(),
        )
        second = pc.classify(dump(*after), {}, allow=pc.DEFAULT_ALLOW, inherits=True)
        pc.compare_counts(first, second)
        return second

    def test_same_counts_pass(self) -> None:
        rep = self._pair((("$sop", "$c1", A), ("$sop", "$c2", A), ("$_DFF_P_", "$c3", A)))
        self.assertEqual(rep.versus["lut"], (2, 2))
        self.assertTrue(rep.ok)
        self.assertIn("a-vs-b lut 2->2", pc.render_table([("d", "b", rep)]))

    def test_fewer_objects_is_a_count_drop(self) -> None:
        rep = self._pair((("$sop", "$c1", A), ("$_DFF_P_", "$c3", A)))
        self.assertEqual([d.kind for d in rep.defects], ["count-drop"])  # one per lost object
        self.assertEqual(pc.defect_id(rep.defects[0]), "count-drop/lut")

    def test_only_modules_the_second_leg_holds_count(self) -> None:
        first = pc.classify(
            pc.parse_dump("t|node:$lut|$c1|a.v:1.1-1.2\nu|node:$lut|$c2|a.v:1.1-1.2\n"),
            {},
            allow=frozenset(),
        )
        second = pc.classify(pc.parse_dump("t|node:$sop|$c1|a.v:1.1-1.2\n"), {}, allow=frozenset())
        pc.compare_counts(first, second)
        self.assertEqual(second.versus["lut"], (1, 1))


class LegsTests(unittest.TestCase):
    def _args(self, legs_arg: str) -> mock.MagicMock:
        return mock.MagicMock(legs=legs_arg, allow_set=pc.DEFAULT_ALLOW)

    def _run(self, legs_arg: str, fail_second: bool) -> list[tuple[str, pc.LegReport]]:
        calls = []

        def run(odin3: str, script: str, work: Path, flags: tuple[str, ...] = ()) -> str:
            calls.append(script)
            if fail_second and len(calls) == 2:
                raise pc.ToolError("read_blif broke")
            return ""

        dumped = legs.Dumped(dump(("$lut", "$c1", A)), {}, "")
        with (
            mock.patch("legs.run_odin3", run),
            mock.patch("legs.dump_state", return_value=dumped),
            mock.patch("legs.parse_techlibs", return_value={}),
            mock.patch("legs.written_counts", return_value={}),
            mock.patch.object(Path, "read_text", return_value=".model t\n"),
        ):
            return legs.lut_legs(self._args(legs_arg), "odin3", Path("d.v"), Path("."))

    def test_leg_a_only_when_requested(self) -> None:
        self.assertEqual([n for n, _ in self._run("b", False)], ["b"])
        self.assertEqual([n for n, _ in self._run("a,b", False)], ["a", "b"])

    def test_a_failure_in_leg_b_keeps_leg_a(self) -> None:
        got = self._run("a,b", True)
        self.assertEqual([n for n, _ in got], ["a", "b"])
        self.assertTrue(got[0][1].ok)
        self.assertEqual(got[1][1].defects[0].kind, "flow-error")

    def test_nothing_compared_fails(self) -> None:
        with fake_leg():
            code, _, err = run_main(pc.main, ["--legs", "a", "x.v"])
        self.assertEqual(code, pc.EXIT_FAIL)
        self.assertIn("nothing", err)

    def test_bad_allow_is_a_usage_error(self) -> None:
        code, _, err = run_main(pc.main, ["--allow", "bogus", "--legs", "a", "x.v"])
        self.assertEqual(code, pc.EXIT_ERROR)
        self.assertIn("bogus", err)


if __name__ == "__main__":
    unittest.main()
