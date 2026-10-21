"""equiv-check: functional equivalence of two BLIF netlists using Berkeley ABC.

Usage::

    equiv-check A.blif B.blif [--abc PATH] [--keep-temp] [--strict-init] [--no-io-normalize]
                [--no-cut] [--match-unnamed] [--x-report] [--blackbox M[,M...]]

Exit 0 equivalent, 1 not equivalent (including mismatched primary I/O sets and black-box
cut-point inputs that are wired differently; the output then holds a line that starts
``equiv-check: NOT equivalent``, and exit 1 means nothing else), 2 usage, input,
unsupported-construct or ABC error, an uncaught internal error, and black-box instances that
cannot be matched one-to-one; 3 only with ``--x-report``: not equivalent, but equivalent outside
the x-cones (below); 4 *unproven*: no verdict either way, a tool limit and not a crash (ABC
timed out or ended ``Networks are UNDECIDED`` on its own engine limit, several clock domains,
or the check failed only where black-box pairs were guessed); 5 *hard*: ABC was killed by a
signal, or this process ran out of memory.

``--x-report`` (2H, 2D spec 14.6 item 4): when the two netlists are not equivalent, recompute
with every output in an x-cone removed and print the removed outputs by name.  Parmys writes a
surviving don't-care x as the net ``unconn`` (``.names unconn`` with no rows: constant 0).  The
x-cone of a netlist is everything the net ``unconn`` reaches going forward, through covers and
through latches (so a latch whose D input is in the cone puts its Q, and the outputs behind it,
in the cone).  An output is removed when it is in the x-cone of *either* netlist; a black box's
probed inputs are outputs like any other.  If the netlists are equivalent on the remaining
outputs the exit code is 3 and ``x-cone: NAME`` lines list the removed ones.  If no output is
in an x-cone, or none is left, or the remainder still differs, the verdict stays "not
equivalent" (exit 1).  The removal is coarse: an output that depends on x for one input vector
only is removed whole.  The cone follows x through a cut black box: when any input pin of a
box is in the cone, every output pin of that box is, so the outputs behind it are removed and
listed as well (a RAM whose address is x has an x read data, whatever the other netlist reads).

Each netlist is flattened in Python into one model (``.subckt`` of models defined in the
same file are inlined; the VTR ``adder`` hard block is replaced by logic; other black boxes
are cut, see below), primary I/O names are normalized so Parmys and Odin II spellings meet,
internal nets get plain names, and ABC runs ``cec`` (no latches) or ``dsec`` (latches) on the
two flat files; if only one side has latches, the other gets a dangling const-0 latch.

Black boxes (cut points).  A black-box instance other than ``adder`` is a ``.subckt`` of a
``.blackbox`` model or a ``.gate`` (Yosys's spelling of a cell instance, parsed like
``.subckt``; ``write_blif -gates`` writes the hard blocks it does not know that way, with no
``.model`` at all, so port directions come from the cell type: the VTR hard blocks' ``out``,
``out1``, ``out2``, ``sumout`` and ``cout``, and Yosys's ``Q``/``QN``/``Y``/``O``).  Each is
cut out of the logic:

- its output bits become fresh primary inputs, shared by the two netlists;
- its input bits (clocks included) become extra primary outputs, compared between the two.

Instances are matched across the files by instance name: the ``.cname`` if the instance has
one, else the first output pin (lowest port, lowest bit) whose net has a stable name (not
``$``-prefixed), as ``out=memtroll.str_out[0]`` (a one-bit port is ``p`` whether written
``p`` or ``p[0]``); the name is normalized like a primary I/O
name.  Matching is exact and never guessed: instances that exist on one side only, two
instances with one name, an instance with no usable name, a different cell type, different
port widths or different ``.param`` lists (a RAM mode, an INIT value: a box's configuration is
part of what it is) all exit 2 with a message listing them.  The cut makes two netlists equivalent
only when everything around the black boxes is equivalent and each box's inputs and outputs
connect identically; a box's behaviour is assumed equal, never checked.  A black box clocked
by a primary clock is fine (its outputs are free inputs), but a clocked black box's internal
state is not modelled: only its connections are compared.  The cut is on whenever black
boxes are present; ``--no-cut`` restores the old behaviour of rejecting them (exit 2, and
``.gate`` is rejected as an unsupported directive).

``--blackbox M[,M...]`` (3D Task 19; the ``abc`` pass changes net names but not its hard blocks):
the instances of the named models (``adder`` included, which is then cut instead of expanded) are
cut as above and paired across the files by ``.cname`` when both have one, else by occurrence order
per model (file order, which ``write_blif`` keeps).  Their outputs become shared primary inputs,
their inputs compared outputs.  A model with a different number of instances in the two files is
exit 2: ``equiv-check: --blackbox: model '<m>' has <x> instances in <A> and <y> in <B>``.  Several
clocks stay unsupported (exit 2) as before.

``--match-unnamed`` (2H; spelled ``--match-dollar`` before) matches every black box that has no
``.cname`` by *simulation signature* instead of by a name: Parmys writes no ``.cname`` for its hard
blocks and numbers their output nets with counters (``$mul~14[0]``,
``dual_port_ram^MEM~0-30^out1[0]``) that differ between two runs.  The signature of a box is its
cell type, its parameters, its port widths and the function of each of its input bits.  Both
netlists are simulated on the same random vectors (biased, primary inputs by normalised name,
latches from their init values, cut boxes as free outputs driven by a pseudo-random function of
their type and input history, see ``bbmatch``), so an input bit's response vector identifies the
function that drives it, even through latches whose names differ.  Boxes are then paired, in
rounds: the boxes whose signature is unique on both sides are anchored (driven by a shared name
from then on, as if they had a ``.cname``) and the simulation repeats, which separates logic that
a stuck cone had made look alike.  When a round anchors nothing more:

- one box with a signature on each side: paired;
- several with one signature on each side whose input nets are the same on every pin (constants
  and buffers looked through; copies of one register, covers with equal inputs and boxes with
  equal inputs counted as one net, a congruence closure) are interchangeable: paired in file
  order, and their outputs are one shared free variable per pin;
- several with one signature but different input functions that simulation cannot tell apart
  (an enable it never raises) are told apart by ABC probes on their input cones when
  ``pair_ram_groups`` runs first (a short budget); what is left is split by the structure of the
  input cones (colour refinement) and paired in file order inside a part: these are *guesses*;
- no partner, or a different count: exit 2, listing the boxes.  A genuine difference upstream of
  a box shows as exit 2 here, not as "not equivalent".

Soundness: the signature only *proposes* a pairing; it is never trusted.  After pairing, each
pair is an ordinary named cut point and the normal ABC check runs: the outputs of paired boxes are
one shared free input, and the input pins of paired boxes are compared as primary outputs.  The
two netlists are called equivalent only if every pair has the same cell type, parameters and port
widths and provably the same function on every input pin, and everything else is equivalent for
any values of the free outputs; since a (deterministic) box with the same inputs has the same
outputs, and the proof held for every value the shared outputs could take, the netlists are
equivalent.  A wrong pairing (boxes of the same type whose inputs differ) makes the proof of some
pin fail, so the verdict is never "equivalent"; sampling can only lose a match, not create a
false one.  The converse is not claimed: when guessed pairs exist and the check fails, the
guess may be at fault, so the result is "inconclusive" (exit 4, unproven) and not "not equivalent"
(a failure outside the guesses and an equivalence are still verdicts; an x-cone result is not: the
cones swallow every pin of a box fed by ``unconn``, so a wrong guess is hidden in them).  As
before, a box's internal behaviour is assumed, not checked (RAM contents, initial state).

Dual-port RAMs (2H, micro sweep of 2026-10-14).  Parmys numbers the ports of a ``dual_port_ram`` in
a hash-table order, so the same memory can come out with ports 1 and 2 exchanged.  Before any
pairing, ``--match-unnamed`` turns every unnamed ``dual_port_ram`` of both netlists to one
canonical orientation (``orient_dual_ports``: the port whose address, data and write-enable
history is smaller becomes port 1).  The two ports are taken as interchangeable, the same kind of
assumption as the RAM contents: a design that differs only in which port wins a same-address
write is called equivalent (``--no-dual-orient`` turns this off).

ABC models every latch as a flip-flop on one implicit clock, so each file must use a single
(edge type, clock) pair -- the clock being a primary input, possibly through buffers -- and
both files must use the same pair (else exit 1).  Level-sensitive (ah/al) and asynchronous
(as) latches and internal clocks are rejected (exit 2), several clock domains are unproven (exit 4,
a tool limit).  Latch init
2/3 (don't care/unknown) or a missing init is treated as 0 with a notice (``--strict-init``
makes it an error).  An undriven net that reaches a primary output or a latch input is an
error (``--allow-undriven`` ties it to 0); a non-black-box model with ports but no cells is
an error ("mark it .blackbox").

ABC lookup order: ``--abc PATH``, ``$ODIN3_ABC``, ``build/*/third_party/abc/abc`` in the
repository (``build/release`` first: a sanitizer build such as ``clangdbg`` runs dsec on the
VTR designs several times slower, and sorting by name used to pick it),
``$VTR_ROOT/build/abc/abc``, ``abc`` on ``PATH``.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blif"))
import bbmatch  # noqa: E402
import blif  # noqa: E402
import rampair  # noqa: E402

TOOL = "equiv-check"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TIMEOUT = 600
LISTED_PREFIX = "?bb:"  # provisional key of an instance of a --blackbox model without a .cname
MATCH_ROUNDS = 12  # most simulate-and-anchor rounds of pair_unnamed

# Verdict strings printed by ABC (see third_party/abc/src):
#   cec  -> Abc_NtkCecFraig, base/abci/abcVerify.c:207,210,233,237 (and :174,:185 strash)
#   dsec -> Fra_FraigSec,    proof/fra/fraSec.c:248,480,633,657,675
# Both spell "Networks are equivalent" / "Networks are NOT EQUIVALENT"; some other ABC
# commands use "NOT equivalent", hence the case-insensitive match.
_NOT_EQUIV_RE = re.compile(r"\bNetworks are NOT EQUIVALENT\b", re.IGNORECASE)
_EQUIV_RE = re.compile(r"\bNetworks are equivalent\b", re.IGNORECASE)
_UNDECIDED_RE = re.compile(r"\bNetworks are UNDECIDED\b", re.IGNORECASE)

XOR3 = [("100", "1"), ("010", "1"), ("001", "1"), ("111", "1")]
MAJ3 = [("11-", "1"), ("1-1", "1"), ("-11", "1")]
# Separator for instance-path prefixes.  A space can never occur inside a parsed BLIF
# token, so prefixed nets cannot collide with original names; all non-port nets are
# renamed before anything is written.
_SEP = " "
_MAX_DEPTH = 1000


class EquivError(Exception):
    """Usage, input or ABC error (exit code 2).  ``detail`` is extra multi-line output."""

    exit_code = 2

    def __init__(self, message: str, detail: str = "") -> None:
        super().__init__(message)
        self.detail = detail


class UnprovenError(EquivError):
    """No verdict, a tool limit and not a crash: ABC timed out (exit code 4)."""

    exit_code = 4


class HardError(EquivError):
    """ABC was killed by a signal (a crash or the memory cap): never a verdict (exit code 5)."""

    exit_code = 5


def notice(msg: str) -> None:
    print(f"{TOOL}: notice: {msg}", file=sys.stderr)


# --------------------------------------------------------------------------- ABC lookup


def _is_exe(path: str | Path) -> bool:
    return os.path.isfile(path) and os.access(path, os.X_OK)


def find_abc(explicit: str | None = None) -> str:
    """Locate the ABC binary (see module docstring for the order).  Raises EquivError."""
    if explicit:
        if not _is_exe(explicit):
            raise EquivError(f"--abc {explicit}: not an executable file")
        return explicit
    env = os.environ.get("ODIN3_ABC")
    if env:
        if not _is_exe(env):
            raise EquivError(f"ODIN3_ABC={env}: not an executable file")
        return env
    found = sorted(REPO_ROOT.glob("build/*/third_party/abc/abc"),
                   key=lambda path: (path.parent.parent.parent.name != "release", str(path)))
    for cand in found:
        if _is_exe(cand):
            return str(cand)
    vtr = os.environ.get("VTR_ROOT")
    if vtr and _is_exe(Path(vtr) / "build" / "abc" / "abc"):
        return str(Path(vtr) / "build" / "abc" / "abc")
    on_path = shutil.which("abc")
    if on_path:
        return on_path
    raise EquivError(
        "ABC not found: pass --abc PATH, set ODIN3_ABC, build the repo "
        "(build/<preset>/third_party/abc/abc), set VTR_ROOT, or put abc on PATH"
    )


# --------------------------------------------------------------------------- I/O names


def normalize_io_name(name: str, top: str) -> str:
    """Map a primary I/O name to a tool-neutral spelling so Parmys and Odin II ports meet.

    HEURISTIC, to be confirmed against real VTR goldens: strips a leading ``<top>^``
    (Odin II hierarchy prefix, ``top`` being the top model name) and rewrites a trailing
    ``~<N>`` (Odin II bit index) to ``[<N>]`` (Yosys/Parmys bit index).  Disable with
    ``--no-io-normalize``.
    """
    prefix = top + "^"
    if name.startswith(prefix):
        name = name[len(prefix):]
    match = re.fullmatch(r"(.*)~(\d+)", name)
    if match:
        name = f"{match.group(1)}[{match.group(2)}]"
    return name


# --------------------------------------------------------------------------- flattening


@dataclass
class BlackBox:
    """A cut black-box instance: its pins as flat nets, keyed by instance name."""

    cell_type: str
    key: str
    line: int
    inputs: dict[str, str]
    outputs: dict[str, str]
    anon: bool = False  # no name: matched by simulation signature (``--match-unnamed``)
    params: tuple[tuple[str, str], ...] = ()
    free: dict[str, str] = field(default_factory=dict)  # output pin -> its free input net
    probes: dict[str, str] = field(default_factory=dict)  # input pin -> its probe output net


@dataclass
class Flat:
    """A flattened netlist ready for ABC: black boxes are cut into ports (``blackboxes``)."""

    path: str
    top: str
    inputs: list[str]
    outputs: list[str]
    names: list[blif.Names] = field(default_factory=list)
    latches: list[blif.Latch] = field(default_factory=list)
    blackboxes: list[BlackBox] = field(default_factory=list)


@dataclass
class _Frame:
    model: blif.Model
    prefix: str
    netmap: dict[str, str]
    path: tuple[str, ...]

    def resolve(self, net: str) -> str:
        mapped = self.netmap.get(net)
        if mapped is not None:
            return mapped
        return self.prefix + net


def _port_bits(ports: list[str], base: str) -> list[str]:
    """Ports named ``base`` or ``base[i]``, ordered by bit index."""
    found: list[tuple[int, str]] = []
    for port in ports:
        match = re.fullmatch(re.escape(base) + r"(?:\[(\d+)\])?", port)
        if match:
            found.append((int(match.group(1) or 0), port))
    return [port for _, port in sorted(found)]


def _adder_shape(model: blif.Model) -> tuple[list[str], list[str], str, list[str], str]:
    """(a bits, b bits, cin, sumout bits, cout) of a VTR ``adder`` model, or EquivError."""
    a, b = _port_bits(model.inputs, "a"), _port_bits(model.inputs, "b")
    cin, s = _port_bits(model.inputs, "cin"), _port_bits(model.outputs, "sumout")
    cout = _port_bits(model.outputs, "cout")
    width = len(a)
    ok = (
        width >= 1 and len(b) == width and len(s) == width and len(cin) == 1
        and len(cout) == 1 and set(model.inputs) == set(a + b + cin)
        and set(model.outputs) == set(s + cout)
    )
    if not ok:
        raise EquivError(f"unsupported black box adder (unexpected ports {model.inputs} "
                         f"-> {model.outputs})")
    return a, b, cin[0], s, cout[0]


def _formal_order(formal: str) -> tuple[str, int]:
    match = re.fullmatch(r"(.*)\[(\d+)\]", formal)
    return (match.group(1), int(match.group(2))) if match else (formal, 0)


def _canonical_formals(formals: list[str]) -> dict[str, str]:
    """Pin name -> name used for matching: a one-bit port is ``p`` whether it is spelled
    ``p`` (Yosys) or ``p[0]`` (Parmys, Odin II); ports of several bits keep their indices."""
    wide = {blif.port_base(f) for f in formals if _formal_order(f)[1] > 0}
    return {f: (blif.port_base(f) if f == blif.port_base(f) + "[0]"
                and blif.port_base(f) not in wide else f) for f in formals}


def _gate_adder_model(cell: blif.Subckt) -> blif.Model:
    """The ``adder`` model a model-less ``.gate adder`` stands for, from its connections."""
    formals = [f for f, _ in cell.conns]
    return blif.Model(
        name="adder", blackbox=True,
        inputs=[f for f in formals if not blif.gate_port_is_output("adder", f)],
        outputs=[f for f in formals if blif.gate_port_is_output("adder", f)])


class _Flattener:
    def __init__(self, netlist: blif.Netlist, strict_init: bool, cut: bool,
                 normalize: bool, match_unnamed: bool = False,
                 blackbox: frozenset[str] = frozenset()) -> None:
        self.netlist = netlist
        self.blackbox = blackbox  # models cut and paired by .cname or occurrence (--blackbox)
        self.occurrence: dict[str, int] = {}
        self.buffer_maps: dict[str, dict[str, str]] = {}
        self.cut = cut
        self.normalize = normalize
        self.match_unnamed = match_unnamed
        self.anon_count = 0
        self.instances: dict[str, BlackBox] = {}
        self.models = netlist.by_name()
        self.strict_init = strict_init
        self.counter = 0
        self.dc_inits = 0
        top = netlist.top
        clocks = [c for c in top.clocks if c not in top.inputs]
        self.flat = Flat(path=netlist.path, top=top.name, inputs=top.inputs + clocks,
                         outputs=list(top.outputs))

    def fresh(self, hint: str) -> str:
        self.counter += 1
        return f"{hint}{_SEP}{self.counter}"

    def const0(self) -> str:
        net = self.fresh("const0")
        self.flat.names.append(blif.Names(inputs=[], output=net))
        return net

    def run(self) -> Flat:
        top = self.netlist.top
        if top.blackbox:
            raise EquivError(f"{self.netlist.path}: top model {top.name!r} is a black box")
        stack = [_Frame(top, "", {}, (top.name,))]
        while stack:
            frame = stack.pop()
            for cell in frame.model.cells:
                self.add_cell(cell, frame, stack)
        if self.dc_inits:
            notice(f"{self.netlist.path}: {self.dc_inits} latch(es) with init 2/3 "
                   "(don't care/unknown) treated as 0")
        return self.flat

    def add_cell(self, cell: blif.Cell, frame: _Frame, stack: list[_Frame]) -> None:
        if isinstance(cell, blif.Names):
            self.flat.names.append(blif.Names(
                inputs=[frame.resolve(n) for n in cell.inputs],
                output=frame.resolve(cell.output), rows=list(cell.rows)))
        elif isinstance(cell, blif.Latch):
            self.add_latch(cell, frame)
        else:
            self.add_subckt(cell, frame, stack)

    def add_latch(self, cell: blif.Latch, frame: _Frame) -> None:
        init = cell.init or "3"
        if init in ("2", "3"):
            if self.strict_init:
                raise EquivError(f"{self.netlist.path}:{cell.line}: latch {cell.output!r} "
                                 f"has init {init} (rejected by --strict-init)")
            self.dc_inits += 1
            init = "0"
        ctrl = blif.latch_control(cell)
        self.flat.latches.append(blif.Latch(
            input=frame.resolve(cell.input), output=frame.resolve(cell.output),
            ltype=cell.ltype, control=cell.control if ctrl is None else frame.resolve(ctrl),
            init=init, line=cell.line))

    def add_subckt(self, cell: blif.Subckt, frame: _Frame, stack: list[_Frame]) -> None:
        sub = self.models.get(cell.model)
        self.check_pins_once(cell, [f for f, _ in cell.conns])
        conns = {f: frame.resolve(a) for f, a in cell.conns}
        if sub is None and cell.model == "adder":
            sub = _gate_adder_model(cell)
        if sub is None or (sub.blackbox and sub.name != "adder") or cell.model in self.blackbox:
            self.add_blackbox(cell, sub, frame)
            return
        if sub.blackbox:
            self.expand_adder(sub, conns)
            return
        if not sub.cells and (sub.inputs or sub.outputs or sub.clocks):
            raise EquivError(f"{self.netlist.path}:{cell.line}: empty model {sub.name!r}; "
                             "mark it .blackbox")
        if cell.model in frame.path or len(frame.path) > _MAX_DEPTH:
            raise EquivError(f"{self.netlist.path}:{cell.line}: recursive instantiation of "
                             f"model {cell.model!r}")
        self.counter += 1
        prefix = f"{frame.prefix}{cell.model}#{self.counter}{_SEP}"
        child = _Frame(sub, prefix, conns, (*frame.path, cell.model))
        for port in sub.inputs + sub.clocks:
            if port not in conns:
                child.netmap[port] = self.const0()
        stack.append(child)

    def check_pins_once(self, cell: blif.Subckt, pins: list[str]) -> None:
        """A formal pin connected twice (``data[0]=a data=b`` is one pin of a one-bit port) is a
        malformed instance: the pin tables are keyed by pin, so the first connection would be
        silently lost and never compared.  EquivError (exit 2)."""
        seen: set[str] = set()
        for pin in pins:
            if pin in seen:
                raise EquivError(f"{self.netlist.path}:{cell.line}: malformed: pin {pin!r} of "
                                 f"{cell.model!r} connected twice")
            seen.add(pin)

    def add_blackbox(self, cell: blif.Subckt, sub: blif.Model | None, frame: _Frame) -> None:
        """Cut a black-box instance: outputs become inputs, inputs become outputs."""
        if not self.cut:
            raise EquivError(f"{self.netlist.path}:{cell.line}: unsupported black box "
                             f"{cell.model}")
        names = _canonical_formals([f for f, _ in cell.conns])
        self.check_pins_once(cell, [names[f] for f, _ in cell.conns])
        if sub is None:
            is_out = {names[f]: blif.gate_port_is_output(cell.model, f) for f, _ in cell.conns}
        else:
            is_out = {names[f]: f in sub.outputs for f, _ in cell.conns}
        ordered = sorted(cell.conns, key=lambda c: _formal_order(names[c[0]]))
        pins = {names[f]: frame.resolve(a) for f, a in ordered}
        key = self.instance_key(cell, frame, pins, is_out)
        anon = key is None
        if key is None:  # a provisional key: it must be a legal BLIF net name, and unique
            self.anon_count += 1
            key = f"?anon{self.anon_count}"
        box = BlackBox(cell.model, key, cell.line,
                       {f: n for f, n in pins.items() if not is_out[f]},
                       {f: n for f, n in pins.items() if is_out[f]}, anon=anon,
                       params=tuple(sorted(a for a in cell.attrs if a[0] != ".cname")))
        other = self.instances.get(box.key)
        if other is not None:
            raise EquivError(f"{self.netlist.path}:{cell.line}: two black-box instances named "
                             f"{box.key!r} (also line {other.line}); cannot match them")
        self.instances[box.key] = box
        self.flat.blackboxes.append(box)
        for formal, net in box.outputs.items():
            free = f"bbout:{box.cell_type}:{box.key}:{formal}"
            box.free[formal] = free
            self.flat.inputs.append(free)
            self.flat.names.append(blif.Names(inputs=[free], output=net, rows=[("1", "1")]))
        for formal, net in box.inputs.items():
            probe = f"bbin:{box.cell_type}:{box.key}:{formal}"
            box.probes[formal] = probe
            self.flat.outputs.append(probe)
            self.flat.names.append(blif.Names(inputs=[net], output=probe, rows=[("1", "1")]))

    def instance_key(self, cell: blif.Subckt, frame: _Frame, pins: dict[str, str],
                     is_out: dict[str, bool]) -> str | None:
        """The instance name used to match black boxes across the two files; None for an
        instance that ``--match-unnamed`` leaves to the signature matching (no ``.cname``)."""
        def norm(name: str) -> str:
            name = normalize_io_name(name, self.flat.top) if self.normalize else name
            return name.replace(" ", "_")

        for attr, value in cell.attrs:
            if attr == ".cname" and value:
                return "/".join([*frame.path[1:], norm(value)])
        if cell.model in self.blackbox:  # --blackbox: the n-th instance of the model
            self.occurrence[cell.model] = self.occurrence.get(cell.model, 0) + 1
            return f"{LISTED_PREFIX}{cell.model}:{self.occurrence[cell.model]}"
        if self.match_unnamed:
            return None
        buffered = self.port_buffers(frame)
        for formal, net in pins.items():
            if not is_out[formal]:
                continue
            if net.startswith("$"):  # a generated net: name it through its port buffer
                net = buffered.get(net, net)
            if _SEP not in net and not net.startswith("$"):
                base, bit = _formal_order(formal)  # width-independent: out[0] and out agree
                return f"{base}{f'[{bit}]' if bit else ''}={norm(net)}"
        raise EquivError(f"{self.netlist.path}:{cell.line}: cannot name black box "
                         f"{cell.model}: no .cname and no named output net to match it by")

    def port_buffers(self, frame: _Frame) -> dict[str, str]:
        """Flat net -> flat net for each generated net (``$...``) that has one reader in the
        frame's model, a one-input ``.names`` buffer (row ``1 1``), and no other pin but its
        driver's; the value is the buffer's output. write_blif --vtr puts a hard block's
        primary output on its own ``$n<ID>`` net behind such a buffer; the box is then named by
        the buffer's output. A net with any other reader maps to nothing, so the box stays
        unnameable (an error, never a silent pass)."""
        cached = self.buffer_maps.get(frame.prefix)
        if cached is not None:
            return cached
        readers: dict[str, int] = {}
        pins_on: dict[str, int] = {}
        for cell in frame.model.cells:
            if isinstance(cell, blif.Names):
                reads = cell.inputs
            elif isinstance(cell, blif.Latch):
                reads = [cell.input]
            else:  # a box pin: the one that drives the net counts once, below
                reads = [a for _, a in cell.conns]
                for net in reads:
                    pins_on[net] = pins_on.get(net, 0) + 1
                continue
            for net in reads:
                readers[net] = readers.get(net, 0) + 1
        for net in frame.model.outputs:
            readers[net] = readers.get(net, 0) + 1
        found: dict[str, str] = {}
        for cell in frame.model.cells:
            if (isinstance(cell, blif.Names) and len(cell.inputs) == 1
                    and cell.rows == [("1", "1")] and cell.inputs[0].startswith("$")
                    and readers.get(cell.inputs[0]) == 1
                    and pins_on.get(cell.inputs[0], 0) <= 1):
                found[frame.resolve(cell.inputs[0])] = frame.resolve(cell.output)
        self.buffer_maps[frame.prefix] = found
        return found

    def expand_adder(self, model: blif.Model, conns: dict[str, str]) -> None:
        """Replace a VTR ``adder`` hard block (ripple of full adders) with logic."""
        a, b, cin, sumout, cout = _adder_shape(model)

        def src(port: str) -> str:
            return conns.get(port) or self.const0()

        def dst(port: str) -> str:
            return conns.get(port) or self.fresh(port)

        carry = src(cin)
        for i, (abit, bbit, sbit) in enumerate(zip(a, b, sumout, strict=True)):
            ins = [src(abit), src(bbit), carry]
            self.flat.names.append(blif.Names(inputs=ins, output=dst(sbit), rows=list(XOR3)))
            carry_out = dst(cout) if i == len(a) - 1 else self.fresh("carry")
            self.flat.names.append(blif.Names(inputs=ins, output=carry_out, rows=list(MAJ3)))
            carry = carry_out


def check_undriven(flat: Flat, allow_undriven: bool) -> None:
    """Undriven nets that reach a primary output or a latch D input are an error (they
    would silently become constants); with --allow-undriven, and for undriven nets that
    only feed dangling logic, they are tied to 0 with a notice."""
    driven = set(flat.inputs)
    driven.update(c.output for c in flat.names)
    driven.update(c.output for c in flat.latches)
    used: dict[str, None] = dict.fromkeys(flat.outputs)
    for names in flat.names:
        used.update(dict.fromkeys(names.inputs))
    used.update(dict.fromkeys(c.input for c in flat.latches))
    undriven = sorted(n for n in used if n not in driven)
    if not allow_undriven:
        _reject_observable(flat, undriven)
    for net in undriven:
        flat.names.append(blif.Names(inputs=[], output=net))
    if undriven:
        notice(f"{flat.path}: {len(undriven)} undriven net(s) tied to 0")


def _reject_observable(flat: Flat, undriven: list[str]) -> None:
    fanout: dict[str, list[str]] = {}
    for names in flat.names:
        for net in names.inputs:
            fanout.setdefault(net, []).append(names.output)
    sinks = {n: "primary output" for n in flat.outputs}
    sinks.update({c.input: f"the D input of latch {show(c.output)}" for c in flat.latches})
    seen: set[str] = set()
    for source in undriven:
        work = [source]
        while work:
            net = work.pop()
            if net in sinks:
                raise EquivError(f"{flat.path}: undriven net {show(source)} reaches "
                                 f"{sinks[net]} {show(net)}; pass --allow-undriven to "
                                 "tie it to 0")
            if net not in seen:
                seen.add(net)
                work.extend(fanout.get(net, []))


def show(net: str) -> str:
    """Printable flat net name (instance-path separators shown as '/')."""
    return net.replace(_SEP, "/")


def flatten(netlist: blif.Netlist, strict_init: bool = False, allow_undriven: bool = False,
            cut: bool = True, normalize: bool = True, check: bool = True,
            match_unnamed: bool = False, blackbox: frozenset[str] = frozenset()) -> Flat:
    """Flatten ``netlist`` into one model; black boxes are cut (``cut``) or rejected.

    ``check=False`` leaves the undriven-net check to the caller (``check_undriven``), so that
    black-box mismatches between the two files are reported first."""
    flat = _Flattener(netlist, strict_init, cut, normalize, match_unnamed, blackbox).run()
    if check:
        check_undriven(flat, allow_undriven)
    return flat


def _widths(box: BlackBox) -> dict[str, int]:
    widths: dict[str, int] = {}
    for formal in (*box.inputs, *box.outputs):
        base = blif.port_base(formal)
        widths[base] = widths.get(base, 0) + 1
    return widths


def _describe(box: BlackBox, flat: Flat) -> str:
    if box.anon:
        first = next(iter(box.outputs.items()), ("", ""))
        return (f"{box.cell_type} unnamed, {first[0]}={show(first[1])} "
                f"({Path(flat.path).name}:{box.line})")
    return f"{box.cell_type} {box.key} ({Path(flat.path).name}:{box.line})"


def _param_map(box: BlackBox) -> dict[str, str]:
    """The ``.param NAME VALUE`` lines of a black box (a repeated name: the last wins)."""
    params: dict[str, str] = {}
    for attr, value in box.params:
        if attr == ".param":
            name, _, rest = value.partition(" ")
            params[name] = rest.strip()
    return params


def _compare_boxes(key: str, box_a: BlackBox, box_b: BlackBox) -> list[str]:
    if box_a.cell_type != box_b.cell_type:
        return [f"cell types differ for instance {key!r}: {box_a.cell_type} in A, "
                f"{box_b.cell_type} in B"]
    wa, wb = _widths(box_a), _widths(box_b)
    if wa != wb:
        diffs = ", ".join(f"{base}: {wa.get(base, 0)} in A, {wb.get(base, 0)} in B"
                          for base in sorted(set(wa) | set(wb)) if wa.get(base) != wb.get(base))
        return [f"port widths differ for {box_a.cell_type} instance {key!r}: {diffs}"]
    pa = {*box_a.inputs, *box_a.outputs}
    pb = {*box_b.inputs, *box_b.outputs}
    if pa != pb or set(box_a.outputs) != set(box_b.outputs):
        return [f"ports differ for {box_a.cell_type} instance {key!r}: only in A: "
                f"{sorted(pa - pb)}; only in B: {sorted(pb - pa)}"]
    ma, mb = _param_map(box_a), _param_map(box_b)
    if ma != mb:  # a box's configuration (a RAM mode, an INIT value) is part of what it is
        diffs = ", ".join(f"{n}: {ma.get(n, '(none)')} in A, {mb.get(n, '(none)')} in B"
                          for n in sorted(set(ma) | set(mb)) if ma.get(n) != mb.get(n))
        return [f"parameters differ for {box_a.cell_type} instance {key!r}: {diffs}"]
    return []


def _spec(box: BlackBox, anchor: str | None = None) -> bbmatch.BoxSpec:
    """The simulator's view of a cut box: pins in port order, named (or anchored) boxes seeded by
    their name."""
    def ordered(pins: dict[str, str]) -> tuple[tuple[str, str], ...]:
        return tuple(sorted(pins.items(), key=lambda kv: _formal_order(kv[0])))

    return bbmatch.BoxSpec(box.cell_type, box.params, anchor or (None if box.anon else box.key),
                           ordered(box.inputs), ordered(box.free))


def _rename_nets(flat: Flat, nets: dict[str, str]) -> None:
    """Apply old net -> new net to the ports and covers of ``flat`` (an input listed twice by
    a merge is kept once)."""
    flat.inputs = list(dict.fromkeys(nets.get(n, n) for n in flat.inputs))
    flat.outputs = [nets.get(n, n) for n in flat.outputs]
    for cell in flat.names:
        cell.output = nets.get(cell.output, cell.output)
        cell.inputs = [nets.get(n, n) for n in cell.inputs]


def _rekey(flat: Flat, renames: dict[str, str]) -> None:
    """Rename black boxes of ``flat`` (old key -> new key) everywhere their keys appear: the
    instance key, the free output inputs and the probe outputs."""
    nets: dict[str, str] = {}
    for box in flat.blackboxes:
        new = renames.get(box.key)
        if new is None:
            continue
        for table, prefix in ((box.free, "bbout"), (box.probes, "bbin")):
            for formal, old_net in table.items():
                table[formal] = nets[old_net] = f"{prefix}:{box.cell_type}:{new}:{formal}"
        box.key = new
    _rename_nets(flat, nets)


def _merge_outputs(flat: Flat, group: Sequence[BlackBox]) -> None:
    """Make the boxes of ``group`` (same type, same inputs) share one free output per pin."""
    nets = {box.free[pin]: group[0].free[pin] for box in group[1:] for pin in box.free}
    _rename_nets(flat, nets)
    for box in group[1:]:
        box.free = {pin: group[0].free[pin] for pin in box.free}


def _signatures(flat: Flat, normalize: bool, anchors: dict[int, str]) -> list[bbmatch.Signature]:
    """Signatures of all boxes of ``flat``; ``anchors`` (box index -> pair name) seeds boxes that
    are already paired like named ones."""
    def pi_key(net: str) -> str:
        return normalize_io_name(net, flat.top) if normalize else net

    specs = [_spec(b, anchors.get(i)) for i, b in enumerate(flat.blackboxes)]
    try:
        return bbmatch.signatures(flat.names, flat.latches, flat.inputs, specs, pi_key)
    except bbmatch.SimError as exc:
        raise EquivError(f"{flat.path}: cannot simulate the netlist to match unnamed black "
                         f"boxes: {exc}") from exc


@dataclass
class _Matching:
    """The outcome of ``_match_rounds`` in black-box indices of the two flats."""

    pairs: list[tuple[int, int]]
    twins: list[tuple[list[int], list[int]]]
    only: tuple[list[int], list[int]]
    guessed: int


def _lazy_classes(flat: Flat) -> Callable[[str], str]:
    """``bbmatch.canonical_classes`` of ``flat``, computed at the first call (a congruence closure
    over every cover is costly and only a tied group needs it)."""
    built: list[Callable[[str], str]] = []

    def classes(net: str) -> str:
        if not built:
            built.append(bbmatch.canonical_classes(
                flat.names, flat.latches, [_spec(b) for b in flat.blackboxes]))
        return built[0](net)

    return classes


def _match_rounds(flats: Sequence[Flat], normalize: bool) -> _Matching:
    """Simulate and pair, round after round.  The boxes whose signature is unique on both sides
    are anchored -- from the next round on they are driven by a shared name, as if they had a
    ``.cname`` -- so that logic behind them, which a stuck cone had made look alike, becomes
    distinguishable.  Stops when a round anchors nothing new (or after ``MATCH_ROUNDS``)."""
    anon = [[i for i, b in enumerate(f.blackboxes) if b.anon] for f in flats]
    anchors: list[dict[int, str]] = [{}, {}]
    anchored: list[tuple[int, int]] = []
    for rnd in range(MATCH_ROUNDS):
        sigs = [_signatures(f, normalize, a) for f, a in zip(flats, anchors, strict=True)]
        free = [[i for i in ids if i not in a] for ids, a in zip(anon, anchors, strict=True)]
        result = bbmatch.pair([sigs[0][i] for i in free[0]], [sigs[1][i] for i in free[1]],
                              (_lazy_classes(flats[0]), _lazy_classes(flats[1])))
        if not result.exact or rnd == MATCH_ROUNDS - 1:
            break
        for i, j in result.exact:
            name = f"@{len(anchored)}"
            anchors[0][free[0][i]] = anchors[1][free[1][j]] = name
            anchored.append((free[0][i], free[1][j]))

    def box(side: int, group: list[int]) -> list[int]:
        return [free[side][i] for i in group]

    return _Matching(
        anchored + [(free[0][i], free[1][j]) for i, j in result.pairs],
        [(box(0, a), box(1, b)) for a, b in result.twins],
        (box(0, result.only_a), box(1, result.only_b)), result.guessed)


def pair_unnamed(flats: Sequence[Flat], normalize: bool = True) -> tuple[int, int]:
    """Pair the unnamed black boxes (``anon``) of the two flats by simulation signature (see
    ``bbmatch``) and give each pair one key in both; returns (pairs, guessed).

    A box is paired when its signature -- type, parameters, ports and the response of each input
    bit -- is the only one of its kind on both sides, or when several have it and they are
    interchangeable (the same input net on every pin).  Boxes with one signature but different
    input nets (copies of one register, say) are split by the structure of their input cones and
    paired in file order inside a part: ``guessed`` counts those pairs, and a "not equivalent"
    verdict is then no verdict (see ``_check``).  Boxes with no partner raise EquivError
    (exit 2), listing them."""
    fa, fb = flats
    if not any(b.anon for f in flats for b in f.blackboxes):
        return 0, 0
    match = _match_rounds(flats, normalize)
    lines = [f"  only in A: {_describe(fa.blackboxes[i], fa)}" for i in match.only[0]]
    lines += [f"  only in B: {_describe(fb.blackboxes[i], fb)}" for i in match.only[1]]
    if lines:
        raise EquivError("unnamed black-box instances cannot be matched one-to-one (matched "
                         "by simulation signature: type, parameters, ports and input functions; "
                         "an instance whose inputs differ between the files is left over)",
                         "\n".join(lines))
    _rekey(fb, {fb.blackboxes[j].key: fa.blackboxes[i].key for i, j in match.pairs})
    for group_a, group_b in match.twins:
        _merge_outputs(fa, [fa.blackboxes[i] for i in group_a])
        _merge_outputs(fb, [fb.blackboxes[j] for j in group_b])
    return len(match.pairs), match.guessed


# --------------------------------------------------------------------------- dual-port orientation

DUAL_PORT_RAM = "dual_port_ram"
_DPR_PIN = re.compile(r"(addr|data|we|out)([12])((?:\[\d+\])?)")


def _swap_pin(formal: str) -> str:
    """``addr1[3]`` -> ``addr2[3]`` and back; any other pin (``clk``) is its own partner."""
    match = _DPR_PIN.fullmatch(formal)
    if match is None:
        return formal
    return f"{match.group(1)}{3 - int(match.group(2))}{match.group(3)}"


def _port_spec(box: BlackBox, port: int) -> bbmatch.BoxSpec:
    """One port of a dual-port RAM as a box with no outputs: the input pins of that port under
    port-neutral names, so that the history digests of port 1 and of port 2 compare."""
    pins: list[tuple[str, str]] = []
    for formal, net in box.inputs.items():
        match = _DPR_PIN.fullmatch(formal)
        if match is not None and match.group(2) == str(port):
            pins.append((match.group(1) + match.group(3), net))
    pins.sort(key=lambda pin: _formal_order(pin[0]))
    return bbmatch.BoxSpec(f"{box.cell_type}-port", (), None, tuple(pins), ())


def _swap_ports(box: BlackBox) -> dict[str, str]:
    """Exchange ports 1 and 2 of a dual-port RAM cut box: every pin takes its partner's name.
    Returns the renaming of the free output and probe nets, which are named after the pin (the
    caller applies the renamings of all the boxes it turns in one ``_rename_nets``)."""
    nets: dict[str, str] = {}
    for table, prefix in ((box.free, "bbout"), (box.probes, "bbin")):
        for formal, net in table.items():
            nets[net] = f"{prefix}:{box.cell_type}:{box.key}:{_swap_pin(formal)}"
    box.inputs = {_swap_pin(f): n for f, n in box.inputs.items()}
    box.outputs = {_swap_pin(f): n for f, n in box.outputs.items()}
    box.free = {_swap_pin(f): nets[n] for f, n in box.free.items()}
    box.probes = {_swap_pin(f): nets[n] for f, n in box.probes.items()}
    return nets


def _orient_specs(flat: Flat, boxes: Sequence[BlackBox]) -> list[bbmatch.BoxSpec]:
    """The simulator's view for orienting: every cut box drives its outputs with a vector that
    depends on its cell type and the *neutral* name of the pin only (``out1`` and ``out2`` of a
    dual-port RAM get one vector), never on its inputs or its port order, so that a swap of one
    RAM changes nothing the simulation sees; then each port of ``boxes`` as a box of its own."""
    specs = []
    for box in flat.blackboxes:
        pins = tuple((_DPR_PIN.sub(r"\1\3", f) if box.cell_type == DUAL_PORT_RAM else f, n)
                     for f, n in sorted(box.free.items(), key=lambda kv: _formal_order(kv[0])))
        specs.append(bbmatch.BoxSpec(box.cell_type, (), "orient", (), pins))
    return specs + [_port_spec(b, port) for b in boxes for port in (1, 2)]


def _orient_flat(flat: Flat, normalize: bool) -> int:
    """Orient the unnamed dual-port RAMs of ``flat``: every one whose port-1 history digest is
    above its port-2 digest is swapped; returns how many."""
    boxes = [b for b in flat.blackboxes if b.anon and b.cell_type == DUAL_PORT_RAM]
    if not boxes:
        return 0

    def pi_key(net: str) -> str:
        return normalize_io_name(net, flat.top) if normalize else net

    specs = _orient_specs(flat, boxes)
    try:
        sigs = bbmatch.signatures(flat.names, flat.latches, flat.inputs, specs, pi_key,
                                  structure=False)
    except bbmatch.SimError as exc:
        raise EquivError(f"{flat.path}: cannot simulate the netlist to orient dual-port RAMs: "
                         f"{exc}") from exc
    base = len(flat.blackboxes)
    swap = [box for k, box in enumerate(boxes)
            if sigs[base + 2 * k].digest > sigs[base + 2 * k + 1].digest]
    renames: dict[str, str] = {}
    for box in swap:
        renames.update(_swap_ports(box))
    if renames:
        _rename_nets(flat, renames)
    return len(swap)


def orient_dual_ports(flats: Sequence[Flat], normalize: bool = True) -> int:
    """Give every unnamed Parmys ``dual_port_ram`` of both flats one canonical orientation;
    returns the number of boxes swapped.

    Parmys numbers the two ports of a dual-port RAM in the order Yosys's ``memory`` pass lists
    the read ports, an order of hash-table iteration: the same memory with the same two accesses
    can come out with port 1 and port 2 exchanged (``common/2r2w``, ``syntax/inferred_DPram``).
    The two ports are interchangeable apart from a same-address write in one cycle, a race
    IEEE 1364-2005 leaves undefined (2D spec section 17 #66), so the netlists are the same
    design.  Without a canonical orientation the boxes would not pair (their input functions
    differ pin by pin).

    The orientation is a function of what each port is fed: a port's *history digest* is that of
    ``bbmatch`` over its address, data and write-enable pins (port-neutral pin names), and the
    port with the smaller digest becomes port 1.  The simulation that gives the digests drives
    every cut box's outputs with a vector of its cell type alone (a RAM's two read ports get the
    same vector), so the digests do not depend on any RAM's orientation and one pass is the
    fixed point; the price is that two ports fed only by different RAMs' data look alike, and
    ports with equal digests stay as they are.

    Soundness: this is an assumption of the check, of the kind "RAM contents are not modelled":
    a paired RAM is a cut point and ABC proves every input pin of it equal; the swap only chooses
    which pins are compared with which.  Ports 1 and 2 are taken as symmetric, so a design whose
    behaviour differs only in which port wins a same-address write is called equivalent."""
    return sum(_orient_flat(f, normalize) for f in flats)


# --------------------------------------------------------------------------- RAM groups

RAM_PROBE_TIMEOUT = 40  # seconds per ABC probe of pair_ram_groups
RAM_PROBE_BUDGET = 1200  # seconds for all the probes of one run
SINGLE_PROBE_TIMEOUT = 15  # seconds per ABC probe of a box that names no memory
SINGLE_PROBE_BUDGET = 120  # seconds for all those probes of one run (cones of multipliers are deep)


def _ram_groups(flat: Flat) -> list[rampair.Group]:
    idents = [(b.cell_type, rampair.slice_identity(b.outputs.values()) if b.anon else None)
              for b in flat.blackboxes]
    try:
        return rampair.build_groups(idents)
    except rampair.RamError as exc:
        raise EquivError(f"{flat.path}: {exc}") from exc


SINGLE = "@single"  # the ``mem`` of a one-box group (a box that names no memory)


def _single_groups(flat: Flat, groups: Sequence[rampair.Group]) -> list[rampair.Group]:
    """A group of its own for every unnamed box that no memory group holds (a multiplier, a RAM
    slice without an identity): the unit ``_probe_classes`` pairs when simulation ties several."""
    held = {index for g in groups for index in g.slices.values()}
    return [rampair.Group(b.cell_type, f"{SINGLE}{index}", {0: index})
            for index, b in enumerate(flat.blackboxes) if b.anon and index not in held]


def _group_key(group: rampair.Group, sigs: Sequence[bbmatch.Signature]
               ) -> tuple[str, tuple[int, ...], tuple[bytes, ...]]:
    return group.cell_type, group.bits, tuple(sigs[group.slices[b]].digest for b in group.bits)


def _class_nets(flat: Flat, groups: Sequence[rampair.Group]) -> dict[str, str]:
    """Free output net -> class net for every unpaired box: the abstraction of a probe.  All boxes
    of one class (type, parameters, memory size, bit) share one free variable per pin, the same on
    both sides.  Equal functions stay equal under it when the true pairing keeps classes, so a
    probe can only lose a candidate that is not one; it can add one that is not (the final proof
    runs without the abstraction and catches that)."""
    where: dict[int, tuple[int, int]] = {}
    for group in groups:
        for bit, index in group.slices.items():
            where[index] = (len(group.slices), bit)
    nets: dict[str, str] = {}
    for index, box in enumerate(flat.blackboxes):
        if not box.anon:
            continue
        size, bit = where.get(index, (0, 0))
        tag = hashlib.sha1(repr(box.params).encode()).hexdigest()[:8]
        for pin, net in box.free.items():
            nets[net] = f"cls:{box.cell_type}:{tag}:{size}:{bit}:{pin}"
    return nets


def _probe_flat(flat: Flat, roots: Sequence[str], nets: dict[str, str]) -> Flat:
    """The fan-in cone of ``roots`` (through covers and latches) as a Flat whose outputs are the
    roots and whose inputs are every undriven net it reads, renamed by ``nets``."""
    def ren(net: str) -> str:
        return nets.get(net, net)

    drivers: dict[str, blif.Names | blif.Latch] = {c.output: c for c in flat.names}
    drivers.update({latch.output: latch for latch in flat.latches})
    seen: set[str] = set()
    todo = list(roots)
    cone = Flat(flat.path, flat.top, [], [])
    leaves: set[str] = set()
    while todo:
        net = todo.pop()
        if net in seen:
            continue
        seen.add(net)
        cell = drivers.get(net)
        if cell is None:
            leaves.add(ren(net))
        elif isinstance(cell, blif.Names):
            cone.names.append(blif.Names([ren(n) for n in cell.inputs], cell.output, cell.rows))
            todo.extend(cell.inputs)
        else:
            cone.latches.append(blif.Latch(ren(cell.input), cell.output, init=cell.init))
            todo.append(cell.input)
    for k, root in enumerate(roots):
        out = f"probe:{k}"
        cone.names.append(blif.Names([ren(root)], out, [("1", "1")]))
        cone.outputs.append(out)
    cone.inputs = sorted(leaves)
    return cone


def _probe_equal(cones: tuple[Flat, Flat], abc: str, normalize: bool, timeout: int) -> bool:
    """True when ABC proves the two cones equal output for output.  Anything else (a
    difference, a timeout, an ABC error) is False: a probe only proposes."""
    fa, fb = cones
    union = sorted({*fa.inputs, *fb.inputs})
    fa.inputs = fb.inputs = union
    verb = "dsec" if fa.latches or fb.latches else "cec"
    if verb == "dsec":
        for flat in cones:
            if not flat.latches:
                add_dangling_latch(flat)
    tmp = Path(tempfile.mkdtemp(prefix="equiv-ram-"))
    try:
        write_flat(fa, io_map(fa, normalize), tmp / "a.blif")
        write_flat(fb, io_map(fb, normalize), tmp / "b.blif")
        return parse_verdict(run_abc(abc, f"{verb} a.blif b.blif", tmp, timeout)) is True
    except HardError:
        raise
    except EquivError:
        return False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _probe_classes(flats: Sequence[Flat], groups: Sequence[Sequence[rampair.Group]],
                   classes: Sequence[tuple[list[int], list[int]]], abc: str, normalize: bool,
                   limits: tuple[float, int],
                   sigs: Sequence[Sequence[bbmatch.Signature]] = ()
                   ) -> tuple[list[tuple[int, int]], set[tuple[int, int]]]:
    """Pair the groups of each ambiguous class (several groups that simulation cannot tell
    apart) by what they are.  ABC compares the input pins of a first and a last slice of an A
    group with those of the B groups, one B group at a time, as cones with the other unpaired
    boxes abstracted (``_class_nets``); only the pins that differ between the groups of the class
    are compared (the write enable of one way of a cache, not the address all ways share).  The
    first B group that ABC proves equal is the partner.  A proof of a difference may take far
    longer than a proof of equality, so ``limits`` is (deadline on ``time.monotonic``, seconds per
    probe); a probe that times out counts as not equal.  When one A and one B group are left
    over in a class they are paired without a probe (every group has a partner or the final
    proof fails): that pair, and every pair whose probe was vacuous (all the compared pins of
    both groups are behind ``unconn``, so any two groups look equal), is *weak*, a guess.  Returns
    (the pairs, the weak ones among them).  A proof that two cones differ is the slow case, so
    the candidates of B are tried in the order of ``sigs``' structure colour: the ones whose input
    cones look like the A group's first."""
    deadline, per_probe = limits
    nets = [_class_nets(f, g) for f, g in zip(flats, groups, strict=True)]

    def first(side: int, group: rampair.Group) -> dict[str, str]:
        return flats[side].blackboxes[group.slices[group.bits[0]]].inputs

    def roots(side: int, group: rampair.Group, pins: list[str]) -> list[str]:
        boxes = [flats[side].blackboxes[group.slices[b]] for b in dict.fromkeys(
            (group.bits[0], group.bits[-1]))]
        return [box.inputs[pin] for box in boxes for pin in pins]

    reached = [x_reached(f) for f in flats]
    found: list[tuple[int, int]] = []
    weak: set[tuple[int, int]] = set()
    for ids_a, ids_b in classes:
        pins = sorted(pin for pin in first(0, groups[0][ids_a[0]])
                      if any(len({first(s, groups[s][g])[pin] for g in ids}) > 1
                             for s, ids in ((0, ids_a), (1, ids_b))))
        if not pins:  # every pin of every group is the same net: interchangeable, not probed
            continue
        left_a, left_b = list(ids_a), list(ids_b)
        for i in ids_a:
            gaps = {g: _colour_gap(groups, sigs, (i, g)) for g in left_b}
            for j in sorted(left_b, key=gaps.__getitem__):
                if time.monotonic() > deadline:
                    return found, weak
                cones = (_probe_flat(flats[0], roots(0, groups[0][i], pins), nets[0]),
                         _probe_flat(flats[1], roots(1, groups[1][j], pins), nets[1]))
                if _probe_equal(cones, abc, normalize, per_probe):
                    found.append((i, j))
                    if all(net in reached[side] for side, grp in ((0, groups[0][i]),
                                                                   (1, groups[1][j]))
                           for net in roots(side, grp, pins)):
                        weak.add((i, j))  # vacuous: every compared pin is behind unconn
                    left_a.remove(i)
                    left_b.remove(j)
                    ga, gb = groups[0][i], groups[1][j]
                    if ga.mem.startswith(SINGLE):
                        notice(f"{ga.cell_type} #{ga.slices[0]} = {gb.cell_type} #{gb.slices[0]} "
                               f"in B (ABC), {len(left_b)} left in class")
                    else:
                        notice(f"RAM group {ga.mem} ({len(ga.slices)} slices) = group {gb.mem} "
                               f"in B (ABC), {len(left_b)} left in class")
                    break
        if len(left_a) == len(left_b) == 1:
            found.append((left_a[0], left_b[0]))  # no probe: a guess
            weak.add((left_a[0], left_b[0]))
    return found, weak


