"""lower-equiv: ``lower`` refines its input, proved in Yosys 0.55 (3B spec 14.4; plan Task 9).

Usage::

    lower-equiv --odin3 O --yosys Y [--top T] [--pre CMD]... [--lower-args A] [--techlib L]
                [--parmys-techlibs DIR] [--parmys-exact] [--expect F] [--root R] [--mem-cap C]
                [--work DIR] [--each] FILE...

Per design (the FILEs together; with ``--each``, every FILE is its own design):

1. Odin III: ``read_verilog --nolatches FILE...; hierarchy; <pre>; lower --lower-snapshot DIR
   <A>`` (``pre`` defaults to ``proc; opt``), which writes ``lower-before.il`` (``write_rtlil`` of
   the design as ``lower`` found it) and ``lower-after.blif`` (``write_blif`` of the bit cells);
2. gold: ``read_rtlil lower-before.il`` with the declared synchronous-conversion maps of spec 6.3
   (``maps/``, the asynchronous flip-flops of decision 19) applied, so the divergence of decision
   19 is the *specification*, not a failure; gate: ``read_blif -wideports lower-after.blif``;
3. Yosys 0.55 proves the gate refines the gold with 2F's refinement script (``opt_equiv.Checker``:
   ``equiv_make gold gate eq; equiv_simple -undef -seq 4; equiv_induct -undef; equiv_status
   -assert``) and its clock-aware lowering family (``async2sync``, ``techmap``, ``memory``,
   ``clk2fflogic``): an x in the gold is a don't-care, so ``$shiftx``, ``$pmux`` with several
   selects and out-of-range ``$mem`` reads are judged correctly, and a flipped edge, a wrong
   clock net or a wrong function is a counterexample.

Every hard-block instance is declared at the widths it connects, so ``expose -evert`` compares
every bit of its inputs and shares every bit of its outputs; a hard block that gold and gate both
hold under one name with another type or other port widths is a difference (``prepare_boxes``).
The induction has no base case, so a passed proof is ``ok`` only when a bounded check from the
initial state (``init`` values honoured, other flip-flops undefined on both sides, so every
starting value is covered) finds no difference on any wire both sides name; a failed proof is a
difference only when a bounded check finds a trace on an output, else ``unproven`` unless the BLIF
cec proves the pair (which then needs the base case too; ``refine``, ``base_case``).
A refinement that the SAT flow cannot decide inside 60 s is ``unproven`` (exit 4), never ``ok``.
A difference fails unless ``--expect F`` lists it: lines ``<path> refine <reason>`` (``#``
comments), ``path`` relative to ``--root`` (else the file name); a listed difference that does not
occur fails too (a stale entry). A design whose snapshot holds no word cell compared nothing:
``NOTHING TO COMPARE`` (exit 1). The last line of every run is ``lower-equiv: compared N of M
designs (U unproven, E empty)``: N counts the designs with a verdict, proved or not a refinement
(the corpus sweeps sum it and fail below a floor, 3B-4 review H3).

Exit codes (one convention across the opt tools): 0 every design refines; 1 a design does not,
compared nothing, or a stale entry; 2 usage, or Odin III or Yosys exited with an error; 3 hard: a
child crashed, was killed by a signal or its memory cap, or timed out (never absorbed by an expect
list); 4 unproven (a SAT size limit; no design failed); 77 skipped (an ASan child and no
systemd-run). The work directory is made under ``$ODIN3_LOWER_WORK`` (else ``$TMPDIR``) and is
never removed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import clock_sig
import lower_common as lc
import lower_diff as ld
import memcap
import opt_equiv

TOOL = "lower-equiv"
EXIT_UNPROVEN = 4
BEFORE_NAME = "lower-before.il"
AFTER_NAME = "lower-after.blif"
TOP_LINE = "attribute \\top 1\n"
# The word cell types that the ``maps/`` flip-flop conversions rewrite (spec 6.3): only a design
# that holds one needs the techmap step.
MAPPED_TYPES = frozenset({"$adff", "$adffe", "$aldff", "$aldffe", "$dffsr", "$dffsre"})
MEM_TYPES = frozenset({"$mem", "$mem_v2"})


@dataclass
class Verdict:
    """One design: ``ok``, ``differs`` (``detail`` is Yosys's last lines), ``unproven`` or
    ``empty`` (nothing to compare)."""

    kind: str
    detail: str = ""
    cells: int = 0


def snapshot_script(files: Sequence[str], top: str | None, techlib: str | None,
                    pre: Sequence[str], lower_args: str, snap: Path) -> str:
    """Step 1's Odin III ``-p`` script."""
    front = lc.read_front(files, top, techlib, True)
    lower = f"lower --lower-snapshot {lc.quote(str(snap))} {lower_args}".rstrip()
    return "; ".join([*front, *pre, lower])


def snapshot(odin3: str, script: str, top: str | None, snap: Path, cap: int) -> tuple[Path, Path]:
    """Runs Odin III in ``snap``; (before.il, after.blif). ``ToolError`` when it fails or wrote no
    file."""
    done = lc.capped("odin3", [lc.exe(odin3), *lc.odin_args(top), "-p", script], cap, cwd=snap)
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode != 0:
        raise lc.ToolError(f"odin3 exit {done.returncode}: {lc.why(log)}")
    before, after = snap / BEFORE_NAME, snap / AFTER_NAME
    for path in (before, after):
        if not path.is_file() or not path.read_text(encoding="utf-8").strip():
            raise lc.ToolError(f"odin3 wrote no {path.name}: {lc.why(log)}")
    return before, after


def synchronous_maps(parmys: Path | None, exact: bool) -> list[Path]:
    """The declared-reference maps of spec 6.3 (``--parmys-exact``: Parmys's enable-over-reset)."""
    names = [ld.PARMYS_EXACT.get(name, name) if exact else name for name in ld.FF_MAPS]
    return [ld.resolve_map(name, parmys) for name in names]


def run_yosys(yosys: str, script: str, work: Path, cap: int) -> None:
    done = lc.capped("yosys", [lc.exe(yosys), "-p", script], cap, cwd=work)
    if done.returncode != 0:
        raise lc.ToolError(f"yosys exit {done.returncode}: "
                           f"{lc.why((done.stdout or '') + (done.stderr or ''))}")


def make_gold(yosys: str, before: Path, parmys: tuple[Path | None, bool], work: Path, cap: int
              ) -> Path:
    """The gold: ``before`` with the maps applied when it holds an asynchronous cell (else a copy
    of it). Written to ``gold.il``. ``parmys`` = (Parmys's techlibs directory, exact)."""
    text = before.read_text(encoding="utf-8")
    gold = work / "gold.il"
    types = lc.cell_types(text)
    # Yosys's ``memory`` (memory_map) makes a read past SIZE alias a word (the address bits
    # above the words are ignored); the spec of ``$mem`` (and of lower) is x there, a don't-care
    # of the gold. ``memory_memx`` writes the x in; ``memory_map`` then maps the memories at once,
    # since ``memory`` (memory_dff) would merge an output flip-flop through the x mux and lose it
    # (multi_edge_reader_writer.v).
    memx = bool(types & MEM_TYPES)
    if not types & MAPPED_TYPES and not memx:
        gold.write_text(text, encoding="utf-8")
        return gold
    maps = synchronous_maps(*parmys) if types & MAPPED_TYPES else []
    steps = [f"read_rtlil {before}", *(["memory_memx", "memory_map"] if memx else []),
             *(f"techmap -map {m}" for m in maps), f"write_rtlil {gold}"]
    run_yosys(yosys, "; ".join(steps), work, cap)
    return gold


PORT_DECL = re.compile(
    r"^  wire (?P<mid>.*?)\s*(?P<dir>input|output) (?P<num>\d+) (?P<name>\\\S+)$")
WIDTH_WORD = re.compile(r"\bwidth (\d+)")
OFFSET_WORD = re.compile(r"\boffset (-?\d+)")
END_LINE = "\nend\n"


def module_span(text: str, top: str) -> tuple[int, int]:
    """(start, end) offsets of the body of module ``top`` in an RTLIL text (after its header
    line, up to its ``end``); (-1, -1) when it is absent."""
    head = f"\nmodule {top}\n"
    start = text.find(head)
    if start < 0:
        return -1, -1
    start += len(head)
    return start, text.index(END_LINE, start) + 1


def port_shapes(body: str) -> dict[str, tuple[str, str, int, int, str]]:
    """name -> (direction, port number, width, offset, the wire's flags) of a module's ports."""
    shapes: dict[str, tuple[str, str, int, int, str]] = {}
    for line in body.splitlines():
        found = PORT_DECL.match(line)
        if found is None:
            continue
        mid = found.group("mid")
        width = WIDTH_WORD.search(mid)
        offset = OFFSET_WORD.search(mid)
        shapes[found.group("name")] = (found.group("dir"), found.group("num"),
                                       int(width.group(1)) if width else 1,
                                       int(offset.group(1)) if offset else 0, mid)
    return shapes


def rename_signal(line: str, old: str, new: str) -> str:
    """``line`` with the signal token ``old`` replaced by ``new``. Only the signal operands of a
    ``connect`` are touched (never a cell's name, type or port names)."""
    stripped = line.lstrip()
    if not stripped.startswith("connect "):
        return line
    indent = line[:len(line) - len(stripped)]
    words = stripped.split(" ")
    first = 2 if len(indent) >= len("    ") else 1  # a cell's `connect \PORT sig`: sig from 2
    pattern = re.compile(rf"(?<!\S){re.escape(old)}(?!\S)")
    tail = pattern.sub(lambda _: new, " ".join(words[first:]))
    return indent + " ".join(words[:first]) + " " + tail


def bridge_port(body: str, name: str, gold: tuple[str, str, int, int, str]) -> str:
    """``body`` (a gate module) with its wide port ``name`` re-exposed with the gold's shape (low
    index and width), or unchanged when it already has that shape or lacks one of the bits (a
    missing bit is a real difference: the port match then reports it)."""
    direction, number, width, offset, flags = gold
    mine = port_shapes(body).get(name)
    if mine is None or mine[2:4] == (width, offset) or mine[0] != direction:
        return body
    low = offset - mine[3]  # the gate's raw index of the gold's lowest bit
    if low < 0 or low + width > mine[2]:
        return body
    wide = f"{name}$wide"
    rewritten = []
    for line in body.splitlines():
        if PORT_DECL.match(line) and line.endswith(f" {name}"):
            line = f"  wire width {mine[2]} offset {mine[3]} {wide}" if mine[3] else \
                f"  wire width {mine[2]} {wide}"
        else:
            line = rename_signal(line, name, wide)
        rewritten.append(line)
    span = f"[{low + width - 1}:{low}]" if width > 1 else f"[{low}]"
    here, there = (name, f"{wide} {span}") if direction == "output" else (f"{wide} {span}", name)
    rewritten += [f"  wire {flags} {direction} {number} {name}", f"  connect {here} {there}"]
    return "\n".join(rewritten) + "\n"


def normalise_ports(gate: str, gold: str, top: str) -> str:
    """The gate's module ``top`` with every port given the gold's width and low index.

    ``read_blif -wideports`` makes a width-8 port ``leds`` of the bits ``leds[1]``..``leds[7]``
    of a gold ``output [7:1] leds``; the equivalence flow matches ports by name, width and offset,
    so a correct lowering would be reported as a difference. Each wide port is kept as
    ``leds$wide`` and bridged to a new port of the gold's shape (bit for bit)."""
    g_start, g_end = module_span(gold, top)
    start, end = module_span(gate, top)
    if g_start < 0 or start < 0:
        return gate
    body = gate[start:end]
    for name, shape in port_shapes(gold[g_start:g_end]).items():
        body = bridge_port(body, name, shape)
    return gate[:start] + body + gate[end:]


def blackbox_modules(text: str) -> dict[str, str]:
    """name -> RTLIL text (attributes included) of every ``blackbox`` module of an RTLIL text: the
    hard blocks (``single_port_ram``, ``dual_port_ram``, ``multiply``) a tech library declares."""
    found: dict[str, str] = {}
    lines = text.splitlines(keepends=True)
    attrs: list[str] = []
    number = 0
    while number < len(lines):
        line = lines[number]
        if line.startswith("attribute "):
            attrs.append(line)
        elif line.startswith("module "):
            end = number
            while end < len(lines) and lines[end] != "end\n":
                end += 1
            if "attribute \\blackbox 1\n" in attrs:
                found[line.split()[1]] = "".join([*attrs, *lines[number:end + 1]])
            attrs = []
            number = end
        else:
            attrs = []
        number += 1
    return found


def add_blackboxes(gate: str, gold: str) -> str:
    """``gate`` plus the gold's black-box modules it lacks. ``read_blif`` makes a hard block's
    ``.subckt`` a cell of an undeclared type, and ``expose -evert`` (which lifts a block's ports
    out so that the check compares what drives its inputs) takes a port's direction from the
    module: without it the gate exposes nothing, ``equiv_make`` cannot match the gold's
    ``<instance>.<port>`` outputs and reports a gate that does not refine (ch_intrinsics,
    mkPktMerge, every micro with a ``single_port_ram``). The module is only a declaration."""
    out = gate if gate.endswith("\n") else gate + "\n"
    for name, module in blackbox_modules(gold).items():
        if module_span(out, name)[0] < 0 and f"cell {name} " in out:
            out += module
    return out


WIRE_DECL = re.compile(r"^  wire (?P<mid>.*?)\s*(?P<name>\\\S+)$")
STORAGE_NAME = re.compile(
    r"^\\(?P<stem>.+)\.[a-z]+_[a-z]?(?P<index>\d+)(?:_(?P<dup>\d+))?$")
STORAGE_Q = "    connect \\Q "
MODULE_LINE = re.compile(r"^module (\\\S+)$", re.M)


def is_storage(kind: str) -> bool:
    """True for a word or bit storage cell type (flip-flop, latch, SR) with a ``Q`` output."""
    return kind.startswith("$") and ("ff" in kind or "latch" in kind or kind == "$sr")


SLICE = re.compile(r"(\\\S+)(?: \[(\d+)(?::(\d+))?\])?")


def driven_bits(q_signal: str) -> dict[str, list[int]]:
    """wire -> the bit indices (low to high) that a gold cell's ``Q`` signal drives: a wire, a
    bit or slice of one, or a concatenation of those. Constants and anything else add nothing."""
    found: dict[str, list[int]] = {}
    for name, high, low in reversed(SLICE.findall(q_signal)):
        if high == "":
            found.setdefault(name, []).append(-1)  # the whole wire (width unknown here)
        else:
            found.setdefault(name, []).extend(range(int(low or high), int(high) + 1))
    return found


def gold_driven(body: str) -> dict[str, list[int]]:
    """wire -> the sorted bit indices that the storage cells of a gold module body drive; a whole
    wire is listed as ``range(width)``."""
    widths = wire_widths(body)
    found: dict[str, set[int]] = {}
    for _, q_signal in storage_cells(body):
        for name, bits in driven_bits(q_signal).items():
            width = widths.get(name, 0)
            found.setdefault(name, set()).update(range(width) if bits == [-1] else bits)
    return {name: sorted(bits) for name, bits in found.items()}


def gold_driver_counts(body: str) -> dict[str, int]:
    """wire -> how many storage cells of a gold module body drive some bit of it."""
    found: dict[str, int] = {}
    for _, q_signal in storage_cells(body):
        for name in driven_bits(q_signal):
            found[name] = found.get(name, 0) + 1
    return found


def storage_cells(body: str) -> list[tuple[str, str]]:
    """(cell name, Q signal) of the ``$dff`` and ``$dlatch`` cells of a module body."""
    found: list[tuple[str, str]] = []
    lines = body.splitlines()
    for number, line in enumerate(lines):
        words = line.split()
        if len(words) != 3 or words[0] != "cell" or not is_storage(words[1]):
            continue
        end = number + 1
        while end < len(lines) and lines[end] != "  end":
            end += 1
        for conn in lines[number + 1:end]:
            if conn.startswith(STORAGE_Q):
                found.append((words[2], conn[len(STORAGE_Q):]))
    return found


def gate_storage(body: str) -> dict[str, list[tuple[tuple[int, int], str]]]:
    """stem -> [((index, dup), Q signal)] of the storage cells of a gate module named
    ``<stem>.<kind>_<index>[_<dup>]`` (the name ``lower`` gives the bit cells of the register
    ``stem``; ``dup`` numbers the cells that would otherwise share a name)."""
    found: dict[str, list[tuple[tuple[int, int], str]]] = {}
    for name, q_signal in storage_cells(body):
        named = STORAGE_NAME.match(name)
        if named is not None:
            found.setdefault(named.group("stem"), []).append(
                ((int(named.group("index")), int(named.group("dup") or 0)), q_signal))
    return found


def wire_widths(body: str) -> dict[str, int]:
    """name -> width of the wires a module body declares."""
    widths: dict[str, int] = {}
    for line in body.splitlines():
        found = WIRE_DECL.match(line)
        if found is not None:
            width = WIDTH_WORD.search(found.group("mid"))
            widths[found.group("name")] = int(width.group(1)) if width else 1
    return widths


def pair_registers(gate: str, gold: str) -> str:
    """``gate`` with each register of the gold that it can recognise by the gold's name.

    The BLIF keeps no internal net name, so the gate's flip-flops drive ``$n123`` and the SAT
    flow's induction cannot pair them with the gold's: a register that feeds itself or a hard
    block fails to prove, whatever the lowering. ``lower`` names the bit cells of a register
    ``<wire>.dff_p<n>`` with ``n`` rising with the bit, so a gold wire of width W with exactly W
    such cells in the same module gets a public gate wire of that name and width, joined to the
    cells' Q bits in order (the bits the gold's own storage cells drive when some are unused and
    were dropped). ``equiv_make`` then compares the two (and the induction uses them as
    the invariant). The pairing is a hint and no assumption: a wrong guess is an ``$equiv`` that
    fails to prove, never a proof."""
    out = gate
    for module in MODULE_LINE.findall(gate):
        g_start, g_end = module_span(gold, module)
        start, end = module_span(out, module)
        if g_start < 0 or start < 0:
            continue
        body = out[start:end]
        gold_body = gold[g_start:g_end]
        gold_wires, have, driven = wire_widths(gold_body), wire_widths(body), gold_driven(gold_body)
        drivers = gold_driver_counts(gold_body)
        extra = ""
        for stem, cells in sorted(gate_storage(body).items()):
            name = "\\" + stem
            if name in have or name not in gold_wires:
                continue
            # Several gold cells drive the wire: the bit order is a guess (sha.v: `cmd` is the Q
            # of four gold cells, `cmd.dff_p2`, `_p2_1`, `_p2_2`, `_p5`: the cell index is the
            # bit within its cell, not within the register), and a wrong name is a false
            # difference in the base case.
            if drivers.get(name, 0) > 1:
                continue
            bits = driven.get(name) or (list(range(gold_wires[name]))
                                        if gold_wires[name] == len(cells) else [])
            if len(bits) != len(cells):
                continue
            width = gold_wires[name]
            extra += f"  wire {f'width {width} ' if width > 1 else ''}{name}\n"
            for bit, (_, sig) in zip(bits, sorted(cells), strict=True):
                target = f"{name} [{bit}]" if width > 1 else name
                extra += f"  connect {target} {sig}\n"
        if extra:
            out = out[:end] + extra + out[end:]
    return out


def make_gate(yosys: str, after: Path, gold: tuple[Path, str], work: Path, cap: int) -> Path:
    """The gate: Yosys's reading of the BLIF as RTLIL (``gate.il``), the module ``top`` marked and
    its ports in the gold's shape. ``gold`` = (gold.il, the top module's RTLIL name)."""
    gate = work / "gate.il"
    script = f"read_blif -wideports {after}; opt_clean -purge; write_rtlil {gate}"
    run_yosys(yosys, script, work, cap)
    text = gate.read_text(encoding="utf-8")
    top = gold[1]
    marker = f"\nmodule {top}\n"
    if marker not in text:
        raise lc.ToolError(f"the lowered BLIF has no module {top[1:]}")
    gold_text = gold[0].read_text(encoding="utf-8")
    text = add_blackboxes(pair_registers(normalise_ports(text, gold_text, top), gold_text),
                          gold_text)
    gate.write_text(text.replace(marker, f"\n{TOP_LINE}module {top}\n", 1), encoding="utf-8")
    return gate


def cec_gold_and_blif(yosys: str, gold: Path, after: Path, work: Path, cap: int) -> tuple[str, str]:
    """The BLIF cec (``equiv-check``) of the gold, lowered to bits by Yosys, against the lowered
    BLIF as Odin III wrote it (with the bit gates' models appended): (verdict, text)."""
    gold_blif = work / "lower.gold.blif"
    script = opt_equiv.cec_lowering_script(gold, gold_blif)
    done = lc.capped("yosys (cec lowering)", [lc.exe(yosys), "-p", script], cap, cwd=work)
    if done.returncode != 0:
        return opt_equiv.CEC_UNPROVEN, "BLIF cec: the gold's lowering failed: " + lc.why(
            (done.stdout or "") + (done.stderr or ""))
    gate_blif = work / "lower.gate.blif"
    gate_blif.write_text(lc.with_gate_models(after.read_text(encoding="utf-8")), encoding="utf-8")
    return cec_blifs(gold_blif, gate_blif, cap, work)


def cec_blifs(gold: Path, gate: Path, cap: int, work: Path) -> tuple[str, str]:
    """``equiv-check`` with undriven nets (the x of a memory read past its words) tied to 0. A
    "proved" is a proof: the gate equals one resolution of the gold's x, and every resolution is a
    legal behaviour of the gold. A "differs" with x in either side is not a counterexample
    (another resolution might match), so it is reported as a difference of the cec only."""
    try:
        done = lc.capped("equiv-check", [lc.exe(str(opt_equiv.EQUIV_CHECK)), "--allow-undriven",
                                         str(gold), str(gate)], cap, cwd=work,
                         timeout=opt_equiv.CEC_TIMEOUT_SECONDS)
    except lc.CrashError as exc:
        return opt_equiv.CEC_UNPROVEN, f"BLIF cec: {exc}"
    text = lc.why((done.stdout or "") + (done.stderr or ""))
    if done.returncode == 0:
        return opt_equiv.CEC_PROVED, ""
    if done.returncode == 1:  # (with x tied to 0 a false alarm is possible; the SAT flow failed)
        return opt_equiv.CEC_DIFFERS, f"BLIF cec: not equivalent: {text}"
    return opt_equiv.CEC_UNPROVEN, f"BLIF cec: no verdict (exit {done.returncode}): {text}"


# ---- Hard blocks at their real widths (soundness review C1, M1) ------------------------------
#
# ``expose -evert`` (``opt_equiv.EVERT_BLACKBOXES``) gives each new port the width of the black
# box's *declaration* and cuts the instance's connection to it (Yosys 0.55 expose.cc:
# ``add_new_wire(.., p->width)``, ``sig.extend_u0(w->width)``). A tech library declares
# ``single_port_ram`` with ``dynports`` and width-1 ``data``/``out``, so only bit 0 of the data was
# compared and ``out[N-1:1]`` became undriven, an x, a don't care in the gold: a wrong data or
# read bit above 0 passed. Every hard-block instance of the flattened top therefore gets a type of
# its own, ``<type>$o3w<k>``, declared with exactly the ports the instance connects at the widths it
# connects them (one declaration per distinct (type, widths), the same on both sides), and the
# instances that gold and gate share by name must agree on type and port widths. Only the widths
# are compared: the gate is read from BLIF, which has no ``.param``, so a parameter of the gate
# side does not exist (no library cell has a parameter that does not also change a port width:
# a ``DATA_WIDTH`` change shows as a width change).
BOX_TAG = "$o3w"
USER_CELL = re.compile(r"^\s+cell \\", re.M)


@dataclass(frozen=True)
class BoxUse:
    """One hard-block instance: its type and the width of each port it connects."""

    kind: str
    widths: tuple[tuple[str, int], ...]


def rtlil_id(name: str) -> str:
    """An RTLIL identifier from a ``write_json`` name (which drops the ``\\`` of a public one)."""
    return name if name.startswith(("$", "\\")) else "\\" + name


def flat_probe(yosys: str, il: Path, tag: str, work: Path, cap: int) -> tuple[Path, dict[str, Any]]:
    """(``<tag>.flat.il``, its ``write_json`` design): ``il`` flattened (after ``proc`` when it
    holds processes), as the refinement script does first anyway."""
    flat, dump = work / f"{tag}.flat.il", work / f"{tag}.flat.json"
    procs = "proc; " if opt_equiv.has_processes(il.read_text(encoding="utf-8")) else ""
    run_yosys(yosys, f"read_rtlil {il}; {procs}flatten; write_rtlil {flat}; write_json {dump}",
              work, cap)
    design: dict[str, Any] = json.loads(dump.read_text(encoding="utf-8"))
    return flat, design


def box_uses(design: dict[str, Any], top: str, boxes: set[str]) -> dict[str, BoxUse]:
    """instance name -> ``BoxUse`` of the hard blocks of the flattened ``top``. ``ToolError`` for a
    user cell whose type has no module: ``expose -evert`` would drop it and leave its outputs
    undriven (an x, a don't care), so it would never be compared."""
    modules = {rtlil_id(name) for name in design["modules"]}
    top_name = top[1:] if top.startswith("\\") else top
    for name, module in design["modules"].items():  # (a keep_hierarchy module stays unflattened)
        if name != top_name and any(rtlil_id(c["type"]) in boxes
                                     for c in module.get("cells", {}).values()):
            raise lc.ToolError(f"module {rtlil_id(name)} holds a hard block after flatten")
    cells = design["modules"][top_name]["cells"]
    uses: dict[str, BoxUse] = {}
    for name, cell in cells.items():
        kind = rtlil_id(cell["type"])
        if kind.startswith("$"):
            continue
        if kind not in modules:
            raise lc.ToolError(f"cell {rtlil_id(name)} has the undeclared type {kind}")
        if kind in boxes:
            uses[rtlil_id(name)] = BoxUse(
                kind, tuple(sorted((rtlil_id(port), len(bits))
                                   for port, bits in cell["connections"].items())))
    return uses


def box_mismatch(gold: dict[str, BoxUse], gate: dict[str, BoxUse]) -> str:
    """The first hard block that gold and gate both hold under one name but with another type or
    other port widths; '' when there is none. Parameters are not compared: the gate side is read
    from BLIF, which carries no ``.param`` (a parameter that matters changes a port width)."""
    for name in sorted(gold.keys() & gate.keys()):
        mine, theirs = gold[name], gate[name]
        if mine.kind != theirs.kind or mine.widths != theirs.widths:
            return (f"hard block {name}: gold {mine.kind[1:]} {dict(mine.widths)}, "
                    f"gate {theirs.kind[1:]} {dict(theirs.widths)}")
    return ""


def box_declaration(module: str, name: str, widths: dict[str, int]) -> str:
    """The black-box ``module`` text renamed ``name`` with exactly the ports in ``widths``, at
    those widths. ``ToolError`` when the instance connects a port the declaration lacks."""
    lines, number = [], 0
    declared = set(port_shapes(module))
    if not set(widths) <= declared:
        raise lc.ToolError(f"a hard block connects {sorted(set(widths) - declared)}, which its "
                           "declaration lacks")
    for line in module.splitlines():
        found = PORT_DECL.match(line)
        if line.startswith("module "):
            line = f"module {name}"
        elif found is not None:
            port = found.group("name")
            if port not in widths:
                continue
            number += 1
            size = f"width {widths[port]} " if widths[port] > 1 else ""
            line = f"  wire {size}{found.group('dir')} {number} {port}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def retype_boxes(text: str, top: str, uses: dict[str, BoxUse], names: dict[tuple[str, tuple[
        tuple[str, int], ...]], str]) -> str:
    """``text`` (a flattened RTLIL design) with each hard-block instance of ``top`` given its
    specialised type from ``names`` ((type, widths) -> new type)."""
    start, end = module_span(text, top)
    body = []
    for line in text[start:end].splitlines(keepends=True):
        words = line.split()
        if len(words) == 3 and words[0] == "cell" and words[2] in uses:
            use = uses[words[2]]
            line = line.replace(f"cell {use.kind} ", f"cell {names[(use.kind, use.widths)]} ", 1)
        body.append(line)
    return text[:start] + "".join(body) + text[end:]


