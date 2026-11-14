# Timing hints and the two-pass flow

A pass that runs before routing has to guess timing. The `abc` pass optimises the unit-delay depth of
the LUT network; VPR then places and routes it and measures the real delays, which are not the unit
delays. **Timing hints** carry what the routed run measured back to a second run of the same flow, so a
pass can adjust its own estimate where it has a measurement. The flow's own estimate stays; a hint only
moves it where one exists. With no hint file nothing changes: the output is byte for byte what it was.

This is a flow and project feature (the project and the flow provide the information), not a GUI one.
The hints are for any logic, not for one kind of cell. The first pass that uses them is `abc`. Spec:
`docs/specs/2026-11-14-timing-hints.md` in the repository.

## One command

```
odin3-flow design.v --top top --two-pass --out run
```

Pass 1 is the flow to routing (it needs VPR and `VTR_ROOT`), with provenance depth 3 and
`ODIN3_PROV_UID=1`. The routed per-pin slack is turned into `run/hints.tsv`, and pass 2 is the same flow
again with `read_hints run/hints.tsv` before `abc`. The output directory holds `pass1/`, `pass2/`,
`hints.tsv` and `two-pass.json` (critical path delay, LUTs, wirelength and channel width of both).

| option | meaning |
|---|---|
| `--two-pass` | the flow described above (needs `--target vtr` and VPR) |
| `--hint-seeds N` | hints from VPR seed 1 (default) or the worst slack over seeds 1 to N (extra VPR runs of the same BLIF) |
| `--hint-cut exact\|all` | LUTs with an exact cut only (default), or also approximate cuts |
| `--hint-max-cells N` | skip a LUT whose cut covers more than N word cells (default 8; 0 for no limit) |
| `--hints FILE` | one pass with a given hint file (`read_hints` before `abc`) |

Routing moves the critical path delay by a few percent between VPR seeds (2.7 % to 6.7 % between seeds 1
to 5 in the pilot), so one pair of runs cannot show an effect under about 5 %: compare over several
seeds.

## What the hints say

A hint file is plain text, TAB separated. The first line is `odin3-timing-hints` and `1`; blank lines
and lines starting with `#` are ignored.

```
odin3-timing-hints	1
meta	cpd_ps	2096.1
meta	slack_min_ps	0
meta	depth	3
meta	seeds	1
cell	717	0.0	1.0000	2	3
cell	745	420.0	0.8001	2	1
net	top/c	3000	-
```

- `meta`: `cpd_ps` (pass 1's routed critical path delay), `slack_min_ps` (its worst slack; every slack is
  read as `slack - slack_min`), `depth` (pass 1's mapped unit depth), `ns_per_level` (default `cpd_ps / 1000 /
  depth`); any other key is kept for the log (`pass`, `seeds`, `reduce`, `cut`, `max_cells`, `tie_seed`).
- `cell <uid> <slack_ps> <crit> <depth> <luts>`: the gates of one word cell (a mux, a comparator, a
  flop's next-state logic, any logic). `uid` is the provenance record ID of the cell's gates (the `uid=`
  flag of a name map written with `ODIN3_PROV_UID=1`). `slack_ps` is the routed slack of the worst path
  through the cell, `crit` its criticality 0 to 1, `depth` the unit depth of the cell's shallowest LUT in
  pass 1, `luts` how many LUTs the numbers were taken over.
- `net <module>/<net> <arrival_ps> <required_ps>`: a net by name; either time may be `-`.

**Greedy.** Each LUT takes the worst slack over all its pins, each word cell the worst of its LUTs, each
number the worst over seeds. A tie between pins is broken at random with a fixed recorded seed
(`tie_seed`, default 20261114): it only picks the witness pin that `--witness` writes.

**The key.** `uid` is the same number in both passes because the hint file is written from the name map
of pass 1 and read against the gates of pass 2, which a flow that runs the same passes with the same
provenance depth numbers in the same way. Hints that match nothing are never an error: the `abc` pass
prints one INFO line (how many cell and net hints matched) and one WARNING with the number that did not.

## Passes and keys

- `read_hints <file>`: load a hint file. A bad file is an error naming the file and the line.
- Project key `timing_hints <file>`: the same, from the project (once; the file is read when a pass first
  asks for hints). It shows in `dump_project`.
- `tools/flow/timing_hints.py`: the back-annotation on its own: `--blif`, `--map`, `--net-timing` (VPR's
  `report_net_timing.csv`, once per seed), `--cpd-ns` (once per seed), `--out`, `--cut`, `--max-cells`,
  `--tie-seed`, `--meta KEY=VALUE`, `--witness FILE`.

## What `abc` does with a hint

`abc` maps with one LUT level as its unit of delay; a time in the file becomes `ps / 1000 / ns_per_level`
levels, where `ns_per_level` is pass 1's routed critical path delay over its mapped depth (the cost of
one level, routing included, along the critical path of that design).

- An output or register input driven by a gate of a hinted cell gets the required time `depth + (slack -
  slack_min) / ns_per_level` levels: the worst routed path may not get deeper than it was in pass 1, a
  path with slack may.
- Every other output keeps the depth of pass 1 as its required time (without one, ABC would treat it as
  unconstrained and map it for area).
- A `net` hint's arrival time (an input) or required time (an output or register input) is used as given
  (ps converted to levels).

The times are given to ABC's mapper `if` through an ABC command `o3_timing`, run just before `if`; the
BLIF exchange and the default script are needed. With `--gates`, the AIG exchange (`ODIN3_ABC_EXCHANGE=aig`)
or a custom script (which runs in the subprocess), the hints are ignored with one WARNING. The abc
pass's INFO line at the end reads, for example:

```
abc: timing hints (hints.tsv): 35 of 35 cell hints match a gate of the design, 0 of 0 net hints match a net; 113 required times and 0 arrival times set
```

## Later consumers

Other passes can use the same table (`odin3_hints_of(design)`): `opt_share` and `opt_muxtree` (do not
share a critical cell), `partial_map` and `lower` (pick the faster form for a critical cell). They are
not implemented yet; the spec names each hook point.
