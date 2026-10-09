"""project-fixtures: reference project parsers and checker for tests/golden/projects.

Usage::

    project-fixtures check  [--root DIR] [--case NAME]...
    project-fixtures parse  CASE_DIR FORMAT [--entry FILE]
    project-fixtures oracle [--root DIR] [--case NAME]... [--write] [--yosys PATH] [--ghdl PATH]

The corpus (``tests/golden/projects/<case>/``, see its README) describes one small design per
case in every project format that can express it: ``.o3proj`` (DESIGN §4.0), an EDA ``-f``
file list, a Quartus ``.qpf``/``.qsf`` pair and an Odin II XML config.  Each case's
``expected.json`` holds the normalized project record all of them must parse to, the resolved
design (top, units reached, files read) or, for a negative case, the expected located error.

These parsers are the reference the Phase 2 C readers are tested against, so they are small
and exact: no format is "mostly" parsed; anything outside the subset is an error or is listed
as ignored.

``check`` verifies the corpus: every applicable format of every case parses (from another
working directory) to the expected record, resolves to the expected design or fails with the
expected error, every non-project file in a case is used, and the oracle files are present.
``parse`` prints what one format of one case parses to (for writing ``expected.json``).
``oracle`` reruns each positive case's ``ref.cmd`` (Yosys, and GHDL for VHDL) in a scratch
copy of the case, smallest case first, and compares the result to the committed ``ref.blif``
(``--write`` replaces it).

Exit 0 success, 1 check failure / oracle mismatch, 2 usage or input error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from xml.parsers import expat

FORMATS = ("o3proj", "f", "qsf", "odin2")
DEFAULT_ENTRY = {"o3proj": "project.o3proj", "f": "files.f", "odin2": "odin2.xml"}
PROJECT_SUFFIXES = (".o3proj", ".f", ".qsf", ".qpf")
CASE_META = ("expected.json", "ref.blif", "ref.cmd")
LANGUAGES = ("verilog", "systemverilog", "vhdl", "blif", "vqm", "edif")
EXT_LANGUAGE = {".v": "verilog", ".vh": "verilog", ".sv": "systemverilog",
                ".svh": "systemverilog", ".vhd": "vhdl", ".vhdl": "vhdl", ".blif": "blif",
                ".vqm": "vqm", ".edf": "edif", ".edif": "edif"}
ARCH_KINDS = {".o3lib": "o3lib", ".xml": "vpr_xml"}
RECORD_KEYS = ("files", "libfiles", "libdirs", "libext", "incdirs", "defines", "top", "params",
               "arch", "rules", "inventory", "overrides", "flow", "output", "ignored")
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT / "tests" / "golden" / "projects"


class ProjectError(Exception):
    """A located project or design error: what a reader must report (kind, file:line, names)."""

    def __init__(self, kind: str, at: str | None, names: Sequence[str], detail: str = "") -> None:
        self.kind = kind
        self.at = at
        self.names = list(names)
        self.detail = detail
        super().__init__(str(self))

    def __str__(self) -> str:
        what = self.detail or ", ".join(self.names)
        return f"{self.at or '<project>'}: error: {self.kind}: {what}"

    def as_json(self) -> dict[str, Any]:
        return {"kind": self.kind, "at": self.at, "names": self.names}


class UsageError(Exception):
    """Bad command line or unreadable corpus (exit 2)."""


def empty_record() -> dict[str, Any]:
    return {"files": [], "libfiles": [], "libdirs": [], "libext": [], "incdirs": [],
            "defines": [], "top": [], "params": [], "arch": [], "rules": [], "inventory": [],
            "overrides": [], "flow": None, "output": None, "ignored": []}


class Ctx:
    """One parse: the case directory (record paths are relative to it), the record being
    filled, and every file touched (read or named) on the way."""

    def __init__(self, case_dir: Path) -> None:
        self.case_dir = Path(os.path.abspath(case_dir))
        self.record = empty_record()
        self.touched: set[str] = set()

    def rel(self, path: Path) -> str:
        return Path(os.path.relpath(os.path.normpath(path), self.case_dir)).as_posix()

    def at(self, path: Path, line: int) -> str:
        return f"{self.rel(path)}:{line}"

    def resolve(self, base: Path, given: str, at: str | None, is_dir: bool = False) -> str:
        """`given` resolved against the directory `base`; it must exist."""
        path = Path(os.path.normpath(base / given))
        if not (path.is_dir() if is_dir else path.is_file()):
            kind = "directory" if is_dir else "file"
            raise ProjectError("missing_file", at, [self.rel(path)], f"{kind} not found: {given}")
        rel = self.rel(path)
        if not is_dir:
            self.touched.add(rel)
        return rel


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines()


def language_of(given: str, at: str) -> str:
    lang = EXT_LANGUAGE.get(Path(given).suffix.lower())
    if lang is None:
        raise ProjectError("unknown_file_type", at, [given], f"no language for {given}")
    return lang


def define_entry(text: str, at: str) -> dict[str, Any]:
    name, sep, value = text.partition("=")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$]*", name):
        raise ProjectError("syntax", at, [text], f"bad macro name in {text!r}")
    return {"name": name, "value": value if sep else None}


def add_file(ctx: Ctx, base: Path, given: str, lang: str, lib: str, at: str) -> None:
    path = ctx.resolve(base, given, at)
    ctx.record["files"].append({"path": path, "language": lang, "library": lib})


def add_arch(ctx: Ctx, base: Path, given: str, at: str) -> None:
    kind = ARCH_KINDS.get(Path(given).suffix.lower())
    if kind is None:
        raise ProjectError("unknown_file_type", at, [given], f"not an architecture: {given}")
    ctx.record["arch"].append({"kind": kind, "path": ctx.resolve(base, given, at)})


def count_of(text: str, at: str) -> int:
    if not text.isdigit():
        raise ProjectError("syntax", at, [text], f"expected a count, got {text!r}")
    return int(text)


# ---- .o3proj (DESIGN §4.0 grammar) ------------------------------------------------------------

THRESHOLDS = ("min_width", "max_width", "min_depth")
O3Handler = Callable[[Ctx, Path, list[str], str], None]


def words_of(line: str, at: str) -> list[str]:
    """Whitespace-separated words; "double quotes" group, `#` starts a comment."""
    lex = shlex.shlex(line, posix=True)
    lex.whitespace_split = True
    lex.commenters = "#"
    try:
        return list(lex)
    except ValueError as exc:
        raise ProjectError("syntax", at, [], str(exc)) from exc


def need(args: list[str], lo: int, hi: int, at: str, usage: str) -> None:
    if not lo <= len(args) <= hi:
        raise ProjectError("syntax", at, [], f"usage: {usage}")


def o3_file(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    usage = "file <language> <path> [library <name>]"
    need(args, 2, 4, at, usage)
    if args[0] not in LANGUAGES or (len(args) > 2 and (len(args) != 4 or args[2] != "library")):
        raise ProjectError("syntax", at, [], f"usage: {usage}")
    add_file(ctx, base, args[1], args[0], args[3] if len(args) == 4 else "work", at)


def o3_libfile(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 2, 2, at, "libfile <language> <path>")
    if args[0] not in LANGUAGES:
        raise ProjectError("syntax", at, [args[0]], f"unknown language {args[0]!r}")
    ctx.record["libfiles"].append({"path": ctx.resolve(base, args[1], at), "language": args[0]})


def o3_libdir(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "libdir <dir>")
    ctx.record["libdirs"].append(ctx.resolve(base, args[0], at, is_dir=True))


def o3_libext(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 99, at, "libext <ext>...")
    ctx.record["libext"] += args


def o3_incdir(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "incdir <dir>")
    ctx.record["incdirs"].append(ctx.resolve(base, args[0], at, is_dir=True))


def o3_define(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "define <name>[=<value>]")
    ctx.record["defines"].append(define_entry(args[0], at))


def o3_top(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "top <module>")
    ctx.record["top"].append(args[0])


def o3_param(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 2, 2, at, "param <module>.<name> <value>")
    module, dot, name = args[0].rpartition(".")
    if not dot or not module or not name:
        raise ProjectError("syntax", at, [args[0]], "usage: param <module>.<name> <value>")
    ctx.record["params"].append({"module": module, "name": name, "value": args[1]})


def o3_arch(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "arch <file.o3lib|file.xml>")
    add_arch(ctx, base, args[0], at)


def o3_device(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 2, at, "device <family> [<part>]")
    part = args[1] if len(args) == 2 else None
    ctx.record["arch"].append({"kind": "device", "family": args[0], "device": part})


def map_cells(words: list[str], at: str) -> list[str]:
    cells = [c.strip() for c in " ".join(words).split(",")]
    if not cells or any(not c or " " in c for c in cells):
        raise ProjectError("syntax", at, [], "expected <cell>[, <cell>...] after 'to'")
    return cells


def o3_map(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    usage = "map <scope> <subject> to <cell>[, <cell>...] | soft | keep [<threshold> <n>]..."
    need(args, 3, 99, at, usage)
    rule: dict[str, Any] = {"rule": "map", "scope": args[0], "subject": args[1],
                            "action": args[2], "cells": []}
    rest = args[3:]
    if args[2] == "to":
        n = next((i for i, w in enumerate(rest) if w in THRESHOLDS), len(rest))
        rule["cells"] = map_cells(rest[:n], at)
        rest = rest[n:]
    elif args[2] not in ("soft", "keep"):
        raise ProjectError("syntax", at, [args[2]], f"usage: {usage}")
    if len(rest) % 2:
        raise ProjectError("syntax", at, [], f"usage: {usage}")
    for key, value in zip(rest[::2], rest[1::2], strict=True):
        if key not in THRESHOLDS or key in rule:
            raise ProjectError("syntax", at, [key], f"bad or repeated threshold {key!r}")
        rule[key] = count_of(value, at)
    ctx.record["rules"].append(rule)


def o3_limit(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 2, 2, at, "limit <libcell> <count>")
    ctx.record["rules"].append({"rule": "limit", "cell": args[0],
                                "count": count_of(args[1], at)})


def o3_patterns(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "patterns <file.o3lib>")
    ctx.record["rules"].append({"rule": "patterns", "path": ctx.resolve(base, args[0], at)})


def o3_available(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 2, 2, at, "available <libcell> <count>")
    ctx.record["inventory"].append({"cell": args[0], "count": count_of(args[1], at)})


def o3_hide(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "hide <libcell>")
    ctx.record["overrides"].append({"override": "hide", "cell": args[0]})


def o3_cell(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    usage = "cell <name> from <libcell> [param <name>=<value>...]"
    need(args, 3, 99, at, usage)
    if args[1] != "from" or (len(args) > 3 and (args[3] != "param" or len(args) == 4)):
        raise ProjectError("syntax", at, [], f"usage: {usage}")
    params: dict[str, str] = {}
    for word in args[4:]:
        key, eq, value = word.partition("=")
        if not eq or not key or key in params:
            raise ProjectError("syntax", at, [word], f"bad parameter {word!r}")
        params[key] = value
    ctx.record["overrides"].append({"override": "cell", "name": args[0], "from": args[2],
                                    "params": params})


def o3_flow(ctx: Ctx, base: Path, args: list[str], at: str) -> None:
    need(args, 1, 1, at, "flow <script.o3>")
    if ctx.record["flow"] is not None:
        raise ProjectError("syntax", at, ["flow"], "flow given twice")
    ctx.record["flow"] = ctx.resolve(base, args[0], at)


O3PROJ_KEYS: dict[str, O3Handler] = {
    "file": o3_file, "libfile": o3_libfile, "libdir": o3_libdir, "libext": o3_libext,
    "incdir": o3_incdir, "define": o3_define, "top": o3_top, "param": o3_param,
    "arch": o3_arch, "device": o3_device, "map": o3_map, "limit": o3_limit,
    "patterns": o3_patterns, "available": o3_available, "hide": o3_hide, "cell": o3_cell,
    "flow": o3_flow,
}


def parse_o3proj(ctx: Ctx, path: Path) -> None:
    for n, line in enumerate(read_lines(path), 1):
        at = ctx.at(path, n)
        words = words_of(line, at)
        if not words:
            continue
        handler = O3PROJ_KEYS.get(words[0])
        if handler is None:
            raise ProjectError("unknown_key", at, [words[0]], f"unknown key {words[0]!r}")
        handler(ctx, path.parent, words[1:], at)


# ---- EDA -f file list -------------------------------------------------------------------------


def f_words(path: Path, ctx: Ctx) -> list[tuple[str, str]]:
    """(word, file:line) for every word; `//` (at a word start) and `#` start comments."""
    out: list[tuple[str, str]] = []
    for n, line in enumerate(read_lines(path), 1):
        at = ctx.at(path, n)
        out += [(w, at) for w in words_of(re.sub(r"(^|\s)//.*", r"\1", line), at)]
    return out


def f_plus(ctx: Ctx, base: Path, word: str, at: str) -> None:
    key, *values = word[1:].split("+")
    values = [v for v in values if v]
    if key not in ("incdir", "define", "libext"):
        raise ProjectError("unknown_option", at, [word], f"unknown option {word!r}")
    if not values:
        raise ProjectError("syntax", at, [word], f"{word!r} has no value")
    for value in values:
        if key == "incdir":
            ctx.record["incdirs"].append(ctx.resolve(base, value, at, is_dir=True))
        elif key == "define":
            ctx.record["defines"].append(define_entry(value, at))
        else:
            ctx.record["libext"].append(value)


def f_option(ctx: Ctx, path: Path, chain: list[Path], opt: str, arg: str, at: str) -> None:
    base = path.parent
    if opt in ("-f", "-F"):
        nested = Path(os.path.normpath(base / arg))
        ctx.resolve(base, arg, at)
        if nested in chain:
            names = [ctx.rel(p) for p in chain[chain.index(nested):]] + [ctx.rel(nested)]
            raise ProjectError("f_cycle", at, names, "file list cycle: " + " -> ".join(names))
        parse_f(ctx, nested, [*chain, nested])
    elif opt == "-v":
        rel = ctx.resolve(base, arg, at)
        ctx.record["libfiles"].append({"path": rel, "language": language_of(arg, at)})
    else:
        ctx.record["libdirs"].append(ctx.resolve(base, arg, at, is_dir=True))


def parse_f(ctx: Ctx, path: Path, chain: list[Path] | None = None) -> None:
    chain = chain or [Path(os.path.normpath(path))]
    words = f_words(path, ctx)
    i = 0
    while i < len(words):
        word, at = words[i]
        i += 1
        if word in ("-f", "-F", "-v", "-y"):
            if i == len(words):
                raise ProjectError("syntax", at, [word], f"{word} needs an argument")
            f_option(ctx, path, chain, word, words[i][0], at)
            i += 1
        elif word.startswith("+"):
            f_plus(ctx, path.parent, word, at)
        elif word.startswith("-"):
            raise ProjectError("unknown_option", at, [word], f"unknown option {word!r}")
        else:
            add_file(ctx, path.parent, word, language_of(word, at), "work", at)


# ---- Quartus .qpf / .qsf ----------------------------------------------------------------------


class TclReader:
    """Splits a .qsf (a flat Tcl script) into commands of words: "quotes" (with backslash
    escapes), {braces} (nested, verbatim), `\\`-newline continuation, `;`/newline separators,
    and `#` comments where a command may start."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.i = 0
        self.line = 1

    def peek(self) -> str:
        return self.text[self.i] if self.i < len(self.text) else ""

    def take(self) -> str:
        c = self.peek()
        self.i += 1
        self.line += c == "\n"
        return c

    def skip_blank(self, newlines: bool) -> None:
        while True:
            c = self.peek()
            if c and (c in " \t\r" or (newlines and c in "\n;")):
                self.take()
            elif c == "\\" and self.text[self.i + 1:self.i + 2] == "\n":
                self.take()
                self.take()
            elif newlines and c == "#":
                while self.peek() not in ("\n", ""):
                    self.take()
            else:
                return

    def quoted(self) -> str:
        self.take()
        out: list[str] = []
        while (c := self.take()) != '"':
            if not c:
                raise ValueError("unterminated quote")
            out.append(self.take() if c == "\\" else c)
        return "".join(out)

    def braced(self) -> str:
        self.take()
        depth, out = 1, list[str]()
        while True:
            c = self.take()
            if not c:
                raise ValueError("unterminated brace")
            depth += (c == "{") - (c == "}")
            if depth == 0:
                return "".join(out)
            out.append(c)

    def bare(self) -> str:
        out: list[str] = []
        while (c := self.peek()) and c not in " \t\r\n;":
            if c == "\\" and self.text[self.i + 1:self.i + 2] == "\n":
                break
            out.append(self.take())
        return "".join(out)

    def commands(self) -> Iterator[tuple[list[str], int]]:
        while True:
            self.skip_blank(newlines=True)
            if not self.peek():
                return
            start, words = self.line, []
            while self.peek() and self.peek() not in "\n;":
                c = self.peek()
                words.append(self.quoted() if c == '"' else self.braced() if c == "{"
                             else self.bare())
                self.skip_blank(newlines=False)
            yield words, start


