"""loc-check: HDL locations through write and read-back, per format (5.4 spec T-3, section 5).

Usage::

    loc-check --level word|gate|lut|lpm[,LEVEL...] [--odin3 O] [--scripts DIR] [--expect F]
              [--techlib-list F] [--lib L] [--work DIR] [--smallest-first] [--verbose] DESIGN.v...

Per design and level, Odin III runs the level script (``tests/loc/<level>.script``), lists the
design with ``dump_locs`` (before), writes every format of the level (word and LPM: JSON and
Verilog; gate and LUT: also BLIF VTR with provenance names, and plain BLIF with ``--src-attr``),
and per format reads the file back with the 5.3/5.4 options (``read_json``, ``read_verilog
-netlist -icells``, ``read_blif --hdl-locs``) and lists it again (after). The two listings are
compared per kind (module, wire, net, node). Objects are paired by ``(module, kind, name)`` where
both sides have a name that is not generated (``$c12``, ``$n3``); every other object is paired in a
pool per (module, kind), exact location list first, then first file and line. The reader renumbers
generated names, so no other pairing exists. Columns:

``objects``  live objects before; ``had`` objects with an HDL location before;
``same``     had objects with the full location list equal after;
``line``     had objects with only the first location's file and line equal;
``inherited`` BLIF VTR cells with a location after (the dialect writes none for a cell: D-5);
``lost``     had objects with none after, each assigned to a class X-1 to X-8 by RULES (spec
             section 5: the object has no carrier in that format), else ``unclassified``;
``gained``   an object with none before that has one after (a pool counts the surplus); a net
             with a declared wire's location is `inherited` (D-2, D-3), an object with its
             module's own location is `modgained` (a constant driver the reader makes, X-9:
             reported, not a failure);
``spec``     median and p90 of the distinct (file, line) pairs per had object, before and after
             (after: the had objects that kept a location).

The run fails on: ``gained`` > 0; a lost object whose (format, kind) has no class; ``line`` where
the format is lossless (LINE_OK lists the others); no object with a location compared. ``--expect
F`` lists the designs a level cannot take (``<level> <design> <outcome> <reason>``, outcome
``error:script``, ``error:read`` or ``fail``); a listed pair that passes, and an unlisted failure,
fail the run. The last line is ``loc-check: compared N of M (X excepted)``.

Exit codes: 0 ok; 1 a rule failed or an expect line is stale; 2 usage or tool error; 3 a child
crashed or timed out.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from collections.abc import Collection, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

TOOL = "loc-check"
ROOT = Path(__file__).resolve().parents[2]
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH = 0, 1, 2, 3
TIMEOUT = 900.0
LEVELS = ("word", "gate", "lut", "lpm")
FORMATS = ("json", "verilog", "blif", "plain")
LEVEL_FORMATS = {
    "word": ("json", "verilog"),
    "gate": FORMATS,
    "lut": FORMATS,
    "lpm": ("verilog", "json"),
}
KINDS = ("module", "wire", "net", "node")
OUTCOMES = ("error:script", "error:read", "fail")
CAP = 8  # W-3: at most 8 locations in a record

# Spec section 5: the class of a lost object by (format, kind, named). "named" is a name that is
# not generated. A missing entry means the format keeps the location (lossless): a loss is a bug.
#   X-1 no carrier: a net with no netname (JSON), declaration (Verilog) or name (BLIF); the BLIF
#       dialects have no per-model record, so a module is the same case
#   X-6 BLIF VTR cells carry no location; X-7 BLIF plain nets keep their own names
#   X-8 BLIF names bits, not wires: the only wires it names are ports, which carry no location
RULES: dict[tuple[str, str, bool], str] = {
    ("json", "net", False): "X-1",
    ("verilog", "net", False): "X-1",
    ("blif", "module", False): "X-1",
    ("blif", "module", True): "X-1",
    ("blif", "wire", False): "X-8",
    ("blif", "wire", True): "X-8",
    ("blif", "node", False): "X-6",
    ("blif", "node", True): "X-6",
    ("blif", "net", False): "X-1",
    ("plain", "module", False): "X-1",
    ("plain", "module", True): "X-1",
    ("plain", "wire", False): "X-8",
    ("plain", "wire", True): "X-8",
    ("plain", "net", False): "X-1",
    ("plain", "net", True): "X-7",
}
# Where a match of the first location's file and line without the rest is the format (not a bug):
# X-2, an `assign` carries `// file:line` only, for the cell and for the net it drives (the
# reader gives that net the cell's comment); and a BLIF VTR net name `hier/short@file:line`
# carries the line only (W-3: names and the Verilog comment take the first location, line only).
LINE_OK = {("verilog", "node"): "X-2", ("verilog", "net"): "X-2", ("blif", "net"): "name"}
LINE_POOL = {*LINE_OK, ("blif", "node")}  # the pools that may pair by file and line
GENERATED = re.compile(r"^\$[cnwm]\d+$")
LOC_RE = re.compile(r"^(.*):(\d+)(?:\.\d+(?:-\d+\.\d+)?)?$")
TOP_RE = re.compile(r"read_verilog: elaborated top '([^']+)'")


class ToolError(Exception):
    """A bad argument or an unusable tool (exit 2)."""


class ScriptError(ToolError):
    """The level script failed on a design (outcome ``error:script``)."""


class CrashError(Exception):
    """A child crashed or timed out (exit 3)."""


# --- the listing -----------------------------------------------------------------------------


@dataclass(frozen=True)
class Obj:
    """One line of ``dump_locs``: module, kind, name and the SRC text ('' for none)."""

    module: str
    kind: str
    name: str
    src: str
    ctype: str = ""  # a node's cell type name (`dump_locs --types`), else ""


@dataclass(frozen=True)
class Loc:
    file: str
    line: int


def unescape(text: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            out.append("\n" if text[i + 1] == "n" else text[i + 1])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def parse_dump(text: str) -> list[Obj]:
    """The objects of a ``dump_locs`` listing: ``module|kind|name|SRC``, the first three fields
    with ``\\\\``, ``\\|`` and ``\\n`` escapes; SRC is the rest of the line."""
    rows: list[Obj] = []
    for number, line in enumerate(text.splitlines(), 1):
        fields: list[str] = []
        cur: list[str] = []
        i = 0
        while i < len(line) and len(fields) < 3:
            ch = line[i]
            if ch == "\\" and i + 1 < len(line):
                cur.append(line[i : i + 2])
                i += 2
            elif ch == "|":
                fields.append("".join(cur))
                cur = []
                i += 1
            else:
                cur.append(ch)
                i += 1
        if len(fields) < 3:
            raise ToolError(f"dump_locs line {number} has fewer than four fields: {line[:80]!r}")
        kind, _, ctype = fields[1].partition(":")
        rows.append(Obj(unescape(fields[0]), kind, unescape(fields[2]), line[i:], unescape(ctype)))
    return rows


def parse_locs(src: str) -> list[Loc]:
    """The locations of a SRC text (L-1, L-2): file and line of each ``|`` part."""
    locs: list[Loc] = []
    for part in src.split("|") if src else []:
        match = LOC_RE.match(part)
        if match:
            locs.append(Loc(match.group(1), int(match.group(2))))
    return locs


def is_generated(name: str) -> bool:
    return bool(GENERATED.match(name))


# --- the comparison --------------------------------------------------------------------------


CONST_TYPES = frozenset({"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"})
PORT_TYPES = frozenset({"$port_in", "$port_out", "$port_inout"})
BLIF_FORMATS = ("blif", "plain")
ABSENT_FORMATS = ("verilog", "blif")  # the formats that may leave a module out of the file
SPLIT_FORMATS = ("verilog",)  # the reader splits an `assign` expression into several objects
OWN_BIT = re.compile(r"\[\d+\]$")


@dataclass(frozen=True)
class Carrier:
    """What the written files say about one module: the cells of its JSON file that only copy a
    net (a one-input identity LUT or buffer), and the (file, line) of every pure alias `assign`
    in its Verilog file (a statement the reader reads as an alias, not as a cell)."""

    buffers: frozenset[str] = frozenset()
    aliases: frozenset[tuple[str, int]] = frozenset()


Written = dict[str, Carrier]


@dataclass(frozen=True)
class Allow:
    """What the expect file lists for one design, level and format: the modules the file may
    leave out (``absent``) and the exact number of objects that take their module's location
    (``modgained``)."""

    absent: frozenset[str] = frozenset()
    modgained: int = 0


NO_ALLOW = Allow()


@dataclass
class Tally:
    """The columns of one kind (one design, or a sum)."""

    objects: int = 0
    had: int = 0
    same: int = 0
    line: int = 0
    inherited: int = 0
    lost: dict[str, int] = field(default_factory=dict)
    unclassified: int = 0
    wrong: int = 0
    gained: int = 0
    modgained: int = 0
    split: int = 0
    capped: int = 0
    absent: list[str] = field(default_factory=list)
    lines_before: list[int] = field(default_factory=list)
    lines_after: list[int] = field(default_factory=list)

    def add(self, other: Tally) -> None:
        for name in (
            "objects",
            "had",
            "same",
            "line",
            "inherited",
            "unclassified",
            "wrong",
            "gained",
            "modgained",
            "split",
        ):
            setattr(self, name, getattr(self, name) + getattr(other, name))
        self.capped += other.capped
        for cls, count in other.lost.items():
            self.lost[cls] = self.lost.get(cls, 0) + count
        self.absent += other.absent
        self.lines_before += other.lines_before
        self.lines_after += other.lines_after


def distinct_lines(locs: Sequence[Loc]) -> int:
    return len({(x.file, x.line) for x in locs})


@dataclass(frozen=True)
class Scope:
    """What a pairing needs: the format, the kind, the located wires of the module (name -> SRC),
    what its written files say (None: not known), whether the read-back has the module, whether
    the expect file lets it be absent, and the module's own SRC."""

    fmt: str
    kind: str
    wires: Mapping[str, str]
    carrier: Carrier | None
    present: bool = True
    absent_ok: bool = False
    module_src: str = ""
    before_lines: frozenset[tuple[str, int]] = frozenset()


