"""Tests for tools/opt-check/opt-diff (2F Task 17, spec 20.3): Odin III's opt against Yosys's.

Fakes stand in for ``odin3``, ``yosys`` and ``netlist-compare``. Each logs its argv; ``fake.json``
beside them says what they print and return. The fake ``yosys`` tells its three scripts apart: one
with ``equiv_make`` is the refinement check, one with ``write_blif`` a lowering, any other the
step-2 run. ``memcap.run`` is a plain subprocess call.
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

import memcap
import opt_diff as od
import opt_equiv

from . import helpers

FAKE_ODIN = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "odin.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
for command in script.split("; "):
    if command.startswith("write_rtlil"):
        Path(command.split()[1]).write_text("module \\t\nend\n")
sys.stderr.write(spec["odin_log"])
if spec.get("odin_signal"):
    os.kill(os.getpid(), spec["odin_signal"])
sys.exit(spec.get("odin_rc", 0))
"""

FAKE_YOSYS = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "yosys.log", "a") as log:
    log.write(script + "\n")
if "equiv_make" in script:
    if spec.get("refine_signal"):
        os.kill(os.getpid(), spec["refine_signal"])
    msg = "ERROR: Found 1 unproven $equiv cells in 'equiv_status -assert'.\n"
    sys.stdout.write(spec.get("refine_out", msg) if spec.get("refine_rc", 0) else "")
    sys.exit(spec.get("refine_rc", 0))
if spec.get("yosys_signal") and "write_blif" not in script:
    os.kill(os.getpid(), spec["yosys_signal"])
if spec.get("yosys_rc") and "write_blif" not in script:
    sys.stderr.write("ERROR: cannot read in.il\n")
    sys.exit(spec["yosys_rc"])
if "write_blif" in script:
    if spec.get("lower_signal"):
        os.kill(os.getpid(), spec["lower_signal"])
    out = script.split("-blackbox ")[1].strip()
    Path(out).write_text(".model t\n.end\n")
    sys.exit(0)
for command in script.split("; "):
    if command.startswith("write_rtlil"):
        Path(command.split()[1]).write_text("module \\t\nend\n")
(Path(os.environ["YOSYS_COVER_DIR"]) / "cover.txt").write_text(spec["cover"])
sys.stdout.write(spec["ystat"])
"""

FAKE_COMPARE = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
with open(HERE / "compare.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
if spec.get("compare_signal"):
    os.kill(os.getpid(), spec["compare_signal"])
sys.exit(spec.get("compare_rc", 0))
"""

ODIN_LOG = ("odin3: info: opt: rule const_and 2\n"
            "odin3: info: stats: module t: cell $xor 2\n")
YSTAT = ("\n=== t ===\n\n   Number of cells:                  2\n"
         "     $xor                            2\n")
COVER = "passes/opt/opt_expr.cc:570:f   3 opt.opt_expr.const_and\nx.cc:1:g   0 opt.opt_expr.zero\n"


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


