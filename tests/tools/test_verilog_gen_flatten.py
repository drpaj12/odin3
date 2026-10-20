"""Tests for the ``--flatten`` generator profile (3A Task 5, spec 14.5)."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import flatten_profile
import verilog_gen

from . import helpers

SEEDS = range(1, 201)


class FlattenProfileTest(unittest.TestCase):
    def test_deterministic_per_seed(self) -> None:
        for seed in (1, 7, 49):
            self.assertEqual(flatten_profile.generate(seed), flatten_profile.generate(seed))
        self.assertNotEqual(flatten_profile.generate(1).text, flatten_profile.generate(2).text)

    def test_every_feature_appears_in_seeds_1_to_200(self) -> None:
        seen: set[str] = set()
        for seed in SEEDS:
            seen |= flatten_profile.generate(seed).features
        self.assertEqual(sorted(set(flatten_profile.FEATURES) - seen), [])

    def test_features_are_named_in_the_file_and_real(self) -> None:
        for seed in SEEDS:
            profile = flatten_profile.generate(seed)
            self.assertEqual(
                profile.text.splitlines()[0],
                "// features: " + " ".join(sorted(profile.features)),
            )
            self.assertLessEqual(profile.features, set(flatten_profile.FEATURES))

    def test_depth_and_fanout_stay_within_the_spec(self) -> None:
        for seed in SEEDS:
            text = flatten_profile.generate(seed).text
            levels = int(re.search(r"^// levels: (\d+)$", text, re.M).group(1))  # type: ignore[union-attr]
            self.assertTrue(1 <= levels <= flatten_profile.MAX_LEVELS, seed)
            for body in re.findall(r"^module \w+ .*?^endmodule", text, re.M | re.S):
                instances = re.findall(r"^\s+(?:\(\* keep_hierarchy \*\) )?\w+ #\(", body, re.M)
                self.assertLessEqual(len(instances), flatten_profile.MAX_FANOUT, seed)

    def test_text_shows_each_construct(self) -> None:
        text = "".join(flatten_profile.generate(s).text for s in SEEDS)
        for needle in (
            "#(.W(2))",
            "genvar gi",
            "begin : g",
            "assign z = a;",
            "{W{1'b1}}",
            ".z()",
            "inout io",
            "1'bz",
            "(* keep_hierarchy *) module",
            "(* keep_hierarchy *) m",
            "posedge clk",
        ):
            self.assertIn(needle, text, needle)

    def test_top_is_always_top_and_every_module_is_defined_before_use(self) -> None:
        for seed in range(1, 51):
            text = flatten_profile.generate(seed).text
            self.assertIn("module top ", text)
            defined: set[str] = set()
            for line in text.splitlines():
                found = re.match(r"^(?:\(\* keep_hierarchy \*\) )?module (\w+) ", line)
                if found:
                    defined.add(found.group(1))
                    continue
                used = re.match(r"^\s+(?:\(\* keep_hierarchy \*\) )?(m\d_\d|bus_rd) #", line)
                if used:
                    self.assertIn(used.group(1), defined, f"seed {seed}: {line}")

    def test_cli_writes_the_profile_and_no_ast(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "g.v"
            self.assertEqual(verilog_gen.main(["--flatten", "--seed", "5", "--out", str(out)]), 0)
            self.assertEqual(out.read_text(encoding="utf-8"), flatten_profile.generate(5).text)
            self.assertFalse(out.with_suffix(".ast").exists())

    def test_flatten_is_exclusive_with_the_other_profiles_and_mutate(self) -> None:
        for other in ("--proc", "--elab", "--opt", "--mutate"):
            code, _, _ = helpers.run_main(
                verilog_gen.main, ["--flatten", other, "--seed", "1", "--out", "/dev/null"]
            )
            self.assertEqual(code, 2, other)

    def test_elaborates_in_odin3(self) -> None:
        odin3 = os.environ.get("ODIN3_CLI")
        if not odin3:
            self.skipTest("ODIN3_CLI is not set")
        with tempfile.TemporaryDirectory() as tmp:
            for seed in range(1, 21):
                path = Path(tmp) / f"s{seed}.v"
                path.write_text(flatten_profile.generate(seed).text, encoding="utf-8")
                script = (
                    f"read_verilog --nolatches {path}; hierarchy --top top; proc; flatten; check"
                )
                done = subprocess.run(
                    [odin3, "--top", "top", "-p", script],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(done.returncode, 0, f"seed {seed}: {done.stderr[-300:]}")


if __name__ == "__main__":
    unittest.main()
