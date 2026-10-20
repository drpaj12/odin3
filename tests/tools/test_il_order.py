"""Tests for tools/opt-check/il_order.py: the operand-first order of an RTLIL file's cells.

The walk mirrors src/passes/opt/wreduce_order.c (3G WR-7); the cell table is il_celltypes.txt,
which the unit test test_il_celltypes keeps equal to the registry.
"""

from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

import il_order

TOOL = Path(il_order.__file__).resolve()


def cell(kind: str, name: str, **ports: str) -> str:
    lines = [f"  cell {kind} {name}"]
    lines += [f"    connect \\{port} {sig}" for port, sig in ports.items()]
    return "\n".join([*lines, "  end"])


def module(
    *parts: str, wires: int = 12, public: tuple[str, ...] = ("x", "y", "z", "a", "b")
) -> str:
    decl = [f"  wire $n{i}" for i in range(wires)] + [f"  wire \\{n}" for n in public]
    return "\n".join(["module \\m", *decl, *parts, "end"]) + "\n"


def names(text: str) -> list[str]:
    return [ln.split()[2] for ln in text.splitlines() if ln.startswith("  cell ")]


def order(text: str) -> list[str]:
    return names(il_order.reorder_text(text))


class TestOrder(unittest.TestCase):
    def test_operand_goes_before_its_consumer(self) -> None:
        text = module(
            cell("$add", "$c1", A="$n1", B="$n2", Y="$n3"),
            cell("$sub", "$c2", A="\\x", B="$n9", Y="$n1"),
        )
        self.assertEqual(order(text), ["$c2", "$c1"])

    def test_ties_keep_file_order(self) -> None:
        text = module(
            cell("$and", "$c1", A="\\x", B="\\y", Y="$n1"),
            cell("$or", "$c2", A="\\z", B="\\a", Y="$n2"),
        )
        self.assertEqual(order(text), ["$c1", "$c2"])

    def test_port_a_before_port_b(self) -> None:
        text = module(
            cell("$add", "$c1", A="$n1", B="$n2", Y="$n3"),
            cell("$not", "$c2", A="\\x", Y="$n2"),
            cell("$not", "$c3", A="\\y", Y="$n1"),
        )
        self.assertEqual(order(text), ["$c3", "$c2", "$c1"])

    def test_concatenation_is_walked_lsb_first(self) -> None:
        # { $n2 $n1 }: $n1 is bit 0, so its driver goes first although it is later in the file
        text = module(
            cell("$add", "$c1", A="{ $n2 $n1 }", B="2'00", Y="{ $n8 $n7 }"),
            cell("$not", "$c2", A="\\x", Y="$n2"),
            cell("$not", "$c3", A="\\y", Y="$n1"),
        )
        self.assertEqual(order(text), ["$c3", "$c2", "$c1"])

    def test_slice_selects_the_driver_of_its_bits(self) -> None:
        text = (
            "\n".join(
                [
                    "module \\m",
                    "  wire width 2 $bus",
                    "  wire \\x",
                    "  wire $n5",
                    cell("$not", "$c1", A="$bus [1]", Y="$n5"),
                    cell("$not", "$c2", A="\\x", Y="$bus [0]"),
                    cell("$not", "$c3", A="\\x", Y="$bus [1]"),
                    "end",
                ]
            )
            + "\n"
        )
        self.assertEqual(order(text), ["$c3", "$c1", "$c2"])

    def test_whole_bus_depends_on_every_bit_driver(self) -> None:
        text = (
            "\n".join(
                [
                    "module \\m",
                    "  wire width 2 $bus",
                    "  wire \\x",
                    "  wire $n5",
                    cell("$not", "$c1", A="$bus", Y="$n5"),
                    cell("$not", "$c2", A="\\x", Y="$bus [1]"),
                    cell("$not", "$c3", A="\\x", Y="$bus [0]"),
                    "end",
                ]
            )
            + "\n"
        )
        self.assertEqual(order(text), ["$c3", "$c2", "$c1"])

    def test_first_driver_of_a_net_is_followed(self) -> None:
        text = module(
            cell("$not", "$c1", A="$n1", Y="$n5"),
            cell("$tribuf", "$c2", A="\\x", EN="\\y", Y="$n1"),
            cell("$tribuf", "$c3", A="\\x", EN="\\y", Y="$n1"),
        )
        self.assertEqual(order(text), ["$c2", "$c1", "$c3"])

    def test_mux_reads_s_then_b_then_a(self) -> None:
        text = module(
            cell("$mux", "$c1", A="$n1", B="$n2", S="$n3", Y="$n4"),
            cell("$not", "$c2", A="\\x", Y="$n1"),
            cell("$not", "$c3", A="\\x", Y="$n2"),
            cell("$not", "$c4", A="\\x", Y="$n3"),
        )
        self.assertEqual(order(text), ["$c4", "$c3", "$c2", "$c1"])

    def test_named_net_is_a_statement_boundary(self) -> None:
        text = module(
            cell("$not", "$c1", A="\\w", Y="$n5"),
            cell("$not", "$c2", A="\\x", Y="$n6"),
            "  connect \\w $n6",
            public=("x", "w"),
        )
        self.assertEqual(order(text), ["$c1", "$c2"])

    def test_wire_aliased_by_a_module_connect_is_named(self) -> None:
        text = module(
            cell("$not", "$c1", A="$n6", Y="$n5"),
            cell("$not", "$c2", A="\\x", Y="$n6"),
            "  connect \\w $n6",
            public=("x", "w"),
        )
        self.assertEqual(order(text), ["$c1", "$c2"])

    def test_module_connect_is_followed_for_an_anonymous_alias(self) -> None:
        text = module(
            cell("$not", "$c1", A="$n7", Y="$n5"),
            cell("$not", "$c2", A="\\x", Y="$n6"),
            "  connect $n7 $n6",
        )
        self.assertEqual(order(text), ["$c2", "$c1"])

    def test_cell_connects_are_not_aliases(self) -> None:
        # a public wire named like a port (\\A, \\Y, \\Q, \\D) must not alias anything (review H1).
        # If cell-internal `connect \\A ...` lines were read as module-level aliases, \\A would
        # stand for $n9 (the A of the cell that reads it), $n9 would count as named and c3 would
        # not be placed after c2.
        for port_name in ("w", "A", "B", "Y", "Q", "D"):
            text = module(
                cell("$not", "$c3", A="$n9", Y="$n10"),
                cell("$not", "$c2", A="\\x", Y="$n9"),
                cell("$not", "$c1", A=f"\\{port_name}", Y="$n8"),
                public=("x", port_name),
            )
            self.assertEqual(order(text), ["$c2", "$c3", "$c1"], port_name)

    def test_storage_output_is_not_followed(self) -> None:
        for kind, ports in (
            ("$dff", {"CLK": "\\x", "D": "$n6", "Q": "$n5"}),
            ("$dffsr", {"CLK": "\\x", "SET": "\\y", "CLR": "\\z", "D": "$n6", "Q": "$n5"}),
            ("$mem_v2", {"RD_ADDR": "\\x", "RD_DATA": "$n5"}),
        ):
            text = module(cell("$not", "$c1", A="$n5", Y="$n6"), cell(kind, "$c2", **ports))
            self.assertEqual(order(text), ["$c1", "$c2"], kind)

    def test_alu_outputs_x_and_co_are_driven(self) -> None:
        text = module(
            cell("$not", "$c1", A="$n2", Y="$n5"),
            cell("$not", "$c3", A="$n3", Y="$n6"),
            cell("$alu", "$c2", A="\\x", B="\\y", CI="1'0", BI="1'0", X="$n2", Y="$n4", CO="$n3"),
        )
        self.assertEqual(order(text), ["$c2", "$c1", "$c3"])

    def test_instance_outputs_are_driven_and_inputs_read(self) -> None:
        sub = "\n".join(["module \\sub", "  wire input 1 \\i", "  wire output 2 \\o", "end"])
        top = module(
            cell("$not", "$c1", A="$n2", Y="$n5"),
            cell("\\sub", "\\u1", i="$n1", o="$n2"),
            cell("$not", "$c3", A="\\x", Y="$n1"),
        )
        self.assertEqual(order(sub + "\n" + top), ["$c3", "\\u1", "$c1"])

    def test_instance_of_an_unknown_module_fails(self) -> None:
        with self.assertRaises(il_order.OrderError):
            order(module(cell("\\nosuch", "\\u1", o="$n2"), cell("$not", "$c3", A="$n2", Y="$n1")))

    def test_unknown_cell_type_fails(self) -> None:
        with self.assertRaises(il_order.OrderError):
            order(
                module(
                    cell("$frobnicate", "$c1", A="\\x", Y="$n1"),
                    cell("$not", "$c2", A="$n1", Y="$n2"),
                )
            )

    def test_unknown_port_fails(self) -> None:
        with self.assertRaises(il_order.OrderError):
            order(
                module(cell("$not", "$c1", Z="\\x", Y="$n1"), cell("$not", "$c2", A="$n1", Y="$n2"))
            )

    def test_unread_signal_fails(self) -> None:
        for sig in ("\\undeclared", "$n1 [9:0]", "{ $n1"):
            with self.assertRaises(il_order.OrderError, msg=sig):
                order(
                    module(
                        cell("$not", "$c1", A=sig, Y="$n2"), cell("$not", "$c2", A="$n2", Y="$n3")
                    )
                )

    def test_loop_terminates(self) -> None:
        text = module(cell("$not", "$c1", A="$n2", Y="$n1"), cell("$not", "$c2", A="$n1", Y="$n2"))
        self.assertEqual(sorted(order(text)), ["$c1", "$c2"])

    def test_deep_chain_has_no_recursion(self) -> None:
        count = 100_000
        parts = [cell("$not", f"$c{k}", A=f"$n{k - 1}", Y=f"$n{k}") for k in range(count, 0, -1)]
        text = module(*parts, wires=count + 1)
        got = order(text)
        self.assertEqual(got[0], "$c1")
        self.assertEqual(got[-1], f"$c{count}")


