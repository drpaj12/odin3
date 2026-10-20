"""qor_fcheck_run: ``run_random_check.py`` with an upstream bug and a 2-state gap worked around.

(3H spec §8.1.)

Run as ``python -I -B qor_fcheck_run.py <VTR run_random_check.py> <its arguments>``. VTR's checker
turns a pre-VPR BLIF into Verilog with Yosys and ``blif_to_verilog.pack_bit_blasted_top_ports``,
which packs the bit-blasted top ports (``\\a~3``) into vectors. For a top-level output that Yosys
also declares as a ``wire`` (any output driven by an ``assign``, e.g. a constant bit) the packing
leaves ``wire a[3] ;`` next to ``output [31:0] a;``, and Verilator stops with "Duplicate
declaration". This wrapper drops the ``wire`` line of a name that is also a port before the
packing runs, then hands over to the unchanged script (``runpy``, so its exit code and output are
its own). VTR's tree is only read (``-B``: no bytecode is written there).

The second change is the initial state. Verilator is two-state: every register the RTL does not
reset (or only assigns in some branches of a combinational block, an inferred latch) starts at 0,
while a four-state simulator calls it ``x``. The netlist side is Yosys ``read_blif`` +
``write_verilog``: a ``.latch`` with init 0 or 1 becomes ``reg q = 1'h0|1'h1``, init 2, 3 or none
leaves the register without an initialiser (so it follows the Verilator run's value), and the
``dff``/``dffl`` of sim_hardblocks.v always start at 0. The compiled testbench is therefore run
first with all uninitialised RTL state 0 and compared on every output bit, as before. That is the
same start as the netlist only where the netlist starts at 0 (init 0, 2, 3, none, ``dff``); a
netlist register with init 1 starts at 1 against an RTL 0, and the mismatch that follows is a
mismatch, not a pass. A ``PASS`` covers this one start, the all-zero one, and no other: a netlist
bug that shows only from another start is not seen when this run is clean. No mismatch on any
bit in that run is a ``PASS``. When there are mismatches it is run five more times (only the
vectors that mismatched are kept from each, so the dumps do not pile up), with all uninitialised
state 1 and with four random states (``--x-initial unique`` plus ``+verilator+rand+reset+0|1|2``,
a different ``+verilator+seed`` for each random one; the stimulus has a generator of its own, so
it is the same in every run). A mismatch on a bit whose RTL value, or whose netlist value, differs
between those runs at that vector is on an ``x`` bit (the netlist has registers without a defined
start too, and a design whose RTL never loads a register can show the arbitrary start through
either side):
it is reported as ``XDEPEND``, never as a pass, and a design with only such mismatches is
``UNTESTED`` (exit 3, the caller's ``error``: the netlist cannot be judged on them). Any
mismatch on a bit whose RTL and netlist values are the same in all the runs is a ``MISMATCH``:
``FAIL``, whatever else is ``x``. No vector, and no stretch of vectors, is skipped. The
testbench's own verdict is dropped (it has no initial-state classification); the verdict lines
are written from the dumps, and a run that compares no output
bit is not a pass.

Before any simulation the RTL is pre-checked (``rtl_precheck``): the flow reads it with
``--nolatches``, so an RTL that infers latches (Yosys ``read_verilog`` + ``proc`` without it gives
a ``$dlatch``) or has an incomplete sensitivity list (Odin III's ``read_verilog`` says so) holds
values or evaluates at times the netlist cannot copy. Such a design is ``UNTESTED`` (exit 3) with
the reason (the latch count, the signals) in the log, and nothing is simulated. A design with
neither is simulated as before. A tool that fails on the RTL says nothing.
"""

from __future__ import annotations

import os
import re
import runpy
import subprocess
import sys
import tempfile
from collections.abc import Callable, Iterable, Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

_PORT = re.compile(r"^\s*(?:input|output|inout)(?:\s+wire|\s+reg)?\s+(\\\S+)\s*;", re.M)
_WIRE = re.compile(r"^[ \t]*wire\s+(\\\S+)\s*;[ \t]*\n", re.M)
_FIRST_MODULE = re.compile(r"^\s*module\b.*?^\s*endmodule\b", re.M | re.S)


def drop_port_wires(verilog: str) -> str:
    """Remove ``wire \\name ;`` for every ``name`` that is also declared ``input``, ``output`` or
    ``inout`` (an escaped identifier, as Yosys writes the BLIF's ``a~3``)."""
    top = _FIRST_MODULE.search(verilog)  # the module the packer rewrites
    if top is None:
        return verilog
    body = top.group(0)
    ports = set(_PORT.findall(body))
    body = _WIRE.sub(lambda m: "" if m.group(1) in ports else m.group(0), body)
    return verilog[: top.start()] + body + verilog[top.end():]


