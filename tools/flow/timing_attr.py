"""timing_attr: attribute the critical path of a VPR run to HDL statements, modules and words
(v1.1.1, docs/V1.1.1.md, docs/user/timing-attribution.md).

    odin3-timing FLOW_DIR [--paths N] [--out DIR]

``FLOW_DIR`` is an ``odin3-flow`` output directory made with ``--timing-attr``. The tool joins the
atoms of VPR's setup timing report to the Odin III provenance and writes ``timing/attribution.json``
(schema_version 1) and ``timing/summary.txt``. Importable: ``attribute_flow_dir(Path) -> dict``.
Exit codes: 0 ok, 1 the attribution failed (missing or inconsistent input), 2 usage error.
Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import timing_report
from timing_report import ReportError, TimingPath

SCHEMA_VERSION = 1
DEFAULT_PATHS = 100  # VPR's --timing_report_npaths default
TIMING_DIR = "timing"
INPUTS_NAME = "inputs.json"
PROV_BLIF_NAME = "prov.blif"
NETLIST_JSON_NAME = "netlist.odin3.json"
SUMMARY_TOP = 10
UNMATCHED_LIST_MAX = 200
TEXT_MAX = 160
PAD_TYPES = (".input", ".output")
MATCHED, PORT, UNMATCHED = "matched", "port", "unmatched"
_PROV = re.compile(r"^(?P<hier>.*?)/(?P<net>[^/]+)@(?P<file>.*):(?P<line>\d+)$")
_BUS = re.compile(r"^(?P<word>.+)\[(?P<bit>\d+)\]$")
_ASSIGN = re.compile(r"^\s*(?:assign\s+)?(?:(?:reg|wire)\s+)?(?:\[[^\]]*\]\s*)?([A-Za-z_]\w*)"
                     r"\s*(?:\[[^\]]*\]\s*)?<?=(?!=)")
_GENERATED = re.compile(r"^\$n(\d+)$")
_SPAN = re.compile(r"^(?P<file>.*):(?P<line>\d+)\.(?P<col>\d+)-\d+\.\d+$")


class AttributionError(RuntimeError):
    """An input that is missing or inconsistent: exit 1."""


class UsageError(Exception):
    """A bad command line: exit 2."""


@dataclass(frozen=True)
class Origin:
    """Where an atom came from: ``category`` matched (a source location), port (an I/O pad) or
    unmatched; ``via`` says which input gave the location."""

    category: str
    atom: str
    module: str | None = None
    word: str | None = None
    bit: int | None = None
    file: str | None = None
    line: int | None = None
    col: int | None = None
    via: str = ""

    @property
    def span(self) -> str | None:
        return f"{self.file}:{self.line}" if self.file and self.line else None


@dataclass
class NameMap:
    """Short BLIF net name -> (module path, net, file, line) read from the provenance BLIF."""

    names: dict[str, tuple[str, str, str, int]] = field(default_factory=dict)
    inputs: set[str] = field(default_factory=set)
    outputs: set[str] = field(default_factory=set)
    blocks: int = 0


def split_word(name: str) -> tuple[str | None, int | None]:
    """(word, bit) of a source net name: ``S[3]`` is (S, 3), ``clk`` is (clk, None); a generated
    net (``$n12``) has no word."""
    if name.startswith("$"):
        return None, None
    m = _BUS.match(name)
    return (m.group("word"), int(m.group("bit"))) if m else (name, None)


def logical_lines(path: Path) -> Iterator[str]:
    """BLIF lines with continuations joined; comments and blank lines dropped."""
    buf = ""
    with path.open(errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if not buf and (not line or line.startswith("#")):
                continue
            buf = f"{buf} {line}" if buf else line
            if buf.endswith("\\"):
                buf = buf[:-1].rstrip()
                continue
            yield buf
            buf = ""
    if buf:
        yield buf


def net_of(token: str) -> str:
    """The net of a BLIF token: the part after ``=`` of a ``.subckt`` pin, else the token."""
    return token.split("=", 1)[1] if "=" in token else token


def build_name_map(short: Path, prov: Path) -> NameMap:
    """Pair the logical lines of the two BLIFs and record every net whose name differs.
    AttributionError when the files do not line up (different line or token counts, one net with
    two provenance names)."""
    result = NameMap()
    other = logical_lines(prov)
    for number, left in enumerate(logical_lines(short), 1):
        right = next(other, None)
        lt, rt = left.split(), (right or "").split()
        if right is None or len(lt) != len(rt) or lt[0] != rt[0]:
            raise AttributionError(
                f"{short.name} and {prov.name} do not line up at logical line {number} "
                f"({left[:60]!r}): they must come from one odin3 run")
        if lt[0] == ".inputs":
            result.inputs.update(lt[1:])
        elif lt[0] == ".outputs":
            result.outputs.update(lt[1:])
        elif lt[0] in (".names", ".latch", ".subckt"):
            result.blocks += 1
        for a, b in zip(lt[1:], rt[1:], strict=True):
            if a != b:
                record_name(result, net_of(a), net_of(b), short.name)
    if next(other, None) is not None:
        raise AttributionError(f"{prov.name} has more lines than {short.name}")
    return result


def record_name(result: NameMap, short: str, prov: str, label: str) -> None:
    m = _PROV.match(prov)
    if m is None:
        return
    entry = (m.group("hier"), m.group("net"), m.group("file"), int(m.group("line")))
    if result.names.setdefault(short, entry) != entry:
        raise AttributionError(f"{label}: net {short} has two provenance names")


@dataclass
class JsonIndex:
    """Drivers of the named top-module nets, from ``write_json --odin3``: used for atoms whose BLIF
    net keeps a port name (the provenance name leaves port names alone)."""

    top: str
    bits: dict[str, list[int | str]] = field(default_factory=dict)
    drivers: dict[int, tuple[str, str | None]] = field(default_factory=dict)
    bit_names: dict[int, tuple[str, int | None]] = field(default_factory=dict)

    def net_word(self, atom: str) -> tuple[str, int | None] | None:
        """(word, bit) of the source wire that holds net ``atom`` when that is a generated name
        ``$n<ID>``: the JSON writer numbers a net's bit ``ID + 1`` (src/backends/json/emit.c),
        so the wire bit with that number is the same net."""
        m = _GENERATED.match(atom)
        return self.bit_names.get(int(m.group(1)) + 1) if m else None

    def lookup(self, name: str) -> tuple[str, str] | None:
        """(span text ``file:line.col-line.col``, hier) of the driver of net ``name``."""
        word, bit = split_word(name)
        if word is None:
            return None
        bits = self.bits.get(name) or self.bits.get(word)
        if bits is None:
            return None
        index = 0 if len(bits) == 1 and bit is None else (bit or 0)
        if index >= len(bits) or not isinstance(bits[index], int):
            return None
        found = self.drivers.get(int(bits[index]))
        return (found[0], found[1] or self.top) if found and found[0] else None


def load_json_index(path: Path, top: str | None) -> JsonIndex | None:
    """The index of ``path`` for module ``top`` (the only module when ``top`` is None);
    None when the file is absent."""
    if not path.is_file():
        return None
    modules = json.loads(path.read_text()).get("modules", {})
    name = top if top in modules else (next(iter(modules)) if len(modules) == 1 else None)
    if name is None:
        return None
    module = modules[name]
    index = JsonIndex(name)
    for net, data in module.get("netnames", {}).items():
        index.bits[net] = list(data.get("bits", []))
        if net.startswith("$"):
            continue
        for position, bit in enumerate(data.get("bits", [])):
            if isinstance(bit, int):
                index.bit_names.setdefault(bit, (net, position if len(data["bits"]) > 1 else None))
    for cell in module.get("cells", {}).values():
        srcs = str(cell.get("attributes", {}).get("src", "")).split("|")
        hier = cell.get("odin3_prov", {}).get("hier") or None
        for port, direction in cell.get("port_directions", {}).items():
            if direction != "output":
                continue
            for bit in cell.get("connections", {}).get(port, []):
                if isinstance(bit, int):
                    index.drivers[bit] = (srcs[0], hier)
    return index


class Resolver:
    """Atom name (and block type) -> Origin, through the provenance names, then the JSON drivers."""

    def __init__(self, names: NameMap, index: JsonIndex | None, top: str | None) -> None:
        self.names, self.index, self.top = names, index, top
        self.cache: dict[tuple[str, str], Origin] = {}

    def resolve(self, atom: str, block_type: str) -> Origin:
        key = (atom, block_type)
        if key not in self.cache:
            self.cache[key] = self._resolve(atom, block_type)
        return self.cache[key]

    def _resolve(self, atom: str, block_type: str) -> Origin:
        if block_type in PAD_TYPES:
            port = timing_report.pad_port(atom)
            word, bit = split_word(port)
            return Origin(PORT, atom, self.top, word, bit, via="pad")
        hit = self.names.names.get(atom)
        if hit is not None:
            hier, net, file, line = hit
            word, bit = split_word(net)
            named = self.index.net_word(net) if self.index and word is None else None
            if named is not None:
                word, bit = named
            return Origin(MATCHED, atom, hier, word, bit, file, line, via="provenance-name")
        found = self.index.lookup(atom) if self.index else None
        if found is not None:
            return self._from_span(atom, found)
        return Origin(UNMATCHED, atom)

    def _from_span(self, atom: str, found: tuple[str, str]) -> Origin:
        text, hier = found
        m = _SPAN.match(text)
        if m is None:
            return Origin(UNMATCHED, atom)
        word, bit = split_word(atom)
        return Origin(MATCHED, atom, hier, word, bit, m.group("file"), int(m.group("line")),
                      int(m.group("col")), via="netlist-json")


@dataclass
class Hop:
    """One atom on a path: its cell delay, the interconnect delay that arrived at it, its pins."""

    atom: str
    block_type: str
    net_in_ns: float
    cell_ns: float = 0.0
    pins: list[str] = field(default_factory=list)
    origin: Origin | None = None

    def delay_ns(self, next_hop: Hop | None) -> float:
        """Cell delay plus the interconnect delay on the net this atom drives."""
        return self.cell_ns + (next_hop.net_in_ns if next_hop else 0.0)


def path_hops(path: TimingPath) -> list[Hop]:
    """The hops of a path: rows of one atom are arcs inside its cell (cell delay); the first row
    of the next atom carries the interconnect delay of the net the previous atom drives. The row
    after a launch (``clock-to-output``) always starts a new hop (a flip-flop feeding itself)."""
    hops: list[Hop] = []
    previous = None
    for row in path.rows:
        same = hops and row.atom == hops[-1].atom and (
            previous is None or previous.note != timing_report.CLOCK_TO_OUTPUT)
        pin = f"{row.port}[{row.bit}]"
        if same:
            hops[-1].cell_ns += row.incr
            hops[-1].pins.append(pin)
        elif hops:
            hops.append(Hop(row.atom, row.block_type, row.incr, 0.0, [pin]))
        else:
            hops.append(Hop(row.atom, row.block_type, 0.0, row.incr, [pin]))
        previous = row
    return hops


def round_ns(value: float) -> float:
    return round(value, 6)


@dataclass
class Stat:
    """Aggregate of one span, module or word over the top-N paths."""

    paths: set[int] = field(default_factory=set)
    worst_slack: float | None = None
    nodes: int = 0
    critical_ns: float = 0.0
    words: set[str] = field(default_factory=set)
    modules: set[str] = field(default_factory=set)
    bits: set[int] = field(default_factory=set)

    def add(self, path_index: int, slack: float, origin: Origin) -> None:
        self.paths.add(path_index)
        self.nodes += 1
        if self.worst_slack is None or slack < self.worst_slack:
            self.worst_slack = slack
        if origin.word:
            self.words.add(origin.word)
        if origin.module:
            self.modules.add(origin.module)
        if origin.bit is not None:
            self.bits.add(origin.bit)


def source_text(file: str, line: int, cache: dict[str, list[str]]) -> str | None:
    """The text of line ``line`` of ``file`` (stripped, cut to TEXT_MAX), None if unreadable."""
    if file not in cache:
        try:
            cache[file] = Path(file).read_text(errors="replace").splitlines()
        except OSError:
            cache[file] = []
    lines = cache[file]
    return lines[line - 1].strip()[:TEXT_MAX] if 0 < line <= len(lines) else None


def assigned_word(text: str | None) -> str | None:
    """The word a source line assigns (``x <= ...``, ``assign y = ...``), None if the line is not a
    plain assignment. A guess from the text, for statements whose nets have no source name."""
    m = _ASSIGN.match(text) if text else None
    return m.group(1) if m else None


def hop_record(hop: Hop, next_hop: Hop | None) -> dict[str, Any]:
    origin = hop.origin or Origin(UNMATCHED, hop.atom)
    return {
        "atom": hop.atom, "block_type": hop.block_type, "pins": hop.pins,
        "cell_delay_ns": round_ns(hop.cell_ns), "net_delay_in_ns": round_ns(hop.net_in_ns),
        "delay_ns": round_ns(hop.delay_ns(next_hop)), "category": origin.category,
        "span": origin.span, "module": origin.module, "word": origin.word, "bit": origin.bit,
    }


def stat_record(stat: Stat, total_ns: float) -> dict[str, Any]:
    return {
        "paths_through": len(stat.paths),
        "worst_slack_ns": None if stat.worst_slack is None else round_ns(stat.worst_slack),
        "nodes": stat.nodes,
        "critical_delay_ns": round_ns(stat.critical_ns),
        "critical_share": round(stat.critical_ns / total_ns, 6) if total_ns > 0 else 0.0,
    }


def collapse(hops: Sequence[Hop]) -> list[dict[str, Any]]:
    """The critical path as an ordered list of spans: consecutive hops with one origin merge."""
    out: list[dict[str, Any]] = []
    key = None
    for i, hop in enumerate(hops):
        origin = hop.origin or Origin(UNMATCHED, hop.atom)
        nxt = hops[i + 1] if i + 1 < len(hops) else None
        label = origin.span or origin.category if origin.category != PORT else f"port:{origin.word}"
        cell, net = hop.cell_ns, (nxt.net_in_ns if nxt else 0.0)
        if out and key == label:
            out[-1]["cell_delay_ns"] += cell
            out[-1]["net_delay_ns"] += net
            out[-1]["atoms"].append(hop.atom)
            out[-1]["words"] = sorted(set(out[-1]["words"]) | ({origin.word} - {None}))
            continue
        key = label
        out.append({"span": origin.span, "kind": origin.category, "module": origin.module,
                    "file": origin.file, "line": origin.line,
                    "words": [origin.word] if origin.word else [], "cell_delay_ns": cell,
                    "net_delay_ns": net, "atoms": [hop.atom]})
    return out


def add_critical(hops: Sequence[Hop], spans: dict[str, Stat], modules: dict[str, Stat],
                 words: dict[tuple[str, str], Stat]) -> float:
    """Add the delay of the critical path's hops to the aggregates; returns the data delay."""
    total = 0.0
    for i, hop in enumerate(hops):
        delay = hop.delay_ns(hops[i + 1] if i + 1 < len(hops) else None)
        total += delay
        origin = hop.origin
        if origin is None:
            continue
        if origin.span:
            spans[origin.span].critical_ns += delay
        if origin.module:
            modules[origin.module].critical_ns += delay
        if origin.word and origin.module:
            words[(origin.module, origin.word)].critical_ns += delay
    return total


