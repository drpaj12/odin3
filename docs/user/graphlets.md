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
