"""``sim_tb`` is 4-state by default (2026-11-07): a reg never written reads x; ``--2state`` is the
old interpreter (0) and ``--4state`` changes nothing. Needs ``ODIN3_CLI``."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")
TB = """module t;
  reg r;
  reg [3:0] v;
  initial begin
    #1 $display("r=%b v=%b", r, v);
    $finish;
  end
endmodule
"""


def run_tb(work: Path, flag: str) -> str:
    (work / "t.v").write_text(TB)
    done = subprocess.run([ODIN3, "-p", f"sim_tb {flag} --out o.txt t.v"], capture_output=True,
                          text=True, cwd=work, check=False, timeout=120)
    if done.returncode != 0:
        raise AssertionError(done.stdout + done.stderr)
    return (work / "o.txt").read_text().strip()


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class DefaultTest(unittest.TestCase):
    def test_default_is_four_state(self) -> None:
        work = helpers.scratch(self, "test-tb-default-")
        self.assertEqual(run_tb(work, ""), "r=x v=xxxx")
        self.assertEqual(run_tb(work, "--4state"), "r=x v=xxxx")

    def test_two_state_flag_keeps_the_old_behaviour(self) -> None:
        work = helpers.scratch(self, "test-tb-default-")
        self.assertEqual(run_tb(work, "--2state"), "r=0 v=0000")


if __name__ == "__main__":
    unittest.main()
