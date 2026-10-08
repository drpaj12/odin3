# src/ir — Phase 1
Core IR: Design → Module → Node/Pin/Net as peers, arena-allocated, `uint32_t` IDs, op registry, `check`, provenance (spec §5). Read docs/DESIGN.md and docs/IR.md before touching anything here.

## Files
- `ids.h` — typed 32-bit IDs (node, pin, net, wire, module, celltype, prov, passrun); 0 is none.
- `value.[ch]` — parameter/attribute values (int, bits, string) and constants.
- `celltype.[ch]`, `cells/` — the cell-type registry (global built-ins, per-design local types, module types) and the built-in cell libraries (gates, flops, arithmetic, mux, memory, ports, constants).
- `design.[ch]` — the design handle: strtab, cell-type table, modules, provenance and tombstone tables.
- `module.h`, `module.c` — modules, ports, name maps and ID-order iteration ends.
- `node.c` — nodes and pins, `create_connected`, `replace`, deletion.
- `net.c` — nets, driver/sink partitions, `merge`.
- `wire.c` — wires, wire bits, aliases and the primary (wire, bit) of a net.
- `attr.c` — attributes on any object.
- `pinpool.[ch]` — size-classed pool of pin-ID blocks backing each net's pins.
- `prov.[ch]` — provenance lineage DAG, pass runs, source index, tombstones.
- `check.[ch]` — the IR §9 invariant checker (FAST and FULL).
- `compact.c` — `odin3_module_compact`: dense renumbering, tombstones, ID maps.
- `ir_test.[ch]`, `ir_internal.h` — test-only fault injection; private struct layouts.
