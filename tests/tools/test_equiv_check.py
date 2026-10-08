"""Tests for tools/equiv-check.

Tests that need ABC are skipped (not failed) when no ABC binary is found; set ODIN3_ABC to
point at one.  Tests that stop before ABC runs (input errors, interface mismatches) always
run.
"""

from __future__ import annotations

import contextlib
import io
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import blif
import equiv_check

from . import helpers


def _find_abc() -> str | None:
    try:
        return equiv_check.find_abc()
    except equiv_check.EquivError:
        return None


ABC = _find_abc()
needs_abc = unittest.skipIf(ABC is None, "ABC not found (set ODIN3_ABC or build the repo)")


def check(*argv: str) -> tuple[int, str, str]:
    return helpers.run_main(equiv_check.main, list(argv))


def fx(name: str) -> str:
    return helpers.fixture(name)


class NormalizeTest(unittest.TestCase):
    def test_normalize_io_name(self) -> None:
        cases = {
            ("blink^clk", "blink"): "clk",
            ("top^a~0", "top"): "a[0]",
            ("top^a~12", "top"): "a[12]",
            ("a[3]", "top"): "a[3]",
            ("other^x~1", "top"): "other^x[1]",
            ("x~y", "top"): "x~y",
        }
        for (name, top), expected in cases.items():
            with self.subTest(name):
                self.assertEqual(equiv_check.normalize_io_name(name, top), expected)

    def test_parse_verdict(self) -> None:
        self.assertTrue(equiv_check.parse_verdict("Networks are equivalent.  Time = 0.00 sec"))
        self.assertTrue(equiv_check.parse_verdict(
            "Networks are equivalent after structural hashing."))
        self.assertFalse(equiv_check.parse_verdict("Networks are NOT EQUIVALENT.  Time ="))
        self.assertFalse(equiv_check.parse_verdict(
            "Networks are NOT EQUIVALENT after simulation.   Time ="))
        self.assertFalse(equiv_check.parse_verdict("Networks are NOT equivalent."))
        self.assertIsNone(equiv_check.parse_verdict("Networks are UNDECIDED.   Time ="))
        self.assertIsNone(equiv_check.parse_verdict("Reading network has failed."))


class FlattenTest(unittest.TestCase):
    def test_adder_ports_both_spellings(self) -> None:
        for name in ("blink.parmys.blif", "blink.odin.blif", "add2_parmys.blif"):
            with self.subTest(name):
                with contextlib.redirect_stderr(io.StringIO()):
                    flat = equiv_check.flatten(blif.parse_file(fx(name)))
                self.assertFalse(any(" " in c.output and c.output.startswith("adder")
                                     for c in flat.names))
                self.assertTrue(flat.names)

    def test_hierarchy_keeps_instances_apart(self) -> None:
        flat = equiv_check.flatten(blif.parse_file(fx("add2_hier.blif")))
        t_nets = {c.output for c in flat.names if c.output.endswith(" t")}
        self.assertEqual(len(t_nets), 2)

    def test_recursive_model_rejected(self) -> None:
        text = (".model t\n.inputs a\n.outputs y\n.subckt r x=a z=y\n.end\n"
                ".model r\n.inputs x\n.outputs z\n.subckt r x=x z=z\n.end\n")
        with self.assertRaises(equiv_check.EquivError):
            equiv_check.flatten(blif.parse_text(text))


