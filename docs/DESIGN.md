# Odin III — Design Specification (v0.2 skeleton)

Status: draft, 2026-10-08. Decisions recorded here are the starting point; sections marked **OPEN** need a decision before the phase that depends on them. v0.2: language changed to structured C with a C-ABI plugin boundary (D8′); §15 code standard added.

## 1. Purpose and positioning

Odin III is an open-source (MIT) HDL elaboration and front-end synthesis framework for CAD research, the successor to Odin (FPL 2005) and Odin II (FCCM 2010). It reads Verilog, SystemVerilog, VHDL, and structural netlists (BLIF, VQM, EDIF) into one hierarchical, provenance-tracked IR; performs architecture-driven partial mapping; integrates ABC for soft-logic optimization; and writes netlists for the VTR flow, for Altera/Intel FPGAs, and for visualisation.

Three research threads drive the design:

1. **Forward synthesis** — a Parmys/Odin II-class front end for VTR with flexible, subgraph-based hard-block inference (multipliers, memories, DSP/MAC, and eventually tensor/matmul tiles).
2. **Reverse engineering** — reading flat netlists and *raising* them: recovering adders, counters, muxes, FSMs, memories, and emitting higher-level Verilog/SV.
3. **Agentic construction** — the project itself is a case study ("Odin III: a human's final frontier") in building a Yosys-class tool with an AI coding agent under human architectural control.

Threads 1 and 2 share one engine: a subgraph matcher over the IR, run forward (bind generic cells to hard blocks) or backward (raise bit-level cones to word-level cells).

## 2. Decisions already made