@dataclass
class Aggregates:
    spans: dict[str, Stat] = field(default_factory=lambda: defaultdict(Stat))
    modules: dict[str, Stat] = field(default_factory=lambda: defaultdict(Stat))
    words: dict[tuple[str, str], Stat] = field(default_factory=lambda: defaultdict(Stat))
    counts: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    unmatched: dict[tuple[str, str], dict[str, Any]] = field(default_factory=dict)

    def add_hop(self, path: TimingPath, hop: Hop) -> None:
        origin = hop.origin or Origin(UNMATCHED, hop.atom)
        self.counts[origin.category] += 1
        slack = path.slack_ns if path.slack_ns is not None else 0.0
        if origin.span:
            self.spans[origin.span].add(path.index, slack, origin)
        if origin.module and origin.category != UNMATCHED:
            self.modules[origin.module].add(path.index, slack, origin)
        if origin.word and origin.module and origin.category != UNMATCHED:
            self.words[(origin.module, origin.word)].add(path.index, slack, origin)
        if origin.category == UNMATCHED:
            entry = self.unmatched.setdefault(
                (hop.atom, hop.block_type),
                {"atom": hop.atom, "block_type": hop.block_type, "nodes": 0, "paths": set()})
            entry["nodes"] += 1
            entry["paths"].add(path.index)