class EquivCheckNoAbcTest(unittest.TestCase):
    """Paths that exit before ABC is needed."""

    def test_unsupported_black_box(self) -> None:
        code, _, err = check(fx("mult_bb.blif"), fx("mul1_logic.blif"))
        self.assertEqual(code, 2)
        self.assertIn("unsupported black box multiply", err)

    def test_interface_mismatch(self) -> None:
        code, out, _ = check(fx("comb_parmys.blif"), fx("comb_po_renamed.blif"))
        self.assertEqual(code, 1)
        self.assertIn("only in A: ['z']; only in B: ['zz']", out)

    def test_no_io_normalize(self) -> None:
        code, out, _ = check("--no-io-normalize", fx("add2_parmys.blif"), fx("add2_odin.blif"))
        self.assertEqual(code, 1)
        self.assertIn("add2^s~0", out)

    def test_malformed_input(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.blif"
            bad.write_text(".model t\n.inputs a\n.outputs y\n.names a y\n11 1\n.end\n",
                           encoding="utf-8")
            code, _, err = check(fx("comb_parmys.blif"), str(bad))
        self.assertEqual(code, 2)
        self.assertRegex(err, r"^equiv-check: .*bad\.blif:5: ")

    def test_strict_init(self) -> None:
        code, _, err = check("--strict-init", fx("blink.odin.blif"), fx("blink.parmys.blif"))
        self.assertEqual(code, 2)
        self.assertIn("--strict-init", err)

    def test_bad_abc_path(self) -> None:
        code, _, err = check("--abc", "/nonexistent/abc", fx("comb_parmys.blif"),
                             fx("comb_renamed.blif"))
        self.assertEqual(code, 2)
        self.assertIn("not an executable", err)

    def test_unparseable_abc_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "abc"
            fake.write_text("#!/bin/sh\necho 'ABC says something unexpected'\n",
                            encoding="utf-8")
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            code, _, err = check("--abc", str(fake), fx("comb_parmys.blif"),
                                 fx("comb_renamed.blif"))
        self.assertEqual(code, 2)
        self.assertIn("ABC says something unexpected", err)


@needs_abc
class EquivCheckAbcTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_equivalent_cec(self) -> None:
        code, out, _ = check(fx("comb_parmys.blif"), fx("comb_renamed.blif"))
        self.assertEqual(code, 0, out)
        self.assertIn("(abc cec)", out)

    def test_not_equivalent_cec(self) -> None:
        code, out, _ = check(fx("comb_parmys.blif"), fx("comb_flipped.blif"))
        self.assertEqual(code, 1, out)
        self.assertIn("NOT EQUIVALENT", out)

    def test_sequential_dsec(self) -> None:
        code, out, _ = check(fx("counter_parmys.blif"), fx("counter_odin.blif"))
        self.assertEqual(code, 0, out)
        self.assertIn("(abc dsec)", out)
        code, out, _ = check(fx("counter_odin.blif"), fx("counter_bug.blif"))
        self.assertEqual(code, 1, out)

    def test_adder_black_box_vs_logic(self) -> None:
        for other in ("add2_logic.blif", "add2_odin.blif", "add2_hier.blif"):
            with self.subTest(other):
                code, out, _ = check(fx("add2_parmys.blif"), fx(other))
                self.assertEqual(code, 0, out)
        code, out, _ = check(fx("add2_odin.blif"), fx("add2_logic_bug.blif"))
        self.assertEqual(code, 1, out)

    def test_real_goldens_odin_vs_parmys(self) -> None:
        code, out, err = check(fx("blink.odin.blif"), fx("blink.parmys.blif"))
        self.assertEqual(code, 0, out + err)
        self.assertIn("(abc dsec)", out)
        self.assertIn("5 latch(es) with init 2/3", err)

    def test_real_golden_mutation_detected(self) -> None:
        """Negative control: the golden pair is not equivalent once o_led is inverted."""
        text = Path(fx("blink.parmys.blif")).read_text(encoding="utf-8")
        mutated = text.replace(".names $sdff~1^Q~4 o_led\n0 1\n",
                               ".names $sdff~1^Q~4 o_led\n1 1\n")
        self.assertNotEqual(mutated, text)
        code, out, _ = check(fx("blink.odin.blif"), self.write("mut.blif", mutated))
        self.assertEqual(code, 1, out)

    def test_tautological_cover(self) -> None:
        """ABC asserts on tautological covers; equiv-check folds them to constants."""
        head = ".model t\n.inputs x y\n.outputs o p\n"
        a = self.write("a.blif", head + ".names x y o\n0- 1\n1- 1\n.names x y p\n-- 0\n.end\n")
        b = self.write("b.blif", head + ".names o\n1\n.names p\n.end\n")
        code, out, err = check(a, b)
        self.assertEqual(code, 0, out + err)

    def test_wrapper_script(self) -> None:
        wrapper = helpers.REPO_ROOT / "tools" / "equiv-check" / "equiv-check"
        assert ABC is not None
        env = dict(os.environ, ODIN3_ABC=ABC)
        result = subprocess.run([str(wrapper), fx("add2_parmys.blif"), fx("add2_logic.blif")],
                                capture_output=True, text=True, check=False, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
