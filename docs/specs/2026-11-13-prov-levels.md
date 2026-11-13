# Provenance depth: `--prov-level` and an up-front size estimate

Date 2026-11-13. Status: approved by Peter (PI) the same day (decisions 1 to 4 below); written and
implemented by the provenance-levels lane. Purpose: hub `yggdrasil/docs/purpose.md`, Odin III goal 3
(provenance to HDL), and the Mimir near-critical data (the mux near-critical runs). Measure:
provenance bytes per depth against the estimate; the netlist is the same at every depth.

Peter's request: "in Odin III I would like a capability to ask for more or less provenance data as
levels (where we can estimate from the HDL how much that data will be)."

## Naming

`docs/user/provenance.md` already uses "level" for the granularity ladder of an object (`module`,
`block`, ..., `lut`). To keep the two apart, this feature is called the **provenance depth** in
prose. The option keeps the name Peter asked for: `--prov-level` (and the project key
`prov_level`, `odin3-flow --prov-level`).

## Why

Provenance size was set by several knobs (`read_verilog --prov-ast`, `abc --prov fine|cone`,
`abc --prov-edges N|unlimited`, `abc --prov-cut on`, `write_* --prov-map`, `odin3-flow --prov`), and
nobody knew the size before the run. A 15.9 GB `02-map.objects.jsonl` came from one AI digit netlist
design, and two VTR designs (arm_core, bgm) overflowed the default edge budget and silently got one
shared record for all LUTs (a WARNING only).

## The depths

| Depth | Name | `read_verilog --prov-ast` | abc edge budget | `abc --prov-cut` | name map (hash names) | step files (`odin3-flow`) |
|---|---|---|---|---|---|---|
| 0 | off | `off` | bulk | off | not written | no |
| 1 | statement | `refs` | bulk | off | written | no |
| 2 | ancestry | `refs` | default formula | off | written | no |
| 3 | cut | `refs` | default formula | on | written | no |
| 4 | full | `refs` | `unlimited` | on | written | yes |

What each keeps, in terms of the record chain (`docs/user/provenance.md`):

- **0, off**: nothing beyond what the IR needs. The IR always keeps its records (every pass makes
  them); no construct table is kept by the read, no map is written, and abc gives the run's LUTs one
  shared record without walking their cones.
- **1, statement**: the construct chain to word cells (the records `lower` makes carry `cell=`,
  `cw=`, `ca=`; the construct table `refs`), and the name map. The LUTs of one abc run share one
  `lossy=bulk` record. This is the abc over-budget form, chosen on purpose: no WARNING.
- **2, ancestry**: level 1 plus each LUT's region record (the logic that went into the ABC box),
  with the default edge budget (the larger of 16 edges per object plus 65536, and 4194304). A run
  over that budget still falls back to the shared record, with the WARNING it prints today.
- **3, cut**: level 2 plus `abc --prov-cut on` (the exact cut of a LUT where provable, else its
  region).
- **4, full**: level 3 plus `--prov-edges unlimited` and, in `odin3-flow`, the step files (`prov/`).

The netlist (LUTs, depth, CPD) is the same at every depth: a depth changes records and files, never
logic. The tools test `tests/tools/test_prov_levels.py` checks this.

## Today's default

With no depth requested nothing changes: `read_verilog` keeps the construct table only as
`--prov-ast` or `ODIN3_PROV_AST` say (CLI default `off`); abc uses the default formula budget, no cut
and the default records; `write_*` writes a map only with `--prov-map`; `odin3-flow` writes the map
and step files only with `--prov`. The IR records are those of depth 2 (default budget, no cut), so
**no depth requested behaves as depth 2 without the construct table, the map and the step files**.
The default level is "none requested" (decision 2: no output changes).

## Precedence

1. An explicit old option wins over the depth, field by field: `read_verilog --prov-ast X` and
   `ODIN3_PROV_AST` set the construct table; `abc --prov-edges N|unlimited` and
   `abc --prov-cut on|off` set the edge budget and the cut; `write_* --prov-map MODE` sets the map and
   its AST layer; `odin3-flow --prov` writes the map and the step files, `--prov-cut` adds the cut.
2. The command line (`--prov-level`, `--prov-budget`) wins over the project's `prov_level` and
   `prov_budget` (as `--top` and `--abc-script` do).
3. The depth then sets what is left. A bare `--prov-budget` without a level is `auto`.

The depth sets the map only where the writer already uses hash names (`write_blif --vtr
--name-style hash`, `write_json` and `write_verilog` with `--name-style hash`): it never changes
the names of a netlist. Writers with other names ignore it. `odin3-flow --prov-level L` (L above
0) adds `--name-style hash` to the BLIF write itself.

