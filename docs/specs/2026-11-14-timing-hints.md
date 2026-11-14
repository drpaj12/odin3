# Timing hints: two-pass flow with routed timing fed back to the passes

Date 2026-11-14. Status: Peter (PI) approved the shape the same day; this spec and the plan follow it
(logged as an agent default, rule 11). Purpose: hub `yggdrasil/docs/purpose.md`, Odin III goal 3
(provenance to HDL). Measure: speed (post-route critical path delay, CPD). Guards: LUT count, total
wirelength.

Peter's request, in their words:

- a two pass flow "that goes all the way to routing, and then with provenance goes back up with that
  information that is 'more accurate based on a pass'";
- "the provenance from routing details back to high-level should just be greedy (assume the worst) - if
  ties in pins then random";
- "we still want to use the original flows timing estimates, but want to allow for hint like timing to
  provide more accuracy";
- it is "a project thing where the flow and project provides this additional information", not Wyrd;
- "these timing hints aren't just for muxes they could be for any logic".

Why abc first. Pilot A (local note, not in this repo) measured how far the pre-route estimates are from
the routed truth: the unit-delay depth estimate (what ABC optimises) finds only 40 % (medium VTR) and
51 % (bigger OpenCores) of the routed near-critical LUTs, so the mapping step is where the estimate is
furthest off. CPD moves 2.7 % to 6.7 % between VPR seeds, so one seed cannot show an effect under about
5 %.

One shared store, one small hook per pass behind an option that is off by default, and the ABC adapter
as the first consumer. With no hints loaded, output is byte-identical to before (the golden tests do
not change).

## 1. The hint record

A hint file holds two kinds of record. Times are in ps, slack is VPR's routed slack.

