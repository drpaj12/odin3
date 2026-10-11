"""pp-fuzz: model-based fuzzing of the Odin III Verilog-2005 preprocessor (spec 2B §10.5).

Usage::

    pp-fuzz run   --odin3 BIN [--seed S] [--cases N] [--first I] [--jobs J] [--keep DIR]
    pp-fuzz show  [--seed S] [--case I]
    pp-fuzz seeds OUT_DIR SOURCE...

``run`` generates N random projects from the seed (macros with up to four formals and
defaults, macro nesting up to six, conditionals up to four deep, includes in a temporary tree
with two include directories, pass-through directives, ``translate_off`` regions, comments and
strings), computes each stream's expected output with the reference expander below, runs
``odin3 --diag-paths resolved --project p.o3proj -p "preprocess --out OUT --segments"`` and
checks, per stream: the tokens equal the reference's, each token byte is spelled where the
reference says (the segments' ``spell_*`` fields chased to a file), every output byte with a
non-zero loc equals the source byte its spelling names, every loc-0 byte is a blank segment of
its own (§7.8 checked from the outside), and ``files-read.txt`` equals the reference's
first-read order.  ``show`` prints one generated project.  ``seeds`` writes a libFuzzer seed
corpus for ``tests/fuzz/fuzz_pp.c``: each source file prefixed with a zero options byte.

The reference expander is written from spec 2B §7 alone (it is not a port of the C): it works
on lists of (character, origin) pairs, origin = (file, byte offset) or None for an inserted
byte, so it predicts both the text and where every byte is spelled.  It covers what the
generator produces; anything else raises ``RefError``.

Exit 0 every case passed, 1 a case failed, 2 usage error.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

Origin = tuple[str, int] | None
Ch = tuple[str, Origin]
Text = list[Ch]

# ---- the reference expander (spec 2B §7) -----------------------------------------------------

CMDLINE = "<command line>"
BLANKS = " \t\r\n"
ID_START = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
ID_CHAR = ID_START | frozenset("0123456789$")
CONDS = frozenset({"ifdef", "ifndef", "elsif", "else", "endif"})
PASS_THROUGH = frozenset({"default_nettype", "timescale", "celldefine", "endcelldefine",
                          "resetall", "begin_keywords", "end_keywords", "unconnected_drive",
                          "nounconnected_drive", "pragma"})
CONSUMED = CONDS | {"define", "undef", "include", "line"}
_PRAGMA = re.compile(r"(?:synopsys|synthesis|pragma) translate_(off|on)")


class RefError(Exception):
    """The input left what the reference models (a generator bug, never an odin3 finding)."""


@dataclass
class Macro:
    formals: list[tuple[str, Text | None]] | None  # None: no formal list; []: `()`
    body: Text


def chars_of(text: str, where: str, base: int = 0) -> Text:
    return [(c, (where, base + i)) for i, c in enumerate(text)]


def text_of(s: Text, i: int, j: int) -> str:
    return "".join(c for c, _ in s[i:j])


def peek(s: Text, i: int) -> str:
    return s[i][0] if 0 <= i < len(s) else ""


def skip(s: Text, i: int, chars: str) -> int:
    while i < len(s) and s[i][0] in chars:
        i += 1
    return i


def ident_end(s: Text, i: int) -> int:
    if peek(s, i) not in ID_START:
        return i
    while i < len(s) and s[i][0] in ID_CHAR:
        i += 1
    return i


def string_end(s: Text, i: int) -> int:
    """After the string at s[i] == '"': its closing quote, or (unterminated) its newline."""
    i += 1
    while i < len(s) and s[i][0] not in '"\n':
        i += 2 if s[i][0] == "\\" else 1
    return i + 1 if peek(s, i) == '"' else min(i, len(s))


def comment_end(s: Text, i: int) -> int:
    """After the comment at s[i]: a // comment ends before its newline."""
    if peek(s, i + 1) == "/":
        while i < len(s) and s[i][0] != "\n":
            i += 1
        return i
    j = i + 2
    while j < len(s) and not (s[j][0] == "*" and peek(s, j + 1) == "/"):
        j += 1
    if j >= len(s):
        raise RefError("unterminated comment")
    return j + 2


def escaped_end(s: Text, i: int) -> int:
    while i < len(s) and s[i][0] not in BLANKS:
        i += 1
    return i


def pragma(comment: str) -> str | None:
    """'off' or 'on' for a translate_off/translate_on comment (§7.6), else None."""
    inner = comment[2:-2] if comment.startswith("/*") else comment[2:]
    m = _PRAGMA.fullmatch(inner.strip(BLANKS))
    return m.group(1) if m else None


def is_comment(s: Text, i: int) -> bool:
    return peek(s, i) == "/" and peek(s, i + 1) in ("/", "*")


def trim(part: Text) -> Text:
    i, j = 0, len(part)
    while i < j and part[i][0] in BLANKS:
        i += 1
    while j > i and part[j - 1][0] in BLANKS:
        j -= 1
    return part[i:j]


