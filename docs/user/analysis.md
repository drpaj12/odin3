# Analysis

Three outputs let you look at a netlist as a graph and map it back to the HDL. The flow script
writes all of them with `--analysis`.

## Graphviz: `write_dot`

```bash
odin3 -p "read_techlib lib/vtr.o3lib;
          read_verilog --nolatches --vtr-compat tests/micro/verilog/micro/bm_DL_2_4_encoder.v;
          proc; fsm; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut 6;
          levelize; write_dot enc.dot --color-by @type --locs"
dot -Tsvg enc.dot -o enc.svg
```

(That script is the `vtr` flow without `write_blif`.) Options: `--focus <hier-path>|<file:line>|cone(<net>)` draws only that part (use it on big
designs: the pass will not draw more than `--max-nodes` nodes), `--color-by <attr>|@type` fills
nodes by attribute value or cell type and adds a legend, `--locs` puts the first HDL
`file:line` in each label. Each module is a cluster. The nodes of the example above are labelled
like this:

```text
$lut
$n6
tests/micro/verilog/micro/bm_DL_2_4_encoder.v:10 +5
```

(`+5` counts the further locations of the object.)

## Levels: `levelize`

`levelize [--key NAME]` counts the levels of combinational logic of every cell and net and stores
the count as the attribute `odin3_level` (or `NAME`). It changes attributes only. The pass logs a
summary line; for the encoder above:

```text
odin3: info: levelize: 1 modules, 6 cells, max level 1, 0 loops cut
```

## Graph export: `write_json --odin3`

`write_json <file> --odin3` writes the Yosys JSON schema with `odin3_version` and, per module,
cell and net name, `odin3_prov` (the HDL locations) and `odin3_level` (after `levelize`). The
loader `plugins/python/odin3_graph.py` reads this file as a graph without needing the library
(see {doc}`python-api`):

```bash
python3 plugins/python/odin3_graph.py out/enc/analysis/bm_DL_2_4_encoder.odin3.json
```

```text
nodes 7 edges 16
```

## Location coverage: `prov-cover`

`tools/prov-cover/prov-cover` measures how many objects of a final netlist have an HDL location,
per design and leg (`--legs a,b,c,d`; leg d needs VPR). It reports defects against a list of
known ones (`--expect`) and exits non-zero when the gate fails. `dump_locs --types` lists the
locations it checks, one `module|kind|name|SRC` line each.
