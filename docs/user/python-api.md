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
example_plugin.py: libodin3 0.0.0 (ABI 4)
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

## Plugins

A shared-object plugin is loaded with `odin3 --plugin FILE` (C ABI, repeatable) or from Python
with `Odin3.load_plugin(path)`. `plugins/python/example_plugin.py` is the smallest Python
program over the binding.
