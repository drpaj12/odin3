"""Tests for tools/opt-check/opt-equiv (2F Task 17, spec 20.2): refinement after every sub-pass.

Fakes stand in for both tools in the argv tests: the fake ``odin3`` writes the snapshot pairs of
``fake.json`` into the ``--opt-snapshot`` directory and prints ``log``; the fake ``yosys`` exits 1
when the script's gate file holds the word ``BAD``. ``memcap.run`` is a plain subprocess call. The
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
if "BAD" in Path(gate).read_text():
    sys.stdout.write("ERROR: Found 1 unproven $equiv cells in module eq\n")
    sys.exit(1)
"""

ODIN_LOG = "odin3: info: opt: rule const_and 2\nodin3: info: opt: rule opt_dff.add_en 1\n"
IL_GOOD = "attribute \\top 1\nmodule \\t\n  wire input 1 \\a\nend\n"
IL_BAD = "attribute \\top 1\nmodule \\t\n  wire input 1 \\a\n  # BAD\nend\n"


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


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
            "read_rtlil g.il; read_rtlil h.il; clk2fflogic; techmap; equiv_make gold gate eq; "
            "hierarchy -top eq; flatten; equiv_simple -undef -seq 4; equiv_induct -undef; "
            "equiv_status -assert")

    def test_script_variants(self) -> None:
        plain = oe.refinement_script("g.il", "h.il", lowering=())
        self.assertNotIn("clk2fflogic", plain)
        self.assertNotIn("proc", plain)
        self.assertIn("read_rtlil h.il; equiv_make gold gate eq", plain)
        procs = oe.refinement_script("g.il", "h.il", procs=True, lowering=oe.LOWERINGS[0])
        self.assertIn("read_rtlil h.il; proc; clk2fflogic; equiv_make", procs)
        self.assertNotIn("techmap", procs)
        self.assertIn("memory; clk2fflogic; techmap; equiv_make",
                      oe.refinement_script("g.il", "h.il", lowering=oe.LOWERINGS[2]))

    def test_phase2_commands_and_stats(self) -> None:
        commands = oe.phase2_commands()
        self.assertEqual(commands[0], "opt_expr")
        self.assertIn("proc", commands)
        stats = oe.with_stats(commands)
        self.assertIn("opt -nodffe -nosdff --opt-stats", stats)
        self.assertIn("opt --opt-stats", stats)
        self.assertIn("proc", stats)  # not an opt word
        self.assertIn("compact", stats)

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
        self.assertEqual(len(self.fakes.log("yosys.log")), 2)

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
                self.assertFalse(self.refines(CLOCK_GOLD, text))

    def test_no_lowering_of_the_chain_hides_the_clock(self) -> None:
        for lowering in oe.LOWERINGS:
            self.assertIn("clk2fflogic", lowering)
            self.assertNotIn("async2sync", lowering)


if __name__ == "__main__":
    unittest.main()
