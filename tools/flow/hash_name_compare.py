"""hash_name_compare: are the hash names of two odin3 binaries the same?

Usage::

    python3 tools/flow/hash_name_compare.py BASE_ODIN3 NEW_ODIN3 [--repo DIR] [--out DIR]

Runs both binaries on every micro under ``tests/micro/verilog/micro`` (smallest file first) through
the VTR flow to ``lower`` (``read_techlib lib/vtr.o3lib; read_verilog --nolatches --vtr-compat``;
``proc; opt; flatten; opt -full; wreduce; hardmap; lower``) and writes BLIF (``write_blif --vtr``),
JSON and Verilog with ``--name-style hash``, each with and without ``--prov-map`` (with ``--prov-ast
refs`` on the read for the mapped runs too), 4 variants per micro and format. Every output and name
map file is compared byte for byte. Prints one dict of counts (``files``, ``same``, ``differ``,
``fail``) and the first differing files; exit 0 when every file is the same and no run failed.

This is the comparison of the hash-name determinism fix (V1.2.0 V112-23, V112-24): Debug against
Release of one commit (``tools/flow/hash_name_compare.py build/debug/odin3 build/release/odin3``),
and the fixed commit against main. Stdlib only.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

WRITERS = {"blif": "write_blif --vtr", "json": "write_json", "v": "write_verilog"}
FLOW = "proc; opt; flatten; opt -full; wreduce; hardmap; lower"
SHOWN = 10


def run_one(exe: Path, repo: Path, source: Path, ast: str, kind: str, prov: str, dst: Path) -> int:
    """One odin3 run; its exit status."""
    lib = repo / "lib" / "vtr.o3lib"
    script = (
        f"read_techlib {lib}; read_verilog --nolatches --vtr-compat {ast} {source}; {FLOW}; "
        f"{WRITERS[kind]} --name-style hash {prov} {dst}"
    )
    return subprocess.run(
        [str(exe), "-p", script], capture_output=True, cwd=repo, check=False
    ).returncode


def variants() -> list[tuple[str, str, str, str]]:
    """The (tag, ast option, writer kind, prov option) runs of one micro."""
    runs = []
    for ast in ("", "--prov-ast refs"):
        for kind in WRITERS:
            for prov in ("", "--prov-map"):
                if not prov and ast:
                    continue
                tag = f"{'a' if ast else 'n'}{'m' if prov else 'x'}.{kind}"
                runs.append((tag, ast, kind, prov))
    return runs


def compare(base: Path, new: Path, repo: Path, out: Path) -> tuple[dict[str, int], list[str]]:
    """The counts and the differing file tags of all micros."""
    micro_dir = repo / "tests" / "micro" / "verilog" / "micro"
    micros = sorted(micro_dir.glob("*.v"), key=lambda p: (p.stat().st_size, p.name))
    counts = {"files": 0, "same": 0, "differ": 0, "fail": 0}
    diffs: list[str] = []
    for side in ("a", "b"):
        (out / side).mkdir(parents=True, exist_ok=True)
    for source in micros:
        for tag, ast, kind, prov in variants():
            name = f"{source.stem}.{tag}"
            a_path, b_path = out / "a" / name, out / "b" / name
            failed = run_one(base, repo, source, ast, kind, prov, a_path) or run_one(
                new, repo, source, ast, kind, prov, b_path
            )
            if failed:
                counts["fail"] += 1
                continue
            for suffix in ("", ".names"):
                first, second = Path(f"{a_path}{suffix}"), Path(f"{b_path}{suffix}")
                if not first.exists():
                    continue
                counts["files"] += 1
                if first.read_bytes() == second.read_bytes():
                    counts["same"] += 1
                else:
                    counts["differ"] += 1
                    diffs.append(name + suffix)
    return counts, diffs


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n", maxsplit=1)[0])
    ap.add_argument("base", type=Path, help="the odin3 binary to compare against")
    ap.add_argument("new", type=Path, help="the odin3 binary under test")
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument("--out", type=Path, default=Path("hash-name-compare"))
    args = ap.parse_args(argv)
    counts, diffs = compare(args.base, args.new, args.repo, args.out)
    print(counts)
    print("\n".join(diffs[:SHOWN]))
    return 0 if counts["differ"] == 0 and counts["fail"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
