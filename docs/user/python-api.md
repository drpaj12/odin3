# Python API

`plugins/python/odin3.py` is a cffi binding to the C ABI (`include/odin3/odin3.h`). The cdef is
generated from the header at import time, so the binding follows the header. There is no API
stability promise before the 1.0 release.

## Setup

The binding needs `cffi` (`pip install -r requirements-dev.txt`) and a shared library built
without sanitizers: `build/release/libodin3.so`.

```bash
cmake --preset release
ninja -C build/release libodin3.so
export ODIN3_LIB="$PWD/build/release/libodin3.so"
```

The library is found through `$ODIN3_LIB`, else `build/{debug,release}/libodin3.so` in the
repository. The `debug` preset library links ASan and cannot be loaded by a plain `python3`.

```bash
python3 plugins/python/example_plugin.py
```

```text
example_plugin.py: libodin3 1.1.1 (ABI 6)
```

## Walking the IR

`Design` holds a design. Run passes with `run_script` (or `run_pass`), then read the IR. Every
object knows its HDL locations through `sources()`.

```python
import sys
sys.path.insert(0, "plugins/python")
from odin3 import Design

with Design() as design:
    design.run_script("read_verilog tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v;"
                      " proc; opt; compact")
    top = design.top()
    print(top.name, top.node_count())
    for node in top.nodes():
        print(node.type_name, [(s.file, s.line) for s in node.sources()])
```

```text
bm_DL_2_cascaded_flip_flops 6
$port_in [('tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v', 4)]
$port_in [('tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v', 4)]
$port_out [('tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v', 5)]
$port_out [('tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v', 5)]
$dff [('tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v', 8)]
$dff [('tests/micro/verilog/micro/bm_DL_2_cascaded_flip_flops.v', 8)]
```

`Module` has `nodes()`, `nets()`, `wires()`, `ports()` and lookups by name; `Node` has `pins()`,
`params()` and `type_name`; `Net` has `pins()` and `drivers()`. `design.objects_at(file, line)`
finds the objects that came from a source line. Objects are small values that name an IR object
by ID; IDs stay valid until a `compact` pass renumbers a module. A failing call raises
`Odin3Error` and the library has logged the reason.

## Graph analysis without the library

`plugins/python/odin3_graph.py` loads the JSON of `write_json` (best with `--odin3`) as a
networkx `MultiDiGraph` and converts it to a PyTorch Geometric graph. It uses only the standard
library until you call `load_json` (networkx) or `to_pyg` (torch, torch_geometric).

```python
from odin3_graph import load_json
g = load_json("out/enc/analysis/bm_DL_2_4_encoder.odin3.json")
```

One node per cell and per module port, one edge per driver-to-sink pin pair on a net. Node
attributes are `kind`, `type`, `parameters`, `attributes`, and `odin3_level` and `odin3_prov` when
the file has them. A file whose `odin3_version` is not 1 is refused.

## Simulation

`Design.simulate()` opens a `Simulation` of the top module (or of a module given by name). It steps
the existing netlist simulator one cycle at a time: set inputs, step, read values, reset. A
Verilog design must be run through the simulable flow first; a BLIF or JSON netlist needs no pass.

```python
design = Design()
design.run_script('read_verilog "traffic.v"; proc; opt; lower')
with design.simulate() as sim:
    sim["rst"] = 1               # inputs keep their value until set again
    sim.step()                   # one clock cycle
    sim["rst"] = 0
    sim["car_ew"] = 1
    sim.step(2)
    print(sim["light_ns"], sim["light_ew"], sim.cycle)   # 1 0 3
```

Rules:

- **Names.** A signal is found by its HDL name; instances are separated by `.`
  (`sim["u0.inner"]`), and a bit select is `sim["a[2]"]`. `sim.signal(name)` returns a
  `SignalInfo` (id, name, width, kind, declared range, scope, IR object) or `None`;
  `sim.signals()` lists the named signals and `sim.scopes()` the expanded instances. A name the
  simulated netlist does not have raises `KeyError`. Names are the same after `flatten`.
- **Values.** Unsigned Python ints, bit i of the int is bit i of the signal, any width. `set` of a
  negative value or one of `2**width` or more raises `ValueError`; `set` of anything but an input
  raises `Odin3Error`.
- **A step.** `step(n)` runs n cycles: settle with the inputs as set, raise every clock and send
  the rising edge to the registers, settle, lower the clocks, settle. Values read afterwards are
  those after the last settle. `settle()` shows the combinational effect of inputs just set
  without advancing state. `reset()` is a power-on reset (all values 0, register INIT applied,
  `cycle` back to 0); it is not the design's own reset input.
- **Clocks.** Clocks must be top input ports. A port whose bits are all clocks is kind `"clock"`
  and the steps drive it; do not set it. For a gated or derived clock use a clock enable instead.
- **Removed names.** `opt` and `abc` delete logic nothing observes, including an unused register.
  Mark a signal you want to keep with `(* keep *)` in the HDL, or open the simulation before that
  pass.
- **Passes are refused while a simulation is open.** `run_pass` and `run_script` on the design
  raise `Odin3Error` until `sim.close()`. Several simulations of one design may be open at once,
  each with its own state. `Design.close()` closes its simulations first, and a design and its
  simulations may be garbage-collected in any order.

## Critical-path attribution

`odin3.timing_attribution(flow_dir, paths=None)` returns the attribution of the critical path of
an `odin3-flow --timing-attr` run: the dict that `timing/attribution.json` holds (`schema_version`
1; fields in {doc}`timing-attribution`). `paths` keeps the first N setup paths of the VPR report.
It raises `RuntimeError` (a `timing_attr.AttributionError`) when an input is missing, unreadable
or inconsistent (the flow ran without `--timing-attr`, the VPR report is gone or cut off, an input
file is not valid JSON or text, the two BLIFs do not line up) and when `paths` is below 1.

```python
import odin3
report = odin3.timing_attribution("out/design")
print(report["timing"]["fmax_mhz"])
for span in report["critical_path"]["spans"]:
    print(span["span"], f"{span['delay_ns']:.3f} ns", span["words"])
```

It does not call the shared library: it reads the files the flow wrote (the package still imports `cffi`).

## Plugins

A shared-object plugin is loaded with `odin3 --plugin FILE` (C ABI, repeatable) or from Python
with `Odin3.load_plugin(path)`. `plugins/python/example_plugin.py` is the smallest Python
program over the binding.

The `abc` and `cone_check` passes need the linked ABC engine, which the `odin3` command registers
at start-up and a bare library does not: call `Odin3.load_abc()` once first (it loads
`libodin3_abc.so` from next to the library, or from `$ODIN3_ABC_LIB`). A worked example of a
round of the reverse-engineering loop is on the [reverse-engineering workflow](re-workflow.md)
page.
