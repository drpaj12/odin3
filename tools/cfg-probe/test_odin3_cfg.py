"""Self-test and evidence for tools/cfg-probe/odin3_cfg.py (Gate-Breaker Algorithm 3 over the
Odin III AST).

Three designs, each compared with itself, with a renamed-identifier variant (same control
structure: SimCFG must stay 1.0) and with a restructured-control-flow variant (SimCFG must fall):

  tests/micro/verilog/micro/bm_DL_74381_ALU.v              one always + case, Verilog
  tests/micro/verilog/micro/bm_if_reset.v                  if/else + an instantiated submodule
  tests/cli/veval/refs/Prob055_conditional_ref.sv          always_comb + if chain, through slang

Run it directly (it preloads the sanitizer runtime for a debug build):
    python3 tools/cfg-probe/test_odin3_cfg.py -v
Under CTest it would run with ODIN3_LIB set, like tests/tools/test_python_ast.py.
"""

from __future__ import annotations

import importlib
import importlib.util
import re
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from types import ModuleType
from typing import ClassVar

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
HAVE_CFFI = importlib.util.find_spec("cffi") is not None

ALU = REPO_ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_DL_74381_ALU.v"
IF_RESET = REPO_ROOT / "tests" / "micro" / "verilog" / "micro" / "bm_if_reset.v"
CONDITIONAL = REPO_ROOT / "tests" / "cli" / "veval" / "refs" / "Prob055_conditional_ref.sv"

ALU_RESTRUCTURED = """\
module bm_DL_74381_ALU(s, A, B, F);
\tinput [2:0] s;
\tinput [3:0] A, B;
\toutput [3:0] F;
\treg [3:0] F;

\talways @(s or A or B)
\t\tif (s == 3'b000)
\t\t\tF = 4'b0000;
\t\telse if (s == 3'b001)
\t\t\tF = B - A;
\t\telse if (s == 3'b010)
\t\t\tF = A - B;
\t\telse if (s == 3'b011)
\t\t\tF = A + B;
\t\telse if (s == 3'b100)
\t\t\tF = A ^ B;
\t\telse if (s == 3'b101)
\t\t\tF = A | B;
\t\telse if (s == 3'b110)
\t\t\tF = A & B;
\t\telse
\t\t\tF = 4'b1111;

endmodule
"""

IF_RESET_RESTRUCTURED = """\
module bm_if_reset(clock, reset_n, a_in, b_in, c_in, d_in, out0, out1);

input clock;
input reset_n;
input [1:0] a_in;
input [1:0] b_in;
input c_in;
input d_in;
output [1:0] out0;
output out1;

reg [1:0] out0;
reg out1;
wire [1:0] temp_a;

a top_a(clock, a_in, temp_a);

always @(posedge clock or negedge reset_n)
begin
\tcase (reset_n)
\t\t1'b0:
\t\tbegin
\t\t\tout0 <= 2'b00;
\t\t\tout1 <= 1'b0;
\t\tend
\t\tdefault:
\t\tbegin
\t\t\tout0 <= a_in & b_in;
\t\t\tout1 <= c_in & d_in;
\t\tend
\tendcase
end

endmodule

module a(clock, a_in, out);
input clock;
input [1:0] a_in;
output [1:0] out;
reg [1:0] out;

always @(posedge clock)
begin
\tif (a_in == 2'b00) out <= 2'b11;
\telse if (a_in == 2'b01) out <= 2'b10;
\telse if (a_in == 2'b10) out <= 2'b01;
\telse out <= 2'b00;
end

endmodule
"""

CONDITIONAL_RESTRUCTURED = """\
module RefModule (
  input [7:0] a,
  input [7:0] b,
  input [7:0] c,
  input [7:0] d,
  output reg [7:0] min
);

  always_comb begin
    if (a <= b && a <= c && a <= d) min = a;
    else if (b <= c && b <= d) min = b;
    else if (c <= d) min = c;
    else min = d;
  end

endmodule
"""


def renamed(text: str, names: dict[str, str]) -> str:
    """text with each whole-word identifier in names replaced by its rename."""
    pattern = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\b")
    return pattern.sub(lambda match: names[match.group(0)], text)


def load_module() -> ModuleType:
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    return importlib.import_module("odin3_cfg")


