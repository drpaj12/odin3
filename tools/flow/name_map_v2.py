"""name_map_v2: a standard-library reader for the Odin III name map, versions 1 and 2
(V1.2.0 Task 7; docs/V1.2.0.md 3.2 and 3.5).

``odin3.read_name_map`` (plugins/python/odin3.py) needs the cffi package at import time, and the
flow tools in ``tools/flow`` are stdlib-only, so this module carries a copy of its parse. The two
must agree: tests/tools/test_name_map_v2.py parses every micro map with both and compares the
objects, records, constructs, files, runs and atoms, and the warnings. A change to the parse in
either file is made in both.

``read_name_map(path)`` restores nothing: it returns a ``NameMap`` (objects, records,
constructs, files, runs, atoms). Lines it cannot use (too long, not UTF-8, ending in CR,
malformed, an unknown type) are skipped with a warning naming path:line and counted in
``NameMap.ignored``, as the C parse of the ``names`` pass does. ``ValueError`` for a bad header
or a file over 1 GiB; ``OSError`` when the file cannot be read.

On top of the parse: ``NameMap.atom(name)`` (the object an atom of the BLIF VPR read is, by its
short name; spec 6, "Atom name join"), ``NameMap.leaf(obj)`` (the first SOURCE leaf with its
location) and ``NameMap.chain(obj)`` (the construct chain, leaf first).
"""

from __future__ import annotations

import logging
import re
from dataclasses import dataclass, field
from pathlib import Path

LOG = logging.getLogger("odin3")

_MAP_ESCAPES_V1 = {"%09": "\t", "%0A": "\n", "%5C": "\\", "%25": "%", "%23": "#"}
_MAP_ESCAPES = {**_MAP_ESCAPES_V1, "%2C": ",", "%7C": "|"}
_MAP_MIN_FIELDS = {"O": 9, "N": 5, "R": 9, "C": 7, "F": 2, "T": 4, "A": 3}
_MAP_MAX_FIELDS = 12  # the fields the C parse keeps (ODIN3_NM2_MAX_FIELDS)
_MAP_MAX_LINE = 1 << 20  # the longest line kept, bytes (ODIN3_NAME_MAP_MAX_LINE); longer: ignored
_MAP_MAX_FILE = 1 << 30  # the largest map file read, bytes (the C `names` pass has the same cap)
_MAP_KINDS = ("net", "node", "wire")
_MAP_DECIMAL = re.compile(r"[0-9]{1,10}")  # as the C parse_decimal: ASCII digits, at most 10
_MAP_UINT32_MAX = 0xFFFFFFFF
_MAP_HEADER = re.compile(
    r"# odin3-names (?P<version>[12]) hash=sha256 width=(?P<width>\d{1,2})"
    r"(?: prov=1 ast=(?P<ast>refs|constructs|full|none))?")


def _map_unescape(text: str, v2: bool = True) -> str:
    """The text of a name map field with its escapes undone: %09 %0A %5C %25 %23 and, in a
    version 2 map, %2C and %7C."""
    table = _MAP_ESCAPES if v2 else _MAP_ESCAPES_V1

    def one(match: re.Match[str]) -> str:
        try:
            return table[match.group(0)]
        except KeyError:
            raise ValueError(f"bad escape '{match.group(0)}'") from None

    return re.sub(r"%.{0,2}", one, text)


def _map_decimal(text: str) -> int:
    """A decimal number as the C parse reads it: 1 to 10 ASCII digits, at most 2**32 - 1."""
    if _MAP_DECIMAL.fullmatch(text) is None or int(text) > _MAP_UINT32_MAX:
        raise ValueError(f"'{text}' is not a decimal number")
    return int(text)


def _map_ref(text: str, prefix: str) -> int:
    """The number in a reference like r12 (0 for an empty field)."""
    if not text:
        return 0
    number = _map_decimal(text[1:]) if text[0] == prefix else 0
    if number == 0:
        raise ValueError(f"'{text}' is not a {prefix}<n> reference")
    return number


