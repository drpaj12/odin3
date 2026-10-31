#!/usr/bin/env python3
"""Graph-construction gate of the graphlets pass (spec docs/specs/2026-10-30-graphlets-design.md,
as-built note on the `$sop`/`$lut` fix).

The ORCA gate (oracle_gate.py) compares counts on the graph the pass itself exports, so it tests the
counting, not the construction. This gate tests the construction: the graph the pass counts
(`graphlets --out` for nodes, edges and self-loops, `--edges-out` for the edge list) must equal an
independent build of the E-4 graph (plugins/python/odin3_graph.py) from the same design written by
`write_json`, collapsed to a simple graph (self-loops dropped, parallel edges merged, direction kept
for the directed count). The global-signal exclusion is off on both sides.

Usage: construction_gate.py --odin3 ODIN3 --lib vtr.o3lib --out DIR [--form gates|lut|both]
                            [--gates-lib FILE.o3lib] BLIF...
Forms: `lut` is the BLIF as read (`.names` become `$sop`); `gates` also reads the gate library
(default lib/cmos6.o3lib) and runs `abc --gates` after the read, so the combinational logic is gate
cells. Compared per design and form: the number of nodes, the undirected simple edges, the
self-loops, the directed distinct edges and the sorted degree sequence.
Exit 0 when every design that odin3 could
read agrees (and at least one did).
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
from pathlib import Path
from types import ModuleType
from typing import NamedTuple

REPO_ROOT = Path(__file__).resolve().parents[2]
# The counting is not under test here (oracle_gate.py does that): size 4 keeps big designs fast.
COUNT_SIZE = 4
DEFAULT_GATES = REPO_ROOT / "lib" / "cmos6.o3lib"


class Shape(NamedTuple):
    """The facts compared: counts and the sorted undirected degree sequence."""

    nodes: int
    edges: int
    self_loops: int
    directed: int
    degrees: tuple[int, ...]


def load_graph_module() -> ModuleType:
    path = REPO_ROOT / "plugins" / "python" / "odin3_graph.py"
    spec = importlib.util.spec_from_file_location("odin3_graph", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["odin3_graph"] = module
    spec.loader.exec_module(module)
    return module


def shape_of_pairs(n: int, pairs: list[tuple[int, int]]) -> Shape:
    """Collapse a directed pair list over n nodes to a simple graph."""
    directed = {pr for pr in pairs}
    loops = sum(1 for a, b in directed if a == b)
    und = {(min(a, b), max(a, b)) for a, b in directed if a != b}
    deg = [0] * n
    for a, b in und:
        deg[a] += 1
        deg[b] += 1
    return Shape(n, len(und), loops, len(directed) - loops, tuple(sorted(deg)))


def reference_shape(graph: ModuleType, json_path: Path, module: str) -> Shape:
    """The E-4 graph of one module of the written JSON, as a simple graph."""
    nodes, edges = graph.parse_graph(json_path, module)
    index = {node.ident: i for i, node in enumerate(nodes)}
    return shape_of_pairs(len(nodes), [(index[e.source], index[e.target]) for e in edges])


def exported_shape(path: Path) -> Shape:
    """The pass's counted graph from --edges-out (a header `n m`, then one `from to` per line)."""
    lines = path.read_text().split("\n")
    n = int(lines[0].split()[0])
    pairs = [(int(w[0]), int(w[1])) for w in (ln.split() for ln in lines[1:]) if len(w) == 2]
    return shape_of_pairs(n, pairs)


def run_script(
    odin3: str,
    lib: str,
    blif: Path,
    form: str,
    gates: Path,
    json_out: Path,
    glet: Path,
    edges: Path,
) -> str | None:
    """Run the pipeline; None on success, else the reason odin3 could not do it."""
    steps = [f"read_techlib {lib}"]
    if form == "gates":
        steps.append(f"read_techlib {gates}")
    steps.append(f"read_blif {blif}")
    if form == "gates":
        steps.append(f"abc --gates {gates.stem}")
    steps += [
        f"write_json {json_out}",
        f"graphlets --size {COUNT_SIZE} --out {glet} --edges-out {edges}",
    ]
    done = subprocess.run([odin3, "-p", "; ".join(steps)], capture_output=True, text=True)
    if done.returncode != 0 or not edges.exists():
        lines = (done.stderr or done.stdout).strip().splitlines()
        return lines[-1] if lines else "odin3 failed"
    return None


def check_one(
    graph: ModuleType, odin3: str, lib: str, blif: Path, form: str, gates: Path, out: Path
) -> tuple[str, Shape | None, Shape | None, str]:
    """(status, pass shape, reference shape, note): status is ok, DIFFER or skipped."""
    stem = f"{blif.name.removesuffix('.blif')}.{form}"
    jpath, gpath, epath = out / f"{stem}.json", out / f"{stem}.glet.json", out / f"{stem}.edges"
    why = run_script(odin3, lib, blif, form, gates, jpath, gpath, epath)
    if why is not None:
        return "skipped", None, None, why
    group = json.loads(gpath.read_text())["groups"][0]
    # the edge file has no self-loops (counted apart): take that count from --out
    mine = exported_shape(epath)._replace(self_loops=group["self_loops"])
    if group["nodes"] != mine.nodes or group["edges"] != mine.edges:
        return "DIFFER", mine, None, f"--out {group['nodes']},{group['edges']} vs --edges-out"
    ref = reference_shape(graph, jpath, group["module"])
    status = "ok" if mine == ref else "DIFFER"
    return status, mine, ref, ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--form", choices=("gates", "lut", "both"), default="both")
    ap.add_argument("--gates-lib", type=Path, default=DEFAULT_GATES)
    ap.add_argument("blifs", nargs="+")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    graph = load_graph_module()
    forms = ["lut", "gates"] if args.form == "both" else [args.form]
    blifs = sorted((Path(b) for b in args.blifs), key=lambda p: p.stat().st_size)  # smallest first
    counts = {"ok": 0, "DIFFER": 0, "skipped": 0}
    print("design,form,nodes,edges,self_loops,directed,status,note")
    for blif in blifs:
        for form in forms:
            status, mine, ref, note = check_one(
                graph, args.odin3, args.lib, blif, form, args.gates_lib, out
            )
            counts[status] += 1
            shape = mine or ref
            row = shape[:4] if shape else ("", "", "", "")
            print(
                f"{blif.name.removesuffix('.blif')},{form},{','.join(map(str, row))},"
                f"{status},{note}"
            )
            if status == "DIFFER" and ref is not None:
                print(f"  reference {ref[:4]}", file=sys.stderr)
    print(
        f"construction gate: {counts['ok']} ok, {counts['DIFFER']} differ, "
        f"{counts['skipped']} skipped",
        file=sys.stderr,
    )
    return 0 if counts["DIFFER"] == 0 and counts["ok"] > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