@unittest.skipUnless(HAVE_CFFI, "cffi not installed")
class CfgProbeTest(unittest.TestCase):
    cfg: ClassVar[ModuleType]
    tmp: ClassVar[tempfile.TemporaryDirectory[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.cfg = load_module()
        binding = cls.cfg.load_binding()
        cls.cfg.ensure_sanitizer_preload(binding)
        cls.tmp = tempfile.TemporaryDirectory(prefix="cfgprobe-")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def variant(self, name: str, text: str, suffix: str) -> Path:
        path = Path(self.tmp.name) / (name + suffix)
        path.write_text(text, encoding="utf-8")
        return path

    def sim(self, ref: Path, gen: Path) -> float:
        binding = self.cfg.load_binding()
        value, ref_cfg, gen_cfg = self.cfg.similarity(binding, ref, gen)
        self.assertGreater(len(ref_cfg), 3)
        self.assertGreater(len(gen_cfg), 3)
        result: float = value
        return result

    def check_design(self, source: Path, transform: Callable[[str], str],
                     restructured: str) -> tuple[float, float, float]:
        text = source.read_text(encoding="utf-8")
        suffix = source.suffix
        same = self.sim(source, source)
        rename = self.sim(source, self.variant("renamed", transform(text), suffix))
        other = self.sim(source, self.variant("restructured", restructured, suffix))
        self.assertAlmostEqual(same, 1.0, places=12)
        self.assertAlmostEqual(rename, 1.0, places=12)
        self.assertLess(other, 0.99)
        self.assertGreater(other, 0.0)
        return same, rename, other

    def test_verilog_case_alu(self) -> None:
        """One always with a case: identity and renamed stay 1.0, an if-chain lowers SimCFG."""
        renaming = {"s": "sel", "A": "opA", "B": "opB", "F": "res"}
        same, rename, other = self.check_design(
            ALU, lambda text: renamed(text, renaming), ALU_RESTRUCTURED)
        print(f"ALU          .v: same {same:.6f} renamed {rename:.6f} if-chain {other:.6f}")

    def test_verilog_if_else_and_submodule(self) -> None:
        """if/else in the top plus a case in an instantiated submodule; top choice is tested."""
        renaming = {"clock": "clk", "reset_n": "rstn", "out0": "res0", "out1": "res1",
                    "a_in": "ia", "b_in": "ib", "c_in": "ic", "d_in": "id", "temp_a": "tmp_a"}
        same, rename, other = self.check_design(
            IF_RESET, lambda text: renamed(text, renaming), IF_RESET_RESTRUCTURED)
        print(f"if_reset     .v: same {same:.6f} renamed {rename:.6f} swapped-case {other:.6f}")

    def test_systemverilog_always_comb(self) -> None:
        """A VerilogEval ref through the slang adapter: always_comb with an if chain."""
        renaming = {"a": "av", "b": "bv", "c": "cv", "d": "dv", "min": "mn"}
        same, rename, other = self.check_design(
            CONDITIONAL, lambda text: renamed(text, renaming), CONDITIONAL_RESTRUCTURED)
        print(f"conditional  .sv: same {same:.6f} renamed {rename:.6f} if/else {other:.6f}")

    def test_kernel_basics(self) -> None:
        """The metric itself: the same graph is 1.0 whatever its labels, a different one is less."""
        chain = self.cfg.Cfg(("Entry", "Assign", "Exit"), ((0, 1), (1, 2)))
        relabelled = self.cfg.Cfg(("Entry", "Block", "Exit"), ((0, 1), (1, 2)))
        fork = self.cfg.Cfg(("Entry", "A", "B", "Join", "Exit"),
                            ((0, 1), (0, 2), (1, 3), (2, 3), (3, 4)))
        self.assertAlmostEqual(self.cfg.cfg_kernel(chain, chain), 1.0, places=12)
        self.assertAlmostEqual(self.cfg.cfg_kernel(chain, relabelled), 1.0, places=12)
        self.assertLess(self.cfg.cfg_kernel(chain, fork), 0.99)

    def test_unrelated_designs_lower(self) -> None:
        """Two unrelated designs are below 1.0 too; the range of these small designs is narrow."""
        value = self.sim(ALU, IF_RESET)
        print(f"unrelated    .v: {value:.6f}")
        self.assertLess(value, 0.99)

    def test_cfg_node_set(self) -> None:
        """The ALU CFG's node set: Entry, Case, eight branches, CaseJoin, Exit + expression nodes.

        A branch node that opens with a statement takes that statement's label ("Assign"), as the
        pyverilog implementation did, so the eight branches show up as eight more "Assign" nodes.
        """
        binding = self.cfg.load_binding()
        design = self.cfg.read_design(binding, ALU)
        try:
            graph = self.cfg.design_cfg(design)
        finally:
            design.close()
        labels = list(graph.labels)
        self.assertEqual(labels[0], "Entry")
        self.assertEqual(labels[-1], "Exit")
        self.assertEqual(labels.count("Case"), 1)
        self.assertEqual(labels.count("CaseJoin"), 1)
        self.assertEqual(labels.count("Assign"), 8)
        self.assertEqual(len(labels), 41)
        self.assertEqual(len(graph.edges), 70)


def main() -> int:
    if not HAVE_CFFI:
        print("skipped 'cffi not installed'")
        return 0
    module = load_module()
    binding = module.load_binding()
    module.ensure_sanitizer_preload(binding)
    print("SimCFG evidence (sigma 0.5, Gate-Breaker Algorithm 3 over the Odin III AST)")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(CfgProbeTest)
    runner = unittest.TextTestRunner(verbosity=1)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
