"""prov-cover: how many final-netlist objects have an HDL location (6.5b, spec 6 section 5).

Usage::

    prov-cover [--legs a,b,c,d] [--allow inherited,pad] [--odin3 O] [--report report.json]
               [--work DIR] [--smallest-first] [--vpr VPR] [--vpr-timeout S]
               [--arch XML] DESIGN.v...

P-1 to P-8 of the spec.

Per design and leg the tool drives the ``odin3`` CLI (``legs.py``), takes ``dump_locs --types`` at
the leg's point and counts the objects P-1 names: LUTs (``$lut``, and the ``$sop`` a BLIF ``.names``
reads back as), latches and flip-flops, hard blocks (any other type that is not ``$`` and not an
LPM) and LPM instances (``lpm_*``, ``altsyncram``). An object is resolved when its SRC is non-empty.

Legs: (a) BLIF after ``abc``, in memory; (b) the same BLIF written and read back with
``--hdl-locs``; (c) the LPM flow, in memory and (``c2``) written and read back; (d) the VPR
``.place`` file (``vpr_leg.py``: the tool starts VPR, pack and place only, itself).

P-3 exception classes apply to a read-back dump only (legs b and c-rb): in memory (legs a and c)
every object without a location is a loss. Each class is counted, none silent (the report lists
every class, zero included, and the strict figure with none of them). The rules, evidence in
brackets:

``const-driver``  a ``$_CONST*_`` node with no location. [writer.c: the BLIF writer folds a
                  constant cell into the generator ``.names gnd|vcc|unconn``, X-9]
``const-table``   a zero-input ``$lut``/``$sop`` with no location whose output net is named
                  ``gnd``, ``vcc`` or ``unconn``. [writer.c ``GEN_NAMES``; the reader reads
                  ``.names gnd`` as a zero-input ``$sop``]
``port-buffer``   a one-input identity ``$lut``/``$sop`` with no location, between two different
                  nets, whose output net is a module output port bit or whose input net is a
                  module input port bit. [writer.c ``port_buffer``: a ``.names net port`` with the
                  row ``1 1``] If the leg-(a) design has a located node driving the same net, the
                  object is a lost location (a defect), not an exception.
``not-helper``    a ``$not``/``$_NOT_`` with no location whose single input net is driven by a
                  ``$sop``. [5.4 X-9 names it; evidence 6.5b: ``read_blif`` makes one ``$sop`` that
                  keeps the OFF-set cover, no ``$not``, so the class is 0 for the BLIF legs]
``port-driver``   (Peter decision 2026-10-21, D1/D3) an object of a read-back dump with no
                  location whose output net is a module output port bit (X-8: the port-bit net
                  name carries no ``@file:line``, so D-5 has nothing to inherit), when the same
                  net name was driven by a located node in leg (a). Without that leg-(a)
                  proof it stays a defect. It stays an object (counted in its class and in the
                  a-vs-b counts) and is removed from the coverage base; never applied to leg (a).
                  An identity table at a port bit that leg (a) located is this class, not a
                  ``port-buffer``. Leg (d): the same rule on the driver block of a port bit.
``inherited``     (``--allow``) a cell whose location exists only through 5.4 D-5: in a BLIF VTR
                  read-back every located cell, as the dialect writes none for a cell (X-6).
``pad``           (``--allow``) a VPR I/O pad block of the ``.place`` file (5.4 X-8).

The objects of a class need the connections of the design: ``dump_locs`` lists no pins, so the
tool also writes ``write_json`` at the same point and joins the cells by name (``Facts``). Every
exception class (but ``inherited`` and ``pad``) is also counted in the written BLIF text
(``written_counts``); the two counts must be equal, else the rule is wrong and the gate fails.

A-vs-b: the objects per class before and after the write and read-back (the modules the read-back
holds, less the tables the writer adds) are printed; fewer after is one ``count-drop`` defect per
lost object.

Gate (P-4): exit status 0 when no object is unresolved outside the classes, no cross-check
differs, and something was compared. Anything else unresolved is a defect; the first ten are
named. Recorded (P-5): objects per class, resolved, coverage (resolved / objects less the
removed exceptions), the strict figure (directly located / all objects; 0 where every location
is inherited), and the specificity (median and p90 of distinct file:line pairs per object,
``capped`` = objects at the 8-location cap of 5.4 W-3).

Exit codes: 0 gate passes; 1 a defect, a mismatch or nothing compared; 2 usage or tool error; 3 a
child crashed or timed out.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from collections import Counter
from collections.abc import Collection, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from loc_check import Loc, Obj, ToolError, parse_dump, parse_locs

__all__ = ["Obj", "ToolError", "parse_dump", "parse_locs"]

TOOL = "prov-cover"
ROOT_DIR = Path(__file__).resolve().parents[2]
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH = 0, 1, 2, 3
CAP = 8  # 5.4 W-3: at most 8 locations in a record
OBJECT_CLASSES = ("lut", "latch", "hard", "lpm")
EXCEPTIONS = (
    "const-driver", "const-table", "port-buffer", "not-helper", "port-driver", "inherited", "pad"
)
APPROVED = frozenset({"inherited", "pad"})  # approved by Peter 2026-10-20 (PHASE6 decision 2)
DEFAULT_ALLOW = APPROVED
NAMED_DEFECTS = 10
CONST_TYPES = frozenset({"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"})
PORT_TYPES = frozenset({"$port_in", "$port_out", "$port_inout"})
LUT_TYPES = frozenset({"$lut", "$sop"})
NOT_TYPES = frozenset({"$not", "$_NOT_"})
GENERATOR_NETS = frozenset({"gnd", "vcc", "unconn"})
LATCH_RE = re.compile(r"^\$_?(S?DFF|DFF|ALDFF|ADFF|DLATCH|FF|SR|DLATCHSR|DFFSR|SDFF)", re.I)
LOC_SUFFIX = re.compile(r"@(.+):(\d+)$")


class CrashError(Exception):
    """A child crashed or timed out (exit 3)."""


@dataclass(frozen=True)
class Cell:
    """One cell of a design: its type, the nets on its inputs and outputs (in pin order), and
    whether it is a one-input identity table (a buffer)."""

    ctype: str
    ins: tuple[str, ...]
    outs: tuple[str, ...]
    identity: bool = False


@dataclass(frozen=True)
class Mod:
    """The connections of one module: cells by name, and the port bits' nets."""

    cells: Mapping[str, Cell]
    port_in: frozenset[str] = frozenset()
    port_out: frozenset[str] = frozenset()
    names: Mapping[str, str] = field(default_factory=dict)  # net key -> its name
    # gnd, vcc or unconn nets that hold a constant bit: write_json folds a zero-input table into
    # the constant, so the cell has no entry in ``cells`` and only this net name remains
    generators: frozenset[str] = frozenset()

    def net_name(self, net: str) -> str:
        return self.names.get(net, net)


