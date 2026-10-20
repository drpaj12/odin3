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
``gained``   an object with none before that has one after (a pool counts the surplus);
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
from collections.abc import Sequence
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
        rows.append(Obj(unescape(fields[0]), fields[1], unescape(fields[2]), line[i:]))
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


@dataclass(frozen=True)
class CellSet:
    """The cells of one module in its JSON file: all names, and those that only copy a net (a
    one-input identity LUT or buffer), which the Verilog writer folds into an alias."""

    names: frozenset[str]
    buffers: frozenset[str]


Written = dict[str, CellSet]


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
    gained: int = 0
    capped: int = 0
    lines_before: list[int] = field(default_factory=list)
    lines_after: list[int] = field(default_factory=list)

    def add(self, other: Tally) -> None:
        for name in ("objects", "had", "same", "line", "inherited", "unclassified", "gained"):
            setattr(self, name, getattr(self, name) + getattr(other, name))
        self.capped += other.capped
        for cls, count in other.lost.items():
            self.lost[cls] = self.lost.get(cls, 0) + count
        self.lines_before += other.lines_before
        self.lines_after += other.lines_after


def distinct_lines(locs: Sequence[Loc]) -> int:
    return len({(x.file, x.line) for x in locs})


def lose(scope: Scope, before: Obj, tally: Tally) -> None:
    """Counts a lost object under its class. A cell the JSON file of the same netlist has no entry
    for (``scope.written``) has no carrier in any format: X-8 when it has the location of a
    declared wire (a port cell: every format writes ports, not cells), else X-9 (a constant driver
    or other cell the writers fold into a literal); a buffer lost in Verilog is X-9 too (the
    writer folds it into an alias). A module the read-back has no trace of (the
    Verilog writer writes the top and what it instantiates) is X-1, no carrier. Everything else
    by RULES."""
    cls = RULES.get((scope.fmt, scope.kind, not is_generated(before.name)))
    if not scope.present:
        cls = "X-1"
    elif (
        scope.kind == "node"
        and scope.written is not None
        and before.name not in scope.written.names
    ):
        cls = "X-8" if before.src in scope.wire_srcs else "X-9"
    elif scope.kind == "node" and scope.fmt == "verilog" and scope.written is not None:
        cls = "X-9" if before.name in scope.written.buffers else cls
    if cls is None:
        tally.unclassified += 1
    else:
        tally.lost[cls] = tally.lost.get(cls, 0) + 1


@dataclass(frozen=True)
class Scope:
    """What a pairing needs: the format, the kind, the location texts of the module's wires and the
    names of the cells its JSON file has (None: not known, every cell counts as written)."""

    fmt: str
    kind: str
    wire_srcs: frozenset[str]
    written: CellSet | None
    present: bool = True


def from_wire(scope: Scope, after: Obj) -> bool:
    """A net that has the location of a declared wire of its module after read-back: the readers
    give a net the location of its netname or declaration (D-2, D-3), so the net is `inherited`
    from the wire, not `gained`."""
    return scope.kind == "net" and after.src in scope.wire_srcs


def judge_pair(scope: Scope, before: Obj, after: Obj | None, tally: Tally) -> None:
    """Counts one paired object: before has a location or not, after is its read-back (or None)."""
    fmt, kind = scope.fmt, scope.kind
    mine = parse_locs(before.src)
    inherits = fmt == "blif" and kind == "node"  # counted as `inherited`, not as same or line
    if not mine:
        if after is not None and after.src and not inherits:
            tally.inherited += from_wire(scope, after)
            tally.gained += not from_wire(scope, after)
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
        lose(scope, before, tally)  # a changed location is a lost one


