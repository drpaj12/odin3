"""yosys-phase: Yosys 0.55's proc sub-passes on an RTLIL file, up to a phase (2E plan point (a)).

Usage::

    yosys-phase --yosys Y (--upto P | --passes "p1; p2") [--allow-version V] F.il

It runs ``Y -q -p "read_rtlil F; <passes>; write_rtlil -"`` and passes Yosys's RTLIL through on
stdout. Every proc unit test records the command that re-derives its expectation (2E spec
§18.1), for example ``yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto rmdead t.il``.

``--upto P`` runs ``proc -norom``'s sub-passes in Yosys's order up to phase P (no ``proc_rom``,
since Parmys runs ``-norom``):

- ``clean``: ``proc_clean``;
- ``rmdead`` adds ``proc_rmdead``;
- ``prune`` adds ``proc_prune``;
- ``arst`` adds ``proc_init; proc_arst``;
- ``mux`` adds ``proc_mux``;
- ``dlatch`` adds ``proc_dlatch``;
- ``dff`` adds ``proc_dff; proc_memwr; proc_clean``.

``--passes`` runs exactly the passes given. A Yosys whose ``-V`` is not 0.55 is refused (exit 2),
as 2D's tools do, unless ``--allow-version V`` names its version (the nightly drift job only).

Exit 0 ok, 1 Yosys failed (its output is shown), 2 usage or a refused version.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence

TOOL = "yosys-phase"

# The phases of proc -norom in Yosys 0.55's order, each with the sub-passes it adds.
PHASES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("clean", ("proc_clean",)),
    ("rmdead", ("proc_rmdead",)),
    ("prune", ("proc_prune",)),
    ("arst", ("proc_init", "proc_arst")),
    ("mux", ("proc_mux",)),
    ("dlatch", ("proc_dlatch",)),
    ("dff", ("proc_dff", "proc_memwr", "proc_clean")),
)

_ORACLE_RE = re.compile(r"\bYosys 0\.55(?:[^0-9]|$)")


def passes_upto(phase: str) -> list[str]:
    """The sub-passes from proc_clean through phase, in order."""
    passes: list[str] = []
    for name, added in PHASES:
        passes.extend(added)
        if name == phase:
            return passes
    raise ValueError(f"unknown phase {phase!r}")


def split_passes(text: str) -> list[str]:
    """The passes of a "p1; p2" string, without empty entries."""
    return [part.strip() for part in text.split(";") if part.strip()]


def script(rtlil: str, passes: Sequence[str]) -> str:
    """The Yosys script: read the file, run the passes, write RTLIL to stdout."""
    return "; ".join([f"read_rtlil {rtlil}", *passes, "write_rtlil -"])


def version_ok(version: str, allow: str | None) -> bool:
    """True for Yosys 0.55, or for a version that names allow."""
    if _ORACLE_RE.search(version):
        return True
    return allow is not None and allow in version


def _parse(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    parser.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    which = parser.add_mutually_exclusive_group(required=True)
    which.add_argument("--upto", choices=[name for name, _ in PHASES], help="the last phase")
    which.add_argument("--passes", help='exactly these passes, "p1; p2"')
    parser.add_argument("--allow-version", help="accept a Yosys whose -V names this version")
    parser.add_argument("rtlil", help="the RTLIL file")
    return parser.parse_args(list(argv))


def main(argv: Sequence[str]) -> int:
    args = _parse(argv)
    try:
        probe = subprocess.run([args.yosys, "-V"], capture_output=True, text=True, check=False)
    except OSError as exc:
        print(f"{TOOL}: cannot run {args.yosys}: {exc}", file=sys.stderr)
        return 2
    version = probe.stdout.strip()
    if probe.returncode != 0 or not version_ok(version, args.allow_version):
        print(f"{TOOL}: {args.yosys} is '{version}', not Yosys 0.55 "
              "(--allow-version accepts another)", file=sys.stderr)
        return 2
    passes = passes_upto(args.upto) if args.upto else split_passes(args.passes)
    if not passes:
        print(f'{TOOL}: --passes names no pass', file=sys.stderr)
        return 2
    run = subprocess.run([args.yosys, "-q", "-p", script(args.rtlil, passes)],
                         capture_output=True, text=True, check=False)
    sys.stdout.write(run.stdout)
    if run.returncode != 0:
        sys.stderr.write(run.stderr)
        print(f"{TOOL}: Yosys exited {run.returncode}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
