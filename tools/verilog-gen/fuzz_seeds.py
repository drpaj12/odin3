#!/usr/bin/env python3
"""fuzz_seeds.py — the libFuzzer seed corpus of tests/fuzz/fuzz_verilog.c (2C spec §7.5).

Usage::

    fuzz_seeds.py OUT_DIR [--generated N] SOURCE...

Each SOURCE is a Verilog file or a directory (its ``*.v``).  The sources, smallest first, are
written into OUT_DIR twice: as a raw-mode input (mode byte 0x00, then the file's bytes) and as
a token-stream input (mode byte 0x01, then one byte per token of the file, the index of the
matching ``k_alphabet`` spelling, which is read from fuzz_verilog.c itself so the two cannot
drift; an unmatched identifier maps to ``a``, an unmatched number to ``42``, anything else is
skipped).  Two more raw inputs come first: a UTF-8 byte order mark before the smallest source
(2C Review Focus 5) and the same with a Latin-1 byte in a comment.  ``--generated N`` adds the
raw text of verilog_gen.py's seeds 1..N.  OUT_DIR is emptied of earlier seeds first.

Exit 0 written, 2 usage or I/O error.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Sequence
from pathlib import Path

import verilog_gen

HARNESS = Path(__file__).resolve().parents[2] / "tests" / "fuzz" / "fuzz_verilog.c"
BOM = b"\xef\xbb\xbf"
TOKEN_RE = re.compile(
    rb"//[^\n]*\n?|/\*.*?\*/|\\\S+\s?|`\w+|\d+\s*'[sS]?[bBoOdDhH]\s*[0-9a-fA-FxXzZ?_]+"
    rb"|'[sS]?[bBoOdDhH][0-9a-fA-FxXzZ?_]+|\d[\d_]*\.\d+(?:[eE][-+]?\d+)?|\d[\d_]*"
    rb'|"(?:\\.|[^"\\\n])*"|[A-Za-z_$][A-Za-z0-9_$]*|\(\*|\*\)|@\*|<<<|>>>|\*\*|~\^|<=|&&'
    rb"|<<|\S", re.S)


def alphabet(harness: Path = HARNESS) -> list[bytes]:
    """The k_alphabet spellings of the harness, unescaped, in order."""
    text = harness.read_text(encoding="utf-8")
    block = re.search(r"k_alphabet\[ALPHABET\] = \{(.*?)\};", text, re.S)
    if block is None:
        raise ValueError(f"{harness}: no k_alphabet")
    literals = re.findall(r'"((?:\\.|[^"\\])*)"', block.group(1))
    return [bytes(lit, "utf-8").decode("unicode_escape").encode("latin-1") for lit in literals]


def token_bytes(source: bytes, spellings: Sequence[bytes]) -> bytes:
    """One alphabet index per token of source (see the module docstring)."""
    index = {spelling.strip(): idx for idx, spelling in enumerate(spellings)}
    ident, number = index[b"a"], index[b"42"]
    out = bytearray()
    for match in TOKEN_RE.finditer(source):
        token = match.group(0).strip()
        if token in index:
            out.append(index[token])
        elif token.startswith(b"//") and b"synopsys" in token:
            out.append(index[b"// synopsys full_case parallel_case"])
        elif token.startswith(b"//"):
            out.append(index[b"// c"])
        elif re.fullmatch(rb"[A-Za-z_\\][A-Za-z0-9_$+\\]*", token):
            out.append(ident)
        elif token[:1].isdigit() or token.startswith(b"'"):
            out.append(number)
    return bytes(out)


def sources(paths: Sequence[str]) -> list[Path]:
    files: list[Path] = []
    for name in paths:
        path = Path(name)
        files.extend(sorted(path.glob("*.v")) if path.is_dir() else [path])
    return sorted(files, key=lambda p: (p.stat().st_size, str(p)))


def write_seeds(out: Path, files: Sequence[Path], generated: int) -> int:
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("seed_*"):
        old.unlink()
    spellings = alphabet()
    seeds: list[bytes] = []
    if files:
        first = files[0].read_bytes()
        seeds.append(b"\x00" + BOM + first)
        seeds.append(b"\x00" + BOM + b"// caf\xe9\n" + first)
    for path in files:
        data = path.read_bytes()
        seeds.append(b"\x00" + data)
        seeds.append(b"\x01" + token_bytes(data, spellings))
    for seed in range(1, generated + 1):
        seeds.append(b"\x00" + verilog_gen.render(seed, "in.v").text.encode("utf-8"))
    for idx, data in enumerate(seeds):
        (out / f"seed_{idx:04d}").write_bytes(data)
    return len(seeds)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("out")
    parser.add_argument("sources", nargs="*")
    parser.add_argument("--generated", type=int, default=0)
    args = parser.parse_args(argv)
    try:
        count = write_seeds(Path(args.out), sources(args.sources), args.generated)
    except (OSError, ValueError) as exc:
        print(f"fuzz_seeds: {exc}", file=sys.stderr)
        return 2
    print(f"fuzz_seeds: {count} seeds in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