def prepare_boxes(yosys: str, gold: Path, gate: Path, top: str, work: Path, cap: int
                  ) -> tuple[Path, Path, str]:
    """(gold, gate, difference) with every hard block declared at its instance's widths (C1). The
    two paths are unchanged when neither side has a user cell; ``difference`` is non-empty when a
    shared hard block differs in type or widths (M1)."""
    texts = [gold.read_text(encoding="utf-8"), gate.read_text(encoding="utf-8")]
    if not any(USER_CELL.search(text) for text in texts):
        return gold, gate, ""
    modules = blackbox_modules(texts[0])
    probes = [flat_probe(yosys, il, f"box.{tag}", work, cap)
              for il, tag in ((gold, "gold"), (gate, "gate"))]
    uses = [box_uses(design, top, set(modules)) for _, design in probes]
    names: dict[tuple[str, tuple[tuple[str, int], ...]], str] = {}
    decls = ""
    for use in sorted({(u.kind, u.widths) for side in uses for u in side.values()}):
        names[use] = f"{use[0]}{BOX_TAG}{len(names)}"
        decls += "attribute \\blackbox 1\n" + box_declaration(modules[use[0]], names[use],
                                                              dict(use[1]))
    out = []
    for (flat, _), side, tag in zip(probes, uses, ("gold", "gate"), strict=True):
        path = work / f"box.{tag}.il"
        path.write_text(retype_boxes(flat.read_text(encoding="utf-8"), top, side, names) + decls,
                        encoding="utf-8")
        out.append(path)
    return out[0], out[1], box_mismatch(uses[0], uses[1])