def node_class(scope: Scope, before: Obj, by_rule: str | None) -> str | None:
    """The class of a lost cell, decided by its type in the design before writing (never by
    whether a file has it): a port cell has no carrier in the BLIF dialects (X-8; JSON and
    Verilog restore a port through its netname or declaration: a loss there is a bug); a constant
    driver is folded into a literal by every writer (X-9); an identity buffer is X-9 in Verilog
    only when the written file has it as a pure alias `assign` at its location."""
    if before.ctype in PORT_TYPES:
        return "X-8" if scope.fmt in BLIF_FORMATS else by_rule
    if before.ctype in CONST_TYPES:
        return "X-9"
    mine = parse_locs(before.src)
    carrier = scope.carrier
    if scope.fmt == "verilog" and carrier is not None and mine:
        if before.name in carrier.buffers and (mine[0].file, mine[0].line) in carrier.aliases:
            return "X-9"
    return by_rule


def lose(scope: Scope, before: Obj, tally: Tally) -> None:
    """Counts a lost object (it has no location after) under its class by RULES (spec section 5:
    the object has no carrier in that format), else ``unclassified``. A cell is classed by its
    type (node_class). An object of a module the read-back has no trace of is X-1 only where the
    expect file lists that module as absent."""
    cls = RULES.get((scope.fmt, scope.kind, not is_generated(before.name)))
    if not scope.present and scope.absent_ok:
        cls = "X-1"
    elif scope.kind == "node":
        cls = node_class(scope, before, cls)
    if cls is None:
        tally.unclassified += 1
    else:
        tally.lost[cls] = tally.lost.get(cls, 0) + 1


