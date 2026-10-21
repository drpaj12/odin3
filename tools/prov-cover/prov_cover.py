"""prov-cover: how many final-netlist objects have an HDL location (6.5b, spec 6 section 5).

Usage::

    prov-cover [--legs a,b,c] [--allow inherited,pad] [--odin3 O] [--report report.json]
               [--work DIR] [--smallest-first] DESIGN.v...

P-1 to P-8 of the spec.

Per design and leg the tool drives the ``odin3`` CLI (``legs.py``), takes ``dump_locs --types`` at
the leg's point and counts the objects P-1 names: LUTs (``$lut``, and the ``$sop`` a BLIF ``.names``
reads back as), latches and flip-flops, hard blocks (any other type that is not ``$`` and not an
LPM) and LPM instances (``lpm_*``, ``altsyncram``). An object is resolved when its SRC is non-empty.

Legs: (a) BLIF after ``abc``, in memory; (b) the same BLIF written and read back with
``--hdl-locs``; (c) the LPM flow, in memory and (``c2``) written and read back; (d) the VPR
``.place`` file (``place_check``; the tool never starts VPR).

P-3 exception classes, each counted, none silent (the report lists every class, zero included,
and the strict figure with none of them). The rules, evidence in brackets:

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
``inherited``     (``--allow``) a cell whose location exists only through 5.4 D-5: in a BLIF VTR
                  read-back every located cell, as the dialect writes none for a cell (X-6).
``pad``           (``--allow``) a VPR I/O pad block of the ``.place`` file (5.4 X-8).

The objects of a class need the connections of the design: ``dump_locs`` lists no pins, so the
tool also writes ``write_json`` at the same point and joins the cells by name (``Facts``). Every
exception class (but ``inherited`` and ``pad``) is also counted in the written BLIF text
(``written_counts``); the two counts must be equal, else the rule is wrong and the gate fails.

Gate (P-4): exit status 0 when no object is unresolved outside the classes, no cross-check
differs, and something was compared. Anything else unresolved is a defect; the first ten are
named. Recorded (P-5): objects per class, resolved, coverage (resolved / objects less the
removed exceptions), the strict figure (directly located / all objects), and the specificity
(median and p90 of distinct file:line pairs per object, ``capped`` = objects at the 8-location
cap of 5.4 W-3).

Exit codes: 0 gate passes; 1 a defect, a mismatch or nothing compared; 2 usage or tool error; 3 a
child crashed or timed out.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
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
EXCEPTIONS = ("const-driver", "const-table", "port-buffer", "not-helper", "inherited", "pad")
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

    def net_name(self, net: str) -> str:
        return self.names.get(net, net)


Facts = Mapping[str, Mod]


@dataclass(frozen=True)
class Defect:
    kind: str  # unresolved, lost-location
    module: str
    name: str
    ctype: str

    def text(self) -> str:
        return f"{self.kind}: {self.module}.{self.name} ({self.ctype or 'no type'})"


@dataclass
class LegReport:
    """The numbers of one design and leg."""

    objects: dict[str, int] = field(default_factory=dict)
    resolved: dict[str, int] = field(default_factory=dict)
    exceptions: dict[str, int] = field(default_factory=lambda: dict.fromkeys(EXCEPTIONS, 0))
    direct: int = 0  # objects located without an exception class (the strict figure)
    removed: int = 0  # objects of the exception classes that stand in for a location
    defects: list[Defect] = field(default_factory=list)
    mismatches: list[str] = field(default_factory=list)
    lines: list[int] = field(default_factory=list)
    capped: int = 0
    blocks: int = 0
    nodes: int = 0  # the nodes counted: objects and the exception nodes that are not objects

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

    def named_defects(self) -> list[Defect]:
        return self.defects[:NAMED_DEFECTS]


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
    if cell is None or cell.ins or len(cell.outs) != 1:
        return False
    return ctx.facts[row.module].net_name(cell.outs[0]) in GENERATOR_NETS


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
    located: Collection[tuple[str, str]] = frozenset(),
    written: Mapping[str, int] | None = None,
) -> LegReport:
    """Counts the nodes of one typed dump. ``inherits``: the dump is a BLIF VTR read-back, where
    a located cell has its location through D-5. ``located``: (module, net name) pairs a located
    leg-(a) node drives. ``written``: the class counts of the written file, for the cross-check
    (None: not checked)."""
    ctx = _Ctx(facts, rows, located)
    rep = LegReport(
        objects=dict.fromkeys(OBJECT_CLASSES, 0), resolved=dict.fromkeys(OBJECT_CLASSES, 0)
    )
    for row in rows:
        if row.kind != "node" or row.ctype in PORT_TYPES:
            continue
        _count_node(ctx, row, rep, allow, inherits)
    _cross_check(rep, written)
    return rep


def _count_node(ctx: _Ctx, row: Obj, rep: LegReport, allow: frozenset[str], inherits: bool) -> None:
    cls = object_class(row.ctype)
    has = bool(row.src)
    cell = ctx.cell(row.module, row.name)
    if cls is None:
        _count_other(ctx, row, rep, cell)
        return
    rep.objects[cls] += 1
    rep.nodes += 1
    if has:
        _count_located(row, rep, cls, allow, inherits)
        return
    if cls == "lut" and _const_table(ctx, row, cell):
        rep.exceptions["const-table"] += 1
        rep.removed += 1
        return
    verdict = _port_buffer(ctx, row, cell) if cls == "lut" else None
    if verdict == "yes":
        rep.exceptions["port-buffer"] += 1
        rep.removed += 1
        return
    kind = "lost-location" if verdict == "lost" else "unresolved"
    rep.defects.append(Defect(kind, row.module, row.name, row.ctype))


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


def _count_other(ctx: _Ctx, row: Obj, rep: LegReport, cell: Cell | None) -> None:
    if row.ctype in CONST_TYPES:
        if not row.src:
            rep.exceptions["const-driver"] += 1
            rep.nodes += 1
        return
    if row.src:
        return
    if row.ctype in NOT_TYPES and _not_helper(ctx, row, cell):
        rep.exceptions["not-helper"] += 1
        rep.nodes += 1
        return
    rep.defects.append(Defect("unresolved", row.module, row.name, row.ctype))


def _cross_check(rep: LegReport, written: Mapping[str, int] | None) -> None:
    """P-3: the dump count of a class equals its count in the written file."""
    if written is None:
        return
    for name in ("const-table", "port-buffer", "not-helper"):
        if name in written and written[name] != rep.exceptions[name]:
            rep.mismatches.append(
                f"{name}: {rep.exceptions[name]} in the dump, {written[name]} in the written file"
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


# --- leg (d): the .place file -----------------------------------------------------------------


def place_check(
    text: str,
    resolved: Collection[str],
    ports: Collection[str],
    allow: frozenset[str] = DEFAULT_ALLOW,
) -> LegReport:
    """P-4 leg (d): every block of a VPR ``.place`` resolves, through its netlist name (a name
    that carries ``@file:line`` is the L-5 decode; else it must be in ``resolved``, the names of
    the located leg-(b) objects), unless it is a pad (a block named for a port bit, or
    ``out:`` + a port bit) and ``pad`` is allowed."""
    rep = LegReport(objects={"block": 0}, resolved={"block": 0})
    for line in text.splitlines():
        words = line.split()
        if len(words) < 5 or line.startswith(("#", "Netlist_File", "Array size")):
            continue
        name = words[0]
        rep.blocks += 1
        base = name[4:] if name.startswith("out:") else name
        if base in ports and "pad" in allow:
            rep.exceptions["pad"] += 1
            rep.removed += 1
            rep.objects["block"] += 1
            continue
        rep.objects["block"] += 1
        if LOC_SUFFIX.search(name) or name in resolved:
            rep.resolved["block"] += 1
            rep.direct += 1
        else:
            rep.defects.append(Defect("unresolved", "", name, "block"))
    rep.objects["block"] -= rep.exceptions["pad"]
    rep.removed -= rep.exceptions["pad"]
    return rep


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
            f"  coverage {_pct(rep.coverage)}  strict {_pct(rep.strict)}  objects {rep.total}"
            f"  median {rep.median:g}  p90 {rep.p90:g}  capped {rep.capped}"
        )
        out += [f"  MISMATCH {m}" for m in rep.mismatches]
        out += [f"  DEFECT {d.text()}" for d in rep.named_defects()]
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
            "median": rep.median,
            "p90": rep.p90,
            "capped": rep.capped,
            "defects": [d.text() for d in rep.named_defects()],
            "defect_count": len(rep.defects),
            "mismatches": rep.mismatches,
        }
    return {"tool": TOOL, "designs": list(designs.values())}


# --- driving ----------------------------------------------------------------------------------


def run_designs(args: argparse.Namespace, work: Path) -> list[tuple[str, str, LegReport]]:
    """The reports of every design and leg (legs.py runs odin3)."""
    import legs

    return legs.run_designs(args, work)


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n")[0])
    ap.add_argument("--legs", default="a,b", help="a, b, c (comma separated)")
    ap.add_argument("--allow", help="exception classes to enable (default inherited,pad)")
    ap.add_argument("--odin3", default=None)
    ap.add_argument("--report", type=Path, help="write report.json here")
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
        bad = sum(not rep.ok for _, _, rep in items)
        print(f"{TOOL}: compared {len(items)} legs, {bad} failed", flush=True)
        return EXIT_FAIL if bad else EXIT_OK
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
