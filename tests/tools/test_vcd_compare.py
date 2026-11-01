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

    def test_mismatch_time_in_the_reference_unit(self) -> None:
        ref = BASE.replace("1ns", "100 ps").replace("#5", "#6")
        proc = run_tool(BASE, ref)
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("t=6 x 100 ps (=600000 fs)", proc.stdout)

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
        self.assertIn("$timescale 1 ns $end", text)
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


def run_tb(tmp: str, body: str, vcd: bool) -> tuple[str, str]:
    """(stdout file text, vcd text) of `sim_tb` on a one-DUT testbench."""
    d, tb = Path(tmp) / "d.v", Path(tmp) / "t.v"
    d.write_text("module d(input [31:0] a, output y);\n  assign y = 1;\nendmodule\n")
    tb.write_text(body)
    out, wave = Path(tmp) / ("v.out" if vcd else "p.out"), Path(tmp) / "w.vcd"
    extra = f" --vcd {wave}" if vcd else ""
    script = f"read_verilog {d}; proc; opt; lower; sim_tb --out {out}{extra} {tb}"
    proc = subprocess.run([CLI, "-p", script], capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stderr
    return out.read_text(), wave.read_text() if vcd else ""


RANDOM_TB = """module t;
  reg [31:0] x; wire y;
  d u(.a($random), .y(y));
  initial begin x = $random; #1 $display("%d", x); #1 $display("%d", $random); end
endmodule
"""


@unittest.skipUnless(CLI, "ODIN3_CLI not set")
class SimTbVcdFixesTest(unittest.TestCase):
    def test_timescale_forms(self) -> None:
        want = {"1ns/1ps": "1 ns", "1 ns / 1 ps": "1 ns", "100 ps/10 ps": "100 ps",
                "10us / 1ns": "10 us"}
        with tempfile.TemporaryDirectory() as tmp:
            for form, unit in want.items():
                _, text = run_tb(tmp, f"`timescale {form}\n" + RANDOM_TB, True)
                self.assertIn(f"$timescale {unit} $end", text, form)

    def test_random_in_a_connection_is_not_sampled(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plain, _ = run_tb(tmp, RANDOM_TB, False)
            waved, _ = run_tb(tmp, RANDOM_TB, True)
        self.assertEqual(plain, waved)

    def test_final_timestamp(self) -> None:
        body = ("module t; reg [31:0] x; wire y; d u(.a(x), .y(y));\n"
                "  initial begin x = 0; #1 x = 1; #3 $display(\"e\"); end\nendmodule\n")
        with tempfile.TemporaryDirectory() as tmp:
            _, text = run_tb(tmp, body, True)
        self.assertEqual(text.splitlines()[-1], "#4")