def collect(s: Text, k: int) -> tuple[Text, int]:
    """One actual or default from s[k] to the next top-level ',' or ')' (PP-5)."""
    depth, part = 0, []
    while k < len(s):
        c = s[k][0]
        if c == '"':
            e = string_end(s, k)
            part += s[k:e]
            k = e
        elif is_comment(s, k):
            k = comment_end(s, k)
            part.append((" ", None))
        elif depth == 0 and c in ",)":
            return part, k
        else:
            depth += 1 if c in "([{" else -1 if c in ")]}" else 0
            part.append(s[k])
            k += 1
    raise RefError("unterminated arguments")


def line_end(s: Text, k: int) -> bool:
    return peek(s, k) == "\n" or text_of(s, k, k + 2) == "\r\n"


def define_body(s: Text, k: int) -> tuple[Text, int]:
    """The text of a `define to the end of its logical line (LF or CRLF; a lone CR is a
    character) or a // comment; a continuation drops the '\\' and keeps its line end. Trailing
    blanks are trimmed, but a body ending in an escaped identifier keeps the space or tab that
    ended it (the 2B Task 6 ruling: the identifier never swallows what follows a use)."""
    body: Text = []
    escaped: tuple[int, Ch | None] = (-1, None)  # (len(body) after the last escaped
    # identifier, the blank that ended it)
    while k < len(s) and not line_end(s, k):
        c, nxt = s[k][0], peek(s, k + 1)
        if c == "\\" and line_end(s, k + 1):
            e = k + (3 if nxt == "\r" else 2)
            body += s[k + 1:e]
        elif c == "/" and nxt == "/":
            k = comment_end(s, k)
            k -= 1 if text_of(s, k - 1, k) == "\r" else 0
            break
        else:
            e = (string_end(s, k) if c == '"' else comment_end(s, k) if is_comment(s, k) else
                 escaped_end(s, k) if c == "\\" else k + 1)
            body += s[k:e]
            if c == "\\":
                escaped = (len(body), s[e] if peek(s, e) in (" ", "\t") else None)
        k = e
    end = len(body)
    while end and body[end - 1][0] in " \t\r":
        end -= 1
    blank = escaped[1] if escaped[0] == end else None
    return body[:end] + ([blank] if blank is not None else []), k


@dataclass
class Scan:
    """One file being scanned. Expansions are pushed back in place (substitute, then rescan):
    s[i:macro_end] is macro text, where a consumed directive is an error; a use's actuals may run
    from macro text on into the file text after it (I11)."""

    s: Text
    chain: list[str]
    i: int = 0
    macro_end: int = 0
    conds: list[list[bool]] = field(default_factory=list)  # [taken, seen_true, else_seen]

    def live(self) -> bool:
        return all(f[0] for f in self.conds)


