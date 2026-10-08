"""Minimal reader for VTR-dialect BLIF, shared by netlist-compare and equiv-check.

Supported: multiple ``.model`` blocks (the first one is the top), ``.inputs``,
``.outputs``, ``.clock``, ``.names`` with single-output covers, ``.latch``,
``.subckt``, ``.blackbox``, the VTR/Yosys extensions ``.cname``, ``.attr`` and
``.param`` (attached to the preceding cell), ``#`` comments, ``\\`` line
continuation and ``.end``.  Anything else raises :class:`BlifError`.

The reader is a single linear pass; nothing here recurses over the netlist.
Standard library only.
"""

from __future__ import annotations

import re
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import TypeAlias

LATCH_TYPES = frozenset({"fe", "re", "ah", "al", "as"})
LATCH_INITS = frozenset({"0", "1", "2", "3"})
ATTR_DIRECTIVES = frozenset({".cname", ".attr", ".param"})
NO_CONTROL = "NIL"

_PLANE_RE = re.compile(r"[01-]*")


class BlifError(Exception):
    """Malformed or unsupported BLIF input.  ``str(err)`` is ``path:line: message``."""


@dataclass
class Names:
    """A ``.names`` cell: single-output cover.  ``rows`` holds (input plane, output bit)."""

    inputs: list[str]
    output: str
    rows: list[tuple[str, str]] = field(default_factory=list)
    attrs: list[tuple[str, str]] = field(default_factory=list)
    line: int = 0


@dataclass
class Latch:
    """A ``.latch``.  ``ltype``/``control`` are both None or both set; ``init`` may be None."""

    input: str
    output: str
    ltype: str | None = None
    control: str | None = None
    init: str | None = None
    attrs: list[tuple[str, str]] = field(default_factory=list)
    line: int = 0


@dataclass
class Subckt:
    """A ``.subckt`` instance; ``conns`` is the list of (formal, actual) pairs."""

    model: str
    conns: list[tuple[str, str]]
    attrs: list[tuple[str, str]] = field(default_factory=list)
    line: int = 0


Cell: TypeAlias = Names | Latch | Subckt


@dataclass
class Model:
    """One ``.model`` block."""

    name: str
    inputs: list[str] = field(default_factory=list)
    outputs: list[str] = field(default_factory=list)
    clocks: list[str] = field(default_factory=list)
    cells: list[Cell] = field(default_factory=list)
    blackbox: bool = False
    line: int = 0

    def primary_nets(self) -> set[str]:
        """All interface nets: inputs, outputs and clocks."""
        return set(self.inputs) | set(self.outputs) | set(self.clocks)


@dataclass
class Netlist:
    """A parsed BLIF file.  ``models[0]`` is the top model."""

    path: str
    models: list[Model]

    @property
    def top(self) -> Model:
        return self.models[0]

    def by_name(self) -> dict[str, Model]:
        return {m.name: m for m in self.models}


def latch_control(latch: Latch) -> str | None:
    """The latch control net, or None when absent or ``NIL``."""
    if latch.control is None or latch.control == NO_CONTROL:
        return None
    return latch.control


def cell_io(cell: Cell, models: dict[str, Model]) -> tuple[list[str], list[str]]:
    """Return (input nets, output nets) of a cell.  Subckt pins are classified by the model."""
    if isinstance(cell, Names):
        return list(cell.inputs), [cell.output]
    if isinstance(cell, Latch):
        ctrl = latch_control(cell)
        return ([cell.input] if ctrl is None else [cell.input, ctrl]), [cell.output]
    outs = set(models[cell.model].outputs)
    ins = [a for f, a in cell.conns if f not in outs]
    return ins, [a for f, a in cell.conns if f in outs]


# --------------------------------------------------------------------------- lexing


