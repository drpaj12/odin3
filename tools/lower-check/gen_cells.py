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


ARITH_WIDTHS = (1, 3, 8, 17)
ARITH_PAIRS = ((3, 5), (5, 3))


def arith_cells() -> list[Cell]:
    """``$add``, ``$sub`` and ``$neg`` (the ``$alu`` structures)."""
    cells: list[Cell] = []
    for name, op in (("add", "+"), ("sub", "-")):
        cells += [binary(name, op, (w, w), "", w) for w in ARITH_WIDTHS]
        cells += [binary(name, op, p, sign, max(p)) for p in ARITH_PAIRS for sign in SIGN_VARIANTS]
        cells += [binary(name, op, (4, 4), "s", 7), binary(name, op, (8, 8), "", 5)]
    cells += [unary("neg", "-", w, sign, y)
              for w in ARITH_WIDTHS for sign in ("", "s") for y in (w, w + 3)]
    return cells


FAMILY_GENERATORS = {"bitwise": bitwise_cells, "pmux": pmux_cells, "shift": shift_cells,
                     "arith": arith_cells}


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
