"""Tests for tools/opt-check/opt-equiv (2F Task 17, spec 20.2): refinement after every sub-pass.

Fakes stand in for both tools in the argv tests: the fake ``odin3`` writes the snapshot pairs of
``fake.json`` into the ``--opt-snapshot`` directory and prints ``log``; the fake ``yosys`` exits 1
when the script's gate file holds the word ``BAD``; on a bounded check (``sat -seq``) it prints a
trace when the gate holds ``BASEFAIL`` (the base case) or ``OUTFAIL`` (the output check), else
no trace. ``memcap.run`` is a plain subprocess call. The
probe class runs the real Yosys 0.55 (``ODIN3_YOSYS055``) and is skipped without it.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import clock_sig
import hard_blocks as hb
import memcap
import opt_equiv as oe

from . import helpers

FAKE_ODIN = r"""#!/usr/bin/env python3
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
with open(HERE / "odin.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
snap = Path(sys.argv[sys.argv.index("--opt-snapshot") + 1])
for name, text in spec["files"].items():
    (snap / name).write_text(text)
sys.stderr.write(spec["log"])
sys.exit(spec.get("exit", 0))
"""

FAKE_YOSYS = r"""#!/usr/bin/env python3
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "yosys.log", "a") as log:
    log.write(script + "\n")
gate = re.findall(r"read_rtlil ([^;\s]+)", script)[1]
if "sat -seq" in script:
    marker = "BASEFAIL" if "expose -shared" in script else "OUTFAIL"
    found = marker in Path(gate).read_text()
    sys.stdout.write("SAT proof finished - model found: FAIL!\n" if found else
                     "SAT proof finished - no model found: SUCCESS!\n")
    sys.exit(0)
if "BAD" in Path(gate).read_text():
    sys.stdout.write("ERROR: Found 1 unproven $equiv cells in 'equiv_status -assert'.\n")
    sys.exit(1)
