"""prov_why: the Q and K lines of the name map, why things were joined and how critical they were
(V1.2.0 Task 10; docs/V1.2.0.md 3.6).

``prov_post`` calls this module after it has made the pack and place records. Nothing here changes
a record: it appends ``Q`` lines (``id why objective gain pcrit rho slack near npaths members basis
stale``) and ``K`` lines (``id member pcrit rho slack basis stale``), and fills the post-route
fields of the objective-only ``Q`` lines that Odin III wrote for its own joins.

* The N17 pair classes (``SC SN SI OR FL``) of every unordered pair of members of a logic CLB, as
  shares by largest-remainder rounding (three decimals, sum exactly 1.000). They need a Norns
  packing census: without one ``why``, ``gain`` and ``pcrit`` are absent. The rules and their
  order are those of ``norns/scripts/r4census.py`` (read only), including its VPR clean-up of the
  BLIF (absorbed buffer LUTs, swept dead logic).
* ``gain``: the molecules of the census cluster that is this CLB, by source.
* ``rho``, ``slack``, ``near`` and ``npaths`` from ``timing/attribution.json`` (V1.1.1): rho is the
  largest arrival over path 1's arrival of a path through a member (0 off every path), a path is
  near-critical at margin m when its arrival is at least (1 - m/100) of path 1's.

Standard library only. ``LOG`` is the ``odin3`` logger: every refusal to use an input is a
WARNING, never a silent skip.
"""

from __future__ import annotations

import collections
import hashlib
import json
import logging
import re
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

LOG = logging.getLogger("odin3")

CLASSES = ("SC", "SN", "SI", "OR", "FL")
THETA_C = 0.5  # the critical threshold of the pair classes (N17)
FANOUT_LOCAL = 32  # a net with more sinks is a broadcast net
MARGINS = (5, 10)  # near-critical margins, percent (N17)
REPORT_PATHS = 1000  # paths the setup report holds when the flow does not say (3.6, npaths)
MEMBER_CAP = 16  # members listed for a join that is not a logic CLB (3.6)
THOUSANDTHS = 1000
# the candidate-queue sources of the Norns census (r4census.py SOURCES), in its order
SOURCES = ("seed", "connectivity_timing", "affinity_group", "transitive", "high_fanout",
           "attraction_group", "ram", "unrelated")
UNKNOWN_SOURCE = "unknown"
MEMBER_KINDS = ("lut", "latch", "adder")
CLOCK_LATCH_TYPES = ("re", "fe", "ah", "al", "as")
ADDER_OUTPUTS = frozenset(("cout", "sumout"))
HARD_OUTPUT_FORMALS = frozenset(("o", "q", "z", "cout", "sumout"))
NO_RHO = 0.0
_CONT = re.compile(r"\\\s*$")
_SHA = re.compile(r"^#\s*net_sha256\s*[=\t ]\s*([0-9a-fA-F]{64})\s*$")
_ESCAPES = (("%", "%25"), ("\t", "%09"), ("\n", "%0A"), ("\\", "%5C"), (",", "%2C"), ("|", "%7C"))


class WhyError(Exception):
    """An input is unreadable or malformed (exit 1 in prov_post)."""


def escape(text: str) -> str:
    """A name map field (version 2): %25 %09 %0A %5C %2C %7C, and %23 for a leading '#'."""
    for raw, esc in _ESCAPES:
        text = text.replace(raw, esc)
    return "%23" + text[1:] if text.startswith("#") else text


def fixed3(value: float) -> str:
    """Three decimals, as the map writes pcrit, rho and slack (-0.000 is written 0.000)."""
    text = f"{value:.3f}"
    return "0.000" if text == "-0.000" else text


# --- shares by largest-remainder rounding (3.6) -----------------------------------------------