def logical_lines(text: str) -> Iterator[tuple[int, list[str]]]:
    """Yield (first physical line number, tokens) per logical line.

    Comments (``#`` to end of line) are removed before joining ``\\`` continuations.
    """
    pending: list[str] = []
    start = 0
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].rstrip()
        if not pending:
            start = lineno
        if line.endswith("\\"):
            pending.append(line[:-1])
            continue
        pending.append(line)
        tokens = " ".join(pending).split()
        pending = []
        if tokens:
            yield start, tokens
    if pending:
        tokens = " ".join(pending).split()
        if tokens:
            yield start, tokens


# --------------------------------------------------------------------------- parsing


class _Parser:
    def __init__(self, path: str) -> None:
        self.path = path
        self.models: list[Model] = []
        self.model: Model | None = None
        self.cell: Cell | None = None
        self.line = 0

    def error(self, msg: str) -> BlifError:
        return BlifError(f"{self.path}:{self.line}: {msg}")

    def need_model(self, directive: str) -> Model:
        if self.model is None:
            raise self.error(f"{directive} outside of a .model")
        return self.model

    def feed(self, lineno: int, tokens: list[str]) -> None:
        self.line = lineno
        head = tokens[0]
        if not head.startswith("."):
            self.cover_row(tokens)
            return
        handler = _HANDLERS.get(head)
        if handler is None:
            raise self.error(f"unsupported directive {head}")
        handler(self, tokens)

    def add_cell(self, cell: Cell) -> None:
        self.need_model(".names/.latch/.subckt").cells.append(cell)
        self.cell = cell

    # -- directives ---------------------------------------------------------

    def d_model(self, tokens: list[str]) -> None:
        if len(tokens) != 2:
            raise self.error(".model takes exactly one name")
        self.model = Model(name=tokens[1], line=self.line)
        self.models.append(self.model)
        self.cell = None

    def d_ports(self, tokens: list[str]) -> None:
        model = self.need_model(tokens[0])
        target = {".inputs": model.inputs, ".outputs": model.outputs, ".clock": model.clocks}
        target[tokens[0]].extend(tokens[1:])
        self.cell = None

    def d_names(self, tokens: list[str]) -> None:
        if len(tokens) < 2:
            raise self.error(".names needs an output net")
        self.add_cell(Names(inputs=tokens[1:-1], output=tokens[-1], line=self.line))

    def d_latch(self, tokens: list[str]) -> None:
        args = tokens[1:]
        if len(args) not in (2, 3, 4, 5):
            raise self.error(".latch takes 2 to 5 arguments")
        latch = Latch(input=args[0], output=args[1], line=self.line)
        if len(args) in (3, 5):
            latch.init = args[-1]
            if latch.init not in LATCH_INITS:
                raise self.error(f"bad .latch init value {latch.init!r}")
        if len(args) >= 4:
            latch.ltype, latch.control = args[2], args[3]
            if latch.ltype not in LATCH_TYPES:
                raise self.error(f"bad .latch type {latch.ltype!r}")
        self.add_cell(latch)

    def d_subckt(self, tokens: list[str]) -> None:
        if len(tokens) < 2:
            raise self.error(".subckt needs a model name")
        conns: list[tuple[str, str]] = []
        for tok in tokens[2:]:
            formal, eq, actual = tok.partition("=")
            if not eq or not formal or not actual:
                raise self.error(f"bad .subckt connection {tok!r}")
            conns.append((formal, actual))
        self.add_cell(Subckt(model=tokens[1], conns=conns, line=self.line))

    def d_attr(self, tokens: list[str]) -> None:
        if self.cell is None:
            raise self.error(f"{tokens[0]} must follow a .names, .latch or .subckt")
        self.cell.attrs.append((tokens[0], " ".join(tokens[1:])))

    def d_blackbox(self, tokens: list[str]) -> None:
        if len(tokens) != 1:
            raise self.error(".blackbox takes no arguments")
        self.need_model(".blackbox").blackbox = True
        self.cell = None

    def d_end(self, tokens: list[str]) -> None:
        self.need_model(".end")
        self.model = None
        self.cell = None

    # -- cover rows -----------------------------------------------------------

    def cover_row(self, tokens: list[str]) -> None:
        cell = self.cell
        if not isinstance(cell, Names) or cell.attrs:
            raise self.error(f"unexpected line starting with {tokens[0]!r}")
        if not cell.inputs:
            if len(tokens) != 1:
                raise self.error("constant .names row must be a single 0 or 1")
            plane, out = "", tokens[0]
        else:
            if len(tokens) != 2:
                raise self.error("cover row must be '<input plane> <output bit>'")
            plane, out = tokens
            if len(plane) != len(cell.inputs) or not _PLANE_RE.fullmatch(plane):
                raise self.error(f"cover row {plane!r} does not match {len(cell.inputs)} inputs")
        if out not in ("0", "1"):
            raise self.error(f"cover output bit must be 0 or 1, got {out!r}")
        if cell.rows and cell.rows[0][1] != out:
            raise self.error("cover mixes on-set and off-set rows")
        cell.rows.append((plane, out))


