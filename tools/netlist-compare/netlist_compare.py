"""netlist-compare: structural comparison of two BLIF netlists via a canonical form.

Usage::

    netlist-compare A.blif B.blif     # exit 0 identical, 1 different (unified diff), 2 error
    netlist-compare --canon A.blif    # print the canonical form of A
    netlist-compare --implicit-models A.blif B.blif   # undefined $-models are black boxes
    netlist-compare --vtr-normalize A.blif B.blif     # undo `write_blif --vtr`'s rewrites first

Canonical form (per model; the first model stays first, the rest are sorted by name):

* Primary I/O names (``.inputs``/``.outputs``/``.clock``) are kept verbatim and listed
  sorted, one per line.  Black-box models keep only their interface.
* Every internal net is renamed ``n<k>`` in an order derived from structure, not from
  names or line order.  Seed labels come from one topological pass (a net's label hashes
  its driver kind, function and input labels; latch outputs are seeded from latch
  type/init/control, which breaks every sequential cycle).  The labels are then refined
  in both directions -- by driver and by every use, including the net's exact role in
  each consuming cover -- to a fixpoint (Weisfeiler-Lehman colour refinement,
  Hopcroft-style so only the neighbourhood of a split is re-examined).  Nets still tied
  after that are separated by individualization-refinement: the smallest tied class
  (by size, then label) has one member individualized and refinement resumes.  The
  member is chosen by trying each candidate and keeping the smallest resulting partition
  (classes of up to MAX_TRIALS members), so the choice is structural.  Original names
  are consulted only to pick among candidates that give identical partitions -- for an
  automorphism orbit (e.g. duplicated logic) every pick gives the same canonical text.
  Residual risk: a tied class that is not an orbit and that the trials cannot separate
  (or that has more than MAX_TRIALS members) may still canonicalize name-dependently.
  That can only produce a false "different" (exit 1), never a false "identical": the
  canonical text is always a renaming of the input netlist (up to duplicate cover rows
  and spelling out the default latch init).
  Nets are finally ordered by (topological level, label).
* ``.names`` covers are sets: input columns are sorted by canonical net name, the cover
  columns permuted to match, rows sorted and de-duplicated.
* ``.subckt`` connections are sorted by formal name; cell blocks are sorted (duplicate
  cells are kept).
* ``.param`` is semantic and always kept; ``.cname``/``.attr`` are dropped unless
  ``--keep-attrs``.

``--implicit-models`` (2E §18.3, for Yosys ``write_blif -blackbox -icells -param``; without
``-blackbox`` a hard block's model is missing): a ``.subckt`` of a ``$``-prefixed model the file
does not define is a black box whose formals are those the file uses; a formal whose port (the
name before any ``[bit]``) is ``Y``, ``Q``, ``X``, ``CO`` or ``RD_DATA`` is an output, and so is
``DATA`` of ``$memrd``/``$memrd_v2`` and ``CTRL_OUT`` of ``$fsm`` (``$memwr``'s ``DATA`` is an
input, so those two are per model); every other formal is an input.  The stub models are listed
in the canonical form like any black box.  The ``A`` bits of an implicit ``$reduce_and``/
``_or``/``_xor``/``_xnor``/``_bool`` are a set: their order cannot change ``Y``, so ``A[k]`` is
compared as ``A`` (this is how ``proc-diff`` absorbs §20 #8's ``$reduce_or`` input order).
A model the file defines is used as written, with one exception: on a ``.blackbox`` model with a
scalar port ``p`` (no ``p[0]``), an instance pin ``p[0]`` with no other bit of ``p`` reads as
``p``.  Yosys 0.55's ``write_blif -blackbox`` writes such a pin for a parameterised black box whose
port is 1 bit at the defaults but whose connection is wider (only bit 0 is written, as for every
black-box pin), so the instance would otherwise name a port the model lacks.

``--vtr-normalize`` (3E §16.3, plan Task 10) applies the rewrites ``write_blif --vtr`` makes to
both files before canonicalising, so a Parmys or Odin II golden compares equal to its ``--vtr``
write: latch init ``3`` becomes ``2``; an input formal bit a ``.subckt`` omits (the model, a
``.blackbox`` stub or the full model, declares it) reads ``=unconn``; ``.clock`` lines become
``.inputs`` (VPR reads both alike, ``--vtr`` writes no ``.clock``); VB-10 names are decoded to a
fixed point (``%XX`` with upper-case hex is the byte, a leading ``%`` followed by anything else is
dropped); a constant cover becomes a zero-input generator and a non-constant 1-input cover one
row (§6); a net with readers and no driver reads `gnd` on a `.names` input, a latch D and a
primary output (a buffer from `gnd` drives it) and `unconn` on a `.subckt` input (§9.2: x or
undriven); the buffer `.names a y` / `1 1` the writer puts on a primary output `y` that a
`.subckt` output `a` drives (and nothing else reads) is dropped, `y` on the box (§9.2); a
`.blackbox` model no `.subckt` uses is dropped (§8.3: used stubs only).

Everything is iterative (explicit worklists, no recursion).
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import heapq
import itertools
import math
import re
import sys
from collections import Counter
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blif"))
import blif  # noqa: E402

TOOL = "netlist-compare"
# --implicit-models: the ports of an undefined $-model that are outputs (2E §18.3).
IMPLICIT_OUTPUTS = frozenset({"Y", "Q", "X", "CO", "RD_DATA"})
# ... and the outputs only some models have (another model's DATA is an input: $memwr).
IMPLICIT_MODEL_OUTPUTS: dict[str, frozenset[str]] = {
    "$memrd": frozenset({"DATA"}), "$memrd_v2": frozenset({"DATA"}),
    "$fsm": frozenset({"CTRL_OUT"}),
}
# --implicit-models: implicit models whose A bits are a set (a reduction's order is irrelevant).
SET_INPUT_MODELS = frozenset({"$reduce_and", "$reduce_or", "$reduce_xor", "$reduce_xnor",
                              "$reduce_bool"})
SET_INPUT_PORT = "A"
_BIT_SUFFIX = re.compile(r"\[\d+\]$")
# Upper bound on column permutations tried when .names inputs have identical labels.
MAX_TIE_PERMUTATIONS = 720
# Above this many canonical lines (both sides) the diff falls back to a linear method.
MAX_UNIFIED_DIFF_LINES = 40000
# Largest tied class whose members are each trial-individualized before choosing one.
MAX_TRIALS = 64


def _h(*parts: str) -> str:
    """Content hash used for labels (stable across processes, unlike hash())."""
    data = "\x1f".join(parts).encode("utf-8", "surrogateescape")
    return hashlib.blake2b(data, digest_size=12).hexdigest()


# --------------------------------------------------------------------------- labelling


@dataclass
class _Graph:
    """Per-model connectivity used for labelling."""

    model: blif.Model
    models: dict[str, blif.Model]
    primary: set[str]
    cell_ins: list[list[str]] = field(default_factory=list)
    cell_outs: list[list[str]] = field(default_factory=list)
    driver: dict[str, int] = field(default_factory=dict)
    fanout: dict[str, list[int]] = field(default_factory=dict)
    internal: list[str] = field(default_factory=list)


def _build_graph(model: blif.Model, models: dict[str, blif.Model]) -> _Graph:
    g = _Graph(model=model, models=models, primary=model.primary_nets())
    seen: set[str] = set()
    for idx, cell in enumerate(model.cells):
        ins, outs = blif.cell_io(cell, models)
        g.cell_ins.append(ins)
        g.cell_outs.append(outs)
        for net in outs:
            g.driver[net] = idx
        for net in dict.fromkeys(ins):
            g.fanout.setdefault(net, []).append(idx)
        for net in itertools.chain(ins, outs):
            if net not in g.primary and net not in seen:
                seen.add(net)
                g.internal.append(net)
    return g


def _is_comb(cell: blif.Cell) -> bool:
    return not isinstance(cell, blif.Latch)


def _topo_order(g: _Graph) -> tuple[list[int], list[int], dict[str, int]]:
    """Kahn's algorithm over combinational cells.

    Returns (ordered cells, cells on combinational loops, net level).  Primary nets,
    latch outputs and undriven nets are sources at level 0.
    """
    cells = g.model.cells

    def comb_driven(net: str) -> bool:
        idx = g.driver.get(net)
        return net not in g.primary and idx is not None and _is_comb(cells[idx])

    pending = [0] * len(cells)
    for idx, cell in enumerate(cells):
        if _is_comb(cell):
            pending[idx] = sum(1 for n in set(g.cell_ins[idx]) if comb_driven(n))
    level: dict[str, int] = {}
    work = [i for i, c in enumerate(cells) if _is_comb(c) and pending[i] == 0]
    order: list[int] = []
    while work:
        idx = work.pop()
        order.append(idx)
        lvl = 1 + max((level.get(n, 0) for n in g.cell_ins[idx]), default=-1)
        for net in g.cell_outs[idx]:
            if net in g.primary:
                continue  # a source at level 0 whatever order its driver is visited in
            level[net] = lvl
            for consumer in g.fanout.get(net, []):
                if _is_comb(cells[consumer]):
                    pending[consumer] -= 1
                    if pending[consumer] == 0:
                        work.append(consumer)
    done = set(order)
    cyclic = [i for i, c in enumerate(cells) if _is_comb(c) and i not in done]
    top = 1 + max(level.values(), default=0)
    for idx in cyclic:
        for net in g.cell_outs[idx]:
            level[net] = top
    return order, cyclic, level


def _column_order(columns: Sequence[str], rows: Sequence[tuple[str, str]]) -> list[int]:
    """Input-column order for a cover that does not depend on the written column order.

    Columns are sorted by ``columns`` (labels or canonical names).  Columns with equal keys
    are interchangeable, so among their permutations the one giving the lexicographically
    smallest sorted cover is chosen (bounded by MAX_TIE_PERMUTATIONS; beyond that the
    written order is kept).
    """
    order = sorted(range(len(columns)), key=lambda i: columns[i])
    groups = [list(grp) for _, grp in itertools.groupby(order, key=lambda i: columns[i])]
    n_perm = math.prod(math.factorial(len(grp)) for grp in groups)
    if n_perm == 1 or n_perm > MAX_TIE_PERMUTATIONS:
        return order
    candidates = itertools.product(*(itertools.permutations(grp) for grp in groups))
    perms = ([i for grp in combo for i in grp] for combo in candidates)
    return min(perms, key=lambda perm: _render_cover(perm, rows))


def _render_cover(perm: Sequence[int], rows: Sequence[tuple[str, str]]) -> list[str]:
    """Sorted, de-duplicated cover rows with input columns taken in ``perm`` order."""
    return sorted({("".join(plane[i] for i in perm) + " " + out).strip() for plane, out in rows})


def _cover_key(columns: Sequence[str], rows: Sequence[tuple[str, str]]) -> str:
    """Cover rendering independent of input-column order (see _column_order)."""
    return ";".join(_render_cover(_column_order(columns, rows), rows))


def _params(cell: blif.Cell) -> str:
    return ";".join(sorted(f"{k} {v}" for k, v in cell.attrs if k == ".param"))


def _comb_labels(cell: blif.Cell, g: _Graph, labels: dict[str, str]) -> None:
    """Assign labels to the outputs of a combinational cell (inputs already labelled)."""
    if isinstance(cell, blif.Names):
        cols = [labels[n] for n in cell.inputs]
        key = _h("names", ",".join(sorted(cols)), _cover_key(cols, cell.rows), _params(cell))
        if cell.output not in g.primary:
            labels[cell.output] = key
        return
    assert isinstance(cell, blif.Subckt)
    outs = set(g.models[cell.model].outputs)
    ins = sorted(f"{f}={labels[a]}" for f, a in cell.conns if f not in outs)
    base = _h("subckt", cell.model, ";".join(ins), _params(cell))
    for formal, actual in cell.conns:
        if formal in outs and actual not in g.primary:
            labels[actual] = _h(base, "out", formal)


def _cyclic_label(cell: blif.Cell) -> str:
    if isinstance(cell, blif.Names):
        return _h("cyclic-names", str(len(cell.inputs)), str(len(cell.rows)))
    assert isinstance(cell, blif.Subckt)
    return _h("cyclic-subckt", cell.model)


def _latch_seed(latch: blif.Latch, primary: set[str]) -> str:
    ctrl = blif.latch_control(latch)
    ctrl_key = "-" if ctrl is None else (ctrl if ctrl in primary else "internal")
    return _h("latch", latch.ltype or "-", latch.init or "3", ctrl_key)


def _forward_labels(g: _Graph) -> tuple[dict[str, str], dict[str, int]]:
    """Seed labels and topological levels.

    One topological pass labels combinational logic from its inputs; latch outputs are
    seeded from latch type/init/control, so every sequential cycle is broken.
    """
    order, cyclic, level = _topo_order(g)
    labels: dict[str, str] = {n: _h("io", n) for n in g.primary}
    for net in g.internal:
        labels[net] = _h("undriven")
    for cell in g.model.cells:
        if isinstance(cell, blif.Latch) and cell.output not in g.primary:
            labels[cell.output] = _latch_seed(cell, g.primary)
    for idx in cyclic:
        for net in g.cell_outs[idx]:
            if net not in g.primary:
                labels[net] = _cyclic_label(g.model.cells[idx])
    for idx in order:
        _comb_labels(g.model.cells[idx], g, labels)
    return labels, level


class _Refiner:
    """Colour refinement over internal nets in both directions, then individualization.

    A net's signature hashes its driver (cell kind, function and the labels of all the
    driver's pins) and every use (the consumer's function with this net's columns marked,
    or its subckt formals / latch pins, and the labels of the consumer's other pins).
    Classes of equal labels are split until stable (Weisfeiler-Lehman / 1-dim colour
    refinement).  Splitting is Hopcroft-style: the largest part of a split class keeps its
    label, only nets adjacent to relabelled nets are re-examined, and the untouched rest of
    a class is represented by one member, so total work is O(pins * log nets) rather than
    proportional to rounds * class size.  If ties remain, the smallest tied class (by size,
    then label) has one member individualized (see _choose) and refinement resumes, until
    every internal net has a unique label.  All labels are content hashes, never names.
    """

    def __init__(self, g: _Graph, labels: dict[str, str]) -> None:
        self.g = g
        self.labels = labels
        self.classes: dict[str, set[str]] = {}
        for net in g.internal:
            self.classes.setdefault(labels[net], set()).add(net)
        self.heap: list[tuple[int, str]] = []
        # While a trial individualization runs, every mutation is journalled for rollback.
        self.journal: list[tuple[str, str, str]] | None = None
        for lab in self.classes:
            self._track(lab)

    # -- bookkeeping ------------------------------------------------------------

    def _log(self, op: str, key: str, value: str) -> None:
        if self.journal is not None:
            self.journal.append((op, key, value))

    def _track(self, lab: str) -> None:
        size = len(self.classes.get(lab, ()))
        if size > 1 and self.journal is None:
            heapq.heappush(self.heap, (size, lab))

    def _relabel(self, net: str, new: str) -> None:
        old = self.labels[net]
        cls = self.classes[old]
        cls.discard(net)
        self._log("discard", old, net)
        if not cls:
            del self.classes[old]
            self._log("delete", old, "")
        self.labels[net] = new
        self._log("label", net, old)
        if new not in self.classes:
            self.classes[new] = set()
            self._log("create", new, "")
        self.classes[new].add(net)
        self._log("add", new, net)

    def _rollback(self, journal: list[tuple[str, str, str]]) -> None:
        for op, key, value in reversed(journal):
            if op == "label":
                self.labels[key] = value
            elif op == "add":
                self.classes[key].discard(value)
            elif op == "create":
                del self.classes[key]
            elif op == "discard":
                self.classes[key].add(value)
            else:  # delete
                self.classes[key] = set()

    # -- signatures ---------------------------------------------------------------

    def _driver_part(self, net: str) -> str:
        idx = self.g.driver.get(net)
        if idx is None:
            return "undriven"
        cell, lab = self.g.model.cells[idx], self.labels
        if isinstance(cell, blif.Names):
            cols = [lab[n] for n in cell.inputs]
            return _h("names", ",".join(sorted(cols)), _cover_key(cols, cell.rows),
                      _params(cell))
        if isinstance(cell, blif.Latch):
            ctrl = blif.latch_control(cell)
            return _h("latch", cell.ltype or "-", cell.init or "3", lab[cell.input],
                      "-" if ctrl is None else lab[ctrl])
        formal = ",".join(sorted(f for f, a in cell.conns if a == net))
        pins = ";".join(sorted(f"{f}={lab[a]}" for f, a in cell.conns))
        return _h("subckt", cell.model, formal, pins, _params(cell))

    def _use_part(self, idx: int, net: str) -> str:
        cell, lab = self.g.model.cells[idx], self.labels
        if isinstance(cell, blif.Names):
            # The cover with this net's columns marked: exactly its role in the function.
            marked = [lab[n] + ("*" if n == net else "") for n in cell.inputs]
            return _h("names-in", ",".join(sorted(marked)), _cover_key(marked, cell.rows),
                      lab[cell.output])
        if isinstance(cell, blif.Latch):
            pins = ("D" if cell.input == net else "") + ("C" if cell.control == net else "")
            return _h("latch-in", pins, lab[cell.output])
        formal = ",".join(sorted(f for f, a in cell.conns if a == net))
        pins = ";".join(sorted(f"{f}={lab[a]}" for f, a in cell.conns))
        return _h("subckt-in", cell.model, formal, pins)

    def signature(self, net: str) -> str:
        uses = sorted(self._use_part(i, net) for i in self.g.fanout.get(net, []))
        return _h(self._driver_part(net), *uses)

    def _neighbours(self, net: str) -> Iterator[str]:
        idx = self.g.driver.get(net)
        cells = self.g.fanout.get(net, []) if idx is None else [idx, *self.g.fanout.get(net, [])]
        for c in cells:
            for nb in itertools.chain(self.g.cell_ins[c], self.g.cell_outs[c]):
                if nb != net:
                    yield nb

    def _tied_near(self, nets: Iterable[str]) -> dict[str, set[str]]:
        """Tied classes with a member adjacent to ``nets`` -> those adjacent members."""
        found: dict[str, set[str]] = {}
        for net in nets:
            for nb in self._neighbours(net):
                lab = self.labels[nb]
                if len(self.classes.get(lab, ())) > 1:
                    found.setdefault(lab, set()).add(nb)
        return found

    # -- refinement -----------------------------------------------------------------

    def _groups(self, lab: str, nets: set[str]) -> dict[str, set[str] | None]:
        """Partition of class ``lab`` by signature.  ``nets`` are the members whose
        neighbourhood changed; all other members share one signature (computed from a
        representative) and appear as the value None (meaning "the untouched rest")."""
        groups: dict[str, set[str] | None] = {}
        members = self.classes[lab]
        if len(nets) < len(members):
            rep = next(n for n in members if n not in nets)  # any one: all share a signature
            groups[self.signature(rep)] = None
        for net in nets:
            sig = self.signature(net)
            if sig in groups and groups[sig] is None:
                continue  # same as the untouched rest
            group = groups.setdefault(sig, set())
            assert group is not None
            group.add(net)
        return groups

    def _apply(self, lab: str, groups: dict[str, set[str] | None]) -> list[str]:
        """Split class ``lab``; the largest part keeps the label.  Returns relabelled nets."""
        members = self.classes[lab]
        moved: set[str] = set().union(*(g for g in groups.values() if g is not None))
        sizes = {sig: (len(members) - len(moved) if g is None else len(g))
                 for sig, g in groups.items()}
        keeper = max(groups, key=lambda sig: (sizes[sig], sig))
        rest = members - moved if keeper in groups and groups[keeper] is not None else set()
        relabelled: list[str] = []
        for sig, group in groups.items():
            if sig == keeper:
                continue
            nets = rest if group is None else group
            for net in sorted(nets):
                self._relabel(net, _h(lab, sig))
                relabelled.append(net)
            self._track(_h(lab, sig))
        self._track(lab)
        return relabelled

    def refine(self, adjacent: dict[str, set[str]]) -> None:
        """Split classes to a fixpoint.  ``adjacent`` maps each class to the members whose
        neighbourhood changed; all signatures of a round are computed before any label
        changes, so the result does not depend on iteration order."""
        while adjacent:
            plan = {lab: self._groups(lab, nets) for lab, nets in adjacent.items()}
            changed: list[str] = []
            for lab, groups in plan.items():
                if len(groups) > 1:
                    changed += self._apply(lab, groups)
            adjacent = self._tied_near(changed)

    # -- individualization ------------------------------------------------------------

    def _individualize(self, lab: str, member: str) -> None:
        # The class size makes the label unique when the same class is individualized again.
        self._relabel(member, _h(lab, "individualized", str(len(self.classes[lab]))))
        self._track(lab)
        self.refine(self._tied_near([member]))

    def _trial(self, lab: str, member: str) -> list[tuple[str, int]]:
        """Individualize ``member``, refine, and return a certificate of the resulting
        partition (the classes it created, with sizes); then undo everything."""
        self.journal = []
        self._individualize(lab, member)
        journal, self.journal = self.journal, None
        cert = sorted((k, len(self.classes.get(k, ()))) for op, k, _ in journal
                      if op == "create")
        self._rollback(journal)
        return cert

    def _choose(self, lab: str) -> str:
        """Member of tied class ``lab`` to individualize.

        Each candidate is tried (individualize, refine, roll back) and the one giving the
        smallest partition certificate wins, so the choice is structural even when the class
        is not an automorphism orbit (a case colour refinement cannot detect, e.g. a latch
        self-loop next to a 2-cycle of identical latches).  Candidates with equal
        certificates and classes too large to try (MAX_TRIALS) fall back to the original
        name; for an automorphism orbit any choice gives the same canonical form.
        """
        members = self.classes[lab]
        if len(members) > MAX_TRIALS:
            return min(members, key=_natural_key)
        return min(members, key=lambda m: (self._trial(lab, m), _natural_key(m)))

    def _individualize_all(self, lab: str) -> None:
        """Individualize a whole class whose members have no tied neighbours.

        Such members are interchangeable and individualizing one cannot affect the others,
        so this gives exactly the labels of individualizing them one by one in name order.
        """
        order = sorted(self.classes[lab], key=_natural_key)
        for j, member in enumerate(order[:-1]):
            self._relabel(member, _h(lab, "individualized", str(len(order) - j)))

    def run(self) -> None:
        self.refine({lab: set(m) for lab, m in self.classes.items() if len(m) > 1})
        while self.heap:
            size, lab = heapq.heappop(self.heap)
            if len(self.classes.get(lab, ())) != size:
                continue  # stale heap entry
            if not self._tied_near(self.classes[lab]):
                self._individualize_all(lab)
            else:
                self._individualize(lab, self._choose(lab))


def _natural_key(name: str) -> tuple[tuple[int, str], ...]:
    """Sort key that orders ``n9`` before ``n10``."""
    parts = re.split(r"(\d+)", name)
    return tuple((1, p) if i % 2 == 0 else (0, p.zfill(20)) for i, p in enumerate(parts))


def _pick_prefix(primary: Iterable[str]) -> str:
    """Prefix for canonical names that cannot collide with a primary I/O name."""
    names = set(primary)
    prefix = "n"
    while any(re.fullmatch(re.escape(prefix) + r"\d+", n) for n in names):
        prefix += "_"
    return prefix


def canonical_names(model: blif.Model, models: dict[str, blif.Model]) -> dict[str, str]:
    """Map every net of ``model`` to its canonical name (primary nets map to themselves)."""
    g = _build_graph(model, models)
    labels, level = _forward_labels(g)
    _Refiner(g, labels).run()
    ranked = sorted(g.internal, key=lambda n: (level.get(n, 0), labels[n]))
    prefix = _pick_prefix(g.primary)
    rename = {n: n for n in g.primary}
    rename.update({net: f"{prefix}{k}" for k, net in enumerate(ranked)})
    return rename


# --------------------------------------------------------------------------- rendering


def _attr_lines(cell: blif.Cell, keep_attrs: bool) -> list[str]:
    kept = [(k, v) for k, v in cell.attrs if keep_attrs or k == ".param"]
    return sorted(f"{k} {v}".rstrip() for k, v in kept)


def _render_names(cell: blif.Names, rn: dict[str, str]) -> list[str]:
    cols = _column_order([rn[n] for n in cell.inputs], cell.rows)
    header = " ".join([".names", *(rn[cell.inputs[i]] for i in cols), rn[cell.output]])
    return [header, *_render_cover(cols, cell.rows)]


def _render_cell(cell: blif.Cell, rn: dict[str, str], keep_attrs: bool) -> str:
    if isinstance(cell, blif.Names):
        lines = _render_names(cell, rn)
    elif isinstance(cell, blif.Latch):
        parts = [".latch", rn[cell.input], rn[cell.output]]
        if cell.ltype is not None and cell.control is not None:
            parts += [cell.ltype, rn.get(cell.control, cell.control)]
        parts.append(cell.init or "3")
        lines = [" ".join(parts)]
    else:
        conns = sorted(f"{f}={rn[a]}" for f, a in cell.conns)
        lines = [" ".join([".subckt", cell.model, *conns])]
    return "\n".join(lines + _attr_lines(cell, keep_attrs))


def canonicalize_model(
    model: blif.Model, models: dict[str, blif.Model], keep_attrs: bool
) -> list[str]:
    """Canonical text lines for one model."""
    lines = [f".model {model.name}"]
    for directive, ports in ((".inputs", model.inputs), (".outputs", model.outputs),
                             (".clock", model.clocks)):
        lines += [f"{directive} {p}" for p in sorted(set(ports))]
    if model.blackbox:
        return [*lines, ".blackbox", ".end"]
    rn = canonical_names(model, models)
    # A list, not a set: duplicate cells (e.g. two identical output-less subckts) count.
    blocks = sorted(_render_cell(c, rn, keep_attrs) for c in model.cells)
    for block in blocks:
        lines += block.split("\n")
    return [*lines, ".end"]


def canonicalize(netlist: blif.Netlist, keep_attrs: bool = False) -> str:
    """Canonical BLIF text for a whole netlist (models separated by a blank line)."""
    models = netlist.by_name()
    ordered = [netlist.top, *sorted(netlist.models[1:], key=lambda m: m.name)]
    chunks = ["\n".join(canonicalize_model(m, models, keep_attrs)) for m in ordered]
    return "\n\n".join(chunks) + "\n"


# --------------------------------------------------------------------------- implicit models


def _port_of(formal: str) -> str:
    """The port a formal names: ``A[3]`` -> ``A``."""
    return _BIT_SUFFIX.sub("", formal)


def implicit_stubs(text: str) -> tuple[str, frozenset[str]]:
    """BLIF text of a black-box stub for every undefined ``$``-model the text instantiates, and
    the names of those models.  A stub's formals are those used, in first-use order."""
    defined: set[str] = set()
    used: dict[str, dict[str, None]] = {}
    for _, tokens in blif.logical_lines(text):
        if tokens[0] == ".model" and len(tokens) > 1:
            defined.add(tokens[1])
        elif tokens[0] == ".subckt" and len(tokens) > 1 and tokens[1].startswith("$"):
            formals = used.setdefault(tokens[1], {})
            for tok in tokens[2:]:
                formal, eq, _ = tok.partition("=")
                if eq and formal:
                    formals[formal] = None
    lines: list[str] = []
    names = sorted(name for name in used if name not in defined)
    for name in names:
        out_ports = IMPLICIT_OUTPUTS | IMPLICIT_MODEL_OUTPUTS.get(name, frozenset())
        outs = [f for f in used[name] if _port_of(f) in out_ports]
        ins = [f for f in used[name] if _port_of(f) not in out_ports]
        lines.append(f".model {name}")
        lines += [" ".join([d, *ports]) for d, ports in ((".inputs", ins), (".outputs", outs))
                  if ports]
        lines += [".blackbox", ".end"]
    return "".join(f"{line}\n" for line in lines), frozenset(names)


