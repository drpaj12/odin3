# Provenance depth: calibration of the size estimate

Date 2026-11-13. Companion to `docs/specs/2026-11-13-prov-levels.md`. The raw table is
`docs/specs/2026-11-13-prov-levels-calibration.tsv` (sizes and design counts only, no host paths);
the fit is `tools/prov-calibrate/prov_fit.py`, which writes `src/passes/prov_depth_fit.h`:

    python3 tools/prov-calibrate/prov_fit.py docs/specs/2026-11-13-prov-levels-calibration.tsv \
        --header src/passes/prov_depth_fit.h --markdown ratios.md

## Result in one paragraph

The estimate is **within 2x of the real name-map size on 38 of 43 calibration designs at depth 1,
30 at depth 2, 27 at depth 3 and 25 at depth 4**; the worst ratios are 2.20, 2.78, 3.41 and 3.86.
The step files (depth 4) are within 2x on 12 of 43 designs, worst 9.4x. The target of decision 4
(within 2x on every calibration design) is therefore **met on no depth except by the tiny designs**
and is not met for the step files. The reasons, found in the data, are below. The estimate is
still useful for what `--prov-budget` needs (the order of magnitude, and a depth that is far too
big for a budget), but it is not accurate to a factor of two, and the doc says so rather than
tuning the table to one set of designs.

## Method

- Designs: 43, from `single_wire` (62 bytes of Verilog) to `aid_nl_B` (1.5 MB; 50 331 cells,
  1 990 LUTs), including `arm_core`, `bgm` and `aid_nl_B`. 28 are the designs of the mux
  near-critical runs (the configs of that run, read only) and 15 are micros of
  `tests/micro/verilog/micro`. `aid_nl_C`, `aid_nl_A` (over 15 GB at depth 4), and `LU64PEEng` and
  `mcml` (over 27 minutes for the depth 0 run alone, which is no calibration at all in a session)
  are not in the set.
- Each design runs through `odin3-flow --no-vpr --prov-level L` for L = 0 to 4 on the same
  Release binary (heavy-run, one design per call, a 10 GiB file-size cap, run roots outside the
  worktree, the bulk deleted after the sizes were recorded). The sizes are the bytes of the
  `<top>.blif.names` map (the name map) and of the `prov/` directory (the step files, written at
  depth 4), the number of `R` lines (records) and the parent ids on them (edges), the LUT count
  and the largest edge count of an abc over-budget WARNING.
- The counts of a design are measured at the end of `proc` of the same run (the INFO line `proc:
  provenance depth ...`) with the read keeping the construct table, over the **flat** design (each
  module's cells times how often it is instantiated below the top, so that a design of a thousand
  instances of one module is not read as one module).
- The LUT count (and so the netlist) is the same at every depth on all 43 designs; the BLIF of
  depths 2, 3 and 4 is byte-identical, and the BLIF of depth 1 equals it up to the hash names (a
  shared bulk record gives a LUT another hash).

## The model

For each depth and measure: `value = exp(scale + sum(exponent x ln(1 + count)))`, at most three
counts of the ten, exponents not below zero (a bigger design never writes less), chosen and fitted
to minimise the worst ratio on the table (least squares in log space, then reweighting). The fit
and its worst ratio (in-sample) and the worst ratio when each design is left out in turn (an
`inf` means the fit without that design has a negative exponent):

```text
depth 1 records    worst x1.92 leave-one-out xinf  exp(-0.582) * pins^0.243, arith^0.154, cons^0.676
depth 1 edges      worst x2.46 leave-one-out x2.68  exp(-1.359) * pins^0.464, logic^0.032, cons^0.664
depth 1 map_bytes  worst x2.20 leave-one-out x2.62  exp(5.254) * outs^0.401, mul^0.047, cons^0.579
depth 2 records    worst x2.17 leave-one-out x3.70  exp(-0.676) * pins^0.353, mul^0.067, cons^0.655
depth 2 edges      worst x6.75 leave-one-out xinf  exp(0.853) * pins^0.085, logic^0.579, cons^0.518
depth 2 map_bytes  worst x2.78 leave-one-out x5.85  exp(4.936) * pins^0.267, logic^0.155, cons^0.645
depth 3 records    worst x2.60 leave-one-out x6.97  exp(-0.399) * pins^0.271, mul^0.091, cons^0.761
depth 3 edges      worst x10.89 leave-one-out x59.08  exp(1.702) * regs^0.048, logic^0.943, cons^0.222
depth 3 map_bytes  worst x3.41 leave-one-out x4.24  exp(5.745) * regs^0.131, logic^0.287, cons^0.747
depth 4 records    worst x2.60 leave-one-out x7.13  exp(-0.399) * pins^0.271, mul^0.091, cons^0.761
depth 4 edges      worst x13.54 leave-one-out x67.74  exp(0.760) * pins^0.528, logic^0.677
depth 4 map_bytes  worst x3.86 leave-one-out x7.49  exp(6.195) * logic^0.478, mux^0.148, cons^0.471
depth 4 step_bytes worst x9.39 leave-one-out xinf  exp(7.661) * pins^0.225, logic^0.351, cons^0.561
```

