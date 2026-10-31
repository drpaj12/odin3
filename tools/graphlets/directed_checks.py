#!/usr/bin/env python3
"""Verification of the directed graphlet counter (spec docs/specs/2026-10-30-graphlets-design.md
section 17), with no oracle for directed graphlets. Plain itertools, independent of the C code.

Usage: directed_checks.py GLET_COUNT [--orca ORCA] [--edges FILE ...] [--random N]
Each check prints "name: passed/total". Exit status 1 when any check fails.
"""

from __future__ import annotations

import argparse
import itertools
import random
import subprocess
import sys
import tempfile
from pathlib import Path

Matrix = tuple[tuple[int, ...], ...]


def run_catalogue(tool: str, directed: bool) -> list[tuple[int, int, int, int, int, int, int]]:
    args = [tool] + (["--directed"] if directed else []) + ["--catalogue"]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    return [tuple(int(x) for x in line.split()) for line in out.splitlines() if line.strip()]  # type: ignore[misc]


def code_matrix(nodes: int, code: int, directed: bool) -> Matrix:
    m = [[0] * nodes for _ in range(nodes)]
    for j in range(1, nodes):
        for i in range(j):
            p = j * (j - 1) // 2 + i
            if directed:
                st = (code >> (2 * p)) & 3
                m[i][j] = st & 1
                m[j][i] = (st >> 1) & 1
            else:
                bit = (code >> p) & 1
                m[i][j] = m[j][i] = bit
    return tuple(tuple(r) for r in m)


def canon(mat: Matrix) -> tuple[tuple[int, ...], tuple[int, ...]]:
    """(canonical flat key, perm giving it): position q of the canonical form is node perm[q]."""
    n = len(mat)
    best = None
    bestp: tuple[int, ...] = ()
    for perm in itertools.permutations(range(n)):
        key = tuple(mat[perm[i]][perm[j]] for i in range(n) for j in range(n))
        if best is None or key < best:
            best, bestp = key, perm
    assert best is not None
    return best, bestp


def aut_class(key: tuple[int, ...], n: int, pos: int) -> int:
    """Smallest image of canonical position pos under the automorphisms of the canonical form."""
    rep = pos
    for perm in itertools.permutations(range(n)):
        if all(key[perm[i] * n + perm[j]] == key[i * n + j] for i in range(n) for j in range(n)):
            rep = min(rep, perm[pos])
    return rep


def orbit_key(mat: Matrix, node: int) -> tuple[tuple[int, ...], int]:
    key, perm = canon(mat)
    pos = perm.index(node)
    return key, aut_class(key, len(mat), pos)


class Catalogue:
    def __init__(self, tool: str, directed: bool) -> None:
        self.directed = directed
        self.rows = run_catalogue(tool, directed)
        self.by_key: dict[tuple[tuple[int, ...], int], int] = {}
        self.info: dict[
            int, tuple[int, int, int, int]
        ] = {}  # orbit -> nodes, graphlet, mult, edges
        for orbit, graphlet, nodes, edges, mult, code, pos in self.rows:
            mat = code_matrix(nodes, code, directed)
            key = orbit_key(mat, pos)
            assert key not in self.by_key, f"orbit {orbit} duplicates another"
            self.by_key[(key[0], key[1])] = orbit
            self.info[orbit] = (nodes, graphlet, mult, edges)
        self.mats = {r[0]: code_matrix(r[2], r[5], directed) for r in self.rows}
        self.pos = {r[0]: r[6] for r in self.rows}


def components_ok(n: int, adj: list[set[int]], subset: tuple[int, ...]) -> bool:
    seen = {subset[0]}
    todo = [subset[0]]
    members = set(subset)
    while todo:
        u = todo.pop()
        for v in adj[u] & members:
            if v not in seen:
                seen.add(v)
                todo.append(v)
    return len(seen) == len(subset)