# --- RTL pre-check (H1) -------------------------------------------------------------------------

ODIN3_ENV = "QOR_FCHECK_ODIN3"  # the odin3 binary (set by qor_legs.fcheck_command)
TECHLIB_ENV = "QOR_FCHECK_TECHLIB"  # its techlib ($LIB: the hard blocks the RTL instantiates)
_OBJECTS = re.compile(r"^(\d+) objects?\.", re.M)
_SENSITIVITY = re.compile(r"incomplete sensitivity list: '([^']+)' is read but not listed")
_PRECHECK_TIMEOUT_S = 600


def _yosys_latches(rtl: Path, yosys: Path, run: Callable[..., Any]) -> int | None:
    """The ``$dlatch`` cells in Yosys's ``read_verilog`` + ``proc`` of ``rtl`` (no
    ``-nolatches``); None when Yosys could not read it."""
    with tempfile.TemporaryDirectory() as tmp:
        count = Path(tmp) / "count.txt"
        script = f"read_verilog {rtl}; proc; tee -o {count} select -count t:$dlatch"
        done = run([str(yosys), "-q", "-p", script], capture_output=True, text=True,
                   check=False, timeout=_PRECHECK_TIMEOUT_S)
        if done.returncode != 0 or not count.is_file():
            return None
        found = _OBJECTS.search(count.read_text(encoding="utf-8", errors="replace"))
    return int(found.group(1)) if found else None


def _odin_sensitivity(rtl: Path, odin3: Path, techlib: Path | None,
                      run: Callable[..., Any]) -> list[str] | None:
    """The signals Odin III reports as read but missing from a sensitivity list (sorted, each
    once); None when Odin III could not read ``rtl``."""
    lib = f"read_techlib {techlib}; " if techlib is not None else ""
    done = run([str(odin3), "-p", f"{lib}read_verilog --vtr-compat {rtl}"], capture_output=True,
               text=True, check=False, timeout=_PRECHECK_TIMEOUT_S)
    if done.returncode != 0:
        return None
    return sorted(set(_SENSITIVITY.findall(done.stdout + done.stderr)))


def rtl_precheck(rtl: Path, yosys: Path | None, odin3: Path | None,
                 run: Callable[..., Any] = subprocess.run,
                 techlib: Path | None = None) -> list[str]:
    """Why ``rtl`` cannot be judged against a ``--nolatches`` netlist: one text per reason, empty
    for an RTL with neither inferred latches nor incomplete sensitivity lists (and for a tool that
    is missing or fails: that checks nothing, it never hides a design)."""
    reasons = []
    latches = _yosys_latches(rtl, yosys, run) if yosys is not None else None
    if latches:
        reasons.append(f"the RTL infers {latches} latches (Yosys read_verilog + proc without "
                       "--nolatches gives $dlatch cells); the flow reads it with --nolatches "
                       "and drops their held values")
    signals = _odin_sensitivity(rtl, odin3, techlib, run) if odin3 is not None else None
    if signals:
        reasons.append("the RTL has incomplete sensitivity lists (read but not listed: "
                       f"{', '.join(signals)}); the flow builds them as @*")
    return reasons


def render_precheck(reasons: Sequence[str]) -> str:
    """The log text of a failed pre-check: one ``PRECHECK`` line per reason and the ``UNTESTED:``
    line the callers (``qor_legs.fcheck_verdict``) read."""
    lines = [f"PRECHECK {reason}" for reason in reasons]
    lines.append("UNTESTED: the RTL behaviour is not preserved by the flow's synthesis "
                 "(--nolatches); nothing was simulated: " + "; ".join(reasons))
    return "\n".join(lines) + "\n"


# --- initial state: classify mismatches by the RTL's dependence on it -------------------------

