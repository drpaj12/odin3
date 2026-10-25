"""timing_report: read VPR's ``report_timing.setup.rpt`` (v1.1.1, docs/V1.1.1.md).

The report lists the worst N setup paths of the routed design. Each path has a data-arrival
section of rows ``<atom>.<port>[<bit>] (<block type>) [note]  <incr>  <path>`` between the
``Point`` header and the first ``data arrival time`` line, then a required-time section and a
``slack`` line. ``parse_report`` returns the paths with their data-path rows (the launch clock
network is skipped). Standard library only.
"""

from __future__ import annotations

import re
from collections.abc import Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path

PATH_HEAD = re.compile(r"^#Path\s+(\d+)\s*$")
_START = re.compile(r"^Startpoint:\s*(.*?)\s*$")
_END = re.compile(r"^Endpoint\s*:\s*(.*?)\s*$")
_TYPE = re.compile(r"^Path Type\s*:\s*(\w+)")
_ROW = re.compile(r"^(?P<text>\S.*?)\s+(?P<incr>-?\d+\.\d+)\s+(?P<path>-?\d+\.\d+)\s*$")
_PIN = re.compile(r"^(?P<atom>\S.*)\.(?P<port>[A-Za-z_]\w*)\[(?P<bit>\d+)\] "
                  r"\((?P<btype>[^()]*)\)(?: \[(?P<note>[^\]]*)\])?$")
_TIME = re.compile(r"^(data arrival time|data required time)\s+(-?\d+\.\d+)\s*$")
_SLACK = re.compile(r"^slack \((\w+)\)\s+(-?\d+\.\d+)\s*$")
_FINAL_CPD = re.compile(r"^Final critical path delay \(least slack\): (-?[\d.e+]+) ns"
                        r"(?:, Fmax: ([\d.e+]+) MHz)?", re.M)
OUT_PREFIX = "out:"  # VPR names an output pad "out:<port>"
CLOCK_TO_OUTPUT = "clock-to-output"
INPUT_DELAY = "input external delay"


class ReportError(Exception):
    """A report that is missing or not in the format this reader knows."""


@dataclass(frozen=True)
class Row:
    """One atom pin of the data path. ``incr`` is the increment shown for the row."""

    atom: str
    port: str
    bit: int
    block_type: str
    note: str
    incr: float
    path: float


@dataclass
class TimingPath:
    index: int
    startpoint: str = ""
    endpoint: str = ""
    path_type: str = ""
    arrival_ns: float | None = None
    required_ns: float | None = None
    slack_ns: float | None = None
    slack_state: str = ""
    rows: list[Row] = field(default_factory=list)


def pad_port(atom: str) -> str:
    """The port name of an I/O pad atom (``out:`` prefix removed)."""
    return atom[len(OUT_PREFIX):] if atom.startswith(OUT_PREFIX) else atom


def _row(line: str) -> tuple[str, float, float] | None:
    m = _ROW.match(line)
    return (m.group("text"), float(m.group("incr")), float(m.group("path"))) if m else None


class _PathReader:
    """The state of one path while its lines are read."""

    def __init__(self, index: int) -> None:
        self.path = TimingPath(index)
        self.in_points = False
        self.arrival_done = False
        self.started = False
        self.after_input_delay = False

    def header(self, line: str) -> bool:
        for regex, name in ((_START, "startpoint"), (_END, "endpoint")):
            m = regex.match(line)
            if m:
                setattr(self.path, name, m.group(1))
                return True
        m = _TYPE.match(line)
        if m:
            self.path.path_type = m.group(1)
            return True
        return False

    def times(self, line: str) -> bool:
        m = _TIME.match(line)
        if m:
            if m.group(1) == "data arrival time":
                if self.path.arrival_ns is None:
                    self.path.arrival_ns = float(m.group(2))
                self.arrival_done = True
            elif self.path.required_ns is None:
                self.path.required_ns = float(m.group(2))
            return True
        m = _SLACK.match(line)
        if m:
            self.path.slack_state, self.path.slack_ns = m.group(1), float(m.group(2))
            return True
        return False

    def data_row(self, line: str) -> None:
        parsed = _row(line)
        if parsed is None:
            return
        text, incr, total = parsed
        pin = _PIN.match(text)
        if pin is None:
            if text.startswith(INPUT_DELAY):
                self.after_input_delay = True
            return
        note = pin.group("note") or ""
        if not self.started:
            if note != CLOCK_TO_OUTPUT and not self.after_input_delay:
                return  # launch clock network (clk pad, flip-flop clock pin)
            self.started = True
        self.path.rows.append(Row(pin.group("atom"), pin.group("port"), int(pin.group("bit")),
                                  pin.group("btype"), note, incr, total))

    def feed(self, line: str) -> None:
        if self.header(line) or self.times(line):
            return
        if line.startswith("Point "):
            self.in_points = True
        elif self.in_points and not self.arrival_done:
            self.data_row(line)


def parse_lines(lines: Sequence[str]) -> list[TimingPath]:
    """The paths of a setup report, in report order (worst slack first)."""
    paths: list[TimingPath] = []
    reader: _PathReader | None = None
    for raw in lines:
        line = raw.rstrip()
        head = PATH_HEAD.match(line)
        if head:
            reader = _PathReader(int(head.group(1)))
            paths.append(reader.path)
        elif reader is not None:
            reader.feed(line)
    return paths


def parse_report(path: Path) -> list[TimingPath]:
    """Read the report at ``path``. ReportError when it is missing or holds no path."""
    if not path.is_file():
        raise ReportError(f"no timing report at {path}")
    paths = parse_lines(path.read_text(errors="replace").splitlines())
    if not paths:
        raise ReportError(f"{path}: no '#Path' sections (not a VPR setup timing report)")
    bad = [p.index for p in paths if not p.rows or p.slack_ns is None]
    if bad and len(bad) == len(paths):
        raise ReportError(f"{path}: no path has data rows and a slack line")
    return [p for p in paths if p.rows and p.slack_ns is not None]


def final_cpd(texts: Sequence[str]) -> tuple[float, float | None] | None:
    """(CPD ns, Fmax MHz or None) from the last ``Final critical path delay`` line of the first
    text that has one; None when no text has it."""
    for text in texts:
        found = _FINAL_CPD.findall(text)
        if found:
            cpd, fmax = found[-1]
            return float(cpd), float(fmax) if fmax else None
    return None


def iter_rows(paths: Sequence[TimingPath]) -> Iterator[Row]:
    for path in paths:
        yield from path.rows
