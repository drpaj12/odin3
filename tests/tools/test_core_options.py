"""v1.0.5 core options through the Python binding (Loki III L1 needs them without a C change).

`BusOptionTest`: `submod -bus BUSKEY` turns boundary nets tagged `NAME[BIT]` into one vector port.
Run through CTest `python_core_options`, which sets ODIN3_LIB and ODIN3_ABC_LIB. Skipped where
cffi or ODIN3_LIB is missing, unless ODIN3_REQUIRE_CFFI=1.
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

ANDS4_BLIF = (
    ".model m\n.inputs a0 a1 a2 a3 b0 b1 b2 b3\n.outputs y0 y1 y2 y3\n"
    ".names a0 b0 y0\n11 1\n.names a1 b1 y1\n11 1\n.names a2 b2 y2\n11 1\n"
    ".names a3 b3 y3\n11 1\n.end\n"
)


@unittest.skipUnless(HAVE_CFFI or os.environ.get("ODIN3_REQUIRE_CFFI") == "1", "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set")
class BusOptionTest(unittest.TestCase):
    def test_vector_ports_from_python(self) -> None:
        from odin3 import Design

        scratch = helpers.scratch(self, "core_options_")
        path = Path(scratch) / "ands4.blif"
        path.write_text(ANDS4_BLIF, encoding="utf-8")
        with Design.read_blif(path) as design:
            top = design.top()
            assert top is not None
            for node in top.nodes():
                if node.granularity != "port":
                    node.set_attr("loki_sm", "sub")
            for i in range(4):
                net = top.net(f"y{i}")
                assert net is not None
                net.set_attr("loki_bus", f"y4[{i}]")
            design.run_pass("submod", "-attr loki_sm -bus loki_bus")
            sub = design.module("sub")
            assert sub is not None
            wide = [p for p in sub.ports() if p.node.ports()[0].width == 4]
            self.assertEqual(len(wide), 1)


if __name__ == "__main__":
    unittest.main()
