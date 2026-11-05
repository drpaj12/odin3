# Graph views: STG, CFG, signal DDG, SCOAP join (design, 2026-11-05)

Status: spec first, code follows (one PR per item). Source: Peter 2026-11-05 (brief
`odin3-graph-views`): "add all of these as graphs that the viewer could add as planes above the HDL
view". Purpose: Odin III goal 2 (graph analysis of netlist high-level structure linked to the AST)
and provenance at every granularity. Consumers: Wyrd o3viz (planes above the HDL plane), Loki,
Mimir. Measure: each graph is exported with stable IDs and provenance links to HDL spans; a
viewer can join it to the cell plane without guessing.

Peter 2026-11-05 picked items 1, 2, 3 (and the SCOAP export check, item 3b). Items 4 to 6 are
**deferred (Peter 2026-11-05)** and not specified further here:

| Item | Status |
|---|---|
| 1 FSM state transition graph (`stg`) | this spec, built |
| 2 Control flow graph (`cfg`) | this spec, built |
| 3 Signal data dependency graph (`ddg`) | this spec, built |
| 3b SCOAP per-node join | this spec (section 7) |
| 4 Sea of nodes (Click and Paleczny, IR'95) | deferred (Peter 2026-11-05) |
| 5 Per-pass snapshots and diffs | deferred (Peter 2026-11-05) |
| 6 IGV export | deferred (Peter 2026-11-05) |

## 1. Command and file

One pass: `write_graph KIND FILE` with KIND one of `stg`, `cfg`, `ddg`. It writes one JSON file,
schema `odin3-graph`, version 1. Registered with `processes = OK` because `cfg` and `ddg` read
`$proc` nodes. Python: `Design.write_graph(kind)` returns the parsed dict (like `Design.scoap`).
The pass changes no cell, net, wire or provenance record. The one exception is `stg`, which runs
the detect stage of `fsm` and so sets `fsm_encoding = "auto"` on state wires (an attribute,
exactly as `fsm -nomap` does, section 3).

Top level: `{"schema": "odin3-graph", "version": 1, "kind": KIND, "modules": [...]}`.
Module: `{"id": MODULE_ID, "name": NAME, "graphs": [...]}`; a module with no graph of that kind has
`graphs: []`. Every module is written, in creation order.

A **graph** has `{"id", "kind", "label", "nodes": [...], "edges": [...]}` plus kind fields. A
**node** has `{"id", "kind", "label", "src": [...], ...}`. An **edge** has `{"id", "from", "to",
"kind", "label", "src": [...], ...}`. `from`/`to` are node IDs of the same graph. Edge IDs are
`e<k>`, k the position in `edges` (deterministic for a given IR). The generic shape is the same for
all three kinds so a viewer needs one reader.

## 2. IDs and provenance (all kinds)

IDs are strings built from IR object IDs of the module, which are stable for a given IR:

| Object | ID |
|---|---|
| FSM graph | `fsm:W` (W = state wire ID) |
| FSM state | `fsm:W/s<i>` (i = state index in the pass's table, reset state first when present in table order) |
| FSM transition edge | `e<k>`; also `trans` = index in the table |
| process graph | `proc:N` (N = `$proc` node ID) |
| CFG node | `proc:N/entry`, `/exit`, `/sw<i>`, `/c<i>`, `/j<i>` (i = index into the body's `switches[]` or `cases[]`) |
| DDG graph | `ddg` |
| DDG node | `w<ID>` for a named wire, `n<ID>` for a net not in any wire |

Provenance fields, same meaning everywhere:

- `src`: array of HDL span strings, the SOURCE leaves of the object's provenance chain, as
  `dump_locs` prints them (`file:L.C-EL.EC`, at most 8, from `odin3_hdl_locs` and
  `odin3_hdl_loc_print`; backends/common/hdl_loc.h). Empty array when there is none.
- `prov`: the provenance record ID (`odin3_*_prov`), 0 for none. This is the exact join key into
  the name map and the provenance index. After `proc` lowers a process, the `$mux`/`$pmux`/`$eq`
  cells it makes are DERIVED from the records of the switch and case items (proc_mux.c, `mx_prov`:
  parents are the switch's rule record and its cases' records). A CFG node's `prov` is therefore the
  record to look up in the lowered design (`odin3_prov_index_by_record`) to find the cells the
  branch became. The CFG file of the pre-lowering design and the netlist of the lowered design
  share the record IDs only when `write_graph cfg` is run in the same design session before
  `proc` (the IDs of records are design-global and never reused). The viewer joins on `prov`.
- `node`, `net`, `wire`: IR IDs, where the object is one.

## 3. Item 1: state transition graph (`stg`)

Source: the `fsm` pass's own state (4.8 spec, `docs/specs/2026-10-20-4.8-fsm-design.md`, section 3
option 2: the FSM table lives inside the pass). `write_graph stg` runs `detect`, `extract` and the
two `opt` stages of `fsm` (as `fsm -nomap -norecode`: no recode, no map) and exports each
extracted FSM from `odin3_fsm_inst`. States therefore carry the codes the HDL gave them, after
unreachable-state removal and input merging, not the one-hot recoding. Modules that still have
processes are skipped with an info line (`fsm` needs `proc` first). Needs `proc` to have run: the
usual place is after `proc; opt_expr`, before `fsm`.

Graph fields: `state_wire` `{wire, name, width}`, `state_node` (the old state register node ID),
`encoding` (the `fsm_encoding` attribute), `reset_state` (state index or -1), `state_bits`,
`inputs` (control-input bits: `{index, label, net}`; `net` 0 for a constant), `outputs` (same),
`select_nodes` (the `$mux`/`$pmux` cells of the next-state tree: `{node, prov, src}`), `src` and
`prov` of the state register node.

State node: `kind: "state"`, `label` the code string MSB first (`0101`; `-` is a don't care),
`index`, `code`, `reset` (bool). Transition edge: `kind: "transition"`, `trans` (table index),
`cond` the control-input pattern MSB first over `inputs` (`1-0`), `cond_expr` the same as text over
the input labels (`a & !b`, `1` when every bit is a don't care), `out` the control-output pattern.
`label` = `cond_expr`.

Provenance limit (stated honestly): the fsm pass extracts a transition by evaluating the whole
selection tree, so a transition is not attributed to one case item. The `src` of the graph, of the
state register and of each selection cell is exact (the case items are the `select_nodes` spans);
the state and transition objects carry `src: []`. A later refinement is to attribute a transition
to the cells the evaluation passed through. Not built.

## 4. Item 2: control flow graph (`cfg`)

Source: `$proc` nodes (before `proc` lowers them), body per `src/ir/proc.h` (2D IR change B). One
graph per live `$proc` node. A body is a tree: a case has actions, then switches in order; a switch
has cases; the root case has no compares. Semantics follow the RTLIL process (the print order of
`odin3_proc_print`): a case executes its actions, then its switches one after the other.

Nodes:
- `entry` (kind `entry`): the root case. Carries `actions`, and `syncs` on the graph (type
  `posedge|negedge|low|high|always`, signal label, `actions`).
- `case` (kind `case`): one per non-root case, `compares` = the compare patterns as sigspec text,
  `default` true when it has none. Carries `actions`.
- `switch` (kind `switch`): `signal` = sigspec text of the selector, `full_case`, `parallel_case`.
- `join` (kind `join`): one per switch, where its cases meet.
- `exit` (kind `exit`): once.

`actions` = list of `{lhs, rhs, prov, src}` (sigspec text, RTLIL spelling of
`odin3_proc_sigspec`; `prov`/`src` of the action).

Edges (kind `control` unless stated):
- a case or the entry to its first switch (kind `seq`), or to its exit-of-case if it has none;
- switch to each of its cases, `label` = the compares text or `default`;
- the exit-of-case of each case to the switch's `join` (kind `seq`);
- `join` to the next switch of the same parent case (kind `seq`), else it is the exit-of-case of the
  parent;
- the root's exit-of-case to `exit`.

The exit-of-case of a case with no switches is the case node itself; otherwise the `join` of its
last switch. A switch with zero cases has an edge switch to join. `src`/`prov` of entry = the node's;
of case/switch = the body record's own `prov` and its spans.

Lowering link: each `switch` and `case` has `prov`; the joined `$mux`/`$pmux`/`$eq` cells are the
objects DERIVED from that record in the lowered design (section 2). The file also lists, per
switch, `signal_nets` (net IDs of the selector, LSB first; 0 for a constant or ANY), so a viewer
that has the pre-lowering netlist can select the `$eq` cells that read those nets.

## 5. Item 3: signal data dependency graph (`ddg`)

One graph per module (`ddg`), over the IR as it is when the command runs. Run it before `proc`
for the process-level view, and after `proc` for the lowered one ("a variant after, for
comparison"): the same code reads whatever cells exist, including `$proc`.

Nodes: a **signal** is a wire (kind `signal`, `wire` ID, `width`, `port` direction when the wire
is on a module port: `in|out|inout`) or, for a net that is in no wire, a `temp` (`net` ID). A bus is
one node; edges are signal level (no bit ranges). Constant nets are not nodes and give no edges.

Edges: a -> b when signal a is read to compute signal b. Fields: `kind` `data` or `control`, `via`
(node ID of the cell or process that computes b), `op` (cell type name, `$proc` for a process),
`seq` true when the cell is a flip-flop or latch (the edge crosses a clock cycle), `src`, `prov` of
`via` (for a `$proc`, of the action or switch the read comes from).

Rules, per live cell (port nodes excluded, constants excluded):
- Normal cell: for every input pin net and every output pin net of the cell, an edge input signal
  -> output signal. Control ports (port name in `S, CLK, C, EN, E, ARST, SRST, ALOAD, SET, CLR`)
  give `control`; every other input port gives `data`. Sequential cells follow the same rule with
  `seq: true`. Instances, black boxes and hard cells: every input -> every output, kind `data`.
- `$proc` node: for every action, each rhs position (an IN position, or an OUT position read as its
  current value) -> each lhs signal of the action, `data`. For every switch enclosing the action
  (the path from the root), the selector's signal -> each lhs signal of the action, `control`, with
  the switch's `prov`. For every sync, the sync signal -> each lhs signal of the sync's actions,
  `control`. Compare patterns give no edge.
- Duplicates (same from, to, kind, via) are written once.

## 6. Tests

Unity unit tests on tiny designs built through the construct/elab fixtures where they exist, else
Verilog through the CLI test harness with expected JSON checked by a Python tool test
(`tests/tools/test_graph_views.py`, CTest `graph_views`): (a) a `case` with default, (b) an
`if / else if / else` chain, (c) a 3-state FSM, (d) two `always` blocks sharing signals. The expected
graphs are hand-written in the test (node and edge sets), not generated. `ch_intrinsics` is the
exercised example in the PR report.

## 7. Item 3b: SCOAP per-node join

Check (2026-11-05, read from `src/passes/scoap/scoap.c`): the `scoap` JSON (schema `odin3-scoap`
version 3) already has, per module, `nets: [{id, name, cc0, cc1, co, [sc0, sc1, so], [p1_ppm]}]`.
`id` is the module net ID (stable key to `odin3_net_prov`); SCOAP is defined per net, and a "node"
value is its driver's output net. What is missing is the join data: the driver node ID and the HDL
spans, so a viewer need not walk provenance itself. Added, additive and opt-in (default output
byte-identical, version stays 3): option `--prov`, which adds per net `"driver"` (node ID of the
first driver pin, 0 for none), `"wire"` (primary wire ID, 0 for none), `"prov"` (the net's record
ID) and `"src"` (the span strings of section 2). With `write_graph ddg`, a viewer joins by net ->
wire (`w<ID>`) for signals and by `driver` for cell-plane objects.