def _blackbox_ports(text: str) -> dict[str, set[str]]:
    """The ports of every ``.blackbox`` model the text defines."""
    ports: dict[str, set[str]] = {}
    found: dict[str, set[str]] = {}
    name: str | None = None
    for _, tokens in blif.logical_lines(text):
        if tokens[0] == ".model" and len(tokens) > 1:
            name = tokens[1]
            ports[name] = set()
        elif name is not None and tokens[0] in (".inputs", ".outputs", ".clock"):
            ports[name].update(tokens[1:])
        elif name is not None and tokens[0] == ".blackbox":
            found[name] = ports[name]
    return found


def _scalar_bit0(tokens: list[str], ports: set[str]) -> list[str]:
    """A ``.subckt``'s tokens with ``p[0]=x`` as ``p=x`` where p is a scalar port and the
    instance names no other bit of p."""
    formals = [tok.partition("=")[0] for tok in tokens[2:]]
    out = tokens[:2]
    for tok in tokens[2:]:
        formal, eq, actual = tok.partition("=")
        port = _port_of(formal)
        if (eq and formal == f"{port}[0]" and port in ports
                and sum(_port_of(f) == port for f in formals) == 1):
            tok = f"{port}={actual}"
        out.append(tok)
    return out


def scalar_blackbox_pins(text: str) -> str:
    """Rewrites each one-line ``.subckt`` of a ``.blackbox`` model per ``_scalar_bit0``."""
    boxes = _blackbox_ports(text)
    if not boxes:
        return text
    lines = text.split("\n")
    for i, line in enumerate(lines):
        tokens = line.split("#", 1)[0].split()
        if (len(tokens) > 2 and tokens[0] == ".subckt" and tokens[1] in boxes
                and not line.rstrip().endswith("\\")):
            lines[i] = " ".join(_scalar_bit0(tokens, boxes[tokens[1]]))
    return "\n".join(lines)


