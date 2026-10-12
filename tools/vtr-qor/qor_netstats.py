"""qor_netstats: netlist statistics from a pre-VPR BLIF, and the ``# abc:`` header (3H spec QH-4.1).

``netstats`` reads a ``.blif`` or ``.blif.gz`` file in one streaming pass for the counts and then
runs Kahn's algorithm over the LUT graph for the depth. It never holds the file in memory: only
the nets (interned to integers) and the gates (flat ``array`` columns) are kept, so a netlist the
size of LU32PEEng's (380 k ``.names``) fits easily in the heavy scope. There is no recursion.

``abc_header`` reads the run information that ``write_blif --vtr`` (3E, spec §5 item 1) copies
from the ``abc`` pass (3D, spec §8.5) into a ``# abc:`` comment before ``.model``.
"""

from __future__ import annotations

import gzip
import hashlib
import io
import re
import sys
from array import array
from collections.abc import Callable, Generator
from dataclasses import dataclass
from pathlib import Path

# Hard-block models counted by ``.subckt`` (spec §4.1); a model not listed here is not counted.
ADDER = "adder"
MULTIPLY = "multiply"
SINGLE_PORT_RAM = "single_port_ram"
DUAL_PORT_RAM = "dual_port_ram"
GZIP_SUFFIX = ".gz"
CHUNK = 1 << 20
IDENTITY_ROWS = (("1", "1"), ("0", "0"))
HEADER_PREFIX = "# abc: "
LOOP_DEPTH = -1
LUT_LEVEL = 1
BUFFER_LEVEL = 0
# BLIF is read as latin-1 so that no name can fail to decode; the header is UTF-8 (a script path).
BLIF_ENCODING = "latin-1"

_NUMBER = re.compile(r"-?[0-9]+", re.ASCII)


@dataclass(frozen=True)
class NetStats:
    """The §4.1 metrics of one netlist. ``depth`` is -1 on a combinational loop."""

    luts: int
    buffers: int
    ffs: int
    adders: int
    mults: int
    ram_slices: int
    depth: int
    blif_sha256: str


@dataclass(frozen=True)
class AbcHeader:
    """The last ``# abc:`` line before ``.model``, one field per key (``k`` is the ``K`` word).

    A word that the line does not carry is the empty string; ``exchange`` and ``depth`` are
    required (``abc_header`` raises ``ValueError`` without them).
    """

    script: str
    sha256: str
    abc: str
    k: str
    exchange: str
    depth: int


def _open(path: Path) -> io.BufferedIOBase:
    if path.suffix == GZIP_SUFFIX:
        return gzip.open(path, "rb")
    return path.open("rb")


# ----------------------------------------------------------------------------------------------
# The graph


class _Graph:
    """Nets as integers and gates as flat columns; the depth is a Kahn pass over the nets.

    A gate is a ``.names`` with at least one input: ``weight`` 1 for a LUT, 0 for an identity
    buffer. A net no gate drives is a source (primary input, latch output, hard-block output,
    constant), so no source needs recording. ``sink`` marks the nets a path may end on: primary
    outputs, latch inputs and every net on a hard-block pin (an output pin's net is undriven, so
    its level is 0 and marking it changes nothing).
    """

    def __init__(self) -> None:
        self.ids: dict[str, int] = {}
        self.sink = bytearray()
        self.out = array("I")
        self.weight = bytearray()
        self.start = array("I", [0])
        self.ins = array("I")

    def net(self, name: str) -> int:
        found = self.ids.get(name)
        if found is not None:
            return found
        new = len(self.ids)
        self.ids[name] = new
        self.sink.append(0)
        return new

    def mark_sink(self, name: str) -> None:
        self.sink[self.net(name)] = 1

    def add_gate(self, inputs: list[str], output: str, weight: int) -> None:
        for name in inputs:
            self.ins.append(self.net(name))
        self.out.append(self.net(output))
        self.weight.append(weight)
        self.start.append(len(self.ins))

    def _consumers(self) -> tuple[array[int], array[int]]:
        """CSR of the gates that read each net, one entry per input occurrence."""
        nets = len(self.ids)
        offset = array("I", [0]) * (nets + 1)
        for net in self.ins:
            offset[net + 1] += 1
        for i in range(nets):
            offset[i + 1] += offset[i]
        fill = array("I", offset)
        gates = array("I", [0]) * len(self.ins)
        for gate in range(len(self.out)):
            for k in range(self.start[gate], self.start[gate + 1]):
                net = self.ins[k]
                gates[fill[net]] = gate
                fill[net] += 1
        return offset, gates

    def depth(self) -> int:
        """The longest weighted path to a sink, or -1 when a loop keeps a net from resolving."""
        nets = len(self.ids)
        offset, gates = self._consumers()
        pending = array("I", [0]) * nets
        for gate, net in enumerate(self.out):
            pending[net] += self.start[gate + 1] - self.start[gate]
        level = array("I", [0]) * nets
        ready = [net for net in range(nets) if pending[net] == 0]
        resolved = 0
        while ready:
            net = ready.pop()
            resolved += 1
            for k in range(offset[net], offset[net + 1]):
                gate = gates[k]
                out = self.out[gate]
                reach = level[net] + self.weight[gate]
                if reach > level[out]:
                    level[out] = reach
                pending[out] -= 1
                if pending[out] == 0:
                    ready.append(out)
        if resolved < nets:
            return LOOP_DEPTH
        return max((level[net] for net in range(nets) if self.sink[net]), default=0)


