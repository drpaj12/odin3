"""Tests for tools/unmap-check (v1.0.4 unmap, spec T-7): the log parser, the judgement of the rows
and the exit codes, over a fake odin3 that prints a canned log."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

REPO = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO / "tools" / "unmap-check"

INFO = ("odin3: info: unmap: 14 adder cells to 2 $add + 1 $sub, 5 multiply to 3 $mul "
        "(2 regrouped), 6 RAM cells to 2 $mem, 1 groups left (see unmap_note)")
CYCLE = "odin3: warning: unmap: module 'm': adder at '$c3' not recovered: cycle"


def load_tool() -> ModuleType:
    spec = importlib.util.spec_from_file_location("unmap_vtr", TOOL_DIR / "unmap_vtr.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["unmap_vtr"] = module
    spec.loader.exec_module(module)
    return module


def fake_odin3(directory: Path, log: str, code: int = 0) -> Path:
    """A script that prints `log` on stderr and exits with `code`."""
    path = directory / "odin3"
    text = f"#!/bin/sh\ncat >&2 <<'LOG'\n{log}\nLOG\nexit {code}\n"
    path.write_text(text)
    path.chmod(0o755)
    return path


class ParseTest(unittest.TestCase):
    def test_parse_info_line(self) -> None:
        tool = load_tool()
        row = tool.parse_log("d", f"noise\n{INFO}\n")
        self.assertEqual(
            (row.adder_cells, row.add, row.sub, row.multiply, row.mul, row.regrouped,
             row.ram_cells, row.mem, row.left), (14, 2, 1, 5, 3, 2, 6, 2, 1))

    def test_parse_reasons_and_candidates(self) -> None:
        tool = load_tool()
        cand = "unmap: 2 RAM cells with equal control nets (candidate set ram:0)"
        log = f"{INFO}\n{CYCLE}\n{CYCLE}\n{cand}\n"
        row = tool.parse_log("d", log)
        self.assertEqual(row.reasons["cycle"], 2)
        self.assertEqual(row.cands, 1)

    def test_judge_listing(self) -> None:
        tool = load_tool()
        row = tool.parse_log("d", f"{INFO}\n{CYCLE}\n")
        self.assertEqual(len(tool.judge([row], [])), 1)
        self.assertEqual(tool.judge([row], [("d", "cycle")]), [])
        self.assertEqual(len(tool.judge([tool.parse_log("d", INFO)], [("d", "cycle")])), 1)


class ExitCodeTest(unittest.TestCase):
    def run_tool(self, log: str, code: int = 0, expect: str | None = None,
                 designs: tuple[str, ...] = ("a.v",)) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            odin3 = fake_odin3(directory, log, code)
            args = [str(TOOL_DIR / "unmap-vtr"), "--odin3", str(odin3)]
            if expect is not None:
                (directory / "expect.txt").write_text(expect)
                args += ["--expect", str(directory / "expect.txt")]
            args += [str(directory / name) for name in designs]
            return subprocess.run(args, capture_output=True, text=True, check=False).returncode

    def test_clean_run_is_ok(self) -> None:
        self.assertEqual(self.run_tool(INFO), 0)

    def test_cycle_note_fails(self) -> None:
        self.assertEqual(self.run_tool(f"{INFO}\n{CYCLE}"), 1)

    def test_listed_cycle_is_ok(self) -> None:
        self.assertEqual(self.run_tool(f"{INFO}\n{CYCLE}", expect="a cycle fake reason\n"), 0)

    def test_stale_listing_fails(self) -> None:
        self.assertEqual(self.run_tool(INFO, expect="a cycle fake reason\n"), 1)

    def test_failed_run_fails(self) -> None:
        self.assertEqual(self.run_tool(INFO, code=1), 1)

    def test_no_designs_is_usage(self) -> None:
        self.assertEqual(self.run_tool(INFO, designs=()), 2)


if __name__ == "__main__":
    unittest.main()