# ---- Bounded check from the initial state (soundness review H1, H2) ---------------------------
#
# ``equiv_simple``/``equiv_induct`` prove that gold and gate never diverge once their named wires
# agree; neither starts from the initial state (equiv_induct's base case only asks that a
# consistent trace exists). So (H1) a failed proof is "differs" only when a bounded model check
# from the initial state finds a concrete trace on which an output differs, else "unproven"; and
# (H2) a passed proof stands only when the same check, run over every public wire both sides
# name (the wires ``equiv_make`` pairs, made outputs by ``expose -shared``), finds no
# difference in the first BASE_STEPS steps: that is the base case the induction lacks.
#
# The model: both sides flattened, hard blocks cut as in the proof, ``memory``, then the clock
# model, ``miter -equiv -ignore_gold_x`` (an x in the gold is a don't care) and ``sat -seq N
# -enable_undef -set-def-inputs``. The initial state honours every ``init`` attribute. Flip-flops
# without one are treated in two ways (N2):
#
# * the base case (the "ok" decision) starts them ALL undefined (``-set-init-undef``, the same x on
#   both sides; three-valued, so it covers every starting value, paired flops included, and a gate
#   that is defined or x where the gold is defined and different fails). x is pessimistic, so a
#   failure here is a doubt, not a difference: it is "unproven" unless a concrete trace confirms it;
# * the output traces (the "differs" decision) are concrete runs: every no-init flop 0 (VTR's
#   power-up value and the BLIF cec's convention for init 2/3), or, as a second start, every no-init
#   flop that gold and gate both name at the top level 1 (``tie_starts``: the same value on both
#   sides), the rest 0. A trace from either start is a real run of both circuits.
#
# Output traces (H1) always use ``clk2fflogic``: every clock a sampled input, exact for any
# number of clocks and asynchronous resets; one step is half a cycle, so BMC_STEPS = 20 covers
# 10 edges of each clock, which finds the shallow bugs of a lowering (a swapped bit, a wrong
# init, a wrong mux). The base case (H2) uses the proof's own steps: ``async2sync`` (one step per
# cycle) when ``clock_sig.same_clocks`` holds, as the proof did, else ``clk2fflogic``.
# ``equiv_simple -seq 4`` proves a cell over at most 5 time frames (equiv_simple.cc: ``step``
# from 4 down to 0) from a free state, the other ``$equiv`` cells cut points, and ``equiv_induct``
# steps from any state where they all hold; so a step t >= 4 is covered by the proof once all named
# wires agree in steps 0..3, and those four are the check: BASE_STEPS = 4.
# (mkPktMerge: 18 s at 4 steps, 48 s at 6.) Either check stops after BMC_TIMEOUT_SECONDS; a
# timeout or any Yosys error is "no answer" (never a trace, never a proof). No ``-verify``: its
# ``log_error`` exits before Yosys flushes stdout, so the model and even the "model found: FAIL"
# line can be lost; without it Yosys exits 0 and the verdict is the final message.
BASE_STEPS = 4
BMC_STEPS = 20
START_ZERO, START_ONES, START_UNDEF = "zero", "ones", "undef"
BMC_TIMEOUT_SECONDS = 60.0
TRACE = "trace"
NO_TRACE = "none"
NO_ANSWER = "unknown"
SAT_FAIL = re.compile(r"SAT proof finished - model found: FAIL!")
SAT_PASS = re.compile(r"SAT proof finished - no model found: SUCCESS!")


