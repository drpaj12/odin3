"""Tests for tools/opt-check/rescan-compare (2F Task 15; Task 13 review M7), over a fake odin3 that
logs its script and writes a.il / b.il from the environment."""

from __future__ import annotations

import contextlib
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import memcap
import rescan_compare

from . import helpers

MEMINFO_32G = "MemTotal:       33554432 kB\nMemFree:        1000 kB\n"
SCRIPT = "opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt; compact"

# The fake odin3 logs its argv; write_rtlil NAME writes "same" or, for b.il when FAKE_DIFFER names
# the source's file, "other"; FAKE_FAIL_B makes a script with --opt-full-rescan fail.
FAKE_ODIN3 = """#!{python}
import json, os, sys
script = sys.argv[-1]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
rescan = "--opt-full-rescan" in script
if rescan and os.environ.get("FAKE_FAIL_B"):
    print("odin3: error: rescan boom", file=sys.stderr)
    sys.exit(1)
out = script.rsplit("write_rtlil ", 1)[1]
differ = os.environ.get("FAKE_DIFFER", "")
text = "other" if rescan and differ and differ in script else "same"
open(out, "w").write(text)
"""


class RescanCompareTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.odin3 = self.dir / "odin3"
        self.odin3.write_text(FAKE_ODIN3.format(python=sys.executable), encoding="utf-8")
        self.odin3.chmod(self.odin3.stat().st_mode | stat.S_IXUSR)
        self.corpus = self.dir / "corpus"
        self.corpus.mkdir()
        (self.corpus / "big.v").write_text("module big; endmodule\n" * 20, encoding="utf-8")
        (self.corpus / "small.v").write_text("module s; endmodule\n", encoding="utf-8")
        self.work = self.dir / "work"
        self.log = self.dir / "argv.log"
        meminfo = self.dir / "meminfo"
        meminfo.write_text(MEMINFO_32G, encoding="ascii")
        stack = contextlib.ExitStack()
        stack.enter_context(
            mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log), "PATH": str(self.dir)})
        )
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(meminfo)))
        self.addCleanup(stack.close)

    def run_tool(self, extra: dict[str, str] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--work", str(self.work), str(self.corpus)]
        with mock.patch.dict(os.environ, extra or {}):
            return helpers.run_main(rescan_compare.main, argv)

    def scripts(self) -> list[str]:
        return [json.loads(line)[-1] for line in self.log.read_text().splitlines()]

    def test_two_variants_per_file_smallest_first(self) -> None:
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0)
        small, big = str(self.corpus / "small.v"), str(self.corpus / "big.v")
        a = (
            "read_verilog --nolatches {f}; opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt; "
            "compact; write_rtlil a.il"
        )
        b = (
            "read_verilog --nolatches {f}; opt_expr --opt-full-rescan; opt_clean; "
            "opt -nodffe -nosdff --opt-full-rescan; proc; opt --opt-full-rescan; compact; "
            "write_rtlil b.il"
        )
        self.assertEqual(
            self.scripts(), [a.format(f=small), b.format(f=small), a.format(f=big), b.format(f=big)]
        )
        self.assertIn("2 files: 2 identical, 0 differ, 0 errors", out)

    def test_a_difference_is_exit_1_and_named(self) -> None:
        code, out, _ = self.run_tool({"FAKE_DIFFER": "big.v"})
        self.assertEqual(code, 1)
        self.assertIn("big.v: DIFFERS", out)
        for side in "ab":  # the pair survives the next design's a.il and b.il
            self.assertTrue((self.work / f"big.v.{side}.il").is_file())
        self.assertIn("small.v: identical", out)

    def test_a_failing_variant_is_exit_2(self) -> None:
        code, out, _ = self.run_tool({"FAKE_FAIL_B": "1"})
        self.assertEqual(code, 2)
        self.assertIn("ERROR in opt --opt-full-rescan", out)
        self.assertIn("rescan boom", out)

    def test_bad_input_is_exit_2(self) -> None:
        code, _, err = helpers.run_main(
            rescan_compare.main, ["--odin3", str(self.odin3), str(self.dir / "none")]
        )
        self.assertEqual(code, 2)
        self.assertIn("no such file", err)

    def test_with_rescan_only_touches_opt_and_opt_expr(self) -> None:
        self.assertEqual(
            rescan_compare.with_rescan(["opt_clean", "opt", "opt_expr -full", "proc"]),
            ["opt_clean", "opt --opt-full-rescan", "opt_expr -full --opt-full-rescan", "proc"],
        )

    def test_techlib_is_read_first(self) -> None:
        argv = [
            "--odin3",
            str(self.odin3),
            "--work",
            str(self.work),
            "--techlib",
            "/nolib/v.o3lib",
            str(self.corpus / "small.v"),
        ]
        self.assertEqual(helpers.run_main(rescan_compare.main, argv)[0], 0)
        self.assertTrue(
            all(
                s.startswith("read_techlib /nolib/v.o3lib; read_verilog --nolatches ")
                for s in self.scripts()
            )
        )

    def test_default_script_is_phase2_script(self) -> None:
        text = rescan_compare.DEFAULT_SCRIPT.read_text(encoding="utf-8")
        self.assertEqual("; ".join(rescan_compare.script_commands(text)), SCRIPT)


if __name__ == "__main__":
    unittest.main()
