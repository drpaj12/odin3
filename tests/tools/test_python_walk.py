"""Phase 1 exit test: a Python plugin walks the IR through the C ABI (plugins/python).

Every committed BLIF fixture is read through the cffi binding and walked from Python; the
per-module counts and the cell-type histogram must equal what the C `stats` pass logs for the
same design (captured through the ABI's log sink, not re-implemented). Run through CTest
`python_walk`, which sets ODIN3_LIB (and preloads the sanitizer runtime for a Debug library).
Skipped where cffi is missing, or where ODIN3_LIB is unset (`python_tools` runs this directory
without a library and without the sanitizer runtime).
"""

from __future__ import annotations

import io
import os
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from typing import TYPE_CHECKING

REPO_ROOT = Path(__file__).resolve().parents[2]
VTR_LIB = REPO_ROOT / "lib" / "vtr.o3lib"
HAND_BODY = REPO_ROOT / "tests" / "golden" / "blif" / "hand_body.blif"
HAND_TOP_LINE = 2  # `.model top`
HAND_N1_LINE = 5  # `.names a b n1`
HAND_SUB_LINE = 18  # `.subckt sub ...` (u_sub)
# Originals that drive one net from several cells (PHASE1 decision #15): the check after
# read_blif fails in a Debug library, so these are compared only against a Release library.
MULTI_DRIVER = {"elsif_both_defined.odin.blif"}

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False

if TYPE_CHECKING or HAVE_CFFI:
    import walk
    from odin3 import Design, Module, Node, Odin3, Odin3Error, Source


def fixtures() -> list[Path]:
    """Every committed BLIF fixture under tests/, smallest first."""
    found = sorted((REPO_ROOT / "tests").rglob("*.blif"))
    return sorted(found, key=lambda path: (path.stat().st_size, str(path)))


def techlibs_for(path: Path) -> list[Path]:
    """Tech-library fixtures instantiate cells of lib/vtr.o3lib; the others need none."""
    return [VTR_LIB] if path.parent.name == "techlib" else []


