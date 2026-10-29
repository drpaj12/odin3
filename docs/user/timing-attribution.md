# Critical-path attribution

After the VTR flow, `odin3-flow --timing-attr` tells you which HDL statements, modules and words
set the critical path and so the Fmax. It answers: "I have 480 MHz; which lines of my Verilog
should I change to get more?"

It reads the worst setup paths of the routed design from VPR and joins every timing node to the
HDL through the names Odin III wrote. It is a tool addition: the core, the IR and the C ABI are
unchanged.

## Run it

```bash
export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing
odin3-flow design.v --top top --timing-attr --out out/design
cat out/design/timing/summary.txt
```

`--timing-paths N` sets how many setup paths VPR reports and the tool attributes (default 100,
VPR's default; it implies `--timing-attr`). `--timing-attr` needs `--target vtr` and VPR. The
flow writes the BLIF VPR reads with hash names and the version 2 name map next to it
(`odin3-flow --prov`, implied) and `write_json --odin3`; no second BLIF is written. When N is not
100 it adds the VPR report option `--timing_report_npaths N`. A BLIF with hash names differs from
the short-name one only in its names, and VPR's packing, placement, routing and critical path
do not depend on them (`docs/V1.2.0.md` 6.1; the check of `docs/V1.1.1.md` is for the second BLIF
of version 1.1.1). `--timing-legacy` runs the version 1.1.1 route instead (a second BLIF with
provenance names).

To attribute a flow output directory that already exists, without rerunning anything:

```bash
odin3-timing out/design [--paths N] [--out DIR]
```

It needs the `timing/` inputs that a run with `--timing-attr` wrote. Exit codes, as `odin3-flow`:
0 ok, 1 the attribution failed (missing report or input, files that do not line up), 2 usage error.

From Python (Norns III, Wyrd III):

```python
import odin3
report = odin3.timing_attribution("out/design")      # the dict of attribution.json
for span in report["critical_path"]["spans"]:
    print(span["span"], span["delay_ns"], span["share"])
```

See {doc}`python-api`.

## What is written

Everything goes into `<out>/timing/`:

| File | Content |
|---|---|
| `attribution.json` | The attribution, `schema_version` 1. The one file the Wyrd III view reads |
| `summary.txt` | Fmax and CPD, then the statements that set them |
| `netlist.odin3.json`, `inputs.json` | The inputs of the join (`odin3-timing` reads them); the name map is `odin3/<top>.blif.names`. A `--timing-legacy` run also writes `prov.blif` |
| `timing.log` | `ok`, or the reason the step failed |

### summary.txt

```text
Critical path attribution for bm_DL_BCD_adder
Fmax 216.48 MHz   CPD 4.619 ns   worst slack -4.619 ns   (10 setup paths)
Critical path: X[0].inpad[0] (.input clocked by virtual_io_clock)  to  out:Cout.outpad[0] (.output ...)
Matched to HDL: 70.6% of 68 path nodes (89.5% of the critical path delay); ports 29.4%; unmatched 0.0%

These statements set your Fmax (critical path, in path order):
  (I/O port)  module bm_DL_BCD_adder  word X  0.483 ns (10.5%)
  tests/micro/verilog/micro/bm_DL_BCD_adder.v:15  module bm_DL_BCD_adder  word -  2.275 ns (49.3%)
  tests/micro/verilog/micro/bm_DL_BCD_adder.v:19  module bm_DL_BCD_adder  word -  0.889 ns (19.2%)
  tests/micro/verilog/micro/bm_DL_BCD_adder.v:16  module bm_DL_BCD_adder  word Cout  0.972 ns (21.0%)
  (I/O port)  module bm_DL_BCD_adder  word Cout  0.000 ns (0.0%)
```

The list "in path order" collapses consecutive nodes of one statement. The tables after it rank
statements, modules and words by their delay on the critical path and show, over all attributed
paths, how many go through them and the worst slack. `word ~name` is a guess from the assignment
on that source line; `-` means the nets there have no source name.

### attribution.json

Top level: `schema_version` (integer, 1), `design`, `timing`, `coverage`, `critical_path`, `spans`,
`modules`, `words`, `paths`. Delays are in nanoseconds, slacks as VPR reports them (negative when
the clock constraint is not met; VPR's default constraint is 0, so slack is minus the delay).

`design`: `top` (module name), `flow_dir`, `report` (the VPR file), `blif` (the BLIF VPR read),
`provenance_blif` (the second BLIF of a version 1.1.1 flow directory; an empty string "" for a flow
directory with a name map, which has no second BLIF; a string on both routes),
`netlist_json` (null when absent) and, with a name map, `name_map` (its path).

`timing`:

| Field | Meaning |
|---|---|
| `cpd_ns`, `fmax_mhz` | From VPR's `Final critical path delay` line (null when absent) |
| `paths_requested` | What the flow asked VPR to report (`--timing-paths`; null for a flow dir that does not say) |
| `paths_limit` | The `--paths` / API `paths` override, null when none was given |
| `paths_in_report` | Usable setup paths the VPR report holds (fewer than requested on a small design) |
| `paths_kept` | Paths in `paths` (`paths_in_report`, cut to `paths_limit`) |
| `paths_parsed` | The same number as `paths_kept`, kept for readers of the first layout |
| `paths_skipped` | Report paths that were not attributed (never dropped silently; also in `summary.txt`) |
| `paths_skipped_reasons` | `paths_skipped` by reason: `hold` (a hold path in the setup report: VPR's setup report has none, a foreign one is skipped), `no_slack` (no `slack` line: a truncated path), `no_rows` (no data rows) |
| `worst_slack_ns` | Slack of path 1 |
| `critical_data_delay_ns` | Sum of the attributed delays of path 1 (launch clock latency and setup time excluded) |

`coverage` (never dropped silently):

| Field | Meaning |
|---|---|
| `nodes`, `matched`, `port`, `unmatched` | Counts of path nodes (atoms on the paths, I/O pads included; an atom on k paths counts k times) |
| `matched_share`, `port_share`, `unmatched_share` | The three counts over `nodes`: pads are in the denominator, so `matched_share` is diluted by them |
| `matched_share_excl_port` | `matched` over `matched + unmatched`: the share of non-pad nodes that reached a source location |
| `critical_matched_delay_share`, `critical_port_delay_share`, `critical_unmatched_delay_share` | The same split by delay on the critical path |
| `unmatched_atoms` | Up to 200 entries `{atom, block_type, nodes, paths}`, most frequent first |
| `unmatched_atoms_total` | Number of distinct unmatched atoms |
| `source_files`, `source_files_unreadable`, `source_files_unreadable_list` | Source files the spans point to, how many could not be read (their `text` and `assigned_word` are null) and their names |

`critical_path` (path 1): `index`, `slack_ns`, `arrival_ns`, `required_ns`, `startpoint`,
`endpoint`, `data_delay_ns` and `spans`, an ordered list of

| Field | Meaning |
|---|---|
| `span` | `file:line`, or null for a port or an unmatched node |
| `kind` | `matched`, `port` or `unmatched` |
| `file`, `line`, `module` | Where it came from; `module` is the module instance path |
| `loc` | With a name map: the full span `file:L.C-EL.EC` of the first source leaf of the first atom |
| `chain`, `chain_text` | With a name map: the constructs around that leaf, leaf first, up to the module (`level`, `kind`, `name`, `span` each), and the same as one line: `stmt ASSIGN 16.5-16.20 < branch IF_ELSE 14.9-18.12 < block ALWAYS 12.5-20.8 < module top` |
| `words` | Source names of the nets (a bus gives its word; a generated net gives the wire that holds it, if any) |
| `words_via` | For each word, how it was found (see "word_via" below) |
| `cell_delay_ns`, `net_delay_ns`, `delay_ns` | Cell delay, interconnect delay on the nets these atoms drive, and their sum |
| `share` | `delay_ns` over `data_delay_ns` |
| `atoms` | The VPR atoms collapsed into this entry |

`spans`: one entry per source line on any attributed path, sorted by `critical_delay_ns`:
`id` (`file:line`), `file`, `line`, `text` (the source line, null if the file cannot be read),
`modules`, `words`, `words_via`, `assigned_word` (guessed from `text`, null if it is not an assignment),
`paths_through` (paths that contain it, each counted once), `worst_slack_ns`, `nodes`,
`critical_delay_ns`, `critical_share`.

`modules`: `module`, `words`, `port_nodes` (how many of `nodes` are I/O pads) and the same
statistics (`paths_through`, `worst_slack_ns`, `nodes`, `critical_delay_ns`, `critical_share`).
`words`: `module`, `word`, `port`, `bits` (declared bit indices seen), `word_via` and the same
statistics. A word seen on an I/O pad (`port` true) is a separate entry from the same name on an
internal net.

Which nodes each table counts (`nodes`, `paths_through` and the delays use exactly these):

| Table | Nodes counted |
|---|---|
| `coverage` | all path nodes: `matched`, `port` and `unmatched` |
| `spans` | `matched` nodes (the ones with a source location) |
| `modules` | `matched` and `port` nodes (a pad belongs to the top module); `port_nodes` says how many |
| `words` | `matched` and `port` nodes that have a word; pads in their own `port` entries |
| `critical_path.spans` | every node of path 1; consecutive nodes of one span merge, a pad is `kind` `port` |

With a name map, each node of `paths` also has `loc` and `chain_text` (null for a pad or an unmatched atom), and `coverage` has the shares of the non-pad path nodes that reach each level: `net_nodes`, `span_nodes` / `span_share` (a line-level span), `loc_nodes` / `loc_share` (a full span), `chain_nodes` / `chain_share` (a chain), and for the generated nets (no source name) `generated_nodes`, `generated_word_nodes` and `generated_word_share`.

`paths`: every parsed path: `index`, `slack_ns`, `arrival_ns`, `required_ns`, `startpoint`,
`endpoint` and `nodes`, in path order, each with `atom` (VPR block name), `block_type` (`.names`,
`.latch`, `adder`, `.input`, ...), `pins` (the pins of the atom the path uses), `cell_delay_ns`,
`net_delay_in_ns` (interconnect delay of the net that arrived at this atom, from the previous
one), `delay_ns` (its contribution: cell delay plus the delay on the net it drives), `category`,
`span`, `module`, `word`, `bit` and `word_via`.

`word_via` (nodes, `words_via` of spans, `word_via` of words) is how the word was joined, so a
consumer can tell a fact from a derived guess. It is `name` when the word is the name of the net in
the provenance name or in the netlist JSON (a fact: `S[3]` is word `S`, bit 3), `pad` for the port
of an I/O pad (a fact), `declarator` when the net has only a generated name and the construct chain
of its records holds a `DECLARATOR` (the word is that declaration's name; a fact of the name map,
bit unknown), and `json-id` when the net has only a generated name (`$n312`) and the
word comes from the heuristic that the JSON writer numbers net `ID` as bit `ID + 1`: derived, and
right only as far as that numbering holds. It is null when there is no word. `assigned_word` is a
further guess, from the text of the line. A bit is the declared index (`offset` and `upto` of the
JSON netname are applied). When several source names hold one net, the shortest name wins, then
the smaller one; that choice is deterministic and has no other meaning.

Layout: `schema_version` stays 1 because every round only added fields. The name map round added,
for a flow directory with a name map only:

- `design.name_map` (the map's path);
- `loc` and `chain` (spans of `critical_path`) and `loc` and `chain_text` (nodes of `paths`);
- the `coverage` fields `net_nodes`, `span_nodes`, `loc_nodes`, `chain_nodes`, `generated_nodes`,
  `generated_word_nodes` and the shares `span_share`, `loc_share`, `chain_share`,
  `generated_word_share`;
- `reason` on the rows of `coverage.unmatched_atoms` (`not in the name map`, `in the name map only
  under another module than <top>`, or `in the name map but no source location`);
- the `word_via` value `declarator`;
- in `summary.txt`, a line `chain: ...` under each statement of the critical path.

`design.provenance_blif` is a string on both routes (empty for the name map route). A flow directory
without a name map gives exactly the older output, byte for byte (no field above, no `reason`). The
earlier round added (`paths_limit`,
`paths_in_report`, `paths_kept`, `paths_skipped`, `paths_skipped_reasons`,
`coverage.matched_share_excl_port`, `coverage.source_files*`, `word_via`, `words_via`,
`words[].port`, `modules[].port_nodes`); no field changed meaning. `paths_parsed` is kept and equals
`paths_kept`.

## How the join works

1. VPR names each atom after the net its first output drives (an I/O pad after the port). Those
   names are the net names of the BLIF VPR reads. With `--prov` they are hash names, and the name
   map `<blif>.names` has one line per name.
2. A timing atom is `matched` when its name is a line of that map (`O`, or `N` for a net that
   keeps its name, such as a port bit): the file and line, the module instance path and, for a
   source net such as `S[3]`, the word `S` and bit 3. The record graph of the line gives the full
   span `L.C-EL.EC` of the first source leaf and the construct chain around it. A net with only a
   generated name gets the word of a `DECLARATOR` in the chain, if the chain has one, else the
   `json-id` guess below. An I/O pad is `port`. The map decides: an atom that is not a line of it
   is `unmatched` (with a `reason`), never looked up elsewhere. Only an `N` line that has no source
   leaf is looked up in the `write_json --odin3` netlist (the driver cell's first source span; the
   node's `via` says `netlist-json (map: no leaf)`). Everything else is `unmatched` and listed. The
   name of a net comes from the full name of its `O` line (the module path and the `@file:line`
   tail taken off; a `/` inside an escaped identifier stays); a full name with nothing usable left
   gives no word, and the hash name is never a word. The `DECLARATOR` word is taken from the chain
   of the same leaf that gives `loc`.
   Before it joins, the tool checks that the map belongs to the BLIF: if fewer than 90% of the BLIF's
   net names (the primary outputs and the constants not counted) are in the map it stops with an
   error that gives the share, because the map is then from another run.
3. A flow directory made by version 1.1.1 has no map. It holds a second BLIF with
   `--name-style provenance` instead (net names `<module path>/<net>@<file>:<line>`), and the tool
   pairs the two files token by token and refuses with a message if they do not line up. That
   route gives line-level spans and no chain.

The delay model: a row of the VPR report that stays inside one atom is a cell delay; the first row
of the next atom carries the interconnect delay of the net the previous atom drives, and that delay
is charged to the previous atom's statement (the net belongs to its driver).

## After the attribution: packing and placement

The same name map can carry what VPR did with each atom. Run the flow with `--keep-results` as well
(`odin3-flow --prov --vpr --keep-results`; `--timing-attr` implies `--prov`), then
`odin3-prov-post FLOW_DIR`. The tool reads VPR's `.net` and `.place`, joins every atom by its name
(the hash name the BLIF holds) and adds the BLE, CLB and site records and the `A` lines to
`odin3/<top>.blif.names`; `odin3-timing` reads the extended map as before. The tool runs once per
map.

## Limits

- Setup paths only; a hold path in the setup report is skipped and counted in `paths_skipped`.
  A report with no `slack` line at all (cut off) is an error; a truncated last path is skipped and
  counted.
- The source text is read from the file name the provenance carries. The flow records the design
  paths and the directory odin3 ran in in `timing/inputs.json` (`designs`, `source_cwd`), and
  `odin3-timing` looks there, never in the directory it is started from. `source_cwd` is relative to
  the flow directory; `source_cwd_abs` records the absolute directory as a fallback, used when the
  relative one does not exist (flow directory moved without the design tree). `timing/inputs.json`
  is a run output and holds absolute paths. A file it cannot read is
  counted in `coverage.source_files_unreadable` and named in `summary.txt`.
- With a name map, `loc` is the full span of the first source leaf and `span` stays `file:line` (the
  aggregates, `spans`, are by line, as before). A flow directory from version 1.1.1 has line-level
  spans only: the provenance name carries a line, not a column, so a statement that spans several
  lines (an `always` block) is the line where its first location is. A register inside a clocked
  block is attributed to the `always` line, its logic to the assignments.
- The chain exists when the read kept the constructs (`odin3-flow --prov` reads with
  `--prov-ast refs`). A net made from restored locations or imported from a netlist has a span
  and no chain.
- A generated net (`$nNNN`) gets a word when its chain holds a `DECLARATOR` (name map) or when the
  netlist JSON names its wire (registers and ports do; the nets inside combinational logic mostly
  do not: `flatten`, `opt` and `lower` do not keep wire names for them). `assigned_word` is a guess
  from the source text for the statements without one.
- Atoms that VPR renames or merges, or that the provenance writer does not name, are `unmatched`.
  Their share is in `coverage`. On the two micros and the declarator design of the test fixtures,
  taken from a real VPR 9 run, VPR kept every name (an adder atom is its first output net, a latch
  its output, a pad its port) and no atom was unmatched.
- The attribution needs the VPR run that routed the BLIF in the flow directory. The flow directory must have
  been made with `--timing-attr`.