Facts = Mapping[str, Mod]


@dataclass(frozen=True)
class Defect:
    kind: str  # unresolved, lost-location
    module: str
    name: str
    ctype: str
    note: str = ""  # drives-output-port: the cell's output net is a module output port bit

    def text(self) -> str:
        extra = f", {self.note}" if self.note else ""
        return f"{self.kind}: {self.module}.{self.name} ({self.ctype or 'no type'}{extra})"


@dataclass
class LegReport:
    """The numbers of one design and leg."""

    objects: dict[str, int] = field(default_factory=dict)
    resolved: dict[str, int] = field(default_factory=dict)
    exceptions: dict[str, int] = field(default_factory=lambda: dict.fromkeys(EXCEPTIONS, 0))
    direct: int = 0  # objects located without an exception class (the strict figure)
    driver_buffers: int = 0  # port-driver objects that are also an identity table at a port bit
    removed: int = 0  # objects of the exception classes that stand in for a location
    defects: list[Defect] = field(default_factory=list)
    mismatches: list[str] = field(default_factory=list)
    lines: list[int] = field(default_factory=list)
    capped: int = 0
    blocks: int = 0
    nodes: int = 0  # the nodes counted: objects and the exception nodes that are not objects
    modules: dict[str, dict[str, int]] = field(default_factory=dict)  # objects per module, class
    versus: dict[str, tuple[int, int]] = field(default_factory=dict)  # class -> (before, after)

    @property
    def total(self) -> int:
        return sum(self.objects.values())

    @property
    def resolved_total(self) -> int:
        return sum(self.resolved.values())

    @property
    def coverage(self) -> float | None:
        base = self.total - self.removed
        return self.resolved_total / base if base > 0 else None

    @property
    def strict(self) -> float | None:
        return self.direct / self.total if self.total else None

    @property
    def median(self) -> float:
        return specificity(self.lines)[0]

    @property
    def p90(self) -> float:
        return specificity(self.lines)[1]

    @property
    def ok(self) -> bool:
        return not self.defects and not self.mismatches

    def kept(self, cls: str) -> int:
        """Objects of a class once the writer-made exception tables are taken out: the LUT and
        latch counts that stand against the other leg's."""
        extra = self.exceptions["const-table"] + self.exceptions["port-buffer"]
        return self.objects.get(cls, 0) - (extra if cls == "lut" else 0)

    def named_defects(self) -> list[Defect]:
        return self.defects[:NAMED_DEFECTS]


