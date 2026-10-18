"""Tests for tools/lower-check/lower-diff (3B Task 2, spec 14.2): Odin III's lower against Yosys.

Fakes stand in for ``odin3``, ``yosys``, ``equiv-check`` and, in most tests, ``netlist-compare``;
each logs its argv, and ``fake.json`` beside them says what they write and return. The last class
runs the real ``netlist-compare`` on real BLIF text, to show a swapped gate input is exit 1. The
work directories are made by ``tempfile.mkdtemp`` and are left in place (these tools never delete).
"""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import lower_common as lc
import lower_diff as ld
import memcap

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
    log.write(script + "\n")
if spec.get("odin_signal"):
    os.kill(os.getpid(), spec["odin_signal"])
if spec.get("odin_rc"):
    sys.stderr.write("odin3: error: bad thing\n")
    sys.exit(spec["odin_rc"])
for command in script.split("; "):
    if command.startswith("write_rtlil"):
        path = Path(command.split()[1])
        path.write_text(spec["rtlil"][path.name])
    elif command.startswith("write_blif"):
        Path(command.split()[1]).write_text(spec["lo_blif"])
"""

FAKE_YOSYS = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
if sys.argv[1:] == ["-V"]:
    sys.stdout.write(spec.get("version", "Yosys 0.55 (git sha1 60f126cd)\n"))
    sys.exit(0)
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "yosys.log", "a") as log:
    log.write(script + "\n")
if spec.get("yosys_signal"):
    os.kill(os.getpid(), spec["yosys_signal"])
if spec.get("yosys_rc"):
    sys.stderr.write("ERROR: cannot read in.il\n")
    sys.exit(spec["yosys_rc"])
for command in script.split("; "):
    if command.startswith("write_rtlil"):
        path = Path(command.split()[1])
        path.write_text(spec["yosys_rtlil"][path.name])
    elif command.startswith("write_blif"):
        Path(command.split()[-1]).write_text(spec["yo_blif"])
"""