def _colour_gap(groups: Sequence[Sequence[rampair.Group]],
                sigs: Sequence[Sequence[bbmatch.Signature]], pair: tuple[int, int]) -> int:
    """0 when the input-cone colours of the first slices of A group ``pair[0]`` and B group
    ``pair[1]`` are equal (or ``sigs`` is empty), else 1: the order the probes try candidates in."""
    if not sigs:
        return 0
    a, b = (groups[side][g] for side, g in enumerate(pair))
    return int(sigs[0][a.slices[a.bits[0]]].struct != sigs[1][b.slices[b.bits[0]]].struct)


def _assert_unique_keys(flat: Flat) -> None:
    """Every black box of ``flat`` has its own key; two boxes sharing one would share their free
    and probe nets (EquivError, exit 2, never a silent merge)."""
    keys = [box.key for box in flat.blackboxes]
    if len(set(keys)) != len(keys):
        dup = sorted(k for k, n in collections.Counter(keys).items() if n > 1)
        raise EquivError(f"{flat.path}: black-box instances share a key after pairing: "
                         + ", ".join(map(repr, dup)))


@dataclass
class RamPairing:
    """What ``pair_ram_groups`` did: RAM slices paired (``slices``) and, of them, those paired
    by an ABC probe and those in a group with interchangeable slices (``tied``); the other unnamed
    boxes paired (``boxes``) and, of them, those paired by an ABC probe."""

    slices: int = 0
    probed: int = 0
    tied: int = 0
    boxes: int = 0
    boxes_probed: int = 0
    weak: int = 0  # paired in a probe round without a trusted probe (leftover or vacuous probe)
    count: int = 0  # the pairs named so far