def _set_inputs(netlist: blif.Netlist, implicit: frozenset[str]) -> None:
    """Compare the A bits of each implicit reduction as a set: ``A[k]`` becomes ``A``."""
    for model in netlist.models:
        for cell in model.cells:
            if (isinstance(cell, blif.Subckt) and cell.model in SET_INPUT_MODELS
                    and cell.model in implicit):
                cell.conns = [(SET_INPUT_PORT if _port_of(f) == SET_INPUT_PORT else f, a)
                              for f, a in cell.conns]


def load(path: str, implicit_models: bool) -> blif.Netlist:
    """Parse a BLIF file; with implicit_models, undefined $-models become black-box stubs."""
    if not implicit_models:
        return blif.parse_file(path)
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise blif.BlifError(f"{path}: cannot read: {exc}") from exc
    if text and not text.endswith("\n"):
        text += "\n"
    text = scalar_blackbox_pins(text)
    stubs, implicit = implicit_stubs(text)
    netlist = blif.parse_text(text + stubs, path)
    _set_inputs(netlist, implicit)
    return netlist


# --------------------------------------------------------------------------- --vtr-normalize

# VB-10 (plan 3E point (o)): `%` and two upper-case hex digits is a byte.
_VB10_ESCAPE = re.compile(r"%([0-9A-F]{2})")
UNCONN = "unconn"
# Covers with more inputs are not evaluated (the writer's cap on truth tables, K <= 16).
MAX_EVAL_INPUTS = 16
LATCH_INIT_UNDEFINED = "3"
LATCH_INIT_UNKNOWN = "2"