"""

ODIN_LOG = "odin3: info: opt: rule const_and 2\nodin3: info: opt: rule opt_dff.add_en 1\n"
IL_GOOD = "attribute \\top 1\nmodule \\t\n  wire input 1 \\a\nend\n"
IL_BAD = "attribute \\top 1\nmodule \\t\n  wire input 1 \\a\n  # BAD\nend\n"


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


def yosys_exits(status: int) -> Any:
    """A memcap.run stand-in: the refinement check's Yosys exits with ``status``."""
    def effect(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
        if "equiv_make" in " ".join(argv):
            return subprocess.CompletedProcess(list(argv), status, "", "")
        return fake_run(argv, cap, **kw)
    return effect


class Fakes:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir()
        for name, text in (("odin3", FAKE_ODIN), ("yosys", FAKE_YOSYS)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        (root / "d.v").write_text("module t; endmodule\n")

    def spec(self, files: dict[str, str], **extra: Any) -> None:
        (self.bin / "fake.json").write_text(json.dumps(
            {"files": files, "log": ODIN_LOG, **extra}))

    def main(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.bin / "odin3"), "--yosys", str(self.bin / "yosys"),
                "--work", str(self.root / "work"), *extra, str(self.root / "d.v")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(oe.main, argv)

    def log(self, name: str) -> list[str]:
        return (self.bin / name).read_text().splitlines()


class PureTest(unittest.TestCase):
    def test_script_default_lowering(self) -> None:
        self.assertEqual(
            oe.refinement_script("g.il", "h.il"),
            "read_rtlil g.il; read_rtlil h.il; flatten; expose -evert c:* t:$* %d; "
            "clk2fflogic; opt_merge; opt_clean; "
            "techmap; equiv_make gold gate eq; hierarchy -top eq; flatten; "
            "equiv_simple -undef -seq 4; equiv_induct -undef; equiv_status -assert")

    def test_script_variants(self) -> None:
        plain = oe.refinement_script("g.il", "h.il", lowering=())
        self.assertNotIn("clk2fflogic", plain)
        self.assertNotIn("proc", plain)
        self.assertIn("read_rtlil h.il; flatten; expose -evert c:* t:$* %d; "
                      "equiv_make gold gate eq", plain)
        procs = oe.refinement_script("g.il", "h.il", procs=True, lowering=oe.ONE_CLOCK[0])
        self.assertIn("read_rtlil h.il; proc; flatten; expose -evert c:* t:$* %d; async2sync; "
                      "equiv_make",
                      procs)
        self.assertNotIn("techmap", procs)
        self.assertIn("flatten; expose -evert c:* t:$* %d; memory; clk2fflogic; opt_merge; "
                      "opt_clean; techmap; equiv_make",
                      oe.refinement_script("g.il", "h.il", lowering=oe.LOWERINGS[2]))
        self.assertIn("techmap; opt_clean -purge; equiv_make",
                      oe.refinement_script("g.il", "h.il", purge=True))

    def test_a_purged_retry_proves_what_the_plain_script_cannot(self) -> None:
        """Every lowering is tried plain first, then with `opt_clean -purge`: a proof in either is
        a proof (purging drops unread wires, and with them names that help equiv_make)."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        work = Path(tmp.name)
        (work / "g.il").write_text(IL_GOOD)
        (work / "h.il").write_text(IL_GOOD)
        seen: list[bool] = []

        def child(argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
            purged = "opt_clean -purge" in " ".join(argv)
            seen.append(purged)
            out = "" if purged else (
                "ERROR: Found 1 unproven $equiv cells in 'equiv_status -assert'.")
            return subprocess.CompletedProcess(list(argv), 0 if purged else 1, out, "")
        checker = oe.Checker("y", work, 1 << 30)
        with mock.patch.object(checker, "child", side_effect=child):
            ok, _ = checker.refines(work / "g.il", work / "h.il", "p")
        self.assertTrue(ok)
        self.assertEqual(seen[:len(seen) - 1].count(True), 0)  # plain lowerings first
        self.assertTrue(seen[-1])

    def test_a_memory_refusal_does_not_hide_the_timeout_of_the_lowering_that_ran(self) -> None:
        """or1200.v (2F VTR triage): `equiv_make` refuses memories in the plain and the techmap
        lowering ("Gold module contains memories or processes"), the third lowering runs `memory`
        first and its proof (a 32x32 multiplier) hits the timeout. That is a size limit, not the
        tool error of the refusals; when every lowering refuses it stays a RefineAbort."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        work = Path(tmp.name)
        (work / "g.il").write_text(IL_GOOD)
        (work / "h.il").write_text(IL_GOOD)
        refuse = "ERROR: Gold module contains memories or processes. Run 'memory' or 'proc'."

        def child(argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
            if "memory;" in " ".join(argv):
                raise oe.CrashError("yosys-refine", "timed out after 60 s", True)
            return subprocess.CompletedProcess(list(argv), 1, refuse, "")
        checker = oe.Checker("y", work, 1 << 30)
        with mock.patch.object(checker, "child", side_effect=child), \
                self.assertRaises(oe.CrashError) as ctx:
            checker.refines(work / "g.il", work / "h.il", "p")
        self.assertTrue(ctx.exception.size_limit)

        def refuses(argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(list(argv), 1, refuse, "")
        with mock.patch.object(checker, "child", side_effect=refuses), \
                self.assertRaises(oe.RefineAbort):
            checker.refines(work / "g.il", work / "h.il", "p")

    def test_a_real_difference_survives_the_purged_retry(self) -> None:
        """A difference on a used wire or an output is not an unread wire: the plain tries and the
        purged tries all fail, and the answer stays False."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        work = Path(tmp.name)
        fakes = Fakes(work)
        (work / "g.il").write_text(IL_GOOD)
        (work / "h.il").write_text(IL_BAD)
        checker = oe.Checker(str(fakes.bin / "yosys"), work, 1 << 30)
        with mock.patch.object(memcap, "run", fake_run):
            ok, text = checker.refines(work / "g.il", work / "h.il", "q")
        self.assertFalse(ok)
        self.assertIn("unproven $equiv", text)
        scripts = (fakes.bin / "yosys.log").read_text().splitlines()
        self.assertTrue(any("opt_clean -purge" in line for line in scripts))
        self.assertTrue(any("opt_clean -purge" not in line for line in scripts))

    def test_flows_follow_the_clocks(self) -> None:
        checker = oe.Checker("y", Path("."), 1 << 30)
        self.assertEqual(checker.flows(True), list(oe.ONE_CLOCK))
        self.assertEqual(checker.flows(False), list(oe.LOWERINGS))
        for lowering in oe.LOWERINGS:
            self.assertIn("clk2fflogic", lowering)
        no_techmap = oe.Checker("y", Path("."), 1 << 30, techmap=False)
        self.assertEqual(no_techmap.flows(True), [oe.ONE_CLOCK[0]])

    def test_phase2_commands_and_stats(self) -> None:
        commands = oe.phase2_commands()
        self.assertEqual(commands[0], "opt_expr")
        self.assertIn("proc", commands)
        stats = oe.with_stats(commands)
        self.assertIn("opt -nodffe -nosdff --opt-stats", stats)
        self.assertIn("opt --opt-stats", stats)
        self.assertIn("proc", stats)  # not an opt word
        self.assertIn("compact", stats)

    def test_phase3_script_and_the_wreduce_call(self) -> None:
        commands = oe.phase2_commands(oe.HERE / "phase3.script")
        self.assertEqual(commands[commands.index("wreduce") + 1:commands.index("compact")],
                         ["opt_clean", "opt -full"])
        self.assertIn("wreduce --opt-stats", oe.with_stats(commands))
        self.assertIsNotNone(oe.PAIR_NAME.match("0019-wreduce-before.il"))
        self.assertIsNotNone(oe.PAIR_NAME.match("0003-opt_expr-before.il"))

    def test_read_commands(self) -> None:
        self.assertEqual(oe.read_commands(["a.v", "b c.v"], "t", True),
                         ["read_verilog --nolatches a.v \"b c.v\"", "hierarchy --top t"])
        self.assertEqual(oe.read_commands(["a.v"], None, False),
                         ["read_verilog a.v", "hierarchy -auto"])

    def test_rename_modules(self) -> None:
        text = ("attribute \\top 1\nmodule \\t\n  cell \\sub \\u\n  end\n  cell $add \\a\n  end\n"
                "end\nmodule \\sub\nend\n")
        top = oe.top_module(text)
        self.assertEqual(top, "\\t")
        out = oe.rename_modules(text, "gold", top)
        self.assertIn("module \\gold\n", out)
        self.assertIn("module \\gold.sub\n", out)
        self.assertIn("cell \\gold.sub \\u", out)
        self.assertIn("cell $add \\a", out)
        self.assertNotIn("attribute \\top", out)

    def test_top_module_choice(self) -> None:
        self.assertEqual(oe.top_module("module \\a\nend\n"), "\\a")
        two = "module \\a\nend\nmodule \\b\nend\n"
        self.assertEqual(oe.top_module(two, "b"), "\\b")
        with self.assertRaises(oe.ToolError):
            oe.top_module(two)

    def test_has_processes(self) -> None:
        self.assertTrue(oe.has_processes("module \\a\n  process $p\n  end\nend\n"))
        self.assertFalse(oe.has_processes("module \\a\n  wire \\processor\nend\n"))

    def test_counters(self) -> None:
        got = oe.parse_counters(ODIN_LOG + "opt: rule const_and 3\n")
        self.assertEqual(got, {"const_and": 5, "opt_dff.add_en": 1})
        self.assertEqual(oe.counter_text(got), "const_and 5, opt_dff.add_en 1")
        self.assertEqual(oe.counter_text({}), "none")


class MainTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.fakes = Fakes(Path(self._tmp.name))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def pair(self, seq: str, sub: str, after: str = IL_GOOD) -> dict[str, str]:
        return {f"{seq}-{sub}-before.il": IL_GOOD, f"{seq}-{sub}-after.il": after}

    def test_all_pairs_refine(self) -> None:
        self.fakes.spec({**self.pair("0001", "opt_expr"), **self.pair("0003", "opt_clean")})
        code, out, err = self.fakes.main("--top", "t")
        self.assertEqual(code, 0, out + err)
        self.assertIn("opt-equiv: 0001-opt_expr: ok", out)
        self.assertIn("opt-equiv: 0003-opt_clean: ok", out)
        self.assertIn("2 calls changed something: 2 refine, 0 do not", out)
        script = self.fakes.log("odin.log")[0]
        self.assertIn("--top t --opt-snapshot ", script)
        self.assertIn("-p read_verilog --nolatches ", script)
        self.assertIn("hierarchy --top t; opt_expr --opt-stats; opt_clean --opt-stats; "
                      "opt -nodffe -nosdff --opt-stats; proc; opt --opt-stats; compact", script)

    def test_failing_pair_is_named_with_counters(self) -> None:
        self.fakes.spec({**self.pair("0001", "opt_expr"),
                         **self.pair("0002", "opt_merge", IL_BAD)})
        code, out, _ = self.fakes.main()
        self.assertEqual(code, 1)
        self.assertIn("opt-equiv: 0001-opt_expr: ok", out)
        self.assertIn("opt-equiv: 0002-opt_merge: not a refinement (rules: "
                      "const_and 2, opt_dff.add_en 1)", out)
        self.assertIn("1 refine, 1 do not", out)

    def test_pairs_run_in_sequence_order(self) -> None:
        self.fakes.spec({**self.pair("0010", "opt_dff"), **self.pair("0002", "opt_expr")})
        _, out, _ = self.fakes.main()
        self.assertLess(out.index("0002-opt_expr"), out.index("0010-opt_dff"))
        proofs = [line for line in self.fakes.log("yosys.log") if "equiv_make" in line]
        self.assertEqual(len(proofs), 2)

    def test_a_failed_base_case_with_an_output_trace_is_not_a_refinement(self) -> None:
        """H2: the proof passes, the base case from the initial state fails and the output
        check finds a trace (an init value changed): not a refinement, exit 1."""
        self.fakes.spec(self.pair("0001", "opt_dff", IL_GOOD + "# BASEFAIL OUTFAIL\n"))
        code, out, _ = self.fakes.main("--no-cec")
        self.assertEqual(code, oe.EXIT_FAIL, out)
        self.assertIn("0001-opt_dff: not a refinement", out)
        self.assertIn("the initial states differ", out)

    def test_a_failed_base_case_without_an_output_trace_is_never_ok(self) -> None:
        """The base case fails on a named wire but no output differs in BMC_STEPS steps: no
        verdict either way, so inconclusive (exit 4), never ok."""
        self.fakes.spec(self.pair("0001", "opt_dff", IL_GOOD + "# BASEFAIL\n"))
        code, out, _ = self.fakes.main("--no-cec")
        self.assertEqual(code, oe.EXIT_UNPROVEN, out)
        self.assertIn("0001-opt_dff: inconclusive (the proof passed but its base case", out)
        self.assertNotIn(": ok", out)

    def test_a_failed_base_case_reaches_only_the_exact_cec(self) -> None:
        """Review H1: equiv-check starts a latch without init at 0, so it proved a pair whose
        base case found a trace (init dropped, 12 flops from the output). The cec now runs with
        ``need_init`` and its refusal is exit 4, never ok."""
        self.fakes.spec(self.pair("0001", "opt_dff", IL_GOOD + "# BASEFAIL\n"))
        refusal = (oe.CEC_UNPROVEN, "BLIF cec: not used, 1 latch(es) have no 0/1 init")
        with mock.patch.object(oe, "cec_ils", return_value=refusal) as cec:
            code, out, _ = self.fakes.main()
        self.assertEqual(code, oe.EXIT_UNPROVEN, out)
        self.assertEqual(cec.call_args.args[-1], True)  # need_init
        self.assertIn("not decided by the BLIF cec: BLIF cec: not used", out)
        self.assertNotIn(": ok", out)
        self.assertNotIn("proved by the BLIF cec", out)

    def test_a_timeout_is_rescued_by_the_cec_only_with_an_init_on_every_latch(self) -> None:
        """A proof timeout may go to the cec; the cec step refuses latches without a 0/1 init."""
        self.fakes.spec(self.pair("0001", "opt_expr"))

        def hang(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                raise subprocess.TimeoutExpired(list(argv), 60.0)
            return fake_run(argv, cap, **kw)
        with mock.patch.object(oe, "cec_ils", return_value=(oe.CEC_PROVED, "")) as cec:
            code, out, err = self.run_with(hang)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(cec.call_args.args[-1], True)  # need_init

    def test_the_cec_refuses_latches_without_a_defined_init(self) -> None:
        loose = ".model m\n.inputs c d\n.outputs q\n"
        cases = {"none": ".latch d q re c\n", "two": ".latch d q re c 2\n",
                 "three": ".latch d q re c 3\n", "bare": ".latch d q\n",
                 "bare3": ".latch d q 3\n", "wrapped": ".latch d q \\\n re c\n"}
        tight = {"zero": ".latch d q re c 0\n", "one": ".latch d q 1\n",
                 "comment": ".latch d q re c 1 # init 1\n"}
        with tempfile.TemporaryDirectory() as tmp:
            for name, line in {**cases, **tight}.items():
                path = Path(tmp) / f"{name}.blif"
                path.write_text(loose + line + ".end\n", encoding="utf-8")
                want = 1 if name in cases else 0
                with self.subTest(latch=name):
                    self.assertEqual(oe.latches_without_init(path), want)
                    if want:
                        with mock.patch.object(oe, "capped") as run:
                            got = oe.cec_blifs(path, path, 1 << 30, Path(tmp), True)
                        self.assertEqual(got[0], oe.CEC_UNPROVEN)
                        self.assertIn("no 0/1 init", got[1])
                        run.assert_not_called()

    def test_a_base_case_timeout_is_a_size_limit_not_a_pass(self) -> None:
        self.fakes.spec(self.pair("0001", "opt_expr"))

        def hang(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "expose -shared" in " ".join(argv):
                raise subprocess.TimeoutExpired(list(argv), 60.0)
            return fake_run(argv, cap, **kw)
        code, out, _ = self.run_with(hang, "--no-cec")
        self.assertEqual(code, oe.EXIT_UNPROVEN, out)
        self.assertIn("inconclusive (yosys-bmc: base case from the initial state", out)

    def test_every_proved_pair_gets_its_base_case(self) -> None:
        self.fakes.spec({**self.pair("0001", "opt_expr"), **self.pair("0002", "opt_clean")})
        code, out, err = self.fakes.main()
        self.assertEqual(code, 0, out + err)
        bases = [line for line in self.fakes.log("yosys.log") if "expose -shared" in line]
        self.assertEqual(len(bases), 2)
        self.assertIn(f"sat -seq {oe.BASE_STEPS} -set-init-undef", bases[0])

    def test_odin_failure_is_a_tool_error(self) -> None:
        self.fakes.spec({}, exit=3)
        code, _, err = self.fakes.main()
        self.assertEqual(code, 2)
        self.assertIn("odin3 exit 3", err)

    def test_lowering_flags_reach_yosys(self) -> None:
        self.fakes.spec(self.pair("0001", "opt_expr"))
        self.fakes.main("--no-lower")
        self.assertNotIn("clk2fflogic", self.fakes.log("yosys.log")[0])

    def test_unpaired_snapshot_is_ignored(self) -> None:
        self.fakes.spec({"0001-opt_expr-before.il": IL_GOOD, "README": "x"})
        code, out, _ = self.fakes.main()
        self.assertEqual(code, 0)
        self.assertIn("0 calls changed something", out)

    def run_with(self, side_effect: Any, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.fakes.bin / "odin3"), "--yosys", str(self.fakes.bin / "yosys"),
                "--work", str(self.fakes.root / "work"), *extra, str(self.fakes.root / "d.v")]
        with mock.patch.object(memcap, "run", side_effect=side_effect):
            return helpers.run_main(oe.main, argv)

    def test_an_odin3_timeout_is_hard_exit_3(self) -> None:
        """Review I1: an uncaught TimeoutExpired was a traceback with exit 1."""
        code, _, err = self.run_with(subprocess.TimeoutExpired(["odin3"], 1.0))
        self.assertEqual(code, 3, err)
        self.assertIn("timed out", err)
        self.assertNotIn("Traceback", err)

    def test_an_odin3_crash_is_hard_exit_3(self) -> None:
        for status in (-11, -9, 137):
            with self.subTest(status=status):
                done = subprocess.CompletedProcess(["odin3"], status, "", "boom")
                code, _, err = self.run_with(lambda *a, _d=done, **k: _d)
                self.assertEqual(code, 3, err)
                self.assertIn("odin3", err)

    def test_a_refinement_crash_is_named_yosys_refine_not_yosys(self) -> None:
        """Review N1: opt-gen-diff must tell our check's crash from the oracle's."""
        self.fakes.spec(self.pair("0001", "opt_expr"))
        code, out, err = self.run_with(yosys_exits(-11))
        self.assertEqual(code, 3, out + err)
        self.assertIn("yosys-refine", err)

    def test_a_yosys_error_abort_is_a_tool_error_not_a_refutation(self) -> None:
        """Review I3: an ERROR that is not the failed proof (parser, port, missing module)."""
        self.fakes.spec(self.pair("0001", "opt_expr"))
        for text in ("ERROR: Parser error in line 1: syntax error",
                     "ERROR: Can't find gate module x.", "ERROR: Can't match gate port y"):
            def abort(argv: Sequence[str], cap: int, _t: str = text, **kw: Any
                      ) -> subprocess.CompletedProcess[str]:
                if "equiv_make" in " ".join(argv):
                    return subprocess.CompletedProcess(list(argv), 1, _t + "\n", "")
                return fake_run(argv, cap, **kw)
            with self.subTest(text=text):
                code, out, err = self.run_with(abort)
                self.assertEqual(code, 2, out + err)
                self.assertNotIn("not a refinement", out + err)

    def test_no_sat_model_is_the_documented_no_proof_not_a_tool_error(self) -> None:
        self.fakes.spec(self.pair("0001", "opt_expr"))

        def nomodel(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                return subprocess.CompletedProcess(
                    list(argv), 1, "ERROR: No SAT model available for cell $c5 ($pow).\n", "")
            return fake_run(argv, cap, **kw)
        code, out, err = self.run_with(nomodel)
        self.assertEqual(code, 1, out + err)
        self.assertIn("not a refinement", out)

    def test_a_yosys_crash_or_timeout_is_hard_not_a_refutation(self) -> None:
        """Review I3: rc -9 or a timeout of the proof is not `not a refinement`."""
        self.fakes.spec(self.pair("0001", "opt_expr"))
        for status in (-11, -6):
            with self.subTest(status=status):
                code, out, err = self.run_with(yosys_exits(status))
                self.assertEqual(code, 3, out + err)
                self.assertNotIn("not a refinement", out + err)
        for status in (-9, 137):  # SIGKILL = the memory cap: a size limit, unproven, never a pass
            with self.subTest(status=status):
                code, out, err = self.run_with(yosys_exits(status), "--no-cec")
                self.assertEqual(code, oe.EXIT_UNPROVEN, out + err)
                self.assertIn("unproven", out)
                self.assertNotIn("not a refinement", out + err)

        def hang(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                raise subprocess.TimeoutExpired(list(argv), 60.0)
            return fake_run(argv, cap, **kw)
        code, out, err = self.run_with(hang, "--no-cec")
        self.assertEqual(code, oe.EXIT_UNPROVEN, out + err)
        self.assertIn("unproven", out)
        self.assertNotIn("not a refinement", out + err)

    def test_a_sat_timeout_falls_back_to_the_blif_cec(self) -> None:
        """A timed-out proof is inconclusive; the whole script then goes to equiv-check. Only a
        proof clears it: a cec that decides nothing, or finds a difference, fails the run."""
        self.fakes.spec(self.pair("0001", "opt_expr"))

        def hang(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                raise subprocess.TimeoutExpired(list(argv), 60.0)
            return fake_run(argv, cap, **kw)
        for verdict, code, needle in ((oe.CEC_PROVED, 0, "proved by the BLIF cec"),
                                      (oe.CEC_UNPROVEN, oe.EXIT_UNPROVEN, "not decided"),
                                      (oe.CEC_DIFFERS, oe.EXIT_FAIL, "NOT equivalent")):
            with self.subTest(verdict=verdict), mock.patch.object(
                    oe, "cec_ils", return_value=(verdict, "text")):
                got, out, err = self.run_with(hang)
                self.assertEqual(got, code, out + err)
                self.assertIn(needle, out)
                self.assertIn("inconclusive", out)

    def test_sat_is_not_retried_after_two_timeouts(self) -> None:
        self.fakes.spec({**self.pair("0001", "opt_expr"), **self.pair("0002", "opt_clean"),
                         **self.pair("0003", "opt_merge"), **self.pair("0004", "opt_dff")})
        calls: list[str] = []

        def hang(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                calls.append("x")
                raise subprocess.TimeoutExpired(list(argv), 60.0)
            return fake_run(argv, cap, **kw)
        code, out, _ = self.run_with(hang, "--no-cec")
        self.assertEqual(code, oe.EXIT_UNPROVEN, out)
        self.assertEqual(len(calls), oe.MAX_SAT_TIMEOUTS)
        self.assertIn("4 inconclusive (SAT size limit or initial state unproven)", out)
        self.assertIn("not tried after 2 SAT size-limit hits", out)

    def test_refine_timeout_reaches_the_proof(self) -> None:
        self.fakes.spec(self.pair("0001", "opt_expr"))
        seen: list[float] = []

        def spy(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                seen.append(kw["timeout"])
            return fake_run(argv, cap, **kw)
        saved = oe.REFINE_TIMEOUT_SECONDS
        self.addCleanup(setattr, oe, "REFINE_TIMEOUT_SECONDS", saved)
        code, out, err = self.run_with(spy, "--refine-timeout", "7")
        self.assertEqual(code, 0, out + err)
        self.assertEqual(set(seen), {7.0})

    def test_a_cec_lowering_crash_stays_unproven(self) -> None:
        crash = oe.CrashError("yosys (cec lowering)", "killed (exit -9)", cap_killed=True)
        with mock.patch.object(oe, "capped", side_effect=crash):
            verdict, text = oe.cec_ils("y", Path("g.il"), Path("h.il"), 1 << 30, Path("."), "t")
        self.assertEqual(verdict, oe.CEC_UNPROVEN)
        self.assertIn("BLIF cec:", text)
        done = subprocess.CompletedProcess([], 1, "", "ERROR: boom")
        with mock.patch.object(oe, "capped", return_value=done):
            verdict, text = oe.cec_ils("y", Path("g.il"), Path("h.il"), 1 << 30, Path("."), "t")
        self.assertEqual(verdict, oe.CEC_UNPROVEN)
        self.assertIn("lowering of the gold side failed", text)

    def test_cec_blif_verdicts(self) -> None:
        """equiv-check exit 0 proves, 1 differs, anything else (4 unproven, 2 unsupported) decides
        nothing."""
        def run(code: int) -> Any:
            done = subprocess.CompletedProcess([], code, "", "equiv-check: why\n")
            return mock.patch.object(oe, "capped", return_value=done)
        for code, want in ((0, oe.CEC_PROVED), (1, oe.CEC_DIFFERS), (4, oe.CEC_UNPROVEN),
                           (2, oe.CEC_UNPROVEN)):
            with run(code):
                self.assertEqual(oe.cec_blifs(Path("a"), Path("b"), 1 << 30, Path("."))[0], want)

    def test_a_cec_difference_with_x_nets_is_unproven_not_a_difference(self) -> None:
        """2F VTR triage (stereovision0 and 2): equiv-check ties the x net `unconn` to 0 on both
        sides, so a design with an x (an opt_dff of a flop that is never written, a mux with an x
        leg) differs from its own refinement; that is no verdict. Without an x it stays a
        difference."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        work = Path(tmp.name)
        plain = work / "plain.blif"
        plain.write_text(".model t\n.inputs a\n.outputs y\n.names unconn\n.names a y\n1 1\n.end\n")
        xed = work / "x.blif"
        xed.write_text(".model t\n.inputs a\n.outputs y\n.names unconn\n.names unconn a y\n"
                       "1- 1\n.end\n")
        self.assertFalse(oe.uses_x(plain))
        self.assertTrue(oe.uses_x(xed))
        longer = work / "longer.blif"  # a net called unconn_q or a.unconn is no x
        longer.write_text(".model t\n.inputs a\n.outputs y\n.names unconn\n"
                          ".names a.unconn unconn_q a y\n11- 1\n.end\n")
        self.assertFalse(oe.uses_x(longer))
        subckt = work / "subckt.blif"
        subckt.write_text(".model t\n.inputs a\n.outputs y\n.names unconn\n"
                          ".subckt m i=unconn o=y\n.end\n")
        self.assertTrue(oe.uses_x(subckt))
        self.assertFalse(oe.uses_x(work / "missing.blif"))
        done = subprocess.CompletedProcess([], 1, "", "equiv-check: NOT equivalent\n")
        with mock.patch.object(oe, "capped", return_value=done):
            self.assertEqual(oe.cec_blifs(plain, plain, 1 << 30, work)[0], oe.CEC_DIFFERS)
            verdict, text = oe.cec_blifs(plain, xed, 1 << 30, work)
            self.assertEqual(oe.cec_blifs(xed, plain, 1 << 30, work)[0], oe.CEC_UNPROVEN)
        self.assertEqual(verdict, oe.CEC_UNPROVEN)
        self.assertIn("unconn", text)

    def test_zero_changing_calls_are_reported_and_min_changed_fails_them(self) -> None:
        """Review I5."""
        self.fakes.spec({})
        code, out, _ = self.fakes.main()
        self.assertEqual(code, 0)
        self.assertIn("0 calls changed something", out)
        self.assertIn("NOTE: 0 calls changed anything", out)
        code, out, err = self.fakes.main("--min-changed", "1")
        self.assertEqual(code, 1, out + err)
        self.assertIn("0 calls changed something, want at least 1", out + err)
        self.fakes.spec(self.pair("0001", "opt_expr"))
        self.assertEqual(self.fakes.main("--min-changed", "1")[0], 0)
        code, out, _ = self.fakes.main("--min-changed", "2")
        self.assertEqual(code, 1, out)

    def test_require_sub_fails_a_run_in_which_that_sub_pass_changed_nothing(self) -> None:
        """Review M1 of feat/opt-share-parity: --min-changed counts any sub-pass."""
        self.fakes.spec(self.pair("0001", "opt_expr"))
        self.assertEqual(self.fakes.main("--require-sub", "opt_expr")[0], 0)
        code, out, err = self.fakes.main("--require-sub", "opt_share")
        self.assertEqual(code, 1, out + err)
        self.assertIn("no opt_share call changed something", out + err)
        self.fakes.spec({**self.pair("0001", "opt_expr"), **self.pair("0002", "opt_share")})
        self.assertEqual(self.fakes.main("--require-sub", "opt_share")[0], 0)

    def test_an_empty_snapshot_file_is_an_error_not_a_pass(self) -> None:
        self.fakes.spec({"0001-opt_expr-before.il": "", "0001-opt_expr-after.il": ""})
        code, _, err = self.fakes.main()
        self.assertEqual(code, 2)
        self.assertIn("empty snapshot", err)

    def test_work_dir_follows_tmpdir_and_is_removed_after_ok_and_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            argv = ["--odin3", str(self.fakes.bin / "odin3"), "--yosys",
                    str(self.fakes.bin / "yosys"), str(self.fakes.root / "d.v")]
            self.fakes.spec(self.pair("0001", "opt_expr"))
            with mock.patch.dict(os.environ, {"TMPDIR": tmpdir}), \
                    mock.patch.object(tempfile, "tempdir", None), \
                    mock.patch.object(memcap, "run", fake_run):
                self.assertEqual(helpers.run_main(oe.main, argv)[0], 0)
                self.assertEqual(list(Path(tmpdir).iterdir()), [])
                self.fakes.spec({}, exit=1)
                self.assertEqual(helpers.run_main(oe.main, argv)[0], 2)
                self.assertEqual(list(Path(tmpdir).iterdir()), [])
                self.fakes.spec({})  # a --min-changed shortfall: nothing to debug
                self.assertEqual(helpers.run_main(oe.main, [*argv, "--min-changed", "1"])[0], 1)
                self.assertEqual(list(Path(tmpdir).iterdir()), [])
                self.fakes.spec(self.pair("0001", "opt_expr", IL_BAD))  # a failing pair: kept
                self.assertEqual(helpers.run_main(oe.main, argv)[0], 1)
                self.assertEqual(len(list(Path(tmpdir).iterdir())), 1)


YOSYS = os.environ.get("ODIN3_YOSYS055", "")

MUX_X = """attribute \\top 1
module \\t
  wire width 4 input 1 \\a
  wire input 2 \\s
  wire width 4 output 3 \\y
  cell $mux \\m
    parameter \\WIDTH 4
    connect \\A 4'xxxx
    connect \\B \\a
    connect \\S \\s
    connect \\Y \\y
  end
end
"""
PLAIN_A = """attribute \\top 1
module \\t
  wire width 4 input 1 \\a
  wire input 2 \\s
  wire width 4 output 3 \\y
  connect \\y \\a
end
"""


def dffe(enable: str) -> str:
    return f"""attribute \\top 1
module \\t
  wire input 1 \\clk
  wire input 2 \\en
  wire input 3 \\d
  wire output 4 \\q
  wire \\ne
  cell $not \\inv
    parameter \\A_SIGNED 0
    parameter \\A_WIDTH 1
    parameter \\Y_WIDTH 1
    connect \\A \\en
    connect \\Y \\ne
  end
  cell $dffe \\f
    parameter \\WIDTH 1
    parameter \\CLK_POLARITY 1
    parameter \\EN_POLARITY 1
    connect \\CLK \\clk
    connect \\EN {enable}
    connect \\D \\d
    connect \\Q \\q
  end
end
"""


def _port(kind: str, name: str, index: int) -> str:
    return f"  wire {kind} {index} \\{name}\n"


AND_CELL = ("  cell $and \\c1\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
            "    parameter \\A_WIDTH 1\n    parameter \\B_WIDTH 1\n    parameter \\Y_WIDTH 1\n"
            "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n  end\n")


def mutant_module(body: str, with_q: bool = True) -> str:
    """The clock-mutant family: ports clk a b rst, outputs y (a & b) and q (a flip-flop)."""
    ports = [_port("input", n, i) for i, n in enumerate(("clk", "a", "b", "rst"), 1)]
    ports.append(_port("output", "y", 5))
    if with_q:
        ports.append(_port("output", "q", 6))
    return "attribute \\top 1\nmodule \\t\n" + "".join(ports) + body + "end\n"


def adff_cell(clk_pol: int = 1, rst_pol: int = 1, value: int = 0, d: str = "\\a",
              clk: str = "\\clk") -> str:
    return ("  cell $adff \\f\n    parameter \\WIDTH 1\n"
            f"    parameter \\CLK_POLARITY {clk_pol}\n    parameter \\ARST_POLARITY {rst_pol}\n"
            f"    parameter \\ARST_VALUE 1'{value}\n    connect \\CLK {clk}\n"
            f"    connect \\ARST \\rst\n    connect \\D {d}\n    connect \\Q \\q\n  end\n")


DFF_CELL = ("  cell $dff \\f\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
            "    connect \\CLK \\clk\n    connect \\D \\a\n    connect \\Q \\q\n  end\n")
SDFF_CELL = ("  cell $sdff \\f\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
             "    parameter \\SRST_POLARITY 1\n    parameter \\SRST_VALUE 0\n"
             "    connect \\CLK \\clk\n    connect \\SRST \\rst\n    connect \\D \\a\n"
             "    connect \\Q \\q\n  end\n")
RENAMED_Y = AND_CELL.replace("\\Y \\y", "\\Y \\yy").replace(
    "  cell $and", "  wire \\yy\n  cell $and")
CLOCK_GOLD = mutant_module(AND_CELL + adff_cell())
# The review's mutants (C1): every one must fail the check; the posedge-to-negedge and the clock-net
# mutants passed all three lowerings of the async2sync flow.
CLOCK_MUTANTS = {
    "negedge": mutant_module(AND_CELL + adff_cell(clk_pol=0)),
    "clock_net": mutant_module(AND_CELL + adff_cell(clk="\\b")),
    "wrong_function": mutant_module(AND_CELL.replace("$and", "$or") + adff_cell()),
    "arst_value": mutant_module(AND_CELL + adff_cell(value=1)),
    "arst_polarity": mutant_module(AND_CELL + adff_cell(rst_pol=0)),
    "no_reset": mutant_module(AND_CELL + DFF_CELL),
    "sync_reset_for_async": mutant_module(AND_CELL + SDFF_CELL),
    "d_source": mutant_module(AND_CELL + adff_cell(d="\\b")),
    "q_tied_0": mutant_module(AND_CELL + "  connect \\q 1'0\n"),
    "q_port_missing": mutant_module(AND_CELL, with_q=False),
    "y_tied_0": mutant_module("  connect \\y 1'0\n" + adff_cell()),
    "y_renamed": mutant_module(RENAMED_Y + adff_cell()),
}


def hard_block_module(feed: str, cut: bool = False) -> str:
    """A top whose output ``y`` is a hard block's (a black box module's) output; ``feed`` is the
    net on its input; ``cut`` ties ``y`` to 0 and leaves the block's output unread."""
    out, extra = ("\\z", "  wire \\z\n  connect \\y 1'0\n") if cut else ("\\y", "")
    return f"""attribute \\top 1
module \\t
  wire input 1 \\a
  wire input 2 \\b
  wire output 3 \\y
  wire \\w
{extra}  cell $and \\g
    parameter \\A_SIGNED 0
    parameter \\B_SIGNED 0
    parameter \\A_WIDTH 1
    parameter \\B_WIDTH 1
    parameter \\Y_WIDTH 1
    connect \\A \\a
    connect \\B 1'1
    connect \\Y \\w
  end
  cell \\hb \\u
    connect \\i {feed}
    connect \\o {out}
  end
end
attribute \\blackbox 1
module \\hb
  wire input 1 \\i
  wire output 2 \\o
end
"""


@unittest.skipUnless(YOSYS and Path(YOSYS).exists(), "Yosys 0.55 (ODIN3_YOSYS055) not found")
class ProbeTest(unittest.TestCase):
    """Spec 20.2's probe: s ? a : x -> a passes; the reverse and a wrong enable fail."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name)
        self.checker = oe.Checker(YOSYS, self.work, 2 << 30)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def refines(self, gold: str, gate: str) -> bool:
        (self.work / "g.il").write_text(gold)
        (self.work / "h.il").write_text(gate)
        return self.checker.refines(self.work / "g.il", self.work / "h.il", "p")[0]

    def accepts(self, gold: str, gate: str) -> bool:
        """True when the check proves the pair; a hard Yosys death (CrashError) is not a proof."""
        try:
            return self.refines(gold, gate)
        except oe.ToolError:  # a Yosys abort (crash or ERROR) is no proof either
            return False

    def test_x_in_the_gold_is_a_dont_care(self) -> None:
        self.assertTrue(self.refines(MUX_X, PLAIN_A))

    def test_defined_to_x_is_not_a_refinement(self) -> None:
        self.assertFalse(self.refines(PLAIN_A, MUX_X))

    def test_a_wrong_enable_fails(self) -> None:
        self.assertTrue(self.refines(dffe("\\en"), dffe("\\en")))
        self.assertFalse(self.refines(dffe("\\en"), dffe("\\ne")))

    def test_the_identical_clocked_module_refines(self) -> None:
        self.assertTrue(self.refines(CLOCK_GOLD, CLOCK_GOLD))

    def test_clock_mutants_all_fail(self) -> None:
        """Review C1: a flipped clock edge and a wrong clock net are no longer accepted."""
        for name, text in CLOCK_MUTANTS.items():
            with self.subTest(mutant=name):
                self.assertFalse(self.accepts(CLOCK_GOLD, text))

    def test_a_lowering_that_aborts_does_not_decide_the_next_one_does(self) -> None:
        """An `$sdff` against an `$adff` makes Yosys abort with an internal-cell ERROR in the
        plain lowering: that lowering gives no verdict, the techmap one finds the failed proof
        (False). Only when every lowering aborts is it a RefineAbort (the junk file below)."""
        self.assertFalse(self.accepts(CLOCK_GOLD, CLOCK_MUTANTS["sync_reset_for_async"]))

    def test_a_real_proof_failure_is_false_and_a_real_error_abort_is_a_tool_error(self) -> None:
        ok, why = self.checker.refines(*self.pair_files(PLAIN_A, MUX_X), "q")
        self.assertFalse(ok)
        self.assertIn("unproven $equiv", why)
        (self.work / "junk.il").write_text("this is not rtlil\n")
        for bad in ("junk.il",):
            with self.assertRaises(oe.ToolError) as ctx:
                self.checker.refines(self.work / bad, self.work / bad, "e")
            self.assertNotIsInstance(ctx.exception, oe.CrashError)

    def pair_files(self, gold: str, gate: str) -> tuple[Path, Path]:
        (self.work / "g2.il").write_text(gold)
        (self.work / "h2.il").write_text(gate)
        return self.work / "g2.il", self.work / "h2.il"

    def test_a_top_rewrite_over_a_sequential_submodule_refines(self) -> None:
        """Review I4: the 0001-opt_expr call of micro/bm_if_collapse.v (an `$eq` of the top to a
        `$not`, with a submodule of x-init flops). equiv_make matches only the two tops, so the
        submodule outputs were `unproven`; flatten before equiv_make proves the pair."""
        self.assertTrue(self.refines(
            (helpers.FIXTURES / "opt_equiv_hier_before.il").read_text(encoding="utf-8"),
            (helpers.FIXTURES / "opt_equiv_hier_after.il").read_text(encoding="utf-8")))

    def test_the_unflattened_script_cannot_prove_it(self) -> None:
        """The control: without `flatten` the same pair is `unproven` (the I4 false negative)."""
        for side in ("before", "after"):
            text = (helpers.FIXTURES / f"opt_equiv_hier_{side}.il").read_text(encoding="utf-8")
            (self.work / f"{side}.il").write_text(
                oe.rename_modules(text, "gold" if side == "before" else "gate", "\\bm_if_collapse"))
        script = oe.refinement_script(str(self.work / "before.il"), str(self.work / "after.il"),
                                      True, oe.ONE_CLOCK[0])
        script = script.replace(f"proc; flatten; {oe.EVERT_BLACKBOXES};", "proc;", 1)
        done = subprocess.run([YOSYS, "-q", "-p", script], capture_output=True, text=True,
                              check=False)
        self.assertEqual(done.returncode, 1, done.stderr[-300:])
        self.assertIn("unproven", done.stdout + done.stderr)

    def test_same_clock_pairs_take_the_one_clock_family(self) -> None:
        self.assertTrue(clock_sig.same_clocks(CLOCK_GOLD, CLOCK_GOLD))
        for name in ("negedge", "clock_net"):
            with self.subTest(mutant=name):
                self.assertFalse(clock_sig.same_clocks(CLOCK_GOLD, CLOCK_MUTANTS[name]))
        self.assertTrue(clock_sig.same_clocks(CLOCK_GOLD, CLOCK_MUTANTS["arst_value"]))

    def test_a_hard_block_is_cut_so_its_outputs_are_shared(self) -> None:
        """ch_intrinsics.v and mkPktMerge.v (2F VTR triage): the two sides' copies of a black box
        module are different modules, and equiv_make could never equate their output wires, so an
        untouched hard block made every pair "not a refinement". It is cut at its ports now."""
        self.assertTrue(self.refines(hard_block_module("\\a"), hard_block_module("\\a")))
        # a rewrite of the logic that feeds the block (a & 1 -> a) is a refinement
        self.assertTrue(self.refines(hard_block_module("\\w"), hard_block_module("\\a")))

    def test_a_change_at_a_hard_block_input_is_not_a_refinement(self) -> None:
        """The cut compares what drives the block: a different net on its input, or its output
        replaced by a constant, still fails."""
        self.assertFalse(self.refines(hard_block_module("\\a"), hard_block_module("\\b")))
        self.assertFalse(self.refines(hard_block_module("\\a"), hard_block_module("\\a", cut=True)))


# The 3B soundness review's adversarial cases, as Odin III writes them (`write_rtlil` of a design
# with the tech library: `single_port_ram` declared `dynports` with 1-bit `data`/`out`).
NOT_CELL = ("  cell $not $n{k}\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
            "    parameter \\Y_WIDTH 1\n    connect \\A {a}\n    connect \\Y {y}\n  end\n")
AND4_CELL = ("  cell $and $g\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
             "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 4\n"
             "    connect \\A \\d\n    connect \\B 4'1111\n    connect \\Y \\dd\n  end\n")
RAM_DECL = ("attribute \\blackbox 1\nattribute \\dynports 1\nmodule \\single_port_ram\n"
            "  parameter \\ADDR_WIDTH 15\n  parameter \\DATA_WIDTH 1\n  wire input 1 \\clk\n"
            "  wire input 2 \\data\n  wire width 15 input 3 \\addr\n  wire input 4 \\we\n"
            "  wire output 5 \\out\nend\n")


def ram(extra: str = "", data: str = "\\d", we: str = "\\we", out: str = "\\q",
        width: int = 4) -> str:
    """A top whose 4-bit output ``q`` is a ``single_port_ram``'s read data."""
    return ("attribute \\top 1\nmodule \\m\n  wire input 1 \\clk\n  wire input 2 \\we\n"
            "  wire width 4 input 3 \\d\n  wire width 4 input 4 \\a\n"
            f"  wire width 4 output 5 \\q\n{extra}  cell \\single_port_ram \\i\n"
            f"    parameter \\ADDR_WIDTH 4\n    parameter \\DATA_WIDTH {width}\n"
            f"    connect \\clk \\clk\n    connect \\data {data}\n    connect \\addr \\a\n"
            f"    connect \\we {we}\n    connect \\out {out}\n  end\nend\n" + RAM_DECL)


def counter(init: str) -> str:
    """A 4-bit counter ``q`` (output ``y``) with ``attribute \\init 4'<init>``."""
    return ("attribute \\top 1\nmodule \\m\n  wire input 1 \\clk\n  wire width 4 output 2 \\y\n"
            f"  attribute \\init 4'{init}\n  wire width 4 \\q\n  wire width 4 \\n\n"
            "  cell $add $a\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
            "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 4\n"
            "    connect \\A \\q\n    connect \\B 4'0001\n    connect \\Y \\n\n  end\n"
            "  cell $dff $f\n    parameter \\WIDTH 4\n    parameter \\CLK_POLARITY 1\n"
            "    connect \\CLK \\clk\n    connect \\D \\n\n    connect \\Q \\q\n  end\n"
            "  connect \\y \\q\nend\n")


RAM = ram()
RAM_MUTANTS = {
    "data_bit_2": ram("  wire \\n2\n" + NOT_CELL.format(k=1, a="\\d [2]", y="\\n2"),
                      data="{ \\d [3] \\n2 \\d [1:0] }"),
    "out_bit_3": ram("  wire width 4 \\qq\n  wire \\n3\n"
                     + NOT_CELL.format(k=1, a="\\qq [3]", y="\\n3")
                     + "  connect \\q { \\n3 \\qq [2:0] }\n", out="\\qq"),
    "data_width_5": ram("  wire \\qx\n", data="{ 1'0 \\d }", out="{ \\qx \\q }", width=5),
    "we_inverted": ram("  wire \\nwe\n" + NOT_CELL.format(k=1, a="\\we", y="\\nwe"),
                       we="\\nwe"),
    "addr_width_param": RAM.replace("parameter \\ADDR_WIDTH 4", "parameter \\ADDR_WIDTH 5"),
    # review M1: a parameter on one side only (the block falls back to its declaration default)
    "data_width_param_dropped": RAM.replace("    parameter \\DATA_WIDTH 4\n", ""),
}


def hold(d_net: str) -> str:
    """The re-check's N2 case: a flip-flop ``r`` with no init, ``r <= <d_net>`` (``\\n`` is
    ``r & d``, ``\\r`` holds), output ``y = r``."""
    return ("attribute \\top 1\nmodule \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
            "  wire output 3 \\y\n  wire \\r\n  wire \\n\n"
            "  cell $and $a\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
            "    parameter \\A_WIDTH 1\n    parameter \\B_WIDTH 1\n    parameter \\Y_WIDTH 1\n"
            "    connect \\A \\r\n    connect \\B \\d\n    connect \\Y \\n\n  end\n"
            "  cell $dff $f\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
            f"    connect \\CLK \\clk\n    connect \\D {d_net}\n    connect \\Q \\r\n  end\n"
            "  connect \\y \\r\nend\n")


class HardBlockTextTest(unittest.TestCase):
    """``hard_blocks``: the declarations at instance width and the mismatch rule (no Yosys)."""

    USE = hb.BoxUse("\\single_port_ram", (("\\addr", 4), ("\\data", 4), ("\\out", 4)),
                    (("\\DATA_WIDTH", "4"),))

    def test_a_declaration_takes_the_instance_widths(self) -> None:
        text = hb.box_declaration(RAM_DECL, "\\r$o3w0", {"\\data": 8, "\\out": 8, "\\we": 1})
        self.assertIn("module \\r$o3w0\n", text)
        self.assertIn("  wire width 8 input 1 \\data\n", text)
        self.assertIn("  wire input 2 \\we\n", text)
        self.assertIn("  wire width 8 output 3 \\out\n", text)
        self.assertNotIn("dynports", text)
        self.assertNotIn("\\addr", text)
        with self.assertRaises(hb.BoxError):
            hb.box_declaration(RAM_DECL, "\\r", {"\\data1": 8})

    def test_a_shared_block_must_agree_on_type_widths_and_parameters(self) -> None:
        same = {"\\i": self.USE}
        self.assertEqual(hb.box_mismatch(same, same), "")
        wider = hb.BoxUse(self.USE.kind, (("\\addr", 4), ("\\data", 5), ("\\out", 5)),
                          self.USE.params)
        self.assertIn("hard block \\i differs", hb.box_mismatch(same, {"\\i": wider}))
        param = hb.BoxUse(self.USE.kind, self.USE.widths, (("\\DATA_WIDTH", "5"),))
        self.assertIn("DATA_WIDTH=5", hb.box_mismatch(same, {"\\i": param}))
        dropped = hb.BoxUse(self.USE.kind, self.USE.widths, ())  # review M1, both directions
        self.assertIn("hard block \\i differs", hb.box_mismatch(same, {"\\i": dropped}))
        self.assertIn("hard block \\i differs", hb.box_mismatch({"\\i": dropped}, same))
        other = hb.BoxUse("\\dual_port_ram", self.USE.widths, self.USE.params)
        self.assertIn("dual_port_ram", hb.box_mismatch(same, {"\\i": other}))

    def test_json_values_and_the_top_mark(self) -> None:
        self.assertEqual(hb.json_value("00000000000000000000000000000100"), "4")
        self.assertEqual(hb.json_value("text "), "text")
        marked = hb.mark_top("attribute \\top 1\nmodule \\a\nend\nmodule \\m\nend\n", "\\m")
        self.assertEqual(marked, "module \\a\nend\nattribute \\top 1\nmodule \\m\nend\n")
        self.assertEqual(list(hb.blackbox_modules(RAM)), ["\\single_port_ram"])

    def test_the_bounded_script_follows_the_proof_model(self) -> None:
        base = oe.bounded_script("g.il", "h.il", False, oe.BASE_STEPS, (True, True, False))
        self.assertIn("async2sync; opt_clean; expose -shared gold gate; miter -equiv", base)
        self.assertIn("sat -seq 4 -set-init-undef -enable_undef", base)
        out = oe.bounded_script("g.il", "h.il", True, oe.BMC_STEPS, (False, False, True))
        self.assertIn("proc; flatten; " + oe.EVERT_BLACKBOXES + "; memory; clk2fflogic; "
                      "opt_clean -purge; miter", out)
        self.assertNotIn("expose -shared", out)


@unittest.skipUnless(YOSYS and Path(YOSYS).exists(), "Yosys 0.55 (ODIN3_YOSYS055) not found")
class SoundnessTest(unittest.TestCase):
    """The 3B soundness review's cases through ``Checker.judge`` (one opt-equiv pair each): a
    wrong RAM data or read bit, a ``DATA_WIDTH`` change, an inverted ``we`` and an init value
    changed must be "not a refinement"; correct pairs stay ok."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name)
        self.count = 0

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def judge(self, gold: str, gate: str) -> tuple[bool, str, bool]:
        self.count += 1
        paths = (self.work / f"g{self.count}.il", self.work / f"h{self.count}.il")
        for path, text in zip(paths, (gold, gate), strict=True):
            path.write_text(text, encoding="utf-8")
        checker = oe.Checker(YOSYS, self.work, 2 << 30)
        return checker.judge(paths[0], paths[1], f"p{self.count}")

    def assert_not_a_refinement(self, gold: str, gate: str) -> str:
        ok, text, unsure = self.judge(gold, gate)
        self.assertFalse(ok, text)
        self.assertFalse(unsure, f"inconclusive, want not a refinement: {text}")
        return text

    def test_correct_pairs_still_refine(self) -> None:
        rewrite = ram("  wire width 4 \\dd\n" + AND4_CELL, data="\\dd")  # d & 4'1111 -> d
        for name, gold, gate in (("ram", RAM, RAM), ("ram_rewrite", rewrite, RAM),
                                 ("init", counter("0000"), counter("0000"))):
            with self.subTest(pair=name):
                self.assertEqual(self.judge(gold, gate), (True, "", False))

    def test_every_hard_block_mutant_is_not_a_refinement(self) -> None:
        """C1 (data bit 2, read bit 3: the 1-bit declaration compared only bit 0), M1
        (``DATA_WIDTH`` 4 -> 5), the ``we`` bug, and a parameter alone (re-check N3: RTLIL keeps
        the parameters, so they are compared)."""
        for name, gate in RAM_MUTANTS.items():
            with self.subTest(mutant=name):
                text = self.assert_not_a_refinement(RAM, gate)
                if name in ("data_width_5", "addr_width_param", "data_width_param_dropped"):
                    self.assertIn("hard block \\i differs", text)

    def test_an_init_value_changed_is_not_a_refinement(self) -> None:
        """H2: `reg q = 0` -> `= 1` passes the induction; the base case finds the trace."""
        text = self.assert_not_a_refinement(counter("0000"), counter("0001"))
        self.assertIn("the initial states differ", text)

    def test_a_flop_without_init_starts_unconstrained_not_at_0(self) -> None:
        """Re-check N2: gold `r <= r & d` against gate `r <= r`, no init, differ only when r
        starts at 1; a check that starts every such flop at 0 calls them equal. The base case
        starts them at x (``sat -set-init-undef``), so every start value counts."""
        self.assert_not_a_refinement(hold("\\n"), hold("\\r"))
        self.assertEqual(self.judge(hold("\\n"), hold("\\n")), (True, "", False))

    def test_a_never_changing_flop_without_init_may_become_x(self) -> None:
        """opt_dff's d_eq_q: a flip-flop with D = Q and no init is x for ever, so the gate may
        drive its output with an x; a base case that starts it at 0 would call that a
        difference (tests/opt/rules/dff/d_eq_q.v)."""
        gate = ("attribute \\top 1\nmodule \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                "  wire output 3 \\y\n  connect \\y 1'x\nend\n")
        self.assertEqual(self.judge(hold("\\r"), gate), (True, "", False))


def deep_delay(init: str, stages: int = 12) -> str:
    """Review H1: a holding flop ``r`` (D = Q, ``init`` as written, '' for none) followed by
    ``stages`` flops to the output ``y``: a difference of ``r`` shows 13 clock edges later,
    beyond the 20-step output check."""
    nets = "".join(f"  wire \\c{k}\n" for k in range(stages))
    flop = ("  cell $dff ${name}\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
            "    connect \\CLK \\clk\n    connect \\D {d}\n    connect \\Q {q}\n  end\n")
    chain = ["\\r", *(f"\\c{k}" for k in range(stages))]
    cells = flop.format(name="hold", d="\\r", q="\\r") + "".join(
        flop.format(name=f"s{k}", d=chain[k], q=chain[k + 1]) for k in range(stages))
    return ("attribute \\top 1\nmodule \\m\n  wire input 1 \\clk\n  wire output 2 \\y\n"
            f"{init}  wire \\r\n{nets}{cells}  connect \\y {chain[-1]}\nend\n")


@unittest.skipUnless(YOSYS and Path(YOSYS).exists(), "Yosys 0.55 (ODIN3_YOSYS055) not found")
class DeepDelayTest(unittest.TestCase):
    """Review H1 (``init_dropped_deep12``) on real Yosys: gold flop init 0, gate init dropped, 12
    flops to the output. The base case fails, the output check finds nothing in 20 steps, and
    the BLIF cec would tie the missing init to 0 and prove it. It must never be ok."""

    GOLD = deep_delay("  attribute \\init 1'0\n")
    GATE = deep_delay("")

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_the_same_pair_refines(self) -> None:
        paths = (self.work / "gold.il", self.work / "same.il")
        for path in paths:
            path.write_text(self.GOLD, encoding="utf-8")
        checker = oe.Checker(YOSYS, self.work, 2 << 30)
        self.assertEqual(checker.judge(paths[0], paths[1], "same"), (True, "", False))

    def test_a_dropped_init_behind_a_deep_delay_is_never_ok(self) -> None:
        """The base case fails, the output check finds nothing in 20 steps: inconclusive."""
        paths = (self.work / "gold.il", self.work / "gate.il")
        for path, text in zip(paths, (self.GOLD, self.GATE), strict=True):
            path.write_text(text, encoding="utf-8")
        ok, text, unsure = oe.Checker(YOSYS, self.work, 2 << 30).judge(paths[0], paths[1], "deep")
        self.assertFalse(ok, text)
        self.assertTrue(unsure, text)
        self.assertIn("no output trace", text)

    def test_the_real_cec_is_refused_for_the_pair(self) -> None:
        """The lowering folds the gate's holding flop (no init) into an x net, which the cec ties
        to 0 = the gold's value, and proves it. ``cec_ils(need_init)`` stops before the checker;
        without ``need_init`` the cec does prove the pair (the hole)."""
        paths = (self.work / "gold.il", self.work / "gate.il")
        for path, text in zip(paths, (self.GOLD, self.GATE), strict=True):
            path.write_text(text, encoding="utf-8")
        verdict, _ = oe.cec_ils(YOSYS, paths[0], paths[1], 2 << 30, self.work, "open")
        self.assertEqual(verdict, oe.CEC_PROVED)
        verdict, why = oe.cec_ils(YOSYS, paths[0], paths[1], 2 << 30, self.work, "e2e", True)
        self.assertEqual(verdict, oe.CEC_UNPROVEN, why)
        self.assertIn("not used", why)


if __name__ == "__main__":
    unittest.main()