def connected_sets(adj: list[set[int]], maxk: int, limit: int) -> list[tuple[int, ...]] | None:
    """Every connected node set of 2..maxk nodes (sorted tuples), grown one neighbour at a time;
    None when more than limit sets exist."""
    level: set[tuple[int, ...]] = {(v,) for v in range(len(adj))}
    found: list[tuple[int, ...]] = []
    for _ in range(2, maxk + 1):
        nxt: set[tuple[int, ...]] = set()
        for subset in level:
            members = set(subset)
            for u in subset:
                for w in adj[u] - members:
                    nxt.add(tuple(sorted(subset + (w,))))
        found.extend(sorted(nxt))
        if len(found) > limit:
            return None
        level = nxt
    return found


def reference(
    n: int, edges: set[tuple[int, int]], cat: Catalogue, maxk: int = 4, limit: int = 30000
) -> list[list[int]] | None:
    """Independent brute force: every connected 2..maxk subset, every anti-parallel resolution.
    None when the graph has more than limit connected sets (too slow for plain Python)."""
    adj: list[set[int]] = [set() for _ in range(n)]
    for u, v in edges:
        adj[u].add(v)
        adj[v].add(u)
    sets = connected_sets(adj, maxk, limit)
    if sets is None:
        return None
    counts = [[0] * (max(cat.info) + 1) for _ in range(n)]
    for subset in sets:
        size = len(subset)
        both = [
            (a, b)
            for a, b in itertools.combinations(range(size), 2)
            if (subset[a], subset[b]) in edges and (subset[b], subset[a]) in edges
        ]
        for mask in range(1 << len(both)):
            m = [
                [1 if (subset[a], subset[b]) in edges else 0 for b in range(size)]
                for a in range(size)
            ]
            for k, (a, b) in enumerate(both):
                if (mask >> k) & 1:
                    m[a][b] = 0
                else:
                    m[b][a] = 0
            mat = tuple(tuple(r) for r in m)
            for node in range(size):
                counts[subset[node]][cat.by_key[orbit_key(mat, node)]] += 1
    return counts


def write_edges(path: Path, n: int, edges: set[tuple[int, int]]) -> None:
    path.write_text(f"{n} {len(edges)}\n" + "".join(f"{u} {v}\n" for u, v in sorted(edges)))


def read_edges(path: Path) -> tuple[int, set[tuple[int, int]]]:
    lines = path.read_text().split("\n")
    n = int(lines[0].split()[0])
    return n, {(int(a), int(b)) for a, b in (ln.split() for ln in lines[1:] if ln.strip())}


def fast(
    tool: str, n: int, edges: set[tuple[int, int]], directed: bool, size: int
) -> list[list[int]]:
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = Path(tmp) / "g.txt", Path(tmp) / "o.txt"
        write_edges(src, n, edges)
        subprocess.run(
            [tool] + (["--directed"] if directed else []) + [str(size), str(src), str(dst)],
            check=True,
        )
        return [[int(x) for x in ln.split()] for ln in dst.read_text().splitlines()]


def random_digraph(rng: random.Random, antiparallel: bool) -> tuple[int, set[tuple[int, int]]]:
    n = rng.randint(4, 8)
    dens = rng.choice([0.1, 0.2, 0.35, 0.5])
    edges: set[tuple[int, int]] = set()
    for u in range(n):
        for v in range(n):
            if u != v and rng.random() < dens and (antiparallel or (v, u) not in edges):
                edges.add((u, v))
    return n, edges


class Tally:
    def __init__(self) -> None:
        self.res: dict[str, list[int]] = {}

    def add(self, name: str, ok: bool) -> None:
        r = self.res.setdefault(name, [0, 0])
        r[0] += int(ok)
        r[1] += 1

    def report(self) -> bool:
        good = True
        for name, (p, t) in self.res.items():
            print(f"{name}: {p}/{t}")
            good &= p == t
        return good


def underlying(mat: Matrix) -> Matrix:
    n = len(mat)
    return tuple(tuple(1 if (mat[i][j] or mat[j][i]) else 0 for j in range(n)) for i in range(n))


