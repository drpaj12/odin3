# graphlets pass: graphlet orbits as a circuit property (design, 2026-10-30)

Status: spec first, code follows on branch `feat/graphlets`. Purpose: Odin III goal 2 (graph and
reverse-engineering research platform); consumers Loki L2/L3 (coarse-grain signature detector) and
the Mimir `graphlet` measurer. Measures: exactness (counts equal to ORCA and to brute force),
runtime, memory. Peter 2026-10-30: build both the undirected and the directed counters in Odin III
as a property, not as an external tool.

## 1. References (read before use)

- Przulj, Bioinformatics 23(2):e177-e183, 2007, doi:10.1093/bioinformatics/btl301: graphlets are
  connected induced subgraphs; automorphism orbits; the graphlet degree vector (GDV) of a node
  counts, per orbit, the graphlets that touch it at that orbit. 2 to 5 nodes: 30 graphlets, 73
  orbits, numbered 0..72 (orbit 0 is the degree).
- Hocevar and Demsar, Bioinformatics 30(4):559-565, 2014, doi:10.1093/bioinformatics/btt717 (ORCA,
  GPL-3.0): used here ONLY as an external binary test oracle. No ORCA source is copied, ported or
  paraphrased; the counter below is complete enumeration (ESU, Wernicke 2006, from the paper's
  description) with table lookup, written independently.
- Sarajlic, Malod-Dognin, Yaveroglu, Przulj, Sci Rep 6:35098, 2016, doi:10.1038/srep35098
  (full text read 2026-10-30, PMC5062067): directed graphlets are induced subgraphs without
  anti-parallel edges; 40 graphlets of 2 to 4 nodes (G0..G39), 129 orbits (0..128); an
  anti-parallel pair is handled "by inducing directed graphlets for each anti-parallel directed
  edge separately" (their Fig. 1c: one pair gives one G21 and one G25); complete enumeration,
  O(n d^(k-1)) time, O(n^2 + n z) space.
- Aparicio, Ribeiro, Silva, IEEE/ACM TCBB 2017, doi:10.1109/TCBB.2016.2586046: NOT read; no number
  from it is used.

## 2. The graph (same rules as plugins/python/odin3_graph.py E-4, collapsed to a simple graph)

Per module (a design with several modules is counted module by module; no flattening here, run
`flatten` first for a whole design).

- Nodes: every live node of the module: cells (including instances and black boxes) and port nodes
  (`$port_in`, `$port_out`, `$port_inout`, one node per port, not per bit). Nodes whose cell type
  has a `const_value` hook (constants) are not nodes, and nets they drive are skipped (E-4: constants
  are not nodes).
- Directed edges: for every net with a driver, for every driver pin (direction OUT or INOUT, or the
  pin of a `$port_in`) and every reading pin (IN or INOUT, or the pin of a `$port_out`) on that net,
  an edge driver node -> reader node. Pins are `odin3_pin_drives` / `odin3_pin_reads`.
- Simple graph: parallel edges (several nets or bus bits between the same two nodes) collapse to
  one. Self-loops (driver node = reader node, e.g. a counter bit feeding itself inside one cell) are
  dropped and counted (`self_loops` in the output). The undirected graph is the union of both
  directions.
- Anti-parallel pairs (u->v and v->u): undirected mode: one undirected edge. Directed mode: see 4.

## 3. Global signals (--exclude-global)

Loki's rule (loki/features/roles.py: clock = a net on a flip-flop `CLK` or hard-block clock pin;
reset/enable = fan-out at least 16 with at least 80% forcing or hold sinks, a LUT truth-table test)
needs LUT tables that Odin III does not have at this level. Odin III's rule, deliberately simpler:
with `--exclude-global`, a net is global, and contributes no edges, when (a) it reaches a clock pin
(a port named `C`, `CLK`, `clk` or `clock`, case-insensitive, of a `SEQ_EDGE` cell) or (b) it has at
least F reading pins (`--global-fanout F`, default 16, Loki's `min_fanout`). Case (b) is a superset
of Loki's reset/enable (no dominance test). The number of global nets is reported. Deviation from
Loki is recorded here; Loki can still classify itself and pass the list through the Python side.

## 4. Modes

- Undirected, `--size 4` (orbits 0..14, 15 orbits, 9 graphlets G0..G8) or `--size 5` (orbits
  0..72, 73 orbits, 30 graphlets). Default 5. Orbit numbering is Przulj's (orbit 0 degree,
  1-2 path3, 3 triangle, 4-14 the 4-node graphlets, 15-72 the 5-node ones). Counts are exact
  int64.
