# Odin III

Odin III is a Verilog synthesis front end and netlist tool for FPGA flows. It reads Verilog-2005
(and BLIF, JSON and VPR architecture files), elaborates to an intermediate representation (IR),
optimises and maps it, and writes BLIF for VPR or Verilog with LPM cells. Every object in the
output keeps a pointer to the HDL text it came from.

This manual follows the layout of the VTR documentation: a getting-started page, an overview of
the flow with one diagram per stage, then the references.

Beyond the manual: the [v1.2.0 release notes](https://github.com/drpaj12/odin3/blob/main/docs/V1.2.0.md)
(provenance at every granularity; see the {doc}`provenance` page; ready to tag, not yet released), the [v1.1.1 release notes](https://github.com/drpaj12/odin3/blob/main/docs/V1.1.1.md)
(critical-path attribution to HDL), the [v1.1.0 release notes](https://github.com/drpaj12/odin3/blob/main/docs/V1.1.0.md)
(SystemVerilog and the VerilogEval run; the [v1.0 notes](https://github.com/drpaj12/odin3/blob/main/docs/RELEASE-NOTES-1.0.md) are older), the [paper](https://github.com/drpaj12/odin3/tree/main/docs/paper),
and the [token, model and time cost](https://github.com/drpaj12/odin3/blob/main/docs/log_cost_of_tokens_and_time.md)
of building Odin III. The other Yggdrasil III pieces are linked in the sidebar; setting up the whole
suite is on the [hub getting-started page](https://drpaj12.github.io/YggdrasilIII/getting-started.html).

```{toctree}
:maxdepth: 2

getting-started
flow
odin3-flow
passes
inputs
outputs
provenance
analysis
graphlets
scoap
timing-attribution
testbench
python-api
re-workflow
```
