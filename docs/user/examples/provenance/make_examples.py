#!/usr/bin/env python3
"""Regenerates every example file of docs/user/provenance.md into ``out/`` (V1.2.0 Task 11).

    python3 docs/user/examples/provenance/make_examples.py [--odin3 PATH] [--work DIR]

Needs a release build of Odin III (default ``build/release/odin3``) and, for the flow example,
``VTR_ROOT`` with a built VPR. Nothing is written by hand: the script runs ``odin3-flow``,
``odin3-prov-post`` and ``odin3`` on the two small designs in ``src/`` and on the hand-made
``why`` fixture of the tests, and writes slices of what they produced (a line type, chosen
records) to ``out/``. A slice that leaves lines out has a line ``... trimmed ...`` where they were.

The hash names of the flow example are a function of the design's absolute path
(``odin3-flow`` resolves it), so the work directory defaults to a fixed path and the output is
byte-identical only for that default. The work directory is only ever added to and overwritten,
never emptied. The second design is run from its own directory by relative path, so its names do
not depend on where the directory is.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
OUT = HERE / "out"
SRC = HERE / "src"
DEFAULT_WORK = Path("/tmp/claude-1000/pdoc-example")
TRIM = "... trimmed ..."
TOP = "bm_DL_BCD_adder"
FIXTURE = REPO / "tests" / "tools" / "fixtures" / "prov_why"
VPR_ARCH_FLOW = ["--timing-attr", "--vpr", "--keep-results"]


def run(cmd: list[str], cwd: Path, check: bool = True) -> str:
    """Run a command, return its stdout. Diagnostics (stderr) are not part of any example."""
    env = dict(os.environ)
    done = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False, env=env)
    if check and done.returncode != 0:
        sys.stderr.write(done.stdout + done.stderr)
        raise SystemExit(f"failed ({done.returncode}): {' '.join(cmd)}")
    return done.stdout


def odin(odin3: Path, script: str, cwd: Path) -> str:
    return run([str(odin3), "--diag-paths", "resolved", "-p", script], cwd)


def emit(name: str, lines: list[str]) -> None:
    OUT.mkdir(exist_ok=True)
    (OUT / name).write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")


def map_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").splitlines()


def of_type(lines: list[str], letter: str) -> list[str]:
    return [x for x in lines if x.startswith(letter + "\t")]


def pick(lines: list[str], wanted: list[str]) -> list[str]:
    """The R lines (or any lines with an id in field 2) whose id is in wanted, in file order,
    with the trim marker between two that were not neighbours."""
    out: list[str] = []
    previous = -1
    for index, line in enumerate(lines):
        if line.split("\t")[1] in wanted:
            if previous >= 0 and index != previous + 1:
                out.append(TRIM)
            out.append(line)
            previous = index
    return out


def head(lines: list[str], count: int) -> list[str]:
    return [*lines[:count], TRIM] if len(lines) > count else lines


def flow_example(odin3: Path, work: Path) -> Path:
    """odin3-flow --timing-attr --vpr --keep-results, then odin3-prov-post."""
    work.mkdir(parents=True, exist_ok=True)
    shutil.copy(SRC / f"{TOP}.v", work / f"{TOP}.v")
    run([str(REPO / "tools/flow/odin3-flow"), f"{TOP}.v", *VPR_ARCH_FLOW,
         "--out", "flow", "--odin3", str(odin3)], work)
    names = work / "flow" / "odin3" / f"{TOP}.blif.names"
    emit("flow_before_post.txt", head(map_lines(names), 3))
    shutil.copy(names, work / "before_post.names")
    post = run([str(REPO / "tools/flow/odin3-prov-post"), "flow"], work)
    emit("prov_post_summary.txt", [x.replace(str(work), ".") for x in post.splitlines()])
    return names


def map_slices(names: Path, odin3: Path, work: Path) -> None:
    lines = map_lines(names)
    emit("map_header_o_n.txt", [lines[0], *of_type(lines, "O")[:2], TRIM,
                                *of_type(lines, "N")[:3], TRIM])
    emit("map_r_odin.txt", pick(of_type(lines, "R"), ["r9", "r10", "r12", "r13", "r14"]))
    emit("map_r_post.txt", pick(of_type(lines, "R"),
                                ["r19", "r20", "r21", "r22", "r23", "r24", "r30", "r31"]))
    emit("map_c.txt", of_type(lines, "C"))
    emit("map_f_t.txt", [*of_type(lines, "F"), *of_type(lines, "T")])
    emit("map_a.txt", head(of_type(lines, "A"), 4))
    qs = [x for x in of_type(lines, "Q") if x.split("\t")[1] == "r30"]
    ks = [x for x in of_type(lines, "K") if x.split("\t")[1] == "r30"]
    emit("map_q_k.txt", [*qs, *head(ks, 4)])
    mp = f"flow/odin3/{TOP}.blif.names"
    look = odin(odin3, f"names --map {mp} --lookup h5j5t26zpp3", work)
    emit("names_lookup.txt", look.splitlines())
    emit("names_chain.txt", odin(odin3, f"names --map {mp} --chain Cout", work).splitlines())
    emit("names_check.txt", odin(odin3, f"names --map {mp} --check", work).splitlines())
    names_v2 = REPO / "tools" / "flow"
    py = (
        "import sys\n"
        f"sys.path.insert(0, {str(names_v2)!r})\n"
        "import name_map_v2\n"
        f"m = name_map_v2.read_name_map({mp!r})\n"
        "o = m.find('Cout')[0]\n"
        "print('Cout', o.kind, o.level, 'records', [r.number for r in m.provenance(o)])\n"
        "print('chain', [c.kind for c in m.chain(o)])\n"
        "q = m.why_of(30)\n"
        "print('r30 objective', q.objective, 'rho', q.rho, 'members', q.members,\n"
        "      len(m.members_of(30)), 'K lines')\n"
    )
    emit("python_reader.txt", run([sys.executable, "-I", "-c", py], work).splitlines())


def restore_examples(odin3: Path, work: Path) -> None:
    """Read the extended map back: unchanged (known gap), then after opt (stale marks)."""
    lib = REPO / "lib" / "vtr.o3lib"
    names = f"flow/odin3/{TOP}.blif.names"
    blif = f"flow/odin3/{TOP}.blif"
    (work / "rt").mkdir(exist_ok=True)
    head_cmd = f"read_techlib {lib}; read_blif --name-map {names} {blif}; "
    odin(odin3, head_cmd + "check; write_blif --vtr --name-style hash --prov-map rt/again.blif",
         work)
    odin(odin3, head_cmd + "opt -full; write_blif --vtr --name-style hash --prov-map rt/stale.blif",
         work)
    first = map_lines(work / names)
    again = map_lines(work / "rt" / "again.blif.names")
    counts = [f"{label:<22}{len(of_type(first, kind)):>4}{len(of_type(again, kind)):>7}"
              for label, kind in (("O lines", "O"), ("N lines", "N"), ("R lines", "R"),
                                  ("A lines", "A"), ("Q lines", "Q"), ("K lines", "K"))]
    emit("restore_counts.txt", ["line type             first  second", *counts])
    stale = map_lines(work / "rt" / "stale.blif.names")
    stale_t = [x for x in of_type(stale, "T") if x.split("\t")[1] in ("t7", "t8", "t9")]
    emit("restore_stale_t.txt", stale_t)
    emit("restore_stale_a.txt", head(of_type(stale, "A"), 3))
    flagged = [x for x in of_type(stale, "R") if "stale=" in x]
    emit("restore_stale_r.txt", [*flagged[:3], TRIM])
    qk = [x for x in stale if x.startswith(("Q\t", "K\t"))]
    emit("restore_stale_qk.txt", [*qk[:2], TRIM])


def abc_example(odin3: Path, work: Path) -> None:
    """Odin III's own joins: abc on a design with three comparisons."""
    micro = "bm_DL_4_bit_comparator"
    where = work / "abc"
    where.mkdir(parents=True, exist_ok=True)
    shutil.copy(SRC / f"{micro}.v", where / f"{micro}.v")
    lib = REPO / "lib" / "vtr.o3lib"
    odin(odin3, f"read_techlib {lib}; read_verilog --nolatches --vtr-compat --prov-ast refs "
         f"{micro}.v; proc; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut 6; "
         f"write_blif --vtr --name-style hash --prov-map {micro}.blif", where)
    lines = map_lines(where / f"{micro}.blif.names")
    joins = [x for x in of_type(lines, "R") if "join" in x.split("\t")[-1]]
    ids = [x.split("\t")[1] for x in joins]
    t3 = [x for x in of_type(lines, "T") if x.split("\t")[1] == "t3"]
    cons = {c for x in joins for c in x.split("\t")[8].split(",") if c}
    cs = [x for x in of_type(lines, "C") if x.split("\t")[1] in cons]
    emit("abc_joins.txt", [*joins, *t3, *of_type(lines, "Q")[:2]])
    emit("abc_cons.txt", cs)
    assert ids, "no join in the abc example"


def why_example(work: Path) -> None:
    """Pair classes: the hand-made fixture of the tests with a Norns census."""
    where = work / "why"
    shutil.copytree(FIXTURE / "flow", where / "flow", dirs_exist_ok=True)
    for name in ("census.tsv", "metrics.json", "arch.xml"):
        shutil.copy(FIXTURE / name, where / name)
    run([str(REPO / "tools/flow/odin3-prov-post"), "flow", "--census", "census.tsv",
         "--arch", "arch.xml"], where)
    lines = map_lines(where / "flow" / "odin3" / "why.blif.names")
    qs = of_type(lines, "Q")
    ks = [x for x in of_type(lines, "K") if x.split("\t")[1] == qs[0].split("\t")[1]]
    emit("why_q_k.txt", [qs[0], *ks[:4], TRIM, qs[1]])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--odin3", type=Path, default=REPO / "build" / "release" / "odin3")
    ap.add_argument("--work", type=Path, default=DEFAULT_WORK)
    args = ap.parse_args()
    odin3 = args.odin3.resolve()
    work = args.work.resolve()
    names = flow_example(odin3, work)
    map_slices(names, odin3, work)
    restore_examples(odin3, work)
    abc_example(odin3, work)
    why_example(work)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
