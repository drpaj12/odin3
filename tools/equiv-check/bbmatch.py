"""bbmatch: simulation signatures that pair the unnamed black boxes of two netlists (equiv-check).

A hard block that Parmys writes has no ``.cname`` and no stable name: its outputs are counters
(``$mul~20[0]``, ``dual_port_ram^MEM~0-30^out1[0]``) that differ between two runs.  Such a box is
identified by what it *is*, not what it is called: its cell type, its parameters, its port list
and widths, and the function of each of its input bits -- the *signature*.

The signature is a simulation signature.  Both netlists are simulated on the same random input
vectors (``LANES`` independent lanes for ``CYCLES`` clock cycles, latches starting at their init
value, primary inputs drawn from their normalised name, so a name means the same vector on both
sides).  A cut box has no logic to simulate; its outputs are driven by a pseudo-random function of
the box's *history*: a digest of its type, its parameters, its ports and every input vector it saw
in the cycles before.  Two boxes with the same type and the same input history therefore get the
same outputs, in either netlist, and a box downstream of them gets the same history again.  That
is congruence closure by simulation, computed in one forward pass with no matching decision taken
during the run (a box in a feedback loop through a latch needs none).  A named box (``.cname``) is
paired by its name; its outputs are driven by a function of the name instead, so a difference
upstream of it does not hide the unnamed boxes downstream.

The signature of a box is its digest after the last cycle.  Boxes of the two netlists are paired
when their signatures are equal and unique on both sides.  Several boxes with one signature are
paired in file order only if they are interchangeable: every one of them has the same input net
for every pin (a buffer chain or a constant is looked through), so which is which cannot matter.
Twins also share their outputs (one free variable per group and pin): a box is a function of its
inputs, so boxes with the same inputs have the same outputs, and which of them a consumer reads
must not decide the verdict.

The pairing is a *guess* that the equivalence check afterwards verifies; nothing here is trusted.
See ``equiv_check``'s docstring for why that makes any pairing sound.
"""

from __future__ import annotations

import collections
import hashlib
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass

import blif

LANES = 128
CYCLES = 48
MASK = (1 << LANES) - 1
_LANE_BYTES = LANES // 8
_DIGEST_BYTES = 16
REFINE_ROUNDS = 8


@dataclass(frozen=True)
class BoxSpec:
    """One cut black box as the simulator sees it.  ``seed`` is its name (None: unnamed),
    ``inputs`` its (pin, flat net) pairs and ``outputs`` its (pin, free input net) pairs, both in
    a fixed pin order."""

    cell_type: str
    params: tuple[tuple[str, str], ...]
    seed: str | None
    inputs: tuple[tuple[str, str], ...]
    outputs: tuple[tuple[str, str], ...]


@dataclass(frozen=True)
class Signature:
    """``digest``: type, parameters, ports and the whole input history.  ``identity``: the
    canonical driver of each input pin (equal identities make two boxes interchangeable)."""

    digest: bytes
    identity: tuple[str, ...]
    struct: int = 0  # colour of the box's input cones (``refine``), a tie-break only


@dataclass
class Pairing:
    """The result of ``pair``: index pairs (A, B); ``twins``, the groups of several interchangeable
    boxes among them (their members are paired in order); the indices left over on each side;
    and ``guessed``, the number of pairs chosen by structure or file order among boxes that
    simulation could not tell apart and whose input nets differ."""

    pairs: list[tuple[int, int]]
    twins: list[tuple[list[int], list[int]]]
    only_a: list[int]
    only_b: list[int]
    guessed: int
    exact: list[tuple[int, int]]  # the pairs whose signature is unique on both sides


class SimError(Exception):
    """The netlist cannot be simulated (a combinational loop)."""