# ----------------------------------------------------------------------------------------------
# The streaming reader


def _logical_lines(
    raw: io.BufferedIOBase, update: Callable[[bytes], object]
) -> Generator[list[str], None, None]:
    """Yield each logical line's blank-separated words, hashing every byte read.

    Comments are cut per physical line; a line ending in ``\\`` is joined to the next. Blank and
    comment-only lines yield nothing. Stops reading when the consumer stops iterating.
    """
    words: list[str] = []
    for chunk in raw:
        update(chunk)
        text = chunk.decode(BLIF_ENCODING)
        cut = text.find("#")
        if cut >= 0:
            text = text[:cut]
        text = text.rstrip()
        continued = text.endswith("\\")
        if continued:
            text = text[:-1]
        words.extend(text.split())
        if not continued and words:
            yield words
            words = []
    if words:
        yield words


class _Scan:
    """The counters and the graph for the first ``.model``; feed it one logical line at a time."""

    def __init__(self) -> None:
        self.graph = _Graph()
        self.luts = 0
        self.buffers = 0
        self.ffs = 0
        self.hard = {ADDER: 0, MULTIPLY: 0, SINGLE_PORT_RAM: 0, DUAL_PORT_RAM: 0}
        self._names: list[str] | None = None
        self._rows = 0
        self._first: tuple[str, ...] = ()

    def feed(self, words: list[str]) -> bool:
        """Take one logical line; return False once the first model is over."""
        head = words[0]
        if not head.startswith("."):
            self._cover_row(words)
            return True
        self._close_names()
        if head in (".end", ".exdc", ".model"):
            return False
        if head == ".names":
            self._names = words[1:]
        elif head == ".latch":
            self._latch(words)
        elif head == ".subckt":
            self._subckt(words)
        elif head == ".outputs":
            for name in words[1:]:
                self.graph.mark_sink(name)
        return True

    def finish(self) -> None:
        self._close_names()

    def _cover_row(self, words: list[str]) -> None:
        if self._names is None:
            return
        if self._rows == 0:
            self._first = tuple(words)
        self._rows += 1

    def _close_names(self) -> None:
        names, rows, first = self._names, self._rows, self._first
        self._names, self._rows, self._first = None, 0, ()
        if names is None or len(names) < 2:
            return
        inputs, output = names[:-1], names[-1]
        if len(inputs) == 1 and rows == 1 and first in IDENTITY_ROWS:
            self.buffers += 1
            self.graph.add_gate(inputs, output, BUFFER_LEVEL)
        else:
            self.luts += 1
            self.graph.add_gate(inputs, output, LUT_LEVEL)

    def _latch(self, words: list[str]) -> None:
        self.ffs += 1
        if len(words) > 1:
            self.graph.mark_sink(words[1])

    def _subckt(self, words: list[str]) -> None:
        if len(words) > 1 and words[1] in self.hard:
            self.hard[words[1]] += 1
        for pin in words[2:]:
            _, eq, net = pin.partition("=")
            if eq:
                self.graph.mark_sink(net)


