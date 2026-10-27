"""V1.2.0 Task 10: the Q and K lines of `odin3-prov-post` (tools/flow/prov_why.py; spec 3.6).

The fixture under tests/tools/fixtures/prov_why is a hand-made flow directory (`gen_fixture.py`):
one logic CLB of seven atoms with a pair of every class (n1..n6 LUTs, q1 a FF), a CLB of one LUT
(o1), an I/O cluster, a Norns census (`census.tsv`) and a four-path attribution. Each test works
on a copy in a temporary directory that the test removes at its end (it removes only the directory
it made); set KEEP_TMP=1 to keep them for debugging.
"""

from __future__ import annotations

import json
import os
import random
import shutil
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import name_map_v2
import prov_post
import prov_why

from .test_names_v2 import CLI_ENV, ODIN3

FIX = Path(__file__).resolve().parent / "fixtures" / "prov_why"
CENSUS = FIX / "census.tsv"
WORDS = FIX / "words" / "members.json"
ARCH = FIX / "arch.xml"
NAMES = "why.blif.names"


def scratch(case: unittest.TestCase) -> Path:
    """A new temporary directory that the test removes at its end (KEEP_TMP=1 keeps it)."""
    root = Path(tempfile.mkdtemp(prefix="provwhy-test-"))
    if not os.environ.get("KEEP_TMP"):
        case.addCleanup(shutil.rmtree, root, True)
    return root


def fresh(case: unittest.TestCase) -> Path:
    work = scratch(case) / "flow"
    shutil.copytree(FIX / "flow", work)
    return work


def run(flow: Path, census: Path | None = CENSUS, **extra: object) -> prov_post.PostResult:
    ns = SimpleNamespace(map=None, net=None, place=None, blif=None, top=None, run_dir=None,
                         census=census, census_net_sha256=None, attribution=None,
                         pack_objective=None, words=WORDS, arch=None)
    for key, value in extra.items():
        setattr(ns, key, value)
    return prov_post.extend_flow_dir(flow, ns)  # type: ignore[arg-type]


def parsed(flow: Path) -> name_map_v2.NameMap:
    return name_map_v2.read_name_map(flow / "odin3" / NAMES)


def q_of(nm: name_map_v2.NameMap, number: int) -> name_map_v2.MapWhy:
    (found,) = [w for w in nm.whys if w.record == number]
    return found


def logic_clbs(nm: name_map_v2.NameMap) -> list[int]:
    """Record numbers of the CLBs that have a Q line (the CLB records are the parents of sites)."""
    sites = [r for r in nm.records if r.level == "site"]
    clbs = {s.parents[0] for s in sites}
    return sorted(w.record for w in nm.whys if w.record in clbs)


class Shares(unittest.TestCase):
    def test_shares_sum_to_exactly_1_000_and_are_deterministic(self) -> None:
        rng = random.Random(10)
        for _ in range(500):
            counts = {c: rng.randint(0, 40) for c in prov_why.CLASSES}
            first = prov_why.shares(counts, prov_why.CLASSES)
            self.assertEqual(first, prov_why.shares(dict(reversed(list(counts.items()))),
                                                    prov_why.CLASSES))
            if sum(counts.values()):
                self.assertEqual(sum(n for _, n in first), 1000)
                self.assertEqual(sorted(first, key=lambda i: -i[1]), first)
                self.assertEqual({k for k, v in counts.items() if v}, {k for k, _ in first})
            else:
                self.assertEqual(first, [])

    def test_largest_remainder_ties_go_in_class_order(self) -> None:
        # 1/3 each: 333 + 333 + 333, the spare thousandth goes to SC (first in order)
        got = prov_why.shares({"SC": 1, "SI": 1, "FL": 1}, prov_why.CLASSES)
        self.assertEqual(got, [("SC", 334), ("SI", 333), ("FL", 333)])
        self.assertEqual(prov_why.share_text(got), "SC:0.334,SI:0.333,FL:0.333")

    def test_a_one_in_2000_pair_keeps_its_class_at_0_000(self) -> None:
        got = prov_why.shares({"SC": 1999, "FL": 1}, prov_why.CLASSES)
        self.assertEqual(got, [("SC", 1000), ("FL", 0)])
        self.assertEqual(prov_why.share_text(got), "SC:1.000,FL:0.000")
        three = prov_why.shares({"SC": 1000, "SN": 999, "FL": 1}, prov_why.CLASSES)
        self.assertIn("FL", dict(three))
        self.assertEqual(sum(n for _, n in three), 1000)


