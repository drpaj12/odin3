"""The Python AST helpers (plugins/python/odin3.py: Design.ast, Ast, AstNode) over ABI v4.

A small module is built through the C ABI from Python (source manager buffer, AST builders), as
an adapter would; the helpers must then find the latest run's store and walk it: kinds, names,
present children and `file:line:col` locations. Run through CTest `python_ast`, which sets
ODIN3_LIB (and preloads the sanitizer runtime for a Debug library). Skipped where cffi is
missing, unless ODIN3_REQUIRE_CFFI=1, and where ODIN3_LIB is unset (`python_tools` runs this
directory without a library).
"""

from __future__ import annotations

import os
import unittest
from typing import TYPE_CHECKING, Any

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"

if TYPE_CHECKING or HAVE_CFFI:
    from odin3 import Ast, AstNode, Design, Odin3

M_V = "module m(input a, output y);\n  assign y = a + 4'b10x?;\nendmodule\n"


class Builder:
    """Builds nodes into one store through the raw ABI (what an adapter does)."""

    def __init__(self, design: Design, run: int, form: int) -> None:
        self.odin3 = design.odin3
        self.lib: Any = design.odin3.lib
        self.ffi: Any = design.odin3.ffi
        self.ptr = design.get("odin3_design_new_ast", "odin3_ast *", form, run)
        self.srcman = design.get("odin3_design_get_srcman", "odin3_srcman *")
        spec = self.ffi.new("odin3_srcfile_spec *", {"name": self.intern("m.v"), "len": len(M_V)})
        self.file = int(self.odin3.get("odin3_srcman_build_file", "uint32_t", self.srcman, spec))
        for offset, char in enumerate(M_V):
            if char == "\n":
                self.odin3.call("odin3_srcman_build_line", self.srcman, self.file, offset + 1)

    def intern(self, text: str) -> int:
        return int(self.odin3.get("odin3_ast_build_intern", "uint32_t", self.ptr, text.encode()))

    def at(self, line: int, col: int) -> int:
        """The loc of 1-based line:col of M_V."""
        offset = sum(len(text) + 1 for text in M_V.split("\n")[: line - 1]) + col - 1
        loc = self.odin3.get("odin3_srcman_get_loc", "uint32_t", self.srcman, self.file, offset)
        return int(loc)

    def node(self, kind: str, kids: list[int], **fields: Any) -> int:
        fields["kind"] = int(getattr(self.lib, "ODIN3_AST_" + kind))
        for key in ("sub", "flags"):
            if isinstance(fields.get(key), str):
                fields[key] = int(getattr(self.lib, fields[key]))
        if isinstance(fields.get("name"), str):
            fields["name"] = self.intern(fields["name"])
        spec = self.ffi.new("odin3_ast_spec *", fields)
        ids = self.ffi.new("uint32_t[]", kids) if kids else self.ffi.NULL
        made = self.odin3.get("odin3_ast_build_node", "uint32_t", self.ptr, spec, ids, len(kids))
        return int(made)

    def module(self) -> None:
        """`module m(input a, output y); assign y = a + 4'b10x?; endmodule` in one UNIT."""
        decl_a = self.node("DECLARATOR", [0], name="a", loc=self.at(1, 16))
        port_a = self.node(
            "PORT_DECL", [0, decl_a], sub="ODIN3_AST_PORT_DECL_INPUT", loc=self.at(1, 10)
        )
        decl_y = self.node("DECLARATOR", [0], name="y", loc=self.at(1, 26))
        port_y = self.node(
            "PORT_DECL", [0, decl_y], sub="ODIN3_AST_PORT_DECL_OUTPUT", loc=self.at(1, 19)
        )
        ports = self.node("LIST", [port_a, port_y], loc=self.at(1, 9))
        lhs = self.node("IDENT", [], name="y", loc=self.at(2, 10))
        a_ref = self.node("IDENT", [], name="a", loc=self.at(2, 14))
        payload = self.odin3.get(
            "odin3_ast_number_new_str", "uint32_t", self.ptr, self.at(2, 18), 4, b"10x?"
        )
        number = self.node(
            "NUMBER", [], sub="ODIN3_AST_NUMBER_BIN", payload=int(payload), loc=self.at(2, 18)
        )
        add = self.node("BINARY", [a_ref, number], sub="ODIN3_AST_BINARY_ADD", loc=self.at(2, 14))
        net = self.node("NET_ASSIGN", [lhs, add], loc=self.at(2, 10))
        assign = self.node("CONT_ASSIGN", [0, 0, net], loc=self.at(2, 3))
        mod = self.node("MODULE", [0, ports, assign], name="m", loc=self.at(1, 1))
        self.node("UNIT", [mod], name="m.v", loc=self.at(1, 1))
        self.odin3.call("odin3_ast_finish", self.ptr)


