"""Tests for tools/vtr-qor/qor_netstats.py (3H plan Task 4, T2).

The fixtures under tests/tools/fixtures/vtr_qor/netstats are real Parmys golden BLIFs (small ones,
copied from ~/odin3-ws/golden), one ABC-mapped netlist and one hand-written file; expected.json was
made by an independent reference, not by qor_netstats. Nothing here runs VTR.

T2 checks ``depth`` against the reference, never against VTR's ``abc_depth``: on a multi-clock
design the two differ by design (see qor_netstats' docstring; final review M5).
"""

from __future__ import annotations

import contextlib
import gzip
import hashlib
import io
import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import qor_netstats
from qor_netstats import AbcHeader, MalformedBlif, NetStats, abc_header, netstats

REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "vtr_qor" / "netstats"
ABC = REPO_ROOT / "build" / "debug" / "third_party" / "abc" / "abc"
SHA = "a" * 64
COMMIT = "b" * 40
HEADER = (
    f"# abc: script=builtin:lib/abc/vtr.abc sha256={SHA} abc={COMMIT} K=6 exchange=blif depth=4\n"
)


def model(body: str, inputs: str = "i", outputs: str = "o") -> str:
    return f".model m\n.inputs {inputs}\n.outputs {outputs}\n{body}.end\n"


def chain(prefix: str, length: int, first: str) -> str:
    """``length`` inverters in a row from net ``first``, ending at ``<prefix><length>``."""
    text = ""
    prev = first
    for k in range(1, length + 1):
        text += f".names {prev} {prefix}{k}\n0 1\n"
        prev = f"{prefix}{k}"
    return text


class TempTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="qor-netstats-test-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def write(self, text: str, name: str = "x.blif") -> Path:
        path = self.tmp / name
        path.write_bytes(text.encode("latin-1"))
        return path

    def stats(self, text: str) -> NetStats:
        return netstats(self.write(text))

    def depth(self, text: str) -> int:
        return self.stats(text).depth


