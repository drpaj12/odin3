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
flow adds two writes to the Odin III step (a second BLIF with provenance names and
`write_json --odin3`; both only read the netlist, so the BLIF VPR reads is byte-identical) and, when
N is not 100, the VPR report option `--timing_report_npaths N`. Placement, routing and the QoR
figures do not depend on it (see `docs/V1.1.1.md` for the check).

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
| `prov.blif`, `netlist.odin3.json`, `inputs.json` | The inputs of the join (`odin3-timing` reads them) |
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
`provenance_blif`, `netlist_json` (null when absent).

`timing`:

| Field | Meaning |
|---|---|
| `cpd_ns`, `fmax_mhz` | From VPR's `Final critical path delay` line (null when absent) |
| `paths_requested` | `--timing-paths` (null for a flow dir that does not say) |
| `paths_parsed` | Paths in `paths` (VPR reports fewer than requested on a small design) |
| `worst_slack_ns` | Slack of path 1 |
| `critical_data_delay_ns` | Sum of the attributed delays of path 1 (launch clock latency and setup time excluded) |

`coverage` (never dropped silently):

| Field | Meaning |
|---|---|
| `nodes`, `matched`, `port`, `unmatched` | Counts of path nodes (atoms on the paths; an atom on k paths counts k times) |
| `matched_share`, `port_share`, `unmatched_share` | The three counts over `nodes` |
| `critical_matched_delay_share`, `critical_port_delay_share`, `critical_unmatched_delay_share` | The same split by delay on the critical path |
| `unmatched_atoms` | Up to 200 entries `{atom, block_type, nodes, paths}`, most frequent first |
| `unmatched_atoms_total` | Number of distinct unmatched atoms |

`critical_path` (path 1): `index`, `slack_ns`, `arrival_ns`, `required_ns`, `startpoint`,
`endpoint`, `data_delay_ns` and `spans`, an ordered list of

| Field | Meaning |
|---|---|
| `span` | `file:line`, or null for a port or an unmatched node |
| `kind` | `matched`, `port` or `unmatched` |
| `file`, `line`, `module` | Where it came from; `module` is the module instance path |
| `words` | Source names of the nets (a bus gives its word; a generated net gives the wire that holds it, if any) |
| `cell_delay_ns`, `net_delay_ns`, `delay_ns` | Cell delay, interconnect delay on the nets these atoms drive, and their sum |
| `share` | `delay_ns` over `data_delay_ns` |
| `atoms` | The VPR atoms collapsed into this entry |

`spans`: one entry per source line on any attributed path, sorted by `critical_delay_ns`:
`id` (`file:line`), `file`, `line`, `text` (the source line, null if the file cannot be read),
`modules`, `words`, `assigned_word` (guessed from `text`, null if it is not an assignment),
`paths_through` (paths that contain it, each counted once), `worst_slack_ns`, `nodes`,
`critical_delay_ns`, `critical_share`.

`modules`: `module`, `words`, and the same statistics (`paths_through`, `worst_slack_ns`, `nodes`,
`critical_delay_ns`, `critical_share`). `words`: `module`, `word`, `bits` (bit indices seen) and the
same statistics.

`paths`: every parsed path: `index`, `slack_ns`, `arrival_ns`, `required_ns`, `startpoint`,
`endpoint` and `nodes`, in path order, each with `atom` (VPR block name), `block_type` (`.names`,
`.latch`, `adder`, `.input`, ...), `pins` (the pins of the atom the path uses), `cell_delay_ns`,
`net_delay_in_ns` (interconnect delay of the net that arrived at this atom, from the previous
one), `delay_ns` (its contribution: cell delay plus the delay on the net it drives), `category`,
`span`, `module`, `word` and `bit`.

## How the join works

1. VPR names each atom after the net its first output drives (an I/O pad after the port). Those
   names are the net names of the BLIF VPR reads, which are short names such as `$n312`.
2. The same Odin III run writes a second BLIF with `--name-style provenance`: the same blocks in the
   same order, with net names `<module path>/<net>@<file>:<line>`. The tool pairs the two files
   token by token and refuses with a message if they do not line up.
3. A timing atom is `matched` when its name is in that map: the file and line, the module instance
   path and, for a source net such as `S[3]`, the word `S` and bit 3. An I/O pad is `port`.
   A block whose net keeps a port name (the provenance writer leaves ports alone) is looked up in the
   `write_json --odin3` netlist instead: the driver cell's first source span. Everything else is
   `unmatched` and listed.

The delay model: a row of the VPR report that stays inside one atom is a cell delay; the first row
of the next atom carries the interconnect delay of the net the previous atom drives, and that delay
is charged to the previous atom's statement (the net belongs to its driver).

## Limits

- Setup paths only; hold paths are not attributed.
- Source locations are line-level: the provenance name carries a line, not a column, so a statement
  that spans several lines (an `always` block) is the line where its first location is. A register
  inside a clocked block is attributed to the `always` line, its logic to the assignments.
- A generated net (`$nNNN`) gets a word only when the netlist JSON names its wire (registers and
  ports do; the nets inside combinational logic do not: `flatten`, `opt` and `lower` do not keep wire
  names for them). `assigned_word` is a guess from the source text for the statements without one.
- Atoms that VPR renames or merges, or that the provenance writer does not name, are `unmatched`.
  Their share is in `coverage`.
- The attribution needs the VPR run that routed the BLIF in the flow directory. The flow directory must have
  been made with `--timing-attr`.
