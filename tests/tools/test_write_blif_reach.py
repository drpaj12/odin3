"""write_blif --reachable writes the top and the modules reachable from it, not modules nothing
instantiates (Parmys writes only the top's netlist; Yosys's write_blif and the default here write
every module). The skipped module stays in the design and a WARNING names it. Run through CTest
`write_blif_reach` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")

VERILOG = """
module leaf(input a, output y); assign y = ~a; endmodule
module mid(input a, output y); leaf u(.a(a), .y(y)); endmodule
module unused(input a, output y); assign y = a; endmodule
module top(input a, output y); mid u(.a(a), .y(y)); endmodule
"""


def run(script_body: str, flags: str = "", top: str = "top") -> tuple[list[str], str]:
    with tempfile.TemporaryDirectory() as tmp:
        src, out = Path(tmp) / "d.v", Path(tmp) / "o.blif"
        src.write_text(VERILOG)
        script = (
            f'read_verilog "{src}"; proc; opt; lower; {script_body}; write_blif {flags} "{out}"'
        )
        done = subprocess.run(
            [str(ODIN3), "--top", top, "-p", script], capture_output=True, text=True
        )
        assert done.returncode == 0, done.stderr
        return re.findall(r"^\.model (\S+)", out.read_text(), re.M), done.stderr


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest write_blif_reach)")
class WriteBlifReachTest(unittest.TestCase):
    def test_hierarchy_is_written_without_warning(self) -> None:
        models, log = run("opt", "--reachable")
        self.assertEqual(models, ["top", "mid", "leaf"])
        self.assertNotIn("not reachable", log)

    def test_default_after_flatten_writes_the_top_only(self) -> None:
        # Yosys's flatten deletes the modules it expanded; the IR cannot, so flatten marks them
        # and the default write_blif leaves them out with one INFO line.
        models, log = run("flatten")
        self.assertEqual(models, ["top"])
        self.assertNotIn("not reachable", log)
        self.assertIn("2 modules that flatten left unused not written", log)
        self.assertIn("deletes them): mid, leaf", log)  # the names, not only the count

    def test_flattened_away_mark_is_internal(self) -> None:
        # the mark is read by write_blif only; no other writer shows it
        writers = (("write_rtlil", "il"), ("write_json", "json"), ("write_verilog", "v"))
        for writer, suffix in writers:
            with tempfile.TemporaryDirectory() as tmp:
                src, out = Path(tmp) / "d.v", Path(tmp) / f"o.{suffix}"
                src.write_text(VERILOG)
                script = f'read_verilog "{src}"; proc; opt; lower; flatten; {writer} "{out}"'
                done = subprocess.run(
                    [str(ODIN3), "--top", "top", "-p", script], capture_output=True, text=True
                )
                self.assertEqual(done.returncode, 0, done.stderr)
                text = out.read_text()
            self.assertNotIn("flattened away", text, writer)
            self.assertNotIn("flattened_away", text, writer)

    def test_second_flatten_with_another_top_keeps_the_new_root(self) -> None:
        # flatten with top `top` marks mid and leaf; after `hierarchy --top mid` a second flatten
        # keeps mid as a root, so the stale mark must go and mid is written.
        models, log = run("flatten; hierarchy --top mid; flatten")
        self.assertEqual(models, ["mid"])
        self.assertIn("deletes them): top, leaf", log)

    def test_default_writes_unreachable_modules_with_one_warning(self) -> None:
        models, log = run("opt")
        self.assertEqual(models, ["top", "mid", "leaf"])
        self.assertNotIn("not reachable", log)

    def test_default_without_flatten_does_not_warn(self) -> None:
        _, log = run("opt")
        self.assertNotIn("not reachable", log)

    def test_top_given_keeps_only_that_module_no_warning(self) -> None:
        # --top unused: the elaborated design holds only the top, nothing unreachable is kept
        models, log = run("flatten", "--reachable", top="unused")
        self.assertEqual(models, ["unused"])
        self.assertNotIn("not reachable", log)

    def test_no_top_warns_reachable_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            script = f'write_blif --reachable "{Path(tmp) / "o.blif"}"'
            done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("--reachable ignored: the design has no top", done.stderr)

    def test_flattened_modules_nothing_instantiates_are_not_written(self) -> None:
        models, log = run("flatten", "--reachable")
        self.assertEqual(models, ["top"])
        self.assertIn("--reachable: module 'mid' is not reachable from the top", log)
        self.assertIn("module 'leaf' is not reachable from the top", log)
        self.assertIn("the module stays in the design", log)


if __name__ == "__main__":
    unittest.main()


PARAM_VERILOG = """
module child #(parameter W = 1)(input [W-1:0] a, output [W-1:0] y); assign y = ~a; endmodule
module top(input [1:0] a, input [3:0] b, output [1:0] y, output [3:0] z);
  child #(.W(2)) u0(.a(a), .y(y));
  child #(.W(4)) u1(.a(b), .y(z));
endmodule
"""


def run_param(script_body: str) -> tuple[str, str]:
    with tempfile.TemporaryDirectory() as tmp:
        src, out = Path(tmp) / "p.v", Path(tmp) / "o.blif"
        src.write_text(PARAM_VERILOG)
        script = f'read_verilog "{src}"; proc; opt; {script_body}; write_blif "{out}"'
        done = subprocess.run(
            [str(ODIN3), "--top", "top", "-p", script], capture_output=True, text=True
        )
        assert done.returncode == 0, done.stderr
        return out.read_text(), done.stderr


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest write_blif_reach)")
class SpecialisedModuleTest(unittest.TestCase):
    """A child instanced twice with different parameters is two specialised modules."""

    def test_flatten_expands_both_and_writes_one_model(self) -> None:
        text, log = run_param("flatten; opt; lower")
        self.assertEqual(re.findall(r"^\.model (\S+)", text, re.M), ["top"])
        self.assertNotIn(".subckt child", text)
        self.assertIn("flatten: 2 instances expanded", log)

    def test_hierarchical_names_carry_no_equals_sign(self) -> None:
        # Yosys's write_blif writes '=' in a name as '?'; ABC cuts a .subckt type at '='.
        text, log = run_param("lower")
        models = re.findall(r"^\.model (\S+)", text, re.M)
        self.assertEqual(len(models), 3, models)
        self.assertTrue(all("=" not in name for name in models), models)
        subckts = {t for t in re.findall(r"^\.subckt (\S+)", text, re.M) if not t.startswith("$")}
        self.assertEqual(subckts, {m for m in models if m != "top"})
        self.assertIn("written as '?'", log)
        self.assertIn("2 distinct model and .subckt names", log)  # 2 modules, not 4 uses

    def test_black_box_model_and_subckt_names_match(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "b.v", Path(tmp) / "o.blif"
            src.write_text("module top(input a, output y); \\bb=x#<> u(.i(a), .o(y)); endmodule\n")
            script = f'read_verilog --auto-blackbox "{src}"; proc; opt; lower; write_blif "{out}"'
            done = subprocess.run(
                [str(ODIN3), "--top", "top", "-p", script], capture_output=True, text=True
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            text = out.read_text()
        self.assertIn(".subckt bb?x??? i=a o=y", text)
        self.assertIn(".model bb?x???", text)
        self.assertNotIn("=x", text.replace("=a", ""))
