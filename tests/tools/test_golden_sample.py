"""Tests for tools/golden-sample."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import golden_sample

from . import helpers

ARCHS = ("A", "B")


def _put(golden: Path, arch: str, name: str, tool: str, status: str, size: int) -> None:
    d = golden / arch / name
    d.mkdir(parents=True, exist_ok=True)
    leaf = name.rsplit("/", 1)[-1]
    (d / f"{leaf}.{tool}.prov").write_text(f"tool={tool}\nstatus={status}\n")
    if status == "ok":
        (d / f"{leaf}.{tool}.blif").write_text("x" * size)


def _design(golden: Path, name: str, size: int = 10, bad: tuple[str, str] | None = None) -> None:
    for arch in ARCHS:
        for tool in golden_sample.TOOLS:
            failed = bad == (arch, tool)
            _put(golden, arch, name, tool, "failed" if failed else "ok", size)


class GoldenSampleTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.golden = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_group_of(self) -> None:
        self.assertEqual(golden_sample.group_of("regression/verilog/syntax/x/y"),
                         "regression/verilog/syntax")
        self.assertEqual(golden_sample.group_of("vtr/mcml"), "vtr")
        self.assertEqual(golden_sample.group_of("quickstart/blink"), "quickstart")

    def test_spread_takes_evenly_spaced_ranks(self) -> None:
        items = [golden_sample.Design(str(i)) for i in range(9)]
        picked = [d.name for d in golden_sample.spread(items, 3)]
        self.assertEqual(picked, ["0", "4", "8"])
        self.assertEqual(len(golden_sample.spread(items[:2], 5)), 2)

    def test_only_fully_ok_designs_are_eligible(self) -> None:
        _design(self.golden, "vtr/good")
        _design(self.golden, "vtr/one_failed", bad=("B", "odin"))
        _put(self.golden, "A", "vtr/one_arch", "parmys", "ok", 10)
        _put(self.golden, "A", "vtr/one_arch", "odin", "ok", 10)
        designs = golden_sample.scan(self.golden)
        chosen = golden_sample.select(designs, 5, 1000)
        self.assertEqual([d.name for d in chosen["vtr"]], ["vtr/good"])

    def test_size_limit_and_spread(self) -> None:
        for i, size in enumerate([10, 20, 30, 40, 50, 5000]):
            _design(self.golden, f"vtr/d{i}", size)
        chosen = golden_sample.select(golden_sample.scan(self.golden), 3, 1000)
        self.assertEqual([d.name for d in chosen["vtr"]], ["vtr/d0", "vtr/d2", "vtr/d4"])

    def test_blink_always_included(self) -> None:
        for i in range(4):
            _design(self.golden, f"quickstart/d{i}", 100 + i)
        _design(self.golden, "quickstart/blink", 1)
        _design(self.golden, "quickstart/zz", 500)
        chosen = golden_sample.select(golden_sample.scan(self.golden), 1, 1000)
        self.assertEqual({d.name for d in chosen["quickstart"]}, {"quickstart/blink"})

    def test_write_replaces_only_the_managed_block(self) -> None:
        _design(self.golden, "vtr/good")
        gi = self.golden / ".gitignore"
        gi.write_text(f"keep-me\n{golden_sample.BEGIN}\n!/stale.blif\n{golden_sample.END}\ntail\n")
        code, out, err = helpers.run_main(golden_sample.main, ["--golden", str(self.golden),
                                                               "--write"])
        self.assertEqual(code, 0, err)
        self.assertIn("total", out)
        lines = gi.read_text().splitlines()
        self.assertEqual(lines[0], "keep-me")
        self.assertEqual(lines[-1], "tail")
        self.assertNotIn("!/stale.blif", lines)
        self.assertIn("*.blif", lines)
        self.assertIn("!/A/vtr/good/good.odin.blif", lines)
        self.assertIn("!/B/vtr/good/good.parmys.blif", lines)

    def test_missing_golden_is_input_error(self) -> None:
        code, _, err = helpers.run_main(golden_sample.main,
                                        ["--golden", str(self.golden / "nope")])
        self.assertEqual(code, 2)
        self.assertIn("golden-sample:", err)

    def test_malformed_prov_name_is_input_error(self) -> None:
        _design(self.golden, "vtr/good")
        (self.golden / "A" / "vtr" / "good" / "stray.prov").write_text("status=ok\n")
        code, _, err = helpers.run_main(golden_sample.main, ["--golden", str(self.golden)])
        self.assertEqual(code, 2)
        self.assertIn("stray.prov", err)

    def test_dirs_without_provs_are_not_archs(self) -> None:
        _design(self.golden, "vtr/good")
        (self.golden / "tmp").mkdir()
        chosen = golden_sample.select(golden_sample.scan(self.golden), 5, 1000)
        self.assertEqual([d.name for d in chosen["vtr"]], ["vtr/good"])


if __name__ == "__main__":
    unittest.main()