class Reference:
    """One preprocessor (§7.1): the macro table and files read persist from stream to stream."""

    def __init__(self, defines: Sequence[tuple[str, str | None]], incdirs: Sequence[str]):
        self.macros: dict[str, Macro] = {}
        self.incdirs = list(incdirs)
        self.files_read: list[str] = []
        self.cmdline = ""
        for name, value in defines:  # the <command line> buffer: no continuations, no comments
            head = f"`define {name}" + ("" if value is None else " ")
            body = chars_of(value or "", CMDLINE, len(self.cmdline) + len(head))
            self.macros[name] = Macro(None, trim(body))
            self.cmdline += head + (value or "") + "\n"
        self.out: Text = []
        self.gap = False

    def stream(self, path: str) -> Text:
        self.out, self.gap = [], False
        self.file(path, [])
        return self.out

    def emit(self, chars: Text) -> None:
        """Appends chars; PP-3's protective space after a gap between two non-blanks."""
        for ch in chars:
            if self.gap and self.out and self.out[-1][0] not in BLANKS and ch[0] not in BLANKS:
                self.out.append((" ", None))
            self.gap = False
            self.out.append(ch)

    def file(self, path: str, stack: list[str]) -> None:
        if path not in self.files_read:
            self.files_read.append(path)
        st = Scan(chars_of(Path(path).read_bytes().decode("latin-1"), path), [*stack, path])
        while st.i < len(st.s):
            if st.live() or st.i < st.macro_end:
                self.active(st)
            else:
                st.i, self.gap = self.skipped(st, st.i), True
        if st.conds:
            raise RefError(f"{path}: unterminated `ifdef")

    def active(self, st: Scan) -> None:
        s, i = st.s, st.i
        c, j = s[i][0], i + 1
        if is_comment(s, i) and i >= st.macro_end:
            j = comment_end(s, i)
            if pragma(text_of(s, i, j)) == "off":
                st.i, self.gap = self.translate_off(s, j), True
                return
        elif c == '"':
            j = string_end(s, i)
        elif c == "\\":
            j = escaped_end(s, i)
        elif c == "`":
            self.directive(st)
            return
        self.emit(s[i:j])
        st.i = j

    def skipped(self, st: Scan, i: int) -> int:
        """PP-4: a false branch tracks comments, strings and nested conditionals only."""
        s = st.s
        if is_comment(s, i):
            return comment_end(s, i)
        if s[i][0] == '"':
            return string_end(s, i)
        j = ident_end(s, i + 1)
        if s[i][0] == "`" and text_of(s, i + 1, j) in CONDS:
            return self.cond(s, j, text_of(s, i + 1, j), st.conds)
        return i + 1

    def translate_off(self, s: Text, k: int) -> int:
        while k < len(s):
            if is_comment(s, k):
                e = comment_end(s, k)
                if pragma(text_of(s, k, e)) == "on":
                    return e
                k = e
            else:
                k += 1
        raise RefError("unterminated translate_off")

    def directive(self, st: Scan) -> None:
        s, i = st.s, st.i
        j = ident_end(s, i + 1)
        name = text_of(s, i + 1, j)
        if name in PASS_THROUGH:
            self.emit(s[i:j])
            st.i = j
        elif name not in CONSUMED:
            self.use(st, j, name)
        elif i < st.macro_end:
            raise RefError("directive inside macro text")
        elif name in CONDS:
            st.i, self.gap = self.cond(s, j, name, st.conds), True
        elif name == "define":
            st.i, self.gap = self.define(s, j), True
        elif name == "undef":
            k = ident_end(s, skip(s, j, " \t"))
            self.macros.pop(text_of(s, skip(s, j, " \t"), k), None)
            st.i, self.gap = k, True
        elif name == "line":
            m = re.match(r'[ \t]+[0-9]+[ \t]+"[^"\n]*"[ \t]+[0-2]', text_of(s, j, j + 300))
            if m is None:
                raise RefError("bad `line")
            st.i, self.gap = j + m.end(), True
        else:
            st.i = self.include(s, j, st.chain)

    def cond(self, s: Text, j: int, name: str, conds: list[list[bool]]) -> int:
        k = ident_end(s, skip(s, j, " \t")) if name not in ("else", "endif") else j
        defined = text_of(s, skip(s, j, " \t"), k) in self.macros
        if name in ("ifdef", "ifndef"):
            live = all(f[0] for f in conds)
            taken = live and defined == (name == "ifdef")
            conds.append([taken, taken or not live, False])
        elif not conds or (name != "endif" and conds[-1][2]):
            raise RefError(f"`{name} without `ifdef")
        elif name == "endif":
            conds.pop()
        else:
            frame = conds[-1]
            frame[0] = not frame[1] and (name == "else" or defined)
            frame[1], frame[2] = frame[1] or frame[0], name == "else"
        return k

    def define(self, s: Text, j: int) -> int:
        k = skip(s, j, " \t")
        e = ident_end(s, k)
        if e == k:
            raise RefError("`define without a name")
        name, formals = text_of(s, k, e), None
        if peek(s, e) == "(":
            formals, e = self.formals(s, e + 1)
        body, k = define_body(s, skip(s, e, " \t"))
        self.macros[name] = Macro(formals, body)
        return k

    def formals(self, s: Text, k: int) -> tuple[list[tuple[str, Text | None]], int]:
        out: list[tuple[str, Text | None]] = []
        k = skip(s, k, " \t")
        if peek(s, k) == ")":
            return out, k + 1
        while True:
            k = skip(s, k, " \t")
            e = ident_end(s, k)
            name, k, default = text_of(s, k, e), skip(s, e, " \t"), None
            if peek(s, k) == "=":
                part, k = collect(s, k + 1)
                default = trim(part)
            out.append((name, default))
            if peek(s, k) == ")":
                return out, k + 1
            k += 1

    def include(self, s: Text, j: int, chain: list[str]) -> int:
        k = skip(s, j, " \t")
        e = string_end(s, k)
        name = text_of(s, k + 1, e - 1)
        dirs = [os.path.dirname(chain[-1]), *self.incdirs]
        found = [p for p in (os.path.normpath(os.path.join(d, name)) for d in dirs)
                 if os.path.isfile(p)]
        if peek(s, k) != '"' or not found or found[0] in chain:
            raise RefError(f"bad `include {name}")
        self.gap = True
        self.file(found[0], chain)
        self.gap = True
        return e

    def use(self, st: Scan, j: int, name: str) -> None:
        """`name at st.i (PP-5, PP-6): its actuals as collected are substituted into the body,
        which replaces the use in st.s as macro text and is scanned next (rescan)."""
        m = self.macros.get(name)
        if m is None:
            raise RefError(f"undefined macro {name}")
        e, binding = j, {}
        if m.formals is not None:
            k = skip(st.s, j, BLANKS)
            if peek(st.s, k) != "(":
                raise RefError(f"macro {name} takes arguments")
            parts, e = self.actuals(st, k + 1)
            binding = bind(name, m.formals, parts)
        text = substitute(m.body, binding)
        st.s[st.i:e] = text
        st.macro_end = st.i + len(text) + max(0, st.macro_end - e)

    def actuals(self, st: Scan, k: int) -> tuple[list[Text], int]:
        """The actuals after '(' at k-1 to the matching ')': in file text, conditionals are
        processed as usual and comments become one loc-0 space (PP-5)."""
        s, depth = st.s, 0
        parts: list[Text] = []
        part: Text = []
        while k < len(s):
            c, in_file = s[k][0], k >= st.macro_end
            name = text_of(s, k + 1, ident_end(s, k + 1)) if c == "`" else ""
            if in_file and (not st.live() or name in CONDS):
                k = self.skipped(st, k) if not st.live() else self.cond(
                    s, k + 1 + len(name), name, st.conds)
            elif in_file and is_comment(s, k):
                k = comment_end(s, k)
                part.append((" ", None))
            elif c == '"':
                part += s[k:string_end(s, k)]
                k = string_end(s, k)
            elif depth == 0 and c in ",)":
                parts.append(trim(part))
                part, k = [], k + 1
                if c == ")":
                    return parts, k
            else:
                depth += 1 if c in "([{" else -1 if c in ")]}" else 0
                part.append(s[k])
                k += 1
        raise RefError("unterminated arguments")


