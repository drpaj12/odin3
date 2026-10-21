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
from unittest import mock

import bbmatch
import blif
import equiv_check
import rampair

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
# A generated net feeding the box's ``we`` pin: the box then has no input signature either.
GEN_IN = ".names c $g\n1 1\n"


class FindAbcTest(unittest.TestCase):
    def test_release_build_comes_before_a_sanitizer_build(self) -> None:
        """A sanitizer ABC (clangdbg) sorts first by name and is several times slower on dsec."""
        with tempfile.TemporaryDirectory() as tmp:
            for preset in ("clangdbg", "debug", "release"):
                exe = Path(tmp) / "build" / preset / "third_party" / "abc" / "abc"
                exe.parent.mkdir(parents=True)
                exe.write_text("#!/bin/sh\n", encoding="utf-8")
                exe.chmod(0o755)
            saved = (equiv_check.REPO_ROOT, os.environ.pop("ODIN3_ABC", None))
            equiv_check.REPO_ROOT = Path(tmp)
            try:
                want = Path(tmp, "build", "release", "third_party", "abc", "abc")
                self.assertEqual(equiv_check.find_abc(), str(want))
            finally:
                equiv_check.REPO_ROOT = saved[0]
                if saved[1] is not None:
                    os.environ["ODIN3_ABC"] = saved[1]


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
        cases = {"l_ah.blif": ("level-sensitive latch type 'ah'", 2),
                 "clk_internal.blif": ("clocked by internal net", 2),
                 # a tool limit, not a bad input: exit 4 (unproven)
                 "clk_two_domains.blif": ("unproven: ", 4)}
        for name, (message, want) in cases.items():
            with self.subTest(name):
                code, _, err = check(cx("l_re.blif"), cx(name))
                self.assertEqual(code, want)
                self.assertIn(message, err)
        self.assertIn("more than one clock domain", check(cx("l_re.blif"),
                                                          cx("clk_two_domains.blif"))[2])

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

    def test_param_mismatch_exit_2(self) -> None:
        """I2 (PR 2D-7b review): matched boxes with different ``.param`` lists are not the same
        function; the cut must not call them equivalent (``.gate`` and ``.subckt`` forms)."""
        gate = ".model top\n.inputs a b\n.outputs y\n.gate mybox x=a w=b out=y\n{}.end\n"
        sub = (".model top\n.inputs a b\n.outputs y\n.subckt mybox x=a w=b out=y\n{}.end\n"
               ".model mybox\n.inputs x w\n.outputs out\n.blackbox\n.end\n")
        for name, form in (("gate", gate), ("subckt", sub)):
            a = self.write(f"{name}_a.blif", form.format(".param MODE 01\n"))
            b = self.write(f"{name}_b.blif", form.format(".param MODE 10\n"))
            code, out, err = check(a, b)
            self.assertEqual(code, 2, f"{name}: {out}{err}")
            self.assertIn("MODE", err)
            self.assertIn("parameters differ", err)
            self.assertIn("mybox", err)
            none = self.write(f"{name}_none.blif", form.format(""))
            code, _, err = check(a, none)  # a parameter on one side only differs too
            self.assertEqual(code, 2, name)
            self.assertIn("parameters differ", err)

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
        text = (BB_HEAD + GEN_IN + RAM_Q.replace("we=c", "we=$g").replace("out[0]=q", "out[0]=$auto$7")
                + ".names $auto$7 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check(a, a)
        self.assertEqual(code, 2)
        self.assertIn("no .cname and no named output net", err)

    def test_generated_output_net_is_named_through_its_port_buffer(self) -> None:
        """write_blif --vtr: the box output is a generated ``$n`` net read by one buffer that
        drives a named net; the box takes the name of the buffer's output."""
        text = (BB_HEAD + RAM_Q.replace("out[0]=q", "out[0]=$n7")
                + ".names $n7 q\n1 1\n" + BB_TAIL)
        flat = equiv_check.flatten(blif.parse_text(text, gates=True))
        self.assertEqual([b.key for b in flat.blackboxes], ["out=q"])
        plain = equiv_check.flatten(blif.parse_text(BB_HEAD + RAM_Q + BB_TAIL, gates=True))
        self.assertEqual([b.key for b in plain.blackboxes], ["out=q"])

    def test_generated_output_net_with_two_readers_exit_2(self) -> None:
        """No unique buffer (the net feeds a buffer and more logic): still unnameable."""
        text = (BB_HEAD + GEN_IN + RAM_Q.replace("we=c", "we=$g").replace("out[0]=q", "out[0]=$n7")
                + ".names $n7 q\n1 1\n.names $n7 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check(a, a)
        self.assertEqual(code, 2)
        self.assertIn("no .cname and no named output net", err)

    def test_generated_output_net_behind_a_generated_buffer_exit_2(self) -> None:
        text = (BB_HEAD + GEN_IN + RAM_Q.replace("we=c", "we=$g").replace("out[0]=q", "out[0]=$n7")
                + ".names $n7 $n8\n1 1\n.names $n8 a y\n11 1\n.end\n")
        a = self.write("a.blif", text)
        code, _, err = check(a, a)
        self.assertEqual(code, 2)
        self.assertIn("no .cname and no named output net", err)

    def test_match_unnamed_compares_params_of_every_pair(self) -> None:
        """A box with a different ``.param`` list never pairs, named or by signature: exit 2."""
        for cname in ("", ".cname ram\n"):
            a = self.write("a.blif", BB_HEAD + RAM_Q + ".param MODE 01\n" + cname + BB_TAIL)
            b = self.write("b.blif", BB_HEAD + RAM_Q + ".param MODE 10\n" + cname + BB_TAIL)
            code, _, err = check("--match-unnamed", a, b)
            self.assertEqual(code, 2, f"{cname!r}: {err}")
            self.assertIn("MODE" if cname else "single_port_ram", err)

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
        self.assertEqual(equiv_check.pair_unnamed(flats), (1, 0))
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
class InputSignaturePairingTest(unittest.TestCase):
    """Boxes with no name at all (no .cname, no named output net, no port buffer) are paired by
    input signature: model plus the named net or paired box that drives each input pin."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    @staticmethod
    def box(out: str, a: str = "a", b: str = "b") -> str:
        return f".gate multiply a[0]={a} b[0]={b} out[0]={out}\n"

    @staticmethod
    def read(*nets: str) -> str:
        """Two readers per net (no port buffer), so a generated net stays unnameable."""
        return "".join(f".names {n} a y{i}\n11 1\n.names {n} b z{i}\n10 1\n"
                       for i, n in enumerate(nets))

    HEAD = ".model t\n.inputs a b\n.outputs {}\n"

    def netlist(self, boxes: str, nets: list[str]) -> str:
        outs = " ".join(f"{p}{i}" for i in range(len(nets)) for p in "yz")
        return self.HEAD.format(outs) + boxes + self.read(*nets) + ".end\n"

    def flats(self, text_a: str, text_b: str) -> list[equiv_check.Flat]:
        return [equiv_check.flatten(blif.parse_text(t, gates=True), check=False)
                for t in (text_a, text_b)]

    def test_pairs_by_signature_whatever_the_output_nets_and_order(self) -> None:
        a = self.netlist(self.box("$n1", "a", "b") + self.box("$n2", "b", "a"), ["$n1", "$n2"])
        b = self.netlist(self.box("$n9", "b", "a") + self.box("$n8", "a", "b"), ["$n8", "$n9"])
        flats = self.flats(a, b)
        self.assertEqual(equiv_check.pair_by_input_signature(flats), 2)
        equiv_check.match_blackboxes(flats)
        self.assertEqual(sorted(x.key for x in flats[0].blackboxes),
                         sorted(x.key for x in flats[1].blackboxes))
        by_in = [{(x.inputs["a"], x.inputs["b"]): x.key for x in f.blackboxes} for f in flats]
        self.assertEqual(by_in[0], by_in[1])

    def test_same_signature_twice_in_one_file_is_an_error(self) -> None:
        twice = self.netlist(self.box("$n1") + self.box("$n2"), ["$n1", "$n2"])
        once = self.netlist(self.box("$n1"), ["$n1"])
        for pair in ((twice, once), (once, twice), (twice, twice)):
            with self.assertRaises(equiv_check.EquivError) as ctx:
                equiv_check.pair_by_input_signature(self.flats(*pair))
            self.assertIn("cannot name black box multiply", str(ctx.exception))
            self.assertIn("same input signature", str(ctx.exception))

    def test_signature_on_one_side_only_is_an_error(self) -> None:
        a = self.netlist(self.box("$n1", "a", "b"), ["$n1"])
        b = self.netlist(self.box("$n1", "b", "a"), ["$n1"])
        with self.assertRaises(equiv_check.EquivError) as ctx:
            equiv_check.pair_by_input_signature(self.flats(a, b))
        self.assertIn("cannot name black box multiply", str(ctx.exception))
        self.assertIn("no .cname and no named output net", str(ctx.exception))
        self.assertIn("no box of the other file", str(ctx.exception))
        gone = self.HEAD.format("y0 z0") + self.read("a") + ".end\n"
        with self.assertRaises(equiv_check.EquivError):
            equiv_check.pair_by_input_signature(self.flats(a, gone))

    def test_model_is_part_of_the_signature(self) -> None:
        a = self.netlist(self.box("$n1"), ["$n1"])
        b = self.netlist(self.box("$n1").replace("multiply", "adder"), ["$n1"])
        with self.assertRaises(equiv_check.EquivError):
            equiv_check.pair_by_input_signature(self.flats(a, b))

    def test_chain_of_unnamed_boxes_pairs_through_the_fixpoint(self) -> None:
        def chain(x: str, y: str, order: int) -> str:
            first, second = self.box(x), self.box(y, a=x)
            return self.netlist(first + second if order else second + first, [y])
        flats = self.flats(chain("$n1", "$n2", 0), chain("$n7", "$n8", 1))
        self.assertEqual(equiv_check.pair_by_input_signature(flats), 2)
        equiv_check.match_blackboxes(flats)

    def test_chain_second_box_signature_differs_is_an_error(self) -> None:
        a = self.netlist(self.box("$n1") + self.box("$n2", a="$n1"), ["$n2"])
        b = self.netlist(self.box("$n1") + self.box("$n2", b="$n1"), ["$n2"])
        with self.assertRaises(equiv_check.EquivError):
            equiv_check.pair_by_input_signature(self.flats(a, b))

    def test_generated_input_net_cannot_be_signed(self) -> None:
        text = (self.HEAD.format("y0 z0") + ".names a $g\n1 1\n" + self.box("$n1", a="$g")
                + self.read("$n1") + ".end\n")
        with self.assertRaises(equiv_check.EquivError) as ctx:
            equiv_check.pair_by_input_signature(self.flats(text, text))
        self.assertIn("generated net", str(ctx.exception))

    def test_named_and_cname_boxes_are_left_alone(self) -> None:
        named = self.HEAD.format("y0") + self.box("q") + ".names q a y0\n11 1\n.end\n"
        flats = self.flats(named, named)
        self.assertEqual(equiv_check.pair_by_input_signature(flats), 0)
        self.assertEqual([x.key for x in flats[0].blackboxes], ["out=q"])
        cname = (self.HEAD.format("y0") + self.box("$n1") + ".cname foo\n"
                 + ".names $n1 a y0\n11 1\n.names $n1 b z\n11 1\n.end\n")
        flats = self.flats(cname, cname)
        self.assertEqual(equiv_check.pair_by_input_signature(flats), 0)
        self.assertEqual([x.key for x in flats[0].blackboxes], ["foo"])

    def test_unpaired_box_still_stops_match_blackboxes(self) -> None:
        a = self.netlist(self.box("$n1"), ["$n1"])
        with self.assertRaises(equiv_check.EquivError) as ctx:
            equiv_check.match_blackboxes(self.flats(a, a))
        self.assertIn("cannot name black box", str(ctx.exception))

    def test_cli_pairs_notes_and_proves(self) -> None:
        if ABC is None and not REQUIRE_ABC:
            self.skipTest(f"ABC not available ({ABC_PROBLEM})")
        a = self.write("a.blif", self.netlist(self.box("$n1") + self.box("$n2", "b", "a"),
                                              ["$n1", "$n2"]))
        b = self.write("b.blif", self.netlist(self.box("$n5", "b", "a") + self.box("$n6"),
                                              ["$n6", "$n5"]))
        code, out, err = check(a, b)
        self.assertEqual(code, 0, out + err)
        self.assertEqual(err.count("paired by input signature (pair"), 4)
        self.assertIn("2 unnamed black-box instance(s) paired by input signature", err)

    def test_cli_wrong_pairing_input_still_not_equivalent(self) -> None:
        if ABC is None and not REQUIRE_ABC:
            self.skipTest(f"ABC not available ({ABC_PROBLEM})")
        a = self.write("a.blif", self.netlist(self.box("$n1"), ["$n1"]))
        b = self.write("b.blif", self.netlist(self.box("$n1"), ["$n1"]).replace(
            ".names $n1 a y0\n11 1", ".names $n1 a y0\n10 1"))
        self.assertEqual(check(a, b)[0], 1)

    def test_cli_unmatched_exit_2(self) -> None:
        a = self.write("a.blif", self.netlist(self.box("$n1", "a", "b"), ["$n1"]))
        b = self.write("b.blif", self.netlist(self.box("$n1", "b", "a"), ["$n1"]))
        code, _, err = check(a, b)
        self.assertEqual(code, 2)
        self.assertIn("cannot name black box multiply", err)


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


MUL_MODEL = ".model multiply\n.inputs a[0] b[0]\n.outputs out[0]\n.blackbox\n.end\n"


def mul_netlist(boxes: int, feed: str = "a", tail: str = ".names p0 y\n1 1\n") -> str:
    """A top model with ``boxes`` multiply instances (no .cname, outputs numbered like Parmys's)
    whose first input is ``feed``; ``tail`` is the logic after them."""
    head = ".model t\n.inputs a b\n.outputs y\n"
    cells = "".join(f".subckt multiply a[0]={feed if i == 0 else 'b'} b[0]=b out[0]=p{i}\n"
                    for i in range(boxes))
    return head + cells + tail + ".end\n" + MUL_MODEL


class BlackboxOptionTest(unittest.TestCase):
    """``--blackbox M[,M...]`` (3D Task 19): pairing by .cname or occurrence order."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_unequal_counts_exit_2_with_the_exact_text(self) -> None:
        a = self.write("a.blif", mul_netlist(3))
        b = self.write("b.blif", mul_netlist(2))
        code, out, err = check("--blackbox", "multiply", a, b)
        self.assertEqual(code, 2, out)
        self.assertIn(f"equiv-check: --blackbox: model 'multiply' has 3 instances in {a} and 2 in "
                      f"{b}", err)

    def test_occurrence_order_pairs_unnamed_instances(self) -> None:
        a = equiv_check.flatten(blif.parse_file(self.write("a.blif", mul_netlist(2)), gates=True),
                                blackbox=frozenset({"multiply"}))
        b = equiv_check.flatten(blif.parse_file(self.write("b.blif", mul_netlist(2)), gates=True),
                                blackbox=frozenset({"multiply"}))
        self.assertEqual(equiv_check.pair_listed([a, b], frozenset({"multiply"})), 2)
        self.assertEqual([x.key for x in a.blackboxes], [x.key for x in b.blackboxes])
        equiv_check.match_blackboxes([a, b])

    def test_cname_pairs_before_occurrence(self) -> None:
        text = mul_netlist(2).replace("out[0]=p0\n", "out[0]=p0\n.cname mulA\n", 1)
        swapped = text.replace(".subckt multiply a[0]=a b[0]=b out[0]=p0\n.cname mulA\n"
                               ".subckt multiply a[0]=b b[0]=b out[0]=p1\n",
                               ".subckt multiply a[0]=b b[0]=b out[0]=p1\n"
                               ".subckt multiply a[0]=a b[0]=b out[0]=p0\n.cname mulA\n")
        self.assertNotEqual(text, swapped)
        a = equiv_check.flatten(blif.parse_file(self.write("a.blif", text), gates=True),
                                blackbox=frozenset({"multiply"}))
        b = equiv_check.flatten(blif.parse_file(self.write("b.blif", swapped), gates=True),
                                blackbox=frozenset({"multiply"}))
        equiv_check.pair_listed([a, b], frozenset({"multiply"}))
        equiv_check.match_blackboxes([a, b])
        by_a = {x.key: x.inputs["a"] for x in a.blackboxes}
        by_b = {x.key: x.inputs["a"] for x in b.blackboxes}
        self.assertEqual(by_a, by_b)

    def test_help_documents_blackbox(self) -> None:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), self.assertRaises(SystemExit):
            equiv_check.main(["--help"])
        self.assertIn("--blackbox", buf.getvalue())


@needs_abc
class BlackboxOptionAbcTest(unittest.TestCase):
    def setUp(self) -> None:
        if ABC is None:
            self.fail(f"ODIN3_REQUIRE_ABC=1 but no usable ABC: {ABC_PROBLEM}")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_differing_only_in_soft_logic_is_equivalent(self) -> None:
        a = self.write("a.blif", mul_netlist(1))
        b = self.write("b.blif", mul_netlist(1, tail=".names p0 t\n1 1\n.names t y\n1 1\n"))
        code, out, err = check("--blackbox", "multiply", a, b)
        self.assertEqual(code, 0, out + err)

    def test_a_change_in_what_feeds_a_box_input_is_not_equivalent(self) -> None:
        a = self.write("a.blif", mul_netlist(1))
        b = self.write("b.blif", mul_netlist(1, feed="b"))
        code, out, err = check("--blackbox", "multiply", a, b)
        self.assertEqual(code, 1, out + err)
        self.assertIn("NOT equivalent", out)


@needs_abc
class DualPortOrientTest(unittest.TestCase):
    """--match-unnamed turns every unnamed dual_port_ram to one port order: Parmys lists the two
    ports in hash-table order, so the same memory can come out with ports 1 and 2 exchanged
    (micros common/2r2w and syntax/inferred_DPram, 2H sweep of 2026-10-14)."""

    HEAD = ".model t\n.inputs clk a b da db wa wb\n.outputs qa qb\n"

    def ram(self, swap: bool = False, cross_data: bool = False, invert: bool = False,
            keep_consumers: bool = False, cross_we: bool = False, cross_addr: bool = False,
            tied: bool = False) -> str:
        """Port 1 = (a, da, wa), port 2 = (b, db, wb); ``swap`` lists them the other way round
        (and reads the outputs the other way round); ``cross_data`` and ``invert`` are different
        designs; ``keep_consumers`` swaps the ports but not the consumers (a different design);
        ``cross_we`` / ``cross_addr`` give each port the other's write enable / address;
        ``tied`` gives both ports the same inputs."""
        ports = [("a", "da", "wa"), ("b", "db", "wb")]
        if cross_data:
            ports = [("a", "db", "wa"), ("b", "da", "wb")]
        if cross_we:
            ports = [("a", "da", "wb"), ("b", "db", "wa")]
        if cross_addr:
            ports = [("b", "da", "wa"), ("a", "db", "wb")]
        if tied:
            ports = [("a", "da", "wa")] * 2
        if swap:
            ports.reverse()
        swap = swap and not keep_consumers
        (a1, d1, w1), (a2, d2, w2) = ports
        return (self.HEAD
                + f".gate dual_port_ram addr1[0]={a1} addr2[0]={a2} clk=clk data1[0]={d1} "
                f"data2[0]={d2} out1[0]=dual_port_ram^MEM~1-0^out1~0 "
                f"out2[0]=dual_port_ram^MEM~1-0^out2~1 we1={w1} we2={w2}\n"
                + f".names dual_port_ram^MEM~1-0^out1~0 {'qb' if swap else 'qa'}\n"
                f"{'0' if invert else '1'} 1\n"
                f".names dual_port_ram^MEM~1-0^out2~1 {'qa' if swap else 'qb'}\n1 1\n"
                + ".end\n")

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_check(self, text_a: str, text_b: str, *flags: str) -> tuple[int, str, str]:
        assert ABC is not None
        paths = []
        for name, text in (("a.blif", text_a), ("b.blif", text_b)):
            path = Path(self.tmp.name) / name
            path.write_text(text, encoding="utf-8")
            paths.append(str(path))
        return check("--match-unnamed", "--abc", ABC, *flags, *paths)

    def test_swap_pin_exchanges_the_ports_and_keeps_the_clock(self) -> None:
        self.assertEqual(equiv_check._swap_pin("addr1[3]"), "addr2[3]")
        self.assertEqual(equiv_check._swap_pin("out2"), "out1")
        self.assertEqual(equiv_check._swap_pin("we1"), "we2")
        self.assertEqual(equiv_check._swap_pin("clk"), "clk")

    def test_the_same_memory_with_its_ports_exchanged_is_equivalent(self) -> None:
        code, out, err = self.run_check(self.ram(), self.ram(swap=True))
        self.assertEqual(code, 0, out + err)
        self.assertIn("dual_port_ram instance(s) turned", err)

    def test_the_same_orientation_needs_no_turn(self) -> None:
        code, out, err = self.run_check(self.ram(), self.ram())
        self.assertEqual(code, 0, out + err)

    def test_exchanged_ports_do_not_pair_without_the_orientation(self) -> None:
        code, _, err = self.run_check(self.ram(), self.ram(swap=True), "--no-dual-orient")
        self.assertEqual(code, 2, err)
        self.assertIn("cannot be matched one-to-one", err)

    def test_data_crossed_between_the_ports_is_never_equivalent(self) -> None:
        """Turning the ports must not hide a real difference: B writes port 1's data to port 2's
        address.  The RAM then has no partner (its input functions differ): exit 2."""
        code, out, err = self.run_check(self.ram(), self.ram(swap=True, cross_data=True))
        self.assertNotEqual(code, 0, out + err)
        self.assertNotIn("equiv-check: equivalent", out)

    def test_a_difference_behind_a_turned_ram_is_not_equivalent(self) -> None:
        code, out, err = self.run_check(self.ram(), self.ram(swap=True, invert=True))
        self.assertEqual(code, 1, out + err)
        self.assertIn("NOT equivalent", out)

    def test_ports_swapped_but_consumers_not_is_not_equivalent(self) -> None:
        code, out, err = self.run_check(self.ram(), self.ram(swap=True, keep_consumers=True))
        self.assertIn(code, (1, 2), out + err)
        self.assertNotIn("equiv-check: equivalent", out)

    def test_write_enable_crossed_is_never_equivalent(self) -> None:
        for swap in (False, True):
            code, out, err = self.run_check(self.ram(), self.ram(swap=swap, cross_we=True))
            self.assertIn(code, (1, 2), out + err)
            self.assertNotIn("equiv-check: equivalent", out)

    def test_address_crossed_is_never_equivalent(self) -> None:
        for swap in (False, True):
            code, out, err = self.run_check(self.ram(), self.ram(swap=swap, cross_addr=True))
            self.assertIn(code, (1, 2), out + err)
            self.assertNotIn("equiv-check: equivalent", out)

    def test_tied_ports_stay_tied_and_a_difference_behind_them_is_found(self) -> None:
        """Both ports with the same inputs have equal digests: no turn is chosen, nothing pairs
        by luck; the same memory is equivalent, an inverted consumer is not."""
        code, out, err = self.run_check(self.ram(tied=True), self.ram(tied=True))
        self.assertEqual(code, 0, out + err)
        code, out, err = self.run_check(self.ram(tied=True), self.ram(tied=True, invert=True))
        self.assertEqual(code, 1, out + err)
        self.assertNotIn("equiv-check: equivalent", out)


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

    def test_x_follows_a_black_box_to_its_outputs(self) -> None:
        """x on a RAM's address (A) and a real signal (B): the RAM's read data, and the output
        behind it, depend on the x, so both are removed and listed (review I6)."""
        head = ".model t\n.inputs clk a b c\n.outputs y p\n"
        tail = ".names q a y\n11 1\n.names a b p\n11 1\n.end\n"
        ram = ".gate single_port_ram addr[0]={} clk=clk data[0]=b out[0]=q we=c\n"
        a = self.write("a.blif", head + ".names unconn\n.names a unconn ax\n11 1\n"
                       + ram.format("ax") + tail)
        b = self.write("b.blif", head + ram.format("a") + tail)
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 3, out + err)
        cones = [line for line in out.splitlines() if line.startswith("x-cone:")]
        self.assertIn("x-cone: y", cones)
        self.assertTrue(any(":addr" in line for line in cones), cones)
        self.assertNotIn("x-cone: p", cones)
        self.assertIn("2 of 6 outputs are in an x-cone", out)

    def test_x_through_a_black_box_hides_nothing_outside_the_cone(self) -> None:
        """A difference outside the black box's cone is still found."""
        head = ".model t\n.inputs clk a b c\n.outputs y p\n"
        ram = ".gate single_port_ram addr[0]={} clk=clk data[0]=b out[0]=q we=c\n"
        a = self.write("a.blif", head + ".names unconn\n.names a unconn ax\n11 1\n"
                       + ram.format("ax") + ".names q a y\n11 1\n.names a b p\n11 1\n.end\n")
        b = self.write("b.blif", head + ram.format("a")
                       + ".names q a y\n11 1\n.names a b p\n10 1\n.end\n")
        code, out, err = check("--x-report", a, b)
        self.assertEqual(code, 1, out + err)
        self.assertIn("NOT equivalent outside x-cones", out)

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
class FaultInjectionTest(unittest.TestCase):
    """Exit codes 2, 4 and 5 (review I3, I4): a fake ABC that hangs or dies of SIGSEGV, and an
    uncaught exception inside the tool. None of them is exit 1 ('not equivalent')."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        text = ".model t\n.inputs a\n.outputs o\n.names a o\n1 1\n.end\n"
        self.a = self.write("a.blif", text)
        self.b = self.write("b.blif", text.replace(".names a o\n1 1",
                                                   ".names a t\n1 1\n.names t o\n1 1"))

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def fake_abc(self, body: str) -> str:
        path = self.write("abc", "#!/bin/sh\n" + body + "\n")
        os.chmod(path, 0o755)
        return path

    def test_abc_timeout_is_unproven_exit_4(self) -> None:
        abc = self.fake_abc("exec sleep 30")
        code, out, err = check("--abc", abc, "--timeout", "1", self.a, self.b)
        self.assertEqual(code, 4, out + err)
        self.assertIn("unproven: ABC cec timed out after 1 seconds", err)
        self.assertNotIn("NOT equivalent", out)

    def test_abc_undecided_is_unproven_exit_4(self) -> None:
        """dsec can end on its own engine limit ("Reached timeout (60 seconds) in frame 21 ...
        Networks are UNDECIDED", exit status 0): no verdict, exit 4, never an error (2H micro
        sweep of 2026-10-14, full/stereovision0)."""
        abc = self.fake_abc("echo 'Reached timeout (60 seconds) in frame 21.'; "
                            "echo 'Networks are UNDECIDED.  Time = 105.75 sec'")
        code, out, err = check("--abc", abc, self.a, self.b)
        self.assertEqual(code, 4, out + err)
        self.assertIn("unproven: ABC cec ended UNDECIDED", err)
        self.assertNotIn("NOT equivalent", out)

    def test_abc_killed_by_a_signal_is_hard_exit_5(self) -> None:
        abc = self.fake_abc("kill -SEGV $$")
        code, out, err = check("--abc", abc, self.a, self.b)
        self.assertEqual(code, 5, out + err)
        self.assertIn("ABC killed by signal 11", err)
        self.assertNotIn("NOT equivalent", out)

    def test_abc_ordinary_failure_stays_exit_2(self) -> None:
        abc = self.fake_abc("echo broken; exit 3")
        code, out, err = check("--abc", abc, self.a, self.b)
        self.assertEqual(code, 2, out + err)
        self.assertIn("ABC exited with status 3", err)

    def test_an_uncaught_exception_is_exit_2_not_1(self) -> None:
        with mock.patch.object(equiv_check, "_check", side_effect=KeyError("boom")):
            code, out, err = check(self.a, self.b)
        self.assertEqual(code, 2, out + err)
        self.assertIn("internal error: KeyError", err)
        self.assertNotIn("NOT equivalent", out)

    def test_out_of_memory_is_hard_exit_5(self) -> None:
        with mock.patch.object(equiv_check, "_check", side_effect=MemoryError()):
            code, out, err = check(self.a, self.b)
        self.assertEqual(code, 5, out + err)
        self.assertIn("out of memory", err)


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
        self.assertEqual(equiv_check.pair_unnamed(flats), (2, 0))
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
        self.assertEqual(equiv_check.pair_unnamed(flats), (2, 0))
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

    def copies(self, first: str, second: str, y_from: str, z_from: str) -> str:
        """Two registers computing one function (r1, r2), each addressing a box: the boxes have
        one signature and different input nets."""
        return (self.HEAD + ".names a b d\n11 1\n.latch d r1 re clk 0\n.latch d r2 re clk 0\n"
                + self.ram(first, "r1") + self.ram(second, "r2")
                + f".names {y_from} c y\n11 1\n.names {z_from} z\n1 1\n.end\n")

    def test_copies_are_paired_in_order_and_proved(self) -> None:
        """Two boxes fed by two copies of one register are interchangeable (the congruence
        closure of bbmatch sees r1 and r2 as one function): paired, none of them guessed."""
        flats = self.flats(self.copies("$m~1", "$m~2", "$m~1", "$m~2"),
                           self.copies("$m~7", "$m~8", "$m~7", "$m~8"))
        self.assertEqual(equiv_check.pair_unnamed(flats), (2, 0))
        if ABC is None and not REQUIRE_ABC:
            return
        a = self.write("a.blif", self.copies("$m~1", "$m~2", "$m~1", "$m~2"))
        b = self.write("b.blif", self.copies("$m~7", "$m~8", "$m~7", "$m~8"))
        code, out, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 0, out + err)

    @needs_abc
    def test_a_guessed_pairing_that_fails_is_inconclusive_exit_4(self) -> None:
        """The same copies read the other way round: the file-order guess is wrong, and 'not
        equivalent' would be a false verdict (the netlists are equivalent)."""
        a = self.write("a.blif", self.copies("$m~1", "$m~2", "$m~1", "$m~2"))
        b = self.write("b.blif", self.copies("$m~7", "$m~8", "$m~8", "$m~7"))
        code, out, err = check("--match-unnamed", a, b)
        self.assertEqual(code, 4, out + err)
        self.assertIn("inconclusive: 2 unnamed black-box pair(s) were guessed", err)

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
        self.assertEqual(equiv_check.pair_unnamed(flats), (1, 0))
        equiv_check.match_blackboxes(flats)

    def test_pairs_through_latches_with_other_names(self) -> None:
        """Box 2 reads a latch that box 1 feeds; the latches and nets are named differently."""
        def text(m1: str, m2: str, latch: str) -> str:
            return (self.HEAD + self.ram(m1) + f".latch {m1} {latch} re clk 0\n"
                    + self.ram(m2, latch, "b") + f".names {m2} c y\n11 1\n"
                    f".names {latch} z\n1 1\n.end\n")

        flats = self.flats(text("$m~1", "$m~2", "$dff~3^Q~0"), text("$m~5", "$m~6", "q"))
        self.assertEqual(equiv_check.pair_unnamed(flats), (2, 0))
        if ABC is None and not REQUIRE_ABC:
            return
        code, out, err = check("--match-unnamed", self.write("a.blif", text("$m~1", "$m~2", "x")),
                               self.write("b.blif", text("$m~5", "$m~6", "q")))
        self.assertEqual(code, 0, out + err)

    @needs_abc
    def test_a_wrong_pairing_is_never_equivalent(self) -> None:
        """Cross the pairing by hand: the check on the paired cut points must reject it."""
        flats = self.flats(self.two_boxes("$m~1", "$m~2"), self.two_boxes("$m~1", "$m~2"))
        self.assertEqual(equiv_check.pair_unnamed(flats), (2, 0))
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
        self.assertFalse(result.only_a or result.only_b or result.guessed)

    def test_not_interchangeable_is_split_by_structure_then_guessed(self) -> None:
        a = [bbmatch.Signature(b"1", ("x",), 5), bbmatch.Signature(b"1", ("y",), 6)]
        b = [bbmatch.Signature(b"1", ("u",), 6), bbmatch.Signature(b"1", ("w",), 5)]
        result = bbmatch.pair(a, b)
        self.assertEqual((result.pairs, result.guessed, result.exact), ([(0, 1), (1, 0)], 2, []))
        same = [bbmatch.Signature(b"1", ("x",), 5), bbmatch.Signature(b"1", ("y",), 5)]
        result = bbmatch.pair(same, b)
        self.assertEqual((result.pairs, result.guessed), ([(0, 0), (1, 1)], 2))

    def test_counts_must_agree(self) -> None:
        result = bbmatch.pair([self.sig(b"1", "x"), self.sig(b"1", "x")], [self.sig(b"1", "x")])
        self.assertEqual((result.pairs, result.only_a, result.only_b), ([], [0, 1], [0]))

    def test_combinational_loop_is_a_sim_error(self) -> None:
        loop = [blif.Names(inputs=["q"], output="p", rows=[("1", "1")]),
                blif.Names(inputs=["p"], output="q", rows=[("1", "1")])]
        with self.assertRaises(bbmatch.SimError):
            bbmatch.signatures(loop, [], [], [], lambda n: n)


if __name__ == "__main__":
    unittest.main()


class RamIdentityTest(unittest.TestCase):
    """rampair: the memory group and bit that a Parmys RAM slice's output net names."""

    def test_slice_identity_reads_the_group_and_the_bit(self) -> None:
        for net, want in [("single_port_ram^MEM~47-9^out~0", ("47", 9)),
                          ("dual_port_ram^MEM~0-30^out1[0]", ("0", 30)),
                          ("dual_port_ram^MEM~4__S-35^out2~1", ("4__S", 35)),
                          ("model#1 single_port_ram^MEM~5-12^out", ("5", 12))]:
            self.assertEqual(rampair.slice_identity([net]), want, net)

    def test_a_net_without_an_identity_gives_none(self) -> None:
        for net in ("$mul~14[0]", "dual_port_ram^MEM~1__S^lAND~608", "out"):
            self.assertIsNone(rampair.slice_identity([net]), net)

    def test_nets_that_name_two_identities_give_none(self) -> None:
        self.assertIsNone(rampair.slice_identity(["x^MEM~1-2^out1", "x^MEM~1-3^out2"]))

    def test_groups_collect_the_bits_of_a_memory(self) -> None:
        idents: list[tuple[str, tuple[str, int] | None]] = [
            ("single_port_ram", ("7", 1)), ("single_port_ram", ("7", 0)), ("mult", None),
            ("single_port_ram", ("9", 0))]
        groups = rampair.build_groups(idents)
        self.assertEqual([(g.mem, g.bits, g.slices) for g in groups],
                         [("7", (0, 1), {1: 0, 0: 1}), ("9", (0,), {0: 3})])

    def test_shapes_must_agree_where_both_sides_name_them(self) -> None:
        small = rampair.Group("single_port_ram", "1", {0: 0})
        big = rampair.Group("single_port_ram", "2", {0: 0, 1: 1})
        rampair.check_shapes([big], [rampair.Group("single_port_ram", "9", {0: 5, 1: 6})])
        with self.assertRaisesRegex(rampair.RamError, "1 in A, 0 in B"):
            rampair.check_shapes([small, big],
                                 [rampair.Group("single_port_ram", "9", {0: 0, 1: 1})])
        rampair.check_shapes([small], [rampair.Group("dual_port_ram", "3", {0: 0, 1: 1})])

    def test_classify_separates_exact_ambiguous_and_left(self) -> None:
        result = rampair.classify(["a", "b", "b", "c", "d", "d"], ["b", "b", "a", "e", "d"])
        self.assertEqual(result.exact, [(0, 2)])
        self.assertEqual(result.ambiguous, [([1, 2], [0, 1])])
        self.assertEqual(sorted(result.left_a), [3, 4, 5])
        self.assertEqual(sorted(result.left_b), [3, 4])


class RamPairingTest(unittest.TestCase):
    """--match-unnamed pairs Parmys RAM slices by memory group and bit.  The write enables are
    ANDs of 800 inputs, which the biased simulation never sets: simulation cannot tell the two
    memories apart, so ABC probes must (the memory ids are counters that differ between runs)."""

    WIDTH = 800

    def tree(self, prefix: str) -> str:
        """``prefix`` AND of WIDTH inputs as covers of at most 10 inputs: net ``w<prefix>``."""
        lines = []
        mids = []
        for k in range(0, self.WIDTH, 10):
            ins = " ".join(f"{prefix}{i}" for i in range(k, k + 10))
            lines.append(f".names {ins} m{prefix}{k}\n{'1' * 10} 1\n")
            mids.append(f"m{prefix}{k}")
        lines.append(f".names {' '.join(mids)} w{prefix}\n{'1' * len(mids)} 1\n")
        return "".join(lines)

    def netlist(self, ids: tuple[str, str], bit_swap: bool = False, read_swap: bool = False,
                drop_bit: bool = False) -> str:
        """Memory x (write enable wx, address ax) and y: two slices each, named by ``ids``."""
        ins = " ".join(f"{p}{i}" for p in "xy" for i in range(self.WIDTH))
        text = (f".model t\n.inputs clk ad d0 d1 {ins}\n.outputs ox0 ox1 oy0 oy1\n"
                + self.tree("x") + self.tree("y"))
        outs = {"x": ("ox0", "ox1"), "y": ("oy0", "oy1")}
        if read_swap:
            outs = {"x": ("oy0", "oy1"), "y": ("ox0", "ox1")}
        for mem, ident in zip("xy", ids, strict=True):
            for bit in (0, 1):
                if drop_bit and mem == "x" and bit == 1:
                    text += f".names d1 {outs[mem][bit]}\n1 1\n"
                    continue
                wired = 1 - bit if (bit_swap and mem == "x") else bit
                net = f"single_port_ram^MEM~{ident}-{bit}^out~0"
                text += (f".gate single_port_ram addr[0]=ad clk=clk data[0]=d{wired} "
                         f"out[0]={net} we=w{mem}\n.names {net} {outs[mem][bit]}\n1 1\n")
        return text + ".end\n"

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def run_check(self, text_a: str, text_b: str, *flags: str) -> tuple[int, str, str]:
        assert ABC is not None
        return check("--match-unnamed", "--abc", ABC, *flags, self.write("a.blif", text_a),
                     self.write("b.blif", text_b))

    @needs_abc
    def test_memory_ids_that_differ_between_runs_pair_by_probe_and_are_equivalent(self) -> None:
        """B is A with the ids of the two memories exchanged: pairing by id would cross them."""
        code, out, err = self.run_check(self.netlist(("7", "9")), self.netlist(("9", "7")))
        self.assertEqual(code, 0, out + err)
        self.assertIn("4 RAM slice(s) paired by memory group and bit (4 of them in groups told "
                      "apart by ABC probes)", err)

    @needs_abc
    def test_identical_ids_pair_and_are_equivalent(self) -> None:
        code, out, err = self.run_check(self.netlist(("7", "9")), self.netlist(("7", "9")))
        self.assertEqual(code, 0, out + err)
        self.assertIn("4 RAM slice(s) paired by memory group and bit", err)

    @needs_abc
    def test_slices_swapped_by_name_are_never_equivalent(self) -> None:
        """B wires the data of the slices ``-0`` and ``-1`` of x the other way round (the same
        names, a different design): the name-and-bit pairing crosses the data pins, ABC sees them
        differ, and the verdict is a difference."""
        code, out, err = self.run_check(self.netlist(("7", "9")),
                                        self.netlist(("7", "9"), bit_swap=True))
        self.assertIn(code, (1, 4), out + err)
        self.assertNotEqual(code, 0)

    @needs_abc
    def test_memories_swapped_by_name_are_never_equivalent(self) -> None:
        """B reads memory y where A reads x: a different design whichever way the groups pair."""
        code, out, err = self.run_check(self.netlist(("7", "9")),
                                        self.netlist(("7", "9"), read_swap=True))
        self.assertIn(code, (1, 4), out + err)
        self.assertNotEqual(code, 0)

    @needs_abc
    def test_ids_exchanged_and_reads_swapped_is_not_equivalent(self) -> None:
        code, out, err = self.run_check(self.netlist(("7", "9")),
                                        self.netlist(("9", "7"), read_swap=True))
        self.assertIn(code, (1, 4), out + err)

    def test_memories_of_different_size_are_exit_2(self) -> None:
        code, _, err = check("--match-unnamed", "--no-ram-probe",
                             self.write("a.blif", self.netlist(("7", "9"))),
                             self.write("b.blif", self.netlist(("7", "9"), drop_bit=True)))
        self.assertEqual(code, 2, err)
        self.assertIn("memories differ in size or number", err)
        self.assertIn("single_port_ram memory with 2 slice(s) (bits 0..1): 2 in A, 1 in B", err)

    @needs_abc
    def test_no_ram_pairing_leaves_everything_to_the_signatures(self) -> None:
        code, _, err = self.run_check(self.netlist(("7", "9")), self.netlist(("7", "9")),
                                      "--no-ram-pairing")
        self.assertIn(code, (0, 4), err)
        self.assertNotIn("RAM slice", err)


class SingleProbeTest(unittest.TestCase):
    """--match-unnamed also tells apart unnamed boxes that name no memory (multipliers) by ABC
    probes when simulation cannot: both ``a`` inputs are ANDs of 800 inputs that the biased
    simulation never raises, and B builds the same functions with another grouping, so neither the
    signature nor the colour of the input cones pairs them (2H micro sweep, rs_decoder_2)."""

    WIDTH = 800

    def tree(self, prefix: str, block: int) -> str:
        lines, mids = [], []
        for k in range(0, self.WIDTH, block):
            ins = " ".join(f"{prefix}{i}" for i in range(k, min(k + block, self.WIDTH)))
            lines.append(f".names {ins} m{prefix}{k}\n{'1' * min(block, self.WIDTH - k)} 1\n")
            mids.append(f"m{prefix}{k}")
        lines.append(f".names {' '.join(mids)} w{prefix}\n{'1' * len(mids)} 1\n")
        return "".join(lines)

    def netlist(self, block: int, swap: bool, xcone: bool = False, cross: bool = False,
                bad_in: bool = False) -> str:
        ins = " ".join(f"{p}{i}" for p in "xy" for i in range(self.WIDTH))
        text = f".model t\n.inputs clk d0 {ins}\n.outputs ox oy\n.names unconn\n"
        text += self.tree("x", block) + self.tree("y", block)
        if xcone:  # every compared pin of a box is in the x-cone
            text += ".names wx unconn wxu\n11 1\n.names wy unconn wyu\n11 1\n"
        mults = [("x", "ox"), ("y", "oy")]
        for name, out in (reversed(mults) if swap else mults):
            src = "x" if bad_in and name == "y" else name  # B's second multiplier: wrong input
            pin = f"w{src}{'u' if xcone else ''}"
            if cross:  # the consumers read the other multiplier
                out = "oy" if out == "ox" else "ox"
            text += (f".gate multiply a[0]={pin} b[0]=d0 out[0]=mul{name}~0\n"
                     f".names mul{name}~0 {out}\n1 1\n")
        return text + ".end\n"

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_check(self, *flags: str, xcone: bool = False, cross: bool = False,
                  bad_in: bool = False) -> tuple[int, str, str]:
        assert ABC is not None
        paths = []
        for name, text in (("a.blif", self.netlist(10, False, xcone)),
                           ("b.blif", self.netlist(8, True, xcone, cross, bad_in))):
            path = Path(self.tmp.name) / name
            path.write_text(text, encoding="utf-8")
            paths.append(str(path))
        return check("--match-unnamed", "--abc", ABC, *flags, *paths)

    @needs_abc
    def test_boxes_that_simulation_cannot_tell_apart_pair_by_probe(self) -> None:
        code, out, err = self.run_check()
        self.assertEqual(code, 0, out + err)
        self.assertIn("multiply #", err)

    @needs_abc
    def test_without_probes_the_wrong_guess_is_inconclusive_not_a_difference(self) -> None:
        code, out, err = self.run_check("--no-ram-probe")
        self.assertEqual(code, 4, out + err)
        self.assertIn("inconclusive", err)

    @needs_abc
    def test_an_x_cone_result_over_guessed_pairs_is_inconclusive_not_eq_x(self) -> None:
        """Every compared pin of both boxes is behind ``unconn``, so the x-report would call a
        wrong guess "equivalent outside the x-cones" (exit 3): with guessed pairs it is exit 4."""
        code, out, err = self.run_check("--no-ram-probe", "--x-report", xcone=True)
        self.assertEqual(code, 4, out + err)
        self.assertIn("equivalent outside the x-cones", err)
        code, out, err = self.run_check("--x-report", xcone=True)
        # a probe over pins that are all behind unconn proves nothing: still exit 4
        self.assertEqual(code, 4, out + err)
        self.assertIn("inconclusive", err)

    @needs_abc
    def test_a_crossed_multiplier_is_never_equivalent_with_probes_on(self) -> None:
        for flags in ((), ("--x-report",)):
            code, out, err = self.run_check(*flags, cross=True)
            self.assertIn(code, (1, 4), out + err)
            self.assertNotIn("equiv-check: equivalent", out)
            self.assertNotIn("equivalent outside", out)

    @needs_abc
    def test_a_multiplier_with_a_wrong_input_is_not_eq_x_with_probes_on(self) -> None:
        code, out, err = self.run_check("--x-report", xcone=True, bad_in=True)
        self.assertIn(code, (1, 4), out + err)  # never 3 (equivalent outside the x-cones)
        self.assertNotIn("equivalent outside the x-cones (abc", out)

    @needs_abc
    def test_a_leftover_pair_with_probes_on_is_a_guess_the_proof_rejects(self) -> None:
        """B's two multipliers both read x: probes pair x with one of them and the last probe (y)
        fails, leaving one pair per side, which is paired as a guess (weak). The final proof then
        finds y's input different: never equivalent, never PROVEN."""
        for flags in ((), ("--x-report",)):
            code, out, err = self.run_check(*flags, bad_in=True)
            self.assertIn(code, (1, 4), out + err)
            self.assertNotIn("equiv-check: equivalent", out)
            self.assertNotIn("equivalent outside", out)
            self.assertIn("multiply #", err)  # the probe round ran and paired the first box


class CongruenceTest(unittest.TestCase):
    """bbmatch.canonical_classes: copies of one register and boxes fed by them are one class."""

    def test_copies_of_a_register_are_one_class_and_so_are_the_boxes_they_feed(self) -> None:
        names = [blif.Names(["a"], "d1", [("1", "1")]), blif.Names(["a", "b"], "d2", [("11", "1")])]
        latches = [blif.Latch("d2", "q1", init="0"), blif.Latch("d2", "q2", init="0"),
                   blif.Latch("d2", "q3", init="1")]
        boxes = [bbmatch.BoxSpec("multiply", (), None, (("a[0]", "q1"),), (("out[0]", "o1"),)),
                 bbmatch.BoxSpec("multiply", (), None, (("a[0]", "q2"),), (("out[0]", "o2"),)),
                 bbmatch.BoxSpec("multiply", (), None, (("a[0]", "q3"),), (("out[0]", "o3"),))]
        canon = bbmatch.canonical_classes(names, latches, boxes)
        self.assertEqual(canon("q1"), canon("q2"))
        self.assertNotEqual(canon("q1"), canon("q3"))  # another init: another register
        self.assertEqual(canon("o1"), canon("o2"))
        self.assertNotEqual(canon("o1"), canon("o3"))
        self.assertEqual(canon("d1"), "a")  # a buffer is looked through


def _stuck_we(name: str, first: int = 0) -> tuple[list[str], str]:
    """(.inputs, cover text) of a write enable that random simulation never raises (a 64-input
    AND), as in the adversarial repros."""
    ins = [f"{name}{i}" for i in range(first, first + 64)]
    lines = []
    for k in range(0, 64, 8):
        lines.append(f".names {' '.join(ins[k:k + 8])} m{name}{k}\n11111111 1\n")
    lines.append(f".names {' '.join(f'm{name}{k}' for k in range(0, 64, 8))} w{name}\n"
                 + "1" * 8 + " 1\n")
    return ins, "".join(lines)


class RamReviewFixTest(unittest.TestCase):
    """Fixes for the adversarial review of the RAM pairing (e1, a4, c1, b1)."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_pair(self, text_a: str, text_b: str, *flags: str) -> tuple[int, str, str]:
        paths = []
        for name, text in (("a.blif", text_a), ("b.blif", text_b)):
            path = Path(self.tmp.name) / name
            path.write_text(text, encoding="utf-8")
            paths.append(str(path))
        abc = ["--abc", ABC] if ABC is not None else []
        return check("--match-unnamed", *abc, *flags, *paths)

    def test_one_formal_bit_connected_twice_is_exit_2(self) -> None:
        """e1: ``data[0]=d0 data=d1`` is one pin of a one-bit port connected twice."""
        head = ".model t\n.inputs clk ad d0 d1 we\n.outputs o\n"
        tail = ".names single_port_ram^MEM~3-0^out~0 o\n1 1\n.end\n"
        gate = (".gate single_port_ram addr[0]=ad clk=clk {} "
                "out[0]=single_port_ram^MEM~3-0^out~0 we=we\n")
        a = head + gate.format("data[0]=d0 data=d1") + tail
        b = head + gate.format("data[0]=d1") + tail
        for flags in ((), ("--no-ram-pairing",)):
            code, _, err = self.run_pair(a, b, *flags)
            self.assertEqual(code, 2, err)
            self.assertIn("connected twice", err)
            code, _, err = self.run_pair(b, a, *flags)
            self.assertEqual(code, 2, err)

    def test_same_formal_twice_on_a_gate_adder_is_exit_2(self) -> None:
        head = ".model t\n.inputs a b c\n.outputs s co\n"
        gate = (".gate adder a[0]=a b[0]=b cin=c {}sumout[0]=s cout=co\n.end\n")
        code, _, err = self.run_pair(head + gate.format("a[0]=b "), head + gate.format(""))
        self.assertEqual(code, 2, err)
        self.assertIn("connected twice", err)

    def a4(self, swapped: bool) -> str:
        ins, cover = _stuck_we("x")
        q0, q1 = [f"single_port_ram^MEM~5-{k}^out~0" for k in (0, 1)]
        outs = (q1, q0) if swapped else (q0, q1)
        ram = ".gate single_port_ram addr[0]=ad clk=clk data[0]=d0 out[0]={} we=wx\n"
        return (f".model t\n.inputs clk ad d0 {' '.join(ins)}\n.outputs o0 o1\n" + cover
                + ram.format(q0) + ram.format(q1)
                + f".names {outs[0]} o0\n1 1\n.names {outs[1]} o1\n1 1\n.end\n")

    @needs_abc
    def test_interchangeable_slices_with_swapped_reads_are_never_exit_1(self) -> None:
        """a4: the two slices have the same input pins, so swapping their reads is a no-op in
        truth.  Exit 1 would be a false 'differs'; 0 or 4 are allowed (never 1)."""
        code, out, err = self.run_pair(self.a4(False), self.a4(True))
        self.assertIn(code, (0, 4), out + err)

    def c1(self) -> str:
        return (".model t\n.inputs clk ad d0 d1 we0 we1\n.outputs o0 o1\n"
                ".subckt sub clk=clk a=ad d=d0 we=we0 q=o0\n"
                ".subckt sub clk=clk a=ad d=d1 we=we1 q=o1\n.end\n"
                ".model sub\n.inputs clk a d we\n.outputs q\n"
                ".gate single_port_ram addr[0]=a clk=clk data[0]=d "
                "out[0]=single_port_ram^MEM~3-0^out~0 we=we\n"
                ".names single_port_ram^MEM~3-0^out~0 q\n1 1\n.end\n")

    @needs_abc
    def test_a_submodel_instantiated_twice_falls_back_to_signatures(self) -> None:
        code, out, err = self.run_pair(self.c1(), self.c1())
        self.assertEqual(code, 0, out + err)

    def test_two_slices_with_one_identity_are_ungrouped_not_an_error(self) -> None:
        groups = rampair.build_groups([("single_port_ram", ("7", 0)),
                                       ("single_port_ram", ("7", 0)),
                                       ("single_port_ram", ("7", 1)),
                                       ("single_port_ram", ("8", 0))])
        self.assertEqual([(g.mem, g.slices) for g in groups], [("8", {0: 3})])

    def b1(self, ids: tuple[str, str], swap: bool = False) -> str:
        ins_x, cov_x = _stuck_we("x")
        ins_y, cov_y = _stuck_we("y")
        ram = (".subckt single_port_ram addr[0]=ad clk=clk data[0]={} "
               "out[0]=single_port_ram^MEM~{}-0^out~0 we={}\n")
        return (f".model t\n.inputs clk ad d0 d1 {' '.join(ins_x + ins_y)}\n.outputs o0 o1\n"
                + cov_x + cov_y + ram.format("d0", ids[0], "wx") + ram.format("d1", "77", "wy")
                + ".cname ram@1/0\n"
                + f".names single_port_ram^MEM~{ids[0]}-0^out~0 {'o1' if swap else 'o0'}\n1 1\n"
                + f".names single_port_ram^MEM~77-0^out~0 {'o0' if swap else 'o1'}\n1 1\n.end\n"
                + ".model single_port_ram\n.inputs addr[0] clk data[0] we\n.outputs out[0]\n"
                ".blackbox\n.end\n")

    @needs_abc
    def test_a_cname_like_a_generated_key_does_not_collide(self) -> None:
        """b1: the user's ``.cname ram@1/0`` must not merge with a generated key."""
        code, out, err = self.run_pair(self.b1(("5", "77")), self.b1(("9", "77"), swap=True))
        self.assertIn(code, (1, 4), out + err)
        self.assertNotIn("loop", err)