def share(part: float, whole: float) -> float:
    return round(part / whole, 6) if whole > 0 else 0.0


def coverage_record(agg: Aggregates, crit_hops: Sequence[Hop]) -> dict[str, Any]:
    total = sum(agg.counts.values())
    delay: dict[str, float] = defaultdict(float)
    for i, hop in enumerate(crit_hops):
        nxt = crit_hops[i + 1] if i + 1 < len(crit_hops) else None
        delay[(hop.origin or Origin(UNMATCHED, hop.atom)).category] += hop.delay_ns(nxt)
    crit_total = sum(delay.values())
    listed = sorted(agg.unmatched.values(), key=lambda e: (-e["nodes"], e["atom"]))
    rows = [{"atom": e["atom"], "block_type": e["block_type"], "nodes": e["nodes"],
             "paths": len(e["paths"])} for e in listed]
    return {
        "nodes": total,
        "matched": agg.counts[MATCHED], "port": agg.counts[PORT],
        "unmatched": agg.counts[UNMATCHED],
        "matched_share": share(agg.counts[MATCHED], total),
        "port_share": share(agg.counts[PORT], total),
        "unmatched_share": share(agg.counts[UNMATCHED], total),
        "critical_matched_delay_share": share(delay[MATCHED], crit_total),
        "critical_port_delay_share": share(delay[PORT], crit_total),
        "critical_unmatched_delay_share": share(delay[UNMATCHED], crit_total),
        "unmatched_atoms": rows[:UNMATCHED_LIST_MAX],
        "unmatched_atoms_total": len(rows),
    }