def _name_pairs(flats: Sequence[Flat], pairs: Sequence[Sequence[rampair.Group]],
                sigs: Sequence[Sequence[bbmatch.Signature]], how: tuple[str, bool],
                result: RamPairing, weak: Sequence[bool] = ()) -> None:
    """Give each paired group of ``pairs`` (A groups, B groups) one key per slice in both flats,
    take the boxes out of the unnamed ones and count them in ``result``.  ``weak[k]`` marks pair
    k of a probe round as not backed by a trusted probe (``RamPairing.weak``)."""
    fa, fb = flats
    prefix, by_probe = how
    names: list[dict[str, str]] = [{}, {}]
    for k, (group_a, group_b) in enumerate(zip(*pairs, strict=True)):
        result.count += 1
        trusted = by_probe and not (k < len(weak) and weak[k])
        single = group_a.mem.startswith(SINGLE)
        for side, group in ((0, group_a), (1, group_b)):
            digests = [sigs[side][group.slices[b]].digest for b in group.bits]
            if len(set(digests)) < len(digests):  # two slices a swap could cross
                result.tied += len(digests)
                break
        for bit, ia in group_a.slices.items():
            key = f"{prefix}{result.count}/{bit}"
            box_a, box_b = fa.blackboxes[ia], fb.blackboxes[group_b.slices[bit]]
            names[0][box_a.key], names[1][box_b.key] = key, key
            box_a.anon = box_b.anon = False
            result.weak += by_probe and not trusted
            if single:
                result.boxes += 1
                result.boxes_probed += trusted
            else:
                result.slices += 1
                result.probed += trusted
    _rekey(fa, names[0])
    _rekey(fb, names[1])
    for flat in flats:
        _assert_unique_keys(flat)


