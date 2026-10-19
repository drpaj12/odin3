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
while the netlist's latches start at the BLIF value, so the first vectors compare a defined 0
against what a four-state simulator calls ``x``. The checker's run of the compiled testbench is
therefore done twice, once with all uninitialised RTL state 0 and once with all of it 1
(``--x-initial unique`` plus ``+verilator+rand+reset+0|1``). A bit of an output port whose RTL
value differs between the two runs at any vector depends on that initial state: it is ``x`` (and
so is whatever the design computes from it later), and it is not compared at any vector. Every
other bit is compared in both runs; one mismatch on a bit with an initial-state-independent RTL
value is a mismatch, whatever else is masked. The testbench's own verdict is dropped (it would
compare every bit of run one); the verdict lines (``MISMATCH``, ``VCHECK_SUMMARY``, ``PASS:``)
are written from the two dumps, with the masked bits in ``VCHECK_INIT_MASK``. A run that compares
no bit is not a pass.
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


# --- initial state: the two-run comparison ---------------------------------------------------

_MAX_REPORT = 20  # MISMATCH lines written, as the testbench's own limit
_NEVER = "2147483647"  # the testbench's abort limit, out of reach: every vector is dumped
_TALLY = "            if (vecFail) begin\n                vecMismatched++;"
_INIT_MODES = (0, 1)  # +verilator+rand+reset+N: all uninitialised state 0, then all 1
_RESET_ARG = "+verilator+rand+reset+"


