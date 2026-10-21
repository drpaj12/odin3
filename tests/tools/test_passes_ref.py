"""Tests for docs/user/_ext/passes_ref.py (7.3, spec 2.2): the pass reference is generated from
docs/PASSES.md and ``odin3 --help`` at build time, with no hand copy."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

import passes_ref

from . import helpers

PASSES_MD = helpers.REPO_ROOT / "docs" / "PASSES.md"

HELP = """usage: odin3 [options]

passes:
  read_blif [--hdl-locs] <file>: read a BLIF netlist
  compact: renumber each module densely
  hardmap (alias of partial_map)
  partial_map: map to hard blocks
"""

TABLE = """| Pass | Phase | Input view | Source | Golden test | Notes |
|---|---|---|---|---|---|
| `read_blif [--hdl-locs] <file>` | 1 (1C) | - | `a.c` | `t1` | n1 |
| `partial_map` (alias `hardmap`) | 3 | any | `b.c` | `t2, t3` | n2 with a | pipe |
| `fsm [-encoding one-hot|binary]` | 4 | any | `c.c` | `t4` | n3 |
"""


def _md_rows(text: str) -> int:
    return sum(1 for line in text.splitlines() if line.startswith("| `"))


class ParseTests(unittest.TestCase):
    def test_rows_parse(self) -> None:
        rows = passes_ref.parse_passes_md(TABLE)
        self.assertEqual([r.name for r in rows], ["read_blif", "partial_map", "fsm"])
        self.assertEqual(rows[1].aliases, ["hardmap"])
        self.assertEqual(rows[1].notes, "n2 with a | pipe")
        self.assertEqual(rows[2].signature, "fsm [-encoding one-hot|binary]")

    def test_help_parse(self) -> None:
        h = passes_ref.parse_help(HELP)
        self.assertEqual(h["read_blif"].usage, "read_blif [--hdl-locs] <file>")
        self.assertEqual(h["read_blif"].text, "read a BLIF netlist")
        self.assertEqual(h["compact"].text, "renumber each module densely")
        self.assertEqual(h["hardmap"].text, "(alias of partial_map)")

    def test_render_attaches_help(self) -> None:
        rows = passes_ref.parse_passes_md(TABLE)
        out = passes_ref.render_markdown(rows, passes_ref.parse_help(HELP))
        self.assertIn("read a BLIF netlist", out)
        self.assertIn("map to hard blocks", out)


class EveryPassListedTests(unittest.TestCase):
    def test_every_pass_listed(self) -> None:
        """Entries generated == rows of docs/PASSES.md."""
        text = PASSES_MD.read_text(encoding="utf-8")
        rows = passes_ref.parse_passes_md(text)
        self.assertEqual(len(rows), _md_rows(text))
        out = passes_ref.render_markdown(rows, {})
        self.assertEqual(passes_ref.count_entries(out), _md_rows(text))

    def test_names_unique_and_nonempty(self) -> None:
        rows = passes_ref.parse_passes_md(PASSES_MD.read_text(encoding="utf-8"))
        names = [r.name for r in rows]
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(n.isidentifier() for n in names))

    def test_live_help_covers_rows(self) -> None:
        """Every PASSES.md pass appears in the odin3 --help list (needs ODIN3_CLI)."""
        cli = os.environ.get("ODIN3_CLI")
        if not cli or not Path(cli).is_file():
            self.skipTest("ODIN3_CLI not set")
        res = subprocess.run([cli, "--help"], capture_output=True, text=True, check=True)
        h = passes_ref.parse_help(res.stdout)
        rows = passes_ref.parse_passes_md(PASSES_MD.read_text(encoding="utf-8"))
        self.assertEqual([r.name for r in rows if r.name not in h], [])


if __name__ == "__main__":
    unittest.main()
