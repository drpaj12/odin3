"""Tests for tools/equiv-check.

Tests that need ABC are skipped (not failed) when no usable ABC binary is found; set
ODIN3_ABC to point at one.  With ODIN3_REQUIRE_ABC=1 (set by the CTest run) a missing or
unusable ABC makes those tests FAIL instead.  Tests that stop before ABC runs (input
errors, interface or clocking mismatches) always run.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

import bbmatch
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
        self.assertIn("only in A: single_port_ram out=q (a.blif:4)", err)
        self.assertIn("only in B: single_port_ram out=r (b.blif:4)", err)

    def test_instance_in_one_file_only_exit_2(self) -> None:
        a = self.write("a.blif", BB_HEAD + RAM_Q + BB_TAIL)
        b = self.write("b.blif", BB_HEAD + ".names a q\n1 1\n" + BB_TAIL)
        code, _, err = check(a, b)
        self.assertEqual(code, 2)
        self.assertIn("only in A: single_port_ram out=q", err)

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

    def test_one_bit_port_spellings_match(self) -> None:
        """Yosys writes a one-bit port ``out``, Parmys and Odin II ``out[0]``."""
        text = (".model t\n.inputs a\n.outputs y\n{} we{}=a out{}=u\n.names u y\n1 1\n.end\n")
        sub = text.format(".subckt single_port_ram", "[0]", "[0]") + (
            ".model single_port_ram\n.inputs we[0]\n.outputs out[0]\n.blackbox\n.end\n")
        gate = text.format(".gate single_port_ram", "", "")
        flats = [equiv_check.flatten(blif.parse_text(sub)),
                 equiv_check.flatten(blif.parse_text(gate, gates=True))]
        equiv_check.match_blackboxes(flats)
        self.assertEqual({f.blackboxes[0].key for f in flats}, {"out=u"})
        self.assertEqual(flats[0].inputs, flats[1].inputs)
        self.assertEqual(flats[0].outputs, flats[1].outputs)

    def test_cut_points_become_ports(self) -> None:
        flat = equiv_check.flatten(blif.parse_text(BB_HEAD + RAM_Q + BB_TAIL, gates=True))
        self.assertIn("bbout:single_port_ram:out=q:out", flat.inputs)
        for formal in ("addr", "clk", "data", "we"):
            self.assertIn(f"bbin:single_port_ram:out=q:{formal}", flat.outputs)
        self.assertNotIn("bbin:single_port_ram:out=q:out", flat.outputs)

    def test_unnameable_instance_exit_2(self) -> None:
        text = (BB_HEAD + RAM_Q.replace("out[0]=q", "out[0]=$auto$7")
                + ".names $auto$7 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check(a, a)
        self.assertEqual(code, 2)
        self.assertIn("no .cname and no named output net", err)

    def test_match_unnamed_ignores_output_net_names(self) -> None:
        """Without a .cname a box is anonymous with --match-unnamed, whatever its nets are called;
        the same netlist twice pairs with itself."""
        text = (BB_HEAD + RAM_Q.replace("out[0]=q", "out[0]=$mul~7")
                + ".names $mul~7 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check("--match-unnamed", a, a)
        self.assertEqual(code, 0 if ABC else 2, err)
        flats = [equiv_check.flatten(blif.parse_file(a, gates=True), match_unnamed=True)
                 for _ in range(2)]
        self.assertTrue(all(f.blackboxes[0].anon for f in flats))
        self.assertEqual(equiv_check.pair_unnamed(flats), 1)
        equiv_check.match_blackboxes(flats)
        self.assertEqual(flats[0].blackboxes[0].key, flats[1].blackboxes[0].key)

    def test_match_dollar_is_the_old_spelling(self) -> None:
        text = (BB_HEAD + RAM_Q.replace("out[0]=q", "out[0]=$mul~7")
                + ".names $mul~7 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check("--match-dollar", a, a)
        self.assertEqual(code, 0 if ABC else 2, err)

    def test_cname_still_names_an_instance_with_match_unnamed(self) -> None:
        flat = equiv_check.flatten(blif.parse_text(BB_HEAD + RAM_Q + ".cname ram\n" + BB_TAIL,
                                                   gates=True), match_unnamed=True)
        self.assertFalse(flat.blackboxes[0].anon)
        self.assertEqual(flat.blackboxes[0].key, "ram")

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


X_HEAD = ".model t\n.inputs a b\n.outputs o p\n"
X_P = ".names a b p\n11 1\n.end\n"


@needs_abc
class XReportTest(unittest.TestCase):
    """--x-report: outputs in the forward cone of ``unconn`` are removed from the comparison."""

    def setUp(self) -> None:
        if ABC is None:
            self.fail(f"ODIN3_REQUIRE_ABC=1 but no usable ABC: {ABC_PROBLEM}")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def x_pair(self, p_b: str = X_P) -> tuple[str, str]:
        """A: o = a & unconn (x kept as 0). B: o = a (x resolved to 1)."""
        a = self.write("a.blif", X_HEAD + ".names unconn\n.names a unconn o\n11 1\n" + X_P)
        b = self.write("b.blif", X_HEAD + ".names a o\n1 1\n" + p_b)
        return a, b

    def test_equivalent_outside_x_cone_exit_3(self) -> None:
        a, b = self.x_pair()
        self.assertEqual(check(a, b)[0], 1)
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 3, out + err)
        self.assertIn("equivalent outside x-cones (abc cec)", out)
        self.assertIn("x-report: 1 of 2 outputs are in an x-cone", out)
        self.assertEqual([line for line in out.splitlines() if line.startswith("x-cone:")],
                         ["x-cone: o"])

    def test_difference_outside_the_cone_is_still_not_equivalent(self) -> None:
        a, b = self.x_pair(".names a b p\n1- 1\n.end\n")
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 1, out + err)
        self.assertIn("NOT equivalent outside x-cones", out)
        self.assertNotIn("x-cone: o", out)

    def test_no_unconn_means_no_cone(self) -> None:
        a = self.write("a.blif", X_HEAD + ".names a o\n1 1\n" + X_P)
        b = self.write("b.blif", X_HEAD + ".names a o\n0 1\n" + X_P)
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 1, out + err)
        self.assertIn("no output is in an x-cone", out)

    def test_equivalent_stays_exit_0_without_cone_lines(self) -> None:
        a = self.write("a.blif", X_HEAD + ".names a o\n1 1\n" + X_P)
        code, out, err = check("--x-report", a, a)
        self.assertEqual(code, 0, out + err)
        self.assertNotIn("x-cone", out)

    def test_every_output_in_a_cone_compares_nothing(self) -> None:
        a = self.write("a.blif", ".model t\n.inputs a\n.outputs o\n.names unconn\n"
                       ".names a unconn o\n11 1\n.end\n")
        b = self.write("b.blif", ".model t\n.inputs a\n.outputs o\n.names a o\n1 1\n.end\n")
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 1, out + err)
        self.assertIn("no output is left to compare", out)

    def test_x_cone_passes_through_a_latch(self) -> None:
        """unconn -> cover -> latch D: the latch's Q, and the output behind it, are removed."""
        head = ".model t\n.inputs clk a\n.outputs q r\n"
        tail = ".names a r\n1 1\n.end\n"
        a = self.write("a.blif", head + ".names unconn\n.names a unconn d\n11 1\n"
                       ".latch d q re clk 0\n" + tail)
        b = self.write("b.blif", head + ".names a d\n1 1\n.latch d q re clk 0\n" + tail)
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 3, out + err)
        self.assertIn("(abc dsec)", out)
        self.assertIn("x-cone: q", out)
        self.assertNotIn("x-cone: r", out)

    def test_x_in_the_other_netlist_counts_too(self) -> None:
        a, b = self.x_pair()
        code, out, err = check("--x-report", b, a)
        self.assertEqual(code, 3, out + err)
        self.assertIn("x-cone: o", out)

    def test_help_documents_the_flag(self) -> None:
        code, out, _ = check("--help")
        self.assertEqual(code, 0)
        self.assertIn("--x-report", out)


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


