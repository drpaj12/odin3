"""prov_post: extend the Odin III name map with what VPR did (V1.2.0 Task 8; docs/V1.2.0.md 6).

    odin3-prov-post FLOW_DIR [--map FILE] [--net FILE] [--place FILE] [--blif FILE] [--top NAME]
                             [--run-dir DIR]

``FLOW_DIR`` is an ``odin3-flow`` output directory made with ``--prov --vpr --keep-results``: the
BLIF VPR read (hash names, ``odin3/<top>.blif``), its version 2 name map (``<blif>.names``) and
VPR's packed netlist (``.net``) and placement (``.place``). The tool appends, in the same file and
schema (3.2), to the map:

* ``R`` lines of level ``ble`` and ``clb`` (``dir`` ``coarsen:lut`` and ``coarsen:ble``, step a new
  ``T`` row ``pack``; parents: the records of the atoms inside, and the BLEs of a cluster);
* ``R`` lines of level ``site`` (``dir`` ``rewrite:clb``, step a new ``T`` row ``place``; parent
  the cluster's record; the ``.place`` coordinates in ``flags`` as ``x=``, ``y=``, ``sub=`` and,
  when the file has the column, ``layer=``);
* ``A`` lines keyed by atom name: the atom's BLE, CLB and site records.

Joins follow 3.3 unchanged (anchors, the reduced set J, the join of instances): a CLB or BLE whose
atoms blend distinct anchors is labelled a ``join`` by ``pack``. The new records are numbered by the
numbering algorithm of 3.2 (post-order from the ``A`` lines in line order). The map is rewritten
atomically (a ``mkstemp`` file in the map's directory, then ``os.replace``; the tool removes that
file if the write fails); a map that already holds ``A`` lines or ``pack``
records is refused. A BLIF written with short names is refused (no map is keyed by them). The
``.net`` is trusted local VPR output, parsed with ElementTree.

Importable: ``extend_flow_dir(Path) -> PostResult``. Exit codes: 0 ok, 1 refused or failed (missing
or inconsistent input, short-name BLIF, map already extended), 2 usage error.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import re
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import name_map_v2
from name_map_v2 import MapRecord, NameMap

LOG = logging.getLogger("odin3")
MAP_MATCH_MIN = 0.9  # least share of the .net's atoms the name map must hold
JOIN_CAP = 16  # members of J listed in cons (3.3, "one cap rule")
ANCHOR_RANK = 5  # a construct of this rank (stmt) or coarser is its own anchor
LEVEL_RANK = {"module": 1, "instance": 2, "block": 3, "branch": 4, "stmt": 5, "expr": 6}
NO_SOURCE_ATOMS = frozenset(("gnd", "vcc", "unconn"))  # constant generators: no source object
PAD_STEMS = frozenset(("inpad", "outpad"))  # primitives of an I/O block: not in any BLE
PACK, PLACE = "pack", "place"
OP_BLE, OP_CLB, OP_SITE = 1, 2, 1  # operation numbers within a pass run (renumbered when written)
BLE_ATOMS_WARN = 10  # a single BLE with more atoms than this: the level-1 block is not a BLE
_ESCAPES = (("%", "%25"), ("\t", "%09"), ("\n", "%0A"), ("\\", "%5C"), (",", "%2C"), ("|", "%7C"))
_SPAN = re.compile(r"(\d+)\.(\d+)-(\d+)\.(\d+)")
_STEM = re.compile(r"^([^\[]*)")


class PostError(Exception):
    """An input is missing, unreadable or inconsistent, or the tool refuses the map."""


class UsageError(Exception):
    pass


def escape(text: str) -> str:
    """A name map field (version 2): %25 %09 %0A %5C %2C %7C, and %23 for a leading '#'."""
    for raw, esc in _ESCAPES:
        text = text.replace(raw, esc)
    return "%23" + text[1:] if text.startswith("#") else text


# --- the graph: anchors, J, instance chains (3.3) -------------------------------------------------


@dataclass(frozen=True)
class Info:
    """What the walk behind a record finds: the distinct anchors of its construct leaves (sorted
    construct numbers), whether a span-only leaf is behind it and whether a context record is."""

    anchors: tuple[int, ...]
    span_only: bool
    has_ctx: bool


@dataclass(frozen=True)
class Label:
    """The join labels of one coarsen record."""

    join: bool
    instance: bool
    cons: tuple[int, ...]  # J, at most JOIN_CAP members, as construct numbers
    n_joined: int
    span_only: bool


Pairs = frozenset[tuple[int, int]]
MULTI = None  # in place of Pairs: an anchor reached through two context chains


class Graph:
    """Section 3.3 over a parsed map. ``nm.records`` may grow after construction (new records are
    appended in number order); a record is only read when asked for."""

    def __init__(self, nm: NameMap) -> None:
        self.nm = nm
        self._info: dict[int, Info] = {}
        self._pairs: dict[int, Pairs | None] = {}
        self._chains: dict[tuple[int, int], int] = {}
        self._row_key: dict[int, tuple[object, ...]] = {}
        self._chain_key: dict[int, tuple[tuple[object, ...], ...]] = {}
        self._anchor: dict[int, int] = {}
        self._reduced: dict[tuple[int, ...], tuple[int, ...]] = {}

    # records and constructs ------------------------------------------------------------------
    def rec(self, number: int) -> MapRecord | None:
        if 1 <= number <= len(self.nm.records):
            return self.nm.records[number - 1]
        return None

    def ctx_tail(self, rec: MapRecord) -> int:
        """The declared context tail (``ctx=<n>``), clamped to the parent count."""
        for flag in rec.flags:
            if flag.startswith("ctx=") and flag[4:].isdigit():
                return min(int(flag[4:]), len(rec.parents))
        return 0

    def data_parents(self, number: int, rec: MapRecord) -> list[int]:
        """The data parents of a record: all but the context tail, only numbers that resolve and
        come before the record (a bad reference is `names --check`'s to report)."""
        count = len(rec.parents)
        return [p for p in rec.parents[:count - self.ctx_tail(rec)] if 1 <= p < number]

    def construct_of(self, rec: MapRecord) -> int:
        """The construct row of a SOURCE or IMPORTED record, 0 when it has none."""
        if rec.kind == "D" or not rec.constructs:
            return 0
        row = rec.constructs[0]
        return row if 1 <= row <= len(self.nm.constructs) else 0

    def anchor_of(self, row: int) -> int:
        """The construct if its level is stmt or coarser, else its nearest such ancestor."""
        first = row
        seen: list[int] = []
        while row not in self._anchor:
            seen.append(row)
            cur = self.nm.constructs[row - 1]
            parent = cur.parent
            if LEVEL_RANK.get(cur.level, 99) <= ANCHOR_RANK or not 1 <= parent <= len(
                    self.nm.constructs) or len(seen) > len(self.nm.constructs):
                self._anchor[row] = row
                break
            row = parent
        found = self._anchor[row]
        for item in seen:
            self._anchor[item] = found
        return self._anchor[first]

    # the walk ----------------------------------------------------------------------------------
    def info(self, number: int) -> Info:
        """The Info of a record, filled bottom-up (iterative: the graph can be deep)."""
        stack = [number]
        while stack:
            top = stack[-1]
            if top in self._info:
                stack.pop()
                continue
            rec = self.rec(top)
            if rec is None:
                self._info[top] = Info((), False, False)
                continue
            if rec.kind != "D":
                row = self.construct_of(rec)
                self._info[top] = Info((self.anchor_of(row),) if row else (), row == 0, False)
                stack.pop()
                continue
            data = self.data_parents(top, rec)
            pending = [p for p in data if p not in self._info]
            if pending:
                stack.extend(pending)
                continue
            stack.pop()
            parts = [self._info[p] for p in data]
            if len(parts) == 1:
                anchors = parts[0].anchors
            else:
                anchors = tuple(sorted({a for part in parts for a in part.anchors}))
            self._info[top] = Info(anchors, any(p.span_only for p in parts),
                                   self.ctx_tail(rec) > 0 or any(p.has_ctx for p in parts))
        return self._info[number]

    # J and its key order -----------------------------------------------------------------------
    def row_key(self, row: int) -> tuple[object, ...]:
        """(file path, line, col, end line, end col, kind, name) of a construct (3.3 key; the kind
        by its name: agent default, the map holds no kind code)."""
        key = self._row_key.get(row)
        if key is None:
            con = self.nm.constructs[row - 1]
            match = _SPAN.fullmatch(con.span)
            span = tuple(int(g) for g in match.groups()) if match else (0, 0, 0, 0)
            path = self.nm.files[con.file - 1] if 1 <= con.file <= len(self.nm.files) else ""
            key = (path.encode(), *span, con.kind.encode(), con.name.encode())
            self._row_key[row] = key
        return key

    def chain_key(self, row: int) -> tuple[tuple[object, ...], ...]:
        """The key of the chain from the module down to the construct."""
        key = self._chain_key.get(row)
        if key is None:
            path: list[int] = []
            cur = row
            while cur and len(path) <= len(self.nm.constructs):
                path.append(cur)
                cur = self.nm.constructs[cur - 1].parent if 1 <= cur <= len(
                    self.nm.constructs) else 0
            key = tuple(self.row_key(r) for r in reversed(path))
            self._chain_key[row] = key
        return key

    def reduced(self, anchors: tuple[int, ...]) -> tuple[int, ...]:
        """J: the anchors without any that is an ancestor of another, in key order, a chain
        once."""
        found = self._reduced.get(anchors)
        if found is not None:
            return found
        members = set(anchors)
        covered: set[int] = set()
        for row in anchors:
            up = self.nm.constructs[row - 1].parent
            for _ in range(len(self.nm.constructs)):
                if not 1 <= up <= len(self.nm.constructs):
                    break
                if up in members:
                    covered.add(up)
                up = self.nm.constructs[up - 1].parent
        keep = sorted((r for r in anchors if r not in covered), key=lambda r: (self.chain_key(r),
                                                                               r))
        out: list[int] = []
        for row in keep:
            if not out or self.chain_key(row) != self.chain_key(out[-1]):
                out.append(row)
        self._reduced[anchors] = tuple(out)
        return self._reduced[anchors]

    def same_set(self, left: tuple[int, ...], right: tuple[int, ...]) -> bool:
        """Two reduced sets hold the same constructs, one by one."""
        return len(left) == len(right) and all(
            self.chain_key(a) == self.chain_key(b) for a, b in zip(left, right, strict=True))

    # instance chains ---------------------------------------------------------------------------
    def chain_id(self, first: int, rest: int) -> int:
        key = (first, rest)
        if key not in self._chains:
            self._chains[key] = len(self._chains) + 1
        return self._chains[key]

    def _parent_pairs(self, parent: int) -> Pairs | None:
        info = self.info(parent)
        if not info.has_ctx:
            return frozenset((a, 0) for a in info.anchors)
        return self._pairs[parent]

    def pairs(self, number: int) -> Pairs | None:
        """The (anchor, chain) pairs behind a record that has a context record behind it, or
        MULTI when an anchor is reached through two chains."""
        stack = [number]
        while stack:
            top = stack[-1]
            if top in self._pairs:
                stack.pop()
                continue
            rec = self.rec(top)
            if rec is None or rec.kind == "S" or rec.kind == "I":
                self._pairs[top] = frozenset()
                continue
            data = self.data_parents(top, rec)
            pending = [p for p in data if self.info(p).has_ctx and p not in self._pairs]
            if pending:
                stack.extend(pending)
                continue
            stack.pop()
            self._pairs[top] = self._combine(rec, data)
        return self._pairs[number]

    def _combine(self, rec: MapRecord, data: list[int]) -> Pairs | None:
        pairs: set[tuple[int, int]] = set()
        for parent in data:
            part = self._parent_pairs(parent)
            if part is None:
                return MULTI
            pairs |= part
        tail = rec.parents[len(rec.parents) - self.ctx_tail(rec):]
        if tail:
            moved: set[tuple[int, int]] = set()
            for anchor, chain in pairs:
                for ctx in reversed(tail):
                    chain = self.chain_id(ctx, chain)
                moved.add((anchor, chain))
            pairs = moved
        chains: dict[int, int] = {}
        for anchor, chain in pairs:
            if chains.setdefault(anchor, chain) != chain:
                return MULTI
        return frozenset(pairs)

    # the label ---------------------------------------------------------------------------------
    def label(self, number: int) -> Label:
        """The join labels of a coarsen record (3.3)."""
        rec = self.rec(number)
        assert rec is not None
        info = self.info(number)
        joined = self.reduced(info.anchors)
        data = self.data_parents(number, rec)
        same = any(self.same_set(joined, self.reduced(self.info(p).anchors)) for p in data)
        instance = info.has_ctx and self.pairs(number) is None
        join = len(joined) >= 2 and not same
        listed = join or instance or len(joined) > JOIN_CAP
        cons = joined[:JOIN_CAP] if listed else ()
        return Label(join, instance, cons, len(joined), info.span_only)


# --- VPR's files ------------------------------------------------------------------------------


@dataclass
class Ble:
    """A BLE of the .net: the atoms inside (names, in document order)."""

    atoms: list[str]
    record: int = 0


@dataclass
class Cluster:
    """A top-level block of the .net: its BLEs, the atoms outside any BLE (pads), in document
    order of the level-1 blocks."""

    name: str
    kind: str  # the instance stem: clb, io, memory, ...
    parts: list[Ble | str]  # a Ble, or the name of a loose atom
    atoms: list[str] = field(default_factory=list)
    clb: int = 0
    site: int = 0
    place: tuple[str, ...] = ()


def primitives(block: ET.Element) -> list[tuple[str, str]]:
    """(atom name, instance stem) of every primitive under a block, in document order. A
    primitive is a block with no sub-block whose name is not ``open`` and whose mode is not
    ``wire`` (a wire-mode leaf is named after a net)."""
    out: list[tuple[str, str]] = []
    stack = [block]
    while stack:
        cur = stack.pop()
        kids = cur.findall("block")
        if not kids:
            name = cur.get("name", "")
            if cur.get("mode") == "wire":  # a routing-only block: its name is a net, not an atom
                continue
            if name and name != "open":
                out.append((name, _STEM.match(cur.get("instance", "")).group(1)))  # type: ignore[union-attr]
        stack.extend(reversed(kids))
    return out


def read_net(path: Path) -> list[Cluster]:
    """The clusters of a VPR packed netlist (.net, XML): each top-level block, its BLEs (a
    level-1 block that holds a primitive other than a pad: a BLE of the cluster, `fle` in
    k6_frac) and its loose atoms (pads, and primitives of a level-1 block of pads)."""
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        raise PostError(f"cannot read the packed netlist {path}: {exc}") from exc
    clusters: list[Cluster] = []
    seen: set[str] = set()
    for top in root.findall("block"):
        name = top.get("name", "")
        if not name or name == "open":
            continue
        kind = _STEM.match(top.get("instance", "")).group(1)  # type: ignore[union-attr]
        cluster = Cluster(name, kind, [])
        kids = top.findall("block")
        for kid in kids or [top]:
            prims = primitives(kid)
            if not prims:
                continue
            if any(stem not in PAD_STEMS for _, stem in prims) and kids:
                ble = Ble([n for n, _ in prims])
                cluster.parts.append(ble)
            else:
                cluster.parts.extend(n for n, _ in prims)
            cluster.atoms.extend(n for n, _ in prims)
        for atom in cluster.atoms:
            if atom in seen:
                raise PostError(f"{path}: the atom name '{atom}' occurs twice in the packed "
                                f"netlist: the A lines are keyed by atom name")
            seen.add(atom)
        bles = [p for p in cluster.parts if isinstance(p, Ble)]
        if len(bles) == 1 and len(bles[0].atoms) > BLE_ATOMS_WARN:
            LOG.warning("%s: cluster '%s' holds one BLE of %d atoms: is the architecture's "
                        "level-1 block a BLE? (the BLE records would equal the CLB's)", path,
                        name, len(bles[0].atoms))
        if cluster.atoms:
            clusters.append(cluster)
    return clusters


def read_place(path: Path) -> dict[str, tuple[str, ...]]:
    """block name -> ('x=..', 'y=..', 'sub=..'[, 'layer=..']) from a VPR .place file. The layer
    column is read when the header or the row has it."""
    out: dict[str, tuple[str, ...]] = {}
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError as exc:
        raise PostError(f"cannot read the placement {path}: {exc}") from exc
    for line in lines:
        text = line.split("#", 1)[0].strip()
        if not text or text.startswith(("Netlist_File:", "Array size:")):
            continue
        fields = text.split()
        if len(fields) < 4 or not all(f.lstrip("-").isdigit() for f in fields[1:]):
            continue
        keys = ("x", "y", "sub", "layer")[:len(fields) - 1]
        out[fields[0]] = tuple(f"{k}={v}" for k, v in zip(keys, fields[1:], strict=False))
    return out


# --- inputs -----------------------------------------------------------------------------------


@dataclass
class Inputs:
    map: Path
    net: Path
    place: Path
    blif: Path | None
    top: str | None


def one(paths: list[Path], what: str, where: Path) -> Path:
    if len(paths) != 1:
        raise PostError(f"{where} holds {len(paths)} {what} files, want exactly 1 (give the file "
                        f"with an option)" if paths else f"no {what} file in {where}: run "
                        f"odin3-flow with --prov --vpr --keep-results")
    return paths[0]


def run_dirs(flow: Path) -> list[Path]:
    """Directories that hold a VPR run's .net and .place: the flow's own and its retries."""
    found = [flow / "vpr" / "run", *sorted((flow / "vpr").glob("retry-*/run"))]
    return [d for d in found if any(d.glob("*.net")) and any(d.glob("*.place"))]


def find_inputs(flow: Path, ns: argparse.Namespace | None = None) -> Inputs:
    """Where the BLIF, its map and VPR's files are: ``timing/inputs.json`` when the flow wrote
    one, else ``odin3/*.blif`` and the VPR run directory under ``vpr/``."""
    opt = ns if ns is not None else argparse.Namespace(map=None, net=None, place=None, blif=None,
                                                       top=None, run_dir=None)
    inputs: dict[str, object] = {}
    path = flow / "timing" / "inputs.json"
    if path.is_file():
        try:
            inputs = json.loads(path.read_text())
        except (OSError, ValueError) as exc:
            raise PostError(f"cannot read {path}: {exc}") from exc

    def rel(key: str) -> Path | None:
        value = inputs.get(key)
        return None if not isinstance(value, str) or not value else flow / value

    blif = opt.blif or rel("blif")
    if blif is None:
        blifs = sorted((flow / "odin3").glob("*.blif"))
        blif = blifs[0] if len(blifs) == 1 else None
    names = opt.map or rel("name_map") or (blif.with_name(blif.name + ".names") if blif else None)
    if names is None:
        raise PostError(f"no BLIF and no name map found in {flow}: run odin3-flow with --prov")
    run = opt.run_dir or rel("vpr_run_dir")
    if run is None:
        dirs = run_dirs(flow)
        if not dirs:
            raise PostError(f"no VPR run directory with a .net and a .place in {flow} (vpr/run, "
                            f"vpr/retry-*/run): run odin3-flow with --prov --vpr --keep-results")
        if len(dirs) != 1:
            raise PostError(f"{flow} has {len(dirs)} VPR run directories with a .net and a "
                            f".place (vpr/run, vpr/retry-*/run); give --run-dir or --net/--place")
        run = dirs[0]
    net = opt.net or one(sorted(Path(run).glob("*.net")), ".net", Path(run))
    place = opt.place or one(sorted(Path(run).glob("*.place")), ".place", Path(run))
    top = opt.top or inputs.get("top")
    return Inputs(Path(names), Path(net), Path(place), Path(blif) if blif else None,
                  top if isinstance(top, str) else None)


def blif_names_style(blif: Path) -> str | None:
    """The ``names <style>`` word of the writer's header comment, None when there is none."""
    try:
        with blif.open(errors="replace") as fh:
            for _ in range(4):
                match = re.search(r"\(K=\d+, names (\w+)\)", fh.readline())
                if match:
                    return match.group(1)
    except OSError:
        return None
    return None


# --- the extension ----------------------------------------------------------------------------


@dataclass
class NewRec:
    """A record the tool makes. ``parents`` holds ints (existing record numbers) and NewRec."""

    level: str
    dir: str
    op: int
    run: str
    parents: list[int | NewRec]
    flags: tuple[str, ...] = ()
    number: int = 0
    label: Label | None = None


@dataclass
class PostResult:
    map_path: Path
    atoms: int
    unmatched: int
    bles: int
    clbs: int
    sites: int
    joins: int
    instance_joins: int
    lossy: int
    bytes_before: int
    bytes_after: int
    lines: list[str] = field(default_factory=list)


class Resolver:
    """The atom name join (6, "Atom name join"): an atom's record from the O or N line keyed by
    its name, a pad (``out:<port>``) by its port; a provenance-named BLIF by the ``full`` field."""

    def __init__(self, nm: NameMap, top: str | None) -> None:
        self.nm, self.top = nm, top
        self.full: dict[str, name_map_v2.MapObject] | None = None

    def lookup(self, name: str) -> name_map_v2.MapObject | None:
        obj = self.nm.atom(name, self.top)
        if obj is None and name.startswith("out:"):
            obj = self.nm.atom(name[4:], self.top)
        if obj is None:
            if self.full is None:
                self.full = {o.full: o for o in self.nm.objects if o.full}
            obj = self.full.get(name)
        return obj

    def record(self, name: str) -> int:
        obj = self.lookup(name)
        return obj.records[0] if obj is not None and obj.records else 0


def check_extendable(nm: NameMap, path: Path) -> None:
    if nm.version != 2:
        raise PostError(f"{path} is a version {nm.version} name map: the post-Odin tool needs "
                        f"version 2 (odin3-flow --prov)")
    levels = {"ble", "clb", "site"}
    if nm.atoms or any(r.level in levels for r in nm.records) or any(
            run[0] in (PACK, PLACE) for run in nm.runs):
        raise PostError(f"{path} already holds post-Odin lines (A lines or pack/place records): "
                        f"refused; write a fresh map with odin3-flow --prov and run the tool once")


def check_names(blif: Path | None, share: float, matched: int, total: int) -> None:
    """Refuse a short-name BLIF: no map is keyed by its names (the V1.1.1 route with two BLIFs
    stays for that case)."""
    style = blif_names_style(blif) if blif is not None else None
    if style == "short":
        raise PostError(f"{blif} was written with short names ('names short'): no name map is "
                        f"keyed by them. Write it with --name-style hash (odin3-flow --prov); "
                        f"the v1.1.1 route with two BLIFs stays for short names")
    if share < MAP_MATCH_MIN:
        raise PostError(f"the name map holds only {matched} of the {total} atoms of the packed "
                        f"netlist ({share:.1%}; at least {MAP_MATCH_MIN:.0%} needed): is the BLIF "
                        f"written with short names, or the map from another run?")


def build(nm: NameMap, clusters: list[Cluster], places: dict[str, tuple[str, ...]],
          res: Resolver) -> tuple[list[NewRec], dict[str, list[NewRec]], int]:
    """The new records of every cluster, and per atom its (BLE, CLB, site) records. The count
    of atoms the map does not hold."""
    atom_recs: dict[str, list[NewRec]] = {}
    made: list[NewRec] = []
    unmatched = 0
    for cl in clusters:
        parents: list[int | NewRec] = []
        pads: set[int] = set()  # a pad record can recur: it is a parent once
        ble_of: dict[str, NewRec] = {}
        for part in cl.parts:
            if isinstance(part, str):
                number = res.record(part)
                unmatched += number == 0
                if number and number not in pads:
                    pads.add(number)
                    parents.append(number)
                continue
            atoms = [res.record(a) for a in part.atoms]
            unmatched += atoms.count(0)
            found: list[int | NewRec] = list(dict.fromkeys(a for a in atoms if a))
            if not found:  # no atom of the BLE is in the map: no record to chain (agent default)
                continue
            ble = NewRec("ble", "coarsen:lut", OP_BLE, PACK, found)
            parents.append(ble)
            made.append(ble)
            ble_of.update({a: ble for a in part.atoms})
        if not parents and any(isinstance(p, Ble) for p in cl.parts):
            continue  # logic with no atom in the map: no CLB, no site, no A lines (agent default)
            # a cluster of pads alone keeps its CLB and site even with no parent: an output pad
            # (`out:<port>`) has no map object by design, and the site is still a placement fact
        clb = NewRec("clb", "coarsen:ble", OP_CLB, PACK, parents)
        made.append(clb)
        site: NewRec | None = None
        if cl.name in places:
            site = NewRec("site", "rewrite:clb", OP_SITE, PLACE, [clb], places[cl.name])
            made.append(site)
        for atom in cl.atoms:
            atom_recs[atom] = [r for r in (ble_of.get(atom), clb, site) if r is not None]
    return made, atom_recs, unmatched


def number_records(atom_order: list[str], atom_recs: dict[str, list[NewRec]], base: int) -> int:
    """The numbering of 3.2: post-order from the A lines in line order; returns the last number."""
    counter = base

    def visit(rec: NewRec) -> None:
        nonlocal counter
        if rec.number:
            return
        for parent in rec.parents:
            if isinstance(parent, NewRec):
                visit(parent)
        counter += 1
        rec.number = counter

    for atom in atom_order:
        for rec in atom_recs[atom]:
            visit(rec)
    return counter


def record_line(rec: NewRec, steps: dict[tuple[str, int], str]) -> str:
    parents = ",".join(f"r{p.number if isinstance(p, NewRec) else p}" for p in rec.parents)
    label = rec.label
    flags = list(rec.flags)
    cons = ""
    if label is not None:
        words = [w for w, on in (("join", label.join), ("join=instance", label.instance)) if on]
        if label.n_joined > JOIN_CAP:
            words += ["lossy=cap", f"n={label.n_joined}"]
        flags = words + (["chain=span"] if label.span_only else []) + flags
        cons = ",".join(f"c{c}" for c in label.cons)
    return "\t".join(("R", f"r{rec.number}", "D", rec.level, rec.dir,
                      steps[(rec.run, rec.op)], parents, "", cons,
                      ",".join(escape(f) for f in flags)))


def extend_map(nm: NameMap, clusters: list[Cluster], places: dict[str, tuple[str, ...]],
               names: Resolver, net: Path, place: Path) -> tuple[list[str], list[str],
                                                                 list[str], PostResult]:
    """The new T, R and A lines (not yet in any file) and the result counts."""
    made, atom_recs, unmatched = build(nm, clusters, places, names)
    order = [a for cl in clusters for a in cl.atoms if a in atom_recs]
    base = len(nm.records)
    number_records(order, atom_recs, base)
    reached = sorted((r for r in made if r.number), key=lambda r: r.number)
    graph = Graph(nm)
    for rec in reached:  # appended in number order: parents come first
        parents = tuple(p.number if isinstance(p, NewRec) else p for p in rec.parents)
        nm.records.append(MapRecord(rec.number, "D", rec.level, rec.dir, "", parents, (), (), (),
                                    ""))
    for rec in reached:
        if rec.level != "site":
            rec.label = graph.label(rec.number)
    # T rows and the operation numbers: only referenced runs and operations are written
    runs = [run for run in (PACK, PLACE) if any(r.run == run for r in reached)]
    first = len(nm.runs)
    steps: dict[tuple[str, int], str] = {}
    for run in runs:
        ops = sorted({r.op for r in reached if r.run == run})
        for op in ops:
            steps[(run, op)] = f"t{first + runs.index(run) + 1}.{ops.index(op) + 1}"
    counts = {lvl: sum(1 for r in reached if r.level == lvl) for lvl in ("ble", "clb", "site")}
    tlines = []
    for run in runs:
        info = (f"clbs={counts['clb']} bles={counts['ble']} atoms={len(order)} net={net.name}"
                if run == PACK else f"sites={counts['site']} place={place.name}")
        tlines.append("\t".join(("T", f"t{first + runs.index(run) + 1}", run, "", escape(info))))
    rlines = [record_line(r, steps) for r in reached]
    alines = ["\t".join(("A", escape(a), ",".join(f"r{r.number}" for r in atom_recs[a]), "-"))
              for a in order]
    labels = [r.label for r in reached if r.label is not None]
    result = PostResult(Path(), len(order), unmatched, counts["ble"], counts["clb"],
                        counts["site"], sum(lb.join for lb in labels),
                        sum(lb.instance for lb in labels),
                        sum(lb.n_joined > JOIN_CAP for lb in labels), 0, 0)
    return tlines, rlines, alines, result


def merged_text(raw: bytes, tlines: list[str], rlines: list[str], alines: list[str]) -> bytes:
    """The map with the new lines inserted: T after the last T line, R after the last R line, A at
    the end; every old line stays as it was."""
    lines = raw.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    last: dict[bytes, int] = {}
    for i, line in enumerate(lines):
        if len(line) > 1 and line[1:2] == b"\t":
            last[line[:1]] = i
    inserts: list[tuple[int, list[str]]] = []
    for letter, new in ((b"T", tlines), (b"R", rlines)):
        if new:
            # a type with no line yet goes after the last line (the typical order is O N R C F T)
            inserts.append((last.get(letter, len(lines) - 1) + 1, new))
    for pos, new in sorted(inserts, reverse=True):
        lines[pos:pos] = [n.encode() for n in new]
    lines.extend(a.encode() for a in alines)
    return b"\n".join(lines) + b"\n"


def write_atomic(path: Path, data: bytes) -> None:
    """Temporary file in the map's directory (``tempfile.mkstemp``, so a concurrent run never
    shares it), then ``os.replace``: a reader sees the old map or the new one. A failed write
    leaves the old map; the tool removes its own temporary file and nothing else."""
    fd, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".", suffix=".tmp")
    temp = Path(name)
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        shutil.copymode(path, temp)  # mkstemp makes the file 0600
        os.replace(temp, path)
    except BaseException:
        temp.unlink(missing_ok=True)
        raise