QSF_ASSIGN = ("set_global_assignment", "set_instance_assignment", "set_io_assignment")
QSF_FLAGS = ("-remove", "-disable")
QSF_FILES = {"VERILOG_FILE": "verilog", "SYSTEMVERILOG_FILE": "systemverilog",
             "VHDL_FILE": "vhdl"}
# Synthesis assignments that are partial-mapping rules (§4.0): value -> subject made soft,
# or None for "the default" (consumed, no rule).  Any other value is ignored (info line).
QSF_RULES: dict[str, dict[str, str | None]] = {
    "AUTO_DSP_RECOGNITION": {"OFF": "$mul", "ON": None},
    "AUTO_RAM_RECOGNITION": {"OFF": "$mem", "ON": None},
    "DSP_BLOCK_BALANCING": {"LOGIC ELEMENTS": "$mul", "AUTO": None},
    "MULTSTYLE": {"LOGIC": "$mul", "AUTO": None},
    "RAMSTYLE": {"LOGIC": "$mem", "AUTO": None},
}


def quartus_scope(target: str | None) -> str:
    """A Quartus -to path (`a|b:c`) as a rule scope (`a.c`); no target is global (`*`)."""
    if not target:
        return "*"
    return ".".join(part.rpartition(":")[2] for part in target.split("|") if part)


