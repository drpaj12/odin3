# Switching activity (vectorless)

Power estimation needs, for every net, the static probability `P1` (the fraction of cycles at 1) and the
transition density `D` (changes per cycle of the reference clock; a clock is 2). VPR reads them as a `.act`
file, one `name P1 D` line per net. ACE 2.0 produces that file by simulating 5000 random cycles. The `activity`
pass computes the same two numbers directly on the netlist view, with no vectors and no random numbers, and
keeps them as a per-net property next to `scoap` and the graphlet signatures. The full method, defaults and
approximations are in `docs/specs/2026-11-08-activity-design.md`.

## Running it

```text
odin3 -p "read_techlib lib/vtr.o3lib; read_verilog uart.v; proc; opt; lower; abc --lut 6; activity --out uart.act --json uart.json"
odin3: info: activity: 1 module, 420 cells, 96 flip-flops (76 in feedback), 265 iterations, converged, 2 ms
```

Run it on the netlist view (after `lower`, `abc`; no processes). It works on the top module; `flatten`
first for a design with instances (an instance is an opaque cell).

| Option | Meaning |
| --- | --- |
| `--pi-prob P`, `--pi-density D` | Primary-input statistics, default 0.5 and 0.2 (the ACE 2.0 defaults). `D` may not exceed `2 * min(P, 1 - P)`. |
| `--pi-file F` | Lines `name P1 D` for named primary inputs (ACE `.act` input format); the rest keep the defaults. |
| `--vcd F [--vcd-scope S] [--vcd-clock C]` | Measure the inputs from a waveform (`sim_tb --vcd`): each input is sampled just before every rising edge of the clock `C`; `P1 = ones / samples`, `D = changes / (samples - 1)`. |
| `--clock NAME` | The reference clock input. Default: the first input that drives a clock pin. Other clock inputs also get `(0.5, 2)` with a WARNING. |
| `--ff-start P` | Start value of the flip-flop fixpoint (default 0.5). |
| `--tol E`, `--max-iter N` | Stopping rule of the fixpoint: largest change of any `P1` in a feedback component below `E` (default 1e-6) or `N` sweeps (default 1000, with Aitken extrapolation every 10 sweeps and damping when the change stops shrinking); hitting the cap logs a WARNING and sets `converged` false. |
| `--exact K` | Widest function treated exactly (default and maximum 16 inputs); a wider cover is opaque. |
| `--opaque-prob P`, `--opaque-density D`, `--opaque-type NAME` | Outputs of opaque cells (black boxes, memories, instances, anything without a function table) act as inputs with these statistics (default the input defaults). `--opaque-type adder` forces a cell type opaque: ACE 2.0 reads a hard `adder` as a box whose outputs are primary inputs, so use it to compare with ACE. |
| `--out F.act` | The VPR activity file, one line per net, under the names `write_blif --vtr` writes for the same `--name-style`. |
| `--name-style short\|provenance\|hash` | Net names (default `short`; `hash` for the Bifrost/Heimdall flow, the names `write_blif --name-style hash` writes). |
| `--json F` | Schema `odin3-activity`, version 1: per net `id` (the module-local net id, the key to provenance), `name`, `p1`, `d`, `kind` (`pi`, `clock`, `const`, `comb`, `ff`, `opaque`, `undriven`); top level `source` (`odin3-vectorless`, the label Heimdall reads), `inputs`, `iterations`, `converged`, `ff_in_loops`, `cone_capped`, `opaque_cells`, `wide_cells`, `latches`, `ms`. |
| `--no-attrs` | Do not write the module attributes `odin3:activity_*` (version, nets, cells, opaque and wide cells, iterations, converged, netlist hash, input mode). |

Python: `Design.activity(pi_prob=0.5, pi_density=0.2, pi_file=None, vcd=None, vcd_clock=None, clock=None,
ff_start=0.5, name_style="short", act=None)` returns the JSON as a dict.

## The model in one page

Each net is the pair of its values in two consecutive cycles, stationary, with joint
`[[1 - P1 - D/2, D/2], [D/2, P1 - D/2]]` (so `D <= 2 min(P1, 1 - P1)`). A node `y = f(x1..xk)` takes its
inputs as independent, and `P1(y) = sum f(a) prod pi_i(a_i)`, `D(y) = 2 (P1(y) - E11)` where `E11` is the
probability that `y` is 1 in both cycles. This is exact for up to 16 inputs, computed in `O(k 2^k)`. Simultaneous
switching is accounted for: XOR of two `(0.5, 0.2)` inputs has `D = 0.32`, not 0.4.