def bounded_script(gold: str, gate: str, procs: bool, steps: int, shared: bool,
                   one_clock: bool = False, start: str = START_ZERO) -> str:
    """The Yosys script of the bounded check of two files ``rename_modules`` made gold/gate;
    ``shared``: every public wire both sides name is compared too (the base case);
    ``one_clock``: ``async2sync`` steps (one per cycle) instead of ``clk2fflogic``'s; ``start``:
    ``START_UNDEF`` leaves the flip-flops without ``init`` undefined, else they start at 0.
    The shared wires are exposed before any ``opt_clean`` (``memory`` runs one): a flip-flop that
    drives only an unread public bit, kept on one side and dropped on the other, would else leave
    that wire undriven there (expression_in_chain_modules.v: a carry bit nothing reads)."""
    clocks = "async2sync" if one_clock else "clk2fflogic"
    init = "-set-init-undef" if start == START_UNDEF else "-set-init-zero"
    script = [f"read_rtlil {gold}", f"read_rtlil {gate}", *(["proc"] if procs else []),
              "flatten", opt_equiv.EVERT_BLACKBOXES,
              *(["expose -shared gold gate"] if shared else []), "memory", clocks, "opt_clean",
              "miter -equiv -flatten -make_assert -ignore_gold_x gold gate miter",
              "hierarchy -top miter",
              f"sat -seq {steps} {init} -enable_undef -set-def-inputs "
              "-prove-asserts -show-inputs miter"]
    return "; ".join(script)


