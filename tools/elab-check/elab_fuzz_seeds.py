#!/usr/bin/env python3
"""elab_fuzz_seeds.py: the libFuzzer seed corpus of tests/fuzz/fuzz_elab.c (2D spec 14.5).

Usage::

    elab_fuzz_seeds.py OUT_DIR [--generated N] SOURCE...

Each SOURCE is a Verilog file or a directory (its ``*.v``). The sources, smallest first, are
written into OUT_DIR as ``seed_NNNN`` files: the mode byte 0x01 (``--nolatches``, the mode the
oracle comparison uses), then the file's bytes. ``--generated N`` adds the text of
``verilog_gen.py --elab`` seeds 1..N, half after mode byte 0x00 (latches kept) and half after
0x03 (``--nomem2reg`` too). A source over MAX_SEED bytes is skipped (the harness runs with
``-max_len=4096``; an unmutated seed over that is never fed to it from the corpus anyway, but a
huge one only slows the seed run). Earlier ``seed_*`` files in OUT_DIR are overwritten, never
deleted: a smaller set leaves stale high-numbered ones, so use a fresh directory.

Exit 0 written, 2 usage or I/O error.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "verilog-gen"))
import elab_profile  # noqa: E402

MAX_SEED = 1 << 16
MODE_NOLATCHES, MODE_NOMEM2REG = 1, 2


def sources(paths: Sequence[str]) -> list[Path]:
    """The ``*.v`` files of the given files and directories, smallest first."""
    files: list[Path] = []
    for name in paths:
        path = Path(name)
        files.extend(sorted(path.glob("*.v")) if path.is_dir() else [path])
    sized = [p for p in files if p.stat().st_size <= MAX_SEED]
    return sorted(sized, key=lambda p: (p.stat().st_size, str(p)))


def seeds(files: Sequence[Path], generated: int) -> list[bytes]:
    """The seed inputs in order: every file, then the generated modules."""
    out = [bytes([MODE_NOLATCHES]) + path.read_bytes() for path in files]
    for seed in range(1, generated + 1):
        text = elab_profile.generate(seed).text.encode("utf-8")
        mode = 0 if seed % 2 else MODE_NOLATCHES | MODE_NOMEM2REG
        out.append(bytes([mode]) + text)
    return out


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out")
    parser.add_argument("sources", nargs="*")
    parser.add_argument("--generated", type=int, default=0)
    args = parser.parse_args(argv)
    try:
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        data = seeds(sources(args.sources), args.generated)
        for idx, blob in enumerate(data):
            (out / f"seed_{idx:04d}").write_bytes(blob)
    except (OSError, ValueError) as exc:
        print(f"elab_fuzz_seeds: {exc}", file=sys.stderr)
        return 2
    print(f"elab_fuzz_seeds: {len(data)} seeds in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