_MAX_REPORT = 20  # MISMATCH and XDEPEND lines written, each, as the testbench's own limit
_NEVER = "2147483647"  # the testbench's abort limit, out of reach: every vector is dumped
_TALLY = "            if (vecFail) begin\n                vecMismatched++;"
# The runs: (+verilator+rand+reset+N, seed offset): uninitialised state all 0 (the decisive run),
# all 1, then four random patterns (a bit that is x slips through all six with chance about 2**-6).
_INIT_RUNS = ((0, 0), (1, 0), (2, 1), (2, 2), (2, 3), (2, 4))
_RESET_ARG = "+verilator+rand+reset+"
_SEED_ARG = "+verilator+seed+"
_SEED = re.compile(r"localparam int SEED_HINT = (\d+);")
_RANDOM = '''\
    // the wrapper's own stimulus generator: Verilator's seed also draws the random initial
    // state of the later runs, and must change nothing but that state
    reg [31:0] rnd_state = 32'h{state:08x};
    function automatic [31:0] rnd32();
        rnd_state = rnd_state ^ (rnd_state << 13);
        rnd_state = rnd_state ^ (rnd_state >> 17);
        rnd_state = rnd_state ^ (rnd_state << 5);
        return rnd_state;
    endfunction
'''


def instrument_testbench(tb: str, outputs: Sequence[tuple[str, int]]) -> str:
    """The generated testbench with a dump of every output port per vector and no verdict of its
    own: one line ``VD <rtl hex>:<synth hex> ...`` (the outputs in port order) before each
    vector's tally, its own random generator for the stimulus (not ``$urandom``), its abort limit
    out of reach, no mismatch printing, and its summary and
    ``PASS``/``FAIL`` lines renamed ``RAW_*`` (so only this wrapper's lines count) without the
    ``$fatal``. Raises ``RuntimeError`` when the generator's text no longer has the expected
    shape: the wrapper must then be updated, never run on a testbench it did not understand."""
    if tb.count(_TALLY) != 1:
        raise RuntimeError("qor_fcheck_run: the generated testbench has no single vector tally; "
                           "update tools/vtr-qor/qor_fcheck_run.py to the new tb_generator")
    fmt = " ".join("%h:%h" for _ in outputs)
    args = ", ".join(f"{name}_rtl, {name}_synth" for name, _ in outputs)
    dump = f'            $display("VD {fmt}"{", " + args if args else ""});\n'
    out = tb.replace(_TALLY, dump + _TALLY)
    seed = _SEED.search(out)
    if seed is None:
        raise RuntimeError("qor_fcheck_run: the generated testbench has no SEED_HINT; update "
                           "tools/vtr-qor/qor_fcheck_run.py to the new tb_generator")
    state = (0x9E3779B9 ^ (int(seed.group(1)) * 0x85EBCA6B)) & 0xFFFFFFFF or 1
    out = out.replace(seed.group(0), seed.group(0) + "\n" + _RANDOM.format(state=state), 1)
    out = out.replace("$urandom_range(0, 1)", "(rnd32() >> 31)").replace("$urandom()", "rnd32()")
    out, limits = re.subn(r"(MAX_ERRORS = )\d+;", rf"\g<1>{_NEVER};", out)
    out = out.replace("if (errors < MAX_ERRORS) begin", "if (1'b0) begin")
    for word in ("VCHECK_SUMMARY", "PASS:", "FAIL:"):
        out = out.replace(f'$display("{word}', f'$display("RAW_{word}')
    out = out.replace("$fatal(1);", "$finish;")
    if limits != 1 or "RAW_VCHECK_SUMMARY" not in out or re.search(r"\$urandom\w*\s*\(", out):
        raise RuntimeError("qor_fcheck_run: the generated testbench has an unexpected limit or "
                           "summary; update tools/vtr-qor/qor_fcheck_run.py to the new "
                           "tb_generator")
    return out


@dataclass
class InitVerdict:
    """The comparison of one design over its initial-state runs."""

    matched: int = 0  # vectors without any mismatch
    mismatched: int = 0  # vectors with a mismatch on a bit that does not depend on the state
    x_dependent: int = 0  # vectors whose only mismatches are on bits that do
    port_errors: int = 0
    x_port_errors: int = 0
    compared_bits: int = 0
    lines: list[str] = field(default_factory=list)  # the first MISMATCH lines
    x_lines: list[str] = field(default_factory=list)  # the first XDEPEND lines

    @property
    def vectors(self) -> int:
        return self.matched + self.mismatched + self.x_dependent

    @property
    def passed(self) -> bool:
        return (self.matched > 0 and self.mismatched == 0 and self.x_dependent == 0
                and self.compared_bits > 0)


def _bits(value: int, width: int, unknown: int) -> str:
    """``value`` as ``width`` binary digits, ``x`` where ``unknown`` has a 1."""
    return "".join("x" if unknown >> i & 1 else str(value >> i & 1)
                   for i in range(width - 1, -1, -1))