class UnnamedBlackBoxTest(unittest.TestCase):
    """--match-unnamed: boxes without a .cname are paired by simulation signature."""

    HEAD = ".model t\n.inputs clk a b c\n.outputs y z\n"

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    @staticmethod
    def ram(out: str, addr: str = "a", data: str = "b", we: str = "c") -> str:
        return (f".gate single_port_ram addr[0]={addr} clk=clk data[0]={data} out[0]={out} "
                f"we={we}\n")

    def flats(self, text_a: str, text_b: str) -> list[equiv_check.Flat]:
        return [equiv_check.flatten(blif.parse_text(t, gates=True), match_unnamed=True,
                                    check=False) for t in (text_a, text_b)]

    def two_boxes(self, first: str, second: str, swap: bool = False) -> str:
        """Two boxes of different inputs, each read by one output; ``swap`` lists them in the
        other order."""
        boxes = [self.ram(first, "a", "b"), self.ram(second, "b", "a")]
        return (self.HEAD + "".join(boxes[::-1] if swap else boxes)
                + f".names {first} c y\n11 1\n.names {second} c z\n10 1\n.end\n")

    def test_unique_signatures_pair_whatever_the_names_and_order(self) -> None:
        flats = self.flats(self.two_boxes("$m~1", "$m~2"),
                           self.two_boxes("$m~9", "$m~4", swap=True))
        self.assertEqual(equiv_check.pair_unnamed(flats), 2)
        equiv_check.match_blackboxes(flats)
        by_inputs = [{(b.inputs["addr"], b.inputs["data"]): b.key for b in f.blackboxes}
                     for f in flats]
        self.assertEqual(by_inputs[0], by_inputs[1])
        self.assertEqual(len(set(by_inputs[0].values())), 2)
        self.assertEqual(sorted(flats[0].inputs), sorted(flats[1].inputs))

    def test_interchangeable_twins_pair_in_order_and_share_outputs(self) -> None:
        def twins(first: str, second: str, y_from: str, z_from: str) -> str:
            return (self.HEAD + self.ram(first) + self.ram(second)
                    + f".names {y_from} c y\n11 1\n.names {z_from} z\n1 1\n.end\n")

        flats = self.flats(twins("$t~1", "$t~2", "$t~1", "$t~2"),
                           twins("$t~9", "$t~4", "$t~4", "$t~9"))
        self.assertEqual(equiv_check.pair_unnamed(flats), 2)
        equiv_check.match_blackboxes(flats)
        for flat in flats:
            self.assertEqual([n for n in flat.inputs if n.startswith("bbout:")],
                             ["bbout:single_port_ram:?anon1:out"])
        self.assertEqual(sorted(flats[0].inputs), sorted(flats[1].inputs))
        self.assertEqual(sorted(flats[0].outputs), sorted(flats[1].outputs))

    def test_equivalent_with_twins_swapped_exit_0(self) -> None:
        if ABC is None and not REQUIRE_ABC:
            self.skipTest("no ABC")
        a = self.write("a.blif", self.HEAD + self.ram("$t~1") + self.ram("$t~2")
                       + ".names $t~1 c y\n11 1\n.names $t~2 z\n1 1\n.end\n")
        b = self.write("b.blif", self.HEAD + self.ram("$t~9") + self.ram("$t~4")
                       + ".names $t~4 c y\n11 1\n.names $t~9 z\n1 1\n.end\n")
        code, out, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 0, out + err)

    def test_same_signature_different_nets_is_ambiguous_exit_2(self) -> None:
        """a2 computes the same function as a, but through another net: the two boxes cannot be
        told apart by their inputs, and are not interchangeable (a and a2 are different nets)."""
        def text(first: str, second: str) -> str:
            return (self.HEAD + ".names a b a2\n11 1\n10 1\n" + self.ram(first, "a")
                    + self.ram(second, "a2") + f".names {first} {second} y\n11 1\n"
                    ".names c z\n1 1\n.end\n")

        a = self.write("a.blif", text("$m~1", "$m~2"))
        b = self.write("b.blif", text("$m~7", "$m~8"))
        code, _, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 2, err)
        self.assertIn("ambiguous: 2 in A and 2 in B", err)

    def test_box_whose_inputs_differ_is_left_over_exit_2(self) -> None:
        a = self.write("a.blif", self.HEAD + self.ram("$m~1", "a") + self.ram("$m~2", "b")
                       + ".names $m~1 $m~2 y\n11 1\n.names c z\n1 1\n.end\n")
        b = self.write("b.blif", self.HEAD + self.ram("$m~1", "a") + self.ram("$m~2", "c")
                       + ".names $m~1 $m~2 y\n11 1\n.names c z\n1 1\n.end\n")
        code, _, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 2, err)
        self.assertIn("cannot be matched one-to-one", err)
        self.assertIn("only in A: single_port_ram unnamed, out=$m~2 (a.blif:5)", err)
        self.assertIn("only in B: single_port_ram unnamed, out=$m~2 (b.blif:5)", err)

    def test_different_counts_exit_2(self) -> None:
        a = self.write("a.blif", self.HEAD + self.ram("$m~1") + self.ram("$m~2")
                       + ".names $m~1 $m~2 y\n11 1\n.names c z\n1 1\n.end\n")
        b = self.write("b.blif", self.HEAD + self.ram("$m~1")
                       + ".names $m~1 c y\n11 1\n.names c z\n1 1\n.end\n")
        code, _, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 2, err)
        self.assertIn("only in A", err)

    def test_named_boxes_stay_named_and_are_not_paired(self) -> None:
        text = (self.HEAD + self.ram("$m~1") + ".cname named\n" + self.ram("$m~2", "b", "a")
                + ".names $m~1 $m~2 y\n11 1\n.names c z\n1 1\n.end\n")
        flats = self.flats(text, text)
        self.assertEqual([b.anon for b in flats[0].blackboxes], [False, True])
        self.assertEqual(equiv_check.pair_unnamed(flats), 1)
        equiv_check.match_blackboxes(flats)

    def test_pairs_through_latches_with_other_names(self) -> None:
        """Box 2 reads a latch that box 1 feeds; the latches and nets are named differently."""
        def text(m1: str, m2: str, latch: str) -> str:
            return (self.HEAD + self.ram(m1) + f".latch {m1} {latch} re clk 0\n"
                    + self.ram(m2, latch, "b") + f".names {m2} c y\n11 1\n"
                    f".names {latch} z\n1 1\n.end\n")

        flats = self.flats(text("$m~1", "$m~2", "$dff~3^Q~0"), text("$m~5", "$m~6", "q"))
        self.assertEqual(equiv_check.pair_unnamed(flats), 2)
        if ABC is None and not REQUIRE_ABC:
            return
        code, out, err = check("--match-unnamed", self.write("a.blif", text("$m~1", "$m~2", "x")),
                               self.write("b.blif", text("$m~5", "$m~6", "q")))
        self.assertEqual(code, 0, out + err)

    @needs_abc
    def test_a_wrong_pairing_is_never_equivalent(self) -> None:
        """Cross the pairing by hand: the check on the paired cut points must reject it."""
        flats = self.flats(self.two_boxes("$m~1", "$m~2"), self.two_boxes("$m~1", "$m~2"))
        self.assertEqual(equiv_check.pair_unnamed(flats), 2)
        args = argparse.Namespace(timeout=60, keep_temp=False)
        assert ABC is not None

        def verdict() -> bool:
            equiv_check.match_blackboxes(flats)
            for flat in flats:
                equiv_check.check_undriven(flat, False)
            maps = [equiv_check.io_map(f, True) for f in flats]
            return equiv_check.prove(flats, maps, ABC, args)[0]

        self.assertTrue(verdict())
        first, second = (b.key for b in flats[1].blackboxes)
        equiv_check._rekey(flats[1], {first: second, second: first})
        self.assertFalse(verdict())

    @needs_abc
    def test_wrong_pairing_of_twins_with_other_logic_is_rejected(self) -> None:
        """Boxes with equal signatures whose inputs the netlists wire differently: not
        equivalent (exit 1) or left over (exit 2), never 0."""
        a = self.write("a.blif", self.two_boxes("$m~1", "$m~2"))
        b = self.write("b.blif", self.two_boxes("$m~1", "$m~2").replace(
            ".names $m~1 c y\n11 1\n.names $m~2 c z\n10 1", ".names $m~2 c y\n11 1\n"
            ".names $m~1 c z\n10 1"))
        code, out, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 1, out + err)


