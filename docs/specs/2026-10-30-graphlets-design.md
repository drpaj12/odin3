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
  (`$port_in`, `$port_out`, `$port_inout`, one node per port, not per bit). Constant drivers are
  not nodes, and nets they drive are skipped (E-4: constants are not nodes). A constant driver is a
  cell with at least one pin, no reading pin and every pin on a net whose `odin3_net_const_value` is
  0, 1, x or z (`$_CONST0_`, `$_CONST1_`, a zero-input `$sop` or `$lut`); this is the test the JSON
  writer uses to leave such a cell out. A live `$sop`, `$lut` or gate is a node.
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

## 11. Next step (resume note, updated at the end of the file)

Written first so a weekly stop loses nothing: (1) tables + counters + tests, (2) pass + CLI + JSON,
(3) oracle gate and timing table, (4) Python method, docs, PASSES.md row, lint, PR. Mimir measurer
only after merge.

## 12. Subgraph input (Peter 2026-10-30, scope addition)

Peter, verbatim: "This should be possible as a subgraph input such as a cone or some partition as
this tells us if the signatures have similarity suggesting (Bayesian condideance) that they are the
same".

The unit of work is a group: a set of nodes of one module, counted on the subgraph induced by it.
Sources of groups (several may be combined; each gives one or more groups):

