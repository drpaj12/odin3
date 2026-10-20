# Phase 6 (6.1 to 6.5): analysis flow and provenance: design

Status: v1 (2026-10-21), **awaiting Peter's approval** (CLAUDE.md rule 2); no code before it. The
open questions of section 9 carry **agent defaults** (the recommendation of each); Peter may override.
Rules have IDs (**G** 6.1, **C** 6.2, **E** 6.3, **V** 6.4, **P** 6.5) so a rule can be changed line by
line; each ends with its test (**T**). Peter's priority (2026-10-20): the front end through
elaboration and the research flow (reverse engineering, graph analysis, provenance to HDL) come first;
VTR-flow links come second, so the VPR leg of 6.5 is the last and lightest part.

Goal (PHASE6.md): read gate-level netlists, see them in Graphviz, export them to graph tools,
levelize them, and trace every object of the final netlist back to the HDL. The experiments (D10
follow-up 2) are plugins after v1.0 and are out of scope.

Inputs read: `docs/PHASE6.md`; `docs/DESIGN.md` 4.4, 5.3, 9; `docs/IR.md` 6 (IR-12, IR-13); the 5.3
spec (J-9, M-11) and 5.4 spec (vocabulary, X-1 to X-9, W-3, D-5); code: `src/backends/dot/writer.{h,c}`,
`src/passes/write_formats.c`, `src/backends/json/{emit.c,writer.c}`, `src/passes/dump_locs.c`,
`src/frontends/project/flist.c` (`-v`, `-y`), `src/frontends/blif/reader.h`,
`src/passes/abc/region.h` (Kahn with loop cut), `src/ir/celltype.h` (sequential flags),
`plugins/python/{odin3.py,walk.py}`, `tools/vtr-qor/`.

## 0. What the repo already gives, and what it does not

**Already there.**
1. `-v <file>` and `-y <dir>` (with `+libext+`) are project words (`flist.c`); `read_verilog` resolves
   an undefined module name through them. So a cell library given as Verilog models needs no new
   reader: the models elaborate like any module. `read_verilog --auto-blackbox`, `--lib`, and the
   `flatten` pass exist.
2. `read_blif` reads `.model`, `.subckt`, `.names`, `.latch`, `.blackbox`, `.attr`; `--hdl-locs` (5.4d).
3. `odin3_dot_write` (`write_dot <file> [--focus F] [--max-nodes N]`): clusters per module, a node per
   cell, an edge per driver-to-sink pin pair, label = cell type and name. No colour, no locations.
4. `write_json` writes the Yosys schema; `read_json` (5.3) skips `odin3_*` keys through the table
   `odin3_json_ext_keys[]` (J-9), reserved for 6.3. Attributes already go out as `attributes`,
   including the `odin3_fsm_*` of 4.8. 5.4 adds `src` with the HDL location text.
5. `abc` has a Kahn sweep that cuts a combinational loop with a warning (`odin3_abc_region_build`),
   and the registry says which cell types are sequential (`SEQ_EDGE`, `SEQ_LEVEL`).
6. `dump_locs` lists `module|kind|name|SRC` per module, wire, net and node (5.4); `tools/loc-check`
   (5.4e) measures a write and read back. `plugins/python/odin3.py` is a cffi binding.

**Not there.** A gate-level fixture set and its equivalence leg; dot colours and location labels; the
`odin3_*` JSON keys and a loader that needs no libodin3; `levelize`; the coverage tool and its classes.

## 1. 6.1 Gate-level netlist ingest (G-1 to G-7)