def pair_ram_groups(flats: Sequence[Flat], normalize: bool = True,
                    find_probe_abc: Callable[[], str] | None = None,
                    probe_limits: tuple[int, int] = (RAM_PROBE_BUDGET, RAM_PROBE_TIMEOUT),
                    box_limits: tuple[int, int] = (SINGLE_PROBE_BUDGET, SINGLE_PROBE_TIMEOUT)
                    ) -> RamPairing:
    """Pair the unnamed Parmys RAM slices of the two flats by memory group and slice index, and
    the other unnamed boxes that simulation can tell apart or an ABC probe can; returns a
    ``RamPairing``.

    A group whose slices have equal signatures (the same input pins, tied constants) can be
    paired by bit in a way that crosses interchangeable slices; a failed proof after such a
    pairing may only mean the crossing, so the caller counts those slices as guessed.

    A slice's name carries (memory id, bit) (``rampair``).  The id is a counter of one Parmys run
    and differs between runs, so the groups are paired first and the slices of two paired groups
    pair by bit, exactly.  Groups pair, round after round, when their simulation signature (the
    response of every input pin of every slice, in bit order) is the only one of its kind on both
    sides; the pairs are named like ``.cname`` boxes, so the logic behind them becomes
    distinguishable in the next round.  Groups that stay tied (write enables and data that
    simulation leaves constant) are compared by ABC on the cones of their input pins, and paired
    when that forces a pair.  The groups that are still tied, and every slice without an identity,
    are left to ``pair_unnamed``.

    Every other unnamed box (a multiplier, a slice that names no memory) is a group of one
    box, ``SINGLE``, in the same rounds: the ones whose signature is unique on both sides are
    paired and anchored, and when only ties are left the probe tells copies of one register or
    one multiplier apart by what feeds them (``pair_unnamed`` alone would pair such a tie by
    structure and file order, a guess; an ABC-proved pair is a proposal that the final proof
    checks, like every pair, but a wrong one is rarer than a wrong guess).  The boxes still tied
    after the probes go to ``pair_unnamed`` as before.  Both sides must hold the same memory
    shapes where both name them (else EquivError, exit 2).

    Soundness: as for ``pair_unnamed``, a pairing is only a proposal.  The paired slices become
    named cut points, ABC must prove every one of their input pins equal, and a wrong pairing can
    make the proof fail, never succeed.  Pairs found by a probe are counted as guessed by the
    caller, so a failure is unproven (exit 4), not 'not equivalent'.  Of those, only pairs a
    probe proved on at least one pin outside the x-cone count as probed (a proof for the x-cone
    verdict, ``RamPairing.probed``); a leftover pair and a vacuous probe (all compared pins
    behind ``unconn``) are weak: guessed, so an x-cone result over them is exit 4."""
    fa, fb = flats
    groups = [_ram_groups(fa), _ram_groups(fb)]
    if groups[0] and groups[1]:
        try:
            rampair.check_shapes(*groups)
        except rampair.RamError as exc:
            head, _, rest = str(exc).partition("\n")
            raise EquivError(head, rest) from exc
    groups = [g + _single_groups(f, g) for f, g in zip(flats, groups, strict=True)]
    result = RamPairing()
    if not groups[0] or not groups[1]:
        return result
    free = [list(range(len(g))) for g in groups]
    # Generated keys live in a namespace no existing key (a ``.cname``) starts with.
    prefix = "ram@"
    while any(box.key.startswith(prefix) for f in flats for box in f.blackboxes):
        prefix = "_" + prefix
    deadline = time.monotonic() + probe_limits[0]
    single_deadline = time.monotonic() + box_limits[0]
    for _ in range(MATCH_ROUNDS):
        sigs = [_signatures(f, normalize, {}) for f in flats]
        keys = [[_group_key(groups[s][g], sigs[s]) for g in free[s]] for s in (0, 1)]
        cls = rampair.classify(*keys)
        new = [(free[0][i], free[1][j]) for i, j in cls.exact]
        by_probe = False
        weak: list[bool] = []
        if not new and find_probe_abc is not None and cls.ambiguous:
            amb = [([free[0][i] for i in a], [free[1][j] for j in b]) for a, b in cls.ambiguous]
            new = []
            for singles, limits in ((False, (deadline, probe_limits[1])),
                                    (True, (single_deadline, box_limits[1]))):
                part = [c for c in amb if groups[0][c[0][0]].mem.startswith(SINGLE) == singles]
                pairs, soft = _probe_classes(flats, groups, part, find_probe_abc(), normalize,
                                             limits, sigs)
                new += pairs
                weak += [p in soft for p in pairs]
            by_probe = True
        if not new:
            break
        _name_pairs(flats, [[groups[0][ga] for ga, _ in new], [groups[1][gb] for _, gb in new]],
                    sigs, (prefix, by_probe), result, weak)
        for side, taken in ((0, {a for a, _ in new}), (1, {b for _, b in new})):
            free[side] = [g for g in free[side] if g not in taken]
    return result