def _map_refs(field: str, prefix: str, sep: str = ",") -> tuple[int, ...]:
    """The numbers in a reference list. Only a wholly empty field means no references. An empty
    element of a longer list is kept as 0 (it keeps the places of the others) and every walk
    skips it, as the C walk does: the C parse does not ignore such a line, `names --check` counts
    it as a reference that does not resolve."""
    return tuple(_map_ref(item, prefix) for item in field.split(sep)) if field else ()


@dataclass(frozen=True)
class MapObject:
    """An O or N line: an object of a netlist and its provenance record. type is O (renamed by a
    hash: name is the hash name and full its full name) or N (kept its name; full is empty);
    kind is net, node or wire; records are record numbers (Odin III writes one); level is the
    granularity of the record. Version 1 maps give O objects without records. raw is the line
    as the file holds it."""

    type: str
    module: str
    kind: str
    name: str
    full: str
    hier: str
    file: str
    line: int
    records: tuple[int, ...]
    level: str
    raw: str


@dataclass(frozen=True)
class MapRecord:
    """An R line: a provenance record. kind is S (source), I (imported) or D (derived); dir is
    origin or <refine|rewrite|coarsen|context>:<from level>; step is t<n>.<op>; parents and
    constructs are record and construct numbers; origins are f<n>:span texts; flags are the
    comma-separated flags (hier=... unescaped)."""

    number: int
    kind: str
    level: str
    dir: str
    step: str
    parents: tuple[int, ...]
    origins: tuple[str, ...]
    constructs: tuple[int, ...]
    flags: tuple[str, ...]
    raw: str


@dataclass(frozen=True)
class MapConstruct:
    """A C line: a construct of the AST layer. parent is a construct number (0 for a module) and
    file a file number (0 when empty)."""

    number: int
    kind: str
    level: str
    parent: int
    file: int
    span: str
    name: str
    raw: str


@dataclass(frozen=True)
class SourceSpan:
    """A location ``file:L.C-EL.EC`` of a record or a construct (columns as the map writes
    them)."""

    file: str
    line: int
    col: int
    end_line: int
    end_col: int

    @property
    def text(self) -> str:
        return f"{self.file}:{self.line}.{self.col}-{self.end_line}.{self.end_col}"

    @property
    def short(self) -> str:
        """``file:line``, the line-level span of version 1."""
        return f"{self.file}:{self.line}"


_MAP_SPAN = re.compile(r"(?:f(?P<f>[0-9]{1,10}):)?(?P<l>\d+)\.(?P<c>\d+)-(?P<el>\d+)\.(?P<ec>\d+)")


def span_of(text: str, files: list[str], file_number: int = 0) -> SourceSpan | None:
    """``f<n>:L.C-EL.EC`` (an origin), or ``L.C-EL.EC`` of the file ``file_number`` (a construct),
    as a SourceSpan; None when the text or the file number is not usable."""
    match = _MAP_SPAN.fullmatch(text)
    if match is None:
        return None
    number = int(match["f"]) if match["f"] else file_number
    if not 1 <= number <= len(files):
        return None
    return SourceSpan(files[number - 1], int(match["l"]), int(match["c"]), int(match["el"]),
                      int(match["ec"]))