@dataclass
class QsfState:
    revision: str
    top: str | None = None
    device: dict[str, Any] | None = None
    params: list[tuple[str | None, str, str]] = field(default_factory=list)
    ignored: set[str] = field(default_factory=set)


def qsf_options(words: list[str], at: str) -> tuple[dict[str, str], list[str]]:
    opts: dict[str, str] = {}
    positional: list[str] = []
    i = 0
    while i < len(words):
        w = words[i]
        if w.startswith("-") and len(w) > 1 and not w[1].isdigit():
            if w in QSF_FLAGS:
                opts[w] = ""
            elif i + 1 == len(words):
                raise ProjectError("syntax", at, [w], f"{w} needs a value")
            else:
                opts[w] = words[i + 1]
                i += 1
        else:
            positional.append(w)
        i += 1
    return opts, positional


def qsf_rule(st: QsfState, ctx: Ctx, name: str, value: str, opts: dict[str, str]) -> None:
    table = QSF_RULES[name]
    if value.upper() not in table:
        st.ignored.add(name)
        return
    subject = table[value.upper()]
    if subject is not None:
        ctx.record["rules"].append({"rule": "map", "scope": quartus_scope(opts.get("-to")),
                                    "subject": subject, "action": "soft", "cells": []})


def qsf_assignment(st: QsfState, ctx: Ctx, base: Path, words: list[str], at: str) -> None:
    opts, positional = qsf_options(words, at)
    name = opts.get("-name", "")
    if not name:
        raise ProjectError("syntax", at, [], "assignment without -name")
    value = positional[-1] if positional else ""
    if name in QSF_FILES:
        add_file(ctx, base, value, QSF_FILES[name], opts.get("-library", "work"), at)
    elif name == "TOP_LEVEL_ENTITY":
        st.top = value
    elif name == "SEARCH_PATH":
        ctx.record["incdirs"].append(ctx.resolve(base, value, at, is_dir=True))
    elif name == "VERILOG_MACRO":
        ctx.record["defines"].append(define_entry(value, at))
    elif name in ("FAMILY", "DEVICE"):
        if st.device is None:
            st.device = {"kind": "device", "family": None, "device": None}
            ctx.record["arch"].append(st.device)
        st.device["family" if name == "FAMILY" else "device"] = value
    elif name in QSF_RULES:
        qsf_rule(st, ctx, name, value, opts)
    else:
        st.ignored.add(name)


