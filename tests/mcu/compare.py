"""compare.py REF OUT: line-by-line compare of two port traces (iverilog reference, Odin III).

A reference field with an x or z digit (iverilog starts at x, Odin III at 0) matches any value of
the other side. Prints the number of lines, the number of masked fields and the first mismatch.
Exit status: 0 identical, 1 mismatch, 2 usage or read error.
"""
import sys
from pathlib import Path

USAGE_ARGS = 3


def field_matches(ref: str, out: str) -> bool:
    """True when out equals ref, a digit of ref that is x or z matching any digit of out."""
    if len(ref) != len(out):
        return False
    return all(r == o or r in "xXzZ" for r, o in zip(ref, out, strict=True))


def compare(ref_lines: list[str], out_lines: list[str]) -> tuple[int, int, str | None]:
    """Returns (lines compared, masked fields, report of the first mismatch or None)."""
    masked = 0
    for n, (ref, out) in enumerate(zip(ref_lines, out_lines, strict=False), 1):
        ref_fields, out_fields = ref.split(), out.split()
        if len(ref_fields) != len(out_fields):
            return n, masked, f"line {n}: field counts differ\n  ref: {ref}\n  out: {out}"
        for i, (r, o) in enumerate(zip(ref_fields, out_fields, strict=True)):
            if r != o and not field_matches(r, o):
                return n, masked, f"line {n} field {i}: ref {r} vs out {o}\n  ref: {ref}\n  out: {out}"
            masked += 1 if r != o else 0
    if len(ref_lines) != len(out_lines):
        return len(ref_lines), masked, f"lengths differ: {len(ref_lines)} vs {len(out_lines)} lines"
    return len(ref_lines), masked, None


def trace(path: str) -> list[str]:
    """The data lines of a trace: those that start with a cycle number."""
    return [ln for ln in Path(path).read_text().splitlines() if ln[:1].isdigit()]


def main(argv: list[str]) -> int:
    if len(argv) != USAGE_ARGS:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        lines, masked, report = compare(trace(argv[1]), trace(argv[2]))
    except OSError as err:
        print(f"compare.py: {err}", file=sys.stderr)
        return 2
    print(f"{lines} lines compared, {masked} x-masked fields")
    if report is not None:
        print(f"first mismatch: {report}")
        return 1
    print("identical")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
