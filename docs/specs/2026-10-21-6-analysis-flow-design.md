# Phase 6 (6.1 to 6.5): analysis flow and provenance: design

Status: v1.2 (2026-10-21), **approved by Peter 2026-10-20** (CLAUDE.md rule 2).
v1.2: Peter's decisions applied (section 9): Q4 memory and black-box rule (V-2, V-7), levelize scope,
`inherited` and `pad` approved (P-3), Q10 and Q11 accepted.
v1.1: critique fixes (E-2/E-3/E-7 level key and round trip, V-1/V-2 view and sequential flags, V-4,
G-7, P-3 exception classes, P-6 line format, tests and risks). All
questions of section 9 are decided (Peter, 2026-10-20).
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
   and the registry says which bit-level cell types are sequential (`SEQ_EDGE`: `$_DFF_P_`, `$_DFF_N_`,
   `$_FF_`; `SEQ_LEVEL`: `$_DLATCH_P_`, `$_DLATCH_N_`; `src/ir/cells/dff.c`). The word-view storage
   types (`$dff`, `$dffe`, `$adff`, `$sdff`, `$dffsr`, `$dlatch`, `$sr`, ...: `src/ir/cells/ff.c`,
   `mem.c`) carry **no** flag (flags 0), so the registry cannot tell a word register from a gate.
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
by the file. T: each model elaborates (`read_verilog` of the file alone, a `--lib` read). Note: the IR
has no bit-level asynchronous-set/reset cell; `lower` makes `$dffsr` synchronous (`src/passes/lower/
ff.c`, PHASE3 decision 19), so after flatten and lower a `DFFSR` is a `$_DFF_P_` plus next-state
logic, the same as the RTL's.

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

**G-7 Fixture micros.** Fixed list (the plan, Task 3): `tests/micro/verilog/common/dffsr.v` (the
DFFSR case: `always @(posedge rst, posedge clk)`), `common/dff.v`, `common/dffe.v`, `common/adff.v`,
`common/mux.v`, `common/adder.v`, `micro/bm_DL_Dff_w_synch_reset.v`, plus two hand-written ones
(`regen`, a 4-bit register with enable; `cone`, a combinational cone with reconvergence), so every
cell of G-2 occurs. `gen.sh` maps Yosys 0.55 to exactly that set: `synth -flatten`, `dfflegalize`
to `$_DFF_P_` and `$_DFFSR_PPP_` only, `abc -g NAND,NOR` (gives `$_NAND_`, `$_NOR_`, `$_NOT_`), then
`techmap -map tests/netlist/cells/cmos_map.v` (a committed map file: `$_DFF_P_` to `DFF`,
`$_DFFSR_PPP_` to `DFFSR`, `$_NAND_` to `NAND2`, `$_NOR_` to `NOR2`, `$_NOT_` to `NOT`, `$_BUF_` to
`BUF`), and `check -assert` fails the script if any other cell type is left; Task 3 verifies the exact
Yosys cell names against 0.55 before relying on them. An unknown cell in a netlist is `unresolved_module`, located (no change);
with `--auto-blackbox` it is a black box whose port directions are inferred from the nets it
connects to (2D EL-11, v4.32; PHASE6 decision 5), so it may sit on driven nets. T: both.

## 2. 6.2 `write_dot --color-by` and location labels (C-1 to C-8)

**C-1 Option.** `write_dot <file> --color-by <attr> [--locs]`. `<attr>` is a node attribute key. A
node is filled with the colour of the text form of its attribute value (integer in decimal, string as
is, bits as the bit text); a node without the attribute is in the class `(none)`. Empty `<attr>` is
`INVALID_ARG`. Attribute on no drawn node: one warning, everything `(none)`, exit OK.

**C-2 Colours.** A fixed palette of 12 fills, in this order, colour-blind safe (Okabe-Ito extended,
dark text stays readable on each): `#e69f00 #56b4e9 #009e73 #f0e442 #0072b2 #d55e00 #cc79a7 #999999
#88ccee #ddcc77 #44aa99 #aa4499`; and `#e8e8e8` for `(none)`. The values live in one table in
`writer.c`. Values are sorted bytewise and take palette entries in
order; more than 12 values reuse entries with one warning naming the count. Deterministic: the same
design and attr give the same bytes. Node style is `style=filled, fillcolor="#rrggbb"`.