def pair_listed(flats: Sequence[Flat], models: frozenset[str]) -> int:
    """Pair the instances of the ``--blackbox`` models across the two flats and give each pair one
    key in both; returns the number of pairs.  Instances with the same ``.cname`` pair by it; the
    rest of a model pair by occurrence (file order).  A model with a different number of
    instances in the two files is EquivError (exit 2)."""
    fa, fb = flats
    pairs = 0
    for model in sorted(models):
        boxes_a = [b for b in fa.blackboxes if b.cell_type == model]
        boxes_b = [b for b in fb.blackboxes if b.cell_type == model]
        if len(boxes_a) != len(boxes_b):
            raise EquivError(f"--blackbox: model '{model}' has {len(boxes_a)} instances in "
                             f"{fa.path} and {len(boxes_b)} in {fb.path}")
        named_b = {b.key for b in boxes_b if not b.key.startswith(LISTED_PREFIX)}
        named_both = {b.key for b in boxes_a if b.key in named_b}
        left_a = [b for b in boxes_a if b.key not in named_both]
        left_b = [b for b in boxes_b if b.key not in named_both]
        _rekey(fb, {y.key: x.key for x, y in zip(left_a, left_b, strict=True)})
        pairs += len(boxes_a)
    return pairs


def match_blackboxes(flats: Sequence[Flat]) -> None:
    """Require the cut black boxes of two flats to match one-to-one by name, type and ports.

    Raises EquivError (exit 2) listing every unmatched instance or mismatch."""
    fa, fb = flats
    for flat in flats:
        _assert_unique_keys(flat)
    boxes_a = {box.key: box for box in fa.blackboxes}
    boxes_b = {box.key: box for box in fb.blackboxes}
    lines = [f"  only in A: {_describe(box, fa)}" for key, box in boxes_a.items()
             if key not in boxes_b]
    lines += [f"  only in B: {_describe(box, fb)}" for key, box in boxes_b.items()
              if key not in boxes_a]
    if lines:
        raise EquivError("black-box instances cannot be matched one-to-one (matched by "
                         "instance name, never guessed)", "\n".join(lines))
    problems = [p for key, box in boxes_a.items() for p in _compare_boxes(key, box, boxes_b[key])]
    if problems:
        raise EquivError("matched black-box instances do not have the same cell type, "
                         "port widths and parameters", "\n".join(f"  {p}" for p in problems))


