"""project-fixtures: reference project parsers and checker for tests/golden/projects.

Usage::

    project-fixtures check  [--root DIR] [--case NAME]...
    project-fixtures parse  CASE_DIR FORMAT [--entry FILE] [--revision REV]
    project-fixtures oracle [--root DIR] [--case NAME]... [--write] [--yosys PATH] [--ghdl PATH]

The corpus (``tests/golden/projects/<case>/``, see its README) describes one small design per
case in every project format that can express it: ``.o3proj``, an EDA ``-f`` file list, a
Quartus ``.qpf``/``.qsf`` project and an Odin II XML config (DESIGN §4.0 is the grammar).  Each
case's ``expected.json`` holds the normalized project record every format must parse to, the
resolved design (top, units reached, files read, analysis order) or, for a negative case, the
expected located error, and the phase in which the case must elaborate.

These parsers are the reference the Phase 2 C readers are tested against: exact, no leniency
beyond §4.0.  Anything outside the grammar is an error or is listed as ignored.

``check`` verifies the corpus: every format of every case parses (from an unrelated working
directory, with only the case's ``env``) to the expected record, resolves to the expected
design or fails with the expected error, every file in a case is used (or listed as unused),
and the oracle files are present.  ``parse`` prints what one format of one case parses to.
``oracle`` reruns each positive case's ``ref.cmd`` in a scratch copy of the case, smallest case
first, checks the tool versions it records, and compares the result to the committed
``ref.blif`` (``--write`` replaces both).

Exit 0 success, 1 check failure / oracle mismatch, 2 usage or input error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
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
CASE_META = ("expected.json", "ref.blif", "ref.cmd")
LANGUAGES = ("verilog", "systemverilog", "vhdl", "blif", "vqm", "edif")
EXT_LANGUAGE = {".v": "verilog", ".vh": "verilog", ".sv": "systemverilog",
                ".svh": "systemverilog", ".vhd": "vhdl", ".vhdl": "vhdl", ".blif": "blif",
                ".vqm": "vqm", ".edf": "edif", ".edif": "edif"}
ARCH_KINDS = {".o3lib": "o3lib", ".xml": "vpr_xml"}
DEFAULT_LIBEXT = [".v"]
RECORD_KEYS = ("files", "libfiles", "libdirs", "libext", "incdirs", "defines", "top", "params",
               "arch", "rules", "inventory", "overrides", "flow", "output", "ignored",
               "duplicates")
# Record fields a format may state differently from the case's common record (it cannot say
# what the others say, yet elaborates to the same design).
OVERRIDABLE = {"o3proj": (), "f": (), "qsf": ("top", "ignored", "libdirs"),
               "odin2": ("top", "output", "ignored")}
PHASES = (2, 5, 6)
ODIN2_READER = "odin2-reader"
ORACLES = ("yosys", "ghdl+yosys", "hand+yosys")
ERROR_KINDS = (
    "missing_file", "unknown_key", "unknown_option", "unknown_file_type",
    "unsupported_input_type", "unsupported_language", "syntax", "undefined_variable",
    "f_cycle", "qip_cycle", "ambiguous_revision", "unknown_revision", "include_not_found",
    "include_cycle", "duplicate_module", "dependency_cycle", "ambiguous_top", "no_top",
    "unknown_top", "unresolved_module",
)
NUMBER = re.compile(r"-?[0-9][0-9_]*|([0-9][0-9_]*)?'[sS]?[bBoOdDhH][0-9a-fA-FxXzZ_?]+")
IDENT = r"[A-Za-z_][A-Za-z0-9_$]*"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO_ROOT / "tests" / "golden" / "projects"


class ProjectError(Exception):
    """A located project or design error: what a reader must report (kind, file:line, names)."""

    def __init__(self, kind: str, at: str | None, names: Sequence[str], detail: str = "") -> None:
        assert kind in ERROR_KINDS, kind
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


def syntax(at: str | None, detail: str, *names: str) -> ProjectError:
    return ProjectError("syntax", at, names, detail)


def empty_record() -> dict[str, Any]:
    return {"files": [], "libfiles": [], "libdirs": [], "libext": [], "incdirs": [],
            "defines": [], "top": None, "params": [], "arch": [], "rules": [], "inventory": [],
            "overrides": [], "flow": None, "output": None, "ignored": [], "duplicates": []}


class Ctx:
    """One parse: the case directory (the harness makes record paths relative to it), the
    record being filled, the environment for `$VAR`, per-format options, and every file
    touched on the way."""

    def __init__(self, case_dir: Path, env: dict[str, str] | None = None,
                 options: dict[str, str] | None = None) -> None:
        self.case_dir = Path(os.path.abspath(case_dir))
        self.record = empty_record()
        self.env = dict(env or {})
        self.options = dict(options or {})
        self.touched: set[str] = set()
        self.ignored: set[str] = set()
        self.dups: set[str] = set()
        self.params_at: list[str] = []  # .o3proj `param` lines, checked against the top
        self.top_at: str | None = None  # where the top was declared
        self.f_root = self.case_dir
        self.f_sv = False

    def rel(self, path: Path) -> str:
        return Path(os.path.relpath(os.path.normpath(path), self.case_dir)).as_posix()

    def at(self, path: Path, line: int) -> str:
        return f"{self.rel(path)}:{line}"

    def touch(self, path: Path) -> None:
        self.touched.add(self.rel(path))

    def resolve(self, base: Path, given: str, at: str | None, is_dir: bool = False) -> str:
        """`given` resolved against the directory `base`; it must exist."""
        path = Path(os.path.normpath(base / given))
        if not (path.is_dir() if is_dir else path.is_file()):
            kind = "directory" if is_dir else "file"
            raise ProjectError("missing_file", at, [self.rel(path)], f"{kind} not found: {given}")
        if not is_dir:
            self.touch(path)
        return self.rel(path)

    def finish(self) -> None:
        self.record["ignored"] = sorted(self.ignored)
        self.record["duplicates"] = sorted(self.dups)


def read_lines(path: Path) -> list[str]:
    """Lines of a project file; LF and CRLF endings are both accepted."""
    return path.read_text(encoding="utf-8").splitlines()


def language_of(given: str, at: str | None) -> str:
    lang = EXT_LANGUAGE.get(Path(given).suffix.lower())
    if lang is None:
        raise ProjectError("unknown_file_type", at, [given], f"no language for {given}")
    return lang


def define_entry(text: str, at: str) -> dict[str, Any]:
    name, sep, value = text.partition("=")
    if not re.fullmatch(IDENT, name):
        raise syntax(at, f"bad macro name in {text!r}", text)
    return {"name": name, "value": value if sep else None}


def add_file(ctx: Ctx, base: Path, given: str, lang: str, lib: str, at: str) -> None:
    path = ctx.resolve(base, given, at)
    if any(f["path"] == path and f["library"] == lib for f in ctx.record["files"]):
        ctx.dups.add(path)  # the same file in the same library: read once
        return
    ctx.record["files"].append({"path": path, "language": lang, "library": lib})


def add_libfile(ctx: Ctx, base: Path, given: str, lang: str, at: str) -> None:
    path = ctx.resolve(base, given, at)
    if any(f["path"] == path for f in ctx.record["libfiles"]):
        ctx.dups.add(path)
        return
    ctx.record["libfiles"].append({"path": path, "language": lang})


def add_libext(ctx: Ctx, ext: str, at: str) -> None:
    language_of("x" + ext, at)
    ctx.record["libext"].append(ext)


def add_arch(ctx: Ctx, base: Path, given: str, at: str) -> None:
    kind = ARCH_KINDS.get(Path(given).suffix.lower())
    if kind is None:
        raise ProjectError("unknown_file_type", at, [given], f"not an architecture: {given}")
    ctx.record["arch"].append({"kind": kind, "path": ctx.resolve(base, given, at)})


def set_top(ctx: Ctx, name: str, at: str | None) -> None:
    if not re.fullmatch(rf"({IDENT}\.)?{IDENT}", name):
        raise syntax(at, f"bad top name {name!r}", name)
    if ctx.record["top"] is not None:
        raise syntax(at, f"top given twice ({ctx.record['top']}, {name})", name)
    ctx.record["top"] = name
    ctx.top_at = at


def count_of(text: str, at: str) -> int:
    if not text.isdigit():
        raise syntax(at, f"expected a count, got {text!r}", text)
    return int(text)


# ---- words: .o3proj and -f ---------------------------------------------------------------------

Word = tuple[str, bool]  # text, was quoted


def _quoted(line: str, i: int, at: str) -> tuple[str, int]:
    """The double-quoted word starting at line[i]; escapes are \\" and \\\\ only."""
    out: list[str] = []
    j = i + 1
    while True:
        if j >= len(line):
            raise syntax(at, "unterminated quote")
        c = line[j]
        if c == '"':
            break
        if c == "\\":
            nxt = line[j + 1:j + 2]
            if nxt not in ('"', "\\"):
                raise syntax(at, f"bad escape \\{nxt} (only \\\" and \\\\)")
            out.append(nxt)
            j += 2
            continue
        out.append(c)
        j += 1
    j += 1
    if j < len(line) and line[j] not in " \t":
        raise syntax(at, "characters after a closing quote")
    if not out:
        raise syntax(at, "empty quoted word")
    return "".join(out), j


