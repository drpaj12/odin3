#!/usr/bin/env python3
"""verilog-gen: random valid Verilog-2005 modules with their own expected parse tree (2C §7.5).

Usage::

    verilog_gen.py --seed N --out F.v [--mutate] [--no-ast]

writes ``F.v``, one compilation unit of one to four modules generated from seed N over the MUST
and SHOULD coverage rows, weighted toward the common forms: ANSI and non-ANSI headers, parameter
ports, nets, variables, memories and integers with consistent ranges (every name declared
before use), continuous assignments, gates, ``always``/``initial`` processes with ``if``,
``case``/``casez``/``casex``, loops, named blocks with locals, tasks, functions (ANSI and not,
constant ones included), loop/if/case generate, instances with parameter overrides, attributes,
``full_case``/``parallel_case`` metacomments, comments between tokens, escaped identifiers and
every number form.

Beside it, ``F.ast`` holds the generator's **own** tree as the 2A s-expression dump
(``odin3_ast_dump`` of each UNIT item, one per line group): the tree is built first, then
printed with the *minimal* parentheses the 1364-2005 Table 5-4 copy below allows, and every
node's ``@file:line:col`` is where this printer put its first token.  Nothing here reads the
parser's code; the tree shapes are the 2A slot table (``docs/specs/2026-10-09-2A-ast-design.md``
§4.3) and the 2C construct map (``docs/specs/2026-10-10-2C-verilog-parser-design.md`` §3.6).
``compare.py`` requires ``odin3-parse-check --dump F.v`` to reproduce it.

``--mutate`` drops, duplicates or swaps one token of the generated text instead (no ``F.ast``);
``--no-ast`` skips ``F.ast``.  The output is a function of N alone.

Exit 0 written, 2 usage or I/O error.
"""

from __future__ import annotations

import argparse
import dataclasses
import random
import re
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

# --- the tree -------------------------------------------------------------------------------

# Flags of each kind in bit order (2A §4.4); fields (wider than a bit) print as NAME=v.
FLAG_ORDER: dict[str, tuple[str, ...]] = {
    "MODULE": ("MACROMODULE",),
    "PORT_REF": ("DOTTED",),
    "PORT_DECL": ("SIGNED", "IN_HEADER", "DT", "NET"),
    "NET_DECL": ("SIGNED", "VECTORED", "SCALARED"),
    "VAR_DECL": ("SIGNED",),
    "PARAM_DECL": ("SIGNED", "IN_HEADER", "DT"),
    "STRENGTH": ("S0", "S1"),
    "FUNCTION_DECL": ("AUTOMATIC", "SIGNED", "DT"),
    "TASK_DECL": ("AUTOMATIC",),
    "ATTR": ("METACOMMENT",),
    "TASK_CALL": ("SYSTEM", "HIER"),
    "CASE_ITEM": ("DEFAULT",),
    "EVENT_CONTROL": ("STAR", "NO_PARENS"),
    "EVENT_EXPR": ("AFTER_OR",),
    "NUMBER": ("SIGNED",),
    "STRING": ("EMPTY",),
    "CALL": ("SYSTEM", "HIER"),
}
FIELDS = frozenset({"DT", "NET", "S0", "S1"})

# The DT field (none reg integer real realtime time) and the NET field (1 + the net kind).
DT_VALUE = {"reg": 1, "integer": 2, "real": 3, "realtime": 4, "time": 5}
NET_KINDS = ("wire", "tri", "wand", "wor", "triand", "trior", "tri0", "tri1", "trireg", "uwire",
             "supply0", "supply1")

MAX_DUMP_COL = 256  # odin3_ast_dump clamps the indentation at 2 * ODIN3_PR_MAX_INDENT
MUTATE_TRIES = 8  # draws of a mutation before a no-op swap is given up on


class Node:
    """One node: kind, sub-kind spelling, name (raw bytes), flags, children (None = an absent
    slot), attributes (ATTR nodes), payload text (NUMBER, REAL), and where it was printed."""

    def __init__(self, kind: str, *kids: Node | None, sub: str | None = None,
                 name: str | None = None, payload: str | None = None) -> None:
        self.kind = kind
        self.sub = sub
        self.name = name
        self.flags: dict[str, int] = {}
        self.kids: list[Node | None] = list(kids)
        self.attrs: list[Node] = []
        self.payload = payload
        self.loc: tuple[int, int] = (0, 0)
        self.spell: dict[str, object] = {}  # how the printer writes it (not part of the tree)

    def flag(self, name: str, value: int = 1) -> Node:
        if value:
            self.flags[name] = value
        return self

    def kid(self, idx: int) -> Node:
        node = self.kids[idx]
        assert node is not None
        return node


def cstring(text: str) -> str:
    """A string as odin3_pr_append_cstring writes it."""
    out = ['"']
    for ch in text:
        code = ord(ch)
        if ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch in '\\"':
            out.append("\\" + ch)
        elif 0x20 <= code <= 0x7E:
            out.append(ch)
        else:
            out.append(f"\\{code:03o}")
    out.append('"')
    return "".join(out)


def _flag_text(node: Node) -> str:
    words = []
    for name in FLAG_ORDER.get(node.kind, ()):
        value = node.flags.get(name, 0)
        if value:
            words.append(f"{name}={value}" if name in FIELDS else name)
    return " ".join(words)


def _head(node: Node, fname: str) -> str:
    text = "(" + node.kind
    if node.sub is not None:
        text += "." + node.sub
    if node.kind == "STRING":
        text += ":" + cstring(node.name or "")
    elif node.name is not None:
        text += ":" + node.name
    flags = _flag_text(node)
    if flags:
        text += " [" + flags + "]"
    text += f" @{fname}:{node.loc[0]}:{node.loc[1]}"
    if node.payload is not None:
        text += " " + node.payload
    return text


def dump(node: Node, fname: str, out: list[str], col: int = 0) -> None:
    """odin3_ast_dump's s-expression of node (2A §6): one node per line."""
    out.append(_head(node, fname))
    for kid in node.kids:
        out.append("\n" + " " * min(col + 2, MAX_DUMP_COL))
        if kid is None:
            out.append("_")
        else:
            dump(kid, fname, out, col + 2)
    for idx, attr in enumerate(node.attrs):
        out.append("\n" + " " * min(col + 2, MAX_DUMP_COL) + ("{" if idx == 0 else " "))
        dump(attr, fname, out, col + 3)
    out.append("})" if node.attrs else ")")


def dump_unit(items: Sequence[Node], fname: str) -> str:
    """What odin3-parse-check --dump prints for a UNIT: each item's dump and a newline."""
    out: list[str] = []
    for item in items:
        dump(item, fname, out)
        out.append("\n")
    return "".join(out)


# --- literals (2A §4.5, 2C §2.3): the payload as the 2A printer writes it -------------------

NUMBER_RE = re.compile(r"(?:([0-9][0-9_]*)\s*)?'([sS]?)([bBoOdDhH])\s*([0-9a-fA-FxXzZ?_]+)")
BASE_SUB = {"b": "bin", "o": "oct", "d": "dec", "h": "hex"}


def number_node(spelling: str) -> Node:
    """A NUMBER from its spelling: digits stored as written, printed in the base written, lower
    case, without '_', z as '?' when a '?' was written, decimals by value; an unsized signed
    decimal (a plain decimal or 'sd) is the plain digits."""
    match = NUMBER_RE.fullmatch(spelling)
    if match is None:
        node = Node("NUMBER", sub="dec", payload=str(int(spelling.replace("_", ""))))
        node.flag("SIGNED")
        node.spell["text"] = spelling
        return node
    size = int(match.group(1).replace("_", "")) if match.group(1) else 0
    signed = match.group(2) != ""
    base = match.group(3).lower()
    digits = match.group(4).replace("_", "").lower()
    question = "?" in digits
    xz = any(ch in "xz?" for ch in digits)
    if base == "d" and not xz:
        body = str(int(digits))
    else:
        body = digits.replace("z", "?") if question else digits
    if base == "d" and size == 0 and signed and not xz:
        payload = body
    else:
        payload = (f"{size}'" if size else "'") + ("s" if signed else "") + base + body
    node = Node("NUMBER", sub=BASE_SUB[base], payload=payload)
    node.flag("SIGNED", int(signed))
    node.spell["text"] = spelling
    return node


def real_node(spelling: str) -> Node:
    """A REAL: strtod of the text without '_', printed %.17g (".0" when it reads as integral)."""
    text = format(float(spelling.replace("_", "")), ".17g")
    if not any(ch in text for ch in ".eni"):
        text += ".0"
    node = Node("REAL", payload=text)
    node.spell["text"] = spelling
    return node


ESCAPES = {"n": "\n", "t": "\t", "\\": "\\", '"': '"'}


def string_node(spelling: str) -> Node:
    """A STRING from its quoted spelling (escapes \\n \\t \\\\ \\" only)."""
    body = spelling[1:-1]
    value = re.sub(r"\\(.)", lambda m: ESCAPES[m.group(1)], body)
    node = Node("STRING", name=value)
    node.flag("EMPTY", int(value == ""))
    node.spell["text"] = spelling
    return node


# --- Table 5-4 (1364-2005 §5.1.2), the generator's own copy ----------------------------------

BINARY_PREC: dict[str, int] = {
    "**": 12,
    "*": 11, "/": 11, "%": 11,
    "+": 10, "-": 10,
    "<<": 9, ">>": 9, "<<<": 9, ">>>": 9,
    "<": 8, "<=": 8, ">": 8, ">=": 8,
    "==": 7, "!=": 7, "===": 7, "!==": 7,
    "&": 6,
    "^": 5, "~^": 5,
    "|": 4,
    "&&": 3,
    "||": 2,
}
TERNARY_PREC = 1
PRIMARY_PREC = 100
UNARY_OPS = ("+", "-", "!", "~", "&", "~&", "|", "~|", "^", "~^")


def prec(node: Node) -> int:
    """How tightly the node's own text binds: a binary operator's level, 1 for ?:, and the
    primary level for everything else (unary operators bind to a primary, 2C §3.3)."""
    if node.kind == "BINARY":
        return BINARY_PREC[node.sub or ""]
    if node.kind == "TERNARY":
        return TERNARY_PREC
    if node.kind == "UNARY":
        return PRIMARY_PREC - 1
    return PRIMARY_PREC


def needs_parens(parent: Node, idx: int) -> bool:
    """Whether child idx of an operator node needs parentheses to keep the tree (every binary
    operator associates left, ?: right, a unary operand is a primary)."""
    child = parent.kid(idx)
    mine = prec(child)
    if parent.kind == "UNARY":
        return mine < PRIMARY_PREC
    if parent.kind == "BINARY":
        level = BINARY_PREC[parent.sub or ""]
        return mine < level or (idx == 1 and mine == level)
    if parent.kind == "TERNARY":
        return idx == 0 and mine == TERNARY_PREC
    return False


# --- identifiers ----------------------------------------------------------------------------

PLAIN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*")
KEYWORD_TEXT = """always and assign automatic begin buf bufif0 bufif1 case casex casez cell
cmos config deassign default defparam design disable edge else end endcase endconfig
endfunction endgenerate endmodule endprimitive endspecify endtable endtask event for force
forever fork function generate genvar highz0 highz1 if ifnone incdir include initial inout
input instance integer join large liblist library localparam macromodule medium module nand
negedge nmos nor noshowcancelled not notif0 notif1 or output parameter pmos posedge primitive
pull0 pull1 pulldown pullup pulsestyle_onevent pulsestyle_ondetect rcmos real realtime reg
release repeat rnmos rpmos rtran rtranif0 rtranif1 scalared showcancelled signed small specify
specparam strong0 strong1 supply0 supply1 table task time tran tranif0 tranif1 tri tri0 tri1
triand trior trireg unsigned use uwire vectored wait wand weak0 weak1 while wire wor xnor
xor"""
KEYWORDS = frozenset(KEYWORD_TEXT.split())