@dataclass
class NameMap:
    """A parsed name map. version is 1 or 2; ast is the header's ast word (version 2). Lines of
    a type this reader does not know are skipped with one warning per type; A lines are kept
    unparsed in atoms (fields after the type letter). Hash names are not verified here
    (`names --check` does)."""

    version: int
    width: int
    ast: str
    objects: list[MapObject] = field(default_factory=list)
    records: list[MapRecord] = field(default_factory=list)
    constructs: list[MapConstruct] = field(default_factory=list)
    files: list[str] = field(default_factory=list)
    runs: list[tuple[str, str, str]] = field(default_factory=list)
    atoms: list[tuple[str, ...]] = field(default_factory=list)
    ignored: int = 0
    _index: dict[str, list[MapObject]] = field(default_factory=dict, init=False, repr=False,
                                               compare=False)
    _indexed: int = field(default=-1, init=False, repr=False, compare=False)

    def find(self, name: str) -> list[MapObject]:
        """The O lines whose hash name, and the N lines whose name, is name, in file order. The
        first call builds an index by name (rebuilt when objects has grown or shrunk since)."""
        if self._indexed != len(self.objects):
            self._index = {}
            for obj in self.objects:
                self._index.setdefault(obj.name, []).append(obj)
            self._indexed = len(self.objects)
        return list(self._index.get(name, ()))

    def provenance(self, obj: MapObject) -> list[MapRecord]:
        """The records of obj, as Node.provenance(): its naming record first, then every record
        reachable through parents, depth first in parent order, each once."""
        records: list[MapRecord] = []
        seen: set[int] = set()
        stack = list(obj.records[:1])
        while stack:
            number = stack.pop()
            if number in seen or not 1 <= number <= len(self.records):
                continue
            seen.add(number)
            record = self.records[number - 1]
            records.append(record)
            stack.extend(reversed(record.parents))
        return records

    def chain(self, obj: MapObject) -> list[MapConstruct]:
        """The HDL constructs of obj, as Node.chain(): for each source leaf of provenance() that
        has a construct, the construct and then its ancestors up to the module, each once."""
        chain: list[MapConstruct] = []
        reached: set[int] = set()
        for record in self.provenance(obj):
            number = record.constructs[0] if record.kind != "D" and record.constructs else 0
            for _ in range(len(self.constructs)):
                if number in reached or not 1 <= number <= len(self.constructs):
                    break
                reached.add(number)
                construct = self.constructs[number - 1]
                chain.append(construct)
                number = construct.parent
        return chain

    # --- the flow's own additions (not in odin3.read_name_map) ---------------------------------

    def atom(self, name: str, module: str | None = None) -> MapObject | None:
        """The object a BLIF net or atom ``name`` is: the O or N line keyed by that name (spec 6,
        "Atom name join"). A name that several modules share is decided by ``module`` when given,
        else the first net, else the first object in file order. None when no line has it."""
        found = self.find(name)
        if module is not None:
            found = [o for o in found if o.module == module] or found
        nets = [o for o in found if o.kind == "net"]
        pick = nets or found
        return pick[0] if pick else None

    def location(self, record: MapRecord) -> SourceSpan | None:
        """The first origin of ``record`` as a SourceSpan, None when it has none or cannot be read
        (a file number the map does not hold, or text that is not f<n>:L.C-EL.EC)."""
        for origin in record.origins:
            span = span_of(origin, self.files)
            if span is not None:
                return span
        return None

    def leaf(self, obj: MapObject) -> tuple[MapRecord, SourceSpan] | None:
        """The first SOURCE or IMPORTED record of ``provenance(obj)`` (depth first in parent
        order, the order of the hash name's first leaf, spec 3.5) that has a location."""
        for record in self.provenance(obj):
            if record.kind != "D":
                span = self.location(record)
                if span is not None:
                    return record, span
        return None


def _map_header(line: str, path: str) -> NameMap:
    match = _MAP_HEADER.fullmatch(line)
    if match is None or not 1 <= int(match["width"]) <= 52 or (
            (match["version"] == "2") != (match["ast"] is not None)):
        raise ValueError(f"{path}:1: not a name map header: '{line}'")
    return NameMap(int(match["version"]), int(match["width"]), match["ast"] or "")


def _map_object_o(fields: list[str], text: str, v2: bool) -> MapObject:
    """The O line (version 1: the seven fields, no records) as the C `build_entry` reads it: the
    module, short, full, hier and file fields are unescaped, kind and line are not."""
    esc = [_map_unescape(item, v2) for item in fields[:6]]
    if fields[1] not in _MAP_KINDS:
        raise ValueError("unknown kind (want net, node or wire)")
    line = _map_decimal(fields[6])
    if not v2:
        return MapObject("O", esc[0], fields[1], esc[2], esc[3], esc[4], esc[5], line, (), "", text)
    return MapObject("O", esc[0], fields[1], esc[2], esc[3], esc[4], esc[5], line,
                     _map_refs(fields[7], "r"), _map_unescape(fields[8]), text)


def _map_line_v1(nm: NameMap, text: str) -> None:
    """Adds one version 1 line (seven fields: module kind short full hier file line)."""
    fields = text.split("\t")
    if len(fields) != 7:
        raise ValueError("not 7 fields")
    nm.objects.append(_map_object_o(fields, text, False))