def preorder(root: AstNode) -> list[tuple[str, str, str, str]]:
    """(kind, sub, name, loc) of every node under root, depth first, without recursion."""
    seen: list[tuple[str, str, str, str]] = []
    stack = [root]
    while stack:
        node = stack.pop()
        seen.append((node.kind, node.sub, node.name, node.loc))
        stack.extend(reversed(node.children()))
    return seen


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_ast)")
class PythonAstTest(unittest.TestCase):
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

    def parsed(self) -> int:
        return int(self.odin3.lib.ODIN3_AST_FORM_PARSED)

    def elaborated(self) -> int:
        return int(self.odin3.lib.ODIN3_AST_FORM_ELABORATED)

    def test_walk_the_built_module(self) -> None:
        with Design(self.odin3) as design:
            Builder(design, 4, self.elaborated()).module()
            tree = design.ast()
            assert tree is not None
            self.assertIsInstance(tree, Ast)
            self.assertEqual((tree.run, tree.form, len(tree)), (4, "elaborated", 13))
            roots = tree.roots()
            self.assertEqual([root.kind for root in roots], ["UNIT"])
            self.assertEqual(
                preorder(roots[0]),
                [
                    ("UNIT", "", "m.v", "m.v:1:1"),
                    ("MODULE", "", "m", "m.v:1:1"),
                    ("LIST", "", "", "m.v:1:9"),
                    ("PORT_DECL", "input", "", "m.v:1:10"),
                    ("DECLARATOR", "", "a", "m.v:1:16"),
                    ("PORT_DECL", "output", "", "m.v:1:19"),
                    ("DECLARATOR", "", "y", "m.v:1:26"),
                    ("CONT_ASSIGN", "", "", "m.v:2:3"),
                    ("NET_ASSIGN", "", "", "m.v:2:10"),
                    ("IDENT", "", "y", "m.v:2:10"),
                    ("BINARY", "+", "", "m.v:2:14"),
                    ("IDENT", "", "a", "m.v:2:14"),
                    ("NUMBER", "bin", "", "m.v:2:18"),
                ],
            )

    def test_children_skip_absent_slots(self) -> None:
        with Design(self.odin3) as design:
            Builder(design, 4, self.parsed()).module()
            tree = design.ast()
            assert tree is not None
            module = tree.roots()[0].children()[0]
            self.assertEqual([kid.kind for kid in module.children()], ["LIST", "CONT_ASSIGN"])
            self.assertEqual(tree.node(module.id), module)
            self.assertEqual(module.children()[0].children()[0].children()[0].name, "a")

    def test_latest_run_elaborated_else_parsed(self) -> None:
        with Design(self.odin3) as design:
            self.assertIsNone(design.ast())
            Builder(design, 2, self.elaborated())
            Builder(design, 5, self.parsed())
            tree = design.ast()
            assert tree is not None
            self.assertEqual((tree.run, tree.form), (5, "parsed"))
            Builder(design, 5, self.elaborated())
            tree = design.ast()
            assert tree is not None
            self.assertEqual((tree.run, tree.form), (5, "elaborated"))


if __name__ == "__main__":
    unittest.main()
