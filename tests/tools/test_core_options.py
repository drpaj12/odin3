"""v1.0.5 core options through the Python binding (Loki III L1 needs them without a C change).

`ConstOptionTest`: `cone_check` and `raise` take `-const PORT=V`, a template input port tied to a
constant; a four-input cone for `A == 13` is proved against `$eq` with `-const B=13` and raised to
one `$eq` node. Run through CTest `python_core_options`, which sets ODIN3_LIB and ODIN3_ABC_LIB.
Skipped where cffi or ODIN3_LIB is missing, unless ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import unittest
from pathlib import Path

from . import helpers

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False

EQ13_BLIF = (
    ".model m\n.inputs a0 a1 a2 a3\n.outputs y\n.names a0 a1 a2 a3 y\n1011 1\n.end\n"
)
EQ_ARGS = "-attr grp=eq13 -cell $eq -param A_WIDTH=4 -param B_WIDTH=4 -param Y_WIDTH=1 -const B=13"


@unittest.skipUnless(HAVE_CFFI or os.environ.get("ODIN3_REQUIRE_CFFI") == "1", "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set")
class ConstOptionTest(unittest.TestCase):
    def test_eq_const_raise_from_python(self) -> None:
        from odin3 import Design, Odin3Error, default

        scratch = helpers.scratch(self, "core_options_")
        path = Path(scratch) / "eq13.blif"
        path.write_text(EQ13_BLIF, encoding="utf-8")
        default().load_abc()
        with Design.read_blif(path) as design:
            top = design.top()
            assert top is not None
            for node in top.nodes():
                if node.granularity != "port":
                    node.set_attr("grp", "eq13")
            for i in range(4):
                net = top.net(f"a{i}")
                assert net is not None
                net.set_attr("odin3:bind", f"A[{i}]")
            out = top.net("y")
            assert out is not None
            out.set_attr("odin3:bind", "Y[0]")
            design.run_pass("cone_check", EQ_ARGS + " -timeout 60")
            self.assertEqual(top.attr("odin3:cone_check"), "equivalent")
            cell = top.attr("odin3:cone_cell")
            assert cell is not None
            self.assertTrue(cell.endswith("-const B=13"), cell)
            design.run_pass("raise", EQ_ARGS)
            eqs = [n for n in top.nodes() if n.type_name == "$eq"]
            self.assertEqual(len(eqs), 1)
            with self.assertRaises(Odin3Error):
                design.run_pass("raise", EQ_ARGS)


if __name__ == "__main__":
    unittest.main()
