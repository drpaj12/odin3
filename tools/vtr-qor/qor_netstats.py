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
import zlib
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
IDENTITY_ROWS = (("1", "1"), ("0", "0"))
HEADER_PREFIX = "# abc: "
LOOP_DEPTH = -1
LUT_LEVEL = 1
BUFFER_LEVEL = 0
# BLIF is read as latin-1 so that no name can fail to decode; the header is UTF-8 (a script path).
BLIF_ENCODING = "latin-1"

_COUNT = re.compile(r"[0-9]+", re.ASCII)
_HEX64 = re.compile(r"[0-9a-fA-F]{64}", re.ASCII)
_HEX40 = re.compile(r"[0-9a-fA-F]{40}", re.ASCII)
_K = re.compile(r"[0-9]+|none", re.ASCII)
_EXCHANGE = re.compile(r"aig|blif", re.ASCII)
# What a damaged gzip stream raises: a short stream, a bad header, bad compressed data.
GZIP_ERRORS = (EOFError, gzip.BadGzipFile, zlib.error)


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


class MalformedBlif(ValueError):
    """The file is not a complete BLIF: a truncated write, plain text, or a damaged ``.gz``."""


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
        self.driven = bytearray()
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
        self.driven.append(0)
        return new

    def mark_sink(self, name: str) -> None:
        self.sink[self.net(name)] = 1

    def drive(self, name: str) -> int:
        """Record a driver (``.names`` or latch output) of ``name``; a second one is malformed."""
        net = self.net(name)
        if self.driven[net]:
            raise MalformedBlif(f"net '{name}' has two drivers")
        self.driven[net] = 1
        return net

    def add_gate(self, inputs: list[str], output: str, weight: int) -> None:
        for name in inputs:
            self.ins.append(self.net(name))
        self.out.append(self.drive(output))
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
    """The counters and the graph for the first ``.model``; feed it one logical line at a time.

    It also tracks every model's ``.model`` and ``.end`` (the stubs after the first are not
    counted), so a file that stops before an ``.end`` is malformed.
    """

    def __init__(self) -> None:
        self.graph = _Graph()
        self.luts = 0
        self.buffers = 0
        self.ffs = 0
        self.hard = {ADDER: 0, MULTIPLY: 0, SINGLE_PORT_RAM: 0, DUAL_PORT_RAM: 0}
        self.models = 0
        self.open = False
        self._exdc = False
        self._names: list[str] | None = None
        self._rows = 0
        self._first: tuple[str, ...] = ()

    def feed(self, words: list[str]) -> None:
        """Take one logical line of the file."""
        head = words[0]
        if head == ".model":
            if self.open:
                raise MalformedBlif(".model inside a model (the previous one has no .end)")
            self.open = True
            self.models += 1
        elif head == ".end":
            if not self.open:
                raise MalformedBlif(".end outside a model")
            self._close_names()
            self.open = False
        elif self.open and self.models == 1 and not self._exdc:
            self._count(head, words)

    def finish(self) -> None:
        if self.models == 0:
            raise MalformedBlif("no .model")
        if self.open:
            raise MalformedBlif("a model has no .end (truncated file?)")

    def _count(self, head: str, words: list[str]) -> None:
        if not head.startswith("."):
            self._cover_row(words)
            return
        self._close_names()
        if head == ".exdc":
            self._exdc = True
        elif head == ".names":
            if len(words) < 2:
                raise MalformedBlif(".names without an output")
            self._names = words[1:]
        elif head == ".latch":
            self._latch(words)
        elif head == ".subckt":
            self._subckt(words)
        elif head == ".outputs":
            for name in words[1:]:
                self.graph.mark_sink(name)

    def _cover_row(self, words: list[str]) -> None:
        if self._names is None:
            raise MalformedBlif(f"a cover row outside .names: {' '.join(words)}")
        width = len(self._names) - 1
        plane = words[0] if width else ""
        if (
            len(words) != (2 if width else 1)
            or len(plane) != width
            or any(c not in "01-" for c in plane)
            or words[-1] not in ("0", "1")
        ):
            raise MalformedBlif(f"a cover row that does not fit its .names: {' '.join(words)}")
        if self._rows == 0:
            self._first = tuple(words)
        self._rows += 1

    def _close_names(self) -> None:
        names, rows, first = self._names, self._rows, self._first
        self._names, self._rows, self._first = None, 0, ()
        if names is None:
            return
        inputs, output = names[:-1], names[-1]
        if not inputs:
            self.graph.drive(output)
        elif len(inputs) == 1 and rows == 1 and first in IDENTITY_ROWS:
            self.buffers += 1
            self.graph.add_gate(inputs, output, BUFFER_LEVEL)
        else:
            self.luts += 1
            self.graph.add_gate(inputs, output, LUT_LEVEL)

    def _latch(self, words: list[str]) -> None:
        self.ffs += 1
        if len(words) < 3:
            raise MalformedBlif(".latch without an input and an output")
        self.graph.mark_sink(words[1])
        self.graph.drive(words[2])

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
    ``.end`` are hashed and checked for ``.model``/``.end`` pairing, not counted. On a
    combinational loop the depth is -1 and ``vtr-qor: netstats: <path>: combinational loop; depth
    -1`` goes to stderr.

    Raises ``MalformedBlif`` (a ``ValueError``) for a file with no ``.model``, a model with no
    ``.end``, a net with two drivers, a cover row that does not fit its ``.names``, or a truncated
    or corrupt ``.gz``; a missing or unreadable file raises ``OSError``.
    """
    digest = hashlib.sha256()
    scan = _Scan()
    try:
        with _open(path) as raw:
            for words in _logical_lines(raw, digest.update):
                scan.feed(words)
        scan.finish()
    except MalformedBlif as exc:
        raise MalformedBlif(f"{path}: malformed BLIF: {exc}") from None
    except GZIP_ERRORS as exc:
        raise MalformedBlif(f"{path}: malformed BLIF: truncated or corrupt gzip ({exc})") from None
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
    try:
        with _open(path) as raw:
            for chunk in raw:
                text = chunk.decode("utf-8", errors="replace").rstrip("\r\n")
                if text.startswith(HEADER_PREFIX):
                    last = text[len(HEADER_PREFIX) :]
                elif text.strip() and not text.lstrip().startswith("#"):
                    break
    except GZIP_ERRORS as exc:
        raise MalformedBlif(f"{path}: malformed BLIF: truncated or corrupt gzip ({exc})") from None
    return last


def _checked(path: Path, key: str, value: str, pattern: re.Pattern[str]) -> str:
    """``value`` when it has the form 3D §8.5 gives ``key`` (or is absent); else ``ValueError``."""
    if value and pattern.fullmatch(value) is None:
        raise ValueError(f"{path}: '# abc:' line with an invalid {key}= ({value!r})")
    return value


def abc_header(path: Path) -> AbcHeader | None:
    """Parse the last ``# abc: `` line before ``.model``; ``None`` when there is none.

    Raises ``ValueError`` when that line has no ``depth=`` or ``exchange=``, a word that is not
    ``key=value``, or a value outside 3D §8.5's forms: ``sha256`` 64 hex, ``abc`` 40 hex, ``K`` an
    integer or ``none``, ``exchange`` ``aig`` or ``blif``, ``depth`` a non-negative integer. A word
    the line does not carry is "" in the result; unknown keys are ignored.
    """
    line = _last_abc_line(path)
    if line is None:
        return None
    words = _words(line, path)
    depth = words.get("depth", "")
    exchange = words.get("exchange", "")
    if not depth or not exchange:
        raise ValueError(f"{path}: '# abc:' line without depth= or exchange=")
    _checked(path, "depth", depth, _COUNT)
    return AbcHeader(
        script=words.get("script", ""),
        sha256=_checked(path, "sha256", words.get("sha256", ""), _HEX64),
        abc=_checked(path, "abc", words.get("abc", ""), _HEX40),
        k=_checked(path, "K", words.get("K", ""), _K),
        exchange=_checked(path, "exchange", exchange, _EXCHANGE),
        depth=int(depth),
    )
