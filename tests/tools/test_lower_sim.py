"""Tests for tools/lower-check/lower-sim (3B Task 2, spec 14.3): the simulator before and after.

A fake ``odin3`` stands in for the real one; ``fake.json`` beside it says what its ``write_rtlil``
and ``sim`` write. The mutant test shows an injected fault in the lowered netlist is exit 1, and the
``$shiftx`` test that a design without a simulate hook is reported ``skipped``, never ``pass``. The
work directories are made by ``tempfile.mkdtemp`` and are left in place.
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
import lower_sim as ls
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
commands = script.split("; ")
lowered = any(c.startswith("lower") for c in commands)
mutated = any(c.startswith("read_blif") and "mutated" in c for c in commands)
if spec.get("odin_signal"):
    os.kill(os.getpid(), spec["odin_signal"])
for command in commands:
    if command.startswith("write_rtlil"):
        Path(command.split()[1]).write_text(spec["before_il"])
    elif command.startswith("write_blif"):
        Path(command.split()[1]).write_text(spec["lowered_blif"])
    elif command.startswith("sim "):
        kind = "mutated" if mutated else "after" if lowered else "before"
        if spec.get(kind + "_rc"):
            sys.stderr.write("odin3: error: cannot simulate `$shiftx`\n")
            sys.exit(spec[kind + "_rc"])
        Path(command.split()[3]).write_text(spec[kind + "_vec"])
"""

AND_IL = "module \\m\n  cell $and $c1\n  cell $mux $c2\nend\n"
SHIFTX_IL = "module \\m\n  cell $and $c1\n  cell $shiftx $c2\nend\n"
VEC = "cycle 0 in 01 out 0\ncycle 1 in 11 out 1\n"
BLIF = ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


class Tree:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir()
        (self.bin / "odin3").write_text(FAKE_ODIN)
        (self.bin / "odin3").chmod(0o755)
        (root / "hand").mkdir()
        (root / "hand" / "and_mux.v").write_text("module m(input a, b, output y); endmodule\n")
        self.set_spec()

    def set_spec(self, **changes: Any) -> None:
        spec: dict[str, Any] = {"before_il": AND_IL, "before_vec": VEC, "after_vec": VEC,
                                "mutated_vec": VEC.replace("out 1", "out 0"),
                                "lowered_blif": BLIF}
        spec.update(changes)
        (self.bin / "fake.json").write_text(json.dumps(spec))

    def run(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.bin / "odin3"), "--work", str(self.root / "work"), *extra,
                str(self.root / "hand" / "and_mux.v")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(ls.main, argv)

    def log(self) -> list[str]:
        return (self.bin / "odin.log").read_text().splitlines()