def gain_kind(scope: Scope, after: Obj) -> str:
    """How an object with no location before has one after: "module" for the module's own
    location (a constant driver the reader makes: X-9, `modgained`); "wire" for a named net that
    takes the location of its own declared wire (the readers give a net its netname's or
    declaration's location, D-2 and D-3: `inherited`); "split" for a Verilog object with the file
    and line of a located object before (the reader splits an `assign` expression into several
    cells and nets, all with its line); else "" (`gained`)."""
    if scope.module_src and after.src == scope.module_src:
        return "module"
    if scope.kind == "net" and not is_generated(after.name):
        if scope.wires.get(OWN_BIT.sub("", after.name)) == after.src:
            return "wire"
    first = parse_locs(after.src)
    if scope.fmt in SPLIT_FORMATS and first and (first[0].file, first[0].line) in scope.before_lines:
        return "split"
    return ""


def count_gain(scope: Scope, after: Obj, tally: Tally) -> None:
    kind_of = gain_kind(scope, after)
    tally.inherited += kind_of == "wire"
    tally.modgained += kind_of == "module"
    tally.split += kind_of == "split"
    tally.gained += kind_of == ""


def judge_pair(scope: Scope, before: Obj, after: Obj | None, tally: Tally) -> None:
    """Counts one paired object: before has a location or not, after is its read-back (or None).
    No location after is a loss (classed or not); a location that differs is wrong, never classed."""
    fmt, kind = scope.fmt, scope.kind
    mine = parse_locs(before.src)
    inherits = fmt == "blif" and kind == "node"  # counted as `inherited`, not as same or line
    if not mine:
        if after is not None and after.src and not inherits:
            count_gain(scope, after, tally)
        return
    theirs = parse_locs(after.src) if after is not None else []
    if not theirs:
        lose(scope, before, tally)
    elif after is not None and after.src == before.src:
        tally.same += not inherits
        tally.lines_after.append(distinct_lines(theirs))
    elif (theirs[0].file, theirs[0].line) == (mine[0].file, mine[0].line):
        tally.line += not inherits
        tally.lines_after.append(distinct_lines(theirs))
    else:
        tally.wrong += 1


def pool_pairs(
    group_b: list[Obj], group_a: list[Obj], by_line: bool
) -> tuple[list[tuple[Obj, Obj | None]], list[Obj]]:
    """Pairs the objects with no name partner: exact location list, then (by_line, for the formats
    that keep a line only) first file and line.
    Returns (before object, after object or None) for every had object, and the after objects
    with a location that nobody took."""
    pairs: list[tuple[Obj, Obj | None]] = []
    free: dict[str, list[Obj]] = {}
    for obj in group_a:
        if obj.src:
            free.setdefault(obj.src, []).append(obj)
    rest: list[Obj] = []
    for obj in (o for o in group_b if o.src):
        if free.get(obj.src):
            pairs.append((obj, free[obj.src].pop()))
        else:
            rest.append(obj)
    lines: dict[tuple[str, int], list[Obj]] = {}
    for objs in free.values() if by_line else []:
        for obj in objs:
            first = parse_locs(obj.src)
            if first:
                lines.setdefault((first[0].file, first[0].line), []).append(obj)
    for obj in rest:
        first = parse_locs(obj.src)
        key = (first[0].file, first[0].line) if first else ("", 0)
        pairs.append((obj, lines[key].pop() if lines.get(key) else None))
    taken = {id(a) for _, a in pairs if a is not None}
    return pairs, [obj for objs in free.values() for obj in objs if id(obj) not in taken]