def _kept_in(rep: LegReport, mod: str, cls: str) -> int:
    """``kept`` for one module: its objects less the writer-made tables in it."""
    per = rep.modules.get(mod, {})
    return per.get(cls, 0) - (per.get("_extra", 0) if cls == "lut" else 0)


def compare_counts(first: LegReport, second: LegReport) -> None:
    """A-vs-b: the objects per class in the modules ``second`` holds (the writer writes the top
    only) against ``second``'s, less the tables the writer added. A module with fewer objects of a
    class after the write and read-back gets one ``count-drop`` defect per lost object: objects
    were lost, not only locations. A gain never offsets a drop in another module; it is only shown
    in ``versus`` (the writer adds buffer tables)."""
    for cls in OBJECT_CLASSES:
        before = sum(first.modules.get(m, {}).get(cls, 0) for m in second.modules)
        after = second.kept(cls)
        second.versus[cls] = (before, after)
        for mod in sorted(second.modules):
            lost = first.modules.get(mod, {}).get(cls, 0) - _kept_in(second, mod, cls)
            for n in range(1, lost + 1):
                second.defects.append(Defect("count-drop", mod, f"{cls}-{n}", cls))


def defect_id(d: Defect) -> str:
    """The key of a defect kind in an expect file: kind / class (a hard block: its type) / note."""
    if d.kind == "flow-error":
        return d.kind
    cls = object_class(d.ctype)
    what = cls if cls in ("lut", "latch", "lpm") else (d.ctype or "none")
    if d.kind == "count-drop":
        what = d.ctype
    return "/".join(x for x in (d.kind, what, d.note) if x)


def defect_ids(defects: Sequence[Defect]) -> Counter[str]:
    """Defect kind -> count. The id is the kind id plus ``@`` and the first 8 hex of the sha256 of
    the sorted ``module/name`` list of its objects, so swapping a fixed defect for a new one of the
    same kind and count changes the id. A flow-error has no objects."""
    groups: dict[str, list[str]] = {}
    for d in defects:
        groups.setdefault(defect_id(d), []).append(f"{d.module}/{d.name}")
    ids: Counter[str] = Counter()
    for base, objs in groups.items():
        if base == "flow-error":
            ids[base] += len(objs)
            continue
        digest = hashlib.sha256("\n".join(sorted(objs)).encode()).hexdigest()[:8]
        ids[f"{base}@{digest}"] += len(objs)
    return ids