# --------------------------------------------------------------------------- clocking

_UNSUPPORTED_LATCH_TYPES = {"ah": "level-sensitive", "al": "level-sensitive",
                            "as": "asynchronous"}


def _is_buffer(cell: blif.Names) -> bool:
    """True if a single-input cover computes the identity, however it is written."""
    if len(cell.inputs) != 1 or not cell.rows:
        return False
    on_set = cell.rows[0][1] == "1"

    def value(bit: str) -> bool:
        matched = any(plane in (bit, "-") for plane, _ in cell.rows)
        return matched if on_set else not matched

    return not value("0") and value("1")


def _clock_source(net: str, buffers: dict[str, str], inputs: set[str]) -> str | None:
    """Follow single-input buffers (output net -> input net) back from ``net``; the primary
    input reached, or None."""
    seen: set[str] = set()
    while net not in inputs and net in buffers and net not in seen:
        seen.add(net)
        net = buffers[net]
    return net if net in inputs else None


def clock_domain(flat: Flat, ports: dict[str, str]) -> tuple[str, str] | None:
    """The single (latch type, I/O-normalized clock) of ``flat``, or None without latches.

    ABC models every latch as a flip-flop on one implicit clock, so anything else is
    rejected with EquivError: level-sensitive or asynchronous latches, clocks that are
    not (buffered) primary inputs, and more than one (type, clock) pair.  Latches without
    type/control form their own domain ("untyped", "-").
    """
    domains: set[tuple[str, str]] = set()
    buffers = {n.output: n.inputs[0] for n in flat.names if _is_buffer(n)}  # once, not per latch
    inputs = set(flat.inputs)
    sources: dict[str, str | None] = {}
    for latch in flat.latches:
        if latch.ltype is None:
            domains.add(("untyped", "-"))
            continue
        kind = _UNSUPPORTED_LATCH_TYPES.get(latch.ltype)
        if kind:
            raise EquivError(f"{flat.path}:{latch.line}: unsupported: {kind} latch type "
                             f"{latch.ltype!r} (latch {show(latch.output)})")
        if latch.control is None or latch.control == blif.NO_CONTROL:
            domains.add((latch.ltype, blif.NO_CONTROL))
            continue
        if latch.control not in sources:
            sources[latch.control] = _clock_source(latch.control, buffers, inputs)
        source = sources[latch.control]
        if source is None:
            raise EquivError(f"{flat.path}:{latch.line}: unsupported: latch "
                             f"{show(latch.output)} is clocked by internal net "
                             f"{show(latch.control)}")
        domains.add((latch.ltype, ports[source]))
    if len(domains) > 1:
        raise UnprovenError(f"unproven: {flat.path}: unsupported: more than one clock domain "
                            f"(type, clock): {sorted(domains)}")
    return next(iter(domains), None)