| kind | key | fields |
|---|---|---|
| `cell` | `uid`: the provenance record ID of the gates of a word cell (the `uid=<n>` flag of a name map written with `ODIN3_PROV_UID=1`) | `slack_ps`, `crit` (0..1), `depth` (pass-1 unit depth of the cell's shallowest LUT), `luts` (how many LUTs the worst slack was taken over) |
| `net` | `<module>/<net>` (the net's name as the IR gives it) | `arrival_ps`, `required_ps` (either may be `-`) |

A `cell` hint is for any logic (a mux, a comparator, a flop's next-state function, an adder's glue),
not only muxes: the key is the record of the word-cell instance, whatever its type; the record's own
`cell=` flag names the type. A `net` hint is for a net at a pass boundary (an input, a register output,
a hard-block output; for outputs and register inputs the key must be a name: unnamed nets print as
`$n<ID>`); the flow does not write them (the micro tests and a user's own hints do).

Source metadata, as `meta` lines: `cpd_ps` (routed CPD of pass 1), `slack_min_ps` (its worst slack, the
reference: every slack is read as `slack - slack_min`), `depth` (mapped unit depth of pass 1, whole
design), `ns_per_level` (default `cpd_ps / 1000 / depth`), and free keys `pass`, `seeds`, `arch`, `vpr`,
`tie_seed`, `cut`, `max_cells`, `reduce` (one seed or worst over N seeds).

### File format (plain text, TAB separated)

```
odin3-timing-hints<TAB>1
meta<TAB>cpd_ps<TAB>2096
meta<TAB>slack_min_ps<TAB>-2096
meta<TAB>depth<TAB>3
meta<TAB>seeds<TAB>1
cell<TAB>717<TAB>-1800<TAB>0.86<TAB>2<TAB>3
net<TAB>top/c<TAB>3000<TAB>-
```

Blank lines and lines starting with `#` (after line 1) are ignored. A duplicate cell or net keeps the
worst (smallest slack, smallest required time, largest arrival). Errors name file, line and what is
wrong (`read_hints: <file>:<line>: ...`).

## 2. The key: what is the same object in pass 1 and pass 2

Candidates were the provenance record uid, stable cell or net names, and the HDL `src` span plus cell
kind. Decision: **the provenance record ID of the cell's gates** (`uid`).

- At abc time, `odin3_node_prov(node)` of a lowered gate is the record that the exact cut of a LUT lists
  as a parent, so the key used to write a hint (from the pass-1 name map) and to read it (from the IR)
  is the same number by construction.
- It is exact: a word cell has its own record (two cells of one `always` block have two records), so
  nothing is merged that the design keeps apart. A span plus kind merges generate-loop copies and cells
  of one expression; names are `$n<ID>` for generated cells, which is the same fragility with more
  steps.
- The price: pass 2 must run the same front end and passes before `abc`, with the same provenance depth
  and `ODIN3_PROV_UID` unset or set (the flag only prints), so the store numbers records the same way.
  The `read_hints` pass opens a pass run but makes no record, so inserting it does not shift IDs.
  Tested: two runs give the same IDs, and a run with `read_hints` gives the same IDs as one without
  (`tests/tools/test_timing_hints.py`).
- Unmatched hints are counted, never an error: at the end of the `abc` pass one INFO line gives the
  cell and net hints that matched a gate or net of the design, and one WARNING gives the count of those
  that matched nothing.

## 3. The store

A read-only side table in `src/passes/hints.c`, cached per design serial (as the provenance depth is),
loaded by `read_hints <file>` or by the project key `timing_hints <file>` (the file is read when the
first pass asks for hints). The IR, its free-form attributes (IR-10) and the ABI are not touched: ABI
stays 7. A side table is chosen over IR attributes because cell hints are keyed by a record ID of cells
that `lower` has already replaced by gates; they have no IR object to hang an attribute on.

## 4. Entry points

- Pass `read_hints <file>`.
- Project key `timing_hints <file>` (follows `abc_script`: one path, once, resolved against the project
  file; shown in `dump_project`).
- `odin3-flow --two-pass`: pass 1 (the whole flow to routing, with `ODIN3_PROV_UID=1`, a provenance
  depth that gives exact LUT cuts, and VPR seed 1 or seeds 1..N), back-annotation, the hint file, pass 2
  (`read_hints` before `abc`, the flow again). `odin3-flow --hints <file>`: one pass with a given file.
  Options for `--two-pass`: `--hint-seeds N` (default 1: seed 1; N: worst over seeds 1..N),
  `--hint-cut exact|all` and `--hint-max-cells N`.

## 5. Back-annotation (`tools/flow/timing_hints.py`)

Input: pass 1's mapped BLIF, its version-2 name map, VPR's `report_net_timing.csv` per seed. For each
LUT, the slack is the worst over all pins of the LUT (greedy: assume the worst). A LUT's gates come from
its provenance record: with `cut=exact` its parents are the gate records of the cut, each a `cell`
hint key. A LUT with an approximate cut or no cut gives no hint (an approximate cut names a whole
region, and phase A showed it marks everything near-critical); `--hint-cut all` includes approximate
cuts. A LUT whose exact cut covers more than `--hint-max-cells` word cells (default 8, the narrow cut
of phase A) gives no hint. Each cell takes the worst (smallest slack) of its LUTs; `depth` is the
smallest unit depth among them (a tighter depth only asks ABC for more). Ties: a tie between two pins
(or between LUTs of equal slack, for the recorded witness) is broken at random with a fixed recorded
seed (`tie_seed`, default 20261114); the file does not depend on it except through the witness
statistics. Worst over N seeds: the smallest slack per object over the seeds.

## 6. Estimate plus hint

A pass keeps its own estimate; a hint adjusts it only where one exists.

### Units for the abc consumer

ABC's mapper `if` without a LUT library counts one unit per LUT level. A time converts to levels with
`ns_per_level = cpd / depth` of pass 1 (routed CPD over mapped depth): one LUT level costs, on average
along the critical path, the routed critical path divided by the number of levels it has. It is a
per-design number, it does not need a delay model, and it is the same number that makes pass 1's
critical path exactly `depth` levels long. Where pass 1's critical path is dominated by hard blocks or
long routes the factor is larger and hints weigh more; where it is shallow and routing-bound
(boundtop: 2.3 ns over 2 levels) the factor is 1.2 ns per level.

### Outputs and register inputs (required times)

For a region output or register input driven by a gate with a cell hint (`depth` d, `slack` s, worst
slack `s_min`): `required = d + (s - s_min) / ns_per_level` levels. The worst routed path gets exactly
the depth it had in pass 1; a path with 1 ns of extra slack may be `1 / ns_per_level` levels deeper.
Required times below what ABC can reach make `if` map that output for minimum depth ("the earliest
arrival times are used"). Every other output keeps the budget of pass 1, `depth` (the default required
time, `if` would otherwise treat an output without one as unconstrained and map for area); in a design
with one region this is what ABC's own estimate gives. A `net` hint's `required_ps` gives the required
time directly.

### Inputs (arrival times)

A `net` hint's `arrival_ps` becomes the arrival time of the input (a later arrival makes ABC put that
input nearer the output). The flow writes none: no pass-1 object measures the arrival of an input net
before the first LUT; the hook is there for hard-block outputs, for hand-written hints and for the micro
test that proves ABC uses arrival times.

## 7. The abc adapter

The region's inputs and outputs are the ABC box boundary: inputs (`o3ci<i>`: primary inputs, hard-block
outputs, loop cuts) and register outputs (`o3q<j>`) are CIs; outputs (`o3co<i>`) and register inputs
are COs. `src/passes/abc/timing.c` computes the times above per region and writes
`abc<r>.timing` (`pi <name> <levels>`, `po`, `li` = register input named by the register's output,
`default_required <levels>`). The adapter registers an ABC command `o3_timing <file>` (bridge.c, always,
in the frame; `o3_timing -c` drops the timing data), and the exchange inserts it **immediately before
the mapper `if`** and `o3_timing -c` after the script (so `write_hie` prints no timing lines). Hints
need: the BLIF exchange (names), the linked engine (the default script; a custom script runs in the
child, which does not have the command), no `--gates`. Otherwise one WARNING says why and the hints are
ignored. With no hints loaded nothing is written and the command list is unchanged.

### What ABC does with the times, per command (read in `third_party/abc` and measured)

The measurement: the trace of `Abc_NtkCiNum` late inputs and tight outputs after every command of the
default script, with the command inserted at each position (micro `reg.v`, 2 registers, 1 late input).

| command | timing data | source |
|---|---|---|
| `read` (BLIF) | none: the reader in use (`Io_ReadBlifMv`) skips `.input_arrival` and `.output_required` ("Skipping line") | measured |
| `strash`, `ifraig`, `scorr`, `dc2`, `dch -f` | kept (strash and the Start-From copies call `Abc_ManTimeDup`); `scorr` that drops a register rebuilds the network from a different object and would lose them, so the command goes after it | `abcNtk.c:270,349,417`; measured with equal register count only |
| `if -K` | read: CI arrival (`Abc_NtkGetCiArrivalFloats`) and CO required (`Abc_NtkGetCoRequiredFloats`) feed `pTimesArr` and `pTimesReq` (`abcIf.c:115`, `ifTime.c:356-370`); a CO with no required time would be unconstrained (area mapping): hence `default_required`. Proven by the micro: a late input moves to the last LUT and the others are deepened | measured |
| `mfs2` | keeps them (no change seen), does not use them | measured |
| `write_hie` | prints `.default_input_arrival` and friends, which Odin III's BLIF reader refuses: hence `o3_timing -c` first | measured |

`&put` does not copy `vCiArrs` (unused: the AIG exchange is not supported for hints). `if` without a
LUT library honours required times through `If_ManComputeRequired` (`pTimesReq` is not NULL then) and
`DelayTarget` stays at -1.

## 8. Hook points for later consumers (none implemented now)

| pass | hook point | hint use |
|---|---|---|
| `opt_share` (and `opt_muxtree`) | where it decides to share or keep a mux tree, before the cells are rewritten | do not share a cell whose `crit` is above a threshold; the key is the cell record |
| `partial_map` | where it picks an adder or LUT-cascade form | choose the faster form for a critical cell |
| `lower` | where it picks a gate expansion (ripple / balanced) | same |
| VPR | `--place_delay`-style hints | out of scope: VPR is not an Odin III pass |

Each takes the hint from `odin3_hints_of(design)` and the key from the cell's gates or from the word
cell's own record (`odin3_node_prov`), and each keeps its own estimate when there is no hint.

## 9. Decisions (agent defaults, Peter may override)

1. Key = provenance record ID (section 2); not names and not spans.
2. Store = side table; ABI stays 7.
3. Units = `cpd / depth` ns per level (section 6); the default required time = pass-1 depth.
4. Approximate and wide cuts give no hint by default (section 5).
5. Hints only change the mapper's required and arrival times; ABC's script is unchanged.
6. Timing command goes immediately before `if`.

## 10. Tests

Unit: file parser (every error, duplicates, sort), lookup, option parsing of `read_hints` and the
project key, the command list with and without timing. Micro: a late input moves in the mapping (mapped
depth from that input drops), a tight required time changes a cell's depth. Tools: `--two-pass` on one
micro, the determinism of keys across runs and with `read_hints` inserted, the back-annotator on a
small crafted name map and net timing report. No-hint output unchanged: the existing golden and abc
tests, plus a test that a run with an empty-of-matches hint file writes the same BLIF as a run without.