def vb10_decode(name: str) -> str:
    """name with VB-10 undone to a fixed point: a leading `%` that is not an escape dropped, then
    every `%XX` (upper-case hex) replaced by its byte, until nothing changes. One step inverts
    one write exactly; the fixed point makes a name written twice (`%255C`) decode like one
    written once (`%5C`), both to `\\`. Bytes outside UTF-8 stay as surrogate escapes."""
    while True:
        step = name[1:] if name.startswith("%") and _VB10_ESCAPE.match(name) is None else name
        raw = step.encode("utf-8", errors="surrogateescape")
        raw = re.sub(rb"%([0-9A-F]{2})", lambda m: bytes([int(m[1], 16)]), raw)
        step = raw.decode("utf-8", errors="surrogateescape")
        if step == name:
            return name
        name = step


def _var_masks(k: int, cache: dict[int, list[int]]) -> list[int]:
    """masks[i] has bit m set iff bit i of the minterm m is 1, for the 2**k minterms."""
    if k not in cache:
        width = 1 << k
        cache[k] = [
            sum(1 << m for m in range(width) if (m >> i) & 1) for i in range(k)
        ]
    return cache[k]


def _cover_function(cell: blif.Names, cache: dict[int, list[int]]) -> int:
    """The truth table of a cover as a bit set over its 2**k minterms (input j is bit j)."""
    k = len(cell.inputs)
    full = (1 << (1 << k)) - 1
    masks = _var_masks(k, cache)
    hit = 0
    for plane, _ in cell.rows:
        term = full
        for i, ch in enumerate(plane):
            if ch == "1":
                term &= masks[i]
            elif ch == "0":
                term &= ~masks[i]
        hit |= term
    on_set = bool(cell.rows) and cell.rows[0][1] == "1"
    if cell.rows and not on_set:  # an off-set cover is 1 everywhere it has no row
        return full & ~hit
    return hit


