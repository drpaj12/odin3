"""Tests for tools/opt-check/gen-fold-table (2F Task 5): the Yosys 0.55 fold table.

A fake `yosys` stands in for the real one: `-V` prints a canned version, and a run reads `b.il`
and writes `o.il` whose `connect \\y<k>` lines (in reverse order, to check that results are matched
by name, not by position) give every fifth output all x (`<w>'x`) and every other zeros. Each
run is logged, so the batching is visible.
"""

from __future__ import annotations

import re
import stat
import tempfile
import textwrap
import unittest
from pathlib import Path

import gen_fold_table as gft

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
text = Path("b.il").read_text()
lines = []
widths = {k: int(w) for w, k in re.findall(r"wire width (\d+) output \d+ \\y(\d+)", text)}
for k in sorted(widths, key=int):
    width = widths[k]
    if int(k) % 5 == 0:
        bits = str(width) + "'x"
    else:
        bits = str(width) + "'" + "0" * width
    lines.append("  connect \\y" + k + " " + bits)
Path("o.il").write_text("module \\b\n" + "\n".join(reversed(lines)) + "\nend\n")
"""

VERSION_055 = "Yosys 0.55 (git sha1 60f126cd0, fake)"
VERSION_069 = "Yosys 0.69 (git sha1 0123abcd, fake)"


class GenFoldTableTest(unittest.TestCase):
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
        code, _, err = helpers.run_main(gft.main, argv)
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
        got = gft.parse_connects(rtlil)
        self.assertEqual(got[3], "1x0z0000")
        self.assertEqual(got[0], "xxxx")
        self.assertEqual(got[1], "101")
        self.assertEqual(got[2], format(5, "032b"))
        self.assertEqual(got[4], "011z0")

    def test_resolves_a_temporary_wire(self) -> None:
        rtlil = textwrap.dedent("""\
            module \\b
              wire $auto$rtlil.cc:2957:Not$2
              wire width 65 output 1 \\y0
              connect \\y0 { 64'%s $auto$rtlil.cc:2957:Not$2 }
              connect $auto$rtlil.cc:2957:Not$2 1'1
            end
            """) % ("0" * 64)
        self.assertEqual(gft.parse_connects(rtlil)[0], "0" * 64 + "1")

    def test_rejects_a_connect_that_is_not_a_constant(self) -> None:
        with self.assertRaises(gft.GenError):
            gft.parse_connects("  connect \\y0 \\a\n")

    def test_deterministic_per_seed(self) -> None:
        self.assertEqual(gft.make_cases(7, 500), gft.make_cases(7, 500))
        self.assertNotEqual(gft.make_cases(7, 500), gft.make_cases(8, 500))

    def test_mix_covers_every_type_width_and_sign(self) -> None:
        cases = gft.make_cases(2026, len(gft.TYPES) * 40)
        self.assertEqual({c.type for c in cases}, set(gft.TYPES))
        self.assertEqual(len(gft.TYPES), 32)
        self.assertEqual({c.y_width for c in cases}, set(gft.WIDTHS))
        self.assertEqual({c.a_signed for c in cases}, {False, True})
        self.assertEqual({c.b_signed for c in cases}, {False, True})
        self.assertGreaterEqual(gft.DEFAULT_COUNT, gft.MIN_COUNT)
        operands = [o for c in cases for o in (c.a, c.b, c.s) if o is not None]
        with_xz = sum(1 for o in operands if re.search("[xz]", o))
        self.assertGreater(with_xz / len(operands), 0.2)
        self.assertLess(with_xz / len(operands), 0.5)
        for case in cases:
            if case.type == "$pmux":
                assert case.b is not None and case.s is not None
                self.assertEqual(len(case.b), len(case.s) * case.y_width)
            if case.type in gft.UNARY:
                self.assertIsNone(case.b)
                if case.type.startswith("$reduce_") and len(case.a) == 1:
                    self.assertNotIn("z", case.a)

    def test_cell_text(self) -> None:
        case = gft.Case("$add", True, False, 7, "01x0101", "1100", None)
        text = gft.cell_text(3, case)
        self.assertIn("cell $add \\c3", text)
        self.assertIn("parameter \\A_SIGNED 1", text)
        self.assertIn("parameter \\B_SIGNED 0", text)
        self.assertIn("parameter \\B_WIDTH 4", text)
        self.assertIn("connect \\A 7'01x0101", text)
        self.assertIn("connect \\Y \\y3", text)
        pmux = gft.Case("$pmux", False, False, 2, "01", "100111", "0z1")
        text = gft.cell_text(0, pmux)
        self.assertIn("parameter \\S_WIDTH 3", text)
        self.assertIn("connect \\B 6'100111", text)
        self.assertIn("connect \\S 3'0z1", text)

    def test_writes_header_and_keeps_batch_order(self) -> None:
        count = 2 * gft.BATCH + 88
        code, err, out = self._run(VERSION_055, "--seed", "5", "--count", str(count))
        self.assertEqual(code, 0, err)
        lines = out.read_text().splitlines()
        self.assertEqual(lines[0], f"# gen-fold-table seed 5 count {count}")
        self.assertEqual(lines[1], f"# yosys: {VERSION_055}")
        body = [line for line in lines if not line.startswith("#")]
        cases = gft.make_cases(5, count)
        self.assertEqual(len(body), count + sum(1 for c in cases if gft.is_mixed(c)))
        picked: list[str] = []
        pos = 0
        for case in cases:
            picked.append(body[pos])
            pos += 1
            if gft.is_mixed(case):  # its twin follows: flags clear, the same value
                fields = body[pos].split()
                self.assertEqual(fields[1:3], ["0", "0"])
                self.assertEqual(fields[3:], body[pos - 1].split()[3:])
                pos += 1
        self.assertEqual(pos, len(body))
        body = picked
        runs = (self.root / "runs.log").read_text().splitlines()
        self.assertEqual(len(runs), 3)
        self.assertEqual(runs[0], "-q -p read_rtlil b.il; opt_expr; write_rtlil o.il")
        for i, (case, line) in enumerate(zip(gft.make_cases(5, count), body, strict=True)):
            fields = line.split()
            self.assertEqual(
                fields[:4],
                [case.type, str(int(case.a_signed)), str(int(case.b_signed)), str(case.y_width)],
            )
            self.assertEqual(fields[4:7], [case.a, case.b or "-", case.s or "-"])
            want = "x" * case.y_width if (i % gft.BATCH) % 5 == 0 else "0" * case.y_width
            self.assertEqual(fields[7], want, line)

    def test_mixed_line_is_followed_by_its_twin(self) -> None:
        mixed = gft.Case("$add", True, False, 3, "011", "101", None)
        clear = gft.Case("$add", False, False, 3, "011", "101", None)
        shift = gft.Case("$sshr", True, False, 3, "011", "1", None)
        text = gft.table_text([], [mixed, shift, clear], ["110", "011", "110"])
        self.assertEqual(
            text.splitlines(),
            [
                "$add 1 0 3 011 101 - 110",
                "$add 0 0 3 011 101 - 110",
                "$sshr 1 0 3 011 1 - 011",
                "$add 0 0 3 011 101 - 110",
            ],
        )
        self.assertTrue(gft.is_mixed(mixed))
        self.assertFalse(gft.is_mixed(shift))
        self.assertFalse(gft.is_mixed(clear))

    def test_same_seed_same_table(self) -> None:
        _, _, first = self._run(VERSION_055, "--seed", "9", "--count", "300")
        text = first.read_text()
        _, _, second = self._run(VERSION_055, "--seed", "9", "--count", "300")
        self.assertEqual(second.read_text(), text)

    def test_refuses_another_yosys(self) -> None:
        code, err, out = self._run(VERSION_069, "--count", "10")
        self.assertEqual(code, 2)
        self.assertIn("needs Yosys 0.55", err)
        self.assertFalse(out.exists())

    def test_usage_error(self) -> None:
        code, _, _ = self._run(VERSION_055, "--count", "0")
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
