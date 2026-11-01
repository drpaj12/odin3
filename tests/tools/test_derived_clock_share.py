"""`proc --share-selects` (one select cell per case, not per signal) leaves `opt` output equal up
to net names; the simulator takes derived clocks and names the net of one it cannot take."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

CLI = os.environ.get("ODIN3_CLI", "")

CASE = """module m(input [1:0] s, input [3:0] a, input [3:0] b, output reg [3:0] x,
          output reg [3:0] y);
  always @* case (s)
    2'd0: begin x = a; y = b; end
    2'd1: begin x = b; y = a; end
    2'd2: begin x = a ^ b; y = 4'd7; end
    default: begin x = 4'd0; y = 4'd1; end
  endcase
endmodule
"""

GATED_BY_TWO = """module m(input a, input b, input d, output reg q);
  wire g = a & b;
  always @(posedge g) q <= d;
endmodule
"""

UNDRIVEN = """module m(input d, output reg q);
  wire g;
  always @(posedge g) q <= d;
endmodule
"""


def run(script: str, top: str = "m") -> "subprocess.CompletedProcess[str]":
    return subprocess.run([CLI, "--top", top, "-p", script], capture_output=True, text=True,
                          check=False)


def cell_types(text: str) -> list[str]:
    """The sorted `.subckt` types of a BLIF."""
    return sorted(line.split()[1] for line in text.splitlines() if line.startswith(".subckt"))


@unittest.skipUnless(CLI, "ODIN3_CLI not set")
class ShareSelectsTest(unittest.TestCase):
    def test_fewer_cells_same_opt_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.v"
            src.write_text(CASE)
            plain = run(f"read_verilog {src}; proc --noopt; write_json {tmp}/p.json")
            shared = run(f"read_verilog {src}; proc --noopt --share-selects; "
                         f"write_json {tmp}/s.json")
            self.assertEqual(plain.returncode, 0, plain.stderr)
            self.assertEqual(shared.returncode, 0, shared.stderr)
            eq_plain = (Path(tmp) / "p.json").read_text().count('"$eq"')
            eq_shared = (Path(tmp) / "s.json").read_text().count('"$eq"')
            self.assertLess(eq_shared, eq_plain)
            for name, extra in (("a", ""), ("b", " --share-selects")):
                out = run(f"read_verilog {src}; proc{extra}; opt; write_blif {tmp}/{name}.blif; "
                          f"lower; sim 64 1 {tmp}/{name}.vec")
                self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(cell_types((Path(tmp) / "a.blif").read_text()),
                             cell_types((Path(tmp) / "b.blif").read_text()))
            self.assertEqual((Path(tmp) / "a.vec").read_text(), (Path(tmp) / "b.vec").read_text())


@unittest.skipUnless(CLI, "ODIN3_CLI not set")
class DerivedClockErrorTest(unittest.TestCase):
    def test_two_candidate_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.v"
            src.write_text(GATED_BY_TWO)
            out = run(f"read_verilog {src}; proc; opt; lower; sim 4 1")
            self.assertNotEqual(out.returncode, 0)
            self.assertIn("derived from 2 primary inputs", out.stderr)
            self.assertIn("cannot simulate: the clock of", out.stderr)

    def test_undriven_clock_net(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.v"
            src.write_text(UNDRIVEN)
            out = run(f"read_verilog {src}; proc; opt; lower; sim 4 1")
            self.assertNotEqual(out.returncode, 0)
            self.assertIn("cannot simulate: the clock of", out.stderr)


if __name__ == "__main__":
    unittest.main()
