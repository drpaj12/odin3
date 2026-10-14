"""rampair: the pure part of pairing Parmys RAM slices by memory group and slice index.

Parmys splits every memory into one ``single_port_ram`` / ``dual_port_ram`` cell per data bit and
names the output nets ``<type>^MEM~<id>-<bit>^out...`` (for example ``dual_port_ram^MEM~0-30^out1``,
``single_port_ram^MEM~47-9^out``).  The part after ``-`` is the slice index inside the memory; it
is stable.  The ``<id>`` is a node counter of the Parmys run: two runs on the same design number
the same memories differently (arm_core: the memory behind id 47 in one run is id 62 in the other),
so an id is only a way to *group* the slices of one memory inside one netlist.  A slice therefore
has the identity (memory group, bit); pairing two netlists comes down to pairing the groups, and
the slices then pair by bit exactly.

This module holds no netlist types: it reads names, builds groups, and sorts groups of two sides
into exact pairs, classes that simulation cannot split, and leftovers.  ``equiv_check`` does the
simulation and the ABC probes that resolve the classes.  Nothing here is trusted by the verdict: a
paired slice is a cut point whose every input pin ABC must still prove equal.
"""

from __future__ import annotations

import collections
import re
from collections.abc import Hashable, Iterable, Sequence
from dataclasses import dataclass, field

# ``MEM~<id>-<bit>^out``: the id may hold anything but a space or ``^`` (``4__S``, ``0``); the
# last ``-<digits>^out`` is the bit.
_SLICE_RE = re.compile(r"MEM~([^\s^]+)-(\d+)\^out")


class RamError(Exception):
    """Memory groups of different shapes (exit 2)."""


def slice_identity(nets: Iterable[str]) -> tuple[str, int] | None:
    """(memory id, bit) read from the output nets, else None.  Nets that carry different
    identities (a cell that is not one slice of one memory) give None."""
    found = {(m.group(1), int(m.group(2))) for net in nets if (m := _SLICE_RE.search(net))}
    return found.pop() if len(found) == 1 else None


@dataclass
class Group:
    """The slices of one memory in one netlist: ``slices`` maps bit -> index in the box list."""

    cell_type: str
    mem: str
    slices: dict[int, int] = field(default_factory=dict)

    @property
    def bits(self) -> tuple[int, ...]:
        return tuple(sorted(self.slices))

    @property
    def shape(self) -> tuple[str, tuple[int, ...]]:
        return self.cell_type, self.bits


def build_groups(idents: Sequence[tuple[str, tuple[str, int] | None]]) -> list[Group]:
    """Group boxes (``idents[i]`` is box i's (cell type, identity or None)) by (type, memory id).

    Two boxes with one (type, id, bit) cannot be told apart by name (a sub-model instantiated
    twice names its memory the same in both instances): the whole (type, id) is dropped, as if
    its slices carried no identity, and they are left to the signature pairing."""
    groups: dict[tuple[str, str], Group] = {}
    clash: set[tuple[str, str]] = set()
    for index, (cell_type, ident) in enumerate(idents):
        if ident is None:
            continue
        mem, bit = ident
        group = groups.setdefault((cell_type, mem), Group(cell_type, mem))
        if bit in group.slices:
            clash.add((cell_type, mem))
        group.slices[bit] = index
    return [g for key, g in groups.items() if key not in clash]


def check_shapes(groups_a: Sequence[Group], groups_b: Sequence[Group]) -> None:
    """Both sides name their slices of a cell type, but not with the same groups: RamError.

    The shape of a group is its cell type and its set of bits.  Where both sides have groups of
    a cell type, the two multisets of shapes must agree; anything else means the netlists do not
    hold the same memories (or one names them in a way this module misreads), and pairing would
    be a guess."""
    for cell_type in sorted({g.cell_type for g in groups_a} & {g.cell_type for g in groups_b}):
        count_a = collections.Counter(g.shape for g in groups_a if g.cell_type == cell_type)
        count_b = collections.Counter(g.shape for g in groups_b if g.cell_type == cell_type)
        if count_a != count_b:
            lines = []
            for shape in sorted(set(count_a) | set(count_b), key=repr):
                if count_a[shape] != count_b[shape]:
                    bits = shape[1]
                    lines.append(f"  {cell_type} memory with {len(bits)} slice(s) "
                                 f"(bits {bits[0]}..{bits[-1]}): {count_a[shape]} in A, "
                                 f"{count_b[shape]} in B")
            raise RamError(f"{cell_type} slices are named by memory and bit in both netlists, "
                           "but the memories differ in size or number\n" + "\n".join(lines))


@dataclass
class Classes:
    """Groups of two sides sorted by an equality key: ``exact`` pairs (the key is unique on both
    sides), ``ambiguous`` classes (the same key on several groups of each side, equal counts) and
    the groups ``left_a`` / ``left_b`` whose key has no partner or a different count."""

    exact: list[tuple[int, int]]
    ambiguous: list[tuple[list[int], list[int]]]
    left_a: list[int]
    left_b: list[int]


def classify(keys_a: Sequence[Hashable], keys_b: Sequence[Hashable]) -> Classes:
    """Sort the groups of A and B (``keys_x[i]`` is group i's key) by key."""
    by_a: dict[Hashable, list[int]] = collections.defaultdict(list)
    by_b: dict[Hashable, list[int]] = collections.defaultdict(list)
    for i, key in enumerate(keys_a):
        by_a[key].append(i)
    for j, key in enumerate(keys_b):
        by_b[key].append(j)
    result = Classes([], [], [], [])
    for key, ids_a in by_a.items():
        ids_b = by_b.get(key, [])
        if len(ids_a) == len(ids_b) == 1:
            result.exact.append((ids_a[0], ids_b[0]))
        elif len(ids_a) == len(ids_b):
            result.ambiguous.append((ids_a, ids_b))
        else:
            result.left_a += ids_a
            result.left_b += ids_b
    result.left_b += [j for key, ids in by_b.items() if key not in by_a for j in ids]
    return result