Depths 2 to 4 are never estimated below the depth before them.

## Why not 2x

- **Edge budget overflow.** Seven designs (`arm_core`, `bgm`, `aid_tl_T1`, `aid_tl_T2`,
  `aid_nl_B`, `aid_bnn_M_f1`, `aid_bnn_M_f2`) need more than the default edge budget in abc at
  depths 2 and 3, so those depths write exactly the depth 1 map (one shared record, with the WARNING
  the older options print). `LU32PEEng` and `aid_bnn_S_par`, which are as large, do not overflow.
  Which side a design falls on follows the depth of its logic cones, which the cell counts do not
  show. The column `over_budget_edges` of the table gives the walk size the WARNING named.
- **Cone size.** `a-z80-modified` writes 4.8 MB at depth 1 and 32.8 MB at depth 2 and 91 MB at depth
  3 (6.8x and 19x), `aid_i3_df_p1` writes 1.2x and 1.2x. The records of a LUT list the cone it came
  from; the counts see the cells, not the cones.
- **Step files.** One JSON line per LUT lists a link for every construct in its ancestry, so the
  size follows the LUTs times the constructs in a cone: 1.1 KB per construct row for the micros,
  19 KB for `aid_nl_B`, 89 KB for `a-z80-modified`.
- **The LUT count is not a function of the word cells.** A non-negative linear fit of the LUT count
  on the ten counts is off by 18x (`LU32PEEng` makes 66 933 LUTs from 591 103 mux work).

What would help, and was not done (it needs the netlist after `lower` or abc, not the cells after
`proc`): run `prov-estimate` after `lower`, where the count of bit cells is within a small factor of
the LUT count. The estimate's measure is "the design as it is now", so it already follows the
design to that point; the calibration is of the end of `proc` because that is where the INFO line
and the budget decision are.

## Estimate against real, per design

`est/real` of the name map at each depth and of the step files at depth 4 (1.00 is exact; the
real sizes are in the TSV). The depth-1 column also gives the real map size in bytes.