def parse_dump(line: str, widths: Sequence[int]) -> list[tuple[int, int]] | None:
    """The ``(rtl, synth)`` values of one ``VD`` line, ``None`` for any other line. Raises
    ``ValueError`` for a ``VD`` line that does not hold one pair per port."""
    if not line.startswith("VD "):
        return None
    fields = line[3:].split()
    if len(fields) != len(widths):
        raise ValueError(f"dump line has {len(fields)} values for {len(widths)} output ports")
    pairs = []
    for field_text in fields:
        rtl, synth = field_text.split(":")
        pairs.append((int(rtl, 16), int(synth, 16)))
    return pairs


def _dump_rows(lines: Iterable[str], widths: Sequence[int]) -> Iterator[list[tuple[int, int]]]:
    for line in lines:
        row = parse_dump(line.rstrip("\n"), widths)
        if row is not None:
            yield row


def compare_runs(outputs: Sequence[tuple[str, int]], runs: Sequence[Iterable[str]],
                 indices: Sequence[int] | None = None) -> InitVerdict:
    """Compare ``runs[0]`` (all uninitialised state 0) vector by vector, RTL against netlist, on
    every output bit. With more runs (other initial states), a mismatching bit whose RTL value, or
    whose netlist value, differs from ``runs[0]``'s in any of them at that vector is ``x``
    (XDEPEND); every other
    mismatch is real. A vector is ``mismatched`` when it has a real mismatch, ``x_dependent``
    when it has only x ones, ``matched`` otherwise. Raises ``ValueError`` when the runs dumped a
    different number of vectors. ``indices`` names the vectors the dumps hold when they were
    filtered (``keep_vectors``); the others are not counted."""
    widths = [w for _, w in outputs]
    verdict = InitVerdict()
    for pos, rows in enumerate(zip(*(_dump_rows(run, widths) for run in runs), strict=True)):
        index = pos if indices is None else indices[pos]
        real_ports = x_ports = 0
        for i, (name, width) in enumerate(outputs):
            rtl, syn = rows[0][i]
            bad = rtl ^ syn
            if not bad:
                continue
            unknown = syn_unknown = 0
            for row in rows[1:]:
                unknown |= rtl ^ row[i][0]
                syn_unknown |= syn ^ row[i][1]  # H2: the netlist's start is arbitrary too
            line = f"vec={index} port={name} rtl={_bits(rtl, width, unknown)} " \
                   f"synth={_bits(syn, width, syn_unknown)}"
            if bad & ~(unknown | syn_unknown):
                real_ports += 1
                if len(verdict.lines) < _MAX_REPORT:
                    verdict.lines.append("MISMATCH " + line)
            else:
                x_ports += 1
                if len(verdict.x_lines) < _MAX_REPORT:
                    verdict.x_lines.append("XDEPEND " + line)
        verdict.compared_bits += sum(w for _, w in outputs)
        verdict.port_errors += real_ports
        verdict.x_port_errors += x_ports
        if real_ports:
            verdict.mismatched += 1
        elif x_ports:
            verdict.x_dependent += 1
        else:
            verdict.matched += 1
    return verdict


def mismatching_vectors(outputs: Sequence[tuple[str, int]],
                        run: Iterable[str]) -> tuple[int, list[int]]:
    """The number of vectors in ``run`` and the indices of those with any RTL/netlist mismatch."""
    bad = []
    count = 0
    for count, row in enumerate(_dump_rows(run, [w for _, w in outputs]), 1):
        if any(rtl != syn for rtl, syn in row):
            bad.append(count - 1)
    return count, bad


def keep_vectors(path: Path, keep: set[int]) -> None:
    """Rewrite the dump ``path`` with only the ``VD`` lines of the vectors in ``keep``."""
    tmp = path.with_suffix(".tmp")
    index = 0
    with open(path, encoding="utf-8", errors="replace") as src, open(
            tmp, "w", encoding="utf-8") as dst:
        for line in src:
            if line.startswith("VD "):
                if index in keep:
                    dst.write(line)
                index += 1
    tmp.replace(path)


