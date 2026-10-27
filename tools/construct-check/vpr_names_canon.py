#!/usr/bin/env python3
"""vpr-names-canon: are VPR's outputs identical when only the BLIF names change? (V1.2.0 6.1)

Usage::

    vpr_names_canon.py BASE DESIGN

``BASE/short`` and ``BASE/hash`` each hold ``<DESIGN>.blif`` (the same script written with short
names and with ``--name-style hash``) and ``vpr/run`` with VPR's ``<DESIGN>.net``, ``.place``,
``.route`` and ``vpr.out`` (``odin3-flow``'s ``vpr_run`` with ``-delete_result_files`` removed).

The BLIFs are tokenised (``#`` comment lines dropped, ``\\`` continuations joined, split on blanks
and ``=``) and paired in order, which gives a hash-name to short-name map; the two token lists
must be the same length. The map is applied to the hash-run files; ``SHA256:<hex>`` is masked and
runs of blanks become one blank. The report gives, per file type, ``identical`` or ``DIFFER``
(with the first differing line), the final critical path delay and total wirelength of both runs
and the BLIF sizes. Exit code 0 when all three files are identical, 1 when not, 2 on bad input.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

EXTENSIONS = ("net", "place", "route")


def tokens(path: Path) -> list[str]:
    text = "\n".join(ln for ln in path.read_text().splitlines() if not ln.startswith("#"))
    return re.split(r"[ \t\n=]+", re.sub(r"\\\n", " ", text).strip())


def rename_map(base: Path, design: str) -> dict[str, str]:
    short = tokens(base / "short" / f"{design}.blif")
    hashed = tokens(base / "hash" / f"{design}.blif")
    if len(short) != len(hashed):
        raise ValueError(f"the BLIFs differ in token count: {len(short)} and {len(hashed)}")
    return {h: s for s, h in zip(short, hashed, strict=True) if s != h}


def canonical(path: Path, names: dict[str, str] | None) -> str:
    text = path.read_text(errors="replace")
    if names:
        key = "|".join(re.escape(h) for h in sorted(names, key=len, reverse=True))
        text = re.sub(r"(?<![\w$\[\]:.])(" + key + r")(?![\w$])", lambda g: names[g.group(1)],
                      text)
    return re.sub(r"[ \t]+", " ", re.sub(r"SHA256:[0-9a-f]+", "SHA256:X", text))


def last_match(path: Path, pattern: str) -> str:
    found = re.findall(pattern, path.read_text(errors="replace"))
    return found[-1] if found else "?"


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    base, design = Path(argv[0]), argv[1]
    try:
        names = rename_map(base, design)
        verdict: dict[str, bool] = {}
        for ext in EXTENSIONS:
            run = Path("vpr") / "run" / f"{design}.{ext}"
            left = canonical(base / "short" / run, None)
            right = canonical(base / "hash" / run, names)
            verdict[ext] = left == right
            if left != right:
                pairs = zip(left.splitlines(), right.splitlines(), strict=False)
                for i, (x, y) in enumerate(pairs):
                    if x != y:
                        print(f"{ext}: first difference at line {i + 1}: {x[:120]!r} {y[:120]!r}")
                        break
        out = [base / s / "vpr" / "run" / "vpr.out" for s in ("short", "hash")]
        delay = r"Final critical path delay \(least slack\): ([\d.]+) ns"
        cpd = [last_match(p, delay) for p in out]
        wire = [last_match(p, r"Total wirelength: (\d+)") for p in out]
        sizes = [(base / s / f"{design}.blif").stat().st_size for s in ("short", "hash")]
    except (OSError, ValueError) as exc:
        print(f"vpr-names-canon: {exc}", file=sys.stderr)
        return 2
    print(design, "renamed", len(names), {k: "identical" if v else "DIFFER" for k, v in
                                          verdict.items()}, "cpd", cpd, "wl", wire,
          "blif bytes", sizes)
    return 0 if all(verdict.values()) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
