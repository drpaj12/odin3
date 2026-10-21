"""prov-cover leg (d) (6.5c, spec 6 P-4): every block VPR places resolves to an HDL location.

The leg takes the BLIF of leg (b) (``write_blif --vtr --name-style provenance``), runs VPR on it
(``--pack --place``; ``tools/vtr-qor`` deletes the ``.net`` and ``.place`` files of its runs, so
this tool starts VPR itself, always under ``tools/heavy-run``) and joins the ``.place`` and
``.net`` files with the leg-(b) read-back dump (``dump_locs --types`` + ``write_json``).

Block name to netlist name, with the evidence (ch_intrinsics, VTR 12 ``vpr``, k6_frac arch, run
2026-10-21; every design of the sweep re-checks it, an unmapped name is a defect):

1. VPR reads each BLIF primitive as one atom named after its first output net (``.names`` and
   ``.latch``: the output net; ``.subckt``: its first output pin's net). A cluster (a line of the
   ``.place`` file, ``<block name="X" instance="clb[0]">`` of the ``.net`` file) is named after
   one of its atoms. Checked: 242 ``.place`` names equal the 242 top-level block names of the
   ``.net`` file, and every cluster name is the name of a leaf block (an atom) inside it.
   ``.net`` leaf blocks (no child ``block``, name not ``open``) are the atoms; a cluster holds
   one atom (a pad, a lone LUT) or several (a LUT and the latch it feeds).
2. An atom name is therefore the BLIF net name, verbatim. The writer's provenance names
   (``HIER/SHORT@FILE:LINE[$N]``, 5.4 L-5) are decoded as ``read_blif --hdl-locs`` decodes
   them (percent-decode, split at the last ``@`` and the last ``/`` before it, drop ``$N``): the
   net of the read-back design is ``SHORT``, its location is in the name.
3. I/O pads: an input pad is named after the port bit (``start``), an output pad is ``out:`` plus
   the port bit (``out:finish``); VPR makes the pad atom, the driver of an output port bit keeps
   the plain net name (a LUT or latch, not a pad). A plain name of an output port bit is
   therefore a logic atom and must resolve like any other.
4. ``gnd``, ``vcc`` and ``unconn`` are the generator ``.names`` of the writer: counted in the
   ``const-table`` class (P-3), not blocks to resolve.

Resolution of an atom: pad (``--allow pad``) or generator name, else the name decodes to
``SHORT`` (with a location in the name, a node driving a net ``SHORT`` must carry exactly that
file and line (its own or its net's): the same short name in two scopes does not pair, the
read-back has no hierarchy to key on) or the name is the net, whose driver cell must carry a
location (inherited through D-5 in a BLIF VTR read-back). Anything else is a defect:
``unmapped`` (no such net: the rule above is wrong for this block) or ``unresolved`` (the
driver has no location). A cluster resolves when its own name resolves and, with the ``.net``
file, every atom in it does; a ``.place`` name that is no block of the ``.net`` file is a defect
too (``unnetted``). With the ``.net`` file a pad is a block whose ``mode`` is
``inpad``/``outpad`` (the name must then be a port bit), not any block named like a port; pad
counts are unique names. VPR runs only with ``ODIN3_HEAVY_OK=1`` (``tools/heavy-run``), per
design ``--vpr-timeout`` seconds (default 1800).
"""

from __future__ import annotations

import os
import re
import subprocess
import xml.etree.ElementTree as ET
from collections.abc import Collection, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from prov_cover import (
    DEFAULT_ALLOW,
    GENERATOR_NETS,
    CrashError,
    Defect,
    Facts,
    LegReport,
    Obj,
    ToolError,
    parse_locs,
)

PERCENT = re.compile(r"%([0-9A-F]{2})")
LOC_TAIL = re.compile(r"(.+:\d+)(?:\$\d+)?")
OUT_PREFIX = "out:"
VPR_TIMEOUT = 1800.0
CHAN_WIDTH = "100"
DEFAULT_ARCH = Path("vtr_flow") / "arch" / "timing" / "k6_frac_N10_frac_chain_mem32K_40nm.xml"


@dataclass(frozen=True)
class Decoded:
    """An L-5 name: the hierarchy, the short name and the location text (``file:line``)."""

    hier: str
    short: str
    loc: str


def percent_decode(name: str) -> str:
    """VB-10 inverse: ``%XX`` (upper-case hex) is the byte."""
    return PERCENT.sub(lambda m: chr(int(m.group(1), 16)), name)