def compare_group(scope: Scope, before: list[Obj], after: list[Obj], tally: Tally) -> None:
    """One (module, kind): pair by name where both names are real, the rest in a pool."""
    fmt, kind = scope.fmt, scope.kind
    pool_all = fmt == "blif" and kind == "node"  # the VTR dialect names no cell
    names_a = {} if pool_all else {o.name: o for o in after if not is_generated(o.name)}
    left_b: list[Obj] = []
    taken: set[str] = set()
    for obj in before:
        partner = None if pool_all or is_generated(obj.name) else names_a.get(obj.name)
        if partner is not None:
            taken.add(obj.name)
            judge_pair(scope, obj, partner, tally)
        else:
            left_b.append(obj)
    left_a = [o for o in after if o.name not in taken or is_generated(o.name) or pool_all]
    # a cell with no carrier (a port or constant driver) takes what is left (stable sort)
    left_b.sort(key=lambda o: o.ctype in PORT_TYPES or o.ctype in CONST_TYPES)
    pairs, surplus = pool_pairs(left_b, left_a, (fmt, kind) in LINE_POOL)
    for obj, partner in pairs:
        judge_pair(scope, obj, partner, tally)
    if pool_all:
        tally.inherited += sum(1 for o in after if o.src)
    else:
        for obj in surplus:  # every located object nobody took counts: none is capped
            count_gain(scope, obj, tally)


def first_lines(objs: Sequence[Obj]) -> frozenset[tuple[str, int]]:
    """The file and line of the first location of every located object."""
    out: set[tuple[str, int]] = set()
    for obj in objs:
        locs = parse_locs(obj.src)
        if locs:
            out.add((locs[0].file, locs[0].line))
    return frozenset(out)


def compare(
    fmt: str,
    before: list[Obj],
    after: list[Obj],
    written: Written | None = None,
    allow: Allow = NO_ALLOW,
) -> dict[str, Tally]:
    """The tally of every kind present, for one design at one level in one format. ``written``:
    module -> what its written files say (the classes X-9); ``allow``: the modules the read-back
    may lack."""
    out: dict[str, Tally] = {}
    groups_b: dict[tuple[str, str], list[Obj]] = {}
    groups_a: dict[tuple[str, str], list[Obj]] = {}
    for obj in before:
        groups_b.setdefault((obj.module, obj.kind), []).append(obj)
    modules_after = {o.module for o in after}
    for obj in after:
        groups_a.setdefault((obj.module, obj.kind), []).append(obj)
    for module in sorted({o.module for o in before} - modules_after):
        out.setdefault("module", Tally()).absent.append(module)
    for key in sorted(set(groups_b) | set(groups_a)):
        module, kind = key
        tally = out.setdefault(kind, Tally())
        group_b = groups_b.get(key, [])
        tally.objects += len(group_b)
        for obj in group_b:
            locs = parse_locs(obj.src)
            if locs:
                tally.had += 1
                tally.lines_before.append(distinct_lines(locs))
                tally.capped += len(locs) >= CAP
        wires = {o.name: o.src for o in groups_b.get((module, "wire"), []) if o.src}
        mods = [o.src for o in groups_b.get((module, "module"), [])]
        seen = first_lines(groups_b.get((module, "node"), []) + groups_b.get((module, "net"), []))
        scope = Scope(
            fmt,
            kind,
            wires,
            written.get(module) if written else None,
            module in modules_after,
            module in allow.absent,
            mods[0] if mods else "",
            seen,
        )
        compare_group(scope, group_b, groups_a.get(key, []), tally)
    return out


def merge_tallies(runs: Sequence[dict[str, Tally]]) -> dict[str, Tally]:
    total: dict[str, Tally] = {}
    for run in runs:
        for kind, tally in run.items():
            total.setdefault(kind, Tally()).add(tally)
    return total


