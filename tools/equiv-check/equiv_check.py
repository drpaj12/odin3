"""equiv-check: functional equivalence of two BLIF netlists using Berkeley ABC.

Usage::

    equiv-check A.blif B.blif [--abc PATH] [--keep-temp] [--strict-init] [--no-io-normalize]
                [--no-cut] [--match-unnamed] [--x-report]

Exit 0 equivalent, 1 not equivalent (including mismatched primary I/O sets and black-box
cut-point inputs that are wired differently), 2 usage, input, unsupported-construct or ABC
error, and black-box instances that cannot be matched one-to-one; 3 only with ``--x-report``:
not equivalent, but equivalent outside the x-cones (below).

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
only is removed whole.

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
instances with one name, an instance with no usable name, a different cell type or different
port widths all exit 2 with a message listing them.  The cut makes two netlists equivalent
only when everything around the black boxes is equivalent and each box's inputs and outputs
connect identically; a box's behaviour is assumed equal, never checked.  A black box clocked
by a primary clock is fine (its outputs are free inputs), but a clocked black box's internal
state is not modelled: only its connections are compared.  The cut is on whenever black
boxes are present; ``--no-cut`` restores the old behaviour of rejecting them (exit 2, and
``.gate`` is rejected as an unsupported directive).

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
  and buffers looked through) are interchangeable: paired in file order, and their outputs are
  one shared free variable per pin;
- several with one signature but different input nets (copies of one register, say) are split
  by the structure of their input cones (colour refinement) and paired in file order inside a
  part: these are *guesses*;
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
guess may be at fault, so the result is "inconclusive" (exit 2) and not "not equivalent" (a
failure outside the guesses, an x-cone result and an equivalence are all still verdicts).  As
before, a box's internal behaviour is assumed, not checked (RAM contents, initial state).

ABC models every latch as a flip-flop on one implicit clock, so each file must use a single
(edge type, clock) pair -- the clock being a primary input, possibly through buffers -- and
both files must use the same pair (else exit 1).  Level-sensitive (ah/al) and asynchronous
(as) latches, internal clocks and several clock domains are rejected (exit 2).  Latch init
2/3 (don't care/unknown) or a missing init is treated as 0 with a notice (``--strict-init``
makes it an error).  An undriven net that reaches a primary output or a latch input is an
error (``--allow-undriven`` ties it to 0); a non-black-box model with ports but no cells is
an error ("mark it .blackbox").

ABC lookup order: ``--abc PATH``, ``$ODIN3_ABC``, ``build/*/third_party/abc/abc`` in the
repository, ``$VTR_ROOT/build/abc/abc``, ``abc`` on ``PATH``.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "blif"))
import bbmatch  # noqa: E402
import blif  # noqa: E402

TOOL = "equiv-check"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TIMEOUT = 600
MATCH_ROUNDS = 12  # most simulate-and-anchor rounds of pair_unnamed

# Verdict strings printed by ABC (see third_party/abc/src):
#   cec  -> Abc_NtkCecFraig, base/abci/abcVerify.c:207,210,233,237 (and :174,:185 strash)
#   dsec -> Fra_FraigSec,    proof/fra/fraSec.c:248,480,633,657,675
# Both spell "Networks are equivalent" / "Networks are NOT EQUIVALENT"; some other ABC
# commands use "NOT equivalent", hence the case-insensitive match.
_NOT_EQUIV_RE = re.compile(r"\bNetworks are NOT EQUIVALENT\b", re.IGNORECASE)
_EQUIV_RE = re.compile(r"\bNetworks are equivalent\b", re.IGNORECASE)

XOR3 = [("100", "1"), ("010", "1"), ("001", "1"), ("111", "1")]
MAJ3 = [("11-", "1"), ("1-1", "1"), ("-11", "1")]
# Separator for instance-path prefixes.  A space can never occur inside a parsed BLIF
# token, so prefixed nets cannot collide with original names; all non-port nets are
# renamed before anything is written.
_SEP = " "
_MAX_DEPTH = 1000


class EquivError(Exception):
    """Usage, input or ABC error (exit code 2).  ``detail`` is extra multi-line output."""

    def __init__(self, message: str, detail: str = "") -> None:
        super().__init__(message)
        self.detail = detail


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
    for cand in sorted(REPO_ROOT.glob("build/*/third_party/abc/abc")):
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
                 normalize: bool, match_unnamed: bool = False) -> None:
        self.netlist = netlist
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
        conns = {f: frame.resolve(a) for f, a in cell.conns}
        if sub is None and cell.model == "adder":
            sub = _gate_adder_model(cell)
        if sub is None or (sub.blackbox and sub.name != "adder"):
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

    def add_blackbox(self, cell: blif.Subckt, sub: blif.Model | None, frame: _Frame) -> None:
        """Cut a black-box instance: outputs become inputs, inputs become outputs."""
        if not self.cut:
            raise EquivError(f"{self.netlist.path}:{cell.line}: unsupported black box "
                             f"{cell.model}")
        names = _canonical_formals([f for f, _ in cell.conns])
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
        if self.match_unnamed:
            return None
        for formal, net in pins.items():
            if is_out[formal] and _SEP not in net and not net.startswith("$"):
                base, bit = _formal_order(formal)  # width-independent: out[0] and out agree
                return f"{base}{f'[{bit}]' if bit else ''}={norm(net)}"
        raise EquivError(f"{self.netlist.path}:{cell.line}: cannot name black box "
                         f"{cell.model}: no .cname and no named output net to match it by")

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
            match_unnamed: bool = False) -> Flat:
    """Flatten ``netlist`` into one model; black boxes are cut (``cut``) or rejected.

    ``check=False`` leaves the undriven-net check to the caller (``check_undriven``), so that
    black-box mismatches between the two files are reported first."""
    flat = _Flattener(netlist, strict_init, cut, normalize, match_unnamed).run()
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
        result = bbmatch.pair([sigs[0][i] for i in free[0]], [sigs[1][i] for i in free[1]])
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


def match_blackboxes(flats: Sequence[Flat]) -> None:
    """Require the cut black boxes of two flats to match one-to-one by name, type and ports.

    Raises EquivError (exit 2) listing every unmatched instance or mismatch."""
    fa, fb = flats
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
        raise EquivError("matched black-box instances do not have the same cell type and "
                         "port widths", "\n".join(f"  {p}" for p in problems))


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


def _clock_source(flat: Flat, net: str) -> str | None:
    """Follow single-input buffers back from ``net``; the primary input reached, or None."""
    buffers = {n.output: n.inputs[0] for n in flat.names if _is_buffer(n)}
    inputs = set(flat.inputs)
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
        source = _clock_source(flat, latch.control)
        if source is None:
            raise EquivError(f"{flat.path}:{latch.line}: unsupported: latch "
                             f"{show(latch.output)} is clocked by internal net "
                             f"{show(latch.control)}")
        domains.add((latch.ltype, ports[source]))
    if len(domains) > 1:
        raise EquivError(f"{flat.path}: unsupported: more than one clock domain "
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
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise EquivError(f"running ABC failed: {exc}") from exc
    output = proc.stdout + proc.stderr
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


def x_sources(flat: Flat) -> set[str]:
    """The flat nets that are ``unconn`` (a constant cover with no inputs and no rows, named
    ``unconn`` in its model)."""
    return {n.output for n in flat.names
            if not n.inputs and not n.rows and n.output.rsplit(_SEP, 1)[-1] == XCONE_NET}


def x_cone_outputs(flat: Flat) -> set[str]:
    """The primary outputs (original names) of ``flat`` that the x sources reach going forward,
    through covers and latches."""
    fanout: dict[str, list[str]] = {}
    for names in flat.names:
        for net in names.inputs:
            fanout.setdefault(net, []).append(names.output)
    for latch in flat.latches:
        fanout.setdefault(latch.input, []).append(latch.output)
    reached = set(x_sources(flat))
    work = list(reached)
    while work:
        for nxt in fanout.get(work.pop(), []):
            if nxt not in reached:
                reached.add(nxt)
                work.append(nxt)
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


def _check(args: argparse.Namespace) -> int:
    nets = [blif.parse_file(f, gates=not args.no_cut) for f in args.files]
    flats = [flatten(n, args.strict_init, args.allow_undriven, not args.no_cut,
                     not args.no_io_normalize, check=False, match_unnamed=args.match_unnamed)
             for n in nets]
    paired, guessed = (pair_unnamed(flats, not args.no_io_normalize) if args.match_unnamed
                       else (0, 0))
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
    if code == 1 and guessed:
        print(f"{TOOL}: inconclusive: {guessed} unnamed black-box pair(s) were guessed (equal "
              "signatures, different input nets, split by structure and file order), so \"not "
              "equivalent\" may only mean a wrong guess", file=sys.stderr)
        return 2
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
        return 2


if __name__ == "__main__":
    sys.exit(main())
