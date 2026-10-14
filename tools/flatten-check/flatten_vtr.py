"""flatten-vtr: the 19 VTR designs through `flatten` (3A spec 14.6, plan Task 5).

Usage::

    flatten-vtr --odin3 O [--yosys Y] [--root DIR] [--golden-root DIR] [--techlib L]
                [--primitives P] [--mem-cap C] [--cap-for NAME=C]... [--expect F]
                [--table F] [--no-yosys-diff] [--only NAME]...

The 19 designs of the Phase 3 exit test (PHASE3 exit item 1) run smallest source first, one child at
a time under memcap. Per design Odin III runs ``read_techlib L; read_verilog --vtr-compat
--nolatches F; hierarchy --top T; proc; opt; stats; flatten; check; stats`` (``T`` is the first
``.model`` of the design's Parmys golden) and the row records: wall seconds and peak RSS (a
Release ``odin3`` only), the flat module's nodes, nets and wires, the instances expanded, ``kept``
(expected 0, spec 7), ``renamed``, the provenance records and bytes the pass added and the bytes per
created object, and the pre-flight estimate against what the run reserved.

Checks, each a failure when it does not hold:

- the child exits 0 (so ``check`` passed) and ``kept`` is 0;
- **node counts**: the flat top's count of every cell type equals the sum, over the instance tree,
  of the counts in each module (the ``stats`` before and after; constants are shared and ports are
  not copied, so those types are left out);
- with ``--yosys``: ``flatten-diff`` (names, widths, cell types by module, MEMIDs; no equivalence
  proof at this size) of Odin III's ``proc; flatten`` against Yosys 0.55's, with ``--primitives``
  read first by Yosys. A difference not listed in ``--expect`` (``design [kinds]: reason``) fails.

``--table F`` writes the rows as a Markdown table (the one PHASE3 records). Exit 0 all rows ok, 1 a
row failed, 2 usage or tool error, 77 without a memory cap.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import flatten_common as fc
import flatten_diff as fd
import memcap
import toolutil

DESIGNS = (
    "bgm", "blob_merge", "boundtop", "ch_intrinsics", "diffeq1", "diffeq2", "LU8PEEng",
    "LU32PEEng", "mcml", "mkDelayWorker32B", "mkPktMerge", "mkSMAdapter4B", "or1200",
    "raygentop", "sha", "stereovision0", "stereovision1", "stereovision2", "stereovision3",
)
GOLDEN_ARCH = "k6_frac_N10_frac_chain_mem32K_40nm"
DEFAULT_CAP = "3G"
TIME_BIN = "/usr/bin/time"
# Types a flatten does not copy (ports) or shares (constants): not part of the node-count check.
SKIPPED_TYPES = ("$port_in", "$port_out", "$port_inout", "$_CONST0_", "$_CONST1_", "$_CONSTX_")

STATS_MODULE = re.compile(r"stats: module (\S+): ports (\d+), nodes (\d+), nets (\d+), wires (\d+)")
STATS_CELL = re.compile(r"stats: module (\S+): cell (\S+) (\d+)$")
EXPANDED = re.compile(
    r"flatten: (\d+) instances expanded \((\d+) kept\) in (\d+) roots; (\d+) modules unused"
)
COST = re.compile(
    r"flatten: cost: bytes (\d+) -> (\d+), provenance records (\d+) -> (\d+); added nodes (\d+) "
    r"pins (\d+) nets (\d+) wires (\d+); estimate (\d+); renamed (\d+)"
)
MIB = 1 << 20


@dataclass
class ModuleStats:
    ports: int = 0
    nodes: int = 0
    nets: int = 0
    wires: int = 0
    cells: Counter[str] = field(default_factory=Counter)


@dataclass
class Row:
    """What one design produced."""

    name: str
    top: str = ""
    wall: float | None = None
    rss_mib: float | None = None
    instances: int = 0
    kept: int = 0
    renamed: int = 0
    flat: ModuleStats = field(default_factory=ModuleStats)
    cost: tuple[int, ...] = ()
    problems: list[str] = field(default_factory=list)
    yosys: str = "-"


def parse_stats(log: str) -> list[dict[str, ModuleStats]]:
    """The ``stats`` outputs of a log, one dict (module -> stats) per ``stats`` pass, in order."""
    runs: list[dict[str, ModuleStats]] = []
    for line in log.splitlines():
        text = line.split("info: ", 1)[-1]
        if text.startswith("stats: design:"):
            runs.append({})
            continue
        if not runs:
            continue
        found = STATS_MODULE.match(text)
        if found:
            ports, nodes, nets, wires = (int(x) for x in found.groups()[1:])
            runs[-1][found.group(1)] = ModuleStats(ports, nodes, nets, wires)
            continue
        cell = STATS_CELL.match(text)
        if cell and cell.group(1) in runs[-1]:
            runs[-1][cell.group(1)].cells[cell.group(2)] = int(cell.group(3))
    return runs


def expected_flat(modules: dict[str, ModuleStats], top: str) -> Counter[str]:
    """The cell counts of ``top`` with every instance of a module replaced by that module's own
    expansion, walking the instance tree from the top with an explicit stack."""
    total: Counter[str] = Counter()
    stack: list[tuple[str, int]] = [(top, 1)]
    while stack:
        name, times = stack.pop()
        for kind, count in modules[name].cells.items():
            if kind in modules:
                stack.append((kind, times * count))
            elif kind not in SKIPPED_TYPES:
                total[kind] += times * count
    return total


def node_count_problems(pre: dict[str, ModuleStats], post: dict[str, ModuleStats], top: str
                        ) -> list[str]:
    """The cell types whose flat count is not the sum over the instance tree."""
    want = expected_flat(pre, top)
    got = Counter({k: v for k, v in post[top].cells.items() if k not in SKIPPED_TYPES})
    return [
        f"node count {kind}: flat {got[kind]}, instance tree {want[kind]}"
        for kind in sorted(want.keys() | got.keys())
        if want[kind] != got[kind]
    ]


def first_model(golden: Path) -> str:
    """The first ``.model`` of a golden BLIF: the design's top."""
    for line in golden.read_text(errors="replace").splitlines():
        if line.startswith(".model "):
            return line.split()[1]
    raise ValueError(f"{golden}: no .model")