@unittest.skipUnless(HAVE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_walk)")
class PythonWalkTest(unittest.TestCase):
    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        cls.odin3 = Odin3()
        # Readers log timings and check warnings at INFO/WARN; keep the test output to errors.
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    def read(self, path: Path) -> Design:
        return Design.read_blif(path, techlibs=techlibs_for(path), odin3=self.odin3)

    def test_walk_equals_stats_on_every_fixture(self) -> None:
        paths = fixtures()
        self.assertGreater(len(paths), 100)
        compared = 0
        for path in paths:
            with self.subTest(fixture=str(path.relative_to(REPO_ROOT))):
                try:
                    design = self.read(path)
                except Odin3Error as err:
                    if path.name in MULTI_DRIVER and err.status == self.odin3.lib.ODIN3_ERR_CHECK:
                        continue  # a Debug library checks after read_blif
                    raise
                with design:
                    from_stats = walk.stats_lines(design)
                    self.assertGreater(len(from_stats), 1)
                    self.assertEqual(walk.walk_lines(design), from_stats)
                    compared += 1
        self.assertGreaterEqual(compared, len(paths) - len(MULTI_DRIVER))
        print(f"python_walk: {compared}/{len(paths)} fixtures, walk == stats")

    def test_pins_and_nets_agree_on_every_fixture(self) -> None:
        for path in fixtures():
            if path.name in MULTI_DRIVER:
                continue
            with self.subTest(fixture=path.name), self.read(path) as design:
                for module in design.modules():
                    self.check_module_connectivity(module)

    def check_module_connectivity(self, module: Module) -> None:
        """Every connected pin is on its net; every net pin points back at the net."""
        for node in module.nodes():
            pins = list(node.pins())
            self.assertEqual(len(pins), sum(port.width for port in node.ports()))
            for pin in pins:
                self.assertEqual(pin.node(), node)
                net = pin.net()
                if net is not None:
                    self.assertIn(pin, net.pins())
        for net in module.nets():
            drivers = net.drivers()
            self.assertEqual(net.driver(), drivers[0] if drivers else None)
            for pin in net.pins():
                self.assertEqual(pin.net(), net)
            for pin in drivers:
                self.assertIn(pin.direction(), ("out", "inout"))

    def test_counts_agree_with_abi_counts(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.top()
            assert top is not None
            self.assertEqual(top.name, "top")
            self.assertEqual(len(list(top.nodes())), top.node_count())
            self.assertEqual(len(list(top.nets())), top.net_count())
            self.assertEqual(len(list(top.wires())), top.wire_count())
            # bb is a .blackbox model: a cell type, not a module
            self.assertEqual([m.name for m in design.modules()], ["top", "sub"])

    def test_provenance_from_python(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.module("top")
            assert top is not None
            sub = top.node("u_sub")
            assert sub is not None
            self.assertEqual(sub.type_name, "sub")
            first = sub.sources()[0]
            self.assertEqual(
                first, Source(str(HAND_BODY), HAND_SUB_LINE, 1, HAND_SUB_LINE, first.end_col)
            )
            self.assertEqual(top.sources()[0].line, HAND_TOP_LINE)
            found = design.objects_at(str(HAND_BODY), HAND_SUB_LINE)
            self.assertIn(sub, [hit.obj for hit in found])
            self.assertTrue(all(hit.live for hit in found))
            self.assertIn(
                top, [hit.obj for hit in design.objects_at(str(HAND_BODY), HAND_TOP_LINE)]
            )
            self.assertEqual(design.objects_at("nope.blif", HAND_SUB_LINE), [])

    def test_provenance_of_every_node_finds_it_again(self) -> None:
        with self.read(HAND_BODY) as design:
            for module in design.modules():
                for node in module.nodes():
                    sources = node.sources()
                    if not sources:
                        continue  # port nodes and implicit objects may have none
                    hits = design.objects_at(sources[0].file, sources[0].line)
                    self.assertIn(node, [hit.obj for hit in hits])

    def test_driver_params_and_ports(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.module("top")
            assert top is not None
            net = top.net("n1")
            assert net is not None
            driver = net.driver()
            assert driver is not None
            cover = driver.node()
            self.assertEqual(cover.type_name, "$sop")
            self.assertEqual(cover.granularity, "bit")
            self.assertEqual(cover.sources()[0].line, HAND_N1_LINE)
            params = cover.params()
            self.assertIn("11 1\n", params.values())
            self.assertEqual(driver.direction(), "out")
            self.assertEqual([port.direction for port in cover.ports()].count("out"), 1)
            self.assertEqual(top.port_count(), len(top.ports()))
            self.assertEqual([port.wire.name for port in top.ports()][:3], ["a", "b", "clk"])

    def test_attributes(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.module("top")
            assert top is not None
            sub = top.node("u_sub")
            assert sub is not None
            # .attr/.param lines become prefixed string attributes; a repeated key keeps its last
            self.assertEqual(sub.attr("blif.attr:src"), '"top.v:4"')
            self.assertEqual(sub.attr("blif.param:P"), "01 01")
            self.assertIsNone(sub.attr("nope"))
            sub.set_attr("note", "from python")
            self.assertEqual(sub.attr("note"), "from python")
            top.set_attr("note", "module")
            self.assertEqual(top.attr("note"), "module")

    def test_errors_raise(self) -> None:
        with Design(self.odin3) as design:
            with self.assertRaises(Odin3Error) as ctx:
                design.run_pass("no_such_pass")
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_INVALID_ARG)
            with self.assertRaises(Odin3Error) as ctx:
                design.run_pass("read_blif", "/nonexistent/x.blif")
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_IO)
            self.assertIsNone(design.top())
            self.assertEqual(list(design.modules()), [])

    def test_script_passes_and_aliases(self) -> None:
        self.assertEqual(self.odin3.passes()[:2], ["read_blif", "read_techlib"])
        with Design(self.odin3) as design:
            design.run_script(f'read_blif "{HAND_BODY}"; compact; stats')
            top = design.top()
            assert top is not None
            self.assertEqual(top.node_count(), len(list(top.nodes())))
            for net in top.nets():
                self.assertNotIn(net.name, net.aliases())
            with self.assertRaises(Odin3Error) as ctx:
                design.run_script("no_such_pass")
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_PARSE)

    def test_walk_script(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            code = walk.main(["--check", str(HAND_BODY)])
        self.assertEqual(code, 0)
        text = out.getvalue()
        self.assertIn("module top: ports 6, nodes 18, nets 14, wires 6", text)
        self.assertIn("module top: cell $sop 4", text)
        self.assertIn("walk == stats", text)

    def test_node_is_hashable_and_equal_by_id(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.top()
            assert top is not None
            first = next(iter(top.nodes()))
            again = next(iter(top.nodes()))
            self.assertEqual(first, again)
            self.assertEqual(len({first, again}), 1)
            self.assertIsInstance(first, Node)


if __name__ == "__main__":
    unittest.main()