def reverse(mat: Matrix) -> Matrix:
    n = len(mat)
    return tuple(tuple(mat[j][i] for j in range(n)) for i in range(n))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tool")
    ap.add_argument("--orca")
    ap.add_argument("--edges", nargs="*", default=[])
    ap.add_argument("--edges-dir", help="directory of *.dir.txt edge files (graphlets --edges-out)")
    ap.add_argument("--random", type=int, default=300)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    dcat = Catalogue(args.tool, True)
    ucat = Catalogue(args.tool, False)
    t = Tally()
    # 3. catalogue
    graphlets = {v[1] for v in dcat.info.values()}
    t.add("catalogue: 40 directed graphlets", len(graphlets) == 40)
    t.add("catalogue: 129 directed orbits", len(dcat.info) == 129)
    t.add(
        "catalogue: orbit mult sums to graphlet size",
        all(
            sum(v[2] for o, v in dcat.info.items() if v[1] == g)
            == dcat.info[min(o for o, w in dcat.info.items() if w[1] == g)][0]
            for g in graphlets
        ),
    )
    t.add("catalogue: undirected 73 orbits", len(ucat.info) == 73)
    # maps from the catalogue, built here independently
    proj = {o: ucat.by_key[orbit_key(underlying(dcat.mats[o]), dcat.pos[o])] for o in dcat.info}
    rev = {o: dcat.by_key[orbit_key(reverse(dcat.mats[o]), dcat.pos[o])] for o in dcat.info}
    t.add("catalogue: reverse map is an involution", all(rev[rev[o]] == o for o in rev))
    graphs: list[tuple[int, set[tuple[int, int]], bool]] = []
    rng = random.Random(args.seed)
    for i in range(args.random):
        n, e = random_digraph(rng, antiparallel=i % 2 == 0)
        graphs.append((n, e, i % 2 == 0))
    files = list(args.edges) + (
        [
            str(p)
            for p in sorted(Path(args.edges_dir).glob("*.dir.txt"), key=lambda p: p.stat().st_size)
        ]
        if args.edges_dir
        else []
    )
    for f in files:
        n, e = read_edges(Path(f))
        graphs.append((n, e, any((v, u) in e for u, v in e)))
    for n, e, anti in graphs:
        got = fast(args.tool, n, e, True, 4)
        ref = reference(n, e, dcat)  # 1/2. independent reference
        if ref is None:
            t.add("independent python reference skipped (too many sets)", True)
        else:
            t.add("independent python reference == fast counter", ref == got)
        # 5. invariances
        perm = list(range(n))
        rng.shuffle(perm)
        got_p = fast(args.tool, n, {(perm[u], perm[v]) for u, v in e}, True, 4)
        t.add(
            "relabelling keeps the orbit vectors", all(got_p[perm[v]] == got[v] for v in range(n))
        )
        got_r = fast(args.tool, n, {(v, u) for u, v in e}, True, 4)
        t.add(
            "reversal maps orbit o to rev(o)",
            all(got_r[v][rev[o]] == got[v][o] for v in range(n) for o in range(129)),
        )
        # 4. projection to the undirected counter (and ORCA)
        und = fast(args.tool, n, e, False, 4)
        if not anti:
            summed = [[0] * 15 for _ in range(n)]
            for v in range(n):
                for o in range(129):
                    if dcat.info[o][0] <= 4:
                        summed[v][proj[o]] += got[v][o]
            t.add("projection of directed == undirected counter (no anti-parallel)", summed == und)
            if args.orca:
                und_set = {(min(u, v), max(u, v)) for u, v in e}
                with tempfile.TemporaryDirectory() as tmp:
                    src, dst = Path(tmp) / "g.txt", Path(tmp) / "o.txt"
                    write_edges(src, n, und_set)
                    subprocess.run(
                        [args.orca, "node", "4", str(src), str(dst)],
                        check=True,
                        capture_output=True,
                    )
                    orca = [[int(x) for x in ln.split()] for ln in dst.read_text().splitlines()]
                t.add("projection of directed == ORCA (4 nodes)", summed == orca)
    return 0 if t.report() else 1


if __name__ == "__main__":
    sys.exit(main())
