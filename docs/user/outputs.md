# Outputs and read-back

| Format | Pass | Notes |
|---|---|---|
| BLIF | `write_blif [--vtr] [--lut-k K] [--name-style short\|provenance\|hash] <file>` | `--vtr` writes only the top module, in the dialect VPR reads |
| Verilog | `write_verilog <file>` | Structural. With LPM cells after `lpmmap`; `read_verilog -icells` reads it back |
| JSON | `write_json <file> [--odin3]` | Yosys schema; `--odin3` adds the version, and provenance and level per module, cell and net name |
| Graphviz dot | `write_dot <file>` | See {doc}`analysis` |
| Yosys RTLIL text | `write_rtlil` | Word-level, with processes |
| Locations | `dump_locs [--types] [<file>]` | One `module\|kind\|name\|file:line.col-line.col` line per object |

A file name of `-` writes to standard output for the formats that say so in {doc}`passes`.

## Names

`--name-style` chooses how nets and cells are named in BLIF, JSON and Verilog output:

- `short` (default for BLIF): the short name of the object.
- `provenance`: names of the form `hier/<name>@file:line`; `read_blif` decodes them back.
- `hash`: a short hash, with a name map written next to the output (`<file>.names`, or the file
  given with `--name-map`). The pass `names --map FILE --lookup SHORT` and `--reverse FULL`
  translate in both directions, and `read_blif --name-map` or `read_json --name-map` puts the full
  names back on read.

## HDL locations

Every IR object carries provenance. Where the writer can express it, so does the file:

- `write_blif --src-attr` writes the HDL locations of a cell as `.attr src "file:line.col-line.col"`.
- `write_json` writes a `src` attribute on modules and objects; `read_json` turns it back into
  provenance (`--keep-src-attr` keeps it as a plain attribute too).
- `write_verilog` writes `(* src = "..." *)` attributes and a trailing `//` comment per line, as the
  LPM output of the flow script shows.
- `read_blif --hdl-locs` restores the locations from the BLIF.

The dump of every location of a run is `dump_locs`; `tools/prov-cover` reports how many objects
of a final netlist have a location (see {doc}`analysis`).