**G-1 No new reader.** Structural Verilog over a cell library is read by `read_verilog` with the cell
models as `-v`/`-y` library files; BLIF by `read_blif`. 6.1 adds fixtures, a conformance tool and
fixes for any defect it finds; a defect fix is its own task and is reported (a reader behaviour
change needs Peter's attention if it alters an existing golden). T: G-5.

**G-2 The cell set.** `tests/netlist/cells/cmos.v` has Verilog models of `BUF`, `NOT`, `NAND2`, `NOR2`,
`DFF` (positive edge) and `DFFSR` (positive edge, asynchronous set and reset), with port names fixed
by the file. T: each model elaborates (`read_verilog` of the file alone, a `--lib` read).

**G-3 Two modes, one switch.** Default: cells stay cells (each model an IR module, each use an instance
node, hierarchy and names kept). Flattened: the existing `flatten` pass then `proc`, `lower` bring
the netlist to IR primitives. No new option; the flow is the pass sequence. T: G-5, G-6.

**G-4 Names and hierarchy kept.** Instance names, net names and bus bits of the netlist survive
(`dump_locs` names equal the netlist's, escaped identifiers included); a Yosys-flattened netlist keeps
its `a.b.c` names as plain names. T: the dump of the ingested design lists every instance and net
name of the fixture.

**G-5 Equivalence.** For each micro of the fixture set (G-7) the Yosys-mapped Verilog netlist and the
same micro as BLIF (both from Yosys 0.55, committed, generator script kept in the fixture directory)
are read, flattened and lowered, written as BLIF, and `tools/equiv-check` reports equivalent to the
RTL's own Odin III BLIF. T: `netlist_ingest_<micro>` for Verilog and for BLIF.

**G-6 Provenance of an ingested object.** A Verilog netlist object gets a `SOURCE` record at the
netlist file's line; a BLIF object an `IMPORTED` record. These are **netlist** locations (5.4
vocabulary), not HDL locations. Where the netlist carries Yosys `src` attributes, `read_verilog
-netlist` (5.4c) restores the HDL location; the fixtures include one with `src` and one without (the
reverse-engineering case: no `src`). T: the dump of each shows the expected kind and, for the `src`
fixture, the RTL file:line.

**G-7 Fixture micros.** Taken from `tests/micro/verilog` plus two hand-written ones (a 4-bit
register with enable, a combinational cone with reconvergence) so every cell of G-2 occurs; the
list is fixed in the plan. An unknown cell in a netlist is `unresolved_module`, located (no change);
with `--auto-blackbox` it is a black box. T: both.

## 2. 6.2 `write_dot --color-by` and location labels (C-1 to C-8)

**C-1 Option.** `write_dot <file> --color-by <attr> [--locs]`. `<attr>` is a node attribute key. A
node is filled with the colour of the text form of its attribute value (integer in decimal, string as
is, bits as the bit text); a node without the attribute is in the class `(none)`. Empty `<attr>` is
`INVALID_ARG`. Attribute on no drawn node: one warning, everything `(none)`, exit OK.

**C-2 Colours.** A fixed palette of 12 fills (fixed hex values in `writer.c`, colour-blind safe, text
stays readable) and a light grey for `(none)`. Values are sorted bytewise and take palette entries in
order; more than 12 values reuse entries with one warning naming the count. Deterministic: the same
design and attr give the same bytes. Node style is `style=filled, fillcolor="#rrggbb"`.

**C-3 Legend.** One extra cluster `cluster_legend` with a node per class, label `<value> (<n>)` where
n counts drawn nodes. The legend is outside the node budget and outside focus. Legend labels use the
existing escaping.

**C-4 Locations.** `--locs` adds a second label line `file:line` (the first HDL location, the first
`SOURCE` leaf with a location, via `odin3_hdl_locs` of 5.4) and `+N` when the node has N more. A node
with none has no second line (no netlist location is printed: it is not an HDL location). Off by
default so existing dot goldens do not change.

**C-5 Interplay.** `--focus` and `--max-nodes` behave as today; `--color-by` and `--locs` work with
every focus kind. Cluster, node and edge order unchanged.

**C-6 Pseudo attribute.** `--color-by @type` colours by cell type name. (Question 2.)

**C-7 Escaping.** Values and locations go through `dot_put_escaped` unchanged (quotes, backslash,
newline, `&`).

**C-8 Tests.** T: (a) a micro with `my_class` set to three values on some cells: expected fills,
legend counts, `(none)` for the rest; (b) `--locs` on a Verilog micro: the labels carry the right
`file:line` and a BLIF-read design gets none; (c) 13 values: reuse and warning; (d) missing attr:
warning; (e) output of every case parses with Graphviz `nop` when `dot` is installed (the test says
`skipped` otherwise and a structural check of balanced quotes and braces always runs); (f) no new
option: output byte-identical to today.

## 3. 6.3 Graph export (E-1 to E-8)

**E-1 Option.** `write_json --odin3 <file>` adds the Odin III keys; without `--odin3` the file is
byte-identical to today's (netlistsvg, nextpnr consumers unchanged). (Question 3.)

**E-2 Keys (version 1).** Root: `"odin3_version": 1`. Per module: `odin3_prov`. Per cell and per
netname: `odin3_prov` and, when the object has the attribute `odin3_level` (6.4), `odin3_level` (an
integer). `odin3_prov` is an object `{"kind": "SOURCE"|"IMPORTED"|"DERIVED", "hier": "<hier path>",
"hdl": ["<SRC>", ...]}` with `hdl` the HDL locations in the 5.4 text, up to 8 (W-3), `[]` when none,
`hier` `""` when none. The module path of a cell is the module that contains it (JSON nests cells in
modules); the instance path of a flattened cell is its `hier`. Attributes (including `odin3_fsm_*`)
are the existing `attributes`; nothing else is added. Key order is fixed.

**E-3 `read_json`.** The `odin3_json_ext_keys[]` table gets handlers: `odin3_level` becomes the attribute
`odin3_level` again (integer); `odin3_prov` is validated and counted, not turned into records (HDL
locations come back through `src`, 5.4); `odin3_version` above 1 is the J-9 error. Unknown `odin3_*`
keys are skipped and counted as today. T: `write_json --odin3`, `read_json`, `write_json --odin3` gives
the same bytes for every micro; the levels survive.

**E-4 Graph definition for external tools.** One node per IR cell and per module port (constant bits are
not nodes; the JSON writes them as `"0"`/`"1"`), one edge per driver-to-sink pin pair on a net whose
driver is a node (the definition of the dot edges). Node features: type, parameters, attributes,
`odin3_level`, `odin3_prov`; edge features: net name, driver port, sink port.

**E-5 Python loader without Odin III.** `plugins/python/odin3_graph.py`, pure Python plus the standard
library: `load_json(path, module=None) -> networkx.MultiDiGraph` (a single module needs no `module`)
and `to_pyg(graph)` returning a `torch_geometric.data.Data` (imports torch and torch_geometric only when
called). It does not import `odin3.py` and does not need `libodin3`. T: run in a clean interpreter
(`python3 -I`) from a temp copy of the file; counts below.

**E-6 Count test.** For each micro the loader's node and edge counts equal the IR's, counted with
the E-4 definition through `plugins/python/walk.py` (cffi) on the same design. T: `graph_export_counts`.
Skipped with a message when networkx is missing; the Torch path is tested only when importable.

**E-7 Levels in the graph.** Run after `levelize` the loaded graph has `odin3_level` on every node; a
test builds the graph, groups by level, and checks each edge goes from a lower or equal level (equal
only across a register or a cut loop).

**E-8 Version policy.** Adding a key is a minor change (readers skip it); changing a key's meaning
raises `odin3_version`. The loader refuses `odin3_version` above 1.

## 4. 6.4 `levelize` (V-1 to V-8)

**V-1 Pass.** `levelize [--key NAME]` (default `odin3_level`), over every module of the design. Netlist
or word view; a module with a `$proc` node is `INVALID_ARG` ("run proc first"). Structure and
provenance are not changed; only attributes are written. Idempotent: a second run gives the same.

**V-2 Definition.** A net is a source (level 0) when it is an input port net, a constant, undriven, or
an output of a sequential cell (`SEQ_EDGE` or `SEQ_LEVEL`, the registry's own flags). The level of a
combinational cell is `1 + max` of the levels of its input nets (1 when it has only source inputs; 0
for a cell with no inputs); its output nets have that level. A sequential cell has level 0 and its
outputs level 0 (it starts a path). Black boxes and hard blocks are combinational cells (no registry
flag says otherwise). (Question 4.)

**V-3 Hierarchy.** An instance of a design module is one combinational cell (every output depends on
every input). Run it after `flatten` for true depths; the pass logs an info line when it meets an
instance.

**V-4 Loops.** Kahn's algorithm. On a stall the lowest-ID remaining cell is cut as the abc region
does: one warning `levelize: module '<m>': combinational loop through <type> '<cell>' (<file>:<line>)`
(location of its first provenance leaf of any kind, omitted when none), its outputs become level 0,
the cell gets attribute `<key>_cut` = 1, and the sweep continues; the pass returns OK. Deterministic.

**V-5 Output.** The attribute `<key>` (integer) on every cell (port nodes excluded) and on every net;
final info line `levelize: M modules, N cells, max level L, K loops cut`.

**V-6 Cost.** One sweep, O(nodes + pins), no recursion. T: a 100000-cell chain levels without stack
growth within the test's time cap.

**V-7 Tests.** (a) hand counts: 3-gate chain 1,2,3; reconvergent paths take the max; a register
resets the level; a constant input; a cell fed by an input and a deeper cone; (b) a loop of three gates
warns once at the lowest-ID cell with file:line, and the levels of the rest are as stated; a self-loop;
two disjoint loops warn twice; (c) rerun is identical; (d) levels survive `write_json --odin3` then
`read_json` (E-3); (e) a design with `$proc` is refused.

**V-8 Registration.** One pass per PR (rule 1): `src/passes/levelize/`, a row in `docs/PASSES.md`,
registered last in the built-in table.

## 5. 6.5 Provenance coverage gate (P-1 to P-8)

**P-1 Final-netlist objects.** Counted per design: LUTs (`$lut`), latches and flip-flops (the sequential
bit-level cells), hard blocks (hard or black-box cells made by `hardmap`), LPM instances (`lpm_*`,
`altsyncram`). **Resolved** = the object has at least one HDL location (`dump_locs` SRC non-empty:
5.4 vocabulary; a netlist location does not count).

**P-2 Legs.** (a) BLIF after `abc` (VTR dialect, in memory); (b) the same BLIF written and read back with
`--hdl-locs`; (c) LPM Verilog (Phase 5.1) in memory and written/read back with `-netlist`; (d) VPR
placed blocks from the `.place` file (last, heavy). Legs (b) and (c) need 5.4. Each leg is per design.

**P-3 Named exception classes**, each counted, none silent: `const` (constant drivers), `writer-buffer`
(port buffers, `.names gnd`, the helper `$not` of an OFF-set cover: 5.4 X-9), `inherited` (BLIF cell
location taken from its output net, 5.4 D-5; reported both ways, question 5), `pad` (VPR I/O pads: the
port bit carries no location, 5.4 X-8). **An unresolved object outside these classes is a bug**: the
tool exits non-zero and names the first ten.

**P-4 Gate.** Per design and leg: every LUT, latch, hard block and LPM instance resolves, apart from
P-3. For leg (d): the atoms of every placed block resolve, through the block-to-netlist-name mapping
and the L-5 name decode (5.4); a block whose atoms do not is a bug unless it is a `pad`. The gate is
exit status 0 with the report.

**P-5 Recorded per design and leg.** Objects per class, resolved, coverage (resolved / objects after
removing P-3 classes), the exception counts, and the **specificity**: the median and 90th percentile of
distinct HDL lines per object (distinct file:line pairs over the object's locations; the 8-location cap
of 5.4 W-3 is noted when reached, counted as `capped`). Specificity is reported, not gated.
(Question 6.)

**P-6 Tool.** `tools/prov-cover/` (Python): drives the `odin3` CLI, uses `dump_locs` with a new option
`--types` that adds the cell type as a field (off by default: `loc-check` output unchanged), and writes
`report.json` and a table. For leg (d) it reads the `.place` of a VPR run (from `tools/vtr-qor` run
directories; the tool never starts VPR itself except through `tools/vtr-qor`).

**P-7 Design sets.** CTest default: the micros at LUT and LPM level (fast). Sweep (`prov_cover_sweep`,
`ODIN3_HEAVY_OK`): the Phase 3 design set (PHASE3 exit test) for legs (a) and (b), the Phase 5 LPM
micros (`tests/lpm`) for (c); leg (d) on the Phase 3 set under `tools/heavy-run`, smallest design
first, only with lanes idle. Peter runs no nightly; the sweep runs at PR time.

**P-8 Tests of the tool.** Unit (Python): a handcrafted dump with each class gives the right
coverage, an unresolved plain LUT fails the gate, median and p90 on a known list, a `.place` fixture
with a pad and an unresolved block. Integration: the micro set passes with all legs a to c.

## 6. Where the code goes

- 6.1: `tests/netlist/` (cells, micro netlists, BLIF, generator script), `tools/netlist-ingest/` (the
  conformance driver around `equiv-check`), fixes in `src/frontends/` only if the tool finds a defect.
- 6.2: `src/backends/dot/writer.{h,c}` (`odin3_dot_opts` gains `color_attr`, `locs`), `src/passes/
  write_formats.c` (options), `docs/PASSES.md`.
- 6.3: `src/backends/json/{emit.c,writer.c}` (`--odin3` in the writer opts), `src/frontends/json/
  reader_ext.c` (handlers), `plugins/python/odin3_graph.py`, tests under `tests/python/` or the
  existing Python test home (the plan checks).
- 6.4: `src/passes/levelize/`.
- 6.5: `src/passes/dump_locs.c` (`--types`), `tools/prov-cover/`, `docs/PHASE6.md` (numbers).
- No IR field, ABI or ADR change. Docs: PASSES.md rows, DESIGN 9 (JSON row: Odin III keys), IR notes none.

## 7. Risks

1. **Gate-level Verilog the front end has not met.** Escaped identifiers, port-less cell models, gate
   primitives, `assign` with delays in Yosys output. G-1 makes defects findable; a fix may touch the
   elaborator (own PR, rule 1).
2. **Graph counts.** The IR counts constants and port nodes in ways the JSON may not; the E-4
   definition has to be implemented identically in the loader and in `walk.py`. The test fixes it.
3. **Specificity cap.** The 8-location cap of 5.4 distorts p90 on merged logic; measured and flagged.
4. **VPR mapping.** The block-to-atom-name rule has not been checked against a real `.place`; Task 5
   starts with a fixture from a small design and may adjust P-4.
5. **Dependencies.** networkx and Torch may be absent in CI; tests skip with a message, never pass
   silently, and the skip is visible in the PR report.
6. **Heavy lock.** Leg (d) stalls every lane's build-locked while it runs.

## 8. Out of scope

Subgraph matcher, `raise`, graph algorithms, the experiments of D10 follow-up 2, large-benchmark
equivalence, VQM, EDIF, new netlist formats, rendering dot to images, a GUI.

## 9. Open questions (agent defaults taken; Peter may override)

1. **`--locs` default.** (a) opt-in flag, existing dot output unchanged; (b) on by default. **Recommend
   (a)**: stable goldens, and locations make big graphs unreadable.
2. **`--color-by @type`.** (a) allow the pseudo attribute `@type`; (b) attributes only. **Recommend
   (a)**: one line of code and the first thing a reverse engineer colours by.
3. **Odin III keys opt-in.** (a) `write_json --odin3`, default byte-identical; (b) always written.
   **Recommend (a)**: the 5.3 round-trip goldens and Yosys-side tools stay untouched.
4. **Black boxes and hard blocks in `levelize`.** (a) combinational; (b) cut points like registers.
   **Recommend (a)**: adders and multipliers are combinational in VTR; a registered hard block is
   flagged sequential in its registry entry if wanted later.
5. **Gate treatment of `inherited` BLIF cells.** (a) count as resolved, report the strict number too;
   (b) strict only. **Recommend (a)**: BLIF VTR cells carry no location of their own (5.4 X-6) and (b)
   would fail every read-back; the strict figure stays visible.
6. **Specificity.** (a) report only, set a threshold after the first sweep; (b) gate now on a guessed
   threshold. **Recommend (a)**: no data yet; PHASE6 asks for a "report".
7. **Port bits and VPR pads.** (a) pads are a named exception class; (b) make port bits carry the
   declaration location (a 5.4 follow-up) so pads resolve. **Recommend (a)** now, (b) as a later item if
   Peter wants pads in the gate.
8. **networkx and PyG in CI.** (a) tests skip when absent, install in the lane environment for the
   PR run; (b) vendor a check. **Recommend (a)**.
9. **Cell library licence/size.** (a) own small CMOS models (G-2); (b) also a SkyWater or Nangate
   subset for realism. **Recommend (a)**: nothing in PHASE6 needs more; (b) when reverse engineering
   a real netlist starts.
