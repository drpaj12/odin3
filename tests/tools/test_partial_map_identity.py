"""Tests for tools/partial-map-check/identity.py (4.2 plan Task 3): the classes, the script each
leg runs, the tiers and the exit codes, with a fake ``odin3``.

The fake logs its ``-p`` string and writes the BLIFs ``fake.json`` names, per pass name. Work
directories come from ``tempfile.TemporaryDirectory`` and are removed by it.
"""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import exit_check
import identity as idt

from .helpers import run_main

FAKE_ODIN = r"""#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "odin.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
name = "hardmap" if "; hardmap; " in script else "partial_map"
leg = spec.get(name, {})
if leg.get("rc"):
    sys.stderr.write(leg.get("err", "odin3: error: boom\n"))
    sys.exit(leg["rc"])
vtr = re.search(r"write_blif --vtr (\S+?);", script).group(1)
plain = script.rsplit("write_blif ", 1)[1].strip()
Path(vtr).write_text(leg.get("vtr", "# v1\n.model a\n.end\n"))
Path(plain).write_text(leg.get("plain", "# v1\n.model b\n.end\n"))
"""


class IdentityCase(unittest.TestCase):
    def setUp(self) -> None:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        self.tmp = Path(holder.name)
        self.odin = self.tmp / "odin3"
        self.odin.write_text(FAKE_ODIN)
        self.odin.chmod(0o755)
        self.spec: dict[str, Any] = {}
        self.design = self.tmp / "d.v"
        self.design.write_text("module top; endmodule\n")

    def run_tool(self, *extra: str) -> tuple[int, str, str]:
        (self.tmp / "fake.json").write_text(json.dumps(self.spec))
        argv = ["--odin3", str(self.odin), *extra]
        return run_main(idt.main, argv if "--tier" in argv else ["--tier", "1", *argv])

    def log(self) -> str:
        path = self.tmp / "odin.log"
        return path.read_text() if path.exists() else ""


class ClassTests(IdentityCase):
    def test_identical_ignores_header_lines(self) -> None:
        self.spec = {"hardmap": {"vtr": "# a\n.model m\n.end\n"},
                     "partial_map": {"vtr": "# b\n.model m\n.end\n"}}
        code, out, _ = self.run_tool("--tier", "1", str(self.design))
        self.assertEqual(code, 0, out)
        self.assertIn("identical", out)

    def test_vtr_blif_difference(self) -> None:
        self.spec = {"partial_map": {"vtr": ".model other\n.end\n"}}
        code, out, _ = self.run_tool("--tier", "1", str(self.design))
        self.assertEqual(code, 1)
        self.assertIn("DIFFERENT (write_blif --vtr: line 1", out)

    def test_plain_blif_difference(self) -> None:
        self.spec = {"partial_map": {"plain": ".model other\n.end\n"}}
        code, out, _ = self.run_tool("--tier", "1", str(self.design))
        self.assertEqual(code, 1)
        self.assertIn("DIFFERENT (write_blif: line 1", out)

    def test_exit_status_difference(self) -> None:
        self.spec = {"partial_map": {"rc": 1}}
        code, out, _ = self.run_tool("--tier", "1", str(self.design))
        self.assertEqual(code, 1)
        self.assertIn("exit 0 against 1", out)

    def test_same_failure_is_not_a_difference_but_does_not_count(self) -> None:
        self.spec = {"hardmap": {"rc": 1}, "partial_map": {"rc": 1}}
        code, out, err = self.run_tool("--tier", "1", str(self.design))
        self.assertEqual(code, 1, out)  # --min-ok 1 not met
        self.assertIn("same-failure", out)
        self.assertIn("--min-ok", err)
        code, out, _ = self.run_tool("--tier", "1", "--min-ok", "0", str(self.design))
        self.assertEqual(code, 0, out)

    def test_crash_is_hard(self) -> None:
        self.spec = {}
        self.odin.write_text("#!/usr/bin/env python3\nimport os, signal\n"
                             "os.kill(os.getpid(), signal.SIGSEGV)\n")
        code, out, _ = self.run_tool("--tier", "1", str(self.design))
        self.assertEqual(code, 3, out)
        self.assertIn("HARD", out)