def span_records(agg: Aggregates, total_ns: float) -> list[dict[str, Any]]:
    cache: dict[str, list[str]] = {}
    rows = []
    for span, stat in agg.spans.items():
        file, _, line = span.rpartition(":")
        row = {"id": span, "file": file, "line": int(line),
               "text": source_text(file, int(line), cache), "modules": sorted(stat.modules),
               "words": sorted(stat.words), **stat_record(stat, total_ns)}
        row["assigned_word"] = assigned_word(row["text"])
        rows.append(row)
    rows.sort(key=lambda r: (-r["critical_delay_ns"], -r["paths_through"], r["id"]))
    return rows


def group_records(agg: Aggregates, total_ns: float) -> tuple[list[dict[str, Any]],
                                                              list[dict[str, Any]]]:
    mods = [{"module": m, "words": sorted(s.words), **stat_record(s, total_ns)}
            for m, s in agg.modules.items()]
    wds = [{"module": m, "word": w, "bits": sorted(s.bits), **stat_record(s, total_ns)}
           for (m, w), s in agg.words.items()]
    for rows in (mods, wds):
        rows.sort(key=lambda r: (-r["critical_delay_ns"], -r["paths_through"],
                                 r["module"], r.get("word", "")))
    return mods, wds


def build_report(paths: Sequence[TimingPath], resolver: Resolver,
                 cpd: tuple[float, float | None] | None, meta: Mapping[str, Any],
                 requested: int | None) -> dict[str, Any]:
    """The attribution of ``paths`` (worst slack first) as the JSON-ready dict."""
    if not paths:
        raise AttributionError("the timing report holds no path")
    agg = Aggregates()
    all_hops: list[list[Hop]] = []
    for path in paths:
        hops = path_hops(path)
        for hop in hops:
            hop.origin = resolver.resolve(hop.atom, hop.block_type)
            agg.add_hop(path, hop)
        all_hops.append(hops)
    crit = all_hops[0]
    total_ns = add_critical(crit, agg.spans, agg.modules, agg.words)
    mods, wds = group_records(agg, total_ns)
    first = paths[0]
    spans = collapse(crit)
    for item in spans:
        item["cell_delay_ns"] = round_ns(item["cell_delay_ns"])
        item["net_delay_ns"] = round_ns(item["net_delay_ns"])
        item["delay_ns"] = round_ns(item["cell_delay_ns"] + item["net_delay_ns"])
        item["share"] = share(item["delay_ns"], total_ns)
    return {
        "schema_version": SCHEMA_VERSION,
        "design": dict(meta),
        "timing": {
            "cpd_ns": cpd[0] if cpd else None, "fmax_mhz": cpd[1] if cpd else None,
            "paths_requested": requested, "paths_parsed": len(paths),
            "worst_slack_ns": first.slack_ns, "critical_data_delay_ns": round_ns(total_ns),
        },
        "coverage": coverage_record(agg, crit),
        "critical_path": {
            "index": first.index, "slack_ns": first.slack_ns, "arrival_ns": first.arrival_ns,
            "required_ns": first.required_ns, "startpoint": first.startpoint,
            "endpoint": first.endpoint, "data_delay_ns": round_ns(total_ns), "spans": spans,
        },
        "spans": span_records(agg, total_ns), "modules": mods, "words": wds,
        "paths": [path_record(p, h) for p, h in zip(paths, all_hops, strict=True)],
    }