def qsf_command(st: QsfState, ctx: Ctx, base: Path, words: list[str], at: str) -> None:
    cmd = words[0]
    if cmd in QSF_ASSIGN:
        qsf_assignment(st, ctx, base, words[1:], at)
    elif cmd == "set_location_assignment":
        st.ignored.add("LOCATION")
    elif cmd == "set_parameter":
        opts, positional = qsf_options(words[1:], at)
        if "-name" not in opts or len(positional) != 1:
            raise ProjectError("syntax", at, [], "usage: set_parameter -name <n> <value>")
        st.params.append((opts.get("-entity"), opts["-name"], positional[0]))
    else:
        st.ignored.add(cmd)


def parse_qsf(ctx: Ctx, path: Path, revision: str) -> None:
    st = QsfState(revision)
    try:
        commands = list(TclReader(path.read_text(encoding="utf-8")).commands())
    except ValueError as exc:
        raise ProjectError("syntax", ctx.rel(path), [], str(exc)) from exc
    for words, n in commands:
        qsf_command(st, ctx, path.parent, words, ctx.at(path, n))
    top = st.top or st.revision
    ctx.record["top"] = [top]
    ctx.record["params"] = [{"module": entity or top, "name": name, "value": value}
                            for entity, name, value in st.params]
    ctx.record["ignored"] = sorted(st.ignored)


