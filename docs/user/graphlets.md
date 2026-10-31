# Graphlets

Graphlet orbit counts are a structural property of a netlist: for every node, how many small
connected subgraphs it sits in, and in which position. The `graphlets` pass counts them exactly,
for the whole module or for a subgraph (a cone, a hierarchy path, a partition), undirected (2 to 5
nodes, 73 orbits) or directed (2 to 4 nodes, 129 orbits). The counter is generic (it works on any
edge list, also from Python) and written from the papers: Przulj 2007 (orbits and graphlet degree
vectors, doi:10.1093/bioinformatics/btl301) and Sarajlic et al. 2016 (directed graphlets,
doi:10.1038/srep35098). The ORCA program of Hocevar and Demsar 2014 is used only as an external
test oracle; none of its code is in Odin III.

## The graph

One node per cell and one per module port; an edge from the node that drives a net to every node
that reads it. Constants are not nodes. Parallel edges, bus bits and a net with several readers
collapse to a simple graph; a node that feeds itself (self-loop) is dropped and counted. Undirected
mode merges both directions of an edge. `--exclude-global` leaves out the nets that reach a clock
pin or have at least `--global-fanout` readers (default 16): they would otherwise make the number of
subgraphs explode (a net of fan-out d adds about d^(k-1) subgraphs of k nodes).

## Running it

```text
odin3 -p "read_techlib lib/vtr.o3lib; read_blif ff.blif; graphlets --exclude-global --out ff.json"
odin3: info: graphlets: 1 groups, 5 nodes, 2 edges, mode undirected-5, 0 global nets, 31 ms, 77 MiB peak
```

| Option | Meaning |
| --- | --- |
| `--directed` | 2 to 4 node directed graphlets, 129 orbits (default: undirected). |
| `--size N` | N is 4 or 5: largest undirected graphlet (15 or 73 orbits, default 5). |
| `--exclude-global`, `--global-fanout F` | Leave out clock and high-fan-out nets. |
| `--focus F` (repeatable) | Count the subgraph a write_dot focus selects: `cone(NET)`, `cone(MODULE:NET)`, `module/inst/...`, `file:line`. |
| `--cells FILE` | A partition file: cell IDs or names one per line, `@NAME [MODULE]` starts a group, `#` comments. |
| `--partition KEY` | One group per distinct text of the node attribute KEY. |
| `--halo H` | H is 0 (default): count inside the group only, so two instances of a pattern give the same signature whatever surrounds them. H is 1: also count graphlets that touch the group through its one-hop neighbours (a pattern cut by the boundary); reported for the group's own nodes. |
| `--per-node` | Write every node's orbit vector (id, name, type) into the file. |
| `--out FILE`, `--edges-out FILE` | The JSON result (schema `odin3-graphlets`, version 1); the directed graph as `N M` plus `u v` lines. |
| `--max-nodes N`, `--force`, `--max-work W` | Refuse a group above N nodes (default 200000) unless forced; stop after W counted subgraphs (default 2e10). |
| `--compare "A#NAME" "B#NAME"`, `--calibration C` | Compare two signature files (below); `#NAME` picks a group by name (quote the word: an unquoted `#` starts a script comment). |

Whole-module runs store module attributes `odin3:glet_mode`, `odin3:glet_nodes`,
`odin3:glet_edges`, `odin3:glet_self_loops`, `odin3:glet_global_nets`, `odin3:glet_version` and
`odin3:glet_orbit_totals` (comma-separated totals per orbit; divide by the orbit's size for a graphlet
count). The per-node vectors are only in the file: 73 integers per cell do not fit the attribute
table. Every vector is keyed by the module-local node ID and name, the key to the provenance chain
(`odin3_node_prov`, `Node.provenance()`), so an orbit count leads back to the HDL.

## Orbit numbering

Undirected orbits use Przulj's numbering: 0 is the degree, 1 and 2 the ends and the centre of a
path of three, 3 the triangle, 4 to 14 the four-node graphlets, 15 to 72 the five-node ones (checked
against ORCA). The 40 directed graphlets and 129 orbits are numbered by node count, edge count and
canonical code; this is stable but is not claimed equal to the labels of Sarajlic's Figure 1. An
anti-parallel pair (u to v and v to u) is counted once per direction (their Figure 1c). The catalogue
(`glet_count --directed --catalogue`) lists orbit, graphlet, nodes, edges, orbit size, canonical code
and a representative position.