def _normalize_names(cell: blif.Names, cache: dict[int, list[int]]) -> None:
    """§6: a constant cover becomes a zero-input generator, a non-constant 1-input cover one row."""
    k = len(cell.inputs)
    if k > MAX_EVAL_INPUTS:
        return
    table = _cover_function(cell, cache)
    full = (1 << (1 << k)) - 1
    if table in (0, full):
        cell.inputs = []
        cell.rows = [("", "1")] if table == full else []
    elif k == 1:
        cell.rows = [("1", "1")] if table == 0b10 else [("0", "1")]


def _normalize_latch(latch: blif.Latch) -> None:
    if latch.init == LATCH_INIT_UNDEFINED:
        latch.init = LATCH_INIT_UNKNOWN


def _decode_cell(cell: blif.Cell) -> None:
    if isinstance(cell, blif.Names):
        cell.inputs = [vb10_decode(n) for n in cell.inputs]
        cell.output = vb10_decode(cell.output)
    elif isinstance(cell, blif.Latch):
        cell.input, cell.output = vb10_decode(cell.input), vb10_decode(cell.output)
        if cell.control is not None:
            cell.control = vb10_decode(cell.control)
    else:
        cell.model = vb10_decode(cell.model)
        cell.conns = [(f, vb10_decode(a)) for f, a in cell.conns]


