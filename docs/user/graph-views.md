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

## Python

`Design.write_graph("stg")` returns the parsed dict.