class TestBytes(unittest.TestCase):
    def test_result_is_a_permutation_and_other_lines_stay(self) -> None:
        text = "attribute \\top 1\n" + module(
            "  attribute \\keep 1\n" + cell("$add", "$c1", A="$n1", B="\\x", Y="$n3"),
            "  process $p\n    assign \\y \\x\n  end",
            cell("$not", "$c2", A="\\x", Y="$n1"),
            "  connect \\z $n3",
        )
        out = il_order.reorder_text(text)
        self.assertEqual(sorted(out.splitlines()), sorted(text.splitlines()))
        lines = out.splitlines()
        self.assertEqual(lines[lines.index("  attribute \\keep 1") + 1], "  cell $add $c1")
        # the process and the connect keep their lines (the cell slots are 1st and 3rd)
        self.assertLess(lines.index("  process $p"), lines.index("  cell $add $c1"))
        self.assertEqual(names(out), ["$c2", "$c1"])

    def test_crlf_and_missing_final_newline_survive(self) -> None:
        text = (
            module(
                cell("$add", "$c1", A="$n1", B="\\x", Y="$n3"),
                cell("$not", "$c2", A="\\x", Y="$n1"),
            )
            .replace("\n", "\r\n")
            .rstrip("\r\n")
        )
        out = il_order.reorder_text(text)
        self.assertFalse(out.endswith("\n"))
        self.assertEqual(sorted(out.split("\r\n")), sorted(text.split("\r\n")))
        self.assertEqual(out.count("\r\n"), text.count("\r\n"))
        self.assertEqual(names(out.replace("\r\n", "\n")), ["$c2", "$c1"])

    def test_two_modules_are_ordered_apart(self) -> None:
        one = module(
            cell("$add", "$c1", A="$n1", B="\\x", Y="$n3"), cell("$not", "$c2", A="\\x", Y="$n1")
        )
        two = one.replace("\\m", "\\n").replace("$c1", "$c5").replace("$c2", "$c6")
        self.assertEqual(order(one + two), ["$c2", "$c1", "$c6", "$c5"])

    def test_repeated_cell_names_get_unique_names(self) -> None:
        # Yosys interns names design-wide: module \\n reusing $c1 and $c2 must not take the order
        # of their first use in module \\m, so the repeats are renamed and the order is its own
        one = module(
            cell("$add", "$c1", A="$n1", B="\\x", Y="$n3"), cell("$not", "$c2", A="\\x", Y="$n1")
        )
        two = one.replace("\\m", "\\n")
        out = il_order.reorder_text(one + two)
        self.assertEqual(names(out), ["$c2", "$c1", "$c2$m2", "$c1$m2"])
        self.assertEqual(out.count("module"), 2)

    def test_user_instance_names_are_kept(self) -> None:
        sub = "\n".join(["module \\sub", "  wire input 1 \\i", "  wire output 2 \\o", "end"])
        top = module(cell("\\sub", "\\u1", i="$n1", o="$n2"))
        out = il_order.reorder_text(sub + "\n" + top + top.replace("\\m", "\\n"))
        self.assertEqual(names(out), ["\\u1", "\\u1"])

    def test_slices_count_storage_bits_whatever_offset_or_upto(self) -> None:
        # RTLIL slices index the stored bits from 0; `offset` and `upto` only name them
        for flags in ("offset 4", "upto", "offset 4 upto"):
            text = (
                "\n".join(
                    [
                        "module \\m",
                        f"  wire width 2 {flags} $bus",
                        "  wire \\x",
                        "  wire $n5",
                        cell("$not", "$c1", A="$bus [1]", Y="$n5"),
                        cell("$not", "$c2", A="\\x", Y="$bus [0]"),
                        cell("$not", "$c3", A="\\x", Y="$bus [1]"),
                        "end",
                    ]
                )
                + "\n"
            )
            self.assertEqual(order(text), ["$c3", "$c1", "$c2"], flags)

    def test_slice_outside_the_wire_fails(self) -> None:
        text = (
            "\n".join(
                [
                    "module \\m",
                    "  wire width 2 offset 1 $bus",
                    "  wire $n5",
                    cell("$not", "$c1", A="$bus [2]", Y="$n5"),
                    cell("$not", "$c2", A="$n5", Y="$bus [0]"),
                    "end",
                ]
            )
            + "\n"
        )
        with self.assertRaises(il_order.OrderError):
            order(text)

    def test_main_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / "in.il", Path(tmp) / "out.il"
            src.write_text(
                module(
                    cell("$add", "$c1", A="$n1", B="\\x", Y="$n3"),
                    cell("$not", "$c2", A="\\x", Y="$n1"),
                ),
                encoding="utf-8",
            )
            self.assertEqual(il_order.main(["il_order.py", str(src), str(dst)]), 0)
            self.assertEqual(names(dst.read_text(encoding="utf-8")), ["$c2", "$c1"])
            src.write_text(
                module(
                    cell("$bogus", "$c1", A="\\x", Y="$n1"), cell("$not", "$c2", A="$n1", Y="$n3")
                ),
                encoding="utf-8",
            )
            err = io.StringIO()
            with redirect_stderr(err):
                self.assertEqual(il_order.main(["il_order.py", str(src), str(dst)]), 2)
            self.assertIn("$bogus", err.getvalue())
            done = subprocess.run(
                [sys.executable, str(TOOL), str(src), str(dst)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(done.returncode, 2)

    def test_table_has_the_cells_the_review_names(self) -> None:
        table = il_order.load_table()
        for kind in ("$mem", "$mem_v2", "$dffsr", "$alu", "$dlatchsr"):
            self.assertIn(kind, table)
        self.assertTrue(table["$mem_v2"].seq and table["$dffsr"].seq)
        self.assertEqual(table["$mux"].walk, ("S", "B", "A", "Y"))
        self.assertIn(("X", "o"), table["$alu"].ports)
        self.assertIn(("CO", "o"), table["$alu"].ports)


if __name__ == "__main__":
    unittest.main()