def specificity(values: Sequence[int]) -> tuple[float, float]:
    """(median, 90th percentile by nearest rank) of a list; (0, 0) when empty."""
    if not values:
        return 0.0, 0.0
    ordered = sorted(values)
    n = len(ordered)
    median = (ordered[(n - 1) // 2] + ordered[n // 2]) / 2
    return median, float(ordered[math.ceil(0.9 * n) - 1])


def kind_problems(fmt: str, kind: str, t: Tally) -> list[str]:
    """The rule failures of one kind (spec T-3)."""
    out: list[str] = []
    if t.unclassified:
        out.append(f"{kind}: {t.unclassified} lost with no class (the format keeps them)")
    if t.wrong:
        out.append(f"{kind}: {t.wrong} with a different location (present, but not the original)")
    if t.gained:
        out.append(f"{kind}: {t.gained} gained a location")
    if t.line and (fmt, kind) not in LINE_OK:
        out.append(f"{kind}: {t.line} line-only where the format is lossless")
    located = t.same + t.line + t.gained + t.inherited + t.modgained + t.split
    if fmt in BLIF_FORMATS and kind in ("wire", "module") and located:
        out.append(
            f"{kind}: {located} carry a location in a BLIF dialect; the rule that BLIF has "
            "none (RULES X-1, X-8) is out of date"
        )
    return out


def allow_problems(tallies: dict[str, Tally], allow: Allow) -> list[str]:
    """Modules absent from the read-back and objects that took their module's location, against
    what the expect file lists (an entry that no longer applies is stale)."""
    absent = {m for t in tallies.values() for m in t.absent}
    out = [f"module {m} is absent from the read-back (not in the expect file)"
           for m in sorted(absent - allow.absent)]
    out += [f"STALE absent entry for module {m}: the read-back has it"
            for m in sorted(allow.absent - absent)]
    modgained = sum(t.modgained for t in tallies.values())
    if modgained != allow.modgained:
        out.append(
            f"{modgained} objects took their module's location, the expect file lists "
            f"{allow.modgained}"
        )
    return out


def check_tallies(fmt: str, tallies: dict[str, Tally], allow: Allow = NO_ALLOW) -> list[str]:
    """The rule failures of one design in one format (spec T-3)."""
    problems: list[str] = []
    if sum(t.had for t in tallies.values()) == 0:
        problems.append("the comparison matched no objects with a location")
    for kind in KINDS:
        if kind in tallies:
            problems += kind_problems(fmt, kind, tallies[kind])
    return problems + allow_problems(tallies, allow)


# --- the expect file, the pins and the scripts ---------------------------------------------------


@dataclass
class Expect:
    """The expect file: designs a level cannot take, modules a read-back may lack, and the
    objects that take their module's location, the last two per (level, design, format)."""

    designs: dict[tuple[str, str], tuple[str, str]] = field(default_factory=dict)
    absent: dict[tuple[str, str, str], set[str]] = field(default_factory=dict)
    modgained: dict[tuple[str, str, str], int] = field(default_factory=dict)

    def allow(self, level: str, design: str, fmt: str) -> Allow:
        key = (level, design, fmt)
        return Allow(frozenset(self.absent.get(key, ())), self.modgained.get(key, 0))


def expect_line(table: Expect, words: list[str]) -> None:
    """One line of the expect file, split into words; ToolError when it is not one of the forms."""
    if words[0] == "absent" and len(words) >= 6 and words[1] in LEVELS:
        if words[3] not in ABSENT_FORMATS:
            raise ToolError(f"absent: only {', '.join(ABSENT_FORMATS)} may lack a module")
        table.absent.setdefault((words[1], words[2], words[3]), set()).add(words[4])
    elif words[0] == "modgained" and len(words) >= 6 and words[1] in LEVELS:
        if words[3] not in FORMATS or not words[4].isdigit():
            raise ToolError("modgained: want '<level> <design> <format> <count> <reason>'")
        table.modgained[(words[1], words[2], words[3])] = int(words[4])
    elif len(words) >= 4 and words[0] in LEVELS and words[2] in OUTCOMES:
        table.designs[(words[0], words[1])] = (words[2], " ".join(words[3:]))
    else:
        raise ToolError("want '<level> <design> <outcome> <reason>', 'absent <level> <design> "
                        "<format> <module> <reason>' or 'modgained <level> <design> <format> "
                        "<count> <reason>'")


def parse_expect(text: str) -> Expect:
    """The expect file; a reason is required on every line."""
    table = Expect()
    for number, raw in enumerate(text.splitlines(), 1):
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        try:
            expect_line(table, words)
        except ToolError as exc:
            raise ToolError(f"expect line {number}: {exc}") from exc
    return table


Pins = dict[tuple[str, str, str, str, str], int]


def parse_pins(text: str) -> Pins:
    """``(set, level, format, kind, class) -> count`` from lines
    ``<set> <level> <format> <kind> <class> <count>``."""
    pins: Pins = {}
    for number, raw in enumerate(text.splitlines(), 1):
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        if (
            len(words) != 6
            or words[1] not in LEVELS
            or words[2] not in FORMATS
            or words[3] not in KINDS
            or not words[4].startswith("X-")
            or not words[5].isdigit()
        ):
            raise ToolError(f"pins line {number}: want '<set> <level> <format> <kind> X-n <count>'")
        pins[(words[0], words[1], words[2], words[3], words[4])] = int(words[5])
    return pins


def pin_lines(name: str, level: str, fmt: str, tallies: dict[str, Tally]) -> list[str]:
    """The pin lines of one (level, format) total."""
    return [
        f"{name} {level} {fmt} {kind} {cls} {n}"
        for kind in KINDS
        if kind in tallies
        for cls, n in sorted(tallies[kind].lost.items())
    ]


def pin_problems(
    pins: Pins, name: str, sums: dict[tuple[str, str], list[dict[str, Tally]]]
) -> tuple[list[str], list[str]]:
    """(failures, notes): a class count above its pin fails, below it is a note to lower the pin.
    A class with no pin is pinned at 0."""
    seen: dict[tuple[str, str, str, str, str], int] = {}
    for (level, fmt), runs in sums.items():
        for kind, t in merge_tallies(runs).items():
            for cls, n in t.lost.items():
                seen[(name, level, fmt, kind, cls)] = n
    fails: list[str] = []
    notes: list[str] = []
    for key in sorted(set(seen) | {k for k in pins if k[0] == name and (k[1], k[2]) in sums}):
        have, pinned = seen.get(key, 0), pins.get(key, 0)
        label = " ".join(key[1:])
        if have > pinned:
            fails.append(f"pin exceeded: {label}: {have} lost, pinned {pinned}")
        elif have < pinned:
            notes.append(f"improved, update the pin: {label}: {have} lost, pinned {pinned}")
    return fails, notes


def parse_techlibs(path: Path) -> dict[str, list[str]]:
    """``design file name -> commands`` from the icells list (same format as 5.2)."""
    table: dict[str, list[str]] = {}
    if not path.is_file():
        return table
    for raw in path.read_text(encoding="utf-8").splitlines():
        words = raw.split("#", 1)[0].split()
        if len(words) not in (0, 2, 3):
            raise ToolError(f"{path}: want '<design> <tech library> [<soft library>]': {raw!r}")
        if words:
            absolute = [w if Path(w).is_absolute() else str(ROOT / w) for w in words[1:]]
            table[words[0]] = [f"read_techlib {absolute[0]}"]
            table[words[0]] += [f"read_verilog --lib {w}" for w in absolute[1:]]
    return table


def level_script(template: str, subs: dict[str, str]) -> str:
    """The script text of a level: comments dropped, lines joined with ``; ``."""
    lines = [ln.strip() for ln in template.splitlines()]
    text = "; ".join(ln for ln in lines if ln and not ln.startswith("#"))
    for key, value in subs.items():
        text = text.replace(key, value)
    text = re.sub(r"\s+", " ", text).replace(" ;", ";")
    return re.sub(r"^(?:; ?)+", "", text).strip()


# --- running ----------------------------------------------------------------------------------


@dataclass
class Config:
    odin3: str
    scripts: Path
    lib: Path
    techlibs: dict[str, list[str]] = field(default_factory=dict)


@dataclass
class ReadFail:
    """A format whose read-back failed (outcome ``error:read``)."""

    reason: str


@dataclass
class Pair:
    """The listings of one format: before, after, and what the written files say by module."""

    before: list[Obj]
    after: list[Obj]
    written: Written | None = None


Result = Pair | ReadFail

OPERAND = r"(?:\\\S+\s|[A-Za-z_][\w$.]*)(?:\[\d+(?::\d+)?\])?"
ALIAS_RE = re.compile(rf"^\s*assign\s+{OPERAND}\s*=\s*{OPERAND}\s*;\s*//\s*(\S.*?)\s*$")
MODULE_RE = re.compile(r"^module\s+(\\\S+|[\w$]+)")


def verilog_aliases(path: Path) -> dict[str, frozenset[tuple[str, int]]]:
    """module -> the (file, line) of every pure alias ``assign x = y;  // file:line`` in a
    ``write_verilog`` file: a statement the reader reads as an alias and makes no cell of."""
    out: dict[str, set[tuple[str, int]]] = {}
    module = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        head = MODULE_RE.match(raw)
        if head:
            module = head.group(1).lstrip("\\")
            out.setdefault(module, set())
            continue
        match = ALIAS_RE.match(raw)
        locs = parse_locs(match.group(1)) if match else []
        if locs:
            out.setdefault(module, set()).add((locs[0].file, locs[0].line))
    return {m: frozenset(v) for m, v in out.items()}


def written_facts(json_path: Path, verilog_path: Path) -> Written:
    """module -> what the JSON and Verilog files of the netlist say (for the class X-9)."""
    modules = json.loads(json_path.read_text(encoding="utf-8")).get("modules", {})
    aliases = verilog_aliases(verilog_path)
    names = set(modules) | set(aliases)
    return {
        name: Carrier(
            frozenset(k for k, v in modules.get(name, {}).get("cells", {}).items() if is_buffer(v)),
            aliases.get(name, frozenset()),
        )
        for name in names
    }


def is_buffer(cell: dict[str, Any]) -> bool:
    """A cell that copies its one input to its one output."""
    if cell.get("type") == "$_BUF_":
        return True
    params = cell.get("parameters", {})
    return (
        cell.get("type") == "$lut"
        and params.get("WIDTH") == "1".zfill(32)
        and params.get("LUT") == "10"
    )


WRITES = {
    "json": ("write_json {p}", "read_json {p}"),
    "verilog": ("write_verilog {p}", "read_verilog -netlist -icells {p}"),
    "blif": ("write_blif --vtr --name-style provenance {p}", "read_blif --hdl-locs {p}"),
    "plain": ("write_blif --src-attr {p}", "read_blif --hdl-locs {p}"),
}
SUFFIX = {"json": "json", "verilog": "v", "blif": "blif", "plain": "plain.blif"}


def odin3_run(cfg: Config, script: str, work: Path, flags: Sequence[str] = ()) -> str:
    """Runs ``odin3 [flags] -p script``; the combined log. A non-zero exit is a ScriptError."""
    try:
        done = subprocess.run(
            [cfg.odin3, *flags, "-p", script],
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
            cwd=work,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CrashError(f"odin3: {exc}") from exc
    log = (done.stderr or "") + (done.stdout or "")
    if done.returncode != 0:
        errors = [ln for ln in log.splitlines() if "error" in ln]
        raise ScriptError(" | ".join((errors or log.splitlines())[-4:]))
    return log


def run_design(cfg: Config, level: str, design: Path, work: Path) -> dict[str, Result]:
    """before and after listings of every format of the level, or a ReadFail for a format."""
    template = (cfg.scripts / f"{level}.script").read_text(encoding="utf-8")
    libs = cfg.techlibs.get(design.name, [])
    subs = {
        "@DESIGN@": str(design),
        "@LIB@": str(cfg.lib),
        "@FAMILY@": "",
        "@TECHLIB@": "; ".join(libs),
    }
    formats = LEVEL_FORMATS[level]
    paths = {fmt: work / f"W.{SUFFIX[fmt]}" for fmt in formats}
    before = work / "before.txt"
    commands = [level_script(template, subs), f"dump_locs --types {before}"]
    commands += [WRITES[fmt][0].format(p=paths[fmt]) for fmt in formats]
    log = odin3_run(cfg, "; ".join(commands), work)
    top = TOP_RE.search(log)
    flags = ["--top", top.group(1)] if top else []
    prefix = "".join(f"{c}; " for c in libs)
    if level == "lpm":
        prefix = f"read_techlib {cfg.lib}; {prefix}"
    results: dict[str, Result] = {}
    written = written_facts(paths["json"], paths["verilog"])
    rows = parse_dump(before.read_text(encoding="utf-8"))
    for fmt in formats:
        after = work / f"after-{fmt}.txt"
        script = f"{prefix}{WRITES[fmt][1].format(p=paths[fmt])}; dump_locs {after}"
        try:
            odin3_run(cfg, script, work, flags)
        except ScriptError as exc:
            results[fmt] = ReadFail(str(exc))
            continue
        results[fmt] = Pair(rows, parse_dump(after.read_text(encoding="utf-8")), written)
    return results


# --- the report --------------------------------------------------------------------------------


def row_text(kind: str, t: Tally) -> str:
    lost = ",".join(f"{c}:{n}" for c, n in sorted(t.lost.items())) or "-"
    if t.unclassified:
        lost += f",NO-CLASS:{t.unclassified}"
    sb, sa = specificity(t.lines_before), specificity(t.lines_after)
    return (
        f"{kind:6s} objects {t.objects:6d} had {t.had:6d} same {t.same:6d} line {t.line:5d} "
        f"inherited {t.inherited:5d} lost {lost} wrong {t.wrong} gained {t.gained} "
        f"modgained {t.modgained} split {t.split} capped {t.capped} "
        f"spec {sb[0]:g}/{sb[1]:g} -> {sa[0]:g}/{sa[1]:g}"
    )


def print_table(title: str, tallies: dict[str, Tally]) -> None:
    for kind in KINDS:
        if kind in tallies:
            print(f"  {title} {row_text(kind, tallies[kind])}")


@dataclass
class RunState:
    """The run: designs compared, excepted and failed, the expect table, the pins and the tallies
    per (level, format)."""

    expect: Expect = field(default_factory=Expect)
    pins: Pins = field(default_factory=dict)
    total: int = 0
    compared: int = 0
    excepted: int = 0
    worst: int = EXIT_OK
    sums: dict[tuple[str, str], list[dict[str, Tally]]] = field(default_factory=dict)


Analysis = dict[str, tuple[dict[str, Tally], list[str]]]


def analyse(level: str, design: str, results: dict[str, Result], expect: Expect) -> Analysis:
    """Per format: the tallies of the comparison and the rule failures."""
    out: Analysis = {}
    for fmt, res in results.items():
        if not isinstance(res, ReadFail):
            allow = expect.allow(level, design, fmt)
            tallies = compare(fmt, res.before, res.after, res.written, allow)
            out[fmt] = (tallies, check_tallies(fmt, tallies, allow))
    return out


def outcome_of(results: dict[str, Result], analysis: Analysis) -> tuple[str, str]:
    """(``ok``, ``error:read`` or ``fail``) of a design and the first reason."""
    for fmt, res in results.items():
        if isinstance(res, ReadFail):
            return "error:read", f"{fmt}: {res.reason}"
    for fmt, (_, problems) in analysis.items():
        if problems:
            return "fail", f"{fmt}: {problems[0]}"
    return "ok", ""


def judge(outcome: str, listed: tuple[str, str] | None) -> tuple[bool, str]:
    if listed is None:
        return outcome == "ok", outcome
    if outcome == listed[0]:
        return True, f"excepted ({listed[0]}: {listed[1]})"
    if outcome == "ok":
        return False, f"STALE expect entry ({listed[0]}: {listed[1]}); it passes now"
    return False, f"{outcome} but the expect file says {listed[0]}"


def one_design(
    cfg: Config, level: str, design: Path, work: Path, state: RunState, verbose: bool
) -> bool:
    """Runs and reports one design at one level; True when it passes the run."""
    listed = state.expect.designs.get((level, design.name))
    analysis: Analysis = {}
    try:
        results = run_design(cfg, level, design, work)
        analysis = analyse(level, design.name, results, state.expect)
        outcome, reason = outcome_of(results, analysis)
    except ScriptError as exc:
        outcome, reason = "error:script", str(exc)
    passed, word = judge(outcome, listed)
    print(f"{level} {design.name}: {word}{'' if outcome == 'ok' else ' ' + reason}", flush=True)
    for fmt, (tallies, problems) in analysis.items():
        if outcome == "ok":
            state.sums.setdefault((level, fmt), []).append(tallies)
        if verbose or not passed:
            print_table(f"{level} {fmt} {design.name}", tallies)
        for problem in problems if not passed else []:
            print(f"  {fmt}: {problem}")
    return passed


# --- the command line --------------------------------------------------------------------------


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n")[0])
    ap.add_argument("--level", required=True, help="word, gate, lut, lpm (comma separated)")
    ap.add_argument("--odin3", default=str(ROOT / "build" / "debug" / "odin3"))
    ap.add_argument("--scripts", type=Path, default=ROOT / "tests" / "loc")
    ap.add_argument("--expect", type=Path)
    ap.add_argument("--pins", type=Path, help="the pinned class counts (with --set)")
    ap.add_argument("--set", dest="set_name", help="the name of the design set, the pins' key")
    ap.add_argument("--print-pins", action="store_true", help="print the pin lines of the run")
    ap.add_argument("--techlib-list", type=Path, default=ROOT / "tests" / "loc" / "techlib.txt")
    ap.add_argument("--lib", type=Path, default=ROOT / "lib" / "lpm.o3lib")
    ap.add_argument("--work", type=Path, help="keep every file here")
    ap.add_argument("--smallest-first", action="store_true", help="run the designs by size")
    ap.add_argument("--verbose", action="store_true", help="a table for every design")
    ap.add_argument("files", nargs="*", type=Path)
    return ap.parse_args(argv)


def totals_report(state: RunState, args: argparse.Namespace) -> int:
    """The totals, the pin lines when asked, and the pin check; the exit code it contributes."""
    for (level, fmt), runs in sorted(state.sums.items()):
        total = merge_tallies(runs)
        print_table(f"TOTAL {level} {fmt}", total)
        if args.print_pins:
            for line in pin_lines(args.set_name or "SET", level, fmt, total):
                print(f"PIN {line}")
    if not state.pins:
        return EXIT_OK
    fails, notes = pin_problems(state.pins, args.set_name, state.sums)
    for line in notes + fails:
        print(f"{TOOL}: {line}")
    return EXIT_FAIL if fails else EXIT_OK


def execute(args: argparse.Namespace, work: Path) -> int:
    levels = args.level.split(",")
    bad = [lv for lv in levels if lv not in LEVELS]
    if bad:
        raise ToolError(f"unknown level {bad[0]} (want one of {', '.join(LEVELS)})")
    if not args.files:
        raise ToolError("no designs given")
    if args.pins and not args.set_name:
        raise ToolError("--pins needs --set")
    cfg = Config(str(Path(args.odin3).resolve()), args.scripts, args.lib)
    cfg.techlibs = parse_techlibs(args.techlib_list)
    state = RunState()
    if args.expect:
        state.expect = parse_expect(args.expect.read_text(encoding="utf-8"))
    if args.pins:
        state.pins = parse_pins(args.pins.read_text(encoding="utf-8"))
    files = [f.resolve() for f in args.files]
    if args.smallest_first:
        files.sort(key=lambda f: (f.stat().st_size, f.name))
    for level in levels:
        for number, item in enumerate(files):
            run_dir = work / f"{level}-{number:04d}"
            run_dir.mkdir(exist_ok=True)
            state.total += 1
            passed = one_design(cfg, level, item, run_dir, state, args.verbose)
            listed = (level, item.name) in state.expect.designs
            state.excepted += passed and listed
            state.compared += passed and not listed
            state.worst = state.worst if passed else EXIT_FAIL
    state.worst = max(state.worst, totals_report(state, args))
    print(
        f"{TOOL}: compared {state.compared} of {state.total} ({state.excepted} excepted)",
        flush=True,
    )
    if state.compared == 0:
        print(f"{TOOL}: nothing was compared", file=sys.stderr)
        return EXIT_FAIL
    return state.worst


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    keep = args.work is not None
    work = Path(args.work).resolve() if keep else Path(tempfile.mkdtemp(prefix="loc-check-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        return execute(args, work)
    except CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        return EXIT_CRASH
    except (OSError, ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