def decode_name(name: str) -> Decoded | None:
    """5.4 L-5, None for a name that is not a provenance name (kept as it is, L-6)."""
    text = percent_decode(name)
    at = text.rfind("@")
    tail = LOC_TAIL.fullmatch(text[at + 1:]) if at >= 0 else None
    if tail is None:
        return None
    hier, _, short = text[:at].rpartition("/")
    return Decoded(hier, short, tail.group(1))


@dataclass(frozen=True)
class Index:
    """What the leg-(b) read-back says about the nets: ``located`` the names of nets a located
    node drives, ``driven`` those any node drives, and the names of the port bits."""

    located: frozenset[str]
    driven: frozenset[str]
    inputs: frozenset[str]
    outputs: frozenset[str]
    # net name -> the (file, line) of the net and of every located node that drives it
    where: dict[str, frozenset[tuple[str, int]]] = field(default_factory=dict)
    # port-driver proof: the net names a located leg-(a) node drives
    a_located: frozenset[str] = frozenset()


def build_index(
    rows: Sequence[Obj], facts: Facts, a_located: Collection[tuple[str, str]] = frozenset()
) -> Index:
    """The net names of a typed dump joined with its connections (``write_json``).
    ``a_located``: (module, net name) pairs a located leg-(a) node drives, the proof of the
    ``port-driver`` class."""
    src = {(r.module, r.name): r.src for r in rows if r.kind == "node"}
    where: dict[str, set[tuple[str, int]]] = {}
    for r in rows:  # the net's own location is what the written name carries (a hard block's)
        if r.kind == "net":
            where.setdefault(r.name, set()).update((x.file, x.line) for x in parse_locs(r.src))
    located: set[str] = set()
    driven: set[str] = set()
    inputs: set[str] = set()
    outputs: set[str] = set()
    for module, mod in facts.items():
        for name, cell in mod.cells.items():
            for out in cell.outs:
                net = mod.net_name(out)
                driven.add(net)
                if src.get((module, name)):
                    located.add(net)
                    where.setdefault(net, set()).update(
                        (loc.file, loc.line) for loc in parse_locs(src[(module, name)])
                    )
        inputs.update(mod.net_name(b) for b in mod.port_in)
        outputs.update(mod.net_name(b) for b in mod.port_out)
        outputs.update(mod.port_bit_name(b) for b in mod.port_out)
    return Index(
        frozenset(located),
        frozenset(driven),
        frozenset(inputs),
        frozenset(outputs),
        {net: frozenset(locs) for net, locs in where.items()},
        frozenset(net for _, net in a_located),
    )


def parse_place(text: str) -> list[str]:
    """The block names of a ``.place`` file (the lines that are not header or comment)."""
    names = []
    for line in text.splitlines():
        words = line.split()
        if len(words) < 5 or line.startswith(("#", "Netlist_File", "Array size")):
            continue
        names.append(words[0])
    return names


def parse_net_atoms(text: str) -> dict[str, list[str]]:
    """Cluster name -> the atom names inside it, from a ``.net`` file: a leaf block (no child
    block) that is not ``open``; a repeated name (the pb levels of one atom) counts once."""
    try:
        root = ET.fromstring(text)  # noqa: S314 (the file is VPR's own output)
    except ET.ParseError as exc:
        raise ToolError(f".net file is not XML: {exc}") from exc

    def leaves(block: ET.Element) -> list[str]:
        kids = block.findall("block")
        if not kids:
            name = block.get("name", "")
            return [] if name == "open" else [name]
        return [n for kid in kids for n in leaves(kid)]

    return {
        top.get("name", ""): list(dict.fromkeys(leaves(top)))
        for top in root.findall("block")
    }


def parse_net_pads(text: str) -> dict[str, str]:
    """Top-level block name -> ``inpad`` or ``outpad``: the clusters VPR made for I/O pads (the
    ``mode`` of the block), the only blocks that are pads."""
    try:
        root = ET.fromstring(text)  # noqa: S314 (the file is VPR's own output)
    except ET.ParseError as exc:
        raise ToolError(f"vpr: unreadable .net file: {exc}") from exc
    return {
        top.get("name", ""): top.get("mode", "")
        for top in root.findall("block")
        if top.get("mode") in ("inpad", "outpad")
    }


def _is_pad(atom: str, index: Index, pads: dict[str, str] | None) -> bool:
    """A pad by its kind in the ``.net`` file when there is one (an input pad is named after an
    input port bit, an output pad ``out:`` plus an output port bit); by name alone without."""
    plain = atom[len(OUT_PREFIX):] if atom.startswith(OUT_PREFIX) else None
    kind = pads.get(atom, "") if pads is not None else ("outpad" if plain else "inpad")
    if kind == "outpad":
        return plain is not None and plain in index.outputs
    return kind == "inpad" and plain is None and atom in index.inputs