def path_record(path: TimingPath, hops: Sequence[Hop]) -> dict[str, Any]:
    return {
        "index": path.index, "slack_ns": path.slack_ns, "arrival_ns": path.arrival_ns,
        "required_ns": path.required_ns, "startpoint": path.startpoint, "endpoint": path.endpoint,
        "nodes": [hop_record(h, hops[i + 1] if i + 1 < len(hops) else None)
                  for i, h in enumerate(hops)],
    }


def fmt_ns(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.3f}"


def summary_text(report: Mapping[str, Any]) -> str:
    """The text summary: Fmax / CPD, coverage, and the statements that set the critical path."""
    t, cov, crit = report["timing"], report["coverage"], report["critical_path"]
    design = report["design"]
    fmax = "n/a" if t["fmax_mhz"] is None else f"{t['fmax_mhz']:.2f} MHz"
    lines = [
        f"Critical path attribution for {design.get('top') or '?'}",
        f"Fmax {fmax}   CPD {fmt_ns(t['cpd_ns'])} ns   worst slack {fmt_ns(t['worst_slack_ns'])} ns"
        f"   ({t['paths_parsed']} setup paths)",
        f"Critical path: {crit['startpoint']}  to  {crit['endpoint']}",
        f"Matched to HDL: {cov['matched_share'] * 100:.1f}% of {cov['nodes']} path nodes "
        f"({cov['critical_matched_delay_share'] * 100:.1f}% of the critical path delay); "
        f"ports {cov['port_share'] * 100:.1f}%; unmatched {cov['unmatched_share'] * 100:.1f}%",
        "", "These statements set your Fmax (critical path, in path order):",
    ]
    lines += [span_line(item) for item in crit["spans"]]
    lines += ["", f"Top {SUMMARY_TOP} statements by delay on the critical path "
                  f"(paths = of the top {t['paths_parsed']} through it):"]
    lines += [stmt_line(row) for row in report["spans"][:SUMMARY_TOP]]
    lines += ["", "Modules (critical path delay):"]
    lines += [f"  {m['module']:<30} {m['critical_delay_ns']:>7.3f} ns  "
              f"{m['critical_share'] * 100:5.1f}%  paths {m['paths_through']}"
              for m in report["modules"][:SUMMARY_TOP]]
    words = [w for w in report["words"] if w["critical_delay_ns"] > 0][:SUMMARY_TOP]
    if words:
        lines += ["", "Words (critical path delay):"] + [
            f"  {w['module']}.{w['word']:<24} {w['critical_delay_ns']:>7.3f} ns  "
            f"{w['critical_share'] * 100:5.1f}%  paths {w['paths_through']}" for w in words]
    lines += ["", "(word ~name: guessed from the assignment on that line; - : no source name)"]
    if cov["unmatched"]:
        lines += ["", f"Unmatched: {cov['unmatched']} path nodes in {cov['unmatched_atoms_total']} "
                      f"atoms (see unmatched_atoms in attribution.json)"]
    return "\n".join(lines) + "\n"


