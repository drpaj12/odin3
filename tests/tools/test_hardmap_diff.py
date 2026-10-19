"""Tests for tools/hardmap-check/hardmap_diff.py (3C Task 4, spec 9.3): the classes, the expect
file, the counts and the exit codes, with fake ``odin3``, ``netlist-compare`` and ``equiv-check``.

Each fake logs its argv; ``fake.json`` beside them says what Odin III writes and what each tool
returns. Work directories are made by ``tempfile.TemporaryDirectory`` and removed by it.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from typing import Any

import hardmap_diff as hd

from .helpers import run_main

FAKE_ODIN = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "odin.log", "a") as log:
    log.write(" ".join(sys.argv[1:3]) + " :: " + script + "\n")
if spec.get("odin_signal"):
    os.kill(os.getpid(), spec["odin_signal"])
if "write_rtlil " in script:  # the lower-gap fallback run (stops before lower)
    out = script.split("write_rtlil ")[1].split(";")[0]
    Path(out).write_text(spec.get("odin_il", ""))
    sys.exit(spec.get("il_rc", 0))
sys.stderr.write(spec.get("odin_stderr", ""))
if spec.get("odin_rc"):
    sys.exit(spec["odin_rc"])
out = script.split("write_blif --vtr ")[1].split(";")[0]
Path(out).write_text(spec["odin_blif"])
"""

FAKE_COMPARE = r"""#!/usr/bin/env python3
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
with open(HERE / "compare.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
sys.exit(spec.get("compare_rc", 1))
"""

FAKE_EQUIV = r"""#!/usr/bin/env python3
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
with open(HERE / "equiv.log", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
sys.stdout.write(spec.get("equiv_out", ""))
sys.exit(spec.get("equiv_rc", 1))
"""

GOLDEN = (
    ".model top\n.inputs a\n.outputs y\n"
    ".subckt adder a[0]=a b[0]=a cin[0]=a cout[0]=n1 sumout[0]=y\n"
    ".subckt adder a[0]=a b[0]=a cin[0]=n1 cout[0]=n2 sumout[0]=y2\n"
    ".subckt multiply a[0]=a b[0]=a out[0]=m\n"
    ".latch a q re clk 0\n.end\n"
    ".model adder\n.inputs a b cin\n.outputs cout sumout\n.blackbox\n.end\n"
)
ONE_ADDER = GOLDEN.replace(".subckt adder a[0]=a b[0]=a cin[0]=n1 cout[0]=n2 sumout[0]=y2\n", "")
IL_OK = (
    "module \\other\n  cell \\multiply m9\n  end\nend\n"
    "module \\top\n  cell \\adder a1\n  end\n  cell \\adder a2\n  end\n"
    "  cell \\multiply m1\n  end\n  cell $dff d1\n  end\nend\n"
)
SIX_LATCHES = GOLDEN.replace(".latch a q re clk 0\n", ".latch a q re clk 0\n" * 7)


class ToolCase(unittest.TestCase):
    """A tmp tree: a golden, a design under micro/verilog, fakes, and a ``run`` helper."""

    def setUp(self) -> None:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        self.tmp = Path(holder.name)
        self.golden_root = self.tmp / "golden"
        self.design = self.tmp / "tests" / "micro" / "verilog" / "common" / "foo.v"
        self.design.parent.mkdir(parents=True)
        self.design.write_text("module top; endmodule\n")
        self.put_golden(GOLDEN)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for name, text in (("odin3", FAKE_ODIN), ("netlist-compare", FAKE_COMPARE),
                           ("equiv-check", FAKE_EQUIV)):
            path = self.bin / name
            path.write_text(text)
            path.chmod(0o755)
        self.spec: dict[str, Any] = {"odin_blif": GOLDEN}
        self.expect_files: list[Path] = []

    def put_golden(self, text: str, design: Path | None = None) -> Path:
        blif = self.golden_root / "regression" / "verilog" / "common" / "foo" / "foo.parmys.blif"
        blif.parent.mkdir(parents=True, exist_ok=True)
        blif.write_text(text)
        return blif

    def expect(self, text: str) -> None:
        path = self.tmp / f"expect{len(self.expect_files)}.txt"
        path.write_text(text)
        self.expect_files.append(path)

    def run_tool(self, *extra: str, design: Path | None = None) -> tuple[int, str, str]:
        (self.bin / "fake.json").write_text(json.dumps(self.spec))
        argv = ["--odin3", str(self.bin / "odin3"),
                "--netlist-compare", str(self.bin / "netlist-compare"),
                "--equiv-check", str(self.bin / "equiv-check"),
                "--golden-root", str(self.golden_root), "--techlib", "/lib/vtr.o3lib"]
        for path in self.expect_files:
            argv += ["--expect", str(path)]
        argv += [*extra, str(design or self.design)]
        return run_main(hd.main, argv)

    def log(self, name: str) -> str:
        path = self.bin / name
        return path.read_text() if path.exists() else ""