def pool_pairs(
    group_b: list[Obj], group_a: list[Obj]
) -> tuple[list[tuple[Obj, Obj | None]], list[Obj]]:
    """Pairs the objects with no name partner: exact location list, then first file and line.
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
    by_line: dict[tuple[str, int], list[Obj]] = {}
    for objs in free.values():
        for obj in objs:
            first = parse_locs(obj.src)
            if first:
                by_line.setdefault((first[0].file, first[0].line), []).append(obj)
    for obj in rest:
        first = parse_locs(obj.src)
        key = (first[0].file, first[0].line) if first else ("", 0)
        pairs.append((obj, by_line[key].pop() if by_line.get(key) else None))
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
    if scope.written is not None:  # a cell with no carrier takes what is left (stable sort)
        left_b.sort(key=lambda o: o.name not in (scope.written.names if scope.written else ()))
    pairs, surplus = pool_pairs(left_b, left_a)
    for obj, partner in pairs:
        judge_pair(scope, obj, partner, tally)
    nolocs = sum(1 for o in left_b if not o.src)
    if not pool_all:
        wired = min(sum(1 for o in surplus if from_wire(scope, o)), nolocs)
        tally.inherited += wired
        tally.gained += min(len(surplus) - wired, nolocs - wired)
    else:
        tally.inherited += sum(1 for o in after if o.src)


def compare(
    fmt: str, before: list[Obj], after: list[Obj], written: Written | None = None
) -> dict[str, Tally]:
    """The tally of every kind present, for one design at one level in one format. ``written``:
    module -> the names of the cells in its JSON file (for the classes X-8 and X-9)."""
    out: dict[str, Tally] = {}
    groups_b: dict[tuple[str, str], list[Obj]] = {}
    groups_a: dict[tuple[str, str], list[Obj]] = {}
    for obj in before:
        groups_b.setdefault((obj.module, obj.kind), []).append(obj)
    modules_after = {o.module for o in after}
    for obj in after:
        groups_a.setdefault((obj.module, obj.kind), []).append(obj)
    for key in sorted(set(groups_b) | set(groups_a)):
        kind = key[1]
        tally = out.setdefault(kind, Tally())
        group_b = groups_b.get(key, [])
        tally.objects += len(group_b)
        for obj in group_b:
            locs = parse_locs(obj.src)
            if locs:
                tally.had += 1
                tally.lines_before.append(distinct_lines(locs))
                tally.capped += len(locs) >= CAP
        wires = frozenset(o.src for o in groups_b.get((key[0], "wire"), []) if o.src)
        cells = written.get(key[0]) if written else None
        scope = Scope(fmt, kind, wires, cells, key[0] in modules_after)
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


def check_tallies(fmt: str, tallies: dict[str, Tally]) -> list[str]:
    """The rule failures of one design in one format (spec T-3)."""
    problems: list[str] = []
    if sum(t.had for t in tallies.values()) == 0:
        problems.append("the comparison matched no objects with a location")
    for kind in KINDS:
        t = tallies.get(kind)
        if t is None:
            continue
        if t.unclassified:
            problems.append(f"{kind}: {t.unclassified} lost with no class (the format keeps them)")
        if t.gained:
            problems.append(f"{kind}: {t.gained} gained a location")
        if t.line and (fmt, kind) not in LINE_OK:
            problems.append(f"{kind}: {t.line} line-only where the format is lossless")
    return problems


# --- the expect file and the scripts -------------------------------------------------------------


def parse_expect(text: str) -> dict[tuple[str, str], tuple[str, str]]:
    """``(level, design) -> (outcome, reason)``."""
    table: dict[tuple[str, str], tuple[str, str]] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split(None, 3)
        if len(words) < 4 or words[0] not in LEVELS or words[2] not in OUTCOMES:
            raise ToolError(f"expect line {number}: want '<level> <design> <outcome> <reason>'")
        table[(words[0], words[1])] = (words[2], words[3])
    return table


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
    """The listings of one format: before, after, and the cells of the JSON file by module."""

    before: list[Obj]
    after: list[Obj]
    written: Written | None = None


Result = Pair | ReadFail


def json_cells(path: Path) -> Written:
    """module -> the cell names of a ``write_json`` file."""
    modules = json.loads(path.read_text(encoding="utf-8")).get("modules", {})
    return {name: cell_set(mod.get("cells", {})) for name, mod in modules.items()}


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


def cell_set(cells: dict[str, Any]) -> CellSet:
    return CellSet(frozenset(cells), frozenset(k for k, v in cells.items() if is_buffer(v)))


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
    commands = [level_script(template, subs), f"dump_locs {before}"]
    commands += [WRITES[fmt][0].format(p=paths[fmt]) for fmt in formats]
    log = odin3_run(cfg, "; ".join(commands), work)
    top = TOP_RE.search(log)
    flags = ["--top", top.group(1)] if top else []
    prefix = "".join(f"{c}; " for c in libs)
    if level == "lpm":
        prefix = f"read_techlib {cfg.lib}; "
    results: dict[str, Result] = {}
    written = json_cells(paths["json"])
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
        f"inherited {t.inherited:5d} lost {lost} gained {t.gained} capped {t.capped} "
        f"spec {sb[0]:g}/{sb[1]:g} -> {sa[0]:g}/{sa[1]:g}"
    )


def print_table(title: str, tallies: dict[str, Tally]) -> None:
    for kind in KINDS:
        if kind in tallies:
            print(f"  {title} {row_text(kind, tallies[kind])}")


@dataclass
class RunState:
    """The run: designs compared, excepted and failed, the expect table, and the tallies per
    (level, format)."""

    expect: dict[tuple[str, str], tuple[str, str]] = field(default_factory=dict)
    total: int = 0
    compared: int = 0
    excepted: int = 0
    worst: int = EXIT_OK
    sums: dict[tuple[str, str], list[dict[str, Tally]]] = field(default_factory=dict)


def outcome_of(results: dict[str, Result]) -> tuple[str, str]:
    """(``ok``, ``error:read`` or ``fail``) of a design and the first reason."""
    for fmt, res in results.items():
        if isinstance(res, ReadFail):
            return "error:read", f"{fmt}: {res.reason}"
    for fmt, res in results.items():
        if not isinstance(res, ReadFail):
            problems = check_tallies(fmt, compare(fmt, res.before, res.after, res.written))
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
    listed = state.expect.get((level, design.name))
    try:
        results = run_design(cfg, level, design, work)
        outcome, reason = outcome_of(results)
    except ScriptError as exc:
        results, outcome, reason = {}, "error:script", str(exc)
    passed, word = judge(outcome, listed)
    print(f"{level} {design.name}: {word}{'' if outcome == 'ok' else ' ' + reason}", flush=True)
    for fmt, res in results.items():
        if isinstance(res, ReadFail):
            continue
        tallies = compare(fmt, res.before, res.after, res.written)
        if outcome == "ok":
            state.sums.setdefault((level, fmt), []).append(tallies)
        if verbose or not passed:
            print_table(f"{level} {fmt} {design.name}", tallies)
        for problem in check_tallies(fmt, tallies) if not passed else []:
            print(f"  {fmt}: {problem}")
    return passed


# --- the command line --------------------------------------------------------------------------


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n")[0])
    ap.add_argument("--level", required=True, help="word, gate, lut, lpm (comma separated)")
    ap.add_argument("--odin3", default=str(ROOT / "build" / "debug" / "odin3"))
    ap.add_argument("--scripts", type=Path, default=ROOT / "tests" / "loc")
    ap.add_argument("--expect", type=Path)
    ap.add_argument("--techlib-list", type=Path, default=ROOT / "tests" / "loc" / "techlib.txt")
    ap.add_argument("--lib", type=Path, default=ROOT / "lib" / "lpm.o3lib")
    ap.add_argument("--work", type=Path, help="keep every file here")
    ap.add_argument("--smallest-first", action="store_true", help="run the designs by size")
    ap.add_argument("--verbose", action="store_true", help="a table for every design")
    ap.add_argument("files", nargs="*", type=Path)
    return ap.parse_args(argv)


def totals_report(state: RunState) -> None:
    for (level, fmt), runs in sorted(state.sums.items()):
        print_table(f"TOTAL {level} {fmt}", merge_tallies(runs))


def execute(args: argparse.Namespace, work: Path) -> int:
    levels = args.level.split(",")
    bad = [lv for lv in levels if lv not in LEVELS]
    if bad:
        raise ToolError(f"unknown level {bad[0]} (want one of {', '.join(LEVELS)})")
    if not args.files:
        raise ToolError("no designs given")
    cfg = Config(str(Path(args.odin3).resolve()), args.scripts, args.lib)
    cfg.techlibs = parse_techlibs(args.techlib_list)
    state = RunState()
    if args.expect:
        state.expect = parse_expect(args.expect.read_text(encoding="utf-8"))
    files = [f.resolve() for f in args.files]
    if args.smallest_first:
        files.sort(key=lambda f: (f.stat().st_size, f.name))
    for level in levels:
        for number, item in enumerate(files):
            run_dir = work / f"{level}-{number:04d}"
            run_dir.mkdir(exist_ok=True)
            state.total += 1
            passed = one_design(cfg, level, item, run_dir, state, args.verbose)
            listed = (level, item.name) in state.expect
            state.excepted += passed and listed
            state.compared += passed and not listed
            state.worst = state.worst if passed else EXIT_FAIL
    totals_report(state)
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
