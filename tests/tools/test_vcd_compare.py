"""vcd-compare: equal waves agree (across timescales, x skipped), a changed value or time is
reported; and `sim_tb --vcd` writes a header, scopes and the changes of a small testbench."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLI = os.environ.get("ODIN3_CLI", "")
TOOL = ROOT / "tools" / "vcd-compare" / "vcd-compare"
MICRO = ROOT / "tests" / "tb" / "micro"

BASE = """$timescale 1ns $end
$scope module tb $end
$var wire 1 ! clk $end
$var reg 4 " q $end
$upscope $end
$enddefinitions $end
#0
$dumpvars
0!
b0000 "
$end
#5
1!
b0001 "
"""


def run_tool(first: str, second: str) -> "subprocess.CompletedProcess[str]":
    with tempfile.TemporaryDirectory() as tmp:
        a, b = Path(tmp) / "a.vcd", Path(tmp) / "b.vcd"
        a.write_text(first)
        b.write_text(second)
        return subprocess.run([sys.executable, str(TOOL), str(a), str(b)],
                              capture_output=True, text=True, check=False)


class VcdCompareTest(unittest.TestCase):
    def test_identical(self) -> None:
        self.assertEqual(run_tool(BASE, BASE).returncode, 0)

    def test_timescale_and_trimmed_vectors(self) -> None:
        other = (BASE.replace("1ns", "1ps").replace("#5", "#5000").replace('b0000 "', 'b0 "')
                 .replace('b0001 "', 'b1 "').replace("#0", "#0"))
        self.assertEqual(run_tool(BASE, other).returncode, 0)

    def test_x_is_skipped(self) -> None:
        other = BASE.replace("0!\nb0000 \"", "x!\nbxxxx \"", 1)
        self.assertEqual(run_tool(BASE, other).returncode, 0)

    def test_value_mismatch(self) -> None:
        proc = run_tool(BASE, BASE.replace('b0001 "', 'b0010 "'))
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("tb.q at", proc.stdout)

    def test_time_mismatch(self) -> None:
        proc = run_tool(BASE, BASE.replace("#5", "#6"))
        self.assertEqual(proc.returncode, 1, proc.stdout)

    def test_missing_signal(self) -> None:
        proc = run_tool(BASE, BASE.replace('$var reg 4 " q $end\n', ""))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("missing", proc.stdout)

    def test_bad_file(self) -> None:
        self.assertEqual(run_tool("garbage", BASE).returncode, 2)


@unittest.skipUnless(CLI, "ODIN3_CLI not set")
class SimTbVcdTest(unittest.TestCase):
    def test_counter_vcd(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            vcd = Path(tmp) / "w.vcd"
            script = (f"read_verilog {MICRO / 'counter.v'}; proc; opt; lower; "
                      f"sim_tb --out {Path(tmp) / 'o.txt'} --vcd {vcd} {MICRO / 'counter_tb.v'}")
            proc = subprocess.run([CLI, "-p", script], capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            text = vcd.read_text()
        self.assertIn("$timescale 1ns $end", text)
        self.assertIn("$scope module counter_tb $end", text)
        self.assertIn("$scope module dut $end", text)
        self.assertIn("$enddefinitions $end", text)
        self.assertIn("#0\n$dumpvars\n", text)
        self.assertIn("#5\n", text)

    def test_vcd_unwritable(self) -> None:
        script = f"sim_tb --vcd /nonexistent-dir/w.vcd {MICRO / 'misc_tb.v'}"
        proc = subprocess.run([CLI, "-p", script], capture_output=True, text=True, check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cannot write the --vcd file", proc.stderr)

    def test_vcd_needs_a_file(self) -> None:
        script = f"sim_tb {MICRO / 'misc_tb.v'} --vcd"
        proc = subprocess.run([CLI, "-p", script], capture_output=True, text=True, check=False)
        self.assertNotEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
