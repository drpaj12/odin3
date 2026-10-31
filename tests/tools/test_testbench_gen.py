"""testbench_gen: the name-based clock fallback takes clk, clock, ck, *_clk, *_clock, not clk_en."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

CLI = os.environ.get("ODIN3_CLI", "")

COMB = "module m(input {clk}, input a, output y);\n  assign y = ~a;\nendmodule\n"


@unittest.skipUnless(CLI, "ODIN3_CLI not set")
class ClockFallbackTest(unittest.TestCase):
    def generate(self, clk: str) -> tuple[str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.v"
            src.write_text(COMB.format(clk=clk))
            out = Path(tmp) / "tb.v"
            script = f"read_verilog {src}; proc; opt; lower; testbench_gen --out {out} --cycles 4"
            proc = subprocess.run([CLI, "-p", script], capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return out.read_text(), proc.stdout + proc.stderr

    def test_clk_en_is_data(self) -> None:
        text, log = self.generate("clk_en")
        self.assertIn("clk_en = vec_", text)
        self.assertNotIn("as the clock", log)

    def test_suffix_is_clock(self) -> None:
        text, log = self.generate("sys_clk")
        self.assertNotIn("sys_clk = vec_", text)
        self.assertIn("taking `sys_clk` as the clock", log)


if __name__ == "__main__":
    unittest.main()
