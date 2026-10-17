"""Tests for tools/opt-check/opt-nightly (2F Task 20, spec 20.2, 20.3): the nightly VTR legs.
The tool runs are replaced by a scripted runner, so no Odin III or Yosys runs."""

from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import elab_sweep
import memcap
import opt_nightly as nightly

from . import helpers

REAL_EXPECT = [nightly.REPO / "tests/opt/vtr_expect_fail.txt",
               nightly.REPO / "tests/opt/vtr_nightly_xl.txt"]


def make_tree(root: Path, sizes: dict[str, int]) -> None:
    for rel, size in sizes.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x" * size, encoding="utf-8")


FAKE = {"mcml.v": 900, "a.v": 30, "b.v": 10, "c.v": 20, "koios/lenet.v": 1000,
        "koios/k1.v": 15, "koios/hard_mem_include.v": 5}


class Runs:
    """A scripted runner: records (file, label, cap) and answers by a function."""

    def __init__(self, answer: object = None) -> None:
        self.calls: list[tuple[str, str, int]] = []
        self.answer = answer

    def __call__(self, _cfg: nightly.Config, unit: nightly.Unit, item: elab_sweep.Item,
                 cap: int) -> nightly.Result:
        self.calls.append((item.rel, unit.label, cap))
        if callable(self.answer):
            res = self.answer(item.rel, unit.label)
            assert isinstance(res, nightly.Result)
            return res
        return nightly.Result(0, 0.1, "", False)


def ok() -> nightly.Result:
    return nightly.Result(0, 0.1, "", False)


def bad(text: str = "odin3: error: boom", code: int = 1, hard: bool = False) -> nightly.Result:
    return nightly.Result(code, 0.1, text, hard)


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "verilog"
        make_tree(self.root, FAKE)
        self.expect_file = Path(self._tmp.name) / "expect.txt"
        self.expect_file.write_text("", encoding="utf-8")

    def args(self, **kw: object) -> argparse.Namespace:
        base: dict[str, object] = dict(
            odin3="odin3", yosys="yosys", set="vtr31", xl=False, keep=None,
            expect_fail=[self.expect_file], diff_expect=Path("diff.txt"), root=self.root,
            tops=None, techlib=None, leg="both", mem_cap=3 << 30, cap_for=[], limit=None,
            timeout=60.0, min_changed_total=0)
        base.update(kw)
        return argparse.Namespace(**base)

    def execute(self, runner: Runs, **kw: object) -> tuple[int, list[str]]:
        lines: list[str] = []
        code = nightly.execute(self.args(**kw), runner, lines.append)
        return code, lines


class MinChangedTest(Base):
    def test_a_leg_in_which_odin3_changes_nothing_fails_the_minimum(self) -> None:
        """Review N3: a do-nothing odin3 passed the equiv leg."""
        silent = Runs(lambda rel, label: nightly.Result(
            0, 0.1, "opt-equiv: 0 calls changed something: 0 refine, 0 do not\n", False))
        self.assertEqual(self.execute(silent)[0], 0)
        code, lines = self.execute(silent, min_changed_total=1, leg="equiv")
        self.assertEqual(code, 1)
        self.assertTrue(any("0 changing calls in total, want at least 1" in x for x in lines))
        busy = Runs(lambda rel, label: nightly.Result(
            0, 0.1, "opt-equiv: 5 calls changed something: 5 refine, 0 do not\n", False))
        self.assertEqual(self.execute(busy, min_changed_total=20, leg="equiv")[0], 0)
        self.assertEqual(self.execute(busy, min_changed_total=21, leg="equiv")[0], 1)


class OrderTest(Base):
    def test_vtr31_is_ascending_size_with_mcml_last(self) -> None:
        names = [i.rel for i in nightly.collect_set(self.root, "vtr31")]
        self.assertEqual(names, ["b.v", "c.v", "a.v", "mcml.v"])

    def test_all64_adds_koios_and_puts_lenet_last(self) -> None:
        names = [i.rel for i in nightly.collect_set(self.root, "all64")]
        self.assertEqual(names, ["koios/hard_mem_include.v", "b.v", "koios/k1.v", "c.v", "a.v",
                                 "mcml.v", "koios/lenet.v"])

    def test_a_missing_root_and_an_unknown_set_are_usage_errors(self) -> None:
        with self.assertRaises(nightly.UsageError):
            nightly.collect_set(self.root, "all65")
        code, _, err = helpers.run_main(
            nightly.main, ["--odin3", "o", "--yosys", "y", "--root", str(self.root / "nope")])
        self.assertEqual(code, 2)
        self.assertIn("no such directory", err)