## Comparing signatures

A signature is the orbit totals plus the graphlet degree distribution (GDD). `--compare` gives two
similarities: the GDD agreement of Przulj 2007 (size independent, in [0,1], the standard measure) and
the cosine of log(1 + orbit totals) (sees how much of each graphlet there is). With a calibration
file (`odin3-graphlet-calibration`, integers only: per mode and metric a binned count of labelled
same and different pairs, and the prior counts) it also reports P(same | similarity) by Bayes' rule
with Laplace-smoothed binned likelihoods, per metric and combined (naive Bayes, which assumes the
metrics are independent: they are not, so read it next to the single-metric values). Without a file
the result says "uncalibrated".

## Python

```python
import odin3
counts = odin3.graphlet_counts(4, [(0, 1), (1, 2), (2, 3)], size=4)   # any edge list
with odin3.Design.read_blif("ff.blif") as design:
    result = design.graphlets(exclude_global=True, per_node=True)       # the JSON, as a dict
    print(design.top().attr("odin3:glet_orbit_totals"))
print(odin3.graphlet_compare("a.json", "b.json#cone"))                  # similarity (+ posterior)
```

## How it was checked

Undirected counts are identical to the ORCA oracle (4 and 5 nodes) on 473 netlists, 946 runs, with
no difference: every micro netlist of the `k6_frac_N10_frac_chain_mem32K_40nm` golden set and the 14
small VTR designs (`tools/graphlets/oracle_gate.py`), on random graphs (80 runs), and against a brute
force on random graphs in the unit tests (`test_graphlets_count`).

The directed counter has no oracle. It is checked five ways (`tools/graphlets/directed_checks.py`,
`test_graphlets_count`, `tests/tools/test_python_graphlets.py`): an independent plain-Python brute
force (every connected 2 to 4 node set, every anti-parallel resolution, canonical form and orbits by
exhaustive permutation) equals the fast counter; a C brute force equals it on 600 random digraphs;
the catalogue has exactly 40 graphlets and 129 orbits and every connected oriented graph of up to
four nodes is isomorphic to exactly one of them; dropping the directions maps the directed counts
onto the undirected ones (and ORCA) for graphs without anti-parallel pairs; relabelling nodes leaves
the orbit vectors unchanged and reversing every arc maps orbit o to its reverse orbit with equal
counts.

## Directed catalogue

Canonical labels, one node of each orbit, and the orbit size. Orbit ids and graphlet numbers are
Odin III's own.