def instrument_testbench(tb: str, outputs: Sequence[tuple[str, int]]) -> str:
    """The generated testbench with a dump of every output port per vector and no verdict of its
    own: one line ``VD <rtl hex>:<synth hex> ...`` (the outputs in port order) before each
    vector's tally, its abort limit out of reach, no mismatch printing, and its summary and
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
    out, limits = re.subn(r"(MAX_ERRORS = )\d+;", rf"\g<1>{_NEVER};", out)
    out = out.replace("if (errors < MAX_ERRORS) begin", "if (1'b0) begin")
    for word in ("VCHECK_SUMMARY", "PASS:", "FAIL:"):
        out = out.replace(f'$display("{word}', f'$display("RAW_{word}')
    out = out.replace("$fatal(1);", "$finish;")
    if limits != 1 or "RAW_VCHECK_SUMMARY" not in out:
        raise RuntimeError("qor_fcheck_run: the generated testbench has an unexpected limit or "
                           "summary; update tools/vtr-qor/qor_fcheck_run.py to the new "
                           "tb_generator")
    return out


@dataclass
class InitVerdict:
    """The comparison of one design over both initial-state runs."""

    matched: int = 0
    mismatched: int = 0
    port_errors: int = 0
    masked_bits: int = 0  # over all vectors
    compared_bits: int = 0  # over all vectors
    masked_ports: list[str] = field(default_factory=list)  # ``name[n bits]``
    lines: list[str] = field(default_factory=list)  # the first MISMATCH lines

    @property
    def vectors(self) -> int:
        return self.matched + self.mismatched

    @property
    def passed(self) -> bool:
        return self.matched > 0 and self.mismatched == 0 and self.compared_bits > 0


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


def _paired_rows(outputs: Sequence[tuple[str, int]], run0: Iterable[str],
                 run1: Iterable[str]) -> Iterator[tuple[list[tuple[int, int]],
                                                        list[tuple[int, int]]]]:
    widths = [w for _, w in outputs]
    return zip(_dump_rows(run0, widths), _dump_rows(run1, widths), strict=True)


def init_dependent_bits(outputs: Sequence[tuple[str, int]], run0: Iterable[str],
                        run1: Iterable[str]) -> list[int]:
    """Per output port, the bits whose RTL value differs between the two runs at any vector.
    Such a bit depends on state the RTL never defined (an unreset register, a latch read before
    it is written): it is ``x``, and so is everything the design computes from it later, so the
    bit is not compared at any vector, whether or not the two runs happen to agree there. Raises
    ``ValueError`` when the two runs dumped a different number of vectors."""
    unknown = [0] * len(outputs)
    for row0, row1 in _paired_rows(outputs, run0, run1):
        for i, ((rtl0, _), (rtl1, _)) in enumerate(zip(row0, row1, strict=True)):
            unknown[i] |= rtl0 ^ rtl1
    return unknown


def compare_runs(outputs: Sequence[tuple[str, int]], unknown: Sequence[int],
                 run0: Iterable[str], run1: Iterable[str]) -> InitVerdict:
    """Compare the two runs vector by vector on the bits that are not ``unknown``: RTL and
    netlist must agree on each of them in both runs. A vector counts as matched when none of its
    compared bits differ, as mismatched when one does; the port errors count the ports with a
    mismatching bit. Raises ``ValueError`` when the runs dumped a different number of vectors."""
    verdict = InitVerdict()
    n_unknown = sum(u.bit_count() for u in unknown)
    width_all = sum(w for _, w in outputs)
    for index, (row0, row1) in enumerate(_paired_rows(outputs, run0, run1)):
        bad_ports = 0
        for (name, width), mask, (rtl0, syn0), (rtl1, syn1) in zip(
                outputs, unknown, row0, row1, strict=True):
            known = ((1 << width) - 1) & ~mask
            if (rtl0 ^ syn0) & known or (rtl1 ^ syn1) & known:
                bad_ports += 1
                if len(verdict.lines) < _MAX_REPORT:
                    rtl, syn = (rtl0, syn0) if (rtl0 ^ syn0) & known else (rtl1, syn1)
                    verdict.lines.append(f"MISMATCH vec={index} port={name} "
                                         f"rtl={_bits(rtl, width, mask)} "
                                         f"synth={_bits(syn, width, 0)}")
        verdict.masked_bits += n_unknown
        verdict.compared_bits += width_all - n_unknown
        verdict.port_errors += bad_ports
        if bad_ports:
            verdict.mismatched += 1
        else:
            verdict.matched += 1
    verdict.masked_ports = [f"{name}[{mask.bit_count()}/{width}]"
                            for (name, width), mask in zip(outputs, unknown, strict=True) if mask]
    return verdict


def render_verdict(verdict: InitVerdict) -> str:
    """The verdict lines the callers (``qor_legs.fcheck_verdict``) read."""
    lines = list(verdict.lines)
    if verdict.port_errors > len(verdict.lines):
        lines.append(f"... {verdict.port_errors - len(verdict.lines)} more port mismatches")
    lines.append(f"VCHECK_INIT_MASK masked_bits={verdict.masked_bits} "
                 f"compared_bits={verdict.compared_bits} "
                 f"masked_ports={','.join(verdict.masked_ports) or '(none)'}")
    lines.append(f"VCHECK_SUMMARY matched={verdict.matched} mismatched={verdict.mismatched} "
                 f"vectors={verdict.vectors} port_errors={verdict.port_errors}")
    if verdict.passed:
        lines.append(f"PASS: {verdict.matched} vectors matched across rtl/post-synth")
    elif verdict.mismatched:
        lines.append(f"FAIL: {verdict.port_errors} mismatches in {verdict.vectors} vectors")
    else:
        lines.append("ERROR: no output bit was compared (every bit depends on the initial state, "
                     "or the design has no output port)")
    return "\n".join(lines) + "\n"


def _lines(path: Path) -> Iterator[str]:
    with open(path, encoding="utf-8", errors="replace") as handle:
        yield from handle


def _run_two_inits(argv: list[str], outputs: Sequence[tuple[str, int]], stdout: Any,
                   real_call: Callable[..., int]) -> int:
    """Run the compiled testbench once per initial state, write the verdict into ``stdout``
    (the checker's ``sim.out``). The binary's own non-zero exit is returned as is, with its
    output: the checker then reports a simulation error."""
    work = Path(argv[0]).resolve().parent.parent
    dumps = []
    header: list[str] = []
    for mode in _INIT_MODES:
        run_argv = [a for a in argv if not a.startswith(_RESET_ARG)] + [f"{_RESET_ARG}{mode}"]
        out = work / f"sim_init{mode}.out"
        with open(out, "w", encoding="utf-8") as handle:
            code = real_call(run_argv, stdout=handle, stderr=subprocess.STDOUT)
        if code != 0:
            stdout.write(out.read_text(encoding="utf-8", errors="replace"))
            return code
        dumps.append(out)
        if mode == _INIT_MODES[0]:
            with open(out, encoding="utf-8", errors="replace") as handle:
                header = [ln for ln in handle if not ln.startswith(("VD ", "RAW_"))]
    try:
        unknown = init_dependent_bits(outputs, _lines(dumps[0]), _lines(dumps[1]))
        verdict = compare_runs(outputs, unknown, _lines(dumps[0]), _lines(dumps[1]))
    except ValueError as exc:  # a dump that does not fit the ports, or runs of unequal length
        stdout.write(f"ERROR: the two initial-state dumps cannot be compared: {exc}\n")
        return 3
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
            return _run_two_inits(argv, outputs, kwargs["stdout"], real_call)
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
