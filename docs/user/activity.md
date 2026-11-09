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

See the table in the pull request that introduced the pass and `tools/activity/act_check.py`, which compares the pass with ACE 2.0 and with
`sim_tb` activity on run directories of `wyrd power run` and reruns VPR `--power` with each activity file.

## Provenance

The JSON `id` of a net is the module-local net id; with `--name-style hash` the name is the one in the version 2 name map, so a tool joins activity to the
HDL line through the existing chain. `odin3-prov-steps` can add `attrs.act_p1`, `attrs.act_density` and `attrs.act_source` to the `luts` objects of a run
({doc}`provenance-files`): the attributes are free-form, no schema changes.
