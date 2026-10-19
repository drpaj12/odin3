"""Tests for tools/lpm-check/lpm_check.py (5.1 plan Task 9, spec T-1..T-4): the parsing of the
written Verilog, the counts, the round-trip rules (a fixpoint; open pins equal x pins and fresh
nets), the lint with a planted width mismatch, and the exit codes, with a fake ``odin3``.

Work directories are made by ``tempfile.TemporaryDirectory`` and removed by it.
"""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import lpm_check as lc

from .helpers import run_main

STUB = """
(* blackbox *)
module lpm_add_sub #(
  parameter lpm_type = "lpm_add_sub",
  parameter lpm_width = 1,
  parameter lpm_direction = "UNUSED",
  parameter lpm_representation = "SIGNED",
  parameter lpm_pipeline = 0,
  parameter lpm_hint = "UNUSED"
) (
  output [lpm_width-1:0] result,
  output cout,
  output overflow,
  input add_sub,
  input cin,
  input [lpm_width-1:0] dataa,
  input [lpm_width-1:0] datab,
  input clock,
  input clken,
  input aclr
);
endmodule
"""

GEN1 = (
    """module m (
  input [7:0] a,  // t.v:2
  input [7:0] b,
  output [8:0] s
);
  (* lpmmap = "lpm_add_sub:add" *)
  lpm_add_sub #(.lpm_width(9), .lpm_direction("ADD"), .lpm_representation("UNSIGNED")) \\$c15 (
    .result(s),
    .cout(),
    .overflow(),
    .add_sub(),
    .cin(1'b0),
    .dataa({1'b0, a}),
    .datab({1'b0, b}),
    .clock(1'b0),
    .clken(1'b1),
    .aclr(1'b0)
  );
endmodule
"""
    + STUB
)

GEN2 = (
    GEN1.replace(".cout()", ".cout(\\$n61 )")
    .replace(".cin()", ".cin(1'bx)")
    .replace("t.v:4", "gen1.v:9")
)

FAKE_ODIN = r"""#!/usr/bin/env python3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "odin.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
out = script.split("write_verilog ")[1].split(";")[0].strip()
name = "gen1" if "lpmmap" in script else "gen2"
Path(out).write_text((HERE / (name + ".v")).read_text())
sys.stderr.write((HERE / "info.txt").read_text())
"""


