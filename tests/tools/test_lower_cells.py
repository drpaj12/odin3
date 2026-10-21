"""Tests for tools/lower-check/lower-cells (3B Task 3 step 3): the per-cell goldens.

A fake ``odin3`` writes ``in.il`` and ``out.blif`` from ``fake.json``; the real ``netlist-compare``
judges the BLIF. The work directories are removed when each test ends.
"""

from __future__ import annotations

import json
import subprocess
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import lower_cells as lcells
import memcap

from . import helpers

FAKE_ODIN = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
if spec.get("signal"):
    os.kill(os.getpid(), spec["signal"])
if spec.get("rc"):
    sys.stderr.write("odin3: error: boom\n")
    sys.exit(spec["rc"])
for command in script.split("; "):
    if command.startswith("write_rtlil"):
        Path(command.split()[1]).write_text(spec["il"])
    elif command.startswith("write_blif"):
        Path(command.split()[1]).write_text(spec["blif"])
"""

IL = "module \\m\nend\n"
BLIF = ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"
OTHER = BLIF.replace("11 1", "00 1")


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


class CellsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = helpers.scratch(self, "lower_cells_test_")
        self.bin = self.root / "odin3"
        self.bin.write_text(FAKE_ODIN)
        self.bin.chmod(0o755)
        self.cells = self.root / "cells"
        self.cells.mkdir()
        (self.cells / "and_w2.v").write_text("module m; endmodule\n")
        self.set_spec()

    def set_spec(self, **changes: Any) -> None:
        spec: dict[str, Any] = {"il": IL, "blif": BLIF}
        spec.update(changes)
        (self.root / "fake.json").write_text(json.dumps(spec))

    def run_tool(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.bin), "--work", str(self.root / "work"), *extra,
                str(self.cells)]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(lcells.main, argv)

    def test_update_writes_the_goldens_and_a_check_then_matches(self) -> None:
        code, out, _ = self.run_tool("--update")
        self.assertEqual(code, 0, out)
        self.assertEqual((self.cells / "and_w2.il").read_text(), IL)
        self.assertEqual((self.cells / "and_w2.blif").read_text(), BLIF)
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        self.assertIn("checked 1 fixtures, 0 problems", out)

    def test_a_missing_golden_is_a_difference(self) -> None:
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("no committed .il/.blif", out)

    def test_a_changed_blif_or_il_is_a_difference(self) -> None:
        self.run_tool("--update")
        self.set_spec(blif=OTHER)
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("out.blif is not identical to and_w2.blif", out)
        self.set_spec(il="module \\other\nend\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("in.il differs from and_w2.il", out)

    def test_odin3_errors_and_crashes(self) -> None:
        self.set_spec(rc=1)
        code, _, err = self.run_tool("--update")
        self.assertEqual(code, 2)
        self.assertIn("odin3 exit 1 on and_w2.v: odin3: error: boom", err)
        self.set_spec(signal=11)
        self.assertEqual(self.run_tool("--update")[0], 3)

    def test_the_script_does_not_run_opt(self) -> None:
        script = lcells.odin_script(Path("x.v"), Path("w"))
        self.assertEqual(script, "read_verilog --nolatches x.v; hierarchy; proc; "
                         "write_rtlil w/in.il; lower; write_blif w/out.blif")

    def test_files_are_taken_smallest_first(self) -> None:
        (self.cells / "big.v").write_text("module m; endmodule\n" * 50)
        names = [p.name for p in lcells.collect([str(self.cells)])]
        self.assertEqual(names, ["and_w2.v", "big.v"])


if __name__ == "__main__":
    unittest.main()