WIRE_LINE = re.compile(r"^  wire (?P<mid>.*?)(?P<name>\\\S+)$")
INIT_LINE = re.compile(r"^  attribute \\init (?P<width>\d+)'(?P<bits>[01xzm-]*)$")


def wire_width(mid: str) -> int:
    """The width in the words between ``wire`` and the name of a wire line."""
    words = mid.split()
    return int(words[words.index("width") + 1]) if "width" in words else 1


def start_candidates(text: str, top: str) -> dict[str, int]:
    """name -> width of the non-input wires of module ``top`` that have no defined ``init``."""
    start, end = module_span(text, top)
    found: dict[str, int] = {}
    defined = False
    for line in text[start:end].splitlines():
        init, wire = INIT_LINE.match(line), WIRE_LINE.match(line)
        if init is not None:
            defined = defined or re.search("[01]", init.group("bits")) is not None
        elif wire is not None:
            if not defined and not {"input", "inout"} & set(wire.group("mid").split()):
                found[wire.group("name")] = wire_width(wire.group("mid"))
            defined = False
        elif not line.startswith("  attribute "):
            defined = False
    return found


def tie_starts(gold: str, gate: str, tops: tuple[str, str]) -> tuple[str, str]:
    """(gold, gate) RTLIL texts where every top-level wire that both name at one width, neither
    with a defined ``init`` and not an input, gets ``init`` all ones on both sides: one starting
    value for the paired flip-flops (the others start at 0 in the sat call)."""
    mine, theirs = (start_candidates(text, top) for text, top in zip((gold, gate), tops,
                                                                      strict=True))
    names = {name for name, width in mine.items() if theirs.get(name) == width}
    out = []
    for text, top in zip((gold, gate), tops, strict=True):
        start, end = module_span(text, top)
        body: list[str] = []
        for line in text[start:end].splitlines(keepends=True):
            wire = WIRE_LINE.match(line.rstrip("\n"))
            if wire is not None and wire.group("name") in names:
                width = wire_width(wire.group("mid"))
                while body and INIT_LINE.match(body[-1].rstrip("\n")):  # (an x init)
                    body.pop()
                body.append(f"  attribute \\init {width}'{'1' * width}\n")
            body.append(line)
        out.append(text[:start] + "".join(body) + text[end:])
    return out[0], out[1]