def substitute(body: Text, binding: dict[str, Text]) -> Text:
    """The body with every formal (an IDENT outside strings and escaped identifiers, not after a
    backtick) replaced by its actual as collected, and every comment by one loc-0 space."""
    out: Text = []
    k = 0
    while k < len(body):
        c, e = body[k][0], k + 1
        if c == '"':
            e = string_end(body, k)
        elif is_comment(body, k):
            out.append((" ", None))
            k = comment_end(body, k)
            continue
        elif c == "\\":
            e = escaped_end(body, k)
        elif c == "`":
            e = max(ident_end(body, k + 1), k + 1)
        elif c in ID_START:
            e = ident_end(body, k)
            word = text_of(body, k, e)
            if word in binding:
                out += binding[word]
                k = e
                continue
        out += body[k:e]
        k = e
    return out


def bind(name: str, formals: list[tuple[str, Text | None]], parts: list[Text]) -> dict[str, Text]:
    """Actuals to formals (PP-5): `F()` is zero actuals for an empty formal list, an empty actual
    takes its default when there is one, missing trailing actuals take theirs."""
    if not formals:
        if parts not in ([], [[]]):
            raise RefError(f"macro {name} takes no arguments")
        return {}
    if len(parts) > len(formals):
        raise RefError(f"too many arguments for {name}")
    out: dict[str, Text] = {}
    for n, (formal, default) in enumerate(formals):
        given = parts[n] if n < len(parts) else None
        if not given and default is not None:
            given = default
        if given is None:
            raise RefError(f"macro {name}: no actual for {formal}")
        out[formal] = given
    return out


# ---- the generator ---------------------------------------------------------------------------

FORMALS = ("pa", "qb", "rc", "sd")  # never inside a based number (8'hff) or another name
IDENTS = ("w", "x_y", "pa_1", "qbx", "k$2", "data", "v0")
NUMBERS = ("0", "1", "42", "8'hff", "4'b1010", "3")
OPS = ("+", "-", "&", "|", "^", "~", "*", "==", "<", ">", "?", ":", ";", ".", "#", "!", "%")
FLAGS = ("F0", "F1", "F2", "F3")
DIRS = ("src", "inc", "inc2")
HEADERS = ("h0.vh", "h1.vh", "h2.vh", "h3.vh")
PASS_LINES = ("`timescale 1ns/1ps", "`default_nettype none", "`celldefine", "`endcelldefine",
              "`resetall", "`default_nettype wire", "`nounconnected_drive")
PRAGMAS = (("// synopsys translate_off", "// synopsys translate_on"),
           ("/* synthesis translate_off */", "/* synthesis translate_on */"),
           ("// pragma translate_off", "//  pragma translate_on"))
MAX_LEVEL = 4  # macro nesting through bodies; uses nest two more in file actuals (<= 6)
MAX_COND = 4


@dataclass
class MacroSpec:
    name: str
    formals: list[bool] | None  # has-default per formal; None: object-like; []: `()`
    level: int
    tail: tuple[int, bool] | None = None  # the body ends in `Mj, or `Mj( when open (I11)


@dataclass
class Project:
    files: dict[str, str] = field(default_factory=dict)  # relative path -> text
    roots: list[str] = field(default_factory=list)
    defines: list[tuple[str, str | None]] = field(default_factory=list)
    incdirs: list[str] = field(default_factory=lambda: ["inc", "inc2"])

    def o3proj(self) -> str:
        lines = [f"incdir {d}" for d in self.incdirs]
        lines += [f"define {n}" + ("" if v is None else f"={v}") for n, v in self.defines]
        return "\n".join(lines + [f"file verilog {r}" for r in self.roots]) + "\n"


