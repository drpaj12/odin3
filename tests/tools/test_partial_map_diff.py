"""Tests for the multiplier micros per sweep arch (4.2 plan Task 16): the wrapper
``tools/partial-map-check/pm-diff-micro`` around ``hardmap-diff --arch`` (PR E gave that option;
no second driver), and the expect files of ``tests/partial_map``.

The expect files must parse with ``hardmap_diff.read_expect`` and must not list the multiplier
differences of the oversize split any more (PR F, X-4): a ``pending-F`` or ``4.3 not done`` line
would be a stale entry. The wrapper skips with 77 without ``VTR_ROOT`` or the golden tree. Work
directories come from ``tempfile.TemporaryDirectory``.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import hardmap_diff as hd

from .helpers import REPO_ROOT

WRAPPER = REPO_ROOT / "tools" / "partial-map-check" / "pm-diff-micro"
EXPECT_DIR = REPO_ROOT / "tests" / "partial_map"
ARCHS = (
    "k6_frac_N10_frac_chain_mem32K_40nm",
    "k6_frac_N10_4add_2chains_depop50_mem20K_22nm",
    "k4_N4_90nm",
)


class ExpectFiles(unittest.TestCase):
    def test_every_expect_file_parses(self) -> None:
        files = sorted(EXPECT_DIR.glob("diff_expect_*.txt"))
        self.assertTrue(files)
        for file in files:
            with self.subTest(file=file.name):
                self.assertTrue(hd.read_expect([file]) is not None)

    def test_no_pending_oversize_split_lines(self) -> None:
        for file in sorted(EXPECT_DIR.glob("diff_expect_*.txt")):
            text = file.read_text(encoding="utf-8")
            with self.subTest(file=file.name):
                self.assertNotIn("pending-F", text)
                self.assertNotIn("4.3 not done", text)

    def test_multiplier_designs_have_no_multiply_or_adder_count_entry(self) -> None:
        for file in sorted(EXPECT_DIR.glob("diff_expect_*.txt")):
            table = hd.read_expect([file])
            for design in ("diffeq1.v", "diffeq2.v", "or1200.v"):
                for entry in table.get(design, []):
                    with self.subTest(file=file.name, design=design):
                        self.assertNotIn(entry.unit, ("multiply", "adder"))


class Wrapper(unittest.TestCase):
    def run_wrapper(self, env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
        full = {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}
        full.update(env)
        return subprocess.run(
            [str(WRAPPER), *args], capture_output=True, text=True, check=False, env=full
        )

    def test_skips_without_vtr_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for arch in ARCHS:
                with self.subTest(arch=arch):
                    proc = self.run_wrapper({}, arch, "/bin/true", tmp)
                    self.assertEqual(77, proc.returncode, proc.stderr)

    def test_skips_without_golden_tree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = self.run_wrapper({"VTR_ROOT": tmp}, ARCHS[0], "/bin/true", tmp)
            self.assertEqual(77, proc.returncode, proc.stderr)
            self.assertIn("skipped", proc.stderr)

    def test_skips_without_the_arch_xml(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            golden = Path(tmp) / "golden"
            (golden / ARCHS[0]).mkdir(parents=True)
            proc = self.run_wrapper({"VTR_ROOT": tmp}, ARCHS[0], "/bin/true", str(golden))
            self.assertEqual(77, proc.returncode, proc.stderr)
            self.assertIn("no k6_frac_N10_frac_chain_mem32K_40nm.xml", proc.stderr)

    def test_unknown_arch_is_a_usage_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            golden = Path(tmp) / "golden"
            (golden / "nonesuch").mkdir(parents=True)
            arch_dir = Path(tmp) / "vtr_flow" / "arch"
            arch_dir.mkdir(parents=True)
            (arch_dir / "nonesuch.xml").write_text("<architecture/>", encoding="utf-8")
            proc = self.run_wrapper({"VTR_ROOT": tmp}, "nonesuch", "/bin/true", str(golden))
            self.assertEqual(2, proc.returncode, proc.stderr)
            self.assertIn("unknown arch", proc.stderr)


if __name__ == "__main__":
    unittest.main()