def split_words(line: str, at: str, file_list: bool = False) -> list[Word]:
    """Blank-separated words; a word is bare or wholly "double-quoted"; `#` (and, in a file
    list, `//`) at the start of a word begins a comment.  Inside a bare word `#` and `\\` are
    ordinary; so is `"` in a file list (`+define+TAG="AB"`), elsewhere it is an error."""
    words: list[Word] = []
    i = 0
    while i < len(line):
        c = line[i]
        if c in " \t":
            i += 1
        elif c == "#" or (file_list and line.startswith("//", i)):
            break
        elif c == '"':
            text, i = _quoted(line, i, at)
            words.append((text, True))
        else:
            j = i
            while j < len(line) and line[j] not in " \t":
                if line[j] == '"' and not file_list:
                    raise syntax(at, "quote inside a word")
                j += 1
            words.append((line[i:j], False))
            i = j
    return words


# ---- .o3proj (DESIGN §4.0 grammar) ------------------------------------------------------------

THRESHOLDS = ("min_width", "max_width", "min_depth")
O3Handler = Callable[[Ctx, Path, list[Word], str], None]


def need(args: list[Word], lo: int, hi: int, at: str, usage: str) -> list[str]:
    if not lo <= len(args) <= hi:
        raise syntax(at, f"usage: {usage}")
    return [w for w, _ in args]