def _fill_omitted(cell: blif.Subckt, models: dict[str, blif.Model]) -> bool:
    """Adds `formal=unconn` for each input formal of the model that the instance omits; true when
    it added any."""
    sub = models.get(cell.model)
    if sub is None:
        return False
    given = {f for f, _ in cell.conns}
    missing = [f for f in sub.inputs if f not in given]
    cell.conns += [(f, UNCONN) for f in missing]
    return bool(missing)


def vtr_normalize(model: blif.Model, models: dict[str, blif.Model] | None = None) -> blif.Model:
    """model with the rewrites of `write_blif --vtr` undone (see the module docstring), in place;
    returns it. `models` (by decoded name; default just this model) gives the stubs that say which
    input formals a `.subckt` omits."""
    cache: dict[int, list[int]] = {}
    model.name = vb10_decode(model.name)
    model.inputs = [vb10_decode(n) for n in model.inputs]
    model.outputs = [vb10_decode(n) for n in model.outputs]
    for clock in (vb10_decode(n) for n in model.clocks):
        if clock not in model.inputs:
            model.inputs.append(clock)
    model.clocks = []
    filled = False
    for cell in model.cells:
        _decode_cell(cell)
        if isinstance(cell, blif.Names):
            _normalize_names(cell, cache)
        elif isinstance(cell, blif.Latch):
            _normalize_latch(cell)
        elif models is not None:
            filled = _fill_omitted(cell, models) or filled
    if filled:
        _ensure_generator(model, UNCONN, models)
    _tie_undriven(model, models or {})
    _elide_po_buffers(model, models or {})
    _mark_open_generator(model)
    return model


