# tests/hardmap

Inputs and expectations of the `hardmap` pass (3C; spec `docs/specs/2026-10-14-3C-hardmap-design.md`).

| File | What |
|---|---|
| `mul_8x7.v`, `add_8x6.v`, `expected/*.blif` | CLI golden tests (`cli_hardmap_mul`, `cli_hardmap_add`): one `multiply` and one 8+6 `adder` chain |
| `sub_pad_u.v`, `sub_pad_s.v`, `sx_add.v`, `sx_add_c.v`, `adder_lib.v`, `adder_use.v` | A-6, A-8, A-9 inputs (Parmys pads, signed extension, `adder #(W)`) |
| `probes/` | Parmys behaviour probes (D1, D2b, D4, address merge); see its README |
| `micros.txt` | the arithmetic and memory micros of `hardmap_diff_micro` (relative to `tests/micro/verilog`) |
| `vtr_designs.txt` | the 19 VTR designs of `hardmap_vtr_sweep`, in run order (smallest work first) |
| `diff_expect.txt`, `vtr_expect.txt` | what `hardmap-diff` may report other than identical or equivalent, one reason per line |

## hardmap-diff

`tools/hardmap-check/hardmap-diff` runs `tools/hardmap-check/phase3_pre_abc.script` (the one copy of
the Phase 3 script up to `lower`) on a design and compares the BLIF with the Parmys golden of
`~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm`:

1. hard-block counts by model (`adder`, `multiply`, `single_port_ram`, `dual_port_ram`) and `.latch`;
2. `netlist-compare --implicit-models` (identical), else `equiv-check --match-unnamed`
   (equivalent, differs, unproven).

Classes and the expect syntax are in the tool's docstring. In short, an expect line is
`<path> <id> <reason>` with `id` one of `D<n>` (the spec's divergence table, section 10),
`counts:<+N|-N> <unit>`, `lower-gap` (now only `$pow`), `unproven`, `no-golden`. A line that does not occur is stale and
fails: the lists shrink. A `run wreduce first` warning from `hardmap` fails the design and cannot be
listed (3C-2a review M-2). Every entry of a design must occur: a `counts:` line skips the netlist
comparison, so any other entry beside it is stale. `hardmap_diff_micro` skips (77) without the
golden tree, so CI has no evidence from it; it passes no `--arch` (the sweep does).

Micros (non-heavy): `ctest --preset debug -R hardmap_diff_micro`. The 19 (heavy, controller, one
design per `tools/heavy-run`, smallest first): `ctest --preset sweep -R hardmap_vtr_sweep` runs them
all in one heavy job; the per-design commands are in PHASE3 decision 42.