def script(source: Path, top: str, techlib: str | None) -> str:
    lib = f"read_techlib {techlib}; " if techlib else ""
    return (
        f"{lib}read_verilog --vtr-compat --nolatches {source}; hierarchy --top {top}; proc; opt; "
        f"stats; flatten; check; stats"
    )


def run_odin(odin3: str, argv_tail: Sequence[str], cap: int, probe: Path | None
             ) -> tuple[int, str, float | None, float | None]:
    """(exit code, log, wall seconds, peak RSS MiB) of one capped run. A Release binary runs under
    GNU time for the wall time and the peak; an ASan one reports neither."""
    if probe is not None and not memcap.is_asan(odin3):
        argv = [TIME_BIN, "-f", "%e %M", "-o", str(probe), odin3, *argv_tail]
    else:
        argv = [odin3, *argv_tail]
    done = memcap.run(argv, cap, capture_output=True)
    log = (done.stdout or "") + (done.stderr or "")
    wall = rss = None
    if probe is not None and probe.is_file() and probe.read_text().strip():
        seconds, kib = probe.read_text().split()[-2:]
        wall, rss = float(seconds), int(kib) / 1024
    return done.returncode, log, wall, rss


def measure(row: Row, odin3: str, source: Path, args: argparse.Namespace) -> None:
    """Fills ``row`` from one Odin III run; failures go to ``row.problems``."""
    cap = cap_of(args, source.name)
    with tempfile.TemporaryDirectory() as tmp:
        code, log, row.wall, row.rss_mib = run_odin(
            odin3,
            ["--top", row.top, "-p", script(source, row.top, args.techlib)],
            cap,
            Path(tmp) / "time.txt",
        )
    if code != 0:
        tail = [x for x in log.strip().splitlines() if "error" in x][-2:]
        row.problems.append(f"odin3 exit {code}: {' | '.join(tail)[:300]}")
        return
    done = EXPANDED.search(log)
    cost = COST.search(log)
    runs = parse_stats(log)
    if done is None or cost is None or len(runs) < 2:
        row.problems.append("the flatten or stats lines are missing from the log")
        return
    row.instances, row.kept = int(done.group(1)), int(done.group(2))
    row.cost = tuple(int(x) for x in cost.groups())
    row.renamed = row.cost[-1]
    row.flat = runs[1][row.top]
    if row.kept != 0:
        row.problems.append(f"kept={row.kept} (expected 0)")
    else:
        row.problems += node_count_problems(runs[0], runs[1], row.top)


def yosys_leg(row: Row, source: Path, args: argparse.Namespace, expect: dict[str, fd.Expected]
              ) -> None:
    """flatten-diff against Yosys 0.55 (no equivalence proof); fills ``row.yosys``."""
    tools = fc.Tools(args.odin3, args.yosys, args.techlib, args.primitives, True)
    result = fd.run_one(source, row.top, tools, None, equiv=False)
    if isinstance(result, str):
        row.yosys = "error"
        row.problems.append(f"flatten-diff: {result}")
        return
    entry = expect.get(row.name)
    unexpected, stale = fd.judge(result, entry)
    if unexpected:
        row.yosys = "DIFFERS"
        row.problems += [f"flatten-diff: {p}" for p in unexpected[:6]]
    elif entry is not None and stale:
        row.yosys = "stale"
        row.problems.append(f"flatten-diff: stale expect entry ({entry.reason})")
    else:
        row.yosys = "expected" if entry is not None else "same"


