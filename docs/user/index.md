# Odin III

Odin III is a Verilog synthesis front end and netlist tool for FPGA flows. It reads Verilog-2005
(and BLIF, JSON and VPR architecture files), elaborates to an intermediate representation (IR),
optimises and maps it, and writes BLIF for VPR or Verilog with LPM cells. Every object in the
output keeps a pointer to the HDL text it came from.

Headline result, from the paper (kept locally by the author): every netlist Odin III
produced for the 15 VTR benchmarks that fit the development laptop routes in VPR, with geomean CLB, channel-width, delay and
wirelength ratios against the Parmys flow inside a 1.20 gate; two designs differ from Parmys in multiplier count and five lose
adders. Power is not measured. The hub is <https://github.com/drpaj12/YggdrasilIII>.

This manual follows the layout of the VTR documentation: a getting-started page, an overview of
the flow with one diagram per stage, then the references.

Beyond the manual: the [v1.2.0 release notes](https://github.com/drpaj12/odin3/blob/main/docs/V1.2.0.md)
(provenance at every granularity; see the {doc}`provenance` page; version 1.3.0 changes are in the CHANGELOG), the [v1.1.1 release notes](https://github.com/drpaj12/odin3/blob/main/docs/V1.1.1.md)
(critical-path attribution to HDL), the [v1.1.0 release notes](https://github.com/drpaj12/odin3/blob/main/docs/V1.1.0.md)
(SystemVerilog and the VerilogEval run; the [v1.0 notes](https://github.com/drpaj12/odin3/blob/main/docs/RELEASE-NOTES-1.0.md) are older),
and the [token, model and time cost](https://github.com/drpaj12/odin3/blob/main/docs/log_cost_of_tokens_and_time.md)
of building Odin III. The other Yggdrasil III pieces are linked in the sidebar; setting up the whole
suite is on the [hub getting-started page](https://drpaj12.github.io/YggdrasilIII/getting-started.html).

## Features at a glance

Version 1.6.0, C ABI 7. Everything below is on `main` with tests; the table with links is in the
[repository README](https://github.com/drpaj12/odin3#features).

- Inputs and outputs: {doc}`inputs`, {doc}`outputs`, {doc}`odin3-flow`.
- Passes: elaboration, `proc`, `opt`, `fsm`, `flatten`, `wreduce`, partial mapping, `lower` (with the `$tribuf` sole-driver
  rule), ABC, LPM, `unmap` ({doc}`passes`).
- Provenance at every granularity ({doc}`provenance`), written as step files at every stage ({doc}`provenance-files`).
- Silicon flow: Odin III to a routed SKY130 layout and back, no Yosys ({doc}`silicon-flow`); flows described in the project file ({doc}`project-flows`).
- Graph features: graphlets and orbits, SCOAP, analysis and timing attribution ({doc}`graphlets`, {doc}`scoap`, switching activity ({doc}`activity`),
  {doc}`analysis`, {doc}`timing-attribution`).
- Netlist editing API and edit events ({doc}`edit`).
- Simulation and testbenches (1.5.0: 4-state `sim_tb` by default, `--vcd-all`, testbench-side modules, DUT memory load, `force`, `lower --async keep`, `read_verilog --case-xz-never`; see `docs/V1.5.0.md`): `sim`, `sim_tb`, `testbench_gen`, `tb-compare`, `run_testbenches`, plusargs, `%t`, VCD, derived
  clocks ({doc}`testbench`); on main after 1.3.0 (Unreleased): up to 16 independent clocks per DUT instance and read-only
  hierarchical reads (`dut.u_regs.r`) in testbenches ({doc}`testbench`), and the tested `$tribuf` exclusions of `lower` ({doc}`passes`).
- Python binding, plugins and the ABI ({doc}`python-api`); reverse-engineering workflow ({doc}`re-workflow`).

```{toctree}
:maxdepth: 2

getting-started
flow
odin3-flow
passes
inputs
outputs
provenance
provenance-files
project-flows
silicon-flow
analysis
graphlets
edit
scoap
activity
graph-views
timing-attribution
testbench
python-api
re-workflow
```
