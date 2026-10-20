"""fsm-equiv: is the recoded design equivalent to the RTL from reset? (4.8 spec section 15, T-2)

Usage::

    fsm-equiv GOLD [--gate FILE] --top T [--rst NAME[:LEVEL]] [--depth N]
              [--gold-script S] [--gate-script S] [--yosys PATH] [--log FILE]
    fsm-equiv --odin ODIN3 FILE --top T [--rst NAME[:LEVEL]] [--depth N] [--yosys PATH]
    fsm-equiv --selftest tests/fsm/micros.txt [--only NAME...] [--odin ODIN3]

GOLD and GATE are Verilog (``.v``) or RTLIL (``.il``) files; GATE defaults to GOLD.  Each side is
read, run through its script (default gold ``proc``, gate ``proc; fsm; opt``) and flattened into
one design.  The tool then proves a bounded miter in Yosys 0.55::

    miter -equiv -flatten -make_assert gold gate m; async2sync
    sat -verify -seq N -set-init-zero -set-at 1 RST V -set-at 2 RST V -prove-asserts m

The reset constraint is load-bearing: a recoded one-hot register powers up all-zero (an illegal
code), so the comparison holds from reset only.  ``--rst`` names the reset input and its active
LEVEL (default 1); without it the miter has no reset constraint.  ``--selftest`` runs Yosys's own
``fsm`` on every row of micros.txt whose ``equiv`` column is ``yes`` and checks the tool says
equivalent; this proves each micro is a sound test. With ``--odin ODIN3`` the gate is Odin III's:
``read_verilog --nolatches FILE; proc`` is the gold, ``fsm; opt`` after it the gate (both written
with ``write_rtlil``), and the same miter proves them equivalent; ``--selftest --odin`` does so for
every ``yes`` row (Task 10, test fsm_equiv_micro).

Exit codes: 0 equivalent; 1 not equivalent; 2 usage or tool error; 3 hard (crash or timeout).
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import fsm_probe

EXIT_EQUIVALENT = 0
EXIT_DIFFERENT = 1
EXIT_ERROR = 2
EXIT_HARD = 3

DEFAULT_DEPTH = 12
DEFAULT_GOLD_SCRIPT = "proc"
DEFAULT_GATE_SCRIPT = "proc; fsm; opt"
TIMEOUT_SECONDS = 600
PROVED = re.compile(r"SAT proof finished - no model found: SUCCESS")
FAILED = re.compile(r"SAT proof finished - model found: FAIL")


class EquivError(Exception):
    """A usage or input error (exit 2)."""


@dataclass
class MicroRow:
    """One row of ``tests/fsm/micros.txt``."""

    name: str
    path: str
    top: str
    reset: str
    clock: str
    depth: int
    equiv: bool


def parse_rst(spec: str | None) -> tuple[str, int] | None:
    """``NAME[:LEVEL]`` to (name, level); ``None`` and ``-`` mean no reset."""
    if spec is None or spec == "-":
        return None
    name, _, level = spec.partition(":")
    if not name or level not in ("", "0", "1"):
        raise EquivError(f"bad reset spec {spec!r}: want NAME[:0|:1]")
    return name, int(level or "1")


def read_cmd(path: str) -> str:
    """The read command; Verilog also gets VTR's RAM models, as the probe's script does."""
    if path.endswith(".il"):
        return f"read_rtlil {path}"
    if path.endswith(".v"):
        return ("read_verilog -nomem2reg +/parmys/vtr_primitives.v; "
                f"read_verilog -sv -nolatches {path}")
    raise EquivError(f"{path}: want a .v or .il file")


def side_script(path: str, top: str, script: str, stash: str) -> list[str]:
    return [read_cmd(path), f"hierarchy -check -top {top} -purge_lib",
            script, "flatten", "opt_clean",
            f"design -stash {stash}"]


def miter_script(gold: str, gate: str, top: str, rst: tuple[str, int] | None, depth: int,
                 scripts: tuple[str, str] = (DEFAULT_GOLD_SCRIPT, DEFAULT_GATE_SCRIPT)) -> str:
    """The whole Yosys script for one check."""
    steps = side_script(gold, top, scripts[0], "gold") + side_script(gate, top, scripts[1], "gate")
    steps += [f"design -copy-from gold -as gold {top}", f"design -copy-from gate -as gate {top}",
              "miter -equiv -flatten -make_assert gold gate m", "async2sync"]
    sat = f"sat -verify -seq {depth} -set-init-zero"
    if rst is not None:
        sat += "".join(f" -set-at {t} in_{rst[0]} {rst[1]}" for t in (1, 2))
    steps.append(sat + " -prove-asserts m")
    return "; ".join(steps)


def run_check(yosys: str, script: str, log: Path) -> int:
    """Run the script; return an exit code of this tool."""
    if shutil.which(yosys) is None and not Path(yosys).is_file():
        raise EquivError(f"yosys not found at {yosys}")
    prefix = ["prlimit", f"--as={fsm_probe.ADDRESS_LIMIT_KIB * 1024}"] if shutil.which(
        "prlimit") else []
    try:
        proc = subprocess.run(prefix + [yosys, "-q", "-l", str(log), "-p", script],
                              capture_output=True, text=True, timeout=TIMEOUT_SECONDS,
                              check=False)
    except subprocess.TimeoutExpired:
        return EXIT_HARD
    text = log.read_text(errors="replace") if log.exists() else ""
    if proc.returncode == 0 and PROVED.search(text):
        return EXIT_EQUIVALENT
    if FAILED.search(text):
        return EXIT_DIFFERENT
    return EXIT_HARD if proc.returncode < 0 else EXIT_ERROR


def check(yosys: str, gold: str, gate: str, top: str, rst: str | None, depth: int,
          scripts: tuple[str, str], log: Path | None = None) -> int:
    script = miter_script(gold, gate, top, parse_rst(rst), depth, scripts)
    with tempfile.TemporaryDirectory(prefix="fsm-equiv-") as tmp:
        return run_check(yosys, script, log or Path(tmp) / "yosys.log")


def read_rows(path: Path) -> list[MicroRow]:
    rows: list[MicroRow] = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        if len(line) != 7 or line[6] not in ("yes", "no"):
            raise EquivError(
                f"{path}: bad row {raw!r}: want NAME PATH TOP RESET CLOCK DEPTH yes|no")
        rows.append(MicroRow(line[0], line[1], line[2], line[3], line[4], int(line[5]),
                             line[6] == "yes"))
    return rows


def odin_script(source: str, gold: Path, gate: Path) -> str:
    """The Odin III script that writes the RTL after proc (gold) and after fsm; opt (gate)."""
    return (f"read_verilog --nolatches {source}; proc; write_rtlil {gold}; fsm; opt; "
            f"write_rtlil {gate}")


def odin_check(odin: str, yosys: str, source: str, spec: Sequence[str]) -> int:
    """Odin III's fsm; opt against its proc, in Yosys. spec: top, reset, depth (as text)."""
    top, rst, depth = spec
    with tempfile.TemporaryDirectory(prefix="fsm-equiv-") as tmp:
        work = Path(tmp)
        gold, gate = work / "gold.il", work / "gate.il"
        proc = subprocess.run([odin, "-p", odin_script(source, gold, gate)], capture_output=True,
                              text=True, timeout=TIMEOUT_SECONDS, check=False)
        if proc.returncode != 0:
            print(f"fsm-equiv: odin3 failed: {(proc.stdout + proc.stderr)[-300:]}", file=sys.stderr)
            return EXIT_ERROR
        return check(yosys, str(gold), str(gate), top, rst, int(depth), ("opt_clean", "opt_clean"))


