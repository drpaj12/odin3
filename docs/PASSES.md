# Passes

One entry per pass, added by the `/new-pass` skill and kept in pipeline order (spec §6).

| Pass | Phase | Input view → output view | Source | Golden test | Notes |
|---|---|---|---|---|---|
| `read_blif <file>` | 1 (1C, 1D) | — → netlist (BLIF cells, black boxes) | `src/frontends/blif/reader.c`, `src/passes/builtin.c` | `blif_roundtrip_fixtures`, `cli_*_roundtrip` | Empty design only; the first model is the top (`odin3_design_set_top`), the CLI `--top` then wins. |
| `read_techlib <file>` | 1 (1G, 1D) | — → same (cell types added) | `src/techlib/reader.c`, `src/passes/builtin.c` | `test_pass_manager` (read_techlib), `cli_techlib`, `blif_roundtrip_techlib` | Each cell of a `.o3lib` becomes a local cell type with its library data; read it before the netlist that instantiates the cells. |
| `hierarchy [--top <name>] [-auto]` | 1 (1D) | any → same (top recorded) | `src/passes/builtin.c` | `test_pass_manager` (hierarchy) | `--top`, else CLI `--top`, else a set top is kept unless `-auto`, else the single uninstantiated module; 0 or ≥2 candidates is an error listing them (DESIGN §4.0, PHASE1 #18). |
| `check [--fast]` | 1 (1B, 1D) | any → same | `src/ir/check.c`, `src/passes/builtin.c` | `test_ir_check`, `test_pass_manager` | IR.md §9; FAST = rules 1–5, 11. Also run FULL by the pass manager before/after every pass in Debug (`--check` in Release). |
| `compact` | 1 (1B, 1D) | any → same (IDs renumbered) | `src/ir/compact.c`, `src/passes/builtin.c` | `test_ir_compact`, `test_pass_manager` | IR-6; tombstones for dead objects. Atomic per module only: an out-of-memory failure leaves earlier modules compacted. |
| `stats` | 1 (1D) | any → same | `src/passes/builtin.c` | `test_pass_manager` (stats format) | Logs `stats: design: modules N, top T`, then per module ports/nodes/nets/wires (live) and a name-sorted cell-type histogram; the Python walk (1D T4) matches it. |
| `write_blif <file>` | 1 (1C, 1D) | netlist → — | `src/backends/blif/writer.c`, `src/passes/builtin.c` | `blif_roundtrip_fixtures`, `cli_*_roundtrip`, `cli_top_written_first` | The top module first (BLIF's first model is the top), then the rest in creation order. |