def render_verdict(verdict: InitVerdict) -> str:
    """The verdict lines the callers (``qor_legs.fcheck_verdict``) read."""
    lines = verdict.lines + verdict.x_lines
    if verdict.port_errors > len(verdict.lines):
        lines.append(f"... {verdict.port_errors - len(verdict.lines)} more MISMATCH ports")
    if verdict.x_port_errors > len(verdict.x_lines):
        lines.append(f"... {verdict.x_port_errors - len(verdict.x_lines)} more XDEPEND ports")
    lines.append(f"VCHECK_SUMMARY matched={verdict.matched} mismatched={verdict.mismatched} "
                 f"vectors={verdict.vectors} port_errors={verdict.port_errors} "
                 f"x_dependent_vectors={verdict.x_dependent}")
    if verdict.passed:
        lines.append(f"PASS: {verdict.matched} vectors matched across rtl/post-synth")
    elif verdict.mismatched:
        lines.append(f"FAIL: {verdict.port_errors} mismatches in {verdict.vectors} vectors")
    elif verdict.x_dependent:
        lines.append(f"UNTESTED: {verdict.x_port_errors} mismatches in {verdict.x_dependent} "
                     "vectors, every one on a bit whose RTL value depends on the initial state "
                     "(x in a four-state simulation); the netlist cannot be judged on them")
    else:
        lines.append("ERROR: no output bit was compared (the design has no output port)")
    return "\n".join(lines) + "\n"


class _Lines:
    """The lines of a file, read again on every iteration."""

    def __init__(self, path: Path) -> None:
        self.path = path

    def __iter__(self) -> Iterator[str]:
        with open(self.path, encoding="utf-8", errors="replace") as handle:
            yield from handle


def _run_init(argv: list[str], mode: int, seed_offset: int,
              real_call: Callable[..., int]) -> tuple[Path, int]:
    """One run of the compiled testbench with initial state ``mode`` (``+verilator+rand+reset``)
    and the seed raised by ``seed_offset``; its output goes to ``sim_init<n>.out`` next to
    ``sim_build`` (``n`` from the offset and mode)."""
    run_argv = []
    for arg in argv:
        if arg.startswith(_SEED_ARG) and arg[len(_SEED_ARG):].isdigit():
            arg = f"{_SEED_ARG}{int(arg[len(_SEED_ARG):]) + seed_offset}"
        if not arg.startswith(_RESET_ARG):
            run_argv.append(arg)
    run_argv.append(f"{_RESET_ARG}{mode}")
    out = Path(argv[0]).resolve().parent.parent / f"sim_init{mode}_{seed_offset}.out"
    with open(out, "w", encoding="utf-8") as handle:
        code = real_call(run_argv, stdout=handle, stderr=subprocess.STDOUT)
    return out, code


def _run_inits(argv: list[str], outputs: Sequence[tuple[str, int]], stdout: Any,
               real_call: Callable[..., int]) -> int:
    """Run the compiled testbench with all uninitialised state 0; when that shows mismatches, also
    with all 1 and with random states, and classify them. The verdict goes into ``stdout`` (the
    checker's ``sim.out``); the return value is 0 for a pass, 1 for a fail, 3 for untested or
    nothing compared. The binary's own non-zero exit is returned as is, with its output."""
    dumps: list[Path] = []
    header: list[str] = []
    try:
        for mode, offset in _INIT_RUNS:
            out, code = _run_init(argv, mode, offset, real_call)
            if code != 0:
                stdout.write(out.read_text(encoding="utf-8", errors="replace"))
                return code
            dumps.append(out)
            if len(dumps) == 1:
                with open(out, encoding="utf-8", errors="replace") as handle:
                    header = [ln for ln in handle if not ln.startswith(("VD ", "RAW_"))]
                total, bad = mismatching_vectors(outputs, _Lines(out))
                if not bad:  # same start as the netlist and no mismatch: nothing to classify
                    verdict = compare_runs(outputs, [_Lines(out)])
                    break
            keep_vectors(out, set(bad))  # the rest of a big dump is not needed again
        else:
            verdict = compare_runs(outputs, [_Lines(path) for path in dumps], bad)
            clean = total - len(bad)  # the vectors that were not kept matched
            verdict.matched += clean
            verdict.compared_bits += sum(w for _, w in outputs) * clean
    except ValueError as exc:  # a dump that does not fit the ports, or runs of unequal length
        stdout.write(f"ERROR: the initial-state dumps cannot be compared: {exc}\n")
        return 3
    for path in dumps:
        path.unlink()
    stdout.write("".join(header) + render_verdict(verdict))
    stdout.flush()
    return 0 if verdict.passed else (1 if verdict.mismatched else 3)


def _unique_x_initial(argv: list[str]) -> list[str]:
    """The Verilator command with ``--x-initial unique`` (the value is read at run time)."""
    out = list(argv)
    if "--x-initial" in out:
        out[out.index("--x-initial") + 1] = "unique"
    else:
        out += ["--x-initial", "unique"]
    return out


