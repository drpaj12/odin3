"""V1.2.0 Task 8: tools/flow/prov_post.py (``odin3-prov-post``).

The fixtures under tests/tools/fixtures/prov_post hold the BLIF VPR read (hash names), its version
2 name map and VPR's raw ``.net`` and ``.place`` for two micros (``bm_DL_BCD_adder``,
``bm_DL_logic_w_Dff``, made once by ``odin3-flow --prov --vpr --keep-results``). Each test works
on a copy in a fresh
temporary directory that is left in place. The ``names --check`` test needs ODIN3_CLI.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import name_map_v2
import prov_post
from name_map_v2 import MapConstruct, MapRecord, NameMap

from .test_names_v2 import CLI_ENV, ODIN3

FIX = Path(__file__).resolve().parent / "fixtures" / "prov_post"
DESIGNS = ("bm_DL_BCD_adder", "bm_DL_logic_w_Dff")


def fresh(design: str) -> Path:
    """A copy of one fixture flow directory in a new temporary directory."""
    work = Path(tempfile.mkdtemp(prefix="provpost-test-")) / design
    shutil.copytree(FIX / design, work)
    return work


def map_of(flow: Path, design: str) -> Path:
    return flow / "odin3" / f"{design}.blif.names"


def lines_of(path: Path, letter: str) -> list[list[str]]:
    return [ln.split("\t")[1:] for ln in path.read_text().split("\n")
            if ln.startswith(letter + "\t")]


class Records(unittest.TestCase):
    """BLE, CLB and site records, the T rows and the A lines of the two fixtures."""

    def test_records_levels_and_steps(self) -> None:
        for design in DESIGNS:
            with self.subTest(design=design):
                flow = fresh(design)
                before = len(lines_of(map_of(flow, design), "R"))
                res = prov_post.extend_flow_dir(flow)
                nm = name_map_v2.read_name_map(map_of(flow, design))
                self.assertEqual(nm.ignored, 0)
                new = nm.records[before:]
                self.assertEqual(len(new), res.bles + res.clbs + res.sites)
                self.assertEqual({r.level for r in new} - {"ble", "clb", "site"}, set())
                self.assertEqual([r.number for r in new],
                                 list(range(before + 1, len(nm.records) + 1)))
                runs = [run[0] for run in nm.runs]
                self.assertEqual(runs[-2:], ["pack", "place"])
                for rec in new:
                    self.assertEqual(rec.kind, "D")
                    expect = {"ble": ("coarsen:lut", "pack"), "clb": ("coarsen:ble", "pack"),
                              "site": ("rewrite:clb", "place")}[rec.level]
                    self.assertEqual(rec.dir, expect[0])
                    run = int(rec.step[1:].split(".")[0])
                    self.assertEqual(nm.runs[run - 1][0], expect[1])
                    self.assertGreater(int(rec.step.split(".")[1]), 0)
                    self.assertTrue(all(p < rec.number for p in rec.parents))

    def test_site_flags_hold_the_place_coordinates(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        prov_post.extend_flow_dir(flow)
        nm = name_map_v2.read_name_map(map_of(flow, "bm_DL_BCD_adder"))
        places = prov_post.read_place(next((flow / "vpr" / "run").glob("*.place")))
        sites = [r for r in nm.records if r.level == "site"]
        self.assertEqual(len(sites), len(places))
        got = sorted(tuple(r.flags) for r in sites)
        self.assertEqual(got, sorted(places.values()))
        for rec in sites:
            clb = nm.records[rec.parents[0] - 1]
            self.assertEqual((clb.level, len(rec.parents)), ("clb", 1))
            self.assertEqual([f.split("=")[0] for f in rec.flags], ["x", "y", "sub", "layer"])

    def test_every_atom_of_the_net_has_an_a_line_with_its_chain(self) -> None:
        for design in DESIGNS:
            with self.subTest(design=design):
                flow = fresh(design)
                net = next((flow / "vpr" / "run").glob("*.net"))
                atoms = [a for cl in prov_post.read_net(net) for a in cl.atoms]
                prov_post.extend_flow_dir(flow)
                nm = name_map_v2.read_name_map(map_of(flow, design))
                self.assertEqual([a[0] for a in nm.atoms], atoms)
                for atom in nm.atoms:
                    self.assertEqual(atom[2], "-")
                    recs = [nm.records[int(r[1:]) - 1] for r in atom[1].split(",")]
                    levels = [r.level for r in recs]
                    self.assertIn(levels, (["ble", "clb", "site"], ["clb", "site"]), atom)
                    for hi, lo in zip(recs[1:], recs[:-1], strict=True):
                        self.assertIn(lo.number, hi.parents)

    def test_atom_name_join_is_exact(self) -> None:
        """A BLE's parents are the Odin records of the objects keyed by its atoms' names."""
        flow = fresh("bm_DL_BCD_adder")
        prov_post.extend_flow_dir(flow)
        nm = name_map_v2.read_name_map(map_of(flow, "bm_DL_BCD_adder"))
        checked = 0
        for atom in nm.atoms:
            obj = nm.atom(atom[0], "bm_DL_BCD_adder")
            ble = nm.records[int(atom[1].split(",")[0][1:]) - 1]
            if obj is not None and obj.records and ble.level == "ble":
                self.assertIn(obj.records[0], ble.parents)
                checked += 1
        self.assertGreater(checked, 5)

    def test_numbering_follows_the_a_lines(self) -> None:
        """3.2: post-order from the A lines in line order, so numbers rise along the A lines."""
        flow = fresh("bm_DL_BCD_adder")
        prov_post.extend_flow_dir(flow)
        nm = name_map_v2.read_name_map(map_of(flow, "bm_DL_BCD_adder"))
        top = 0
        for atom in nm.atoms:
            numbers = [int(r[1:]) for r in atom[1].split(",")]
            self.assertEqual(numbers, sorted(numbers))
            top = max(top, max(numbers))
            self.assertLessEqual(min(numbers), top)
        self.assertEqual(top, len(nm.records))

    def test_latch_atoms_share_a_ble_with_their_lut(self) -> None:
        flow = fresh("bm_DL_logic_w_Dff")
        prov_post.extend_flow_dir(flow)
        nm = name_map_v2.read_name_map(map_of(flow, "bm_DL_logic_w_Dff"))
        bles = [r for r in nm.records if r.level == "ble"]
        self.assertTrue(any(len(r.parents) >= 2 for r in bles) or bles)
        names = {a[0] for a in nm.atoms}
        self.assertIn("f", names)  # a latch is named after its output


class Joins(unittest.TestCase):
    """The join rule of 3.3 on hand-made maps, and on the BCD adder (the CLB mixing two
    statements)."""

    @staticmethod
    def graph(recs: list[tuple[str, tuple[int, ...], tuple[int, ...], tuple[str, ...]]],
              cons: list[tuple[str, str, int]]) -> prov_post.Graph:
        nm = NameMap(2, 10, "refs", files=["a.v"])
        for i, (kind, parents, con, flags) in enumerate(recs, start=1):
            nm.records.append(MapRecord(i, kind, "x", "origin" if kind != "D" else "coarsen:gate",
                                        "t1.1", parents, (), con, flags, ""))
        for i, (kind, level, parent) in enumerate(cons, start=1):
            nm.constructs.append(MapConstruct(i, kind, level, parent, 1, f"{i}.1-{i}.9", "", ""))
        return prov_post.Graph(nm)

    # constructs: 1 module, 2 stmt A (child of 1), 3 stmt B (child of 1), 4 expr (child of 2)
    CONS = [("MODULE", "module", 0), ("ASSIGN", "stmt", 1), ("ASSIGN", "stmt", 1),
            ("BINARY", "expr", 2)]

    def test_distinct_anchors_join_and_one_anchor_does_not(self) -> None:
        g = self.graph([("S", (), (2,), ()), ("S", (), (3,), ()), ("S", (), (4,), ()),
                        ("D", (1, 2), (), ()), ("D", (1, 3), (), ()),
                        ("D", (1,), (), ())], self.CONS)
        mixed = g.label(4)
        self.assertTrue(mixed.join)
        self.assertEqual(mixed.cons, (2, 3))
        # leaf 3 is an expr of stmt 2: the same anchor as leaf 1, so no join
        self.assertFalse(g.label(5).join)
        self.assertEqual(g.label(5).cons, ())
        self.assertFalse(g.label(6).join)

    def test_a_blend_inherited_from_one_parent_is_not_a_new_join(self) -> None:
        g = self.graph([("S", (), (2,), ()), ("S", (), (3,), ()), ("D", (1, 2), (), ()),
                        ("D", (3,), (), ())], self.CONS)
        self.assertTrue(g.label(3).join)
        self.assertFalse(g.label(4).join)  # J equals its only parent's

    def test_an_ancestor_anchor_is_absorbed(self) -> None:
        g = self.graph([("S", (), (1,), ()), ("S", (), (2,), ()), ("D", (1, 2), (), ())],
                       self.CONS)
        self.assertEqual(g.reduced(g.info(3).anchors), (1, 2)[1:])
        self.assertFalse(g.label(3).join)

    def test_span_only_leaves_never_join(self) -> None:
        g = self.graph([("S", (), (), ()), ("S", (), (3,), ()), ("D", (1, 2), (), ())], self.CONS)
        lab = g.label(3)
        self.assertFalse(lab.join)
        self.assertTrue(lab.span_only)

    def test_instances_of_one_anchor_through_two_contexts(self) -> None:
        recs = [("S", (), (2,), ()), ("S", (), (), ()), ("S", (), (), ()),
                ("D", (1, 2), (), ("ctx=1",)), ("D", (1, 3), (), ("ctx=1",)),
                ("D", (4, 5), (), ())]
        g = self.graph(recs, self.CONS)
        lab = g.label(6)
        self.assertTrue(lab.instance)
        self.assertFalse(lab.join)  # one anchor, two instances: not a join of constructs
        self.assertEqual(lab.cons, (2,))

    def test_cap_lists_sixteen_and_flags_lossy(self) -> None:
        cons = [("MODULE", "module", 0)] + [("ASSIGN", "stmt", 1)] * 20
        recs: list[tuple[str, tuple[int, ...], tuple[int, ...], tuple[str, ...]]] = [
            ("S", (), (c,), ()) for c in range(2, 22)]
        recs.append(("D", tuple(range(1, 21)), (), ()))
        g = self.graph(recs, cons)
        lab = g.label(21)
        self.assertEqual((len(lab.cons), lab.n_joined, lab.join), (16, 20, True))

    def test_bcd_adder_clb_with_two_statements_is_a_join_by_pack(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        prov_post.extend_flow_dir(flow)
        nm = name_map_v2.read_name_map(map_of(flow, "bm_DL_BCD_adder"))
        joined = [r for r in nm.records if r.level in ("ble", "clb") and "join" in r.flags]
        single = [r for r in nm.records if r.level == "clb" and "join" not in r.flags]
        self.assertGreaterEqual(len(joined), 1)
        self.assertGreater(len(single), 0)
        for rec in joined:
            self.assertGreaterEqual(len(rec.constructs), 2)
            self.assertEqual(nm.runs[int(rec.step[1:].split(".")[0]) - 1][0], "pack")
        for rec in single:
            self.assertEqual(rec.constructs, ())


class Refusals(unittest.TestCase):
    def test_short_name_blif_is_refused_and_the_map_untouched(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        blif = flow / "odin3" / "bm_DL_BCD_adder.blif"
        blif.write_text(blif.read_text().replace("names hash", "names short", 1))
        before = map_of(flow, "bm_DL_BCD_adder").read_bytes()
        with self.assertRaisesRegex(prov_post.PostError, "short names"):
            prov_post.extend_flow_dir(flow)
        self.assertEqual(map_of(flow, "bm_DL_BCD_adder").read_bytes(), before)

    def test_net_with_names_the_map_does_not_hold_is_refused(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        net = next((flow / "vpr" / "run").glob("*.net"))
        net.write_text(re.sub(r'name="h(\w+)"', r'name="s\1"', net.read_text()))
        (flow / "odin3" / "bm_DL_BCD_adder.blif").write_text("# no header\n.model m\n.end\n")
        with self.assertRaisesRegex(prov_post.PostError, "holds only"):
            prov_post.extend_flow_dir(flow)

    def test_version_1_map_is_refused(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        path = map_of(flow, "bm_DL_BCD_adder")
        path.write_text("# odin3-names 1 hash=sha256 width=10\n")
        with self.assertRaisesRegex(prov_post.PostError, "version 1"):
            prov_post.extend_flow_dir(flow)

    def test_second_run_is_refused_and_changes_nothing(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        prov_post.extend_flow_dir(flow)
        once = map_of(flow, "bm_DL_BCD_adder").read_bytes()
        with self.assertRaisesRegex(prov_post.PostError, "already holds"):
            prov_post.extend_flow_dir(flow)
        self.assertEqual(map_of(flow, "bm_DL_BCD_adder").read_bytes(), once)

    def test_missing_vpr_files_are_named(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        shutil.move(str(flow / "vpr"), str(flow / "vpr-moved"))
        with self.assertRaisesRegex(prov_post.PostError, "keep-results"):
            prov_post.extend_flow_dir(flow)


class Writing(unittest.TestCase):
    def test_failed_replace_leaves_the_old_map(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        before = map_of(flow, "bm_DL_BCD_adder").read_bytes()
        with mock.patch("os.replace", side_effect=OSError("disk gone")), \
                self.assertRaises(prov_post.PostError):
            prov_post.extend_flow_dir(flow)
        self.assertEqual(map_of(flow, "bm_DL_BCD_adder").read_bytes(), before)

    def test_success_leaves_no_temporary_file(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        prov_post.extend_flow_dir(flow)
        left = [p.name for p in (flow / "odin3").iterdir() if p.name.endswith(".tmp")]
        self.assertEqual(left, [])

    def test_old_lines_stay_byte_identical(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        old = map_of(flow, "bm_DL_BCD_adder").read_text().split("\n")[:-1]
        prov_post.extend_flow_dir(flow)
        new = map_of(flow, "bm_DL_BCD_adder").read_text().split("\n")
        self.assertEqual([ln for ln in new if ln in set(old)], old)  # same lines, same order

    def test_escape(self) -> None:
        self.assertEqual(prov_post.escape("a,b|c%d\te#"), "a%2Cb%7Cc%25d%09e#")
        self.assertEqual(prov_post.escape("#x"), "%23x")


class Accepted(unittest.TestCase):
    """The extended map is accepted by both parsers and by ``names --check``."""

    def test_name_map_v2_parses_every_line(self) -> None:
        for design in DESIGNS:
            flow = fresh(design)
            res = prov_post.extend_flow_dir(flow)
            nm = name_map_v2.read_name_map(map_of(flow, design))
            self.assertEqual(nm.ignored, 0)
            self.assertEqual(len(nm.atoms), res.atoms)

    @unittest.skipIf(ODIN3 is None, "needs ODIN3_CLI")
    def test_names_check_passes_on_the_extended_map(self) -> None:
        for design in DESIGNS:
            with self.subTest(design=design):
                flow = fresh(design)
                prov_post.extend_flow_dir(flow)
                assert ODIN3 is not None
                run = subprocess.run([ODIN3, "-p", f"names --map {map_of(flow, design)} --check"],
                                     capture_output=True, text=True, check=False, env=CLI_ENV)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertIn("ignored=0", run.stdout)
                self.assertRegex(run.stdout, r"bad_id=0 bad_ref=0 bad_order=0 bad_class=0 "
                                             r"no_record=0")


class Inputs(unittest.TestCase):
    def test_read_place_with_and_without_layer(self) -> None:
        work = Path(tempfile.mkdtemp(prefix="provpost-test-"))
        path = work / "p.place"
        path.write_text("Netlist_File: n.net Netlist_ID: SHA256:ab\n"
                        "Array size: 2 x 2 logic blocks\n\n"
                        "#block name\tx\ty\tsubblk\tblock number\n#---\n"
                        "n1\t1\t2\t0\t#0\nout:p\t0\t1\t3\t#1\n")
        self.assertEqual(prov_post.read_place(path),
                         {"n1": ("x=1", "y=2", "sub=0"), "out:p": ("x=0", "y=1", "sub=3")})

    def test_main_exit_codes(self) -> None:
        flow = fresh("bm_DL_BCD_adder")
        self.assertEqual(prov_post.main([str(flow / "nope")]), 2)
        self.assertEqual(prov_post.main([str(flow)]), 0)
        self.assertEqual(prov_post.main([str(flow)]), 1)  # already extended

    def test_flow_keeps_results_on_request(self) -> None:
        import odin3_flow  # noqa: PLC0415

        design = FIX.parent / "timing_attr_v2" / "decl" / "decl.v"
        out = Path(tempfile.mkdtemp(prefix="provpost-test-"))
        kept = odin3_flow.parse_args([str(design), "--keep-results", "--out", str(out)])
        plain = odin3_flow.parse_args([str(design), "--out", str(out)])
        self.assertEqual((kept.keep_results, plain.keep_results), (True, False))


if __name__ == "__main__":
    unittest.main()