class PureTest(unittest.TestCase):
    def test_odin_script_before_and_after(self) -> None:
        args = ls.parse(["--odin3", "o", "--vectors", "8", "--seed", "3", "a.v"])
        self.assertEqual(
            ls.odin_script(["a.v"], args, Path("w"), False, Path("w/b.vec")),
            "read_verilog --nolatches a.v; hierarchy -auto; proc; opt; write_rtlil w/before.il; "
            "sim 8 3 w/b.vec")
        self.assertEqual(
            ls.odin_script(["a.v"], args, Path("w"), True, Path("w/a.vec")),
            "read_verilog --nolatches a.v; hierarchy -auto; proc; opt; lower; sim 8 3 w/a.vec")

    def test_mutation_inverts_the_first_cover_only(self) -> None:
        blif = ".model m\n.names a b y\n11 1\n.names y z\n1 1\n.end\n"
        self.assertEqual(ls.mutate(blif, "invert-first"),
                         ".model m\n.names a b y\n11 0\n.names y z\n1 1\n.end\n")
        with self.assertRaises(lc.ToolError):
            ls.mutate(".model m\n.end\n", "invert-first")
        with self.assertRaises(lc.ToolError):
            ls.mutate(blif, "nosuch")

    def test_first_difference_names_the_line(self) -> None:
        self.assertIn("line 2", ls.first_difference("a\nb\n", "a\nc\n"))
        self.assertIn("2 and 3 lines", ls.first_difference("a\nb\n", "a\nb\nc\n"))

    def test_the_skip_table_covers_the_types_without_a_hook(self) -> None:
        for name in ("$shl", "$shr", "$sshr", "$shiftx", "$div", "$mod", "$pow", "$tribuf", "$mem",
                     "$dff", "$adffe", "$dlatch", "$sr"):
            self.assertIn(name, ls.SKIP_REASONS)
        for name in ("$and", "$mux", "$pmux", "$add", "$mul", "$eq", "$reduce_or"):
            self.assertNotIn(name, ls.SKIP_REASONS)


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tree = Tree(Path(tempfile.mkdtemp(prefix="lower_sim_test_")))

    def test_equal_vectors_pass_per_cell_type_and_print_the_sample_points(self) -> None:
        code, out, err = self.tree.run()
        self.assertEqual(code, 0, out + err)
        self.assertIn("lower-sim: and_mux.v: $and: pass", out)
        self.assertIn("lower-sim: and_mux.v: $mux: pass", out)
        self.assertIn("sample points: outputs after each cycle's active edge has settled "
                      "(64 cycles, seed 1)", out)
        self.assertEqual(len(self.tree.log()), 2)

    def test_different_vectors_are_exit_1_with_the_first_line(self) -> None:
        self.tree.set_spec(after_vec=VEC.replace("out 1", "out 0"))
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("DIFFERS: line 2: before 'cycle 1 in 11 out 1', after 'cycle 1 in 11 out 0'",
                      out)
        self.assertNotIn(": pass", out)

    def test_an_injected_mutant_of_the_lowered_netlist_is_exit_1(self) -> None:
        self.tree.set_spec(after_vec=VEC)
        code, out, _ = self.tree.run("--mutant", "invert-first")
        self.assertEqual(code, 1, out)
        self.assertIn("DIFFERS", out)
        mutated = self.tree.root / "work" / "d0000" / "mutated.blif"
        self.assertIn("11 0", mutated.read_text())
        self.assertIn("read_blif", self.tree.log()[-1])

    def test_a_design_folded_to_no_word_cell_compares_nothing_and_fails(self) -> None:
        # `assign y = a & 4'b0000;` after opt: the cell is gone, so is every type
        self.tree.set_spec(before_il="module \\m\nend\n")
        code, out, err = self.tree.run()
        self.assertEqual(code, 1, out + err)
        self.assertIn("lower-sim: and_mux.v: NOTHING TO COMPARE: no word cell left after "
                      "'proc; opt'", out)
        self.assertNotIn("pass", out)
        self.assertEqual(len(self.tree.log()), 1)  # the lowered run never happens

    def test_only_bit_cells_left_is_also_nothing_to_compare(self) -> None:
        self.tree.set_spec(before_il="module \\m\n  cell $_AND_ $g\nend\n")
        self.assertEqual(self.tree.run()[0], 1)

    def test_allow_empty_makes_nothing_to_compare_exit_0_but_still_says_so(self) -> None:
        self.tree.set_spec(before_il="module \\m\nend\n")
        code, out, _ = self.tree.run("--allow-empty")
        self.assertEqual(code, 0)
        self.assertIn("NOTHING TO COMPARE", out)

    def test_a_shiftx_design_is_skipped_never_passed(self) -> None:
        self.tree.set_spec(before_il=SHIFTX_IL, before_rc=1)
        code, out, err = self.tree.run()
        self.assertEqual(code, ls.EXIT_SKIPPED, out + err)  # H3: a skip is not a success
        self.assertIn("lower-sim: compared 0 of 1 designs (1 skipped, 0 empty)", out)
        self.assertEqual(len(self.tree.log()), 1)  # the lowered run never happens
        code, out, err = self.tree.run("--allow-skipped")
        self.assertEqual(code, 0, out + err)
        self.assertIn("$shiftx: skipped (no simulate hook; x outputs: covered by lower-equiv)", out)
        self.assertIn("$and: skipped (design skipped)", out)
        self.assertNotIn("pass", out)

    def test_a_compared_design_is_counted_and_a_difference_beats_a_skip(self) -> None:
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0)
        self.assertIn("lower-sim: compared 1 of 1 designs (0 skipped, 0 empty)", out)
        args = ls.parse(["--odin3", "x", "f.v"])
        self.assertEqual(ls.exit_code(["equal", "skipped"], args), ls.EXIT_SKIPPED)
        self.assertEqual(ls.exit_code(["skipped", "differs"], args), 1)
        self.assertEqual(ls.exit_code(["equal", "empty"], args), 1)

    def test_a_before_failure_that_is_not_a_known_skip_is_a_tool_error(self) -> None:
        self.tree.set_spec(before_rc=1)
        code, _, err = self.tree.run()
        self.assertEqual(code, 2)
        self.assertIn("odin3 (before) exit 1", err)

    def test_a_lowered_design_that_does_not_simulate_is_a_difference(self) -> None:
        self.tree.set_spec(after_rc=1)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("DIFFERS: the lowered design does not simulate", out)

    def test_an_odin3_crash_is_hard(self) -> None:
        self.tree.set_spec(odin_signal=11)
        code, _, err = self.tree.run()
        self.assertEqual(code, 3)
        self.assertIn("HARD", err)

    def test_vectors_and_seed_reach_both_runs(self) -> None:
        self.tree.run("--vectors", "5", "--seed", "9")
        for line in self.tree.log():
            self.assertIn("sim 5 9 ", line)

    def test_a_timeout_is_hard_with_no_traceback(self) -> None:
        argv = ["--odin3", str(self.tree.bin / "odin3"), "--work", str(self.tree.root / "work"),
                str(self.tree.root / "hand" / "and_mux.v")]
        with mock.patch.object(memcap, "run",
                               side_effect=subprocess.TimeoutExpired(["odin3"], 1.0)):
            code, _, err = helpers.run_main(ls.main, argv)
        self.assertEqual(code, 3, err)
        self.assertNotIn("Traceback", err)


if __name__ == "__main__":
    unittest.main()