class BbmatchTest(unittest.TestCase):
    def sig(self, digest: bytes, *identity: str) -> bbmatch.Signature:
        return bbmatch.Signature(digest, identity)

    def test_unique_and_twins(self) -> None:
        a = [self.sig(b"1", "x"), self.sig(b"2", "y"), self.sig(b"2", "y")]
        b = [self.sig(b"2", "u"), self.sig(b"2", "u"), self.sig(b"1", "w")]
        result = bbmatch.pair(a, b)
        self.assertEqual(result.pairs, [(0, 2), (1, 0), (2, 1)])
        self.assertEqual(result.twins, [([1, 2], [0, 1])])
        self.assertFalse(result.ambiguous or result.only_a or result.only_b)

    def test_not_interchangeable_is_ambiguous(self) -> None:
        a = [self.sig(b"1", "x"), self.sig(b"1", "y")]
        b = [self.sig(b"1", "x"), self.sig(b"1", "x")]
        result = bbmatch.pair(a, b)
        self.assertEqual((result.pairs, result.ambiguous), ([], [([0, 1], [0, 1])]))

    def test_counts_must_agree(self) -> None:
        result = bbmatch.pair([self.sig(b"1", "x"), self.sig(b"1", "x")], [self.sig(b"1", "x")])
        self.assertEqual((result.pairs, result.only_a, result.only_b), ([], [0, 1], [0]))

    def test_combinational_loop_is_a_sim_error(self) -> None:
        loop = [blif.Names(inputs=["q"], output="p", rows=[("1", "1")]),
                blif.Names(inputs=["p"], output="q", rows=[("1", "1")])]
        with self.assertRaises(bbmatch.SimError):
            bbmatch.simulate(loop, [], [], [], lambda n: n)


if __name__ == "__main__":
    unittest.main()
