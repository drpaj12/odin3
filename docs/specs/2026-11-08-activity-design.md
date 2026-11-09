# activity pass: native vectorless switching-activity estimation on the netlist view (design, 2026-11-08)

Status: spec first, code follows on branch `feat/activity` (v1.6) after the controller has reviewed this
file. Brief: `odin3-activity-v16`. Purpose: hub B Q1.2 (power); Odin III goal 2 (graph and
reverse-engineering research platform: activity is a per-net property, graph-generic, like `scoap`) and
goal 3 (activity per net joined back to the HDL line through provenance). Peter 2026-11-08: "an ACE like
tool from the netlist perspective". Measures: per design, correlation and mean absolute error of static
probability and transition density against (a) ACE 2.0 on the same netlist with the same input
statistics and (b) testbench simulation (`sim_tb --vcd-all`); total power difference through VPR
`--power`; run time and peak memory against ACE 2.0 (section 8).

## 1. What was read, and what was not

Read (VTR checkout in `external/vtr-verilog-to-routing`, ACE 2.0 in `ace2/`):

- `ace2/ace.c`, `sim.c`, `bdd.c`, `io_ace.c`, `depth.c`, `cycle.c`, `ace.h`; `vpr/src/base/read_activity.cpp`;
  `doc/src/vtr/power_estimation/index.rst`.
- What ACE 2.0 actually does (this differs from a purely analytic tool):
  1. Primary-input statistics `(p, d)` default to `p = 0.5` (`ACE_PI_STATIC_PROB`), `d = 0.2`
     (`ACE_PI_SWITCH_PROB`), set by `-p`/`-d`, or read from a `.act` (`-a`) or a vector file (`-v`: p = ones/vectors,
     d = toggles/vectors). The clock PI (`-c`) is `p = 0.5`, density 2.
  2. Stage 1 is a simulation: 5000 cycles (`ACE_NUM_VECTORS`), each PI a two-state Markov chain with
     P(0 to 1) = `0.5*d/(1-p)` and P(1 to 0) = `0.5*d/p` from `rand()` (seed `-s`), flip-flops updated every cycle,
     all values start at 0. Static probability of every net is `ones / 5000`; its switching probability
     `toggles / 5000`. So ACE's static probability is a sampled value with sampling noise.
  3. Stage 4 recomputes the density of every logic node (not the flip-flop outputs, which keep the simulated
     toggle rate) from a BDD of the node function: the fanin transition probabilities
     `p01 = 0.5*switch/(1-p)`, `p10 = 0.5*switch/p` (switch = the simulated toggle rate of the fanin divided by
     `d_depth = max(1, int(0.4 * node_depth))`), summed over the paths of the BDD, times 2 and times `d_depth`.
     The depth factor is a heuristic of the tool; nothing in the code says why 0.4.
  4. Output: one line `name static_prob switch_act` per net under the names of the rewritten BLIF; VPR reads
     `<net> <signal probability> <transition density>` (`read_activity.cpp`), warns about extra names and
     stops with a fatal error when a BLIF net has no line.
- Lamoureux and Wilton, "Activity estimation for field-programmable gate arrays", FPL 2006
  (doi 10.1109/FPL.2006.311199) is the reference the VTR documentation gives for ACE 2.0. The paper itself was
  NOT available to this lane; nothing below is attributed to it. Najm's transition density (IEEE TCAD 1993) was also
  not read; the formulas of section 3 are derived here and checked by enumeration (section 9), not quoted.

## 2. Definitions

Per net (one bit), in units of the reference clock cycle:

