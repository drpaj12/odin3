"""tb-compare: run one Verilog testbench on two designs and report the first mismatch.

Each side is an odin3 script that builds one design (read_verilog/read_blif ...; proc; opt; lower).
The same testbench is run on both with `sim_tb --out`, and the two outputs are compared line by
line. A line of a testbench made by `testbench_gen` starts with the cycle number; the report names
that cycle and the fields (outputs, in port order) that differ.
Exit status: 0 identical, 1 mismatch, 2 error.
"""
import argparse
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path


def run_side(odin3: str, script: str, tb: str, out: Path, extra: Sequence[str]) -> str | None:
    """Runs one side; returns an error text, or None on success."""
    # The argv is a list; inside odin3's -p script the paths are double-quoted, which keeps blanks.
    cmd = [odin3, "-p", f'{script}; sim_tb --out "{out}" {" ".join(extra)} "{tb}"']
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        errors = [ln for ln in proc.stderr.splitlines() if "error" in ln]
        return "\n".join(errors[:3]) or proc.stderr[-300:]
    return None


def first_mismatch(a_lines: list[str], b_lines: list[str]) -> str | None:
    """A report for the first differing line, or None when identical."""
    for n, (a, b) in enumerate(zip(a_lines, b_lines, strict=False), 1):
        if a == b:
            continue
        fa, fb = a.split(), b.split()
        head = fa[0] if fa else "?"
        pairs = enumerate(zip(fa, fb, strict=False))
        diff = [f"field {i}: {x} vs {y}" for i, (x, y) in pairs if x != y]
        what = "; ".join(diff) or "different field counts"
        return f"line {n} (time/cycle {head}): {what}\n  A: {a}\n  B: {b}"
    if len(a_lines) != len(b_lines):
        return f"outputs have different lengths: {len(a_lines)} vs {len(b_lines)} lines"
    return None


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--odin3", default="odin3", help="odin3 executable")
    ap.add_argument("--tb", required=True, help="testbench file")
    ap.add_argument("--a", required=True, help="odin3 script building design A")
    ap.add_argument("--b", required=True, help="odin3 script building design B")
    ap.add_argument("--max-time", default=None, help="sim_tb --max-time")
    args = ap.parse_args(argv)
    extra = ["--max-time", args.max_time] if args.max_time else []
    with tempfile.TemporaryDirectory() as tmp:
        outs = [Path(tmp) / "a.out", Path(tmp) / "b.out"]
        for name, script, out in (("A", args.a, outs[0]), ("B", args.b, outs[1])):
            err = run_side(args.odin3, script, args.tb, out, extra)
            if err is not None:
                print(f"tb-compare: design {name} failed: {err}", file=sys.stderr)
                return 2
        a_lines = outs[0].read_text().splitlines()
        b_lines = outs[1].read_text().splitlines()
        if not a_lines and not b_lines:
            print(
                "tb-compare: both runs printed nothing (an empty or non-printing testbench, or "
                "--max-time reached first): nothing was compared",
                file=sys.stderr,
            )
            return 2
        report = first_mismatch(a_lines, b_lines)
    if report is None:
        print("tb-compare: identical")
        return 0
    print("tb-compare: MISMATCH " + report)
    return 1


if __name__ == "__main__":
    sys.exit(main())
