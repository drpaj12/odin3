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

`--prov-map [refs|constructs|full]` (with `--name-style hash`, on `write_blif --vtr`,
`write_json` and `write_verilog`) writes the name map in version 2: the same `O` lines, then one
`R` line per provenance record behind the named objects (class, parents, source locations, the
joins a merge or a LUT made), the `C` lines of the AST constructs they come from, and the `F` and
`T` lines for the files and pass runs they name. The netlist is unchanged. The AST layer is at
most what `read_verilog --prov-ast` kept; the command line keeps none by default, so give
`--prov-ast refs` to the read, or the map has `ast=none` and no `C` lines. Without the option the
map is version 1, byte for byte as before.

`--prov-map` is given once. A word right after it that is `refs`, `constructs` or `full` is read as
the mode, so write a file of that name as `./refs`. A derived record whose operation has no
operation class stops the export (the error names the record, run and operation); an export that
fails writes neither the map nor the netlist.

After VPR, `odin3-prov-post FLOW_DIR` appends to the same map the records of packing and placement:
`R` lines of level `ble` and `clb` (step `pack`), of level `site` (step `place`, with the `.place`
coordinates `x=`, `y=`, `sub=`, `layer=` in `flags`), and one `A` line per atom of the packed
netlist with its BLE, CLB and site records. A cluster whose atoms blend distinct constructs is a
`join` labelled by `pack`. It needs the flow directory of `odin3-flow --prov --vpr --keep-results`,
refuses a BLIF written with short names, and refuses a map that it has already extended. The `.net` is trusted local VPR output, parsed with ElementTree. `names
--check` accepts the extended map.

The flags of an `R` line are `join`, `join=instance`, `lossy=bulk`, `lossy=cap`, `n=<total>`, `chain=span`,
`capped`, `hier=<text>`, `call=c<n>`, `cell=<type>`, `ctx=<n>`, `bulk`, and, written by `odin3-prov-post` and the
restore, `stale=t<n>` (a post-Odin record that a later pass made stale) and the site flags `x=`, `y=`, `sub=` and
`layer=`.