def add_dangling_latch(flat: Flat) -> None:
    """Give a latch-free netlist one const-0 latch driving nothing, so ABC dsec (which
    refuses networks without latches) can compare it with a sequential one."""
    const = f"dangling_d{_SEP}"
    flat.names.append(blif.Names(inputs=[], output=const))
    flat.latches.append(blif.Latch(input=const, output=f"dangling_q{_SEP}", init="0"))


# --------------------------------------------------------------------------- writing


def io_map(flat: Flat, normalize: bool) -> dict[str, str]:
    """Original primary I/O name -> name used for matching.  Raises EquivError on clashes."""
    result: dict[str, str] = {}
    owner: dict[str, str] = {}
    for port in dict.fromkeys(flat.inputs + flat.outputs):
        new = normalize_io_name(port, flat.top) if normalize else port
        if new in owner and owner[new] != port:
            raise EquivError(f"{flat.path}: ports {owner[new]!r} and {port!r} both "
                             f"normalize to {new!r}; try --no-io-normalize")
        owner[new] = port
        result[port] = new
    return result


def _renamer(ports: dict[str, str]) -> Callable[[str], str]:
    taken = set(ports.values())
    prefix = "n"
    while any(re.fullmatch(re.escape(prefix) + r"\d+", p) for p in taken):
        prefix += "_"
    internal: dict[str, str] = {}

    def rename(net: str) -> str:
        if net in ports:
            return ports[net]
        if net not in internal:
            internal[net] = f"{prefix}{len(internal)}"
        return internal[net]

    return rename


def _is_tautology(rows: list[tuple[str, str]], width: int) -> bool:
    """True if the cubes of ``rows`` cover every input minterm (exact up to 12 inputs)."""
    if any(set(plane) <= {"-"} for plane, _ in rows):
        return True
    if width > 12 or not rows:
        return False
    covered = 0
    for plane, _ in rows:
        cube = 1
        for i, ch in enumerate(plane):  # expand the cube's minterm set as a bitmask
            half = 1 << (1 << i)
            cube = cube * (half + 1) if ch == "-" else (cube << (1 << i) if ch == "1" else cube)
        covered |= cube
    return covered == (1 << (1 << width)) - 1


def constant_folded(names: blif.Names) -> tuple[list[str], list[tuple[str, str]]]:
    """(inputs, rows) to write for ``names``.  A cover that is a tautology is replaced by
    the equivalent constant: ABC asserts on tautological covers (Dec_Factor,
    src/bool/dec/decFactor.c:76)."""
    if names.inputs and names.rows and _is_tautology(names.rows, len(names.inputs)):
        return [], ([("", "1")] if names.rows[0][1] == "1" else [])
    return names.inputs, names.rows


def write_flat(flat: Flat, ports: dict[str, str], path: Path) -> None:
    """Write ``flat`` as plain single-model BLIF that ABC's reader accepts."""
    rn = _renamer(ports)
    lines = [f".model {flat.top}"]
    # Sorted, so that cut points added in file order line up between the two files.
    lines += [f".inputs {rn(p)}" for p in sorted(flat.inputs, key=lambda p: ports[p])]
    lines += [f".outputs {rn(p)}" for p in sorted(flat.outputs, key=lambda p: ports[p])]
    for names in flat.names:
        inputs, rows = constant_folded(names)
        lines.append(" ".join([".names", *(rn(n) for n in inputs), rn(names.output)]))
        lines += [f"{plane} {out}".strip() for plane, out in rows]
    for latch in flat.latches:
        lines.append(f".latch {rn(latch.input)} {rn(latch.output)} {latch.init}")
    lines.append(".end")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------- ABC


def parse_verdict(output: str) -> bool | None:
    """True if ABC proved equivalence, False if it found a difference, None otherwise."""
    if _NOT_EQUIV_RE.search(output):
        return False
    if _EQUIV_RE.search(output):
        return True
    return None


