"""equiv-check: functional equivalence of two BLIF netlists using Berkeley ABC.

Usage::

    equiv-check A.blif B.blif [--abc PATH] [--keep-temp] [--strict-init] [--no-io-normalize]

Exit 0 equivalent, 1 not equivalent (including mismatched primary I/O sets), 2 usage,
input, unsupported-construct or ABC error.

Each netlist is flattened in Python into one model (``.subckt`` of models defined in the
same file are inlined; known VTR hard blocks are replaced by logic; other black boxes are
rejected), primary I/O names are normalized so Parmys and Odin II spellings meet, internal
nets get plain names, and ABC runs ``cec`` (no latches) or ``dsec`` (latches) on the two
flat files; if only one side has latches, the other gets a dangling const-0 latch.

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
import blif  # noqa: E402

TOOL = "equiv-check"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TIMEOUT = 600

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
class Flat:
    """A flattened, black-box-free netlist ready for ABC."""

    path: str
    top: str
    inputs: list[str]
    outputs: list[str]
    names: list[blif.Names] = field(default_factory=list)
    latches: list[blif.Latch] = field(default_factory=list)


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


class _Flattener:
    def __init__(self, netlist: blif.Netlist, strict_init: bool, allow_undriven: bool) -> None:
        self.netlist = netlist
        self.models = netlist.by_name()
        self.strict_init = strict_init
        self.allow_undriven = allow_undriven
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
        self.check_undriven()
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
        sub = self.models[cell.model]
        conns = {f: frame.resolve(a) for f, a in cell.conns}
        if sub.blackbox:
            if sub.name != "adder":
                raise EquivError(f"{self.netlist.path}:{cell.line}: unsupported black box "
                                 f"{sub.name}")
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

    def check_undriven(self) -> None:
        """Undriven nets that reach a primary output or a latch D input are an error (they
        would silently become constants); with --allow-undriven, and for undriven nets that
        only feed dangling logic, they are tied to 0 with a notice."""
        flat = self.flat
        driven = set(flat.inputs)
        driven.update(c.output for c in flat.names)
        driven.update(c.output for c in flat.latches)
        used: dict[str, None] = dict.fromkeys(flat.outputs)
        for names in flat.names:
            used.update(dict.fromkeys(names.inputs))
        used.update(dict.fromkeys(c.input for c in flat.latches))
        undriven = sorted(n for n in used if n not in driven)
        if not self.allow_undriven:
            self._reject_observable(undriven)
        for net in undriven:
            flat.names.append(blif.Names(inputs=[], output=net))
        if undriven:
            notice(f"{flat.path}: {len(undriven)} undriven net(s) tied to 0")

    def _reject_observable(self, undriven: list[str]) -> None:
        flat = self.flat
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


def flatten(netlist: blif.Netlist, strict_init: bool = False,
            allow_undriven: bool = False) -> Flat:
    """Flatten ``netlist`` into one model without black boxes (see module docstring)."""
    return _Flattener(netlist, strict_init, allow_undriven).run()


# --------------------------------------------------------------------------- clocking

_UNSUPPORTED_LATCH_TYPES = {"ah": "level-sensitive", "al": "level-sensitive",
                            "as": "asynchronous"}


def _clock_source(flat: Flat, net: str) -> str | None:
    """Follow single-input buffers back from ``net``; the primary input reached, or None."""
    buffers = {n.output: n.inputs[0] for n in flat.names
               if len(n.inputs) == 1 and n.rows == [("1", "1")]}
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
    lines += [f".inputs {rn(p)}" for p in flat.inputs]
    lines += [f".outputs {rn(p)}" for p in flat.outputs]
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


def _check(args: argparse.Namespace) -> int:
    nets = [blif.parse_file(f) for f in args.files]
    flats = [flatten(n, args.strict_init, args.allow_undriven) for n in nets]
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
    print(f"{TOOL}: {'equivalent' if verdict else 'NOT equivalent'} (abc {verb})")
    if not verdict:
        print(output.rstrip())
    return 0 if verdict else 1


def _parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog=TOOL,
        description="Prove two BLIF netlists functionally equivalent with ABC cec/dsec. "
        "Exit 0 equivalent, 1 not equivalent, 2 usage/input/ABC error.",
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
    ap.add_argument("--no-io-normalize", action="store_true",
                    help="match primary I/O names verbatim")
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
