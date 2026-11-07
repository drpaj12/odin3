"""``sim_tb``: nonblocking assignment with an intra-assignment delay, ``a <= #d b``. The expected
text is what iverilog 12 prints for the same bench. Needs ``ODIN3_CLI``."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")

# The right side is read when the statement runs; the write lands in the NBA region of time + d,
# after the processes that run in the active region of that time.
TB = """module t;
  reg [3:0] q;
  reg r;
  reg [1:0] m [0:1];
  always @(q) $display("%0t q=%0d", $time, q);
  initial begin
    q = 0; r = 0;
    q <= #3 5;
    q <= #5 9;
    {r, m[1]} <= #2 3'b101;
    #1 $display("%0t at 1: q=%0d", $time, q);
    #1 $display("%0t at 2: r=%b m1=%b", $time, r, m[1]);
    #1 $display("%0t at 3 active: q=%0d", $time, q);
    #1 $display("%0t at 4: q=%0d", $time, q);
    #10 $finish;
  end
endmodule
"""

EXPECTED = ["0 q=0", "1 at 1: q=0", "2 at 2: r=0 m1=xx", "3 at 3 active: q=0", "3 q=5",
            "4 at 4: q=5", "5 q=9"]


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class NbaDelayTest(unittest.TestCase):
    def test_delayed_nonblocking_writes(self) -> None:
        work = helpers.scratch(self, "test-tb-nba-")
        (work / "t.v").write_text(TB)
        done = subprocess.run([ODIN3, "-p", "sim_tb t.v"], capture_output=True, text=True,
                              cwd=work, check=False, timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip().split("\n"), EXPECTED)

    def test_blocking_intra_assignment_delay_still_refused(self) -> None:
        work = helpers.scratch(self, "test-tb-nba-")
        (work / "t.v").write_text("module t;\n  reg a;\n  initial a = #1 1'b1;\nendmodule\n")
        done = subprocess.run([ODIN3, "-p", "sim_tb t.v"], capture_output=True, text=True,
                              cwd=work, check=False, timeout=120)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("t.v:3: unsupported: intra-assignment delay", done.stderr)


if __name__ == "__main__":
    unittest.main()