class Gen:
    """One random project: object-like, function-like and empty-list macros, each body using
    only lower-level macros (no recursion, nesting bounded), used only once defined."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.nl = "\r\n" if rng.random() < 0.15 else "\n"
        self.macros: list[MacroSpec] = []
        for i in range(rng.randint(1, 7)):
            r = rng.random()
            formals = (None if r < 0.3 else [] if r < 0.38 else
                       [rng.random() < 0.4 for _ in range(rng.randint(1, len(FORMALS)))])
            spec = MacroSpec(f"M{i}", formals, min(i + 1, MAX_LEVEL))
            callees = [j for j, m in enumerate(self.macros) if m.formals is not None and
                       m.tail is None and m.level < spec.level]
            if callees and rng.random() < 0.25:  # its uses supply the callee's actuals
                spec.tail = (rng.choice(callees), rng.random() < 0.4)
            self.macros.append(spec)
        self.defines: list[tuple[str, str | None]] = [("P0", rng.choice(("8", "(4+4)", "w_x",
                                                                         "1+2", "")))]
        if rng.random() < 0.5:  # a <command line> body using another
            self.defines.append(("P1", "x`P0"))
        self.defines += [(f, None) for f in FLAGS if rng.random() < 0.3]

    def pick(self, *options: str) -> str:
        return self.rng.choice(options)

    def string(self) -> str:
        return '"' + "".join(self.rng.choice((" pa ", ",", "`M0", "//", "/*", '\\"', "x y", ")"))
                             for _ in range(self.rng.randint(0, 3))) + '"'

    def objects(self, below: int | None = None) -> list[MacroSpec]:
        """Object-like macros (no tail) defined before macro below."""
        return [m for m in self.macros[:below] if m.formals is None and m.tail is None]

    def use(self, spec: MacroSpec, ctx: list[str] | None, nest: int) -> str:
        """`NAME with its actuals, then those of the macro its body ends in (I11). ctx: the
        formals of the body the use is in, None in file text."""
        text = "`" + spec.name + self.args(spec, ctx, nest)
        if spec.tail is not None:
            tail = self.args(self.macros[spec.tail[0]], ctx, nest)
            text += " " + tail.lstrip(" \n").removeprefix("(") if spec.tail[1] else tail
        return text

    def args(self, spec: MacroSpec, ctx: list[str] | None, nest: int) -> str:
        """The actuals of a use: enough for every formal without a default, each possibly empty,
        nested (parentheses, brackets, braces, strings, comments, newlines, uses)."""
        if spec.formals is None:
            return ""
        gap = self.pick("", " ", "\n  ") if ctx is None else self.pick("", " ")
        if not spec.formals:
            return f"{gap}()"
        need = max([1] + [n + 1 for n, has in enumerate(spec.formals) if not has])
        count = self.rng.randint(need, len(spec.formals))
        return f"{gap}(" + ",".join(self.actual(ctx, nest) for _ in range(count)) + ")"

    def actual(self, ctx: list[str] | None, nest: int) -> str:
        r, rng, names = self.rng.random(), self.rng, ctx or []
        if r < 0.15:
            text = ""
        elif r < 0.3 and names:
            text = rng.choice(names)
        elif r < 0.45:
            text = f"({self.atom(names)} + {self.atom(names)}, {self.atom(names)})"
        elif r < 0.55:
            text = self.pick("[w:0]", "{a, b}", "{2{x}}") + self.pick("", " - 1")
        elif r < 0.62:
            text = self.string()
        elif r < 0.7 and ctx is None:
            text = self.pick("/*c,)*/ w", "x // c, )" + self.nl + " + 1", "a" + self.nl + "+ b",
                             f"1 `ifdef {rng.choice(FLAGS)} + (2, 3) `else - 3 `endif",
                             f"[`ifndef {rng.choice(FLAGS)} 4 `elsif {rng.choice(FLAGS)} 5 `endif]")
        elif r < 0.85 and nest < 2 and ctx is None:  # an open tail's commas would split this
            text = self.use(rng.choice([m for m in self.macros if not (m.tail and m.tail[1])]),
                            None, nest + 1)
        else:
            text = self.atom(names)
        return self.pick("", " ") + text + self.pick("", " ", "  ")

    def atom(self, ctx: list[str]) -> str:
        return self.rng.choice([*IDENTS, *NUMBERS, *ctx])

    def body(self, i: int) -> str:
        spec, rng = self.macros[i], self.rng
        names = FORMALS[:len(spec.formals or [])]
        lower = [m for m in self.macros[:i] if m.level < spec.level]
        objects = [m for m in self.objects(i) if m.level < spec.level]
        pieces = []
        for _ in range(rng.randint(0, 6)):
            r = rng.random()
            if r < 0.3 and names:
                pieces.append(rng.choice(names))
            elif r < 0.5:
                pieces.append(rng.choice((*IDENTS, *NUMBERS, *OPS)))
            elif r < 0.65 and lower:
                pieces.append(self.use(rng.choice(lower), list(names), MAX_LEVEL))
            elif r < 0.7:
                pieces.append(self.string())
            elif r < 0.75:
                pieces.append(self.pick("/* c pa */", "/**/", "/* `M0 // */"))
            elif r < 0.8:
                pieces.append("\\" + rng.choice((*names, *IDENTS)) + "+q ")
            elif r < 0.85 and objects:
                pieces.append(rng.choice(IDENTS) + "`" + rng.choice(objects).name)
            elif r < 0.88:
                pieces.append(self.pick("`celldefine", "`resetall"))
            else:
                pieces.append("(" + " ".join(rng.choice((*names, *IDENTS, "+")) for _ in
                                             range(rng.randint(0, 3))) + ")")
        if spec.tail is not None:  # the uses of this macro supply the callee's actuals (I11)
            pieces.append("`" + self.macros[spec.tail[0]].name + ("(" if spec.tail[1] else ""))
        seps = [self.pick(" ", "  ", "\t", " \\" + self.nl + "  ") for _ in pieces]
        return "".join(p + s for p, s in zip(pieces, seps, strict=True)).rstrip(" \t")

    def define(self, i: int) -> str:
        spec = self.macros[i]
        head = f"`define {spec.name}"
        if spec.formals is not None:
            formals = []
            for n, has in enumerate(spec.formals):
                default = ""
                if has:
                    objects = [m.name for m in self.objects(i)]
                    default = self.pick("=3", " = (1, 2)", '="d,s)"', "= w ",
                                        *[f"=`{o}" for o in objects])
                formals.append(self.pick("", " ") + FORMALS[n] + default)
            head += "(" + ",".join(formals) + ")"
        tail = self.pick("", "", " // note `M0 \\")
        return head + " " + self.body(i) + tail + self.nl

    def expr(self) -> str:
        """A line of file text using the macros (all defined before any item)."""
        parts = []
        for _ in range(self.rng.randint(1, 4)):
            r = self.rng.random()
            if r < 0.5:
                parts.append(self.use(self.rng.choice(self.macros), None, 0))
            elif r < 0.6:
                parts.append(self.rng.choice([f"`{n}" for n, _ in self.defines[:2] if n[0] == "P"]))
            elif r < 0.7:
                parts.append(self.string())
            elif r < 0.75:
                objects = self.objects()
                parts.append("x" + ("`" + objects[0].name if objects else ""))
            else:
                parts.append(self.atom([]))
        return " + ".join(parts)

    def cond_block(self, depth: int, header: int, budget: int) -> str:
        """`ifdef/`ifndef … [`elsif …]* [`else …] `endif, on lines or mid-line (PP-3)."""
        rng, sep = self.rng, self.pick(self.nl, " ")
        out = f"`{self.pick('ifdef', 'ifndef')} {rng.choice((*FLAGS, 'M0', 'P0'))}{sep}"
        out += self.items(depth + 1, header, budget // 2)
        for _ in range(rng.randint(0, 2)):
            out += f"`elsif {rng.choice(FLAGS)}{sep}" + self.items(depth + 1, header, budget // 3)
        if rng.random() < 0.5:
            out += f"`else{sep}" + self.items(depth + 1, header, budget // 3)
        return out + "`endif" + self.pick(self.nl, " ")

    def item(self, depth: int, header: int, budget: int) -> str:
        rng, nl = self.rng, self.nl
        r = rng.random()
        if r < 0.3:
            return f"wire {rng.choice(IDENTS)} = {self.expr()};{nl}"
        if r < 0.42 and depth < MAX_COND:
            return self.cond_block(depth, header, budget)
        if r < 0.5:
            flag = rng.choice(FLAGS)
            return self.pick(f"`define {flag}{nl}", f"`define {flag} 1 // f{nl}", f"`undef {flag}"
                             + nl, f"`undef {flag} ")
        if r < 0.55:  # a redefinition, possibly after an `undef (the table drops the name)
            i = rng.randrange(len(self.macros))
            return self.pick("", f"`undef M{i}{nl}", f"`undef M{i} ") + self.define(i)
        if r < 0.62 and header + 1 < len(HEADERS):
            name = rng.choice(HEADERS[header + 1:])
            return self.pick(f'`include "{name}"{nl}', f'assign q = `include "{name}" ;{nl}')
        if r < 0.67:
            return rng.choice(PASS_LINES) + nl
        if r < 0.72:
            return self.pick(f"// c `M0 \"q{nl}", f"/* a{nl} `ifdef */ b{nl}") + nl
        if r < 0.77:
            off, on = rng.choice(PRAGMAS)
            return f"{off}{nl}  junk `UNDEF_X ( \"{nl}{on}{nl}"
        if r < 0.8:
            return f'`line 12 "x.v" 1{nl}'
        if r < 0.84:
            return f"wire \\bus`M0+ ;{nl}"
        if r < 0.9:
            return f"`ifdef NEVER_SET `UNDEF_Y ( `include \"none.vh\" `endif{nl}"
        flag = rng.choice(FLAGS)
        if r < 0.95:
            return f"assign {rng.choice(IDENTS)} = 1 `ifdef {flag} + 2 `else + 3 `endif;{nl}"
        use = self.use(rng.choice(self.macros), None, 0)  # directives glued to an expansion
        return f"wire g = {use} `ifdef {flag}+1`else-1`endif{use};{nl}"  # (after it, a body's
        # final escaped identifier would swallow the directive)

    def items(self, depth: int, header: int, budget: int) -> str:
        return "".join(self.item(depth, header, budget) for _ in range(self.rng.randint(0, budget)))

    def project(self) -> Project:
        proj = Project(defines=list(self.defines))
        for n in range(len(HEADERS)):  # header n includes only headers after it: no cycle
            dirs = self.rng.sample(DIRS, self.rng.randint(1, 2))  # one of them an incdir, so
            if dirs == ["src"]:  # every includer finds it
                dirs.append(self.rng.choice(proj.incdirs))
            for d in dirs:
                text = self.items(1, n, 5)
                bare = text.rstrip()  # a header ending without a newline (PP-3), unless a
                keep = self.rng.random() < 0.8 or bare.endswith("\\")  # continuation needs it
                proj.files[f"{d}/{HEADERS[n]}"] = text if keep else bare
        for r in range(self.rng.randint(1, 2)):
            head = "".join(self.define(i) for i in range(len(self.macros))) if r == 0 else ""
            proj.files[f"src/top{r}.v"] = head + self.items(0, -1, 14) + "endmodule" + self.nl
            proj.roots.append(f"src/top{r}.v")
        return proj


def generate(seed: int, case: int) -> Project:
    return Gen(random.Random(f"pp-fuzz/{seed}/{case}")).project()


# ---- checking odin3's streams ----------------------------------------------------------------

ODIN3_TIMEOUT = 60  # seconds per case (a sanitizer build preprocesses one in milliseconds)
SHOW_PROBLEMS = 6


def tokens(s: Text) -> list[tuple[str, tuple[Origin, ...]]]:
    """Verilog-ish tokens (identifiers and numbers, strings, comments, escaped identifiers,
    `NAME, single characters), blanks dropped; each with the origins of its bytes."""
    out: list[tuple[str, tuple[Origin, ...]]] = []
    k = 0
    while k < len(s):
        c, e = s[k][0], k + 1
        if c in BLANKS:
            k += 1
            continue
        if is_comment(s, k):
            try:
                e = comment_end(s, k)
            except RefError:
                e = len(s)
        elif c == '"':
            e = string_end(s, k)
        elif c == "\\":
            e = escaped_end(s, k)
        elif c == "`":
            e = max(ident_end(s, k + 1), k + 1)
        elif c in ID_CHAR:
            e = skip(s, k, "".join(ID_CHAR) + "'")
        out.append((text_of(s, k, e), tuple(o for _, o in s[k:e])))
        k = e
    return out


class Sources:
    """The bytes every origin names: the files on disk and the <command line> buffer."""

    def __init__(self, cmdline: str):
        self.text: dict[str, str] = {CMDLINE: cmdline}
        self.lines: dict[str, list[int]] = {}

    def get(self, path: str) -> str:
        if path not in self.text:
            self.text[path] = Path(path).read_bytes().decode("latin-1")
        return self.text[path]

    def offset(self, path: str, line: int, col: int) -> int:
        if path not in self.lines:
            text = self.get(path)
            self.lines[path] = [0] + [i + 1 for i, c in enumerate(text) if c == "\n"]
        return self.lines[path][line - 1] + col - 1


def stream_origins(text: str, segs: list[dict[str, object]], src: Sources,
                   probs: list[str]) -> list[Origin]:
    """Every output byte's spelling from the segment map (segment byte k is spelled k bytes
    after the segment's spell_file:spell_line:spell_col); checks §7.8 from the outside."""
    origins: list[Origin] = [None] * len(text)
    starts = [int(str(seg["out_offset"])) for seg in segs]
    if starts[:1] != [0] or any(a >= b for a, b in zip(starts, starts[1:], strict=False)):
        probs.append(f"segments do not start at 0 and increase: {starts[:8]}")
        return origins
    for n, seg in enumerate(segs):
        end = starts[n + 1] if n + 1 < len(segs) else len(text)
        if seg["spell_file"] is None:
            if end - starts[n] != 1 or text[starts[n]] not in BLANKS:
                probs.append(f"loc-0 segment at {starts[n]} is {text[starts[n]:end]!r}")
            continue
        path = str(seg["spell_file"])
        base = src.offset(path, int(str(seg["spell_line"])), int(str(seg["spell_col"])))
        source = src.get(path)
        for k in range(starts[n], end):
            origins[k] = (path, base + k - starts[n])
            if base + k - starts[n] >= len(source) or source[base + k - starts[n]] != text[k]:
                probs.append(f"output byte {k} {text[k]!r} is not the byte spelled at "
                             f"{path}:{seg['spell_line']}:{seg['spell_col']}+{k - starts[n]}")
                return origins
    return origins


def where(origin: Origin, src: Sources) -> str:
    if origin is None:
        return "loc 0"
    try:
        text = src.get(origin[0])
    except OSError:
        return f"{origin[0]}+{origin[1]}"
    line = text.count("\n", 0, origin[1]) + 1
    return f"{os.path.basename(origin[0])}:{line}:{origin[1] - text.rfind(chr(10), 0, origin[1])}"


def compare_tokens(want: Text, got: Text, src: Sources) -> list[str]:
    exp, act = tokens(want), tokens(got)
    for n, (a, b) in enumerate(zip(exp, act, strict=False)):
        if a != b:
            context = " ".join(t for t, _ in exp[max(0, n - 4):n])
            bad = next((k for k, (x, y) in enumerate(zip(a[1], b[1], strict=False)) if x != y), 0)
            spelled = (f" (byte {bad} at {where(a[1][bad], src)} vs {where(b[1][bad], src)})"
                       if a[0] == b[0] else "")
            return [f"token {n} after {context!r}: expected {a[0]!r}, got {b[0]!r}{spelled}"]
    if len(exp) != len(act):
        return [f"expected {len(exp)} tokens, got {len(act)}; the tail differs: "
                f"{[t for t, _ in exp[len(act):][:5]]} vs {[t for t, _ in act[len(exp):][:5]]}"]
    return []


def check_stream(n: int, want: Text, out_dir: Path, src: Sources) -> list[str]:
    text = (out_dir / f"{n}.v").read_bytes().decode("latin-1")
    segs = json.loads((out_dir / f"{n}.segments.json").read_text(encoding="latin-1"))
    probs: list[str] = []
    origins = stream_origins(text, segs, src, probs)
    probs += compare_tokens(want, list(zip(text, origins, strict=True)), src)
    return [f"stream {n}: {p}" for p in probs]


def script_word(word: str) -> str:
    return f'"{word}"' if re.search(r"[\s;#]", word) else word


def run_odin3(odin3: str, root: Path) -> subprocess.CompletedProcess[str]:
    script = f"preprocess --out {script_word(str(root / 'out'))} --segments"
    argv = [odin3, "--diag-paths", "resolved", "--project", str(root / "p.o3proj"), "-p", script]
    try:
        return subprocess.run(argv, cwd=root, capture_output=True, text=True, errors="replace",
                              timeout=ODIN3_TIMEOUT, check=False)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(argv, -1, "", f"timed out after {ODIN3_TIMEOUT} s")


def write_project(proj: Project, root: Path) -> None:
    for d in DIRS:
        (root / d).mkdir(exist_ok=True)
    for rel, text in proj.files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_bytes(text.encode("latin-1"))
    (root / "p.o3proj").write_text(proj.o3proj(), encoding="latin-1")


def check_project(odin3: str, proj: Project, root: Path) -> list[str]:
    """Runs odin3 on the project written under root and compares it with the reference."""
    ref = Reference(proj.defines, [os.path.normpath(root / d) for d in proj.incdirs])
    try:
        want = [ref.stream(os.path.normpath(root / r)) for r in proj.roots]
    except RefError as exc:
        return [f"the reference rejects the generated project (generator bug): {exc}"]
    proc = run_odin3(odin3, root)
    if proc.returncode != 0:
        return [f"odin3 exited {proc.returncode}: {proc.stderr.strip()[-2000:]}"]
    src = Sources(ref.cmdline)
    probs: list[str] = []
    for n, stream in enumerate(want):
        probs += check_stream(n, stream, root / "out", src)
    read = (root / "out" / "files-read.txt").read_text(encoding="latin-1").splitlines()
    if read != ref.files_read:
        probs.append(f"files-read.txt: expected {ref.files_read}, got {read}")
    return probs


def run_case(odin3: str, seed: int, case: int, keep: Path | None) -> list[str]:
    proj = generate(seed, case)
    with tempfile.TemporaryDirectory(prefix="pp-fuzz-") as tmp:
        root = Path(os.path.abspath(tmp))
        write_project(proj, root)
        probs = check_project(odin3, proj, root)
        if probs and keep is not None:
            shutil.copytree(root, keep / f"seed{seed}-case{case}", dirs_exist_ok=True)
    return probs


# ---- command line ----------------------------------------------------------------------------


def cmd_run(args: argparse.Namespace) -> int:
    binary = shutil.which(args.odin3)
    if binary is None:
        print(f"pp-fuzz: {args.odin3}: no such executable", file=sys.stderr)
        return 2
    odin3 = os.path.abspath(binary)
    cases = range(args.first, args.first + args.cases)
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = pool.map(lambda c: (c, run_case(odin3, args.seed, c, args.keep)), cases)
        for case, probs in results:
            if probs:
                failed += 1
                print(f"FAIL case {case} (replay: pp-fuzz run --seed {args.seed} --first {case} "
                      f"--cases 1 --keep DIR)")
                for p in probs[:SHOW_PROBLEMS]:
                    print(f"     {p}")
    print(f"pp-fuzz: {len(cases)} cases from seed {args.seed}, {failed} failed")
    return 1 if failed else 0


def cmd_show(args: argparse.Namespace) -> int:
    proj = generate(args.seed, args.case)
    for rel, text in [("p.o3proj", proj.o3proj()), *proj.files.items()]:
        print(f"==> {rel}")
        print(text, end="" if text.endswith("\n") else "\n")
    return 0


def cmd_seeds(args: argparse.Namespace) -> int:
    """A libFuzzer seed corpus: every .v/.vh under the sources, after a zero options byte."""
    args.out.mkdir(parents=True, exist_ok=True)
    count = 0
    for source in args.sources:
        paths = sorted(source.rglob("*.v*")) if source.is_dir() else [source]
        for path in (p for p in paths if p.suffix in (".v", ".vh") and p.is_file()):
            name = re.sub(r"[^A-Za-z0-9_.-]", "_", f"{count}_{path.name}")
            (args.out / name).write_bytes(b"\0" + path.read_bytes())
            count += 1
    print(f"pp-fuzz: {count} seeds in {args.out}")
    return 0 if count else 1


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(prog="pp-fuzz", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("--odin3", required=True)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--cases", type=int, default=1000)
    p.add_argument("--first", type=int, default=0)
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--keep", type=Path)
    p = sub.add_parser("show")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--case", type=int, default=0)
    p = sub.add_parser("seeds")
    p.add_argument("out", type=Path)
    p.add_argument("sources", type=Path, nargs="+")
    args = ap.parse_args(argv)
    commands = {"run": cmd_run, "show": cmd_show, "seeds": cmd_seeds}
    return commands[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