Why a join was made, and how critical it was (`Q` and `K` lines). Odin III writes one `Q` line per join record with
its objective (the name of the pass; for `abc` the goal its script states, or `area` or `depth` from the script's last
mapping command: `# objective: <word>` in the script states it, `if`, `&if`, `map` and `&nf` map for delay unless `-a`
toggles area). `odin3-prov-post` adds, for every logic CLB, the pair classes of its members (`SC` series-critical, `SN`
series-noncritical, `SI` shared-input, `OR` orthogonal, `FL` filler; shares that sum to 1.000), and for every CLB, BLE join
and Odin III join the post-route criticality `rho`, `slack`, `near` and `npaths` at the margins 5 and 10 percent, from
`timing/attribution.json`. With `--census FILE` (the Norns packer's `--norns_pack_census` log; its `.net` SHA-256 must
match, else the census values are absent with a WARNING; the SHA-256 is read from `--census-net-sha256`, else from the
`net_sha256` of the `metrics.json` next to the census, else from a `# net_sha256=` line of the census) it also gives the classes, the `gain` terms and the pack-time
criticality `pcrit`. A `K` line per member (every member of a CLB) carries the member's own values. `basis` says per field
whether the value was recorded (`r`) or inferred (`i`), `-` when absent. The `OR` class needs a word label of each atom:
`--words FILE` (a Norns `words/members.json`) gives it (`why=r`); without it the nearest declarator of the name map is used
and `why=i`. The output pins of a hard block come from `--arch FILE` (the architecture's models) or, by default, from the
`.net`'s primitive ports. Without a census there are no classes, `gain` or
`pcrit`. `odin3.read_name_map(path)` returns these as `NameMap.whys` and `NameMap.members`.

For a version 2 map the pass `names` reads the records too:

- `names --map FILE --lookup SHORT` prints the `O` line of the object (or the `N` line, when the argument
  is the name of an object that kept its name), then its records indented by their depth (its own record
  first, then the parents, depth first, each once), then the constructs of its chain (the statement,
  block and module each source leaf comes from) and the `F` and `T` lines they name.
- `names --map FILE --chain SHORT` prints the chain alone: the `C` lines and the files they name.
- `names --map FILE --check` adds a line with the counts of `R`, `C`, `F`, `T` and object lines and of each
  problem (an id that is not its line number, a reference that does not resolve, a parent numbered after its
  child, a record with no valid class, an object whose record is missing), and fails on any of them.

Output is the map's own text, so it can be read back by the same parse. In Python,
`odin3.read_name_map(path)` reads the file without a design: `NameMap.find(name)`, `provenance(obj)`
and `chain(obj)` give what the pass prints. Version 1 maps behave as before (`--chain` needs version 2).

### Reading a version 2 map back

`read_blif --name-map`, `read_json --name-map` and `read_verilog -netlist -name-map`, given a map of
version 2, rebuild what the map holds: the pass runs (the `T` lines), the provenance records with their
classes (the `R` lines), the AST constructs the records come from (the `C` lines) and the post-Odin
lines (`A`, `Q`, `K`), and bind every object the map lists (an `O` or `N` line) to its record. The
design then answers `Node.provenance()`, `Net.provenance()` and `chain()` as the design that wrote the
map did. The readers make no restored-location record when the map is version 2, and a version 1 map
restores nothing, as before. A map that fails `names --check`, or one the restore cannot take (a
construct or class that is not valid), is refused with a located error and nothing is restored.

What a read-back does not bring back: a record no listed object or extra line reaches, the AST node IDs, the
IR run numbers, and the records of objects the file does not mention (a net with a record that no cell
uses is listed by the writer and cannot be bound). `read_blif` binds nets only (a cell has no record, a
port vector's bits share the record of the first bit); `read_json` binds netnames and cells;
`read_verilog -netlist` binds module-level wires and nets. A `clb` or `site` record that
`odin3-prov-post` wrote with no parent (a cluster of pads alone, in a map from before this version) makes
the restore fail with a located error: a derived record has a parent. The tool now writes no record for
such a cluster. A restore is all or nothing: a map that fails restores nothing and leaves the design as
it was read. A line over the limits of its type (more fields than the type has, or longer than 1 MiB) and
a flag with a bad value are refused too.

Lines of the map written after Odin III (`A`, `Q`, `K` and the `ble`, `clb`, `site` records only they
reach) describe the netlist as it was. After a read-back the first pass that changes the netlist (not a reader, a writer, `check`, `stats`,
`hierarchy`, `names`, `dump_locs`, `dump_project`, `sim` or `preprocess`)
is the mutating run: when the map is written again those lines end in `t<n>`, the `T` line of that
run, and those records carry `stale=t<n>` in their flags. They are history: still written, never
presented as current. A join made after the restore gets a fresh `Q` line with stale `-` and the name
of the pass as its objective. Writing the map again after an unchanged read-back keeps every line.

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

## Waveforms (VCD)

`sim` writes a waveform of the whole run next to its vectors:

```
odin3 -p "read_verilog top.v; proc; opt; lower; sim 64 1 out.vec --vcd wave.vcd"
```

`--vcd <file>` works with random inputs and with `--vectors-in`. The file is an IEEE 1364 VCD
with the values 0 and 1 only, written atomically, and is a function of the netlist and the inputs
(no date). Every named signal appears: the top ports first, then the wires and named nets of each
module instance, in one scope per instance, named by the HDL names (a bus shows as `name [msb:lsb]`).

One cycle is 10 time units (`$timescale 1ns`). Time 0 holds the power-on state. In cycle `c` the
inputs are driven at `10c+2`, the clock rises at `10c+5` and falls at `10c+10`, so a clock reads
0, 1, 0 at those three times, as in a testbench that changes its inputs 2 units after the falling
edge. A time is written only when some signal changed. The values at `10c+10` are the outputs the
vector file prints on line `c`. Open the file with GTKWave (`gtkwave wave.vcd`) or any other VCD
viewer.