class UnitsTest(unittest.TestCase):
    def test_leg_selects_one_tool(self) -> None:
        self.assertEqual([u.label for u in nightly.units("equiv")], ["opt-equiv"])
        self.assertEqual(len(nightly.units("diff")), 5)
        self.assertEqual(len(nightly.units()), 6)

    def test_opt_equiv_then_the_five_opt_diff_scripts(self) -> None:
        found = nightly.units()
        self.assertEqual(found[0].label, "opt-equiv")
        diffs = [u for u in found[1:]]
        self.assertEqual([u.args for u in diffs],
                         [("--script", s) for s in ("phase2", "opt -full", "opt -fast",
                                                    "opt -fast -noff", "opt -nodffe -nosdff")])
        self.assertTrue(all(u.tool == nightly.OPT_DIFF for u in diffs))

    def test_the_scripts_are_the_ones_opt_diff_accepts(self) -> None:
        import opt_diff
        for script in nightly.SCRIPTS:
            opt_diff.script_named(script)

    def test_argv_carries_the_cap_top_expect_root_and_keep_dir(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg = nightly.Config("O", "Y", Path("/r"), 3 << 30, {}, {"d.v": "top"}, "L.o3lib",
                                 Path("de.txt"), Path(tmp), 60.0)
            item = elab_sweep.Item(Path("/r/koios/d.v"), "koios/d.v", 1)
            argv = nightly.unit_argv(cfg, nightly.units()[1], item, 5 << 30)
            self.assertEqual(argv[2:6], ["--odin3", "O", "--yosys", "Y"])
            self.assertEqual(argv[argv.index("--mem-cap") + 1], str(5 << 30))
            self.assertEqual(argv[argv.index("--top") + 1], "top")
            self.assertEqual(argv[argv.index("--script") + 1], "phase2")
            self.assertEqual(argv[argv.index("--expect") + 1], "de.txt")
            self.assertEqual(argv[argv.index("--root") + 1], "/r")
            self.assertTrue(Path(argv[argv.index("--work") + 1]).is_dir())
            self.assertEqual(argv[-1], "/r/koios/d.v")
            eq = nightly.unit_argv(cfg, nightly.units()[0], item, 1 << 30)
            self.assertNotIn("--expect", eq)

    def test_run_unit_goes_through_memcap_run(self) -> None:
        cfg = nightly.Config("O", "Y", Path("/r"), 3 << 30, {}, {}, None, Path("de"), None, 9.0)
        item = elab_sweep.Item(Path("/r/a.v"), "a.v", 1)
        done = mock.Mock(returncode=0, stdout="fine\n", stderr="")
        with mock.patch.object(memcap, "run", return_value=done) as run:
            res = nightly.run_unit(cfg, nightly.units()[0], item, 3 << 30)
        self.assertEqual(res.code, 0)
        self.assertEqual(run.call_args.args[1], 3 << 30)
        self.assertEqual(run.call_args.kwargs["timeout"], 9.0)

    def test_exit_3_of_a_tool_is_hard(self) -> None:
        """Review I1: opt-equiv/opt-diff exit 3 for a crash, kill or timeout of a child."""
        cfg = nightly.Config("O", "Y", Path("/r"), 3 << 30, {}, {}, None, Path("de"), None, 9.0)
        item = elab_sweep.Item(Path("/r/a.v"), "a.v", 1)
        for code, hard in ((3, True), (-11, True), (1, False), (2, False)):
            done = mock.Mock(returncode=code, stdout="a message\n", stderr="")
            with mock.patch.object(memcap, "run", return_value=done):
                res = nightly.run_unit(cfg, nightly.units()[0], item, 3 << 30)
            self.assertEqual(res.hard, hard, code)
        # opt-diff has no unproven class: a cap kill of it stays hard
        done = mock.Mock(returncode=-9, stdout="", stderr="")
        with mock.patch.object(memcap, "run", return_value=done):
            res = nightly.run_unit(cfg, nightly.units("diff")[0], item, 3 << 30)
        self.assertTrue(res.hard)
        self.assertFalse(res.unproven)

    def test_opt_equiv_exit_4_and_a_cap_kill_of_the_tool_are_unproven_not_hard(self) -> None:
        """2F VTR triage: a proof that timed out says "timed out" in its lines (a HARD_MARK) but
        is the checker's size limit; opt-equiv killed by its own cap is the checker's too (Odin
        III's cap kill is the tool's exit 3). Both are listable."""
        cfg = nightly.Config("O", "Y", Path("/r"), 3 << 30, {}, {}, None, Path("de"), None, 9.0)
        item = elab_sweep.Item(Path("/r/a.v"), "a.v", 1)
        lines = ("opt-equiv: 0001-opt_expr: inconclusive (yosys-refine: timed out after 60 s)\n"
                 "opt-equiv: unproven: 2 calls hit the SAT size limit (timeout or cap kill)\n")
        for code in (4, -9, 137):
            done = mock.Mock(returncode=code, stdout=lines, stderr="")
            with mock.patch.object(memcap, "run", return_value=done):
                res = nightly.run_unit(cfg, nightly.units()[0], item, 3 << 30)
            self.assertTrue(res.unproven, code)
            self.assertFalse(res.hard, code)
            self.assertIn("unproven", res.text, code)


class RunTest(Base):
    def test_every_file_runs_six_units_smallest_first_and_prints_a_line_each(self) -> None:
        runs = Runs()
        code, lines = self.execute(runs)
        self.assertEqual(code, 0)
        self.assertEqual(len(runs.calls), 4 * 6)
        self.assertEqual([c[0] for c in runs.calls[::6]], ["b.v", "c.v", "a.v", "mcml.v"])
        self.assertEqual(len(lines), 4 * 6 + 1)
        self.assertIn("opt-nightly: b.v [opt-equiv]: ok", lines[0])
        self.assertIn("opt-nightly: b.v [opt-diff[opt -fast -noff]]: ok", lines[4])
        self.assertEqual(
            lines[-1], "opt-nightly: summary: set vtr31: 4 files (0 skipped as xl or unselected), "
            "24 runs: 24 ok, 0 fail as listed, 0 UNEXPECTED")

    def test_limit_runs_the_smallest_files_only(self) -> None:
        runs = Runs()
        code, lines = self.execute(runs, limit=3)
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in runs.calls[::6]], ["b.v", "c.v", "a.v"])
        self.assertIn("3 files (1 skipped", lines[-1])

    def test_only_runs_the_named_files_in_size_order_and_an_unknown_name_is_an_error(self) -> None:
        runs = Runs()
        code, lines = self.execute(runs, only=["a.v", "b.v"])
        self.assertEqual(code, 0)
        self.assertEqual([c[0] for c in runs.calls[::6]], ["b.v", "a.v"])
        self.assertIn("2 files (2 skipped", lines[-1])
        with self.assertRaises(nightly.UsageError):
            self.execute(Runs(), only=["nope.v"])

    def test_limit_0_selects_nothing_and_is_a_usage_error_not_a_pass(self) -> None:
        with self.assertRaises(nightly.UsageError):
            self.execute(Runs(), limit=0)

    def test_cap_for_raises_one_files_cap(self) -> None:
        runs = Runs()
        self.execute(runs, cap_for=["mcml.v=5G"])
        caps = {c[0]: c[2] for c in runs.calls}
        self.assertEqual(caps["mcml.v"], 5 << 30)
        self.assertEqual(caps["a.v"], 3 << 30)

    def test_an_unlisted_failure_fails_the_run(self) -> None:
        code, lines = self.execute(
            Runs(lambda f, label: bad("diff") if (f, label) == ("a.v", "opt-equiv") else ok()))
        self.assertEqual(code, 1)
        self.assertTrue(any("a.v [opt-equiv]: FAIL: diff" in line for line in lines))
        self.assertIn("1 UNEXPECTED", lines[-1])

    def test_a_hard_failure_is_never_expected(self) -> None:
        self.expect_file.write_text("a.v out of memory\n", encoding="utf-8")
        code, lines = self.execute(Runs(lambda f, _l: bad("out of memory (cap 3G)", 2, True)
                                        if f == "a.v" else ok()))
        self.assertEqual(code, 1)
        self.assertTrue(any("HARD FAIL" in line for line in lines))

    def test_an_unproven_file_is_listable_and_unlisted_it_is_named_unproven(self) -> None:
        unproven = nightly.Result(4, 0.1, "opt-equiv: unproven: 2 calls hit the SAT size limit",
                                  False, True)
        answer = Runs(lambda f, label: unproven if (f, label) == ("a.v", "opt-equiv") else ok())
        code, lines = self.execute(answer)
        self.assertEqual(code, 1)
        self.assertTrue(any("a.v [opt-equiv]: UNPROVEN:" in line for line in lines))
        self.expect_file.write_text("a.v [unproven:] untested for now (size limit): 60 s timeout\n",
                                    encoding="utf-8")
        code, lines = self.execute(answer)
        self.assertEqual(code, 0, lines)
        self.assertTrue(any("a.v [opt-equiv]: fails as listed (untested for now" in line
                            for line in lines))

    def test_a_listed_failure_passes_and_a_listed_pass_is_stale(self) -> None:
        self.expect_file.write_text("a.v [boom] a known reason\nb.v a known reason\n",
                                    encoding="utf-8")
        code, lines = self.execute(Runs(lambda f, _l: bad() if f == "a.v" else ok()))
        self.assertEqual(code, 1)
        self.assertTrue(any("a.v [opt-equiv]: fails as listed (a known reason)" in x
                            for x in lines))
        self.assertTrue(any("b.v: STALE entry" in line for line in lines))
        self.assertIn("6 fail as listed, 1 UNEXPECTED", lines[-1])

    def test_a_listed_failure_without_its_needle_is_unexpected(self) -> None:
        self.expect_file.write_text("a.v [other] reason\n", encoding="utf-8")
        code, lines = self.execute(Runs(lambda f, _l: bad() if f == "a.v" else ok()))
        self.assertEqual(code, 1)
        self.assertTrue(any("the needle 'other' is missing" in line for line in lines))

    def test_a_skip_from_a_tool_skips_the_run(self) -> None:
        with self.assertRaises(nightly.SkipRun):
            self.execute(Runs(lambda _f, _l: bad("", 77)))