class Parsing(unittest.TestCase):
    def test_strip_comments(self) -> None:
        self.assertEqual(lc.strip_comments("a;  // t.v:2\nb;\n"), "a;\nb;\n")

    def test_instances_and_pins(self) -> None:
        (inst,) = lc.instances(GEN1)
        kind, params, name, pins = inst
        self.assertEqual(kind, "lpm_add_sub")
        self.assertEqual(params["lpm_width"], "9")
        self.assertEqual(params["lpm_direction"], '"ADD"')
        self.assertEqual(name, "\\$c15")
        self.assertEqual(
            sorted(pins), ["aclr", "cin", "clken", "clock", "dataa", "datab", "result"]
        )

    def test_open_pins_equal_x_and_fresh_nets(self) -> None:
        self.assertEqual(lc.instances(GEN1), lc.instances(GEN2))

    def test_defaults_must_be_tied(self) -> None:
        lc.check_defaults(GEN1, "gen1")
        lc.check_defaults(GEN2, "gen2")  # add_sub is exempt
        for pin, tied in (("cin", "1'b0"), ("clken", "1'b1"), ("aclr", "1'b0"), ("clock", "1'b0")):
            opened = GEN1.replace(f".{pin}({tied})", f".{pin}(1'bx)")
            self.assertNotEqual(opened, GEN1)
            with self.assertRaises(lc.CheckError, msg=pin):
                lc.check_defaults(opened, "gen1")
        blank = GEN1.replace(".cin(1'b0)", ".cin()")
        with self.assertRaises(lc.CheckError):
            lc.check_defaults(blank, "gen1")

    def test_a_real_difference_is_seen(self) -> None:
        changed = GEN2.replace(".dataa({1'b0, a})", ".dataa({1'b1, a})")
        self.assertNotEqual(lc.instances(GEN1), lc.instances(changed))

    def test_check_counts(self) -> None:
        info = (
            "odin3: info: lpmmap: 1 lpm_add_sub, 0 lpm_mult, 0 lpm_compare, 0 altsyncram; "
            "0 cells left soft\n"
        )
        lc.check_counts(GEN1, info, [1, 0, 0, 0, 0], [("lpm_add_sub", "lpm_width", "9")])
        with self.assertRaises(lc.CheckError):
            lc.check_counts(GEN1, info, [2, 0, 0, 0, 0], [])
        with self.assertRaises(lc.CheckError):
            lc.check_counts(GEN1, info, None, [("lpm_add_sub", "lpm_width", "7")])
        with self.assertRaises(lc.CheckError):
            lc.check_counts(GEN1, "no info line\n", None, [])

    def test_read_expect(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "expect.txt"
            path.write_text(
                "# c\ncounts a.v 1 2 3 4 5\nparam a.v lpm_mult lpm_widtha 8\ncounts b.v 0 0 0 0 0\n"
                "noroundtrip b.v\nnolint b.v\npin b.v lpm_compare alb lt\n"
            )
            other = lc.read_expect(path, "b.v")
            self.assertFalse(other.roundtrip or other.lint)
            self.assertEqual(other.pins, [("lpm_compare", "alb", "lt")])
            exp = lc.read_expect(path, "a.v")
            self.assertEqual(exp.counts, [1, 2, 3, 4, 5])
            self.assertEqual(exp.params, [("lpm_mult", "lpm_widtha", "8")])
            self.assertTrue(exp.roundtrip and exp.lint)


@unittest.skipUnless(shutil.which("iverilog"), "iverilog is not installed")
class Lint(unittest.TestCase):
    def test_clean_file_and_planted_width_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "good.v"
            good.write_text(GEN1)
            lc.check_lint(good)
            bad = Path(tmp) / "bad.v"
            bad.write_text(GEN1.replace(".lpm_width(9)", ".lpm_width(10)"))
            with self.assertRaises(lc.CheckError):
                lc.check_lint(bad, use_verilator=False)
            code, _, err = run_main(lc.main, ["--lint-only", str(bad)])
            self.assertEqual(code, 1, err)
            code, out, err = run_main(lc.main, ["--lint-only", str(good)])
            self.assertEqual(code, 0, err)


class Driver(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        (self.dir / "gen1.v").write_text(GEN1)
        (self.dir / "gen2.v").write_text(GEN2)
        (self.dir / "info.txt").write_text(
            "odin3: info: lpmmap: 1 lpm_add_sub, 0 lpm_mult, 0 lpm_compare, 0 altsyncram; "
            "0 cells left soft\n"
        )
        fake = self.dir / "odin3"
        fake.write_text(FAKE_ODIN)
        fake.chmod(0o755)
        self.expect = self.dir / "expect.txt"
        self.expect.write_text("counts micro.v 1 0 0 0 0\nparam micro.v lpm_add_sub lpm_width 9\n")
        self.micro = self.dir / "micro.v"
        self.micro.write_text("module m(); endmodule\n")

    def args(self, *extra: str) -> list[str]:
        return [
            "--odin3",
            str(self.dir / "odin3"),
            "--expect",
            str(self.expect),
            "--lib",
            "lib/lpm.o3lib",
            *extra,
            str(self.micro),
        ]

    @unittest.skipUnless(shutil.which("iverilog"), "iverilog is not installed")
    def test_pass_exit_0(self) -> None:
        code, out, err = run_main(lc.main, self.args())
        self.assertEqual(code, 0, err)
        self.assertIn("micro.v: ok", out)
        log = (self.dir / "odin.log").read_text().splitlines()
        self.assertEqual(len(log), 3)  # the script, the read-back, the second read-back
        self.assertIn("lpmmap", log[0])
        self.assertTrue(all(line.startswith("--check ") for line in log[1:]))  # IR invariants

    @unittest.skipUnless(shutil.which("iverilog"), "iverilog is not installed")
    def test_count_mismatch_exit_1(self) -> None:
        self.expect.write_text("counts micro.v 2 0 0 0 0\n")
        code, _, err = run_main(lc.main, self.args())
        self.assertEqual(code, 1)
        self.assertIn("counts:", err)

    @unittest.skipUnless(shutil.which("iverilog"), "iverilog is not installed")
    def test_roundtrip_difference_exit_1(self) -> None:
        (self.dir / "gen2.v").write_text(GEN2.replace(".lpm_width(9)", ".lpm_width(8)"))
        code, _, err = run_main(lc.main, self.args())
        self.assertEqual(code, 1)
        self.assertIn("roundtrip:", err)

    def test_missing_file_exit_2(self) -> None:
        if shutil.which("iverilog") is None:
            self.skipTest("iverilog is not installed")
        code, _, err = run_main(lc.main, ["--odin3", "odin3", str(self.dir / "nope.v")])
        self.assertEqual(code, 2)
        self.assertIn("no such file", err)

    def test_no_iverilog_exit_77(self) -> None:
        with mock.patch.dict(os.environ, {"PATH": str(self.dir)}):
            code, _, err = run_main(lc.main, self.args())
        self.assertEqual(code, 77)
        self.assertIn("iverilog", err)


if __name__ == "__main__":
    unittest.main()