- Directed, `--directed` (sizes 2 to 4 only): 40 graphlets, 129 orbits. The set of node sets is the
  connected node sets of the undirected graph. For a node set holding p anti-parallel pairs, the
  2^p orientations that keep exactly one direction of each pair are each counted as a separate
  graphlet occurrence (Sarajlic's rule, Fig. 1c). A graphlet/orbit numbering of our own is used:
  Fig. 1a is a picture and not machine readable, so graphlets are ordered by (node count, edge
  count, canonical code) and orbits within a graphlet by the first position of the orbit in the
  canonical labelling. The ids are therefore stable and documented in `docs/user/graphlets.md` but
  are NOT claimed equal to Sarajlic's G0..G39 labels. Checked: 1 + 5 + 34 = 40 graphlets,
  2 + 11 + 116 = 129 orbits.
- Typed variant (spec only, later flag `--typed`): node classes FF / LUT-gate / PI / PO / hard; a
  class vector per orbit vector; not built here.
- `--partition KEY`: only nodes carrying attribute KEY take part; nodes with equal attribute text
  form one group; each group is counted on its induced subgraph. Results go to the file only.

## 5. Algorithm

Complete enumeration of connected induced subgraphs with ESU, run once to depth k (so sizes 2..k
come out of one traversal, because ESU visits every connected set of every size exactly once). The
explicit stack has depth at most 5 (no recursion). The adjacency among the chosen nodes is
accumulated incrementally as a code (undirected: one bit per pair, at most 10 bits; directed: two
bits per pair, at most 12 bits); a table built at start (canonical form = minimum code over all
permutations of the positions, orbit of a position = class under the automorphisms) maps the code to
the orbit of each position. Each visited set adds 1 to the (node, orbit) cell of its members.
Cost O(sum over sets of k log d); in a netlist with a high-fan-out net the number of sets explodes
(a star of degree d has d^(k-1) sets), hence section 3 and the limits below. Exactness does not
depend on the order of enumeration.

## 6. Outputs

- Log line like `stats`: `graphlets: M modules, N nodes, E edges (undirected), mode directed|undirected
  size K, G global nets, T s, R MiB peak`.
- Module attributes (attr API, STRING or INT, on the module object): `odin3:glet_mode`
  (`undirected-4`, `undirected-5`, `directed-4`), `odin3:glet_nodes`, `odin3:glet_edges`,
  `odin3:glet_self_loops`, `odin3:glet_global_nets` (INT), `odin3:glet_orbit_totals` (STRING:
  comma list of the per-orbit totals over all nodes, 15, 73 or 129 integers; a total divided by the
  orbit's size in nodes is the graphlet count), `odin3:glet_version` (INT 1). Attributes are replaced
  on a rerun. Per-node vectors are NOT attributes: 73 integers per cell is too big for the attribute
  table; they go in the file.
- JSON file (`--out FILE`, schema `odin3-graphlets`, `version` 1): `{schema, version, mode, size,
  n_orbits, options, modules: {NAME: {nodes, edges, self_loops, global_nets, orbit_totals[],
  gdd: [per orbit: [[degree, node_count], ...]], seconds, nodes_list: [...] }}}`. With `--per-node`
  every node has `{id, name, kind, type, v:[n_orbits ints]}`; `id` is the Odin III node ID of the
  module, `name` the cell name (empty when unnamed). No new IDs: the node ID is the existing key to
  provenance (`odin3_node_prov`, the Python `Node.provenance()` chain to HDL).
- `--edges-out FILE`: the undirected simple graph as `N M` then `u v` lines (0-based, node IDs
  renumbered densely; ORCA input format), for the oracle gate and for external tools. One module
  only (the top, or the only module).

## 7. Cost limits

`--max-nodes N` (default 200000): a module above N nodes is refused with the count unless
`--force`. `--max-work W` (default 20e9 visited sets): counting stops with an error that suggests
`--exclude-global`, and nothing is written. Memory: orbit matrix n x n_orbits x 8 bytes (73 orbits
and 200000 nodes = 117 MB) plus the CSR graph; the budget of `odin3_budget_check` is applied to this
working memory. Time and peak RSS are reported in the log line and the file.

## 8. API

- Pass: `graphlets [--directed] [--size 4|5] [--exclude-global] [--global-fanout F] [--out FILE]
  [--per-node] [--partition KEY] [--edges-out FILE] [--max-nodes N] [--force] [--max-work W]`.
  `--size` applies to undirected mode only. Views: any (structure only; the design is changed only
  by module attributes).
- Python (`plugins/python/odin3.py`): `Design.graphlets(directed=False, size=5, exclude_global=False,
  per_node=False, partition=None, global_fanout=16) -> dict` runs the pass with `--out` in a
  temporary file and returns the parsed JSON; `Module.attr("odin3:glet_orbit_totals")` reads the
  summary without the file.

## 9. Tests (the exactness gate)

1. Unity: tables (15 / 73 undirected orbits and 9 / 30 graphlets; 40 / 129 directed); brute force
   on random small graphs (n <= 9 undirected, enumerate all k-subsets, test connectivity, classify
   with an independent canonicalisation by exhaustive permutation) equals the ESU counts for every
   node and orbit; hand-worked example per directed family (single edge, out-star, in-star, chain,
   cyclic triangle, transitive triangle, 4-node examples, an anti-parallel pair).
2. Oracle gate (script, not in CI unless the binary exists): `--edges-out` graph of every module
   graph of tests/micro goldens and the 14 small VTR designs fed to the ORCA binary (4 and 5);
   per-node vectors must be identical.
3. Runtime and memory table versus ORCA by graph size.

## 10. Provenance and IDs

Every orbit vector is keyed by the module-local node ID plus the name, so a consumer reaches the
provenance chain and the HDL location of the cell with the existing API. The pass writes no
provenance records and creates no IDs.

## 11. Next step (resume note)

Written first so a weekly stop loses nothing: (1) tables + counters + tests, (2) pass + CLI + JSON,
(3) oracle gate and timing table, (4) Python method, docs, PASSES.md row, lint, PR. Mimir measurer
only after merge.
