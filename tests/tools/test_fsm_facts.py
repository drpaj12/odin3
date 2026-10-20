"""Tests for the path normalisation and the --check report of tools/fsm-check/fsm_facts.py."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import fsm_facts


class NormaliseTest(unittest.TestCase):
    def test_repo_and_vtr_roots_are_stripped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, vtr = Path(tmp) / "co1", Path(tmp) / "vtr"
            wire = f"$memwr$\\m${repo}/tests/micro/a.v:40$38_EN"
            facts = {
                f"$fsm${wire}": {
                    "wire": wire,
                    "regs": [f"{vtr}/build/bin/../share/p.v:3$1", "plain"],
                    "n": 3,
                }
            }
            got = fsm_facts.normalise(facts, repo, vtr)
            want_wire = "$memwr$\\m$tests/micro/a.v:40$38_EN"
            self.assertEqual(
                got,
                {
                    f"$fsm${want_wire}": {
                        "wire": want_wire,
                        "regs": ["$VTR_ROOT/build/bin/../share/p.v:3$1", "plain"],
                        "n": 3,
                    }
                },
            )

    def test_two_checkouts_normalise_equal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            vtr = Path(tmp) / "vtr"
            one = {"w": f"x${Path(tmp) / 'a'}/tests/f.v:1"}
            two = {"w": f"x${Path(tmp) / 'b'}/tests/f.v:1"}
            self.assertEqual(
                fsm_facts.normalise(one, Path(tmp) / "a", vtr),
                fsm_facts.normalise(two, Path(tmp) / "b", vtr),
            )


class DiffTest(unittest.TestCase):
    def test_differences_are_named_and_limited(self) -> None:
        old = {"a": {"x": 1, "y": [1, 2]}, "b": 2, "gone": 1}
        new = {"a": {"x": 2, "y": [1, 3]}, "b": 2, "added": 1}
        lines = fsm_facts.describe_diff(old, new, limit=10)
        self.assertEqual(len(lines), 4)
        self.assertTrue(any(line.startswith("a.x:") for line in lines))
        self.assertTrue(any(line.startswith("a.y[1]:") for line in lines))
        self.assertTrue(any(line.startswith("gone:") for line in lines))
        self.assertTrue(any(line.startswith("added:") for line in lines))
        self.assertEqual(len(fsm_facts.describe_diff(old, new, limit=2)), 2)
        self.assertEqual(fsm_facts.describe_diff(old, old, limit=5), [])


if __name__ == "__main__":
    unittest.main()