_HANDLERS = {
    ".model": _Parser.d_model,
    ".inputs": _Parser.d_ports,
    ".outputs": _Parser.d_ports,
    ".clock": _Parser.d_ports,
    ".names": _Parser.d_names,
    ".latch": _Parser.d_latch,
    ".subckt": _Parser.d_subckt,
    ".cname": _Parser.d_attr,
    ".attr": _Parser.d_attr,
    ".param": _Parser.d_attr,
    ".blackbox": _Parser.d_blackbox,
    ".end": _Parser.d_end,
}


# --------------------------------------------------------------------------- validation


def _err(path: str, line: int, msg: str) -> BlifError:
    return BlifError(f"{path}:{line}: {msg}")


def _check_subckt(path: str, cell: Subckt, models: dict[str, Model]) -> None:
    sub = models.get(cell.model)
    if sub is None:
        raise _err(path, cell.line, f".subckt of undefined model {cell.model!r}")
    ports = set(sub.inputs) | set(sub.outputs) | set(sub.clocks)
    seen: set[str] = set()
    for formal, _ in cell.conns:
        if formal not in ports:
            raise _err(path, cell.line, f"model {cell.model!r} has no port {formal!r}")
        if formal in seen:
            raise _err(path, cell.line, f"port {formal!r} connected twice")
        seen.add(formal)


def _check_model(path: str, model: Model, models: dict[str, Model]) -> None:
    if model.blackbox and model.cells:
        raise _err(path, model.line, f"black-box model {model.name!r} contains cells")
    drivers: dict[str, int] = {n: model.line for n in model.inputs + model.clocks}
    for cell in model.cells:
        if isinstance(cell, Subckt):
            _check_subckt(path, cell, models)
        for net in cell_io(cell, models)[1]:
            if net in drivers:
                raise _err(path, cell.line, f"net {net!r} has more than one driver")
            drivers[net] = cell.line


def validate(netlist: Netlist) -> None:
    """Check model-name uniqueness, subckt references and single drivers.  Raises BlifError."""
    if not netlist.models:
        raise BlifError(f"{netlist.path}:1: no .model found")
    models: dict[str, Model] = {}
    for model in netlist.models:
        if model.name in models:
            raise _err(netlist.path, model.line, f"duplicate model {model.name!r}")
        models[model.name] = model
    for model in netlist.models:
        _check_model(netlist.path, model, models)


def parse_text(text: str, path: str = "<string>") -> Netlist:
    """Parse and validate BLIF text."""
    parser = _Parser(path)
    for lineno, tokens in logical_lines(text):
        parser.feed(lineno, tokens)
    netlist = Netlist(path=path, models=parser.models)
    validate(netlist)
    return netlist


def parse_file(path: str) -> Netlist:
    """Parse and validate a BLIF file.  I/O and decode errors become BlifError."""
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        raise BlifError(f"{path}: cannot read: {exc}") from exc
    return parse_text(text, path)