def _resolve(
    atom: str, index: Index, allow: frozenset[str], pads: dict[str, str] | None = None
) -> tuple[str, str]:
    """(verdict, note): verdict is pad, const, resolved, unresolved or unmapped."""
    if "pad" in allow and _is_pad(atom, index, pads):
        return "pad", ""
    if atom in GENERATOR_NETS:
        return "const", ""
    decoded = decode_name(atom)
    key = decoded.short if decoded else percent_decode(atom)
    if decoded:
        file, _, line = decoded.loc.rpartition(":")
        if (file, int(line)) in index.where.get(key, frozenset()):
            return "resolved", "decode"
        if key in index.located:
            return "unresolved", "location-differs"
    elif key in index.located:
        return "resolved", "net"
    if key in index.driven:
        if key in index.outputs and key in index.a_located:
            return "port-driver", ""  # Peter 2026-10-21: located in leg a, lost at the port bit
        note = "drives-output-port" if key in index.outputs else "driver-has-no-location"
        return "unresolved", note
    return "unmapped", "no-such-net"


def place_check(
    place_text: str,
    index: Index,
    net_text: str | None = None,
    allow: frozenset[str] = DEFAULT_ALLOW,
) -> LegReport:
    """P-4 leg (d). Blocks are the ``.place`` lines; with the ``.net`` file the atoms of every
    block are checked too (``atom`` objects). Pads are the ``pad`` class, generator names the
    ``const-table`` class; neither counts as an object."""
    rep = LegReport(
        objects={"block": 0, "atom": 0}, resolved={"block": 0, "atom": 0}
    )
    atoms = parse_net_atoms(net_text) if net_text is not None else {}
    pads = parse_net_pads(net_text) if net_text is not None else None
    seen: set[str] = set()
    excepted: set[str] = set()
    failed: set[str] = set()
    drivers: set[str] = set()

    def count(cls: str, name: str) -> None:
        verdict, note = _resolve(name, index, allow, pads)
        if verdict in ("pad", "const"):
            if name not in excepted:  # a pad cluster and its atom have one name: count it once
                excepted.add(name)
                rep.exceptions["pad" if verdict == "pad" else "const-table"] += 1
        else:
            rep.objects[cls] += 1
            if verdict == "port-driver":  # a cluster and its same-named atom: counted once
                if name not in drivers:
                    drivers.add(name)
                    rep.exceptions["port-driver"] += 1
                    rep.removed += 1
            elif verdict == "resolved":
                rep.resolved[cls] += 1
                rep.direct += 1
            elif name not in failed:  # a cluster and its same-named atom are one defect
                failed.add(name)
                rep.defects.append(Defect(verdict, "", name, "block", note))

    names = parse_place(place_text)
    for name in names:
        rep.blocks += 1
        count("block", name)
        for atom in atoms.get(name, []):
            if atom not in seen:
                seen.add(atom)
                count("atom", atom)
    for name in sorted(atoms.keys() - set(names)):
        rep.defects.append(Defect("unplaced", "", name, "block", "in-net-not-in-place"))
    if net_text is not None:
        for name in sorted(set(names) - atoms.keys()):
            rep.defects.append(Defect("unnetted", "", name, "block", "in-place-not-in-net"))
    return rep


def vpr_command(vpr: Path, arch: Path, blif: Path, net: Path, place: Path) -> list[str]:
    """Pack and place only (no routing), the channel width fixed; the files are kept."""
    return [
        str(vpr), str(arch), str(blif), "--pack", "--place", "--route_chan_width", CHAN_WIDTH,
        "--seed", "1", "--net_file", str(net), "--place_file", str(place),
    ]


def run_vpr(
    vpr: Path, arch: Path, blif: Path, work: Path, timeout: float = VPR_TIMEOUT
) -> tuple[Path, Path]:
    """Runs VPR in ``work``; (net file, place file). A failing VPR is a ToolError. Heavy: it
    refuses to run unless ``ODIN3_HEAVY_OK=1`` (``tools/heavy-run`` sets it)."""
    if os.environ.get("ODIN3_HEAVY_OK") != "1":
        raise ToolError("vpr: refusing to run outside tools/heavy-run (ODIN3_HEAVY_OK is not 1)")
    net, place = work / "d.net", work / "d.place"
    try:
        done = subprocess.run(
            vpr_command(vpr, arch, blif, net, place),
            capture_output=True, text=True, timeout=timeout, cwd=work, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CrashError(f"vpr: {exc}") from exc
    if done.returncode != 0 or not place.exists() or not net.exists():
        lines = (done.stdout + done.stderr).splitlines()
        errors = [ln for ln in lines if "rror" in ln]
        raise ToolError("vpr: " + " | ".join((errors or lines)[-3:]))
    return net, place
