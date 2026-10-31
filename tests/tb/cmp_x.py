#!/usr/bin/env python3
"""cmp_x.py REF OUT: compare an iverilog output (REF, may contain x/z digits) with a 2-state
simulator output (OUT) line by line; a hex digit that is x or z in REF matches anything in OUT.
Prints 'same', 'masked N' (equal apart from N reference digits that are x/z) or 'DIFF line L: ...';
exit 0 unless DIFF."""
import sys
from pathlib import Path


def main() -> int:
    ref = Path(sys.argv[1]).read_text().splitlines()
    out = Path(sys.argv[2]).read_text().splitlines()
    if len(ref) != len(out):
        print(f"DIFF line count {len(ref)} vs {len(out)}")
        return 1
    masked = 0
    for n, (a, b) in enumerate(zip(ref, out, strict=True), 1):
        fa, fb = a.split(), b.split()
        if len(fa) != len(fb):
            print(f"DIFF line {n}: {a!r} vs {b!r}")
            return 1
        for x, y in zip(fa, fb, strict=True):
            if len(x) != len(y):
                print(f"DIFF line {n}: field width {x!r} vs {y!r}")
                return 1
            for cx, cy in zip(x, y, strict=True):
                if cx in "xXzZ":
                    masked += 1
                elif cx != cy:
                    print(f"DIFF line {n}: {a!r} vs {b!r}")
                    return 1
    print("same" if masked == 0 else f"masked {masked}")
    return 0


sys.exit(main())
