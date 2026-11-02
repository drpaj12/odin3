"""Hierarchical references into another module (tinyriscv_soc_top.v: `u_regs.regs[26]` read in an
always block). As in Yosys each dotted path that no declaration answers is an undriven net of the
reading module (a WARNING). The micro tests/flatten/hierref.v runs through proc, opt, flatten,
opt, lower, write_blif and sim; the flatten collision (`assign z = u.w;` with a child wire `w`)
leaves the foreign net `u.w` and renames the child wire to `u.w_1`. Run through CTest
`hier_ref_flow` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")
ROOT = Path(__file__).resolve().parents[2]
MICRO = ROOT / "tests" / "flatten" / "hierref.v"
FLOW = "read_verilog --nolatches {src}; hierarchy --top top; proc; opt; flatten; opt; lower"


def run(script: str) -> subprocess.CompletedProcess[str]:
    assert ODIN3 is not None
    return subprocess.run([ODIN3, "--top", "top", "-p", script], capture_output=True, text=True)


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest hier_ref_flow)")
class HierRefFlowTest(unittest.TestCase):
    def test_micro_flows_to_blif_and_sim(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            blif, vec = Path(tmp) / "o.blif", Path(tmp) / "v.txt"
            done = run(f"{FLOW.format(src=MICRO)}; write_blif {blif}; sim 16 1 {vec}")
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertTrue(blif.read_text().startswith(".model top"))
            self.assertTrue(vec.exists() and vec.read_text())
        # one warning per reference, each saying the net is undriven
        warned = [ln for ln in done.stderr.splitlines() if "is not supported; it is treated" in ln]
        self.assertEqual(len(warned), 2, done.stderr)
        self.assertTrue(all("undriven net" in ln for ln in warned))

    def test_flatten_collision_renames_the_child_wire(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "o.v"
            done = run(f"read_verilog --nolatches {MICRO}; hierarchy --top top; proc; flatten; "
                       f"write_verilog {out}")
            self.assertEqual(done.returncode, 0, done.stderr)
            text = out.read_text()
        self.assertIn("wire \\u.w ;", text)  # the foreign net keeps the name
        self.assertIn("wire \\u.w_1 ;", text)  # the real child wire is renamed
        self.assertIn("assign z = \\u.w ;", text)  # z reads the undriven foreign net
        self.assertIn("assign \\u.w_1 = y;", text)


if __name__ == "__main__":
    unittest.main()
