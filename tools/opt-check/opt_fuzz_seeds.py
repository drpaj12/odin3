#!/usr/bin/env python3
"""opt_fuzz_seeds.py: the libFuzzer seed corpus of tests/fuzz/fuzz_opt.c (2F spec 20.4, Task 18).

Usage::

    opt_fuzz_seeds.py OUT_DIR [--generated N] [--regress DIR] BLIF_DIR...

OUT_DIR is created if missing; its files are written by name (an earlier seed of the same name is
overwritten; nothing is deleted). Each file starts with fuzz_opt's mode byte (bits 0-2 the ``opt``
spelling, bit 3 the payload: 0 BLIF, 1 builder records):

- one builder seed per cell type (``T_*`` of fuzz_opt.c, counted from the file itself so the two
  cannot drift): a 4-bit design with that cell over two inputs, and a second with an enable or
  reset nest around it, the spelling cycling over the eight;
- ``--generated N`` (default 24) random builder seeds: seed ``k`` is a function of ``k`` alone;
- every ``*.blif`` of the BLIF_DIRs, smallest first, each under the default spelling;
- ``--regress DIR``: the files of DIR as they are (each already starts with its mode byte): the
  inputs of earlier findings, named ``regress-<file name>``.

Exit 0 written, 2 usage or I/O error.
"""

from __future__ import annotations

import argparse
import random
import re
import sys
from collections.abc import Sequence
from pathlib import Path

FUZZER = Path(__file__).resolve().parents[2] / "tests" / "fuzz" / "fuzz_opt.c"
RECORD_BYTES = 12
SPELLINGS = 8
MODE_BUILDER = 0x08
MAX_CELLS = 24
OPERAND_BYTES = 10
DEFAULT_GENERATED = 24
EXIT_OK, EXIT_USAGE = 0, 2


def cell_types(source: str) -> int:
    """The number of ``T_*`` cell types of fuzz_opt.c: the enumerators before ``N_TYPES``."""
    body = source.split("typedef enum cell_type {", 1)[1].split("N_TYPES", 1)[0]
    return len(re.findall(r"\bT_[A-Z_]+,", body))


def record(kind: int, width: int, operands: Sequence[int], output: bool = False) -> bytes:
    flags = (width - 1) | (0x80 if output else 0)
    return bytes([kind, flags, *operands, *([0] * (OPERAND_BYTES - len(operands)))])


def design(inputs: Sequence[int], records: Sequence[bytes]) -> bytes:
    """A builder payload: the input count (n - 2 mod 4), the input widths, then the records."""
    head = bytes([len(inputs) - 2, *[w - 1 for w in inputs]])
    return head + b"".join(records[:MAX_CELLS])


def type_seeds(count: int) -> dict[str, bytes]:
    """One design per cell type, over inputs i0 (4 bits), i1 (4 bits), i2 (1 bit)."""
    seeds: dict[str, bytes] = {}
    for kind in range(count):
        # operands: pick bytes 0x10.. choose input signals (pick >> 3 indexes the visible list)
        plain = record(kind, 4, [0x12, 0x1A, 0x22, 0x12, 0x1A, 0x22], output=True)
        seeds[f"cell{kind:02d}"] = bytes([MODE_BUILDER | kind % SPELLINGS]) + design(
            [4, 4, 1], [plain])
        # the same cell behind a mux (an enable nest), its output also an output
        mux = record(14, 4, [0x1A, 0x20, 0x12, 0x2A, 0x32], output=True)
        nest = record(kind, 4, [0x1A, 0x22, 0x12, 0x1A, 0x22])
        seeds[f"nest{kind:02d}"] = bytes([MODE_BUILDER | (kind + 1) % SPELLINGS]) + design(
            [4, 4, 1], [nest, mux])
    return seeds


def random_seed(k: int, count: int) -> bytes:
    rng = random.Random(k)
    inputs = [rng.randint(1, 8) for _ in range(rng.randint(2, 5))]
    records = [record(rng.randrange(count), rng.randint(1, 8),
                      [rng.randrange(256) for _ in range(OPERAND_BYTES)], rng.random() < 0.3)
               for _ in range(rng.randint(3, 14))]
    return bytes([MODE_BUILDER | rng.randrange(SPELLINGS)]) + design(inputs, records)


def blif_seeds(dirs: Sequence[Path]) -> dict[str, bytes]:
    files = sorted((p for d in dirs for p in d.glob("*.blif")),
                   key=lambda p: (p.stat().st_size, p.name))
    return {f"blif{i:03d}": b"\x00" + p.read_bytes() for i, p in enumerate(files)}


def write_all(out: Path, seeds: dict[str, bytes]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (out / name).write_bytes(payload)


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--generated", type=int, default=DEFAULT_GENERATED)
    ap.add_argument("--regress", type=Path, help="a directory of earlier findings, copied as is")
    ap.add_argument("blif_dirs", nargs="*", type=Path)
    args = ap.parse_intermixed_args(argv)
    try:
        count = cell_types(FUZZER.read_text(encoding="utf-8"))
        seeds = type_seeds(count)
        seeds.update({f"gen{k:03d}": random_seed(k, count) for k in range(1, args.generated + 1)})
        seeds.update(blif_seeds(args.blif_dirs))
        if args.regress is not None:
            seeds.update({f"regress-{p.name}": p.read_bytes()
                          for p in sorted(args.regress.iterdir()) if p.is_file()})
        write_all(args.out, seeds)
    except (OSError, IndexError) as exc:
        print(f"opt_fuzz_seeds: {exc}", file=sys.stderr)
        return EXIT_USAGE
    print(f"opt_fuzz_seeds: {len(seeds)} seeds in {args.out}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
