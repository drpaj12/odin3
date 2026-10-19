# lower-check — evidence tools for the `lower` pass (3B)

| Tool | Spec | What it does |
|---|---|---|
| `lower-diff` | §14.2 | Odin III's `read_verilog; proc; opt; lower` against Yosys 0.55's `techmap; dffunmap; opt_clean -purge` on the same `in.il`, by tier: **S** gate for gate (`netlist-compare` exit 0 and equal cell-type histograms), **T** the `techmap.v` structures (`equiv-check` exit 0 and equal histograms after `opt -fast`; an inconclusive proof is `UNPROVEN`: exit 1 unless an `equiv` expect line lists it, never `ok`), **D** a declared reference (the asynchronous families, the other latches, `$mem`: `equiv-check` against Yosys's lowering with `maps/`). A design whose `in.il` has no word cell (folded away by `opt`) prints `NOTHING TO COMPARE` and exits 1 (`lower-sim`: same, unless `--allow-empty`); `ok (N cells)` says how many were compared. A difference fails unless `--expect` lists it with a reason; a listed difference that does not occur fails too. |
| `lower-sim` | §14.3 | The 1E `sim` pass on the design before `lower` (the word cells run through the registry's simulate hooks) and after (the bit cells), same seeded vectors, vector files compared. Per cell type: `pass`, or `skipped (reason)` for a type without a hook; **a skip is never a pass**. |
| `gen_cells.py` | §14.2 | Writes `tests/lower/cells/*.v`, the (type, widths, signs) grid of per-cell fixtures. |
| `lower-gen-diff` | §14.5 | Seeds of `verilog-gen --lower` (`tools/verilog-gen/lower_profile.py`) through `lower-diff` (tier by the seed's kind: `comb` arith/T, `ff` ff/D with `--neg-edge keep`, `latch` S, `mem` D) and `lower-equiv` (`--latches` for the latch kind). A `NOT A REFINEMENT` is printed `BUG` and exits 1, never listed; `UNPROVEN` and folded seeds are counted; `--expect tests/lower/gen_diff_expect.txt` lists tier-T histogram differences; `--min-compared N`, `--strict-unproven`. CTest `lower_gen_vs_yosys` (seeds 1..200) and `lower_gen_vs_yosys_long` (1..5000, nightly) in the sweep preset. |

`maps/` holds the declared-reference techmaps of the asynchronous flip-flops (spec §6.3, §14.2
tier D): `adffe2dff_reset_first.v` and `aldffe2dff_reset_first.v` are Parmys's `techlibs/` files
(VTR `3c9a4d23`, read-only under `external/`) with the enable order changed so that an active
asynchronous reset or load wins over the enable (PHASE3 #32 ruling 1), and `dffsre2dff.v` is
written in the same style (Parmys does not convert `$dffsre`). `dffsr2dff.v` is the same without the enable. Parmys's own `adff2dff.v` and
`aldff2dff.v` are used unmodified from `$VTR_ROOT/parmys/parmys-plugin/techlibs` (`--parmys-techlibs`); `--parmys-exact` also takes Parmys's unmodified `adffe2dff.v` and `aldffe2dff.v` (enable over reset, spec 16 row 4) in place of the reset-first maps. For the `ff` and `latch` families the lowered netlist holds storage bit cells, which `write_rtlil` rejects (1F): `lower-diff` writes only `lo.blif` on the Odin III side, reads both histograms from the BLIFs, and rewrites Yosys's `.subckt $_DFF_P_` as `.latch` before comparing.

Exit codes (one convention across the opt tools): 0 everything holds; 1 a difference; 2 usage or a
tool error; 3 hard (a child crashed, was killed by a signal or its memory cap, or timed out: never
absorbed by an expect list); 77 skipped (an ASan child and no `systemd-run`). Every child runs
through `memcap.run` (default cap 3 GiB for `lower-diff`'s Yosys). The work directory is made under
`$ODIN3_LOWER_WORK` (else `$TMPDIR`) and **is never removed**. Yosys other than 0.55 is refused.
