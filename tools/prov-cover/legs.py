"""prov-cover legs (6.5b, spec 6 P-2): the odin3 flows of legs (a) to (c) and the join of a typed
dump with the connections of the same design (``write_json``).

Leg (a): the Phase 3 flow of ``tools/vtr-qor`` (``qor_legs._READ_VERILOG`` + ``abc --lut 6``), then
``dump_locs --types``, ``write_json`` (the connections) and ``write_blif --vtr --name-style
provenance``. Leg (b): ``read_blif --hdl-locs`` of that file, dumped and written to JSON again.
Leg (c): the 5.1 LPM flow (``tests/loc/lpm.script``) dumped in memory; ``c2``: ``write_verilog``,
``read_verilog -netlist -icells`` and the dump of the read-back. The exact options are the ones
``docs/PASSES.md`` lists and ``tools/loc-check`` uses.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from loc_check import level_script, parse_techlibs

from prov_cover import (
    ROOT_DIR,
    Cell,
    CrashError,
    Defect,
    Facts,
    LegReport,
    Mod,
    Obj,
    ToolError,
    classify,
    parse_dump,
    written_counts,
)

TIMEOUT = 1800.0
LUT_K = 6
LIB_VTR = ROOT_DIR / "lib" / "vtr.o3lib"
LIB_LPM = ROOT_DIR / "lib" / "lpm.o3lib"
LPM_SCRIPT = ROOT_DIR / "tests" / "loc" / "lpm.script"
TECHLIBS = ROOT_DIR / "tests" / "loc" / "techlib.txt"
INFO_INHERITED = re.compile(r"(\d+) inherited")

# The Phase 3 front half (tools/vtr-qor/qor_legs.py _READ_VERILOG).
FLOW_LUT = (
    "read_techlib {lib}; read_verilog --nolatches --vtr-compat {design}; proc; fsm; opt; flatten; "
    "opt -full; wreduce; hardmap; lower; abc --lut {k}"
)


def run_odin3(odin3: str, script: str, work: Path, flags: Sequence[str] = ()) -> str:
    """``odin3 [flags] -p script``; the combined log. A non-zero exit is a ToolError."""
    try:
        done = subprocess.run(
            [odin3, *flags, "-p", script],
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
            cwd=work,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CrashError(f"odin3: {exc}") from exc
    log = (done.stderr or "") + (done.stdout or "")
    if done.returncode != 0:
        errors = [ln for ln in log.splitlines() if "error" in ln]
        raise ToolError(" | ".join((errors or log.splitlines())[-4:]))
    return log


# --- the connections of a design (write_json) -------------------------------------------------


def _bits(conn: Sequence[Any]) -> tuple[str, ...]:
    return tuple(str(b) for b in conn)


def _is_identity(ctype: str, params: dict[str, Any], width: int) -> bool:
    """A one-input table that copies its input: ``$lut`` with LUT ``10``; a ``$sop`` with the
    single row ``1`` on one input and depth one."""
    if width != 1:
        return False
    if ctype == "$lut":
        return str(params.get("LUT", "")) == "10"
    if ctype == "$sop":
        return str(params.get("TABLE", "")).strip() in ("01", "1")
    return False


def _module_facts(mod: dict[str, Any]) -> Mod:
    names: dict[str, str] = {}
    for name, net in mod.get("netnames", {}).items():
        for bit in _bits(net.get("bits", [])):
            if bit not in names or (net.get("hide_name") == 0 and not name.startswith("$")):
                names[bit] = name
    cells: dict[str, Cell] = {}
    for name, cell in mod.get("cells", {}).items():
        dirs = cell.get("port_directions", {})
        ins: list[str] = []
        outs: list[str] = []
        for pin, conn in sorted(cell.get("connections", {}).items()):
            (outs if dirs.get(pin) == "output" else ins).extend(_bits(conn))
        params = cell.get("parameters", {})
        cells[name] = Cell(
            cell["type"], tuple(ins), tuple(outs), _is_identity(cell["type"], params, len(ins))
        )
    port_in = {
        b
        for p in mod.get("ports", {}).values()
        if p.get("direction") == "input"
        for b in _bits(p["bits"])
    }
    port_out = {
        b
        for p in mod.get("ports", {}).values()
        if p.get("direction") == "output"
        for b in _bits(p["bits"])
    }
    return Mod(cells, frozenset(port_in), frozenset(port_out), names)


def facts_from_json(text: str) -> Facts:
    """The connections of every module of a Yosys-schema JSON file."""
    data = json.loads(text)
    return {name: _module_facts(mod) for name, mod in data.get("modules", {}).items()}


def located_nets(rows: Sequence[Any], facts: Facts) -> set[tuple[str, str]]:
    """(module, net name) of the nets a node with a location drives: the leg-(a) side of the
    lost-location rule."""
    found: set[tuple[str, str]] = set()
    for row in rows:
        mod = facts.get(row.module)
        cell = mod.cells.get(row.name) if mod else None
        if row.kind == "node" and row.src and mod and cell:
            found.update((row.module, mod.net_name(o)) for o in cell.outs)
    return found


# --- the flows --------------------------------------------------------------------------------


@dataclass
class Dumped:
    """One odin3 run's dump and connections."""

    rows: list[Obj]
    facts: Facts
    log: str