def bounded_check(yosys: str, sides: tuple[Path, Path], work: Path, cap: int,
                  run: tuple[str | None, int, bool, str]) -> tuple[str, str]:
    """(TRACE | NO_TRACE | NO_ANSWER, Yosys's last lines) of the bounded check of ``sides`` (gold,
    gate). ``run`` = (top, steps, shared, start); the base case (``shared``) steps like the proof;
    ``start``: START_ZERO, START_ONES (tied paired flip-flops at 1, the rest 0) or START_UNDEF."""
    top, steps, shared, start = run
    texts = [path.read_text(encoding="utf-8") for path in sides]
    tops = (opt_equiv.top_module(texts[0], top), opt_equiv.top_module(texts[1], top))
    if start == START_ONES:
        texts = list(tie_starts(texts[0], texts[1], tops))
    names = []
    for text, side, name in zip(texts, ("gold", "gate"), tops, strict=True):
        path = work / f"bmc.{side}.il"
        path.write_text(opt_equiv.rename_modules(text, side, name), encoding="utf-8")
        names.append(str(path))
    procs = any(opt_equiv.has_processes(text) for text in texts)
    one_clock = shared and clock_sig.same_clocks(texts[0], texts[1])
    script = bounded_script(names[0], names[1], procs, steps, shared, one_clock, start)
    try:
        done = lc.capped("yosys-bmc", [lc.exe(yosys), "-p", script], cap, cwd=work,
                         timeout=BMC_TIMEOUT_SECONDS)
    except lc.CrashError as exc:
        return NO_ANSWER, str(exc)
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode == 0 and SAT_FAIL.search(log):
        return TRACE, lc.why(log)
    if done.returncode == 0 and SAT_PASS.search(log):
        return NO_TRACE, ""
    return NO_ANSWER, f"bounded check exit {done.returncode}: {lc.why(log)}"