def _elide_po_buffers(model: blif.Model, models: dict[str, blif.Model]) -> None:
    """Undoes `write_blif --vtr`'s buffer on a primary output a `.subckt` output pin drives (3E
    §9.2): `.subckt ... out=a` and `.names a y` / `1 1` with `y` an output, `a` read by nothing
    else, become `.subckt ... out=y`. VTR's ABC drops an output a hard block drives directly, so
    the writer buffers it; a Parmys or Odin II golden has the output on the box."""
    reads: dict[str, int] = {}
    for cell in model.cells:
        for net in blif.cell_io(cell, models)[0]:
            reads[net] = reads.get(net, 0) + 1
    boxes = {a: cell for cell in model.cells if isinstance(cell, blif.Subckt)
             for a in blif.cell_io(cell, models)[1]}
    outputs = set(model.outputs)
    gone: list[blif.Cell] = []
    for cell in model.cells:
        if not (isinstance(cell, blif.Names) and len(cell.inputs) == 1
                and cell.rows == [("1", "1")] and cell.output in outputs):
            continue
        net = cell.inputs[0]
        box = boxes.get(net)
        if box is None or net in outputs or reads.get(net, 0) != 1 or cell.output in boxes:
            continue
        box.conns = [(f, cell.output if a == net else a) for f, a in box.conns]
        gone.append(cell)
    model.cells = [c for c in model.cells if not any(c is g for g in gone)]


OPEN_MARK = (".param", "open")