class ClassTests(ToolCase):
    def test_identical(self) -> None:
        self.spec["compare_rc"] = 0
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertIn("common/foo.v: identical", out)
        self.assertEqual(self.log("equiv.log"), "")

    def test_equivalent(self) -> None:
        self.spec["equiv_rc"] = 0
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertIn("common/foo.v: equivalent", out)
        self.assertIn("--match-unnamed", self.log("equiv.log"))
        self.assertIn("--implicit-models", self.log("compare.log"))

    def test_counts_differ_one_adder_missing(self) -> None:
        self.spec["odin_blif"] = ONE_ADDER
        self.spec["compare_rc"] = 0
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("COUNTS-DIFFER", out)
        self.assertIn("adder -1", out)
        self.assertEqual(self.log("compare.log"), "")

    def test_differs_unlisted_fails(self) -> None:
        self.spec["equiv_out"] = "equiv-check: NOT equivalent\n"
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("DIFFERS", out)

    def test_exit_1_without_verdict_line_is_an_error(self) -> None:
        code, out, _ = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("equiv-check exited 1", out)

    def test_unproven_unlisted_fails(self) -> None:
        self.spec["equiv_rc"] = hd.EQUIV_UNPROVEN
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("UNPROVEN", out)


class ListedTests(ToolCase):
    def test_listed_difference_with_reason(self) -> None:
        self.expect("common/foo.v D1 signed operands are sign extended\n")
        self.spec["equiv_out"] = "equiv-check: NOT equivalent\n"
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertIn("common/foo.v: listed:D1 (signed operands are sign extended)", out)

    def test_stale_entry_fails(self) -> None:
        self.expect("common/foo.v D1 signed\n")
        self.spec["compare_rc"] = 0
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("STALE", out)

    def test_counts_form(self) -> None:
        self.expect("common/foo.v counts:+6 latch D4: select flip-flops\n")
        self.spec["odin_blif"] = SIX_LATCHES
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertIn("listed:counts:+6 latch", out)
        self.assertEqual(self.log("compare.log"), "")

    def test_counts_beside_another_entry_is_stale(self) -> None:
        self.expect("common/foo.v counts:+6 latch D4: select flip-flops\n"
                    "common/foo.v unproven cannot decide\n")
        self.spec["odin_blif"] = SIX_LATCHES
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1, out)
        self.assertIn("STALE", out)

    def test_counts_form_wrong_delta_fails(self) -> None:
        self.expect("common/foo.v counts:+5 latch D4: select flip-flops\n")
        self.spec["odin_blif"] = SIX_LATCHES
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("COUNTS-DIFFER", out)

    def test_counts_listed_but_equal_is_counts_differ(self) -> None:
        self.expect("common/foo.v counts:+6 latch D4: select flip-flops\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("listed: latch +6", out)

    def test_unproven_listed(self) -> None:
        self.expect("common/foo.v unproven equiv-check cannot decide it\n")
        self.spec["equiv_rc"] = hd.EQUIV_UNPROVEN
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertIn("listed:unproven", out)

    def test_listed_difference_that_is_only_unproven_fails(self) -> None:
        self.expect("common/foo.v D1 signed\n")
        self.spec["equiv_rc"] = hd.EQUIV_UNPROVEN
        code, _, _ = self.run_tool()
        self.assertEqual(code, 1)

    def test_no_golden_listed(self) -> None:
        other = self.design.with_name("bar.v")
        other.write_text("x")
        self.expect("common/bar.v no-golden Parmys rejects it\n")
        code, out, _ = self.run_tool(design=other)
        self.assertEqual(code, 0, out)
        self.assertIn("listed:no-golden", out)


class ErrorTests(ToolCase):
    def test_wreduce_warning_fails_and_is_not_listable(self) -> None:
        self.expect("common/foo.v D1 anything\n")
        self.spec["compare_rc"] = 0
        self.spec["odin_stderr"] = (
            "odin3: warning: hardmap: cell 'm': left soft for lower (run wreduce first)\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("WARNED", out)
        self.assertIn("run wreduce first", out)

    def test_odin_error_is_exit_2(self) -> None:
        self.spec["odin_rc"] = 1
        self.spec["odin_stderr"] = "odin3: error: boom\n"
        code, out, _ = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("boom", out)

    def test_lower_gap_fails_unless_listed(self) -> None:
        self.spec["odin_rc"] = 1
        self.spec["odin_stderr"] = (
            "odin3: error: lower: no lowering rule for cell type '$mul' ('m1'): not yet (3B-2)\n")
        self.spec["odin_il"] = IL_OK
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("LOWER-GAP (counts equal Parmys's", out)
        self.expect("common/foo.v lower-gap waits for the $mul rule of 3B-2\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)
        self.assertIn("listed:lower-gap", out)

    def test_wreduce_warning_in_a_lower_gap_run_is_warned(self) -> None:
        self.spec["odin_rc"] = 1
        self.spec["odin_stderr"] = (
            "odin3: warning: hardmap: cell 'm': left soft for lower (run wreduce first)\n"
            "odin3: error: lower: no lowering rule for cell type '$mul' ('m1')\n")
        self.spec["odin_il"] = IL_OK
        self.expect("common/foo.v lower-gap waits for 3B-2\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("WARNED", out)

    def test_lower_gap_counts_differ_is_not_absorbed(self) -> None:
        self.spec["odin_rc"] = 1
        self.spec["odin_stderr"] = "odin3: error: lower: no lowering rule for cell type '$x'\n"
        self.spec["odin_il"] = IL_OK.replace("  cell \\multiply m1\n  end\n", "")
        self.expect("common/foo.v lower-gap waits for 3B-2\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("COUNTS-DIFFER", out)
        self.assertIn("multiply -1", out)
        self.expect("common/foo.v counts:-1 multiply D9: mapped by Parmys only\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0, out)

    def test_count_rtlil_reads_only_the_top_module(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.il"
            path.write_text(IL_OK)
            self.assertEqual(hd.count_rtlil(path, "top"), Counter(adder=2, multiply=1))

    def test_lower_gap_listed_but_odin_succeeds_is_stale(self) -> None:
        self.expect("common/foo.v lower-gap waits for 3B-2\n")
        self.spec["compare_rc"] = 0
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("STALE", out)

    def test_odin_signal_is_exit_3(self) -> None:
        self.spec["odin_signal"] = 9
        code, out, _ = self.run_tool()
        self.assertEqual(code, 3)
        self.assertIn("HARD", out)

    def test_missing_golden_is_exit_2(self) -> None:
        other = self.design.with_name("none.v")
        other.write_text("x")
        code, out, _ = self.run_tool(design=other)
        self.assertEqual(code, 2)
        self.assertIn("no golden", out)

    def test_bad_expect_lines(self) -> None:
        lines = ("common/foo.v D1\n", "common/foo.v Q9 why\n", "common/foo.v counts:+x latch w\n",
                 "common/foo.v counts:+1 wire why\n", "common/foo.v counts:+0 latch why\n")
        for line in lines:
            with self.subTest(line=line):
                self.expect(line)
                code, _, err = self.run_tool()
                self.assertEqual(code, 2)
                self.assertIn("hardmap-diff: error", err)
                self.expect_files.clear()

    def test_arch_mismatch(self) -> None:
        golden = self.golden_root / "regression" / "verilog" / "common" / "foo" / "foo.parmys.blif"
        golden.with_suffix(".prov").write_text("arch=vtr_flow/arch/other.xml\n")
        code, out, _ = self.run_tool("--arch", "/x/k6.xml")
        self.assertEqual(code, 2)
        self.assertIn("not --arch", out)


class MechanicsTests(ToolCase):
    def test_script_and_top_passed(self) -> None:
        self.spec["compare_rc"] = 0
        self.run_tool()
        line = self.log("odin.log")
        self.assertIn("--top top", line)
        for needle in ("read_techlib /lib/vtr.o3lib", "read_verilog --nolatches --vtr-compat",
                       "wreduce; opt_clean; opt -full; flatten; opt -full; hardmap; opt -full; "
                       "opt_merge -ffbits; lower; opt -fast; compact; write_blif --vtr "):
            self.assertIn(needle, line)

    def test_adder_lib_read_before_the_design(self) -> None:
        self.spec["compare_rc"] = 0
        self.run_tool("--adder-lib", "/x/adder_lib.v")
        self.assertIn("read_verilog --lib /x/adder_lib.v; read_verilog --nolatches",
                      self.log("odin.log"))
        self.run_tool("--adder-lib", "")
        self.assertNotIn("--lib", self.log("odin.log").splitlines()[-1])

    def test_keep(self) -> None:
        self.spec["compare_rc"] = 0
        keep = self.tmp / "keep"
        code, _, _ = self.run_tool("--keep", str(keep))
        self.assertEqual(code, 0)
        self.assertTrue((keep / "common_foo.v" / "odin.blif").is_file())
        self.assertTrue((keep / "common_foo.v" / "odin.log").is_file())

    def test_work_removed_without_keep(self) -> None:
        self.spec["compare_rc"] = 0
        self.run_tool()
        out = self.log("odin.log").split("write_blif --vtr ")[1].strip()
        self.assertFalse(Path(out).exists())

    def test_entries_for_designs_not_run_are_ignored(self) -> None:
        self.expect("common/other.v D3 not run here\n")
        self.spec["compare_rc"] = 0
        code, _, _ = self.run_tool()
        self.assertEqual(code, 0)

    def test_no_golden_root_skips(self) -> None:
        code, _, err = self.run_tool("--golden-root", str(self.tmp / "nowhere"))
        self.assertEqual(code, 77)
        self.assertIn("skipped", err)

    def test_summary_line(self) -> None:
        self.spec["compare_rc"] = 0
        _, out, _ = self.run_tool()
        self.assertIn("hardmap-diff: 1 designs: 1 identical", out)


class UnitTests(unittest.TestCase):
    def test_design_key(self) -> None:
        key = hd.design_key
        self.assertEqual(key(Path("/a/tests/micro/verilog/common/mult.v"), None), "common/mult.v")
        self.assertEqual(key(Path("/v/vtr_flow/benchmarks/verilog/sha.v"), None), "sha.v")
        self.assertEqual(key(Path("/x/y/z.v"), None), "z.v")
        self.assertEqual(key(Path("/x/y/z.v"), Path("/x")), "y/z.v")

    def test_count_delta(self) -> None:
        odin = Counter({"adder": 3, "latch": 2})
        gold = Counter({"adder": 3, "multiply": 1})
        self.assertEqual(hd.count_delta(odin, gold), {"latch": 2, "multiply": -1})

    def test_count_blocks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "g.blif"
            path.write_text(GOLDEN)
            self.assertEqual(hd.count_blocks(path), Counter(adder=2, multiply=1, latch=1))

    def test_script_file_is_the_single_source(self) -> None:
        text = hd.script_text(hd.SCRIPT, Path("/l"), Path("/d.v"), Path("/o.blif"))
        self.assertTrue(text.startswith("read_techlib /l; "))
        self.assertIn("; read_verilog --nolatches --vtr-compat /d.v; proc; ", text)
        self.assertNotIn("--lib", text)
        with_lib = hd.script_text(hd.SCRIPT, Path("/l"), Path("/d.v"), Path("/o"), Path("/a.v"))
        self.assertIn("read_techlib /l; read_verilog --lib /a.v; read_verilog --nolatches",
                      with_lib)
        self.assertTrue(text.endswith("write_blif --vtr /o.blif"))
        self.assertNotIn("@", text)
        self.assertNotIn("#", text)


if __name__ == "__main__":
    unittest.main()
