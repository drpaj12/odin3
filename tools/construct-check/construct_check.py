"""construct-check: the construct snapshot of V1.2.0 Task 4 (4.2, 4.3 item 4, section 8).

Usage::

    construct-check golden --odin3 O [--write] DIR
    construct-check bytes  --odin3 O [--odin3-base B --base-lib DIR] [--list FILE] [--jobs N]
                           [--out DIR] [--limit N] <design.v or dir>...

``golden``: every ``.v`` and ``.sv`` file of DIR is read (``read_verilog``), checked (``check``,
which runs rule 12) and dumped with ``dump_locs --constructs``; the dump must equal
``DIR/<stem>.constructs`` (``--write`` rewrites the files instead).  Exit 0 all equal, 1 any
difference or failed run.

``bytes``: the writer outputs of every design under the paths given are compared byte for byte
between two runs of the same flow: with ``--odin3-base`` (a build of main, run with
``LD_LIBRARY_PATH=--base-lib``) against ``--odin3``, else ``--odin3`` with ``--prov-ast off``
against ``--odin3`` with each of the other modes.  Files are written under a relative path in a
fresh directory per run, so no output holds a run-dependent path.  Logs are not compared.  A
design that fails in both runs at the same step with the same exit code is equal.  Exit 0 no
difference, 1 any difference.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

MODES = ("off", "refs", "constructs", "full")
TIMEOUT_S = 600

# One flow: every writer, before and after the lowering and ABC.  The first pass that fails ends
# the run (the exit code and the files written so far are what is compared).
WORD = [
    "write_rtlil w.il",
    "dump_locs w.locs",
    "proc",
    "opt",
    "write_json w.json",
    "write_json w.odin3.json --odin3",
    "write_json w.hash.json --name-style hash",
    "write_json w.prov.json --name-style provenance",
    "write_verilog w.v",
    "write_verilog w.hash.v --name-style hash",
    "write_dot w.dot --locs",
    "write_blif w.blif",
    "write_blif w.src.blif --src-attr",
    "flatten",
    "opt",
    "lower",
    "write_blif g.blif",
    "dump_locs g.locs",
    "abc --lut 6",
    "write_blif l.vtr.blif --vtr",
    "write_blif l.hash.blif --vtr --name-style hash",
    "write_blif l.prov.blif --vtr --name-style provenance",
    "write_json l.json",
    "write_verilog l.v",
    "dump_locs l.locs",
]


def designs_of(paths: Sequence[str]) -> list[Path]:
    """The ``.v`` and ``.sv`` files under the paths, smallest first (ties by path)."""
    found: list[Path] = []
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            found += sorted(p for p in path.rglob("*") if p.suffix in (".v", ".sv"))
        else:
            found.append(path)
    return sorted(found, key=lambda p: (p.stat().st_size, str(p)))


def run_flow(odin3: str, lib: str | None, design: Path, mode: str | None, out: Path) -> int:
    """One odin3 process in ``out`` (cwd); returns its exit code, 125 on a timeout."""
    out.mkdir(parents=True, exist_ok=True)
    flag = f" --prov-ast {mode}" if mode is not None else ""
    script = f"read_verilog{flag} {design.resolve()}; " + "; ".join(WORD)
    env = dict(os.environ)
    if lib is not None:
        env["LD_LIBRARY_PATH"] = lib
    try:
        proc = subprocess.run([odin3, "-p", script], cwd=out, env=env, capture_output=True,
                              timeout=TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        return 125
    (out / "exit.txt").write_text(f"{proc.returncode}\n")
    return proc.returncode


def tree_of(root: Path) -> dict[str, bytes]:
    files = sorted(p for p in root.rglob("*") if p.is_file())
    return {str(p.relative_to(root)): p.read_bytes() for p in files}


def diff_trees(a: dict[str, bytes], b: dict[str, bytes]) -> list[str]:
    return [name for name in sorted(set(a) | set(b)) if a.get(name) != b.get(name)]


class Side:
    def __init__(self, odin3: str, lib: str | None, mode: str | None) -> None:
        self.odin3, self.lib, self.mode = odin3, lib, mode


def compare_one(design: Path, base: Side, other: Side, work: Path, index: int) -> list[str]:
    ra, rb = work / f"{index}a", work / f"{index}b"
    run_flow(base.odin3, base.lib, design, base.mode, ra)
    run_flow(other.odin3, other.lib, design, other.mode, rb)
    return diff_trees(tree_of(ra), tree_of(rb))


def cmd_bytes(args: argparse.Namespace) -> int:
    designs = designs_of(args.paths)
    if args.limit:
        designs = designs[: args.limit]
    pairs: list[tuple[Side, Side]] = []
    if args.odin3_base:
        pairs.append((Side(args.odin3_base, args.base_lib, None), Side(args.odin3, None, None)))
    else:
        off = Side(args.odin3, None, "off")
        pairs += [(off, Side(args.odin3, None, m)) for m in MODES[1:]]
    # the run directory is left in place (the controller cleans /tmp/claude-1000)
    work = Path(tempfile.mkdtemp(prefix="ops-cc-", dir=args.out))
    jobs: list[tuple[Path, Side, Side, Path, int]] = []
    for p, (base, other) in enumerate(pairs):
        for i, design in enumerate(designs):
            jobs.append((design, base, other, work / str(p), i))
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for job, diffs in zip(jobs, pool.map(lambda j: compare_one(*j), jobs),
                               strict=True):
            if diffs:
                bad += 1
                print(f"DIFF {job[0]} ({job[2].mode or 'new'}): {', '.join(diffs[:6])}")
    files = sum(1 for p in work.rglob("*") if p.is_file())
    print(f"{len(designs)} designs, {len(pairs)} comparison(s), {bad} with differences "
          f"({files} files compared, both sides)")
    return 1 if bad else 0


def dump_of(odin3: str, design: Path, mode: str) -> tuple[int, str, str]:
    script = f"read_verilog --prov-ast {mode} {design}; check; dump_locs --constructs -"
    proc = subprocess.run([odin3, "-p", script], capture_output=True, text=True, check=False,
                          cwd=design.parent)
    return proc.returncode, proc.stdout, proc.stderr[-300:]


def mode_problems(odin3: str, design: Path, refs_rows: int) -> list[str]:
    """What the other modes must give (4.2): off no table; constructs no expression row but the
    call rows; full at least the rows of refs.  Every run passes ``check``."""
    problems = []
    for mode in ("off", "constructs", "full"):
        code, out, err = dump_of(odin3, design, mode)
        rows = [line.split("|") for line in out.splitlines() if line.startswith("construct|")]
        if code != 0:
            problems.append(f"{mode}: exit {code}: {err}")
        elif mode == "off" and out != "":
            problems.append("off: the table is not empty")
        elif mode == "constructs" and any(r[3] == "expr" and r[2] != "CALL" for r in rows):
            problems.append("constructs: an expression row")
        elif mode == "full" and len(rows) < refs_rows:
            problems.append(f"full: {len(rows)} rows, fewer than refs ({refs_rows})")
    return problems


def cmd_golden(args: argparse.Namespace) -> int:
    root = Path(args.dir).resolve()
    bad = 0
    count = 0
    for design in sorted(p for p in root.iterdir() if p.suffix in (".v", ".sv")):
        count += 1
        script = f"read_verilog --prov-ast refs {design}; check; dump_locs --constructs -"
        proc = subprocess.run([args.odin3, "-p", script],
                              capture_output=True, text=True, check=False, cwd=root)
        golden = design.with_suffix(".constructs")
        if proc.returncode != 0:
            print(f"FAIL {design.name}: exit {proc.returncode}: {proc.stderr[-300:]}")
            bad += 1
        elif args.write:
            golden.write_text(proc.stdout)
        elif not golden.exists() or golden.read_text() != proc.stdout:
            print(f"DIFF {design.name}: the dump differs from {golden.name}")
            bad += 1
        if proc.returncode == 0:
            refs_rows = sum(1 for line in proc.stdout.splitlines() if line.startswith("construct|"))
            for problem in mode_problems(args.odin3, design, refs_rows):
                print(f"FAIL {design.name}: {problem}")
                bad += 1
    print(f"{count} designs, {bad} failed")
    return 1 if bad or count == 0 else 0


def main(argv: Sequence[str] | None = None) -> int:
    top = argparse.ArgumentParser(prog="construct-check", description=__doc__)
    sub = top.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("golden")
    g.add_argument("--odin3", required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("dir")
    b = sub.add_parser("bytes")
    b.add_argument("--odin3", required=True)
    b.add_argument("--odin3-base")
    b.add_argument("--base-lib")
    b.add_argument("--jobs", type=int, default=4)
    b.add_argument("--limit", type=int, default=0)
    b.add_argument("--out", default=None)
    b.add_argument("paths", nargs="+")
    args = top.parse_args(argv)
    args.odin3 = str(Path(args.odin3).resolve())
    if getattr(args, "odin3_base", None):
        args.odin3_base = str(Path(args.odin3_base).resolve())
    if getattr(args, "base_lib", None):
        args.base_lib = str(Path(args.base_lib).resolve())
    return cmd_golden(args) if args.cmd == "golden" else cmd_bytes(args)


if __name__ == "__main__":
    sys.exit(main())