def _mark_open_generator(model: blif.Model) -> None:
    """Marks the constant-0 generator of the net `unconn` (an open pin) so that it hashes and
    renders differently from `gnd` (a tied-low net): VPR treats the two differently on a hard
    block's input, and a bare `.names unconn` equals `.names gnd` otherwise."""
    for cell in model.cells:
        if (isinstance(cell, blif.Names) and cell.output == UNCONN and not cell.inputs
                and not cell.rows and OPEN_MARK not in cell.attrs):
            cell.attrs.append(OPEN_MARK)


GND = "gnd"


def _driven_nets(model: blif.Model, models: dict[str, blif.Model]) -> set[str]:
    driven = set(model.inputs) | set(model.clocks)
    for cell in model.cells:
        driven.update(blif.cell_io(cell, models)[1])
    return driven


def _ensure_generator(
    model: blif.Model, name: str, models: dict[str, blif.Model] | None
) -> None:
    """Adds `.names <name>` (a constant 0) when nothing drives the net."""
    if name not in _driven_nets(model, models or {}):
        model.cells.append(blif.Names(inputs=[], output=name, rows=[]))


def _tie_undriven(model: blif.Model, models: dict[str, blif.Model]) -> None:
    """§9.2: a net that has readers and no driver is x, which `--vtr` writes as `gnd` on logic and
    primary outputs and as `unconn` on a hard block's input. Done on the model as read, before the
    generators it names are added."""
    if model.blackbox:
        return
    driven = _driven_nets(model, models)
    for cell in model.cells:
        if isinstance(cell, blif.Names):
            cell.inputs = [GND if n not in driven else n for n in cell.inputs]
        elif isinstance(cell, blif.Latch):
            cell.input = GND if cell.input not in driven else cell.input
        else:
            sub = models.get(cell.model)
            ins = set(sub.inputs) if sub is not None else set()
            cell.conns = [(f, UNCONN if f in ins and a not in driven else a)
                          for f, a in cell.conns]
    for out in model.outputs:
        if out not in driven:
            model.cells.append(blif.Names(inputs=[GND], output=out, rows=[("1", "1")]))
    reads = {n for cell in model.cells for n in blif.cell_io(cell, models)[0]}
    for name in (GND, UNCONN):
        if name in reads:
            _ensure_generator(model, name, models)


def vtr_normalize_netlist(netlist: blif.Netlist) -> blif.Netlist:
    """Every model of the netlist through vtr_normalize (the stubs decode first, so a `.subckt`
    finds its model under the decoded name)."""
    for model in netlist.models:
        model.name = vb10_decode(model.name)
        model.inputs = [vb10_decode(n) for n in model.inputs]
    models = {m.name: m for m in netlist.models}
    for model in netlist.models:
        vtr_normalize(model, models)
    used = {c.model for m in netlist.models for c in m.cells if isinstance(c, blif.Subckt)}
    netlist.models = [m for i, m in enumerate(netlist.models)
                      if i == 0 or not m.blackbox or m.name in used]
    return netlist


# --------------------------------------------------------------------------- CLI


def diff_lines(a: str, b: str, name_a: str, name_b: str) -> list[str]:
    """Unified diff of two canonical forms.

    difflib is quadratic in the worst case, so above MAX_UNIFIED_DIFF_LINES the diff
    degrades to a linear multiset difference: every line only in A (``-``) or only in B
    (``+``), in file order, under one ``@@`` header.
    """
    la, lb = a.splitlines(), b.splitlines()
    if len(la) + len(lb) <= MAX_UNIFIED_DIFF_LINES:
        return list(difflib.unified_diff(la, lb, fromfile=name_a, tofile=name_b, lineterm=""))
    only_a, only_b = Counter(la) - Counter(lb), Counter(lb) - Counter(la)
    out = [f"--- {name_a}", f"+++ {name_b}",
           "@@ large inputs: unordered line difference, not a unified diff @@"]
    for sign, lines, extra in (("-", la, only_a), ("+", lb, only_b)):
        for line in lines:
            if extra[line] > 0:
                extra[line] -= 1
                out.append(sign + line)
    return out



def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog=TOOL,
        description="Compare two BLIF netlists structurally after canonicalization. "
        "Exit 0 identical, 1 different, 2 usage/input error.",
    )
    ap.add_argument("files", nargs="+", metavar="FILE.blif")
    ap.add_argument("--canon", action="store_true", help="print the canonical form of one file")
    ap.add_argument("--keep-attrs", action="store_true", help="keep .cname/.attr lines")
    ap.add_argument("--quiet", action="store_true", help="no diff output, exit code only")
    ap.add_argument("--implicit-models", action="store_true",
                    help="an undefined $-model is a black box whose formals are those used")
    ap.add_argument("--vtr-normalize", action="store_true",
                    help="undo write_blif --vtr's rewrites on both files before comparing")
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    ap = _parser()
    args = ap.parse_args(argv)
    files: list[str] = args.files
    if len(files) != (1 if args.canon else 2):
        ap.error("--canon takes one file" if args.canon else "expected two files")
    try:
        nets = [load(f, args.implicit_models) for f in files]
        if args.vtr_normalize:
            nets = [vtr_normalize_netlist(net) for net in nets]
        canon = [canonicalize(net, args.keep_attrs) for net in nets]
    except blif.BlifError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return 2
    if args.canon:
        sys.stdout.write(canon[0])
        return 0
    if canon[0] == canon[1]:
        return 0
    if not args.quiet:
        if args.vtr_normalize and hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(errors="surrogateescape")  # a decoded 0xFE name may differ
        for line in diff_lines(canon[0], canon[1], files[0], files[1]):
            print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
