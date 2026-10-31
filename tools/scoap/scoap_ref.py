#!/usr/bin/env python3
"""Independent reference of the scoap pass (spec docs/specs/2026-10-31-scoap-design.md).

Reads a BLIF itself (no Odin III code, no JSON from Odin III) and computes the SCOAP measures of
section 3 of the spec by brute force: prime implicants by enumeration of ternary cubes,
sensitising assignments by enumeration of minterms, and a plain fixed point over dictionaries.
The pass must give the same integers.

Usage: scoap_ref.py [--sequential] FILE.blif   (prints JSON: {model: {net: {...}}})
"""

from __future__ import annotations

import argparse
import itertools
import json
import sys
from functools import cache
from pathlib import Path
from typing import Any

INF = 1_000_000_000
MAX_EXACT = 8

Net = str
Values = dict[Net, list[int]]


class Unsupported(Exception):
    """A .subckt of a model the BLIF does not define: directions unknown, design skipped."""


class Model:
    """One .model of a BLIF."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.inputs: list[Net] = []
        self.outputs: list[Net] = []
        self.names: list[tuple[list[Net], Net, list[tuple[str, str]]]] = []
        self.latches: list[tuple[Net, Net, str | None]] = []
        self.subckts: list[tuple[str, dict[str, Net]]] = []
        self.blackbox = False


def logical_lines(path: Path) -> list[list[str]]:
    lines: list[list[str]] = []
    pending = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        text = raw.split("#", 1)[0].rstrip()
        if text.endswith("\\"):
            pending += text[:-1] + " "
            continue
        text = pending + text
        pending = ""
        if text.strip():
            lines.append(text.split())
    return lines


def parse_blif(path: Path) -> dict[str, Model]:
    models: dict[str, Model] = {}
    cur: Model | None = None
    cover: tuple[list[Net], Net, list[tuple[str, str]]] | None = None
    for words in logical_lines(path):
        head = words[0]
        if head.startswith("."):
            cover = None
        if head == ".model":
            cur = Model(words[1])
            models[cur.name] = cur
        elif cur is None:
            continue
        elif head == ".inputs":
            cur.inputs += words[1:]
        elif head == ".outputs":
            cur.outputs += words[1:]
        elif head == ".blackbox":
            cur.blackbox = True
        elif head == ".names":
            cover = (words[1:-1], words[-1], [])
            cur.names.append(cover)
        elif head == ".latch":
            ctrl = words[4] if len(words) >= 5 and words[3] in ("re", "fe", "ah", "al") else None
            cur.latches.append((words[1], words[2], ctrl))
        elif head == ".subckt":
            pins = dict(item.split("=", 1) for item in words[2:])
            cur.subckts.append((words[1], pins))
        elif cover is not None:
            n_in = len(cover[0])
            row = (words[0], words[1]) if n_in > 0 else ("", words[0])
            cover[2].append(row)
    return models


def sop_value(rows: list[tuple[str, str]], bits: tuple[int, ...]) -> int:
    """The simulator's $sop semantics: ON rows give 1 on a hit, OFF rows 0 on a hit, none 0."""
    has_on = False
    off_hit = False
    for pattern, out in rows:
        hit = all(c == "-" or int(c) == b for c, b in zip(pattern, bits, strict=True))
        if out == "1":
            if hit:
                return 1
            has_on = True
        else:
            off_hit = off_hit or hit
    return 1 if (not has_on and rows and not off_hit) else 0


Cube = tuple[int | None, ...]


@cache
def primes(table: tuple[int, ...], n: int, val: int) -> tuple[Cube, ...]:
    """Prime implicants of {f = val}; table[m] = f(m), bit j of m = input j."""

    @cache
    def implicant(cube: Cube) -> bool:
        for j, lit in enumerate(cube):
            if lit is None:
                return implicant(cube[:j] + (0,) + cube[j + 1 :]) and implicant(
                    cube[:j] + (1,) + cube[j + 1 :]
                )
        minterm = sum((lit or 0) << j for j, lit in enumerate(cube))
        return table[minterm] == val

    out = []
    for cube in itertools.product((0, 1, None), repeat=n):
        if not implicant(cube):
            continue
        widened = (
            cube[:j] + (None,) + cube[j + 1 :] for j, lit in enumerate(cube) if lit is not None
        )
        if not any(implicant(w) for w in widened):
            out.append(cube)
    return tuple(out)