class CountsTest(TempTest):
    def test_counts(self) -> None:
        path = FIXTURES / "mixed.blif"
        got = netstats(path)
        self.assertEqual(
            got,
            NetStats(
                luts=3,
                buffers=2,
                ffs=1,
                adders=1,
                mults=1,
                ram_slices=1,
                depth=2,
                blif_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            ),
        )
        self.assertEqual(
            got.blif_sha256, "d9249ecffc50cdb4e1dcf75b217a4afbfcf4759f48ac6824bfbfb3e14ee92b90"
        )

    def test_golden_fixtures(self) -> None:
        expected = json.loads((FIXTURES / "expected.json").read_text(encoding="utf-8"))["files"]
        self.assertGreaterEqual(len(expected), 6)
        for name, want in sorted(expected.items()):
            with self.subTest(name):
                path = FIXTURES / name
                self.assertEqual(netstats(path), NetStats(**want))
                self.assertEqual(want["blif_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_fixture_features(self) -> None:
        """The committed fixtures do exercise buffers, RAM, adders, latches and a loop-free cut."""
        expected = json.loads((FIXTURES / "expected.json").read_text(encoding="utf-8"))["files"]
        for key in ("buffers", "ram_slices", "adders", "ffs"):
            self.assertTrue(any(v[key] > 0 for v in expected.values()), key)

    def test_lut_and_buffer_classification(self) -> None:
        cases = {
            "inverter 0 1": (".names a y\n0 1\n", 1, 0),
            "buffer 1 1": (".names a y\n1 1\n", 0, 1),
            "buffer 0 0": (".names a y\n0 0\n", 0, 1),
            "off-set 1 0": (".names a y\n1 0\n", 1, 0),
            "off-set 0 1 0 0 rows": (".names a y\n0 1\n1 1\n", 1, 0),
            "two identical rows": (".names a y\n1 1\n1 1\n", 1, 0),
            "no rows": (".names a y\n", 1, 0),
            "two inputs 11 1": (".names a b y\n11 1\n", 1, 0),
            "two inputs 1- 1": (".names a b y\n1- 1\n", 1, 0),
            "constant 1": (".names y\n1\n", 0, 0),
            "constant 0": (".names y\n", 0, 0),
        }
        for label, (names, luts, buffers) in cases.items():
            with self.subTest(label):
                got = self.stats(model(names, "a b", "y"))
                self.assertEqual((got.luts, got.buffers), (luts, buffers))

    def test_two_input_row_1_1_is_not_a_buffer(self) -> None:
        with self.assertRaisesRegex(MalformedBlif, "cover row that does not fit its .names: 1 1"):
            self.stats(model(".names a b y\n1 1\n", "a b", "y"))

    def test_exdc_stops_counting_but_needs_its_end(self) -> None:
        text = model(".names i o\n0 1\n")[: -len(".end\n")] + ".exdc\n.names i o\n0 1\n.end\n"
        self.assertEqual(self.stats(text).luts, 1)
        with self.assertRaises(MalformedBlif):
            self.stats(text[: -len(".end\n")])

    def test_text_after_end_is_ignored_even_without_model(self) -> None:
        text = model(".names i o\n0 1\n") + ".names x y\n0 1\n.latch x y\n.subckt adder a=x\n"
        got = self.stats(text)
        self.assertEqual((got.luts, got.ffs, got.adders), (1, 0, 0))

    def test_lines_before_model_are_ignored(self) -> None:
        text = ".latch a b\n.names a b\n0 1\n.subckt adder a=a\n" + model(".names i o\n0 1\n")
        got = self.stats(text)
        self.assertEqual((got.luts, got.ffs, got.adders), (1, 0, 0))

    def test_trailing_continuation_at_eof(self) -> None:
        text = ".model m\n.inputs i\n.outputs o\n.subckt adder a=i\n.end \\"
        self.assertEqual(self.stats(text).adders, 1)
        self.assertEqual(self.stats(text + "\n").adders, 1)

    def test_zero_input_names_count_nowhere(self) -> None:
        got = self.stats(model(".names gnd\n.names vcc\n1\n.names unconn\n.names i o\n0 1\n"))
        self.assertEqual((got.luts, got.buffers, got.ffs), (1, 0, 0))

    def test_hard_blocks(self) -> None:
        body = (
            ".subckt adder a=i b=i cin=i cout=c1 sumout=s1\n"
            ".subckt adder a=i b=i cin=i cout=c2 sumout=s2\n"
            ".subckt multiply a[0]=i b[0]=i out[0]=m1\n"
            ".subckt single_port_ram addr[0]=i clk=i out[0]=r1\n"
            ".subckt dual_port_ram addr1[0]=i clk=i out1[0]=r2\n"
            ".subckt dual_port_ram addr1[0]=i clk=i out1[0]=r3\n"
            ".subckt mystery a=i out=q\n"
            ".subckt adder_like a=i out=q2\n"
        )
        got = self.stats(model(body))
        self.assertEqual((got.adders, got.mults, got.ram_slices), (2, 1, 3))
        self.assertEqual((got.luts, got.buffers, got.ffs), (0, 0, 0))

    def test_latch_lines(self) -> None:
        body = ".latch d q re clk 2\n.latch d2 q2\n.latch d3 q3 fe clk 0\n"
        self.assertEqual(self.stats(model(body)).ffs, 3)

    def test_only_first_model(self) -> None:
        first = model(".names i i2 o\n11 1\n", "i i2", "o")
        stubs = (
            "\n.model other\n.inputs x\n.outputs y\n"
            ".names x y\n0 1\n.latch x y\n.subckt adder a=x\n.end\n"
            ".model adder\n.inputs a\n.outputs b\n.blackbox\n.end\n"
        )
        alone = self.stats(first)
        both = self.stats(first + stubs)
        self.assertEqual(
            (both.luts, both.buffers, both.ffs, both.adders, both.depth),
            (alone.luts, alone.buffers, alone.ffs, alone.adders, alone.depth),
        )
        self.assertEqual((both.luts, both.ffs, both.adders), (1, 0, 0))
        self.assertEqual(both.blif_sha256, hashlib.sha256((first + stubs).encode()).hexdigest())
        self.assertNotEqual(both.blif_sha256, alone.blif_sha256)

    def assertMalformed(self, text: str | bytes, why: str, name: str = "x.blif") -> None:
        path = self.tmp / name
        path.write_bytes(text if isinstance(text, bytes) else text.encode("latin-1"))
        with self.assertRaises(MalformedBlif) as caught:
            netstats(path)
        self.assertEqual(str(caught.exception).split(": malformed BLIF: ")[0], str(path))
        self.assertIn(why, str(caught.exception))

    def test_missing_end_is_malformed(self) -> None:
        self.assertMalformed(".model m\n.inputs i\n.outputs o\n.names i o\n0 1\n", "no .end")

    def test_no_model_is_malformed(self) -> None:
        self.assertMalformed(".names i o\n0 1\n", "no .model")
        self.assertMalformed("this is not a netlist\n", "no .model")
        self.assertMalformed("", "no .model")
        self.assertMalformed("# only a comment\n", "no .model")

    def test_half_a_file_is_malformed(self) -> None:
        raw = (FIXTURES / "ch_intrinsics.parmys.blif").read_bytes()
        self.assertMalformed(raw[: len(raw) // 2], "no .end")
        self.assertMalformed(raw[: raw.index(b".end\n")], "no .end")
        # Cut inside the stubs: the first model is whole, a later one is not.
        stub = raw.rindex(b".model ")
        self.assertMalformed(raw[: stub + 10], "no .end")
        # Cut right after the first model's .end: nothing is missing that the file promises.
        first = raw.index(b".end\n") + len(b".end\n")
        self.assertEqual(netstats(self.write(raw[:first].decode("latin-1"))).luts, 395)

    def test_model_structure_is_malformed(self) -> None:
        self.assertMalformed(".end\n", ".end outside a model")
        self.assertMalformed(model("") + ".end\n", ".end outside a model")
        self.assertMalformed(".model a\n.model b\n.end\n", ".model inside a model")
        self.assertMalformed(
            model(".names i j o\n11 1\n", "i j")[: -len(".end\n")] + ".model two\n.end\n",
            ".model inside a model",
        )

    def test_names_and_rows_are_checked(self) -> None:
        self.assertMalformed(model(".names\n"), ".names without an output")
        self.assertMalformed(model("0 1\n"), "cover row outside .names")
        self.assertMalformed(model(".names a y\n11 1\n"), "does not fit its .names: 11 1")
        self.assertMalformed(model(".names a y\n1 1 1\n"), "does not fit its .names: 1 1 1")
        self.assertMalformed(model(".names a y\n1 2\n"), "does not fit its .names: 1 2")
        self.assertMalformed(model(".names a y\nx 1\n"), "does not fit its .names: x 1")
        self.assertMalformed(model(".names y\n1 1\n"), "does not fit its .names: 1 1")
        self.assertMalformed(model(".names y\n2\n"), "does not fit its .names: 2")
        self.assertMalformed(model(".latch d\n"), ".latch without")

    def test_two_drivers_are_malformed(self) -> None:
        both = ".names a y\n0 1\n.names b y\n0 1\n"
        self.assertMalformed(model(both, "a b", "y"), "net 'y' has two drivers")
        self.assertMalformed(model(".names y\n1\n.names a y\n0 1\n", "a", "y"), "net 'y' has")
        self.assertMalformed(model(".names a y\n0 1\n.latch a y re clk 0\n"), "net 'y' has")
        self.assertMalformed(model(".latch a y\n.latch b y\n"), "net 'y' has two drivers")
        self.assertMalformed(model(".names a y\n1 1\n.names b y\n1 1\n"), "net 'y' has")

    def test_one_driver_per_net_is_fine(self) -> None:
        body = ".names gnd\n.names vcc\n1\n.names a y\n0 1\n.latch y q\n.latch y q2\n"
        self.assertEqual(self.stats(model(body, "a")).ffs, 2)

    def test_truncated_gz_is_malformed(self) -> None:
        raw = (FIXTURES / "ch_intrinsics.parmys.blif").read_bytes()
        packed = gzip.compress(raw)
        path = self.tmp / "t.blif.gz"
        path.write_bytes(packed[: len(packed) // 2])
        with self.assertRaises(MalformedBlif) as caught:
            netstats(path)
        self.assertTrue(str(caught.exception).startswith(f"{path}: malformed BLIF: "))
        self.assertIn("gzip", str(caught.exception))
        path.write_bytes(packed[:-8])  # the trailer (CRC and length) is gone
        with self.assertRaises(MalformedBlif):
            netstats(path)

    def test_complete_gz_of_incomplete_blif_is_malformed(self) -> None:
        self.assertMalformed(
            gzip.compress(b".model m\n.names a b\n0 1\n"), "no .end", name="g.blif.gz"
        )

    def test_comments_and_blank_lines(self) -> None:
        text = (
            "# header\n\n.model m # trailing\n.inputs a\n.outputs y\n"
            ".names a y # a comment\n\n# between rows\n1 1\n\n.end\n"
        )
        got = self.stats(text)
        self.assertEqual((got.luts, got.buffers), (0, 1))

    def test_reads_line_by_line(self) -> None:
        """The reader never asks for the whole file: no unbounded read, no readlines."""

        class Lazy(io.BufferedReader):
            def read(self, size: int | None = -1) -> bytes:
                raise AssertionError(f"read({size})")

            def readall(self) -> bytes:
                raise AssertionError("readall")

            def readlines(self, hint: int = -1) -> list[bytes]:
                raise AssertionError("readlines")

        def lazy_open(path: Path) -> io.BufferedIOBase:
            return Lazy(io.FileIO(path, "rb"))

        for name in ("mixed.blif", "ch_intrinsics.parmys.blif", "abc_buffers.blif"):
            with self.subTest(name):
                path = FIXTURES / name
                want = netstats(path)
                with mock.patch.object(qor_netstats, "_open", lazy_open):
                    self.assertEqual(netstats(path), want)
                    abc_header(path)
        with (
            mock.patch.object(Path, "read_bytes", side_effect=AssertionError("read_bytes")),
            mock.patch.object(Path, "read_text", side_effect=AssertionError("read_text")),
        ):
            self.assertEqual(netstats(FIXTURES / "mixed.blif").luts, 3)


class FormatTest(TempTest):
    def continued(self, text: str) -> str:
        """Break every directive line at each blank with a continuation."""
        out = []
        for line in text.splitlines():
            out.append(" \\\n ".join(line.split(" ")) if line.startswith(".") else line)
        return "\n".join(out) + "\n"

    def test_continuations_and_gz(self) -> None:
        plain_text = (FIXTURES / "mixed.blif").read_text(encoding="utf-8")
        cont_text = self.continued(plain_text)
        self.assertIn(" \\\n ", cont_text)
        plain = self.write(plain_text, "p.blif")
        cont = self.write(cont_text, "c.blif")
        gz_plain = self.tmp / "p.blif.gz"
        gz_plain.write_bytes(gzip.compress(plain.read_bytes()))
        gz_cont = self.tmp / "c.blif.gz"
        gz_cont.write_bytes(gzip.compress(cont.read_bytes()))
        want = netstats(plain)
        self.assertEqual(netstats(gz_plain), want)
        self.assertEqual(want.blif_sha256, hashlib.sha256(plain.read_bytes()).hexdigest())
        got = netstats(cont)
        self.assertEqual(netstats(gz_cont), got)
        self.assertEqual(got.blif_sha256, hashlib.sha256(cont.read_bytes()).hexdigest())
        self.assertNotEqual(got.blif_sha256, want.blif_sha256)
        for field in ("luts", "buffers", "ffs", "adders", "mults", "ram_slices", "depth"):
            self.assertEqual(getattr(got, field), getattr(want, field), field)

    def test_gz_sha_is_of_uncompressed_bytes(self) -> None:
        raw = (FIXTURES / "blink.parmys.blif").read_bytes()
        path = self.tmp / "b.blif.gz"
        path.write_bytes(gzip.compress(raw, mtime=1))
        self.assertEqual(netstats(path).blif_sha256, hashlib.sha256(raw).hexdigest())
        self.assertNotEqual(
            hashlib.sha256(path.read_bytes()).hexdigest(), netstats(path).blif_sha256
        )

    def test_continuation_in_cover_header_and_crlf(self) -> None:
        text = (
            ".model m\r\n.inputs a \\\r\n b\r\n.outputs y\r\n"
            ".names a \\\r\n b y\r\n11 1\r\n.end\r\n"
        )
        got = self.stats(text)
        self.assertEqual((got.luts, got.buffers), (1, 0))

    def test_continued_outputs_mark_sinks(self) -> None:
        text = model(chain("c", 3, "i"), "i", "x \\\n c3")
        self.assertEqual(self.depth(text), 3)

    def test_non_ascii_names(self) -> None:
        text = ".model m\n.inputs \xe9\n.outputs \xfc\n.names \xe9 \xfc\n0 1\n.end\n"
        self.assertEqual(self.stats(text).depth, 1)

    def test_corrupt_gz_is_malformed(self) -> None:
        path = self.tmp / "bad.blif.gz"
        path.write_bytes(b"not gzip")
        with self.assertRaises(MalformedBlif):
            netstats(path)
        packed = bytearray(gzip.compress((FIXTURES / "ch_intrinsics.parmys.blif").read_bytes()))
        middle = len(packed) // 2
        packed[middle : middle + 16] = bytes(16)
        path.write_bytes(bytes(packed))
        with self.assertRaises(MalformedBlif):
            netstats(path)

    def test_no_space_continuation(self) -> None:
        """Odin II's goldens continue a line with ``a\\`` and no blank before the backslash."""
        text = ".model m\n.inputs a b\n.outputs y\n.names a\\\nb\\\ny\n11 1\n.end\n"
        got = self.stats(text)
        self.assertEqual((got.luts, got.depth), (1, 1))

    def test_missing_file_raises(self) -> None:
        with self.assertRaises(FileNotFoundError):
            netstats(self.tmp / "nope.blif")


class DepthTest(TempTest):
    def test_depth_chain(self) -> None:
        three = model(chain("c", 3, "i"), "i", "c3")
        self.assertEqual(self.depth(three), 3)
        # Through a buffer: still 3.
        buf = model(
            ".names i b0\n1 1\n.names b0 c1\n0 1\n.names c1 b1\n1 1\n"
            ".names b1 c2\n0 1\n.names c2 b2\n0 0\n.names b2 c3\n0 1\n",
            "i",
            "c3",
        )
        got = self.stats(buf)
        self.assertEqual((got.luts, got.buffers, got.depth), (3, 3, 3))
        # A multiply in the middle cuts the path: the longer side is 2.
        cut = model(
            ".names i l1\n0 1\n.subckt multiply a[0]=l1 b[0]=i out[0]=m\n"
            ".names m l2\n0 1\n.names l2 l3\n0 1\n",
            "i",
            "l3",
        )
        got = self.stats(cut)
        self.assertEqual((got.luts, got.mults, got.depth), (3, 1, 2))

    def test_cut_other_side_longer(self) -> None:
        cut = model(
            chain("a", 3, "i") + ".subckt multiply a[0]=a3 out[0]=m\n.names m l1\n0 1\n",
            "i",
            "l1",
        )
        self.assertEqual(self.depth(cut), 3)

    def test_depth_latch_cut(self) -> None:
        body = ".names i l1\n0 1\n.latch l1 q re clk 2\n.names q l2\n0 1\n"
        self.assertEqual(self.depth(model(body, "i clk", "l2")), 1)
        longer = chain("a", 2, "i") + ".latch a2 q re clk 2\n" + chain("b", 3, "q")
        self.assertEqual(self.depth(model(longer, "i clk", "b3")), 3)

    def test_latch_input_is_a_sink(self) -> None:
        body = chain("a", 4, "i") + ".latch a4 q re clk 2\n"
        self.assertEqual(self.depth(model(body, "i clk", "q")), 4)

    def test_hard_block_input_is_a_sink(self) -> None:
        body = chain("a", 3, "i") + ".subckt adder a=a3 b=i cin=i cout=c sumout=s\n"
        self.assertEqual(self.depth(model(body, "i", "s")), 3)

    def test_hard_block_output_is_a_source(self) -> None:
        body = ".subckt adder a=i b=i cin=i cout=c sumout=s\n" + chain("a", 2, "s")
        self.assertEqual(self.depth(model(body, "i", "a2")), 2)

    def test_hard_block_output_straight_to_hard_block_input(self) -> None:
        body = (
            ".subckt adder a=i b=i cin=i cout=c sumout=s\n.subckt multiply a[0]=s b[0]=c out[0]=m\n"
        )
        self.assertEqual(self.depth(model(body, "i", "m")), 0)

    def test_dangling_logic_is_not_depth(self) -> None:
        body = chain("a", 5, "i") + ".names i k\n0 1\n"
        self.assertEqual(self.depth(model(body, "i", "k")), 1)

    def test_primary_input_to_output(self) -> None:
        self.assertEqual(self.depth(model("", "i", "i")), 0)

    def test_constants_are_sources(self) -> None:
        body = ".names vcc\n1\n.names vcc i x\n11 1\n.names x o\n0 1\n"
        self.assertEqual(self.depth(model(body, "i", "o")), 2)

    def test_reconvergence_takes_the_longest(self) -> None:
        body = chain("a", 3, "i") + ".names i a3 y\n11 1\n"
        self.assertEqual(self.depth(model(body, "i", "y")), 4)

    def test_buffer_adds_nothing_and_buffer_chain(self) -> None:
        body = ".names i b1\n1 1\n.names b1 b2\n0 0\n.names b2 b3\n1 1\n"
        got = self.stats(model(body, "i", "b3"))
        self.assertEqual((got.luts, got.buffers, got.depth), (0, 3, 0))

    def test_duplicate_input(self) -> None:
        self.assertEqual(self.depth(model(".names i i y\n11 1\n", "i", "y")), 1)

    def test_long_chain_is_iterative(self) -> None:
        length = 30000
        self.assertEqual(self.depth(model(chain("c", length, "i"), "i", f"c{length}")), length)

    def test_depth_from_unsorted_gates(self) -> None:
        body = ".names b2 y\n0 1\n.names b1 b2\n0 1\n.names i b1\n0 1\n"
        self.assertEqual(self.depth(model(body, "i", "y")), 3)


class LoopTest(TempTest):
    def run_stats(self, text: str) -> tuple[Path, NetStats, str]:
        path = self.write(text)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            got = netstats(path)
        return path, got, err.getvalue()

    def test_loop(self) -> None:
        body = ".names y2 i y1\n10 1\n.names y1 y2\n0 1\n"
        path, got, err = self.run_stats(model(body, "i", "y2"))
        self.assertEqual(got.depth, -1)
        self.assertEqual(err, f"vtr-qor: netstats: {path}: combinational loop; depth -1\n")
        self.assertEqual((got.luts, got.buffers), (2, 0))
        self.assertEqual(got.blif_sha256, hashlib.sha256(path.read_bytes()).hexdigest())

    def test_self_loop(self) -> None:
        _, got, err = self.run_stats(model(".names y i y\n11 1\n", "i", "y"))
        self.assertEqual(got.depth, -1)
        self.assertIn("combinational loop", err)

    def test_loop_through_buffers(self) -> None:
        body = ".names y1 y2\n1 1\n.names y2 y1\n1 1\n"
        _, got, err = self.run_stats(model(body, "i", "y1"))
        self.assertEqual((got.depth, got.buffers), (-1, 2))
        self.assertIn("combinational loop", err)

    def test_loop_elsewhere_still_loops(self) -> None:
        body = chain("a", 2, "i") + ".names p q\n0 1\n.names q p\n0 1\n"
        _, got, _ = self.run_stats(model(body, "i", "a2"))
        self.assertEqual(got.depth, -1)

    def test_latch_breaks_the_loop(self) -> None:
        body = ".names q i d\n11 1\n.latch d q re clk 2\n"
        _, got, err = self.run_stats(model(body, "i clk", "q"))
        self.assertEqual((got.depth, err), (1, ""))

    def test_hard_block_breaks_the_loop(self) -> None:
        body = ".names m i d\n11 1\n.subckt adder a=d b=i cin=i cout=c sumout=m\n"
        _, got, err = self.run_stats(model(body, "i", "m"))
        self.assertEqual((got.depth, err), (1, ""))

    def test_no_loop_prints_nothing(self) -> None:
        _, got, err = self.run_stats(model(chain("a", 3, "i"), "i", "a3"))
        self.assertEqual((got.depth, err), (3, ""))


class AbcStatsTest(unittest.TestCase):
    def test_abc_print_stats(self) -> None:
        """``depth == lev`` is the contract for ABC's own mapping, where the written file's
        identity buffers (a PO named after another net, a PI wired to a PO) are not levels in the
        ``print_stats`` network and ours add 0 as well. Re-reading a written file in ABC would
        count those buffers as nodes; ``abc_buffers.blif`` shows the two agree anyway when no
        buffer sits on a deepest path, which is what ABC's own mapping produces."""
        path = FIXTURES / "abc_small.blif"
        match = re.search(r"^# print_stats printed: lev = ([0-9]+)$", path.read_text(), re.M)
        assert match is not None
        lev = int(match.group(1))
        got = netstats(path)
        self.assertEqual(got.depth, lev)
        self.assertEqual((got.adders, got.mults, got.ram_slices, got.buffers), (0, 0, 0, 0))
        self.assertGreater(got.ffs, 0)
        self.assertGreater(lev, 1)

    def test_abc_print_stats_with_a_real_buffer(self) -> None:
        path = FIXTURES / "abc_buffers.blif"
        match = re.search(r"^# print_stats printed: lev = ([0-9]+)$", path.read_text(), re.M)
        assert match is not None
        got = netstats(path)
        self.assertEqual(got.buffers, 1)
        self.assertEqual(got.depth, int(match.group(1)))
        self.assertEqual((got.adders, got.mults, got.ram_slices), (0, 0, 0))

    @unittest.skipUnless(ABC.is_file(), "the debug build's ABC is not built")
    def test_abc_print_stats_live(self) -> None:
        """Ask the pinned ABC itself for the committed netlists' ``lev``."""
        for name in ("abc_small.blif", "abc_buffers.blif"):
            with self.subTest(name):
                path = FIXTURES / name
                run = subprocess.run(
                    [str(ABC), "-c", f"read_blif {path}; print_stats"],
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout=120,
                )
                match = re.search(r"lev = ([0-9]+)", run.stdout)
                assert match is not None
                self.assertEqual(netstats(path).depth, int(match.group(1)))


class AbcHeaderTest(TempTest):
    def header(self, text: str) -> AbcHeader | None:
        return abc_header(self.write(text))

    def test_abc_header(self) -> None:
        text = (
            HEADER
            + HEADER.replace("exchange=blif depth=4", "exchange=aig depth=7")
            + ".model m\n.end\n"
        )
        got = self.header(text)
        assert got is not None
        self.assertEqual(got.exchange, "aig")
        self.assertEqual(got.depth, 7)
        self.assertEqual(
            got,
            AbcHeader(
                script="builtin:lib/abc/vtr.abc",
                sha256=SHA,
                abc=COMMIT,
                k="6",
                exchange="aig",
                depth=7,
            ),
        )
        quoted = self.header(
            f'# abc: script="my dir/x.abc" sha256={SHA} abc={COMMIT} K=6 exchange=blif depth=1\n'
            ".model m\n"
        )
        assert quoted is not None
        self.assertEqual(quoted.script, "my dir/x.abc")
        self.assertIsNone(self.header(".model m\n.end\n"))
        self.assertIsNone(self.header("# Odin III write_blif --vtr\n\n.model m\n.end\n"))
        self.assertIsNone(abc_header(FIXTURES / "abc_small.blif"))
        self.assertIsNone(abc_header(FIXTURES / "mixed.blif"))

    def test_prefix_needs_the_space(self) -> None:
        for line in ("# abc:exchange=blif depth=1", "#abc: exchange=blif depth=1", " # abc: x"):
            with self.subTest(line):
                self.assertIsNone(self.header(line + "\n.model m\n"))

    def test_tab_separated_words(self) -> None:
        got = self.header("# abc: script=x\texchange=aig\tdepth=2\n")
        assert got is not None
        self.assertEqual((got.script, got.exchange, got.depth), ("x", "aig", 2))

    def test_duplicate_key_last_wins(self) -> None:
        got = self.header("# abc: exchange=aig depth=1 depth=9\n")
        assert got is not None
        self.assertEqual(got.depth, 9)

    def test_first_abc_line_is_not_used(self) -> None:
        first = HEADER.replace("depth=4", "depth=1")
        second = HEADER.replace("depth=4", "depth=2")
        got = self.header(f"# Odin III\n{first}{second}\n.model m\n")
        assert got is not None
        self.assertEqual(got.depth, 2)

    def test_line_after_model_is_not_header(self) -> None:
        self.assertIsNone(self.header(f".model m\n{HEADER}.end\n"))
        got = self.header(f"{HEADER}.model m\n# abc: script=x\n")
        assert got is not None
        self.assertEqual(got.depth, 4)

    def test_stops_at_first_non_comment_line(self) -> None:
        self.assertIsNone(self.header(f".inputs a\n{HEADER}"))

    def test_quoting(self) -> None:
        line = (
            f'# abc: script="a \\"b\\" \\\\c d" sha256={SHA} abc={COMMIT} '
            "K=none exchange=aig depth=0\n"
        )
        got = self.header(line + ".model m\n")
        assert got is not None
        self.assertEqual(got.script, 'a "b" \\c d')
        self.assertEqual((got.k, got.depth, got.sha256, got.abc), ("none", 0, SHA, COMMIT))

    def test_quoted_value_in_other_keys(self) -> None:
        got = self.header('# abc: exchange="aig" depth="3"\n')
        assert got is not None
        self.assertEqual((got.exchange, got.depth), ("aig", 3))

    def test_missing_depth_or_exchange(self) -> None:
        for line in (
            "# abc: script=x exchange=aig\n",
            "# abc: script=x depth=3\n",
            "# abc: script=x\n",
            "# abc: \n",
            "# abc: exchange= depth=3\n",
            "# abc: exchange=aig depth=\n",
        ):
            with self.subTest(line):
                path = self.write(line + ".model m\n")
                with self.assertRaises(ValueError) as caught:
                    abc_header(path)
                self.assertEqual(
                    str(caught.exception), f"{path}: '# abc:' line without depth= or exchange="
                )

    def test_malformed_lines(self) -> None:
        for line in (
            "# abc: exchange=aig depth=3 junk\n",
            "# abc: junk exchange=aig depth=3\n",
            '# abc: exchange=aig depth=3 script="a"b=c\n',
            "# abc: exchange=aig depth=3 =x\n",
            '# abc: exchange=aig depth=3 script="open\n',
            '# abc: exchange=aig depth=3 script="a"b\n',
            '# abc: exchange=aig depth=3 script="a\\n"\n',
            '# abc: exchange=aig depth=3 script="a\\\n',
            '# abc: exchange=aig depth=3 script=a"b\n',
            "# abc: exchange=aig depth=3 script=a\\b\n",
            "# abc: exchange=aig depth=x\n",
            "# abc: exchange=aig depth=3.5\n",
            "# abc: exchange=aig depth=-\n",
        ):
            with self.subTest(line):
                with self.assertRaises(ValueError) as caught:
                    self.header(line + ".model m\n")
                self.assertIn("'# abc:' line", str(caught.exception))
                self.assertIn(str(self.tmp / "x.blif"), str(caught.exception))

    def test_values_are_validated(self) -> None:
        good = f"script=x sha256={SHA} abc={COMMIT} K=6 exchange=aig depth=4"
        cases = {
            "exchange": ("exchange=aig", ["exchange=AIG", "exchange=mig", 'exchange="aig "']),
            "depth": ("depth=4", ["depth=-1", "depth=1.5", "depth=x", "depth=+3"]),
            "sha256": (
                f"sha256={SHA}",
                ["sha256=1", f"sha256={SHA[:-1]}", f"sha256={SHA}0", "sha256=" + "g" * 64],
            ),
            "abc": (f"abc={COMMIT}", ["abc=2", f"abc={COMMIT[:-1]}", "abc=" + "z" * 40]),
            "K": ("K=6", ["K=six", "K=-1", "K=6.0", "K=None"]),
        }
        for key, (ok, bad_words) in cases.items():
            for bad in bad_words:
                with self.subTest(bad):
                    path = self.write("# abc: " + good.replace(ok, bad) + "\n.model m\n")
                    with self.assertRaises(ValueError) as caught:
                        abc_header(path)
                    self.assertEqual(
                        str(caught.exception).split(" (")[0],
                        f"{path}: '# abc:' line with an invalid {key}=",
                    )

    def test_valid_forms_are_accepted(self) -> None:
        for k in ("2", "6", "16", "none"):
            for exchange in ("aig", "blif"):
                line = (
                    f"# abc: sha256={SHA.upper()} abc={COMMIT} K={k} exchange={exchange} depth=0\n"
                )
                got = self.header(line)
                assert got is not None
                self.assertEqual((got.k, got.exchange, got.sha256), (k, exchange, SHA.upper()))

    def test_truncated_gz_header_is_malformed(self) -> None:
        packed = gzip.compress(("#" * 100 + "\n").encode() * 500)
        path = self.tmp / "t.blif.gz"
        path.write_bytes(packed[: len(packed) // 2])
        with self.assertRaises(MalformedBlif):
            abc_header(path)

    def test_bare_word_is_named(self) -> None:
        for line in ("# abc: junk exchange=aig depth=3", "# abc: exchange=aig depth=3 junk"):
            with self.subTest(line):
                with self.assertRaises(ValueError) as caught:
                    self.header(line + "\n")
                self.assertIn("not key=value", str(caught.exception))

    def test_depth_forms(self) -> None:
        got = self.header("# abc: exchange=blif depth=0\n")
        assert got is not None
        self.assertEqual(got.depth, 0)
        got = self.header("# abc: exchange=blif depth=12\n")
        assert got is not None
        self.assertEqual(got.depth, 12)

    def test_unknown_keys_and_missing_optional_keys(self) -> None:
        got = self.header('# abc: future=1 exchange=blif depth=2 more="x y"\n')
        assert got is not None
        self.assertEqual(got, AbcHeader("", "", "", "", "blif", 2))

    def test_crlf_and_gz(self) -> None:
        path = self.tmp / "h.blif.gz"
        path.write_bytes(gzip.compress((HEADER.replace("\n", "\r\n") + ".model m\r\n").encode()))
        got = abc_header(path)
        assert got is not None
        self.assertEqual((got.depth, got.exchange, got.k), (4, "blif", "6"))

    def test_non_ascii_script(self) -> None:
        path = self.tmp / "u.blif"
        path.write_bytes('# abc: script="déjà vu.abc" exchange=aig depth=1\n'.encode())
        got = abc_header(path)
        assert got is not None
        self.assertEqual(got.script, "déjà vu.abc")

    def test_binary_body_is_never_decoded(self) -> None:
        path = self.tmp / "bin.blif"
        path.write_bytes(HEADER.encode() + b".model m\n" + bytes(range(256)) * 64)
        got = abc_header(path)
        assert got is not None
        self.assertEqual(got.depth, 4)

    def test_missing_file(self) -> None:
        with self.assertRaises(FileNotFoundError):
            abc_header(self.tmp / "nope.blif")


class SourceTest(unittest.TestCase):
    def test_no_environment_access(self) -> None:
        text = (REPO_ROOT / "tools" / "vtr-qor" / "qor_netstats.py").read_text(encoding="utf-8")
        for needle in ("os.environ", "getenv", "import os"):
            self.assertNotIn(needle, text)


if __name__ == "__main__":
    unittest.main()
