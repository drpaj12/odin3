"""ffbits-check: the flip-flop bit counts of ``opt -ffbits`` against Yosys and a file.

Usage::

    ffbits-check --odin3 O [--yosys Y] [--expect tests/opt/ffbits/expect.txt] [--dir D]

``expect.txt`` has one row per micro, ``<name> <plain> <merged>``: the flip-flop bits Odin III
leaves after ``read_verilog --nolatches X.v; proc; opt; opt -full`` without and with
``opt -full -ffbits`` (a word cell counts its WIDTH). With ``--yosys`` (0.55) the ``merged`` count
must also be what Yosys leaves after ``proc; opt -full; techmap; opt -fast``, the bit cells it
merges (PHASE3 #48): the parity this pass exists for. Exit codes: 0 all rows pass, 1 a row fails,
2 error or usage.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FF_CELL = re.compile(
    r"^\s*cell \$(?:dff|dffe|adff|adffe|aldff|aldffe|sdff|sdffe|sdffce|dffsr|dffsre)\b"
)
WIDTH = re.compile(r"^\s*parameter \\WIDTH (\d+)")
YOSYS_FF = re.compile(r"^\s+\$_\w*DFF\w*_\s+(\d+)\s*$", re.M)
TIMEOUT = 120


def rtlil_ff_bits(text: str) -> int:
    """Sum of the WIDTH of every flip-flop cell of an RTLIL text."""
    total, in_ff = 0, False
    for line in text.splitlines():
        if FF_CELL.match(line):
            in_ff = True
        elif in_ff and (m := WIDTH.match(line)):
            total += int(m.group(1))
            in_ff = False
    return total


def odin_bits(odin3: str, source: Path, ffbits: bool) -> int:
    """FF bits after the script, with or without a further ``opt -full -ffbits``."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "out.il"
        steps = ["read_verilog --nolatches " + str(source), "proc", "opt", "opt -full"]
        steps += ["opt -full -ffbits"] if ffbits else []
        steps += [f"write_rtlil {out}"]
        done = subprocess.run([odin3, "-p", "; ".join(steps)], capture_output=True, text=True,
                              timeout=TIMEOUT, check=False)
        if done.returncode != 0:
            raise RuntimeError(f"{source.name}: odin3 failed: {done.stderr.strip()[-300:]}")
        return rtlil_ff_bits(out.read_text())


def yosys_bits(yosys: str, source: Path) -> int:
    """Bit flip-flop cells after techmap and ``opt -fast`` in Yosys 0.55."""
    script = f"read_verilog {source}; proc; opt -full; techmap; opt -fast; stat"
    done = subprocess.run([yosys, "-p", script], capture_output=True, text=True, timeout=TIMEOUT,
                          check=False)
    if done.returncode != 0:
        raise RuntimeError(f"{source.name}: yosys failed: {done.stdout.strip()[-300:]}")
    return sum(int(n) for n in YOSYS_FF.findall(done.stdout))


def read_expect(path: Path) -> dict[str, tuple[int, int]]:
    rows: dict[str, tuple[int, int]] = {}
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].split()
        if line:
            rows[line[0]] = (int(line[1]), int(line[2]))
    return rows


def check_row(args: argparse.Namespace, name: str, want: tuple[int, int]) -> list[str]:
    source = Path(args.dir) / f"{name}.v"
    got = (odin_bits(args.odin3, source, False), odin_bits(args.odin3, source, True))
    errors = [] if got == want else [f"odin3 plain/merged {got}, expected {want}"]
    if args.yosys:
        ybits = yosys_bits(args.yosys, source)
        if ybits != got[1]:
            errors.append(f"yosys leaves {ybits} bits, odin3 {got[1]}")
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--odin3", required=True)
    parser.add_argument("--yosys")
    parser.add_argument("--dir", default=str(ROOT / "tests/opt/ffbits"))
    parser.add_argument("--expect", default=str(ROOT / "tests/opt/ffbits/expect.txt"))
    args = parser.parse_args(argv)
    failed = 0
    try:
        for name, want in sorted(read_expect(Path(args.expect)).items()):
            errors = check_row(args, name, want)
            print(f"{'ok  ' if not errors else 'FAIL'} {name}: {'; '.join(errors)}".rstrip(": "))
            failed += bool(errors)
    except (RuntimeError, OSError, subprocess.TimeoutExpired, ValueError) as exc:
        print(f"ffbits-check: {exc}", file=sys.stderr)
        return 2
    print(f"ffbits-check: {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
