"""Tests for tools/elab-check/gen-const-table (2D Task 7): the Yosys 0.55 constant table.

A fake `yosys` stands in for the real one: `-V` prints a canned version, and a run reads `b.v`
and writes `b.il` whose `connect \\y<k>` lines (in reverse order, to check that results are
matched by name, not by position) give each output the bits of its first operand when that is a
sized literal of the output's width, all x (`<w>'x`) for every fifth output, else zeros. Each
run is logged, so the batching is visible.
"""

from __future__ import annotations

import re
import stat
import tempfile
import textwrap
import unittest
from collections import Counter
from pathlib import Path

import gen_const_table as gct

from . import helpers

FAKE_YOSYS = r"""#!/usr/bin/env python3
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if sys.argv[1:] == ["-V"]:
    print("@VERSION@")
    sys.exit(0)
with open(HERE / "runs.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
text = Path("b.v").read_text()
widths = {k: int(top) + 1 for top, k in re.findall(r"output \[(\d+):0\] y(\d+)", text)}
lines = []
for k, rhs in re.findall(r"assign y(\d+) = (.*);", text):
    width = widths[k]
    first = re.match(r"[^0-9']*(\d+)'s?b([01xz]+)", rhs)
    if int(k) % 5 == 0:
        bits = str(width) + "'x"
    elif first is not None and int(first.group(1)) == width:
        bits = str(width) + "'" + first.group(2)
    else:
        bits = str(width) + "'" + "0" * width
    lines.append("  connect \\y" + k + " " + bits)
Path("b.il").write_text("module \\b\n" + "\n".join(reversed(lines)) + "\nend\n")
"""

VERSION_055 = "Yosys 0.55 (git sha1 60f126cd0, fake)"
VERSION_069 = "Yosys 0.69 (git sha1 0123abcd, fake)"


class GenConstTableTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _fake(self, version: str) -> Path:
        path = self.root / "yosys"
        path.write_text(FAKE_YOSYS.replace("@VERSION@", version))
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _run(self, version: str, *extra: str) -> tuple[int, str, Path]:
        out = self.root / "table.txt"
        argv = ["--yosys", str(self._fake(version)), "--out", str(out), *extra]
        code, _, err = helpers.run_main(gct.main, argv)
        return code, err, out

    def test_parses_connect_lines(self) -> None:
        rtlil = textwrap.dedent("""\
            module \\b
              wire width 8 output 4 \\y3
              connect \\y3 8'1x0z0000
              connect \\y0 4'x
              connect \\y1 3's101
              connect \\y2 5
              connect \\y4 { 2'01 3'1z0 }
            end
            """)
        got = gct.parse_connects(rtlil)
        self.assertEqual(got[3], "1x0z0000")
        self.assertEqual(got[0], "xxxx")
        self.assertEqual(got[1], "101")
        self.assertEqual(got[2], format(5, "032b"))
        self.assertEqual(got[4], "011z0")

    def test_rejects_a_connect_that_is_not_a_constant(self) -> None:
        with self.assertRaises(gct.GenError):
            gct.parse_connects("  connect \\y0 \\a\n")

    def test_deterministic_per_seed(self) -> None:
        self.assertEqual(gct.make_cases(7, 500), gct.make_cases(7, 500))
        self.assertNotEqual(gct.make_cases(7, 500), gct.make_cases(8, 500))

    def test_mix_covers_every_op_width_and_sign(self) -> None:
        cases = gct.make_cases(2026, len(gct.OPS) * len(gct.WIDTH_CYCLE) * 2 * 2)
        self.assertEqual({c.op for c in cases}, set(gct.OPS))
        self.assertEqual({c.width for c in cases}, set(gct.WIDTHS))
        self.assertEqual({c.signed for c in cases}, {False, True})
        self.assertEqual(set(gct.WIDTHS), {1, 7, 64, 65, 200})
        self.assertEqual(set(gct.WIDTH_CYCLE), set(gct.WIDTHS))
        self.assertGreaterEqual(gct.DEFAULT_COUNT, gct.MIN_COUNT)
        operands = [o for c in cases for o in (c.a, c.b) if o is not None]
        with_xz = sum(1 for o in operands if re.search("[xz]", o))
        self.assertGreater(with_xz / len(operands), 0.25)
        self.assertLess(with_xz / len(operands), 0.42)
        for case in cases:
            self.assertEqual(len(case.a), case.width)
            self.assertEqual(case.b is None, gct.OPS[case.op].arity == 1)
            if case.b is not None:
                self.assertEqual(len(case.b), case.width)

    def test_operands_are_sized_literals_at_the_case_width_and_sign(self) -> None:
        case = gct.Case("ADD", 7, True, "01x0101", "1100110")
        self.assertEqual(gct.expression(case), "7'sb01x0101 + 7'sb1100110")
        case = gct.Case("RNAND", 4, False, "1z01", None)
        self.assertEqual(gct.expression(case), "~&4'b1z01")
        text = gct.module_text([case, gct.Case("POW", 1, False, "1", "0")])
        self.assertIn("output [3:0] y0", text)
        self.assertIn("assign y1 = 1'b1 ** 1'b0;", text)

    def test_writes_header_and_keeps_batch_order(self) -> None:
        count = 2 * gct.BATCH + 88
        code, err, out = self._run(VERSION_055, "--seed", "5", "--count", str(count))
        self.assertEqual(code, 0, err)
        lines = out.read_text().splitlines()
        self.assertEqual(lines[0], f"# gen-const-table seed 5 count {count}")
        self.assertEqual(lines[1], f"# yosys: {VERSION_055}")
        body = [line for line in lines if not line.startswith("#")]
        self.assertEqual(len(body), count)
        runs = (self.root / "runs.log").read_text().splitlines()
        self.assertEqual(len(runs), 3)
        self.assertEqual(runs[0], "-q -p read_verilog -sv b.v; write_rtlil b.il")
        for i, (case, line) in enumerate(zip(gct.make_cases(5, count), body, strict=True)):
            fields = line.split()
            self.assertEqual(
                fields[:5],
                [case.op, str(case.width), "1" if case.signed else "0", case.a, case.b or "-"],
            )
            want = "x" * case.width if (i % gct.BATCH) % 5 == 0 else case.a
            self.assertEqual(fields[5], want, line)

    def test_mixed_operands_and_context(self) -> None:
        signed_one = gct.Operand("1" + "0" * 31, True, True)
        self.assertEqual(signed_one.bits, "0" + signed_one.digits)  # const2ast's extra 0
        self.assertEqual(signed_one.flags, "S")
        self.assertEqual(signed_one.literal(), "'sb1" + "0" * 31)
        self.assertEqual(gct.Operand("1" * 32, False, True).bits, "1" * 32)
        self.assertEqual(gct.Operand("101", False, False).flags, "u")
        sa = gct.Operand("1000", True, False)
        ub = gct.Operand("1", False, False)
        shift = gct.MixedCase("SSHR", 4, False, sa, ub)
        self.assertFalse(shift.signed)  # the unsigned wrapper makes the context unsigned
        self.assertTrue(gct.MixedCase("SSHR", 4, True, sa, ub).signed)
        self.assertFalse(gct.MixedCase("ADD", 4, True, sa, ub).signed)
        self.assertFalse(gct.MixedCase("LT", 4, True, sa, sa).signed)
        self.assertEqual(gct.mixed_expression(shift), "1'b1 ? (4'sb1000 >>> 1'b1) : 1'b0")
        self.assertEqual(gct.MixedCase("ADD", 1, True, sa, signed_one).self_width, 33)
        self.assertEqual(gct.MixedCase("SHL", 1, True, sa, signed_one).self_width, 4)

    def test_mixed_mix(self) -> None:
        cases = gct.make_mixed_cases(2026, gct.DEFAULT_MIXED_COUNT)
        self.assertEqual(cases, gct.make_mixed_cases(2026, gct.DEFAULT_MIXED_COUNT))
        self.assertEqual({c.op for c in cases}, set(gct.OPS))
        binary = [c for c in cases if c.b is not None]
        self.assertTrue(any(len(c.a.bits) != len(c.b.bits) for c in binary if c.b))
        self.assertTrue(any(c.a.signed != c.b.signed for c in binary if c.b))
        self.assertTrue(any(c.a.unsized for c in cases))
        self.assertTrue(any(c.op in gct.A_SIGN and c.a.signed and not c.signed for c in cases))
        tight_sshr = [
            c
            for c in cases
            if c.op == "SSHR" and c.a.signed and not c.signed and c.width == len(c.a.bits)
        ]
        self.assertTrue(tight_sshr)
        for case in cases:
            self.assertGreaterEqual(case.width, case.self_width)
            self.assertEqual(case.b is None, gct.OPS[case.op].arity == 1)

    def test_mixed_table_lines(self) -> None:
        code, err, out = self._run(VERSION_055, "--mixed", "--count", "300")
        self.assertEqual(code, 0, err)
        lines = out.read_text().splitlines()
        self.assertEqual(lines[0], "# gen-const-table mixed seed 2026 count 300")
        body = [line for line in lines if not line.startswith("#")]
        for case, line in zip(gct.make_mixed_cases(2026, 300), body, strict=True):
            fields = line.split()
            self.assertEqual(len(fields), 8, line)
            want = [
                case.op,
                str(case.width),
                "1" if case.signed else "0",
                case.a.bits,
                case.a.flags,
            ]
            self.assertEqual(fields[:5], want)
            self.assertEqual(len(fields[7]), case.width)

    def test_refuses_a_yosys_that_is_not_0_55(self) -> None:
        code, err, out = self._run(VERSION_069, "--count", "10")
        self.assertEqual(code, 2)
        self.assertIn("0.55", err)
        self.assertFalse(out.exists())

    def test_cop_order_matches_the_c_enum(self) -> None:
        names = list(gct.OPS)
        self.assertEqual(names[:6], ["ADD", "SUB", "MUL", "DIV", "MOD", "POW"])
        self.assertEqual(names[-6:], ["RAND", "RNAND", "ROR", "RNOR", "RXOR", "RXNOR"])
        self.assertEqual(len(names), 34)
        self.assertEqual(Counter(op.arity for op in gct.OPS.values()), Counter({2: 24, 1: 10}))


if __name__ == "__main__":
    unittest.main()