def cap_of(args: argparse.Namespace, name: str) -> int:
    """The memory cap for the file ``name``: ``--cap-for NAME=C``, else ``--mem-cap``."""
    text = dict(item.split("=", 1) for item in args.cap_for).get(name, args.mem_cap)
    return int(toolutil.parse_size(text))


def ordered(root: Path, only: Sequence[str]) -> list[Path]:
    """The designs' source files, smallest first (ties by name)."""
    names = [n for n in DESIGNS if not only or n in only]
    files = [root / f"{n}.v" for n in names]
    return sorted(files, key=lambda p: (p.stat().st_size, p.name))


def table(rows: Sequence[Row]) -> str:
    head = (
        "| design | wall s | peak MiB | instances | kept | renamed | flat nodes | flat nets | "
        "flat wires | records | bytes/object | estimate/actual | yosys | ok |\n"
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
    )
    lines = [head]
    for row in rows:
        lines.append(table_row(row))
    return "\n".join(lines) + "\n"


def table_row(row: Row) -> str:
    wall = "-" if row.wall is None else f"{row.wall:.2f}"
    rss = "-" if row.rss_mib is None else f"{row.rss_mib:.0f}"
    if row.cost:
        before, after, rec0, rec1, nodes, _pins, nets, wires, estimate, _ = row.cost
        objects = nodes + nets + wires
        per = f"{(after - before) / objects:.0f}" if objects else "-"
        ratio = f"{estimate / (after - before):.2f}" if after > before else "-"
        records = str(rec1 - rec0)
    else:
        per = ratio = records = "-"
    return (
        f"| {row.name} | {wall} | {rss} | {row.instances} | {row.kept} | {row.renamed} | "
        f"{row.flat.nodes} | {row.flat.nets} | {row.flat.wires} | {records} | {per} | {ratio} | "
        f"{row.yosys} | {'yes' if not row.problems else 'NO'} |"
    )


def raw_lines(rows: Sequence[Row]) -> str:
    """``design,bytes_added,records_added,nodes,pins,nets,wires,estimate`` per design (the numbers
    the estimate constants are fitted to; nodes..wires are the objects the run added)."""
    lines = ["design,bytes_added,records_added,nodes,pins,nets,wires,estimate"]
    for row in rows:
        if row.cost:
            before, after, rec0, rec1, nodes, pins, nets, wires, estimate, _ = row.cost
            numbers = (after - before, rec1 - rec0, nodes, pins, nets, wires, estimate)
            lines.append(f"{row.name}," + ",".join(str(n) for n in numbers))
    return "\n".join(lines) + "\n"


def parse(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="flatten-vtr", description=__doc__)
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--yosys", help="the Yosys 0.55 binary (enables the flatten-diff leg)")
    ap.add_argument("--root", type=Path, help="benchmarks/verilog (default $VTR_ROOT/...)")
    ap.add_argument(
        "--golden-root", type=Path, default=Path.home() / "odin3-ws/golden" / GOLDEN_ARCH
    )
    ap.add_argument("--techlib", default="lib/vtr.o3lib")
    ap.add_argument("--primitives", help="VTR's primitives.v for Yosys (default $VTR_ROOT/...)")
    ap.add_argument("--mem-cap", default=DEFAULT_CAP)
    ap.add_argument("--cap-for", action="append", default=[], metavar="NAME.v=CAP")
    ap.add_argument("--expect", type=Path, help="design [kinds]: reason lines for the Yosys leg")
    ap.add_argument("--table", type=Path, help="write the Markdown table here")
    ap.add_argument("--raw", type=Path, help="write the cost numbers, one CSV line per design")
    ap.add_argument("--only", action="append", default=[], help="run only this design")
    return ap.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse(argv)
    vtr = os.environ.get("VTR_ROOT", "")
    root = args.root or Path(vtr) / "vtr_flow/benchmarks/verilog"
    if args.yosys and not args.primitives:
        args.primitives = str(Path(vtr) / "vtr_flow/primitives.v")
    expect = fd.load_expect(args.expect)
    rows: list[Row] = []
    try:
        for source in ordered(root, args.only):
            row = Row(source.stem)
            row.top = first_model(args.golden_root / "vtr" / row.name / f"{row.name}.parmys.blif")
            measure(row, args.odin3, source, args)
            if args.yosys and not row.problems:
                yosys_leg(row, source, args, expect)
            rows.append(row)
            print(table_row(row), flush=True)
            for problem in row.problems:
                print(f"  {row.name}: {problem}", flush=True)
    except memcap.CapUnavailable as exc:
        print(f"flatten-vtr: skipped: {exc}")
        return fc.EXIT_SKIP
    text = table(rows)
    if args.table:
        args.table.write_text(text)
    if args.raw:
        args.raw.write_text(raw_lines(rows))
    bad = [r.name for r in rows if r.problems]
    names = f": {' '.join(bad)}" if bad else ""
    print(f"flatten-vtr: {len(rows)} designs, {len(bad)} failed{names}")
    return fc.EXIT_DIFF if bad else fc.EXIT_SAME


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