def o3_file(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    usage = "file <language> <path> [library <name>]"
    a = need(args, 2, 4, at, usage)
    if a[0] not in LANGUAGES or (len(a) > 2 and (len(a) != 4 or a[2] != "library")):
        raise syntax(at, f"usage: {usage}")
    add_file(ctx, base, a[1], a[0], a[3] if len(a) == 4 else "work", at)


def o3_libfile(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 2, 2, at, "libfile <language> <path>")
    if a[0] not in LANGUAGES:
        raise syntax(at, f"unknown language {a[0]!r}", a[0])
    add_libfile(ctx, base, a[1], a[0], at)


def o3_libdir(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 1, at, "libdir <dir>")
    ctx.record["libdirs"].append(ctx.resolve(base, a[0], at, is_dir=True))


def o3_libext(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    for ext in need(args, 1, 99, at, "libext <ext>..."):
        add_libext(ctx, ext, at)


def o3_incdir(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 1, at, "incdir <dir>")
    ctx.record["incdirs"].append(ctx.resolve(base, a[0], at, is_dir=True))


def o3_define(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 1, at, "define <name>[=<value>]")
    ctx.record["defines"].append(define_entry(a[0], at))


def o3_top(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    set_top(ctx, need(args, 1, 1, at, "top [<library>.]<module>")[0], at)


def param_value(word: Word, at: str) -> tuple[str, str]:
    text, quoted = word
    if quoted:
        return text, "string"
    if not NUMBER.fullmatch(text):
        raise syntax(at, f"parameter value {text!r}: quote a string, or give a number", text)
    return text, "number"


def o3_param(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    usage = "param <top>[.<instance>...].<name> <value>"
    a = need(args, 2, 2, at, usage)
    scope, dot, name = a[0].rpartition(".")
    if not dot or not re.fullmatch(rf"{IDENT}(\.{IDENT})*", scope) or not re.fullmatch(IDENT,
                                                                                       name):
        raise syntax(at, f"usage: {usage}", a[0])
    value, kind = param_value(args[1], at)
    ctx.record["params"].append({"scope": scope, "name": name, "value": value, "type": kind})
    ctx.params_at.append(at)


def o3_arch(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    add_arch(ctx, base, need(args, 1, 1, at, "arch <file.o3lib|file.xml>")[0], at)


def o3_device(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 2, at, "device <family> [<part>]")
    part = a[1] if len(a) == 2 else None
    ctx.record["arch"].append({"kind": "device", "family": a[0], "device": part})


def map_cells(words: list[str], at: str) -> list[str]:
    cells = [c.strip() for c in " ".join(words).split(",")]
    if not cells or any(not c or " " in c for c in cells):
        raise syntax(at, "expected <cell>[, <cell>...] after 'to'")
    return cells


def o3_map(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    usage = "map <scope> <subject> to <cell>[, <cell>...] | soft | keep [<threshold> <n>]..."
    a = need(args, 3, 99, at, usage)
    rule: dict[str, Any] = {"rule": "map", "scope": a[0], "subject": a[1], "action": a[2],
                            "cells": []}
    rest = a[3:]
    if a[2] == "to":
        n = next((i for i, w in enumerate(rest) if w in THRESHOLDS), len(rest))
        rule["cells"] = map_cells(rest[:n], at)
        rest = rest[n:]
    elif a[2] not in ("soft", "keep"):
        raise syntax(at, f"usage: {usage}", a[2])
    if len(rest) % 2:
        raise syntax(at, f"usage: {usage}")
    for key, value in zip(rest[::2], rest[1::2], strict=True):
        if key not in THRESHOLDS or key in rule:
            raise syntax(at, f"bad or repeated threshold {key!r}", key)
        rule[key] = count_of(value, at)
    ctx.record["rules"].append(rule)


def split_rule(subject: str, width: bool, depth: str | None) -> dict[str, Any]:
    return {"rule": "split", "subject": subject, "width": width, "depth": depth}


def o3_split(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    usage = "split <subject> [width] [depth min|max|<n>]"
    a = need(args, 2, 4, at, usage)
    rest, width, depth = a[1:], False, None
    if rest[0] == "width":
        width, rest = True, rest[1:]
    if rest:
        if len(rest) != 2 or rest[0] != "depth" or not (rest[1] in ("min", "max")
                                                        or rest[1].isdigit()):
            raise syntax(at, f"usage: {usage}")
        depth = rest[1]
    if not width and depth is None:
        raise syntax(at, f"usage: {usage}")
    ctx.record["rules"].append(split_rule(a[0], width, depth))


def o3_limit(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 2, 2, at, "limit <libcell> <count>")
    ctx.record["rules"].append({"rule": "limit", "cell": a[0], "count": count_of(a[1], at)})


def o3_patterns(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 1, at, "patterns <file.o3lib>")
    ctx.record["rules"].append({"rule": "patterns", "path": ctx.resolve(base, a[0], at)})


def o3_available(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 2, 2, at, "available <libcell> <count>")
    ctx.record["inventory"].append({"cell": a[0], "count": count_of(a[1], at)})


def o3_hide(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 1, at, "hide <libcell>")
    ctx.record["overrides"].append({"override": "hide", "cell": a[0]})


def o3_cell(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    usage = "cell <name> from <libcell> [param <name>=<value>...]"
    a = need(args, 3, 99, at, usage)
    if a[1] != "from" or (len(a) > 3 and (a[3] != "param" or len(a) == 4)):
        raise syntax(at, f"usage: {usage}")
    params: dict[str, str] = {}
    for word in a[4:]:
        key, eq, value = word.partition("=")
        if not eq or not key or key in params:
            raise syntax(at, f"bad parameter {word!r}", word)
        params[key] = value
    ctx.record["overrides"].append({"override": "cell", "name": a[0], "from": a[2],
                                    "params": params})


def o3_flow(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    a = need(args, 1, 1, at, "flow <script.o3>")
    if ctx.record["flow"] is not None:
        raise syntax(at, "flow given twice", "flow")
    ctx.record["flow"] = ctx.resolve(base, a[0], at)


def o3_import(ctx: Ctx, base: Path, args: list[Word], at: str) -> None:
    usage = "import <file.qpf|file.qsf> [revision <name>]"
    a = need(args, 1, 3, at, usage)
    if len(a) == 2 or (len(a) == 3 and a[1] != "revision"):
        raise syntax(at, f"usage: {usage}")
    path = Path(os.path.normpath(base / a[0]))
    if path.suffix not in (".qpf", ".qsf"):
        raise ProjectError("unknown_file_type", at, [a[0]], "import takes a .qpf or .qsf")
    ctx.resolve(base, a[0], at)
    parse_quartus(ctx, path, a[2] if len(a) == 3 else None, at)


O3PROJ_KEYS: dict[str, O3Handler] = {
    "file": o3_file, "libfile": o3_libfile, "libdir": o3_libdir, "libext": o3_libext,
    "incdir": o3_incdir, "define": o3_define, "top": o3_top, "param": o3_param,
    "arch": o3_arch, "device": o3_device, "map": o3_map, "split": o3_split, "limit": o3_limit,
    "patterns": o3_patterns, "available": o3_available, "hide": o3_hide, "cell": o3_cell,
    "flow": o3_flow, "import": o3_import,
}


def check_param_scopes(ctx: Ctx) -> None:
    """Parameter overrides apply to the top or an instance path under it."""
    top = ctx.record["top"]
    for param, at in zip(ctx.record["params"], ctx.params_at, strict=False):
        root = param["scope"].split(".")[0]
        if top is None or root != top.rpartition(".")[2]:
            raise syntax(at, f"param scope {param['scope']!r} must start at the top "
                             f"({top or 'none declared'})", param["scope"])


def parse_o3proj(ctx: Ctx, path: Path) -> None:
    ctx.touch(path)
    for n, line in enumerate(read_lines(path), 1):
        at = ctx.at(path, n)
        words = split_words(line, at)
        if not words:
            continue
        handler = O3PROJ_KEYS.get(words[0][0])
        if handler is None or words[0][1]:
            raise ProjectError("unknown_key", at, [words[0][0]], f"unknown key {words[0][0]!r}")
        handler(ctx, path.parent, words[1:], at)
    check_param_scopes(ctx)


# ---- EDA -f file list -------------------------------------------------------------------------

F_ARG_OPTIONS = ("-f", "-F", "-v", "-y", "-top", "--top-module")
VAR = re.compile(r"\$(?:\{(" + IDENT + r")\}|(" + IDENT + "))")


def expand_vars(ctx: Ctx, text: str, at: str) -> str:
    def value(m: re.Match[str]) -> str:
        name = m[1] or m[2]
        if name not in ctx.env:
            raise ProjectError("undefined_variable", at, [name], f"${name} is not set")
        return ctx.env[name]

    out = VAR.sub(value, text)
    if "$" in VAR.sub("", text):
        raise syntax(at, f"bad variable reference in {text!r}", text)
    return out


def f_words(ctx: Ctx, path: Path) -> list[tuple[str, str]]:
    """(word, file:line); `$VAR` is expanded when the word is used (parse_f)."""
    out: list[tuple[str, str]] = []
    for n, line in enumerate(read_lines(path), 1):
        at = ctx.at(path, n)
        out += [(w, at) for w, _ in split_words(line, at, True)]
    return out


def f_plus(ctx: Ctx, base: Path, word: str, at: str) -> None:
    key, *values = word[1:].split("+")
    values = [v for v in values if v]
    if key not in ("incdir", "define", "libext"):
        raise ProjectError("unknown_option", at, [word], f"unknown option {word!r}")
    if not values:
        raise syntax(at, f"{word!r} has no value", word)
    for value in values:
        if key == "incdir":
            ctx.record["incdirs"].append(ctx.resolve(base, value, at, is_dir=True))
        elif key == "define":
            ctx.record["defines"].append(define_entry(value, at))
        else:
            add_libext(ctx, value, at)


def f_option(ctx: Ctx, base: Path, chain: list[Path], opt: str, arg: str, at: str) -> None:
    if opt in ("-f", "-F"):
        nested = Path(os.path.normpath(base / arg))
        ctx.resolve(base, arg, at)
        if nested in chain:
            names = [ctx.rel(p) for p in chain[chain.index(nested):]] + [ctx.rel(nested)]
            raise ProjectError("f_cycle", at, names, "file list cycle: " + " -> ".join(names))
        # -f: entries relative to the outermost list; -F: relative to the nested list.
        sv = ctx.f_sv
        parse_f(ctx, nested, ctx.f_root if opt == "-f" else nested.parent, [*chain, nested])
        ctx.f_sv = sv  # -sv inside a nested list ends with that list
    elif opt == "-v":
        add_libfile(ctx, base, arg, language_of(arg, at), at)
    elif opt == "-y":
        ctx.record["libdirs"].append(ctx.resolve(base, arg, at, is_dir=True))
    else:
        set_top(ctx, arg, at)


def f_source(ctx: Ctx, base: Path, word: str, at: str) -> None:
    lang = language_of(word, at)
    if ctx.f_sv and lang == "verilog":
        lang = "systemverilog"
    add_file(ctx, base, word, lang, "work", at)


def parse_f(ctx: Ctx, path: Path, base: Path, chain: list[Path]) -> None:
    ctx.touch(path)
    words = f_words(ctx, path)
    i = 0
    while i < len(words):
        word, at = words[i]
        word = expand_vars(ctx, word, at)
        i += 1
        if word in F_ARG_OPTIONS:
            if i == len(words):
                raise syntax(at, f"{word} needs an argument", word)
            f_option(ctx, base, chain, word, expand_vars(ctx, words[i][0], words[i][1]), at)
            i += 1
        elif word == "-sv":
            ctx.f_sv = True
        elif word.startswith("+"):
            f_plus(ctx, base, word, at)
        elif word.startswith("-"):
            raise ProjectError("unknown_option", at, [word], f"unknown option {word!r}")
        else:
            f_source(ctx, base, word, at)


def parse_f_entry(ctx: Ctx, path: Path) -> None:
    ctx.f_root = path.parent
    parse_f(ctx, path, path.parent, [Path(os.path.normpath(path))])


# ---- Quartus .qpf / .qsf / .qip ---------------------------------------------------------------

QIP_IDIOM = re.compile(r'\[file join \$::quartus\(qip_path\) (?:"([^"\\$\[\]]+)"'
                       r"|([^\s\"\\$\[\]{}]+))\]")


class TclReader:
    """Splits a .qsf/.qip (a flat Tcl script) into commands of words, as Tcl would, for the
    subset Quartus writes: bare words, "quotes" (escapes \\" and \\\\ only), {braces} (nested,
    verbatim), `\\`-newline continuation, `;`/newline separators, `#` comments where a command
    may start.  `[` and `]` inside a word are ordinary (`-to LEDR[0]`); a word that starts
    with `[` is command substitution, an error except in a .qip the idiom
    `[file join $::quartus(qip_path) "<path>"]`, which reads as <path> relative to the .qip
    (returned absolute).  `$` substitution is an error."""

    def __init__(self, text: str, where: Callable[[int], str], qip_dir: Path | None) -> None:
        self.text = text
        self.where = where
        self.qip_dir = qip_dir
        self.i = 0
        self.line = 1

    def error(self, detail: str) -> ProjectError:
        return syntax(self.where(self.line), detail)

    def peek(self, k: int = 0) -> str:
        return self.text[self.i + k] if self.i + k < len(self.text) else ""

    def take(self) -> str:
        c = self.peek()
        self.i += 1
        self.line += c == "\n"
        return c

    def continuation(self) -> bool:
        return self.peek() == "\\" and self.peek(1) == "\n"

    def skip_blank(self, newlines: bool) -> None:
        while True:
            c = self.peek()
            if c and (c in " \t\r" or (newlines and c in "\n;")):
                self.take()
            elif self.continuation():
                self.take()
                self.take()
            elif newlines and c == "#":
                while self.peek() not in ("\n", ""):
                    self.take()
            else:
                return

    def end_of_word(self, what: str) -> None:
        c = self.peek()
        if c and c not in " \t\r\n;" and not self.continuation():
            raise self.error(f"extra characters after {what}")

    def quoted(self) -> str:
        self.take()
        out: list[str] = []
        while (c := self.take()) != '"':
            if c in ("", "\n"):
                raise syntax(self.where(self.line - (c == "\n")), "unterminated quote")
            if c == "$":
                raise self.error("$ substitution is not supported")
            if c == "\\":
                c = self.take()
                if c not in ('"', "\\"):
                    raise self.error(f"bad escape \\{c} (only \\\" and \\\\)")
            out.append(c)
        self.end_of_word("close-quote")
        return "".join(out)

    def braced(self) -> str:
        self.take()
        depth, out = 1, list[str]()
        while True:
            c = self.take()
            if not c:
                raise self.error("unterminated brace")
            depth += (c == "{") - (c == "}")
            if depth == 0:
                self.end_of_word("close-brace")
                return "".join(out)
            out.append(c)

    def bracket(self) -> str:
        m = QIP_IDIOM.match(self.text, self.i)
        if self.qip_dir is None or m is None:
            raise self.error("command substitution [...] is not supported (only the .qip "
                             "idiom [file join $::quartus(qip_path) \"<path>\"])")
        while self.i < m.end():
            self.take()
        self.end_of_word("close-bracket")
        return os.path.normpath(self.qip_dir / (m[1] or m[2]))

    def bare(self) -> str:
        out: list[str] = []
        while (c := self.peek()) and c not in " \t\r\n;":
            if self.continuation():
                break
            if c in "$\\":
                raise self.error(f"{c} in a bare word is not supported")
            out.append(self.take())
        return "".join(out)

    def word(self) -> str:
        c = self.peek()
        return (self.quoted() if c == '"' else self.braced() if c == "{"
                else self.bracket() if c == "[" else self.bare())

    def commands(self) -> Iterator[tuple[list[str], int]]:
        while True:
            self.skip_blank(newlines=True)
            if not self.peek():
                return
            start, words = self.line, []
            while self.peek() and self.peek() not in "\n;":
                words.append(self.word())
                self.skip_blank(newlines=False)
            yield words, start


QSF_ASSIGN = ("set_global_assignment", "set_instance_assignment", "set_io_assignment")
QUARTUS_OPTIONS = ("-name", "-to", "-from", "-entity", "-section_id", "-library", "-hdl_version",
                   "-tag", "-comment", "-disable", "-remove")
QSF_FLAGS = ("-disable", "-remove")
QSF_FILES = {"VERILOG_FILE": "verilog", "SYSTEMVERILOG_FILE": "systemverilog",
             "VHDL_FILE": "vhdl", "VQM_FILE": "vqm", "EDIF_FILE": "edif"}
QSF_SINGLE = ("TOP_LEVEL_ENTITY", "SEARCH_PATH", "USER_LIBRARIES", "VERILOG_MACRO", "FAMILY",
              "DEVICE", "QIP_FILE")
# Synthesis assignments that are partial-mapping rules (§4.0): value (case-insensitive, as in
# Quartus) -> subject made soft, or None for "the default" (no rule).  Other values: ignored.
QSF_RULES: dict[str, dict[str, str | None]] = {
    "AUTO_DSP_RECOGNITION": {"OFF": "$mul", "ON": None},
    "AUTO_RAM_RECOGNITION": {"OFF": "$mem", "ON": None},
    "DSP_BLOCK_BALANCING": {"LOGIC ELEMENTS": "$mul", "AUTO": None},
    "MULTSTYLE": {"LOGIC": "$mul", "AUTO": None},
    "RAMSTYLE": {"LOGIC": "$mem", "AUTO": None},
}


@dataclass
class QsfState:
    revision: str
    top: str | None = None
    top_at: str | None = None
    device: dict[str, Any] | None = None
    params: list[tuple[str | None, str | None, str, str, str]] = field(default_factory=list)
    rules: list[tuple[str | None, str | None, str]] = field(default_factory=list)
    qips: list[Path] = field(default_factory=list)
    project_dir: Path = Path()


def qsf_options(words: list[str], at: str, allowed: Sequence[str]) -> tuple[dict[str, str],
                                                                             list[str]]:
    opts: dict[str, str] = {}
    positional: list[str] = []
    i = 0
    while i < len(words):
        w = words[i]
        if w.startswith("-") and len(w) > 1 and not w[1].isdigit():
            if w not in QUARTUS_OPTIONS or w not in allowed:
                raise ProjectError("unknown_option", at, [w], f"option {w} not allowed here")
            if w not in QSF_FLAGS:
                if i + 1 == len(words):
                    raise syntax(at, f"{w} needs a value", w)
                i += 1
            opts[w] = "" if w in QSF_FLAGS else words[i]
        else:
            positional.append(w)
        i += 1
    if len(positional) != 1:
        raise syntax(at, f"expected one value, got {len(positional)}")
    return opts, positional


def allowed_options(name: str) -> Sequence[str]:
    if name in QSF_FILES:
        return ("-name", "-library", "-hdl_version")
    if name in QSF_SINGLE:
        return ("-name",)
    if name in QSF_RULES:
        return ("-name", "-to", "-entity")
    return QUARTUS_OPTIONS  # ignored assignments: any Quartus option


def quartus_path(target: str) -> list[str]:
    """A Quartus -to path `a|b:c` as instance path components [a, c]."""
    return [part.rpartition(":")[2] for part in target.split("|") if part]


def anchored(entity: str | None, target: str | None, top: str) -> str:
    """A scope: `*` (no -entity/-to), the entity (module rule), or the -to instance path
    anchored at the entity, else at the top."""
    if target is None:
        return entity or "*"
    anchor = entity or top.rpartition(".")[2]
    parts = quartus_path(target)
    if not parts or parts[0] != anchor:
        parts = [anchor, *parts]
    return ".".join(parts)


def qsf_value_type(text: str) -> tuple[str, str]:
    if NUMBER.fullmatch(text):
        return text, "number"
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return text[1:-1], "string"
    return text, "string"


def qsf_dirs(ctx: Ctx, base: Path, value: str, at: str, include: bool) -> None:
    for d in (v for v in value.split(";") if v):
        rel = ctx.resolve(base, d, at, is_dir=True)
        if include:
            ctx.record["incdirs"].append(rel)
        ctx.record["libdirs"].append(rel)


def qsf_single(st: QsfState, ctx: Ctx, base: Path, name: str, value: str, at: str) -> None:
    if name == "TOP_LEVEL_ENTITY":
        st.top, st.top_at = value, at  # repeated: the last one wins (Quartus)
    elif name in ("SEARCH_PATH", "USER_LIBRARIES"):
        qsf_dirs(ctx, base, value, at, include=name == "SEARCH_PATH")
    elif name == "VERILOG_MACRO":
        ctx.record["defines"].append(define_entry(value, at))
    elif name == "QIP_FILE":
        qip = Path(os.path.normpath(base / value))
        ctx.resolve(base, value, at)
        if qip in st.qips:
            names = [ctx.rel(p) for p in st.qips[st.qips.index(qip):]] + [ctx.rel(qip)]
            raise ProjectError("qip_cycle", at, names, "QIP_FILE cycle: " + " -> ".join(names))
        st.qips.append(qip)
        run_qsf(st, ctx, qip, qip=True)
        st.qips.pop()
    else:
        if st.device is None:
            st.device = {"kind": "device", "family": None, "device": None}
            ctx.record["arch"].append(st.device)
        st.device["family" if name == "FAMILY" else "device"] = value


def qsf_assignment(st: QsfState, ctx: Ctx, base: Path, words: list[str], at: str) -> None:
    names = [words[i + 1] for i, w in enumerate(words[:-1]) if w == "-name"]
    if len(names) != 1:
        raise syntax(at, "an assignment needs one -name")
    name = names[0].upper()  # assignment names are case-insensitive in Quartus
    opts, (value,) = qsf_options(words, at, allowed_options(name))
    if name in QSF_FILES:
        add_file(ctx, base, value, QSF_FILES[name], opts.get("-library", "work"), at)
        if "-hdl_version" in opts:
            ctx.ignored.add("-hdl_version")
    elif name in QSF_SINGLE:
        qsf_single(st, ctx, base, name, value, at)
    elif name in QSF_RULES and value.upper() in QSF_RULES[name]:
        subject = QSF_RULES[name][value.upper()]
        if subject is not None:
            st.rules.append((opts.get("-entity"), opts.get("-to"), subject))
    else:
        ctx.ignored.add(name)


def qsf_command(st: QsfState, ctx: Ctx, base: Path, words: list[str], at: str) -> None:
    cmd = words[0]
    if cmd in QSF_ASSIGN:
        qsf_assignment(st, ctx, base, words[1:], at)
    elif cmd == "set_location_assignment":
        qsf_options(words[1:], at, QUARTUS_OPTIONS)
        ctx.ignored.add("LOCATION")
    elif cmd == "set_parameter":
        opts, (value,) = qsf_options(words[1:], at, ("-name", "-entity", "-to"))
        if "-name" not in opts:
            raise syntax(at, "set_parameter needs -name")
        st.params.append((opts.get("-entity"), opts.get("-to"), opts["-name"], value, at))
    else:
        ctx.ignored.add(cmd)


def run_qsf(st: QsfState, ctx: Ctx, path: Path, qip: bool = False) -> None:
    """Plain relative paths, in the .qsf and in every .qip, are relative to the project
    directory (Quartus); the .qip idiom is relative to the .qip."""
    ctx.touch(path)
    reader = TclReader(path.read_text(encoding="utf-8").replace("\r\n", "\n"),
                       lambda n: ctx.at(path, n), path.parent if qip else None)
    for words, n in list(reader.commands()):
        qsf_command(st, ctx, st.project_dir, words, ctx.at(path, n))


def parse_qsf(ctx: Ctx, path: Path, revision: str) -> None:
    st = QsfState(revision, project_dir=path.parent)
    run_qsf(st, ctx, path)
    top = st.top or st.revision  # Quartus: the top defaults to the revision name
    set_top(ctx, top, st.top_at or ctx.rel(path))
    for entity, target, name, value, at in st.params:
        if entity is not None and entity != top.rpartition(".")[2]:
            raise syntax(at, f"set_parameter -entity {entity}: parameters apply to the top "
                             f"({top}) or an instance under it", entity)
        text, kind = qsf_value_type(value)
        scope = anchored(entity, target, top) if target else top.rpartition(".")[2]
        ctx.record["params"].append({"scope": scope, "name": name,
                                     "value": text, "type": kind})
    for entity, target, subject in st.rules:
        ctx.record["rules"].append({"rule": "map", "scope": anchored(entity, target, top),
                                    "subject": subject, "action": "soft", "cells": []})


def qpf_revision(ctx: Ctx, path: Path, wanted: str | None, wanted_at: str | None) -> str:
    revisions: list[tuple[str, int]] = []
    for n, line in enumerate(read_lines(path), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = re.fullmatch(r'\s*([A-Z_]+)\s*=\s*"([^"]*)"\s*', line)
        if m is None:
            raise syntax(ctx.at(path, n), 'expected NAME = "value"')
        if m[1] == "PROJECT_REVISION":
            revisions.append((m[2], n))
    names = [r for r, _ in revisions] or [path.stem]
    if wanted is not None:
        if wanted not in names:
            raise ProjectError("unknown_revision", wanted_at, [wanted],
                               f"revision {wanted!r} not in {', '.join(names)}")
        return wanted
    if len(names) > 1:
        raise ProjectError("ambiguous_revision", ctx.at(path, revisions[0][1]), names,
                           "several revisions; choose one with --revision")
    return names[0]


def parse_quartus(ctx: Ctx, path: Path, wanted: str | None, wanted_at: str | None) -> None:
    """A .qpf (its revision's .qsf) or a .qsf directly (revision = its name).  `wanted_at`
    locates the choice of revision: the `import` line, or None for --revision."""
    ctx.touch(path)
    if path.suffix == ".qpf":
        revision = qpf_revision(ctx, path, wanted, wanted_at)
        qsf = Path(os.path.normpath(path.parent / f"{revision}.qsf"))
        ctx.resolve(path.parent, qsf.name, ctx.rel(path))
    else:
        revision, qsf = path.stem, path
        if wanted is not None and wanted != revision:
            raise ProjectError("unknown_revision", wanted_at, [wanted],
                               f"{path.name} is revision {revision}")
    parse_qsf(ctx, qsf, revision)


def parse_qsf_entry(ctx: Ctx, path: Path) -> None:
    parse_quartus(ctx, path, ctx.options.get("revision"), None)


# ---- Odin II XML config -----------------------------------------------------------------------


@dataclass
class XElem:
    tag: str
    line: int
    attrs: dict[str, str] = field(default_factory=dict)
    text: list[str] = field(default_factory=list)
    children: list[XElem] = field(default_factory=list)

    @property
    def value(self) -> str:
        return "".join(self.text).strip()


def read_xml(path: Path, ctx: Ctx) -> XElem:
    parser = expat.ParserCreate()
    stack: list[XElem] = [XElem("", 0)]

    def start(tag: str, attrs: dict[str, str]) -> None:
        elem = XElem(tag, parser.CurrentLineNumber, dict(attrs))
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
        raise syntax(ctx.at(path, exc.lineno), str(exc)) from exc
    return stack[0].children[0]


ODIN2_INPUT_TYPES = ("verilog", "blif")


def odin2_inputs(ctx: Ctx, path: Path, elem: XElem) -> None:
    types = [c for c in elem.children if c.tag == "input_type"]
    if len(types) > 1:
        raise syntax(ctx.at(path, types[1].line), "more than one <input_type>")
    lang = types[0].value.lower() if types else "verilog"  # Odin II lower-cases it
    if lang not in ODIN2_INPUT_TYPES:
        raise ProjectError("unsupported_input_type", ctx.at(path, types[0].line),
                           [types[0].value], f"input_type {types[0].value!r}")
    for child in elem.children:
        if child.tag == "input_path_and_name":
            add_file(ctx, path.parent, child.value, lang, "work", ctx.at(path, child.line))
        elif child.tag != "input_type":
            ctx.ignored.add(f"inputs/{child.tag}")


def odin2_output(ctx: Ctx, path: Path, elem: XElem) -> None:
    out: dict[str, Any] = {"path": None, "format": None}
    for child in elem.children:
        if child.tag == "output_type":
            out["format"] = child.value.lower()
        elif child.tag == "output_path_and_name":
            out["path"] = ctx.rel(path.parent / child.value)
        elif child.tag == "target":
            for arch in child.children:
                if arch.tag != "arch_file":
                    ctx.ignored.add(f"output/target/{arch.tag}")
                    continue
                add_arch(ctx, path.parent, arch.value, ctx.at(path, arch.line))
        else:
            ctx.ignored.add(f"output/{child.tag}")
    if out["path"] is not None:
        ctx.record["output"] = out


# Odin II <optimizations> elements that are partial-mapping rules: element -> (attribute,
# rule maker); every other attribute and element is ignored and listed.
def _odin2_mul(v: str, at: str) -> dict[str, Any]:
    return {"rule": "map", "scope": "*", "subject": "$mul", "action": "to",
            "cells": ["multiply"], "min_width": count_of(v, at)}


def _odin2_add(v: str, at: str) -> dict[str, Any]:
    return {"rule": "map", "scope": "*", "subject": "$add", "action": "to", "cells": ["adder"],
            "min_width": count_of(v, at)}


ODIN2_RULES: dict[str, tuple[str, Callable[[str, str], dict[str, Any]]]] = {
    "multiply": ("size", _odin2_mul), "adder": ("threshold_size", _odin2_add),
}


def odin2_memory(ctx: Ctx, elem: XElem, at: str) -> None:
    width = elem.attrs.get("split_memory_width", "0")
    depth = elem.attrs.get("split_memory_depth")
    if width not in ("0", "1") or (depth is not None and depth not in ("min", "max")
                                   and not depth.isdigit()):
        raise syntax(at, "split_memory_width 0|1, split_memory_depth min|max|<n>")
    if width == "1" or depth is not None:
        ctx.record["rules"].append(split_rule("$mem", width == "1", depth))
    ctx.ignored |= {f"optimizations/memory@{a}" for a in elem.attrs
                    if a not in ("split_memory_width", "split_memory_depth")}


def odin2_optimizations(ctx: Ctx, path: Path, elem: XElem) -> None:
    for child in elem.children:
        at = ctx.at(path, child.line)
        if child.tag == "memory":
            odin2_memory(ctx, child, at)
        elif child.tag in ODIN2_RULES:
            attr, make = ODIN2_RULES[child.tag]
            if attr in child.attrs:
                ctx.record["rules"].append(make(child.attrs[attr], at))
            ctx.ignored |= {f"optimizations/{child.tag}@{a}" for a in child.attrs if a != attr}
        else:
            ctx.ignored.add(f"optimizations/{child.tag}")


def parse_odin2(ctx: Ctx, path: Path) -> None:
    ctx.touch(path)
    root = read_xml(path, ctx)
    if root.tag != "config":
        raise syntax(ctx.at(path, root.line), "root must be <config>", root.tag)
    for child in root.children:
        if child.tag == "verilog_files":
            for vf in child.children:
                if vf.tag != "verilog_file":
                    ctx.ignored.add(f"verilog_files/{vf.tag}")
                    continue
                add_file(ctx, path.parent, vf.value, "verilog", "work", ctx.at(path, vf.line))
        elif child.tag == "inputs":
            odin2_inputs(ctx, path, child)
        elif child.tag == "output":
            odin2_output(ctx, path, child)
        elif child.tag == "optimizations":
            odin2_optimizations(ctx, path, child)
        else:
            ctx.ignored.add(child.tag)


PARSERS: dict[str, Callable[[Ctx, Path], None]] = {
    "o3proj": parse_o3proj, "f": parse_f_entry, "qsf": parse_qsf_entry, "odin2": parse_odin2,
}


# ---- source scan: design units, instantiations, includes --------------------------------------


@dataclass
class Ref:
    """An instantiation (or VHDL `use`): unit name, library when the source names one."""

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
    path: str = ""
    refs: list[Ref] = field(default_factory=list)

    @property
    def instantiable(self) -> bool:
        return self.kind != "package"

    @property
    def qualified(self) -> str:
        return f"{self.library}.{self.name}"

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
        self.stack: list[Path] = []

    def find_include(self, name: str, including: Path, at: str) -> Path:
        for d in [including.parent, *self.incdirs]:
            cand = Path(os.path.normpath(d / name))
            if cand.is_file():
                if cand in self.stack:
                    raise ProjectError("include_cycle", at, [name], f"`include cycle at {name}")
                return cand
        raise ProjectError("include_not_found", at, [name], f"`include \"{name}\" not found")

    def directive(self, word: str, arg: str, stack: list[list[bool]]) -> bool | None:
        """Updates the conditional stack; returns whether the line is active (None: not a
        conditional directive)."""
        active = all(s[0] for s in stack)
        if word in ("ifdef", "ifndef"):
            taken = active and ((arg in self.defined) == (word == "ifdef"))
            stack.append([taken, taken or not active])
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

    def lines(self, path: Path) -> Iterator[tuple[str, str]]:
        """(text, file:line) of every active non-directive line, includes expanded."""
        self.read.add(self.ctx.rel(path))
        self.stack.append(path)
        cond: list[list[bool]] = []
        text = strip_comments(path.read_text(encoding="utf-8"))
        for n, line in enumerate(text.split("\n"), 1):
            at = self.ctx.at(path, n)
            m = re.match(r"\s*`(\w+)\s*(.*)", line)
            if m is None:
                if all(s[0] for s in cond):
                    yield line, at
                continue
            word, arg = m[1], m[2].strip()
            first = arg.split()[0] if arg.split() else ""
            if self.directive(word, first, cond) is not None or not all(s[0] for s in cond):
                continue
            if word == "define":
                self.defined.add(first.split("(")[0])
            elif word == "undef":
                self.defined.discard(first)
            elif word == "include":
                name = arg.strip().strip('"<>').strip()
                yield from self.lines(self.find_include(name, path, at))
        self.stack.pop()


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
                raise syntax(at, f"{text} without a name", text)
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


@dataclass
class VhdlFile:
    units: list[Unit]
    needs: list[Ref]  # analysis dependencies: `use lib.pkg`, `entity lib.e` instantiations,
    #                   the entity of an architecture, the package of a package body
    bodies: dict[str, list[Ref]]  # entity name -> instances in its architectures (this file)


def vhdl_units(ctx: Ctx, path: Path, library: str) -> VhdlFile:
    toks = vhdl_tokens(ctx, path)
    units: list[Unit] = []
    needs: list[Ref] = []
    bodies: dict[str, list[Ref]] = {}
    owner: list[Ref] | None = None
    for i, (text, at) in enumerate(toks):
        prev = toks[i - 1][0] if i else ""
        nxt = toks[i + 1][0] if i + 1 < len(toks) else ""
        if text in ("entity", "package") and prev not in (":", "end") and nxt != "body":
            units.append(Unit(nxt, library, text, "vhdl", at))
            owner = None
        elif text == "package" and nxt == "body" and prev != "end" and i + 2 < len(toks):
            needs.append(Ref(toks[i + 2][0], library, at, True))
            owner = None
        elif text == "architecture" and prev != "end" and i + 3 < len(toks):
            owner = bodies.setdefault(toks[i + 3][0], [])
            needs.append(Ref(toks[i + 3][0], library, at, True))
        elif text == "use" and i + 3 < len(toks) and toks[i + 2][0] == ".":
            lib = toks[i + 1][0]
            needs.append(Ref(toks[i + 3][0], library if lib == "work" else lib, at, True))
        elif text == ":" and owner is not None:
            ref = vhdl_ref(toks, i, library)
            if ref is not None:
                owner.append(ref)
                if ref.library is not None:
                    needs.append(ref)
    return VhdlFile(units, needs, bodies)


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
    """Reads source files into design units (defines carry across files, like one Verilog
    compilation unit)."""

    def __init__(self, ctx: Ctx) -> None:
        rec = ctx.record
        self.ctx = ctx
        self.pre = Preprocessor(ctx, rec["incdirs"], rec["defines"])
        self.needs: dict[str, list[Ref]] = {}
        self.bodies: dict[tuple[str, str], list[Ref]] = {}  # (library, entity) -> instances

    @property
    def read(self) -> set[str]:
        return self.pre.read

    def scan(self, rel: str, language: str, library: str) -> list[Unit]:
        path = self.ctx.case_dir / rel
        if language in ("verilog", "systemverilog", "vqm"):
            units = verilog_units(verilog_tokens(self.pre, path), library, language)
        elif language == "vhdl":
            self.pre.read.add(rel)
            vf = vhdl_units(self.ctx, path, library)
            units = vf.units
            self.needs[f"{library}:{rel}"] = vf.needs
            for name, refs in vf.bodies.items():
                self.bodies.setdefault((library, name), []).extend(refs)
        elif language == "blif":
            self.pre.read.add(rel)
            units = blif_units(self.ctx, path, library)
        else:
            raise ProjectError("unsupported_language", None, [rel],
                               f"the reference scan does not read {language}")
        for unit in units:
            unit.path = rel
        return units


def check_duplicates(units: list[Unit]) -> None:
    seen: dict[tuple[str, str], Unit] = {}
    for unit in units:
        first = seen.setdefault(unit.key(), unit)
        if first is not unit:
            raise ProjectError("duplicate_module", unit.at, [unit.name],
                               f"{unit.name} already defined at {first.at}")


def analysis_order(record: dict[str, Any], scanner: Scanner, design: list[Unit]) -> list[str]:
    """Files in analysis order: listing order, except that a VHDL file comes after the files
    defining the packages and entities it names (stable topological order)."""
    files = [f"{f['library']}:{f['path']}" for f in record["files"]]
    deps: dict[str, set[str]] = {}
    for key in files:
        deps[key] = set()
        for ref in scanner.needs.get(key, []):
            deps[key] |= {f"{u.library}:{u.path}" for u in design if u.matches(ref)} - {key}
    order: list[str] = []
    while len(order) < len(files):
        ready = next((k for k in files if k not in order and deps[k] <= set(order)), None)
        if ready is None:
            left = [k.partition(":")[2] for k in files if k not in order]
            raise ProjectError("dependency_cycle", None, left, "VHDL dependency cycle")
        order.append(ready)
    return [k.partition(":")[2] for k in order]


def select_top(record: dict[str, Any], design: list[Unit], top_at: str | None) -> Unit:
    if record["top"] is not None:
        lib, _, name = record["top"].rpartition(".")
        ref = Ref(name, lib or None, "", from_vhdl=False)
        found = [u for u in design if u.instantiable and u.matches(ref)]
        if not found:
            raise ProjectError("unknown_top", top_at, [record["top"]], "top not found")
        if len(found) > 1:
            names = sorted(u.qualified for u in found)
            raise ProjectError("ambiguous_top", top_at, names, "top in several libraries")
        return found[0]
    if all(f["language"] == "blif" for f in record["files"]) and design:
        return design[0]  # BLIF: the first model of the first file is the top
    refs = [r for u in design for r in u.refs]
    cands = [u for u in design if u.instantiable and not any(u.matches(r) for r in refs)]
    if len(cands) == 1:
        return cands[0]
    names = sorted(u.qualified for u in cands)
    if not cands:
        raise ProjectError("no_top", None, [], "no top candidate: every module is instantiated")
    raise ProjectError("ambiguous_top", None, names, "several top candidates: " + ", ".join(names))


# Instances that are not project units: Altera device primitives in a VQM netlist (the VQM
# reader's library, Phase 6), and Altera megafunctions in RTL (the Phase 2 primitive library).
VQM_PRIMITIVES = ("dffeas", "cyclonev_lcell_comb", "cyclonev_io_ibuf", "cyclonev_io_obuf",
                  "cycloneive_lcell_comb", "cycloneive_io_ibuf", "cycloneive_io_obuf")
RTL_PRIMITIVE = re.compile(r"altsyncram|altdpram|altshift_taps|altmult_add|lpm_[a-z_]+")


def is_primitive(name: str, language: str) -> bool:
    if language == "vqm":
        return name in VQM_PRIMITIVES
    return RTL_PRIMITIVE.fullmatch(name.lower() if language == "vhdl" else name) is not None


class Elaborator:
    """Walks the hierarchy from the top: design units first, then -v library files (first
    file wins), then -y directories (first directory, then first extension, wins); library
    modules load only when instantiated and never shadow design units."""

    def __init__(self, scanner: Scanner, design: list[Unit]) -> None:
        self.scanner = scanner
        self.design = design
        rec = scanner.ctx.record
        self.lib: list[Unit] = []
        for lf in rec["libfiles"]:
            self.lib += scanner.scan(lf["path"], lf["language"], "work")
        self.libdirs: list[str] = rec["libdirs"]
        self.libext: list[str] = rec["libext"] or DEFAULT_LIBEXT

    def from_libdirs(self, ref: Ref) -> Unit | None:
        for d in self.libdirs:
            for ext in self.libext:
                rel = Path(os.path.normpath(Path(d) / (ref.name + ext))).as_posix()
                if (self.scanner.ctx.case_dir / rel).is_file():
                    self.lib += self.scanner.scan(rel, EXT_LANGUAGE[ext], "work")
                    self.scanner.ctx.touched.add(rel)
                    return next((u for u in self.lib if u.matches(ref)), None)
        return None

    def find(self, ref: Ref, owner: Unit) -> Unit | None:
        found = [u for u in self.design if u.instantiable and u.matches(ref)]
        found.sort(key=lambda u: u.library != owner.library)
        unit = (found[0] if found else
                next((u for u in self.lib if u.matches(ref)), None) or self.from_libdirs(ref))
        if unit is None and not is_primitive(ref.name, owner.language):
            raise ProjectError("unresolved_module", ref.at, [ref.name],
                               f"no module or entity {ref.name!r}")
        return unit

    def walk(self, top: Unit) -> list[Unit]:
        reached: list[Unit] = []
        work = [top]
        while work:
            unit = work.pop()
            if any(unit is r for r in reached):
                continue
            reached.append(unit)
            work += [u for u in (self.find(ref, unit) for ref in unit.refs) if u is not None]
        return reached


def resolve(ctx: Ctx) -> dict[str, Any]:
    """Top selection and hierarchy resolution over the parsed record."""
    scanner = Scanner(ctx)
    try:
        return resolve_with(ctx, scanner)
    finally:
        ctx.touched |= scanner.read  # files read before an error count as used


def resolve_with(ctx: Ctx, scanner: Scanner) -> dict[str, Any]:
    design: list[Unit] = []
    for f in ctx.record["files"]:
        design += scanner.scan(f["path"], f["language"], f["library"])
    check_duplicates(design)
    for unit in design:  # an architecture may sit in another file than its entity
        if unit.kind == "entity":
            unit.refs = scanner.bodies.get((unit.library, unit.name), [])
    order = analysis_order(ctx.record, scanner, design)
    elab = Elaborator(scanner, design)
    top = select_top(ctx.record, design, ctx.top_at)
    reached = elab.walk(top)
    return {"top": top.qualified, "units": sorted(u.qualified for u in reached),
            "read": sorted(scanner.read), "order": order}


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


def load_project(case_dir: Path, fmt: str, entry: Path, env: dict[str, str] | None = None,
                 options: dict[str, str] | None = None) -> Result:
    ctx = Ctx(case_dir, env, options)
    entry = Path(os.path.abspath(entry))
    res = Result()
    try:
        with elsewhere():
            if not entry.is_file():
                raise ProjectError("missing_file", None, [ctx.rel(entry)], "project not found")
            PARSERS[fmt](ctx, entry)
            ctx.finish()
            res.record = ctx.record
            res.resolved = resolve(ctx)
    except ProjectError as exc:
        res.error = exc
    res.touched = ctx.touched
    return res


EXPECTED_KEYS = {"description", "features", "phase", "formats", "format_phase", "entry", "env",
                 "options", "project", "format_overrides", "resolved", "oracle",
                 "oracle_reason", "error", "unused"}


def format_schema(exp: dict[str, Any]) -> list[str]:
    fmts = exp.get("formats", [])
    probs = []
    if not fmts or len(set(fmts)) != len(fmts) or not set(fmts) <= set(FORMATS):
        probs.append(f"bad formats {fmts!r}")
    if exp.get("phase") not in PHASES:
        probs.append(f"phase must be one of {PHASES}")
    fphase = exp.get("format_phase", {})
    if ("odin2" in fmts) != ("odin2" in fphase) or any(
            v != ODIN2_READER for v in fphase.values()):
        probs.append(f'format_phase must be {{"odin2": "{ODIN2_READER}"}} exactly when odin2 '
                     "is a format")
    for fmt, over in exp.get("format_overrides", {}).items():
        if fmt not in fmts or not set(over) <= set(OVERRIDABLE.get(fmt, ())):
            probs.append(f"format_overrides for {fmt!r} may only hold "
                         f"{', '.join(OVERRIDABLE.get(fmt, ())) or 'nothing'}")
    if not set(exp.get("options", {})) <= {"qsf"} or not set(
            exp.get("options", {}).get("qsf", {})) <= {"revision"}:
        probs.append('options may only be {"qsf": {"revision": ...}}')
    return probs


def schema_problems(exp: dict[str, Any]) -> list[str]:
    probs = [f"unknown key {k!r}" for k in sorted(set(exp) - EXPECTED_KEYS)]
    probs += [f"missing key {k!r}" for k in ("description", "features", "phase", "formats")
              if k not in exp]
    probs += format_schema(exp)
    if "error" in exp:
        probs += [f"negative case has {k!r}" for k in ("project", "resolved", "oracle")
                  if k in exp]
        return probs
    probs += [f"missing key {k!r}" for k in ("project", "resolved", "oracle") if k not in exp]
    if "project" in exp and list(exp["project"]) != list(RECORD_KEYS):
        probs.append(f"project keys must be exactly {', '.join(RECORD_KEYS)} in order")
    if exp.get("oracle") not in (*ORACLES, None):
        probs.append(f"oracle must be one of {ORACLES} or null")
    if exp.get("oracle") in (None, "hand+yosys") and not exp.get("oracle_reason"):
        probs.append("oracle null or hand+yosys needs an oracle_reason")
    return probs


def expected_error(exp: dict[str, Any], fmt: str) -> dict[str, Any]:
    err = dict(exp["error"])
    if isinstance(err.get("at"), dict):
        err["at"] = err["at"].get(fmt)
    return err


def format_problems(fmt: str, exp: dict[str, Any], res: Result) -> list[str]:
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


def is_meta(rel: str) -> bool:
    return rel in CASE_META or rel.startswith("ref/")


def oracle_file_problems(case_dir: Path, exp: dict[str, Any]) -> list[str]:
    probs: list[str] = []
    has_oracle = "error" not in exp and exp.get("oracle") is not None
    for meta in ("ref.blif", "ref.cmd"):
        if (case_dir / meta).is_file() != has_oracle:
            probs.append(f"{meta} {'missing' if has_oracle else 'not expected'}")
    if not has_oracle or not (case_dir / "ref.cmd").is_file():
        return probs
    cmd = (case_dir / "ref.cmd").read_text(encoding="utf-8")
    top = exp["resolved"]["top"].rpartition(".")[2]
    if f"-top {top}" not in cmd:
        probs.append(f"ref.cmd does not name top {top!r}")
    if not cmd.startswith("# Oracle") or "\n# versions: " not in cmd:
        probs.append("ref.cmd lacks the '# Oracle' header or the '# versions:' line")
    probs += [f"{rel} is not named in ref.cmd" for rel in case_files(case_dir)
              if rel.startswith("ref/") and rel not in cmd]
    if ('"$GHDL"' in cmd) != exp["oracle"].startswith("ghdl") or (
            "ref/" in cmd) != (exp["oracle"] == "hand+yosys"):
        probs.append(f"ref.cmd does not match oracle {exp['oracle']!r}")
    return probs


def file_problems(case_dir: Path, exp: dict[str, Any], touched: set[str]) -> list[str]:
    probs: list[str] = []
    for fmt in set(FORMATS) - set(exp["formats"]) - {"qsf"}:
        if (case_dir / DEFAULT_ENTRY[fmt]).exists():
            probs.append(f"{DEFAULT_ENTRY[fmt]} present but {fmt} is not in formats")
    unused = set(exp.get("unused", []))
    for rel in case_files(case_dir):
        if is_meta(rel):
            continue
        if rel in unused and rel in touched:
            probs.append(f"{rel} is listed as unused but a format used it")
        elif rel not in unused and rel not in touched:
            probs.append(f"{rel} is not used by any format")
    probs += [f"unused {rel} does not exist" for rel in sorted(unused)
              if not (case_dir / rel).is_file()]
    return probs + oracle_file_problems(case_dir, exp)


def run_format(case_dir: Path, fmt: str, exp: dict[str, Any]) -> Result:
    opts = exp.get("options", {}).get(fmt)
    return load_project(case_dir, fmt, entry_of(case_dir, fmt, exp), exp.get("env", {}), opts)


def check_case(case_dir: Path) -> tuple[list[str], dict[str, Any]]:
    try:
        exp: dict[str, Any] = json.loads((case_dir / "expected.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"expected.json: {exc}"], {}
    probs = schema_problems(exp)
    if probs:
        return probs, exp
    touched: set[str] = set()
    for fmt in exp["formats"]:
        res = run_format(case_dir, fmt, exp)
        touched |= res.touched
        probs += format_problems(fmt, exp, res)
    return probs + file_problems(case_dir, exp, touched), exp


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


def summary(exps: list[dict[str, Any]]) -> list[str]:
    """Counts per phase: records (every format but odin2) are Phase 2 for every case;
    elaboration and the oracle are due in the case's phase; odin2 when its reader lands."""
    lines = []
    per_fmt = {f: sum(f in e.get("formats", []) for e in exps) for f in FORMATS}
    lines.append("records (Phase 2): " + ", ".join(
        f"{f} {per_fmt[f]}" for f in FORMATS if f != "odin2")
        + f"; odin2 {per_fmt['odin2']} (when the Odin II reader lands)")
    for phase in PHASES:
        cases = [e for e in exps if e.get("phase") == phase]
        neg = sum("error" in e for e in cases)
        lines.append(f"elaborate in Phase {phase}: {len(cases)} cases "
                     f"({len(cases) - neg} positive, {neg} negative)")
    return lines


def cmd_check(args: argparse.Namespace) -> int:
    failed = 0
    dirs = case_dirs(args.root, args.case)
    exps = []
    for d in dirs:
        probs, exp = check_case(d)
        exps.append(exp)
        failed += bool(probs)
        print(f"{'FAIL' if probs else 'ok  '} {d.name}")
        for p in probs:
            print(f"     {p}")
    for line in summary(exps):
        print(f"project-fixtures: {line}")
    print(f"project-fixtures: {len(dirs) - failed}/{len(dirs)} cases ok")
    return 1 if failed else 0


def cmd_parse(args: argparse.Namespace) -> int:
    case_dir = Path(args.case_dir)
    exp: dict[str, Any] = {}
    if (case_dir / "expected.json").is_file():
        exp = json.loads((case_dir / "expected.json").read_text(encoding="utf-8"))
    entry = Path(args.entry) if args.entry else entry_of(case_dir, args.format, exp)
    opts = exp.get("options", {}).get(args.format, {})
    if args.revision:
        opts = {"revision": args.revision}
    res = load_project(case_dir, args.format, entry, exp.get("env", dict(os.environ)), opts)
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


def tool_versions(cmd: str, yosys: str, ghdl: str) -> str:
    """The '# versions:' line for a ref.cmd: Yosys, and GHDL when it runs."""
    found = []
    for tool, argv in (("$YOSYS", [yosys, "-V"]), ("$GHDL", [ghdl, "--version"])):
        if tool in cmd:
            try:
                out = subprocess.run(argv, capture_output=True, text=True, check=False,
                                     timeout=60).stdout
            except OSError as exc:
                raise UsageError(f"{argv[0]}: {exc}") from exc
            found.append(out.splitlines()[0].strip() if out else f"{argv[0]}: no version")
    return "# versions: " + " | ".join(found)


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


def oracle_case(d: Path, args: argparse.Namespace) -> str:
    cmd_path = d / "ref.cmd"
    cmd = cmd_path.read_text(encoding="utf-8")
    versions = tool_versions(cmd, args.yosys, args.ghdl)
    old_line = next((ln for ln in cmd.splitlines() if ln.startswith("# versions:")), None)
    new, log = run_oracle(d, args.yosys, args.ghdl)
    if new is None:
        print("\n".join(f"     {line}" for line in log.splitlines()[-20:]))
        return "FAILED"
    ref = d / "ref.blif"
    if ref.is_file() and ref.read_bytes() == new and old_line == versions:
        return "same"
    if args.write:
        ref.write_bytes(new)
        lines = [ln for ln in cmd.splitlines() if not ln.startswith("# versions:")]
        lines.insert(2, versions)
        cmd_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return "written"
    if old_line != versions:
        print(f"     ref.cmd has {old_line!r}; running {versions!r}")
        return "VERSION"
    return "DIFFERS" if ref.is_file() else "MISSING"


def cmd_oracle(args: argparse.Namespace) -> int:
    cases = []
    for d in case_dirs(args.root, args.case):
        exp = json.loads((d / "expected.json").read_text(encoding="utf-8"))
        if "error" not in exp and exp.get("oracle") is not None:
            cases.append(d)
    cases.sort(key=lambda d: (source_bytes(d), d.name))  # smallest first
    bad = 0
    for d in cases:
        status = oracle_case(d, args)
        bad += status not in ("same", "written")
        print(f"{status:8} {d.name}")
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
    p.add_argument("--revision")
    args = ap.parse_args(argv)
    commands = {"check": cmd_check, "parse": cmd_parse, "oracle": cmd_oracle}
    try:
        return commands[args.cmd](args)
    except (UsageError, OSError, ValueError) as exc:
        print(f"project-fixtures: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