@cache
def sensitising(table: tuple[int, ...], n: int, i: int) -> tuple[tuple[int, ...], ...]:
    """Assignments (full minterms with bit i = 0) of the other inputs on which f depends on i."""
    return tuple(
        tuple((m >> j) & 1 for j in range(n))
        for m in range(1 << n)
        if not (m >> i) & 1 and table[m] != table[m | (1 << i)]
    )


class Cell:
    def __init__(self, kind: str, ins: list[Net], out: Net) -> None:
        self.kind = kind  # func, wide, ff
        self.ins = ins
        self.out = out
        self.table: tuple[int, ...] = ()
        self.rows: list[tuple[str, str]] = []
        self.cover_val = -2


class Solver:
    def __init__(self, model: Model, models: dict[str, Model], seq: bool) -> None:
        self.seq = seq
        self.cc: Values = {}
        self.sc: Values = {}
        self.co: dict[Net, int] = {}
        self.so: dict[Net, int] = {}
        self.fixed: set[Net] = set()
        self.cells: list[Cell] = []
        self.sweeps = 0
        self.converged = True
        self.opaque = 0
        self.build(model, models)

    def touch(self, net: Net) -> None:
        self.cc.setdefault(net, [INF, INF])
        self.sc.setdefault(net, [INF, INF])
        self.co.setdefault(net, INF)
        self.so.setdefault(net, INF)

    def source(self, net: Net) -> None:
        self.touch(net)
        if net not in self.fixed:
            self.cc[net] = [1, 1]
            self.sc[net] = [0, 0]

    def sink(self, net: Net) -> None:
        self.touch(net)
        self.co[net] = 0
        self.so[net] = 0

    def build(self, model: Model, models: dict[str, Model]) -> None:
        for ins, out, rows in model.names:
            if not ins:
                self.touch(out)
                held = 1 if any(r[1] == "1" for r in rows) else 0
                self.fixed.add(out)
                self.cc[out] = [INF, INF]
                self.sc[out] = [INF, INF]
                self.cc[out][held] = 1
                self.sc[out][held] = 0
        for net in model.inputs:
            self.source(net)
        for net in model.outputs:
            self.sink(net)
        for ins, out, rows in model.names:
            if ins:
                self.add_names(ins, out, rows)
        for din, qout, ctrl in model.latches:
            self.add_latch(din, qout, ctrl)
        for mname, pins in model.subckts:
            if mname not in models:
                raise Unsupported(f"model {mname} not defined in the file")
            self.add_subckt(models[mname], pins)

    def add_names(self, ins: list[Net], out: Net, rows: list[tuple[str, str]]) -> None:
        for net in [*ins, out]:
            self.touch(net)
        n = len(ins)
        if n > MAX_EXACT:
            cell = Cell("wide", ins, out)
            cell.rows = rows
            on = any(r[1] == "1" for r in rows)
            cell.cover_val = 1 if on else (0 if rows else -1)
        else:
            cell = Cell("func", ins, out)
            cell.table = tuple(
                sop_value(rows, tuple((m >> j) & 1 for j in range(n))) for m in range(1 << n)
            )
        self.cells.append(cell)

    def add_latch(self, din: Net, qout: Net, ctrl: Net | None) -> None:
        for net in [din, qout, *([ctrl] if ctrl else [])]:
            self.touch(net)
        if self.seq:
            self.cells.append(Cell("ff", [din, ctrl or ""], qout))
        else:
            self.source(qout)
            self.sink(din)
            if ctrl:
                self.sink(ctrl)

    def add_subckt(self, target: Model, pins: dict[str, Net]) -> None:
        for formal, actual in pins.items():
            self.touch(actual)
            if formal in target.outputs:
                self.source(actual)
            else:
                self.sink(actual)
        self.opaque += 1

    # --- cost helpers ------------------------------------------------------------------------

    def cost(self, dom: str, net: Net, v: int) -> int:
        bonus = 1 if dom == "c" else 0
        tab = self.cc if dom == "c" else self.sc
        if net == "":
            return 0
        return tab[net][v] if net else (bonus if v == 0 else INF)

    def lower(self, arr: dict[Net, int] | list[int], key: Any, value: int, net: Net) -> bool:
        value = min(value, INF)
        if net in self.fixed or value >= arr[key]:
            return False
        arr[key] = value
        return True

    def forward(self, dom: str) -> bool:
        bonus = 1 if dom == "c" else 0
        tab = self.cc if dom == "c" else self.sc
        changed = False
        for cell in self.cells:
            for v in (0, 1):
                best = self.fwd_cost(cell, dom, v)
                val = (
                    INF
                    if best >= INF
                    else best + (bonus if cell.kind != "ff" else (0 if dom == "c" else 1))
                )
                if cell.kind == "ff":
                    val = INF if best >= INF else val
                changed |= self.lower(tab[cell.out], v, val, cell.out)
        return changed

    def fwd_cost(self, cell: Cell, dom: str, v: int) -> int:
        n = len(cell.ins)
        if cell.kind == "ff":
            return self.cost(dom, cell.ins[0], v) + self.clock(dom, cell.ins[1])
        if cell.kind == "func":
            best = INF
            for cube in primes(cell.table, n, v):
                total = sum(
                    self.cost(dom, cell.ins[j], lit)
                    for j, lit in enumerate(cube)
                    if lit is not None
                )
                best = min(best, total)
            return best
        if cell.cover_val == v:
            best = INF
            for pattern, out in cell.rows:
                if out == str(v):
                    total = sum(
                        self.cost(dom, cell.ins[j], int(c))
                        for j, c in enumerate(pattern)
                        if c != "-"
                    )
                    best = min(best, total)
            return best
        if cell.cover_val == -1:
            return 0 if v == 0 else INF
        return sum(min(self.cost(dom, x, 0), self.cost(dom, x, 1)) for x in cell.ins)

    def clock(self, dom: str, clk: Net) -> int:
        if clk == "":
            return 0
        tab = self.cc if dom == "c" else self.sc
        return tab[clk][0] + tab[clk][1]

    def backward(self, dom: str) -> bool:
        bonus = 1 if dom == "c" else 0
        obs = self.co if dom == "c" else self.so
        tab = self.cc if dom == "c" else self.sc
        changed = False
        for cell in reversed(self.cells):
            base = obs[cell.out]
            if base >= INF:
                continue
            if cell.kind == "ff":
                frame = 0 if dom == "c" else 1
                clk = self.clock(dom, cell.ins[1])
                for j, net in enumerate(cell.ins):
                    if j != 1 and net:
                        changed |= self.lower(obs, net, base + clk + frame, net)
                if cell.ins[1]:
                    via0 = self.cost(dom, cell.ins[0], 0) + tab[cell.out][1]
                    via1 = self.cost(dom, cell.ins[0], 1) + tab[cell.out][0]
                    flip = min(via0, via1)
                    if flip < INF:
                        changed |= self.lower(
                            obs, cell.ins[1], base + clk + frame + flip, cell.ins[1]
                        )
                continue
            for i, net in enumerate(cell.ins):
                best = self.bwd_cost(cell, dom, i)
                if best < INF:
                    changed |= self.lower(obs, net, base + bonus + best, net)
        return changed

    def bwd_cost(self, cell: Cell, dom: str, i: int) -> int:
        n = len(cell.ins)
        if cell.kind == "func":
            best = INF
            for bits in sensitising(cell.table, n, i):
                total = sum(self.cost(dom, cell.ins[j], bits[j]) for j in range(n) if j != i)
                best = min(best, total)
            return best
        if cell.cover_val == -1:
            return INF
        return sum(
            min(self.cost(dom, x, 0), self.cost(dom, x, 1))
            for j, x in enumerate(cell.ins)
            if j != i
        )

    def run_phase(self, dom: str, fwd: bool) -> None:
        cap = len(self.cc) + 2
        done = 0
        changed = True
        while changed and done < cap:
            changed = self.forward(dom) if fwd else self.backward(dom)
            done += 1
        self.sweeps += done
        self.converged = self.converged and not changed

    def solve(self) -> None:
        self.run_phase("c", True)
        self.run_phase("c", False)
        if self.seq:
            self.run_phase("s", True)
            self.run_phase("s", False)


def reference(path: Path, seq: bool) -> dict[str, dict[Net, dict[str, int]]]:
    models = parse_blif(path)
    result: dict[str, dict[Net, dict[str, int]]] = {}
    for name, model in models.items():
        if model.blackbox:
            continue
        sol = Solver(model, models, seq)
        sol.solve()
        nets: dict[Net, dict[str, int]] = {}
        for net in sol.cc:
            item = {"cc0": sol.cc[net][0], "cc1": sol.cc[net][1], "co": sol.co[net]}
            if seq:
                item |= {"sc0": sol.sc[net][0], "sc1": sol.sc[net][1], "so": sol.so[net]}
            nets[net] = item
        result[name] = nets
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sequential", action="store_true")
    parser.add_argument("blif", type=Path)
    args = parser.parse_args()
    json.dump(reference(args.blif, args.sequential), sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
