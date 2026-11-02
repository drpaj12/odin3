"""`$tribuf` lowering (tinyriscv gpio inout). Yosys keeps a `$_TBUF_` bit cell on a port; the
registry has none, VPR has no tri-state and Yosys `opt_expr -mux_undef` (so Parmys) drops the
enable of a sole driver, so `lower` drives the output from the data input with a WARNING per
cell (the function differs while the enable is low). Several tri-states on one net (a bus), an
inout below the top, a constant 0, x or z enable and a chain stay an error. The micro
tests/lower/tribuf_inout.v (inout and output tri-states on the top) runs through proc, opt, lower,
write_blif and sim: write_blif and sim treat an inout port driven from inside as an output. With
ODIN3_TINYRISCV_TOP set (the unmodified tinyriscv_soc_top.v) it also lowers. Run through CTest
`tribuf_lower_flow` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")
ROOT = Path(__file__).resolve().parents[2]
MICRO = ROOT / "tests" / "lower" / "tribuf_inout.v"
WARNING = "the enable is dropped and the output is driven by the data input"


def run(script: str, top: str) -> subprocess.CompletedProcess[str]:
    assert ODIN3 is not None
    return subprocess.run([ODIN3, "--top", top, "-p", script], capture_output=True, text=True)


Bits = Sequence[object]


def tribuf_json(cells: Sequence[tuple[str, Bits, Bits, Bits]]) -> str:
    """A Yosys JSON module `m` (inputs a b e1 e2, output y) of 1-bit `$tribuf` cells. Each cell is
    (name, A bits, EN bits, Y bits); a bit is a net number or the constant "0", "1", "x", "z"."""
    ports = {name: {"direction": "input", "bits": [num]} for name, num in
             (("a", 2), ("b", 3), ("e1", 4), ("e2", 5))}
    ports["y"] = {"direction": "output", "bits": [7]}
    body = {name: {"type": "$tribuf", "parameters": {"WIDTH": "1".rjust(32, "0")},
                   "port_directions": {"A": "input", "EN": "input", "Y": "output"},
                   "connections": {"A": a, "EN": en, "Y": y}} for name, a, en, y in cells}
    return json.dumps({"creator": "test", "modules": {"m": {"attributes": {"top": 1},
                                                           "ports": ports, "cells": body}}})


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest tribuf_lower_flow)")
class TribufLowerTest(unittest.TestCase):
    def test_micro_flows_to_blif_and_sim_with_the_enable_dropped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            blif, vec = Path(tmp) / "o.blif", Path(tmp) / "v.txt"
            done = run(f"read_verilog --nolatches {MICRO}; hierarchy --top tribuf_inout; proc; "
                       f"opt; lower; write_blif {blif}; sim 32 1 {vec}", "tribuf_inout")
            self.assertEqual(done.returncode, 0, done.stderr)
            text = blif.read_text()
            vectors = vec.read_text()
        # one warning per tri-state: io (1 bit), bus (2 bits, one cell), o
        warned = [ln for ln in done.stderr.splitlines() if WARNING in ln]
        self.assertEqual(len(warned), 3, done.stderr)
        # the inout ports are written as outputs, each bit driven from its data input
        self.assertIn(".inputs d en dv[0] dv[1]\n.outputs io bus[0] bus[1] o\n", text)
        self.assertNotIn(".names en ", text)
        rows = [ln.split() for ln in vectors.splitlines() if ln and not ln.startswith("#")]
        self.assertEqual(len(rows), 32)
        enables = set()
        for _, ins, outs in rows:  # inputs: d en dv[1:0]; outputs: io bus[1:0] o
            d, en, dv = ins[0], ins[1], ins[2:4]
            enables.add(en)
            self.assertEqual((outs[0], outs[1:3], outs[3]), (d, dv, d), (ins, outs))
        self.assertEqual(enables, {"0", "1"})  # the outputs follow the data with en low as well

    def test_inout_without_an_inside_driver_is_still_refused(self) -> None:
        # in_out2 is read, never driven: not an output, so BLIF and sim refuse it as before.
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.v"
            src.write_text("module m(input d, input en, inout a, inout b, output o);\n"
                           "  assign a = en ? d : 1'bz;\n  assign o = b;\nendmodule\n")
            flow = f"read_verilog {src}; hierarchy --top m; proc; opt; lower; "
            blif = run(flow + f"write_blif {Path(tmp) / 'o.blif'}", "m")
            sim = run(flow + f"sim 4 1 {Path(tmp) / 'v.txt'}", "m")
        self.assertNotEqual(blif.returncode, 0)
        self.assertIn("BLIF has no inout ports; cannot write port 'b'", blif.stderr)
        self.assertNotEqual(sim.returncode, 0)
        self.assertIn("cannot simulate inout port `b`", sim.stderr)

    def test_bus_of_two_tristates_is_an_error_and_writes_nothing(self) -> None:
        # Two drivers of one net: dropping both enables would short b into a (a bus short).
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "m.v", Path(tmp) / "o.blif"
            src.write_text("module m(input a, input b, input ea, input eb, output y);\n"
                           "  assign y = ea ? a : 1'bz;\n  assign y = eb ? b : 1'bz;\nendmodule\n")
            done = run(f"read_verilog {src}; hierarchy --top m; proc; opt; lower; write_blif {out}",
                       "m")
            written = out.exists()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("has no bit-level cell in the registry", done.stderr)
        self.assertIn("tri-state bus", done.stderr)
        self.assertFalse(written)

    def lower_json(self, text: str) -> tuple[subprocess.CompletedProcess[str], bool]:
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "m.json", Path(tmp) / "o.blif"
            src.write_text(text)
            done = run(f"read_json {src}; hierarchy --top m; lower; write_blif {out}", "m")
            return done, out.exists()

    def test_constant_enables_are_refused_with_the_dead_enable_reason(self) -> None:
        # EN tied to 0, x or z: the output is never driven by the data (z, x), or never at all,
        # so dropping the enable would change the function more than the sole-driver case does.
        for value in ("0", "x", "z"):
            with self.subTest(enable=value):
                done, written = self.lower_json(tribuf_json([("t", [2], [value], [7])]))
                self.assertNotEqual(done.returncode, 0, done.stderr)
                self.assertIn("has no bit-level cell in the registry", done.stderr)
                self.assertIn("its enable is the constant 0, x or z", done.stderr)
                self.assertFalse(written)

    def test_constant_one_enable_is_the_sole_driver_case(self) -> None:
        # EN tied to 1 is not dead: the cell is lowered with the warning, as for a live enable.
        done, written = self.lower_json(tribuf_json([("t", [2], ["1"], [7])]))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn(WARNING, done.stderr)
        self.assertTrue(written)

    def test_tribuf_fed_by_a_tribuf_is_refused_in_both_cell_orders(self) -> None:
        # t1 drives net 6 from a; t2 drives y from net 6: the chain stays an error, whichever
        # of the two cells comes first in the module.
        t1 = ("t1", [2], [4], [6])
        t2 = ("t2", [6], [5], [7])
        for cells in ([t1, t2], [t2, t1]):
            with self.subTest(order=[c[0] for c in cells]):
                done, written = self.lower_json(tribuf_json(cells))
                self.assertNotEqual(done.returncode, 0, done.stderr)
                self.assertIn("has no bit-level cell in the registry", done.stderr)
                self.assertIn("its data input is the output of another $tribuf", done.stderr)
                self.assertFalse(written)

    @unittest.skipUnless(os.environ.get("ODIN3_TINYRISCV_TOP"), "ODIN3_TINYRISCV_TOP not set")
    def test_unmodified_tinyriscv_soc_lowers(self) -> None:
        top = Path(os.environ["ODIN3_TINYRISCV_TOP"])
        rtl = top.parents[1]
        others = sorted(str(f) for d in ("core", "perips", "debug", "utils")
                        for f in (rtl / d).glob("*.v") if f.name != "defines.v")
        files = " ".join(others)
        done = run(f"read_verilog --nolatches {top} {files}; "
                   f"hierarchy --top tinyriscv_soc_top; proc; opt; flatten; opt; lower",
                   "tinyriscv_soc_top")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertGreaterEqual(sum(WARNING in ln for ln in done.stderr.splitlines()), 2)


if __name__ == "__main__":
    unittest.main()