**C-3 Legend.** One extra cluster `cluster_legend`, emitted last (after every module cluster; dot
places it by its own layout), with a node per class, label `<value> (<n>)` where
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
warning; (e) output of every case parses with Graphviz `nop` when `dot` is installed; when `dot` is absent the
test prints the named skip `SKIP graphviz_nop: dot not installed` (never a silent pass) and a
structural check of balanced quotes and braces always runs; (f) no new
option: output byte-identical to today.

## 3. 6.3 Graph export (E-1 to E-8)

**E-1 Option.** `write_json --odin3 <file>` adds the Odin III keys; without `--odin3` the file is
byte-identical to today's for every design that has no `odin3_level` attribute (a design not
levelized; netlistsvg, nextpnr consumers unchanged). `levelize` stores `odin3_level` as an ordinary
attribute, and default `write_json` already writes every attribute under `attributes`, so a levelized
design's default output contains it there: that is today's behaviour and 6.3 does not change it.
(Question 3.)

**E-2 Keys (version 1).** Root: `"odin3_version": 1`. Per module: `odin3_prov`. Per cell and per
netname: `odin3_prov` and, when the object has the attribute `odin3_level` (6.4), the key
`odin3_level` (an integer). **One place for the level**: under `--odin3` the level is written as the
Odin III key only and the attribute `odin3_level` is **not** repeated under `attributes`; without
`--odin3` it is an ordinary attribute under `attributes`, as today. Any other attribute (including
`<key>_cut`, a `levelize --key` level under another name, and `odin3_fsm_*`) stays under `attributes`
in both modes. `odin3_prov` is an object `{"kind": "SOURCE"|"IMPORTED"|"DERIVED", "hier": "<hier path>",
"hdl": ["<SRC>", ...]}` with `hdl` the HDL locations in the 5.4 text, up to 8 (W-3), `[]` when none,
`hier` `""` when none. The module path of a cell is the module that contains it (JSON nests cells in
modules); the instance path of a flattened cell is its `hier`. Nothing else is added. Key order is
fixed.

**E-3 `read_json`.** The `odin3_json_ext_keys[]` table gets handlers: `odin3_level` becomes the attribute
`odin3_level` again (integer; the same result as reading the attribute form); `odin3_prov` is
validated and counted, **not** turned into provenance records (HDL locations come back through `src`,
5.4; no change to provenance records is proposed here: Question 10); `odin3_version` above 1 is the
J-9 error. Unknown `odin3_*` keys are skipped and counted as today. T, for every micro: write A =
`write_json --odin3`, read, write B, read, write C. (i) A and B are byte-equal after the
`odin3_prov` objects are dropped from both (levels, structure, attributes survive); (ii) B and C are
byte-equal (a fixpoint after one read). A and B are **not** claimed byte-equal as a whole, because
`odin3_prov` (kind, `hier`) is validated and not restored. If (ii) fails the lane reports it
(Question 10) and does not change provenance records.

**E-4 Graph definition for external tools.** One node per IR cell and one node per module **port**
(the `$port_in`/`$port_out`/`$port_inout` cell, whose `WIDTH` is the port's bit count: one node per
port, not per bit; constant bits are not nodes; the JSON writes them as `"0"`/`"1"`), one edge per driver-to-sink pin pair on a net whose
driver is a node (the definition of the dot edges); so a 4-bit bus between two nodes gives four edges
(`MultiDiGraph`). Node features: type, parameters, attributes,
`odin3_level`, `odin3_prov`; edge features: net name, driver port, sink port.

**E-5 Python loader without Odin III.** `plugins/python/odin3_graph.py`, pure Python plus the standard
library: `load_json(path, module=None) -> networkx.MultiDiGraph` (a single module needs no `module`)
and `to_pyg(graph)` returning a `torch_geometric.data.Data` (imports torch and torch_geometric only when
called). It does not import `odin3.py` and does not need `libodin3`. T: run in a clean interpreter
(`python3 -I`) from a temp copy of the file; counts below.