def _install_init_runs() -> bool:
    """Patch the checker's testbench generator and its ``subprocess.call`` (Verilator compile
    with ``--x-initial unique``; the compiled testbench run once per initial state). False when
    upstream's ``tb_generator`` has no ``generate_dual_testbench``."""
    import tb_generator  # type: ignore[import-not-found]  # VTR's, from the script's directory

    original = getattr(tb_generator, "generate_dual_testbench", None)
    if original is None:
        print("qor_fcheck_run: VTR's tb_generator has no generate_dual_testbench (renamed or "
              "removed upstream); update tools/vtr-qor/qor_fcheck_run.py", file=sys.stderr)
        return False
    outputs: list[tuple[str, int]] = []

    def generate(ports: Sequence[Any], **kwargs: Any) -> str:
        outputs[:] = [(p.name, int(p.width)) for p in ports if p.direction == "output"]
        return instrument_testbench(str(original(ports, **kwargs)), outputs)

    real_call = subprocess.call

    def call(cmd: Any, *args: Any, **kwargs: Any) -> int:
        argv = [str(a) for a in cmd] if isinstance(cmd, (list, tuple)) else []
        name = Path(argv[0]).name if argv else ""
        if name == "verilator":
            return int(real_call(_unique_x_initial(argv), *args, **kwargs))
        if name.startswith("Vtb") and any(a.startswith(_RESET_ARG) for a in argv):
            return _run_inits(argv, outputs, kwargs["stdout"], real_call)
        return int(real_call(cmd, *args, **kwargs))

    tb_generator.generate_dual_testbench = generate
    subprocess.call = call  # type: ignore[assignment]
    return True


def _option(args: Sequence[str], name: str) -> str | None:
    """The value after ``name`` in ``args`` (``--name value`` or ``--name=value``), or None."""
    for i, arg in enumerate(args):
        if arg == name and i + 1 < len(args):
            return args[i + 1]
        if arg.startswith(name + "="):
            return arg[len(name) + 1:]
    return None


def _precheck_args(args: Sequence[str], blif_to_verilog: Any) -> list[str]:
    """``rtl_precheck`` of the checker's ``--rtl`` with its Yosys (``--yosys``, else the one
    ``resolve_yosys`` finds) and the odin3 named by ``ODIN3_ENV``; empty when there is no
    ``--rtl`` (a ``--run-dir`` run) or no tool."""
    rtl = _option(args, "--rtl")
    if rtl is None:
        return []
    given = _option(args, "--yosys")
    resolve = getattr(blif_to_verilog, "resolve_yosys", None)
    yosys: Path | None = Path(given) if given and Path(given).is_file() else None
    if yosys is None and resolve is not None:
        try:
            yosys = resolve(Path(given) if given else None)
        except (FileNotFoundError, OSError):
            yosys = None
    odin = os.environ.get(ODIN3_ENV)
    odin3 = Path(odin) if odin and Path(odin).is_file() else None
    lib = os.environ.get(TECHLIB_ENV)
    return rtl_precheck(Path(rtl), yosys, odin3, techlib=Path(lib) if lib else None)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: qor_fcheck_run.py <run_random_check.py> [args...]", file=sys.stderr)
        return 2
    script = Path(argv[1]).resolve()
    sys.path.insert(0, str(script.parent))
    import blif_to_verilog  # type: ignore[import-not-found]  # VTR's, from the script's directory

    original = getattr(blif_to_verilog, "pack_bit_blasted_top_ports", None)
    if original is None:
        print("qor_fcheck_run: VTR's blif_to_verilog has no pack_bit_blasted_top_ports (renamed "
              "or removed upstream); update tools/vtr-qor/qor_fcheck_run.py to the new code",
              file=sys.stderr)
        return 2

    def patched(text: str) -> str:
        return str(original(drop_port_wires(text)))

    blif_to_verilog.pack_bit_blasted_top_ports = patched
    if (script.parent / "tb_generator.py").is_file() and not _install_init_runs():
        return 2
    reasons = _precheck_args(argv[2:], blif_to_verilog)
    if reasons:
        sys.stdout.write(render_precheck(reasons))
        sys.stdout.flush()
        return 3
    sys.argv = [str(script), *argv[2:]]
    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        code = exc.code
        if isinstance(code, str):
            print(code, file=sys.stderr)  # as Python does for sys.exit("message")
        return code if isinstance(code, int) else (0 if code is None else 1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
