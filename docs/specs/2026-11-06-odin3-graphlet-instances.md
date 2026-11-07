# Odin III addition: graphlet instances (spec, 2026-11-06)

Status: implemented (branch feat/graphlet-instances). Spec written by the Heimdall III analytics lane.

Purpose: Heimdall III (Peter 2026-11-06) shows, for one chosen graphlet type, every instance in the netlist
graph. Research question: structure signatures as reverse-engineering hints. Measure: for every graphlet type
G and graph, the number of instances written (when not truncated) equals the graphlet count derived from the
orbit totals of the same run: `orbit_total(o) / orbit_size(o)` for any orbit o of G (the table of
`glet_count --catalogue`).

## 1. What is added

Pass options of `graphlets` (spec 2026-10-30 section 8 gains):

    graphlets [existing options] --instances <graphlet id> [--directed] [--max N] --instances-out FILE

- `<graphlet id>`: undirected G0..G29 (size 5, Przulj numbering; with `--size 4` G0..G8) or, with
  `--directed`, G0..G39 (Odin III numbering of section 4 of the earlier spec; `glet_count --catalogue` lists
  them). The pass counts as usual and in addition writes the instances of that one type.
- `--max N` (default 100000): at most N instances are written; the file says whether it was cut.
- `--instances-out FILE`: the result, integers only (the repository JSON reader is integer-only):

      {"schema": "odin3-graphlet-instances", "version": 1, "mode": "undirected-5", "graphlet": 7,
       "nodes_per_instance": 4, "total": 120, "written": 100, "truncated": 1, "max": 100,
       "module": "top", "instances": [[12, 40, 41, 77], ...]}

  Each instance is the ascending list of the module-local node IDs of one induced connected subgraph that
  is isomorphic to the graphlet (undirected: the induced undirected simple graph; directed: the induced
  oriented graph, where an anti-parallel pair is resolved as in section 4 of the earlier spec, so one node
  set can appear twice under two different directed types, and once per resolution under the same type if the
  resolutions are isomorphic). `total` is the exact count of instances (the enumeration continues past
  `max`, only the writing stops), so `total` can be compared with the orbit totals.

## 2. Rules

- Same enumerator and same counting rules as the orbit counter (ESU, section 5; same graph construction,
  `--exclude-global`, `--focus`, `--cells`, `--halo` apply unchanged). No second enumerator.
- Instance order: the order in which the enumerator reaches the node sets (deterministic); within an
  instance, ascending node IDs.
- Cost: no extra memory beyond the written instances (at most N tuples of at most 5 IDs).

## 3. C ABI and Python (graph-generic path, no netlist)

Implemented as additive exports (the ABI stays 7). The first draft of this section had seven parameters; the lint
gate allows five, so the buffer is a struct:

    typedef struct odin3_graphlet_instance_buf {
        uint32_t *out;     /* in: receives the instances */
        uint64_t out_len;  /* in: capacity of out in ids */
        uint64_t max;      /* in: write at most this many instances */
        uint64_t total;    /* out: the exact number of instances */
    } odin3_graphlet_instance_buf;

    odin3_status odin3_graphlets_instances(const odin3_graphlet_graph *graph,
                                           const odin3_graphlet_opts *opts, uint32_t graphlet,
                                           odin3_graphlet_instance_buf *buf);
    odin3_status odin3_graphlet_nodes(const odin3_graphlet_opts *opts, uint32_t graphlet,
                                      uint32_t *nodes);

`odin3_graphlets_instances` writes up to `buf->max` instances of `graphlet` as consecutive tuples of
`odin3_graphlet_nodes` ids into `buf->out` (`out_len` must be at least `max * nodes`), and the exact total into
`buf->total` (same status codes as `odin3_graphlets_count`). Python:
`odin3.graphlet_instances(n, edges, graphlet, directed=False, size=5, max=100000)` returns `(total, [tuple, ...])`.
Heimdall uses it through its bridge program for the planes that have no netlist (clb, stdcell, die, graph planes).

Metric precision (added 2026-11-07 for the Heimdall port of the metrics): `graphlets --compare` prints 6 decimals, so
the C ABI also exports the two metrics at full double precision for two sets of orbit rows:

    odin3_status odin3_graphlets_similarity(const odin3_graphlet_rows *rows_a,
                                            const odin3_graphlet_rows *rows_b, uint32_t n_orbits,
                                            odin3_graphlet_similarity *out);

(`gdd_agreement`, `cosine_log`; the signature of a set of rows is built inside, as the pass does). Python:
`odin3.graphlet_similarity(rows_a, rows_b, n_orbits)` returns `(gdd_agreement, cosine_log)`.

## 4. Tests (gate)

1. Unity: on random graphs (undirected n <= 9, directed n <= 8) the instance count of every graphlet type
   equals the type count derived from the orbit totals (each orbit total divided by the orbit size), and
   every written tuple is a connected node set whose induced subgraph is isomorphic to the type (checked
   with the brute-force classifier of the existing tests).
2. Hand cases: the path a-b-c-d-e gives for G1 (3-node path) the instances (a,b,c), (b,c,d), (c,d,e); a
   triangle with a pendant node gives one triangle (G2) and two 3-node paths.
3. `--max 1` writes one instance and `truncated` 1, `total` unchanged (CLI test `graphlets_cli_*`, which also
   compares G1 of `ff.parmys` with the committed golden `tests/golden/graphlets/ff.parmys.g1.instances.json`).
4. Python test of `graphlet_instances` against the same hand cases.

## 5. Heimdall side (already written against a fixture)

`heimdall/analytics.py` asks the bridge for the instances of the rarest types of each plane
(`add_instances`), `analytics/instances.<plane>.<mode>.json` holds them as plane object indices; the viewer
colours the conflict graph of the instances with DSatur (two instances conflict when they share a node or an
edge joins them), the number of colours is shown in the legend, shared nodes are pies.
