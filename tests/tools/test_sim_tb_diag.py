"""``sim_tb`` diagnostics: a refusal names the source file and line, also when the testbench is
several files and `include d text (the preprocessor joins them). Needs ``ODIN3_CLI``."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")

HEADER = """// header
`define W 4
real bad;
"""

# `real` sits on line 3 of defs.h, included inside module a on line 3 of a.v, which is joined first.
A_V = """// a.v
module t;
`include "defs.h"
  reg [`W-1:0] x;
  initial $finish;
endmodule
"""

B_V = """// b.v
module t;
  initial begin
    $display("hi");
    $finish;
  end
  real r;
endmodule
"""


def run(work: Path, files: list[str]) -> subprocess.CompletedProcess[str]:
    script = "sim_tb --top t -I . " + " ".join(files)
    return subprocess.run([ODIN3, "-p", script], capture_output=True, text=True, cwd=work,
                          check=False, timeout=120)


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class DiagTest(unittest.TestCase):
    def test_error_in_the_second_file(self) -> None:
        work = helpers.scratch(self, "test-tb-diag-")
        (work / "defs.h").write_text(HEADER)
        (work / "a.v").write_text("// a.v\nmodule a;\nendmodule\n")
        (work / "b.v").write_text(B_V)
        done = run(work, ["a.v", "b.v"])
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("b.v:7: unsupported: real", done.stderr)

    def test_error_in_an_included_file(self) -> None:
        work = helpers.scratch(self, "test-tb-diag-")
        (work / "defs.h").write_text(HEADER)
        (work / "a.v").write_text(A_V)
        (work / "b.v").write_text("// b.v\nmodule other;\nendmodule\n")
        done = run(work, ["b.v", "a.v"])
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("defs.h:3: unsupported: real", done.stderr)


if __name__ == "__main__":
    unittest.main()
