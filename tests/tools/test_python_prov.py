"""V1.2.0 Task 3: the provenance readers of ABI v7 through the Python binding.

`Node.provenance()`, `Net.provenance()`, `Module.provenance()` and `chain()` over
odin3_obj_get_prov, odin3_prov_get_info, odin3_prov_visit_parents, odin3_prov_visit_joined and
odin3_prov_visit_chain, on a design whose construct table the test-only odin3_procfix library
fills by hand (tests/unit/construct_fixture_lib.c: the elaborator fills it from Task 4), and on a
design a real read made (no table yet: the chain is empty, the records still answer). Run through
CTest `python_prov`, which sets ODIN3_LIB and ODIN3_PROC_LIB. Skipped where cffi is missing,
unless ODIN3_REQUIRE_CFFI=1, and where the libraries are not set.
"""

from __future__ import annotations

import os
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"
PROC_LIB = os.environ.get("ODIN3_PROC_LIB")

if TYPE_CHECKING or HAVE_CFFI:
    from odin3 import Construct, Design, Odin3, Odin3Error

REPO_ROOT = Path(__file__).resolve().parents[2]
VERILOG = REPO_ROOT / "tests" / "micro" / "verilog" / "full" / "ansiportlist_2.v"


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(PROC_LIB, "ODIN3_PROC_LIB not set (run CTest python_prov)")
class ConstructDesignTest(unittest.TestCase):
    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        assert PROC_LIB is not None
        cls.odin3 = Odin3(Path(PROC_LIB))
        cls.odin3.ffi.cdef("odin3_design *odin3_test_construct_design(void);")
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    def design(self) -> Design:
        return Design(self.odin3, handle=self.odin3.lib.odin3_test_construct_design())

    def test_abi_version_is_7(self) -> None:
        self.assertEqual(self.odin3.abi_version(), 7)

    def test_enum_values_reach_python(self) -> None:
        lib = self.odin3.lib
        self.assertEqual(
            [int(lib.ODIN3_LEVEL_NONE), int(lib.ODIN3_LEVEL_STMT), int(lib.ODIN3_LEVEL_SITE)],
            [0, 5, 12],
        )
        self.assertEqual(int(lib.ODIN3_DIR_COARSEN), 3)
        self.assertEqual(int(lib.ODIN3_PROV_F_SPAN_ONLY), 16)
        self.assertEqual(int(lib.ODIN3_CONSTRUCT_CALL_SITE), 0x82)

    def test_construct_table_rows(self) -> None:
        with self.design() as design:
            self.assertEqual(design.construct_count(), 7)
            module = design.construct(1)
            self.assertEqual((module.kind, module.level, module.name, module.parent),
                             ("MODULE", "module", "top", 0))
            stmt = design.construct(4)
            self.assertEqual((stmt.kind, stmt.level, stmt.parent, stmt.file, stmt.line),
                             ("BLOCKING_ASSIGN", "stmt", 2, "t.v", 40))
            with self.assertRaises(Odin3Error):
                design.construct(0)
            with self.assertRaises(Odin3Error):
                design.construct(7)

    def test_node_chain_leaf_to_module(self) -> None:
        with self.design() as design:
            top = design.module("top")
            assert top is not None
            node = next(iter(top.nodes()))
            chain = node.chain()
            self.assertEqual([c.row for c in chain], [5, 4, 2, 1])
            pairs = node.chain_with_leaves()
            self.assertEqual([c.row for c, _leaf in pairs], [5, 4, 2, 1])
            self.assertEqual(len({leaf for _c, leaf in pairs}), 1)  # one leaf reached them all
            self.assertEqual(pairs[0][1], node.provenance()[0].record)
            self.assertEqual([c.level for c in chain], ["expr", "stmt", "block", "module"])
            self.assertEqual(chain[0].kind, "BINARY")
            self.assertTrue(all(isinstance(c, Construct) for c in chain))

    def test_node_provenance_is_the_source_record(self) -> None:
        with self.design() as design:
            top = design.module("top")
            assert top is not None
            node = next(iter(top.nodes()))
            records = node.provenance()
            self.assertEqual(len(records), 1)
            record = records[0]
            self.assertEqual((record.kind, record.dir, record.parents), ("source", "origin", ()))
            self.assertEqual(record.flags, frozenset())
            assert record.construct is not None
            self.assertEqual(record.construct.row, 5)
            self.assertEqual((record.from_level, record.to_level), ("none", "none"))

    def test_module_provenance_shows_the_join(self) -> None:
        with self.design() as design:
            top = design.module("top")
            assert top is not None
            records = top.provenance()
            self.assertEqual([r.kind for r in records], ["derived", "source", "source"])
            join = records[0]
            self.assertEqual((join.dir, join.from_level, join.to_level),
                             ("coarsen", "stmt", "word"))
            self.assertEqual(join.flags, frozenset({"join"}))
            self.assertEqual(join.n_joined, 2)
            self.assertEqual([c.row for c in join.joined], [4, 6])
            self.assertEqual(join.parents, (records[1].record, records[2].record))
            self.assertEqual([c.row for c in top.chain()], [5, 4, 2, 1, 6, 3])
            leaves = [leaf for _c, leaf in top.chain_with_leaves()]
            self.assertEqual(leaves, [records[1].record] * 4 + [records[2].record] * 2)

    def test_net_without_provenance(self) -> None:
        with self.design() as design:
            top = design.module("top")
            assert top is not None
            net = top.net("w")
            assert net is not None
            self.assertEqual(net.provenance(), [])
            self.assertEqual(net.chain(), [])


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_prov)")
class ReadDesignTest(unittest.TestCase):
    """A design a real read made: records answer, the construct table is not filled yet."""

    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        cls.odin3 = Odin3()
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    @unittest.skipIf("ODIN3_PROV_AST" in os.environ, "ODIN3_PROV_AST is set by the caller")
    def test_design_does_not_touch_environment(self) -> None:
        with Design(self.odin3) as design:
            design.run_pass("read_verilog", f'"{VERILOG}"')
            self.assertNotIn("ODIN3_PROV_AST", os.environ)
            self.assertGreater(design.construct_count(), 1)
        with Design(self.odin3) as design:
            design.run_script(f'read_verilog "{VERILOG}"; check')
            self.assertNotIn("ODIN3_PROV_AST", os.environ)
            self.assertGreater(design.construct_count(), 1)

    def test_records_after_proc_and_opt(self) -> None:
        with Design(self.odin3) as design:
            design.run_pass("read_verilog", f'"{VERILOG}"')
            design.run_pass("proc")
            design.run_pass("opt")
            # Design() asks for --prov-ast refs (the CLI default is off), so the read made rows
            self.assertGreater(design.construct_count(), 1)
            top = design.top()
            assert top is not None
            seen = 0
            chained = 0
            for node in top.nodes():
                records = node.provenance()
                if not records:
                    continue
                seen += 1
                sources = node.sources()
                self.assertTrue(sources)
                leaves = [r for r in records if r.kind == "source"]
                self.assertTrue(leaves)
                chain = node.chain()
                if chain:
                    chained += 1
                    self.assertIn("module", [c.level for c in chain])
                for record in records:
                    self.assertIn(record.dir, ("origin", "refine", "rewrite", "coarsen", "context"))
                    if record.kind == "derived":
                        self.assertNotEqual(record.dir, "origin")
                        self.assertNotEqual(record.to_level, "none")
            self.assertGreater(seen, 0)
            self.assertGreater(chained, 0)


if __name__ == "__main__":
    unittest.main()
