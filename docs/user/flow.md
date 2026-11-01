# The flow

A design goes through a fixed sequence of passes. Every pass reads and writes the same IR and the
pass manager checks the IR invariants between passes (always in a debug build, and with `--check`
otherwise). Each stage below shows what it consumes and what it produces. The text follows
`docs/DESIGN.md` section 6; the options of each pass are in {doc}`passes`.

```{mermaid}
flowchart LR
  A[HDL, BLIF, JSON] --> R[read] --> E[elaborate] --> P[proc] --> O[opt] --> F[flatten]
  F --> S[fsm] --> H[hardmap] --> L[lower] --> B[abc] --> W[write]
```

The script of the `vtr` target is `read_techlib; read_verilog; proc; fsm; opt; flatten; opt -full;
wreduce; hardmap; lower; abc; write_blif`. Stages the script does not use (`memory_merge`,
`lpmmap`) are listed where they belong.

## 1. Read

`read_verilog` parses Verilog-2005 into an AST. `read_blif`, `read_json` and `read_arch` read
netlists and architectures instead (see {doc}`inputs`). A tech library (`read_techlib`) adds the
cell types the design may instantiate.

```{mermaid}
flowchart LR
  F[".v / .blif / .json / .o3lib / arch XML"] --> R["read_*"] --> X["AST or netlist, cell types"]
```

## 2. Elaborate

Elaboration, part of `read_verilog`, resolves parameters and generate constructs and produces the
word-level IR. Each `always` block becomes a `$proc` node that later stages must not see.

```{mermaid}
flowchart LR
  A[AST] --> E[elaborate] --> W["word-level IR with $proc nodes"]
```

## 3. proc

`proc` lowers every `$proc` to muxes and flip-flops: decision trees become `$mux`/`$pmux`,
clocked blocks become `$dff`, `$adff`, `$aldff` or `$dffsr`, and latches become `$dlatch` (with a
warning). Passes that need a process-free design (`flatten`, `lower`, `abc`, the netlist writers)
refuse a design that still has processes and say so.

```{mermaid}
flowchart LR
  A["IR with $proc"] --> P[proc] --> B["IR with $mux, $dff, $dlatch"]
```

## 4. opt

`opt` is the Yosys optimisation family run to a fixed point: `opt_expr` (constant folding and
arithmetic simplification), `opt_merge` (common subexpressions), `opt_muxtree`, `opt_reduce`,
`opt_share`, `opt_dff` and `opt_clean` (dead nodes). It never renumbers objects; run `compact`
for that.

```{mermaid}
flowchart LR
  A[IR] --> O["opt_expr, opt_merge, opt_muxtree, opt_reduce, opt_share, opt_dff, opt_clean"] --> A2["smaller IR (repeat until no change)"]
```

## 5. flatten

`flatten` expands each module instance into the module that holds it. Objects get dotted
instance-path names such as `u1.mid.x`, and the instance path is added to the provenance of each
object. Black boxes, hard cells and library cells stay as they are.

```{mermaid}
flowchart LR
  A["top + sub-modules"] --> F[flatten] --> B["one module, dotted names"]
```

## 6. fsm

`fsm` finds finite-state machines, extracts them, recodes them (one-hot below 32 states for the
`auto` encoding) and maps them back to registers and logic.

```{mermaid}
flowchart LR
  A["registers + next-state logic"] --> D[fsm_detect] --> X[fsm_extract / recode] --> M[fsm_map] --> B["recoded FSM"]
```

## 7. Memories and hard blocks

`memory_merge` merges a flip-flop on the read data of an inferred memory into a synchronous read
port. `hardmap` (alias of `partial_map`) maps multipliers, adders and memories to the hard cells
of the loaded library and leaves the rest as soft logic. `lpmmap` does the same for the LPM library of the
Altera-style target.

```{mermaid}
flowchart LR
  A[IR] --> H["hardmap / lpmmap (library cells)"] --> B["hard cells + soft logic"]
```

## 8. lower

`lower` converts word-level soft logic to single-bit gates and flip-flops. Hard cells and black
boxes are untouched. Asynchronous controls are made synchronous unless `--async error` is given.

