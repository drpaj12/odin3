"""sim_tb plusargs ($test$plusargs, $value$plusargs fed by --plusarg) and the lazy check of system
functions in generate branches (a dead branch with $value$plusargs or $fopen is no error, as in
Yosys; a live one is). Run through CTest `sim_tb_plusargs` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")

DUT = "module dut(input a, output y); assign y = ~a; endmodule\n"

TB = """
module tb;
  reg a; wire y;
  reg [31:0] n; reg [15:0] h; reg [63:0] s; reg [7:0] b;
  integer found_n, found_h, found_s, found_x, test_hit, test_miss;
  dut u(.a(a), .y(y));
  initial begin
    found_n = $value$plusargs("N=%d", n);
    found_h = $value$plusargs("H=%h", h);
    found_s = $value$plusargs("S=%s", s);
    found_x = $value$plusargs("MISSING=%d", b);
    test_hit = $test$plusargs("N");
    test_miss = $test$plusargs("nothere");
    $display("n %0d %0d", found_n, n);
    $display("h %0d %0h", found_h, h);
    $display("s %0d %0h", found_s, s);
    $display("x %0d %0d", found_x, b);
    $display("t %0d %0d", test_hit, test_miss);
    $finish;
  end
endmodule
"""


def run_script(plusargs: list[str], tb_text: str = TB) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as tmp:
        dut, tbf, out = Path(tmp) / "dut.v", Path(tmp) / "tb.v", Path(tmp) / "o.txt"
        dut.write_text(DUT)
        tbf.write_text(tb_text)
        args = " ".join(f"--plusarg {p}" for p in plusargs)
        script = f'read_verilog "{dut}"; proc; opt; lower; sim_tb {args} --out "{out}" "{tbf}"'
        done = subprocess.run(
            [str(ODIN3), "-p", script], capture_output=True, text=True, check=False
        )
        done.out_lines = out.read_text().splitlines() if out.exists() else []  # type: ignore[attr-defined]
        return done


def run_tb(plusargs: list[str], tb_text: str = TB) -> list[str]:
    done = run_script(plusargs, tb_text)
    assert done.returncode == 0, done.stderr
    return done.out_lines  # type: ignore[attr-defined,no-any-return]


def value_tb(conv: str, width: int = 64) -> str:
    """A testbench that prints found and value of $value$plusargs("V=%<conv>")."""
    return (
        f"module tb; reg a; wire y; reg [{width - 1}:0] v; integer f; dut u(.a(a), .y(y));\n"
        f'initial begin v = 7; f = $value$plusargs("V=%{conv}", v);\n'
        '$display("%0d %0d", f, v); $finish; end endmodule\n'
    )


GATED = """
module gated #(parameter [0:0] sim = 1'b0)(input a, output y);
  assign y = a;
  generate
    if (sim) begin : g
      integer f = 0;
      reg [63:0] name;
      initial if ($value$plusargs("signature=%s", name)) f = $fopen("sig.txt", "w");
    end
  endgenerate
endmodule
module top(input a, output y);
  gated #(.sim(SIMVAL)) u(.a(a), .y(y));
endmodule
"""


def read_gated(sim: int) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "g.v"
        src.write_text(GATED.replace("SIMVAL", f"1'b{sim}"))
        return subprocess.run(
            [str(ODIN3), "--top", "top", "-p", f'read_verilog "{src}"; proc; opt'],
            capture_output=True,
            text=True,
            check=False,
        )


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_plusargs)")
class PlusargsTest(unittest.TestCase):
    def test_values_and_tests(self) -> None:
        lines = run_tb(["N=42", "+H=beef", "S=abc"])
        self.assertEqual(
            lines, ["n 1 42", "h 1 beef", "s 1 616263", "x 0 0", "t 1 0"]
        )

    def test_no_plusargs_finds_nothing(self) -> None:
        lines = run_tb([])
        self.assertEqual(lines, ["n 0 0", "h 0 0", "s 0 0", "x 0 0", "t 0 0"])

    def test_bad_value_is_not_found(self) -> None:
        lines = run_tb(["N=4x2"])
        self.assertEqual(lines[0], "n 0 0")


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_plusargs)")
class PlusargParseTest(unittest.TestCase):
    def check(self, conv: str, arg: str, expect: str) -> None:
        self.assertEqual(run_tb([arg], value_tb(conv)), [expect], f"%{conv} {arg}")

    def test_accepted(self) -> None:
        self.check("d", "V=42", "1 42")
        self.check("h", "V=ff", "1 255")
        self.check("H", "V=FF", "1 255")
        self.check("b", "V=101", "1 5")
        self.check("B", "V=101", "1 5")
        self.check("o", "V=17", "1 15")
        self.check("O", "V=17", "1 15")
        self.check("x", "V=1f", "1 31")
        self.check("d", "V=18446744073709551615", "1 18446744073709551615")

    def test_negative(self) -> None:
        self.check("d", "V=-1", "1 18446744073709551615")
        self.check("d", "V=-0", "1 0")

    def test_rejected(self) -> None:
        for arg in ["V=-", "V=", "V=+5", "V=-+5", "V=0x1f"]:
            self.check("d" if arg != "V=0x1f" else "h", arg, "0 7")
        self.check("b", "V=0b101", "0 7")
        self.check("b", "V=102", "0 7")

    def test_overflow_is_not_wrapped(self) -> None:
        self.check("d", "V=18446744073709551616", "0 7")
        self.check("h", "V=10000000000000000", "0 7")
        self.check("d", "V=-18446744073709551616", "0 7")

    def test_non_numeric_conversions_fail(self) -> None:
        for conv in "cefmt":
            self.check(conv, "V=5", "0 7")

    def test_prefix_collision(self) -> None:
        tb = value_tb("d").replace('"V=%d"', '"N=%d"')
        self.assertEqual(run_tb(["NN=9", "N=3"], tb), ["1 3"])
        self.assertEqual(run_tb(["NN=9"], tb), ["0 7"])

    def test_too_many_plusargs(self) -> None:
        ok = run_script([f"A{i}=1" for i in range(16)], value_tb("d"))
        self.assertEqual(ok.returncode, 0, ok.stderr)
        bad = run_script([f"A{i}=1" for i in range(17)], value_tb("d"))
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("at most 16 --plusarg", bad.stderr)

    def test_overlength_plusarg(self) -> None:
        bad = run_script(["A=" + "1" * 300], value_tb("d"))
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("--plusarg needs a text of 1 to 255 bytes", bad.stderr)


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_plusargs)")
class GenerateLazyTest(unittest.TestCase):
    def test_dead_generate_branch_is_no_error(self) -> None:
        done = read_gated(0)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("is not supported", done.stderr)

    def test_live_generate_branch_is_an_error(self) -> None:
        done = read_gated(1)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("system function '$value$plusargs' is not supported", done.stderr)


if __name__ == "__main__":
    unittest.main()