def selftest(yosys: str, micros: Path, only: Sequence[str], root: Path,
             odin: str | None = None) -> int:
    worst = EXIT_EQUIVALENT
    for row in read_rows(micros):
        if not row.equiv or (only and row.name not in only):
            continue
        source = str(root / row.path)
        if odin is not None:
            code = odin_check(odin, yosys, source, (row.top, row.reset, str(row.depth)))
        else:
            code = check(yosys, source, source, row.top, row.reset, row.depth,
                         (DEFAULT_GOLD_SCRIPT, DEFAULT_GATE_SCRIPT))
        print(f"{row.name}: {'equivalent' if code == EXIT_EQUIVALENT else f'exit {code}'}")
        worst = max(worst, code)
    return worst


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fsm-equiv", description=__doc__.split("\n")[0])
    ap.add_argument("gold", nargs="?")
    ap.add_argument("--gate")
    ap.add_argument("--top")
    ap.add_argument("--rst")
    ap.add_argument("--depth", type=int, default=DEFAULT_DEPTH)
    ap.add_argument("--gold-script", default=DEFAULT_GOLD_SCRIPT)
    ap.add_argument("--gate-script", default=DEFAULT_GATE_SCRIPT)
    ap.add_argument("--yosys", default=fsm_probe.default_yosys())
    ap.add_argument("--log", type=Path)
    ap.add_argument("--odin", help="Odin III executable: the gate is its fsm; opt")
    ap.add_argument("--selftest", type=Path, metavar="MICROS_TXT")
    ap.add_argument("--only", nargs="*", default=[])
    args = ap.parse_args(argv)
    try:
        if args.selftest is not None:
            return selftest(args.yosys, args.selftest, args.only, args.selftest.parents[1].parent,
                            args.odin)
        if args.gold is None or args.top is None:
            raise EquivError("need GOLD and --top (or --selftest)")
        if args.odin is not None:
            return odin_check(args.odin, args.yosys, args.gold,
                              (args.top, args.rst or "-", str(args.depth)))
        return check(args.yosys, args.gold, args.gate or args.gold, args.top, args.rst,
                     args.depth, (args.gold_script, args.gate_script), args.log)
    except EquivError as exc:
        print(f"fsm-equiv: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