def _vector(*parts: bytes | str) -> int:
    """A pseudo-random ``LANES``-bit integer determined by ``parts``."""
    hasher = hashlib.blake2b(digest_size=_LANE_BYTES)
    for part in parts:
        raw = part.encode() if isinstance(part, str) else part
        hasher.update(len(raw).to_bytes(4, "little"))
        hasher.update(raw)
    return int.from_bytes(hasher.digest(), "little")


_QUARTER = LANES // 4
_Q_MASKS = [((1 << _QUARTER) - 1) << (q * _QUARTER) for q in range(4)]


def _stimulus(name: str, cycle: int) -> int:
    """A primary input's vector.  Plain random words saturate deep decode logic at a constant
    (an AND of twenty inputs is never 1), so the four quarters of the lanes are biased: 1/2, 7/8,
    1/8 and 15/16 ones."""
    words = [_vector("pi", name, str(cycle), str(k)) for k in range(12)]
    half = words[0] & _Q_MASKS[0]
    high = (words[1] | words[2] | words[3]) & _Q_MASKS[1]
    low = (words[4] & words[5] & words[6]) & _Q_MASKS[2]
    top = (words[7] | words[8] | words[9] | words[10] | words[11]) & _Q_MASKS[3]
    return half | high | low | top


def _bytes(value: int) -> bytes:
    return value.to_bytes(_LANE_BYTES, "little")


# A compiled cover: (output net, input nets, rows as (positive nets, negative nets), on-set).
_Cover = tuple[str, tuple[str, ...], tuple[tuple[tuple[str, ...], tuple[str, ...]], ...], bool]


def _compile(cell: blif.Names) -> _Cover:
    rows = []
    for plane, _ in cell.rows:
        pos = tuple(n for n, ch in zip(cell.inputs, plane, strict=False) if ch == "1")
        neg = tuple(n for n, ch in zip(cell.inputs, plane, strict=False) if ch == "0")
        rows.append((pos, neg))
    on_set = not cell.rows or cell.rows[0][1] == "1"
    return cell.output, tuple(cell.inputs), tuple(rows), on_set


def _topological(covers: Sequence[_Cover], sources: set[str]) -> list[_Cover]:
    """The covers in an order where each follows the covers that drive its inputs."""
    driver = {c[0]: c for c in covers}
    waiting: dict[str, int] = {}
    fanout: dict[str, list[str]] = {}
    ready: list[_Cover] = []
    for cover in covers:
        deps = {n for n in cover[1] if n in driver and n not in sources}
        waiting[cover[0]] = len(deps)
        for net in deps:
            fanout.setdefault(net, []).append(cover[0])
        if not deps:
            ready.append(cover)
    order: list[_Cover] = []
    while ready:
        cover = ready.pop()
        order.append(cover)
        for out in fanout.get(cover[0], []):
            waiting[out] -= 1
            if waiting[out] == 0:
                ready.append(driver[out])
    if len(order) != len(driver):
        raise SimError("combinational loop")
    return order


def _evaluate(order: Iterable[_Cover], vals: dict[str, int]) -> None:
    for out, _, rows, on_set in order:
        acc = 0
        for pos, neg in rows:
            term = MASK
            for net in pos:
                term &= vals[net]
            for net in neg:
                term &= ~vals[net]
            acc |= term
        vals[out] = acc if on_set else ~acc & MASK


def canonical_drivers(names: Sequence[blif.Names]) -> Callable[[str], str]:
    """net -> its canonical identity: buffers are looked through and constants are named."""
    buffers: dict[str, str] = {}
    consts: dict[str, str] = {}
    for cell in names:
        if not cell.inputs:
            consts[cell.output] = "const1" if cell.rows and cell.rows[0][1] == "1" else "const0"
        elif len(cell.inputs) == 1 and _is_identity(cell):
            buffers[cell.output] = cell.inputs[0]

    def canon(net: str) -> str:
        seen = 0
        while net in buffers and seen <= len(buffers):
            net = buffers[net]
            seen += 1
        return consts.get(net, net)

    return canon


