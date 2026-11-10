"""Tests for write_verilog --quartus through tools/quartus-check: every flip-flop and latch type of
the IR (the eleven word cells in both polarities and the 68 gate-level cells) is read with
``read_verilog -icells``, written as plain always blocks and simulated against a hand-written
behavioural model with random stimulus.

Needs ``ODIN3_CLI`` (the odin3 binary) and iverilog; skipped otherwise.
"""

from __future__ import annotations

import itertools
import os
import shutil
import tempfile
import unittest
from pathlib import Path

import quartus_check as qc

ODIN3 = os.environ.get("ODIN3_CLI", "")
WIDTH = 3
PORTS = (
    "input clk, input rst, input en, input ald, input [2:0] d, input [2:0] ad, "
    "input [2:0] set, input [2:0] clr, output reg [2:0] q"
)


def edge(positive: bool, sig: str) -> str:
    return f"{'posedge' if positive else 'negedge'} {sig}"


def act(positive: bool, sig: str) -> str:
    return sig if positive else f"!{sig}"


RV = "3'b101"


def cell(kind: str, pins: str, params: str) -> str:
    """An instance of word cell `$kind` (read with -icells)."""
    return f"\\${kind} #(.WIDTH(3), {params}) u ({pins});"


def per_bit(body: str) -> str:
    """The model of a set/clear cell: one block per bit, `#` the bit number."""
    return " ".join(body.replace("#", str(bit)) for bit in range(WIDTH))


def word_cases_for(pol: int) -> list[tuple[str, str, str]]:
    """(label, cell instance, model) of the eleven word cells for one polarity of every control."""
    p = bool(pol)
    clk, en = f".CLK_POLARITY({pol})", f".EN_POLARITY({pol})"
    arst = f".ARST_POLARITY({pol}), .ARST_VALUE({RV})"
    srst = f".SRST_POLARITY({pol}), .SRST_VALUE({RV})"
    aload, setclr = f".ALOAD_POLARITY({pol})", f".SET_POLARITY({pol}), .CLR_POLARITY({pol})"
    ck, rs, al = edge(p, "clk"), edge(p, "rst"), edge(p, "ald")
    on, re_, ae = act(p, "en"), act(p, "rst"), act(p, "ald")
    sr_edges = f"{edge(p, 'set[#]')}, {edge(p, 'clr[#]')}"
    clear, setb = act(p, "clr[#]"), act(p, "set[#]")
    sr_body = f"if ({clear}) q[#] <= 1'b0; else if ({setb}) q[#] <= 1'b1;"
    pins_sr = ".SET(set), .CLR(clr)"
    return [
        (f"adffe{pol}", cell("adffe", ".CLK(clk), .ARST(rst), .EN(en), .D(d), .Q(q)",
                             f"{clk}, {en}, {arst}"),
         f"always @({ck}, {rs}) if ({re_}) q <= {RV}; else if ({on}) q <= d;"),
        (f"sdffe{pol}", cell("sdffe", ".CLK(clk), .SRST(rst), .EN(en), .D(d), .Q(q)",
                             f"{clk}, {en}, {srst}"),
         f"always @({ck}) if ({re_}) q <= {RV}; else if ({on}) q <= d;"),
        (f"sdffce{pol}", cell("sdffce", ".CLK(clk), .SRST(rst), .EN(en), .D(d), .Q(q)",
                              f"{clk}, {en}, {srst}"),
         f"always @({ck}) if ({on}) begin if ({re_}) q <= {RV}; else q <= d; end"),
        (f"aldff{pol}", cell("aldff", ".CLK(clk), .ALOAD(ald), .AD(ad), .D(d), .Q(q)",
                             f"{clk}, {aload}"),
         f"always @({ck}, {al}) if ({ae}) q <= ad; else q <= d;"),
        (f"aldffe{pol}", cell("aldffe", ".CLK(clk), .ALOAD(ald), .AD(ad), .EN(en), .D(d), .Q(q)",
                              f"{clk}, {en}, {aload}"),
         f"always @({ck}, {al}) if ({ae}) q <= ad; else if ({on}) q <= d;"),
        (f"dlatch{pol}", cell("dlatch", ".EN(en), .D(d), .Q(q)", en),
         f"always @* if ({on}) q <= d;"),
        (f"adlatch{pol}", cell("adlatch", ".EN(en), .ARST(rst), .D(d), .Q(q)", f"{en}, {arst}"),
         f"always @* if ({re_}) q <= {RV}; else if ({on}) q <= d;"),
        (f"dffsr{pol}", cell("dffsr", f".CLK(clk), {pins_sr}, .D(d), .Q(q)", f"{clk}, {setclr}"),
         per_bit(f"always @({ck}, {sr_edges}) {sr_body[:-1]} else q[#] <= d[#];")),
        (f"dffsre{pol}", cell("dffsre", f".CLK(clk), {pins_sr}, .EN(en), .D(d), .Q(q)",
                              f"{clk}, {setclr}, {en}"),
         per_bit(f"always @({ck}, {sr_edges}) {sr_body[:-1]} else if ({on}) q[#] <= d[#];")),
        (f"dlatchsr{pol}", cell("dlatchsr", f".EN(en), {pins_sr}, .D(d), .Q(q)", f"{en}, {setclr}"),
         per_bit(f"always @* {sr_body[:-1]} else if ({on}) q[#] <= d[#];")),
        (f"sr{pol}", cell("sr", f"{pins_sr}, .Q(q)", setclr), per_bit(f"always @* {sr_body}")),
    ]


def word_cases() -> list[tuple[str, str, str]]:
    return word_cases_for(1) + word_cases_for(0)


def pol(letter: str) -> bool:
    return letter == "P"