def extend_flow_dir(flow_dir: Path | str, ns: argparse.Namespace | None = None) -> PostResult:
    """Extend the name map of ``flow_dir`` with the pack, place and atom lines. PostError when an
    input is missing or inconsistent, the BLIF has short names or the map is already extended."""
    return _extend(Path(flow_dir).resolve(), ns)


def _read_map(path: Path) -> tuple[bytes, NameMap]:
    try:
        raw = path.read_bytes()
        return raw, name_map_v2.parse_name_map(raw, path)
    except (OSError, ValueError) as exc:
        raise PostError(f"cannot read the name map {path}: {exc}") from exc


def _extend(flow: Path, ns: argparse.Namespace | None) -> PostResult:
    inp = find_inputs(flow, ns)
    for need in (inp.map, inp.net, inp.place):
        if not need.is_file():
            raise PostError(f"missing input {need}")
    raw, nm = _read_map(inp.map)
    check_extendable(nm, inp.map)
    if nm.records and nm.records[-1].number != len(nm.records) or any(
            r.number != i + 1 for i, r in enumerate(nm.records)):
        raise PostError(f"{inp.map}: the R lines are not numbered r1, r2, ... in line order "
                        f"(run `odin3 -p 'names --map FILE --check'`)")
    clusters = read_net(inp.net)
    places = read_place(inp.place)
    names = Resolver(nm, inp.top)
    # the share is over atoms a map should hold: not the constant generators, and not output pads
    # (`out:<port>`: a port bit has no object when its driver is a hash-named net)
    atoms = [a for cl in clusters for a in cl.atoms
             if a not in NO_SOURCE_ATOMS and not a.startswith("out:")]
    matched = sum(1 for a in atoms if names.lookup(a) is not None)
    check_names(inp.blif, matched / len(atoms) if atoms else 1.0, matched, len(atoms))
    tlines, rlines, alines, result = extend_map(nm, clusters, places, names, inp.net, inp.place)
    data = merged_text(raw, tlines, rlines, alines)
    try:
        write_atomic(inp.map, data)
    except OSError as exc:
        raise PostError(f"cannot write {inp.map}: {exc}") from exc
    result.map_path, result.bytes_before, result.bytes_after = inp.map, len(raw), len(data)
    result.lines = [*tlines, *rlines, *alines]
    return result


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="odin3-prov-post", description="Extend the Odin III name map of an odin3-flow output "
        "directory (made with --prov --vpr --keep-results) with VPR's packing and placement: "
        "BLE, CLB and site records, joins labelled by pack, and one A line per atom.",
        epilog="Exit codes: 0 ok, 1 refused or failed, 2 usage error.")
    ap.add_argument("flow_dir", type=Path, help="odin3-flow output directory")
    ap.add_argument("--map", type=Path, help="the name map (default: odin3/<top>.blif.names)")
    ap.add_argument("--blif", type=Path, help="the BLIF VPR read (default: odin3/*.blif)")
    ap.add_argument("--run-dir", type=Path, help="VPR run directory with the .net and .place")
    ap.add_argument("--net", type=Path, help="VPR packed netlist (default: <run-dir>/*.net)")
    ap.add_argument("--place", type=Path, help="VPR placement (default: <run-dir>/*.place)")
    ap.add_argument("--top", help="top module (default: top in timing/inputs.json)")
    ns = ap.parse_args(argv)
    if not ns.flow_dir.is_dir():
        raise UsageError(f"no such directory: {ns.flow_dir}")
    return ns


def main(argv: Sequence[str] | None = None) -> int:
    try:
        ns = parse_args(sys.argv[1:] if argv is None else argv)
    except UsageError as exc:
        print(f"odin3-prov-post: {exc}", file=sys.stderr)
        return 2
    except SystemExit as exc:
        return int(exc.code) if isinstance(exc.code, int) else 2
    try:
        res = extend_flow_dir(ns.flow_dir, ns)
    except PostError as exc:
        print(f"odin3-prov-post: {exc}", file=sys.stderr)
        return 1
    print(f"atoms {res.atoms} (not in the map: {res.unmatched}); bles {res.bles}, clbs {res.clbs}, "
          f"sites {res.sites}; joins by pack {res.joins}, of instances {res.instance_joins}, "
          f"lossy {res.lossy}")
    print(f"wrote {res.map_path} (+{res.bytes_after - res.bytes_before} bytes, "
          f"{len(res.lines)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