Flip-flops and latches are cells whose next state `g(D, enable, reset, Q)` depends on the state. Outside a feedback loop a
flip-flop output has exactly the statistics of its data input (a shift register keeps `D = 0.2`). Inside a feedback component
the probabilities are iterated to a fixpoint, and the density of a state bit is `P(next != state)` computed with the state held at 0 and at 1
and the logic between state and next-state recomputed under that condition (a counter has `D = 1, 1/2, 1/4, ...`; a toggle
flip-flop has `D = 1`). A cone larger than 2048 cells is not recomputed: that state bit uses its next-state probability as if independent of
its state, and `cone_capped` counts them.

## What it does not know

- Spatial correlation: the inputs of a node are independent. Reconvergent fanout is the main source of error against simulation.
- Reachable states: a one-hot machine or a counter that a testbench never leaves has fixed bits that the pass cannot see (it starts the
  fixpoint at `--ff-start`, not at the reset state). A testbench-driven activity must come from simulation.
- Glitches (like ACE 2.0 and the cycle simulator), primary-input bus correlations, multi-clock domains (all densities per cycle of one clock).
- Engine: analytic only. The interface leaves room for a later `sim:N` engine (random-vector simulation through `sim`, as ACE 2.0 does) behind the same options and outputs.

## Results

Measured with `tools/activity/act_check.py` on the 51 designs of `wyrd power run` (`work/power-runs`: VTR and VerilogEval micros, OpenCores,
sha3, DES, FFT, vga_lcd, dblclockfft), netlists as ACE 2.0 read them (6-LUT netlists from the `odin3-flow` synthesis, hard adders). The
run times are wall clock on a loaded machine (other jobs were running), including reading the BLIF; ACE 2.0 is the VTR build (commit 3c9a4d23b2).

**Against ACE 2.0** on the same netlist and the same input statistics (0.5, 0.2), in ACE's own configuration (`--opaque-type adder --ff-start 0`;
ACE reads an `adder` as a box with primary-input outputs and starts every flip-flop at 0): P1 mean absolute error 0.025 (median 0.011), density mean
absolute error 0.054 (median 0.038). Density differs systematically: ACE's density of a combinational node is below the exact value of the model
(its fanin switching probabilities are divided by a depth factor `0.4 * depth` and the BDD recursion handles simultaneous switching differently).

**Against a Monte Carlo of the same model** (`tools/activity/act_mc.py`: independent two-state Markov chains on the inputs, 30000 to 100000 cycles,
34 designs of up to 640 nets, adders propagated): the density mean absolute error is 0.008 for `activity` against 0.096 for ACE 2.0 on 21
combinational designs, and 0.024 against 0.055 on 13 sequential ones; `activity` is closer in 32 of 34 designs for density and 31 of 34 for P1.
The exceptions are `ca_prng` and `Prob085_shift4` (density), `ca_prng`, `i2cslave` and `bm_DL_4_bit_comparator` (P1): state-correlated sequential logic, see "What it does not know".

**Against `sim_tb` activity** (the design's own testbench through the netlist; primary inputs given to `activity` from the simulation with
`--pi-file`, or the defaults). The reference is one run of a testbench that leaves most state at its reset value (170 of 414 nets of mmuart never
change), which the analytic model cannot know:

| design | activity file | nets | P1 r | P1 MAE | D r | D MAE | D bias | VPR total power (mW) |
|---|---|---|---|---|---|---|---|---|
| mmuart | simulation (reference) | - | - | - | - | - | - | 2.07 |
| mmuart | activity, inputs measured from the simulation, flip-flops start 0 | 397 | 0.523 | 0.257 | 0.853 | 0.0321 | +0.0181 | 2.36 |
| mmuart | same, flip-flops start 0.5 | 397 | 0.616 | 0.219 | 0.852 | 0.0336 | +0.0196 | - |
| mmuart | activity, default inputs (0.5, 0.2), start 0 | 397 | 0.657 | 0.214 | 0.712 | 0.0375 | +0.0113 | 2.60 |
| mmuart | ACE 2.0 (default inputs) | - | - | - | - | - | - | 3.06 |
| wb_lcd | simulation (reference) | - | - | - | - | - | - | 5.21 |
| wb_lcd | activity, inputs measured from the simulation, flip-flops start 0 | 1888 | 0.608 | 0.127 | 0.709 | 0.0163 | +0.0161 | 6.44 |
| wb_lcd | same, flip-flops start 0.5 | 1888 | 0.271 | 0.370 | 0.701 | 0.0170 | +0.0167 | - |
| wb_lcd | activity, default inputs (0.5, 0.2), start 0 | 1888 | 0.284 | 0.371 | 0.540 | 0.0220 | +0.0210 | 8.34 |
| wb_lcd | ACE 2.0 (default inputs) | - | - | - | - | - | - | 7.89 |
| verilog_cordic_core | simulation (reference) | - | - | - | - | - | - | 28.97 |
| verilog_cordic_core | activity, inputs measured from the simulation, flip-flops start 0 | 3653 | 0.433 | 0.117 | 0.475 | 0.1733 | +0.1492 | 42.28 |
| verilog_cordic_core | same, flip-flops start 0.5 | 3653 | 0.433 | 0.117 | 0.475 | 0.1733 | +0.1492 | - |
| verilog_cordic_core | activity, default inputs (0.5, 0.2), start 0 | 3653 | 0.218 | 0.226 | 0.154 | 0.1559 | +0.0240 | 34.59 |
| verilog_cordic_core | ACE 2.0 (default inputs) | - | - | - | - | - | - | 19.36 |

Power through VPR `--power` (one placement per design, channel width 100, `k6_frac_N10_frac_chain_mem32K_40nm`, PTM 45 nm): the total with the `activity`
file in ACE's configuration is 1.14 times the total with the ACE 2.0 file (geometric mean of 44 designs, range 1.02 to 1.42; the combinational designs
differ most, because of the density difference above). Against simulation: mmuart 2.07 mW (simulation), 2.36 (`activity`, measured inputs), 2.60
(`activity`, default inputs), 3.06 (ACE 2.0); wb_lcd 5.21, 6.44, 8.34, 7.89; verilog_cordic_core 28.97, 42.28, 34.59, 19.36.

Run time and memory: on 7 designs of 7000 to 108000 nets `activity` takes 12 to 520 ms and 18 to 93 MB, ACE 2.0 2.7 to 40 s and 55 to 288 MB.
A component of feedback logic that does not reach the tolerance in `--max-iter` sweeps is reported (WARNING, `converged` false): `wb_lcd`, `sdr_ctrl` and `vga_lcd`
have such components; their values are those of the last sweep.

Per design against ACE 2.0 (ACE-like configuration), and against the Monte Carlo where it was run:

| design | nets | P1 r | P1 MAE | D r | D MAE | D MAE vs MC (ours / ACE) | ACE s / MB | activity s / MB | power ours/ACE |
|---|---|---|---|---|---|---|---|---|---|
| Prob010_mt2015_q4a | 3 | 0.999 | 0.0104 | 1.000 | 0.0476 | 0.001 / 0.048 | 0.03 / 40 | 0.01 / 11 | 1.06 |
| Prob011_norgate | 3 | 0.999 | 0.0130 | 1.000 | 0.0469 | 0.001 / 0.047 | 0.02 / 40 | 0.01 / 11 | 1.06 |
| Prob012_xnorgate | 3 | n/a | 0.0119 | -1.000 | 0.0811 | 0.001 / 0.082 | 0.02 / 40 | 0.01 / 11 | 1.10 |
| bm_DL_2_1_mux | 4 | n/a | 0.0083 | -1.000 | 0.0606 | 0.001 / 0.060 | 0.02 / 40 | 0.01 / 11 | 1.09 |
| bm_DL_D_flipflop | 3 | n/a | 0.0206 | 0.000 | 0.0359 | 0.001 / 0.027 | 0.02 / 40 | 0.01 / 11 | 1.00 |
| bm_my_D_latch1 | 4 | 0.998 | 0.0060 | 1.000 | 0.0361 | 0.001 / 0.035 | 0.02 / 40 | 0.01 / 11 | 1.06 |
| bm_DL_Dff_w_synch_reset | 4 | 1.000 | 0.0055 | 0.663 | 0.0364 | 0.000 / 0.029 | 0.02 / 40 | 0.01 / 11 | 1.00 |
| bm_DL_2_4_encoder | 7 | 1.000 | 0.0058 | 0.942 | 0.0425 | 0.002 / 0.043 | 0.02 / 40 | 0.01 / 11 | 1.10 |
| bm_dag1_log | 8 | 0.998 | 0.0086 | 0.997 | 0.0366 | 0.001 / 0.036 | 0.02 / 40 | 0.01 / 11 | 1.10 |
| bm_dag2_log | 8 | 0.998 | 0.0078 | 0.997 | 0.0381 | 0.001 / 0.038 | 0.02 / 41 | 0.01 / 11 | 1.10 |
| Prob025_reduction | 10 | n/a | 0.0101 | -0.995 | 0.0915 | 0.001 / 0.092 | 0.02 / 41 | 0.01 / 11 | 1.14 |
| Prob024_hadd | 10 | n/a | 0.0096 | n/a | 0.0237 | 0.073 / 0.172 | 0.02 / 51 | 0.01 / 11 | 1.04 |
| bm_DL_4_bit_comparator | 13 | 0.984 | 0.0156 | -0.424 | 0.0811 | 0.005 / 0.077 | 0.02 / 41 | 0.01 / 11 | 1.16 |
| bm_if_reset | 13 | 0.999 | 0.0064 | 0.885 | 0.0281 | 0.001 / 0.026 | 0.02 / 40 | 0.01 / 11 | 1.00 |
| bm_DL_4_bit_shift_register | 14 | n/a | 0.0127 | -0.107 | 0.0734 | 0.004 / 0.068 | 0.02 / 40 | 0.01 / 11 | 0.99 |
| Prob085_shift4 | 15 | 0.999 | 0.0070 | 0.493 | 0.0723 | 0.039 / 0.035 | 0.02 / 41 | 0.01 / 11 | 1.12 |
| Prob009_popcount3 | 19 | n/a | 0.0112 | n/a | 0.0154 | 0.017 / 0.132 | 0.03 / 51 | 0.01 / 11 | 1.04 |
| Prob150_review2015_fsmonehot | 21 | 0.990 | 0.0130 | 0.110 | 0.0565 | 0.001 / 0.056 | 0.02 / 41 | 0.01 / 11 | 1.14 |
| bm_DL_behavioural_full_adder | 19 | n/a | 0.0112 | n/a | 0.0154 | 0.017 / 0.132 | 0.03 / 52 | 0.01 / 11 | 1.03 |
| Prob016_m2014_q4j | 25 | n/a | 0.0121 | n/a | 0.0235 | 0.001 / 0.117 | 0.03 / 51 | 0.01 / 11 | 1.10 |
| bm_DL_16_1_mux | 27 | n/a | 0.0113 | -0.933 | 0.0775 | 0.001 / 0.077 | 0.03 / 41 | 0.01 / 11 | 1.09 |
| Prob043_vector5 | 31 | -0.999 | 0.1989 | 0.396 | 0.1549 | 0.001 / 0.155 | 0.02 / 41 | 0.01 / 11 | 1.60 |
| bm_DL_74381_ALU | 51 | 0.938 | 0.0134 | -0.815 | 0.0707 | 0.007 / 0.138 | 0.03 / 52 | 0.01 / 11 | 1.25 |
| Prob123_bugs_addsubz | 56 | 0.986 | 0.0119 | -0.264 | 0.0592 | 0.010 / 0.145 | 0.03 / 52 | 0.01 / 11 | 1.14 |
| bm_DL_BCD_adder | 57 | 0.890 | 0.0106 | -0.735 | 0.0298 | 0.026 / 0.156 | 0.03 / 52 | 0.01 / 11 | 1.13 |
| Prob141_count_clock | 140 | 0.999 | 0.0071 | 0.958 | 0.0101 | 0.006 / 0.104 | 0.05 / 52 | 0.01 / 11 | 1.04 |
| mips_16 | 179 | 0.975 | 0.0263 | 0.537 | 0.0587 | 0.046 / 0.085 | 0.06 / 53 | 0.01 / 11 | 1.20 |
| ca_prng | 242 | 0.887 | 0.0755 | -0.280 | 0.2053 | 0.120 / 0.088 | 0.12 / 41 | 0.01 / 12 | 1.56 |
| i2cslave | 375 | 0.958 | 0.0460 | 0.829 | 0.0170 | 0.013 / 0.029 | 0.10 / 53 | 0.01 / 11 | 1.12 |
| ch_intrinsics | 382 | 0.916 | 0.0423 | 0.869 | 0.0266 | - | 0.09 / 53 | 0.01 / 12 | 0.99 |
| mmuart | 410 | 0.995 | 0.0110 | 0.843 | 0.0292 | 0.005 / 0.079 | 0.10 / 53 | 0.01 / 12 | 1.13 |
| simple_spi | 413 | 0.883 | 0.0336 | 0.932 | 0.0110 | 0.024 / 0.044 | 0.11 / 53 | 0.01 / 12 | 1.06 |
| i2c | 628 | 0.988 | 0.0222 | 0.720 | 0.0430 | 0.041 / 0.047 | 0.22 / 54 | 0.01 / 12 | 1.22 |
| sdram_16bit | 636 | 0.998 | 0.0089 | 0.811 | 0.0341 | 0.007 / 0.047 | 0.17 / 54 | 0.01 / 12 | 1.09 |
| bm_lpm_all | 640 | 0.976 | 0.0108 | -0.493 | 0.0980 | 0.004 / 0.172 | 0.19 / 54 | 0.01 / 11 | 1.36 |
| mips_16_noxarm | 947 | 0.993 | 0.0213 | 0.542 | 0.0450 | - | 0.33 / 54 | 0.01 / 12 | 1.02 |
| spi | 1006 | 0.995 | 0.0091 | 0.789 | 0.0304 | - | 0.37 / 54 | 0.01 / 12 | 1.21 |
| uart2spi | 1354 | 0.911 | 0.0639 | 0.540 | 0.0505 | - | 0.50 / 56 | 0.01 / 12 | 1.33 |
| wb_lcd | 1903 | 0.510 | 0.2206 | 0.834 | 0.0144 | - | 0.56 / 56 | 0.01 / 12 | 1.05 |
| sha | 2860 | 0.908 | 0.0444 | 0.543 | 0.0590 | - | 0.86 / 59 | 0.01 / 13 | 1.37 |
| attiny_atmega_xmega_core | 2943 | 0.997 | 0.0110 | 0.750 | 0.0331 | - | 1.12 / 58 | 0.03 / 13 | 1.11 |
| verilog_cordic_core | 3665 | 0.786 | 0.0408 | -0.056 | 0.0593 | - | 0.76 / 61 | 0.01 / 13 | 1.17 |
| sdr_ctrl | 3691 | 0.925 | 0.0463 | 0.675 | 0.0373 | - | 1.61 / 60 | 0.02 / 14 | 1.25 |
| des | 5454 | 0.975 | 0.0082 | 0.230 | 0.1878 | - | 2.29 / 50 | 0.02 / 15 | 1.42 |
| sha3_low | 7316 | 1.000 | 0.0043 | 0.132 | 0.0297 | - | 2.72 / 55 | 0.08 / 18 | - |
| sha3_high | 10105 | 1.000 | 0.0205 | 0.206 | 0.0382 | - | 4.88 / 61 | 0.51 / 21 | - |
| reed_solomon_decoder | 10998 | 0.999 | 0.0064 | 0.850 | 0.0230 | - | 3.38 / 76 | 0.04 / 20 | - |
| usbhostslave | 18684 | 0.994 | 0.0053 | 0.722 | 0.0115 | - | 6.25 / 90 | 0.08 / 28 | - |
| pipelined_fft_256 | 21318 | 0.919 | 0.0286 | 0.159 | 0.1067 | - | 5.58 / 100 | 0.08 / 28 | - |
| vga_lcd | 42069 | 0.966 | 0.0113 | 0.424 | 0.0233 | - | 16.92 / 136 | 0.67 / 49 | - |
| dblclockfft | 108036 | 0.992 | 0.0133 | -0.136 | 0.1203 | - | 39.66 / 288 | 0.52 / 93 | - |


## Provenance

The JSON `id` of a net is the module-local net id; with `--name-style hash` the name is the one in the version 2 name map, so a tool joins activity to the
HDL line through the existing chain. `odin3-prov-steps FLOW_DIR --activity F.act` (`F.act` from `odin3 activity --name-style hash --out F.act` on the same
BLIF) adds `attrs.act_p1`, `attrs.act_density` and `attrs.act_source` (`odin3-vectorless`, or `--activity-source LABEL`) to the `luts` objects of the run it
names ({doc}`provenance-files`); `prov_steps.fpga_steps(..., activity={name: (p1, density)})` does the same from Python. The attributes are free-form: no
schema changes, and an object the file does not name has none.