| Graphlet | Nodes | Arcs (canonical labels 0..n-1) | Orbits (id: size, a node of the orbit) |
| --- | --- | --- | --- |
| G0 | 2 | 0>1 | 0: 1, node 0, 1: 1, node 1 |
| G1 | 3 | 0>1 0>2 | 2: 1, node 0, 3: 2, node 1 |
| G2 | 3 | 1>0 0>2 | 4: 1, node 0, 5: 1, node 1, 6: 1, node 2 |
| G3 | 3 | 1>0 2>0 | 7: 1, node 0, 8: 2, node 1 |
| G4 | 3 | 0>1 0>2 1>2 | 9: 1, node 0, 10: 1, node 1, 11: 1, node 2 |
| G5 | 3 | 0>1 2>0 1>2 | 12: 3, node 0 |
| G6 | 4 | 0>1 0>2 0>3 | 13: 1, node 0, 14: 3, node 1 |
| G7 | 4 | 1>0 0>2 0>3 | 15: 1, node 0, 16: 1, node 1, 17: 2, node 2 |
| G8 | 4 | 1>0 2>0 0>3 | 18: 1, node 0, 19: 2, node 1, 20: 1, node 3 |
| G9 | 4 | 0>1 1>2 0>3 | 21: 1, node 0, 22: 1, node 1, 23: 1, node 2, 24: 1, node 3 |
| G10 | 4 | 0>2 1>2 0>3 | 25: 1, node 0, 26: 1, node 1, 27: 1, node 2, 28: 1, node 3 |
| G11 | 4 | 2>0 1>2 0>3 | 29: 1, node 0, 30: 1, node 1, 31: 1, node 2, 32: 1, node 3 |
| G12 | 4 | 1>0 2>0 3>0 | 33: 1, node 0, 34: 3, node 1 |
| G13 | 4 | 0>2 1>2 3>0 | 35: 1, node 0, 36: 1, node 1, 37: 1, node 2, 38: 1, node 3 |
| G14 | 4 | 0>1 0>2 1>2 0>3 | 39: 1, node 0, 40: 1, node 1, 41: 1, node 2, 42: 1, node 3 |
| G15 | 4 | 1>0 0>2 1>2 0>3 | 43: 1, node 0, 44: 1, node 1, 45: 1, node 2, 46: 1, node 3 |
| G16 | 4 | 0>1 2>0 1>2 0>3 | 47: 1, node 0, 48: 1, node 1, 49: 1, node 2, 50: 1, node 3 |
| G17 | 4 | 1>0 2>0 1>2 0>3 | 51: 1, node 0, 52: 1, node 1, 53: 1, node 2, 54: 1, node 3 |
| G18 | 4 | 0>1 0>2 1>2 3>0 | 55: 1, node 0, 56: 1, node 1, 57: 1, node 2, 58: 1, node 3 |
| G19 | 4 | 1>0 0>2 1>2 3>0 | 59: 1, node 0, 60: 1, node 1, 61: 1, node 2, 62: 1, node 3 |
| G20 | 4 | 0>1 2>0 1>2 3>0 | 63: 1, node 0, 64: 1, node 1, 65: 1, node 2, 66: 1, node 3 |
| G21 | 4 | 1>0 2>0 1>2 3>0 | 67: 1, node 0, 68: 1, node 1, 69: 1, node 2, 70: 1, node 3 |
| G22 | 4 | 0>2 1>2 0>3 1>3 | 71: 2, node 0, 72: 2, node 2 |
| G23 | 4 | 2>0 1>2 0>3 1>3 | 73: 1, node 0, 74: 1, node 1, 75: 1, node 2, 76: 1, node 3 |
| G24 | 4 | 2>0 2>1 0>3 1>3 | 77: 2, node 0, 78: 1, node 2, 79: 1, node 3 |
| G25 | 4 | 0>2 2>1 3>0 1>3 | 80: 4, node 0 |
| G26 | 4 | 0>1 0>2 1>2 0>3 1>3 | 81: 1, node 0, 82: 1, node 1, 83: 2, node 2 |
| G27 | 4 | 0>1 2>0 1>2 0>3 1>3 | 84: 1, node 0, 85: 1, node 1, 86: 1, node 2, 87: 1, node 3 |
| G28 | 4 | 1>0 2>0 1>2 0>3 1>3 | 88: 1, node 0, 89: 1, node 1, 90: 1, node 2, 91: 1, node 3 |
| G29 | 4 | 0>1 2>0 2>1 0>3 1>3 | 92: 1, node 0, 93: 1, node 1, 94: 1, node 2, 95: 1, node 3 |
| G30 | 4 | 0>1 2>0 1>2 3>0 1>3 | 96: 1, node 0, 97: 1, node 1, 98: 2, node 2 |
| G31 | 4 | 1>0 2>0 1>2 3>0 1>3 | 99: 1, node 0, 100: 1, node 1, 101: 2, node 2 |
| G32 | 4 | 0>1 0>2 2>1 3>0 1>3 | 102: 1, node 0, 103: 1, node 1, 104: 1, node 2, 105: 1, node 3 |
| G33 | 4 | 0>1 2>0 2>1 3>0 1>3 | 106: 1, node 0, 107: 1, node 1, 108: 1, node 2, 109: 1, node 3 |
| G34 | 4 | 1>0 2>0 2>1 3>0 1>3 | 110: 1, node 0, 111: 1, node 1, 112: 1, node 2, 113: 1, node 3 |
| G35 | 4 | 0>1 2>0 2>1 3>0 3>1 | 114: 1, node 0, 115: 1, node 1, 116: 2, node 2 |
| G36 | 4 | 0>1 0>2 1>2 0>3 1>3 2>3 | 117: 1, node 0, 118: 1, node 1, 119: 1, node 2, 120: 1, node 3 |
| G37 | 4 | 0>1 2>0 1>2 0>3 1>3 2>3 | 121: 3, node 0, 122: 1, node 3 |
| G38 | 4 | 0>1 0>2 1>2 3>0 1>3 2>3 | 123: 1, node 0, 124: 1, node 1, 125: 1, node 2, 126: 1, node 3 |
| G39 | 4 | 1>0 0>2 1>2 3>0 1>3 2>3 | 127: 3, node 0, 128: 1, node 1 |
