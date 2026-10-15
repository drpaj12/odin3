"""Tests for tools/flatten-check/flatten_vtr.py (3A Task 5, spec 14.6): the stats parser, the
node-count check against the instance tree, the table and the cap lookup."""

from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path

import flatten_vtr as fv

PRE = """odin3: info: stats: design: modules 3, top top
odin3: info: stats: module top: ports 4, nodes 9, nets 20, wires 5
odin3: info: stats: module top: cell $and 1
odin3: info: stats: module top: cell $port_in 2
odin3: info: stats: module top: cell mid 2
odin3: info: stats: module mid: ports 3, nodes 5, nets 9, wires 3
odin3: info: stats: module mid: cell $xor 1
odin3: info: stats: module mid: cell leaf 3
odin3: info: stats: module leaf: ports 2, nodes 3, nets 4, wires 2
odin3: info: stats: module leaf: cell $not 2
odin3: info: stats: module leaf: cell $_CONST0_ 1
"""
POST_OK = """odin3: info: stats: design: modules 3, top top
odin3: info: stats: module top: ports 4, nodes 40, nets 90, wires 9
odin3: info: stats: module top: cell $and 1
odin3: info: stats: module top: cell $xor 2
odin3: info: stats: module top: cell $not 12
odin3: info: stats: module top: cell $_CONST0_ 1
odin3: info: stats: module top: cell $port_in 2
odin3: info: stats: module mid: ports 3, nodes 5, nets 9, wires 3
odin3: info: stats: module mid: cell $xor 1
odin3: info: stats: module mid: cell leaf 3
"""


class StatsTests(unittest.TestCase):
    def test_parse_stats_splits_the_passes(self) -> None:
        runs = fv.parse_stats(PRE + POST_OK)
        self.assertEqual(len(runs), 2)
        self.assertEqual(runs[0]["top"].nodes, 9)
        self.assertEqual(runs[0]["mid"].cells["leaf"], 3)
        self.assertEqual(runs[1]["top"].cells["$not"], 12)

    def test_expected_flat_counts_the_instance_tree(self) -> None:
        want = fv.expected_flat(fv.parse_stats(PRE)[0], "top")
        self.assertEqual(dict(want), {"$and": 1, "$xor": 2, "$not": 12})

    def test_node_counts_agree(self) -> None:
        pre, post = fv.parse_stats(PRE)[0], fv.parse_stats(POST_OK)[0]
        self.assertEqual(fv.node_count_problems(pre, post, "top"), [])

    def test_a_dropped_node_is_a_problem(self) -> None:
        pre = fv.parse_stats(PRE)[0]
        post = fv.parse_stats(POST_OK.replace("cell $not 12", "cell $not 11"))[0]
        problems = fv.node_count_problems(pre, post, "top")
        self.assertEqual(problems, ["node count $not: flat 11, instance tree 12"])

    def test_a_type_only_in_the_flat_module_is_a_problem(self) -> None:
        pre = fv.parse_stats(PRE)[0]
        extra = POST_OK.replace(
            "cell $port_in 2", "cell $port_in 2\nodin3: info: stats: module top: cell $or 1"
        )
        post = fv.parse_stats(extra)[0]
        self.assertEqual(
            fv.node_count_problems(pre, post, "top"), ["node count $or: flat 1, instance tree 0"]
        )


class MiscTests(unittest.TestCase):
    def test_first_model(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "g.blif"
            path.write_text("# c\n.model memset\n.inputs a\n.end\n.model other\n")
            self.assertEqual(fv.first_model(path), "memset")
            path.write_text("# nothing\n")
            with self.assertRaises(ValueError):
                fv.first_model(path)

    def test_cap_of(self) -> None:
        args = argparse.Namespace(mem_cap="3G", cap_for=["mcml.v=5G"])
        self.assertEqual(fv.cap_of(args, "bgm.v"), 3 << 30)
        self.assertEqual(fv.cap_of(args, "mcml.v"), 5 << 30)

    def test_script(self) -> None:
        text = fv.script(Path("/x/sha.v"), "sha1", "lib/vtr.o3lib")
        self.assertEqual(
            text,
            "read_techlib lib/vtr.o3lib; read_verilog --vtr-compat --nolatches /x/sha.v; "
            "hierarchy --top sha1; proc; opt; stats; flatten; check; stats",
        )

    def test_ordered_is_smallest_first(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for name, size in (("sha", 30), ("boundtop", 10), ("bgm", 20)):
                (Path(tmp) / f"{name}.v").write_text("x" * size)
            order = [p.stem for p in fv.ordered(Path(tmp), ["sha", "boundtop", "bgm"])]
        self.assertEqual(order, ["boundtop", "bgm", "sha"])

    def test_raw_lines(self) -> None:
        row = fv.Row("sha", "sha1")
        row.cost = (1000, 4300, 5, 25, 100, 400, 200, 30, 6600, 2)
        text = fv.raw_lines([row, fv.Row("none", "n")])
        self.assertEqual(
            text.splitlines(),
            [
                "design,bytes_added,records_added,nodes,pins,nets,wires,estimate",
                "sha,3300,20,100,400,200,30,6600",
            ],
        )

    def test_table_row_columns(self) -> None:
        row = fv.Row("sha", "sha1", 1.5, 120.0, 4, 0, 2)
        row.flat = fv.ModuleStats(0, 100, 200, 30)
        row.cost = (1000, 4300, 5, 25, 100, 400, 200, 30, 6600, 2)
        line = fv.table_row(row)
        self.assertIn("| sha | 1.50 | 120 | 4 | 0 | 2 | 100 | 200 | 30 | 20 | 10 | 2.00 |", line)
        self.assertTrue(line.endswith("| yes |"))
        row.problems.append("kept=1")
        self.assertTrue(fv.table_row(row).endswith("| NO |"))


if __name__ == "__main__":
    unittest.main()