def output_trace(yosys: str, sides: tuple[Path, Path], work: Path, cap: int, top: str | None
                 ) -> tuple[str, str]:
    """(TRACE | NO_TRACE | NO_ANSWER, text) of the concrete runs on the outputs: from the all-0
    start, then from the tied all-1 start (N2). A trace from either is a real difference."""
    answers = [bounded_check(yosys, sides, work, cap, (top, BMC_STEPS, False, start))
               for start in (START_ZERO, START_ONES)]
    for found, text in answers:
        if found == TRACE:
            return found, text
    for found, text in answers:
        if found == NO_ANSWER:
            return found, text
    return NO_TRACE, ""


def refine(yosys: str, sides: tuple[Path, Path], after: Path, work: Path, cap: int,
           top: str | None) -> tuple[str, str]:
    """("ok" | "differs" | "unproven", detail) for gate against gold (``sides``). A crash or a
    tool error of the proof propagates (never a verdict)."""
    gold, gate = sides
    checker = opt_equiv.Checker(yosys, work, cap, True, True, top)
    try:
        ok, last = checker.refines(gold, gate, "lower")
    except opt_equiv.RefineAbort as exc:
        if PORT_MISMATCH.search(str(exc)):  # a port the gate lacks is a gate that does not refine
            return "differs", str(exc)
        raise
    except lc.CrashError as exc:
        if exc.size_limit:  # a SAT timeout or a checker cap kill: no verdict (opt-equiv's rule)
            return "unproven", str(exc)
        raise
    if ok:
        return base_case(yosys, sides, work, cap, top)
    found, text = output_trace(yosys, sides, work, cap, top)
    if found == TRACE:
        return "differs", f"{last} | bounded check from the initial state: {text}"
    # No trace inside BMC_STEPS. The induction cannot pair the registers of a memory, nor those
    # whose BLIF names were lost, nor across clk2fflogic's shadow registers, so a failed proof is
    # no evidence; the BLIF cec (ABC's dsec pairs registers itself) may still prove the pair. Its
    # "not equivalent" ties the x of the gold to 0 and is not a trace either: "unproven". Its
    # proof starts every flip-flop at 0 (init 2/3), so it also needs the base case (N2).
    verdict, cec = cec_gold_and_blif(yosys, gold, after, work, cap)
    if verdict == opt_equiv.CEC_PROVED:
        kind, detail = base_case(yosys, sides, work, cap, top)
        return kind, (detail if kind != "ok" else
                      "proved by the BLIF cec (the SAT flow could not pair the registers)")
    return "unproven", (f"the SAT flow failed ({last.strip()[-120:]}), the bounded check found no "
                        f"trace in {BMC_STEPS} steps ({found}: {text[-200:]}) and {cec}")


def base_case(yosys: str, sides: tuple[Path, Path], work: Path, cap: int, top: str | None
              ) -> tuple[str, str]:
    """The verdict of a proved pair: "ok" when the base case holds for every starting value of
    the flip-flops without ``init`` (H2, N2: undefined on both sides, so x covers all values), else
    "differs" on a concrete output trace, else "unproven" (x is pessimistic, a timeout is no
    answer)."""
    found, text = bounded_check(yosys, sides, work, cap, (top, BASE_STEPS, True, START_UNDEF))
    if found == NO_TRACE:
        return "ok", ""
    trace, detail = output_trace(yosys, sides, work, cap, top)
    if trace == TRACE:
        return "differs", f"the initial states differ: bounded check: {detail}"
    outputs = (f"no output trace in {BMC_STEPS} steps" if trace == NO_TRACE else
               f"the output check gave no answer ({detail[-120:]})")
    return "unproven", (f"the proof passed but its base case from the initial state "
                        f"{'failed' if found == TRACE else 'gave no answer'} on the named wires "
                        f"({text[-200:]}) and {outputs}")


