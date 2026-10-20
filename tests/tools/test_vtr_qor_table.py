"""Tests for tools/vtr-qor/qor_table.py and the ``table``/``gate``/``publish`` subcommands (3H
plan Tasks 8 and 9; spec §6, T3, T7).

The runs are synthetic: built in the test, written as a small job tree under a temporary
directory, or copied from the committed fixture tree ``fixtures/vtr_qor/table/runs`` (P, O', P'
and O on the 15 + 2 + 4 offload designs; its README records the hand-computed geomeans).
Nothing here runs VTR or writes ``~/odin3-ws/golden``: ``publish`` runs with ``HOME`` pointed at
a temporary directory.
"""

from __future__ import annotations

import json
import os
import shutil
import tempfile
import time
import unittest
from collections.abc import Mapping, Sequence
from pathlib import Path
from unittest import mock

import qor_legs
import qor_sched
import qor_table
import vtr_qor
from qor_heavy import HEAVY_DIR, HeavyError
from qor_table import Bands, GateInputs, Run, Verdict

from .helpers import FIXTURES, run_main

REPO = qor_legs.REPO_ROOT
BANDS_JSON = REPO / "tests" / "vtr" / "qor" / "bands.json"
EXPLANATIONS = REPO / "tests" / "vtr" / "qor" / "explanations.tsv"
SETS = qor_sched.load_sets()
GATE = SETS.gate
ARCH = "k6_frac_N10_frac_chain_mem32K_40nm"
VTR12 = qor_legs.VTR_PIN[:12]
BANDS = Bands(pair=("O", "P"), set="gate",
              metrics=("num_clb", "min_chan_width", "critical_path_delay_ns",
                       "crit_path_routed_wirelength"),
              geomean_max=1.20, per_design_max=1.50,
              equal_or_explained=("num_mult", "num_memories"))
# P's values: integers, and a CPD that is an exact binary fraction, so k * P is exact.
P_VALUES = {"num_clb": 100, "min_chan_width": 60, "critical_path_delay_ns": 2.5,
            "crit_path_routed_wirelength": 1000}
GOOD_VERDICTS = {"fcheck": "pass", "equiv": "equivalent", "rerun": "same"}


def metrics(leg: str, design: str, scale: float = 1.0,
            override: Mapping[str, object] | None = None) -> dict[str, object]:
    """A metrics.json object: P's values times ``scale`` (rounded to 9 places, as a parsed
    value would be); ``override`` sets single fields (``None`` deletes one) in whichever section
    holds them."""
    scaled = {name: round(value * scale, 9) for name, value in P_VALUES.items()}
    data: dict[str, object] = {
        "schema": 1, "leg": leg, "design": design, "arch": ARCH, "status": "ok",
        "gating": design in GATE, "vtr_commit": qor_legs.VTR_PIN,
        "netlist": {"luts": 10, "ffs": 5, "adders": 0, "mults": 0, "ram_slices": 0, "depth": 3},
        "packed": {"num_clb": scaled["num_clb"], "num_memories": 0, "num_mult": 4},
        "routed": {"vpr_status": "success", "min_chan_width": scaled["min_chan_width"],
                   "critical_path_delay_ns": scaled["critical_path_delay_ns"],
                   "crit_path_routed_wirelength": scaled["crit_path_routed_wirelength"]},
        "cost": {"vpr_time_s": 1.0},
        "verdicts": dict(GOOD_VERDICTS),
    }
    for name, value in (override or {}).items():
        for section in qor_table.SECTIONS:
            block = data[section]
            assert isinstance(block, dict)
            if name in block:
                if value is None:
                    del block[name]
                else:
                    block[name] = value
                break
        else:
            raise KeyError(name)
    return data


def run(leg: str, design: str, scale: float = 1.0, status: str = "ok",
        override: Mapping[str, object] | None = None) -> Run:
    return Run(leg=leg, design=design, arch=ARCH, key12="0123456789ab", status=status,
               gating=design in GATE, metrics=metrics(leg, design, scale, override))


def table(scale: float = 1.0, *, legs: Sequence[str] = ("P", "O"),
          o_scales: Mapping[str, float] | None = None) -> list[Run]:
    """P and O on every gate design; O = ``scale`` × P (or ``o_scales[d]``)."""
    runs = []
    for design in GATE:
        for leg in legs:
            factor = 1.0 if leg == "P" else (o_scales or {}).get(design, scale)
            runs.append(run(leg, design, factor))
    return runs


def replace(runs: list[Run], new: Run) -> list[Run]:
    return [new if (r.leg, r.design) == (new.leg, new.design) else r for r in runs]


def drop(runs: list[Run], leg: str, design: str) -> list[Run]:
    return [r for r in runs if (r.leg, r.design) != (leg, design)]


def verdict(runs: Sequence[Run], *, explanations: Sequence[tuple[str, str, str]] = (),
            divergences: Sequence[tuple[str, str, str]] = (),
            exceptions: Sequence[tuple[str, str, str]] = ()) -> Verdict:
    return qor_table.gate(GateInputs(runs=runs, bands=BANDS, gate_set=GATE,
                                     explanations=explanations, divergences=divergences,
                                     equiv_exceptions=exceptions))


class RatioTest(unittest.TestCase):
    def test_ratio_rules(self) -> None:
        for x, y in ((0.0, 5.0), (5.0, 0.0), (None, 5.0), (5.0, None), (-2.0, 5.0)):
            self.assertIsNone(qor_table.ratio(x, y, True), (x, y))
        self.assertEqual(qor_table.ratio(6.0, 5.0, True), 1.2)
        self.assertEqual(qor_table.ratio(0.0, 0.0, False), 1.0)
        self.assertEqual(qor_table.ratio(0.0, 5.0, False), 0.0)
        self.assertIsNone(qor_table.ratio(5.0, 0.0, False))
        self.assertIsNone(qor_table.ratio(None, 0.0, False))
        self.assertIsNone(qor_table.ratio(-2.0, 5.0, False))

    def test_geomean(self) -> None:
        value = qor_table.geomean([1.1, 1.2, 1.3])
        assert value is not None
        self.assertEqual(round(value, 4), 1.1972)
        self.assertIsNone(qor_table.geomean([]))
        # Review I2: a report-only ratio of 0 (O has no adders, P has some) makes it 0.0.
        self.assertEqual(qor_table.geomean([0.0, 2.0]), 0.0)
        self.assertEqual(qor_table.geomean_n([qor_table.ratio(0.0, 5.0, False), 1.0]), (0.0, 2))
        runs = replace(table(1.1), run("O", "sha", override={"num_clb": -1}))
        ratios = qor_table.pair_ratios(runs, ("O", "P"), "num_clb", GATE, gated=True)
        self.assertEqual(len(ratios), 15)
        mean, n = qor_table.geomean_n(ratios)
        self.assertEqual(n, 14)
        assert mean is not None
        self.assertAlmostEqual(mean, 1.1, places=12)
        self.assertEqual(qor_table.geomean_n([None, None]), (None, 0))
        # Review M2: only ok runs give a ratio.
        runs = replace(table(1.1), run("O", "sha", 1.1, status="failed"))
        ratios = qor_table.pair_ratios(runs, ("O", "P"), "num_clb", GATE, gated=True)
        self.assertIsNone(ratios[GATE.index("sha")])
        self.assertEqual(sum(r is None for r in ratios), 1)

    def test_metric_present(self) -> None:
        r = run("P", "sha", override={"num_clb": -1, "min_chan_width": float("nan"),
                                      "num_mult": True, "crit_path_routed_wirelength": None})
        self.assertIsNone(qor_table.metric(r, "num_clb"))
        self.assertIsNone(qor_table.metric(r, "min_chan_width"))
        self.assertIsNone(qor_table.metric(r, "num_mult"))
        self.assertIsNone(qor_table.metric(r, "crit_path_routed_wirelength"))
        self.assertIsNone(qor_table.metric(r, "no_such_metric"))
        self.assertIsNone(qor_table.metric(r, "fcheck"))
        self.assertEqual(qor_table.metric(r, "critical_path_delay_ns"), 2.5)
        self.assertEqual(qor_table.metric(r, "luts"), 10.0)
        self.assertEqual(qor_table.metric(r, "vpr_time_s"), 1.0)
        self.assertEqual(qor_table.text_field(r, "fcheck"), "pass")
        self.assertEqual(qor_table.text_field(r, "nothing"), "missing")

    def test_fmt_ratio(self) -> None:
        bounds = (1.20, 1.50)
        self.assertEqual(qor_table.fmt_ratio(1.2001, bounds), "1.2001")
        self.assertEqual(qor_table.fmt_ratio(1.2004, bounds), "1.2004")
        self.assertEqual(qor_table.fmt_ratio(1.150, bounds), "1.150")
        self.assertEqual(qor_table.fmt_ratio(1.5001, bounds), "1.5001")
        self.assertEqual(qor_table.fmt_ratio(1.6, bounds), "1.600")
        self.assertEqual(qor_table.fmt_ratio(1.087, bounds), "1.087")

    def test_pairs(self) -> None:
        self.assertEqual(qor_table.PAIRS, (("O", "P"), ("Oprime", "P"), ("Pprime", "P"),
                                           ("O", "Oprime"), ("R", "P"), ("Oprov", "O")))


