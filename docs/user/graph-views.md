# Graph views

`write_graph KIND FILE` writes one JSON file (schema `odin3-graph`, version 1) that a viewer can lay
over the HDL: the state transition graph of each FSM (`stg`), the control flow graph of each
process (`cfg`) and the signal data dependency graph (`ddg`). The definitions, IDs and provenance links are in
`docs/specs/2026-11-05-graph-views-design.md`.

```text
odin3 -p "read_verilog --nolatches fsm.v; proc; write_graph stg fsm.stg.json"
```

Every module is listed, in creation order; each has `graphs`, a list of graphs with `nodes` and `edges`.
Every node and edge has a string `id`, `label`, `prov` (the provenance record ID, the exact join key into the
name map) and `src` (HDL spans `file:L.C-EL.EC`, as `dump_locs` prints them).

## stg

Run after `proc`. The pass runs the detect, extract and optimise stages of `fsm` (as `fsm -nomap -norecode`),
so states carry the HDL's own codes, not the recoded ones. It sets the `fsm_encoding` attribute on state wires
(as `fsm_detect` does) and changes nothing else. A design that still has processes is refused.
State ids are `fsm:W/s<i>` (W the state wire ID); `code` is MSB first, `-` a don't care. A transition edge has
`cond` (pattern over `inputs`), `cond_expr` (for example `go & !rst`), and `out`.
The graph's `src` and `select_nodes` give the HDL of the state register and the next-state selection cells;
a single transition is not attributed to one case item (`src` is empty on states and transitions).

## cfg

Run before `proc` (after `read_verilog`). One graph per `$proc` node, id `proc:N`. Nodes: `entry` (the root
case, with its assignments in `actions`), `switch` (`signal`, `signal_nets`, `full_case`, `parallel_case`),
`case` (`compares`, `default`, `actions`), one `join` per switch, one `exit`. Edges: `control` from a switch to
each case (`label` the compare patterns or `default`), `seq` for everything else. `syncs` on the graph lists the
clock/level conditions and their update assignments. Each switch and case carries `prov`: after `proc` lowers
the process, the `$mux`, `$pmux` and `$eq` cells made for it are DERIVED from these records, so look the
record up in the lowered design (for example `odin3_prov_index_by_record`) to find them.

## ddg

One graph per module, id `ddg`, over the IR as it is. Run it before `proc` for the process-level view and after
`proc` for the lowered one. Nodes: `signal` (a wire, bus whole, with `width` and `port` `in|out|inout|""`) and
`temp` (a net outside every wire). Edge a to b: a is read to compute b; `kind` `data` (an operand) or `control` (a
select, clock, enable, reset or a switch selector), `via` (the cell or process node ID), `op` (its cell type),
`seq` (true across a flip-flop or latch), `src` (the HDL of the cell or action). Edges are signal level; bit ranges
are not given. Before `proc`, the outputs of a `$proc` are unnamed temporaries copied to the wires by the sync
actions, so `temp` nodes appear between the process and its wires.

## Python

`Design.write_graph("stg")` returns the parsed dict.