def _map_line(nm: NameMap, text: str) -> None:
    """Adds one version 2 line (type letter, TAB, fields); ValueError when it is malformed. Only
    the first 12 fields are read, as the C parse keeps no more."""
    kind, fields = text[0], text[2:].split("\t")
    if len(fields) < _MAP_MIN_FIELDS[kind]:
        raise ValueError("too few fields for its type")
    fields = fields[:_MAP_MAX_FIELDS]
    if kind == "O":
        nm.objects.append(_map_object_o(fields, text, True))
        return
    raw = [_map_unescape(item) for item in fields]
    if kind == "N":
        nm.objects.append(MapObject("N", raw[0], raw[1], raw[2], "", "", "", 0,
                                    _map_refs(fields[3], "r"), raw[4], text))
    elif kind == "R":
        nm.records.append(MapRecord(
            _map_ref(fields[0], "r"), raw[1], raw[2], raw[3], raw[4], _map_refs(fields[5], "r"),
            tuple(_map_unescape(item) for item in fields[6].split("|")) if fields[6] else (),
            _map_refs(fields[7], "c"),
            tuple(_map_unescape(item) for item in fields[8].split(",")) if fields[8] else (),
            text))
    elif kind == "C":
        nm.constructs.append(MapConstruct(_map_ref(fields[0], "c"), raw[1], raw[2],
                                          _map_ref(fields[3], "c"), _map_ref(fields[4], "f"),
                                          raw[5], raw[6], text))
    elif kind == "F":
        nm.files.append(raw[1])
    elif kind == "T":
        nm.runs.append((raw[1], raw[2], raw[3]))
    else:
        nm.atoms.append(tuple(raw))


def _map_add(nm: NameMap, raw: bytes, path: str, number: int, warned: set[str]) -> None:
    """Parses one line (bytes, no LF) into nm; a line the reader cannot use is counted in
    nm.ignored with a warning naming path:number."""
    try:
        if len(raw) > _MAP_MAX_LINE:
            raise ValueError("line is too long")
        line = raw.decode("utf-8")
        if line.endswith("\r"):
            raise ValueError("CR at the end of the line (a name map uses LF line ends)")
        if nm.version == 1:
            _map_line_v1(nm, line)
        elif len(line) < 2 or line[1] != "\t":
            raise ValueError("no type letter and TAB at the start of the line")
        elif line[0] in _MAP_MIN_FIELDS:
            _map_line(nm, line)
        else:
            if line[0] not in warned:
                warned.add(line[0])
                LOG.warning("%s:%d: name map line type '%s' is unknown and ignored", path, number,
                            line[0])
            nm.ignored += 1
    except ValueError as err:  # includes UnicodeDecodeError
        LOG.warning("%s:%d: name map entry ignored: %s", path, number, err)
        nm.ignored += 1


def read_name_map(path: str | Path) -> NameMap:
    """Read a name map (version 1 or 2, `<netlist>.names`) from a file. Nothing is restored into
    the IR and no design is needed; lines the reader cannot use (too long, not UTF-8, ending in
    CR, malformed) are skipped with a warning naming path:line and counted in ignored, as the C
    parse does. Raises ValueError (with path:line) for a bad header or a file over 1 GiB and
    OSError when the file cannot be read. See NameMap.provenance() and NameMap.chain()."""
    size = Path(path).stat().st_size
    if size > _MAP_MAX_FILE:
        raise ValueError(f"{path}: the name map is over {_MAP_MAX_FILE} bytes")
    lines = Path(path).read_bytes().split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    if not lines:
        raise ValueError(f"{path}:1: not a name map header: ''")
    try:
        head = lines[0].decode("utf-8")
    except UnicodeDecodeError:
        head = lines[0].decode("utf-8", errors="replace")
    nm = _map_header(head, str(path))
    warned: set[str] = set()
    for number, raw in enumerate(lines[1:], start=2):
        if raw and not raw.startswith(b"#"):
            _map_add(nm, raw, str(path), number, warned)
    return nm
