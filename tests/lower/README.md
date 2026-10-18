# tests/lower — fixtures of the `lower` pass (3B)

Layout (3B spec 14.1, 14.2; plan Task 2):

- `cells/<type>_<variant>.v` — one tiny module per rule and variant, written by
  `tools/lower-check/gen_cells.py` (`--out tests/lower/cells`), a (type, widths, signs) grid. Task 3
  adds the committed `.il` (the input of `lower`, `read_verilog; proc; write_rtlil`) and `.blif` (the
  committed output) beside each `.v`; CTest `lower_cells` compares a fresh run with the `.blif` by
  `netlist-compare`, and `lower_cells_yosys` runs `lower-diff` over the same files (needs Yosys 0.55).
- `hand/` — hand-built designs for `lower-sim`: `and_mux.v` (a design every cell of which has a
  simulate hook) and `shiftx.v` (a `$shiftx`, which has none: reported `skipped`).
- `diff_expect.txt`, `vtr_expect.txt` — later tasks (the differences with a reason, the expected
  cap outcomes).

The tools (`tools/lower-check/`): `lower-diff` (Odin III's `lower` against Yosys 0.55 on the same
input, by tier S, T or D), `lower-sim` (the 1E simulator before and after `lower`), and in Task 9
`lower-equiv`.