def is_plain(name: str) -> bool:
    return PLAIN_RE.fullmatch(name) is not None and name not in KEYWORDS


# --- the printer: text and positions --------------------------------------------------------


class Out:
    """The text being printed, with the line and column of the next byte and the byte span of
    every token (for --mutate)."""

    def __init__(self) -> None:
        self.parts: list[str] = []
        self.line = 1
        self.col = 1
        self.off = 0
        self.tokens: list[tuple[int, int]] = []

    def raw(self, text: str) -> None:
        self.parts.append(text)
        self.off += len(text)
        newline = text.rfind("\n")
        if newline < 0:
            self.col += len(text)
        else:
            self.line += text.count("\n")
            self.col = len(text) - newline

    def tok(self, text: str) -> None:
        self.tokens.append((self.off, self.off + len(text)))
        self.raw(text)

    def here(self) -> tuple[int, int]:
        return (self.line, self.col)

    def text(self) -> str:
        return "".join(self.parts)


COMMENT_WORDS = ("note", "check this", "x <= y", "a*b", "TODO", "sum", "(* not an attr *)",
                 "begin end", "/ slash", "`not a macro")


RANGE_FIRST = ("PORT_DECL", "VAR_DECL", "PARAM_DECL", "FUNCTION_DECL")


class Printer:
    """Prints a tree as Verilog with minimal parentheses and records each node's loc."""

    def __init__(self, rng: random.Random) -> None:
        self.rng = rng
        self.out = Out()
        self.line_comment = False  # a // comment ends the current line

    # -- spacing and comments --

    def gap(self) -> None:
        """A blank between two tokens, now and then with a block comment in it."""
        if self.rng.random() < 0.04:
            self.out.raw(f" /* {self.rng.choice(COMMENT_WORDS).replace('`', '')} */ ")
        else:
            self.out.raw(" ")

    def newline(self, indent: int) -> None:
        if self.rng.random() < 0.05 and not self.line_comment:
            self.out.raw(f" // {self.rng.choice(COMMENT_WORDS)}")
        self.line_comment = False
        self.out.raw("\n" + "  " * indent)

    def comment_line(self, indent: int) -> None:
        if self.rng.random() < 0.08:
            self.out.raw(f"// {self.rng.choice(COMMENT_WORDS)}\n" + "  " * indent)

    def ident(self, name: str) -> None:
        if is_plain(name) and self.rng.random() >= 0.03:
            self.out.tok(name)
        else:
            self.out.tok("\\" + name)
            self.out.raw(" ")

    def name_node(self, node: Node) -> None:
        node.loc = self.out.here()
        self.ident(node.name or "")

    # -- attributes --

    def attrs(self, node: Node) -> None:
        """(* … *) before node: its non-metacomment attributes, in one or two instances."""
        plain = [attr for attr in node.attrs if "METACOMMENT" not in attr.flags]
        if not plain:
            return
        split = node.spell.get("attr_split")
        split = split if isinstance(split, int) else len(plain)
        groups = [plain[:split], plain[split:]] if 0 < split < len(plain) else [plain]
        for group in groups:
            self.out.tok("(*")
            self.out.raw(" ")
            for idx, attr in enumerate(group):
                if idx:
                    self.out.tok(",")
                    self.out.raw(" ")
                attr.loc = self.out.here()
                self.ident(attr.name or "")
                if attr.kids[0] is not None:
                    self.out.raw(" ")
                    self.out.tok("=")
                    self.out.raw(" ")
                    self.expr(attr.kid(0))
            self.out.raw(" ")
            self.out.tok("*)")
            self.out.raw(" ")

    def metacomment(self, node: Node) -> None:
        """The case's METACOMMENT attributes as one comment after its header."""
        keys = [attr for attr in node.attrs if "METACOMMENT" in attr.flags]
        if not keys:
            return
        words = [attr.name or "" for attr in keys]
        if node.spell.get("meta_reversed"):
            words.reverse()
        prefix = str(node.spell.get("meta_prefix", "synopsys"))
        self.out.raw(" ")
        here = self.out.here()
        for attr in keys:
            attr.loc = here
        if node.spell.get("meta_block"):
            self.out.raw(f"/* {prefix} {' '.join(words)} */")
        else:
            self.out.raw(f"// {prefix} {' '.join(words)}")
            self.line_comment = True

    # -- expressions --

    def expr(self, node: Node, paren: bool = False) -> None:
        if paren:
            self.out.tok("(")
        node.loc = self.out.here()
        EXPR_PRINTERS[node.kind](self, node)
        if paren:
            self.out.tok(")")

    def operand(self, parent: Node, idx: int) -> None:
        self.expr(parent.kid(idx), needs_parens(parent, idx))

    def p_leaf(self, node: Node) -> None:
        self.out.tok(str(node.spell["text"]))

    def p_ident(self, node: Node) -> None:
        self.ident(node.name or "")

    def p_unary(self, node: Node) -> None:
        self.out.tok(str(node.spell.get("op", node.sub)))
        if node.attrs:
            self.out.raw(" ")
            self.attrs(node)
        self.operand(node, 0)

    def p_binary(self, node: Node) -> None:
        self.operand(node, 0)
        self.gap()
        self.out.tok(str(node.spell.get("op", node.sub)))
        self.gap()
        self.attrs(node)
        self.operand(node, 1)

    def p_ternary(self, node: Node) -> None:
        self.operand(node, 0)
        self.gap()
        self.out.tok("?")
        self.gap()
        self.attrs(node)
        self.operand(node, 1)
        self.gap()
        self.out.tok(":")
        self.gap()
        self.operand(node, 2)

    def p_select(self, node: Node) -> None:
        self.expr(node.kid(0))
        self.out.tok("[")
        self.expr(node.kid(1))
        if node.sub == "part":
            self.out.tok(":")
            self.expr(node.kid(2))
        elif node.sub != "bit":
            self.out.raw(" ")
            self.out.tok("+:" if node.sub == "part_plus" else "-:")
            self.out.raw(" ")
            self.expr(node.kid(2))
        self.out.tok("]")

    def list_of(self, items: Sequence[Node | None], each: Callable[[Node], None]) -> None:
        for idx, item in enumerate(items):
            if idx:
                self.out.tok(",")
                self.out.raw(" ")
            if item is not None:
                each(item)

    def p_concat(self, node: Node) -> None:
        self.out.tok("{")
        self.list_of(node.kids, self.expr)
        self.out.tok("}")

    def p_replicate(self, node: Node) -> None:
        self.out.tok("{")
        self.expr(node.kid(0))
        self.expr(node.kid(1))
        self.out.tok("}")

    def p_call(self, node: Node) -> None:
        if "SYSTEM" in node.flags:
            self.out.tok(node.name or "")
        else:
            self.ident(node.name or "")
        self.out.tok("(")
        self.list_of(node.kids, self.expr)
        self.out.tok(")")

    # -- declarations --

    def p_range(self, node: Node) -> None:
        node.loc = self.out.here()
        self.out.tok("[")
        self.expr(node.kid(0))
        self.out.tok(":")
        self.expr(node.kid(1))
        self.out.tok("]")

    def p_declarator(self, node: Node) -> None:
        self.name_node(node)
        for dim in node.kids[1:]:
            assert dim is not None
            self.out.raw(" ")
            self.p_range(dim)
        if node.kids[0] is not None:
            self.out.raw(" ")
            self.out.tok("=")
            self.gap()
            self.expr(node.kid(0))

    def type_words(self, node: Node, keyword: str | None) -> None:
        """keyword [signed] [range | type] before the declarators."""
        if keyword is not None:
            self.out.tok(keyword)
            self.out.raw(" ")
        dt = node.spell.get("dt")
        if dt is not None:
            self.out.tok(str(dt))
            self.out.raw(" ")
        if "SIGNED" in node.flags:
            self.out.tok("signed")
            self.out.raw(" ")
        rng = node.kids[0] if node.kind in RANGE_FIRST else None
        if node.kind == "NET_DECL":
            rng = node.kids[1]
        if rng is not None:
            self.p_range(rng)
            self.out.raw(" ")

    def p_decl(self, node: Node, keyword: str | None = None) -> None:
        """A PORT_DECL/NET_DECL/VAR_DECL/PARAM_DECL/GENVAR_DECL without its ';'."""
        node.loc = self.out.here()
        if node.kind == "PORT_DECL":
            self.out.tok(node.sub or "")
            self.out.raw(" ")
            net = node.spell.get("net")
            if net is not None:
                self.out.tok(str(net))
                self.out.raw(" ")
            self.type_words(node, None)
        elif node.kind == "GENVAR_DECL":
            self.out.tok("genvar")
            self.out.raw(" ")
        else:
            self.type_words(node, keyword or node.sub)
        first = {"NET_DECL": 3, "GENVAR_DECL": 0}.get(node.kind, 1)
        self.list_of(node.kids[first:], self.p_declarator)

    # -- items --

    def item(self, node: Node, indent: int) -> None:
        self.comment_line(indent)
        self.attrs(node)
        ITEM_PRINTERS[node.kind](self, node, indent)

    def items(self, nodes: Sequence[Node | None], indent: int) -> None:
        for node in nodes:
            assert node is not None
            self.newline(indent)
            self.item(node, indent)

    def i_decl(self, node: Node, indent: int) -> None:
        self.p_decl(node)
        self.out.tok(";")

    def i_cont_assign(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("assign")
        self.out.raw(" ")
        if node.kids[1] is not None:
            self.p_delay(node.kid(1))
            self.out.raw(" ")
        self.list_of(node.kids[2:], self.p_net_assign)
        self.out.tok(";")

    def p_net_assign(self, node: Node) -> None:
        node.loc = self.out.here()
        self.expr(node.kid(0))
        self.out.raw(" ")
        self.out.tok("=")
        self.gap()
        self.expr(node.kid(1))

    def p_delay(self, node: Node) -> None:
        node.loc = self.out.here()
        self.out.tok("#")
        if node.spell.get("parens"):
            self.out.tok("(")
            self.list_of(node.kids, self.expr)
            self.out.tok(")")
        else:
            self.expr(node.kid(0))

    def i_process(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok(node.sub or "initial")
        self.out.raw(" ")
        self.stmt(node.kid(0), indent)

    def i_instantiation(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.ident(node.name or "")
        self.out.raw(" ")
        if node.kids[0] is not None:
            self.params_list(node.kid(0))
            self.out.raw(" ")
        self.list_of(node.kids[1:], self.p_instance)
        self.out.tok(";")

    def params_list(self, node: Node) -> None:
        node.loc = self.out.here()
        self.out.tok("#")
        self.out.tok("(")
        self.list_of(node.kids, self.p_connection)
        self.out.tok(")")

    def p_instance(self, node: Node) -> None:
        node.loc = self.out.here()
        if node.name is not None:
            self.ident(node.name)
            self.out.raw(" ")
        if node.kids[0] is not None:
            self.p_range(node.kid(0))
            self.out.raw(" ")
        if node.name is None:
            node.loc = self.out.here()
        self.out.tok("(")
        for idx, conn in enumerate(node.kids[1:]):
            assert conn is not None
            if idx:
                self.out.tok(",")
            after = self.out.here()  # an empty slot sits one past the '(' or ','
            if idx and (conn.name is not None or conn.kids[0] is not None):
                self.out.raw(" ")
            self.p_connection(conn)
            if conn.name is None and conn.kids[0] is None:
                conn.loc = after
        self.out.tok(")")

    def p_connection(self, node: Node) -> None:
        node.loc = self.out.here()
        if node.name is not None:
            self.out.tok(".")
            self.ident(node.name)
            self.out.tok("(")
            if node.kids[0] is not None:
                self.expr(node.kid(0))
            self.out.tok(")")
        elif node.kids[0] is not None:
            self.expr(node.kid(0))

    def i_gate(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok(node.sub or "")
        self.out.raw(" ")
        self.list_of(node.kids[2:], self.p_instance)
        self.out.tok(";")

    def i_generate(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("generate")
        self.items(node.kids, indent + 1)
        self.newline(indent)
        self.out.tok("endgenerate")

    def i_gen_for(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.for_header(node)
        self.out.raw(" ")
        self.gen_body(node.kid(3), indent)

    def for_header(self, node: Node) -> None:
        self.out.tok("for")
        self.out.raw(" ")
        self.out.tok("(")
        self.p_assign(node.kid(0), "=")
        self.out.tok(";")
        self.out.raw(" ")
        self.expr(node.kid(1))
        self.out.tok(";")
        self.out.raw(" ")
        self.p_assign(node.kid(2), "=")
        self.out.tok(")")

    def gen_body(self, node: Node, indent: int) -> None:
        """A generate branch: a GEN_BLOCK on this line, else the item on the next one."""
        if node.kind == "GEN_BLOCK":
            self.item(node, indent)
        else:
            self.newline(indent + 1)
            self.item(node, indent + 1)

    def i_gen_block(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("begin")
        if node.name is not None:
            self.out.raw(" ")
            self.out.tok(":")
            self.out.raw(" ")
            self.ident(node.name)
        self.items(node.kids[1:], indent + 1)
        self.newline(indent)
        self.out.tok("end")

    def i_gen_if(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("if")
        self.out.raw(" ")
        self.out.tok("(")
        self.expr(node.kid(0))
        self.out.tok(")")
        self.out.raw(" ")
        self.gen_body(node.kid(1), indent)
        if node.kids[2] is not None:
            self.newline(indent)
            self.out.tok("else")
            self.out.raw(" ")
            other = node.kid(2)
            if other.kind in ("GEN_IF", "GEN_BLOCK"):
                self.item(other, indent)
            else:
                self.gen_body(other, indent)

    def i_gen_case(self, node: Node, indent: int) -> None:
        self.case_common(node, indent, self.gen_body)

    def i_defparam(self, node: Node, indent: int) -> None:
        """defparam u.P = v; -- one DEFPARAM per pair, its range the pair (2C §3.6)."""
        self.out.tok("defparam")
        self.out.raw(" ")
        node.loc = self.out.here()
        target = node.kid(0)
        target.loc = node.loc
        for idx, part in enumerate(target.kids):
            assert part is not None
            if idx:
                self.out.tok(".")
            self.expr(part)
        self.out.raw(" ")
        self.out.tok("=")
        self.out.raw(" ")
        self.expr(node.kid(1))
        self.out.tok(";")

    def i_null(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok(";")

    def i_function(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("function")
        self.out.raw(" ")
        if "AUTOMATIC" in node.flags:
            self.out.tok("automatic")
            self.out.raw(" ")
        self.type_words(node, None)
        self.ident(node.name or "")
        self.subprogram_rest(node, 1, indent, "endfunction")

    def i_task(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("task")
        self.out.raw(" ")
        if "AUTOMATIC" in node.flags:
            self.out.tok("automatic")
            self.out.raw(" ")
        self.ident(node.name or "")
        self.subprogram_rest(node, 0, indent, "endtask")

    def subprogram_rest(self, node: Node, first: int, indent: int, end: str) -> None:
        """Ports (ANSI header or body declarations), locals, the body statement, the end."""
        ports, locs, body = node.kid(first), node.kid(first + 1), node.kid(first + 2)
        ansi = bool(node.spell.get("ansi"))
        if ansi:
            self.out.raw(" ")
            self.out.tok("(")
            ports.loc = self.out.here()
            self.list_of(ports.kids, self.p_decl)
            self.out.tok(")")
        self.out.tok(";")
        after_header = self.out.here()
        spelled = node.spell.get("body_order")
        order = [k for k in spelled if isinstance(k, Node)] if isinstance(spelled, list) else []
        if not order:
            order = [k for k in locs.kids if k is not None]
            if not ansi:
                order = [k for k in ports.kids if k is not None] + order
        for decl in order:
            self.newline(indent + 1)
            self.item(decl, indent + 1)
        for lst in (ports, locs):
            if lst.kids and (lst is locs or not ansi):
                lst.loc = lst.kid(0).loc
            elif not lst.kids:
                lst.loc = after_header
        self.newline(indent + 1)
        self.stmt(body, indent + 1)
        self.newline(indent)
        self.out.tok(end)

    # -- statements --

    def stmt(self, node: Node, indent: int) -> None:
        self.attrs(node)
        STMT_PRINTERS[node.kind](self, node, indent)

    def p_assign(self, node: Node, op: str) -> None:
        node.loc = self.out.here()
        self.expr(node.kid(0))
        self.out.raw(" ")
        self.out.tok(op)
        self.gap()
        if node.kids[1] is not None:
            self.p_delay(node.kid(1))
            self.out.raw(" ")
        self.expr(node.kid(2))

    def s_blocking(self, node: Node, indent: int) -> None:
        self.p_assign(node, "=")
        self.out.tok(";")

    def s_nonblocking(self, node: Node, indent: int) -> None:
        self.p_assign(node, "<=")
        self.out.tok(";")

    def s_seq_block(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("begin")
        if node.name is not None:
            self.out.raw(" ")
            self.out.tok(":")
            self.out.raw(" ")
            self.ident(node.name)
        locs = node.kids[0]
        if locs is not None:
            self.items(locs.kids, indent + 1)
            locs.loc = locs.kid(0).loc
        for sub in node.kids[1:]:
            assert sub is not None
            self.newline(indent + 1)
            self.stmt(sub, indent + 1)
        self.newline(indent)
        self.out.tok("end")

    def sub_stmt(self, node: Node, indent: int) -> None:
        """A controlled statement: a block on this line, anything else on the next."""
        if node.kind == "SEQ_BLOCK" and not node.attrs:
            self.out.raw(" ")
            self.stmt(node, indent)
        else:
            self.newline(indent + 1)
            self.stmt(node, indent + 1)

    def s_if(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok("if")
        self.out.raw(" ")
        self.out.tok("(")
        self.expr(node.kid(0))
        self.out.tok(")")
        self.sub_stmt(node.kid(1), indent)
        if node.kids[2] is not None:
            self.newline(indent)
            self.out.tok("else")
            other = node.kid(2)
            if other.kind == "IF" and not other.attrs:
                self.out.raw(" ")
                self.stmt(other, indent)
            else:
                self.sub_stmt(other, indent)

    def s_case(self, node: Node, indent: int) -> None:
        self.case_common(node, indent, lambda body, ind: self.sub_stmt_inline(body, ind))

    def sub_stmt_inline(self, node: Node, indent: int) -> None:
        self.stmt(node, indent + 1)

    def case_common(self, node: Node, indent: int,
                    body: Callable[[Node, int], None]) -> None:
        node.loc = self.out.here()
        self.out.tok(node.sub or "case")
        self.out.raw(" ")
        self.out.tok("(")
        self.expr(node.kid(0))
        self.out.tok(")")
        self.metacomment(node)
        for item in node.kids[1:]:
            assert item is not None
            self.newline(indent + 1)
            item.loc = self.out.here()
            if "DEFAULT" in item.flags:
                self.out.tok("default")
                if not item.spell.get("no_colon"):
                    self.out.tok(":")
            else:
                self.list_of(item.kids[1:], self.expr)
                self.out.tok(":")
            self.out.raw(" ")
            body(item.kid(0), indent + 1)
        self.newline(indent)
        self.out.tok("endcase")

    def s_for(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.for_header(node)
        self.sub_stmt(node.kid(3), indent)

    def s_repeat(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.out.tok(node.kind.lower())
        self.out.raw(" ")
        self.out.tok("(")
        self.expr(node.kid(0))
        self.out.tok(")")
        self.sub_stmt(node.kid(1), indent)

    def s_timing(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        self.p_event_control(node.kid(0))
        self.sub_stmt(node.kid(1), indent)

    def p_event_control(self, node: Node) -> None:
        node.loc = self.out.here()
        if "STAR" in node.flags:
            self.out.tok(str(node.spell.get("star", "@*")))
            return
        self.out.tok("@")
        if "NO_PARENS" in node.flags:
            only = node.kid(1)
            only.loc = self.out.here()
            self.expr(only.kid(0))
            return
        self.out.tok("(")
        for idx, ev in enumerate(node.kids[1:]):
            assert ev is not None
            if idx:
                if "AFTER_OR" in ev.flags:
                    self.out.raw(" ")
                    self.out.tok("or")
                    self.out.raw(" ")
                else:
                    self.out.tok(",")
                    self.out.raw(" ")
            ev.loc = self.out.here()
            if ev.sub != "none":
                self.out.tok(ev.sub or "")
                self.out.raw(" ")
            self.expr(ev.kid(0))
        self.out.tok(")")

    def s_task_call(self, node: Node, indent: int) -> None:
        node.loc = self.out.here()
        if "SYSTEM" in node.flags:
            self.out.tok(node.name or "")
        else:
            self.ident(node.name or "")
        if node.kids or node.spell.get("parens"):
            self.out.tok("(")
            self.list_of(node.kids, self.expr)
            self.out.tok(")")
        self.out.tok(";")

    # -- units --

    def module(self, node: Node) -> None:
        self.comment_line(0)
        self.attrs(node)
        node.loc = self.out.here()
        self.out.tok("macromodule" if "MACROMODULE" in node.flags else "module")
        self.out.raw(" ")
        self.ident(node.name or "")
        params, ports = node.kids[0], node.kids[1]
        if params is not None:
            self.out.raw(" ")
            params.loc = self.out.here()
            self.out.tok("#")
            self.out.tok("(")
            self.list_of(params.kids, self.header_param)
            self.out.tok(")")
        if ports is not None:
            self.out.raw(" ")
            ports.loc = self.out.here()
            self.out.tok("(")
            self.list_of(ports.kids, self.header_port)
            self.out.tok(")")
        self.out.tok(";")
        self.items(node.kids[2:], 1)
        self.newline(0)
        self.out.tok("endmodule")
        self.out.raw("\n")

    def header_param(self, node: Node) -> None:
        self.p_decl(node)

    def header_port(self, node: Node) -> None:
        if node.kind == "PORT_REF":
            self.name_node(node)
        else:
            self.attrs(node)
            self.p_decl(node)

    def directive(self, node: Node) -> None:
        node.loc = self.out.here()
        self.out.tok("`" + (node.sub or ""))
        if node.name is not None:
            self.out.raw(" " + node.name)
        self.out.raw("\n")


EXPR_PRINTERS: dict[str, Callable[[Printer, Node], None]] = {
    "NUMBER": Printer.p_leaf, "REAL": Printer.p_leaf, "STRING": Printer.p_leaf,
    "IDENT": Printer.p_ident, "UNARY": Printer.p_unary, "BINARY": Printer.p_binary,
    "TERNARY": Printer.p_ternary, "SELECT": Printer.p_select, "CONCAT": Printer.p_concat,
    "REPLICATE": Printer.p_replicate, "CALL": Printer.p_call,
}
ITEM_PRINTERS: dict[str, Callable[[Printer, Node, int], None]] = {
    "PORT_DECL": Printer.i_decl, "NET_DECL": Printer.i_decl, "VAR_DECL": Printer.i_decl,
    "PARAM_DECL": Printer.i_decl, "GENVAR_DECL": Printer.i_decl,
    "CONT_ASSIGN": Printer.i_cont_assign, "ALWAYS": Printer.i_process,
    "INITIAL": Printer.i_process, "INSTANTIATION": Printer.i_instantiation,
    "GATE_DECL": Printer.i_gate, "GENERATE": Printer.i_generate, "GEN_FOR": Printer.i_gen_for,
    "GEN_IF": Printer.i_gen_if, "GEN_CASE": Printer.i_gen_case, "GEN_BLOCK": Printer.i_gen_block,
    "NULL_STMT": Printer.i_null, "FUNCTION_DECL": Printer.i_function, "TASK_DECL": Printer.i_task,
    "DEFPARAM": Printer.i_defparam,
}
STMT_PRINTERS: dict[str, Callable[[Printer, Node, int], None]] = {
    "BLOCKING_ASSIGN": Printer.s_blocking, "NONBLOCKING_ASSIGN": Printer.s_nonblocking,
    "SEQ_BLOCK": Printer.s_seq_block, "IF": Printer.s_if, "CASE": Printer.s_case,
    "FOR": Printer.s_for, "REPEAT": Printer.s_repeat, "WHILE": Printer.s_repeat,
    "TIMING_STMT": Printer.s_timing, "TASK_CALL": Printer.s_task_call,
    "NULL_STMT": Printer.i_null,
}


def print_unit(items: Sequence[Node], rng: random.Random) -> Out:
    printer = Printer(rng)
    for item in items:
        if item.kind == "DIRECTIVE":
            printer.directive(item)
        else:
            printer.module(item)
            printer.out.raw("\n")
    return printer.out


# --- the generator --------------------------------------------------------------------------


@dataclass
class Sig:
    """A declared name: its packed range (msb, lsb), signedness, what it is (net, var, param,
    int (integer variable), genvar, mem) and, for a memory, its word range."""

    name: str
    msb: int = 0
    lsb: int = 0
    signed: bool = False
    kind: str = "net"
    words: tuple[int, int] | None = None
    value: int = 0  # a param's value

    @property
    def width(self) -> int:
        return abs(self.msb - self.lsb) + 1


@dataclass
class PortInfo:
    name: str
    direction: str
    width: int
    param: str | None = None  # the range is [param-1:0]


@dataclass
class ModInfo:
    name: str
    params: list[tuple[str, int]]
    ports: list[PortInfo]


@dataclass
class Subprogram:
    """A function (returns a value of width) or task: its input and output widths."""

    name: str
    inputs: list[int]
    outputs: list[int] = field(default_factory=list)
    width: int = 1
    constant: bool = False  # a constant function of one integer argument


@dataclass
class Scope:
    """What a body may read and must drive."""

    reads: list[Sig] = field(default_factory=list)
    consts: list[Sig] = field(default_factory=list)
    wires: list[Sig] = field(default_factory=list)  # nets still needing a driver
    regs: list[Sig] = field(default_factory=list)  # variables still needing a process
    mems: list[Sig] = field(default_factory=list)
    funcs: list[Subprogram] = field(default_factory=list)
    tasks: list[Subprogram] = field(default_factory=list)
    clocks: list[Sig] = field(default_factory=list)
    loop_vars: list[Sig] = field(default_factory=list)
    in_subprogram: bool = False  # Yosys inlines functions only without repeat loops


GATES_N = ("and", "nand", "or", "nor", "xor", "xnor")
GATES_TRI = ("bufif0", "bufif1", "notif0", "notif1")
WEIGHTED_BINARY = (("+", 6), ("-", 5), ("*", 3), ("/", 1), ("%", 1), ("**", 2), ("==", 3),
                   ("!=", 2), ("===", 1), ("!==", 1), ("&&", 3), ("||", 3), ("<", 2), ("<=", 2),
                   (">", 2), (">=", 2), ("&", 6), ("|", 6), ("^", 4), ("~^", 2), ("<<", 2),
                   (">>", 2), ("<<<", 1), (">>>", 1))
COMMENT_TEXT = ("// a plain comment", "/* block */", "// synopsys keep")


def ident(name: str) -> Node:
    return Node("IDENT", name=name)


def num(value: int) -> Node:
    return number_node(str(value))


def rng_node(msb: int, lsb: int, param: str | None = None) -> Node:
    """[msb:lsb], or [param - 1:0] when the width is a parameter's."""
    if param is not None:
        return Node("RANGE", Node("BINARY", ident(param), num(1), sub="-"), num(0))
    return Node("RANGE", num(msb), num(lsb))


def attr(name: str, value: Node | None = None, meta: bool = False) -> Node:
    node = Node("ATTR", value, name=name)
    return node.flag("METACOMMENT", int(meta))


class Gen:
    """Builds one unit from a seed: the tree, the printer's spelling choices included."""

    def __init__(self, seed: int) -> None:
        self.rng = random.Random(seed)
        self.counter = 0
        self.mods: list[ModInfo] = []

    # -- small helpers --

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    def pick(self, items: Sequence[str | int]) -> str | int:
        return self.rng.choice(list(items))

    def fresh(self, prefix: str) -> str:
        """A new name: mostly plain, sometimes with '$' or as an escaped identifier."""
        self.counter += 1
        roll = self.rng.random()
        if roll < 0.04:
            return f"{prefix}+{self.counter}"  # escaped: \p+7
        if roll < 0.08:
            return f"{prefix}_{self.counter}$x"
        return f"{prefix}{self.counter}"

    def attrs_for(self, node: Node, p: float = 0.12) -> Node:
        if not self.chance(p):
            return node
        count = 1 + int(self.chance(0.4))
        for _ in range(count):
            value: Node | None = None
            roll = self.rng.random()
            if roll < 0.3:
                value = self.number(8)
            elif roll < 0.4:
                value = string_node(str(self.pick(('"yes"', '"a b"', '""', '"q\\"t"'))))
            node.attrs.append(attr(self.fresh("at"), value))
        node.spell["attr_split"] = self.rng.randint(1, len(node.attrs))
        return node

    # -- numbers --

    def number(self, width: int, allow_xz: bool = True) -> Node:
        """A literal that fits width bits, in one of every form (L4-L8)."""
        width = max(1, min(width, 32))
        value = self.rng.randrange(0, 1 << min(width, 16))
        roll = self.rng.random()
        if roll < 0.25:
            return number_node(self.decimal_text(value))
        base = str(self.pick("bodh"))
        digits = self.digits(base, value, width, allow_xz and self.chance(0.25))
        if self.chance(0.4):
            base = base.upper()
        if self.chance(0.3):
            digits = digits.upper()
        sign = str(self.pick(("s", "S"))) if self.chance(0.15) else ""
        size = "" if self.chance(0.12) else str(width)
        size_sep = " " if size and self.chance(0.08) else ""
        digit_sep = " " if self.chance(0.06) else ""
        return number_node(f"{size}{size_sep}'{sign}{base}{digit_sep}{digits}")

    def decimal_text(self, value: int) -> str:
        text = str(value)
        if len(text) > 3 and self.chance(0.4):
            text = text[:-3] + "_" + text[-3:]
        return text

    def digits(self, base: str, value: int, width: int, xz: bool) -> str:
        if base == "d":
            if xz:
                return str(self.pick("xz?"))
            return self.decimal_text(value)
        bits = {"b": 1, "o": 3, "h": 4}[base]
        count = max(1, (width + bits - 1) // bits)
        alphabet = "0123456789abcdef"[: 1 << bits]
        out = [alphabet[(value >> (bits * k)) % len(alphabet)] for k in range(count)][::-1]
        if xz:
            out[self.rng.randrange(count)] = str(self.pick("xz?"))
        text = "".join(out)
        if len(text) > 2 and self.chance(0.3):
            cut = self.rng.randrange(1, len(text))
            text = text[:cut] + "_" + text[cut:]
        return text

    # -- expressions --

    def operand(self, scope: Scope, depth: int, width: int) -> Node:
        if depth <= 0 or self.chance(0.3):
            return self.leaf(scope, width)
        roll = self.rng.random()
        if roll < 0.45:
            return self.binary(scope, depth, width)
        if roll < 0.57:
            return self.unary(scope, depth, width)
        if roll < 0.67:
            return Node("TERNARY", self.operand(scope, depth - 1, 1),
                        self.operand(scope, depth - 1, width),
                        self.operand(scope, depth - 1, width))
        if roll < 0.75:
            return self.concat(scope, depth)
        if roll < 0.80:
            return self.replicate(scope, depth)
        if roll < 0.90 and scope.funcs:
            return self.call(scope, depth)
        if roll < 0.95:
            return self.sys_call(scope, depth, width)
        return self.leaf(scope, width)

    def expr(self, scope: Scope, depth: int, width: int) -> Node:
        node = self.operand(scope, depth, width)
        if node.kind in ("BINARY", "UNARY", "TERNARY"):
            self.attrs_for(node, 0.04)
        return node

    def binary(self, scope: Scope, depth: int, width: int) -> Node:
        ops = [op for op, _ in WEIGHTED_BINARY]
        weights = [w for _, w in WEIGHTED_BINARY]
        op = self.rng.choices(ops, weights)[0]
        right_width = 2 if op in ("**", "<<", ">>", "<<<", ">>>") else width
        node = Node("BINARY", self.expr(scope, depth - 1, width),
                    self.expr(scope, depth - 1, right_width), sub=op)
        if op == "~^" and self.chance(0.4):
            node.spell["op"] = "^~"
        return node

    def unary(self, scope: Scope, depth: int, width: int) -> Node:
        op = str(self.pick(UNARY_OPS))
        node = Node("UNARY", self.expr(scope, depth - 1, width), sub=op)
        if op == "~^" and self.chance(0.4):
            node.spell["op"] = "^~"
        return node

    def concat(self, scope: Scope, depth: int) -> Node:
        parts = [self.sized(scope, depth - 1) for _ in range(self.rng.randint(1, 3))]
        return Node("CONCAT", *parts)

    def replicate(self, scope: Scope, depth: int) -> Node:
        count: Node = num(self.rng.randint(1, 3))
        params = [c for c in scope.consts if c.kind == "param" and 1 <= c.value <= 8]
        if params and self.chance(0.3):
            count = ident(self.rng.choice(params).name)
        return Node("REPLICATE", count, self.concat(scope, depth))

    def sized(self, scope: Scope, depth: int) -> Node:
        """A concatenation operand: no unsized literal (E12)."""
        if self.chance(0.3):
            return self.sized_number()
        node = self.operand(scope, depth, 4)
        if node.kind == "NUMBER" or self.contains_unsized(node):
            return self.sized_number()
        return node

    def sized_number(self) -> Node:
        width = self.rng.randint(1, 8)
        node = self.number(width)
        while "'" not in str(node.spell["text"]) or str(node.spell["text"]).startswith("'"):
            node = self.number(width)
        return node

    def contains_unsized(self, node: Node | None) -> bool:
        if node is None:
            return False
        if node.kind == "NUMBER":
            text = str(node.spell["text"])
            return "'" not in text or text.lstrip().startswith("'")
        return any(self.contains_unsized(kid) for kid in node.kids)

    def call(self, scope: Scope, depth: int) -> Node:
        func = self.rng.choice(scope.funcs)
        args = [self.expr(scope, depth - 1, w) for w in func.inputs]
        return Node("CALL", *args, name=func.name)

    def sys_call(self, scope: Scope, depth: int, width: int) -> Node:
        name = str(self.pick(("$signed", "$unsigned")))
        return Node("CALL", self.expr(scope, depth - 1, width), name=name).flag("SYSTEM")

    def leaf(self, scope: Scope, width: int) -> Node:
        roll = self.rng.random()
        if roll < 0.25 or not scope.reads:
            return self.number(width)
        if roll < 0.32 and scope.consts:
            return ident(self.rng.choice(scope.consts).name)
        if roll < 0.38 and scope.loop_vars:
            return ident(self.rng.choice(scope.loop_vars).name)
        if roll < 0.45 and scope.mems:
            return self.mem_read(scope)
        sig = self.rng.choice(scope.reads)
        if sig.width > 1 and self.chance(0.35):
            return self.select(scope, sig)
        return ident(sig.name)

    def select(self, scope: Scope, sig: Sig) -> Node:
        """A bit, part or indexed part select inside sig's range, in its direction."""
        lo, hi = min(sig.msb, sig.lsb), max(sig.msb, sig.lsb)
        roll = self.rng.random()
        if roll < 0.45:
            index = num(self.rng.randint(lo, hi))
            if scope.loop_vars and self.chance(0.3):
                index = ident(self.rng.choice(scope.loop_vars).name)
            return Node("SELECT", ident(sig.name), index, None, sub="bit")
        if roll < 0.75 or hi == lo:
            a, b = sorted((self.rng.randint(lo, hi), self.rng.randint(lo, hi)))
            first, second = (b, a) if sig.msb >= sig.lsb else (a, b)
            return Node("SELECT", ident(sig.name), num(first), num(second), sub="part")
        span = self.rng.randint(1, hi - lo)
        base_hi = hi - span + 1
        start: Node = num(self.rng.randint(lo, base_hi))
        reads = [r for r in scope.reads if r.kind in ("net", "var") and r.width <= 3]
        if reads and self.chance(0.4):
            start = ident(self.rng.choice(reads).name)
        sub = "part_plus" if self.chance(0.6) else "part_minus"
        if sub == "part_minus" and start.kind == "NUMBER":
            start = num(self.rng.randint(lo + span - 1, hi))
        return Node("SELECT", ident(sig.name), start, num(span), sub=sub)

    def mem_read(self, scope: Scope) -> Node:
        mem = self.rng.choice(scope.mems)
        assert mem.words is not None
        addr = self.operand(scope, 1, 3) if self.chance(0.5) else num(mem.words[0])
        word = Node("SELECT", ident(mem.name), addr, None, sub="bit")
        if mem.width > 1 and self.chance(0.3):
            return Node("SELECT", word, num(self.rng.randint(0, mem.width - 1)), None, sub="bit")
        return word

    # -- statements --

    def lvalue(self, targets: Sequence[Sig], concat: bool = True) -> Node:
        """A variable lvalue: a target, a select of one, or a concatenation of targets."""
        sig = self.rng.choice(list(targets))
        roll = self.rng.random()
        if roll < 0.15 and len(targets) > 1 and concat:
            others = self.rng.sample(list(targets), 2)
            return Node("CONCAT", *(ident(t.name) for t in others))
        if roll < 0.35 and sig.width > 1:
            return self.select(Scope(), sig)
        return ident(sig.name)

    def assign(self, scope: Scope, targets: Sequence[Sig], nonblocking: bool) -> Node:
        lhs = self.lvalue(targets, not scope.in_subprogram)  # Yosys: no composite lhs there
        delay: Node | None = None
        if nonblocking and self.chance(0.05):
            delay = Node("DELAY", num(1))
        kind = "NONBLOCKING_ASSIGN" if nonblocking else "BLOCKING_ASSIGN"
        return Node(kind, lhs, delay, self.expr(scope, 3, 8))

    def stmt(self, scope: Scope, targets: Sequence[Sig], nonblocking: bool, depth: int) -> Node:
        roll = self.rng.random() if depth > 0 else 0.0
        node: Node
        if roll < 0.40:
            node = self.assign(scope, targets, nonblocking)
        elif roll < 0.58:
            node = self.if_stmt(scope, targets, nonblocking, depth)
        elif roll < 0.72:
            node = self.case_stmt(scope, targets, nonblocking, depth)
        elif roll < 0.80:
            node = self.block(scope, targets, nonblocking, depth)
        elif roll < 0.86 and scope.loop_vars:
            node = self.for_stmt(scope, targets, nonblocking, depth)
        elif roll < 0.90 and not scope.in_subprogram:
            node = Node("REPEAT", num(self.rng.randint(1, 3)),
                        self.stmt(scope, targets, nonblocking, depth - 1))
        elif roll < 0.95 and scope.tasks and not nonblocking:
            node = self.task_call(scope, targets)
        else:
            node = self.assign(scope, targets, nonblocking)
        self.attrs_for(node, 0.05)
        # (* *) attributes come first, then the metacomments the CASE reduction adds (2C §3.5)
        node.attrs.sort(key=lambda a: "METACOMMENT" in a.flags)
        return node

    def if_stmt(self, scope: Scope, targets: Sequence[Sig], nb: bool, depth: int) -> Node:
        then = self.stmt(scope, targets, nb, depth - 1)
        other: Node | None = None
        if self.chance(0.6):
            other = self.stmt(scope, targets, nb, depth - 1)
            if is_open(then):
                then = Node("SEQ_BLOCK", None, then)
        return Node("IF", self.expr(scope, 2, 1), then, other)

    def case_stmt(self, scope: Scope, targets: Sequence[Sig], nb: bool, depth: int) -> Node:
        sub = str(self.pick(("case", "case", "casez", "casex")))
        width = self.rng.randint(1, 4)
        items = [self.case_item(width, sub != "case",
                                lambda: self.stmt(scope, targets, nb, depth - 1))
                 for _ in range(self.rng.randint(1, 4))]
        if self.chance(0.6):
            body = self.stmt(scope, targets, nb, depth - 1)
            items.insert(self.rng.randint(0, len(items)), self.default_item(body))
        node = Node("CASE", self.expr(scope, 1, width), *items, sub=sub)
        self.case_attrs(node)
        return node

    def case_item(self, width: int, wild: bool, body: Callable[[], Node]) -> Node:
        labels = [self.number(width, allow_xz=wild or self.chance(0.1))
                  for _ in range(self.rng.randint(1, 2))]
        stmt = Node("NULL_STMT") if self.chance(0.08) else body()
        return Node("CASE_ITEM", stmt, *labels)

    def default_item(self, body: Node) -> Node:
        node = Node("CASE_ITEM", body).flag("DEFAULT")
        node.spell["no_colon"] = self.chance(0.2)
        return node

    def case_attrs(self, node: Node) -> None:
        """(* full_case *)-style attributes and/or a full_case/parallel_case metacomment."""
        if self.chance(0.08):
            node.attrs.append(attr(str(self.pick(("full_case", "parallel_case")))))
        if self.chance(0.2):
            keys = [k for k in ("full_case", "parallel_case") if self.chance(0.6)]
            keys = keys or ["parallel_case"]
            node.attrs.extend(attr(k, meta=True) for k in keys)
            node.spell["meta_prefix"] = self.pick(("synopsys", "synthesis"))
            node.spell["meta_reversed"] = self.chance(0.3)
            node.spell["meta_block"] = self.chance(0.25)

    def block(self, scope: Scope, targets: Sequence[Sig], nb: bool, depth: int) -> Node:
        name = self.fresh("blk") if self.chance(0.4) else None
        locs: Node | None = None
        inner = scope
        stmts: list[Node] = []
        if name is not None and self.chance(0.4) and not nb:
            local = Sig(self.fresh("lv"), self.rng.randint(0, 5), 0, kind="var")
            locs = Node("LIST", self.var_decl(local))
            inner = dataclasses.replace(scope, reads=scope.reads + [local])
            stmts.append(Node("BLOCKING_ASSIGN", ident(local.name), None,
                              self.expr(scope, 2, local.width)))
        stmts += [self.stmt(inner, targets, nb, depth - 1) for _ in range(self.rng.randint(1, 3))]
        return Node("SEQ_BLOCK", locs, *stmts, name=name)

    def for_stmt(self, scope: Scope, targets: Sequence[Sig], nb: bool, depth: int) -> Node:
        var = self.rng.choice(scope.loop_vars).name
        bound = self.rng.randint(1, 4)
        if self.chance(0.3):  # counting down
            init = Node("BLOCKING_ASSIGN", ident(var), None, num(bound - 1))
            cond = Node("BINARY", ident(var), num(0), sub=">=")
            step = Node("BINARY", ident(var), num(1), sub="-")
        else:
            init = Node("BLOCKING_ASSIGN", ident(var), None, num(0))
            cond = Node("BINARY", ident(var), num(bound), sub="<")
            step = Node("BINARY", ident(var), num(1), sub="+")
        inner = dataclasses.replace(scope, loop_vars=[v for v in scope.loop_vars
                                                      if v.name != var])
        body = self.stmt(inner, targets, nb, depth - 1)
        return Node("FOR", init, cond, Node("BLOCKING_ASSIGN", ident(var), None, step), body)

    def task_call(self, scope: Scope, targets: Sequence[Sig]) -> Node:
        task = self.rng.choice(scope.tasks)
        args = [self.expr(scope, 1, w) for w in task.inputs]
        args += [ident(self.rng.choice(list(targets)).name) for _ in task.outputs]
        return Node("TASK_CALL", *args, name=task.name)

    def var_decl(self, sig: Sig, sub: str = "reg") -> Node:
        rng = rng_node(sig.msb, sig.lsb) if sig.width > 1 or self.chance(0.1) else None
        if sub != "reg":
            rng = None
        node = Node("VAR_DECL", rng, Node("DECLARATOR", None, name=sig.name), sub=sub)
        return node.flag("SIGNED", int(sig.signed and sub == "reg"))


def yosys_cell_trap(node: Node) -> bool:
    """A connection Yosys 0.55 asserts on (genrtlil.cc:2133, `arg->is_signed ==
    sig.as_wire()->is_signed`): `.p($signed(e))`, `.p($unsigned(e))`, `.p({s})`, `.p({n{s}})`
    with s signed. Not a 2C question; the generator avoids the shapes."""
    return (node.kind == "CALL" and "SYSTEM" in node.flags) or node.kind == "REPLICATE" or (
        has_single_concat(node))


def has_single_concat(node: Node | None) -> bool:
    """A one-element concatenation anywhere below node (`.p({1'b0} ? b : i)` asserts too)."""
    if node is None:
        return False
    if node.kind == "CONCAT" and len(node.kids) == 1:
        return True
    return any(has_single_concat(kid) for kid in node.kids)


def is_open(node: Node | None) -> bool:
    """Whether a statement or generate item ends in an if without else (it would capture a
    following else)."""
    if node is None:
        return False
    if node.kind in ("IF", "GEN_IF"):
        return node.kids[2] is None or is_open(node.kids[2])
    if node.kind in ("FOR", "GEN_FOR"):
        return is_open(node.kids[3])
    if node.kind in ("REPEAT", "WHILE", "TIMING_STMT"):
        return is_open(node.kids[1])
    return False


class ModuleGen:
    """One module: its header, declarations, subprograms and items."""

    def __init__(self, gen: Gen, name: str, earlier: Sequence[ModInfo]) -> None:
        self.gen = gen
        self.name = name
        self.earlier = earlier
        self.scope = Scope()
        self.decls: list[Node] = []
        self.subprograms: list[Node] = []
        self.items: list[Node] = []
        self.header_params: list[tuple[str, int]] = []
        self.ports: list[PortInfo] = []
        self.after_subprograms: list[Node] = []
        self.pending_genvars: list[Node] = []
        self.defparams: list[Node] = []

    @property
    def rng(self) -> random.Random:
        return self.gen.rng

    def chance(self, p: float) -> bool:
        return self.gen.chance(p)

    # -- the header --

    def params(self) -> Node | None:
        if not self.chance(0.4):
            return None
        decls = []
        for _ in range(self.rng.randint(1, 2)):
            names = [self.gen.fresh("P") for _ in range(self.rng.randint(1, 2))]
            values = [self.rng.randint(2, 6) for _ in names]
            decl = Node("PARAM_DECL", None, *(Node("DECLARATOR", num(v), name=n)
                                              for n, v in zip(names, values, strict=True)),
                        sub="parameter").flag("IN_HEADER")
            roll = self.rng.random()
            if roll < 0.15:
                decl.kids[0] = rng_node(self.rng.randint(3, 7), 0)
            elif roll < 0.25:
                decl.flag("DT", DT_VALUE["integer"])
                decl.spell["dt"] = "integer"
            decls.append(decl)
            for name, value in zip(names, values, strict=True):
                self.header_params.append((name, value))
                self.scope.consts.append(Sig(name, kind="param", value=value))
        return Node("LIST", *decls)

    def make_ports(self) -> None:
        """Clock and reset, one to three data inputs, one to three outputs (wire or reg)."""
        self.ports.append(PortInfo(self.gen.fresh("clk"), "input", 1))
        if self.chance(0.6):
            self.ports.append(PortInfo(self.gen.fresh("rst"), "input", 1))
        for _ in range(self.rng.randint(1, 3)):
            self.ports.append(self.data_port("input", "in"))
        for _ in range(self.rng.randint(1, 3)):
            self.ports.append(self.data_port("output", "out"))
        if self.chance(0.08):
            self.ports.append(PortInfo(self.gen.fresh("io"), "inout", 1))

    def data_port(self, direction: str, prefix: str) -> PortInfo:
        if self.header_params and self.chance(0.3):
            name, value = self.rng.choice(self.header_params)
            return PortInfo(self.gen.fresh(prefix), direction, value, name)
        return PortInfo(self.gen.fresh(prefix), direction, self.rng.randint(1, 8))

    def port_sigs(self) -> list[tuple[PortInfo, Sig]]:
        out = []
        for port in self.ports:
            ascending = port.param is None and port.width > 1 and self.chance(0.1)
            msb, lsb = (0, port.width - 1) if ascending else (port.width - 1, 0)
            kind = "var" if port.direction == "output" and self.chance(0.45) else "net"
            kind = "inout" if port.direction == "inout" else kind
            out.append((port, Sig(port.name, msb, lsb, kind=kind,
                                  signed=port.width > 1 and self.chance(0.1))))
        return out

    def port_decl(self, group: list[tuple[PortInfo, Sig]], in_header: bool) -> Node:
        port, sig = group[0]
        rng: Node | None = None
        if sig.width > 1 or port.param is not None:
            rng = rng_node(sig.msb, sig.lsb, port.param)
        decls = [Node("DECLARATOR", None, name=s.name) for _, s in group]
        node = Node("PORT_DECL", rng, *decls, sub=port.direction)
        node.flag("IN_HEADER", int(in_header)).flag("SIGNED", int(sig.signed))
        if sig.kind == "var" and (in_header or self.chance(0.5)):
            node.flag("DT", DT_VALUE["reg"])
            node.spell["dt"] = "reg"
            sig.kind = "var_declared"
        elif sig.kind == "net" and self.chance(0.15):
            node.flag("NET", 1)
            node.spell["net"] = "wire"
        return node

    def group_ports(self, sigs: list[tuple[PortInfo, Sig]]) -> list[list[tuple[PortInfo, Sig]]]:
        """Consecutive ports of one direction, range and kind may share a declaration."""
        groups: list[list[tuple[PortInfo, Sig]]] = []
        for port, sig in sigs:
            last = groups[-1][-1] if groups else None
            if (last is not None and last[0].direction == port.direction
                    and (last[1].msb, last[1].lsb, last[0].param, last[1].kind, last[1].signed)
                    == (sig.msb, sig.lsb, port.param, sig.kind, sig.signed)
                    and self.chance(0.6)):
                groups[-1].append((port, sig))
            else:
                groups.append([(port, sig)])
        return groups

    def header(self) -> tuple[Node | None, Node | None, list[Node]]:
        """The params and ports LISTs and the body port declarations (non-ANSI)."""
        params = self.params()
        self.make_ports()
        sigs = self.port_sigs()
        ansi = self.chance(0.7)
        body: list[Node] = []
        if ansi:
            decls = [self.gen.attrs_for(self.port_decl(g, True), 0.05)
                     for g in self.group_ports(sigs)]
            ports = Node("LIST", *decls)
        else:
            ports = Node("LIST", *(Node("PORT_REF", None, name=p.name) for p in self.ports))
            for group in self.group_ports(sigs):
                body.append(self.port_decl(group, False))
                for _, sig in group:
                    if sig.kind == "var":
                        body.append(self.gen.var_decl(sig))
        for port, sig in sigs:
            self.register_port(port, sig)
        return params, ports, body

    def register_port(self, port: PortInfo, sig: Sig) -> None:
        if port.direction == "input":
            self.scope.reads.append(sig)
            if sig.width == 1:
                self.scope.clocks.append(sig)
        elif sig.kind.startswith("var"):
            sig.kind = "var"
            self.scope.regs.append(sig)
        else:
            self.scope.wires.append(sig)
            if port.direction == "inout":
                self.scope.reads.append(sig)

    # -- declarations --

    def declarations(self) -> None:
        for _ in range(self.rng.randint(0, 2)):
            self.decls.append(self.gen.attrs_for(self.localparam(), 0.05))
        for _ in range(self.rng.randint(0, 3)):
            self.decls.append(self.gen.attrs_for(self.wire_decl(), 0.08))
        for _ in range(self.rng.randint(0, 2)):
            sig = Sig(self.gen.fresh("r"), self.rng.randint(0, 7), 0, kind="var",
                      signed=self.chance(0.1))
            self.decls.append(self.gen.attrs_for(self.gen.var_decl(sig), 0.08))
            self.scope.regs.append(sig)
        if self.chance(0.6):
            ints = [Sig(self.gen.fresh("i"), 31, 0, True, "int")
                    for _ in range(self.rng.randint(1, 2))]
            decls = [Node("DECLARATOR", None, name=s.name) for s in ints]
            self.decls.append(Node("VAR_DECL", None, *decls, sub="integer"))
            self.scope.loop_vars.extend(ints)
        if self.chance(0.3):
            self.memory()
        if self.chance(0.06):  # names reserved only in later standards are identifiers here
            name = str(self.gen.pick(("logic", "bit", "int", "byte", "interface")))
            init = self.gen.expr(self.scope, 1, 1)
            self.decls.append(Node("NET_DECL", None, None, None,
                                   Node("DECLARATOR", init, name=name), sub="wire"))
            self.scope.reads.append(Sig(name))

    def localparam(self) -> Node:
        name = self.gen.fresh("LP")
        sub = "localparam" if self.chance(0.75) else "parameter"
        roll = self.rng.random()
        rng: Node | None = None
        dt: str | None = None
        value: Node
        known: int | None = None
        if roll < 0.35:
            width = self.rng.randint(2, 8)
            rng = rng_node(width - 1, 0)
            value = self.gen.number(width, allow_xz=False)
            known = None
        elif roll < 0.55:
            dt = "integer"
            known = self.rng.randint(1, 9)
            value = num(known)
        elif roll < 0.70 and self.scope.consts:
            base = self.rng.choice(self.scope.consts)
            arg = Node("BINARY", ident(base.name), num(1), sub="+")
            value = Node("CALL", arg, name="$clog2").flag("SYSTEM")
            known = max(0, (base.value).bit_length())
        elif roll < 0.80:
            dt = "real"
            value = real_node(str(self.gen.pick(("1.5", "2.0e1", "1e3", "3_0.25", "0.5E-1"))))
        elif roll < 0.88:
            value = string_node(str(self.gen.pick(('"ab"', '"x\\ty"', '""'))))
        else:
            known = self.rng.randint(1, 9)
            value = num(known)
        node = Node("PARAM_DECL", rng, Node("DECLARATOR", value, name=name), sub=sub)
        if dt is not None:
            node.flag("DT", DT_VALUE[dt])
            node.spell["dt"] = dt
        if known is not None:
            self.scope.consts.append(Sig(name, kind="param", value=known))
        return node

    def wire_decl(self) -> Node:
        kind = str(self.gen.pick(("wire",) * 16 + ("tri",) * 2 + ("wand", "wor") * 2
                                 + ("supply0", "supply1")))
        supply = kind.startswith("supply")  # N16: scalar, since Yosys 0.55 takes no range on one
        width = 1 if supply else self.rng.randint(1, 8)
        sig = Sig(self.gen.fresh("w"), width - 1, 0, signed=width > 1 and self.chance(0.1))
        init: Node | None = None
        if supply:
            pass  # a supply net has its value: it is read, never driven (Yosys takes no `= e`)
        elif self.chance(0.3):
            init = self.gen.expr(self.scope, 2, width)
        else:
            self.scope.wires.append(sig)
        rng = rng_node(sig.msb, sig.lsb) if width > 1 else None
        node = Node("NET_DECL", None, rng, None, Node("DECLARATOR", init, name=sig.name), sub=kind)
        node.flag("SIGNED", int(sig.signed))
        self.scope.reads.append(sig)
        return node

    def memory(self) -> None:
        width = self.rng.randint(1, 8)
        words = (0, self.rng.randint(1, 7))
        if self.chance(0.4):
            words = (words[1], 0)
        mem = Sig(self.gen.fresh("mem"), width - 1, 0, kind="mem", words=words)
        rng = rng_node(width - 1, 0) if width > 1 else None
        decl = Node("DECLARATOR", None, rng_node(words[0], words[1]), name=mem.name)
        self.decls.append(Node("VAR_DECL", rng, decl, sub="reg"))
        self.scope.mems.append(mem)
        clocks = self.scope.clocks
        if clocks:
            addr = self.gen.operand(self.scope, 1, 3)
            data = self.gen.expr(self.scope, 2, width)
            write = Node("NONBLOCKING_ASSIGN", Node("SELECT", ident(mem.name), addr, None,
                                                   sub="bit"), None, data)
            self.items.append(self.always_on([self.edge(clocks[0], "posedge")], write))
        if self.scope.loop_vars and self.chance(0.6):
            var = self.scope.loop_vars[0].name
            lo, hi = min(words), max(words)
            loop = Node("FOR", Node("BLOCKING_ASSIGN", ident(var), None, num(lo)),
                        Node("BINARY", ident(var), num(hi + 1), sub="<"),
                        Node("BLOCKING_ASSIGN", ident(var), None,
                             Node("BINARY", ident(var), num(1), sub="+")),
                        Node("BLOCKING_ASSIGN",
                             Node("SELECT", ident(mem.name), ident(var), None, sub="bit"), None,
                             self.gen.number(width, allow_xz=False)))
            self.items.append(Node("INITIAL", Node("SEQ_BLOCK", None, loop)))

    # -- subprograms --

    def function(self) -> Node:
        name = self.gen.fresh("f")
        width = self.rng.randint(1, 8)
        inputs = [Sig(self.gen.fresh("x"), self.rng.randint(0, 5), 0, kind="var")
                  for _ in range(self.rng.randint(1, 3))]
        locals_ = [Sig(self.gen.fresh("t"), self.rng.randint(0, 5), 0, kind="var")
                   for _ in range(self.rng.randint(0, 2))]
        integer_ret = self.chance(0.15)
        ret = Sig(name, 31 if integer_ret else width - 1, 0, kind="var")
        inner = Scope(reads=inputs + locals_, consts=list(self.scope.consts),
                      funcs=list(self.scope.funcs), in_subprogram=True)
        body: list[Node] = [Node("BLOCKING_ASSIGN", ident(t.name), None,
                                 self.gen.expr(inner, 2, t.width)) for t in locals_]
        if self.chance(0.4):
            body.append(self.gen.stmt(inner, [ret] + locals_, False, 2))
        body.append(Node("BLOCKING_ASSIGN", ident(name), None, self.gen.expr(inner, 3, width)))
        rng = None if integer_ret or width == 1 else rng_node(width - 1, 0)
        node = self.subprogram("FUNCTION_DECL", name, inputs, [], locals_, body)
        node.kids.insert(0, rng)
        if integer_ret:
            node.flag("DT", DT_VALUE["integer"])
            node.spell["dt"] = "integer"
        node.flag("SIGNED", int(rng is not None and self.chance(0.1)))
        self.scope.funcs.append(Subprogram(name, [s.width for s in inputs], width=width))
        return node

    def task(self) -> Node:
        name = self.gen.fresh("tk")
        inputs = [Sig(self.gen.fresh("ti"), self.rng.randint(0, 5), 0, kind="var")
                  for _ in range(self.rng.randint(0, 2))]
        outputs = [Sig(self.gen.fresh("to"), self.rng.randint(0, 5), 0, kind="var")
                   for _ in range(self.rng.randint(1, 2))]
        inner = Scope(reads=list(inputs), consts=list(self.scope.consts),
                      funcs=list(self.scope.funcs), in_subprogram=True)
        body = [Node("BLOCKING_ASSIGN", ident(o.name), None, self.gen.expr(inner, 2, o.width))
                for o in outputs]
        node = self.subprogram("TASK_DECL", name, inputs, outputs, [], body)
        self.scope.tasks.append(Subprogram(name, [s.width for s in inputs],
                                           [s.width for s in outputs]))
        return node

    def subprogram(self, kind: str, name: str, inputs: list[Sig], outputs: list[Sig],
                   locals_: list[Sig], body: list[Node]) -> Node:
        ansi = self.chance(0.5)
        ports = [self.io_decl(s, "input", ansi) for s in inputs]
        ports += [self.io_decl(s, "output", ansi) for s in outputs]
        local_decls = [self.gen.var_decl(s) for s in locals_]
        stmt = body[0] if len(body) == 1 and self.chance(0.4) else Node("SEQ_BLOCK", None, *body)
        node = Node(kind, Node("LIST", *ports), Node("LIST", *local_decls), stmt, name=name)
        node.flag("AUTOMATIC", int(self.chance(0.15)))
        node.spell["ansi"] = ansi and bool(ports)
        if not node.spell["ansi"]:
            node.spell["body_order"] = self.merge(ports, local_decls)
        return self.gen.attrs_for(node, 0.05)

    def merge(self, first: list[Node], second: list[Node]) -> list[Node]:
        """A random interleaving that keeps each list's order (the LISTs keep source order)."""
        out: list[Node] = []
        i = j = 0
        while i < len(first) or j < len(second):
            if j >= len(second) or (i < len(first) and self.chance(0.5)):
                out.append(first[i])
                i += 1
            else:
                out.append(second[j])
                j += 1
        return out

    def io_decl(self, sig: Sig, direction: str, ansi: bool) -> Node:
        rng = rng_node(sig.msb, sig.lsb) if sig.width > 1 else None
        node = Node("PORT_DECL", rng, Node("DECLARATOR", None, name=sig.name), sub=direction)
        return node.flag("IN_HEADER", int(ansi))

    def constant_function(self) -> Node:
        """K7, F1, F2: an integer function of one integer used in a localparam."""
        name = self.gen.fresh("cf")
        arg, var = self.gen.fresh("n"), self.gen.fresh("k")
        use_while = self.chance(0.5)
        acc = Node("BLOCKING_ASSIGN", ident(name), None,
                   Node("BINARY", ident(name), ident(var), sub="+"))
        step = Node("BLOCKING_ASSIGN", ident(var), None,
                    Node("BINARY", ident(var), num(1), sub="+"))
        cond = Node("BINARY", ident(var), ident(arg), sub="<")
        loop: Node
        if use_while:
            loop = Node("WHILE", cond, Node("SEQ_BLOCK", None, acc, step))
        else:
            loop = Node("FOR", Node("BLOCKING_ASSIGN", ident(var), None, num(0)), cond, step, acc)
        body = [Node("BLOCKING_ASSIGN", ident(name), None, num(0))]
        if use_while:
            body.append(Node("BLOCKING_ASSIGN", ident(var), None, num(0)))
        body.append(loop)
        ports = [Node("PORT_DECL", None, Node("DECLARATOR", None, name=arg), sub="input")
                 .flag("DT", DT_VALUE["integer"])]
        ports[0].spell["dt"] = "integer"
        local = Node("VAR_DECL", None, Node("DECLARATOR", None, name=var), sub="integer")
        node = Node("FUNCTION_DECL", None, Node("LIST", *ports), Node("LIST", local),
                    Node("SEQ_BLOCK", None, *body), name=name)
        node.flag("DT", DT_VALUE["integer"])
        node.spell["dt"] = "integer"
        node.spell["body_order"] = ports + [local]
        value = self.rng.randint(1, 5)
        param = self.gen.fresh("LC")
        self.after_subprograms.append(
            Node("PARAM_DECL", None, Node("DECLARATOR", Node("CALL", num(value), name=name),
                                          name=param), sub="localparam"))
        self.scope.consts.append(Sig(param, kind="param", value=value * (value - 1) // 2))
        return node

    # -- processes --

    def edge(self, sig: Sig, edge: str, after_or: bool = False) -> Node:
        node = Node("EVENT_EXPR", ident(sig.name), sub=edge)
        return node.flag("AFTER_OR", int(after_or))

    def always_on(self, events: list[Node], body: Node) -> Node:
        control = Node("EVENT_CONTROL", None, *events)
        return Node("ALWAYS", Node("TIMING_STMT", control, body), sub="always")

    def comb_process(self, targets: list[Sig]) -> Node:
        if self.chance(0.015) and self.scope.reads:
            sig = self.rng.choice(self.scope.reads)
            control = Node("EVENT_CONTROL", None, self.edge(sig, "none")).flag("NO_PARENS")
        elif self.chance(0.7) or not self.scope.reads:
            control = Node("EVENT_CONTROL", None).flag("STAR")
            control.spell["star"] = self.gen.pick(("@*", "@*", "@(*)", "@ ( * )"))
        else:
            reads = self.rng.sample(self.scope.reads, min(len(self.scope.reads), 3))
            events = [self.edge(sig, "none", idx > 0 and self.chance(0.5))
                      for idx, sig in enumerate(reads)]
            control = Node("EVENT_CONTROL", None, *events)
        stmts = [Node("BLOCKING_ASSIGN", ident(t.name), None, self.gen.expr(self.scope, 2, t.width))
                 for t in targets]
        stmts += [self.gen.stmt(self.scope, targets, False, 3)
                  for _ in range(self.rng.randint(0, 2))]
        body = stmts[0] if len(stmts) == 1 else Node("SEQ_BLOCK", None, *stmts)
        return self.gen.attrs_for(Node("ALWAYS", Node("TIMING_STMT", control, body),
                                       sub="always"), 0.08)

    def ff_process(self, targets: list[Sig]) -> Node:
        clocks = self.scope.clocks
        clock = clocks[0]
        events = [self.edge(clock, str(self.gen.pick(("posedge", "negedge"))))]
        main: list[Node] = [self.gen.stmt(self.scope, targets, True, 2)
                            for _ in range(self.rng.randint(1, 2))]
        body = main[0] if len(main) == 1 else Node("SEQ_BLOCK", None, *main)
        if len(clocks) > 1 and self.chance(0.6):
            reset = clocks[1]
            edge = str(self.gen.pick(("posedge", "negedge")))
            events.append(self.edge(reset, edge, self.chance(0.5)))
            cond: Node = ident(reset.name)
            if edge == "negedge":
                cond = Node("UNARY", cond, sub=str(self.gen.pick(("!", "~"))))
            clears = [Node("NONBLOCKING_ASSIGN", ident(t.name), None, num(0)) for t in targets]
            then = clears[0] if len(clears) == 1 else Node("SEQ_BLOCK", None, *clears)
            if is_open(then):
                then = Node("SEQ_BLOCK", None, then)
            body = Node("IF", cond, then, body)
        return self.gen.attrs_for(self.always_on(events, body), 0.08)

    def processes(self) -> None:
        regs = list(self.scope.regs)
        self.rng.shuffle(regs)
        while regs:
            count = self.rng.randint(1, min(3, len(regs)))
            group, regs = regs[:count], regs[count:]
            if self.scope.clocks and self.chance(0.45):
                self.items.append(self.ff_process(group))
            else:
                self.items.append(self.comb_process(group))
        if self.chance(0.15):
            text = string_node(str(self.gen.pick(('"v=%d"', '"x"', '"a\\tb %b"'))))
            args: list[Node] = [text]
            if "%" in str(text.spell["text"]) and self.scope.reads:
                args.append(ident(self.rng.choice(self.scope.reads).name))
            call = Node("TASK_CALL", *args, name="$display").flag("SYSTEM")
            self.items.append(Node("INITIAL", call))

    # -- nets --

    def drive_wires(self) -> None:
        wires = list(self.scope.wires)
        self.scope.wires = []
        self.rng.shuffle(wires)
        while wires:
            wire = wires.pop()
            roll = self.rng.random()
            if roll < 0.12 or (wire.width == 1 and roll < 0.25):
                self.items.append(self.gen.attrs_for(self.gate(wire), 0.05))
            elif wire.width > 1 and roll < 0.35:
                self.items.append(self.region(self.gen_for(wire)))
            elif self.scope.consts and roll < 0.5:
                self.items.append(self.region(self.gen_branch(wire)))
            else:
                more = [wires.pop()] if wires and self.chance(0.2) else []
                self.items.append(self.cont_assign([wire] + more))

    def cont_assign(self, wires: list[Sig]) -> Node:
        pairs = [Node("NET_ASSIGN", ident(w.name), self.gen.expr(self.scope, 3, w.width))
                 for w in wires]
        for pair, wire in zip(pairs, wires, strict=True):
            if wire.kind == "inout":
                pair.kids[1] = Node("TERNARY", self.gen.expr(self.scope, 1, 1),
                                    self.gen.expr(self.scope, 1, 1), number_node("1'bz"))
        if len(wires) == 2 and self.chance(0.4) and "inout" not in (w.kind for w in wires):
            lhs = Node("CONCAT", *(ident(w.name) for w in wires))
            pairs = [Node("NET_ASSIGN", lhs, self.gen.expr(self.scope, 3, 8))]
        delay: Node | None = None
        if self.chance(0.05):
            delay = Node("DELAY", num(self.rng.randint(1, 5)))
            delay.spell["parens"] = self.chance(0.5)
        return Node("CONT_ASSIGN", None, delay, *pairs)

    def scalar(self) -> Node:
        """A one-bit operand: a scalar or a bit of a vector."""
        sig = self.rng.choice(self.scope.reads)
        if sig.width == 1:
            return ident(sig.name)
        return Node("SELECT", ident(sig.name), num(min(sig.msb, sig.lsb)), None, sub="bit")

    def gate(self, wire: Sig) -> Node:
        roll = self.rng.random()
        if roll < 0.6:
            kind = str(self.gen.pick(GATES_N))
            terms = [self.scalar() for _ in range(self.rng.randint(2, 3))]
        elif roll < 0.85:
            kind = str(self.gen.pick(("buf", "not")))
            terms = [self.scalar()]
        else:
            kind = str(self.gen.pick(GATES_TRI))
            terms = [self.scalar(), self.scalar()]
        out: Node = ident(wire.name)
        if wire.width > 1:  # a gate drives one bit (Q1)
            out = Node("SELECT", out, num(self.rng.randint(0, wire.width - 1)), None, sub="bit")
        conns = [Node("CONNECTION", e) for e in [out] + terms]
        name = self.gen.fresh("g") if self.chance(0.5) else None
        return Node("GATE_DECL", None, None, Node("INSTANCE", None, *conns, name=name), sub=kind)

    def region(self, node: Node) -> Node:
        """G1: now and then inside generate … endgenerate (with its genvar declared there)."""
        genvars, self.pending_genvars = self.pending_genvars, []
        if not self.chance(0.35):
            self.decls.extend(genvars)
            return node
        return Node("GENERATE", *genvars, node)

    def genvar(self) -> str:
        name = self.gen.fresh("gv")
        self.pending_genvars.append(Node("GENVAR_DECL", Node("DECLARATOR", None, name=name)))
        return name

    def loop(self, var: str, count: int, body: Node) -> Node:
        """for (var = 0; var < count; var = var + 1) body."""
        init = Node("BLOCKING_ASSIGN", ident(var), None, num(0))
        cond = Node("BINARY", ident(var), num(count), sub="<")
        step = Node("BLOCKING_ASSIGN", ident(var), None,
                    Node("BINARY", ident(var), num(1), sub="+"))
        return Node("GEN_FOR", init, cond, step, body)

    def nested_gen_for(self, wire: Sig) -> Node:
        """G2 nested: w[i * half + j] over i < 2 and j < half, the inner loop in a block."""
        outer, inner = self.genvar(), self.genvar()
        half = wire.width // 2
        index = Node("BINARY", Node("BINARY", ident(outer), num(half), sub="*"), ident(inner),
                     sub="+")
        bit = Node("SELECT", ident(wire.name), index, None, sub="bit")
        assign = Node("CONT_ASSIGN", None, None,
                      Node("NET_ASSIGN", bit, self.gen.expr(self.scope, 2, 1)))
        body = Node("GEN_BLOCK", None, self.loop(inner, half, assign), name=self.gen.fresh("go"))
        return self.loop(outer, 2, body)

    def gen_for(self, wire: Sig) -> Node:
        if wire.width >= 4 and wire.width % 2 == 0 and min(wire.msb, wire.lsb) == 0 \
                and self.chance(0.25):
            return self.nested_gen_for(wire)
        var = self.genvar()
        lo = min(wire.msb, wire.lsb)
        bit = Node("SELECT", ident(wire.name), ident(var), None, sub="bit")
        wide = [s for s in self.scope.reads if s.width >= wire.width and s is not wire
                and min(s.msb, s.lsb) == 0]
        rhs = self.gen.expr(self.scope, 2, 1)
        if wide:
            src = Node("SELECT", ident(self.rng.choice(wide).name), ident(var), None, sub="bit")
            rhs = Node("BINARY", src, rhs, sub=str(self.gen.pick(("&", "|", "^", "+"))))
        body: Node = Node("CONT_ASSIGN", None, None, Node("NET_ASSIGN", bit, rhs))
        if self.chance(0.75):
            items: list[Node] = []
            if self.chance(0.3):
                local = self.gen.fresh("lw")
                items.append(Node("NET_DECL", None, None, None,
                                  Node("DECLARATOR", self.gen.expr(self.scope, 1, 1), name=local),
                                  sub="wire"))
                body.kid(2).kids[1] = Node("BINARY", body.kid(2).kid(1), ident(local), sub="^")
            items.append(body)
            name = self.gen.fresh("gb") if self.chance(0.7) else None
            body = Node("GEN_BLOCK", None, *items, name=name)
        init = Node("BLOCKING_ASSIGN", ident(var), None, num(lo))
        cond = Node("BINARY", ident(var), num(lo + wire.width), sub="<")
        step = Node("BLOCKING_ASSIGN", ident(var), None,
                    Node("BINARY", ident(var), num(1), sub="+"))
        return Node("GEN_FOR", init, cond, step, body)

    def gen_item(self, wire: Sig) -> Node:
        """A branch of an if/case generate: an assign, bare or in a block."""
        item = self.cont_assign([wire])
        if self.chance(0.5):
            name = self.gen.fresh("gs") if self.chance(0.6) else None
            return Node("GEN_BLOCK", None, item, name=name)
        return item

    def gen_branch(self, wire: Sig) -> Node:
        const = self.rng.choice(self.scope.consts)
        if self.chance(0.5):
            items = []
            values = list(range(0, 8))
            self.rng.shuffle(values)
            for _ in range(self.rng.randint(1, 2)):
                labels = [num(values.pop()) for _ in range(self.rng.randint(1, 2))]
                items.append(Node("CASE_ITEM", self.gen_item(wire), *labels))
            if self.chance(0.7):
                items.append(self.gen.default_item(self.gen_item(wire)))
            node = Node("GEN_CASE", ident(const.name), *items)
            return node
        cond = Node("BINARY", ident(const.name), num(self.rng.randint(0, 6)),
                    sub=str(self.gen.pick((">", "==", "<", "!="))))
        other: Node | None = None
        roll = self.rng.random()
        if roll < 0.4:
            other = self.gen_item(wire)
        elif roll < 0.55:
            other = self.gen_branch(wire) if self.chance(0.5) else Node("NULL_STMT")
        return Node("GEN_IF", cond, self.gen_item(wire), other)

    # -- instances --

    def instantiation(self) -> Node:
        mod = self.rng.choice(list(self.earlier))
        values = {name: value for name, value in mod.params}
        params: Node | None = None
        if mod.params and self.chance(0.6):
            values = {name: self.rng.randint(2, 6) for name, _ in mod.params}
            if self.chance(0.5):
                conns = [Node("CONNECTION", num(values[n])) for n, _ in mod.params]
            else:
                conns = [Node("CONNECTION", num(values[n]), name=n) for n, _ in mod.params]
            params = Node("LIST", *conns)
        instances = [self.instance(mod, values) for _ in range(self.rng.randint(1, 2))]
        if mod.ports and self.chance(0.08):
            instances.append(self.instance_array(mod, values))
        elif mod.params and params is None and self.chance(0.2):
            name, _ = self.rng.choice(mod.params)
            target = Node("HIER_NAME", ident(instances[0].name or ""), ident(name))
            self.defparams.append(Node("DEFPARAM", target, num(self.rng.randint(2, 6))))
        return self.gen.attrs_for(Node("INSTANTIATION", params, *instances, name=mod.name), 0.08)

    def instance_array(self, mod: ModInfo, values: dict[str, int]) -> Node:
        """u [1:0] (…): inputs connected, outputs left open."""
        node = self.instance(mod, values)
        node.kids[0] = rng_node(self.rng.randint(1, 2), 0)
        for conn, port in zip(node.kids[1:], mod.ports, strict=True):
            if conn is not None and port.direction != "input":
                conn.kids[0] = None
        return node

    def instance(self, mod: ModInfo, values: dict[str, int]) -> Node:
        named = self.chance(0.6)
        conns = []
        for port in mod.ports:
            width = values[port.param] if port.param is not None else port.width
            expr: Node | None
            if port.direction == "input":
                expr = self.gen.expr(self.scope, 2, width)
                while yosys_cell_trap(expr):
                    expr = self.gen.expr(self.scope, 2, width)
            elif self.chance(0.2):
                expr = None
            else:
                sig = Sig(self.gen.fresh("iw"), width - 1, 0)
                rng = rng_node(sig.msb, sig.lsb) if width > 1 else None
                self.decls.append(Node("NET_DECL", None, rng, None,
                                       Node("DECLARATOR", None, name=sig.name), sub="wire"))
                expr = ident(sig.name)
            conns.append(Node("CONNECTION", expr, name=port.name if named else None))
        return Node("INSTANCE", None, *conns, name=self.gen.fresh("u"))

    # -- the module --

    def build(self) -> Node:
        params, ports, body_ports = self.header()
        self.declarations()
        for _ in range(self.rng.randint(0, 2)):
            self.subprograms.append(self.function())
        if self.scope.regs and self.chance(0.3):
            self.subprograms.append(self.task())
        if self.chance(0.08):
            name = self.gen.fresh("tn")
            self.subprograms.append(Node("TASK_DECL", Node("LIST"), Node("LIST"),
                                         Node("SEQ_BLOCK", None), name=name))
            self.scope.tasks.append(Subprogram(name, []))
        if self.chance(0.2):
            self.subprograms.append(self.constant_function())
        if self.earlier and self.chance(0.7):
            for _ in range(self.rng.randint(1, 2)):
                self.items.append(self.instantiation())
        self.drive_wires()
        self.processes()
        self.rng.shuffle(self.items)
        self.items.extend(self.defparams)  # after the instances they name
        items = body_ports + self.decls + self.subprograms + self.after_subprograms + self.items
        node = Node("MODULE", params, ports, *items, name=self.name)
        node.flag("MACROMODULE", int(self.chance(0.01)))  # M8 (Yosys 0.55 rejects it)
        self.gen.mods.append(ModInfo(self.name, self.header_params, self.ports))
        return self.gen.attrs_for(node, 0.1)


def generate(seed: int) -> list[Node]:
    """The unit for seed: optional directives, then one to four modules."""
    gen = Gen(seed)
    items: list[Node] = []
    if gen.chance(0.15):
        text = str(gen.pick(("1ns/1ps", "1ns / 1ps", "10ns/1ns")))
        items.append(Node("DIRECTIVE", sub="timescale", name=text))
    if gen.chance(0.05):
        items.append(Node("DIRECTIVE", sub="resetall"))  # L20
    if gen.chance(0.1):
        # L19: wire only. Yosys 0.55 takes only wire and none, and none would reject the
        # header ports declared without a net type.
        items.append(Node("DIRECTIVE", sub="default_nettype", name="wire"))
    if gen.chance(0.1):
        items.append(empty_module(gen, f"e{seed}"))
    for idx in range(gen.rng.randint(1, 4)):
        items.append(ModuleGen(gen, f"m{seed}_{idx}", list(gen.mods)).build())
    return items


def empty_module(gen: Gen, name: str) -> Node:
    """M5: `module m;` or `module m ();`, empty or with one wire; instantiated as `m u ();`."""
    ports = Node("LIST") if gen.chance(0.5) else None
    body = []
    if gen.chance(0.5):
        body.append(Node("NET_DECL", None, None, None, Node("DECLARATOR", None, name="unused"),
                         sub="wire"))
    gen.mods.append(ModInfo(name, [], []))
    return Node("MODULE", None, ports, *body, name=name)


# --- mutation -------------------------------------------------------------------------------


def mutate(text: str, tokens: Sequence[tuple[int, int]], rng: random.Random) -> tuple[str, str]:
    """Drops, duplicates or swaps (with the next) one token; the text and what was done. A swap
    of two equal spellings would be a no-op, so it is drawn again."""
    for _ in range(MUTATE_TRIES):
        mutated, note = mutate_once(text, tokens, rng)
        if mutated != text:
            break
    return mutated, note


def mutate_once(text: str, tokens: Sequence[tuple[int, int]],
                rng: random.Random) -> tuple[str, str]:
    """One draw of mutate."""
    idx = rng.randrange(len(tokens) - 1)
    start, end = tokens[idx]
    op = rng.choice(("drop", "dup", "swap"))
    if op == "drop":
        return text[:start] + text[end:], f"drop {text[start:end]!r} at byte {start}"
    if op == "dup":
        return (text[:end] + " " + text[start:end] + text[end:],
                f"duplicate {text[start:end]!r} at byte {start}")
    nstart, nend = tokens[idx + 1]
    swapped = text[:start] + text[nstart:nend] + text[end:nstart] + text[start:end] + text[nend:]
    return swapped, f"swap {text[start:end]!r} and {text[nstart:nend]!r} at byte {start}"


# --- the command line -----------------------------------------------------------------------


@dataclass
class Generated:
    text: str
    ast: str
    note: str


def render(seed: int, fname: str, mutated: bool = False) -> Generated:
    """The Verilog text, the expected dump (empty when mutated) and the mutation note."""
    items = generate(seed)
    out = print_unit(items, random.Random(seed * 7919 + 1))
    if mutated:
        text, note = mutate(out.text(), out.tokens, random.Random(seed * 104729 + 3))
        return Generated(text, "", note)
    return Generated(out.text(), dump_unit(items, fname), "")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--out", required=True, help="the Verilog file to write (F.v)")
    parser.add_argument("--mutate", action="store_true", help="mutate one token, no F.ast")
    parser.add_argument("--no-ast", action="store_true", help="do not write F.ast")
    args = parser.parse_args(argv)
    out = Path(args.out)
    result = render(args.seed, out.name, args.mutate)
    try:
        out.write_text(result.text, encoding="utf-8")
        if not args.mutate and not args.no_ast:
            header = f"# generated: verilog_gen.py --seed {args.seed}\n"
            out.with_suffix(".ast").write_text(header + result.ast, encoding="utf-8")
    except OSError as exc:
        print(f"verilog_gen: {exc}", file=sys.stderr)
        return 2
    if result.note:
        print(result.note)
    return 0


if __name__ == "__main__":
    sys.exit(main())