def _is_identity(cell: blif.Names) -> bool:
    if not cell.rows:
        return False
    on_set = cell.rows[0][1] == "1"

    def value(bit: str) -> bool:
        hit = any(plane in (bit, "-") for plane, _ in cell.rows)
        return hit if on_set else not hit

    return not value("0") and value("1")


def _start_digest(box: BoxSpec) -> bytes:
    """The digest of a box before any cycle: type, parameters and the port list."""
    hasher = hashlib.blake2b(digest_size=_DIGEST_BYTES)
    for field in (box.cell_type, repr(box.params), repr([p for p, _ in box.inputs]),
                  repr([p for p, _ in box.outputs])):
        hasher.update(field.encode())
        hasher.update(b"\0")
    return hasher.digest()


def _next_digest(digest: bytes, box: BoxSpec, vals: dict[str, int]) -> bytes:
    hasher = hashlib.blake2b(digest, digest_size=_DIGEST_BYTES)
    for _, net in box.inputs:
        hasher.update(_bytes(vals[net]))
    return hasher.digest()


def _digests(covers: Sequence[_Cover], order: Sequence[_Cover], latches: Sequence[blif.Latch],
             pis: Sequence[str], boxes: Sequence[BoxSpec],
             pi_key: Callable[[str], str]) -> list[bytes]:
    """The final history digest of each box, from simulating the netlist."""
    state = {latch.output: MASK if latch.init == "1" else 0 for latch in latches}
    nets = {n for c in covers for n in (c[0], *c[1])} | {lat.input for lat in latches}
    nets |= {net for box in boxes for _, net in box.inputs}
    base = dict.fromkeys(nets - {c[0] for c in covers}, 0)
    digests = [_start_digest(box) for box in boxes]
    for cycle in range(CYCLES):
        vals = dict(base)
        vals.update(state)
        for net in pis:
            vals[net] = _stimulus(pi_key(net), cycle)
        for box, digest in zip(boxes, digests, strict=True):
            key = digest if box.seed is None else f"{box.cell_type}\0{box.seed}".encode()
            for pin, net in box.outputs:
                vals[net] = _vector("out", key, pin, str(cycle))
        _evaluate(order, vals)
        digests = [_next_digest(d, box, vals) for box, d in zip(boxes, digests, strict=True)]
        state = {latch.output: vals[latch.input] for latch in latches}
    return digests


def _cover_colour(cover: _Cover, colours: dict[str, int]) -> int:
    rows = frozenset((tuple(sorted(colours[n] for n in pos)),
                      tuple(sorted(colours[n] for n in neg))) for pos, neg in cover[2])
    return hash((cover[3], rows))


def refine(covers: Sequence[_Cover], order: Sequence[_Cover], latches: Sequence[blif.Latch],
           pis: Sequence[str], boxes: Sequence[BoxSpec],
           pi_key: Callable[[str], str]) -> list[int]:
    """A structural colour per box (colour refinement): equal colours mean the input cones of
    the boxes are isomorphic given the primary input names.  A latch or a box output takes the
    colour of what feeds it, round after round, until no colour is told apart any more.  Used
    only to split a group that simulation cannot split."""
    zero = hash((True, frozenset()))  # an undriven net, like a constant-0 cover
    sources = {n: hash(("pi", pi_key(n))) for n in pis}
    sources.update((lat.output, hash(("q", lat.init, 0))) for lat in latches)
    sources.update((net, hash(("free", b.cell_type, b.params, pin, b.seed)))
                   for b in boxes for pin, net in b.outputs)
    inputs_of: list[tuple[int, ...]] = []
    for _ in range(REFINE_ROUNDS):
        colours = collections.defaultdict(lambda: zero, sources)
        for cover in order:
            colours[cover[0]] = _cover_colour(cover, colours)
        inputs_of = [tuple(colours[n] for _, n in box.inputs) for box in boxes]
        fresh = dict(sources)
        for lat in latches:
            fresh[lat.output] = hash(("q", lat.init, colours[lat.input]))
        for box, ins in zip(boxes, inputs_of, strict=True):
            for pin, net in box.outputs:
                fresh[net] = hash(("free", box.cell_type, box.params, pin,
                                   box.seed if box.seed is not None else ins))
        settled = len(set(fresh.values())) == len(set(sources.values()))
        sources = fresh
        if settled:
            break
    return [hash((box.cell_type, box.params, ins)) for box, ins in zip(boxes, inputs_of,
                                                                         strict=True)]