class GateTest(unittest.TestCase):
    def assertVerdict(self, got: Verdict, state: str, *lines: str) -> None:
        self.assertEqual(got.state, state, got.violations)
        for line in lines:
            self.assertIn(line, got.violations)

    def test_gate_pass(self) -> None:
        self.assertEqual(verdict(table(1.0)), Verdict(state="PASS", violations=()))

    def test_gate_boundaries(self) -> None:
        self.assertEqual(verdict(table(1.2)).state, "PASS")
        got = verdict(table(1.2001))
        self.assertEqual(got.state, "FAIL")
        self.assertEqual(got.violations[0], "gate: FAIL: num_clb geomean O/P 1.2001 > 1.20")
        # One design at exactly 1.500 and the rest at 1.0: the geomean stays below 1.20.
        self.assertEqual(verdict(table(1.0, o_scales={"sha": 1.5})).state, "PASS")
        got = verdict(table(1.0, o_scales={"sha": 1.5001}))
        self.assertEqual(got.state, "FAIL")
        self.assertEqual(got.violations[0], "gate: FAIL: num_clb sha O/P 1.5001 > 1.50")
        self.assertEqual(len(got.violations), 4)  # the four gated metrics, nothing else

    def test_gate_boundaries_decimal(self) -> None:
        """Review I1: values are taken as the decimals VTR printed, so CPD 3.6 / 2.4 is exactly
        1.50 and 15 × 3.6 / 3.0 is a geomean of exactly 1.20; both meet the inclusive bounds."""
        cpd = "critical_path_delay_ns"
        runs = replace(replace(table(), run("P", "sha", override={cpd: 2.4})),
                       run("O", "sha", override={cpd: 3.6}))
        self.assertEqual(verdict(runs), Verdict(state="PASS", violations=()))
        runs = replace(runs, run("O", "sha", override={cpd: 3.600000001}))
        self.assertEqual(verdict(runs), Verdict(state="FAIL", violations=(
            f"gate: FAIL: {cpd} sha O/P 1.5000 > 1.50",)))
        runs = table()
        for design in GATE:
            runs = replace(replace(runs, run("P", design, override={cpd: 3.0})),
                           run("O", design, override={cpd: 3.6}))
        self.assertEqual(verdict(runs), Verdict(state="PASS", violations=()))
        runs = replace(runs, run("O", "sha", override={cpd: 3.600000001}))
        self.assertEqual(verdict(runs), Verdict(state="FAIL", violations=(
            f"gate: FAIL: {cpd} geomean O/P 1.2000 > 1.20",)))

    def test_gate_boundary_float_noise(self) -> None:
        """Rule 3 compares exactly: five ratios of exactly 1.51 meet a 1.51 bound, although the
        float exp(mean(ln r)) of them is 1.5100000000000002."""
        designs = GATE[:5]
        self.assertGreater(qor_table.geomean([1.51] * len(designs)) or 0.0, 1.51)
        bands = Bands(pair=("O", "P"), set="gate", metrics=("num_clb",), geomean_max=1.51,
                      per_design_max=1.51, equal_or_explained=())
        runs = [r for d in designs for r in (run("P", d), run("O", d, override={"num_clb": 151}))]
        inp = GateInputs(runs=runs, bands=bands, gate_set=designs, explanations=(),
                         divergences=(), equiv_exceptions=())
        self.assertEqual(qor_table.gate(inp), Verdict(state="PASS", violations=()))
        runs = replace(runs, run("O", designs[0], override={"num_clb": 151.0001}))
        inp = GateInputs(runs=runs, bands=bands, gate_set=designs, explanations=(),
                         divergences=(), equiv_exceptions=())
        self.assertEqual(qor_table.gate(inp).violations, (
            "gate: FAIL: num_clb geomean O/P 1.5100 > 1.51",
            f"gate: FAIL: num_clb {designs[0]} O/P 1.5100 > 1.51"))

    def test_gate_missing_metric(self) -> None:
        text = "gate: FAIL: min_chan_width missing for sha (O)"
        for bad in (-1, 0, None):
            runs = replace(table(), run("O", "sha", override={"min_chan_width": bad}))
            got = verdict(runs)
            self.assertEqual(got, Verdict(state="FAIL", violations=(text,)), bad)
        runs = replace(table(), run("P", "sha", override={"min_chan_width": None}))
        self.assertEqual(verdict(runs), Verdict(
            state="FAIL", violations=("gate: FAIL: min_chan_width missing for sha (P)",)))

    def test_gate_missing_design(self) -> None:
        got = verdict(drop(table(), "O", "raygentop"))
        self.assertVerdict(got, "FAIL", "table: raygentop has no ok O job")
        got = verdict(replace(table(), run("O", "raygentop", status="failed",
                                           override={"vpr_status": None})))
        self.assertVerdict(got, "FAIL", "table: raygentop has no ok O job")

    def test_gate_incomplete_reference(self) -> None:
        runs = replace(table(), run("P", "or1200", status="capped"))
        self.assertEqual(verdict(runs), Verdict(
            state="INCOMPLETE",
            violations=("gate: INCOMPLETE: or1200 P is capped (reference); rerun it",)))
        runs = replace(runs, run("O", "sha", override={"min_chan_width": -1}))
        self.assertVerdict(verdict(runs), "INCOMPLETE",
                           "gate: INCOMPLETE: or1200 P is capped (reference); rerun it",
                           "gate: FAIL: min_chan_width missing for sha (O)")
        self.assertVerdict(verdict(drop(table(), "P", "raygentop")), "INCOMPLETE",
                           "gate: INCOMPLETE: raygentop P is missing (reference); rerun it")

    def test_gate_reference_excluded_from_rules_2_to_4(self) -> None:
        """Review M2: a capped P's metrics are not judged (no duplicate "(P)" lines)."""
        runs = replace(table(), run("P", "or1200", status="capped",
                                    override={"min_chan_width": None, "num_mult": 9}))
        self.assertEqual(verdict(runs), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: or1200 P is capped (reference); rerun it",)))

    def test_gate_incomplete_attribution_reference(self) -> None:
        runs = table(legs=("P", "Oprime", "O"))
        self.assertEqual(verdict(runs).state, "PASS")
        runs = replace(runs, run("Oprime", "raygentop", status="timeout"))
        self.assertVerdict(verdict(runs), "INCOMPLETE",
                           "gate: INCOMPLETE: raygentop Oprime is timeout (reference); rerun it")

    def test_gate_no_o_leg(self) -> None:
        self.assertEqual(verdict(table(legs=("P",))), Verdict(
            state="INCOMPLETE", violations=("gate: INCOMPLETE: no O leg in the table",)))

    def test_gate_vtr_drift(self) -> None:
        other = "0123456789abcdef" * 2 + "01234567"
        runs = [Run(r.leg, r.design, r.arch, r.key12, r.status, r.gating,
                    {**r.metrics, "vtr_commit": other}) for r in table()]
        self.assertEqual(verdict(runs), Verdict(
            state="INCOMPLETE",
            violations=("gate: INCOMPLETE: VTR at 0123456789ab is not the pin (report-only run)",)))

    def test_gate_equal_or_explained(self) -> None:
        runs = replace(table(), run("O", "sha", override={"num_mult": 5}))
        self.assertEqual(verdict(runs), Verdict(state="FAIL", violations=(
            "gate: FAIL: num_mult sha O 5 != P 4 (no row in explanations.tsv)",)))
        rows = [("sha", "num_mult", "Odin III maps a 4x4 product to a multiplier")]
        self.assertEqual(verdict(runs, explanations=rows).state, "PASS")
        rows = [("sha", "num_memories", "wrong metric")]
        self.assertEqual(verdict(runs, explanations=rows).state, "FAIL")
        # Review M5: missing counts are rule 2's text, and no row explains them.
        gone = replace(replace(table(), run("O", "sha", override={"num_mult": -1})),
                       run("P", "sha", override={"num_mult": None}))
        rows = [("sha", "num_mult", "a row")]
        self.assertEqual(verdict(gone, explanations=rows), Verdict(state="FAIL", violations=(
            "gate: FAIL: num_mult missing for sha (O)",
            "gate: FAIL: num_mult missing for sha (P)")))

    def test_gate_fcheck(self) -> None:
        fail = replace(table(), run("O", "sha", override={"fcheck": "fail"}))
        self.assertEqual(verdict(fail), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha O fcheck fail (no row in divergences.tsv)",)))
        rows = [("sha", "mem-init-dropped", "decision 6 step 5")]
        self.assertEqual(verdict(fail, divergences=rows).state, "PASS")
        rows = [("sha", "async-reset-made-sync", "decision 19")]
        self.assertEqual(verdict(fail, divergences=rows).state, "PASS")
        rows = [("raygentop", "mem-init-dropped", "another design")]
        self.assertEqual(verdict(fail, divergences=rows).state, "FAIL")
        rows = [("sha", "baseline-fails", "P's log")]
        self.assertEqual(verdict(fail, divergences=rows).state, "FAIL")
        p_fails = replace(fail, run("P", "sha", override={"fcheck": "fail"}))
        self.assertEqual(verdict(p_fails, divergences=rows).state, "PASS")
        error = replace(table(), run("O", "sha", override={"fcheck": "error"}))
        self.assertEqual(verdict(error), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha O fcheck error",)))
        p_error = replace(table(), run("P", "sha", override={"fcheck": "error"}))
        self.assertEqual(verdict(p_error), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha P fcheck is error (reference); rerun it",)))
        p_capped = replace(table(), run("P", "sha", override={"fcheck": "capped"}))
        self.assertEqual(verdict(p_capped), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha P fcheck is capped (reference); rerun it",)))
        # Review M1: P's error does not hide O's own undeclared fail ...
        hidden = replace(fail, run("P", "sha", override={"fcheck": "error"}))
        self.assertEqual(verdict(hidden), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha P fcheck is error (reference); rerun it",
            "gate: FAIL: sha O fcheck fail (no row in divergences.tsv)")))
        # ... but a declared row waits for P's verdict.
        self.assertEqual(verdict(hidden, divergences=rows), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha P fcheck is error (reference); rerun it",)))
        # Ruling D1: a check that did not run is INCOMPLETE, never FAIL.
        skipped = replace(table(), run("O", "sha", override={"fcheck": "skipped"}))
        self.assertEqual(verdict(skipped), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha O fcheck skipped (not run); run it",)))
        absent = replace(table(), run("O", "sha", override={"fcheck": None}))
        self.assertEqual(verdict(absent), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha O fcheck missing (not run); run it",)))

    def test_gate_fcheck_untested_and_baseline(self) -> None:
        # Odin III's own untested leg is never a pass and not a failure: INCOMPLETE
        untested = replace(table(), run("O", "sha", override={"fcheck": "untested"}))
        self.assertEqual(verdict(untested), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha O fcheck untested (the RTL leaves the mismatching bits "
            "undefined); it cannot be judged",)))
        # a P-leg baseline defect (fail or untested) does not fail the gate when Odin III passes
        for baseline in ("fail", "untested"):
            p_bad = replace(table(), run("P", "sha", override={"fcheck": baseline}))
            self.assertEqual(verdict(p_bad).state, "PASS", baseline)
        # a declared baseline row lets O's fail stand against P's untested, as against P's fail
        rows = [("sha", "baseline-fails", "D2b: subtract pad")]
        both = replace(replace(table(), run("O", "sha", override={"fcheck": "fail"})),
                       run("P", "sha", override={"fcheck": "untested"}))
        self.assertEqual(verdict(both, divergences=rows).state, "PASS")
        self.assertEqual(verdict(both).state, "FAIL")  # no row: O's own fail

    def test_gate_equiv(self) -> None:
        listed = [("stereovision3", "multiclock", "two clocks; the Verilator check stands in")]
        differs = replace(table(), run("O", "stereovision3", override={"equiv": "differs"}))
        self.assertEqual(verdict(differs, exceptions=listed), Verdict(state="FAIL", violations=(
            "gate: FAIL: stereovision3 equiv differs",)))
        timeout = replace(table(), run("O", "sha", override={"equiv": "timeout"}))
        self.assertEqual(verdict(timeout, exceptions=listed), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha equiv timeout (not in equiv-exceptions.tsv)",)))
        error = replace(table(), run("O", "sha", override={"equiv": "error"}))
        self.assertVerdict(verdict(error), "FAIL",
                           "gate: FAIL: sha equiv error (not in equiv-exceptions.tsv)")
        for word in ("timeout", "multiclock", "error"):
            runs = replace(table(), run("O", "stereovision3", override={"equiv": word}))
            self.assertEqual(verdict(runs, exceptions=listed).state, "PASS", word)
        ram = replace(table(), run("O", "or1200", override={"equiv": "ram"}))
        self.assertEqual(verdict(ram).state, "PASS")
        skipped = replace(table(), run("O", "sha", override={"equiv": "skipped"}))
        self.assertEqual(verdict(skipped, exceptions=listed), Verdict(
            state="INCOMPLETE",
            violations=("gate: INCOMPLETE: sha O equiv skipped (not run); run it",)))
        skipped = replace(table(), run("O", "stereovision3", override={"equiv": "skipped"}))
        self.assertEqual(verdict(skipped, exceptions=listed).state, "PASS")

    def test_gate_equiv_scope(self) -> None:
        """PHASE3 #16: equivalence gates only the small set. A big design's missing, timed-out,
        errored or capped equiv neither fails nor holds the gate; ``differs`` still fails."""
        for design in ("or1200", "boundtop", "raygentop"):
            for word in ("skipped", "timeout", "error", "capped", "multiclock"):
                runs = replace(table(), run("O", design, override={"equiv": word}))
                self.assertEqual(verdict(runs).state, "PASS", (design, word))
            absent = replace(table(), run("O", design, override={"equiv": None}))
            self.assertEqual(verdict(absent), Verdict(state="PASS", violations=()))
        timed = verdict(replace(table(), run("O", "or1200", override={"equiv": "timeout"})))
        self.assertEqual(timed.notes, (
            "gate: note: or1200 equiv timeout; not required (PHASE3 #16)",))
        differs = replace(table(), run("O", "or1200", override={"equiv": "differs"}))
        self.assertEqual(verdict(differs).state, "FAIL")
        # ... and a small-set design is still held to it.
        small = replace(table(), run("O", "sha", override={"equiv": "timeout"}))
        self.assertEqual(verdict(small).state, "FAIL")
        self.assertEqual(len(qor_sched.EQUIV_REQUIRED), 14)
        self.assertTrue({"single_wire", "blob_merge", "stereovision3"}
                        <= qor_sched.EQUIV_REQUIRED)

    def test_gate_equiv_capped_is_incomplete(self) -> None:
        """Review M2: ABC killed by the memory cap on a required design is unmeasured."""
        capped = replace(table(), run("O", "sha", override={"equiv": "capped"}))
        self.assertEqual(verdict(capped), Verdict(state="INCOMPLETE", violations=(
            "gate: INCOMPLETE: sha O equiv capped; rerun it",)))

    def test_gate_fcheck_on_big_designs(self) -> None:
        """PHASE3 #16: a capped or timed-out fcheck on a big design is a note; a mismatch fails;
        the same on a small design still holds or fails the gate."""
        for word in ("capped", "timeout"):
            runs = replace(table(), run("O", "or1200", override={"fcheck": word}))
            got = verdict(runs)
            self.assertEqual((got.state, len(got.notes)), ("PASS", 1), word)
            p_side = replace(table(), run("P", "or1200", override={"fcheck": word}))
            self.assertEqual(verdict(p_side).state, "PASS", word)
        fail = replace(table(), run("O", "or1200", override={"fcheck": "fail"}))
        self.assertEqual(verdict(fail).state, "FAIL")
        capped = replace(table(), run("O", "sha", override={"fcheck": "capped"}))
        self.assertEqual(verdict(capped).state, "FAIL")
        p_timeout = replace(table(), run("P", "sha", override={"fcheck": "timeout"}))
        self.assertEqual(verdict(p_timeout).state, "INCOMPLETE")

    def test_gate_rerun_and_status(self) -> None:
        rerun = replace(table(), run("O", "sha", override={"rerun": "differs"}))
        self.assertEqual(verdict(rerun), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha rerun differs",)))
        vpr = replace(table(), run("O", "sha", override={"vpr_status": "failed"}))
        self.assertEqual(verdict(vpr), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha O vpr_status=failed",)))
        vpr = replace(table(), run("O", "sha", status="failed",
                                   override={"vpr_status": "failed"}))
        self.assertEqual(verdict(vpr), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha O vpr_status=failed",)))
        for word in ("skipped", None):
            runs = replace(table(), run("O", "sha", override={"rerun": word}))
            self.assertEqual(verdict(runs), Verdict(state="INCOMPLETE", violations=(
                f"gate: INCOMPLETE: sha O rerun {word or 'missing'} (not run); run it",)))
        # Ruling D1 does not hide a failure: a FAIL elsewhere is still listed.
        runs = replace(rerun, run("O", "boundtop", override={"rerun": "skipped"}))
        self.assertEqual(verdict(runs).violations, (
            "gate: FAIL: sha rerun differs",
            "gate: INCOMPLETE: boundtop O rerun skipped (not run); run it"))

    def test_gate_collects_in_rule_order(self) -> None:
        runs = drop(table(), "O", "raygentop")
        runs = replace(runs, run("O", "sha", override={"num_mult": 5, "rerun": "differs"}))
        runs = replace(runs, run("O", "diffeq1", 1.6))
        got = verdict(runs)
        self.assertEqual(got.violations, (
            "table: raygentop has no ok O job",
            "gate: FAIL: num_clb diffeq1 O/P 1.600 > 1.50",
            "gate: FAIL: min_chan_width diffeq1 O/P 1.600 > 1.50",
            "gate: FAIL: critical_path_delay_ns diffeq1 O/P 1.600 > 1.50",
            "gate: FAIL: crit_path_routed_wirelength diffeq1 O/P 1.600 > 1.50",
            "gate: FAIL: num_mult sha O 5 != P 4 (no row in explanations.tsv)",
            "gate: FAIL: sha rerun differs",
        ))

    def test_gate_extras_ignored(self) -> None:
        runs = [*table(), run("P", "spree"), run("O", "spree", 3.0, override={"rerun": "skipped"})]
        self.assertEqual(verdict(runs).state, "PASS")


class InputsTest(unittest.TestCase):
    def test_bands_json(self) -> None:
        bands = qor_table.load_bands(BANDS_JSON)
        self.assertEqual(bands, BANDS)
        self.assertEqual(bands.pair, ("O", "P"))
        self.assertEqual(bands.geomean_max, 1.20)
        self.assertEqual(bands.per_design_max, 1.50)
        self.assertEqual(bands.equal_or_explained, ("num_mult", "num_memories"))

    def test_bands_rejects(self) -> None:
        good = json.loads(BANDS_JSON.read_text(encoding="utf-8"))
        cases: list[dict[str, object]] = [
            {"schema": 2}, {"pair": ["O"]}, {"metrics": []}, {"geomean_max": "1.2"},
            {"per_design_max": 0}, {"set": ""}, {"equal_or_explained": [1]}]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bands.json"
            for case in cases:
                path.write_text(json.dumps({**good, **case}), encoding="utf-8")
                with self.assertRaises(ValueError, msg=str(case)) as cm:
                    qor_table.load_bands(path)
                self.assertIn(str(path), str(cm.exception))

    def test_tsv(self) -> None:
        self.assertEqual(qor_table.load_tsv(EXPLANATIONS), [])
        first = EXPLANATIONS.read_text(encoding="utf-8").splitlines()[0]
        self.assertEqual(first, "# design\tmetric\ttext (a row explains why O != P for "
                                "num_mult or num_memories)")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "divergences.tsv"
            path.write_text("# head\nsha\tmem-init-dropped\twhy\n\n#x\nbgm\tbaseline-fails\tP\n",
                            encoding="utf-8")
            self.assertEqual(qor_table.load_tsv(path), [
                ("sha", "mem-init-dropped", "why"), ("bgm", "baseline-fails", "P")])
            path.write_text("sha \t num_mult\ttext \n", encoding="utf-8")
            self.assertEqual(qor_table.load_tsv(path), [("sha", "num_mult", "text")])
            for bad in ("sha\tonly-two\n", "a\tb\tc\td\n", "a b c\n", "sha\t\ttext\n"):
                path.write_text(f"# head\n{bad}", encoding="utf-8")
                with self.assertRaises(ValueError) as cm:
                    qor_table.load_tsv(path)
                self.assertEqual(str(cm.exception),
                                 f"{path}:2: expected 3 tab-separated fields")


def write_job(root: Path, leg: str, key12: str, design: str, *, status: str = "ok",
              data: Mapping[str, object] | None = None) -> Path:
    job = root / VTR12 / ARCH / leg / key12 / design
    job.mkdir(parents=True)
    (job / "status").write_text(f"{status}\n", encoding="utf-8")
    if data is not None:
        (job / "metrics.json").write_text(json.dumps(data), encoding="utf-8")
    return job


class LoadRunsTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def latest(self, leg: str, key12: str) -> None:
        (self.root / VTR12 / ARCH / leg / "latest").write_text(f"{key12}\n", encoding="utf-8")

    def test_load_latest_and_keys(self) -> None:
        p_key, o_old, o_new = "aaaaaaaaaaaa", "bbbbbbbbbbbb", "cccccccccccc"
        for design in ("sha", "ch_intrinsics", "spree"):
            write_job(self.root, "P", p_key, design, data=metrics("P", design))
        (self.root / VTR12 / ARCH / "P" / p_key / "planned.json").write_text("{}")
        self.latest("P", p_key)
        write_job(self.root, "O", o_old, "sha", data=metrics("O", "sha", 2.0))
        write_job(self.root, "O", o_new, "sha", data=metrics("O", "sha"))
        self.latest("O", o_new)
        runs = qor_table.load_runs(self.root, VTR12, ARCH, {})
        self.assertEqual([(r.design, r.leg, r.key12) for r in runs], [
            ("ch_intrinsics", "P", p_key), ("spree", "P", p_key), ("sha", "P", p_key),
            ("sha", "O", o_new)])
        self.assertEqual([r.gating for r in runs], [True, False, True, True])
        self.assertTrue(all(r.status == "ok" and r.arch == ARCH for r in runs))
        runs = qor_table.load_runs(self.root, VTR12, ARCH, {"O": o_old}, legs=["O"])
        self.assertEqual([(r.leg, r.key12, qor_table.metric(r, "num_clb")) for r in runs],
                         [("O", o_old, 200.0)])

    def test_status_and_partial_attempts(self) -> None:
        key = "aaaaaaaaaaaa"
        write_job(self.root, "P", key, "mkSMAdapter4B", status="capped")
        write_job(self.root, "P", key, "sha")  # status ok, no metrics.json: partial
        stale = {**metrics("P", "raygentop"), "status": "failed"}
        write_job(self.root, "P", key, "raygentop", data=stale)  # ok word, a failed record
        write_job(self.root, "P", key, "diffeq1", data=metrics("P", "diffeq2"))  # wrong design
        good = write_job(self.root, "P", key, "or1200", status="failed",
                         data=metrics("P", "or1200"))
        self.latest("P", key)
        got = {r.design: r.status for r in qor_table.load_runs(self.root, VTR12, ARCH, {})}
        self.assertEqual(got, {"mkSMAdapter4B": "capped", "sha": "incomplete",
                               "raygentop": "incomplete", "diffeq1": "incomplete",
                               "or1200": "failed"})
        (good / "status").unlink()
        got = {r.design: r.status for r in qor_table.load_runs(self.root, VTR12, ARCH, {})}
        self.assertEqual(got["or1200"], "missing")

    def test_no_complete_key(self) -> None:
        def refused(keys: Mapping[str, str], legs: Sequence[str] | None = None) -> str:
            with self.assertRaises(HeavyError) as cm:
                qor_table.load_runs(self.root, VTR12, ARCH, keys, legs=legs)
            self.assertEqual(cm.exception.code, 1)
            return cm.exception.message

        text = f"table: no complete key for leg P on {ARCH}"
        self.assertEqual(refused({}, ["P"]), text)
        write_job(self.root, "P", "aaaaaaaaaaaa", "sha", data=metrics("P", "sha"))
        # Review M3: by default a leg directory without latest (retracted mid-rerun) is left
        # out; asked for by name, it is refused.
        self.assertEqual(qor_table.load_runs(self.root, VTR12, ARCH, {}), [])
        self.assertEqual(refused({}, ["P"]), text)
        self.assertEqual(refused({"P": "bbbbbbbbbbbb"}), text)
        self.assertEqual(refused({"P": "../../xx"}), text)
        self.latest("P", "not-a-key")
        self.assertEqual(refused({}), text)  # a latest that names no key
        self.latest("P", "aaaaaaaaaaaa")
        self.assertEqual(refused({"O'": "aaaaaaaaaaaa"}, ["P", "Oprime"]),
                         f"table: no complete key for leg Oprime on {ARCH}")
        self.assertEqual(len(qor_table.load_runs(self.root, VTR12, ARCH, {})), 1)


# ---------------------------------------------------------------- Task 9: the writers and CLI

TABLE_FIXTURE = FIXTURES / "vtr_qor" / "table"
EXPECTED = TABLE_FIXTURE / "expected"
FAKE_VTR = TABLE_FIXTURE / "vtr"
FIXTURE_DIR = "20261013-0441-fixture"  # the newest job.json end time: O arm_core
RUNS_CSV_HEADER = (
    "arch,design,leg,key12,status,gating,vpr_status,luts,ffs,adders,mults,ram_slices,depth,"
    "abc_depth,num_clb,num_memories,num_mult,device_width,device_height,min_chan_width,"
    "critical_path_delay_ns,fmax_mhz,crit_path_routed_wirelength,frontend_wall_s,"
    "frontend_peak_rss_mib,vpr_time_s,vpr_total_runtime_s,max_vpr_mem_mib,fcheck,equiv,rerun,"
    "blif_sha256,vtr_commit,odin3_commit")
RATIOS_CSV_HEADER = (
    "arch,pair,design,n,luts,ffs,adders,mults,ram_slices,depth,num_clb,num_memories,num_mult,"
    "min_chan_width,critical_path_delay_ns,crit_path_routed_wirelength,frontend_wall_s,"
    "frontend_peak_rss_mib,vpr_time_s,max_vpr_mem_mib")
E_QH15 = ("vtr-qor: publish: set ODIN3_QOR_PUBLISH=1 (controller only; agents never write "
          "~/odin3-ws/golden)")


def header(runs: Sequence[Run], label: str = "t") -> qor_table.TableHeader:
    return qor_table.table_header(runs, arch=ARCH, label=label, vtr12=VTR12)


class TableCliTest(unittest.TestCase):
    """``vtr-qor table``, ``gate`` and ``publish`` over a copy of the fixture tree."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-table-test-")
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.out = self.root / "out"
        shutil.copytree(TABLE_FIXTURE / "runs", self.out)

    def job(self, leg: str, design: str) -> Path:
        keys = {"P": "111111111111", "Oprime": "222222222222", "Pprime": "333333333333",
                "O": "444444444444"}
        return self.out / VTR12 / ARCH / leg / keys[leg] / design

    def table(self, legs: str = "P,O',P',O", label: str = "fixture",
              *extra: str) -> tuple[int, str, str]:
        return run_main(vtr_qor.main, ["table", "--out", str(self.out), "--vtr-root",
                                       str(FAKE_VTR), "--legs", legs, "--label", label, *extra])

    def made(self, label: str = "fixture", legs: str = "P,O',P',O") -> Path:
        code, out, err = self.table(legs, label)
        self.assertEqual(code, 0, err)
        table_dir = Path(out.strip())
        self.assertEqual(table_dir.parent, self.out / "tables")
        return table_dir

    def files(self, table_dir: Path) -> dict[str, bytes]:
        return {name: (table_dir / name).read_bytes() for name in qor_table.TABLE_FILES}

    def gate(self, table_dir: Path) -> tuple[int, list[str]]:
        code, out, err = run_main(vtr_qor.main, ["gate", str(table_dir)])
        self.assertEqual(err, "")
        return code, out.splitlines()

    def test_table_golden_files(self) -> None:
        table_dir = self.made()
        self.assertEqual(table_dir.name, FIXTURE_DIR)
        for name in qor_table.TABLE_FILES:
            self.assertEqual((table_dir / name).read_text(encoding="utf-8"),
                             (EXPECTED / name).read_text(encoding="utf-8"), name)
            self.assertEqual((table_dir / name).read_bytes(), (EXPECTED / name).read_bytes(),
                             name)

    def test_equiv_cell_says_not_required(self) -> None:
        """PHASE3 #16: a design outside the small set with no equiv verdict reads ``not
        required``, one inside the set stays blank."""
        for design in ("spree", "sha"):
            path = self.job("O", design) / "metrics.json"
            record = json.loads(path.read_text(encoding="utf-8"))
            record["verdicts"].pop("equiv", None)
            path.write_text(json.dumps(record), encoding="utf-8")
        text = (self.made() / "qor.md").read_text(encoding="utf-8")
        self.assertEqual(text.count("not required (PHASE3 #16)"), 1)
        (line,) = [row for row in text.splitlines() if "not required (PHASE3 #16)" in row]
        self.assertIn("| spree |", line)

    def test_table_twice_identical(self) -> None:
        """T7: identical bytes with every file's mtime and the clock changed."""
        first = self.files(self.made())
        later = time.time() + 86400 * 400
        for path in self.out.rglob("*"):
            os.utime(path, (later, later))
        with mock.patch("time.time", return_value=later), \
                mock.patch("time.time_ns", return_value=int(later * 1e9)):
            second = self.files(self.made())
        self.assertEqual(first, second)

    def test_csv_headers(self) -> None:
        table_dir = self.made()
        runs = (table_dir / "runs.csv").read_text(encoding="utf-8").split("\n")
        ratios = (table_dir / "ratios.csv").read_text(encoding="utf-8").split("\n")
        self.assertEqual(runs[0], RUNS_CSV_HEADER)
        self.assertEqual(ratios[0], RATIOS_CSV_HEADER)
        self.assertEqual(len(runs), 68 + 2)  # 17 designs x 4 legs, the header, the last "\n"
        # 4 pairs (O/P, O'/P, P'/P, O/O'): 17 design rows and 2 geomean rows each.
        self.assertEqual(len(ratios), 4 * (17 + 2) + 2)
        self.assertNotIn("\r", (table_dir / "runs.csv").read_text(encoding="utf-8"))

    def test_gate_cli(self) -> None:
        self.assertEqual(self.gate(self.made("pass")), (0, ["Gate (decision 3): PASS"]))
        data = json.loads((self.job("O", "sha") / "metrics.json").read_text(encoding="utf-8"))
        data["packed"]["num_clb"] = 400
        (self.job("O", "sha") / "metrics.json").write_text(json.dumps(data), encoding="utf-8")
        self.assertEqual(self.gate(self.made("fail")), (1, [
            "Gate (decision 3): FAIL", "gate: FAIL: num_clb sha O/P 2.000 > 1.50"]))
        (self.job("P", "or1200") / "status").write_text("capped\n", encoding="utf-8")
        code, lines = self.gate(self.made("incomplete"))
        self.assertEqual(code, 3)
        self.assertEqual(lines, ["Gate (decision 3): INCOMPLETE",
                                 "gate: INCOMPLETE: or1200 P is capped (reference); rerun it",
                                 "gate: FAIL: num_clb sha O/P 2.000 > 1.50"])
        code, out, err = run_main(vtr_qor.main, ["gate", str(self.root)])
        self.assertEqual(code, 2)
        self.assertIn(f"vtr-qor: gate: {self.root / 'runs.json'}: not a table", err)

    def test_publish(self) -> None:
        table_dir = self.made()
        home = self.root / "home"
        home.mkdir()
        with mock.patch.dict(os.environ, {"HOME": str(home)}):
            os.environ.pop("ODIN3_QOR_PUBLISH", None)
            code, out, err = run_main(vtr_qor.main, ["publish", str(table_dir)])
            self.assertEqual((code, out, err), (2, "", E_QH15 + "\n"))
            self.assertEqual(list(home.iterdir()), [])
            os.environ["ODIN3_QOR_PUBLISH"] = "1"
            code, out, err = run_main(vtr_qor.main, ["publish", str(table_dir)])
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(out, "git -C ~/odin3-ws/golden add qor/3c9a4d23b27d/fixture\n"
                              'git -C ~/odin3-ws/golden commit -m "qor: fixture (3c9a4d23b27d)"\n')
        target = home / "odin3-ws" / "golden" / "qor" / "3c9a4d23b27d" / "fixture"
        self.assertEqual(sorted(p.name for p in target.iterdir()), sorted(qor_table.TABLE_FILES))
        self.assertEqual(self.files(target), self.files(table_dir))

    def test_publish_refusals(self) -> None:
        """Review I2/M3: only ODIN3_QOR_PUBLISH=1, a valid header, all four files; an existing
        label only with --force, replaced whole; nothing written under HOME on a refusal."""
        table_dir = self.made()
        home = self.root / "home"
        home.mkdir()
        target = home / "odin3-ws" / "golden" / "qor" / "3c9a4d23b27d" / "fixture"

        def publish(*extra: str, where: Path = table_dir) -> tuple[int, str, str]:
            return run_main(vtr_qor.main, ["publish", str(where), *extra])

        bad = self.root / "bad"
        with mock.patch.dict(os.environ, {"HOME": str(home)}):
            for value in ("0", "", "yes", "1 "):
                os.environ["ODIN3_QOR_PUBLISH"] = value
                self.assertEqual(publish(), (2, "", E_QH15 + "\n"), value)
            os.environ["ODIN3_QOR_PUBLISH"] = "1"
            shutil.copytree(table_dir, bad)
            (bad / "runs.csv").unlink()
            code, _, err = publish(where=bad)
            self.assertEqual((code, err), (2, f"vtr-qor: publish: {bad} lacks runs.csv\n"))
            shutil.copy(table_dir / "runs.csv", bad / "runs.csv")
            data = json.loads((bad / "runs.json").read_text(encoding="utf-8"))
            for field_, value in (("label", "../x"), ("label", ".hidden"), ("vtr12", "../..")):
                data["header"] = {**data["header"], "label": "fixture", "vtr12": VTR12,
                                  field_: value}
                (bad / "runs.json").write_text(json.dumps(data), encoding="utf-8")
                code, _, err = publish(where=bad)
                self.assertEqual((code, err), (2, "vtr-qor: publish: the table header has no "
                                                  "valid vtr12 and label\n"), value)
            self.assertEqual(list(home.iterdir()), [])
            self.assertEqual(publish()[0], 0)
            code, _, err = publish()
            self.assertEqual((code, err), (2, f"vtr-qor: publish: {target} exists (--force "
                                              "replaces it)\n"))
            other = self.made("other")
            (other / "runs.json").write_text((other / "runs.json").read_text(
                encoding="utf-8").replace('"label": "other"', '"label": "fixture"'),
                encoding="utf-8")
            self.assertEqual(publish("--force", where=other)[0], 0)
            self.assertEqual(self.files(target), self.files(other))
            self.assertEqual(sorted(p.name for p in target.parent.iterdir()), ["fixture"])
            stray = target.parent / ".fixture.tmp"
            stray.mkdir()
            (stray / "notes.txt").write_text("x", encoding="utf-8")
            code, _, err = publish("--force")
            self.assertEqual((code, err), (2, "vtr-qor: publish: refusing to replace or remove "
                                              f"{stray}: it holds notes.txt\n"))
            self.assertEqual(self.files(target), self.files(other))
            (stray / "notes.txt").unlink()
            stray.rmdir()
            # Review N2: a label holding another file is refused before any rename.
            (target / "README").write_text("x", encoding="utf-8")
            code, _, err = publish("--force")
            self.assertEqual((code, err), (2, "vtr-qor: publish: refusing to replace or remove "
                                              f"{target}: it holds README\n"))
            self.assertEqual(sorted(p.name for p in target.parent.iterdir()), ["fixture"])
            self.assertEqual(self.files(target), self.files(other))
            # Review N3: a symlinked label is refused, --force or not.
            real = self.root / "elsewhere"
            real.mkdir()
            linked = target.parent / "linked"
            linked.symlink_to(real)
            data = json.loads((table_dir / "runs.json").read_text(encoding="utf-8"))
            data["header"]["label"] = "linked"
            shutil.copytree(table_dir, self.root / "linked-table")
            (self.root / "linked-table" / "runs.json").write_text(json.dumps(data),
                                                                   encoding="utf-8")
            for extra in ((), ("--force",)):
                code, _, err = publish(*extra, where=self.root / "linked-table")
                self.assertEqual((code, err), (2, f"vtr-qor: publish: refusing {linked}: it is "
                                                  "a symlink\n"), extra)
            self.assertEqual(list(real.iterdir()), [])

    def test_smoke_and_extra_sets(self) -> None:
        """Ruling N1 (review addendum's reproduction): on k6, decision 3 needs all 15, so a
        table narrowed by --set is not gated and INCOMPLETE (exit 3), never a 5-design PASS."""
        for sets, n in (("smoke", 5), ("extra", 0), ("gate", 15)):
            code, out, err = self.table("P,O',P',O", f"set-{sets}", "--set", sets)
            self.assertEqual(code, 0, err)
            table_dir = Path(out.strip())
            md = (table_dir / "qor.md").read_text(encoding="utf-8")
            if n == 15:
                self.assertEqual(self.gate(table_dir), (0, ["Gate (decision 3): PASS"]))
                self.assertIn("## Gate (decision 3): PASS\n", md)
                continue
            reason = f"decision 3 needs the 15 (this table has {n})"
            self.assertIn(f"## Gate (decision 3): not gated — {reason}\n", md)
            self.assertNotIn("**geomean (", md.split("## Netlist")[0])
            self.assertNotIn("## Gate (decision 3): PASS", md)
            self.assertEqual(self.gate(table_dir), (3, [
                "Gate (decision 3): INCOMPLETE", f"gate: INCOMPLETE: {reason}"]))
            ratios = (table_dir / "ratios.csv").read_text(encoding="utf-8")
            self.assertIn(f"{ARCH},O/P,GEOMEAN_GATE,{n},", ratios)  # n of the 15
            geomeans = json.loads((table_dir / "runs.json").read_text(encoding="utf-8"))
            want = ("O/P", "GEOMEAN_GATE", "num_clb")
            gate_rows = [g for g in geomeans["geomeans"]
                         if (g["pair"], g["set"], g["metric"]) == want]
            self.assertEqual([(g["n"], g["of"]) for g in gate_rows], [(n, 15)])

    def test_earch_table(self) -> None:
        """Review M1: an EArch table gates over the smoke set and has no k6 cross-check."""
        shutil.copytree(self.out / VTR12 / ARCH, self.out / VTR12 / "EArch")
        code, out, err = run_main(vtr_qor.main, [
            "table", "--arch", "earch", "--out", str(self.out), "--vtr-root", str(FAKE_VTR),
            "--legs", "P,O", "--label", "earch"])
        self.assertEqual(code, 0, err)
        table_dir = Path(out.strip())
        title = "Gate (EArch smoke; agent default, not decision 3)"
        self.assertEqual(self.gate(table_dir), (0, [f"{title}: PASS"]))
        md = (table_dir / "qor.md").read_text(encoding="utf-8")
        self.assertIn("# Odin III QoR — EArch — earch", md)
        self.assertIn(f"## {title}: PASS\n", md)
        self.assertNotIn("Gate (decision 3)", md)
        self.assertIn("| **geomean (5)** |", md)
        self.assertIn("golden has no row for these designs on EArch; no cross-check.", md)
        self.assertNotIn("| CLB P | CLB golden |", md)
        self.assertEqual(vtr_qor.gate_designs(qor_sched.load_sets(), "earch", {"sha"}), ())

    def test_p_only_table(self) -> None:
        table_dir = self.made("p-only", "P")
        md = (table_dir / "qor.md").read_text(encoding="utf-8")
        self.assertNotIn("## Gate", md)
        self.assertNotIn("## Non-gating rows", md)
        self.assertIn("## Cross-check against VTR's golden", md)
        self.assertIn("| ch_intrinsics | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |", md)
        self.assertIn("| ch_intrinsics | 200 | 160 | 1.250 | 50 | 50 | 1.000 | 2.500 | 2.500 | "
                      "1.000 | 1000 | 1000 | 1.000 | num_clb |", md)
        self.assertIn("| **geomean (15)** |  |  | 1.015 |  |  | 1.015 |  |  | 1.000 (14/15) |"
                      "  |  | 1.000 |  |", md)
        self.assertNotIn("| O |", md)
        self.assertIn("legs P 111111111111\n", md)
        self.assertEqual(self.gate(table_dir), (3, [
            "Gate (decision 3): INCOMPLETE", "gate: INCOMPLETE: no O leg in the table"]))
        ratios = (table_dir / "ratios.csv").read_text(encoding="utf-8")
        self.assertEqual(ratios, RATIOS_CSV_HEADER + "\n")

    def test_table_refusals(self) -> None:
        code, _, err = self.table("P", "../x")
        self.assertEqual(code, 2)
        self.assertIn("--label '../x' is not a name", err)
        code, _, err = self.table("P", "x", "--keys", "O=444444444444")
        self.assertEqual((code, err), (2, "vtr-qor: --keys names leg O, which --legs does not\n"))
        code, _, err = self.table("P", "x", "--keys", "P")
        self.assertEqual(code, 2)
        code, _, err = run_main(vtr_qor.main, ["table", "--out", str(self.out), "--vtr-root",
                                               str(self.root), "--legs", "P", "--label", "x"])
        self.assertEqual(code, 2)
        self.assertIn("table: cannot read VTR's golden results under", err)
        code, _, err = self.table("P,Oprov", "x")
        self.assertEqual((code, err), (1, "vtr-qor: table: no complete key for leg Oprov on "
                                          f"{ARCH}\n"))
        self.assertFalse((self.out / "tables").exists())

    def test_table_keys_and_sets(self) -> None:
        code, out, err = self.table("P", "keyed", "--keys", "P=111111111111", "--set", "gate")
        self.assertEqual(code, 0, err)
        runs = (Path(out.strip()) / "runs.csv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(runs), 1 + 15)
        self.assertNotIn("spree", "".join(runs))


class WriterTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def test_md_four_decimals_near_bound(self) -> None:
        for scale, text in ((1.2004, "1.2004"), (1.150, "1.150")):
            runs = table(scale)
            verdict_ = verdict(runs)
            qor_table.write_md(runs, verdict_, header(runs), self.dir / "qor.md", golden=None)
            qor_table.write_csv(runs, self.dir / "runs.csv", self.dir / "ratios.csv")
            md = (self.dir / "qor.md").read_text(encoding="utf-8")
            self.assertIn(f"| **geomean (15)** |  |  | **{text}** |", md)
            gm = [line for line in (self.dir / "ratios.csv").read_text(encoding="utf-8")
                  .splitlines() if ",GEOMEAN_GATE," in line]
            self.assertEqual(len(gm), 1)
            self.assertIn(f",{text},", gm[0])
        runs = table(1.2004)
        qor_table.write_md(runs, verdict(runs), header(runs), self.dir / "qor.md", golden=None)
        md = (self.dir / "qor.md").read_text(encoding="utf-8")
        self.assertIn("## Gate (decision 3): FAIL", md)
        self.assertIn("- gate: FAIL: num_clb geomean O/P 1.2004 > 1.20", md)
        self.assertIn("| sha | 100 | 120.04 | 1.2004 |", md)

    def test_cost_pprime_shows_both_front_end_parts(self) -> None:
        """Point (o): a P' row's front-end cell is frontend.time plus the Parmys stage."""
        runs = table(legs=("P", "Pprime"))
        runs = [Run(r.leg, r.design, r.arch, r.key12, r.status, r.gating,
                    {**r.metrics, "cost": {"frontend_wall_s": 3.0, "vpr_time_s": 1.0}},
                    {"parmys_stage_wall_s": 12.5} if r.leg == "Pprime" else {})
                for r in runs]
        qor_table.write_md(runs, None, header(runs), self.dir / "qor.md", golden=None)
        md = (self.dir / "qor.md").read_text(encoding="utf-8")
        self.assertIn("| sha | P' | 3.0 + 12.5 Parmys |", md)
        self.assertIn("| sha | P | 3.0 |", md)

    def test_header_from_newest_job(self) -> None:
        def with_job(r: Run, ended: str, cpu: str) -> Run:
            return Run(r.leg, r.design, r.arch, r.key12, r.status, r.gating, r.metrics,
                       {"ended": ended, "host": {"cpu": cpu, "mem_gib": 23.5}})
        runs = table()
        runs = [with_job(r, "2026-10-12T01:00:00Z", "old") for r in runs]
        runs = replace(runs, with_job(run("P", "sha"), "2026-10-14T09:08:07Z", "new"))
        got = header(runs, "lab")
        self.assertEqual((got.date_utc, got.host), ("2026-10-14T09:08:07Z", "new, 23.5 GiB"))
        self.assertEqual(qor_table.table_stamp(got), "20261014-0908")
        self.assertEqual(dict(got.legs), {"P": "0123456789ab", "O": "0123456789ab"})
        self.assertIsNone(got.odin3_commit12)
        bare = header(table())  # no job.json: metrics.json has no start either
        self.assertEqual((bare.date_utc, bare.host), ("unknown", "unknown"))
        self.assertEqual(qor_table.table_stamp(bare), "00000000-0000")

    def test_golden_xcheck(self) -> None:
        golden = qor_table.load_golden_xcheck(FAKE_VTR)
        self.assertEqual(dict(golden.bands), {
            "num_clb": (0.9, 1.1), "num_memories": (0.9, 1.1), "min_chan_width": (0.25, 1.3),
            "crit_path_routed_wirelength": (0.6, 1.5), "critical_path_delay": (0.5, 1.4)})
        self.assertEqual(golden.values[("ch_intrinsics", "num_clb")], 160.0)
        self.assertEqual(golden.values[("stereovision3", "critical_path_delay")], -1.0)
        self.assertNotIn(("sha", "vtr_max_mem"), golden.values)  # "70.00 MiB": not a number
        ratio_ = qor_table.golden_ratio
        self.assertEqual(ratio_(golden, 200.0, "ch_intrinsics", "num_clb"), (1.25, "outside"))
        self.assertEqual(ratio_(golden, 50.0, "diffeq1", "min_chan_width"), (1.25, "ok"))
        self.assertEqual(ratio_(golden, 2.5, "stereovision3", "critical_path_delay"), (None, ""))
        self.assertEqual(ratio_(golden, None, "sha", "num_clb"), (None, ""))
        self.assertEqual(ratio_(golden, 1.0, "sha", "num_mult"), (None, ""))
        with self.assertRaises(OSError):
            qor_table.load_golden_xcheck(self.dir)

    def test_verdict_state(self) -> None:
        state = qor_table.verdict_state
        self.assertEqual(state({"pass": True, "violations": []}), "PASS")
        self.assertEqual(state({"pass": False, "violations": ["gate: FAIL: x"]}), "FAIL")
        self.assertEqual(state({"pass": False, "violations": [
            "gate: FAIL: x", "gate: INCOMPLETE: no O leg in the table"]}), "INCOMPLETE")
        for verdict_ in (Verdict("PASS", ()), Verdict("FAIL",
                                                    ("table: raygentop has no ok O job",)),
                         Verdict("INCOMPLETE", ("gate: INCOMPLETE: no P leg in the table",))):
            got = qor_table.verdict_json(verdict_)
            assert got is not None
            self.assertEqual(state(got), verdict_.state)

    def test_numbers_from_ok_runs_only(self) -> None:
        runs = replace(table(), run("O", "sha", status="failed"))
        qor_table.write_csv(runs, self.dir / "runs.csv", self.dir / "ratios.csv")
        row = next(line for line in (self.dir / "runs.csv").read_text(encoding="utf-8")
                   .splitlines() if ",sha,O," in line)
        self.assertEqual(row, f"{ARCH},sha,O,0123456789ab,failed,true" + "," * 27
                         + qor_legs.VTR_PIN + ",")
        ratios = (self.dir / "ratios.csv").read_text(encoding="utf-8").splitlines()
        self.assertIn(f"{ARCH},O/P,GEOMEAN_GATE,14,", "\n".join(ratios))

    def test_noise_floor_and_name_style(self) -> None:
        """Review I2/M5: both sections render; the noise floor shows min and max."""
        runs = table(legs=("P", "O"))
        for design in GATE:
            scale = {"sha": 0.95, "raygentop": 1.05}.get(design, 1.0)
            runs.append(run("R", design, scale))
            runs.append(run("Oprov", design, override={"vpr_time_s": 2.0}
                            if design == "sha" else None))
        qor_table.write_md(runs, verdict(runs), header(runs), self.dir / "qor.md", golden=None)
        md = (self.dir / "qor.md").read_text(encoding="utf-8")
        self.assertIn("## Noise floor\n", md)
        self.assertIn("| metric | R/P geomean (15) | min | max |", md)
        self.assertIn("| num_clb | 1.000 | 0.950 (sha) | 1.050 (raygentop) |", md)
        self.assertIn("## Name style\n", md)
        self.assertIn("| sha | 1.0 | 2.0 | 2.000 |  |  |  |", md)
        self.assertIn("| raygentop | 1.0 | 1.0 | 1.000 |  |  |  |", md)

    def test_md_shows_the_baseline_defect_never_a_pass(self) -> None:
        runs = replace(table(), run("P", "sha", override={"fcheck": "fail"}))
        runs = replace(runs, run("P", "raygentop", override={"fcheck": "untested"}))
        divergences = [("sha", "baseline-fails", "D2b: subtract pad"),
                       ("sha", "baseline-fails", "dropped-latches: RTL latches"),
                       ("raygentop", "baseline-fails", "x-state: unreset pipeline")]
        qor_table.write_md(runs, verdict(runs), header(runs), self.dir / "qor.md", golden=None,
                           divergences=divergences, equiv_exceptions=())
        md = (self.dir / "qor.md").read_text(encoding="utf-8")
        self.assertIn("| sha | fail (baseline: D2b, dropped-latches) | pass |", md)
        self.assertIn("| raygentop | untested (baseline: x-state) | pass |", md)
        self.assertEqual(qor_table.baseline_tags(divergences, "or1200"), [])

    def test_md_checks_divergences_marks(self) -> None:
        """Review I2: divergence rows, the equiv exception note, the ? mark, -dirty."""
        runs = replace(table(), run("O", "sha", override={"fcheck": "skipped"}))
        runs = replace(runs, run("O", "or1200", override={"fcheck": "fail"}))
        runs = replace(runs, run("O", "stereovision3", override={"equiv": "multiclock"}))
        o_bgm = run("O", "raygentop")
        dirty = {**o_bgm.metrics, "odin3_commit": "f" * 40 + "-dirty", "odin3_build": "e" * 64}
        runs = replace(runs, Run(o_bgm.leg, o_bgm.design, o_bgm.arch, o_bgm.key12, o_bgm.status,
                                 o_bgm.gating, dirty))
        exceptions = [("stereovision3", "multiclock", "two clocks")]
        divergences = [("ch_intrinsics", "mem-init-dropped", "decision 6 step 5"),
                       ("not_a_design", "baseline-fails", "elsewhere")]
        got = verdict(runs, exceptions=exceptions, divergences=divergences)
        self.assertEqual(got.state, "INCOMPLETE")
        qor_table.write_md(runs, got, header(runs), self.dir / "qor.md", golden=None,
                           divergences=divergences, equiv_exceptions=exceptions)
        md = (self.dir / "qor.md").read_text(encoding="utf-8")
        rows = {line.split(" | ")[0][2:]: line for line in md.splitlines()
                if line.startswith("| ") and line.endswith((" ✓ |", " ✗ |", " ? |"))}
        self.assertTrue(rows["sha"].endswith(" ? |"))
        self.assertTrue(rows["or1200"].endswith(" ✗ |"))
        self.assertTrue(rows["raygentop"].endswith(" ✓ |"))
        self.assertIn("| stereovision3 | pass | pass | multiclock (exception: multiclock) | same |",
                      md)
        self.assertIn("## Declared divergences\n\n| design | class | text |\n|---|---|---|\n"
                      "| ch_intrinsics | mem-init-dropped | decision 6 step 5 |\n", md)
        self.assertNotIn("not_a_design", md)
        self.assertIn(" · odin3 ffffffffffff-dirty (build eeeeeeeeeeee) · ", md)
        qor_table.write_md(table(), verdict(table()), header(table()), self.dir / "q2.md",
                           golden=None)
        self.assertIn("## Declared divergences\n\nNone.\n",
                      (self.dir / "q2.md").read_text(encoding="utf-8"))

    def test_golden_equal_values_ok(self) -> None:
        """VTR's rule: equal values pass whatever the band; otherwise the band decides."""
        golden = qor_table.GoldenXcheck(values={("sha", "num_clb"): 100.0},
                                        bands={"num_clb": (1.1, 1.2)})
        self.assertEqual(qor_table.golden_ratio(golden, 100.0, "sha", "num_clb"), (1.0, "ok"))
        self.assertEqual(qor_table.golden_ratio(golden, 110.0, "sha", "num_clb"), (1.1, "ok"))
        self.assertEqual(qor_table.golden_ratio(golden, 121.0, "sha", "num_clb"),
                         (1.21, "outside"))
        k6 = qor_table.load_golden_xcheck(FAKE_VTR, arch=ARCH)
        self.assertEqual(len(k6.values), len(qor_table.load_golden_xcheck(FAKE_VTR).values))
        self.assertEqual(dict(qor_table.load_golden_xcheck(FAKE_VTR, arch="EArch").values), {})

    def test_runs_json_status_word(self) -> None:
        runs = replace(table(), run("O", "sha", status="failed"))
        qor_table.write_json(runs, verdict(runs), header(runs), self.dir / "runs.json")
        data = json.loads((self.dir / "runs.json").read_text(encoding="utf-8"))
        sha = next(r for r in data["runs"] if (r["leg"], r["design"]) == ("O", "sha"))
        self.assertEqual((sha["status"], sha["key12"], sha["packed"]["num_clb"]),
                         ("failed", "0123456789ab", 100))
        self.assertEqual(data["gate"]["pass"], False)
        self.assertIn("table: sha has no ok O job", data["gate"]["violations"])

    def test_fmt_value(self) -> None:
        self.assertEqual(qor_table.fmt_value("critical_path_delay_ns", 9.3045), "9.305")
        self.assertEqual(qor_table.fmt_value("vpr_time_s", 4.0600000000000005), "4.1")
        self.assertEqual(qor_table.fmt_value("max_vpr_mem_mib", 2264.4), "2264")
        self.assertEqual(qor_table.fmt_value("num_clb", 131.0), "131")
        self.assertEqual(qor_table.fmt_value("num_clb", None), "")
        self.assertEqual(qor_table.leg_name("Pprime"), "P'")


if __name__ == "__main__":
    unittest.main()