class XlTest(Base):
    def setUp(self) -> None:
        super().setUp()
        self.expect_file.write_text("koios/lenet.v xl: too big\n", encoding="utf-8")

    def test_without_xl_the_listed_file_is_skipped(self) -> None:
        runs = Runs()
        self.execute(runs, set="all64")
        self.assertNotIn("koios/lenet.v", {c[0] for c in runs.calls})
        self.assertEqual(len({c[0] for c in runs.calls}), 6)

    def test_with_xl_only_the_listed_file_runs_and_must_pass(self) -> None:
        runs = Runs()
        code, lines = self.execute(runs, set="all64", xl=True)
        self.assertEqual(code, 0)
        self.assertEqual({c[0] for c in runs.calls}, {"koios/lenet.v"})
        self.assertEqual(len(runs.calls), 6)
        code, _ = self.execute(Runs(lambda _f, _l: bad()), set="all64", xl=True)
        self.assertEqual(code, 1)

    def test_an_empty_xl_selection_exits_2(self) -> None:
        with self.assertRaises(nightly.UsageError):
            self.execute(Runs(), set="vtr31", xl=True)  # lenet.v is not in vtr31
        code, _, err = helpers.run_main(nightly.main, [
            "--odin3", "o", "--yosys", "y", "--root", str(self.root), "--xl",
            "--expect-fail", str(self.expect_file)])
        self.assertEqual(code, 2)
        self.assertIn("selects no file", err)


class RealExpectTest(Base):
    def test_all64_xl_against_the_real_lists_selects_exactly_lenet(self) -> None:
        expect = elab_sweep.merge_expect(REAL_EXPECT)
        items = nightly.collect_set(self.root, "all64")
        run, _ = nightly.select(items, expect, True)
        self.assertEqual([i.rel for i in run], ["koios/lenet.v"])

    def test_without_xl_the_real_lists_skip_lenet_and_keep_the_rest(self) -> None:
        expect = elab_sweep.merge_expect(REAL_EXPECT)
        items = nightly.collect_set(self.root, "all64")
        run, skipped = nightly.select(items, expect, False)
        self.assertEqual([i.rel for i in skipped], ["koios/lenet.v"])
        self.assertEqual(len(run), len(items) - 1)

    def test_the_real_xl_list_is_only_lenet_and_vtr31_has_no_xl(self) -> None:
        expect = elab_sweep.merge_expect(REAL_EXPECT)
        self.assertEqual([k for k, v in expect.items() if v.xl], ["koios/lenet.v"])
        with self.assertRaises(nightly.UsageError):
            nightly.select(nightly.collect_set(self.root, "vtr31"), expect, True)


if __name__ == "__main__":
    unittest.main()
