"""write_blif --reachable writes the top and the modules reachable from it, not modules nothing
instantiates (Parmys writes only the top's netlist; Yosys's write_blif and the default here write
every module). The skipped module stays in the design and a WARNING names it. Run through CTest `write_blif_reach`
(ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")

VERILOG = """
module leaf(input a, output y); assign y = ~a; endmodule
module mid(input a, output y); leaf u(.a(a), .y(y)); endmodule
module unused(input a, output y); assign y = a; endmodule
module top(input a, output y); mid u(.a(a), .y(y)); endmodule
"""


def run(script_body: str, flags: str = "") -> tuple[list[str], str]:
    with tempfile.TemporaryDirectory() as tmp:
        src, out = Path(tmp) / "d.v", Path(tmp) / "o.blif"
        src.write_text(VERILOG)
        script = f'read_verilog "{src}"; proc; opt; lower; {script_body}; write_blif {flags} "{out}"'
        done = subprocess.run(
            [str(ODIN3), "--top", "top", "-p", script], capture_output=True, text=True
        )
        assert done.returncode == 0, done.stderr
        return re.findall(r"^\.model (\S+)", out.read_text(), re.M), done.stderr


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest write_blif_reach)")
class WriteBlifReachTest(unittest.TestCase):
    def test_hierarchy_is_written_without_warning(self) -> None:
        models, log = run("opt", "--reachable")
        self.assertEqual(models, ["top", "mid", "leaf"])
        self.assertNotIn("not reachable", log)

    def test_default_writes_every_module(self) -> None:
        models, log = run("flatten")
        self.assertEqual(models, ["top", "mid", "leaf"])
        self.assertNotIn("not reachable", log)

    def test_flattened_modules_nothing_instantiates_are_not_written(self) -> None:
        models, log = run("flatten", "--reachable")
        self.assertEqual(models, ["top"])
        self.assertIn("module 'mid' is not reachable from the top", log)
        self.assertIn("module 'leaf' is not reachable from the top", log)


if __name__ == "__main__":
    unittest.main()