class FixtureLines(unittest.TestCase):
    def setUp(self) -> None:
        self.flow = fresh(self)
        self.result = run(self.flow)
        self.nm = parsed(self.flow)

    def test_a_hybrid_clb_gives_several_classes_and_a_one_member_clb_gives_none(self) -> None:
        big, small = (q_of(self.nm, n) for n in logic_clbs(self.nm))
        self.assertEqual(big.why, ("FL:0.762", "SC:0.095", "SN:0.048", "SI:0.048", "OR:0.047"))
        self.assertEqual(big.members, "7")
        self.assertEqual(small.why, ())
        self.assertEqual(small.members, "1")
        self.assertEqual(small.basis[0], "why=-")

    def test_the_census_fixture_gives_r(self) -> None:
        big = q_of(self.nm, logic_clbs(self.nm)[0])
        self.assertEqual(big.basis, ("why=r", "objective=i", "gain=r", "pcrit=r", "rho=r",
                                     "slack=r", "near=r", "npaths=r"))
        self.assertEqual(big.gain, ("connectivity_timing:0.333", "unrelated:0.333", "seed:0.167",
                                    "affinity_group:0.167"))
        self.assertEqual((big.pcrit, big.objective), ("0.900", "norns-pack"))
        self.assertEqual(big.stale, "-")
        keys = [(m.member, m.pcrit, m.basis) for m in self.nm.members if m.record == big.record]
        self.assertEqual(len(keys), 7)  # every member of a logic CLB
        self.assertEqual(keys[0][2], ("pcrit=r", "rho=r", "slack=r"))

    def test_rho_slack_and_near_critical_fields_come_from_the_attribution(self) -> None:
        big, small = (q_of(self.nm, n) for n in logic_clbs(self.nm))
        self.assertEqual((big.rho, big.slack, big.near, big.npaths),
                         ("1.000", "-1.000", ("5:1", "10:1"), ("5:2", "10:3")))
        self.assertEqual((small.rho, small.slack, small.near, small.npaths),
                         ("0.970", "-0.700", ("5:1", "10:1"), ("5:1", "10:1")))
        members = [m for m in self.nm.members if m.record == big.record]
        self.assertEqual([m.member for m in members][:2], ["n1", "n2"])  # highest rho, then name
        n6 = next(m for m in members if m.member == "n6")
        self.assertEqual((n6.rho, n6.slack), ("0.500", "4.000"))

    def test_an_io_block_gets_no_q(self) -> None:
        clbs = {r.number for r in self.nm.records if r.level == "clb"}
        with_q = {w.record for w in self.nm.whys}
        self.assertGreater(len(clbs - with_q), 5)  # the pad clusters
        self.assertEqual(len(with_q & clbs), 2)
        sites = {r.parents[0]: r for r in self.nm.records if r.level == "site"}
        self.assertTrue(all(sites[c].flags[0].startswith("x=") for c in clbs))

    def test_the_lines_are_appended_after_the_a_lines_and_every_old_line_is_kept(self) -> None:
        lines = (self.flow / "odin3" / NAMES).read_text().splitlines()
        first_q = next(i for i, x in enumerate(lines) if x.startswith("Q\t"))
        self.assertTrue(all(x[0] in "AQK" for x in lines[first_q - len(self.nm.atoms):]))
        self.assertEqual(self.result.q_lines, 2)
        self.assertEqual(self.result.k_lines, 8)

    def test_names_check_accepts_the_q_and_k_lines(self) -> None:
        if not ODIN3:
            if os.environ.get("ODIN3_REQUIRE_CLI"):  # set by the CTest registration
                self.fail("ODIN3_CLI is not set: names --check cannot run under CTest")
            self.skipTest("ODIN3_CLI is not set")
        import subprocess

        done = subprocess.run([ODIN3, "-p", f"names --map {self.flow / 'odin3' / NAMES} --check"],
                              capture_output=True, text=True, env=CLI_ENV, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("bad_id=0 bad_ref=0", done.stdout)


class WithoutCensus(unittest.TestCase):
    def test_without_a_census_why_gain_and_pcrit_are_absent(self) -> None:
        flow = fresh(self)
        run(flow, census=None)
        nm = parsed(flow)
        for number in logic_clbs(nm):
            w = q_of(nm, number)
            self.assertEqual((w.why, w.gain, w.pcrit), ((), (), ""))
            self.assertEqual(w.basis[0], "why=-")
            self.assertEqual((w.basis[2], w.basis[3]), ("gain=-", "pcrit=-"))
            self.assertEqual(w.basis[4], "rho=r")  # the attribution is still read
            self.assertEqual(w.objective, "")  # no VPR log, no census: no packer is named
            self.assertEqual(w.basis[1], "objective=-")
        self.assertTrue(all(m.pcrit == "" for m in nm.members))

    def test_a_vpr_log_names_the_packer_as_inferred(self) -> None:
        flow = fresh(self)
        (flow / "vpr" / "run" / "vpr.out").write_text("# Packing\nBegin packing 'why.blif'.\n")
        run(flow, census=None)
        nm = parsed(flow)
        w = q_of(nm, logic_clbs(nm)[0])
        self.assertEqual((w.objective, w.basis[1]), ("vpr-pack", "objective=i"))

    def test_a_stated_pack_objective_is_recorded(self) -> None:
        flow = fresh(self)
        run(flow, pack_objective="norns-pack:beta=4")
        nm = parsed(flow)
        w = q_of(nm, logic_clbs(nm)[0])
        self.assertEqual((w.objective, w.basis[1]), ("norns-pack:beta=4", "objective=r"))

    def test_without_an_attribution_the_post_route_fields_are_absent(self) -> None:
        flow = fresh(self)
        (flow / "timing" / "attribution.json").rename(flow / "timing" / "attribution.moved")
        run(flow)
        nm = parsed(flow)
        w = q_of(nm, logic_clbs(nm)[0])
        self.assertEqual((w.rho, w.slack, w.near, w.npaths), ("", "", (), ()))
        self.assertEqual(w.basis[4:], ("rho=-", "slack=-", "near=-", "npaths=-"))
        self.assertEqual(w.why[0], "FL:0.762")


class PackingDiffers(unittest.TestCase):
    def check_absent(self, flow: Path, census: Path, **extra: object) -> None:
        with self.assertLogs("odin3", level="WARNING") as logs:
            run(flow, census=census, **extra)
        self.assertTrue(any("WARNING" in line for line in logs.output), logs.output)
        nm = parsed(flow)
        for number in logic_clbs(nm):
            w = q_of(nm, number)
            self.assertEqual((w.why, w.gain, w.pcrit), ((), (), ""))
            self.assertNotIn("r", {b.split("=")[1] for b in w.basis[:1] + w.basis[2:4]})
            self.assertEqual((w.objective, w.basis[1]), ("", "objective=-"))

    def census_in(self, metrics: object = "keep", header: str | None = None) -> Path:
        """A census in a new directory: with the fixture's metrics.json ("keep"), a metrics.json
        holding `metrics` (a dict), or none (None); `header` becomes a '# net_sha256=' line."""
        where = scratch(self)
        text = CENSUS.read_text()
        (where / "census.tsv").write_text(f"# net_sha256={header}\n{text}" if header else text)
        if metrics == "keep":
            shutil.copy(FIX / "metrics.json", where / "metrics.json")
        elif metrics is not None:
            (where / "metrics.json").write_text(json.dumps(metrics))
        return where / "census.tsv"

    def check_used(self, flow: Path, census: Path, **extra: object) -> None:
        run(flow, census=census, **extra)
        nm = parsed(flow)
        self.assertEqual(q_of(nm, logic_clbs(nm)[0]).basis[0], "why=r")

    def test_a_net_sha256_mismatch_gives_absent_with_a_warning(self) -> None:
        self.check_absent(fresh(self), self.census_in({"net_sha256": "0" * 64}))

    def test_the_sha_is_read_from_the_metrics_json_next_to_the_census(self) -> None:
        self.check_used(fresh(self), self.census_in())  # the fixture's metrics.json names the .net

    def test_the_sha_option_overrides_the_metrics_and_the_header(self) -> None:
        self.check_absent(fresh(self), CENSUS, census_net_sha256="1" * 64)
        good = json.loads((FIX / "metrics.json").read_text())["net_sha256"]
        wrong = self.census_in({"net_sha256": "2" * 64}, header="3" * 64)
        self.check_used(fresh(self), wrong, census_net_sha256=good.upper())

    def test_a_census_header_is_the_last_source(self) -> None:
        good = json.loads((FIX / "metrics.json").read_text())["net_sha256"]
        self.check_used(fresh(self), self.census_in(None, header=good))
        self.check_absent(fresh(self), self.census_in(None, header="4" * 64))
        # a metrics.json that has a sha wins over the header
        self.check_absent(fresh(self), self.census_in({"net_sha256": "5" * 64}, header=good))

    def test_a_census_with_no_sha_from_any_source_is_unchecked(self) -> None:
        self.check_absent(fresh(self), self.census_in(None))
        self.check_absent(fresh(self), self.census_in({"net_sha256": None}))  # no .net kept
        self.check_absent(fresh(self), self.census_in({"rc": 0}))


class CensusJoin(unittest.TestCase):
    def test_a_cseq_that_maps_to_no_clb_changes_nothing(self) -> None:
        # cseq 9 holds the atom `zz`, which is in no block of the .net; cseq 3 never closed
        flow = fresh(self)
        run(flow)
        nm = parsed(flow)
        gains = [q_of(nm, n).gain for n in logic_clbs(nm)]
        self.assertEqual(gains[1], ("seed:1.000",))  # cseq 2 -> the o1 CLB
        self.assertNotIn("zz", {m.member for m in nm.members})

    def test_a_clb_over_two_clusters_or_a_destroyed_one_has_no_gain(self) -> None:
        census = prov_why.read_census(CENSUS)
        spec = prov_why.JoinSpec(1, ["n1", "n2"], classes=True)
        census.cseq_of["n2"] = 7
        self.assertEqual(prov_why._gain(spec, census, ["n1", "n2"], None), [])
        census.cseq_of["n2"] = 1
        self.assertNotEqual(prov_why._gain(spec, census, ["n1", "n2"], None), [])
        del census.cseq_of["n2"]  # the cluster that added it was never closed
        self.assertEqual(prov_why._gain(spec, census, ["n1", "n2"], None), [])

    def test_a_cluster_with_atoms_outside_the_block_gives_no_gain(self) -> None:
        census = prov_why.read_census(CENSUS)
        spec = prov_why.JoinSpec(1, ["n1"], classes=True)
        census.cseq_of["n1"] = 9  # cseq 9 holds `zz`, which the block does not
        self.assertEqual(
            prov_why._gain(spec, census, ["n1"], {1: frozenset({"n1"})}), [])


class Paths(unittest.TestCase):
    """rho and the near-critical counts on a synthetic path report."""

    @staticmethod
    def report(arrivals: list[float], atoms: list[list[str]], requested: int = 1000,
               in_report: int | None = None) -> prov_why.PathReport:
        paths = [{"arrival_ns": a, "slack_ns": 10 - a, "nodes": [{"atom": x} for x in n]}
                 for a, n in zip(arrivals, atoms, strict=True)]
        timing = {"paths_requested": requested,
                  "paths_in_report": len(paths) if in_report is None else in_report}
        built = prov_why.PathReport.from_attribution({"timing": timing, "paths": paths})
        assert built is not None
        return built

    def test_rho_is_the_largest_arrival_ratio_and_zero_off_every_path(self) -> None:
        rep = self.report([10.0, 9.5, 9.0, 5.0], [["a", "b"], ["b", "c"], ["c"], ["d"]])
        self.assertEqual([rep.rho_of(x) for x in "abcd"], [1.0, 1.0, 0.95, 0.5])
        self.assertEqual(rep.rho_of("zz"), 0.0)
        crit = rep.crit_of(["c", "d"])
        self.assertEqual((crit.rho, crit.slack), (0.95, 0.5))
        self.assertEqual(rep.crit_of(["zz"]).slack, None)

    def test_near_critical_counts_at_5_and_10_percent_with_the_edge_included(self) -> None:
        rep = self.report([10.0, 9.5, 9.0, 8.99], [["a"], ["b"], ["c"], ["d"]])
        self.assertEqual(rep.crit_of(["b"]).near, {5: True, 10: True})
        self.assertEqual(rep.crit_of(["c"]).near, {5: False, 10: True})
        self.assertEqual(rep.crit_of(["d"]).near, {5: False, 10: False})
        both = rep.crit_of(["a", "b", "c", "d"])
        self.assertEqual(both.npaths, {5: 2, 10: 3})
        self.assertEqual(both.capped, {5: False, 10: False})

    def test_a_capped_report_gives_a_lower_bound(self) -> None:
        rep = self.report([10.0, 9.9, 9.6, 9.2], [["a"], ["b"], ["c"], ["d"]], requested=4)
        # the report holds its 4 paths and the last (9.2) is inside the 10% margin, not the 5% one
        self.assertEqual(rep.capped, {5: False, 10: True})
        crit = rep.crit_of(["a", "b", "c", "d"])
        self.assertEqual(crit.npaths, {5: 3, 10: 4})
        spec = prov_why.JoinSpec(1, ["a", "b", "c", "d"])
        line = prov_why.q_line(spec, "", "", "", crit, {})
        self.assertIn("\t5:3,10:>=4\t", line)
        tight = self.report([10.0, 9.9, 9.8, 9.7], [["a"], ["b"], ["c"], ["d"]], requested=4)
        self.assertEqual(tight.capped, {5: True, 10: True})
        line = prov_why.q_line(spec, "", "", "", tight.crit_of(["a", "b", "c", "d"]), {})
        self.assertIn("\t5:>=4,10:>=4\t", line)

    def test_a_report_cut_inside_one_margin_only(self) -> None:
        rep = self.report([10.0, 9.9, 9.2], [["a"], ["b"], ["c"]], requested=3)
        self.assertEqual(rep.capped, {5: False, 10: True})
        line = prov_why.q_line(prov_why.JoinSpec(1, ["a", "b", "c"]), "", "", "",
                               rep.crit_of(["a", "b", "c"]), {})
        self.assertIn("\t5:2,10:>=3\t", line)

    def test_a_report_below_its_cap_is_exact(self) -> None:
        rep = self.report([10.0, 9.9], [["a"], ["b"]], requested=100)
        self.assertEqual(rep.capped, {5: False, 10: False})

    def test_paths_kept_below_those_in_the_report_count_as_capped(self) -> None:
        rep = self.report([10.0, 9.9], [["a"], ["b"]], requested=100, in_report=50)
        self.assertEqual(rep.capped, {5: True, 10: True})


class R4censusReproduction(unittest.TestCase):
    """prov_post reproduces the class shares of Norns' r4census.py on the committed design."""

    def test_the_pair_counts_equal_r4census(self) -> None:
        ref = json.loads((FIX / "r4census_reference.json").read_text())
        flow = fresh(self)
        run(flow)
        nm = parsed(flow)
        counts: dict[str, dict[str, int]] = {}
        net = prov_why.build_netlist(flow / "odin3" / "why.blif",
                                     prov_why.net_output_ports(flow / "vpr" / "run" / "why.net"))
        census = prov_why.read_census(CENSUS)
        names = prov_post.Resolver(nm, None)
        word = prov_post.word_labeler(nm, names, prov_post.read_words(WORDS))
        for clb, info in ref["clbs"].items():
            counts[clb] = prov_why.pair_counts(net, census.pin_crit, word, info["members"])
        for clb, info in ref["clbs"].items():
            want = {k: v for k, v in info["counts"].items() if v}
            self.assertEqual(counts[clb], want, clb)
        # and the lines of the map carry the same shares
        big = q_of(nm, logic_clbs(nm)[0])
        want_text = prov_why.share_text(prov_why.shares(
            ref["clbs"]["clb0"]["counts"], prov_why.CLASSES))
        self.assertEqual(",".join(big.why), want_text)
        total = ref["total"]["pairs"]
        for item in big.why:
            cls, share = item.split(":")
            self.assertAlmostEqual(float(share), ref["total"]["counts"][cls] / total, delta=0.0011)

    def test_the_word_label_is_the_declarator_of_the_first_leaf_without_a_words_file(self) -> None:
        flow = fresh(self)
        nm = parsed(flow)
        word = prov_post.word_labeler(nm, prov_post.Resolver(nm, None))
        self.assertEqual((word("n1"), word("n6")), ("c2", "c2"))
        self.assertIsNone(word("n2"))
        self.assertIsNone(word("not_an_atom"))

    def test_the_word_label_is_read_from_a_norns_words_file(self) -> None:
        # the real members.json format: {atom: {word: <id or null>, bit, bus, kind, ngroups}}
        words = prov_post.read_words(WORDS)
        self.assertEqual(words, {"n1": "w1", "n6": "w1"})  # atoms with a null word are absent
        flow = fresh(self)
        nm = parsed(flow)
        word = prov_post.word_labeler(nm, prov_post.Resolver(nm, None), words)
        self.assertEqual((word("n1"), word("n6"), word("n2")), ("w1", "w1", None))
        # a word differing from the declarator splits a pair the declarator would join
        other = scratch(self) / "members.json"
        other.write_text(json.dumps({"n1": {"word": "wa"}, "n6": {"word": "wb"}}))
        split = prov_post.word_labeler(nm, prov_post.Resolver(nm, None),
                                       prov_post.read_words(other))
        self.assertNotEqual(split("n1"), split("n6"))

    def test_the_why_basis_says_whether_the_word_labels_were_read_or_inferred(self) -> None:
        with_words, without = fresh(self), fresh(self)
        run(with_words)
        run(without, words=None)
        a, b = parsed(with_words), parsed(without)
        wa, wb = (q_of(x, logic_clbs(x)[0]) for x in (a, b))
        self.assertEqual((wa.basis[0], wb.basis[0]), ("why=r", "why=i"))
        self.assertEqual(wa.why, wb.why)  # the fixture's declarator and word agree

    def test_a_words_file_that_is_not_a_norns_one_is_refused(self) -> None:
        bad = scratch(self) / "members.json"
        bad.write_text("[1, 2]")
        with self.assertRaises(prov_post.PostError):
            prov_post.read_words(bad)
        bad.write_text("{")
        with self.assertRaises(prov_post.PostError):
            prov_post.read_words(bad)

    def test_precedence_a_pair_that_fits_sc_and_si_is_sc(self) -> None:
        net = prov_why.build_netlist(FIX / "flow" / "odin3" / "why.blif",
                                     prov_why.arch_output_ports(ARCH))
        self.assertIn(("n1", "n2"), net.shared)  # both read `a`
        self.assertIn(("n1", "n2"), net.series)  # and n1 drives n2
        crit = {"n2": {"n1": 0.9}}
        self.assertEqual(prov_why.classify(net, crit, lambda _a: None, "n1", "n2"), "SC")
        self.assertEqual(prov_why.classify(net, {"n2": {"n1": 0.1}}, lambda _a: None, "n2", "n1"),
                         "SN")


class HardBlockOutputs(unittest.TestCase):
    """The output pins of a hard block come from the architecture or the .net, not from a name."""

    BLIF = FIX / "flow" / "odin3" / "why.blif"
    NET = FIX / "flow" / "vpr" / "run" / "why.net"

    def test_a_memory_dout_gets_a_driver_from_the_net_ports(self) -> None:
        net = prov_why.build_netlist(self.BLIF, prov_why.net_output_ports(self.NET))
        self.assertEqual(net.driver["md"], "md")  # the block is named after its output net
        self.assertNotIn("md", net.kind)  # not a pair member: a memory
        self.assertIn("md", net.driver)

    def test_a_memory_dout_gets_a_driver_from_the_architecture(self) -> None:
        net = prov_why.build_netlist(self.BLIF, prov_why.arch_output_ports(ARCH))
        self.assertEqual(net.driver["md"], "md")

    def test_the_two_sources_give_the_same_netlist(self) -> None:
        a = prov_why.build_netlist(self.BLIF, prov_why.net_output_ports(self.NET))
        b = prov_why.build_netlist(self.BLIF, prov_why.arch_output_ports(ARCH))
        self.assertEqual((a.series, a.shared, a.bcast, a.driver, a.kind),
                         (b.series, b.shared, b.bcast, b.driver, b.kind))

    def test_no_name_heuristic_a_port_that_is_not_an_output_is_not_one(self) -> None:
        # `dout` is an input of a model that does not list it: no driver, and a WARNING
        with self.assertLogs("odin3", level="WARNING") as logs:
            net = prov_why.build_netlist(self.BLIF, {"adder": frozenset(("cout", "sumout"))})
        self.assertNotIn("md", net.driver)
        self.assertTrue(any("single_port_ram" in line for line in logs.output))
        # the old name rule would have taken `out1`/`q`; ports say what is an output
        ports = {"m": frozenset(("dout",))}
        self.assertTrue(prov_why.is_output_formal(ports, "m", "dout[3]"))
        self.assertFalse(prov_why.is_output_formal(ports, "m", "out"))
        self.assertTrue(prov_why.is_output_formal({"": frozenset(("q",))}, "any", "q"))

    def test_prov_post_option_arch_gives_the_same_lines_as_the_net_ports(self) -> None:
        a, b = fresh(self), fresh(self)
        run(a)
        run(b, arch=ARCH)
        self.assertEqual((a / "odin3" / NAMES).read_text(), (b / "odin3" / NAMES).read_text())


class NonLogicNames(unittest.TestCase):
    """A block is not a logic CLB by exact architecture names, never by a substring."""

    @staticmethod
    def logic(kind: str, stems: dict[str, str]) -> bool:
        return prov_post.Cluster("c", kind, [], list(stems), stems=stems).logic

    def test_exact_names_of_the_architecture(self) -> None:
        for kind, stem in (("memory", "mem_512x64_sp"), ("mult_36", "mult_18x18"),
                           ("clb", "single_port_ram"), ("clb", "mem_2048x8_dp"),
                           ("clb", "multiply"), ("memory", "lut6")):
            self.assertFalse(self.logic(kind, {"a": stem}), (kind, stem))

    def test_a_substring_does_not_make_a_block_non_logic(self) -> None:
        for kind, stem in (("clb", "lut6"), ("program_clb", "ff"), ("dsp_helper", "lut6"),
                           ("clb", "ramp_lut"), ("clb", "multiplexer")):
            self.assertTrue(self.logic(kind, {"a": stem}), (kind, stem))


class OdinJoins(unittest.TestCase):
    """The objective-only Q lines Odin III wrote get the post-route fields from the attribution."""

    def test_an_odin_q_is_filled_in_place_and_keeps_its_objective(self) -> None:
        flow = fresh(self)
        path = flow / "odin3" / NAMES
        text = path.read_text()
        # r17 (n1's record, a SOURCE) gets a join above it: a derived record over n1 and n2
        # r21 is a join of r16 and r17 (the sources of n4 and n5); n4 and n5 are bound to it
        join = "R\tr21\tD\tlut\tcoarsen:stmt\tt2.1\tr16,r17\t\t\tjoin\n"
        text = text.replace("F\tf1\twhy.v\n", join + "F\tf1\twhy.v\n")
        text = text.replace("net\tn4\tr16", "net\tn4\tr21")
        text = text.replace("net\tn5\tr17", "net\tn5\tr21")
        first_run = "T\tt1\tread_verilog\t\t\n"
        text = text.replace(first_run, first_run + "T\tt2\topt\t\t\n")
        basis = "why=-,objective=r,gain=-,pcrit=-,rho=-,slack=-,near=-,npaths=-"
        old = "Q\tr21\t\topt" + "\t" * 8 + basis + "\t-"
        path.write_text(text + old + "\n")
        before = path.read_text().splitlines()
        run(flow, census=None)
        after = path.read_text().splitlines()
        self.assertNotIn(old, after)
        self.assertEqual(len(before) - 1, len([x for x in after if x in before]))
        nm = parsed(flow)
        w = q_of(nm, 21)
        self.assertEqual(w.objective, "opt")
        self.assertEqual((w.why, w.gain, w.pcrit), ((), (), ""))
        self.assertEqual(w.basis[:4], ("why=-", "objective=r", "gain=-", "pcrit=-"))
        self.assertEqual((w.rho, w.slack, w.members), ("0.970", "-0.700", "2"))
        self.assertEqual([m.member for m in nm.members if m.record == 21], ["n4", "n5"])
        self.assertEqual(len([x for x in nm.whys if x.record == 21]), 1)


if __name__ == "__main__":
    unittest.main()
