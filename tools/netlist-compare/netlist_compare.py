"""netlist-compare: structural comparison of two BLIF netlists via a canonical form.

Usage::

    netlist-compare A.blif B.blif     # exit 0 identical, 1 different (unified diff), 2 error
    netlist-compare --canon A.blif    # print the canonical form of A

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
# Upper bound on column permutations tried when .names inputs have identical labels.
MAX_TIE_PERMUTATIONS = 720
# Above this many canonical lines (both sides) the diff falls back to a linear method.
MAX_UNIFIED_DIFF_LINES = 40000
# Largest tied class whose members are each trial-individualized before choosing one.
MAX_TRIALS = 64


def _h(*parts: str) -> str:
    """Content hash used for labels (stable across processes, unlike hash())."""
    return hashlib.blake2b("\x1f".join(parts).encode(), digest_size=12).hexdigest()


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
            level[net] = lvl
            if net in g.primary:
                continue
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
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    ap = _parser()
    args = ap.parse_args(argv)
    files: list[str] = args.files
    if len(files) != (1 if args.canon else 2):
        ap.error("--canon takes one file" if args.canon else "expected two files")
    try:
        canon = [canonicalize(blif.parse_file(f), args.keep_attrs) for f in files]
    except blif.BlifError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return 2
    if args.canon:
        sys.stdout.write(canon[0])
        return 0
    if canon[0] == canon[1]:
        return 0
    if not args.quiet:
        for line in diff_lines(canon[0], canon[1], files[0], files[1]):
            print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
