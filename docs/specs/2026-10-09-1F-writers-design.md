# 1F — dot, JSON and structural Verilog writers

Status: agent-approved under PHASE1 #12. Phase 1, sub-project 1F. Spec §9.

## Goal

Three writers over the IR, each validated against an independent tool on the goldens:

| Writer | Use | Validation on every `ok` golden (RAM goldens excluded where the tool cannot model them) |
|---|---|---|
| `odin3_json_write` (Yosys JSON schema) | netlistsvg, nextpnr, external tools | Yosys `read_json; write_blif` → `netlist-compare` identical to the golden |
| `odin3_verilog_write` (structural) | readable output, RE output later, simulation | Yosys `read_verilog` (with black-box stubs) → `write_blif` → `tools/equiv-check` equivalent to the golden; Icarus parses it |
| `odin3_dot_write` (Graphviz) | looking at a design | `dot -Tsvg` accepts it; node budget honoured |

## JSON (Yosys netlist schema)

`{"creator", "modules": {name: {"attributes", "ports": {name: {"direction", "bits"}}, "cells":
{name: {"type", "parameters", "attributes", "port_directions", "connections"}}, "netnames":
{name: {"bits", "attributes"}}}}}`. Bits are integers ≥ 2 (0/1 reserved for constants "0"/"1"):
one per IR net, numbered in net ID order. A BLIF cover (`$sop`, our WIDTH/COVER form) is written
as Yosys `$lut` when it has at most 6 inputs (exact truth table), otherwise as Yosys `$sop` with
`DEPTH`/`TABLE` converted from the cover (Yosys's `$sop` has a different parameter shape). Latches map to Yosys `$_DFF_P_`/`$_DFF_N_`/`$_DLATCH_P_`/`$_DLATCH_N_`/`$_FF_`; their
`INIT` goes to the `init` attribute of the Q net (Yosys convention; values 2/3 → `x`). Hard and
black-box cells keep their type names; parameters are written as binary strings (Yosys style).
Provenance is written as a `src` attribute (`file:line.col`) on cells and netnames.

## Verilog (structural)

One `module` per IR module in creation order, ports in port order (vector ports as
`input [w-1:0] a`; scalar as written); nets as `wire` (names escaped `\name ` when not simple
identifiers — BLIF names like `$add~5^ADD~5-1[0]` need escaping); `$sop` → `assign y = …;` as a
sum of products (ON-set) or its complement (OFF-set); gates → `assign` with operators; latches →
`always @(posedge|negedge C) Q <= D;` with `initial Q = …` for INIT 0/1, level latches with
`always @*`; constants as `1'b0/1'b1`; word cells as `assign` with operators and signedness;
instances of modules, hard and black-box cells as module instances with named port connections
(vector pins concatenated `{…}` MSB first); declared black boxes as `(* blackbox *)` empty module
stubs. Deterministic output, provenance as `// file:line` comments (option `--name-style` from
IR.md §6 picks provenance names or short names).

## dot

`digraph` with one `subgraph cluster_<module>` per module (hierarchy), a node per cell (label:
type and name), an edge per driver→sink pin pair; `--focus` = a hierarchical path, a
`file:line` (via the provenance forward index: objects whose ancestry includes that line), or
`cone(net)` (fan-in cone, iterative); refuses to render more than `--max-nodes` (default 2000)
without a focus and says how many it would have drawn.

## Files

`src/backends/json/{writer.h,writer.c}`, `src/backends/verilog/{writer.h,writer.c,escape.c}`,
`src/backends/dot/{writer.h,writer.c}`, `tests/unit/test_{json,verilog,dot}_writer.c`,
`tools/writer-check/writer-check` (bash: runs the three validations over a golden root or the
fixtures, table + summary, `-j` ≤ 2), CTest `writer_check_fixtures`.

## Out of scope

Altera-primitive Verilog flavour (Phase 6), JSON reading (Phase 7), CIRCT (Phase 7), word-level
Verilog with `always` blocks recovered from mux trees (that is `raise`, Phase 6).
