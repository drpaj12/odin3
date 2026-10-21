# Odin III

Odin III is a Verilog synthesis front end and netlist tool for FPGA flows. It reads Verilog-2005
(and BLIF, JSON and VPR architecture files), elaborates to an intermediate representation (IR),
optimises and maps it, and writes BLIF for VPR or Verilog with LPM cells. Every object in the
output keeps a pointer to the HDL text it came from.

This manual follows the layout of the VTR documentation: a getting-started page, an overview of
the flow with one diagram per stage, then the references.

```{toctree}
:maxdepth: 2

getting-started
flow
odin3-flow
passes
inputs
outputs
analysis
python-api
```