```{mermaid}
flowchart LR
  A["word-level soft logic"] --> L[lower] --> B["bit-level gates and flip-flops"]
```

## 9. abc

`abc --lut K` maps each module's soft logic to K-input look-up tables with the linked ABC, one
run per clock domain. Hard cells, black boxes and other domains' flip-flops are cut points. The
result keeps provenance from ABC's correspondence.

```{mermaid}
flowchart LR
  A["bit-level logic"] --> C[abc --lut K] --> B["$lut cells"]
```

### Gate mapping

`abc --gates L` ends in the gate cells of a library instead of LUTs. `L` is the name of an `.o3lib`
library already read with `read_techlib`; `lib/generic_gates.o3lib` (buffer, inverter, 2 to 4
input gates, multiplexers, constant cells, flip-flops and latches) and `lib/cmos6.o3lib` (BUF, INV,
NAND2, NOR2, DFF and the black-box DFFSR) ship with Odin III. Odin III derives ABC's gate library
from the cells that have a combinational function (1 to 6 inputs, every input used), loads it into
the linked ABC and runs `lib/abc/gates.abc` (the optimisation steps of the LUT script, then `map
-a`). A `--script` of your own must end in `map` or `&nf`. The mapped gates come back as instances
of the library cells, each with provenance to the HDL text of its cone.

`dfflibmap L` then maps the bit flip-flops and latches (`$_DFF_P_`, `$_DFF_N_`, `$_DLATCH_P_`,
`$_DLATCH_N_`) onto the library's state cells: the smallest compatible cell by area, with a clock
inverter for a falling-edge flip-flop when the library has no falling-edge cell. A flip-flop with
INIT 0 or 1 mapped onto a cell whose initial value is unknown loses it; the pass warns with the count.

```bash
odin3 -p "read_techlib lib/cmos6.o3lib; read_verilog --nolatches design.v; hierarchy -auto;
          proc; opt; lower; abc --gates cmos6; dfflibmap cmos6; write_verilog gates.v"
```

`write_verilog` writes structural Verilog of library cells; constants use the library's tie cells
or, without them, `1'b0` and `1'b1`. To prove the result, `write_blif --gates-as-names` writes every
gate as a `.names` cover and every library state cell as a `.latch`: write it right after `lower`
and again at the end, and compare the two files with `tools/equiv-check/equiv-check`. A gate
netlist can be written as BLIF, read back with the same library and mapped again; the result stays
equivalent. Reading the written Verilog back with `read_verilog -netlist` does not work for gate
cells, because only hard and black-box cell types resolve outside a module.

Limits: libraries are `.o3lib` only; `read_genlib`, `read_lib` and `amap` are not accepted;
asynchronous set and reset cells (DFFSR) are never chosen; the equivalence checker cannot read a
flip-flop clocked by an internal net, so a falling-edge flip-flop behind a clock inverter is not
checked. The decisions are in `docs/V1.0.2.md`.

## 10. write

`write_blif --vtr` writes the top module in the dialect VPR reads. Plain `write_blif` writes every module; `write_blif --reachable` writes only the top and the modules reachable from it (a WARNING names each module it skips, e.g. after `flatten`; Parmys writes only the top). `write_verilog` writes
structural Verilog (with LPM cells for the LPM target), `write_json` the Yosys JSON schema,
`write_dot` a Graphviz graph. See {doc}`outputs`.

```{mermaid}
flowchart LR
  A["mapped IR"] --> W["write_blif / write_verilog / write_json / write_dot"] --> F["files"]
```

## Targets

| Target | Passes after `read_verilog` | Writes |
|---|---|---|
| `vtr` | `proc; fsm; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut K` | BLIF for VPR |
| `lpm` | `proc; opt; wreduce; opt_clean; opt -full; lpmmap; opt_clean; opt -full` | Verilog with LPM cells |

The `vtr` script lives in `tools/vtr-qor/qor_legs.py` and the `lpm` script in
`tools/flow/lpm.script`; `odin3_flow.py` uses both unchanged.
