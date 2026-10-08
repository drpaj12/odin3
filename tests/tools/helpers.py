"""Shared helpers for the tool tests: fixture paths, BLIF dumping and scrambling."""

from __future__ import annotations

import contextlib
import io
import random
from collections.abc import Callable, Sequence
from pathlib import Path

import blif

FIXTURES = Path(__file__).resolve().parent / "fixtures"
REPO_ROOT = Path(__file__).resolve().parents[2]


def fixture(name: str) -> str:
    return str(FIXTURES / name)


def run_main(main: Callable[[Sequence[str]], int], argv: Sequence[str]) -> tuple[int, str, str]:
    """Run a tool's main() in-process; return (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = main(argv)
        except SystemExit as exc:  # argparse usage errors
            code = exc.code if isinstance(exc.code, int) else 2
    return code, out.getvalue(), err.getvalue()


def _cell_lines(cell: blif.Cell) -> list[str]:
    if isinstance(cell, blif.Names):
        lines = [" ".join([".names", *cell.inputs, cell.output])]
        lines += [f"{plane} {out}".strip() for plane, out in cell.rows]
    elif isinstance(cell, blif.Latch):
        parts = [".latch", cell.input, cell.output]
        if cell.ltype is not None and cell.control is not None:
            parts += [cell.ltype, cell.control]
        if cell.init is not None:
            parts.append(cell.init)
        lines = [" ".join(parts)]
    else:
        lines = [" ".join([".subckt", cell.model, *(f"{f}={a}" for f, a in cell.conns)])]
    return lines + [f"{k} {v}".rstrip() for k, v in cell.attrs]


def dump(netlist: blif.Netlist) -> str:
    """Write a netlist back to BLIF (no canonicalization)."""
    out: list[str] = []
    for model in netlist.models:
        out.append(f".model {model.name}")
        for directive, ports in ((".inputs", model.inputs), (".outputs", model.outputs),
                                 (".clock", model.clocks)):
            if ports:
                out.append(" ".join([directive, *ports]))
        if model.blackbox:
            out.append(".blackbox")
        for cell in model.cells:
            out += _cell_lines(cell)
        out.append(".end")
    return "\n".join(out) + "\n"


def _rename(net: str, mapping: dict[str, str]) -> str:
    return mapping.get(net, net)


def _scramble_cell(cell: blif.Cell, rn: dict[str, str], rng: random.Random) -> blif.Cell:
    if isinstance(cell, blif.Names):
        perm = list(range(len(cell.inputs)))
        rng.shuffle(perm)
        rows = [("".join(p[i] for i in perm), o) for p, o in cell.rows]
        rng.shuffle(rows)
        return blif.Names(inputs=[_rename(cell.inputs[i], rn) for i in perm],
                          output=_rename(cell.output, rn), rows=rows, attrs=list(cell.attrs))
    if isinstance(cell, blif.Latch):
        ctrl = None if cell.control is None else _rename(cell.control, rn)
        return blif.Latch(input=_rename(cell.input, rn), output=_rename(cell.output, rn),
                          ltype=cell.ltype, control=ctrl, init=cell.init, attrs=list(cell.attrs))
    conns = [(f, _rename(a, rn)) for f, a in cell.conns]
    rng.shuffle(conns)
    return blif.Subckt(model=cell.model, conns=conns, attrs=list(cell.attrs))


def scramble(netlist: blif.Netlist, seed: int) -> blif.Netlist:
    """Same circuit with random internal net names, shuffled cells/rows/conns/columns
    and shuffled non-top model order."""
    rng = random.Random(seed)
    models = blif.Netlist(path=netlist.path, models=[]).models
    for model in netlist.models:
        primary = model.primary_nets()
        nets: dict[str, None] = {}
        for cell in model.cells:
            ins, outs = blif.cell_io(cell, netlist.by_name())
            nets.update(dict.fromkeys(n for n in ins + outs if n not in primary))
        names = [f"w{rng.randrange(10**9)}_{i}" for i in range(len(nets))]
        rng.shuffle(names)
        rn = dict(zip(nets, names, strict=True))
        cells = [_scramble_cell(c, rn, rng) for c in model.cells]
        rng.shuffle(cells)
        models.append(blif.Model(name=model.name, inputs=list(model.inputs),
                                 outputs=list(model.outputs), clocks=list(model.clocks),
                                 cells=cells, blackbox=model.blackbox))
    rest = models[1:]
    rng.shuffle(rest)
    return blif.Netlist(path=netlist.path, models=models[:1] + rest)
