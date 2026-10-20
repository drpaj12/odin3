"""Tests for tools/flatten-check (3A Task 4, spec 14.3, 14.4): the RTLIL comparison, the expect
file, and the simulator harness with a fake odin3."""

from __future__ import annotations

import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import flatten_common as fc
import flatten_diff as fd
import flatten_sim as fs
import memcap
import opt_equiv

A = """module \\t
  wire width 4 input 1 \\x
  wire width 4 \\u1.a
  wire $tmp
  cell $and \\u1.add
  cell $xor $c1
end
"""
B_SAME = A.replace("$c1", "$flatten\\u1.$xor$1")


def top(text: str) -> fc.RtlilModule:
    return fc.parse_rtlil(text)["t"]


class CompareTests(unittest.TestCase):
    def test_parse(self) -> None:
        mod = top(A)
        self.assertEqual(mod.wires["x"], 4)
        self.assertEqual(mod.wires["$tmp"], 1)
        self.assertEqual(mod.cells, [("$and", "u1.add"), ("$xor", "$c1")])

    def test_base_type_names_the_module(self) -> None:
        paramod = "$paramod\\m1_0\\W=s32'00000000000000000000000000000100"
        self.assertEqual(fc.base_type(paramod), "m1_0")
        self.assertEqual(fc.base_type("$paramod$0c55\\single_port_ram"), "single_port_ram")
        self.assertEqual(fc.base_type("m1_0$W=2"), "m1_0")
        self.assertEqual(fc.base_type("$and"), "$and")
        self.assertEqual(fc.base_type("memory_controller"), "memory_controller")

    def test_same(self) -> None:
        self.assertEqual(fd.compare(top(A), top(B_SAME)), [])

    def test_missing_public_wire(self) -> None:
        other = A.replace("  wire width 4 \\u1.a\n", "")
        problems = fd.compare(top(A), top(other))
        self.assertEqual(len(problems), 1)
        self.assertIn("wire u1.a", problems[0])

    def test_different_cell_type(self) -> None:
        other = A.replace("cell $xor", "cell $or")
        problems = fd.compare(top(A), top(other))
        self.assertTrue(any("cell type $xor" in line for line in problems))
        self.assertTrue(any("cell type $or" in line for line in problems))

    def test_expect_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "expect.txt"
            path.write_text("# comment\na.v: private names differ\n")
            self.assertEqual(fd.load_expect(path), {"a.v": fd.Expected((), "private names differ")})
        self.assertEqual(fd.load_expect(None), {})

    def test_expect_kinds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "expect.txt"
            path.write_text("m.v [cell-type, memid]: why: not this\\n".replace("\\n", "\n"))
            entry = fd.load_expect(path)["m.v"]
        self.assertEqual(entry.kinds, ("cell-type", "memid"))
        self.assertEqual(entry.reason, "why: not this")
        self.assertTrue(entry.covers("cell type $dff: odin3 2 yosys 8"))
        self.assertFalse(entry.covers("wire x: odin3 1 yosys None"))
        self.assertFalse(entry.covers("equiv: differs: x"))

    def test_judge_kinds(self) -> None:
        entry = fd.Expected(("equiv",), "r")
        self.assertEqual(fd.judge(["equiv: differs: x"], entry), ([], False))
        wire = "wire a: odin3 1 yosys 2"
        self.assertEqual(fd.judge([wire], entry), ([wire], True))
        self.assertEqual(fd.judge([], entry), ([], True))
        self.assertEqual(fd.judge(["wire a"], None), (["wire a"], False))

    def test_memids_compared_as_sets(self) -> None:
        text = 'module \\t\n  cell $mem \\m\n    parameter \\MEMID "\\\\u1.mem"\n  end\nend\n'
        a = fc.parse_rtlil(text)["t"]
        self.assertEqual(a.memids, ["u1.mem"])
        b = fc.RtlilModule(memids=["u1.mem", "u1.mem"])
        c = fc.RtlilModule(memids=["u2.mem"])
        self.assertEqual([p for p in fd.compare(a, b) if p.startswith("memid")], [])
        self.assertTrue(any(p.startswith("memid") for p in fd.compare(a, c)))

    def _equiv(self, result: object) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.il"
            path.write_text("module \\t\nend\n")
            side = (
                mock.patch.object(opt_equiv.Checker, "refines", side_effect=result)
                if isinstance(result, BaseException)
                else mock.patch.object(opt_equiv.Checker, "refines", return_value=result)
            )
            with side:
                return fd.equivalence(path, path, "t", "yosys", Path(tmp))

    def test_equivalence_classes(self) -> None:
        self.assertEqual(self._equiv((True, "")), [])
        differs = self._equiv((False, "a\nERROR: Found 2 unproven"))
        self.assertTrue(differs[0].startswith("equiv: differs"))
        timeout = self._equiv(opt_equiv.CrashError("yosys-refine", "slow", True))
        self.assertEqual(timeout, ["equiv: unproven (timeout)"])
        abort = self._equiv(opt_equiv.RefineAbort("yosys-refine exit 1: boom"))
        self.assertTrue(abort[0].startswith("equiv: unproven"))
        with self.assertRaises(opt_equiv.CrashError):
            self._equiv(opt_equiv.CrashError("yosys-refine", "killed"))

    def test_memory_files_use_the_memory_lowering(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            plain, mem = Path(tmp) / "p.il", Path(tmp) / "m.il"
            plain.write_text("module \\t\n  cell $and \\a\nend\n")
            mem.write_text("module \\t\n  cell $mem_v2 \\a\nend\n")
            self.assertFalse(fd.has_memories(plain))
            self.assertTrue(fd.has_memories(plain, mem))
        checker = fd.MemoryChecker("yosys", Path("."), 1)
        self.assertEqual(len(checker.flows(True)), 1)
        self.assertIn("memory", checker.flows(True)[0])

    def _main(self, results: dict[str, list[str] | str], expect: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            exp = Path(tmp) / "expect.txt"
            exp.write_text(expect)
            files = [str(Path(tmp) / name) for name in results]
            with (
                mock.patch.object(fd, "run_one", side_effect=list(results.values())),
                mock.patch("builtins.print") as printed,
            ):
                code = fd.main(["--expect", str(exp), *files])
            return code, "\n".join(str(c.args[0]) for c in printed.call_args_list)

    def test_exit_codes(self) -> None:
        self.assertEqual(self._main({"a.v": []}, "")[0], 0)
        code, text = self._main({"a.v": ["wire x"]}, "")
        self.assertEqual(code, 1)
        self.assertIn("DIFFERS", text)
        self.assertEqual(self._main({"a.v": ["wire x"]}, "a.v: reason")[0], 0)
        code, text = self._main({"a.v": []}, "a.v: reason")  # stale entry
        self.assertEqual(code, 1)
        self.assertIn("stale", text)
        self.assertEqual(self._main({"a.v": "boom"}, "")[0], 2)

    def test_skip_without_cap(self) -> None:
        with (
            mock.patch.object(fd, "run_one", side_effect=memcap.CapUnavailable("asan")),
            mock.patch("builtins.print"),
        ):
            self.assertEqual(fd.main(["a.v"]), fc.EXIT_SKIP)


FAKE_ODIN = """#!/usr/bin/env python3
import sys
from pathlib import Path
script = sys.argv[sys.argv.index("-p") + 1]
flat = "flatten" in script
out = Path(script.split()[-1])
mode = Path(__file__).with_name("mode").read_text().strip()
if mode == "fail":
    sys.exit(1)
out.write_text("v2\\n" if flat and mode == "differ" else "v1\\n")
"""


class SimTests(unittest.TestCase):
    def _classify(self, mode: str, roundtrip: bool = False) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "odin3"
            fake.write_text(FAKE_ODIN)
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            (Path(tmp) / "mode").write_text(mode)
            with mock.patch.object(
                fc,
                "run_capped",
                side_effect=lambda argv: __import__("subprocess").run(
                    argv, capture_output=True, text=True, check=False
                ),
            ):
                return fs.classify(Path("a.v"), "top", str(fake), (4, 1), roundtrip)

    def test_equal(self) -> None:
        self.assertEqual(self._classify("same"), "equal")

    def test_differs(self) -> None:
        self.assertEqual(self._classify("differ"), "differs")

    def test_roundtrip_writes_then_reads_back(self) -> None:
        self.assertEqual(self._classify("same", roundtrip=True), "equal")
        self.assertTrue(self._classify("fail", roundtrip=True).startswith("skipped"))

    def test_read_steps(self) -> None:
        self.assertIn("proc", fs.read_steps(Path("a.v"), "t"))
        self.assertEqual(fs.read_steps(Path("a.blif"), "t"), "read_blif a.blif; ")

    def test_skipped_is_not_a_pass(self) -> None:
        self.assertTrue(self._classify("fail").startswith("skipped"))


if __name__ == "__main__":
    unittest.main()
