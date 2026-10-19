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
while a four-state simulator calls it ``x``. The netlist's latches start at the BLIF value, 0.
The compiled testbench is therefore run first with all uninitialised RTL state 0 (the BLIF's
state: same start, same behaviour expected) and compared on every output bit, as before; no
mismatch is a ``PASS``. When there are mismatches it is run five more times, with all uninitialised
state 1 and with four random states (``--x-initial unique`` plus ``+verilator+rand+reset+0|1|2``,
a different ``+verilator+seed`` for each random one; the stimulus has a generator of its own, so
it is the same in every run). A mismatch on a bit whose RTL value differs between those runs at
that vector is on an ``x`` bit:
it is reported as ``XDEPEND``, never as a pass, and a design with only such mismatches is
``UNTESTED`` (exit 3, the caller's ``error``: the netlist cannot be judged on them). Any
mismatch on a bit whose RTL value is the same in all the runs is a ``MISMATCH``: ``FAIL``,
whatever else is ``x``. The testbench's own verdict is dropped (it has no initial-state
classification); the verdict lines are written from the dumps, and a run that compares no output
bit is not a pass.
"""

from __future__ import annotations

import re
import runpy
import subprocess
import sys
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


def compare_runs(outputs: Sequence[tuple[str, int]],
                 runs: Sequence[Iterable[str]]) -> InitVerdict:
    """Compare ``runs[0]`` (all uninitialised state 0) vector by vector, RTL against netlist, on
    every output bit. With more runs (other initial states), a mismatching bit whose RTL value
    differs from ``runs[0]``'s in any of them at that vector is ``x`` (XDEPEND); every other
    mismatch is real. A vector is ``mismatched`` when it has a real mismatch, ``x_dependent``
    when it has only x ones, ``matched`` otherwise. Raises ``ValueError`` when the runs dumped a
    different number of vectors."""
    widths = [w for _, w in outputs]
    verdict = InitVerdict()
    for index, rows in enumerate(zip(*(_dump_rows(run, widths) for run in runs), strict=True)):
        real_ports = x_ports = 0
        for i, (name, width) in enumerate(outputs):
            rtl, syn = rows[0][i]
            bad = rtl ^ syn
            if not bad:
                continue
            unknown = 0
            for row in rows[1:]:
                unknown |= rtl ^ row[i][0]
            line = f"vec={index} port={name} rtl={_bits(rtl, width, unknown)} " \
                   f"synth={_bits(syn, width, 0)}"
            if bad & ~unknown:
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
    try:
        for mode, offset in _INIT_RUNS:
            out, code = _run_init(argv, mode, offset, real_call)
            if code != 0:
                stdout.write(out.read_text(encoding="utf-8", errors="replace"))
                return code
            dumps.append(out)
            if len(dumps) in (1, len(_INIT_RUNS)):
                verdict = compare_runs(outputs, [_Lines(path) for path in dumps])
                if len(dumps) == 1 and verdict.vectors == verdict.matched:
                    break  # same start as the netlist and no mismatch: nothing to classify
    except ValueError as exc:  # a dump that does not fit the ports, or runs of unequal length
        stdout.write(f"ERROR: the initial-state dumps cannot be compared: {exc}\n")
        return 3
    with open(dumps[0], encoding="utf-8", errors="replace") as handle:
        header = [ln for ln in handle if not ln.startswith(("VD ", "RAW_"))]
    for path in dumps:  # one line per vector: tens of MB on a big design; the log has the verdict
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