def span_line(item: Mapping[str, Any]) -> str:
    where = item["span"] or ("(I/O port)" if item["kind"] == PORT else "(no source location)")
    words = ",".join(item["words"]) or "-"
    return (f"  {where}  module {item['module'] or '-'}  word {words}  "
            f"{item['delay_ns']:.3f} ns ({item['share'] * 100:.1f}%)")


def stmt_line(row: Mapping[str, Any]) -> str:
    text = f"   | {row['text']}" if row.get("text") else ""
    words = ",".join(row["words"]) or (f"~{row['assigned_word']}" if row["assigned_word"] else "-")
    return (f"  {row['id']}  module {','.join(row['modules']) or '-'}  word {words}  "
            f"{row['critical_delay_ns']:.3f} ns ({row['critical_share'] * 100:.1f}%)  "
            f"slack {fmt_ns(row['worst_slack_ns'])}  paths {row['paths_through']}{text}")


def read_inputs(flow_dir: Path) -> dict[str, Any]:
    path = flow_dir / TIMING_DIR / INPUTS_NAME
    if not path.is_file():
        raise AttributionError(
            f"{path} not found: run odin3-flow with --timing-attr to make the inputs "
            f"(it writes {TIMING_DIR}/{PROV_BLIF_NAME} and {INPUTS_NAME})")
    data: dict[str, Any] = json.loads(path.read_text())
    return data