def signatures(names: Sequence[blif.Names], latches: Sequence[blif.Latch],
               inputs: Sequence[str], boxes: Sequence[BoxSpec],
               pi_key: Callable[[str], str], structure: bool = True) -> list[Signature]:
    """The signature of each of ``boxes``, from simulating the flat netlist ``names``/``latches``.

    ``inputs`` are the flat primary inputs (the boxes' free output nets included); ``pi_key``
    names a primary input in the way both netlists share.  ``structure`` False skips the colour
    refinement (every ``Signature.struct`` is 0).  Raises SimError on a loop."""
    covers = [_compile(c) for c in names]
    free = {net for box in boxes for _, net in box.outputs}
    pis = [n for n in inputs if n not in free]
    order = _topological(covers, set(pis) | free | {lat.output for lat in latches})
    digests = _digests(covers, order, latches, pis, boxes, pi_key)
    colours = (refine(covers, order, latches, pis, boxes, pi_key) if structure
               else [0] * len(boxes))
    canon = canonical_drivers(names)
    return [Signature(d, tuple(canon(net) for _, net in box.inputs), c)
            for box, d, c in zip(boxes, digests, colours, strict=True)]


def pair(sigs_a: Sequence[Signature], sigs_b: Sequence[Signature]) -> Pairing:
    """Pair two lists of signatures (see the module docstring for the rules)."""
    groups: dict[bytes, tuple[list[int], list[int]]] = {}
    for side, sigs in enumerate((sigs_a, sigs_b)):
        for index, sig in enumerate(sigs):
            groups.setdefault(sig.digest, ([], []))[side].append(index)
    result = Pairing([], [], [], [], 0, [])
    for list_a, list_b in groups.values():
        if len(list_a) != len(list_b):
            result.only_a += list_a
            result.only_b += list_b
        elif len(list_a) == 1 or (_interchangeable(sigs_a, list_a)
                                  and _interchangeable(sigs_b, list_b)):
            result.pairs += zip(list_a, list_b, strict=True)
            if len(list_a) > 1:
                result.twins.append((list_a, list_b))
            else:
                result.exact.append((list_a[0], list_b[0]))
        else:
            _split_by_structure(sigs_a, sigs_b, (list_a, list_b), result)
    result.pairs.sort()
    result.exact.sort()
    result.only_a.sort()
    result.only_b.sort()
    return result


def _split_by_structure(sigs_a: Sequence[Signature], sigs_b: Sequence[Signature],
                        group: tuple[list[int], list[int]], result: Pairing) -> None:
    """Pair a group that simulation cannot split (same signature, different input nets) by the
    colour of the input cones, then in file order inside one colour; if the colours do not split
    it into equal parts, the whole group in file order.  These pairs are guesses --
    ``result.guessed`` counts them -- that the equivalence check must confirm."""
    parts: dict[int, tuple[list[int], list[int]]] = {}
    for side, (sigs, members) in enumerate(((sigs_a, group[0]), (sigs_b, group[1]))):
        for index in members:
            parts.setdefault(sigs[index].struct, ([], []))[side].append(index)
    if any(len(a) != len(b) for a, b in parts.values()):
        parts = {0: group}
    for list_a, list_b in parts.values():
        result.pairs += zip(list_a, list_b, strict=True)
        result.guessed += len(list_a)


def _interchangeable(sigs: Sequence[Signature], group: Sequence[int]) -> bool:
    return all(sigs[i].identity == sigs[group[0]].identity for i in group)