class ScriptTests(IdentityCase):
    def test_tier1_runs_both_names_with_the_tech_library(self) -> None:
        self.run_tool("--tier", "1", f"{self.design}:top")
        lines = self.log().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertIn("--top top", lines[0])
        self.assertIn("; hardmap; ", lines[0])
        self.assertIn("; partial_map; ", lines[1])
        for line in lines:
            self.assertIn("read_techlib", line)
            self.assertIn("opt -fast; compact; write_blif --vtr", line)

    def test_tier2_swaps_the_library_for_read_arch(self) -> None:
        xml = self.tmp / "k6.xml"
        xml.write_text("<architecture/>")
        code, out, _ = self.run_tool("--tier", "2", "--arch-xml", str(xml), str(self.design))
        self.assertEqual(code, 0, out)
        lines = self.log().splitlines()
        self.assertIn("read_techlib", lines[0])
        self.assertIn(f"read_arch {xml}", lines[1])
        self.assertNotIn("read_techlib", lines[1])
        self.assertNotIn("; hardmap; ", lines[0])

    def test_tier2_skips_without_vtr_root(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("VTR_ROOT", None)
            code, _, err = self.run_tool("--tier", "2", str(self.design))
        self.assertEqual(code, 77)
        self.assertIn("skipped", err)

    def test_tier3_uses_two_binaries(self) -> None:
        other = self.tmp / "base"
        other.write_text(FAKE_ODIN)
        other.chmod(0o755)
        (self.tmp / "fake.json").write_text("{}")
        argv = ["--tier", "3", "--baseline", str(other), "--candidate", str(self.odin),
                str(self.design)]
        code, out, _ = run_main(idt.main, argv)
        self.assertEqual(code, 0, out)
        self.assertIn("tier 3: 1 designs: 1 identical", out)

    def test_tier3_needs_both_binaries(self) -> None:
        code, _, err = self.run_tool("--tier", "3", str(self.design))
        self.assertEqual(code, 2)
        self.assertIn("--baseline", err)


class InputTests(IdentityCase):
    def test_list_file_and_vtr_list(self) -> None:
        listing = self.tmp / "list.txt"
        listing.write_text(f"# c\n{self.design} top\n")
        vtr = self.tmp / "vtr.txt"
        vtr.write_text("# c\nd\n")
        code, out, _ = self.run_tool("--list", str(listing), "--vtr-list", str(vtr),
                                     "--vtr-root", str(self.tmp))
        self.assertEqual(code, 0, out)
        self.assertIn("2 designs: 2 identical", out)

    def test_bad_list_line(self) -> None:
        listing = self.tmp / "list.txt"
        listing.write_text("a b c\n")
        code, _, err = self.run_tool("--list", str(listing))
        self.assertEqual(code, 2)
        self.assertIn("expected 'path [top]'", err)

    def test_shipped_list_names_existing_files(self) -> None:
        designs = idt.read_list(idt.DEFAULT_INPUTS)
        self.assertEqual(len(designs), 33)
        for design in designs:
            self.assertTrue(design.path.is_file(), design.key)
            self.assertIsNotNone(design.top)

    def test_vtr_list_uses_the_golden_top(self) -> None:
        golden = self.tmp / "golden"
        vtr = self.tmp / "vtr.txt"
        vtr.write_text("d\n")
        with mock.patch.object(exit_check, "golden_path",
                               return_value=str(golden / "d.parmys.blif")):
            golden.mkdir()
            (golden / "d.parmys.blif").write_text(".model top_x\n.end\n")
            designs = idt.read_vtr(vtr, self.tmp, golden)
        self.assertEqual(designs[0].top, "top_x")


if __name__ == "__main__":
    unittest.main()