| # | Decision | Rationale |
|---|---|---|
| D1 | One IR, two granularities (word-level "RTLIL view", bit-level "netlist view"), reached by passes `lower` / `raise`; `to_rtlil` and `to_netlist` are views/checks, not separate data structures | Every writer, reader, checker, simulator, and the matcher exist once |
| D2 | Odin III owns a Bison/Flex Verilog-2005 grammar (evolved from Odin II's). SystemVerilog and VHDL arrive via adapters: slang → Odin III AST; GHDL via subprocess (`ghdl --synth`) → Verilog → Odin III | slang and GHDL are not Bison/Flex and would take years to re-create; adapters preserve the common-AST contract |
| D3 | License: MIT. External tools are subprocesses (GHDL, Yosys for oracle, Quartus, VPR) **except ABC, which is linked** (`libabc`) | Keeps the core MIT; ABC's permissive license allows linking |
| D4 | Both a built-in netlist simulator **and** equivalence checking (ABC `cec`/`dsec`, Verilator on emitted Verilog) | Simulator serves hard-block semantics and RE; equivalence is the regression oracle |
| D5 | Hard-block inference is subgraph-isomorphism based over IR fragments, with a cost model; patterns are IR, not text macros | Must scale to DSP cascades, MACs, matmul tiles; same engine serves RE |
| D6 | Altera primitives (LPM, `alt*`, `stratixiv_*`/`cycloneiv_*`) are first-class: a primitive library elaborates them to generic cells; `unbind`/`bind` passes move between generic and target-bound forms | LPM instances are pre-bound hard blocks; Titan is in scope early |
| D7 | Not built on MLIR/CIRCT. Borrow: op registry, pass manager, verifiers, first-class attributes. Provide `write_circt` (hw/comb/seq) and, later, `read_circt` | LLVM dependency and SSA semantics fight the node/pin/net model needed for RE and in-place rewiring |
| D8′ | **Core in structured C17** with a public C ABI (`include/odin3/odin3.h`). Plugins are `.so` files or executables speaking that ABI; Python tools and plugins use it through `cffi`. C++ is confined to `adapters/` (the slang shim) and `third_party/`. Platform: WSL2 Ubuntu, CMake, GitHub Actions CI, Claude Code as primary developer, human as architect/reviewer. Supersedes the C++20 choice in v0.1 | Simplest possible code for agent authorship and human review; stable plugin boundary; libabc is C; Odin II lineage; complexity is enforceable by lint in C |
| D9 | Provenance on every IR object: pointers in-process plus stable string IDs that survive serialization and appear in emitted names | Source-line → VPR-block tracing without Odin III loaded |

## 3. Architecture overview

```
 Verilog-2005 ─ Bison/Flex ─┐
 SystemVerilog ─ slang ─────┤                      ┌─ write_blif (VTR, black-boxed) ─ ABC(linked) ─ read back
 VHDL ─ ghdl --synth ─ Vlog ┼─► AST + Symbol Table ─► IR (word-level) ─► partial map ─► IR (bit-level) ─┼─ write_verilog (structural / Altera primitives)
 BLIF / VQM / EDIF ─────────┘        │                      ▲                                            ├─ write_json, write_dot, write_circt
                                     └── provenance ────────┘◄── raise (RE) ◄── BLIF/VQM/EDIF readers    └─ simulate / equiv
```

Every arrow is a *pass* registered with the pass manager; every pass runs `check` before and after in debug builds. Passes, readers, writers, and matcher patterns can be supplied by plugins through the C ABI (§15.3).

## 4. Front ends

### 4.0 Project input

A design is described to Odin III by a **project**: one in-memory record that every reader gets,
filled from a script or a project file (PHASE1 #18). It holds:

- the source files, each with its language (Verilog-2005, SystemVerilog, VHDL, BLIF, VQM, EDIF)
  and library (default `work`), in order;
- include search paths and macro defines (global, and per file where a format allows it);
- the top module(s) and top-level parameter overrides;
- the **architecture**: tech libraries and architecture files (`read_arch`, §6 step 7: `.o3lib`,
  later VPR XML or an Altera device), in load order;
- **partial-mapping rules** (§7), so a project says how its design meets that architecture;
- optionally the flow itself (a pass script, as `odin3 script.o3`), so one file reproduces a run.

**Architecture at the project level.** The project states what the target offers, not only
which files describe it, so the mapper (and a reader of the project) sees the available hard
resources in one place:
- **inventory**: `available <libcell> <count>` per hard cell (DSPs, RAM blocks, carry-chain
  adders, …); filled from the device (Quartus `DEVICE`, a VPR arch's grid/layout) when known,
  overridable in the project; unlimited when neither says. Budgets (`limit`) are checked
  against it, and `stats` reports used / available per cell after mapping;
- **capability overrides** for architecture research without editing the shared library:
  `hide <libcell>` (pretend the target lacks it), `cell <name> from <libcell> param …`
  (a variant with other widths/modes, e.g. a 27×27 multiplier), or a project-local `.o3lib`
  adding hypothetical hard blocks; all are scoped to the project and recorded in provenance;
- **architecture summary**: `odin3 --arch-report` (and the `stats` pass) prints the loaded
  architecture as the project sees it — each cell, its ports/parameter ranges, inventory and the
  rules that apply — so "what is available" is visible before mapping runs.

**Partial-mapping rules** steer §7 without code. Each rule has a scope (global, a module, or a
hierarchical instance path, with `*` wildcards) and a subject (an operator or cell kind such as
`$mul`, `$mem`, `$add`, or a tech-library cell); forms:
- `map <scope> <subject> to <libcell>[, <libcell>…]` / `soft` / `keep` (leave as a black box);
- thresholds: `min_width`, `max_width`, `min_depth` (e.g. multipliers under 9×9 stay soft; this
  overrides the arch-derived small-multiplier threshold);
- budgets: `limit <libcell> <count>` (e.g. at most 40 DSPs; the binder spills the rest to soft
  logic, largest-savings-first);
- extra patterns: `patterns <file.o3lib>` adds matcher patterns (§8) for this project only.

Precedence, most specific first: an attribute in the source (`(* odin3_map = "soft" *)`) > an
instance-path rule > a module rule > a global rule > the architecture's defaults. Every decision
the binder makes records which rule (file:line) chose it, in the object's provenance run, so
`stats` and the dot view can explain a mapping. Rule syntax and semantics are fixed with the
Phase 4 partial mapper; Phase 2 reads and stores them (unknown keys are errors, so a typo never
silently changes a mapping).

Ways to fill it (all produce the same record; a script may also list files directly with
`read_*`):

- **native project file** `.o3proj` (`read_project <file>`): line-oriented like `.o3lib`
  (`file verilog rtl/top.v`, `incdir rtl/include`, `define WIDTH=8`, `top top`,
  `param top.WIDTH 16`, `arch k6_frac_N10_mem32K.o3lib`, `map * $mul to multiply min_width 9`,
  `limit multiply 40`, `available multiply 112`, `hide dual_port_ram`, `flow synth.o3`); relative paths resolve against the
  project file's directory;
- **EDA file list** (`-f files.f`): one file per line, `+incdir+<dir>`, `+define+<name>[=<v>]`,
  `-v <file>` / `-y <dir>` library files and directories, nested `-f`;
- **Quartus `.qsf` / `.qpf` import**: `VERILOG_FILE`, `SYSTEMVERILOG_FILE`, `VHDL_FILE` (with
  `-library`), `TOP_LEVEL_ENTITY`, `SEARCH_PATH`, `VERILOG_MACRO`, `PARAMETER` assignments, `DEVICE`/`FAMILY` as
  the architecture, and the synthesis assignments that are mapping rules in Quartus terms
  (`AUTO_RAM_RECOGNITION`, `AUTO_DSP_RECOGNITION`, `DSP_BLOCK_BALANCING`, per-instance
  `RAMSTYLE`/`MULTSTYLE`) translated to the rules below; the
  `.qpf` names the revision, whose `.qsf` is read; every other assignment is ignored with one
  info line listing the ignored names (timing, pin and device assignments are not synthesis
  input in Phase 2);
- **Odin II XML config** (`-c config.xml`: `<verilog_files>`, `<arch_file>`, `<output>`), for
  users moving from Odin II (after Phase 2).

**Project file syntax (normative).** Reference parsers: `tools/project-fixtures`; test corpus
(every case in every format that can express it, expected records, oracle netlists, negatives):
`tests/golden/projects` (PHASE1 #19).
- All formats: relative paths resolve against the directory of the file that names them
  (nested `-f` lists too; unlike Odin II and most `-f` tools, never the working directory);
  every named file or directory must exist. Language by extension where a format has no
  language: `.v .vh` Verilog, `.sv .svh` SystemVerilog, `.vhd .vhdl` VHDL, `.blif`, `.vqm`,
  `.edf .edif`.
- `.o3proj`: one statement per line, blank-separated words, `"…"` groups (backslash escapes),
  `#` starts a comment outside quotes; unknown keys are errors. `file <lang> <path> [library
  <lib>]`, `libfile <lang> <path>`, `libdir <dir>`, `libext <ext>…` (= `-v`/`-y`/`+libext+`),
  `incdir <dir>`, `define <name>[=<value>]`, `top <module>` (repeatable), `param
  <module>.<name> <value>`, `arch <file.o3lib|file.xml>`, `device <family> [<part>]`, `map
  <scope> <subject> (to <cell>[, <cell>…] | soft | keep) [min_width|max_width|min_depth <n>]…`,
  `limit <cell> <n>`, `available <cell> <n>`, `hide <cell>`, `cell <name> from <libcell>
  [param <k>=<v>…]`, `patterns <file.o3lib>`, `flow <script.o3>` (once). Scopes use `.` as the
  hierarchy separator.
- `-f`: blank-separated words, `//` and `#` comments; a non-option word is a source file
  (library `work`); `-f` and `-F` (same meaning), `-v <file>`, `-y <dir>`, `+incdir+`,
  `+define+`, `+libext+` (several `+`-separated values each); any other option is an error. It
  has no top (selected automatically), parameters or libraries.
- `.qpf`: `NAME = "value"` lines; the first `PROJECT_REVISION` is read (none: the `.qpf`'s own
  name). `.qsf`: Tcl words (quotes, braces, `\` continuation, `#` comments).
  `TOP_LEVEL_ENTITY` defaults to the revision name (Quartus); `set_parameter -name N
  [-entity E] V` (no `-entity`: the top); `SEARCH_PATH` is an include directory; `VHDL_FILE
  -library`; `FAMILY`/`DEVICE` form one `device` entry; `AUTO_DSP_RECOGNITION OFF`,
  `AUTO_RAM_RECOGNITION OFF`, `DSP_BLOCK_BALANCING "LOGIC ELEMENTS"`, and per instance
  `MULTSTYLE LOGIC` / `RAMSTYLE LOGIC` (`-to a|b:c` → scope `a.c`) become `map <scope>
  $mul|$mem soft`, their `ON`/`AUTO` is the default (no rule); any other value, assignment
  (`QIP_FILE`, `SDC_FILE`, pins, timing, partitions, …) or command is ignored and named in one
  line `info: <file>: ignored: A, B, …`.
- Odin II XML: `<verilog_files><verilog_file>` (legacy) or `<inputs>` (`<input_type>`
  verilog|systemverilog|blif, `<input_path_and_name>`…); `<output>` (`<output_type>`,
  `<output_path_and_name>`, `<target><arch_file>`); other elements ignored and listed; no top.
- Sources: `` `include `` searches the including file's directory, then the include path in
  order; macros stay defined for later files (one compilation unit); `-v`/`-y` modules load
  only when instantiated and are never top candidates; VHDL names are case-insensitive, also
  across a Verilog/VHDL instantiation; units are identified by library and name.

**Top module.** `--top <name>` (or the project's `top`) wins. Otherwise the top is the single
module no other module instantiates; zero or several candidates is an error that lists them.
The chosen top is recorded in the IR (`odin3_design_set_top`, IR-11 design record) and reported
by `stats`.

**Provenance.** Each source record names its file through the project (path as given, library),
so `file:line` queries stay unambiguous when two libraries hold files of the same name.

### 4.1 Verilog-2005 (owned)
- Preprocessor first: `define/`ifdef/`else/`endif/`include, macros with args, `` `timescale `` tolerated.
- Bison/Flex grammar, location-tracked tokens; every AST node has `{file, line, col, end_line, end_col}` and an attribute list (`(* ... *)` and pragmas).
- Elaboration: parameters/localparam/defparam, `generate` (if/for/case), functions and (synthesizable) tasks, integer/genvar, signed semantics per IEEE 1364-2005 §5, implicit nets (warn), Quartus-dialect tolerance.
- Comments kept in a side table keyed by location (not a red/green tree).

### 4.2 SystemVerilog (slang adapter)
- slang is C++20 and MIT. `adapters/slang/` is a small C++ shim that links slang, walks its *elaborated* AST (post-parameter, post-generate), and emits the Odin III AST through the C ABI (`odin3_ast_*` builders). It exports exactly one C function, `odin3_read_slang(...)`, and is built as a separate shared library so the core stays pure C. slang handles packages, interfaces (flattened), structs, enums, `always_ff/comb/latch`, `logic`.
- Source locations copied verbatim.

### 4.3 VHDL (GHDL subprocess)
- `ghdl --synth --out=verilog` per design unit → Odin III Verilog front end. GHDL's generated names are stable enough to keep provenance to the VHDL line via a name map GHDL emits. **OPEN:** confirm GHDL's `--out=verilog` coverage on the VTR benchmark set once converted.

### 4.4 Netlist readers
- BLIF (full: `.subckt`, `.latch` with init, `.names`, black-box `.model`s) — required for the ABC round-trip.
- VQM (Quartus post-synthesis Verilog): the Verilog grammar plus the Altera primitive library; no new parser.
- EDIF 2.0.0: S-expression reader, structural only.
- These produce bit-level IR with structural-only provenance; the `raise` pass (§8) recovers word-level structure.

### 4.5 Common AST and Symbol Table
- Scoped symbol table (module → generate scope → block), each symbol with declaration location, type (net/var, width, signedness, array dims), and the list of AST nodes that assign it.
- AST is the retained "high" layer: it is never discarded; IR objects back-point to it.

## 5. The IR

### 5.1 Object model (Odin II lineage)
- `Design` → `Module`s (hierarchy kept until an explicit `flatten` pass).
- `Module` owns `Node`s, `Pin`s, `Net`s as **peers** (Odin II's key property): a node has pin lists; a pin belongs to one node and attaches to one net; a net has one or more driver pins and any number of sink pins. Multi-driver and tristate are representable and flagged by `check`.
- `Node` has a `CellType` from the **op registry** and an `Attributes` map.
- Storage: arena-allocated, dense integer IDs (`uint32_t`), stable across passes; name → ID maps kept per module. Objects are plain structs; all cross-references are IDs, never pointers, so the IR is trivially serializable and the C ABI can hand out IDs safely. Target: 2M nodes in < 2 GB (Titan).

### 5.2 Op registry (CIRCT-inspired, one file per cell type)
Each cell type is a `const struct odin3_celltype` table entry declaring: name, typed ports (direction, width expression, signedness), parameters, verifier function pointer, **simulation semantics** (C function pointer), BLIF/Verilog/JSON emitter function pointers, and a `granularity` tag (`word`, `bit`, `hard`, `blackbox`, plus the structural `module` and `port` — `docs/IR.md` IR-9). Adding a cell type touches exactly one file; plugins can register cell types at load time.

Word-level set (mirrors Yosys RTLIL): `$add $sub $mul $div $mod $and $or $xor $not $shl $shr $sshr $eq $ne $lt $le $gt $ge $mux $pmux $dff $dffe $adff $sdff $mem $memrd $memwr $reduce_*`. (`$concat`/`$slice` are not cells: nets are one bit and a port is a pin vector, so slicing and concatenation are which nets a port uses — `docs/IR.md` IR-1.)
Bit-level set: `$_AND_ $_OR_ $_XOR_ $_NOT_ $_MUX_ $_DFF_* $_DLATCH_* $_FF_ $_CONST*_ $sop` (BLIF `.names` covers) and `$_LUT_K_`. Structural: `$port_in/out/inout`. Hard and black-box cells keep their own names (e.g. VTR `adder`, `multiply`), registered by the tech library or declared by a netlist (`docs/IR.md` IR-7b).
Hard set: generated from the VPR arch `<model>` list and from the Altera primitive library.

### 5.3 Provenance
Every node, net and wire carries a provenance ID (pins inherit their node's) into an append-only, hash-consed lineage DAG: each record holds its kind (source, imported, derived), the pass run and operation that made it, source locations, AST node, hierarchical path, and parent records — the parents replace v0.2's `origin` field. Navigation runs backward (object → sources) and forward (location or record → objects, live or dead) on demand. Details: `docs/IR.md` §6 (IR-12, IR-13). Emitted names follow `hier/path/cellname@file:line`; a `--name-style` flag chooses between provenance names and short names.

### 5.4 Views
- `rtlil view`: asserts all nodes are `word`/`hard`/`blackbox`/`module`/`port`; exposes a Yosys-like API for passes.
- `netlist view`: asserts all nodes are `bit`/`hard`/`blackbox`/`module`/`port`.
- Mixed granularity is legal during `lower`/`raise` only.

## 6. Pass pipeline (forward)

1. `read_*` → AST, symbol table
2. `elaborate` → word-level IR with processes
3. `proc` — predicate-tracked lowering of `always` blocks to mux trees + `$dff*`; blocking/non-blocking analysis; latch detection (warn)
4. `opt` — constant folding, dead-node removal, CSE, algebraic simplification on arithmetic, mux collapsing (Odin I)
5. `fsm_detect`, `fsm_recode` (one-hot by default; AST-assisted as in Odin II)
6. `memory_infer` — `reg` arrays → `$mem` with port analysis; attribute overrides
7. `read_arch` — `.o3lib` tech libraries (target-agnostic: gates first, then FPGA hard cells; `docs/specs/2026-10-08-1G-techlib-design.md`) → hard/gate set; VPR XML `<model>`s and the Altera device via importers into the same form
8. `partial_map` (§7)
9. `lower` — word → bit; hard/blackbox nodes untouched
10. `abc` — linked ABC on the soft-logic cone, black boxes preserved, results read back and re-stitched
11. `write_*`

## 7. Partial mapping

Partial mapping binds *some* of the word-level IR to the target's hard circuits and leaves the remainder as soft logic for ABC. It runs before `lower` because word-level operators are the only place hard-block shape is visible.

Components:
- **Inference** — three tiers, as in Odin II: explicit instantiation (primitive library), coding-style rules (memory inference), and open-ended **subgraph matching** (Odin I, re-implemented generically).
- **Binding/packing** — for each matched structure choose an implementation: hard block(s) + generated soft glue, or all-soft. Includes recursive multiplier splitting with the small-multiplier threshold auto-derived from the arch, signed multipliers, memory depth/width splitting with width-depth trading, carry chains (`adder` model), DFF feature matching (enable, sync/async reset) to what the arch's FF supports.
- **Generic hard blocks** — any arch `<model>`; matched by exact port signature (Odin II) or by a user-supplied pattern.
- **Project rules** (§4.0) steer all three: per-scope `map … to/soft/keep`, width/depth thresholds, per-cell budgets, project-only patterns; each binding records the rule that chose it.

Matcher (§8) pattern format: an IR fragment written as a tech-library cell function — the `.o3lib` expression language (Verilog-like, parametric widths) compiled to IR — plus a cost (resolved 2026-10-08, PHASE1 #7/#8). Patterns live in the tech library beside the cell they map to. Overlap resolution: maximum-cover with cost tie-break (Odin I's rule generalized).

## 8. Subgraph matcher and reverse engineering

One matcher, two directions:
- Forward: patterns = target hard blocks; candidates seeded at anchor cells (e.g. `$mul`) as in Odin I.
- Backward (`raise`): patterns = generic word-level structures (ripple/CLA adders, comparators, mux trees, counters, shift registers, memories, FSM state registers); matched cones are replaced by word-level cells, so RE is literally `lower` run backwards.

Algorithm: VF2-style with anchor seeding and width-agnostic matching; semantic verification of each candidate by local equivalence (ABC on the cone) before commit. Outputs for RE: raised IR → `write_verilog` at word level, plus a report mapping recovered structures to netlist regions.

## 9. Writers and readers

| Format | Read | Write | Notes |
|---|---|---|---|
| BLIF | yes | yes | VTR dialect; black boxes; `.latch` init |
| Verilog | yes (front end) | structural (bit/word), Altera-primitive flavoured | the RE output |
| SystemVerilog | via slang | word-level structural | |
| VQM | yes | — | Titan |
| EDIF | yes | later | |
| JSON (Yosys schema) | later | yes | netlistsvg, nextpnr |
| Graphviz dot | — | yes | hierarchy clusters; `--focus <hier-path | file:line | cone(net)>`; never renders > N nodes without a focus |
| CIRCT MLIR | later | yes | hw/comb/seq |

## 10. Verification

- `simulate`: cycle-based event-free simulator over the netlist view using op-registry semantics; hard blocks simulate via their registry function; random or file vectors; VCD out.
- `equiv`: ABC `cec` (combinational) / `dsec` (sequential) between any two IRs or BLIFs; Verilator harness comparing emitted Verilog against source.
- `check`: structural invariants (no dangling pins, driver counts, width consistency, clock-domain attribute consistency, granularity view) — runs around every pass in debug.

## 11. Testing and benchmarks

- **Oracle**: Parmys (and Odin II via `WITH_ODIN=ON`) golden BLIFs, archived per VTR release.
- **Microbenchmarks** (VTR `odin_ii/regression_test/benchmark/...`): required *identical* canonicalized netlist across Verilog, SV (converted), and VHDL (converted) front ends, and equivalent to Parmys.
- **VTR 19**: full flow to P&R; QoR table (min W, critical path, LUTs, FFs, hard blocks) vs. Parmys.
- **Titan**: smallest designs first; VQM path and source path both; must match Quartus-synthesized BLIF functionally.
- **Fuzzing**: random Verilog generator with Yosys as differential oracle, from Phase 2.
- **Reverse engineering**: round-trip test — Verilog → lower → BLIF → read → raise → Verilog → equiv, plus structure-recovery scorecard.
- CI runs lint gate + unit + micro + fuzz on every push; VTR 19 nightly; Titan weekly.

## 12. Development path

| Phase | Deliverable | Exit test |
|---|---|---|
| 0 | WSL2, builds of VTR/Yosys/Parmys/ABC/GHDL; golden BLIFs; `netlist-compare`, `equiv-check`; lint gate; skills; `docs/DESIGN.md`; CI | Oracles run green on all goldens; lint gate green (see `odin3-phase0-setup.md`) |
| 1 | `util/`, core IR, op registry, pass manager, `check`, provenance, C ABI v0, BLIF read/write, dot/JSON/Verilog writers, simulator, tech-library format + reader + generic gate library | BLIF→IR→BLIF bit-identical on goldens; sim matches ABC on goldens; a Python plugin can walk the IR |
| 2 | Verilog-2005 front end + preprocessor + elaboration; project input (§4.0: `.o3proj`, `-f` file lists, Quartus `.qsf`/`.qpf` import, top selection); `proc`, `opt`; smallest Titan design parsed from its `.qsf`; primitive library v0 | Micros identical/equivalent to Parmys; every project fixture (`tests/golden/projects`) reads identically in all its formats and matches its oracle |
| 3 | `lower`, linked ABC, VTR flow hookup | VTR 19 through P&R; QoR table |
| 4 | VPR-XML import into the tech library, partial mapping, memory inference, carry chains, FSM, mux collapsing, matcher v1 | QoR parity on arch sweep (paper 1) |
| 5 | slang adapter, GHDL path, cross-language identical-netlist test | Three front ends, one netlist |
| 6 | `raise`, RE scorecard, VQM/EDIF, Altera `bind` output, Titan subset | RE round-trip + Titan (paper 2) |
| 7 | CIRCT writer, JSON reader, visual tooling polish, docs | Release |

Agentic working rules: one pass per PR; golden test per pass; `check` on in debug; lint gate must pass before commit; agent reads `docs/DESIGN.md` and the op-registry file for any task; human reviews IR/invariant changes and all mapping algorithms; no PR merges red. Per-pass workflow: `superpowers:brainstorming` → `superpowers:writing-plans` → human approves → `superpowers:executing-plans` → `/run-micro` → PR.

## 13. Open questions

1. ~~Matcher pattern DSL and where patterns live.~~ **Resolved 2026-10-08:** tech-library (`.o3lib`) cell functions compiled to IR fragments, living in the tech library (PHASE1 #7/#8).
2. GHDL `--out=verilog` coverage; fallback is linking libghdl in a separately-licensed optional module.
3. Primitive library v0 scope: exactly which `lpm_*`/`alt*`/`stratixiv_*` the smallest Titan designs need.
4. Memory layout details (arena vs. SoA) — decide in Phase 1 with a 2M-node synthetic benchmark.
5. Which Altera generation for `bind` output first (Cyclone IV/V for Quartus Lite vs. Stratix IV for Titan).
6. Name-style policy for VTR: provenance names are long; measure VPR runtime impact.
7. Whether `raise` should also target Yosys's internal cell names for interop.
8. ~~Unit-test framework for C: Unity vs. CMocka (decide in PR #1).~~ **Resolved 2026-10-08: Unity**, vendored as `third_party/unity`.

## 14. References
- Jamieson & Rose, "A Verilog RTL Synthesis Tool for Heterogeneous FPGAs", FPL 2005.
- Jamieson, Kent, Gharibian, Shannon, "Odin II", FCCM 2010.
- Luu et al., "VPR 5.0", FPGA 2009.
- Rose et al., "The VTR Project", FPGA 2012.
- VTR docs (Parmys default, Odin II deprecated); yosys-slang; ghdl-yosys-plugin; CIRCT.

## 15. Code standard and lint gate

The goal is code an agent can write and a human can review in one pass: small functions, no cleverness, every rule machine-checked. The gate runs as a pre-commit hook and as a required CI check; nothing merges with a violation.

### 15.1 Language rules (C17)
- `-std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Werror`.
- No `goto` except the single `cleanup:` label pattern for error paths. No variable-length arrays. No recursion in IR traversals (use an explicit worklist) — the IR can be 2M nodes deep.
- Memory: every IR object lives in storage owned by its `Module` (per-module paged arrays and arena; the string table and provenance are design-global — `docs/IR.md` IR-18); passes never `malloc` IR objects directly. `util/` provides `arena`, `vec`, `pagevec`, `u64map`, `idindex`, `str`, `log`; nothing else in the tree defines a generic container.
- Errors: functions return `odin3_status` (an enum); no `exit()`/`_exit()`/`_Exit()`/`quick_exit()` outside `cli/`; no `abort()`, `raise(SIGABRT)` or `__builtin_trap()` outside `check` in debug builds. `assert()` is allowed (it compiles out of release builds). Checked by `tools/check_rules.py` and, at link level, `tools/check-symbols.sh`.
- One header per module, `#include` only what you use (`include-what-you-use` advisory in CI, not blocking).
- Every public function in `odin3.h` has a one-paragraph comment: purpose, ownership of returned memory, failure modes.

### 15.2 Tooling
| Tool | Config | Blocking |
|---|---|---|
| clang-format | LLVM base, 100 cols, 4-space indent; applied by a Claude Code `PostToolUse` hook and checked with `--dry-run -Werror` | yes |
| rules | `tools/check_rules.py` (exit/abort/goto per §15.1) + `-Wvla` | yes |
| clang-tidy | `bugprone-*`, `cert-*`, `misc-*`, `performance-*`, `readability-*` (including `readability-function-size`, `readability-function-cognitive-complexity`), `modernize-*` **off** (C, not C++) | yes |
| cppcheck | `--enable=warning,style,performance,portability --error-exitcode=1` | yes |
| lizard | `-C 15` (cyclomatic complexity), `-L 60` (lines per function), `-a 5` (parameters) over `src/` | yes |
| ASan + UBSan | `debug` CMake preset; all unit and micro tests run under it in CI | yes |
| ruff + mypy --strict | `tools/`, `plugins/` Python | yes |
| include-what-you-use | advisory | no |

`tools/lint.sh` runs all of the above; `/lint` is its skill wrapper. The pre-commit hook runs `tools/lint.sh --no-tidy` (clang-tidy is slow); CI runs the full gate, including clang-tidy, and is required to merge. Tests are exempt from `readability-magic-numbers`; `i`/`j`/`k` are the only allowed one-letter names.

### 15.3 Plugin and hook points (C ABI)
`include/odin3/odin3.h` is the only header plugins see. It exposes opaque handles plus ID-based accessors: design/module/node/pin/net iteration and lookup, attribute get/set, cell-type registration, pass registration, reader/writer registration, matcher-pattern registration, and provenance queries. Plugins are:
- **Shared objects** (`.so`) loaded with `--plugin foo.so`; they call `odin3_register_pass(...)` etc. from an `odin3_plugin_init` entry point.
- **Executables** run at a hook point (`--hook after:partial_map ./myscript`); the IR is handed over as JSON on stdin and read back from stdout, so any language works and the hook can be a shell one-liner.
- **Python** via `cffi` against `odin3.h` (`plugins/python/odin3.py` is the generated binding); the same accessors, so a Python pass is a function taking a module handle. Used for research scripts, QoR analysis, and prototyping a pass before porting it to C.