def run_abc(abc: str, command: str, cwd: Path, timeout: int) -> str:
    """Run ``abc -q <command>`` (no shell) in ``cwd``; return combined stdout+stderr."""
    try:
        proc = subprocess.run([abc, "-q", command], cwd=cwd, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as exc:
        raise UnprovenError(f"unproven: ABC {command.split()[0]} timed out after {timeout} "
                            "seconds") from exc
    except OSError as exc:
        raise EquivError(f"running ABC failed: {exc}") from exc
    output = proc.stdout + proc.stderr
    if proc.returncode < 0:
        raise HardError(f"ABC killed by signal {-proc.returncode}", output)
    if proc.returncode != 0:
        raise EquivError(f"ABC exited with status {proc.returncode}", output)
    return output


def _io_mismatch(a: tuple[set[str], set[str]], b: tuple[set[str], set[str]]) -> list[str]:
    report = []
    for kind, sa, sb in (("inputs", a[0], b[0]), ("outputs", a[1], b[1])):
        if sa != sb:
            report.append(f"primary {kind} differ: only in A: {sorted(sa - sb)}; "
                          f"only in B: {sorted(sb - sa)}")
    return report


XCONE_NET = "unconn"  # Parmys's don't-care net: `.names unconn`, no rows (constant 0)
EXIT_XCONE = 3
EXIT_UNPROVEN = 4
EXIT_HARD = 5


def x_sources(flat: Flat) -> set[str]:
    """The flat nets that are ``unconn`` (a constant cover with no inputs and no rows, named
    ``unconn`` in its model)."""
    return {n.output for n in flat.names
            if not n.inputs and not n.rows and n.output.rsplit(_SEP, 1)[-1] == XCONE_NET}


def x_reached(flat: Flat) -> set[str]:
    """The flat nets that the x sources reach going forward, through covers, latches and cut
    boxes (x on any input pin of a cut box taints all its output pins), the sources included."""
    fanout: dict[str, list[str]] = {}
    for names in flat.names:
        for net in names.inputs:
            fanout.setdefault(net, []).append(names.output)
    for latch in flat.latches:
        fanout.setdefault(latch.input, []).append(latch.output)
    for box in flat.blackboxes:
        for net in box.inputs.values():
            fanout.setdefault(net, []).extend(box.outputs.values())
    reached = set(x_sources(flat))
    work = list(reached)
    while work:
        for nxt in fanout.get(work.pop(), []):
            if nxt not in reached:
                reached.add(nxt)
                work.append(nxt)
    return reached


def x_cone_outputs(flat: Flat) -> set[str]:
    """The primary outputs (original names) of ``flat`` that the x sources reach going forward,
    through covers and latches."""
    reached = x_reached(flat)
    return {out for out in flat.outputs if out in reached}


def prove(flats: Sequence[Flat], maps: Sequence[dict[str, str]], abc: str,
          args: argparse.Namespace) -> tuple[bool, str, str]:
    """(equivalent, ABC command, ABC output) for the two flats on their current outputs."""
    verb = "dsec" if any(f.latches for f in flats) else "cec"
    if verb == "dsec":
        for flat in flats:
            if not flat.latches:
                add_dangling_latch(flat)
    tmp = Path(tempfile.mkdtemp(prefix="equiv-check-"))
    try:
        write_flat(flats[0], maps[0], tmp / "a.blif")
        write_flat(flats[1], maps[1], tmp / "b.blif")
        output = run_abc(abc, f"{verb} a.blif b.blif", tmp, args.timeout)
    finally:
        if args.keep_temp:
            notice(f"temporary files kept in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    verdict = parse_verdict(output)
    if verdict is None:
        if _UNDECIDED_RE.search(output):  # dsec's own engine limit (a frame's 60 s), no verdict
            raise UnprovenError(f"unproven: ABC {verb} ended UNDECIDED (its own time or depth "
                                "limit, not --timeout)", output)
        raise EquivError(f"could not find a verdict in ABC {verb} output", output)
    return verdict, verb, output


def x_report(flats: Sequence[Flat], maps: Sequence[dict[str, str]], abc: str,
             args: argparse.Namespace) -> int:
    """The ``--x-report`` rerun after a failed check: exit 3 when the netlists agree outside the
    x-cones, else 1 (nothing in a cone, nothing left, or still different)."""
    removed = {m[o] for f, m in zip(flats, maps, strict=True) for o in x_cone_outputs(f)}
    if not removed:
        print(f"{TOOL}: x-report: no output is in an x-cone ({XCONE_NET} reaches none)")
        return 1
    total = len(flats[0].outputs)
    print(f"{TOOL}: x-report: {len(removed)} of {total} outputs are in an x-cone "
          f"({XCONE_NET} reaches them in A or B)")
    if len(removed) >= total:
        print(f"{TOOL}: NOT equivalent outside x-cones: no output is left to compare")
        return 1
    for flat, ports in zip(flats, maps, strict=True):
        flat.outputs = [o for o in flat.outputs if ports[o] not in removed]
    verdict, verb, output = prove(flats, maps, abc, args)
    if not verdict:
        print(f"{TOOL}: NOT equivalent outside x-cones (abc {verb})")
        print(output.rstrip())
        return 1
    print(f"{TOOL}: equivalent outside x-cones (abc {verb})")
    for name in sorted(removed):
        print(f"x-cone: {name}")
    return EXIT_XCONE


def _pair_boxes(flats: Sequence[Flat], args: argparse.Namespace) -> tuple[int, int, int]:
    """The ``--match-unnamed`` steps in order: turn dual-port RAMs to one port order, pair the RAM
    slices by memory group and the unnamed boxes that simulation or a probe can tell apart, pair
    the rest by signature.  Returns (unnamed boxes paired, of them guessed or found by a probe, of
    them found by a probe)."""
    if not args.match_unnamed:
        return 0, 0, 0
    normalize = not args.no_io_normalize
    ram = RamPairing()
    if not args.no_dual_orient:
        oriented = orient_dual_ports(flats, normalize)
        if oriented:
            notice(f"{oriented} dual_port_ram instance(s) turned so that port 1 is the port with "
                   "the smaller input history (the two ports are taken as interchangeable)")
    if not args.no_ram_pairing:
        ram = pair_ram_groups(flats, normalize,
                              None if args.no_ram_probe else lambda: find_abc(args.abc),
                              (args.ram_probe_budget, args.ram_probe_timeout),
                              (args.box_probe_budget, args.box_probe_timeout))
    paired, guessed = pair_unnamed(flats, normalize)
    if ram.slices:
        notice(f"{ram.slices} RAM slice(s) paired by memory group and bit"
               + (f" ({ram.probed} of them in groups told apart by ABC probes)" if ram.probed
                  else ""))
    probed = ram.probed + ram.boxes_probed
    return paired + ram.boxes, guessed + probed + ram.weak + ram.tied, probed


def _check(args: argparse.Namespace) -> int:
    nets = [blif.parse_file(f, gates=not args.no_cut) for f in args.files]
    listed = frozenset(m for m in args.blackbox.split(",") if m) if args.blackbox else frozenset()
    flats = [flatten(n, args.strict_init, args.allow_undriven, not args.no_cut,
                     not args.no_io_normalize, check=False, match_unnamed=args.match_unnamed,
                     blackbox=listed)
             for n in nets]
    if listed:
        pair_listed(flats, listed)
    paired, guessed, probed = _pair_boxes(flats, args)
    match_blackboxes(flats)
    for flat in flats:
        check_undriven(flat, args.allow_undriven)
    if paired:
        notice(f"{paired} unnamed black-box instance(s) paired by simulation signature"
               + (f" ({guessed} of them guessed by structure)" if guessed else ""))
    cuts = sum(len(f.blackboxes) for f in flats)
    if cuts:
        notice(f"{cuts // 2} black-box instance(s) cut: outputs are free inputs shared by both "
               "files, inputs are compared; a clocked black box's internal state is not modelled")
    maps = [io_map(f, not args.no_io_normalize) for f in flats]
    domains = [clock_domain(f, m) for f, m in zip(flats, maps, strict=True)]
    sets = [({m[p] for p in f.inputs}, {m[p] for p in f.outputs}) for f, m in zip(flats, maps,
                                                                                    strict=True)]
    mismatch = _io_mismatch(sets[0], sets[1])
    if mismatch:
        print(f"{TOOL}: NOT equivalent: interfaces differ")
        for line in mismatch:
            print(f"  {line}")
        return 1
    if domains[0] is not None and domains[1] is not None and domains[0] != domains[1]:
        print(f"{TOOL}: NOT equivalent: latch clocking differs: A is {domains[0]}, "
              f"B is {domains[1]} (type, clock)")
        return 1
    abc = find_abc(args.abc)
    verdict, verb, output = prove(flats, maps, abc, args)
    print(f"{TOOL}: {'equivalent' if verdict else 'NOT equivalent'} (abc {verb})")
    if verdict:
        return 0
    print(output.rstrip())
    code = x_report(flats, maps, abc, args) if args.x_report else 1
    weak = guessed if code == 1 else guessed - probed if code == EXIT_XCONE else 0
    if weak > 0:  # a pair found by a probe is a proof of equal input cones, not a guess
        what = ("\"not equivalent\" may only mean a wrong guess" if code == 1 else
                "the x-cones, which hold every pin of a box fed by unconn, hide what a wrong "
                "guess breaks, so \"equivalent outside the x-cones\" says nothing about the "
                "boxes")
        print(f"{TOOL}: inconclusive: {weak} unnamed black-box pair(s) were guessed or are "
              "interchangeable (equal signatures, split by structure, file order or slice "
              f"index), so {what}", file=sys.stderr)
        return EXIT_UNPROVEN
    return code


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog=TOOL,
        description="Prove two BLIF netlists functionally equivalent with ABC cec/dsec. "
        "Exit 0 equivalent, 1 not equivalent, 2 usage/input/ABC error, 3 (--x-report) "
        "equivalent outside the x-cones.",
        epilog="Latches with init 2 (don't care) or 3 (unknown) are treated as starting at 0; "
        "use --strict-init to reject them. Each file must clock all its latches with one "
        "(edge type, primary-input clock) pair, the same in both files.",
    )
    ap.add_argument("files", nargs=2, metavar="FILE.blif")
    ap.add_argument("--abc", metavar="PATH", help="ABC binary to use")
    ap.add_argument("--keep-temp", action="store_true", help="keep the temp dir for ABC")
    ap.add_argument("--strict-init", action="store_true",
                    help="latch init 2 (don't care) and 3 (unknown), and a missing init, are "
                    "treated as 0 by default (with a notice); this flag makes them an "
                    "error (exit 2) instead")
    ap.add_argument("--allow-undriven", action="store_true",
                    help="tie undriven nets that reach an output or latch to 0 instead of "
                    "failing (exit 2)")
    ap.add_argument("--blackbox", metavar="M[,M...]", default="",
                    help="cut the instances of these models (for example "
                    "adder,multiply,single_port_ram,dual_port_ram) and pair them across the "
                    "files by .cname when both have one, else by occurrence order per model; "
                    "their outputs become shared primary inputs and their inputs are compared "
                    "outputs; a model with a different number of instances in the two files "
                    "is an error (exit 2)")
    ap.add_argument("--no-cut", action="store_true",
                    help="reject black boxes other than adder (exit 2) instead of cutting them "
                    "out as free outputs / compared inputs matched by instance name")
    ap.add_argument("--no-io-normalize", action="store_true",
                    help="match primary I/O names verbatim")
    ap.add_argument("--match-unnamed", "--match-dollar", dest="match_unnamed",
                    action="store_true",
                    help="match every black box without a .cname by simulation signature (type, "
                    "parameters, ports and the response of each input bit) instead of by its "
                    "output net names, which Parmys numbers with counters ($mul~14); sound, "
                    "since a paired box must have equal input functions, so a wrong pairing is "
                    "a false 'not equivalent' (or exit 2), never a false 'equivalent' "
                    "(--match-dollar is the old spelling)")
    ap.add_argument("--no-dual-orient", action="store_true",
                    help="with --match-unnamed: do not turn unnamed dual_port_ram instances to a "
                    "canonical port order first (ports 1 and 2 are then not interchangeable)")
    ap.add_argument("--box-probe-budget", type=int, default=SINGLE_PROBE_BUDGET, metavar="SECONDS",
                    help="with --match-unnamed: seconds for all the ABC probes that tell unnamed "
                    "boxes of no memory (multipliers) apart (default %(default)s)")
    ap.add_argument("--box-probe-timeout", type=int, default=SINGLE_PROBE_TIMEOUT,
                    metavar="SECONDS", help="seconds for one such probe (default %(default)s)")
    ap.add_argument("--no-ram-pairing", action="store_true",
                    help="with --match-unnamed: do not pair Parmys RAM slices by memory group and "
                    "bit first (pair every unnamed box by signature)")
    ap.add_argument("--no-ram-probe", action="store_true",
                    help="with --match-unnamed: pair RAM groups by simulation only, no ABC probes "
                    "for the groups it cannot tell apart")
    ap.add_argument("--ram-probe-budget", type=int, default=RAM_PROBE_BUDGET, metavar="SECONDS",
                    help="seconds for all the ABC probes that tell RAM groups apart "
                    "(default %(default)s)")
    ap.add_argument("--ram-probe-timeout", type=int, default=RAM_PROBE_TIMEOUT, metavar="SECONDS",
                    help="seconds for one ABC probe (default %(default)s); proving a difference "
                    "takes longer than that, and a probe that times out counts as 'not equal'")
    ap.add_argument("--x-report", action="store_true",
                    help="when not equivalent, rerun without the outputs in an x-cone (reached "
                    "by `unconn`) and list them; exit 3 if equivalent outside them")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                    help="ABC timeout in seconds (default %(default)s)")
    return ap


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        return _check(args)
    except blif.BlifError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return 2
    except EquivError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        if exc.detail:
            print(exc.detail.rstrip(), file=sys.stderr)
        return exc.exit_code
    except MemoryError:
        print(f"{TOOL}: out of memory", file=sys.stderr)
        return EXIT_HARD
    except Exception as exc:  # noqa: BLE001  (exit 2: exit 1 is only ever 'not equivalent')
        traceback.print_exc()
        print(f"{TOOL}: internal error: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