def check_pair(yosys: str, before: Path, after: Path, work: Path, cap: int,
               options: tuple[str | None, Path | None, bool]) -> Verdict:
    """Steps 2 and 3 on a snapshot pair. ``options`` = (top, parmys techlibs, parmys-exact)."""
    top, parmys, exact = options
    cells = ld.word_cells(before.read_text(encoding="utf-8"))
    if cells == 0:
        return Verdict("empty")
    try:
        gold = make_gold(yosys, before, (parmys, exact), work, cap)
        gold_top = opt_equiv.top_module(gold.read_text(encoding="utf-8"), top)
        gate = make_gate(yosys, after, (gold, gold_top), work, cap)
        boxed_gold, boxed_gate, mismatch = prepare_boxes(yosys, gold, gate, gold_top, work, cap)
    except lc.CrashError as exc:
        if exc.size_limit:  # the memory cap killed Yosys while it read or flattened a big design
            return Verdict("unproven", f"{exc} (the design is past the memory cap of the check)",
                           cells)
        raise
    if mismatch:
        return Verdict("differs", mismatch, cells)
    kind, detail = refine(yosys, (boxed_gold, boxed_gate), after, work, cap, top)
    return Verdict(kind, detail, cells)


def check_design(args: argparse.Namespace, files: Sequence[str], run_dir: Path) -> Verdict:
    techlib = str(Path(args.techlib).resolve()) if args.techlib else None
    pre = args.pre if args.pre else list(ld.DEFAULT_PRE)
    snap = Path(tempfile.mkdtemp(prefix="snap-", dir=run_dir))  # fresh: never a stale snapshot
    script = snapshot_script(files, args.top, techlib, pre, args.lower_args, snap)
    before, after = snapshot(args.odin3, script, args.top, snap, args.mem_cap)
    parmys = Path(args.parmys_techlibs) if args.parmys_techlibs else ld.vtr_techlibs()
    return check_pair(args.yosys, before, after, run_dir, args.mem_cap,
                      (args.top, parmys, args.parmys_exact))


PORT_MISMATCH = re.compile(r"Can't match (?:gold|gate) port")
EXPECT_LINE = re.compile(r"^(\S+)\s+refine\s+(\S.*)$")


def parse_expect(text: str) -> dict[str, str]:
    """``<path> refine <reason>`` lines; ``ToolError`` on a malformed one."""
    found: dict[str, str] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        match = EXPECT_LINE.match(line)
        if match is None:
            raise lc.ToolError(f"expect line {number}: want '<path> refine <reason>', got "
                               f"'{raw.strip()}'")
        found[match.group(1)] = match.group(2)
    return found


def report_line(name: str, key: str, verdict: Verdict, table: dict[str, str],
                allow_empty: bool = False) -> tuple[str, int]:
    """(output line, exit code contribution: 0, 1 or 4) of one design."""
    tag = f"{TOOL}: {name}"
    listed = table.get(key)
    if verdict.kind == "ok":
        if listed is not None:
            return f"{tag}: stale entry in the expect list ({listed})", lc.EXIT_FAIL
        return f"{tag}: ok ({verdict.cells} cells)", lc.EXIT_OK
    if verdict.kind == "differs" and listed is not None:
        return f"{tag}: not a refinement, as listed ({listed})", lc.EXIT_OK
    if verdict.kind == "differs":
        return f"{tag}: NOT A REFINEMENT: {verdict.detail}", lc.EXIT_FAIL
    if verdict.kind == "unproven":
        return f"{tag}: UNPROVEN: {verdict.detail}", EXIT_UNPROVEN
    if allow_empty:
        return f"{tag}: nothing to compare (no word cell; allowed)", lc.EXIT_OK
    return f"{tag}: NOTHING TO COMPARE: the snapshot holds no word cell (folded away?)", \
        lc.EXIT_FAIL


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--lower-args", default="", metavar="A", help="arguments of the lower pass")
    ap.add_argument("--pre", action="append", metavar="CMD",
                    help="a command between hierarchy and lower (repeatable; default proc, opt)")
    ap.add_argument("--expect", type=Path, help="designs that are expected not to refine")
    ap.add_argument("--root", type=Path, help="expect-list paths are relative to this directory")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first")
    ap.add_argument("--parmys-techlibs", metavar="DIR",
                    help="Parmys's techlibs (default $VTR_ROOT/parmys/parmys-plugin/techlibs)")
    ap.add_argument("--parmys-exact", action="store_true",
                    help="Parmys's unmodified enable-over-reset maps (spec 16 row 4)")
    ap.add_argument("--mem-cap", type=lc.parse_size, default=lc.DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 3G)")
    ap.add_argument("--work", type=Path, help="the work directory")
    ap.add_argument("--refine-timeout", type=float, default=opt_equiv.REFINE_TIMEOUT_SECONDS,
                    metavar="S", help="seconds for one SAT proof and for one bounded check "
                    "(default 60); a design that needs longer is unproven")
    ap.add_argument("--allow-empty", action="store_true",
                    help="a design with no word cell after the pre commands is not a failure")
    ap.add_argument("--each", action="store_true", help="every FILE is its own design")
    ap.add_argument("files", nargs="+", help="the Verilog (or BLIF) files")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    designs = [[f] for f in files] if args.each else [files]
    table = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
    ld.check_yosys(args.yosys, work, args.mem_cap)
    worst = lc.EXIT_OK
    unproven = False
    kinds: list[str] = []
    for number, design in enumerate(designs):
        run_dir = work / f"d{number:04d}"
        run_dir.mkdir(exist_ok=True)
        verdict = check_design(args, design, run_dir)
        line, code = report_line(Path(design[0]).name, ld.rel_key(design, args.root), verdict,
                                 table, args.allow_empty)
        print(line, flush=True)
        kinds.append(verdict.kind)
        unproven = unproven or code == EXIT_UNPROVEN
        worst = lc.EXIT_FAIL if code == lc.EXIT_FAIL else worst
    compared = sum(kind in ("ok", "differs") for kind in kinds)
    print(f"{TOOL}: compared {compared} of {len(kinds)} designs ({kinds.count('unproven')} "
          f"unproven, {kinds.count('empty')} empty)", flush=True)
    return worst if worst else (EXIT_UNPROVEN if unproven else lc.EXIT_OK)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    opt_equiv.REFINE_TIMEOUT_SECONDS = args.refine_timeout
    global BMC_TIMEOUT_SECONDS
    BMC_TIMEOUT_SECONDS = args.refine_timeout
    work = lc.make_work(args.work, "lower-equiv-")
    try:
        return execute(args, work)
    except lc.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
        return lc.EXIT_CRASH
    except (OSError, lc.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return lc.EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