def parse_qpf(ctx: Ctx, path: Path) -> None:
    revisions: list[tuple[str, int]] = []
    for n, line in enumerate(read_lines(path), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = re.fullmatch(r'\s*([A-Z_]+)\s*=\s*"([^"]*)"\s*', line)
        if m is None:
            raise ProjectError("syntax", ctx.at(path, n), [], 'expected NAME = "value"')
        if m[1] == "PROJECT_REVISION":
            revisions.append((m[2], n))
    revision, n = revisions[0] if revisions else (path.stem, 0)
    at = ctx.at(path, n) if n else ctx.rel(path)
    qsf = Path(ctx.resolve(path.parent, f"{revision}.qsf", at))
    parse_qsf(ctx, ctx.case_dir / qsf, revision)


# ---- Odin II XML config -----------------------------------------------------------------------


@dataclass
class XElem:
    tag: str
    line: int
    text: list[str] = field(default_factory=list)
    children: list[XElem] = field(default_factory=list)

    @property
    def value(self) -> str:
        return "".join(self.text).strip()


def read_xml(path: Path, ctx: Ctx) -> XElem:
    parser = expat.ParserCreate()
    stack: list[XElem] = [XElem("", 0)]

    def start(tag: str, attrs: dict[str, str]) -> None:
        elem = XElem(tag, parser.CurrentLineNumber)
        stack[-1].children.append(elem)
        stack.append(elem)

    def end(tag: str) -> None:
        stack.pop()

    def text(data: str) -> None:
        stack[-1].text.append(data)

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = text
    try:
        parser.Parse(path.read_bytes(), True)
    except expat.ExpatError as exc:
        raise ProjectError("syntax", ctx.at(path, exc.lineno), [], str(exc)) from exc
    return stack[0].children[0]


ODIN2_INPUT_TYPES = {"verilog": "verilog", "systemverilog": "systemverilog", "blif": "blif"}


def odin2_inputs(ctx: Ctx, path: Path, elem: XElem, ignored: set[str]) -> None:
    lang = "verilog"
    for child in elem.children:
        if child.tag == "input_type":
            lang = ODIN2_INPUT_TYPES.get(child.value.lower(), "")
            if not lang:
                raise ProjectError("syntax", ctx.at(path, child.line), [child.value],
                                   f"unsupported input_type {child.value!r}")
    for child in elem.children:
        if child.tag == "input_path_and_name":
            add_file(ctx, path.parent, child.value, lang, "work", ctx.at(path, child.line))
        elif child.tag != "input_type":
            ignored.add(child.tag)


def odin2_output(ctx: Ctx, path: Path, elem: XElem, ignored: set[str]) -> None:
    out: dict[str, Any] = {"path": None, "format": None}
    for child in elem.children:
        if child.tag == "output_type":
            out["format"] = child.value.lower()
        elif child.tag == "output_path_and_name":
            out["path"] = ctx.rel(path.parent / child.value)
        elif child.tag == "target":
            for arch in child.children:
                if arch.tag != "arch_file":
                    ignored.add(arch.tag)
                    continue
                add_arch(ctx, path.parent, arch.value, ctx.at(path, arch.line))
        else:
            ignored.add(child.tag)
    if out["path"] is not None:
        ctx.record["output"] = out


def parse_odin2(ctx: Ctx, path: Path) -> None:
    root = read_xml(path, ctx)
    if root.tag != "config":
        raise ProjectError("syntax", ctx.at(path, root.line), [root.tag], "root must be <config>")
    ignored: set[str] = set()
    for child in root.children:
        if child.tag == "verilog_files":
            for vf in child.children:
                if vf.tag != "verilog_file":
                    ignored.add(vf.tag)
                    continue
                add_file(ctx, path.parent, vf.value, "verilog", "work", ctx.at(path, vf.line))
        elif child.tag == "inputs":
            odin2_inputs(ctx, path, child, ignored)
        elif child.tag == "output":
            odin2_output(ctx, path, child, ignored)
        else:
            ignored.add(child.tag)
    ctx.record["ignored"] = sorted(ignored)


PARSERS: dict[str, Callable[[Ctx, Path], None]] = {
    "o3proj": parse_o3proj, "f": parse_f, "qsf": parse_qpf, "odin2": parse_odin2,
}


# ---- source scan: design units, instantiations, includes --------------------------------------


@dataclass
class Ref:
    """An instantiation: unit name, library when the source names one (VHDL `entity l.e`)."""

    name: str
    library: str | None
    at: str
    from_vhdl: bool


@dataclass
class Unit:
    """A design unit: Verilog module/interface/program/package, VHDL entity/package, BLIF
    model."""

    name: str
    library: str
    kind: str
    language: str
    at: str
    refs: list[Ref] = field(default_factory=list)

    @property
    def instantiable(self) -> bool:
        return self.kind != "package"

    def key(self) -> tuple[str, str]:
        return self.library, self.name.lower() if self.language == "vhdl" else self.name

    def matches(self, ref: Ref) -> bool:
        if ref.library is not None and ref.library != self.library:
            return False
        if self.language == "vhdl" or ref.from_vhdl:
            return self.name.lower() == ref.name.lower()
        return self.name == ref.name


_VERILOG_KEYWORDS = """
alias always always_comb always_ff always_latch and assert assign assume automatic before
begin bind bins binsof bit break buf bufif0 bufif1 byte case casex casez cell chandle checker
class clocking cmos config const constraint context continue cover covergroup coverpoint cross
deassign default defparam design disable dist do edge else end endcase endchecker endclass
endclocking endconfig endfunction endgenerate endgroup endinterface endmodule endpackage
endprimitive endprogram endproperty endspecify endsequence endtable endtask enum event
eventually expect export extends extern final first_match for force foreach forever fork
forkjoin function generate genvar global highz0 highz1 if iff ifnone ignore_bins
illegal_bins implements implies import incdir include initial inout input inside instance int
integer interconnect interface intersect join join_any join_none large let liblist library
local localparam logic longint macromodule matches medium modport module nand negedge
nettype new nexttime nmos nor noshowcancelled not notif0 notif1 null or output package packed
parameter pmos posedge primitive priority program property protected pull0 pull1 pulldown
pullup pulsestyle_ondetect pulsestyle_onevent pure rand randc randcase randsequence rcmos real
realtime ref reg reject_on release repeat restrict return rnmos rpmos rtran rtranif0 rtranif1
s_always s_eventually s_nexttime s_until s_until_with scalared sequence shortint shortreal
showcancelled signed small soft solve specify specparam static string strong strong0 strong1
struct super supply0 supply1 sync_accept_on sync_reject_on table tagged task this throughout
time timeprecision timeunit tran tranif0 tranif1 tri tri0 tri1 triand trior trireg type
typedef union unique unique0 unsigned until until_with untyped use uwire var vectored virtual
void wait wait_order wand weak weak0 weak1 while wildcard wire with within wor xnor xor
"""
VERILOG_KEYWORDS = frozenset(_VERILOG_KEYWORDS.split())
VERILOG_DECL = {"module": "module", "macromodule": "module", "interface": "interface",
                "program": "program", "package": "package"}
VERILOG_END = ("endmodule", "endinterface", "endprogram", "endpackage")
NOT_INSTANCE_AFTER = ("function", "task", "automatic", "static", "virtual", "extern",
                      "import", "export", "typedef", "new", "return")
VERILOG_TOKEN = re.compile(r"""
    "(?:\\.|[^"\\])*"
  | (?P<id>[A-Za-z_][A-Za-z0-9_$]*)
  | (?P<esc>\\\S+)
  | [$`][A-Za-z0-9_$]+
  | [0-9][0-9_]*
  | '[sS]?[bBoOdDhH]?[0-9a-fA-FxXzZ_?]+
  | ::
  | \S
""", re.VERBOSE)

Token = tuple[str, bool, str]  # text, is a (non-keyword) identifier, file:line


def strip_comments(text: str) -> str:
    """Verilog/SV text without // and /* */ comments; strings and newlines kept."""
    out: list[str] = []
    pat = re.compile(r'"(?:\\.|[^"\\\n])*"|//[^\n]*|/\*.*?\*/', re.DOTALL)
    pos = 0
    for m in pat.finditer(text):
        out.append(text[pos:m.start()])
        tok = m.group()
        out.append(tok if tok.startswith('"') else "\n" * tok.count("\n"))
        pos = m.end()
    out.append(text[pos:])
    return "".join(out)


class Preprocessor:
    """The Verilog preprocessor subset the fixtures need: `define/`undef (names only),
    `ifdef/`ifndef/`elsif/`else/`endif, `include (the including file's directory, then the
    include path, in order).  Other directives are dropped; macro uses are not expanded."""

    def __init__(self, ctx: Ctx, incdirs: list[str], defines: list[dict[str, Any]]) -> None:
        self.ctx = ctx
        self.incdirs = [ctx.case_dir / d for d in incdirs]
        self.defined = {d["name"] for d in defines}
        self.read: set[str] = set()

    def find_include(self, name: str, including: Path, at: str) -> Path:
        for d in [including.parent, *self.incdirs]:
            cand = Path(os.path.normpath(d / name))
            if cand.is_file():
                return cand
        raise ProjectError("include_not_found", at, [name], f"`include \"{name}\" not found")

    def directive(self, word: str, arg: str, stack: list[list[bool]]) -> bool | None:
        """Updates the conditional stack; returns whether the line is active (None: not a
        conditional directive)."""
        active = all(s[0] for s in stack)
        if word in ("ifdef", "ifndef"):
            outer = active
            taken = outer and ((arg in self.defined) == (word == "ifdef"))
            stack.append([taken, taken or not outer])
        elif word == "elsif":
            top = stack[-1]
            top[0] = not top[1] and arg in self.defined
            top[1] = top[1] or top[0]
        elif word == "else":
            top = stack[-1]
            top[0], top[1] = not top[1], True
        elif word == "endif":
            stack.pop()
        else:
            return None
        return all(s[0] for s in stack)

    def lines(self, path: Path, depth: int = 0) -> Iterator[tuple[str, str]]:
        """(text, file:line) of every active non-directive line, includes expanded."""
        self.read.add(self.ctx.rel(path))
        stack: list[list[bool]] = []
        text = strip_comments(path.read_text(encoding="utf-8"))
        for n, line in enumerate(text.split("\n"), 1):
            at = self.ctx.at(path, n)
            m = re.match(r"\s*`(\w+)\s*(.*)", line)
            if m is None:
                if all(s[0] for s in stack):
                    yield line, at
                continue
            word, arg = m[1], m[2].strip()
            first = arg.split()[0] if arg.split() else ""
            if self.directive(word, first, stack) is not None or not all(s[0] for s in stack):
                continue
            if word == "define":
                self.defined.add(first.split("(")[0])
            elif word == "undef":
                self.defined.discard(first)
            elif word == "include":
                name = arg.strip().strip('"<>').strip()
                if depth > 32:
                    raise ProjectError("include_cycle", at, [name], "`include nested too deep")
                yield from self.lines(self.find_include(name, path, at), depth + 1)


def verilog_tokens(pre: Preprocessor, path: Path) -> list[Token]:
    toks: list[Token] = []
    for line, at in pre.lines(path):
        for m in VERILOG_TOKEN.finditer(line):
            text = m.group()
            ident = (m["id"] is not None and text not in VERILOG_KEYWORDS) or m["esc"] is not None
            toks.append((text, ident, at))
    return toks


def skip_brackets(toks: list[Token], j: int) -> int:
    depth = 0
    while j < len(toks):
        depth += (toks[j][0] == "[") - (toks[j][0] == "]")
        j += 1
        if depth == 0:
            break
    return j


def is_instance(toks: list[Token], i: int) -> bool:
    """`type #(...) name (...)` or `type name [range] (...)` at i (a module scope)."""
    if not toks[i][1] or (i and toks[i - 1][0] in NOT_INSTANCE_AFTER):
        return False
    nxt = toks[i + 1][0] if i + 1 < len(toks) else ""
    if nxt == "#":
        return True
    if i + 2 >= len(toks) or not toks[i + 1][1]:
        return False
    j = i + 2
    if toks[j][0] == "[":
        j = skip_brackets(toks, j)
    return j < len(toks) and toks[j][0] == "("


def verilog_units(toks: list[Token], library: str, language: str) -> list[Unit]:
    units: list[Unit] = []
    cur: Unit | None = None
    i = 0
    while i < len(toks):
        text, _, at = toks[i]
        if cur is None and text in VERILOG_DECL:
            j = i + 1
            while j < len(toks) and toks[j][0] in ("automatic", "static"):
                j += 1
            if j == len(toks) or not toks[j][1]:
                raise ProjectError("syntax", at, [text], f"{text} without a name")
            cur = Unit(toks[j][0].lstrip("\\"), library, VERILOG_DECL[text], language, at)
            units.append(cur)
            i = j
        elif cur is not None and text in VERILOG_END:
            cur = None
        elif cur is not None and cur.kind != "package" and is_instance(toks, i):
            cur.refs.append(Ref(text.lstrip("\\"), None, at, from_vhdl=False))
        i += 1
    return units


VHDL_TOKEN = re.compile(r'"[^"\n]*"|\\[^\\\n]*\\|[a-z][a-z0-9_]*|\'.\'|[0-9][0-9_.#a-f]*|'
                        r":=|=>|<=|\S")


def vhdl_tokens(ctx: Ctx, path: Path) -> list[tuple[str, str]]:
    toks: list[tuple[str, str]] = []
    for n, line in enumerate(read_lines(path), 1):
        line = re.sub(r"--.*", "", line.lower())
        toks += [(m.group(), ctx.at(path, n)) for m in VHDL_TOKEN.finditer(line)]
    return toks


def vhdl_ref(toks: list[tuple[str, str]], i: int, library: str) -> Ref | None:
    """The instantiation after the `:` at i, if any."""
    def tok(k: int) -> str:
        return toks[k][0] if k < len(toks) else ""
    at = toks[i][1]
    if tok(i + 1) == "entity" and tok(i + 3) == ".":
        lib = tok(i + 2)
        return Ref(tok(i + 4), library if lib == "work" else lib, at, from_vhdl=True)
    if tok(i + 1) == "component":
        return Ref(tok(i + 2), None, at, from_vhdl=True)
    if tok(i + 2) in ("port", "generic") and tok(i + 3) == "map":
        return Ref(tok(i + 1), None, at, from_vhdl=True)
    return None


def vhdl_units(ctx: Ctx, path: Path, library: str) -> list[Unit]:
    toks = vhdl_tokens(ctx, path)
    units: list[Unit] = []
    bodies: dict[str, list[Ref]] = {}
    owner: list[Ref] | None = None
    for i, (text, at) in enumerate(toks):
        prev = toks[i - 1][0] if i else ""
        nxt = toks[i + 1][0] if i + 1 < len(toks) else ""
        if text in ("entity", "package") and prev != ":" and nxt != "body" and prev != "end":
            units.append(Unit(nxt, library, text, "vhdl", at))
            owner = None
        elif text == "architecture" and prev != "end" and i + 3 < len(toks):
            owner = bodies.setdefault(toks[i + 3][0], [])
        elif text == ":" and owner is not None:
            ref = vhdl_ref(toks, i, library)
            if ref is not None:
                owner.append(ref)
    for unit in units:
        unit.refs = bodies.get(unit.name, []) if unit.kind == "entity" else []
    return units


def blif_units(ctx: Ctx, path: Path, library: str) -> list[Unit]:
    units: list[Unit] = []
    for n, line in enumerate(read_lines(path), 1):
        words = line.split("#")[0].split()
        if len(words) >= 2 and words[0] == ".model":
            units.append(Unit(words[1], library, "model", "blif", ctx.at(path, n)))
        elif len(words) >= 2 and words[0] == ".subckt" and units:
            units[-1].refs.append(Ref(words[1], None, ctx.at(path, n), from_vhdl=False))
    return units


class Scanner:
    """Reads source files into design units, in project order (defines carry across files,
    like one Verilog compilation unit)."""

    def __init__(self, ctx: Ctx) -> None:
        rec = ctx.record
        self.ctx = ctx
        self.pre = Preprocessor(ctx, rec["incdirs"], rec["defines"])

    @property
    def read(self) -> set[str]:
        return self.pre.read

    def scan(self, rel: str, language: str, library: str) -> list[Unit]:
        path = self.ctx.case_dir / rel
        if language in ("verilog", "systemverilog"):
            return verilog_units(verilog_tokens(self.pre, path), library, language)
        self.pre.read.add(rel)
        if language == "vhdl":
            return vhdl_units(self.ctx, path, library)
        if language == "blif":
            return blif_units(self.ctx, path, library)
        raise UsageError(f"{rel}: the reference scanner does not read {language}")


def check_duplicates(units: list[Unit]) -> None:
    seen: dict[tuple[str, str], Unit] = {}
    for unit in units:
        first = seen.setdefault(unit.key(), unit)
        if first is not unit:
            raise ProjectError("duplicate_module", unit.at, [unit.name],
                               f"{unit.name} already defined at {first.at}")


def select_top(record: dict[str, Any], design: list[Unit]) -> list[Unit]:
    if record["top"]:
        tops = []
        for name in record["top"]:
            ref = Ref(name, None, "", from_vhdl=False)
            unit = next((u for u in design if u.instantiable and u.matches(ref)), None)
            if unit is None:
                raise ProjectError("unknown_top", None, [name], f"top {name!r} not found")
            tops.append(unit)
        return tops
    if all(f["language"] == "blif" for f in record["files"]) and design:
        return [design[0]]  # BLIF: the first model is the top
    refs = [r for u in design for r in u.refs]
    cands = [u for u in design if u.instantiable and not any(u.matches(r) for r in refs)]
    if len(cands) == 1:
        return cands
    names = sorted(u.name for u in cands)
    if not cands:
        raise ProjectError("no_top", None, [], "no top candidate: every module is instantiated")
    raise ProjectError("ambiguous_top", None, names, "several top candidates: " + ", ".join(names))


class Elaborator:
    """Walks the hierarchy from the top, loading -v/-y library modules on demand."""

    def __init__(self, scanner: Scanner, design: list[Unit]) -> None:
        self.scanner = scanner
        self.design = design
        rec = scanner.ctx.record
        self.lib: list[Unit] = []
        for lf in rec["libfiles"]:
            self.lib += scanner.scan(lf["path"], lf["language"], "work")
        self.libdirs: list[str] = rec["libdirs"]
        self.libext: list[str] = rec["libext"] or [""]

    def from_libdirs(self, ref: Ref) -> Unit | None:
        for d in self.libdirs:
            for ext in self.libext:
                rel = Path(os.path.normpath(Path(d) / (ref.name + ext))).as_posix()
                if (self.scanner.ctx.case_dir / rel).is_file():
                    self.lib += self.scanner.scan(rel, EXT_LANGUAGE.get(ext, "verilog"), "work")
                    self.scanner.ctx.touched.add(rel)
                    return next((u for u in self.lib if u.matches(ref)), None)
        return None

    def find(self, ref: Ref, owner: Unit) -> Unit:
        found = [u for u in self.design if u.instantiable and u.matches(ref)]
        found.sort(key=lambda u: u.library != owner.library)
        unit = (found[0] if found else
                next((u for u in self.lib if u.matches(ref)), None) or self.from_libdirs(ref))
        if unit is None:
            raise ProjectError("unresolved_module", ref.at, [ref.name],
                               f"no module or entity {ref.name!r}")
        return unit

    def walk(self, tops: list[Unit]) -> list[Unit]:
        reached: list[Unit] = []
        work = list(tops)
        while work:
            unit = work.pop()
            if any(unit is r for r in reached):
                continue
            reached.append(unit)
            work += [self.find(ref, unit) for ref in unit.refs]
        return reached


def resolve(ctx: Ctx) -> dict[str, Any]:
    """Top selection and hierarchy resolution over the parsed record."""
    scanner = Scanner(ctx)
    design: list[Unit] = []
    for f in ctx.record["files"]:
        design += scanner.scan(f["path"], f["language"], f["library"])
    check_duplicates(design)
    elab = Elaborator(scanner, design)
    tops = select_top(ctx.record, design)
    reached = elab.walk(tops)
    ctx.touched |= scanner.read
    return {"top": [u.name for u in tops],
            "units": sorted(f"{u.library}.{u.name}" for u in reached),
            "read": sorted(scanner.read)}


# ---- running a format, checking a case --------------------------------------------------------


@dataclass
class Result:
    record: dict[str, Any] | None = None
    resolved: dict[str, Any] | None = None
    error: ProjectError | None = None
    touched: set[str] = field(default_factory=set)


def entry_of(case_dir: Path, fmt: str, expected: dict[str, Any]) -> Path:
    given = expected.get("entry", {}).get(fmt)
    if given is not None:
        return case_dir / str(given)
    if fmt != "qsf":
        return case_dir / DEFAULT_ENTRY[fmt]
    qpfs = sorted(case_dir.glob("*.qpf"))
    if len(qpfs) != 1:
        raise UsageError(f"{case_dir}: expected exactly one .qpf, found {len(qpfs)}")
    return qpfs[0]


@contextmanager
def elsewhere() -> Iterator[None]:
    """Run with an unrelated working directory: project paths must not depend on it."""
    old = os.getcwd()
    with tempfile.TemporaryDirectory() as tmp:
        os.chdir(tmp)
        try:
            yield
        finally:
            os.chdir(old)


def load_project(case_dir: Path, fmt: str, entry: Path) -> Result:
    ctx = Ctx(case_dir)
    entry = Path(os.path.abspath(entry))
    res = Result()
    try:
        with elsewhere():
            if not entry.is_file():
                raise ProjectError("missing_file", None, [ctx.rel(entry)], "project not found")
            PARSERS[fmt](ctx, entry)
            res.record = ctx.record
            res.resolved = resolve(ctx)
    except ProjectError as exc:
        res.error = exc
    res.touched = ctx.touched
    return res


EXPECTED_KEYS = {"description", "features", "formats", "entry", "project", "format_overrides",
                 "resolved", "oracle", "oracle_reason", "error", "unused"}
ORACLES = ("yosys", "ghdl+yosys")


def schema_problems(exp: dict[str, Any]) -> list[str]:
    probs = [f"unknown key {k!r}" for k in sorted(set(exp) - EXPECTED_KEYS)]
    probs += [f"missing key {k!r}" for k in ("description", "features", "formats")
              if k not in exp]
    fmts = exp.get("formats", [])
    if not fmts or len(set(fmts)) != len(fmts) or not set(fmts) <= set(FORMATS):
        probs.append(f"bad formats {fmts!r}")
    for fmt, over in exp.get("format_overrides", {}).items():
        if fmt not in fmts or not set(over) <= set(RECORD_KEYS):
            probs.append(f"bad format_overrides for {fmt!r}")
    if "error" in exp:
        probs += [f"negative case has {k!r}" for k in ("project", "resolved", "oracle")
                  if k in exp]
    else:
        probs += [f"missing key {k!r}" for k in ("project", "resolved", "oracle")
                  if k not in exp]
        if "project" in exp and list(exp["project"]) != list(RECORD_KEYS):
            probs.append(f"project keys must be exactly {', '.join(RECORD_KEYS)} in order")
        if exp.get("oracle") not in (*ORACLES, None):
            probs.append(f"oracle must be one of {ORACLES} or null")
        if exp.get("oracle") is None and not exp.get("oracle_reason"):
            probs.append("oracle null needs an oracle_reason")
    return probs


def expected_error(exp: dict[str, Any], fmt: str) -> dict[str, Any]:
    err = dict(exp["error"])
    if isinstance(err.get("at"), dict):
        err["at"] = err["at"].get(fmt)
    return err


def format_problems(case_dir: Path, fmt: str, exp: dict[str, Any], res: Result) -> list[str]:
    if "error" in exp:
        want = expected_error(exp, fmt)
        got = res.error.as_json() if res.error else None
        return [] if got == want else [f"{fmt}: expected error {want}, got {got or 'success'}"]
    if res.error is not None:
        return [f"{fmt}: {res.error}"]
    want_rec = {**exp["project"], **exp.get("format_overrides", {}).get(fmt, {})}
    probs = [f"{fmt}: record {k}: expected {want_rec[k]!r}, got {res.record[k]!r}"
             for k in RECORD_KEYS if res.record is not None and res.record[k] != want_rec[k]]
    if res.resolved != exp["resolved"]:
        probs.append(f"{fmt}: resolved: expected {exp['resolved']}, got {res.resolved}")
    return probs


def case_files(case_dir: Path) -> list[str]:
    return sorted(p.relative_to(case_dir).as_posix() for p in case_dir.rglob("*") if p.is_file())


def file_problems(case_dir: Path, exp: dict[str, Any], entries: dict[str, Path],
                  touched: set[str]) -> list[str]:
    probs: list[str] = []
    entry_rels = {p.relative_to(case_dir).as_posix() for p in entries.values()}
    for fmt in set(FORMATS) - set(exp["formats"]):
        if fmt != "qsf" and (case_dir / DEFAULT_ENTRY[fmt]).exists():
            probs.append(f"{DEFAULT_ENTRY[fmt]} present but {fmt} is not in formats")
    if "qsf" not in exp["formats"] and next(case_dir.glob("*.qpf"), None):
        probs.append(".qpf present but qsf is not in formats")
    unused = set(exp.get("unused", []))
    for rel in case_files(case_dir):
        project = rel in entry_rels or rel.endswith(PROJECT_SUFFIXES)
        if rel in CASE_META or project:
            continue
        if rel in unused and rel in touched:
            probs.append(f"{rel} is listed as unused but a format used it")
        elif rel not in unused and rel not in touched:
            probs.append(f"{rel} is not used by any format")
    probs += [f"unused {rel} does not exist" for rel in sorted(unused)
              if not (case_dir / rel).is_file()]
    positive_oracle = "error" not in exp and exp.get("oracle") is not None
    for meta in ("ref.blif", "ref.cmd"):
        if (case_dir / meta).is_file() != positive_oracle:
            probs.append(f"{meta} {'missing' if positive_oracle else 'not expected'}")
    if positive_oracle and (case_dir / "ref.cmd").is_file():
        cmd = (case_dir / "ref.cmd").read_text(encoding="utf-8")
        probs += [f"ref.cmd does not name top {t!r}" for t in exp["resolved"]["top"]
                  if f"-top {t}" not in cmd]
    return probs


def check_case(case_dir: Path) -> list[str]:
    try:
        exp = json.loads((case_dir / "expected.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"expected.json: {exc}"]
    probs = schema_problems(exp)
    if probs:
        return probs
    entries = {fmt: entry_of(case_dir, fmt, exp) for fmt in exp["formats"]}
    touched: set[str] = set()
    for fmt, entry in entries.items():
        res = load_project(case_dir, fmt, entry)
        touched |= res.touched
        probs += format_problems(case_dir, fmt, exp, res)
    return probs + file_problems(case_dir, exp, entries, touched)


def case_dirs(root: Path, names: Sequence[str]) -> list[Path]:
    if not root.is_dir():
        raise UsageError(f"{root}: not a directory")
    dirs = sorted(p for p in root.iterdir() if (p / "expected.json").is_file())
    if names:
        missing = sorted(set(names) - {d.name for d in dirs})
        if missing:
            raise UsageError(f"no such case: {', '.join(missing)}")
        dirs = [d for d in dirs if d.name in names]
    if not dirs:
        raise UsageError(f"{root}: no cases")
    return dirs


def cmd_check(args: argparse.Namespace) -> int:
    failed = 0
    dirs = case_dirs(args.root, args.case)
    for d in dirs:
        probs = check_case(d)
        failed += bool(probs)
        print(f"{'FAIL' if probs else 'ok  '} {d.name}")
        for p in probs:
            print(f"     {p}")
    print(f"project-fixtures: {len(dirs) - failed}/{len(dirs)} cases ok")
    return 1 if failed else 0


def cmd_parse(args: argparse.Namespace) -> int:
    case_dir = Path(args.case_dir)
    exp: dict[str, Any] = {}
    if args.entry is None and (case_dir / "expected.json").is_file():
        exp = json.loads((case_dir / "expected.json").read_text(encoding="utf-8"))
    entry = Path(args.entry) if args.entry else entry_of(case_dir, args.format, exp)
    res = load_project(case_dir, args.format, entry)
    out: dict[str, Any] = {"record": res.record, "resolved": res.resolved}
    if res.error is not None:
        out = {"error": res.error.as_json(), "message": str(res.error)}
    print(json.dumps(out, indent=2))
    return 0


# ---- oracle -----------------------------------------------------------------------------------


def source_bytes(case_dir: Path) -> int:
    return sum(p.stat().st_size for p in case_dir.rglob("*")
               if p.is_file() and p.name not in CASE_META)


def default_yosys() -> str:
    built = REPO_ROOT.parent / "external" / "yosys" / "build" / "yosys"
    return os.environ.get("YOSYS") or (str(built) if built.is_file() else "yosys")


def run_oracle(case_dir: Path, yosys: str, ghdl: str) -> tuple[bytes | None, str]:
    """Runs ref.cmd in a scratch copy of the case; returns (ref.blif bytes, log)."""
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp) / case_dir.name
        shutil.copytree(case_dir, work, ignore=shutil.ignore_patterns("ref.blif"))
        env = {**os.environ, "YOSYS": yosys, "GHDL": ghdl}
        try:
            proc = subprocess.run(["bash", "ref.cmd"], cwd=work, env=env, capture_output=True,
                                  text=True, timeout=600, check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            return None, str(exc)
        out = work / "out" / "ref.blif"
        if proc.returncode != 0 or not out.is_file():
            return None, (proc.stdout + proc.stderr).strip()
        return out.read_bytes(), ""


def cmd_oracle(args: argparse.Namespace) -> int:
    cases = []
    for d in case_dirs(args.root, args.case):
        exp = json.loads((d / "expected.json").read_text(encoding="utf-8"))
        if "error" not in exp and exp.get("oracle") is not None:
            cases.append(d)
    cases.sort(key=lambda d: (source_bytes(d), d.name))  # smallest first
    bad = 0
    for d in cases:
        new, log = run_oracle(d, args.yosys, args.ghdl)
        ref = d / "ref.blif"
        if new is None:
            status = "FAILED"
        elif ref.is_file() and ref.read_bytes() == new:
            status = "same"
        elif args.write:
            ref.write_bytes(new)
            status = "written"
        else:
            status = "DIFFERS" if ref.is_file() else "MISSING"
        bad += status in ("FAILED", "DIFFERS", "MISSING")
        print(f"{status:8} {d.name}")
        if log:
            print("\n".join(f"     {line}" for line in log.splitlines()[-20:]))
    return 1 if bad else 0


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(prog="project-fixtures", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("check", "oracle"):
        p = sub.add_parser(name)
        p.add_argument("--root", type=Path, default=DEFAULT_ROOT)
        p.add_argument("--case", action="append", default=[])
    sub.choices["oracle"].add_argument("--write", action="store_true")
    sub.choices["oracle"].add_argument("--yosys", default=default_yosys())
    sub.choices["oracle"].add_argument("--ghdl", default=os.environ.get("GHDL", "ghdl"))
    p = sub.add_parser("parse")
    p.add_argument("case_dir")
    p.add_argument("format", choices=FORMATS)
    p.add_argument("--entry")
    args = ap.parse_args(argv)
    commands = {"check": cmd_check, "parse": cmd_parse, "oracle": cmd_oracle}
    try:
        return commands[args.cmd](args)
    except (UsageError, OSError, ValueError) as exc:
        print(f"project-fixtures: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
