"""netlist-compare: structural comparison of two BLIF netlists via a canonical form.

Usage::

    netlist-compare A.blif B.blif     # exit 0 identical, 1 different (unified diff), 2 error
    netlist-compare --canon A.blif    # print the canonical form of A

Canonical form (per model; the first model stays first, the rest are sorted by name):

* Primary I/O names (``.inputs``/``.outputs``/``.clock``) are kept verbatim and listed
  sorted, one per line.  Black-box models keep only their interface.
* Every internal net is renamed ``n<k>``.  The order is derived from a structural
  signature, not from the original names or line order: a Weisfeiler-Lehman style
  refinement where a net's label hashes its driver kind, function and the labels of the
  driver's inputs.  Combinational logic is labelled in one topological pass; latch
  outputs (which break every sequential cycle) are seeded from latch type/init/control
  and refined once per round from the labels of their D/control inputs, until no latch
  class splits any more (or ``--max-rounds``); each round only recomputes the fan-out
  cone of the latches that split.  A fan-out signature is a secondary key.  Nets are
  then ordered by
  (topological level, label, fan-out label) and only nets that are still tied -- which
  happens only for structurally symmetric nets (e.g. duplicated logic, undriven nets,
  combinational loops, or latch chains deeper than ``--max-rounds``) -- are ordered by
  their original name.
* ``.names`` covers are sets: input columns are sorted by canonical net name, the cover
  columns permuted to match, rows sorted and de-duplicated.
* ``.subckt`` connections are sorted by formal name; cell blocks are sorted.
* ``.param`` is semantic and always kept; ``.cname``/``.attr`` are dropped unless
  ``--keep-attrs``.

Everything is iterative (explicit worklists, no recursion); the initial pass is linear
in the netlist size.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import itertools
import math
import re
import sys
from collections import Counter
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blif"))
import blif  # noqa: E402

TOOL = "netlist-compare"
DEFAULT_MAX_ROUNDS = 1000
# Upper bound on column permutations tried when .names inputs have identical labels.
MAX_TIE_PERMUTATIONS = 720
# Above this many canonical lines (both sides) the diff falls back to a linear method.
MAX_UNIFIED_DIFF_LINES = 40000


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


def _cover_key(columns: Sequence[str], rows: Sequence[tuple[str, str]]) -> str:
    """Cover rendering independent of input-column order.

    Columns are ordered by ``columns`` (their labels).  Columns with equal labels are
    interchangeable, so the lexicographically smallest rendering over their permutations
    is used (bounded by MAX_TIE_PERMUTATIONS; beyond that the original order is kept).
    """
    order = sorted(range(len(columns)), key=lambda i: columns[i])
    groups = [list(grp) for _, grp in itertools.groupby(order, key=lambda i: columns[i])]
    n_perm = math.prod(math.factorial(len(grp)) for grp in groups)

    def render(perm: Sequence[int]) -> str:
        cubes = {"".join(plane[i] for i in perm) + " " + out for plane, out in rows}
        return ";".join(sorted(cubes))

    if n_perm == 1 or n_perm > MAX_TIE_PERMUTATIONS:
        return render(order)
    candidates = itertools.product(*(itertools.permutations(grp) for grp in groups))
    return min(render([i for grp in combo for i in grp]) for combo in candidates)


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


def _cone(g: _Graph, nets: Iterable[str], pos: dict[int, int]) -> list[int]:
    """Combinational cells in the transitive fan-out of ``nets``, in topological order."""
    found: set[int] = set()
    work = list(nets)
    while work:
        net = work.pop()
        for idx in g.fanout.get(net, []):
            if idx in pos and idx not in found:
                found.add(idx)
                work.extend(n for n in g.cell_outs[idx] if n not in g.primary)
    return sorted(found, key=pos.__getitem__)


def _refine_latches(g: _Graph, latches: list[blif.Latch], labels: dict[str, str]) -> list[str]:
    """One WL round over latch outputs.  Returns the outputs whose class split (relabelled)."""
    keys: dict[str, tuple[str, str, str]] = {}
    for latch in latches:
        ctrl = blif.latch_control(latch)
        keys[latch.output] = (labels[latch.output], labels[latch.input],
                              "-" if ctrl is None else labels[ctrl])
    split: dict[str, set[tuple[str, str, str]]] = {}
    for key in keys.values():
        split.setdefault(key[0], set()).add(key)
    dirty = [out for out, key in keys.items() if len(split[key[0]]) > 1]
    for out in dirty:
        labels[out] = _h(*keys[out])
    return dirty


def _label_nets(g: _Graph, max_rounds: int) -> tuple[dict[str, str], dict[str, int]]:
    """WL refinement.  Returns (label per net, topological level per net).

    Combinational labels are computed in one topological pass.  Each further round
    refines latch-output labels by their D/control labels; only latch classes that split
    are relabelled, and only their combinational fan-out cone is recomputed.  The loop
    ends when no latch class splits (then no class anywhere can split) or after
    ``max_rounds`` rounds.
    """
    order, cyclic, level = _topo_order(g)
    labels: dict[str, str] = {n: _h("io", n) for n in g.primary}
    for net in g.internal:
        labels[net] = _h("undriven")
    latches = [c for c in g.model.cells
               if isinstance(c, blif.Latch) and c.output not in g.primary]
    for latch in latches:
        labels[latch.output] = _latch_seed(latch, g.primary)
    for idx in cyclic:
        for net in g.cell_outs[idx]:
            if net not in g.primary:
                labels[net] = _cyclic_label(g.model.cells[idx])
    for idx in order:
        _comb_labels(g.model.cells[idx], g, labels)
    pos = {idx: i for i, idx in enumerate(order)}
    for _ in range(max_rounds):
        dirty = _refine_latches(g, latches, labels)
        if not dirty:
            break
        for idx in _cone(g, dirty, pos):
            _comb_labels(g.model.cells[idx], g, labels)
    return labels, level


def _fanout_labels(g: _Graph, labels: dict[str, str]) -> dict[str, str]:
    """Secondary key: hash of how each internal net is consumed."""
    result: dict[str, str] = {}
    for net in g.internal:
        uses: list[str] = []
        for idx in g.fanout.get(net, []):
            cell = g.model.cells[idx]
            outs = ",".join(sorted(labels[o] for o in g.cell_outs[idx]))
            if isinstance(cell, blif.Subckt):
                pins = ",".join(sorted(f for f, a in cell.conns if a == net))
                uses.append(_h("subckt", cell.model, pins, outs))
            elif isinstance(cell, blif.Latch):
                pin = "D" if cell.input == net else "C"
                uses.append(_h("latch", pin, outs))
            else:
                uses.append(_h("names", str(cell.inputs.count(net)), outs))
        result[net] = _h(*sorted(uses))
    return result


def _natural_key(name: str) -> tuple[tuple[int, str], ...]:
    """Sort key that orders ``n9`` before ``n10`` (keeps --canon idempotent on ties)."""
    parts = re.split(r"(\d+)", name)
    return tuple((1, p) if i % 2 == 0 else (0, p.zfill(20)) for i, p in enumerate(parts))


def _pick_prefix(primary: Iterable[str]) -> str:
    """Prefix for canonical names that cannot collide with a primary I/O name."""
    names = set(primary)
    prefix = "n"
    while any(re.fullmatch(re.escape(prefix) + r"\d+", n) for n in names):
        prefix += "_"
    return prefix


def canonical_names(
    model: blif.Model, models: dict[str, blif.Model], max_rounds: int
) -> dict[str, str]:
    """Map every net of ``model`` to its canonical name (primary nets map to themselves)."""
    g = _build_graph(model, models)
    labels, level = _label_nets(g, max_rounds)
    fanout = _fanout_labels(g, labels)
    ranked = sorted(
        g.internal, key=lambda n: (level.get(n, 0), labels[n], fanout[n], _natural_key(n))
    )
    prefix = _pick_prefix(g.primary)
    rename = {n: n for n in g.primary}
    rename.update({net: f"{prefix}{k}" for k, net in enumerate(ranked)})
    return rename


# --------------------------------------------------------------------------- rendering


def _attr_lines(cell: blif.Cell, keep_attrs: bool) -> list[str]:
    kept = [(k, v) for k, v in cell.attrs if keep_attrs or k == ".param"]
    return sorted(f"{k} {v}".rstrip() for k, v in kept)


def _render_names(cell: blif.Names, rn: dict[str, str]) -> list[str]:
    cols = sorted(range(len(cell.inputs)), key=lambda i: (rn[cell.inputs[i]], i))
    header = " ".join([".names", *(rn[cell.inputs[i]] for i in cols), rn[cell.output]])
    rows = {("".join(p[i] for i in cols) + " " + o).strip() for p, o in cell.rows}
    return [header, *sorted(rows)]


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
    model: blif.Model, models: dict[str, blif.Model], keep_attrs: bool, max_rounds: int
) -> list[str]:
    """Canonical text lines for one model."""
    lines = [f".model {model.name}"]
    for directive, ports in ((".inputs", model.inputs), (".outputs", model.outputs),
                             (".clock", model.clocks)):
        lines += [f"{directive} {p}" for p in sorted(set(ports))]
    if model.blackbox:
        return [*lines, ".blackbox", ".end"]
    rn = canonical_names(model, models, max_rounds)
    blocks = sorted({_render_cell(c, rn, keep_attrs) for c in model.cells})
    for block in blocks:
        lines += block.split("\n")
    return [*lines, ".end"]


def canonicalize(
    netlist: blif.Netlist, keep_attrs: bool = False, max_rounds: int = DEFAULT_MAX_ROUNDS
) -> str:
    """Canonical BLIF text for a whole netlist (models separated by a blank line)."""
    models = netlist.by_name()
    ordered = [netlist.top, *sorted(netlist.models[1:], key=lambda m: m.name)]
    chunks = ["\n".join(canonicalize_model(m, models, keep_attrs, max_rounds)) for m in ordered]
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
    ap.add_argument("--max-rounds", type=int, default=DEFAULT_MAX_ROUNDS,
                    help="latch refinement rounds (default %(default)s)")
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    ap = _parser()
    args = ap.parse_args(argv)
    files: list[str] = args.files
    if len(files) != (1 if args.canon else 2):
        ap.error("--canon takes one file" if args.canon else "expected two files")
    try:
        canon = [canonicalize(blif.parse_file(f), args.keep_attrs, args.max_rounds)
                 for f in files]
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