class Tree:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir()
        for name, text in (("odin3", FAKE_ODIN), ("yosys", FAKE_YOSYS),
                           ("netlist-compare", FAKE_COMPARE)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        (root / "micro").mkdir()
        (root / "micro" / "d.v").write_text("module t; endmodule\n")
        self.set_spec()

    def set_spec(self, **changes: Any) -> None:
        spec = {"odin_log": ODIN_LOG, "ystat": YSTAT, "cover": COVER}
        spec.update(changes)
        (self.bin / "fake.json").write_text(json.dumps(spec))

    def expect(self, text: str) -> list[str]:
        path = self.root / "expect.txt"
        path.write_text(text)
        return ["--expect", str(path), "--root", str(self.root / "micro")]

    def run(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.bin / "odin3"), "--yosys", str(self.bin / "yosys"),
                "--netlist-compare", str(self.bin / "netlist-compare"),
                "--work", str(self.root / "work"), *extra, str(self.root / "micro" / "d.v")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(od.main, argv)

    def log(self, name: str) -> list[str]:
        return (self.bin / name).read_text().splitlines()


class ScriptTest(unittest.TestCase):
    """Step 1 and step 2 command strings for each S."""

    IN, ODIN, YOSYS = Path("w/in.il"), Path("w/odin.il"), Path("w/yosys.il")

    def odin(self, name: str, top: str | None = None) -> str:
        return od.odin_script(od.script_named(name), ["a.v", "b.v"], top, self.IN, self.ODIN)

    def yosys(self, name: str) -> str:
        return od.yosys_script(od.script_named(name), self.IN, self.YOSYS)

    def test_phase2_writes_in_il_before_proc(self) -> None:
        self.assertEqual(
            self.odin("phase2", "t"),
            "read_verilog --nolatches a.v b.v; hierarchy --top t; write_rtlil w/in.il; "
            "opt_expr --opt-stats; opt_clean --opt-stats; opt -nodffe -nosdff --opt-stats; proc; "
            "opt --opt-stats; compact; write_rtlil w/odin.il; stats")
        self.assertEqual(
            self.yosys("phase2"),
            "read_rtlil w/in.il; opt_expr; opt_clean; opt -nodffe -nosdff; proc -norom; opt; "
            "write_rtlil w/yosys.il; stat")

    def test_every_opt_script(self) -> None:
        for name, slug in (("opt -full", "full"), ("opt -fast", "fast"),
                           ("opt -fast -noff", "fast-noff"),
                           ("opt -nodffe -nosdff", "nodffe-nosdff")):
            self.assertEqual(od.script_named(name).slug, slug)
            self.assertEqual(
                self.odin(name),
                f"read_verilog --nolatches a.v b.v; hierarchy -auto; proc; opt_expr -keepdc; "
                f"write_rtlil w/in.il; {name} --opt-stats; write_rtlil w/odin.il; stats")
            self.assertEqual(self.yosys(name),
                             f"read_rtlil w/in.il; {name}; write_rtlil w/yosys.il; stat")

    def test_wreduce_and_phase3_scripts(self) -> None:
        self.assertEqual(od.script_named("wreduce").slug, "wreduce")
        self.assertEqual(
            self.odin("wreduce"),
            "read_verilog --nolatches a.v b.v; hierarchy -auto; proc; opt_expr -keepdc; opt; "
            "write_rtlil w/in.il; wreduce --opt-stats; write_rtlil w/odin.il; stats")
        self.assertEqual(self.yosys("wreduce"),
                         "read_rtlil w/in.il; wreduce; write_rtlil w/yosys.il; stat")
        # Phase 2's lines, then wreduce; opt_clean; opt -full (3G WR-9); in.il before proc
        self.assertEqual(
            self.odin("phase3", "t"),
            "read_verilog --nolatches a.v b.v; hierarchy --top t; write_rtlil w/in.il; "
            "opt_expr --opt-stats; opt_clean --opt-stats; opt -nodffe -nosdff --opt-stats; proc; "
            "opt --opt-stats; wreduce --opt-stats; opt_clean --opt-stats; opt -full --opt-stats; "
            "compact; write_rtlil w/odin.il; stats")
        self.assertIn("wreduce; opt_clean; opt -full; write_rtlil", self.yosys("phase3"))
        self.assertIn("widths", od.STEPS)
        table = od.parse_expect("a.v wreduce widths structural only: a reason\n"
                                "a.v phase3 identity structural only: a reason\n")
        self.assertEqual(len(table), 2)

    def test_unknown_script(self) -> None:
        with self.assertRaises(opt_equiv.ToolError):
            od.script_named("opt -mux_undef")

    def test_lowering_script(self) -> None:
        self.assertEqual(
            od.lowering_script(Path("o.il"), Path("o.blif")),
            "read_rtlil o.il; hierarchy -auto-top; flatten; memory; techmap; opt -fast; dffunmap; "
            "opt -fast -noff; opt_clean -purge; "
            "write_blif -true + vcc -false + gnd -undef + unconn -blackbox o.blif")

    def test_expect_parser(self) -> None:
        table = od.parse_expect("# c\nsub/d.v phase2 histogram why not  # note\n"
                                "e.v * refine another\n")
        self.assertEqual(table[("sub/d.v", "phase2", "histogram")].reason, "why not")
        self.assertIn(("e.v", "*", "refine"), table)
        for bad in ("d.v phase2 histogram\n", "d.v nosuch refine why\n", "d.v full nostep why\n"):
            with self.assertRaises(opt_equiv.ToolError):
                od.parse_expect(bad)

    def test_rel_key(self) -> None:
        self.assertEqual(od.rel_key(["/a/b/c/d.v"], Path("/a/b")), "c/d.v")
        self.assertEqual(od.rel_key(["/a/b/c/d.v"], Path("/x")), "d.v")
        self.assertEqual(od.rel_key(["/a/b/c/d.v"], None), "d.v")


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tree = Tree(Path(self._tmp.name))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_all_steps_agree(self) -> None:
        code, out, err = self.tree.run("--script", "phase2", "--script", "opt -full")
        self.assertEqual(code, 0, out + err)
        self.assertIn("opt-diff: d.v [phase2]: ok", out)
        self.assertIn("opt-diff: d.v [full]: ok", out)
        self.assertNotIn("counters", out)
        odin = self.tree.log("odin.log")
        self.assertEqual(len(odin), 2)
        self.assertIn("-p read_verilog --nolatches ", odin[0])
        self.assertEqual(len(self.tree.log("compare.log")), 2)
        self.assertIn("--implicit-models", self.tree.log("compare.log")[0])

    def test_techlib_is_read_first(self) -> None:
        code, _, _ = self.tree.run("--techlib", "lib/x.o3lib")
        self.assertEqual(code, 0)
        self.assertIn("-p read_techlib ", self.tree.log("odin.log")[0])
        self.assertIn("x.o3lib; read_verilog --nolatches ", self.tree.log("odin.log")[0])

    def test_no_refine_leaves_the_proof_out(self) -> None:
        code, out, _ = self.tree.run("--no-refine")
        self.assertEqual(code, 0, out)
        self.assertFalse(any("equiv_make" in line for line in self.tree.log("yosys.log")))

    def test_default_script_is_phase2(self) -> None:
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0)
        self.assertIn("[phase2]: ok", out)
        self.assertNotIn("[full]", out)

    def test_verbose_prints_counters(self) -> None:
        _, out, _ = self.tree.run("--verbose")
        self.assertIn("counters odin3: const_and 2; yosys: opt.opt_expr.const_and 3", out)

    def test_histogram_difference_fails(self) -> None:
        self.tree.set_spec(ystat=YSTAT.replace("$xor                            2",
                                               "$xor                            3"))
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("[phase2]: histogram: DIFFERS: module 't'", out)
        self.assertIn("counters odin3: const_and 2; yosys: opt.opt_expr.const_and 3", out)

    def test_refinement_failure(self) -> None:
        self.tree.set_spec(refine_rc=1)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("[phase2]: refine: DIFFERS", out)

    def test_a_yosys_error_abort_in_the_refinement_is_exit_2_not_absorbed(self) -> None:
        """Review I3: `refine anything goes` must not absorb a parser ERROR."""
        self.tree.set_spec(refine_rc=1, refine_out="ERROR: Can't match gate port y\n")
        extra = self.tree.expect("d.v phase2 refine anything goes\n")
        code, out, err = self.tree.run(*extra)
        self.assertEqual(code, 2, out + err)
        self.assertNotIn("differs as listed", out)

    def test_a_sat_timeout_falls_back_to_the_blif_cec(self) -> None:
        """A timed-out proof goes to equiv-check on the lowered BLIFs: proved is a pass, a cec
        that decides nothing is the difference `unproven`, --no-cec keeps the hard timeout."""
        def hang(argv: Sequence[str], cap: int, **kw: Any) -> subprocess.CompletedProcess[str]:
            if "equiv_make" in " ".join(argv):
                raise subprocess.TimeoutExpired(list(argv), 60.0)
            return fake_run(argv, cap, **kw)
        argv = ["--odin3", str(self.tree.bin / "odin3"), "--yosys", str(self.tree.bin / "yosys"),
                "--netlist-compare", str(self.tree.bin / "netlist-compare"),
                "--work", str(self.tree.root / "work"), str(self.tree.root / "micro" / "d.v")]
        for verdict, code, needle in ((opt_equiv.CEC_PROVED, 0, "[phase2]: ok"),
                                      (opt_equiv.CEC_UNPROVEN, 1, "refine: DIFFERS: unproven"),
                                      (opt_equiv.CEC_DIFFERS, 1, "refine: DIFFERS: text")):
            with self.subTest(verdict=verdict), mock.patch.object(
                    memcap, "run", hang), mock.patch.object(
                    opt_equiv, "cec_blifs", return_value=(verdict, "text")):
                got, out, err = helpers.run_main(od.main, argv)
                self.assertEqual(got, code, out + err)
                self.assertIn(needle, out)
        with mock.patch.object(memcap, "run", hang):
            got, _, _ = helpers.run_main(od.main, [*argv[:-1], "--no-cec", argv[-1]])
            self.assertEqual(got, 3)

    def test_identity_failure_and_error(self) -> None:
        self.tree.set_spec(compare_rc=1)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("identity: DIFFERS: netlist-compare: not identical", out)
        self.tree.set_spec(compare_rc=2)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 2)
        self.assertNotIn("DIFFERS", out)

    def test_tool_failures_are_errors(self) -> None:
        self.tree.set_spec(odin_rc=1, odin_log="odin3: error: bad thing\n")
        code, _, err = self.tree.run()
        self.assertEqual(code, 2)
        self.assertIn("odin3 exit 1: odin3: error: bad thing", err)

    def test_an_odin3_crash_is_hard_exit_3(self) -> None:
        """Review I1: a signal or cap kill was a ToolError, exit 2 (not hard)."""
        for signal in (11, 9):
            with self.subTest(signal=signal):
                self.tree.set_spec(odin_signal=signal)
                code, out, err = self.tree.run()
                self.assertEqual(code, 3, out + err)
                self.assertIn("odin3", err)

    def test_a_timeout_is_hard_exit_3_with_no_traceback(self) -> None:
        argv = ["--odin3", str(self.tree.bin / "odin3"), "--yosys", str(self.tree.bin / "yosys"),
                "--work", str(self.tree.root / "work"), str(self.tree.root / "micro" / "d.v")]
        with mock.patch.object(memcap, "run",
                               side_effect=subprocess.TimeoutExpired(["odin3"], 1.0)):
            code, _, err = helpers.run_main(od.main, argv)
        self.assertEqual(code, 3, err)
        self.assertIn("timed out", err)
        self.assertNotIn("Traceback", err)

    def test_a_yosys_crash_is_hard_and_a_yosys_error_is_not(self) -> None:
        self.tree.set_spec(yosys_signal=11)
        self.assertEqual(self.tree.run()[0], 3)
        self.tree.set_spec(yosys_rc=1)
        self.assertEqual(self.tree.run()[0], 2)

    def test_a_yosys_cap_kill_is_unproven_exit_4_not_a_crash(self) -> None:
        """2H triage (LU64PEEng, arm_core): Yosys killed at the cap, in its step-2 run or in a BLIF
        lowering, is a limit of the oracle: exit 4, so a sweep may list it. A signal other than
        the cap kill, an Odin III cap kill, a netlist-compare one and the refinement child with
        --no-cec stay hard (exit 3)."""
        for spec in ({"yosys_signal": 9}, {"lower_signal": 9}):
            with self.subTest(spec=spec):
                self.tree.set_spec(**spec)
                code, out, err = self.tree.run()
                self.assertEqual(code, 4, out + err)
                self.assertIn("opt-diff: unproven: yosys", err)
                self.assertNotIn("HARD", err)
        for spec in ({"yosys_signal": 11}, {"lower_signal": 11}, {"odin_signal": 9},
                     {"compare_signal": 9}):
            with self.subTest(spec=spec):
                self.tree.set_spec(**spec)
                code, out, err = self.tree.run()
                self.assertEqual(code, 3, out + err)
                self.assertIn("HARD", err)
        self.tree.set_spec(refine_signal=9)
        code, out, err = self.tree.run("--no-cec")
        self.assertEqual(code, 3, out + err)

    def test_a_refinement_crash_is_hard_not_a_refine_difference(self) -> None:
        """Review I3: a dying Yosys is not `refine: DIFFERS`, and no needle-less line absorbs it."""
        self.tree.set_spec(refine_signal=11)
        extra = self.tree.expect("d.v phase2 refine anything goes\n")
        code, out, err = self.tree.run(*extra)
        self.assertEqual(code, 3, out + err)
        self.assertNotIn("differs as listed", out)

    def test_a_cap_kill_of_the_refinement_is_a_size_limit_not_a_crash(self) -> None:
        """SIGKILL (the memory cap) takes the BLIF cec fallback like a timeout: unproven there is a
        difference a narrow expect line may list, never a pass."""
        self.tree.set_spec(refine_signal=9)
        with mock.patch.object(opt_equiv, "cec_blifs",
                               return_value=(opt_equiv.CEC_UNPROVEN, "BLIF cec: no verdict")):
            code, out, err = self.tree.run()
            self.assertEqual(code, 1, out + err)
            self.assertIn("refine: DIFFERS: unproven: the SAT proof hit its size limit", out)
            extra = self.tree.expect("d.v phase2 refine untested for now (size limit)\n")
            code, out, err = self.tree.run(*extra)
            self.assertEqual(code, 0, out + err)
            self.assertIn("differs as listed (untested for now (size limit))", out)

    def test_a_netlist_compare_crash_is_hard_not_an_identity_difference(self) -> None:
        self.tree.set_spec(compare_signal=11)
        extra = self.tree.expect("d.v phase2 identity anything goes\n")
        code, out, err = self.tree.run(*extra)
        self.assertEqual(code, 3, out + err)
        self.assertNotIn("identity", out)

    def test_no_module_in_either_histogram_is_an_error(self) -> None:
        """Review M4c: two empty histograms compare equal; that is no verdict."""
        self.tree.set_spec(ystat="", odin_log="odin3: info: opt: rule const_and 2\n")
        code, _, err = self.tree.run()
        self.assertEqual(code, 2)
        self.assertIn("no module", err)

    def test_the_work_dir_follows_tmpdir_and_goes_after_a_clean_run(self) -> None:
        argv = ["--odin3", str(self.tree.bin / "odin3"), "--yosys", str(self.tree.bin / "yosys"),
                "--netlist-compare", str(self.tree.bin / "netlist-compare"),
                str(self.tree.root / "micro" / "d.v")]
        with tempfile.TemporaryDirectory() as tmpdir, \
                mock.patch.dict(os.environ, {"TMPDIR": tmpdir}), \
                mock.patch.object(tempfile, "tempdir", None), \
                mock.patch.object(memcap, "run", fake_run):
            self.assertEqual(helpers.run_main(od.main, argv)[0], 0)
            self.assertEqual(list(Path(tmpdir).iterdir()), [])

    def test_expected_difference_passes(self) -> None:
        self.tree.set_spec(refine_rc=1)
        extra = self.tree.expect("d.v phase2 refine x chosen differently\n")
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 0, out)
        self.assertIn("[phase2]: refine: differs as listed (x chosen differently)", out)

    def test_wildcard_script_and_other_step_still_fails(self) -> None:
        self.tree.set_spec(refine_rc=1, compare_rc=1)
        extra = self.tree.expect("d.v * refine reason one\n")
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 1)
        self.assertIn("refine: differs as listed", out)
        self.assertIn("identity: DIFFERS", out)

    def test_stale_entry_fails(self) -> None:
        extra = self.tree.expect("d.v phase2 histogram it used to differ\n")
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 1)
        self.assertIn("histogram: stale entry in the expect list (it used to differ)", out)

    def test_entry_for_another_file_is_ignored(self) -> None:
        extra = self.tree.expect("other.v phase2 histogram reason\n")
        self.assertEqual(self.tree.run(*extra)[0], 0)


if __name__ == "__main__":
    unittest.main()
