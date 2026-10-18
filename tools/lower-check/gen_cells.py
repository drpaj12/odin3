#!/usr/bin/env python3
"""gen_cells: writes the per-cell fixtures ``tests/lower/cells/<type>_<variant>.v`` of 3B (plan
Task 2 step 4, spec 14.2): one tiny module per rule and variant, a (type, widths, signs) grid.

Usage::

    gen_cells.py --out DIR [--family F]... [--list]

``--out`` is created if absent and each file is overwritten with identical text on a second run
(the generator is deterministic and removes nothing). ``--list`` prints the file names instead.
The families are those of ``lower-diff``: ``bitwise`` is the ``simplemap`` family (§4).

A fixture is a module ``m`` with inputs ``a`` (``b``, ``s``) and an output ``y``, one assign. The
variant in the file name is ``w<A>`` for equal widths, ``w<A>x<B>`` for unequal operand widths,
``_y<Y>`` for a result width other than the natural one, and ``_s`` / ``_ms`` for signed operands
(both signed) and mixed-sign operands (``a`` signed, ``b`` not: the both-signed rule of §3.3).
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

# (type name, operator spelling, kind) of the bitwise family; kind decides the module's shape.
UNARY_BITWISE = (("not", "~"),)
BINARY_BITWISE = (("and", "&"), ("or", "|"), ("xor", "^"), ("xnor", "~^"))
REDUCE = (("reduce_and", "&"), ("reduce_or", "|"), ("reduce_xor", "^"), ("reduce_xnor", "~^"))
LOGIC_BIN = (("logic_and", "&&"), ("logic_or", "||"))
COMPARE = (("eq", "=="), ("ne", "!="))
WIDTHS = (1, 2, 3, 5, 8, 17)
PAIRS = ((3, 5), (5, 3), (1, 4), (8, 17))
SIGN_VARIANTS = ("", "s", "ms")


@dataclass(frozen=True)
class Cell:
    """One fixture: its file stem, the module text and its family."""

    stem: str
    text: str
    family: str


def _decl(direction: str, name: str, width: int, signed: bool) -> str:
    sign = "signed " if signed else ""
    rng = f"[{width - 1}:0] " if width > 1 else ""
    return f"{direction} {sign}{rng}{name}"


def _module(ports: Sequence[str], body: str) -> str:
    return f"module m({', '.join(ports)});\n  {body}\nendmodule\n"


def _suffix(sign: str, y_width: int | None, natural: int) -> str:
    out = f"_{sign}" if sign else ""
    return out + (f"_y{y_width}" if y_width is not None and y_width != natural else "")


def _signs(sign: str) -> tuple[bool, bool]:
    return sign in ("s", "ms"), sign == "s"  # (a signed, b signed)


def unary(name: str, op: str, width: int, sign: str, y_width: int) -> Cell:
    a_signed, _ = _signs(sign)
    ports = [_decl("input", "a", width, a_signed), _decl("output", "y", y_width, False)]
    stem = f"{name}_w{width}{_suffix(sign, y_width, width)}"
    return Cell(stem, _module(ports, f"assign y = {op}a;"), "bitwise")


def binary(name: str, op: str, widths: tuple[int, int], sign: str, y_width: int,
           natural: int | None = None) -> Cell:
    a_signed, b_signed = _signs(sign)
    wa, wb = widths
    ports = [_decl("input", "a", wa, a_signed), _decl("input", "b", wb, b_signed),
             _decl("output", "y", y_width, False)]
    shape = f"w{wa}" if wa == wb else f"w{wa}x{wb}"
    natural = max(wa, wb) if natural is None else natural
    stem = f"{name}_{shape}{_suffix(sign, y_width, natural)}"
    return Cell(stem, _module(ports, f"assign y = a {op} b;"), "bitwise")


def reduction(name: str, op: str, width: int) -> Cell:
    ports = [_decl("input", "a", width, False), _decl("output", "y", 1, False)]
    return Cell(f"{name}_w{width}", _module(ports, f"assign y = {op}a;"), "bitwise")


def logic_not(width: int) -> Cell:
    ports = [_decl("input", "a", width, False), _decl("output", "y", 1, False)]
    return Cell(f"logic_not_w{width}", _module(ports, "assign y = !a;"), "bitwise")


def mux(width: int) -> Cell:
    ports = [_decl("input", "a", width, False), _decl("input", "b", width, False),
             "input s", _decl("output", "y", width, False)]
    return Cell(f"mux_w{width}", _module(ports, "assign y = s ? b : a;"), "bitwise")


def bitwise_cells() -> list[Cell]:
    cells: list[Cell] = []
    for name, op in UNARY_BITWISE:
        cells += [unary(name, op, w, sign, y)
                  for w in WIDTHS for sign in ("", "s") for y in (w, w + 3)]
    for name, op in BINARY_BITWISE:
        cells += [binary(name, op, (w, w), "", w) for w in WIDTHS]
        cells += [binary(name, op, p, sign, max(p)) for p in PAIRS for sign in SIGN_VARIANTS]
        cells.append(binary(name, op, (4, 4), "s", 7))
    for name, op in REDUCE:
        cells += [reduction(name, op, w) for w in WIDTHS if w > 1]
    cells += [logic_not(w) for w in WIDTHS]
    for name, op in LOGIC_BIN:
        cells += [binary(name, op, p, "", 1, 1) for p in ((1, 1), (3, 5), (8, 2))]
    for name, op in COMPARE:
        cells += [binary(name, op, (w, w), "", 1, 1) for w in WIDTHS]
        cells += [binary(name, op, p, sign, 1, 1) for p in PAIRS for sign in SIGN_VARIANTS]
    cells += [mux(w) for w in WIDTHS]
    return cells


PMUX_WIDTHS = (1, 2, 5, 8)
PMUX_SELECTS = (2, 3, 4)


def pmux_cell(width: int, selects: int) -> Cell:
    """A one-hot priority-free case (``parallel_case``), which ``proc`` turns into one ``$pmux``."""
    ports = [_decl("input", "a", width, False)]
    ports += [_decl("input", f"b{i}", width, False) for i in range(selects)]
    ports += [_decl("input", "s", selects, False), _decl("output reg", "y", width, False)]
    items = "\n".join(f"      s[{i}]: y = b{i};" for i in range(selects))
    body = ("always @* begin\n    y = a;\n    (* parallel_case *) case (1'b1)\n"
            f"{items}\n    endcase\n  end")
    return Cell(f"pmux_w{width}s{selects}", _module(ports, body), "pmux")


def pmux_cells() -> list[Cell]:
    return [pmux_cell(w, n) for w in PMUX_WIDTHS for n in PMUX_SELECTS]


SHIFT_OPS = (("shl", "<<"), ("shr", ">>"), ("sshr", ">>>"))
SHIFT_A = (3, 8)
SHIFT_B = (2, 3, 5)
SHIFT_WIDE_Y = ((3, 2, 6), (8, 3, 5))  # (A width, B width, Y width) with a Y other than A's
SHIFTX_A = (4, 8)
SHIFTX_B = (2, 3, 4)


def shift_cell(name: str, op: str, widths: tuple[int, int], signed: bool, y_width: int) -> Cell:
    """``y = a <op> b``; ``a`` is signed for the ``_s`` variant (``$sshr`` needs it to sign-fill);
    ``b`` is the shift amount, always unsigned."""
    wa, wb = widths
    ports = [_decl("input", "a", wa, signed), _decl("input", "b", wb, False),
             _decl("output", "y", y_width, False)]
    stem = f"{name}_w{wa}b{wb}{'_s' if signed else ''}{_suffix('', y_width, wa)}"
    return Cell(stem, _module(ports, f"assign y = a {op} b;"), "shift")


def shiftx_cell(wa: int, wb: int, signed: bool, y_width: int) -> Cell:
    """``y = a[b +: Y]``: a variable part select, which elaboration makes a ``$shiftx``."""
    ports = [_decl("input", "a", wa, False), _decl("input", "b", wb, signed),
             _decl("output", "y", y_width, False)]
    stem = f"shiftx_w{wa}b{wb}{'_s' if signed else ''}_y{y_width}"
    return Cell(stem, _module(ports, f"assign y = a[b +: {y_width}];"), "shift")


def shift_cells() -> list[Cell]:
    cells: list[Cell] = []
    for name, op in SHIFT_OPS:
        for signed in (False, True):
            cells += [shift_cell(name, op, (wa, wb), signed, wa)
                      for wa in SHIFT_A for wb in SHIFT_B]
            cells += [shift_cell(name, op, (wa, wb), signed, wy) for wa, wb, wy in SHIFT_WIDE_Y]
    for signed in (False, True):
        cells += [shiftx_cell(wa, wb, signed, 2) for wa in SHIFTX_A for wb in SHIFTX_B]
    return cells


COMPARE_OPS = (("lt", "<"), ("le", "<="), ("gt", ">"), ("ge", ">="))
COMPARE_WIDTHS = (1, 2, 5, 8)
COMPARE_SIGNED_WIDTHS = (1, 3, 8)
ARITH_WIDTHS = (1, 3, 8, 17)
ARITH_PAIRS = ((3, 5), (5, 3))


def arith_cells() -> list[Cell]:
    """``$add``, ``$sub``, ``$neg`` and the comparisons (the ``$alu`` structures)."""
    cells: list[Cell] = []
    for name, op in (("add", "+"), ("sub", "-")):
        cells += [binary(name, op, (w, w), "", w) for w in ARITH_WIDTHS]
        cells += [binary(name, op, p, sign, max(p)) for p in ARITH_PAIRS for sign in SIGN_VARIANTS]
        cells += [binary(name, op, (4, 4), "s", 7), binary(name, op, (8, 8), "", 5)]
    cells += [unary("neg", "-", w, sign, y)
              for w in ARITH_WIDTHS for sign in ("", "s") for y in (w, w + 3)]
    for name, op in COMPARE_OPS:
        cells += [binary(name, op, (w, w), "", 1, 1) for w in COMPARE_WIDTHS]
        cells += [binary(name, op, (w, w), "s", 1, 1) for w in COMPARE_SIGNED_WIDTHS]
        cells += [binary(name, op, p, sign, 1, 1) for p in ARITH_PAIRS for sign in SIGN_VARIANTS]
    return cells


MUL_WIDTHS = (1, 2, 3, 4, 5, 8)
MUL_WIDE = (12, 16)
MUL_PAIRS = ((3, 5), (5, 3), (2, 7), (1, 4))
DIV_WIDTHS = (1, 2, 3, 4, 5, 8)
DIV_WIDE = (12,)
DIV_PAIRS = ((3, 5), (5, 3), (2, 6), (6, 2))


def mul_cells() -> list[Cell]:
    """``$mul`` (maccmap): equal and unequal operands, signs, ``Y`` narrower, equal, wider."""
    cells: list[Cell] = []
    for w in MUL_WIDTHS + MUL_WIDE:
        cells += [binary("mul", "*", (w, w), sign, w) for sign in ("", "s")]
    for w in MUL_WIDTHS[1:]:
        cells += [binary("mul", "*", (w, w), sign, 2 * w) for sign in ("", "s")]
    cells += [binary("mul", "*", (4, 4), sign, 3) for sign in ("", "s")]
    for p in MUL_PAIRS:
        cells += [binary("mul", "*", p, sign, max(p)) for sign in SIGN_VARIANTS]
        cells.append(binary("mul", "*", p, "", sum(p)))
    return cells


def div_cells() -> list[Cell]:
    """``$div`` and ``$mod`` (the restoring divider of ``$__div_mod_u``), the same grid."""
    cells: list[Cell] = []
    for name, op in (("div", "/"), ("mod", "%")):
        for w in DIV_WIDTHS + DIV_WIDE:
            cells += [binary(name, op, (w, w), sign, w) for sign in ("", "s")]
        for w in (3, 4):
            cells += [binary(name, op, (w, w), sign, w + 3) for sign in ("", "s")]
        cells += [binary(name, op, (4, 4), sign, 2) for sign in ("", "s")]
        for p in DIV_PAIRS:
            cells += [binary(name, op, p, sign, max(p)) for sign in SIGN_VARIANTS]
    return cells


FF_KINDS = ("dff", "dffe", "sdff", "sdffe", "sdffce", "adff", "adffe", "aldff", "aldffe", "dffsr",
            "dffsre")
FF_ENABLE_KINDS = ("dffe", "sdffe", "sdffce", "adffe", "aldffe", "dffsre")
FF_WIDTHS = (1, 4, 8)
FF_VALUES = {1: "1'b1", 4: "4'd5", 8: "8'hA5"}
FF_SET_VALUES = {1: "1'b1", 4: "4'b1010", 8: "8'hA5"}


def _active(name: str, low: bool) -> str:
    return f"!{name}" if low else name


def _event(name: str, low: bool) -> str:
    return f"{'negedge' if low else 'posedge'} {name}"


def ff_cell(kind: str, width: int, neg_clk: bool, low: bool) -> Cell:
    """One flip-flop module: ``kind`` is the word cell ``proc; opt`` infers (``$dff``..``$dffsre``),
    ``neg_clk`` a falling-edge clock, ``low`` every control active low. Reset values are constants
    (``$dffsr`` gets a set value with ones and a clear value of zeros, so ``proc`` keeps both)."""
    vec = f"[{width - 1}:0] " if width > 1 else ""
    value, setv = FF_VALUES[width], FF_SET_VALUES[width]
    ctl = "ld" if kind.startswith("al") else "rst"
    ports = ["input clk"]
    if kind in FF_ENABLE_KINDS:
        ports.append("input en")
    if kind.startswith("al"):
        ports += ["input ld", f"input {vec}ad"]
    elif kind.startswith("sd") or kind.startswith("ad"):
        ports.append("input rst")
    elif kind.startswith("dffsr"):
        ports += ["input set", "input clr"]
    ports += [f"input {vec}d", f"output reg {vec}q"]
    clk_ev = _event("clk", neg_clk)
    en = _active("en", low)
    srst = _active("rst", low)
    if kind == "dff":
        body = f"always @({clk_ev}) q <= d;"
    elif kind == "dffe":
        body = f"always @({clk_ev}) if ({en}) q <= d;"
    elif kind == "sdff":
        body = f"always @({clk_ev}) if ({srst}) q <= {value}; else q <= d;"
    elif kind == "sdffe":
        body = f"always @({clk_ev}) if ({srst}) q <= {value}; else if ({en}) q <= d;"
    elif kind == "sdffce":
        body = f"always @({clk_ev}) if ({en}) begin if ({srst}) q <= {value}; else q <= d; end"
    elif kind in ("adff", "adffe"):
        tail = f" else if ({en}) q <= d;" if kind == "adffe" else " else q <= d;"
        body = (f"always @({clk_ev} or {_event('rst', low)}) "
                f"if ({_active('rst', low)}) q <= {value};{tail}")
    elif kind in ("aldff", "aldffe"):
        tail = f" else if ({en}) q <= d;" if kind == "aldffe" else " else q <= d;"
        body = (f"always @({clk_ev} or {_event(ctl, low)}) "
                f"if ({_active(ctl, low)}) q <= ad;{tail}")
    else:  # dffsr, dffsre
        tail = f" else if ({en}) q <= d;" if kind == "dffsre" else " else q <= d;"
        body = (f"always @({clk_ev} or {_event('set', low)} or {_event('clr', low)}) "
                f"if ({_active('clr', low)}) q <= {width}'d0; "
                f"else if ({_active('set', low)}) q <= {setv};{tail}")
    suffix = f"_w{width}" + ("_neg" if neg_clk else "") + ("_lo" if low else "")
    return Cell(f"{kind}{suffix}", _module(ports, body), "ff")


def ff_cells() -> list[Cell]:
    """The eleven word flip-flops: widths 1, 4, 8 with active-high controls and a rising clock,
    then at width 4 the falling clock, the active-low controls and both."""
    cells: list[Cell] = []
    for kind in FF_KINDS:
        has_ctl = kind != "dff"
        cells += [ff_cell(kind, width, False, False) for width in FF_WIDTHS]
        cells.append(ff_cell(kind, 4, True, False))
        if has_ctl:
            cells += [ff_cell(kind, 4, False, True), ff_cell(kind, 4, True, True)]
    return cells


LATCH_WIDTHS = (1, 4, 8)


def dlatch_cell(width: int, low: bool) -> Cell:
    """One ``$dlatch`` module: a level-sensitive ``always @*`` latch, transparent while ``en`` is
    high (``low``: while it is low, ``EN_POLARITY`` 0). The other latch cells ($adlatch, $dlatchsr,
    $sr) have no Verilog form that ``proc; opt`` turns into them (Yosys has none either), so they
    are covered by ``test_lower_latch`` alone."""
    vec = f"[{width - 1}:0] " if width > 1 else ""
    ports = ["input en", f"input {vec}d", f"output reg {vec}q"]
    body = f"always @* if ({_active('en', low)}) q = d;"
    return Cell(f"dlatch_w{width}{'_lo' if low else ''}", _module(ports, body), "latch")


def latch_cells() -> list[Cell]:
    """The ``$dlatch`` fixtures: widths 1, 4, 8, then the active-low enable at width 4."""
    return [*(dlatch_cell(width, False) for width in LATCH_WIDTHS), dlatch_cell(4, True)]


def _vec(width: int) -> str:
    return f"[{width - 1}:0] " if width > 1 else ""


def _mem_plain(stem: str, width: int, lo: int, hi: int, abits: int) -> Cell:
    """One memory ``mem[lo:hi]`` of ``width`` bits, one write port and one asynchronous read
    (a one-bit port is a scalar: Odin III names a ``[0:0]`` vector's bit ``x[0]``)."""
    text = (f"module m(input clk, input we, input {_vec(abits)}wa, input {_vec(width)}wd, "
            f"input {_vec(abits)}ra, output {_vec(width)}rd);\n  reg {_vec(width)}mem [{lo}:{hi}];"
            "\n  always @(posedge clk) if (we) mem[wa] <= wd;\n  assign rd = mem[ra];\nendmodule\n")
    return Cell(stem, text, "mem")


MEM_TWO_PORTS = ("module m(input clk, input we, input we2, input [1:0] wa, input [1:0] wa2, "
                 "input [3:0] wd, input [3:0] wd2, input [1:0] ra, input [1:0] rb, "
                 "output [3:0] rd, output [3:0] rd2);\n  reg [3:0] mem [0:3];\n")
MEM_BYTE_EN = ("module m(input clk, input [1:0] we, input [1:0] wa, input [3:0] wd, "
               "input [1:0] ra, output [3:0] rd);\n  reg [3:0] mem [0:3];\n"
               "  always @(posedge clk) begin\n    if (we[0]) mem[wa][1:0] <= wd[1:0];\n"
               "    if (we[1]) mem[wa][3:2] <= wd[3:2];\n  end\n"
               "  assign rd = mem[ra];\nendmodule\n")
MEM_SYNC_READ = ("module m(input clk, input we, input [1:0] wa, input [3:0] wd, input [1:0] ra, "
                 "output reg [3:0] rd);\n  reg [3:0] mem [0:3];\n  always @(posedge clk) begin\n"
                 "    if (we) mem[wa] <= wd;\n    rd <= mem[ra];\n  end\nendmodule\n")


def mem_cells() -> list[Cell]:
    """The ``$mem`` fixtures (spec 7): SIZE <= 16 and WIDTH <= 4, so ``equiv-check`` decides them.
    Sizes (powers of two and not), OFFSET 1, a negative OFFSET, SIZE above 2^ABITS, two read
    ports, two write ports (the later wins), per-bit write enables, a registered read, a falling
    clock."""
    plain = _mem_plain("mem_w4_d4", 4, 0, 3, 2)
    cells = [
        _mem_plain("mem_w1_d2", 1, 0, 1, 1),
        plain,
        _mem_plain("mem_w4_d16", 4, 0, 15, 4),
        _mem_plain("mem_w2_d6", 2, 0, 5, 3),
        _mem_plain("mem_w4_d4_off1", 4, 1, 4, 3),
        _mem_plain("mem_w3_d8_offm2", 3, -2, 5, 3),
        _mem_plain("mem_w4_d8_a2_off1", 4, 1, 8, 2),  # SIZE 8 > 2^ABITS 4 (3B-3 L-1)
        Cell("mem_w4_d4_r2", MEM_TWO_PORTS + "  always @(posedge clk) if (we) mem[wa] <= wd;\n"
             "  assign rd = mem[ra];\n  assign rd2 = mem[rb];\nendmodule\n", "mem"),
        Cell("mem_w4_d4_w2", MEM_TWO_PORTS + "  always @(posedge clk) begin\n"
             "    if (we) mem[wa] <= wd;\n    if (we2) mem[wa2] <= wd2;\n  end\n"
             "  assign rd = mem[ra];\n  assign rd2 = mem[rb];\nendmodule\n", "mem"),
        Cell("mem_w4_d4_be", MEM_BYTE_EN, "mem"),
        Cell("mem_w4_d4_sync", MEM_SYNC_READ, "mem"),
        Cell("mem_w4_d4_neg", plain.text.replace("posedge", "negedge"), "mem"),
    ]
    return cells


FAMILY_GENERATORS = {"bitwise": bitwise_cells, "pmux": pmux_cells, "shift": shift_cells,
                     "arith": arith_cells, "mul": mul_cells, "div": div_cells, "ff": ff_cells,
                     "latch": latch_cells, "mem": mem_cells}



def generate(families: Sequence[str]) -> list[Cell]:
    """Every fixture of the families, in a fixed order, without duplicate stems."""
    cells: list[Cell] = []
    seen: set[str] = set()
    for family in families:
        if family not in FAMILY_GENERATORS:
            raise SystemExit(f"gen_cells: unknown family '{family}': one of "
                             f"{sorted(FAMILY_GENERATORS)}")
        for cell in FAMILY_GENERATORS[family]():
            if cell.stem not in seen:
                seen.add(cell.stem)
                cells.append(cell)
    return cells


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="gen_cells", description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--out", type=Path, help="the directory to write the .v files to")
    ap.add_argument("--family", action="append", help="a family (default: all)")
    ap.add_argument("--list", action="store_true", help="print the file names, write nothing")
    args = ap.parse_args(argv)
    cells = generate(args.family or sorted(FAMILY_GENERATORS))
    if args.list:
        for cell in cells:
            print(f"{cell.stem}.v")
        return 0
    if args.out is None:
        ap.error("--out is required unless --list")
    args.out.mkdir(parents=True, exist_ok=True)
    for cell in cells:
        (args.out / f"{cell.stem}.v").write_text(cell.text, encoding="utf-8")
    print(f"gen_cells: wrote {len(cells)} fixtures to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