FAKE_CHECK = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
name = Path(sys.argv[0]).name
with open(HERE / (name + ".log"), "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
if spec.get(name + "_signal"):
    os.kill(os.getpid(), spec[name + "_signal"])
sys.stdout.write(spec.get(name + "_out", ""))
sys.exit(spec.get(name + "_rc", 0))
"""

IN_IL = "module \\m\n  cell $and $c1\n  end\nend\n"
LO_IL = "module \\m\n  cell $_AND_ $a\n  cell $_AND_ $b\nend\n"
YO_IL = LO_IL.replace("$a", "$x").replace("$b", "$y")
BLIF = ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"
MUX = ".model m\n.inputs s a b\n.outputs y\n.names s a b y\n01- 1\n1-1 1\n.end\n"
MUX_SWAPPED = MUX.replace("s a b y", "s b a y")


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


class Tree:
    def __init__(self, root: Path, real_compare: bool = False) -> None:
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir()
        for name, text in (("odin3", FAKE_ODIN), ("yosys", FAKE_YOSYS),
                           ("equiv-check", FAKE_CHECK)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        self.compare = str(helpers.REPO_ROOT / "tools/netlist-compare/netlist-compare")
        if not real_compare:
            (self.bin / "netlist-compare").write_text(FAKE_CHECK)
            (self.bin / "netlist-compare").chmod(0o755)
            self.compare = str(self.bin / "netlist-compare")
        (root / "cells").mkdir()
        (root / "cells" / "and_2.v").write_text("module m(input a, b, output y); endmodule\n")
        (root / "techlibs").mkdir()
        for name in ("adff2dff.v", "aldff2dff.v"):
            (root / "techlibs" / name).write_text("module x; endmodule\n")
        self.set_spec()

    def set_spec(self, **changes: Any) -> None:
        spec: dict[str, Any] = {
            "rtlil": {"in.il": IN_IL, "lo.il": LO_IL, "lo_opt.il": LO_IL},
            "yosys_rtlil": {"yo.il": YO_IL, "yo_opt.il": YO_IL},
            "lo_blif": BLIF, "yo_blif": BLIF}
        spec.update(changes)
        (self.bin / "fake.json").write_text(json.dumps(spec))

    def expect(self, text: str) -> list[str]:
        path = self.root / "expect.txt"
        path.write_text(text)
        return ["--expect", str(path), "--root", str(self.root / "cells")]

    def run(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.bin / "odin3"), "--yosys", str(self.bin / "yosys"),
                "--netlist-compare", self.compare,
                "--equiv-check", str(self.bin / "equiv-check"),
                "--parmys-techlibs", str(self.root / "techlibs"),
                "--work", str(self.root / "work"), *extra, str(self.root / "cells" / "and_2.v")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(ld.main, argv)

    def log(self, name: str) -> list[str]:
        return (self.bin / name).read_text().splitlines()


class PureTest(unittest.TestCase):
    def test_odin_script_is_the_spec_14_2_step_1(self) -> None:
        plan = ld.Plan(("a.v", "b.v"), "t", ("proc", "opt"), "--async error", None)
        self.assertEqual(
            ld.odin_script(plan, Path("w")),
            "read_verilog --nolatches a.v b.v; hierarchy --top t; proc; opt; write_rtlil w/in.il; "
            "lower --async error; write_rtlil w/lo.il; write_blif w/lo.blif; opt -fast; "
            "write_rtlil w/lo_opt.il")

    def test_a_blif_file_is_read_with_read_blif_and_the_techlib_first(self) -> None:
        plan = ld.Plan(("x.blif",), None, (), "", None)
        self.assertTrue(ld.odin_script(plan, Path("w")).startswith("read_blif x.blif; write_rtlil"))
        plan = ld.Plan(("a.v",), None, ("proc",), "", "lib/x.o3lib")
        self.assertTrue(ld.odin_script(plan, Path("w")).startswith(
            "read_techlib lib/x.o3lib; read_verilog --nolatches a.v; hierarchy -auto; proc;"))

    def test_the_storage_families_write_no_rtlil_of_the_lowered_netlist(self) -> None:
        plan = ld.Plan(("a.v",), None, ("proc", "opt"), "", None, False)
        self.assertEqual(
            ld.odin_script(plan, Path("w")),
            "read_verilog --nolatches a.v; hierarchy -auto; proc; opt; write_rtlil w/in.il; lower; "
            "write_blif w/lo.blif")

    def test_yosys_storage_subckts_become_latches_and_blif_histograms_count_them(self) -> None:
        yosys = (".model m\n.inputs c d\n.outputs q r\n.subckt $_DFF_P_ C=c D=d Q=q\n"
                 ".subckt $_DFF_N_ D=d Q=r C=c\n.subckt $_MUX_ A=a B=b S=s Y=y\n.end\n")
        odin = ".model m\n.inputs c d\n.outputs q r\n.latch d q re c 3\n.latch d r fe c 3\n.end\n"
        self.assertEqual(lc.storage_to_latch(yosys).splitlines()[3:5],
                         [".latch d q re c 3", ".latch d r fe c 3"])
        self.assertEqual(dict(lc.blif_histogram(odin)["m"]), {"$_DFF_P_": 1, "$_DFF_N_": 1})
        self.assertEqual(dict(lc.blif_histogram(lc.storage_to_latch(yosys))["m"]),
                         {"$_DFF_P_": 1, "$_DFF_N_": 1, "$_MUX_": 1})

    def test_the_ff_maps_cover_dffsr_and_parmys_exact_swaps_the_enable_maps(self) -> None:
        self.assertIn("dffsr2dff.v", ld.FF_MAPS)
        self.assertEqual(ld.PARMYS_EXACT["adffe2dff_reset_first.v"], "adffe2dff.v")
        self.assertEqual(ld.PARMYS_EXACT["aldffe2dff_reset_first.v"], "aldffe2dff.v")
        for name in ld.FF_MAPS:
            parmys = name in ("adff2dff.v", "aldff2dff.v")
            self.assertTrue((ld.MAPS / name).exists() or parmys, name)

    def test_yosys_script_is_the_spec_14_2_step_2(self) -> None:
        self.assertEqual(
            ld.yosys_script(Path("w"), [Path("m/a.v"), Path("m/b.v")]),
            "read_rtlil w/in.il; techmap -map m/a.v; techmap -map m/b.v; techmap; dffunmap; "
            "opt_clean -purge; write_rtlil w/yo.il; "
            "write_blif -icells -true + vcc -false + gnd -undef + unconn w/yo.blif; opt -fast; "
            "write_rtlil w/yo_opt.il")

    def test_tiers(self) -> None:
        self.assertEqual(ld.tier_of(ld.family_named("bitwise"), None, ""), "S")
        self.assertEqual(ld.tier_of(ld.family_named("arith"), None, ""), "T")
        self.assertEqual(ld.tier_of(ld.family_named("mem"), None, ""), "D")
        self.assertEqual(ld.tier_of(ld.family_named("arith"), "D", ""), "D")
        sync = "module \\m\n  cell $dffe $c\nend\n"
        async_ = "module \\m\n  cell $adffe $c\nend\n"
        self.assertEqual(ld.tier_of(ld.family_named("ff"), None, sync), "S")
        self.assertEqual(ld.tier_of(ld.family_named("ff"), None, async_), "D")
        with self.assertRaises(lc.ToolError):
            ld.family_named("nosuch")

    def test_expect_parser(self) -> None:
        table = ld.parse_expect("# c\nsub/d.v arith histogram why not  # note\n"
                                "e.v * equiv another\n")
        self.assertEqual(table[("sub/d.v", "arith", "histogram")].reason, "why not")
        self.assertIn(("e.v", "*", "equiv"), table)
        for bad in ("d.v arith histogram\n", "d.v nosuch equiv why\n", "d.v arith nostep why\n"):
            with self.assertRaises(lc.ToolError):
                ld.parse_expect(bad)

    def test_rel_key(self) -> None:
        self.assertEqual(ld.rel_key(["/a/b/c/d.v"], Path("/a/b")), "c/d.v")
        self.assertEqual(ld.rel_key(["/a/b/c/d.v"], Path("/x")), "d.v")

    def test_unused_constant_generators_are_stripped(self) -> None:
        blif = (".model m\n.inputs a\n.outputs y z\n.names gnd\n.names vcc\n1\n.names unconn\n"
                ".subckt $_AND_ A=a B=gnd Y=y\n.names z\n1\n.end\n")
        stripped = lc.strip_unused_constants(blif)
        self.assertEqual(stripped, ".model m\n.inputs a\n.outputs y z\n.names gnd\n"
                         ".subckt $_AND_ A=a B=gnd Y=y\n.names z\n1\n.end\n")

    def test_a_net_read_only_on_a_continuation_line_keeps_its_constant(self) -> None:
        blif = (".model m\n.inputs a\n.outputs y\n.names one\n1\n"
                ".subckt $_AND_ A=a \\\n B=one Y=y\n.end\n")
        self.assertIn(".names one", lc.strip_unused_constants(blif))

    def test_histograms_ignore_constants_and_compare_per_module(self) -> None:
        text = "module \\m\n  cell $_AND_ $a\n  cell $_CONST0_ $z\n  cell $_AND_ $b\nend\n"
        self.assertEqual(dict(lc.histogram(text)["m"]), {"$_AND_": 2})
        other = "module \\m\n  cell $_AND_ $a\nend\n"
        self.assertEqual(lc.histogram_diff(lc.histogram(text), lc.histogram(text)), "")
        self.assertIn("module m: odin3 $_AND_ 2; yosys $_AND_ 1",
                      lc.histogram_diff(lc.histogram(text), lc.histogram(other)))


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tree = Tree(Path(tempfile.mkdtemp(prefix="lower_diff_test_")))

    def test_tier_s_agrees(self) -> None:
        code, out, err = self.tree.run()
        self.assertEqual(code, 0, out + err)
        self.assertIn("lower-diff: and_2.v [bitwise/S]: ok", out)
        self.assertEqual(len(self.tree.log("odin.log")), 1)
        self.assertIn("lower;", self.tree.log("odin.log")[0] + ";")
        self.assertIn("--implicit-models", self.tree.log("netlist-compare.log")[0])
        self.assertFalse((self.tree.bin / "equiv-check.log").exists())

    def test_tier_s_histogram_difference_fails(self) -> None:
        self.tree.set_spec(yosys_rtlil={"yo.il": "module \\m\n  cell $_AND_ $x\nend\n",
                                        "yo_opt.il": YO_IL})
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("[bitwise/S]: histogram: DIFFERS: module m: odin3 $_AND_ 2; yosys $_AND_ 1",
                      out)

    def test_tier_s_identity_failure_and_error(self) -> None:
        self.tree.set_spec(**{"netlist-compare_rc": 1})
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("identity: DIFFERS: netlist-compare: not identical", out)
        self.tree.set_spec(**{"netlist-compare_rc": 2})
        code, out, _ = self.tree.run()
        self.assertEqual(code, 2)
        self.assertNotIn("DIFFERS", out)

    def test_tier_t_uses_equiv_check_and_the_opt_fast_histograms(self) -> None:
        lone_or = "module \\m\n  cell $_OR_ $a\nend\n"
        self.tree.set_spec(rtlil={"in.il": IN_IL, "lo.il": LO_IL, "lo_opt.il": lone_or})
        code, out, _ = self.tree.run("--family", "arith")
        self.assertEqual(code, 1)
        self.assertIn("[arith/T]: histogram: DIFFERS", out)
        self.assertEqual(len(self.tree.log("equiv-check.log")), 1)
        self.assertFalse((self.tree.bin / "netlist-compare.log").exists())

    def test_tier_t_equiv_failure_unproven_hard_and_error(self) -> None:
        self.tree.set_spec(**{"equiv-check_rc": 1, "equiv-check_out": "NOT equivalent\n"})
        code, out, _ = self.tree.run("--family", "arith")
        self.assertEqual(code, 1)
        self.assertIn("equiv: DIFFERS: equiv-check: NOT equivalent", out)
        self.tree.set_spec(**{"equiv-check_rc": 4, "equiv-check_out": "ABC timed out\n"})
        code, out, _ = self.tree.run("--family", "arith")
        self.assertEqual(code, 1)
        self.assertIn("equiv: UNPROVEN: ABC timed out", out)
        self.assertNotIn(": ok", out)
        listed = self.tree.expect("and_2.v arith equiv timeout, known\n")
        code, out, _ = self.tree.run("--family", "arith", *listed)
        self.assertEqual(code, 0)
        self.assertIn("equiv: differs as listed (timeout, known)", out)
        self.assertNotIn(": ok", out)
        self.tree.set_spec(**{"equiv-check_rc": 5})
        self.assertEqual(self.tree.run("--family", "arith")[0], 3)
        self.tree.set_spec(**{"equiv-check_rc": 2})
        self.assertEqual(self.tree.run("--family", "arith")[0], 2)

    def test_a_design_with_no_word_cell_compared_nothing_and_fails(self) -> None:
        folded = "module \\m\nend\n"  # opt folded the cell away: nothing in, nothing out
        self.tree.set_spec(rtlil={"in.il": folded, "lo.il": folded, "lo_opt.il": folded},
                           yosys_rtlil={"yo.il": folded, "yo_opt.il": folded})
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("[bitwise/S]: compared: NOTHING TO COMPARE", out)
        self.assertNotIn(": ok", out)
        code, out, _ = self.tree.run("--family", "arith")
        self.assertEqual(code, 1)
        self.assertIn("[arith/T]: compared: NOTHING TO COMPARE", out)

    def test_an_expect_line_may_list_an_empty_comparison_until_cells_return(self) -> None:
        folded = "module \\m\nend\n"
        self.tree.set_spec(rtlil={"in.il": folded, "lo.il": folded, "lo_opt.il": folded},
                           yosys_rtlil={"yo.il": folded, "yo_opt.il": folded})
        extra = self.tree.expect("and_2.v * compared folded by opt\n")
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 0)
        self.assertIn("compared: differs as listed (folded by opt)", out)
        self.tree.set_spec()
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 1)
        self.assertIn("compared: stale entry in the expect list", out)

    def test_ok_says_how_many_word_cells_were_compared(self) -> None:
        self.tree.set_spec(rtlil={"in.il": "module \\m\n  cell $and $a\n  cell $or $b\n"
                                  "  cell $_AND_ $g\nend\n", "lo.il": LO_IL, "lo_opt.il": LO_IL})
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0, out)
        self.assertIn("[bitwise/S]: ok (2 cells)", out)

    def test_tier_d_compares_with_the_declared_maps(self) -> None:
        self.tree.set_spec(rtlil={"in.il": "module \\m\n  cell $adffe $c\nend\n", "lo.il": LO_IL,
                                  "lo_opt.il": LO_IL})
        code, out, err = self.tree.run("--family", "ff")
        self.assertEqual(code, 0, out + err)
        self.assertIn("[ff/D]: ok", out)
        script = self.tree.log("yosys.log")[0]
        self.assertIn("techmap -map " + str(self.tree.root / "techlibs" / "adff2dff.v"), script)
        self.assertIn("adffe2dff_reset_first.v", script)
        self.assertIn("dffsre2dff.v", script)
        self.assertEqual(len(self.tree.log("equiv-check.log")), 1)

    def test_a_missing_map_is_an_error(self) -> None:
        code, _, err = self.tree.run("--family", "ff", "--parmys-techlibs",
                                     str(self.tree.root / "nowhere"))
        self.assertEqual(code, 2)
        self.assertIn("adff2dff.v not found", err)

    def test_a_listed_difference_is_not_a_failure_and_a_stale_entry_is(self) -> None:
        self.tree.set_spec(**{"netlist-compare_rc": 1})
        extra = self.tree.expect("and_2.v bitwise identity because\n")
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 0)
        self.assertIn("identity: differs as listed (because)", out)
        self.tree.set_spec()
        code, out, _ = self.tree.run(*extra)
        self.assertEqual(code, 1)
        self.assertIn("identity: stale entry in the expect list (because)", out)

    def test_yosys_other_than_0_55_is_refused(self) -> None:
        self.tree.set_spec(version="Yosys 0.54 (git sha1 abc)\n")
        code, _, err = self.tree.run()
        self.assertEqual(code, 2)
        self.assertIn("is Yosys 0.54; the oracle is Yosys 0.55", err)

    def test_tool_failures_are_errors_and_crashes_are_hard(self) -> None:
        self.tree.set_spec(odin_rc=1)
        code, _, err = self.tree.run()
        self.assertEqual(code, 2)
        self.assertIn("odin3 exit 1: odin3: error: bad thing", err)
        self.tree.set_spec(odin_signal=11)
        self.assertEqual(self.tree.run()[0], 3)
        self.tree.set_spec(yosys_signal=9)
        self.assertEqual(self.tree.run()[0], 3)
        self.tree.set_spec(yosys_rc=1)
        self.assertEqual(self.tree.run()[0], 2)

    def test_a_crash_is_never_absorbed_by_an_expect_line(self) -> None:
        self.tree.set_spec(**{"netlist-compare_signal": 11})
        extra = self.tree.expect("and_2.v bitwise identity anything goes\n")
        code, out, err = self.tree.run(*extra)
        self.assertEqual(code, 3, out + err)
        self.assertNotIn("differs as listed", out)

    def test_a_timeout_is_hard_with_no_traceback(self) -> None:
        argv = ["--odin3", str(self.tree.bin / "odin3"), "--yosys", str(self.tree.bin / "yosys"),
                "--work", str(self.tree.root / "work"), str(self.tree.root / "cells" / "and_2.v")]
        with mock.patch.object(memcap, "run",
                               side_effect=subprocess.TimeoutExpired(["odin3"], 1.0)):
            code, _, err = helpers.run_main(ld.main, argv)
        self.assertEqual(code, 3, err)
        self.assertIn("timed out", err)
        self.assertNotIn("Traceback", err)

    def test_each_runs_every_file_as_its_own_design(self) -> None:
        second = self.tree.root / "cells" / "and_3.v"
        second.write_text("module m; endmodule\n")
        code, out, _ = self.tree.run("--each", str(second))
        self.assertEqual(code, 0)
        self.assertIn("and_2.v [bitwise/S]: ok", out)
        self.assertIn("and_3.v [bitwise/S]: ok", out)
        self.assertEqual(len(self.tree.log("odin.log")), 2)

    def test_the_work_dir_is_kept(self) -> None:
        code, _, _ = self.tree.run()
        self.assertEqual(code, 0)
        self.assertTrue((self.tree.root / "work" / "d0000" / "lo.blif").exists())


class BeyondXTest(unittest.TestCase):
    """3B-2 F1 (3B-4 review M1): a ``shift`` equiv entry excuses a difference only on the outputs
    the input leaves open (x), which is what ``lower-equiv``'s refinement check decides."""

    def setUp(self) -> None:
        self.tree = Tree(Path(tempfile.mkdtemp(prefix="lower_diff_beyond_x_")))
        self.tree.set_spec(**{"equiv-check_rc": 1, "equiv-check_out": "NOT equivalent\n"})
        self.extra = self.tree.expect("and_2.v shift equiv open (x) output bits differ\n")

    def run_with(self, kind: str) -> tuple[int, str]:
        import lower_equiv

        with mock.patch.object(lower_equiv, "make_gate", return_value=Path("gate.il")), \
                mock.patch.object(lower_equiv, "refine", return_value=(kind, "detail")):
            code, out, err = self.tree.run("--family", "shift", *self.extra)
        return code, out + err

    def test_a_difference_confined_to_x_outputs_is_excused_by_the_entry(self) -> None:
        code, out = self.run_with("ok")
        self.assertEqual(code, 0, out)
        self.assertIn("equiv: differs as listed (open (x) output bits differ)", out)
        self.assertNotIn("NOT confined", out)

    def test_a_difference_beyond_x_fails_although_the_entry_lists_it(self) -> None:
        code, out = self.run_with("differs")
        self.assertEqual(code, 1, out)
        self.assertIn("equiv: the difference is NOT confined to outputs the input leaves open", out)

    def test_an_unproven_refinement_is_not_an_excuse(self) -> None:
        code, out = self.run_with("unproven")
        self.assertEqual(code, 1, out)
        self.assertIn("NOT confined", out)

    def test_the_narrowing_applies_to_the_shift_family_only(self) -> None:
        import lower_equiv

        extra = self.tree.expect("and_2.v arith equiv a listed difference\n")
        with mock.patch.object(lower_equiv, "refine", side_effect=AssertionError("asked")):
            code, out, err = self.tree.run("--family", "arith", *extra)
        self.assertEqual(code, 0, out + err)


class RealCompareTest(unittest.TestCase):
    """The real netlist-compare on real BLIF: equal passes, a swapped gate input is exit 1."""

    def test_known_equal_pair_is_ok_and_swapped_gate_inputs_differ(self) -> None:
        tree = Tree(Path(tempfile.mkdtemp(prefix="lower_diff_test_")), real_compare=True)
        tree.set_spec(lo_blif=MUX, yo_blif=MUX)
        code, out, err = tree.run()
        self.assertEqual(code, 0, out + err)
        tree.set_spec(lo_blif=MUX, yo_blif=MUX_SWAPPED)
        code, out, err = tree.run()
        self.assertEqual(code, 1, out + err)
        self.assertIn("identity: DIFFERS: netlist-compare: not identical", out)


class GateModelsTest(unittest.TestCase):
    def test_every_gate_spelled_as_a_subckt_has_a_model(self) -> None:
        text = lc.with_gate_models(".model m\n.end\n")
        for gate in ("NOT", "AND", "OR", "XOR", "XNOR", "MUX"):
            self.assertIn(f".model $_{gate}_\n", text)
        self.assertTrue(text.startswith(".model m\n.end\n"))


if __name__ == "__main__":
    unittest.main()