- `P1`: static probability, long-run fraction of cycles at 1.
- `D`: transition density, expected number of value changes between two consecutive cycles (glitch-free:
  values are sampled once per cycle, as ACE and the cycle simulator do). A clock net has `P1 = 0.5`, `D = 2`
  (VTR's definition; a net sampled per cycle cannot exceed 1, a clock is the one exception and is marked).
- Two-frame model: a net is the pair `(v(t), v(t+1))`, stationary, with joint
  `J = [[1-P1-D/2, D/2], [D/2, P1-D/2]]`. Valid iff `D <= 2*min(P1, 1-P1)`; the pass clamps rounding error of at
  most 1e-9 and fails (ERROR naming the net) on anything larger, as ACE asserts the same inequality. Every
  result is checked against it.

## 3. Method

The netlist view after `abc`/`lower`: `$lut`, bit gates (`$_AND_`, `$_MUX_` ...), `$sop`, tech-library gates and
hard cells with `fn` lines (`adder`), flip-flops and latches. Registry flag `processes = REFUSE`, as `scoap`.
Model building follows `src/passes/scoap/sc_model.c`: each node becomes a function cell (truth table over its
distinct input nets, built with `odin3_gate_truth` for gates, the table of a `$lut`, the cover of a `$sop`; a
hard cell with `fn` lines gets one table per output by running its simulate hook on every assignment).

### 3.1 Combinational node, exact up to K inputs

Inputs of one node are taken as spatially independent (DEFAULT, section 10 lists it as the main approximation).
For `y = f(x1..xk)`, `pi_i(a)` the marginal and `J_i` the pair joint of input `i`:

    P1(y) = sum_a f(a) * prod_i pi_i(a_i)
    E11   = sum_{a,b} f(a) f(b) * prod_i J_i(a_i, b_i)      (probability y is 1 in both frames)
    D(y)  = 2 * (P1(y) - E11)                                (P(y(t) != y(t+1)) = 2 P1 - 2 E11 at stationarity)

`E11 = f^T (J_1 x ... x J_k) f` is computed by applying the 2x2 matrix `J_i` along each input dimension in turn
(mode products): `O(k 2^k)` instead of `O(4^k)`. Exact for `k <= 16` (`--exact K`, default 16; a 6-LUT costs
about 400 multiply-adds). Duplicate pins of one node are merged first (XOR(a,a) is the constant 0), as `scoap`
does. This reproduces the two-input values worked by hand in section 9 (AND of two `(0.5, 0.2)` inputs:
`P1 = 0.25`, `D = 0.18`; XOR: `P1 = 0.5`, `D = 0.32 = 0.2 + 0.2 - 2*0.2*0.2`, which a first-order Boolean-difference
estimate `D = sum P(df/dx_i) D_i` would give as 0.4: the pair model accounts for simultaneous switching).

A node with more than K distinct inputs (only a wide `$sop`; abc output never has one) is not expanded: it is
treated as opaque (3.4), counted in `wide_cells` and logged, never skipped silently. DEFAULT; a BDD or cube
method for wide covers is an open gap (section 10), not needed by `abc --lut 6` output.

### 3.2 Primary inputs and clocks

`--pi-prob P` (default 0.5), `--pi-density D` (default 0.2), the ACE defaults. `--pi-file F`: lines
`name P1 D` (ACE `.act` input format) for named PIs, the rest default. `--vcd F [--vcd-scope S] [--vcd-clock C]`:
reads a waveform, samples every PI at each rising edge of the clock (the end-of-step values `sim_tb --vcd` writes),
`P1 = ones/samples`, `D = changes/(samples-1)`; the same estimator as ACE `-v` (which divides by the vector count; the
difference is one sample, noted in the JSON). PIs with `D > 2 min(P1,1-P1)` from a file are ERROR.
`--clock NAME`: the reference clock PI (default: the only PI that drives a clock pin; none or several is a
WARNING and the first in port order). The reference clock is `(0.5, 2)`. Other PI clocks also get `(0.5, 2)` with
a WARNING (multi-clock density is relative to one reference; open gap). A net that is a clock pin driver but not a PI
(divided or gated clock) is propagated like any data net and also listed under `clock_nets` in the JSON; downstream
data nets in its domain are counted per reference cycle (the same convention ACE has, single `-c`).
Clock nets are written in the `.act` with their `(0.5, 2)` (VPR needs a line for every net).

### 3.3 Flip-flops, latches, and the fixpoint

A storage cell has a next-state function `g(D, enable, set/reset, ..., Q)` over its non-clock inputs and its own
state `Q`, taken from the registry description of the type (the flip-flop families, `$dlatch`; the
same cells `scoap --sequential` models; an asynchronous set/reset is a synchronous override of `Q`, DEFAULT).
A level-sensitive latch is the same cell with its enable (DEFAULT, counted in `latches`). Cells whose next state
the pass cannot tabulate (arrays, black boxes) are opaque (3.4).

Static probability: Gauss-Seidel fixpoint `P1(Q) <- P1(g(inputs))`, nets in topological order of the
combinational graph with `Q` as sources. Start value of every `Q` is `--ff-start P` (default 0.5; `init` uses the
cell's INIT attribute when 0/1; ACE starts every value at 0 but runs 5000 cycles). Only strongly connected
components of the flip-flop graph (a flip-flop reaches its own `D` through combinational logic and other flip-flops)
are iterated; the rest of the netlist is one forward pass, so cost is one pass plus the iterations of the SCCs.
Stopping rule: stop when `max |dP1| < --tol` (default 1e-6) over the SCC, or after `--max-iter` (default 1000) sweeps;
hitting the cap logs a WARNING with the last delta and sets `converged = false` for the module (the values are
those of the last sweep, with damping `x <- (x_new + x_old)/2` from sweep 100 on to break a 2-cycle). Absorbing states
(`Q <- Q & x`) converge geometrically to the start-dependent limit; the start value is therefore an option and is
logged.

Density of `Q`, two rules:

- `Q` not in a feedback SCC (a pipeline stage, shift register): `pair(Q) = pair(D)`; `D(Q) = D(D-net)`, `P1(Q) = P1(D-net)`
  exactly, because `Q(t+1) = D(t)`. A shift register of `n` stages from a `(0.5, 0.2)` input keeps `D = 0.2` at every stage.
- `Q` in a feedback SCC (a counter, an FSM state bit, an enable register that reads itself): the pair model
  cannot see that `Q(t)` is a function of `Q(t-1)` (a toggle flip-flop `Q <- ~Q` would stay at its start density
  forever: any density is a fixpoint of `D(~x) = D(x)`). Instead
  `D(Q) = P( g(x) != Q )` with the cell's inputs in one frame, independent with their current marginals (including `Q`
  itself with `P1(Q)` and the other flip-flops of the SCC). This is exact when the inputs of `g` are independent
  bits: toggle flip-flop 1.0; a binary counter, bit `k` has `D = 2^-k` with `P1 = 0.5` at every bit, matching the
  true values; an enable register `Q <- en ? d : Q` gives `D = P(en) * P(d != Q)`. Its approximation: a primary input or
  other net feeding `g` is taken at its marginal, so the temporal correlation of the input with the previous
  state is lost. Iterated with the `P1` fixpoint under the same stopping rule (both deltas).

### 3.4 Opaque cells and constants

Constants: `P1 = 0` or `1`, `D = 0`; an x/z constant is `(0.5, 0)` with a WARNING. Black boxes, hard cells with no
function table (memories, multipliers over the table cap), module instances (flatten first, as `scoap`), tristate
and anything the pass cannot tabulate: every output net is a pseudo primary input with `--opaque-prob`/`--opaque-density`
(default the PI defaults), the inputs are ignored. Counted in `opaque_cells`, listed by type in the log. The comparison
runs of section 8 use the flow of `wyrd power` (adders left soft), so ACE and this pass see the same cells.
An undriven net reads constant 0 (as the simulator does) and is counted.

## 4. The pass

    activity [--pi-prob P] [--pi-density D] [--pi-file F] [--vcd F [--vcd-scope S] [--vcd-clock C]]
             [--clock NAME] [--ff-start P|init] [--tol E] [--max-iter N] [--exact K]
             [--opaque-prob P] [--opaque-density D]
             [--out F.act] [--name-style short|provenance|hash [--name-map F]] [--json F] [--no-attrs]

Netlist view (no processes) to the same design; only attributes change. It runs on the top module (flatten first for
a design with instances; a module that has instances is refused with the `flatten` hint, as `scoap` does for its
whole-design value).

- `--out F.act` writes the VPR file: one line `name P1 D` per net of the top module that `write_blif --vtr` writes,
  under the name `write_blif --vtr` gives it for the same `--name-style` (the naming run `odin3_blif_vtr_name_top`
  through `sim_netnames`, which `sim_tb --vcd-all` already uses, so waveform, BLIF and `.act` agree). Hash names:
  the name map is the one `write_blif --name-style hash` writes; `--name-map F` names it (the pass does not write one).
  Fixed `%.9g` formatting. Clock nets and PIs are included; unconnected pins (`unconn`) are not. A net that the BLIF
  writer drops (unused) is not listed.
- `--json F`: `odin3-activity` version 1: per module the nets `{"id","name","p1","d","kind"}` (`kind` one of `pi`,
  `clock`, `const`, `comb`, `ff`, `opaque`), `id` the module-local net id (the key for `odin3_net_prov`), plus
  `inputs` (the PI statistics and where they came from), `sweeps`, `iterations`, `converged`, `ff_in_scc`, `opaque_cells`,
  `wide_cells`, `latches`, `ms`. Written atomically.
- Module attributes (INT or STRING; per-net values are only in the files and the Python dict, as `scoap`):
  `odin3:activity_version` (1), `activity_nets`, `activity_cells`, `activity_opaque_cells`, `activity_wide_cells`,
  `activity_iterations`, `activity_converged`, `activity_source` (`odin3-vectorless`, the label Heimdall reads),
  `activity_netlist_hash` (a snapshot goes stale when a later pass edits the netlist, as `scoap_netlist_hash`),
  `activity_pi_mode` (`default`, `file`, `vcd`).
- Info line: `activity: M modules, N nets, C cells, F flip-flops (S in feedback), I iterations, converged|NOT converged, T ms`.
- Python: `Design.activity(**options)` returns the JSON as a dict; `docs/user/activity.md` (a user page with the model
  and its limits stated), PASSES.md row, CHANGELOG. No IR change, no ADR, no C ABI function (ABI stays 7).

## 5. Provenance (decision requested before this is built)

Step files: `docs/user/provenance-files.md`, schemas `docs/schemas/prov-*.schema.json`. The `attrs` of an object is
free-form (`{"type":"object"}`), the `stage` of a step is an enum. Two options:

- A (recommended): no schema change. `odin3-prov-steps` (and `odin3-flow --prov`) take the `.act`/JSON of the same
  run and add `attrs.act_p1`, `attrs.act_density`, `attrs.act_source = "odin3-vectorless"` to the existing `luts`-stage
  objects (a `lut`, a latch, a PI). The per-net chain is the object's existing links; nothing new to resolve.
- B: a new step `activity` with `net` objects (`activity:<blif name>`, link `name` to the `luts` object that drives the
  net). Adds `activity` to the stage enum of `prov-step.schema.json` and the Loki contract: a schema change. The
  brief says to stop and report before doing it, so this PR does not.

Option A is the whole of step 2's "per-net values into the provenance step files". It needs no change to Odin III C
code, only to `tools/flow/prov_steps.py` and `odin3_flow.py` (an `--activity F.act` option) and the schema doc
(attribute names). Decision 1 below.

## 6. Files

`src/passes/activity/{activity.c (pass, options), act_model.c (nets, cells, truth tables), act_prop.c (3.1 and the
mode product), act_seq.c (SCC finding, 3.3), act_out.c (.act, JSON, attributes), act_vcd.c (PI statistics from a VCD)}`,
`src/passes/activity/act_int.h`; registered in `builtin.c`; `tests/tools/test_activity.py` (CTest `activity_unit`),
`tests/activity/*.v` micros and goldens, `tools/activity/{act_ref.py, act_check.py}`; docs as in section 4.
The VCD reader reuses `tools/vcd-compare` semantics (end-of-step values) and `src/sim/vcd.c`'s format knowledge;
no new third-party code. Lint rules apply (no NOLINT; functions under the lizard limits).

## 7. Cost

Per node `O(k 2^k)` with `k <= 6` for abc output; one forward pass over the netlist plus iterations only inside
feedback SCCs. Memory: two doubles per net plus the model (about `scoap`'s). Target: no worse than ACE 2.0 on time and
memory on the benchmark set (ACE builds a CUDD BDD manager and simulates 5000 cycles); measured, not assumed (section 8).

## 8. Comparison and measurement (step 3)

`tools/activity/act_check.py` (Python, standard library plus the repo's own VCD reader):

1. Against ACE 2.0 (`external/vtr-verilog-to-routing/build/bin/ace`, run as `wyrd power` runs it: same BLIF, `-p 0.5 -d 0.2 -s 1`,
   adders soft): per design the nets common to both `.act` files; Pearson and Spearman correlation and mean absolute error of
   `P1` and of `D`, mean signed error of `D`, the fraction of nets within 0.05. ACE's own sampling noise (5000 cycles, so about
   `0.007` standard deviation on `P1` at 0.5) is stated beside each result, and ACE re-run with a second seed gives the
   ACE-to-ACE floor.
2. Against simulation (`sim_tb --vcd-all --vcd-names hash` of the design's passing testbench on the same netlist, as `wyrd power run
   --activity odin3`): `P1` and `D` of every net from the waveform, with the PI statistics measured from the same waveform fed to the
   pass (`--vcd`), so the inputs are identical; and, separately, the default PI statistics (to separate model error from
   input-statistics error). Designs: the Mimir designs whose testbenches pass on the netlist (mmuart, verilog_cordic_core, wb_lcd with
   `--2state` [step 0 note], mips_16, gng, and the other OpenCores with a passing bench) and the VTR micro/small sets that have
   vectors.
3. Power: VPR `--power` on one placement and routing per design with each of the three `.act` files (ACE, `activity`, simulated):
   total, dynamic, clock and routing power, percent difference from the simulated-activity run. This is the number that matters
   for hub B Q1.2.
4. Run time and peak RSS (`/usr/bin/time -v`) of `activity` and of ACE 2.0 on the same BLIFs, smallest first, largest last (heavy
   runs through `tools/heavy-run`).

Reporting follows the CAD convention: the headline first (geomean and win/loss/tie of mean absolute error against simulation,
`activity` versus ACE), then the strict label. Results go in the PR description and a table in `docs/user/activity.md`.

## 9. Test gates

1. Exact combinational cases with hand-derived values (the Python reference `act_ref.py` enumerates all `2^k x 2^k` pairs, shares no
   code with the pass): AND2 `(0.25, 0.18)`; XOR2 `(0.5, 0.32)`; OR2 `(0.75, 0.18)`; NOT `(0.5, 0.2)`; a MUX with select
   `(0.5, 0.2)`; a 4-input AND tree; a 6-LUT with a random table against enumeration; a node with a duplicate pin; constants. Agreement to 1e-12.
2. Sequential micros with known answers: a 4-bit binary counter (`P1 = 0.5` at every bit, `D = 1, 0.5, 0.25, 0.125`), a toggle flip-flop
   (`D = 1`), a 4-stage shift register from `(0.5, 0.2)` (`D = 0.2` at every stage), an enable register, a flip-flop with an absorbing state
   (convergence and the start-value option), a 2-flip-flop ring.
3. Cross-check of the model itself: on a random small netlist whose PIs are independent Markov chains and which has no reconvergent fanout,
   the pass equals the long-simulation estimate (5 sigma). With reconvergence the difference is expected and is reported (section 10), not a failure.
4. Format: the `.act` of every golden BLIF micro has exactly one line per net of `write_blif --vtr`, and VPR accepts it (`vpr --power`
   reads it without the "does not contain signal" error); names equal `sim_tb --vcd-all` names.
5. `check` in Debug before and after; ASan/UBSan clean; `tools/lint.sh`; `/run-micro` where C changes; ABI check (still 7).

## 10. Known approximations and open gaps (stated up front, to be measured)

- Spatial independence of the inputs of a node. Reconvergent fanout (the same signal reaching a node by two paths) is the main source of error;
  ACE's simulation sees it, this pass does not. The comparison measures how much it costs. A later refinement (signal-correlation
  coefficients for pairs of nets, or a simulation-based engine under the same output) is possible without changing the interface.
- Primary-input bus correlations (a one-hot or counted input bus) are not modelled; a `--vcd` input gives per-bit statistics only.
- Feedback density uses the one-frame rule of 3.3; an FSM with strongly input-dependent transitions will differ from simulation.
- Glitches are not counted (as ACE and the cycle simulator); a unit-delay glitch model would be a separate option.
- Multi-clock designs: densities relative to one reference clock.
- A wide cover (over `--exact` inputs) and non-tabulated hard cells are opaque.
- The ACE depth factor `0.4*depth` is a heuristic of the tool, not a reference truth; where `activity` differs from ACE, simulation decides.

## 11. Decisions for the controller (numbered, with recommendations)

1. Provenance: option A (attributes on the existing `luts` objects, no schema change) or option B (new `activity` stage, schema change)?
   Recommend A.
2. Engine: analytic only (this spec) or also `--method sim:N` (random-vector simulation through `sim`, ACE-like, for the designs where reconvergence hurts)?
   Recommend analytic only now; the sim-based comparison is already in `act_check.py`, and a second engine can be added without changing the interface.
3. Flip-flop start value 0.5 (recommended) or 0 (ACE's start)?
4. Reference-clock convention (single reference clock, other PI clocks `(0.5, 2)` with a WARNING): accept for v1.6?
5. `.act` and JSON names: accept `short` default plus `hash` for the Bifrost/Heimdall flow (matches `sim_tb --vcd-all`)?

## 12. Step 0 note (separate PR, #410)

`sim_tb` on the abc-mapped netlist failed mmuart and verilog_cordic_core because the generic 4-state wrapper made every output x when any
input of a hard `adder` was unknown; fixed in #410 (exact over the unknown inputs, micro `tb_hard4`). wb_lcd fails on the same netlist
for another reason: `abc` drops the RTL `if (x)` select attribute, so the LUT cover propagates x where the RTL takes the else branch;
`sim_tb --2state` passes all 307 checks. The activity comparison for wb_lcd therefore uses `--2state`.
