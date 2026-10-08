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

ALL_FIXTURES = sorted(str(p.relative_to(helpers.FIXTURES))
                      for p in helpers.FIXTURES.glob("**/*.blif"))
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
        code, out, _ = compare(helpers.fixture("comb_parmys.blif"),
                               helpers.fixture("comb_renamed.blif"))
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
        swapped = (text.replace("q0", "QA").replace("q1", "Q0").replace("q2", "q0")
                   .replace("QA", "q2").replace("Q0", "q1"))
        self.assertNotEqual(swapped, text)
        self.assertEqual(canon_text(swapped), canon_text(text))

    def test_flipped_cover_row(self) -> None:
        code, out, _ = compare(helpers.fixture("comb_parmys.blif"),
                               helpers.fixture("comb_flipped.blif"))
        self.assertEqual(code, 1)
        self.assertIn("-10 1", out)
        self.assertIn("+11 1", out)

    def test_renamed_primary_output(self) -> None:
        code, out, _ = compare(helpers.fixture("comb_parmys.blif"),
                               helpers.fixture("comb_po_renamed.blif"))
        self.assertEqual(code, 1)
        self.assertIn("+.outputs zz", out)

    def test_quiet(self) -> None:
        code, out, _ = compare("--quiet", helpers.fixture("comb_parmys.blif"),
                               helpers.fixture("comb_flipped.blif"))
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
        self.assertEqual(canon_text(shift_chain(depth, forward)),
                         canon_text(shift_chain(depth, backward)))

    def test_attrs_dropped_by_default_params_kept(self) -> None:
        base = ".model t\n.inputs a\n.outputs y\n.subckt s x=a z=y\n{extra}.end\n" \
               ".model s\n.inputs x\n.outputs z\n.blackbox\n.end\n"
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
        text = ".model t\n.inputs n0 n1\n.outputs n2\n.names n0 n1 w\n11 1\n" \
               ".names w n2\n0 1\n.end\n"
        canon = canon_text(text)
        self.assertIn(".names n0 n1 n_0", canon)

    def test_large_netlist_is_fast(self) -> None:
        n = 20000  # ~ 60k lines
        lines = [".model big", ".inputs clk " + " ".join(f"i{k}" for k in range(8)),
                 ".outputs o"]
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

    def test_wrapper_script(self) -> None:
        wrapper = helpers.REPO_ROOT / "tools" / "netlist-compare" / "netlist-compare"
        a, b = helpers.fixture("comb_parmys.blif"), helpers.fixture("comb_flipped.blif")
        result = subprocess.run([str(wrapper), a, b], capture_output=True, text=True,
                                check=False)
        self.assertEqual(result.returncode, 1)
        result = subprocess.run([str(wrapper), a, a], capture_output=True, text=True,
                                check=False)
        self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