def parse_allow(text: str | None) -> frozenset[str]:
    """The ``--allow`` set: the approved classes by default; a name outside them is an error."""
    if text is None:
        return DEFAULT_ALLOW
    names = [w for w in text.split(",") if w]
    for word in names:
        if word not in APPROVED:
            raise ToolError(f"--allow: unknown class {word!r} (want {', '.join(sorted(APPROVED))})")
    return frozenset(names)


def specificity(values: Sequence[int]) -> tuple[float, float]:
    """(median, 90th percentile by nearest rank); (0, 0) for none."""
    if not values:
        return 0.0, 0.0
    ordered = sorted(values)
    n = len(ordered)
    median = (ordered[(n - 1) // 2] + ordered[n // 2]) / 2
    return float(median), float(ordered[math.ceil(0.9 * n) - 1])


def object_class(ctype: str) -> str | None:
    """The P-1 class of a node type, None for a node that is no object."""
    if ctype in LUT_TYPES:
        return "lut"
    if LATCH_RE.match(ctype):
        return "latch"
    if ctype.startswith("lpm_") or ctype == "altsyncram":
        return "lpm"
    if ctype and not ctype.startswith("$"):
        return "hard"
    return None


def distinct_lines(src: str) -> int:
    locs: list[Loc] = parse_locs(src)
    return len({(x.file, x.line) for x in locs})


def location_count(src: str) -> int:
    return len(src.split("|")) if src else 0


class _Ctx:
    """What a node's rules need: the facts of its module, the node types of the dump (the JSON
    file calls a ``$sop`` a ``$lut``), the driver of every net, and the leg-(a) located nets."""

    def __init__(
        self, facts: Facts, rows: Sequence[Obj], located: Collection[tuple[str, str]]
    ) -> None:
        self.facts = facts
        self.located = located
        self.types = {(r.module, r.name): r.ctype for r in rows if r.kind == "node"}
        # one folded zero-input table per generator net of a module (gnd, vcc, unconn)
        self.folded = {module: len(mod.generators) for module, mod in facts.items()}
        self.drivers: dict[tuple[str, str], str] = {}
        for module, mod in facts.items():
            for name, cell in mod.cells.items():
                for out in cell.outs:
                    self.drivers[(module, out)] = name

    def cell(self, module: str, name: str) -> Cell | None:
        mod = self.facts.get(module)
        return mod.cells.get(name) if mod else None

    def driver_type(self, module: str, net: str) -> str:
        name = self.drivers.get((module, net))
        return self.types.get((module, name), "") if name is not None else ""


def _const_table(ctx: _Ctx, row: Obj, cell: Cell | None) -> bool:
    mod = ctx.facts.get(row.module)
    if mod is None:
        return False
    if cell is None:  # folded into a constant by write_json: a zero-input table
        # no pins to check, so the table must be one of the generator nets' own: at most one per
        # generator net, else a dropped real table would pass as a constant
        if ctx.folded.get(row.module, 0) <= 0:
            return False
        ctx.folded[row.module] -= 1
        return True
    if cell.ins or len(cell.outs) != 1:
        return False
    return mod.net_name(cell.outs[0]) in GENERATOR_NETS


def _port_note(ctx: _Ctx, row: Obj, cell: Cell | None) -> str:
    mod = ctx.facts.get(row.module)
    if cell is not None and mod is not None and any(o in mod.port_out for o in cell.outs):
        return "drives-output-port"
    return ""


def _port_buffer(ctx: _Ctx, row: Obj, cell: Cell | None) -> str | None:
    """'yes' (an exception), 'lost' (a located leg-(a) driver of the net) or None."""
    if cell is None or not cell.identity or len(cell.ins) != 1 or len(cell.outs) != 1:
        return None
    mod = ctx.facts[row.module]
    src, dst = cell.ins[0], cell.outs[0]
    if src == dst or not (dst in mod.port_out or src in mod.port_in):
        return None
    if (row.module, mod.net_name(dst)) in ctx.located:
        return "lost"
    return "yes"


def _port_driver(ctx: _Ctx, row: Obj, cell: Cell | None) -> bool:
    """True when the object drives a module output port bit that a located leg-(a) node drove."""
    mod = ctx.facts.get(row.module)
    if cell is None or mod is None:
        return False
    return any(
        o in mod.port_out and (row.module, mod.net_name(o)) in ctx.located for o in cell.outs
    )


def _not_helper(ctx: _Ctx, row: Obj, cell: Cell | None) -> bool:
    if cell is None or len(cell.ins) != 1:
        return False
    return ctx.driver_type(row.module, cell.ins[0]) == "$sop"


def classify(
    rows: Sequence[Obj],
    facts: Facts,
    *,
    allow: frozenset[str] = DEFAULT_ALLOW,
    inherits: bool = False,
    readback: bool = False,
    located: Collection[tuple[str, str]] = frozenset(),
    written: Mapping[str, int] | None = None,
) -> LegReport:
    """Counts the nodes of one typed dump. ``readback``: the dump is of a written file read back
    (legs b and c-rb); only there the P-3 exception rules apply, in memory (leg a, leg c) every
    object without a location is a loss. ``inherits``: the dump is a BLIF VTR read-back, where a
    located cell has its location through D-5. ``located``: (module, net name) pairs a located
    leg-(a) node drives. ``written``: the class counts of the written file, for the cross-check
    (None: not checked)."""
    ctx = _Ctx(facts, rows, located)
    rep = LegReport(
        objects=dict.fromkeys(OBJECT_CLASSES, 0), resolved=dict.fromkeys(OBJECT_CLASSES, 0)
    )
    for row in rows:
        if row.kind != "node" or row.ctype in PORT_TYPES:
            continue
        _count_node(ctx, row, rep, allow, inherits, readback)
    _cross_check(rep, written)
    return rep


def _count_node(
    ctx: _Ctx, row: Obj, rep: LegReport, allow: frozenset[str], inherits: bool, readback: bool
) -> None:
    cls = object_class(row.ctype)
    has = bool(row.src)
    cell = ctx.cell(row.module, row.name)
    if cls is None:
        _count_other(ctx, row, rep, cell, readback)
        return
    rep.objects[cls] += 1
    rep.nodes += 1
    per = rep.modules.setdefault(row.module, {})
    per[cls] = per.get(cls, 0) + 1
    if has:
        _count_located(row, rep, cls, allow, inherits)
        return
    if not readback:
        rep.defects.append(
            Defect("unresolved", row.module, row.name, row.ctype, _port_note(ctx, row, cell))
        )
        return
    if cls == "lut" and _const_table(ctx, row, cell):
        rep.exceptions["const-table"] += 1
        per["_extra"] = per.get("_extra", 0) + 1
        rep.removed += 1
        return
    verdict = _port_buffer(ctx, row, cell) if cls == "lut" else None
    if verdict != "yes" and _port_driver(ctx, row, cell):
        rep.exceptions["port-driver"] += 1
        rep.driver_buffers += verdict == "lost"  # the written file counts it as a buffer table
        rep.removed += 1
        return
    if verdict == "yes":
        rep.exceptions["port-buffer"] += 1
        per["_extra"] = per.get("_extra", 0) + 1
        rep.removed += 1
        return
    kind = "lost-location" if verdict == "lost" else "unresolved"
    rep.defects.append(Defect(kind, row.module, row.name, row.ctype, _port_note(ctx, row, cell)))


def _count_located(
    row: Obj, rep: LegReport, cls: str, allow: frozenset[str], inherits: bool
) -> None:
    rep.lines.append(distinct_lines(row.src))
    rep.capped += location_count(row.src) >= CAP
    if inherits:
        rep.exceptions["inherited"] += 1
        if "inherited" in allow:
            rep.resolved[cls] += 1
        else:
            rep.defects.append(Defect("unresolved", row.module, row.name, row.ctype))
        return
    rep.resolved[cls] += 1
    rep.direct += 1


def _count_other(ctx: _Ctx, row: Obj, rep: LegReport, cell: Cell | None, readback: bool) -> None:
    if row.ctype in CONST_TYPES:
        if not row.src and readback:
            rep.exceptions["const-driver"] += 1
            rep.nodes += 1
        elif not row.src:
            rep.defects.append(Defect("unresolved", row.module, row.name, row.ctype))
        return
    if row.src:
        return
    if readback and row.ctype in NOT_TYPES and _not_helper(ctx, row, cell):
        rep.exceptions["not-helper"] += 1
        rep.nodes += 1
        return
    rep.defects.append(Defect("unresolved", row.module, row.name, row.ctype))


def _cross_check(rep: LegReport, written: Mapping[str, int] | None) -> None:
    """P-3: the dump count of a class equals its count in the written file."""
    if written is None:
        return
    for name in ("const-table", "port-buffer", "not-helper"):
        have = rep.exceptions[name] + (rep.driver_buffers if name == "port-buffer" else 0)
        if name in written and written[name] != have:
            rep.mismatches.append(
                f"{name}: {have} in the dump, {written[name]} in the written file"
            )


# --- the written BLIF -------------------------------------------------------------------------


def _logical_lines(text: str) -> list[list[str]]:
    lines: list[list[str]] = []
    pending = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if line.endswith("\\"):
            pending += line[:-1] + " "
            continue
        words = (pending + line).split()
        pending = ""
        if words:
            lines.append(words)
    return lines


def written_counts(text: str) -> dict[str, int]:
    """The counts of the exception classes in the text of a written BLIF (the top model of the VTR
    dialect): ``const-table`` zero-input ``.names`` named gnd, vcc or unconn; ``port-buffer`` a
    one-input ``1 1`` ``.names`` between different nets, with an output net named without a
    location (such a cell inherits none, D-5) and an output or input port bit on one side;
    ``not-helper`` is always 0 (``read_blif`` reads an OFF-set cover as one ``$sop`` that keeps the
    cover: reader.c ``end_names``, no ``$not``); ``offset-cover`` counts the ``.names`` with an
    OFF-set row (output column ``0``), informational; and ``lut`` / ``latch`` totals."""
    inputs: set[str] = set()
    outputs: set[str] = set()
    names: list[tuple[list[str], list[list[str]]]] = []
    latches = 0
    for words in _logical_lines(text):
        head = words[0]
        if head == ".inputs":
            inputs.update(words[1:])
        elif head == ".outputs":
            outputs.update(words[1:])
        elif head == ".latch":
            latches += 1
        elif head == ".names":
            names.append((words[1:], []))
        elif not head.startswith(".") and names:
            names[-1][1].append(words)
    counts = dict.fromkeys(("const-table", "port-buffer", "not-helper", "offset-cover", "lut"), 0)
    for nets, rows in names:
        out, ins = nets[-1], nets[:-1]
        if not ins and out in GENERATOR_NETS:
            counts["const-table"] += 1
            continue
        counts["lut"] += 1
        buffer = len(ins) == 1 and rows == [["1", "1"]] and ins[0] != out
        if buffer and not LOC_SUFFIX.search(out) and (out in outputs or ins[0] in inputs):
            counts["port-buffer"] += 1
        if ins and any(r[-1] == "0" for r in rows):
            counts["offset-cover"] += 1
    counts["latch"] = latches
    return counts


# --- the report -------------------------------------------------------------------------------


def _pct(value: float | None) -> str:
    return "n/a" if value is None else f"{100 * value:.1f}%"


def render_table(items: Sequence[tuple[str, str, LegReport]]) -> str:
    """One block per design and leg: the per-class counts (every class, zero included), the
    shares, coverage, the strict figure and the specificity; then the defects (first ten)."""
    out: list[str] = []
    for design, leg, rep in items:
        out.append(f"== {design} leg {leg}: {'ok' if rep.ok else 'FAIL'}")
        for cls in rep.objects:
            out.append(f"  object {cls:<13} {rep.objects[cls]:>7} resolved {rep.resolved[cls]:>7}")
        base = max(rep.nodes, rep.blocks, 1)
        for name in EXCEPTIONS:
            n = rep.exceptions[name]
            out.append(f"  except {name:<13} {n:>7} share {_pct(n / base)}")
        out.append(
            f"  coverage {_pct(rep.coverage)}  strict (no inherited/pad) {_pct(rep.strict)}"
            f"  objects {rep.total}"
            f"  median {rep.median:g}  p90 {rep.p90:g}  capped {rep.capped}"
        )
        if rep.versus:
            out.append(
                "  a-vs-b "
                + " ".join(f"{c} {x}->{y}" for c, (x, y) in rep.versus.items())
                + "  (objects before and after the write and read-back, less writer tables)"
            )
        out += [f"  MISMATCH {m}" for m in rep.mismatches]
        out += [f"  DEFECT {d.text()}" for d in rep.named_defects()]
        if rep.defects:
            notes = Counter(d.note or "other" for d in rep.defects)
            out.append(
                "  defects by note: " + ", ".join(f"{k} {v}" for k, v in sorted(notes.items()))
            )
        if len(rep.defects) > NAMED_DEFECTS:
            out.append(f"  ... and {len(rep.defects) - NAMED_DEFECTS} more")
    return "\n".join(out) + "\n"


def report_dict(items: Sequence[tuple[str, str, LegReport]]) -> dict[str, Any]:
    designs: dict[str, dict[str, Any]] = {}
    for design, leg, rep in items:
        entry = designs.setdefault(design, {"design": design, "legs": {}})
        entry["legs"][leg] = {
            "ok": rep.ok,
            "objects": rep.objects,
            "resolved": rep.resolved,
            "exceptions": rep.exceptions,
            "coverage": rep.coverage,
            "strict": rep.strict,
            "versus": {c: list(v) for c, v in rep.versus.items()},
            "defect_ids": dict(defect_ids(rep.defects)),
            "median": rep.median,
            "p90": rep.p90,
            "capped": rep.capped,
            "defects": [d.text() for d in rep.named_defects()],
            "defect_count": len(rep.defects),
            "mismatches": rep.mismatches,
        }
    return {"tool": TOOL, "designs": list(designs.values())}


# --- driving ----------------------------------------------------------------------------------


Expect = dict[tuple[str, str], dict[str, tuple[int, str]]]


def parse_expect(text: str) -> Expect:
    """``(design, leg) -> {defect id: (count, reason)}`` from lines
    ``<leg> <design file name> <defect id> <count> <reason...>``. A ``flow-error`` is never
    listed."""
    table: Expect = {}
    for raw in text.splitlines():
        words = raw.split("#", 1)[0].split()
        if not words:
            continue
        if len(words) < 5 or not words[3].isdigit():
            raise ToolError(
                f"expect line wants '<leg> <design> <defect id> <count> <reason>': {raw!r}"
            )
        if words[2].startswith("flow-error"):
            raise ToolError(f"a flow-error is never excepted: {raw!r}")
        entry = table.setdefault((words[1], words[0]), {})
        if words[2] in entry:
            raise ToolError(f"expect: {words[2]} listed twice for {words[0]} {words[1]}")
        entry[words[2]] = (int(words[3]), " ".join(words[4:]))
    return table


def judge(
    items: Sequence[tuple[str, str, LegReport]],
    expect: Expect,
    requested: Collection[str] | None = None,
) -> tuple[int, int, list[str]]:
    """(failed, excepted, problems). A leg fails unless it is clean or every defect kind it has is
    listed with exactly its count (more is a new defect; fewer is a stale line, a problem). A
    mismatch or a flow-error is never excepted. A listed leg that passes, or that did not run for a
    design that ran, is stale; a listed leg that was not in ``requested`` (``c-rb`` counts as ``c``)
    is not checked for that."""
    bad = excepted = 0
    problems: list[str] = []
    seen: set[tuple[str, str]] = set()
    for design, leg, rep in items:
        key = (design, leg)
        seen.add(key)
        listed = expect.get(key, {})
        found = defect_ids(rep.defects)
        if rep.ok:
            problems += [f"stale expect line: {leg} {design} {i}" for i in sorted(listed)]
            continue
        extra = [i for i, n in found.items() if i not in listed or n > listed[i][0]]
        stale = [i for i in listed if found.get(i, 0) < listed[i][0]]
        problems += [
            f"stale expect line: {leg} {design} {i} lists {listed[i][0]}, found {found.get(i, 0)}"
            for i in sorted(stale)
        ]
        if rep.mismatches or extra or "flow-error" in {i.split("/")[0] for i in found}:
            bad += 1
        else:
            excepted += 1
    problems += [
        f"stale expect line: {leg} {design} did not run"
        for design, leg in sorted(expect)
        if design in {d for d, _ in seen}
        and (design, leg) not in seen
        and (requested is None or leg.split("-")[0] in requested)
    ]
    return bad, excepted, problems


def run_designs(args: argparse.Namespace, work: Path) -> list[tuple[str, str, LegReport]]:
    """The reports of every design and leg (legs.py runs odin3)."""
    import legs

    return legs.run_designs(args, work)


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n")[0])
    ap.add_argument("--legs", default="a,b", help="a, b, c, d (comma separated)")
    ap.add_argument("--vpr", type=Path, help="leg d: the vpr binary (default $VTR_ROOT/vpr/vpr)")
    ap.add_argument("--vpr-timeout", type=float, default=1800.0,
                    help="leg d: seconds per design for VPR (default 1800)")
    ap.add_argument("--arch", type=Path, help="leg d: the architecture XML (default k6_frac)")
    ap.add_argument("--allow", help="exception classes to enable (default inherited,pad)")
    ap.add_argument("--odin3", default=None)
    ap.add_argument("--report", type=Path, help="write report.json here")
    ap.add_argument(
        "--expect", type=Path, help="known defects: '<leg> <design> <defect id> <count> <reason>'"
    )
    ap.add_argument("--work", type=Path, help="keep every file here")
    ap.add_argument("--smallest-first", action="store_true")
    ap.add_argument("files", nargs="*", type=Path)
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    import shutil
    import tempfile

    args = parse(argv)
    keep = args.work is not None
    work = Path(args.work).resolve() if keep else Path(tempfile.mkdtemp(prefix="prov-cover-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        args.allow_set = parse_allow(args.allow)
        items = run_designs(args, work)
        sys.stdout.write(render_table(items))
        if args.report:
            args.report.write_text(
                json.dumps(report_dict(items), indent=2) + "\n", encoding="utf-8"
            )
        if not items:
            print(f"{TOOL}: nothing was compared", file=sys.stderr)
            return EXIT_FAIL
        expect = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
        bad, excepted, problems = judge(items, expect, args.legs.split(","))
        for line in problems:
            print(f"{TOOL}: {line}")
        print(f"{TOOL}: compared {len(items)} legs, {bad} failed, {excepted} excepted", flush=True)
        return EXIT_FAIL if bad or problems else EXIT_OK
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
