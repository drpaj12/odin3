"""qor_fcheck_run: ``run_random_check.py`` with one upstream bug worked around (3H spec §8.1).

Run as ``python -I -B qor_fcheck_run.py <VTR run_random_check.py> <its arguments>``. VTR's checker
turns a pre-VPR BLIF into Verilog with Yosys and ``blif_to_verilog.pack_bit_blasted_top_ports``,
which packs the bit-blasted top ports (``\\a~3``) into vectors. For a top-level output that Yosys
also declares as a ``wire`` (any output driven by an ``assign``, e.g. a constant bit) the packing
leaves ``wire a[3] ;`` next to ``output [31:0] a;``, and Verilator stops with "Duplicate
declaration". This wrapper drops the ``wire`` line of a name that is also a port before the
packing runs, then hands over to the unchanged script (``runpy``, so its exit code and output are
its own). VTR's tree is only read (``-B``: no bytecode is written there).
"""

from __future__ import annotations

import re
import runpy
import sys
from pathlib import Path

_PORT = re.compile(r"^\s*(?:input|output|inout)(?:\s+wire|\s+reg)?\s+(\\\S+)\s*;", re.M)
_WIRE = re.compile(r"^[ \t]*wire\s+(\\\S+)\s*;[ \t]*\n", re.M)


def drop_port_wires(verilog: str) -> str:
    """Remove ``wire \\name ;`` for every ``name`` that is also declared ``input``, ``output`` or
    ``inout`` (an escaped identifier, as Yosys writes the BLIF's ``a~3``)."""
    ports = set(_PORT.findall(verilog))
    return _WIRE.sub(lambda m: "" if m.group(1) in ports else m.group(0), verilog)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: qor_fcheck_run.py <run_random_check.py> [args...]", file=sys.stderr)
        return 2
    script = Path(argv[1]).resolve()
    sys.path.insert(0, str(script.parent))
    import blif_to_verilog  # type: ignore[import-not-found]  # VTR's, from the script's directory

    original = blif_to_verilog.pack_bit_blasted_top_ports

    def patched(text: str) -> str:
        return str(original(drop_port_wires(text)))

    blif_to_verilog.pack_bit_blasted_top_ports = patched
    sys.argv = [str(script), *argv[2:]]
    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        code = exc.code
        return code if isinstance(code, int) else (0 if code is None else 1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