def dump_state(work: Path, stem: str, log: str, connections: bool = True) -> Dumped:
    """The dump of ``stem``; its connections only when asked (the LPM legs have no cell that an
    exception class needs them for, and ``write_json`` refuses a design with processes)."""
    rows = parse_dump((work / f"{stem}.txt").read_text(encoding="utf-8"))
    path = work / f"{stem}.json"
    facts = facts_from_json(path.read_text(encoding="utf-8")) if connections else {}
    return Dumped(rows, facts, log)


def tail_commands(stem: str, connections: bool = True) -> str:
    text = f"dump_locs --types {stem}.txt"
    return f"{text}; write_json {stem}.json" if connections else text


def flow_lut(design: Path, libs: Sequence[str]) -> str:
    pre = "; ".join(libs) if libs else f"read_techlib {LIB_VTR}"
    return FLOW_LUT.format(lib=LIB_VTR, design=design, k=LUT_K).replace(
        f"read_techlib {LIB_VTR}", pre, 1
    )


def lut_legs(
    args: argparse.Namespace, odin3: str, design: Path, work: Path
) -> list[tuple[str, LegReport]]:
    libs = parse_techlibs(TECHLIBS).get(design.name, [])
    blif = work / "W.blif"
    script = (
        f"{flow_lut(design, libs)}; {tail_commands('a')}; "
        f"write_blif --vtr --lut-k {LUT_K} --name-style provenance {blif}"
    )
    run_odin3(odin3, script, work)
    a = dump_state(work, "a", "")
    out = [("a", classify(a.rows, a.facts, allow=args.allow_set))]
    if "b" not in args.legs.split(","):
        return out
    pre = "; ".join(libs) if libs else f"read_techlib {LIB_VTR}"
    log = run_odin3(odin3, f"{pre}; read_blif --hdl-locs {blif}; {tail_commands('b')}", work)
    b = dump_state(work, "b", log)
    rep = classify(
        b.rows,
        b.facts,
        allow=args.allow_set,
        inherits=True,
        located=located_nets(a.rows, a.facts),
        written=written_counts(blif.read_text(encoding="utf-8")),
    )
    info = INFO_INHERITED.search(log)
    if info and int(info.group(1)) != rep.exceptions["inherited"]:
        rep.mismatches.append(
            f"inherited: {rep.exceptions['inherited']} in the dump, "
            f"{info.group(1)} in the read_blif INFO line"
        )
    out.append(("b", rep))
    return out


def lpm_legs(
    args: argparse.Namespace, odin3: str, design: Path, work: Path
) -> list[tuple[str, LegReport]]:
    libs = parse_techlibs(TECHLIBS).get(design.name, [])
    subs = {
        "@DESIGN@": str(design),
        "@LIB@": str(LIB_LPM),
        "@FAMILY@": "",
        "@TECHLIB@": "; ".join(libs),
    }
    flow = level_script(LPM_SCRIPT.read_text(encoding="utf-8"), subs)
    verilog = work / "W.v"
    run_odin3(odin3, f"{flow}; {tail_commands('c', False)}; write_verilog {verilog}", work)
    c = dump_state(work, "c", "", False)
    out = [("c", classify(c.rows, c.facts, allow=args.allow_set))]
    pre = "; ".join([f"read_techlib {LIB_LPM}", *libs])
    run_odin3(
        odin3, f"{pre}; read_verilog -netlist -icells {verilog}; {tail_commands('c2', False)}", work
    )
    c2 = dump_state(work, "c2", "", False)
    out.append(("c-rb", classify(c2.rows, c2.facts, allow=args.allow_set)))
    return out


def run_designs(args: argparse.Namespace, work: Path) -> list[tuple[str, str, LegReport]]:
    """The reports of every design and leg; a flow that fails is a ``flow-error`` defect."""
    odin3 = str(Path(args.odin3 or ROOT_DIR / "build" / "debug" / "odin3").resolve())
    wanted = args.legs.split(",")
    bad = [w for w in wanted if w not in ("a", "b", "c")]
    if bad or not args.files:
        raise ToolError(f"bad --legs {bad[0]!r}" if bad else "no designs given")
    files = [f.resolve() for f in args.files]
    if args.smallest_first:
        files.sort(key=lambda f: (f.stat().st_size, f.name))
    items: list[tuple[str, str, LegReport]] = []
    for number, design in enumerate(files):
        run_dir = work / f"{number:04d}-{design.stem}"
        run_dir.mkdir(exist_ok=True)
        flows = []
        if "a" in wanted or "b" in wanted:
            flows.append(("a", lut_legs))
        if "c" in wanted:
            flows.append(("c", lpm_legs))
        for leg, flow in flows:
            try:
                items += [
                    (design.name, name, rep) for name, rep in flow(args, odin3, design, run_dir)
                ]
            except ToolError as exc:
                rep = LegReport()
                rep.defects.append(Defect("flow-error", "", design.name, str(exc)[:200]))
                items.append((design.name, leg, rep))
    return items