**E-6 Count test.** For each micro the loader's node and edge counts equal the IR's. The two sides
share no derivation code: the loader counts from the JSON text; the reference counts from the IR, by
the E-4 definition, through `dump_locs` (nodes: one line per node) and the C ABI via
`plugins/python/walk.py` (cffi: pins and nets, for edges). The test helper does not import the
loader. T: `graph_export_counts`.
Skipped with a message when networkx is missing; the Torch path is tested only when importable.

**E-7 Levels in the graph.** After `levelize` and `write_json --odin3` the loaded graph has
`odin3_level` on every cell node (port nodes carry none, V-5). The test builds the graph and checks
that each edge into a **combinational, not cut** cell goes from a strictly lower level (a port node
or a source counts as level 0). Edges **into a sequential cell** (a combinational level L to a
register's D, whose level is 0) and into a cut cell (V-4) are exempt: they are the edges that close
paths. The test learns which cells are sequential from the IR through the C ABI (the registry
flags), not from a type-name list.

**E-8 Version policy.** Adding a key is a minor change (readers skip it); changing a key's meaning
raises `odin3_version`. The loader refuses `odin3_version` above 1.

## 4. 6.4 `levelize` (V-1 to V-8)

Scope (Peter, 2026-10-20): `levelize` is just an algorithm to count the levels of logic, from a
combinational node perspective; additional details for timing can be added later. It counts levels
only: no delays, no timing arcs, no criticality.

**V-1 Pass.** `levelize [--key NAME]` (default `odin3_level`), over every module of the design.
`NAME` must not start with `odin3_` unless it is exactly the default `odin3_level` (the prefix is
reserved for Odin III keys; a name that does is `INVALID_ARG`), and the pass refuses an existing
attribute of that key on a cell or net unless the previous value was written by `levelize` (it has
`<key>` and `<key>_cut` or none; otherwise `INVALID_ARG`: no overwrite of a user attribute).
Netlist view (bit-level cells and module instances): a module with a word-granularity cell or a
`$proc` node is `INVALID_ARG` ("run proc, lower first"), because the word-view storage types
(`$dff`, `$mem`, ...) carry no `SEQ_EDGE`/`SEQ_LEVEL` flag (see section 0), so the registry cannot
mark them as sequential and a word-view result would be wrong. (Question 11, decided: word view is refused in v1; word-level levelize is a post-v1 item.) Structure and
provenance are not changed; only attributes are written. Idempotent: a second run gives the same.

**V-2 Definition.** A net is a source (level 0) when it is an input port net, a constant, undriven, or
an output of a sequential cell (`SEQ_EDGE` or `SEQ_LEVEL`, the registry's own flags; today
`$_DFF_P_`, `$_DFF_N_`, `$_FF_`, `$_DLATCH_P_`, `$_DLATCH_N_`). The level of a
combinational cell is `1 + max` of the levels of its input nets (1 when it has only source inputs; 0
for a cell with no inputs); its output nets have that level. A sequential cell has level 0 and its
outputs level 0 (it starts a path).

Memories and black boxes (Peter's rule, Question 4, clarified 2026-10-20), one uniform rule:
- **Memories** (hard RAM blocks, any memory cell) and **black boxes or hard blocks that state a latch or
  register** (a sequential flag in the registry, or a clocked/sequential model in the library): their
  **inputs are treated as primary outputs** (path endpoints: a path ends there) and their **outputs as
  primary inputs** (level 0 sources). No combinational path passes through them. Registers and latches
  in the netlist itself already cut paths this way; all are treated alike for uniformity.
- **All other black boxes and hard blocks** are **combinational paths**: each output takes level
  `max(levels of its input nets) + 1`, the rule of a combinational cell above.
Task 1 step 2 confirms the exact accessors (memory class and sequential flag) in `src/ir/celltype.h`
and the library model, and records them in the pass header comment.

**V-3 Hierarchy.** An instance of a design module is one combinational cell (every output depends on
every input). Run it after `flatten` for true depths; the pass logs an info line when it meets an
instance. A register kept as a library cell instance (G-3 default mode, a `DFF` module) is such an
instance, so state loops through it are combinational loops and are cut (V-4): flatten and lower
first for the meaningful levels.

**V-4 Loops.** Kahn's algorithm. On a stall the remaining cells contain at least one strongly connected
set; the pass takes the lowest-ID cell **of the remaining set that lies on a cycle** (a cell that
is only downstream of a loop is not on it; the pass finds it with a reachability walk over the
remaining cells, iterative) and cuts it, as the abc region does: one warning `levelize: module '<m>': combinational loop through <type> '<cell>' (<file>:<line>)`
(location of its first provenance leaf of any kind, omitted when none), its outputs become level 0,
its own level is 0, the cell gets attribute `<key>_cut` = 1, and the sweep continues; the pass returns OK. Deterministic.

**V-5 Output.** The attribute `<key>` (integer) on every cell (port nodes excluded) and on every net;
final info line `levelize: M modules, N cells, max level L, K loops cut`.

**V-6 Cost.** One sweep, O(nodes + pins), no recursion. T: a chain of 100000 `$_NOT_` cells (built
by the test, run under the default stack limit, `ulimit -s` as the harness sets it, no change) gets
levels 1 to 100000 and returns OK in under 10 seconds in a debug build, and the process stays within a
resident-memory bound of 256 MB (measured by the test with `getrusage`), which a recursion that
deep would exceed or crash on.

**V-7 Tests.** (a) hand counts: 3-gate chain 1,2,3; reconvergent paths take the max; a register
resets the level; a constant input; a cell fed by an input and a deeper cone; (b) a loop of three gates
warns once, naming the lowest-ID cell of the loop (the cell on the cycle, not a cell fed by it) with
file:line, and the levels of the rest are as stated; a self-loop;
two disjoint loops warn twice; (c) rerun is identical; (d) levels survive `write_json --odin3` then
`read_json` (E-3); (e) a design with `$proc` or a word-view `$dff` is refused; (f) `--key odin3_x` and an
existing attribute are refused; (g) a design with `$_DFF_P_`, `$_DFF_N_`, `$_DLATCH_P_`: each is
level 0 and starts a path; (h) a RAM between two LUT chains: the level resets at the RAM outputs
(level 0 sources, the chain after it counts from 1) and a path ends at the RAM inputs (the chain
before it keeps its own levels, none flows through); (i) a black box with no stated latch passes
levels through (outputs = max(inputs) + 1); (j) a black box with a stated latch or register follows
the memory rule: its inputs end paths and its outputs are level 0 sources, same as the RAM in (h).

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

**P-3 Named exception classes**, each counted, none silent. PHASE6 decision 2 allows only two
exceptions: constant drivers and the buffers the writers add. P-3 splits them into named classes and
adds two classes, approved by Peter on top of decision 2
(Questions 5 and 7): `inherited` and `pad`, **approved by Peter 2026-10-20** as named classes
(PHASE6 decision 2 amended, decision 3). The tool enables them (`--allow inherited,pad` is the
default); the strict figure (no exceptions) is always reported next to the resolved one.

How an object is seen. The gate counts the nodes of a `dump_locs --types` dump taken at the leg's
point (P-6). Objects the writers add are not IR nodes while the design is in memory (leg a, and the
in-memory side of c): they exist in the written file, and after read-back (legs b, c) they are
ordinary nodes of the read-back design, in its dump, without a location. So the exceptions apply to
read-back dumps; in leg (a) there is none to apply. For each class the tool also counts the same
objects in the written file text and **the two counts must be equal** (a mismatch is a bug in the
rule, fails the gate and is printed).

Exact rule per class (the tool reads the `--types` field and the net names of the dump; Task 8
confirms the shapes against `src/backends/blif/writer.c` and the 5.4 spec X-9 and records the
evidence in the tool's docstring):
- `const-driver`: a node of a constant cell type (the `$_CONST*_` cells, ANYVIEW) with no location.
- `const-table`: a zero-input `.names` (read back as a zero-input `$lut` or `$sop`) whose output
  net is named `gnd`, `vcc` or `unconn`.
- `port-buffer`: a one-input identity `$lut` (single `1 1` row) with no location whose output net
  is a module output port bit or whose input net is a module input port bit, and whose output net
  name differs from its input net name. If leg (a) has a node with a location driving the same net
  name, the object is a **lost location**, not an exception.
- `not-helper`: an inverter-type node with no location whose single input net is the output net
  of a `$sop` node (the helper `$not` of an OFF-set cover, 5.4 X-9).
- `inherited` (approved): a node whose location exists only through 5.4 D-5 (an output net's
  location); counted both ways, resolved and strict (Question 5).
- `pad` (approved): a VPR I/O pad block of leg (d): the port bit carries no location (5.4 X-8)
  (Question 7).

Anything else unresolved is a bug: the tool exits non-zero and names the first ten. The report
**always** prints per-class counts (every class, zero included), the share of objects each class
covers, and the strict figure (no exceptions), so a class cannot silently grow into a catch-all;
an exception total above the count of the written-file cross-check is a failure.

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
`--types` (off by default: the output without it is byte-identical, `loc-check` unchanged), and writes
`report.json` and a table. **`dump_locs --types` line format.** The parse rule of `src/passes/dump_locs.c` (the first three
unescaped `|` end the fields; SRC is the rest) is **kept**; the type goes **inside the kind field**
and no field is added. Default line: `module|node|name|SRC`. With `--types`, a node line is
`module|node:TYPE|name|SRC`, where TYPE is the cell type name escaped like the other fields (only
the first `:` of the kind field splits kind from type; a `:` in TYPE is plain). Module, wire, net
lines are unchanged and keep the bare kinds `module`, `wire`, `net`. A reader tells the formats
apart by the kind field alone: `node` is the old form, `node:` + text is the typed form; no header
line, and a file never mixes the forms (the option is per run). The comment at the top of
`dump_locs.c` gets one sentence for it.
For leg (d) it reads the `.place` of a VPR run (from `tools/vtr-qor` run
directories; the tool never starts VPR itself except through `tools/vtr-qor`).

**P-7 Design sets.** CTest default: the micros at LUT and LPM level (fast). Sweep (`prov_cover_sweep`,
`ODIN3_HEAVY_OK`): the Phase 3 design set (PHASE3 exit test) for legs (a) and (b), the Phase 5 LPM
micros (`tests/lpm`) for (c); leg (d) on the Phase 3 set under `tools/heavy-run`, smallest design
first, only with lanes idle. Peter runs no nightly; the sweep runs at PR time.

**P-8 Tests of the tool.** Unit (Python): a handcrafted dump with each class gives the right
coverage, an unresolved plain LUT fails the gate, median and p90 on a known list, a `.place` fixture
with a pad and an unresolved block; per class, the dump count equals the written-file count, and a
mismatch fails; `inherited` and `pad` are enabled by default and `--allow` can narrow the set. Integration: the micro set passes with all legs a to c.

## 6. Where the code goes

- 6.1: `tests/netlist/` (cells, micro netlists, BLIF, generator script), `tools/netlist-ingest/` (the
  conformance driver around `equiv-check`), fixes in `src/frontends/` only if the tool finds a defect.
- 6.2: `src/backends/dot/writer.{h,c}` (`odin3_dot_opts` gains `color_attr`, `locs`), `src/passes/
  write_formats.c` (options), `docs/PASSES.md`.
- 6.3: `src/backends/json/{emit.c,writer.c}` (`--odin3` in the writer opts), `src/frontends/json/
  reader_ext.c` (handlers), `plugins/python/odin3_graph.py`, Python tests in `tests/tools/` (the
  existing home of the Python tests; there is no `tests/python/`).
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
7. **`--key` collisions.** A `levelize --key` that collides with a user attribute or an `odin3_`
   key would overwrite data or confuse the JSON keys; V-1 reserves the prefix and refuses an
   existing key.
8. **`odin3_level` in default JSON.** A levelized design's default `write_json` shows the level as an
   ordinary attribute (today's behaviour); E-2 keeps one place per mode, and E-1's byte-identity
   holds for designs that are not levelized.
9. **Exception classes weakening the gate.** Every class is a way to pass without a location.
   P-3 keeps each narrow, counted per class, cross-checked against the written file, and two of them
   (`inherited`, `pad`) are approved by Peter, with the strict figure always reported.
10. **Word-view storage unflagged.** `levelize` refuses word view (V-1) because the registry has no
    sequential flag on `$dff` and the like; supporting it needs a registry change (Question 11).

## 8. Out of scope

Subgraph matcher, `raise`, graph algorithms, the experiments of D10 follow-up 2, large-benchmark
equivalence, VQM, EDIF, new netlist formats, rendering dot to images, a GUI.

## 9. Open questions (all decided, Peter, 2026-10-20)

1. **`--locs` default.** (a) opt-in flag, existing dot output unchanged; (b) on by default. **Recommend
   (a)**: stable goldens, and locations make big graphs unreadable.
   **Decided (Peter, 2026-10-20): option (a), as recommended.**
2. **`--color-by @type`.** (a) allow the pseudo attribute `@type`; (b) attributes only. **Recommend
   (a)**: one line of code and the first thing a reverse engineer colours by.
   **Decided (Peter, 2026-10-20): option (a), as recommended.**
3. **Odin III keys opt-in.** (a) `write_json --odin3`, default byte-identical; (b) always written.
   **Recommend (a)**: the 5.3 round-trip goldens and Yosys-side tools stay untouched.
   **Decided (Peter, 2026-10-20): option (a), as recommended.**
4. **Black boxes and hard blocks in `levelize`.** (a) combinational; (b) cut points like registers.
   **Recommend (a)**: adders and multipliers are combinational in VTR; a registered hard block is
   flagged sequential in its registry entry if wanted later.
   **Decided (Peter, 2026-10-20): Peter's own rule replaces (a)/(b): memories (hard RAM and any memory cell) treat inputs as primary outputs and outputs as primary inputs; memories and black boxes with a stated latch or register: inputs are primary outputs, outputs primary inputs; all other black boxes are combinational paths (V-2, V-7 h to j).**
5. **Gate treatment of `inherited` BLIF cells (a new exception class; PHASE6 decision 2 does not
   allow it).** (a) approve `inherited` as a named class: counted as resolved, the strict number
   reported too; (b) strict only. **Recommend (a)**: BLIF VTR cells carry no location of their own
   (5.4 X-6) and (b) would fail every read-back; the strict figure stays visible.
   **Decided (Peter, 2026-10-20): option (a): `inherited` is an approved named class, strict figure always reported (amends PHASE6 decision 2).**
6. **Specificity.** (a) report only, set a threshold after the first sweep; (b) gate now on a guessed
   threshold. **Recommend (a)**: no data yet; PHASE6 asks for a "report".
   **Decided (Peter, 2026-10-20): option (a), as recommended.**
7. **Port bits and VPR pads (a new exception class `pad`, not allowed by PHASE6 decision 2).** (a)
   approve `pad` as a named class; (b) make port bits carry the
   declaration location (a 5.4 follow-up) so pads resolve. **Recommend (a)** now, (b) as a later item if
   Peter wants pads in the gate.
   **Decided (Peter, 2026-10-20): option (a): `pad` is an approved named class, strict figure always reported (amends PHASE6 decision 2).**
8. **networkx and PyG in CI.** (a) tests skip when absent, install in the lane environment for the
   PR run; (b) vendor a check. **Recommend (a)**.
   **Decided (Peter, 2026-10-20): option (a), as recommended.**
9. **Cell library licence/size.** (a) own small CMOS models (G-2); (b) also a SkyWater or Nangate
   subset for realism. **Recommend (a)**: nothing in PHASE6 needs more; (b) when reverse engineering
   a real netlist starts.
   **Decided (Peter, 2026-10-20): option (a), as recommended.**
10. **Provenance records on `read_json`.** E-3 does not restore `odin3_prov` (kind, `hier`) into
    records, so write, read, write is a fixpoint only from the second write. (a) accept that and test
    B equals C (E-3); (b) make `read_json` restore the records (a provenance-record change: needs your
    approval and an IR note). **Recommend (a)**: nothing in 6.3 to 6.5 needs the kind or `hier` back.
   **Decided (Peter, 2026-10-20): option (a): accept the second-write fixpoint.**
11. **Word-view `levelize`.** (a) netlist view only; refuse word view (V-1); (b) add a sequential
    flag to the word-view storage types in the registry so word view works (a registry change).
    **Recommend (a)**: levelize after `lower` is the meaningful use; (b) only if you want word-level
    depth.
   **Decided (Peter, 2026-10-20): option (a): `levelize` refuses word view in v1; word-level levelize is a post-v1 item.**
