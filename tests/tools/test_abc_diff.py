"""Tests for tools/abc-check/abc-diff (3D Task 21), with fakes for odin3 and equiv-check."""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import abc_diff
import abc_equiv

from . import helpers

# The fake odin3 writes before.blif as the input and after.blif with a LUT count that depends on the
# exchange: the design name says how many LUTs the BLIF exchange makes (``l<N>``) and the ratio of
# the in-memory exchange's count (``r<per mille>``, default 1000).
FAKE_ODIN3 = """#!{python}
import json, os, re, sys
argv = sys.argv
with open(os.environ["FAKE_LOG"] + ".odin3", "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"argv": argv, "exchange": os.environ.get("ODIN3_ABC_EXCHANGE")}}) + "\\n")
script = argv[2]
design = re.search(r"read_blif (\\S+);", script).group(1)
targets = re.findall(r"write_blif (?:--vtr )?(\\S+?)(?:;|$)", script)
name = os.path.basename(design)
if "views" in name:
    sys.stderr.write("abc: module 'pow' is not in the netlist view: $pow 'c'; run lower first\\n")
    sys.exit(1)
if "broken" in name:
    sys.stderr.write("abc: something else\\n")
    sys.exit(0)
aig = os.environ.get("ODIN3_ABC_EXCHANGE") == "aig"
count = int((re.search(r"l(\\d+)", name) or [0, "1"])[1])
per_mille = int((re.search(r"r(\\d+)", name) or [0, "1000"])[1])
if aig:
    count = count * per_mille // 1000
text = ".model t\\n.inputs a\\n.outputs y\\n"
if "aig" in name and aig:
    text += "# aigside\\n"
for i in range(count):
    text += ".names a n%d\\n1 1\\n" % i
text += ".names a y\\n1 1\\n.end\\n"
for target in targets:
    with open(target, "w", encoding="utf-8") as fh:
        fh.write(open(design, encoding="utf-8").read() if "before" in target else text)
"""

# Differs when the file named in the third-last argument has "# aigside" and the design asks for it.
FAKE_EQUIV = """#!{python}
import json, os, sys
argv = sys.argv
with open(os.environ["FAKE_LOG"] + ".equiv", "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"argv": argv}}) + "\\n")
left, right = argv[-2], argv[-1]
if "ierr" in left and left.endswith("before.blif"):
    sys.stderr.write("equiv-check: cannot read\\n")
    sys.exit(3)
if "unp" in left and left.endswith("before.blif"):
    sys.exit(4)
text = open(left, encoding="utf-8").read() + open(right, encoding="utf-8").read()
if "# aigside" in text:
    print("equiv-check: NOT equivalent: outputs differ")
    sys.exit(1)
print("equiv-check: equivalent (abc cec)")
"""

CHAIN = (".model t\n.inputs a\n.outputs y\n.names a b\n1 1\n.names b c\n1 1\n.names c d\n1 1\n"
         ".names d y\n1 1\n.latch y q re clk 0\n.end\n")


def design_text() -> str:
    return ".model t\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n"


class RatioTest(unittest.TestCase):
    def test_the_explain_threshold(self) -> None:
        self.assertTrue(abc_diff.needs_explanation(1000, 1051))
        self.assertFalse(abc_diff.needs_explanation(1000, 1049))
        self.assertFalse(abc_diff.needs_explanation(1000, 1050))
        self.assertTrue(abc_diff.needs_explanation(1000, 949))
        self.assertFalse(abc_diff.needs_explanation(1000, 951))
        self.assertFalse(abc_diff.needs_explanation(0, 0))
        self.assertTrue(abc_diff.needs_explanation(0, 3))

    def test_ratio_text(self) -> None:
        self.assertEqual(abc_diff.ratio_text(1000, 1051), "1.051")
        self.assertEqual(abc_diff.ratio_text(3, 2), "0.667")
        self.assertEqual(abc_diff.ratio_text(0, 0), "1.000")
        self.assertEqual(abc_diff.ratio_text(0, 4), "inf")


class AbcDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.odin3 = self.script("odin3", FAKE_ODIN3)
        self.equiv = self.script("equiv-check", FAKE_EQUIV)
        self.log = str(self.root / "fake")
        patcher = mock.patch.dict(os.environ, {"FAKE_LOG": self.log})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.keep = self.root / "keep"

    def script(self, name: str, text: str) -> str:
        path = self.root / name
        path.write_text(text.format(python=sys.executable), encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def blif(self, rel: str, text: str) -> str:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return str(path)

    def calls(self, kind: str) -> list[dict[str, object]]:
        path = Path(self.log + "." + kind)
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

    def run_tool(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", self.odin3, "--equiv-check", self.equiv, "--keep", str(self.keep),
                *extra]
        return helpers.run_main(abc_diff.main, argv)

    def test_the_line_format_and_both_exchanges_run(self) -> None:
        design = self.blif("d/l100.blif", design_text())
        code, out, err = self.run_tool(design)
        self.assertEqual(code, 0, out + err)
        first = out.splitlines()[0]
        self.assertEqual(first, f"{design} luts 101 101 1.000 ffs 0 0 1.000 depth 1 1 1.000 "
                                f"equivalent")
        blif_call, aig_call = self.calls("odin3")
        self.assertIsNone(blif_call["exchange"])
        self.assertEqual(aig_call["exchange"], "aig")
        blif_script = str(blif_call["argv"][2])  # type: ignore[index]
        work = self.keep / "0000-l100"
        self.assertIn(
            f"abc --lut 6 --via-blif {work}/blif-files; write_blif {work}/blif/after.blif",
            blif_script)
        self.assertIn(f"abc --lut 6; write_blif {work}/aig/after.blif",
                      str(aig_call["argv"][2]))  # type: ignore[index]
        self.assertEqual(len(self.calls("equiv")), 3)  # input~blif, input~aig, blif~aig

    def test_explain_marks_a_ratio_more_than_five_percent_from_one(self) -> None:
        high = self.blif("d/l1000r1051.blif", design_text())
        low = self.blif("d/l1000r1049.blif", design_text() + "#\n")
        code, out, err = self.run_tool(high, low)
        self.assertEqual(code, 0, out + err)
        lines = {Path(ln.split()[0]).name: ln for ln in out.splitlines() if ln.startswith("/")}
        self.assertIn("luts 1001 1052 1.051 ffs 0 0 1.000 depth 1 1 1.000 explain",
                      lines["l1000r1051.blif"])
        self.assertTrue(lines["l1000r1049.blif"].endswith("equivalent"))
        self.assertIn("abc-diff: 2 designs: 2 passed, 0 failed (equivalent 1, explain 1)", out)

    def test_smallest_first_across_directories(self) -> None:
        big = self.blif("d1/l1.blif", design_text() + "#" * 50)
        small = self.blif("d1/l2.blif", design_text())
        mid = self.blif("d2/l3.blif", design_text() + "#" * 10)
        code, out, err = self.run_tool(str(self.root / "d1"), str(self.root / "d2"))
        self.assertEqual(code, 0, out + err)
        self.assertEqual([ln.split()[0] for ln in out.splitlines() if ln.startswith("/")],
                         [small, mid, big])

    def test_a_non_equivalent_pair_is_exit_one(self) -> None:
        design = self.blif("d/l2aig.blif", design_text())
        code, out, err = self.run_tool(design)
        self.assertEqual(code, 1, out + err)
        self.assertRegex(out, r"l2aig\.blif luts 3 3 1\.000 ffs 0 0 1\.000 depth 1 1 1\.000 "
                              r"differs \(input~aig: equiv-check: NOT equivalent")
        self.assertIn("1 failed", out)

    def test_an_odin3_failure_has_no_counts(self) -> None:
        design = self.blif("d/broken.blif", design_text())
        views = self.blif("d/views.blif", design_text() + "#\n")
        code, out, err = self.run_tool(design, views)
        self.assertEqual(code, 2, out + err)
        self.assertRegex(out, r"broken\.blif odin3-error \(blif exchange: ")
        self.assertRegex(out, r"views\.blif views \(blif exchange: ")

    def test_the_expect_list(self) -> None:
        diff = self.blif("d/l2aig.blif", design_text())
        fine = self.blif("d/l1.blif", design_text() + "#\n")
        expect = self.blif("expect.txt", "l2aig.blif differs x\nl1.blif differs x\n")
        code, out, err = self.run_tool("--expect-fail", expect, diff, fine)
        self.assertEqual(code, 1, out + err)
        self.assertRegex(out, r"l2aig\.blif .* differs \(listed: differs x ")
        self.assertRegex(out, r"l1\.blif .* equivalent \(XPASS: listed \(differs x\)")

    def test_a_listed_error_cannot_hide_a_difference_of_the_two_results(self) -> None:
        both = self.blif("d/ierraig.blif", design_text())
        only = self.blif("d/ierr.blif", design_text() + "#\n")
        expect = self.blif("expect.txt", "ierraig.blif error x\nierr.blif error x\n")
        code, out, err = self.run_tool("--expect-fail", expect, both, only)
        self.assertEqual(code, 1, out + err)
        self.assertRegex(out, r"ierraig\.blif .* differs \(UNEXPECTED: listed as error but differs")
        self.assertRegex(out, r"ierr\.blif .* error \(listed: error x ")
        self.assertEqual(len(self.calls("equiv")), 6)  # all three pairs of both designs
        code, out, err = self.run_tool("--expect-fail", expect, only)
        self.assertEqual(code, 0, out + err)

    def test_an_explain_design_listed_as_failing_is_an_xpass(self) -> None:
        design = self.blif("d/l1000r1051.blif", design_text())
        expect = self.blif("expect.txt", "l1000r1051.blif differs x\n")
        code, out, err = self.run_tool("--expect-fail", expect, design)
        self.assertEqual(code, 1, out + err)
        self.assertIn("XPASS: listed (differs x) but explain", out)

    def test_unproven_is_exit_four_and_absorbed_only_by_a_listing(self) -> None:
        design = self.blif("d/unp.blif", design_text())
        code, out, err = self.run_tool(design)
        self.assertEqual(code, 4, out + err)
        self.assertIn(" unproven", out)
        expect = self.blif("expect.txt", "unp.blif unproven x\n")
        code, out, err = self.run_tool("--expect-fail", expect, design)
        self.assertEqual(code, 0, out + err)
        other = self.blif("expect2.txt", "unp.blif error x\n")
        self.assertEqual(self.run_tool("--expect-fail", other, design)[0], 4)

    def test_a_missing_abc_is_exit_77(self) -> None:
        design = self.blif("d/l1.blif", design_text())
        with mock.patch.dict(os.environ, {"ODIN3_ABC": str(self.root / "no-such-abc")}):
            code, _, err = self.run_tool(design)
        self.assertEqual(code, 77)
        self.assertIn("skipped", err)

    def test_depth_of_a_cycle_is_not_defined(self) -> None:
        loop = ".model t\n.inputs a\n.outputs y\n.names a q p\n11 1\n.names p q\n1 1\n.end\n"
        counts = abc_diff.netlist_counts(Path(self.blif("loop.blif", loop)))
        self.assertEqual((counts.luts, counts.depth), (2, abc_diff.NO_DEPTH))
        line = abc_diff.line_text(Path("x"), abc_diff.Verdict("equivalent", 0.0, (counts, counts)))
        self.assertIn("depth n/a n/a n/a", line)

    def test_a_usage_error_is_exit_two(self) -> None:
        code, _, err = self.run_tool(str(self.root / "missing.blif"))
        self.assertEqual(code, 2)
        self.assertIn("no such file", err)

    def test_depth_counts_the_longest_chain(self) -> None:
        path = Path(self.blif("chain.blif", CHAIN))
        counts = abc_diff.netlist_counts(path)
        self.assertEqual((counts.luts, counts.ffs, counts.depth), (4, 1, 4))

    def test_golden_paths_need_a_heavy_run(self) -> None:
        with mock.patch.object(abc_equiv, "GOLDEN_ROOT", self.root):
            design = self.blif("g/one.blif", design_text())
            code, _, err = self.run_tool(design)
        self.assertEqual(code, 2)
        self.assertIn("heavy", err.lower())


if __name__ == "__main__":
    unittest.main()