| design | map1 real | est/real d1 | d2 | d3 | d4 | steps4 real | est/real |
|---|---|---|---|---|---|---|---|
| LU32PEEng | 16630049 | 0.58 | 0.36 | 0.32 | 0.77 | 524558499 | 0.37 |
| LU8PEEng | 5042580 | 1.00 | 0.65 | 0.60 | 1.08 | 142649258 | 0.60 |
| a-z80-modified | 4799760 | 1.05 | 0.36 | 0.30 | 0.30 | 3191055239 | 0.11 |
| adder_hard_block | 4439 | 0.76 | 0.54 | 0.64 | 0.64 | 18187 | 1.34 |
| aid_bnn_M_f1 | 7782427 | 0.46 | 0.99 | 0.99 | 0.26 | 1350277036 | 0.11 |
| aid_bnn_M_f2 | 22119731 | 0.53 | 1.32 | 1.87 | 1.09 | 1274736410 | 0.53 |
| aid_bnn_S_f1 | 3530501 | 0.65 | 1.10 | 1.50 | 2.34 | 22919492 | 3.99 |
| aid_bnn_S_f2 | 1740917 | 0.57 | 0.53 | 0.52 | 0.80 | 195465236 | 0.14 |
| aid_bnn_S_par | 21217593 | 0.72 | 1.62 | 3.29 | 3.50 | 84676803 | 9.39 |
| aid_i3_df_p1 | 863778 | 1.02 | 0.55 | 0.67 | 0.67 | 9989845 | 0.75 |
| aid_i3_df_p2 | 1017967 | 0.96 | 0.49 | 0.57 | 0.57 | 14583417 | 0.53 |
| aid_i3_df_p4 | 1337073 | 0.89 | 0.42 | 0.46 | 0.46 | 24325494 | 0.37 |
| aid_i3_df_p8 | 2009823 | 0.80 | 0.36 | 0.37 | 0.37 | 46664551 | 0.24 |
| aid_nl_B | 18377136 | 0.67 | 1.33 | 1.33 | 1.11 | 1970749527 | 0.15 |
| aid_tl_T1 | 14447384 | 2.20 | 2.71 | 2.94 | 2.51 | 293997728 | 3.50 |
| aid_tl_T2 | 4117986 | 1.94 | 2.32 | 2.32 | 1.87 | 24125713 | 9.39 |
| and_latch | 1400 | 1.12 | 1.00 | 1.81 | 1.81 | 5797 | 3.07 |
| arm_core | 3035050 | 0.87 | 1.17 | 1.50 | 0.70 | 449249015 | 0.15 |
| bgm | 4145958 | 1.09 | 1.11 | 1.32 | 1.76 | 439043727 | 0.28 |
| bm_DL_16_1_mux | 8164 | 1.46 | 1.21 | 1.17 | 1.43 | 25125 | 4.85 |
| bm_DL_2_cascaded_flip_flops | 880 | 1.66 | 1.41 | 2.46 | 2.46 | 4578 | 3.05 |
| bm_DL_4_16_encoder | 3711 | 2.20 | 1.74 | 1.74 | 1.74 | 25990 | 2.20 |
| bm_DL_4_bit_comparator | 2400 | 1.71 | 1.31 | 1.62 | 1.62 | 10951 | 3.07 |
| bm_DL_74381_ALU | 10591 | 0.64 | 0.70 | 0.76 | 1.28 | 55720 | 2.15 |
| bm_DL_BCD_7_segment_without_x | 2818 | 1.68 | 1.92 | 1.92 | 2.19 | 17183 | 3.33 |
| bm_DL_BCD_adder | 10354 | 0.56 | 0.36 | 0.36 | 0.36 | 49618 | 0.72 |
| bm_base_multiply | 50954 | 0.52 | 0.38 | 0.50 | 0.50 | 203950 | 1.68 |
| bm_dag3_log | 2670 | 1.81 | 2.10 | 3.40 | 3.42 | 9798 | 9.39 |
| bm_expr_all_mod | 118736 | 0.46 | 0.54 | 0.68 | 0.68 | 598697 | 2.65 |
| bm_functional_test | 69146 | 0.52 | 0.46 | 0.50 | 0.97 | 497145 | 1.28 |
| bm_stmt_all_mod | 14062 | 1.26 | 1.76 | 2.26 | 2.26 | 31214 | 7.89 |
| bm_tester | 7552 | 0.96 | 0.96 | 0.94 | 0.94 | 23188 | 3.42 |
| boundtop_nolatches | 548135 | 1.93 | 2.78 | 3.41 | 3.86 | 2588611 | 8.20 |
| ch_intrinsics_nolatches | 40045 | 1.77 | 1.57 | 1.52 | 2.12 | 333391 | 2.90 |
| multiclock_output_and_latch | 11805 | 0.62 | 0.50 | 0.67 | 0.67 | 35186 | 2.01 |
| multiclock_reader_writer | 26419 | 0.56 | 0.44 | 0.45 | 0.45 | 84133 | 1.47 |
| multiclock_separate_and_latch | 3395 | 1.02 | 0.91 | 1.50 | 1.50 | 10657 | 3.44 |
| multiply_hard_block | 1662 | 1.51 | 1.06 | 1.21 | 1.21 | 9541 | 1.97 |
| muxtest | 4020 | 1.27 | 1.02 | 1.29 | 1.37 | 21705 | 2.70 |
| raygentop_nolatches | 1166722 | 0.98 | 0.65 | 0.79 | 0.79 | 8521195 | 1.35 |
| single_ff | 586 | 1.90 | 1.53 | 2.76 | 2.76 | 3807 | 2.77 |
| single_wire | 276 | 2.04 | 1.65 | 3.19 | 3.41 | 3046 | 1.94 |
| spree | 366343 | 1.93 | 1.45 | 2.05 | 2.67 | 3654036 | 3.92 |


Seconds and memory of the runs are not part of the table. Depth 4 is the expensive one: `aid_bnn_M_f2`
took 514 s against 54 s at depth 0 and wrote 1.27 GB of step files.