def shares(counts: Mapping[str, int], order: Sequence[str]) -> list[tuple[str, int]]:
    """(name, thousandths) for every name with a non-zero count, by largest-remainder rounding:
    floor each share to 0.001, add 0.001 to the names with the largest remainders (ties in
    ``order``) until the sum is 1.000, and list them descending, ties in ``order``. A name with a
    non-zero count is always listed, even at 0 thousandths. Exact integer arithmetic. No counts:
    an empty list."""
    total = sum(counts.get(name, 0) for name in order)
    if total <= 0:
        return []
    names = [name for name in order if counts.get(name, 0) > 0]
    floors = {name: counts[name] * THOUSANDTHS // total for name in names}
    rest = {name: counts[name] * THOUSANDTHS % total for name in names}
    by_rest = sorted(names, key=lambda name: (-rest[name], order.index(name)))
    for name in by_rest[:THOUSANDTHS - sum(floors.values())]:
        floors[name] += 1
    return sorted(((name, floors[name]) for name in names),
                  key=lambda item: (-item[1], order.index(item[0])))


def share_text(items: Iterable[tuple[str, int]]) -> str:
    """``name:0.500`` elements, comma separated (the why and gain fields)."""
    return ",".join(f"{name}:{n // THOUSANDTHS}.{n % THOUSANDTHS:03d}" for name, n in items)


# --- the Norns packing census ------------------------------------------------------------------


@dataclass
class Census:
    """The parts of a Norns census file (``--norns_pack_census``) that Task 10 reads. Record types:
    ``M cseq idx kind source natoms ...`` (a molecule added), ``A cseq idx atom ...`` (its atoms),
    ``P cseq atom net crit`` (pack-time criticality of an input pin), ``K cseq n_added ...`` (the
    cluster closed). Others (Q, F) are not read. A comment line ``# net_sha256=<hex>`` names the
    ``.net`` the census ran with."""

    path: Path
    net_sha256: str | None = None
    source_of: dict[tuple[int, int], int] = field(default_factory=dict)
    atoms_of: dict[tuple[int, int], list[str]] = field(default_factory=dict)
    pin_crit: dict[str, dict[str, float]] = field(default_factory=dict)
    closed: set[int] = field(default_factory=set)
    cseq_of: dict[str, int] = field(default_factory=dict)
    cseq_atoms: dict[int, set[str]] = field(default_factory=dict)
    molecules: dict[int, list[int]] = field(default_factory=dict)  # cseq -> source codes

    def atom_crit(self, atom: str) -> float:
        """The pack-time criticality of an atom: the largest over its input pins (0 if none)."""
        pins = self.pin_crit.get(atom)
        return max(pins.values()) if pins else 0.0


def _census_int(text: str, path: Path, number: int) -> int:
    try:
        return int(text)
    except ValueError as exc:
        raise WhyError(f"{path}:{number}: '{text}' is not an integer") from exc


def _census_line(cen: Census, latest: dict[str, int], f: list[str], number: int) -> None:
    """One data line of a census (M, A, P or K); other types are not read."""
    path, kind = cen.path, f[0]
    if kind == "M" and len(f) >= 6:
        key = (_census_int(f[1], path, number), _census_int(f[2], path, number))
        cen.source_of[key] = _census_int(f[4], path, number)
    elif kind == "A" and len(f) >= 4:
        key = (_census_int(f[1], path, number), _census_int(f[2], path, number))
        cen.atoms_of.setdefault(key, []).append(f[3])
    elif kind == "P" and len(f) >= 5:
        cseq, atom, net = _census_int(f[1], path, number), f[2], f[3]
        try:
            crit = float(f[4])
        except ValueError as exc:
            raise WhyError(f"{path}:{number}: '{f[4]}' is not a number") from exc
        if latest.get(atom, 0) <= cseq:
            if latest.get(atom) != cseq:
                cen.pin_crit[atom] = {}
            latest[atom] = cseq
            cen.pin_crit[atom][net] = max(crit, cen.pin_crit[atom].get(net, 0.0))
    elif kind == "K" and len(f) >= 2:
        cen.closed.add(_census_int(f[1], path, number))


def read_census(path: Path) -> Census:
    """Parse a census file. An atom's final cluster is the latest closed cluster that added it (a
    destroyed cluster has no K line), as in r4census.py; a pin's criticality is the latest
    cluster's. WhyError on an unreadable file or a number that is not one."""
    cen = Census(path)
    latest: dict[str, int] = {}
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise WhyError(f"cannot read the census {path}: {exc}") from exc
    for number, line in enumerate(text.splitlines(), start=1):
        if line.startswith("#"):
            match = _SHA.match(line)
            if match:
                cen.net_sha256 = match.group(1).lower()
            continue
        _census_line(cen, latest, line.split("\t"), number)
    for cseq, idx in sorted(cen.source_of):
        cen.molecules.setdefault(cseq, []).append(cen.source_of[(cseq, idx)])
        if cseq in cen.closed:
            for atom in cen.atoms_of.get((cseq, idx), ()):
                cen.cseq_of[atom] = cseq
    for (cseq, _idx), atoms in cen.atoms_of.items():
        if cseq in cen.closed:
            cen.cseq_atoms.setdefault(cseq, set()).update(atoms)
    return cen


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --- the netlist relations the pair classes need (r4census.py Netlist) --------------------------


@dataclass
class Block:
    name: str
    kind: str  # lut, latch or hard
    inputs: list[str]
    rows: list[tuple[str, str]] = field(default_factory=list)
    pins: dict[str, str] = field(default_factory=dict)
    model: str = ""


def logical_lines(path: Path) -> list[str]:
    """BLIF lines with the continuation backslashes joined and comments cut."""
    out: list[str] = []
    buf = ""
    try:
        raw_lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise WhyError(f"cannot read the BLIF {path}: {exc}") from exc
    for raw in raw_lines:
        line = raw.split("#", 1)[0].rstrip()
        buf = buf + " " + line if buf else line
        if _CONT.search(buf):
            buf = _CONT.sub("", buf)
            continue
        out.append(buf)
        buf = ""
    if buf:
        out.append(buf)
    return out


def is_output_formal(model: str, formal: str) -> bool:
    """Whether a hard block's port formal is an output. The architecture would say; without it the
    adder's ports are known and other models follow VTR's names (``out``, ``out1``, ... and
    ``o``, ``q``, ``z``)."""
    base = formal.split("[")[0].lower()
    if model == "adder":
        return base in ADDER_OUTPUTS
    return base in HARD_OUTPUT_FORMALS or base.startswith("out")


def read_blif(path: Path) -> tuple[list[Block], list[str], set[str]]:
    """(blocks, primary outputs, clock nets) of a VTR-dialect BLIF. A block is named after its
    output net (``.names``, ``.latch``) or its first output pin's net (``.subckt``: an adder is
    named after its ``cout`` net, as VPR names it)."""
    blocks: list[Block] = []
    outputs: list[str] = []
    clocks: set[str] = set()
    cur: Block | None = None
    for line in logical_lines(path):
        tok = line.split()
        if not tok:
            continue
        head = tok[0]
        if head == ".outputs":
            outputs.extend(tok[1:])
        elif head == ".names":
            cur = Block(tok[-1] if len(tok) > 1 else "", "lut", tok[1:-1])
            blocks.append(cur)
        elif head in ("0", "1"):
            if cur is not None and len(tok) >= 2:
                cur.rows.append((tok[0], tok[1]))
        elif head == ".latch":
            blocks.append(Block(tok[2] if len(tok) > 2 else "", "latch",
                                [tok[1]] if len(tok) > 1 else []))
            if len(tok) >= 5 and tok[3] in CLOCK_LATCH_TYPES:
                clocks.add(tok[4])
        elif head == ".subckt":
            blocks.append(_subckt(tok, clocks))
    return blocks, outputs, clocks


def _subckt(tok: list[str], clocks: set[str]) -> Block:
    model = tok[1] if len(tok) > 1 else ""
    pins: dict[str, str] = {}
    for item in tok[2:]:
        if "=" in item:
            formal, actual = item.split("=", 1)
            pins[formal] = actual
            if formal.lower().startswith("clk") or formal.lower() == "clock":
                clocks.add(actual)
    outs = [actual for formal, actual in pins.items() if is_output_formal(model, formal)]
    name = outs[0] if outs else (next(iter(pins.values())) if pins else "")
    return Block(name, "hard", list(pins.values()), [], pins, model)


def absorbed_buffers(blocks: Sequence[Block]) -> set[str]:
    """Buffer LUTs (one input, table ``1 1``) that VPR's clean-up absorbs: the net they drive is
    the net they read."""
    return {b.name for b in blocks
            if b.kind == "lut" and len(b.inputs) == 1 and b.rows == [("1", "1")]}


def _resolver(alias: Mapping[str, str]) -> Callable[[str], str]:
    """Follows absorbed buffers: a net they drive is the net they read."""

    def res(net: str) -> str:
        for _ in range(len(alias) + 1):
            if net not in alias:
                break
            net = alias[net]
        return net

    return res


def _pins(b: Block, res: Callable[[str], str]) -> tuple[list[str], list[str]]:
    """(output nets, input nets resolved through absorbed buffers) of a block."""
    if b.kind == "hard":
        outs = [n for f, n in b.pins.items() if is_output_formal(b.model, f)]
        ins = [res(n) for f, n in b.pins.items() if not is_output_formal(b.model, f)]
        return outs, ins
    return [b.name], [res(n) for n in b.inputs]


def swept_blocks(blocks: Sequence[Block], outputs: Sequence[str], gone: set[str]) -> set[str]:
    """Blocks whose outputs nobody uses once the buffers in ``gone`` are absorbed, repeatedly
    (VPR's ``sweep_blocks``)."""
    res = _resolver({b.name: b.inputs[0] for b in blocks if b.name in gone and b.inputs})
    outs_of: dict[str, list[str]] = {}
    ins_of: dict[str, list[str]] = {}
    for b in blocks:
        if b.name not in gone:
            outs_of[b.name], ins_of[b.name] = _pins(b, res)
    users: dict[str, int] = collections.defaultdict(int)
    driver: dict[str, str] = {}
    for nets in ins_of.values():
        for net in nets:
            users[net] += 1
    for name, nets in outs_of.items():
        for net in nets:
            driver[net] = name
    for net in outputs:
        users[res(net)] += 1
    dead: set[str] = set()
    work = [n for n, o in outs_of.items() if o and all(users[x] == 0 for x in o)]
    while work:
        name = work.pop()
        if name in dead:
            continue
        dead.add(name)
        for net in ins_of[name]:
            users[net] -= 1
            found = driver.get(net)
            if found and found not in dead and all(users[x] == 0 for x in outs_of[found]):
                work.append(found)
    return dead


@dataclass
class Netlist:
    """Relations among the LUT, FF and adder atoms of the BLIF: the local direct nets (series),
    the common local nets (shared inputs) and the broadcast nets each atom reads."""

    kind: dict[str, str] = field(default_factory=dict)
    series: dict[tuple[str, str], list[str]] = field(default_factory=dict)
    shared: set[tuple[str, str]] = field(default_factory=set)
    bcast: dict[str, set[str]] = field(default_factory=dict)


def _drivers_and_sinks(blocks: Sequence[Block], gone: set[str], clocks: set[str],
                       res: Callable[[str], str], out: Netlist
                       ) -> tuple[dict[str, str], dict[str, list[str]]]:
    """The driver of each net and the sink pins of each (a block once per pin); fills out.kind."""
    driver: dict[str, str] = {}
    sinks: dict[str, list[str]] = collections.defaultdict(list)
    for b in blocks:
        if b.name in gone:
            continue
        if b.kind == "hard":
            if b.model == "adder":
                out.kind[b.name] = "adder"
            for formal, net in b.pins.items():
                net = res(net)
                if is_output_formal(b.model, formal):
                    driver[net] = b.name
                elif net not in clocks:
                    sinks[net].append(b.name)
            continue
        out.kind[b.name] = b.kind
        driver[b.name] = b.name
        sinks_here = (res(n) for n in b.inputs)
        for net in sinks_here:
            if net not in clocks:
                sinks[net].append(b.name)
    return driver, sinks


def build_netlist(blif: Path) -> Netlist:
    """r4census.py ``Netlist`` over a BLIF: the buffers VPR absorbs and the logic it sweeps are
    left out, a net is local when it has at most FANOUT_LOCAL sink pins (a pad counts as one) and
    broadcast above, and the clock nets are not nets."""
    blocks, outputs, clocks = read_blif(blif)
    absorbed = absorbed_buffers(blocks)
    gone = absorbed | swept_blocks(blocks, outputs, absorbed)
    res = _resolver({b.name: b.inputs[0] for b in blocks if b.name in absorbed and b.inputs})
    out = Netlist()
    driver, sinks = _drivers_and_sinks(blocks, gone, clocks, res, out)
    for net in outputs:
        sinks[res(net)].append("<pad>")
    bcast: dict[str, set[str]] = collections.defaultdict(set)
    for net, ss in sinks.items():
        drv = driver.get(net)
        members = sorted({s for s in ss if s in out.kind})
        if len(ss) > FANOUT_LOCAL:
            for sink in members:
                bcast[sink].add(net)
            continue
        if drv in out.kind:
            for sink in members:
                if sink != drv:
                    out.series.setdefault((drv, sink), []).append(net)
        for i, left in enumerate(members):
            for right in members[i + 1:]:
                out.shared.add((left, right))
    out.bcast = dict(bcast)
    return out


def classify(net: Netlist, crit: Mapping[str, Mapping[str, float]],
             word: Callable[[str], str | None], left: str, right: str) -> str:
    """The pair class of two atoms: the first rule that applies (3.6, the N17 definitions). SC and
    SN need a direct local net from one to the other and the pack-time criticality of the sink
    pin; SI both read a common local net; OR have the same word label or read a common broadcast
    net; FL otherwise. A pair that is SC and SI is SC."""
    best: float | None = None
    for drv, sink in ((left, right), (right, left)):
        nets = net.series.get((drv, sink))
        if nets:
            value = max(crit.get(sink, {}).get(n, 0.0) for n in nets)
            best = value if best is None else max(best, value)
    if best is not None:
        return "SC" if best >= THETA_C else "SN"
    if ((left, right) if left < right else (right, left)) in net.shared:
        return "SI"
    wl, wr = word(left), word(right)
    if (wl is not None and wl == wr) or (net.bcast.get(left, set()) & net.bcast.get(right, set())):
        return "OR"
    return "FL"


def pair_counts(net: Netlist, crit: Mapping[str, Mapping[str, float]],
                word: Callable[[str], str | None], members: Sequence[str]) -> dict[str, int]:
    """Pairs of ``members`` (sorted) per class; a one-member list has none."""
    counts: dict[str, int] = {}
    for i, left in enumerate(members):
        for right in members[i + 1:]:
            cls = classify(net, crit, word, left, right)
            counts[cls] = counts.get(cls, 0) + 1
    return counts


# --- post-route criticality (3.6) --------------------------------------------------------------


@dataclass(frozen=True)
class JoinCrit:
    """The post-route criticality of a set of atoms: ``rho`` (max over the atoms), ``slack`` (the
    smallest, None when no atom is on a reported path), the near-critical flags and path counts
    per margin and whether that count is a lower bound (a capped report)."""

    rho: float
    slack: float | None
    near: Mapping[int, bool]
    npaths: Mapping[int, int]
    capped: Mapping[int, bool]


@dataclass
class PathReport:
    """The setup paths of the attribution, worst slack first: arrival, slack and the atoms of each.
    ``capped`` says per margin that the report was cut while its last path was still inside the
    margin, so a count is a lower bound."""

    arrivals: list[float]
    slacks: list[float | None]
    atoms: list[frozenset[str]]
    capped: Mapping[int, bool]
    by_atom: dict[str, list[int]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        for index, atoms in enumerate(self.atoms):
            for atom in atoms:
                self.by_atom.setdefault(atom, []).append(index)

    @classmethod
    def from_attribution(cls, data: Mapping[str, object]) -> PathReport | None:
        """Read ``timing/attribution.json``. None when it holds no usable path."""
        paths = data.get("paths")
        timing = data.get("timing")
        if not isinstance(paths, list) or not paths:
            return None
        timing = timing if isinstance(timing, dict) else {}
        arrivals: list[float] = []
        slacks: list[float | None] = []
        atoms: list[frozenset[str]] = []
        for path in paths:
            arrival = path.get("arrival_ns") if isinstance(path, dict) else None
            if not isinstance(arrival, (int, float)):
                continue
            slack = path.get("slack_ns")
            arrivals.append(float(arrival))
            slacks.append(float(slack) if isinstance(slack, (int, float)) else None)
            nodes = path.get("nodes") or []
            atoms.append(frozenset(n["atom"] for n in nodes
                                   if isinstance(n, dict) and isinstance(n.get("atom"), str)))
        if not arrivals:
            return None
        requested = timing.get("paths_requested")
        cap = requested if isinstance(requested, int) and requested > 0 else REPORT_PATHS
        in_report = timing.get("paths_in_report")
        kept = len(arrivals)
        in_report = in_report if isinstance(in_report, int) else kept
        truncated = in_report >= cap or kept < in_report
        first, last = arrivals[0], arrivals[-1]
        capped = {m: truncated and last * 100 >= (100 - m) * first for m in MARGINS}
        return cls(arrivals, slacks, atoms, capped)

    def rho_of(self, atom: str) -> float:
        """The largest arrival over path 1's arrival of a path through the atom (0 off every
        path)."""
        first = self.arrivals[0]
        if first <= 0:
            return NO_RHO
        return max((self.arrivals[i] / first for i in self.by_atom.get(atom, ())), default=NO_RHO)

    def crit_of(self, members: Iterable[str]) -> JoinCrit:
        """The criticality of a join: rho, the smallest slack, and for each margin whether a path
        through any member is near-critical and how many such paths there are."""
        through: set[int] = set()
        rho = NO_RHO
        for atom in members:
            through.update(self.by_atom.get(atom, ()))
            rho = max(rho, self.rho_of(atom))
        slack_values = [s for i in through if (s := self.slacks[i]) is not None]
        first = self.arrivals[0]
        near: dict[int, bool] = {}
        npaths: dict[int, int] = {}
        for m in MARGINS:
            count = sum(1 for i in through if self.arrivals[i] * 100 >= (100 - m) * first)
            npaths[m] = count
            near[m] = count > 0
        return JoinCrit(rho, min(slack_values) if slack_values else None, near, npaths,
                        dict(self.capped))


def read_attribution(path: Path) -> PathReport | None:
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError) as exc:
        raise WhyError(f"cannot read the attribution {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise WhyError(f"{path} is not an attribution (a JSON object is expected)")
    return PathReport.from_attribution(data)


# --- the lines -----------------------------------------------------------------------------------


@dataclass
class JoinSpec:
    """A join record that gets a Q line. ``classes`` makes the pair classes and gain apply (a
    logic CLB); ``members`` are the atoms of the join in a stable order."""

    record: int
    members: list[str]
    objective: str = ""
    objective_basis: str = "-"
    classes: bool = False
    replaces: str = ""  # the raw text of the objective-only Q line this one replaces
    census_cseq: int | None = None


@dataclass
class WhyStats:
    q_lines: int = 0
    k_lines: int = 0
    with_classes: int = 0
    with_gain: int = 0
    census_used: bool = False


@dataclass
class WhyResult:
    qlines: list[str] = field(default_factory=list)
    klines: dict[int, list[str]] = field(default_factory=dict)
    replace: dict[str, str] = field(default_factory=dict)
    stats: WhyStats = field(default_factory=WhyStats)

    def appended(self) -> list[str]:
        """Q and K lines to append, each Q followed by its K lines, in record order."""
        out: list[str] = []
        for spec_record, line in self._appended:
            if line:  # a Q line that replaces one in place has K lines only
                out.append(line)
            out.extend(self.klines.get(spec_record, ()))
        return out

    _appended: list[tuple[int, str]] = field(default_factory=list)


def _basis(items: Sequence[tuple[str, str]]) -> str:
    return ",".join(f"{name}={letter}" for name, letter in items)


def _optional(value: str | None) -> str:
    return "" if value is None else value


def q_line(spec: JoinSpec, why: str, gain: str, pcrit: str, crit: JoinCrit | None,
           letters: Mapping[str, str]) -> str:
    """One Q line (3.2): ``why`` and ``gain`` are the finished share lists."""
    if crit is None:
        rho = slack = near = npaths = ""
    else:
        rho = fixed3(crit.rho)
        slack = "" if crit.slack is None else fixed3(crit.slack)
        near = ",".join(f"{m}:{int(crit.near[m])}" for m in MARGINS)
        npaths = ",".join(
            f"{m}:{'>=' if crit.capped.get(m) and crit.npaths[m] > 0 else ''}{crit.npaths[m]}"
            for m in MARGINS)
    basis = _basis([(name, letters.get(name, "-")) for name in
                    ("why", "objective", "gain", "pcrit", "rho", "slack", "near", "npaths")])
    fields = ("Q", f"r{spec.record}", why, escape(spec.objective), gain, pcrit, rho, slack, near,
              npaths, str(len(spec.members)), basis, "-")
    return "\t".join(fields)


def k_line(record: int, member: str, pcrit: str, rho: str, slack: str, letters: str) -> str:
    return "\t".join(("K", f"r{record}", escape(member), pcrit, rho, slack, letters, "-"))


def build_lines(specs: Sequence[JoinSpec], net: Netlist | None, census: Census | None,
                report: PathReport | None, word: Callable[[str], str | None],
                cluster_atoms: Mapping[int, frozenset[str]] | None = None) -> WhyResult:
    """The Q lines (and their K lines) of ``specs``. ``net`` and ``census`` together give the pair
    classes, gain and pack-time criticality of the specs marked ``classes`` (None for either: they
    are absent); ``report`` gives rho, slack and the near-critical fields (None: absent)."""
    result = WhyResult()
    stats = result.stats
    stats.census_used = census is not None
    for spec in sorted(specs, key=lambda s: s.record):
        members = spec.members
        why = gain = pcrit = ""
        letters = {"objective": spec.objective_basis if spec.objective else "-"}
        crit = report.crit_of(members) if report is not None else None
        if crit is not None:
            letters.update({"rho": "r", "slack": "r" if crit.slack is not None else "-",
                            "near": "r", "npaths": "r"})
        if census is not None and spec.classes:
            pcrit = fixed3(max((census.atom_crit(a) for a in members), default=0.0))
            letters["pcrit"] = "r"
            if net is not None and len(members) >= 2:
                counts = pair_counts(net, census.pin_crit, word, members)
                why = share_text(shares(counts, CLASSES))
                letters["why"] = "r"
                stats.with_classes += 1
            gain_items = _gain(spec, census, members, cluster_atoms)
            if gain_items:
                gain = share_text(gain_items)
                letters["gain"] = "r"
                stats.with_gain += 1
        line = q_line(spec, why, gain, pcrit, crit, letters)
        result.qlines.append(line)
        result._appended.append((spec.record, "" if spec.replaces else line))
        if spec.replaces:
            result.replace[spec.replaces] = line
        result.klines[spec.record] = _member_lines(spec, census, report)
        stats.q_lines += 1
        stats.k_lines += len(result.klines[spec.record])
    return result


def _gain(spec: JoinSpec, census: Census, members: Sequence[str],
          cluster_atoms: Mapping[int, frozenset[str]] | None) -> list[tuple[str, int]]:
    """The gain terms of the census cluster that is this CLB, [] when there is none: its atoms are
    all in the CLB's block (a destroyed or never closed cluster, a CLB over several clusters and
    a cluster with atoms in no block give none)."""
    seqs = {census.cseq_of.get(a) for a in members}
    if len(seqs) != 1 or None in seqs:
        return []
    cseq = next(iter(seqs))
    assert cseq is not None
    block = cluster_atoms.get(spec.record) if cluster_atoms is not None else None
    if block is not None and not census.cseq_atoms.get(cseq, set()) <= block:
        return []
    counts: dict[str, int] = collections.Counter(
        SOURCES[s] if 0 <= s < len(SOURCES) else UNKNOWN_SOURCE for s in census.molecules[cseq])
    return shares(counts, (*SOURCES, UNKNOWN_SOURCE))


def _member_lines(spec: JoinSpec, census: Census | None, report: PathReport | None) -> list[str]:
    """The K lines: every member of a logic CLB, else at most MEMBER_CAP; the highest rho first,
    ties by name (name order without a report)."""
    members = list(spec.members)
    if report is not None:
        members.sort(key=lambda a: (-report.rho_of(a), a))
    else:
        members.sort()
    if not spec.classes:
        members = members[:MEMBER_CAP]
    lines: list[str] = []
    for atom in members:
        letters: list[tuple[str, str]] = []
        pcrit = ""
        if census is not None and spec.classes:
            pcrit = fixed3(census.atom_crit(atom))
            letters.append(("pcrit", "r"))
        else:
            letters.append(("pcrit", "-"))
        rho = slack = ""
        if report is not None:
            rho = fixed3(report.rho_of(atom))
            letters.append(("rho", "r"))
            values = [report.slacks[i] for i in report.by_atom.get(atom, ())
                      if report.slacks[i] is not None]
            if values:
                slack = fixed3(min(v for v in values if v is not None))
            letters.append(("slack", "r" if values else "-"))
        else:
            letters += [("rho", "-"), ("slack", "-")]
        lines.append(k_line(spec.record, atom, pcrit, rho, slack, _basis(letters)))
    return lines
