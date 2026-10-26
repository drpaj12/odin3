"""construct-check: the construct snapshot of V1.2.0 Task 4 (4.2, 4.3 item 4, section 8).

Usage::

    construct-check golden --odin3 O [--write] [--no-sv] DIR
    construct-check escape --odin3 O
    construct-check bytes  --odin3 O [--odin3-base B --base-lib DIR [--mode M]] [--list FILE]
                           [--jobs N] [--out DIR] [--limit N] [--no-sv] [--no-abc]
                           <design.v or dir>...

``golden``: every ``.v`` and ``.sv`` file of DIR is read (``read_verilog --prov-ast M``), checked
(``check``, which runs rule 12) and dumped with ``dump_locs --constructs`` for the modes ``refs``,
``constructs`` and ``full``; the dumps must equal ``DIR/<stem>.constructs`` (refs),
``DIR/<stem>.mode-constructs`` and ``DIR/<stem>.mode-full`` (``--write`` rewrites the files
instead).  ``off`` must give an empty dump.  Exit 0 all equal, 1 any difference or failed run.
``--no-sv`` leaves out the ``.sv`` designs (a build without slang).

Note on ``sv_always.sv``: slang leaves the end spans of MODULE, PORT_DECL and ALWAYS unset, so
its golden holds ``0:0`` there; the golden changes when slang fills them.

``escape``: a design whose file name holds a ``|`` is dumped with ``dump_locs --constructs``; the
file field of every row must show the ``|`` escaped, so each ``construct`` line keeps its ten
fields when split on the unescaped ``|``.

``bytes``: the writer outputs of every design under the paths given are compared byte for byte
between two runs of the same flow: with ``--odin3-base`` (a build of main, run with
``LD_LIBRARY_PATH=--base-lib``) against ``--odin3``, else ``--odin3`` with ``--prov-ast off``
against ``--odin3`` with each of the other modes.  Both sides run with ``--prov-ast off`` given
(the main build, which has no such option, with ``ODIN3_PROV_AST`` removed from its environment).
``--no-abc`` leaves out the ``abc`` step (a build without ABC).  Files are written under a relative
path in a fresh directory per run, so no output holds a run-dependent path.  Logs are not
compared.  A design that fails in both runs at the same step with the same exit code is equal.
Exit 0 no difference, 1 any difference.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
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


def designs_of(paths: Sequence[str], sv: bool = True) -> list[Path]:
    """The ``.v`` (and ``.sv``) files under the paths, smallest first (ties by path)."""
    suffixes = (".v", ".sv") if sv else (".v",)
    found: list[Path] = []
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            found += sorted(p for p in path.rglob("*") if p.suffix in suffixes)
        else:
            found.append(path)
    return sorted(found, key=lambda p: (p.stat().st_size, str(p)))


def run_flow(odin3: str, lib: str | None, design: Path, mode: str | None, out: Path,
             flow: Sequence[str] = WORD) -> int:
    """One odin3 process in ``out`` (cwd); returns its exit code, 125 on a timeout."""
    out.mkdir(parents=True, exist_ok=True)
    flag = f" --prov-ast {mode}" if mode is not None else ""
    script = f"read_verilog{flag} {design.resolve()}; " + "; ".join(flow)
    env = dict(os.environ)
    if mode is None:
        env.pop("ODIN3_PROV_AST", None)  # a build of main: no such option, and no stray default
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


def compare_one(design: Path, base: Side, other: Side, work: tuple[Path, int],
                flow: Sequence[str]) -> list[str]:
    ra, rb = work[0] / f"{work[1]}a", work[0] / f"{work[1]}b"
    run_flow(base.odin3, base.lib, design, base.mode, ra, flow)
    run_flow(other.odin3, other.lib, design, other.mode, rb, flow)
    return diff_trees(tree_of(ra), tree_of(rb))


def cmd_bytes(args: argparse.Namespace) -> int:
    designs = designs_of(args.paths, not args.no_sv)
    flow = [w for w in WORD if not (args.no_abc and w.startswith("abc "))]
    if args.limit:
        designs = designs[: args.limit]
    pairs: list[tuple[Side, Side]] = []
    if args.odin3_base:
        base = Side(args.odin3_base, args.base_lib, None)
        pairs.append((base, Side(args.odin3, None, args.mode)))
    else:
        off = Side(args.odin3, None, "off")
        pairs += [(off, Side(args.odin3, None, m)) for m in MODES[1:]]
    # the run directory is left in place (the controller cleans /tmp/claude-1000)
    work = Path(tempfile.mkdtemp(prefix="ops-cc-", dir=args.out))
    jobs: list[tuple[Path, Side, Side, tuple[Path, int]]] = []
    for p, (base, other) in enumerate(pairs):
        for i, design in enumerate(designs):
            jobs.append((design, base, other, (work / str(p), i)))
    bad = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = pool.map(lambda j: compare_one(j[0], j[1], j[2], j[3], flow), jobs)
        for job, diffs in zip(jobs, results, strict=True):
            if diffs:
                bad += 1
                print(f"DIFF {job[0]} ({job[2].mode}): {', '.join(diffs[:6])}")
    files = sum(1 for p in work.rglob("*") if p.is_file())
    print(f"{len(designs)} designs, {len(pairs)} comparison(s), {bad} with differences "
          f"({files} files compared, both sides)")
    return 1 if bad else 0


def dump_of(odin3: str, design: Path, mode: str) -> tuple[int, str, str]:
    script = f"read_verilog --prov-ast {mode} {design}; check; dump_locs --constructs -"
    proc = subprocess.run([odin3, "-p", script], capture_output=True, text=True, check=False,
                          cwd=design.parent)
    return proc.returncode, proc.stdout, proc.stderr[-300:]


def golden_suffix(mode: str) -> str:
    """The golden file suffix of a mode (refs keeps the plain ``.constructs``)."""
    return ".constructs" if mode == "refs" else f".mode-{mode}"


def mode_problems(odin3: str, design: Path, rows_by_mode: dict[str, list[list[str]]]) -> list[str]:
    """What the modes must give (4.2): off no table; constructs no expression row but the call
    rows; full at least the rows of refs.  Every run passes ``check``."""
    problems = []
    code, out, err = dump_of(odin3, design, "off")
    if code != 0:
        problems.append(f"off: exit {code}: {err}")
    elif out != "":
        problems.append("off: the table is not empty")
    if any(r[3] == "expr" and r[2] != "CALL" for r in rows_by_mode["constructs"]):
        problems.append("constructs: an expression row")
    if len(rows_by_mode["full"]) < len(rows_by_mode["refs"]):
        problems.append(f"full: {len(rows_by_mode['full'])} rows, fewer than refs "
                        f"({len(rows_by_mode['refs'])})")
    return problems


# Designs whose function or task is called twice: the declaration kind, and how many call-instance
# copies of it the refs table holds (two calls).  Order of declaration and call does not matter.
CALL_INSTANCES = {
    "func_twice": ("FUNCTION_DECL", 2),
    "func_after": ("FUNCTION_DECL", 2),
    "task_call": ("TASK_DECL", 2),
}


# The refs row count of the shared-subtree regression design (it fixes the number the walk emits
# when it enters each node once).
ROW_COUNTS = {"shared_expr": 96}


def call_problems(name: str, rows: list[list[str]]) -> list[str]:
    """Every call-instance row hangs, through its copies, under a FUNCTION_DECL or TASK_DECL whose
    parent is its call row (a CALL or TASK_CALL); the counts of CALL_INSTANCES hold."""
    by_index = {int(r[1]): r for r in rows}
    problems = []
    decls = 0
    for row in rows:
        call = int(row[6])
        if call == 0:
            continue
        at: list[str] | None = row
        while at is not None and int(at[5]) != call:
            at = by_index.get(int(at[5]))
        ok = (at is not None and at[2] in ("FUNCTION_DECL", "TASK_DECL")
              and by_index[call][2] in ("CALL", "TASK_CALL"))
        if not ok:
            problems.append(f"row {row[1]}: chain misses a declaration under call {call}")
        decls += 1 if row[2] in ("FUNCTION_DECL", "TASK_DECL") and int(row[5]) == call else 0
    if name in ROW_COUNTS and len(rows) != ROW_COUNTS[name]:
        problems.append(f"{len(rows)} refs rows, expected {ROW_COUNTS[name]}")
    want = CALL_INSTANCES.get(name)
    if want is not None:
        found = sum(
            1 for r in rows if int(r[6]) != 0 and r[2] == want[0] and int(r[5]) == int(r[6])
        )
        if found != want[1]:
            problems.append(f"{found} {want[0]} call-instance rows, expected {want[1]}")
    elif decls == 0 and name in CALL_INSTANCES:
        problems.append("no call-instance declaration")
    return problems


def golden_one(args: argparse.Namespace, design: Path) -> int:
    """Failures of one design: its three dumps against their goldens, then the mode rules."""
    bad = 0
    rows: dict[str, list[list[str]]] = {}
    for mode in ("refs", "constructs", "full"):
        code, out, err = dump_of(args.odin3, design, mode)
        golden = design.with_suffix(golden_suffix(mode))
        if code != 0:
            print(f"FAIL {design.name} ({mode}): exit {code}: {err}")
            return bad + 1
        rows[mode] = [ln.split("|") for ln in out.splitlines() if ln.startswith("construct|")]
        if args.write:
            golden.write_text(out)
        elif not golden.exists() or golden.read_text() != out:
            print(f"DIFF {design.name} ({mode}): the dump differs from {golden.name}")
            bad += 1
    problems = mode_problems(args.odin3, design, rows) + call_problems(design.stem, rows["refs"])
    for problem in problems:
        print(f"FAIL {design.name}: {problem}")
        bad += 1
    return bad


def cmd_golden(args: argparse.Namespace) -> int:
    root = Path(args.dir).resolve()
    suffixes = (".v",) if args.no_sv else (".v", ".sv")
    designs = sorted(p for p in root.iterdir() if p.suffix in suffixes)
    bad = sum(golden_one(args, design) for design in designs)
    print(f"{len(designs)} designs, {bad} failed")
    return 1 if bad or not designs else 0


def cmd_escape(args: argparse.Namespace) -> int:
    """A file name with a ``|`` must not break the lines of ``dump_locs --constructs``."""
    work = Path(tempfile.mkdtemp(prefix="ops-esc-", dir=args.out))  # left in place, not deleted
    design = work / "a|b.v"
    design.write_text("module m (input a, output reg q);\n  always @* q = a;\nendmodule\n")
    code, out, err = dump_of(args.odin3, design, "refs")
    lines = [ln for ln in out.splitlines() if ln.startswith("construct|")]
    bad = [ln for ln in lines if len(re.split(r"(?<!\\)\|", ln)) != 10 or "a\\|b.v:" not in ln]
    if code != 0 or not lines or bad:
        print(f"FAIL escape: exit {code}, {len(lines)} rows, {len(bad)} broken: {err}")
        return 1
    print(f"escape: {len(lines)} rows, all with the file name escaped")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    top = argparse.ArgumentParser(prog="construct-check", description=__doc__)
    sub = top.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("golden")
    g.add_argument("--odin3", required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--no-sv", action="store_true")
    g.add_argument("dir")
    e = sub.add_parser("escape")
    e.add_argument("--odin3", required=True)
    e.add_argument("--out", default=None)
    b = sub.add_parser("bytes")
    b.add_argument("--odin3", required=True)
    b.add_argument("--odin3-base")
    b.add_argument("--mode", default="off", choices=MODES,
                   help="with --odin3-base: the --prov-ast mode of the new build (default off)")
    b.add_argument("--base-lib")
    b.add_argument("--jobs", type=int, default=4)
    b.add_argument("--limit", type=int, default=0)
    b.add_argument("--out", default=None)
    b.add_argument("--no-sv", action="store_true")
    b.add_argument("--no-abc", action="store_true")
    b.add_argument("paths", nargs="+")
    args = top.parse_args(argv)
    args.odin3 = str(Path(args.odin3).resolve())
    if getattr(args, "odin3_base", None):
        args.odin3_base = str(Path(args.odin3_base).resolve())
    if getattr(args, "base_lib", None):
        args.base_lib = str(Path(args.base_lib).resolve())
    commands = {"golden": cmd_golden, "bytes": cmd_bytes, "escape": cmd_escape}
    return commands[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
