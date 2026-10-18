# tests/lower — fixtures of the `lower` pass (3B)

Layout (3B spec 14.1, 14.2; plan Task 2):

- `cells/<type>_<variant>.v` — one tiny module per rule and variant, written by
  `tools/lower-check/gen_cells.py` (`--out tests/lower/cells`), a (type, widths, signs) grid. Task 3
  adds the committed `.il` (the input of `lower`, `read_verilog; proc; write_rtlil`) and `.blif` (the
  committed output) beside each `.v`; CTest `lower_cells` compares a fresh run with the `.blif` by
  `netlist-compare`, and `lower_cells_yosys` runs `lower-diff` over the same files (needs Yosys 0.55).
- `cells/{dff,dffe,sdff,sdffe,sdffce,adff,adffe,aldff,aldffe,dffsr,dffsre}_*` (Task 6): the flip-flops,
  64 fixtures (widths 1, 4, 8; at width 4 the falling clock, active-low controls and both). They run
  `proc; opt` (not `proc` alone) so that the enable and synchronous reset are folded into the cell;
  the `.blif` is the lowered netlist (`.latch`); `lower-diff --family ff` reads the Odin III side as
  BLIF only (the storage bit cells have no RTLIL form). `diff_expect_parmys.txt` lists the cells on
  which Parmys's unmodified `adffe2dff.v` / `aldffe2dff.v` differ (spec 16 row 4).
- `cells/dlatch_*` (Task 7): the `$dlatch` fixtures (widths 1, 4, 8 and the active-low enable at width 4), read
  without `--nolatches` (the flag turns an inferred latch into a feedback mux) and run `proc; opt; opt_clean`; tier S
  against Yosys (`lower-diff --family latch`). `$adlatch`, `$dlatchsr` and `$sr` have no Verilog form (neither
  Yosys nor Odin III infers them from `proc; opt`), so they have no fixture; `test_lower_latch` covers them.
- `cells/mem_*` (Task 8): eleven `$mem` fixtures (SIZE <= 16, WIDTH <= 4 so `equiv-check` decides them): sizes 2, 4, 6,
  16, OFFSET 1 and -2, two read ports, two write ports (the later wins), per-bit write enables via two partial writes, a
  registered read (a `$mem` plus a `$dff` here; `hardmap`'s FF merge makes sync ports) and a falling clock (`_neg`, run
  with `--neg-edge keep`). Tier D against Yosys's `memory -nomap; memory_map` (`lower-diff --family mem`; the Odin III
  side is BLIF only). `mem_w4_d4_off1` is the listed difference (`diff_expect.txt`): ABITS above clog2(SIZE). A transparent
  read, INIT, a ROM and the errors have no Verilog form in the front end's `$mem` output: `test_lower_mem` covers them.
- `hand/` — hand-built designs for `lower-sim`: `and_mux.v` (a design every cell of which has a
  simulate hook) and `shiftx.v` (a `$shiftx`, which has none: reported `skipped`); `pow.v` is a surviving `$pow` (CTest `cli_lower_pow`: the located error of
  spec 5.7; no golden exists).
- `diff_expect.txt`, `vtr_expect.txt` — later tasks (the differences with a reason, the expected
  cap outcomes).

The tools (`tools/lower-check/`): `lower-diff` (Odin III's `lower` against Yosys 0.55 on the same
input, by tier S, T or D), `lower-sim` (the 1E simulator before and after `lower`), and in Task 9
`lower-equiv`.
