"""Tests for tools/netlist-compare."""

from __future__ import annotations

import random
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

import blif
import netlist_compare

from . import helpers

ALL_FIXTURES = sorted(
    str(p.relative_to(helpers.FIXTURES)) for p in helpers.FIXTURES.glob("**/*.blif")
)
CRITIC = helpers.FIXTURES / "critic"


def cx(name: str) -> str:
    return str(CRITIC / name)


def random_netlist(rng: random.Random) -> str:
    """Small random sequential netlist with many structurally similar nets (constants,
    duplicate gates, latch cycles, repeated input columns, multi-output black boxes)."""
    ins = [f"i{k}" for k in range(rng.randint(0, 3))]
    latches = [f"q{k}" for k in range(rng.randint(0, 6))]
    avail = ins + latches + ["c0"]
    body = [".names c0"]
    for c in range(rng.randint(2, 20)):
        k = rng.randint(0, 3)
        src = [rng.choice(avail) for _ in range(k)]
        body.append(" ".join([".names", *src, f"w{c}"]))
        if k:
            pol = rng.choice("01")
            rows = rng.randint(0, 2)
            planes = {"".join(rng.choice("01-") for _ in range(k)) for _ in range(rows)}
            body += [f"{plane} {pol}" for plane in sorted(planes)]
        elif rng.random() < 0.5:
            body.append("1")
        avail.append(f"w{c}")
    for b in range(rng.randint(0, 2)):
        body.append(f".subckt bb a={rng.choice(avail)} b={rng.choice(avail)} z=s{b}")
        avail.append(f"s{b}")
    body += [f".latch {rng.choice(avail)} {q} re clk {rng.choice('01')}" for q in latches]
    outs = [f"o{j}" for j in range(rng.randint(0, 2))]
    for out in outs:
        body += [f".names {rng.choice(avail)} {out}", "1 1"]
    head = [".model top", " ".join([".inputs", *ins, "clk"]), " ".join([".outputs", *outs])]
    tail = [".end", ".model bb", ".inputs a b", ".outputs z", ".blackbox", ".end"]
    return "\n".join(head + body + tail) + "\n"


def compare(*argv: str) -> tuple[int, str, str]:
    return helpers.run_main(netlist_compare.main, list(argv))


def canon_text(text: str, **kwargs: bool) -> str:
    return netlist_compare.canonicalize(blif.parse_text(text), **kwargs)


def shift_chain(depth: int, names: list[str]) -> str:
    """A ``depth``-stage shift register from input d to output q via nets ``names``."""
    lines = [".model chain", ".inputs clk d", ".outputs q"]
    prev = "d"
    for net in names[:depth]:
        lines.append(f".latch {prev} {net} re clk 0")
        prev = net
    lines += [f".names {prev} q", "1 1", ".end"]
    return "\n".join(lines) + "\n"


class CompareTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name: str, text: str) -> str:
        path = Path(self.tmp.name) / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def test_identical_files(self) -> None:
        for name in ALL_FIXTURES:
            with self.subTest(name):
                path = helpers.fixture(name)
                self.assertEqual(compare(path, path)[0], 0)

    def test_real_goldens_against_themselves(self) -> None:
        for name in ("blink.odin.blif", "blink.parmys.blif"):
            copy = self.write(name, Path(helpers.fixture(name)).read_text(encoding="utf-8"))
            self.assertEqual(compare(helpers.fixture(name), copy)[0], 0)

    def test_renamed_shuffled_permuted(self) -> None:
        code, out, _ = compare(
            helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_renamed.blif")
        )
        self.assertEqual(code, 0, out)

    def test_random_scrambles(self) -> None:
        for name in ALL_FIXTURES:
            original = blif.parse_file(helpers.fixture(name))
            expected = netlist_compare.canonicalize(original)
            for seed in range(5):
                with self.subTest(name=name, seed=seed):
                    text = helpers.dump(helpers.scramble(original, seed))
                    self.assertEqual(canon_text(text), expected)

    def test_random_netlists_scramble_invariant(self) -> None:
        """Mini version of the review fuzzers: renaming/reordering never changes the form."""
        for seed in range(300):
            original = blif.parse_text(random_netlist(random.Random(seed)))
            expected = netlist_compare.canonicalize(original)
            for variant in range(2):
                with self.subTest(seed=seed, variant=variant):
                    text = helpers.dump(helpers.scramble(original, variant))
                    self.assertEqual(canon_text(text), expected)

    def test_symmetric_but_distinguishable_nets(self) -> None:
        """Duplicate gates told apart only by their fan-out (review repro dup_a/dup_b)."""
        self.assertEqual(compare(cx("dup_a.blif"), cx("dup_b.blif"))[0], 0)

    def test_duplicate_cells_are_kept(self) -> None:
        code, out, _ = compare(cx("sd_a.blif"), cx("sd_b.blif"))
        self.assertEqual(code, 1)
        self.assertIn("-.subckt sink a=x", out)

    def test_individualization_is_structural(self) -> None:
        """A latch self-loop vs a 2-cycle: colour refinement ties them, the trial
        individualization must still pick structurally."""
        text = Path(cx("wl_cycles.blif")).read_text(encoding="utf-8")
        swapped = (
            text.replace("q0", "QA")
            .replace("q1", "Q0")
            .replace("q2", "q0")
            .replace("QA", "q2")
            .replace("Q0", "q1")
        )
        self.assertNotEqual(swapped, text)
        self.assertEqual(canon_text(swapped), canon_text(text))

    def test_flipped_cover_row(self) -> None:
        code, out, _ = compare(
            helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_flipped.blif")
        )
        self.assertEqual(code, 1)
        self.assertIn("-10 1", out)
        self.assertIn("+11 1", out)

    def test_renamed_primary_output(self) -> None:
        code, out, _ = compare(
            helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_po_renamed.blif")
        )
        self.assertEqual(code, 1)
        self.assertIn("+.outputs zz", out)

    def test_quiet(self) -> None:
        code, out, _ = compare(
            "--quiet", helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_flipped.blif")
        )
        self.assertEqual((code, out), (1, ""))

    def test_malformed_is_error(self) -> None:
        good = helpers.fixture("comb_parmys.blif")
        bad = self.write("bad.blif", ".model t\n.inputs a\n.outputs y\n.names a y\n11 1\n.end\n")
        code, out, err = compare(good, bad)
        self.assertEqual(code, 2)
        self.assertRegex(err, r"^netlist-compare: .*bad\.blif:5: ")
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertEqual(compare(good, str(Path(self.tmp.name) / "missing.blif"))[0], 2)

    def test_usage_errors(self) -> None:
        good = helpers.fixture("comb_parmys.blif")
        self.assertEqual(compare(good)[0], 2)
        self.assertEqual(compare("--canon", good, good)[0], 2)

    def test_continuation_lines(self) -> None:
        one = ".model t\n.inputs a b c\n.outputs y\n.names a b c y\n111 1\n.end\n"
        split = ".model t\n.inputs a \\\nb \\\n c\n.outputs y\n.names a b\\\n c y\n111 1\n.end\n"
        self.assertEqual(canon_text(one), canon_text(split))

    def test_canon_idempotent(self) -> None:
        for name in ALL_FIXTURES:
            with self.subTest(name):
                once = netlist_compare.canonicalize(blif.parse_file(helpers.fixture(name)))
                self.assertEqual(canon_text(once), once)

    def test_canon_cli(self) -> None:
        code, out, _ = compare("--canon", helpers.fixture("comb_renamed.blif"))
        self.assertEqual(code, 0)
        self.assertIn(".names c n2 z\n10 1\n", out)
        self.assertNotIn("and_lo", out)

    def test_latch_feedback_loop_terminates(self) -> None:
        ring = Path(helpers.fixture("ring.blif")).read_text(encoding="utf-8")
        renamed = ring.replace("r_a", "zz9").replace("r_b", "aa1").replace("r_c", "mm5")
        self.assertEqual(canon_text(ring), canon_text(renamed))

    def test_deep_latch_chain(self) -> None:
        depth = 60
        forward = [f"s{i}" for i in range(depth)]
        backward = [f"t{depth - i}" for i in range(depth)]  # name order opposite to depth
        self.assertEqual(
            canon_text(shift_chain(depth, forward)), canon_text(shift_chain(depth, backward))
        )

    def test_attrs_dropped_by_default_params_kept(self) -> None:
        base = (
            ".model t\n.inputs a\n.outputs y\n.subckt s x=a z=y\n{extra}.end\n"
            ".model s\n.inputs x\n.outputs z\n.blackbox\n.end\n"
        )
        plain = canon_text(base.format(extra=""))
        self.assertEqual(canon_text(base.format(extra=".cname u1\n.attr src a.v:1\n")), plain)
        self.assertNotEqual(canon_text(base.format(extra=".param WIDTH 4\n")), plain)
        kept = canon_text(base.format(extra=".cname u1\n"), keep_attrs=True)
        self.assertIn(".cname u1", kept)

    def test_keep_attrs_flag(self) -> None:
        a, b = helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_renamed.blif")
        self.assertEqual(compare(a, b)[0], 0)
        self.assertEqual(compare("--keep-attrs", a, b)[0], 1)

    def test_primary_name_collision_with_canonical_prefix(self) -> None:
        text = (
            ".model t\n.inputs n0 n1\n.outputs n2\n.names n0 n1 w\n11 1\n.names w n2\n0 1\n.end\n"
        )
        canon = canon_text(text)
        self.assertIn(".names n0 n1 n_0", canon)

    def test_large_netlist_is_fast(self) -> None:
        n = 20000  # ~ 60k lines
        lines = [".model big", ".inputs clk " + " ".join(f"i{k}" for k in range(8)), ".outputs o"]
        for k in range(n):
            a = f"i{k % 8}" if k < 8 else f"w{k - 8}"
            b = f"w{k // 2}" if k >= 16 else f"i{(k + 3) % 8}"
            lines += [f".names {a} {b} w{k}", "10 1", "01 1"]
            if k % 100 == 99:
                lines.append(f".latch w{k} w{k}_q re clk 0")
        lines += [f".names w{n - 1} o", "1 1", ".end"]
        start = time.monotonic()
        canon = canon_text("\n".join(lines) + "\n")
        self.assertLess(time.monotonic() - start, 30.0)
        self.assertIn(".outputs o", canon)

    # -- --implicit-models (2E §18.3) ---------------------------------------------------------

    DFF = ".model top\n.inputs c d\n.outputs q\n.subckt $dff CLK=c D=d Q=q\n.param WIDTH 1\n.end\n"

    def test_implicit_models_dff(self) -> None:
        """An undefined $-model is a black box whose formals are those used (Q an output)."""
        a = self.write("a.blif", self.DFF)
        b = self.write("b.blif", self.DFF.replace("CLK=c D=d Q=q", "Q=q CLK=c D=d"))
        self.assertEqual(compare("--implicit-models", a, b)[0], 0)
        code, _, err = compare(a, b)
        self.assertEqual(code, 2)
        self.assertIn("undefined model '$dff'", err)

    def test_implicit_models_swapped_q_d_differ(self) -> None:
        a = self.write("a.blif", self.DFF)
        swapped = self.DFF.replace(".inputs c d", ".inputs c q").replace(".outputs q", ".outputs d")
        b = self.write("b.blif", swapped.replace("D=d Q=q", "D=q Q=d"))
        code, out, _ = compare("--implicit-models", a, b)
        self.assertEqual(code, 1, out)

    def test_implicit_models_outputs_drive(self) -> None:
        """Y, Q, X, CO and RD_DATA (any bit) are outputs: a second driver is an error."""
        for formal in ("Y", "Q", "X", "CO", "RD_DATA[3]"):
            with self.subTest(formal):
                text = (
                    ".model top\n.inputs a\n.outputs y\n"
                    f".subckt $cell A=a {formal}=y\n.names a y\n1 1\n.end\n"
                )
                code, _, err = compare(
                    "--implicit-models", self.write("t.blif", text), self.write("u.blif", text)
                )
                self.assertEqual(code, 2)
                self.assertIn("more than one driver", err)

    def test_inout_driven_internally_compares(self) -> None:
        """2H: Yosys lists an inout in .inputs and .outputs and a $tribuf drives it; that is
        one primary-input net plus one internal driver, not a clash (simple_inout.v)."""
        text = (
            ".model simple_op\n.inputs in in_out1 in_out2 en\n.outputs in_out1 in_out2 out\n"
            ".subckt $tribuf A=in EN=en Y=in_out1\n.param WIDTH 1\n"
            ".names in_out2 out\n1 1\n.end\n"
        )
        a, b = self.write("a.blif", text), self.write("b.blif", text)
        self.assertEqual(compare("--implicit-models", a, b)[0], 0)
        clash = self.write("c.blif", text.replace(".end\n", ".names en in_out1\n1 1\n.end\n"))
        code, _, err = compare("--implicit-models", clash, clash)
        self.assertEqual(code, 2)
        self.assertIn("more than one driver", err)

    def test_implicit_models_per_model_outputs(self) -> None:
        """DATA is $memrd's output but $memwr's input; CTRL_OUT is $fsm's output."""
        text = (
            ".model top\n.inputs c a d\n.outputs y z\n"
            ".subckt $memrd CLK=c ADDR=a DATA=y\n.subckt $memwr CLK=c ADDR=a DATA=d\n"
            ".subckt $fsm CLK=c CTRL_IN=d CTRL_OUT=z\n.end\n"
        )
        code, out, _ = compare("--implicit-models", "--canon", self.write("a.blif", text))
        self.assertEqual(code, 0)
        self.assertIn(".model $memrd\n.inputs ADDR\n.inputs CLK\n.outputs DATA\n", out)
        self.assertIn(".model $memwr\n.inputs ADDR\n.inputs CLK\n.inputs DATA\n.blackbox", out)
        self.assertIn(".model $fsm\n.inputs CLK\n.inputs CTRL_IN\n.outputs CTRL_OUT\n", out)

    def test_implicit_models_scalar_blackbox_bit0(self) -> None:
        """I3: Yosys 0.55 writes `addr[0]=a[0]` for a black box whose model port `addr` is 1 bit
        at its defaults (parameter W = 1) but is connected wider; it reads as `addr`."""
        text = (
            ".model top\n.inputs clk a[0] a[1]\n.outputs o[0]\n"
            ".subckt single_port_ram addr[0]=a[0] clk=clk out[0]=o[0]\n.end\n"
            ".model single_port_ram\n.inputs clk addr\n.outputs out\n.blackbox\n.end\n"
        )
        a = self.write("a.blif", text)
        self.assertEqual(compare(a, a)[0], 2)  # without the flag: no port addr[0]
        self.assertEqual(compare("--implicit-models", a, a)[0], 0)
        moved = self.write("b.blif", text.replace("addr[0]=a[0]", "addr[0]=a[1]"))
        self.assertEqual(compare("--implicit-models", a, moved)[0], 1)
        # Not rewritten: the model has addr[0] itself, or the instance names two bits of addr.
        ports = text.replace(".inputs clk addr", ".inputs clk addr[0]")
        self.assertIn(
            " addr[0]=a[0] clk=clk out=o[0]\n", netlist_compare.scalar_blackbox_pins(ports)
        )
        two = text.replace("addr[0]=a[0]", "addr[0]=a[0] addr[1]=a[1]")
        self.assertIn(
            " addr[0]=a[0] addr[1]=a[1] clk=clk out=o[0]\n",
            netlist_compare.scalar_blackbox_pins(two),
        )
        not_box = text.replace(".blackbox\n", "")
        self.assertEqual(netlist_compare.scalar_blackbox_pins(not_box), not_box)

    def test_implicit_models_canon_lists_the_stub(self) -> None:
        code, out, _ = compare("--implicit-models", "--canon", self.write("a.blif", self.DFF))
        self.assertEqual(code, 0)
        self.assertIn(".model $dff\n.inputs CLK\n.inputs D\n.outputs Q\n.blackbox\n", out)

    def test_implicit_models_defined_model_wins(self) -> None:
        """A $-model the file defines is used as written (no stub, no set inputs)."""
        text = (
            ".model top\n.inputs a b\n.outputs y\n.subckt $reduce_or A[0]=a A[1]=b Y=y\n"
            ".end\n.model $reduce_or\n.inputs A[0] A[1]\n.outputs Y\n.blackbox\n.end\n"
        )
        swapped = text.replace("A[0]=a A[1]=b", "A[0]=b A[1]=a")
        code, _, _ = compare(
            "--implicit-models", self.write("a.blif", text), self.write("b.blif", swapped)
        )
        self.assertEqual(code, 1)

    def test_implicit_models_reduce_inputs_are_a_set(self) -> None:
        """§20 #8: the A bits of an implicit $reduce_* are a set (their order cannot change Y)."""
        for model in ("$reduce_or", "$reduce_and", "$reduce_xor", "$reduce_xnor", "$reduce_bool"):
            with self.subTest(model):
                text = (
                    ".model top\n.inputs a b c\n.outputs y z\n"
                    f".subckt {model} A[0]=a A[1]=n A[2]=c Y=y\n"
                    ".names b n\n0 1\n.subckt $mux A=a B=b S=c Y=z\n.end\n"
                )
                swapped = text.replace("A[0]=a A[1]=n A[2]=c", "A[0]=n A[1]=c A[2]=a")
                a, b = self.write("a.blif", text), self.write("b.blif", swapped)
                self.assertEqual(compare("--implicit-models", a, b)[0], 0)
                # $mux is not commutative: swapping its A and B differs.
                muxed = text.replace("A=a B=b S=c", "A=b B=a S=c")
                self.assertEqual(compare("--implicit-models", a, self.write("c.blif", muxed))[0], 1)

    def test_implicit_models_reduce_width_still_counts(self) -> None:
        text = ".model top\n.inputs a b\n.outputs y\n.subckt $reduce_or A[0]=a A[1]=b Y=y\n.end\n"
        narrow = text.replace(" A[1]=b", "")
        code, _, _ = compare(
            "--implicit-models", self.write("a.blif", text), self.write("b.blif", narrow)
        )
        self.assertEqual(code, 1)

    # -- --vtr-normalize (3E §16.3, plan Task 10) ---------------------------------------------

    def both(self, a: str, b: str, *flags: str) -> int:
        return compare(*flags, self.write("a.blif", a), self.write("b.blif", b))[0]

    def test_vtr_normalize_decodes_names(self) -> None:
        """Review Focus 3: `%fe` is the name fe, `%FE` the byte 0xFE; the decode is exact."""
        template = ".model top\n.inputs {n}\n.outputs y\n.names {n} y\n1 1\n.end\n"
        for written, name in (
            ("%fe", "fe"),
            ("a%23b", "a%2523b"),
            ("%2Eend", ".end"),
            ("%252Eend", ".end"),
            ("x%5Cy", "x%255Cy"),
        ):
            with self.subTest(written):
                same = template.format(n=written)
                plain = template.format(n=name)
                self.assertEqual(self.both(same, plain, "--vtr-normalize"), 0)
                self.assertEqual(self.both(same, plain), 1)
        byte = template.format(n="%FE")
        self.assertEqual(self.both(byte, template.format(n="fe"), "--vtr-normalize"), 1)
        self.assertEqual(netlist_compare.vb10_decode("%FE"), "\udcfe")
        self.assertEqual(netlist_compare.vb10_decode("%fe"), "fe")
        self.assertEqual(netlist_compare.vb10_decode("%2Eend"), ".end")
        self.assertEqual(netlist_compare.vb10_decode("%NIL"), "NIL")
        self.assertEqual(netlist_compare.vb10_decode("%25NIL"), "NIL")

    def test_vtr_normalize_twice_escaped_name(self) -> None:
        """`$2%5Cbuffer` against `$2%255Cbuffer`, a primary output (the name is kept)."""
        template = ".model top\n.inputs a\n.outputs {n}\n.names a {n}\n1 1\n.end\n"
        once, twice = template.format(n="$2%5Cbuffer"), template.format(n="$2%255Cbuffer")
        self.assertEqual(self.both(once, twice, "--vtr-normalize"), 0)
        self.assertEqual(self.both(once, twice), 1)

    def test_vtr_normalize_latch_init(self) -> None:
        text = ".model top\n.inputs d c\n.outputs q\n.latch d q re c {i}\n.end\n"
        self.assertEqual(self.both(text.format(i="3"), text.format(i="2"), "--vtr-normalize"), 0)
        self.assertEqual(self.both(text.format(i="3"), text.format(i="2")), 1)
        self.assertEqual(self.both(text.format(i="1"), text.format(i="2"), "--vtr-normalize"), 1)

    MULTIPLY = (
        ".model top\n.inputs x y\n.outputs o\n{gen}.subckt multiply {pins} out[0]=o\n"
        ".end\n"
        ".model multiply\n.inputs a[0] a[1] a[2] b[0] b[1] b[2]\n.outputs out[0]\n"
        ".blackbox\n.end\n"
    )

    def test_vtr_normalize_omitted_subckt_input_is_unconn(self) -> None:
        gen = ".names unconn\n"
        short = self.MULTIPLY.format(gen="", pins="a[0]=x b[0]=y")
        full = self.MULTIPLY.format(
            gen=gen, pins="a[0]=x a[1]=unconn a[2]=unconn b[0]=y b[1]=unconn b[2]=unconn"
        )
        self.assertEqual(self.both(short, full, "--vtr-normalize"), 0)
        self.assertEqual(self.both(short, full), 1)
        wrong = self.MULTIPLY.format(
            gen=gen, pins="a[0]=x a[1]=y a[2]=unconn b[0]=y b[1]=unconn b[2]=unconn"
        )
        self.assertEqual(self.both(short, wrong, "--vtr-normalize"), 1)

    def test_vtr_normalize_clock_lines_dropped(self) -> None:
        with_clock = ".model top\n.inputs d\n.clock c\n.outputs q\n.latch d q re c 2\n.end\n"
        inputs = ".model top\n.inputs d c\n.outputs q\n.latch d q re c 2\n.end\n"
        self.assertEqual(self.both(with_clock, inputs, "--vtr-normalize"), 0)
        self.assertEqual(self.both(with_clock, inputs), 1)

    def test_vtr_normalize_one_input_cover_is_one_row(self) -> None:
        template = ".model top\n.inputs a\n.outputs y\n.names a y\n{rows}.end\n"
        for rows, one in (("1 1\n1 1\n", "1 1\n"), ("1 0\n", "0 1\n"), ("0 0\n", "1 1\n")):
            with self.subTest(rows):
                a, b = template.format(rows=rows), template.format(rows=one)
                self.assertEqual(self.both(a, b, "--vtr-normalize"), 0)
        both_rows = template.format(rows="0 1\n1 1\n0 1\n")
        const1 = ".model top\n.inputs a\n.outputs y\n.names y\n1\n.end\n"
        self.assertEqual(self.both(both_rows, const1, "--vtr-normalize"), 0)
        self.assertEqual(
            self.both(
                template.format(rows="1 1\n"), template.format(rows="0 1\n"), "--vtr-normalize"
            ),
            1,
        )

    def test_vtr_normalize_constant_cover_is_a_generator(self) -> None:
        template = ".model top\n.inputs a b\n.outputs y\n{cell}.end\n"
        one = template.format(cell=".names y\n1\n")
        zero = template.format(cell=".names y\n")
        for cell in (".names a b y\n-- 1\n", ".names a b y\n0- 1\n1- 1\n"):
            self.assertEqual(self.both(template.format(cell=cell), one, "--vtr-normalize"), 0)
            self.assertEqual(self.both(template.format(cell=cell), one), 1)
        self.assertEqual(
            self.both(template.format(cell=".names a b y\n"), zero, "--vtr-normalize"), 0
        )
        self.assertEqual(
            self.both(template.format(cell=".names a b y\n-- 0\n"), one, "--vtr-normalize"), 1
        )
        nand = template.format(cell=".names a b y\n11 0\n")
        self.assertEqual(self.both(nand, nand, "--vtr-normalize"), 0)  # not constant: kept

    def test_vtr_normalize_undriven_nets_read_gnd_or_unconn(self) -> None:
        """§9.2: an undriven net is x: `gnd` on logic and outputs, `unconn` on a hard block."""
        undriven = (
            ".model top\n.inputs a\n.outputs y z o\n.names a u y\n11 1\n"
            ".latch u z re a 2\n.subckt multiply a[0]=u b[0]=a out[0]=o\n.end\n"
            ".model multiply\n.inputs a[0] b[0]\n.outputs out[0]\n.blackbox\n.end\n"
        )
        tied = (
            ".model top\n.inputs a\n.outputs y z o\n.names gnd\n.names unconn\n"
            ".names a gnd y\n11 1\n.latch gnd z re a 2\n"
            ".subckt multiply a[0]=unconn b[0]=a out[0]=o\n.end\n"
            ".model multiply\n.inputs a[0] b[0]\n.outputs out[0]\n.blackbox\n.end\n"
        )
        self.assertEqual(self.both(undriven, tied, "--vtr-normalize"), 0)
        self.assertEqual(self.both(undriven, tied), 1)
        out = ".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n"
        buffered = out.replace(".outputs y\n", ".outputs y q\n")
        gnd_q = buffered.replace(".end", ".names gnd\n.names gnd q\n1 1\n.end")
        self.assertEqual(self.both(buffered, gnd_q, "--vtr-normalize"), 0)

    def test_vtr_normalize_undriven_ties_have_negative_cases(self) -> None:
        """An undriven net is only x: a vcc, a wrong input or a real driver all differ."""
        base = ".model top\n.inputs a b\n.outputs y\n.names a u y\n11 1\n{extra}.end\n"
        undriven = base.format(extra="")
        vcc = base.format(extra=".names u\n1\n")
        wired = base.format(extra=".names b u\n1 1\n")
        gnd = base.format(extra=".names u\n")
        for other in (vcc, wired):
            self.assertEqual(self.both(undriven, other, "--vtr-normalize"), 1)
        self.assertEqual(self.both(undriven, gnd, "--vtr-normalize"), 0)  # driven low = tied low
        out = ".model top\n.inputs a\n.outputs y q\n.names a y\n1 1\n{extra}.end\n"
        self.assertEqual(
            self.both(
                out.format(extra=""), out.format(extra=".names a q\n1 1\n"), "--vtr-normalize"
            ),
            1,
        )  # an output gaining a real driver
        self.assertEqual(
            self.both(out.format(extra=""), out.format(extra=".names q\n1\n"), "--vtr-normalize"), 1
        )  # an output tied high

    def test_vtr_normalize_tells_a_tied_low_net_from_an_open_pin(self) -> None:
        """A writer regression from `unconn` to `gnd` on a hard-block input is caught."""
        stub = ".model multiply\n.inputs a[0] b[0]\n.outputs out[0]\n.blackbox\n.end\n"
        top = (
            ".model top\n.inputs a\n.outputs o\n{gen}"
            ".subckt multiply a[0]={pin} b[0]=a out[0]=o\n.end\n"
        )
        golden = top.format(gen="", pin="u") + stub  # u undriven: written as unconn
        opened = top.format(gen=".names unconn\n", pin="unconn") + stub
        tied = top.format(gen=".names gnd\n", pin="gnd") + stub
        self.assertEqual(self.both(golden, opened, "--vtr-normalize"), 0)
        self.assertEqual(self.both(golden, tied, "--vtr-normalize"), 1)
        self.assertEqual(self.both(opened, tied, "--vtr-normalize"), 1)

    def test_vtr_normalize_drops_unused_stubs_only(self) -> None:
        stub = ".model adder\n.inputs a[0]\n.outputs sumout[0]\n.blackbox\n.end\n"
        top = ".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n"
        self.assertEqual(self.both(top + stub, top, "--vtr-normalize"), 0)
        self.assertEqual(self.both(top + stub, top), 1)
        used = ".model top\n.inputs a\n.outputs y\n.subckt adder a[0]=a sumout[0]=y\n.end\n" + stub
        self.assertEqual(
            self.both(used, used + stub.replace("adder", "other"), "--vtr-normalize"), 0
        )
        self.assertEqual(self.both(used, top + stub, "--vtr-normalize"), 1)

    def test_wrapper_script(self) -> None:
        wrapper = helpers.REPO_ROOT / "tools" / "netlist-compare" / "netlist-compare"
        a, b = helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_flipped.blif")
        result = subprocess.run([str(wrapper), a, b], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        result = subprocess.run([str(wrapper), a, a], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0)


class OrderIndependenceTest(unittest.TestCase):
    """The canonical text depends on the netlist, never on the order of its cells."""

    def test_level_of_a_primary_net_does_not_depend_on_visit_order(self) -> None:
        """proc-gen-diff seed 33 (twin avoidance off): ``q1`` is a $dff output (primary) read
        by a $not; its level once depended on whether the $dff was visited before the $not."""
        path = str(Path(__file__).resolve().parent / "data" / "proc_gen_seed33.blif")
        texts = set()
        for seed in range(20):
            netlist = netlist_compare.load(path, True)
            random.Random(seed).shuffle(netlist.top.cells)
            texts.add(netlist_compare.canonicalize(netlist))
        self.assertEqual(len(texts), 1)


if __name__ == "__main__":
    unittest.main()
