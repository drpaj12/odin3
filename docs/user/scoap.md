# SCOAP testability and signal probability

SCOAP (Goldstein 1979, doi:10.1109/TCS.1979.1084687) gives every net three small integers: how hard
it is to set the net to 0 (`CC0`) and to 1 (`CC1`) from the inputs, and how hard it is to see the net
at an output (`CO`). The sequential versions `SC0`, `SC1`, `SO` count flip-flop frames instead of
gates. Hardware-trojan work uses them to find rare nets; any reverse-engineering flow can use them as
a circuit property next to the graphlet signature. The `scoap` pass computes them exactly for the
netlist view; the full rules, defaults and their justification are in
`docs/specs/2026-10-31-scoap-design.md`.

## Running it

```text
odin3 -p "read_techlib lib/vtr.o3lib; read_blif ff.blif; scoap --top 5 --out ff.json"
odin3: info: scoap: 1 modules, 4 nets, 3 cells, mode comb, 4 sweeps, 0 unreachable, 1 ms
```

| Option | Meaning |
| --- | --- |
| `--sequential` | Also SC0, SC1, SO, and flip-flops by the sequential rules (default: scan view, a flip-flop output is an input and its data pin an output). |
| `--out FILE` | The JSON (schema `odin3-scoap`, version 3): per module `nets` (`id`, `name`, `cc0`, `cc1`, `co`, ...), `top`, `rare`. |
| `--top N` | The N hardest-to-control (largest `max(CC0,CC1)`) and hardest-to-observe (largest `CO`) nets per module. |
| `--focus F` | Report only the nets of a write_dot focus (`cone(NET)`, instance path); values are still computed on the whole module. |
| `--sigprob V`, `--seed S` | Simulate V random vectors (default seed 1) on the top module and add `p1_ppm`, P(net = 1) in parts per million (-1: the simulator does not list the net). |
| `--threshold P` | With `--sigprob`: nets with `min(P1, 1-P1)` below P (default 0.01) are listed in `rare`. |

If the simulator refuses the netlist because of a combinational loop (for example through a transparent latch)
or a net with two drivers, `--sigprob` is skipped with a WARNING that names the refusal; the SCOAP values do
not need the simulator. The JSON says so: top level `"sigprob_ran": false`, `"sigprob_vectors": 0`, `p1_ppm`
is -1 on every net and the top module's `"rare"` is `null`. Test `sigprob_ran` before reading `p1_ppm` or
`rare`. Any other simulator failure (`max_cells` budget, an unsupported cell, a clock that is not a primary
input) fails the pass. A latch
loop is common: Verilog `always @*` with an incomplete assignment infers a latch whose data input reads its own
output on the hold path (Yosys builds the same loop). SCOAP takes loops through gates and storage cells as a
least fixed point; with `--sequential` a latch is a storage cell (Q from D, enable observed like D), and without
it the latch is cut: Q is an input and D an output.

`INF` is 10^18 (`odin3:scoap_inf`) and means only "unreachable": the net cannot be set to that value,
or cannot be observed. Finite values saturate at `SAT` = 10^15 (`odin3:scoap_sat`) and stay finite;
a deep reconvergent circuit gives a saturated value, never `INF`. `odin3:scoap_saturated` and the
module field `saturated` count the nets with a saturated value.
Black boxes, hard blocks and module instances are opaque: their outputs behave as inputs and their
inputs as outputs, so run `flatten` first for a design-wide value. Every opaque cell type is named in
the log. Functions of more than 8 inputs use the bounds stated in the spec (`odin3:scoap_wide_cells`
counts them).

Module attributes (`odin3:scoap_version`, `_inf`, `_sequential`, `_nets`, `_cells`, `_scan_cells`,
`_opaque_cells`, `_wide_cells`, `_sweeps`, `_converged`, `_unreachable`, `_max_cc`, `_max_co`) hold the
summary (also `_sat`, `_saturated`, `_netlist_hash`); per-net values are only in the file (and the
Python dict). The net `id` and `name` lead back
to the provenance chain (`odin3_net_prov`).

Stale values. The attributes are a snapshot: a later pass that edits the netlist does not update
them. `odin3:scoap_version` is the schema version and `odin3:scoap_netlist_hash` a fingerprint of what
the values were computed from (nets, boundary values, cells, their functions). To test whether
the attributes are current, run `scoap` again and compare the module's `netlist_hash` (JSON, and the
attribute) with the stored one: different means stale.

## Python

```python
result = design.scoap(sequential=True, top=10, sigprob=20000, seed=1)
for net in result["modules"][0]["top"]["hardest_to_observe"]:
    print(net["id"], net["value"])
```

## Checks

`tools/scoap/scoap_ref.py` is an independent reference (it reads the BLIF itself); `tools/scoap/gate.py`
compares every integer of the pass with it on any list of BLIFs, in both modes, and fails on a net
that has no value. `tests/tools/test_scoap.py` holds the hand-computed circuits and the exact
signal-probability enumeration.
