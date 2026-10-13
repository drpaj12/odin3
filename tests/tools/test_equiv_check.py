"""Tests for tools/equiv-check.

Tests that need ABC are skipped (not failed) when no usable ABC binary is found; set
ODIN3_ABC to point at one.  With ODIN3_REQUIRE_ABC=1 (set by the CTest run) a missing or
unusable ABC makes those tests FAIL instead.  Tests that stop before ABC runs (input
errors, interface or clocking mismatches) always run.
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


def _find_abc() -> tuple[str | None, str]:
    """(usable ABC path or None, reason it is unusable)."""
    try:
        path = equiv_check.find_abc()
    except equiv_check.EquivError as exc:
        return None, str(exc)
    try:
        proc = subprocess.run([path, "-q", "quit"], capture_output=True, text=True,
                              timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return None, f"{path}: {exc}"
    if proc.returncode != 0:
        return None, f"{path} -q quit exited with status {proc.returncode}"
    return path, ""


ABC, ABC_PROBLEM = _find_abc()
REQUIRE_ABC = os.environ.get("ODIN3_REQUIRE_ABC") == "1"
needs_abc = unittest.skipIf(ABC is None and not REQUIRE_ABC,
                            f"ABC not available ({ABC_PROBLEM}); set ODIN3_ABC")
CRITIC = helpers.FIXTURES / "critic"


def cx(name: str) -> str:
    return str(CRITIC / name)


def check(*argv: str) -> tuple[int, str, str]:
    return helpers.run_main(equiv_check.main, list(argv))


def fx(name: str) -> str:
    return helpers.fixture(name)


BB_HEAD = ".model t\n.inputs clk a b c\n.outputs y\n"
BB_TAIL = ".names q a y\n11 1\n.end\n"
# A Yosys write_blif -gates hard block: a ``.gate`` with no model and no instance name.
RAM_Q = ".gate single_port_ram addr[0]=a clk=clk data[0]=b out[0]=q we=c\n"


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
        code, _, err = check("--no-cut", fx("mult_bb.blif"), fx("mul1_logic.blif"))
        self.assertEqual(code, 2)
        self.assertIn("unsupported black box multiply", err)

    def test_no_cut_rejects_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "g.blif"
            path.write_text(BB_HEAD + RAM_Q + BB_TAIL, encoding="utf-8")
            code, _, err = check("--no-cut", str(path), str(path))
        self.assertEqual(code, 2)
        self.assertIn("g.blif:4: unsupported directive .gate", err)

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

    def test_clocking_mismatch(self) -> None:
        for other in ("l_fe.blif", "l_clk2.blif"):
            with self.subTest(other):
                code, out, _ = check(cx("l_re.blif"), cx(other))
                self.assertEqual(code, 1, out)
                self.assertIn("latch clocking differs", out)
        code, out, _ = check(cx("nil.blif"), cx("noctl.blif"))
        self.assertEqual(code, 1, out)
        self.assertIn("('untyped', '-')", out)

    def test_unsupported_clocking(self) -> None:
        cases = {"l_ah.blif": "level-sensitive latch type 'ah'",
                 "clk_internal.blif": "clocked by internal net",
                 "clk_two_domains.blif": "more than one clock domain"}
        for name, message in cases.items():
            with self.subTest(name):
                code, _, err = check(cx("l_re.blif"), cx(name))
                self.assertEqual(code, 2)
                self.assertIn(message, err)

    def test_empty_non_blackbox_model(self) -> None:
        code, _, err = check(cx("bb_a.blif"), cx("bb_b.blif"))
        self.assertEqual(code, 2)
        self.assertIn("empty model 'multiply'; mark it .blackbox", err)

    def test_undriven_output_is_error(self) -> None:
        code, _, err = check(cx("ud_a.blif"), cx("ud_b.blif"))
        self.assertEqual(code, 2)
        self.assertIn("undriven net o reaches primary output o", err)

    def test_help_documents_init_handling(self) -> None:
        code, out, _ = check("--help")
        self.assertEqual(code, 0)
        self.assertIn("init 2 (don't care) and 3 (unknown)", " ".join(out.split()))
        self.assertIn("--strict-init", out)

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


class BlackBoxCutNoAbcTest(unittest.TestCase):
    """Cut-point mode paths that stop before ABC runs."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_unmatched_instance_exit_2(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + RAM_Q.replace("out[0]=q", "out[0]=r") + ".names r a y\n"
                       "11 1\n.end\n")
        code, out, err = check(a, b)
        self.assertEqual(code, 2, out)
        self.assertIn("cannot be matched one-to-one", err)
        self.assertIn("only in A: single_port_ram out[0]=q (a.blif:4)", err)
        self.assertIn("only in B: single_port_ram out[0]=r (b.blif:4)", err)

    def test_instance_in_one_file_only_exit_2(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + ".names a q\n1 1\n" + BB_TAIL)
        code, _, err = check(a, b)
        self.assertEqual(code, 2)
        self.assertIn("only in A: single_port_ram out[0]=q", err)

    def test_width_mismatch_exit_2(self) -> None:
        wide = RAM_Q.replace("out[0]=q", "out[0]=q out[1]=q1").replace(
            "data[0]=b", "data[0]=b data[1]=b")
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + wide + BB_TAIL)
        code, _, err = check(a, b)
        self.assertEqual(code, 2)
        self.assertIn("port widths differ", err)
        self.assertIn("data: 1 in A, 2 in B", err)
        self.assertIn("out: 1 in A, 2 in B", err)

    def test_width_mismatch_is_reported_before_undriven_nets(self) -> None:
        """A narrow block leaves the wide block's upper output bits undriven on side A: the
        cause (the widths) is reported, not the symptom (an undriven net)."""
        head = ".model t\n.inputs clk a b c\n.outputs y z\n"
        wide = RAM_Q.replace("out[0]=q", "out[0]=q out[1]=q1").replace(
            "data[0]=b", "data[0]=b data[1]=b")
        tail = BB_TAIL.replace(".end", ".names q1 z\n1 1\n.end")
        a = self.write("a.blif", head + RAM_Q + tail)
        b = self.write("b.blif", head + wide + tail)
        code, _, err = check(a, b)
        self.assertEqual(code, 2)
        self.assertIn("port widths differ", err)
        self.assertNotIn("undriven", err)

    def test_cell_type_mismatch_exit_2(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + RAM_Q.replace("single_port_ram", "multiply")
                       + BB_TAIL)
        code, _, err = check(a, b)
        self.assertEqual(code, 2)
        self.assertIn("cell types differ", err)
        self.assertIn("single_port_ram", err)
        self.assertIn("multiply", err)

    def test_cname_names_the_instance(self) -> None:
        """A model-less ``.gate`` with a ``.cname`` matches a ``.subckt`` of a black-box model
        with the same ``.cname``, through the Odin II ``top^`` prefix and ``~N`` spelling."""
        gate = BB_HEAD + RAM_Q.replace("q", "w") + ".cname t^ram~1\n" + BB_TAIL.replace("q", "w")
        sub = (BB_HEAD + ".subckt single_port_ram addr[0]=a clk=clk data[0]=b out[0]=w we=c\n"
               ".cname ram[1]\n" + BB_TAIL.replace("q", "w") + ".model single_port_ram\n"
               ".inputs addr[0] clk data[0] we\n.outputs out[0]\n.blackbox\n.end\n")
        a, b = self.write("a.blif", gate), self.write("b.blif", sub)
        with self.assertRaises(equiv_check.EquivError):  # not matched without normalisation
            equiv_check.match_blackboxes(
                [equiv_check.flatten(blif.parse_file(a, gates=True), normalize=False),
                 equiv_check.flatten(blif.parse_file(b, gates=True), normalize=False)])
        flats = [equiv_check.flatten(blif.parse_file(f, gates=True)) for f in (a, b)]
        equiv_check.match_blackboxes(flats)
        self.assertEqual([len(f.blackboxes) for f in flats], [1, 1])

    def test_cut_points_become_ports(self) -> None:
        flat = equiv_check.flatten(blif.parse_text(BB_HEAD + RAM_Q + BB_TAIL, gates=True))
        self.assertIn("bbout:single_port_ram:out[0]=q:out[0]", flat.inputs)
        for formal in ("addr[0]", "clk", "data[0]", "we"):
            self.assertIn(f"bbin:single_port_ram:out[0]=q:{formal}", flat.outputs)
        self.assertNotIn("bbin:single_port_ram:out[0]=q:out[0]", flat.outputs)

    def test_unnameable_instance_exit_2(self) -> None:
        text = (BB_HEAD + RAM_Q.replace("out[0]=q", "out[0]=$auto$7")
                + ".names $auto$7 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check(a, a)
        self.assertEqual(code, 2)
        self.assertIn("no .cname and no named output net", err)

    def test_duplicate_cname_exit_2(self) -> None:
        text = (BB_HEAD + RAM_Q + ".cname ram\n" + RAM_Q.replace("q", "r") + ".cname ram\n"
                + BB_TAIL)
        a = self.write("a.blif", text)
        code, _, err = check(a, a)
        self.assertEqual(code, 2)
        self.assertIn("two black-box instances named", err)

    def test_help_documents_no_cut(self) -> None:
        code, out, _ = check("--help")
        self.assertEqual(code, 0)
        self.assertIn("--no-cut", out)


@needs_abc
class BlackBoxCutAbcTest(unittest.TestCase):
    def setUp(self) -> None:
        if ABC is None:
            self.fail(f"ODIN3_REQUIRE_ABC=1 but no usable ABC: {ABC_PROBLEM}")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_matching_instances_equivalent(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        # Same wiring; the logic around the block is spelled differently.
        b = self.write("b.blif", BB_HEAD + RAM_Q + ".names a q t\n11 1\n.names t y\n1 1\n.end\n")
        code, out, err = check(a, b)
        self.assertEqual(code, 0, out + err)
        self.assertIn("(abc cec)", out)

    def test_different_input_wiring_not_equivalent(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + RAM_Q.replace("data[0]=b", "data[0]=a") + BB_TAIL)
        code, out, err = check(a, b)
        self.assertEqual(code, 1, out + err)
        self.assertIn("NOT equivalent", out)

    def test_different_logic_around_block_not_equivalent(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + RAM_Q + ".names q a y\n10 1\n.end\n")
        self.assertEqual(check(a, b)[0], 1)

    def test_hard_block_output_is_free(self) -> None:
        """The two sides' outputs are the same free input, not a function of the inputs."""
        a = self.write("a.blif", BB_HEAD + RAM_Q + ".names q y\n1 1\n.end\n")
        b = self.write("b.blif", BB_HEAD + RAM_Q + ".names q y\n0 1\n.end\n")
        self.assertEqual(check(a, b)[0], 1)

    def test_dual_port_ram_and_clocked_sequential(self) -> None:
        """A clocked black box beside latches: dsec runs with the outputs cut."""
        body = (".model t\n.inputs clk a b\n.outputs y z\n"
                ".gate dual_port_ram addr1[0]=a addr2[0]=b clk=clk data1[0]=a data2[0]=b "
                "out1[0]=o1 out2[0]=o2 we1=a we2=b\n"
                ".latch d q re clk 0\n.names o1 o2 d\n10 1\n.names q y\n1 1\n"
                ".names o2 z\n1 1\n.end\n")
        a = self.write("a.blif", body)
        b = self.write("b.blif", body.replace(".names o2 z\n1 1", ".names o2 z\n1 1\n"))
        code, out, err = check(a, b)
        self.assertEqual(code, 0, out + err)
        self.assertIn("(abc dsec)", out)


@needs_abc
class EquivCheckAbcTest(unittest.TestCase):
    def setUp(self) -> None:
        if ABC is None:
            self.fail(f"ODIN3_REQUIRE_ABC=1 but no usable ABC: {ABC_PROBLEM}")
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

    def test_latch_init_and_buffered_clock(self) -> None:
        self.assertEqual(check(cx("l_re.blif"), cx("l_init1.blif"))[0], 1)
        code, out, err = check(cx("l_re.blif"), cx("l_init3.blif"))
        self.assertEqual(code, 0, out + err)
        self.assertIn("treated as 0", err)
        code, out, err = check(cx("l_re.blif"), cx("clk_buffered.blif"))
        self.assertEqual(code, 0, out + err)
        code, out, err = check(cx("l_re.blif"), cx("clk_buffered_offset.blif"))
        self.assertEqual(code, 0, out + err)

    def test_latches_on_one_side_only(self) -> None:
        code, out, err = check(cx("mix_a.blif"), cx("mix_b.blif"))
        self.assertEqual(code, 0, out + err)
        self.assertIn("(abc dsec)", out)
        self.assertEqual(check(cx("mix_a.blif"), cx("mix_c.blif"))[0], 1)

    def test_allow_undriven(self) -> None:
        code, out, err = check("--allow-undriven", cx("ud_a.blif"), cx("ud_b.blif"))
        self.assertEqual(code, 0, out + err)
        self.assertIn("undriven net(s) tied to 0", err)

    def test_wrapper_script(self) -> None:
        wrapper = helpers.REPO_ROOT / "tools" / "equiv-check" / "equiv-check"
        assert ABC is not None
        env = dict(os.environ, ODIN3_ABC=ABC)
        result = subprocess.run([str(wrapper), fx("add2_parmys.blif"), fx("add2_logic.blif")],
                                capture_output=True, text=True, check=False, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
