# Inputs

| Input | Pass | Notes |
|---|---|---|
| Verilog-2005 | `read_verilog [<file>...]` | Parses and elaborates. `--vtr-compat` follows the VTR flow's front-end choices; `--parse-only` stops after parsing; `-icells` reads back the LPM-cell Verilog Odin III writes; `-netlist` reads structural Verilog; `--prov-ast refs\|constructs\|full\|off` keeps the construct table that gives a provenance record its statement, block and module (default `off`; the Python `Design` sets `ODIN3_PROV_AST=refs`); `dump_locs --constructs` lists the table |
| SystemVerilog | `read_verilog [<file>...]` | `.sv`/`.svh` files (or a project's SYSTEMVERILOG entries) go to the slang adapter (`docs/V1.1.0.md`): it elaborates packages, enums, structs, interfaces, `always_ff/comb/latch` and writes structural Verilog through the same gate flow. A mixed Verilog/SystemVerilog read, `--lib`, `--parse-only`, `-icells`, `-netlist` and `--vtr-compat` are refused on that path; the project's include directories and defines apply |
| BLIF | `read_blif [--hdl-locs] <file>` | First model is the top. `--hdl-locs` restores HDL locations from the file (see {doc}`outputs`) |
| Yosys JSON | `read_json [--top NAME] [--auto-blackbox] <file>` | Reads Yosys `write_json` output and Odin III's own. Needs an empty design |
| Tech library | `read_techlib <file.o3lib>` | Adds the cell types a design may instantiate; read it before the netlist that uses them. `lib/vtr.o3lib` is the VTR set, `lib/lpm.o3lib` the LPM set |
| VPR architecture | `read_arch <file.xml>` | Imports the `<model>` and `pb_type` entries; the models become hard cell types. `read_arch` also takes a `.o3lib` |
| Project | `--project FILE`, `-f LIST`, `read_project` | A project record read from `o3proj`, `f` (file list), `qsf` or `odin2` input (`read_project --format`) |

Verilog files and a top: `odin3 --top NAME -p "read_verilog a.v b.v"`. Without `--top` the top is
the one module no other module instantiates; the log says which one was chosen.

Library elements the design uses but does not define can be allowed with `--auto-blackbox`
(`read_verilog`, `read_json`): the cell becomes an implicit black box and a warning is printed.

The exact options of every reader are in {doc}`passes`.