def absolute(flow_dir: Path, value: str | None) -> Path | None:
    return None if not value else (flow_dir / value if not Path(value).is_absolute()
                                   else Path(value))


def attribute_flow_dir(flow_dir: Path | str, paths: int | None = None) -> dict[str, Any]:
    """Attribute the critical path of the VPR run in ``flow_dir`` (an ``odin3-flow`` output
    directory made with ``--timing-attr``); returns the dict that ``attribution.json`` holds.
    ``paths`` keeps the first N paths of the report (default: all the report holds).
    AttributionError when an input is missing or inconsistent."""
    flow = Path(flow_dir).resolve()
    inputs = read_inputs(flow)
    run_dir = absolute(flow, inputs.get("vpr_run_dir"))
    blif, prov = absolute(flow, inputs.get("blif")), absolute(flow, inputs.get("prov_blif"))
    if run_dir is None or blif is None or prov is None:
        raise AttributionError(
            f"{flow / TIMING_DIR / INPUTS_NAME} lacks vpr_run_dir, blif or prov_blif")
    for need in (blif, prov):
        if not need.is_file():
            raise AttributionError(f"missing input {need}")
    try:
        parsed = timing_report.parse_report(run_dir / "report_timing.setup.rpt")
    except ReportError as exc:
        raise AttributionError(str(exc)) from exc
    logs = [(run_dir / n).read_text(errors="replace") for n in ("vpr.crit_path.out", "vpr.out")
            if (run_dir / n).is_file()]
    names = build_name_map(blif, prov)
    jpath = absolute(flow, inputs.get("netlist_json"))
    index = load_json_index(jpath, inputs.get("top")) if jpath else None
    top = inputs.get("top")
    kept = parsed[:paths] if paths else parsed
    meta = {"top": top, "flow_dir": str(flow), "report": str(run_dir / "report_timing.setup.rpt"),
            "blif": str(blif), "provenance_blif": str(prov),
            "netlist_json": str(jpath) if jpath and jpath.is_file() else None}
    return build_report(kept, Resolver(names, index, top), timing_report.final_cpd(logs), meta,
                        inputs.get("paths_requested"))


def write_outputs(report: Mapping[str, Any], out_dir: Path) -> tuple[Path, Path]:
    """Write ``attribution.json`` and ``summary.txt`` into ``out_dir``."""
    out_dir.mkdir(parents=True, exist_ok=True)
    js, txt = out_dir / "attribution.json", out_dir / "summary.txt"
    js.write_text(json.dumps(report, indent=1) + "\n")
    txt.write_text(summary_text(report))
    return js, txt


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        prog="odin3-timing", description="Attribute the critical path of an odin3-flow output "
        "directory (made with --timing-attr) to HDL statements, modules and words.",
        epilog="Exit codes: 0 ok, 1 attribution failed, 2 usage error.")
    ap.add_argument("flow_dir", type=Path, help="odin3-flow output directory")
    ap.add_argument("--paths", type=int, default=None,
                    help="use the first N paths of the report (default: all it holds)")
    ap.add_argument("--out", type=Path, default=None,
                    help=f"output directory (default: <flow_dir>/{TIMING_DIR})")
    ns = ap.parse_args(argv)
    if not ns.flow_dir.is_dir():
        raise UsageError(f"no such directory: {ns.flow_dir}")
    if ns.paths is not None and ns.paths < 1:
        raise UsageError("--paths needs a positive number")
    return ns


def main(argv: Sequence[str] | None = None) -> int:
    try:
        ns = parse_args(sys.argv[1:] if argv is None else argv)
    except UsageError as exc:
        print(f"odin3-timing: {exc}", file=sys.stderr)
        return 2
    except SystemExit as exc:
        return int(exc.code) if isinstance(exc.code, int) else 2
    try:
        report = attribute_flow_dir(ns.flow_dir, ns.paths)
    except (AttributionError, OSError, ValueError) as exc:
        print(f"odin3-timing: {exc}", file=sys.stderr)
        return 1
    js, txt = write_outputs(report, ns.out or ns.flow_dir / TIMING_DIR)
    print(txt.read_text(), end="")
    print(f"wrote {js} and {txt}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