def netstats(path: Path) -> NetStats:
    """Count and measure the first ``.model`` of ``path`` (``.blif`` or ``.blif.gz``).

    ``blif_sha256`` is that of the uncompressed bytes of the whole file. The stubs after the first
    ``.end`` are hashed but not read. On a combinational loop the depth is -1 and
    ``vtr-qor: netstats: <path>: combinational loop; depth -1`` goes to stderr.
    """
    digest = hashlib.sha256()
    scan = _Scan()
    with _open(path) as raw:
        lines = _logical_lines(raw, digest.update)
        in_model = False
        for words in lines:
            if not in_model:
                in_model = words[0] == ".model"
            elif not scan.feed(words):
                break
        lines.close()
        scan.finish()
        while chunk := raw.read(CHUNK):
            digest.update(chunk)
    depth = scan.graph.depth()
    if depth == LOOP_DEPTH:
        print(f"vtr-qor: netstats: {path}: combinational loop; depth -1", file=sys.stderr)
    return NetStats(
        luts=scan.luts,
        buffers=scan.buffers,
        ffs=scan.ffs,
        adders=scan.hard[ADDER],
        mults=scan.hard[MULTIPLY],
        ram_slices=scan.hard[SINGLE_PORT_RAM] + scan.hard[DUAL_PORT_RAM],
        depth=depth,
        blif_sha256=digest.hexdigest(),
    )


# ----------------------------------------------------------------------------------------------
# The ``# abc:`` header


def _quoted(text: str, at: int, path: Path) -> tuple[str, int]:
    """Read a double-quoted value that starts after the quote at ``at``; return it and the end."""
    chars: list[str] = []
    i = at
    while i < len(text):
        c = text[i]
        if c == '"':
            return "".join(chars), i + 1
        if c == "\\":
            if i + 1 >= len(text) or text[i + 1] not in '"\\':
                break
            i += 1
            c = text[i]
        chars.append(c)
        i += 1
    raise ValueError(f"{path}: '# abc:' line with a malformed quoted value")


def _words(text: str, path: Path) -> dict[str, str]:
    """Parse blank-separated ``key=value`` words; a value may be in double quotes (3D §8.5)."""
    found: dict[str, str] = {}
    i = 0
    while i < len(text):
        if text[i].isspace():
            i += 1
            continue
        eq = i
        while eq < len(text) and text[eq] != "=" and not text[eq].isspace():
            eq += 1
        if eq >= len(text) or text[eq] != "=" or eq == i:
            raise ValueError(f"{path}: '# abc:' line with a word that is not key=value")
        key = text[i:eq]
        if eq + 1 < len(text) and text[eq + 1] == '"':
            value, i = _quoted(text, eq + 2, path)
            if i < len(text) and not text[i].isspace():
                raise ValueError(f"{path}: '# abc:' line with a malformed quoted value")
        else:
            i = eq + 1
            while i < len(text) and not text[i].isspace():
                if text[i] in '"\\':
                    raise ValueError(f"{path}: '# abc:' line with a malformed value for {key}=")
                i += 1
            value = text[eq + 1 : i]
        found[key] = value
    return found


def _last_abc_line(path: Path) -> str | None:
    """The last ``# abc: `` line before the first line that is neither blank nor a comment."""
    last: str | None = None
    with _open(path) as raw:
        for chunk in raw:
            text = chunk.decode("utf-8", errors="replace").rstrip("\r\n")
            if text.startswith(HEADER_PREFIX):
                last = text[len(HEADER_PREFIX) :]
            elif text.strip() and not text.lstrip().startswith("#"):
                break
    return last


def abc_header(path: Path) -> AbcHeader | None:
    """Parse the last ``# abc: `` line before ``.model``; ``None`` when there is none.

    Raises ``ValueError`` when that line has no ``depth=`` or ``exchange=`` (or a non-integer
    depth), or a word that is not ``key=value``. A word the line does not carry is "" in the result.
    """
    line = _last_abc_line(path)
    if line is None:
        return None
    words = _words(line, path)
    depth = words.get("depth", "")
    exchange = words.get("exchange", "")
    if not depth or not exchange:
        raise ValueError(f"{path}: '# abc:' line without depth= or exchange=")
    if not _NUMBER.fullmatch(depth):
        raise ValueError(f"{path}: '# abc:' line with a depth= that is not an integer")
    return AbcHeader(
        script=words.get("script", ""),
        sha256=words.get("sha256", ""),
        abc=words.get("abc", ""),
        k=words.get("K", ""),
        exchange=exchange,
        depth=int(depth),
    )