def gate_case(name: str, pins: str, model: str) -> tuple[str, str, str]:
    return (name, f"\\$_{name}_ u ({pins});", model)


def gate_cases() -> list[tuple[str, str, str]]:
    """The 68 gate-level types of ir/cells/dffx.h with a model written from the name letters."""
    cases = []
    pols = "PN"
    for c, e in itertools.product(pols, pols):  # $_DFFE_<c><e>_
        cases.append(gate_case(f"DFFE_{c}{e}", ".C(clk), .E(en), .D(d0), .Q(q0)",
                               f"always @({edge(pol(c), 'clk')}) "
                               f"if ({act(pol(e), 'en')}) q[0] <= d[0];"))
    for c, r, v in itertools.product(pols, pols, "01"):  # async and sync reset, no enable
        reset = f"if ({act(pol(r), 'rst')}) q[0] <= 1'b{v}; else q[0] <= d[0];"
        pins = ".C(clk), .R(rst), .D(d0), .Q(q0)"
        cases.append(gate_case(f"DFF_{c}{r}{v}", pins,
                               f"always @({edge(pol(c), 'clk')}, {edge(pol(r), 'rst')}) {reset}"))
        cases.append(gate_case(f"SDFF_{c}{r}{v}", pins,
                               f"always @({edge(pol(c), 'clk')}) {reset}"))
    for c, r, v, e in itertools.product(pols, pols, "01", pols):
        pins = ".C(clk), .R(rst), .E(en), .D(d0), .Q(q0)"
        rst, en = act(pol(r), "rst"), act(pol(e), "en")
        ck = edge(pol(c), "clk")
        tail = f"if ({rst}) q[0] <= 1'b{v}; else if ({en}) q[0] <= d[0];"
        cases.append(gate_case(f"DFFE_{c}{r}{v}{e}", pins,
                               f"always @({ck}, {edge(pol(r), 'rst')}) {tail}"))
        cases.append(gate_case(f"SDFFE_{c}{r}{v}{e}", pins, f"always @({ck}) {tail}"))
        cases.append(gate_case(f"SDFFCE_{c}{r}{v}{e}", pins,
                               f"always @({ck}) if ({en}) begin "
                               f"if ({rst}) q[0] <= 1'b{v}; else q[0] <= d[0]; end"))
    return cases


def micro(instance: str) -> str:
    body = f"  wire q0;\n  assign q = {{2'b0, q0}};\n  wire d0 = d[0];\n  {instance}\n"
    if "WIDTH" in instance:
        body = f"  {instance}\n"
    return f"module t({PORTS.replace('output reg', 'output')});\n{body}endmodule\n"


def gold(blocks: str, word: bool) -> str:
    high = "" if word else " initial q[2:1] = 2'b00;"  # a gate-level cell drives bit 0 only
    return f"module t({PORTS});\n  {blocks}{high}\nendmodule\n"


@unittest.skipUnless(ODIN3 and shutil.which("iverilog"), "ODIN3_CLI and iverilog are needed")
class EveryStorageCell(unittest.TestCase):
    def run_case(self, label: str, instance: str, blocks: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "t.v").write_text(micro(instance))
            (work / "gold.v").write_text(gold(blocks, "WIDTH" in instance))
            code = qc.main(["--odin3", ODIN3, "--icells", "--gold", str(work / "gold.v"),
                            "--top", "t", "--cycles", "600", str(work / "t.v")])
            self.assertEqual(code, 0, label)

    def test_word_cells(self) -> None:
        for label, instance, blocks in word_cases():
            with self.subTest(label):
                self.run_case(label, instance, blocks)

    def test_gate_cells(self) -> None:
        cases = gate_cases()
        self.assertEqual(len(cases), 68)
        for label, instance, blocks in cases:
            with self.subTest(label):
                self.run_case(label, instance, blocks)


@unittest.skipUnless(ODIN3, "ODIN3_CLI is needed")
class BlackBox(unittest.TestCase):
    def test_megafunction_passes_through_with_parameters(self) -> None:
        micro_path = Path(__file__).resolve().parents[1] / "quartus" / "blackbox_scfifo.v"
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "net.v"
            script = f"read_verilog --auto-blackbox {micro_path}; write_verilog {out} --quartus"
            qc.odin3_run(ODIN3, script)
            net = out.read_text()
            sim = (Path(tmp) / "net_lpm_sim.v").read_text()
        self.assertIn('scfifo #(.add_ram_output_register("ON")', net)
        self.assertIn(".lpm_widthu(4)", net)
        self.assertIn(".aclr()", net)
        self.assertIn(".usedw()", net)
        self.assertNotIn("1'bx", net)
        self.assertNotIn("module scfifo", net)
        self.assertIn("module scfifo", sim)
        qc.check_pure(net, sim)


class Purity(unittest.TestCase):
    def test_internal_cell_instance_is_found(self) -> None:
        with self.assertRaises(qc.CheckError):
            qc.check_pure("module m();\n  \\$sdffe #(.WIDTH(2)) \\$c1 (\n  );\nendmodule\n", "")

    def test_reg_name_is_not_an_instance(self) -> None:
        qc.check_pure("module m();\n  \\$c1 <= 1;\n  \\$c2 [3] <= 1;\nendmodule\n", "")

    def test_library_module_is_found(self) -> None:
        with self.assertRaises(qc.CheckError):
            qc.check_pure("module lpm_add_sub #(parameter w = 1) ();\nendmodule\n", "")

    def test_blackbox_attribute_is_found(self) -> None:
        with self.assertRaises(qc.CheckError):
            qc.check_pure("(* blackbox *)\nmodule m();\nendmodule\n", "")


if __name__ == "__main__":
    unittest.main()