- none of the options below: one group per module (the whole module);
- `--focus F` (repeatable): the syntax and the resolver are write_dot's `--focus` (the code is
  shared: `odin3_dot_select` in src/backends/dot/writer.c, same parser as `write_dot --focus`):
  `cone(<net>)` / `cone(<module>:<net>)` (the fan-in cone, stops at instance nodes), a hierarchical
  path `module/inst/...` (the module's subtree, one group per module the subtree reaches), or
  `<file>:<line>` (provenance forward index). One group per module with a selected node, named by F.
- `--cells FILE`: a partition file. Lines are node IDs (decimal) or node names; `#` starts a comment;
  a line `@NAME [MODULE]` starts a group (MODULE defaults to the top module, or the only module).
  Lines before the first header form the group `default`. Unknown names are an error.
- `--partition KEY`: groups by the text of node attribute KEY (section 4).

`--halo 0|1` (default 0). Halo 0: orbits are counted inside the induced subgraph only. Halo 1: the
subgraph is the group plus its one-hop neighbours (the halo), orbits are counted on that larger
induced subgraph but reported (vectors, totals, GDD) for the group's own nodes only, so a graphlet
cut by the boundary still counts at the group members it touches. Default is 0 because the question
is whether two subgraphs have the same pattern: halo counts depend on the surroundings, which differ
between two instances of one pattern and would lower the similarity of true matches. Halo 1 is for
looking for a pattern cut by a partition boundary. Both modes work undirected and directed. Graphlets
that need a node more than one hop away from the group are never counted.

## 13. Signature comparison

A signature is the pair (orbit totals over the group's reported nodes, GDD per orbit). Two metrics,
computed on two signatures of the same mode:

1. GDD agreement (Przulj 2007, doi:10.1093/bioinformatics/btl301; restated in Sarajlic 2016 for the
   directed case, read there): for orbit j, d_j(k) is the number of nodes that touch orbit j exactly
   k times (k >= 1), scaled S_j(k) = d_j(k)/k, normalised N_j(k) = S_j(k)/T_j with T_j = sum_k S_j(k),
   distance D_j = (1/sqrt 2) sqrt(sum_k (N_j^A(k) - N_j^B(k))^2), agreement A_j = 1 - D_j in [0, 1];
   the GDD agreement is the arithmetic mean of A_j over all orbits (Sarajlic restates this). Note: the
   Bioinformatics 2007 text is not open access and was not read here; the per-orbit distance formula
   above is the published definition as known from the literature and is checked only by its
   properties (identical signatures give 1, disjoint support gives 0). Orbit with T_j = 0 on both
   sides: A_j = 1; on one side only: A_j = 0. Why: it is the standard, published measure of
   network similarity from graphlets, size-independent, and comparable with the literature.
2. Cosine similarity of the vectors log(1 + orbit total) over orbits. Why: GDD agreement looks only
   at per-node degree distributions and ignores how many subgraphs there are; the log-total cosine
   sees the overall graphlet mix, is cheap and bounded in [0, 1] (counts are non-negative), and the
   log damps the dominance of the big orbits. Zero vectors on both sides give 1, on one side 0.

`graphlets --compare A.json B.json [--calibration C.json] [--out R.json]` reads two signature files
written by this pass (suffix `#NAME` picks a group by name; default the first group) and reports both
metrics. In a run with two or more groups the same comparison is done for all pairs (at most 32
groups) and written to `compare` in the output file. Modes must match (else an error). The reader
is the repository's JSON parser, which takes integers only, so signature files hold integers only;
results written for readers may hold decimals.

## 14. Bayesian confidence

P(same | s) = L(s | same) P(same) / ( L(s | same) P(same) + L(s | different) P(different) ), per
metric, with s the similarity value. The likelihoods and the prior come from a calibration file
(`--calibration C.json`), versioned, integers only (so it parses with the repository's reader):

    {"schema": "odin3-graphlet-calibration", "version": 1, "source": "<where the labels came from>",
     "modes": {"undirected-5": {
         "prior": {"same": 30, "different": 70},
         "metrics": {"gdd_agreement": {"edges": [0, 500, 800, 900, 1000],
                                      "same": [1, 4, 10, 15], "different": [40, 20, 8, 2]},
                     "cosine_log": { ... }}}}}

Similarity is scaled to per-mille and binned by `edges` (bin i is [edges[i], edges[i+1]), the last
bin includes 1000). Binned density: L = (count_in_bin + 1) / (total + n_bins) (Laplace smoothing, so
no likelihood is zero). The prior is its two counts normalised. If the file has no entry for the
mode or metric, or `--calibration` is not given, the pass reports the similarities only and logs
"uncalibrated". The combined posterior (both metrics, naive Bayes, which assumes the metrics are
independent given the label; they are not, so it is reported next to, never instead of, the
single-metric posteriors). The real calibration comes from Loki's labelled composites later and is
not built here; a tiny file ships for unit tests only (tests/data/graphlet_calibration_test.json).

## 15. Changes to earlier sections

Section 6's file layout is `groups: [...]` (one entry per group) instead of `modules: {...}`; each
group has `name, module, source, nodes, edges, self_loops, global_nets, halo, reported,
orbit_totals, gdd, ms` and with `--per-node` a `node_vectors` list. Section 8's pass line gains
`--focus F` (repeatable), `--cells FILE`, `--halo 0|1`, `--compare A B`, `--calibration C`.
Python gets `Design.graphlets(..., focus=None, cells=None, halo=0)` and `graphlet_compare(a, b,
calibration=None)`. Unit tests (section 9) gain: identical subgraphs give GDD agreement 1 and cosine
1; the posterior rises with similarity under the test calibration; a focus equals brute force on the
induced subgraph; a halo-1 run equals brute force on the group-plus-neighbours subgraph reported for
the group.

## 16. Graph-generic core and build order (Peter 2026-10-30)

Peter, verbatim: "Go with your plan trying to build it graph generically as this might be useful in
other instances ... test it out against the oracle and then build the directed graph one (which has
no oracle) think how you will verify it works."

- The counters live in `src/graph/` (new directory), not in `src/util/` (containers only, CLAUDE.md)
  and not in `src/passes/` (netlist passes). `src/graph/` includes no IR or netlist header: input is
  a node count and a directed edge list (`glet_edge`), converted to a CSR simple graph
  (`glet_graph_build`); undirected use is the union of the two directions; optional node colours are
  a later addition (spec only, section 4 "typed"). The pass `src/passes/graphlets/` is a thin
  adapter: netlist -> graph (section 2 rules, global-signal exclusion, focus / partitions / halo) ->
  core -> attributes + JSON.
- Public C ABI: `odin3_graphlets_count(n, from[], to[], n_edges, directed, size, max_work, out,
  out_len)` in include/odin3/odin3.h, int64 `n x n_orbits` row-major into the caller's buffer, so the
  Python package exposes `odin3.graphlet_counts(n, edges, directed=False, size=5)` for any edge list
  (Norns, Mimir, anything non-netlist). Unit tests of the core run without any netlist.
- Build order: (1) undirected core, gated against ORCA (identical counts) and brute force, committed
  and pushed as a gate before (2) the directed counter is started.

## 17. Verification plan for the directed counter (no oracle), written before the code

Each check reports a pass count in the PR.

1. Independent brute-force reference in C (tests/unit/test_graphlets_count.c): separate code, the
   simplest possible: enumerate every connected 2-4 node subset of a small digraph, resolve
   anti-parallel pairs, classify the induced oriented graph by exhaustive-permutation canonical form,
   and assign orbits by automorphisms found by exhaustive permutation. The fast counter must equal it
   on thousands of random small digraphs (varied density, with and without anti-parallel pairs) and
   on the directed graph of every micro netlist.
2. A second independent implementation in Python (plain itertools, tests/tools/): the same
   brute-force by a different author path, run on a few hundred random digraphs through the Python
   binding: three-way agreement (C fast, C brute force, Python).
3. Catalogue checks: exactly 40 graphlets and 129 orbits for 2-4 nodes; per graphlet the orbit
   partition (automorphism classes) is listed (`docs/user/graphlets.md` table) and the orbit
   multiplicities are checked against a hand count of the automorphism group of each of the 40
   graphs (checked by a test that recomputes |Aut| by permutation and compares).
4. Projection to the oracle: drop directions (anti-parallel pair -> one undirected edge) and map each
   directed orbit to the undirected orbit of the same position of the underlying undirected graphlet
   (a many-to-one map built from the catalogue); the sum of directed counts mapped must equal the
   gated undirected counts (themselves equal to ORCA) for 2-4 nodes on every test graph, once the
   multiplicity of resolutions is divided out (an anti-parallel pair contributes 2^p occurrences; the
   projection check is therefore run on graphs without anti-parallel pairs, and on graphs with them
   the identity total_directed(per undirected set) = 2^p is checked).
5. Invariances: relabelling the nodes (random permutation) leaves the multiset of orbit vectors
   unchanged; reversing every edge maps each orbit o to its reverse orbit rev(o) (defined from the
   catalogue by reversing the canonical graph) with equal counts.
6. Public data: Sarajlic et al. 2016 publish no per-orbit counts for a public network in the text
   read; if the supplementary data gives one it is reproduced, otherwise this check is reported as
   not available.

## 18. As built: changes to the sections above (2026-10-30)

- Clock rule (section 3): a reading pin is a clock pin when it is port 0 of a type with the
  `CLOCK_PIN0` flag, or a port named `C`, `CLK` or `clock` (case-insensitive) of any cell; a net that
  reaches one is global, as is a net with at least F reading pins.
- Files hold integers only (the repository JSON reader is integer-only): the per-group time is
  `us` (microseconds), and comparisons written inside a signature file use `_ppm` keys
  (similarity in millionths). The result of `--compare` (its own `--out` file) holds decimals.
- `--compare` takes quoted words (`"a.json#NAME"`): an unquoted `#` starts a script comment.
- ABI: `odin3_graphlets_count(const odin3_graphlet_graph *, const odin3_graphlet_opts *, int64_t
  *out, uint64_t out_len)` (two small structs keep it at four parameters); ABI number unchanged
  (a function was added, nothing changed).
- `--edges-out` writes the directed simple graph (`N M`, then `u v` per arc, dense node numbers)
  of the first group; the undirected tools symmetrise it.
- Orbit catalogue: `glet_count [--directed] --catalogue` prints `orbit graphlet nodes edges mult
  code pos`; `docs/user/graphlets.md` lists the 40 directed graphlets.
- Directed graphlet and orbit ids are our own numbering (section 4), not Sarajlic's labels.
- Sarajlic et al. publish no per-orbit counts for a public network in the text read (check 17.6).
- PR #304 review fixes (2026-10-31): the orbit tables are built once under a mutex and a release
  flag, set only after a complete build (a failed build returns ODIN3_ERR_NO_MEMORY and is retried),
  so `odin3_graphlets_count` is thread-safe. `--compare` inputs and the calibration file are
  validated on load (non-empty string mode and group name; GDD pairs with degree and count at least 1
  and strictly increasing degrees; non-negative totals and counts; sorted bin edges; at most 4096
  bins; invalid files are ODIN3_ERR_INVALID_ARG); `--partition` stops at 4096 groups. The ABI number
  stays 7 (v1.2.0 is tagged at ABI 7 without `odin3_graphlets_count`; repo precedent, docs/IR.md:
  an additive export needs no bump), so a libodin3 built from v1.2.0 lacks the symbol: the Python
  wrapper raises "libodin3 too old: needs odin3_graphlets_count, Odin III > v1.2.0". The Python
  `max_work` default is the CLI bound, 2e10. No post-1.2.0 patch notes file exists yet; this entry
  is the record.
- `$sop`/`$lut` fix (2026-10-31, found by the Loki lane). The merged pass (#304) took every cell
  whose type defines a `const_value` hook for a constant, so every `$sop` and `$lut` (all LUTs and
  gates read from BLIF) was dropped: `.names a b y` / `11 1` gave 3 nodes and 0 edges. The test is
  now the net-level one of section 2 (all pins non-reading and on constant nets), the same as the
  JSON writer's. Second finding of the same gate: an inout pin was its own sink, so a black-box
  cell with inout pins (`$_DFFSR_PPP_` read from BLIF) counted one self-loop per pin; E-4 skips
  driver == sink and the pass now does too (the simple graph is unchanged, only `self_loops` fell).
  Lesson: the ORCA gate compares counts on the pass's own exported graph, so it tests counting, not
  construction. `tools/graphlets/construction_gate.py` (CTest `graphlets_construction`) compares
  the counted graph (nodes, simple edges, self-loops, directed edges, degree sequence) with an
  independent E-4 build from `write_json` through `plugins/python/odin3_graph.py`, on LUT form
  (BLIF as read) and gate form (`abc --gates cmos6`). Results (main 0a44260b plus the fix, EArch
  parmys goldens): 68 micros and the 14 small VTR
  designs, LUT and gate form, 164 of 164 identical. Negative control: the unfixed binary fails the
  gate on 3 of the 4 smallest LUT-form micros with logic (for example `bm_DL_2_1_mux`: 4 nodes and
  0 edges against 5 nodes and 4 edges in the E-4 build). ORCA gate on the pass's graphs: 904 of 904
  identical over 452 designs (LU32PEEng, LU64PEEng and mcml are above `--max-nodes` 200000, as
  designed); directed checks all pass (Python reference 701/701, relabelling 752/752, reversal 752/752,
  projection to the undirected counter 556/556 and to ORCA 556/556).