## Where the depth is read

- CLI global options `--prov-level 0|1|2|3|4|auto` and `--prov-budget SIZE` (SIZE: a positive
  integer with an optional `K`, `M` or `G`, powers of 1024). Invalid values are an option error.
- Project statements `prov_level <0..4|auto>` and `prov_budget <size>`, each at most once;
  `--dump-project` shows them (only when given).
- `odin3-flow --prov-level L --prov-budget SIZE` passes them to odin3 and adds the hash names; the
  step files (`prov/`) are written at depth 4, or with `--prov`. It reads the depth actually used
  from the `proc` INFO line.
- Python: `odin3_pass_set_prov_level(level)` and `odin3_pass_set_prov_budget(budget)`, the setters the CLI uses. It is additive; **the
  ABI stays 7**. The depth is not part of the IR, the design handle or any plugin-visible struct. A
  pass sees it through the internal `odin3_prov_depth_effective(design)`
  (`src/passes/prov_depth.h`).

## The estimate

The pass `prov-estimate` (any time; best after `proc`) logs, per depth, the predicted records
(`R` lines of the name map), parent edges, bytes of the name map and bytes of the step files. The
inputs are ten counts of the design as it is, over its live cells: cells, pins, output pins,
registers, and the **work** of the arithmetic, logic, mux, shift and multiply cells (output bits;
for a mux the output bits times its select width; for a shift the output bits times the bit length
of the width; for a multiply the width of A times the width of B), plus the rows of the construct
table the read kept (0 when `--prov-ast off`; a depth of 1 or more keeps `refs`, so ask for the
depth on the read, not after it).

Each measure of each depth is a power law in these counts, `exp(scale + sum(exponent x ln(1 +
count)))` with at most three counts (a straight line fits badly from a few cells to hundreds of
thousands). The factors are the fit of `docs/specs/2026-11-13-prov-levels-calibration.md`, written
by `tools/prov-calibrate/prov_fit.py` into `src/passes/prov_depth_fit.h`. Target (decision 4):
within 2x of the real size on every calibration design; the calibration doc reports the worst
ratio per measure, including the ones that miss the target.

Two things the estimate cannot see from the cells, both visible in the calibration:

- **Edge budget overflow.** When the cone walk of an abc run needs more than the default edge
  budget (about 4.7 million for arm_core and bgm), depths 2 and 3 fall back to one shared record
  and write the same bytes as depth 1. Which designs overflow is not predictable from the cells;
  the fit absorbs it for the calibration set.
- **Bulk against per-LUT records.** Depth 1's one shared record lists every old cell of the run as
  parents; with few LUTs and many old cells it is larger than depth 2's per-LUT records. So the
  size is **not** monotone from depth 1 to depth 2 on every design (it is from 2 to 3 to 4). The
  estimate is not asked to be, and `auto` takes the highest depth that fits, not the first.

When a depth or a budget is requested, `proc` ends with one INFO line: the depth in force, its
estimate, and the counts.

Budget (decision 3): with `--prov-budget SIZE`,

- `--prov-level auto` takes the highest depth whose estimated total (name map plus step files) fits;
- a fixed depth over the budget prints a WARNING naming both sizes and drops to the highest depth
  that fits (depth 0 writes nothing, so one always fits). It does not stop with an error.

The read comes before `proc`, so the construct table is kept for the requested depth (`auto` as 4);
a later drop only means the table was kept for nothing. The depth is resolved once, at the end of
`proc` (or at the first use, when there is no `proc`), and kept for the run.

## Limits

- The estimate is a size of the output, not of memory.
- Designs far from the calibration set (mostly tri-state, memory-heavy, black boxes) may be off by
  more than 2x; the calibration doc lists the worst ratio found. The step files are the least
  predictable measure: their size follows the number of HDL constructs in each LUT's ancestry,
  which the cell counts only hint at.
- `auto` without a budget is a WARNING and depth 4.

## Agent defaults (Peter may override)

- Level 0 and 1 both use bulk records: level 0 differs by writing no map and keeping no AST.
- The project keys and the `prov-estimate` pass name follow the brief; `prov-estimate` takes no
  arguments.
- The estimate's inputs are the ten counts above (cells, pins, outputs, registers, the work of five
  cell classes, construct rows), not the width and arms of each cell separately.
- The depth is kept in a process-wide cache keyed by design and request, not in the IR (no IR or
  ABI change).
