"""Tests for tools/vtr-qor/qor_table.py, the verdict half (3H plan Task 8; spec §6.1, §6.2, T3).

The runs are synthetic: built in the test, or written as a small job tree under a temporary
directory. Nothing here runs VTR.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from collections.abc import Mapping, Sequence
from pathlib import Path

import qor_legs
import qor_sched
import qor_table
from qor_heavy import HeavyError
from qor_table import Bands, GateInputs, Run, Verdict

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
        runs = replace(table(1.1), run("O", "sha", override={"num_clb": -1}))
        ratios = qor_table.pair_ratios(runs, ("O", "P"), "num_clb", GATE, gated=True)
        self.assertEqual(len(ratios), 19)
        mean, n = qor_table.geomean_n(ratios)
        self.assertEqual(n, 18)
        assert mean is not None
        self.assertAlmostEqual(mean, 1.1, places=12)
        self.assertEqual(qor_table.geomean_n([None, None]), (None, 0))

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
        got = verdict(drop(table(), "O", "bgm"))
        self.assertVerdict(got, "FAIL", "table: bgm has no ok O job")
        got = verdict(replace(table(), run("O", "bgm", status="failed",
                                           override={"vpr_status": None})))
        self.assertVerdict(got, "FAIL", "table: bgm has no ok O job")

    def test_gate_incomplete_reference(self) -> None:
        runs = replace(table(), run("P", "mcml", status="capped"))
        self.assertEqual(verdict(runs), Verdict(
            state="INCOMPLETE",
            violations=("gate: INCOMPLETE: mcml P is capped (reference); rerun it",)))
        runs = replace(runs, run("O", "sha", override={"min_chan_width": -1}))
        self.assertVerdict(verdict(runs), "INCOMPLETE",
                           "gate: INCOMPLETE: mcml P is capped (reference); rerun it",
                           "gate: FAIL: min_chan_width missing for sha (O)")
        self.assertVerdict(verdict(drop(table(), "P", "bgm")), "INCOMPLETE",
                           "gate: INCOMPLETE: bgm P is missing (reference); rerun it")

    def test_gate_incomplete_attribution_reference(self) -> None:
        runs = table(legs=("P", "Oprime", "O"))
        self.assertEqual(verdict(runs).state, "PASS")
        runs = replace(runs, run("Oprime", "bgm", status="timeout"))
        self.assertVerdict(verdict(runs), "INCOMPLETE",
                           "gate: INCOMPLETE: bgm Oprime is timeout (reference); rerun it")

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

    def test_gate_fcheck(self) -> None:
        fail = replace(table(), run("O", "sha", override={"fcheck": "fail"}))
        self.assertEqual(verdict(fail), Verdict(state="FAIL", violations=(
            "gate: FAIL: sha O fcheck fail (no row in divergences.tsv)",)))
        rows = [("sha", "mem-init-dropped", "decision 6 step 5")]
        self.assertEqual(verdict(fail, divergences=rows).state, "PASS")
        rows = [("sha", "async-reset-made-sync", "decision 19")]
        self.assertEqual(verdict(fail, divergences=rows).state, "PASS")
        rows = [("bgm", "mem-init-dropped", "another design")]
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
        skipped = replace(table(), run("O", "sha", override={"fcheck": "skipped"}))
        self.assertVerdict(verdict(skipped), "FAIL", "gate: FAIL: sha O fcheck skipped")

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
        ram = replace(table(), run("O", "mcml", override={"equiv": "ram"}))
        self.assertEqual(verdict(ram).state, "PASS")

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

    def test_gate_collects_in_rule_order(self) -> None:
        runs = drop(table(), "O", "bgm")
        runs = replace(runs, run("O", "sha", override={"num_mult": 5, "rerun": "differs"}))
        runs = replace(runs, run("O", "diffeq1", 1.6))
        got = verdict(runs)
        self.assertEqual(got.violations, (
            "table: bgm has no ok O job",
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
        write_job(self.root, "P", key, "mcml", status="capped")
        write_job(self.root, "P", key, "sha")  # status ok, no metrics.json: partial
        stale = {**metrics("P", "bgm"), "status": "failed"}
        write_job(self.root, "P", key, "bgm", data=stale)  # ok word, a failed record
        write_job(self.root, "P", key, "diffeq1", data=metrics("P", "diffeq2"))  # wrong design
        good = write_job(self.root, "P", key, "or1200", status="failed",
                         data=metrics("P", "or1200"))
        self.latest("P", key)
        got = {r.design: r.status for r in qor_table.load_runs(self.root, VTR12, ARCH, {})}
        self.assertEqual(got, {"mcml": "capped", "sha": "incomplete", "bgm": "incomplete",
                               "diffeq1": "incomplete", "or1200": "failed"})
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
        self.assertEqual(refused({}), text)  # a leg directory without latest
        self.assertEqual(refused({"P": "bbbbbbbbbbbb"}), text)
        self.assertEqual(refused({"P": "../../xx"}), text)
        self.latest("P", "not-a-key")
        self.assertEqual(refused({}), text)
        self.latest("P", "aaaaaaaaaaaa")
        self.assertEqual(refused({"O'": "aaaaaaaaaaaa"}, ["P", "Oprime"]),
                         f"table: no complete key for leg Oprime on {ARCH}")
        self.assertEqual(len(qor_table.load_runs(self.root, VTR12, ARCH, {})), 1)


if __name__ == "__main__":
    unittest.main()
