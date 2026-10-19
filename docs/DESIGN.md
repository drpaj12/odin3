# Odin III — Design Specification (v0.2 skeleton)

Status: draft, 2026-10-08; v1.0 scope amended 2026-10-19 (D10: §7, §8, §12, §12.1, §13). Decisions recorded here are the starting point; sections marked **OPEN** need a decision before the phase that depends on them. v0.2: language changed to structured C with a C-ABI plugin boundary (D8′); §15 code standard added.

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
| D5 (amended by D10: no matcher in v1.0; port signature and fixed rules) | Hard-block inference is subgraph-isomorphism based over IR fragments, with a cost model; patterns are IR, not text macros | Must scale to DSP cascades, MACs, matmul tiles; same engine serves RE |
| D6 (amended by D10: LPM structural Verilog only; no atoms, no bind) | Altera primitives (LPM, `alt*`, `stratixiv_*`/`cycloneiv_*`) are first-class: a primitive library elaborates them to generic cells; `unbind`/`bind` passes move between generic and target-bound forms | LPM instances are pre-bound hard blocks; Titan is in scope early |
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

v1.0 scope (D10): the SystemVerilog and VHDL inputs, the VQM/EDIF readers and the `raise` arrow are deferred or removed (§12.1); the Altera path is LPM structural Verilog.

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
- optionally the flow itself (a pass script, as `odin3 script.o3`), so one file reproduces a run;
- optionally the **ABC script** the `abc` pass runs (§6 step 10), so that soft-logic
  optimisation is part of what one file reproduces.

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
  `limit multiply 40`, `available multiply 112`, `hide dual_port_ram`, `flow synth.o3`,
  `abc_script vtr.abc`);
- **EDA file list** (`-f files.f`): source files, `+incdir+`, `+define+`, `-v`/`-y` library
  files and directories with `+libext+`, nested `-f`/`-F`, `-top`;
- **Quartus `.qsf` / `.qpf` import**: design files (`VERILOG_FILE`, `SYSTEMVERILOG_FILE`,
  `VHDL_FILE -library`, `VQM_FILE`, `EDIF_FILE`, `QIP_FILE`), `TOP_LEVEL_ENTITY`,
  `SEARCH_PATH`, `USER_LIBRARIES`, `VERILOG_MACRO`, `set_parameter`, `FAMILY`/`DEVICE` as the
  architecture, and the synthesis assignments that are mapping rules in Quartus terms
  (`AUTO_RAM_RECOGNITION`, `AUTO_DSP_RECOGNITION`, `DSP_BLOCK_BALANCING`, `RAMSTYLE`,
  `MULTSTYLE`); every other assignment (pins, timing, partitions, …) is ignored and listed in one
  info line;
- **Odin II XML config** (`-c config.xml`: `<verilog_files>` or `<inputs>`, `<output>` with
  `<arch_file>`, `<optimizations>`), for users moving from Odin II (its reader lands with 2B).

**Project file syntax (normative).** The reference parsers are `tools/project-fixtures`; the
test corpus `tests/golden/projects` (PHASE1 #19) holds every case in every format that can
express it, with expected records, oracle netlists and negatives.

*All formats.* Paths given relative resolve against the directory of the file that names them,
except `-f` lists (below); never against the working directory. Every named file or directory
must exist (`missing_file`). LF and CRLF line endings are accepted. Where a format names no
language, the extension does: `.v .vh` Verilog, `.sv .svh` SystemVerilog, `.vhd .vhdl` VHDL,
`.blif`, `.vqm`, `.edf .edif`; any other extension is `unknown_file_type`. A reader keeps each
path as given and as resolved (absolute); tests compare paths relative to the case directory.
The same file listed again in the same library is read once and named in an info line
(`info: <file>: listed more than once, read once: …`); the same file in two libraries is two
design files. One top at most (`top` twice is an error).

*Words* (`.o3proj` and `-f`). Words are separated by blanks. A word is bare, or wholly enclosed
in double quotes; inside quotes the only escapes are `\"` and `\\` (any other backslash is an
error). Outside quotes a backslash is an ordinary character: Windows `\` paths are not
translated. Characters right after a closing quote, an unterminated quote and an empty quoted
word `""` are `syntax` errors; so is a quote inside a bare word in `.o3proj`, while in a `-f`
list it is an ordinary character (`+define+TAG="AB"` defines `TAG` as `"AB"`, quotes
included). `#` at the start of a word starts a comment, as does `//` in a `-f` list; inside a
word both are ordinary (`+define+D=#1`). A string macro keeps its quotes in its value: `define
"MSG=\"hi\""` defines `MSG` as `"hi"`.

*`.o3proj`.* One statement per line; the first word is the key; unknown keys are `unknown_key`.
- `file <lang> <path> [library <lib>]` (`lang`: verilog, systemverilog, vhdl, blif, vqm, edif;
  library default `work`); `libfile <lang> <path>`, `libdir <dir>`, `libext <ext>…` (the
  `-v`/`-y`/`+libext+` of a file list); `incdir <dir>`; `define <name>[=<value>]`.
- `top [<library>.]<module>` (once); `param <top>[.<instance>…].<name> <value>`: the scope
  starts at the declared top (a parameter of the top, or of an instance under it; anything else
  is an error); a quoted value is a string, a bare one must be an integer or a sized literal
  (`16`, `-3`, `8'hFF`).
- `arch <file.o3lib|file.xml>`, `device <family> [<part>]`; `available <cell> <n>`;
  `hide <cell>`; `cell <name> from <libcell> [param <k>=<v>…]`.
- `map <scope> <subject> (to <cell>[, <cell>…] | soft | keep)
  [min_width|max_width|min_depth <n>]…`; `split <subject> [width] [depth min|max|<n>]`
  (memory splitting, Odin II's `<memory>`); `limit <cell> <n>`; `patterns <file.o3lib>`.
  Scopes are `*`, a module, or an instance path `top.u_a.u_b` (`*` wildcards); `.` separates.
- `flow <script.o3>` (once); `abc_script <file>` (once): the ABC script of the `abc` pass, a
  path (never inline text), any extension; `import <file.qpf|file.qsf> [revision <name>]` reads
  a Quartus project into this one (its top counts as this project's `top`; an imported `.qsf`
  cannot set `abc_script`, the importing `.o3proj` can).

*`-f` lists.* A bare word is a source file (library `work`). Entries of the outermost list, and
of every list reached through `-f <list>` — from the outermost list or from any nested list,
including one reached through `-F` — resolve against the outermost list's directory (what other
tools do when run from there); entries of a `-F <list>` resolve against that list's own
directory. Options: `-f`/`-F <list>` (a cycle is `f_cycle`), `-v <file>`, `-y <dir>`,
`+incdir+<dir>[+<dir>…]`, `+define+<name>[=<v>][+…]`, `+libext+<ext>[+…]` (also without `-y`),
`-top <name>` / `--top-module <name>`, `-sv` (Verilog files after it are SystemVerilog, to the
end of the list that says it, including the lists it reaches; it does not leak out of a nested
list). `$NAME` and `${NAME}` expand from the environment when the word
is used; an unset name is `undefined_variable`, any other `$` a `syntax` error. Any other option
is `unknown_option`. A file list has no parameters or libraries.

*Quartus.* A `.qpf` holds `NAME = "value"` lines. With one `PROJECT_REVISION` (or none: the
`.qpf`'s own name) that revision's `.qsf` is read; with several, `--revision <r>` (or
`import … revision <r>`) chooses, and without it the error is `ambiguous_revision`, listing them;
a revision not listed is `unknown_revision` (located at the `import` line; unlocated for
`--revision`). A `.qsf` may also be given directly (its name is the revision). A `.qsf` (and a `.qip`) is read as Tcl words: bare words, `"…"` (escapes `\"`,
`\\` only), `{…}` (verbatim, nested), `\`-newline continuation, `;` or newline between
commands, `#` comments where a command starts. Inside a word, bare or quoted, `[` and `]` are
ordinary characters (`-to LEDR[0]`, `-to "SW[1]"`); a word that starts with `[` is command
substitution, a `syntax` error except, in a `.qip`, the idiom
`[file join $::quartus(qip_path) "<path>"]`, which means `<path>` relative to the `.qip`.
Characters after a closing quote or brace and `$` substitution are `syntax` errors. Plain
relative paths in a `.qsf` and in every `.qip` resolve against the project directory (the
`.qsf`'s), as in Quartus.
- Commands: `set_global_assignment`, `set_instance_assignment`, `set_io_assignment` (`-name
  <N> … <value>`), `set_location_assignment <value> -to <pin>` (listed as `LOCATION`),
  `set_parameter -name <N> [-entity <top>] [-to <path>] <value>`; any other command is ignored
  and listed. Each takes exactly one value (`syntax` otherwise). Options are those of Quartus
  (`-name -to -from -entity -section_id -library -hdl_version -tag -comment -disable -remove`);
  an assignment Odin III reads allows only its own options below, an ignored one any of them;
  any other option is `unknown_option`.
- Assignment names and enumerated values (`ON`, `OFF`, `AUTO`, `LOGIC`, `LOGIC ELEMENTS`) are
  case-insensitive, as in Quartus; paths, entity names and macro values are exact.
- `VERILOG_FILE`, `SYSTEMVERILOG_FILE`, `VHDL_FILE`, `VQM_FILE`, `EDIF_FILE`: a design file
  (`-library <lib>`; `-hdl_version` is accepted and listed as ignored — the language standard
  comes from the language). `QIP_FILE`: read now, as part of this `.qsf` (nested `.qip` files
  allowed; a cycle is `qip_cycle`). `TOP_LEVEL_ENTITY`: the top (repeated: the last one wins;
  absent: the revision name, as in Quartus). `SEARCH_PATH`: include directory and library
  directory; `USER_LIBRARIES`: library directory (both take `;`-separated lists).
  `VERILOG_MACRO`: a define. `FAMILY`/`DEVICE`: one `device` entry. `VERILOG_INCLUDE_FILE`:
  ignored (headers are found through `` `include ``).
- `set_parameter` without `-entity`/`-to` sets a top parameter; `-entity` must name the top;
  `-to <path>` names an instance. A value that is an integer or sized literal is a number,
  `"…"` (literal quotes) a string without them, anything else a string as written.
- Mapping assignments: `AUTO_DSP_RECOGNITION OFF`, `AUTO_RAM_RECOGNITION OFF`,
  `DSP_BLOCK_BALANCING "LOGIC ELEMENTS"`, `MULTSTYLE LOGIC`, `RAMSTYLE LOGIC` become `map <scope>
  $mul|$mem soft`; `ON`/`AUTO` is the default (no rule); other values (device block names such
  as `M10K`, `DSP`) are ignored and listed. Scope: none of `-entity`/`-to` is `*`; `-entity e`
  alone is module `e`; `-to a|b:c` is the instance path `a.c` (`entity:instance` keeps the
  instance), anchored at `-entity` or else the top (`u_x` becomes `top.u_x`).
- Ignored names are sorted, without repeats, into one line `info: <file>: ignored: A, B, …`.

*Odin II XML.* Root `<config>`. Files from `<verilog_files><verilog_file>` (legacy) or
`<inputs>`: at most one `<input_type>` (`verilog` or `blif`, any case; anything else is
`unsupported_input_type`) and `<input_path_and_name>`…. `<output>`: `<output_type>`,
`<output_path_and_name>` (recorded, not design input), `<target><arch_file>` (architecture).
`<optimizations>`: `<multiply size="N">` → `map * $mul to multiply min_width N`, `<adder
threshold_size="N">` → `map * $add to adder min_width N`, `<memory split_memory_width="1"
split_memory_depth="min|max|N">` → `split $mem [width] [depth …]`; their other attributes
(`fixed`, `fracture`, `padding`, adder `size`) and other elements (`mix_soft_hard_blocks`,
`debug_outputs`, …) are ignored and listed (`optimizations/multiply@fixed`). No top.

*The ABC script.* Only `.o3proj` names one; a `-f` list has no form for it (`-abc_script` is
`unknown_option`), Quartus has no such assignment (an `ABC_SCRIPT` assignment is ignored and
listed like any other), Odin II XML never had one. The CLI loads one with `--abc-script
<file>`, resolved against the working directory. Precedence, as for `--top` and `--revision`:
`abc --script <file>` (that run only) beats `--abc-script`, which beats the project's
`abc_script`, which beats the built-in `lib/abc/vtr.abc`. `read_project` puts `--abc-script`
into the record, with the info line `<entry>: --abc-script <new> replaces the project
abc_script <old>` when they differ. The language, `{K}`, the allowed commands and the caps are
in the 3D spec §7.

*Sources.* `` `include `` searches the including file's directory, then the include path in
order (the first match wins; an include of a file already being included is `include_cycle`).
Macros stay defined for later files (one compilation unit). Design units are identified by
library and name; VHDL names are case-insensitive, also across a Verilog/VHDL instantiation.
VHDL files are analysed in dependency order (a file after the files defining the packages it
`use`s, the entities it instantiates as `entity lib.e`, the entity of an architecture and the
package of a package body it holds; otherwise listing order; a cycle is `dependency_cycle`);
Verilog keeps listing order. The instances in an architecture belong to its entity, whichever
file holds it. An instance resolves to a design unit (the
instantiating unit's library first), else a `-v`/`libfile` module (first file wins), else a
`-y`/`libdir` file `<dir>/<name><ext>` (directories in order, then `+libext+` extensions in
order, default `.v`); library modules load only when instantiated, never shadow design units
and are never top candidates. Instances of Altera megafunctions in RTL (`altsyncram`,
`altdpram`, `altshift_taps`, `altmult_add`, `lpm_*`) resolve through the LPM tech library (Phase 5 item 5.1, D10, D11; PHASE2 #2 had planned a Phase 6 primitive
library), not the project; in a VQM netlist (VQM reader removed from v1.0, D10), instances of the device primitives the VQM reader
would know (`dffeas`, `cyclonev_lcell_comb`, `cyclonev_io_ibuf`, `cyclonev_io_obuf`,
`cycloneive_lcell_comb`, `cycloneive_io_ibuf`, `cycloneive_io_obuf`; the VQM library would have grown in
Phase 6, removed by D10) resolve through that library. Any other unresolved instance is `unresolved_module`,
unless `read_verilog --auto-blackbox` makes it an implicit black box (2D EL-11: named
connections, `inout` ports sized by the first instance, an info line naming it). `read_verilog -icells` (5.2) is the one
exception for names beginning with `$`: an instance of a registered cell type of that name (`\$mem`,
`\$shiftx`, `\$dffsr`, ...) is that cell, as in Yosys; it is how the Verilog the writer emits at the
word, gate, LUT and LPM levels reads back.

*Errors* are located at the project-file or source line (`file:line`) where there is one:
`missing_file`, `unknown_key`, `unknown_option`, `unknown_file_type`, `unsupported_input_type`,
`unsupported_language` (a design file in a language the build cannot elaborate yet, e.g.
EDIF, removed from v1.0 by D10), `syntax`, `undefined_variable`, `f_cycle`, `qip_cycle`,
`ambiguous_revision`, `unknown_revision`, `include_not_found`, `include_cycle`,
`duplicate_module` (at the second definition), `dependency_cycle`, `ambiguous_top`, `no_top`,
`unknown_top`, `unresolved_module`.

**Top module.** `--top <name>` (or the project's `top`, `[<library>.]<module>`) wins; a name
found nowhere is `unknown_top`, one found in several libraries `ambiguous_top`, both located at
the declaring line (`top`, `-top`, `TOP_LEVEL_ENTITY`). Otherwise the top is the single design unit no
other unit instantiates; zero (`no_top`) or several (`ambiguous_top`) candidates is an error that
lists them. A project whose files are all BLIF takes the first model of the first file (BLIF's
rule). The chosen top is recorded in the IR (`odin3_design_set_top`, IR-11 design record) and
reported by `stats`.

**Provenance.** Each source record names its file through the project (path as given, library),
so `file:line` queries stay unambiguous when two libraries hold files of the same name.

### 4.1 Verilog-2005 (owned)
- Preprocessor first: `define/`ifdef/`else/`endif/`include, macros with args, `` `timescale `` tolerated.
- Bison/Flex grammar (LALR(1), zero conflicts; generated at build time (Bison ≥ 3.8, Flex ≥ 2.6.4)), location-tracked tokens; every AST node has a source range `{loc, end}` of two 32-bit source-manager locations, decoded on demand to file, line, column and the macro/include chain (2A spec §3, AST-2), not five fields per node, and an attribute list (`(* ... *)` and pragmas).
- Elaboration: parameters/localparam/defparam, `generate` (if/for/case), functions and (synthesizable) tasks, integer/genvar, signed semantics per IEEE 1364-2005 §5, implicit nets (warn), Quartus-dialect tolerance.
- Comments kept in a side table keyed by location (not a red/green tree).

### 4.2 SystemVerilog (slang adapter) (deferred, D10)

Not built in v1.0; how it would be done is in §12.1.

- slang is C++20 and MIT. `adapters/slang/` is a small C++ shim that links slang, walks its *elaborated* AST (post-parameter, post-generate), and emits the Odin III AST through the C ABI (`odin3_ast_*` builders). It exports exactly one C function, `odin3_read_slang(...)`, and is built as a separate shared library so the core stays pure C. slang handles packages, interfaces (flattened), structs, enums, `always_ff/comb/latch`, `logic`.
- Source locations copied verbatim.

### 4.3 VHDL (GHDL subprocess) (deferred, D10)

Not built in v1.0; how it would be done is in §12.1.

- `ghdl --synth --out=verilog` per design unit → Odin III Verilog front end. GHDL's generated names are stable enough to keep provenance to the VHDL line via a name map GHDL emits. **OPEN:** confirm GHDL's `--out=verilog` coverage on the VTR benchmark set once converted.

### 4.4 Netlist readers
- BLIF (full: `.subckt`, `.latch` with init, `.names`, black-box `.model`s) — required for the ABC round-trip.
- VQM (Quartus post-synthesis Verilog): the Verilog grammar plus the Altera primitive library; no new parser. (removed from v1.0, D10)
- EDIF 2.0.0: S-expression reader, structural only. (removed from v1.0, D10)
- These produce bit-level IR with structural-only provenance; the `raise` pass (§8; removed from v1.0, D10) recovers word-level structure.
- LPM-structured Verilog (v1.0, Phase 5 item 5.1, D10, D11): read by the Verilog front end; the LPM tech library makes the `lpm_*` instances hard cells.

### 4.5 Common AST and Symbol Table
- Scoped symbol table (module → generate scope → block), each symbol with declaration location, type (net/var, width, signedness, array dims), and the list of AST nodes that assign it.
- AST is the "high" layer, one store per read run (2A spec §9, AST-15/16): it is freed at the end of its read pass unless kept (`--keep-ast`, or a pass whose registry entry sets `wants_ast`). IR objects reach it only through the SOURCE provenance record's `ast` node ID, resolved in the held store whose run and form match; locations survive in provenance and the source manager either way.

## 5. The IR

### 5.1 Object model (Odin II lineage)
- `Design` → `Module`s (hierarchy kept until an explicit `flatten` pass).
- `Module` owns `Node`s, `Pin`s, `Net`s as **peers** (Odin II's key property): a node has pin lists; a pin belongs to one node and attaches to one net; a net has one or more driver pins and any number of sink pins. Multi-driver and tristate are representable and flagged by `check`.
- `Node` has a `CellType` from the **op registry** and an `Attributes` map.
- Storage: arena-allocated, dense integer IDs (`uint32_t`), stable across passes; name → ID maps kept per module. Objects are plain structs; all cross-references are IDs, never pointers, so the IR is trivially serializable and the C ABI can hand out IDs safely. Target: 2M nodes in < 2 GB (Titan).

### 5.2 Op registry (CIRCT-inspired, one file per cell type)
Each cell type is a `const struct odin3_celltype` table entry declaring: name, typed ports (direction, width expression, signedness), parameters, verifier function pointer, **simulation semantics** (C function pointer), BLIF/Verilog/JSON emitter function pointers, and a `granularity` tag (`word`, `bit`, `hard`, `blackbox`, plus the structural `module` and `port` — `docs/IR.md` IR-9). Adding a cell type touches exactly one file; plugins can register cell types at load time.

Word-level set (mirrors Yosys RTLIL): `$add $sub $mul $div $mod $neg $pow $and $or $xor $xnor $not $shl $shr $sshr $shiftx $eq $ne $lt $le $gt $ge $logic_not $logic_and $logic_or $mux $pmux $dff $dffe $adff $sdff $adffe $sdffe $sdffce $aldff $aldffe $dffsr $dffsre $dlatch $adlatch $dlatchsr $sr $mem $memrd $memwr $reduce_and $reduce_or $reduce_xor $reduce_xnor $reduce_bool`. (`$neg $xnor $reduce_xnor $reduce_bool $logic_not $logic_and $logic_or $shiftx $pow` are 2D's IR change A, EL-31: simlib semantics, with simulate hooks for all but `$shiftx` and `$pow`.) (`$adffe $sdffe $sdffce $aldff $aldffe $dffsr $dffsre $dlatch $adlatch $dlatchsr $sr` are 2E's IR change D, 2E spec §13: simlib's ports, parameters and meaning, the whole word flip-flop and latch family, with no simulate hooks, as `$dff $dffe $adff $sdff`.) Process cell: `$proc` (2D's IR change B, EL-33; `docs/IR.md` IR-19), one node per `always` block, with real `I`/`O` pins and the block's decision structure as its opaque `BODY` parameter; `proc` lowers it. (`$concat`/`$slice` are not cells: nets are one bit and a port is a pin vector, so slicing and concatenation are which nets a port uses — `docs/IR.md` IR-1.)
Bit-level set: `$_AND_ $_OR_ $_XOR_ $_NOT_ $_MUX_ $_DFF_* $_DLATCH_* $_FF_ $_CONST*_ $sop` (BLIF `.names` covers) and `$lut` (a K-input LUT, Yosys semantics; `docs/IR.md` LUT cells). Structural: `$port_in/out/inout`. Hard and black-box cells keep their own names (e.g. VTR `adder`, `multiply`), registered by the tech library or declared by a netlist (`docs/IR.md` IR-7b).
Hard set: generated from the VPR arch `<model>` list and from the Altera primitive library.

### 5.3 Provenance
Every node, net and wire carries a provenance ID (pins inherit their node's) into an append-only, hash-consed lineage DAG: each record holds its kind (source, imported, derived), the pass run and operation that made it, source locations, AST node, hierarchical path, and parent records — the parents replace v0.2's `origin` field. Navigation runs backward (object → sources) and forward (location or record → objects, live or dead) on demand. Details: `docs/IR.md` §6 (IR-12, IR-13). Emitted names follow `hier/path/cellname@file:line`; a `--name-style` flag chooses between provenance names and short names.

### 5.4 Views
- `rtlil view`: asserts all nodes are `word`/`hard`/`blackbox`/`module`/`port`; exposes a Yosys-like API for passes.
- `netlist view`: asserts all nodes are `bit`/`hard`/`blackbox`/`module`/`port`.
- Mixed granularity is legal during `lower`/`raise` only.

## 6. Pass pipeline (forward)

1. `read_*` → AST, symbol table
2. `elaborate` → word-level IR with processes: each process is a `$proc` node (`docs/IR.md` IR-19). The pass manager enforces the order through each pass's `processes` flag (2D EL-33, mode P since 2D spec v4.2): a `REFUSE` pass (`flatten`, `lower`, `abc`, `sim` and the netlist writers) fails on a design that still has processes (`pass '<name>': module '<m>' has processes; run proc first`), so `proc` runs before them. The opt family is `OK` and may run before `proc`, as Parmys runs `opt_expr; opt_clean; opt -nodffe -nosdff` (2F OP-3): `$proc` stays opaque to it, and `opt_muxtree`/`opt_clean` skip a module that still has processes
3. `proc` — Yosys-equivalent lowering of `$proc` cells: decision trees to `$mux`/`$pmux`, syncs to `$dff`/`$adff`/`$aldff`/`$dffsr`, latches to `$dlatch` (warned) (blocking/non-blocking analysis is 2D's)
4. `opt` — Yosys 0.55's `opt` family, run to a fixed point (`opt_expr`, `opt_merge`, `opt_muxtree`, `opt_reduce`, `opt_share`, `opt_dff`, `opt_clean`; `docs/specs/2026-10-11-2F-opt-design.md`): constant folding, dead-node removal and CSE are `opt_merge`'s, `opt_clean`'s and `opt_expr`'s; arithmetic simplification is `opt_expr`'s; mux collapsing is `opt_muxtree` + `opt_reduce` + `opt_dff`'s. It never compacts: scripts run `compact` after it (IR-6, PHASE2 #19)
4a. `flatten` — expands every module instance of the top (and of every `keep_hierarchy` module) into the module that holds it; objects get Yosys-style dotted instance-path names (`u1.mid.x`), the joined port nets keep every name as IR-2 aliases, and the instance path (`top/u1/mid`) goes into `DERIVED` provenance records (`odin3_prov_derive_hier`); black boxes, hard cells and library cells stay (3A; `docs/specs/2026-10-14-3A-flatten-design.md`). The Phase 3 script runs `opt -full` after it (as Parmys does)
5. `fsm` — Yosys 0.55's `fsm` (`fsm_detect`, `fsm_extract`, `fsm_opt`, `fsm_recode`, `fsm_map`), right after `proc` as Parmys runs it; `auto` encoding is one-hot below 32 states (Phase 4 item 4.8; restored by D10 follow-up 2)
6. `memory_infer` — `reg` arrays → `$mem` with port analysis; attribute overrides
7. `read_arch` — `.o3lib` tech libraries (target-agnostic: gates first, then FPGA hard cells; `docs/specs/2026-10-08-1G-techlib-design.md`) → hard/gate set; VPR XML `<model>`s and pb_types via the importer (Phase 4.1) into the same form. The Altera target is an LPM `.o3lib` (Phase 5 item 5.1, D10, D11); there is no Altera device importer in v1.0
8. `partial_map` (§7)
9. `lower` — word → bit; hard/blackbox nodes untouched. `lower` is Yosys 0.55's `simplemap`/`techmap.v` structures; asynchronous controls become synchronous (decision 19); the script follows it with `opt -fast; compact`
10. `abc`: linked ABC (`adapters/abc`, an engine behind the C ABI) maps each module's soft logic to `$lut` cells, one ABC run per clock domain, with hard cells, black boxes, instances and other domains' flip-flops as cut points (hard-block inputs on `unconn`, x or a constant pass unchanged; soft logic reads them as 0); the script is `abc --script`, else `--abc-script`, else the project's `abc_script`, else `lib/abc/vtr.abc`; results are re-stitched with provenance from ABC's correspondence (3D spec)
11. `write_*`

## 7. Partial mapping

Partial mapping binds *some* of the word-level IR to the target's hard circuits and leaves the remainder as soft logic for ABC. It runs before `lower` because word-level operators are the only place hard-block shape is visible.

Components:
- **Inference** — three tiers, as in Odin II: explicit instantiation (primitive library), coding-style rules (memory inference), and open-ended **subgraph matching** (Odin I, re-implemented generically). v1.0 (D10) has the first two plus exact-port-signature matching; subgraph matching is deferred (D5 amended).
- **Binding/packing** — for each matched structure choose an implementation: hard block(s) + generated soft glue, or all-soft. Includes recursive multiplier splitting with the small-multiplier threshold auto-derived from the arch, signed multipliers, memory depth/width splitting with width-depth trading, carry chains (`adder` model), DFF feature matching (enable, sync/async reset) to what the arch's FF supports.
- **LPM mapping** (v1.0, Phase 5 item 5.1, D10, D11): the separate pass `lpmmap` binds `$add`/`$sub`, `$mul`, the comparisons and inferred `$mem` to `lpm_add_sub`, `lpm_mult`, `lpm_compare` and `altsyncram` by fixed rules (`docs/specs/2026-10-19-5.1-lpm-design.md`); it is not a rule set of `hardmap`, which stays the VTR-arch pass.
- **Generic hard blocks** — any arch `<model>`; matched by exact port signature (Odin II) (v1.0, Phase 4.2) or by a user-supplied pattern (deferred, D10).
- **Project rules** (§4.0) steer all three (mapping rules: deferred, D10; the project file still reads and stores them): per-scope `map … to/soft/keep`, width/depth thresholds, per-cell budgets, project-only patterns; each binding records the rule that chose it.

Matcher (§8, deferred, D10) pattern format: an IR fragment written as a tech-library cell function — the `.o3lib` expression language (Verilog-like, parametric widths) compiled to IR — plus a cost (resolved 2026-10-08, PHASE1 #7/#8). Patterns live in the tech library beside the cell they map to. Overlap resolution: maximum-cover with cost tie-break (Odin I's rule generalized).

## 8. Subgraph matcher and reverse engineering (deferred, D10)

Not in v1.0 (D5 amended by D10). The text below is the original plan, kept as the design for later work. Reverse engineering is now approached from Altera (LPM structural Verilog) up, which Phase 5 item 5.1 starts (§12.1, D11).

One matcher, two directions:
- Forward: patterns = target hard blocks; candidates seeded at anchor cells (e.g. `$mul`) as in Odin I.
- Backward (`raise`): patterns = generic word-level structures (ripple/CLA adders, comparators, mux trees, counters, shift registers, memories, FSM state registers); matched cones are replaced by word-level cells, so RE is literally `lower` run backwards.

Algorithm: VF2-style with anchor seeding and width-agnostic matching; semantic verification of each candidate by local equivalence (ABC on the cone) before commit. Outputs for RE: raised IR → `write_verilog` at word level, plus a report mapping recovered structures to netlist regions.

## 9. Writers and readers

| Format | Read | Write | Notes |
|---|---|---|---|
| BLIF | yes | yes | black boxes; `.latch` init. VTR dialect (`write_blif --vtr`): top only, `.latch … re` init 0/1/2, `unconn` on open hard-block inputs |
| Verilog | yes (front end) | structural (bit/word); LPM-instantiating flavour (v1.0, Phase 5 item 5.1, D10, D11) | Altera atoms flavour removed from v1.0 |
| SystemVerilog | via slang (deferred, D10) | word-level structural | |
| VQM | removed from v1.0 (D10) | — | Titan (removed from v1.0) |
| EDIF | removed from v1.0 (D10) | dropped (7.5, D10) | |
| JSON (Yosys schema) | later | yes | netlistsvg, nextpnr |
| RTLIL (Yosys text) | — | yes | word level with processes; the Yosys bridge for 2D–2F tests |
| Graphviz dot | — | yes | hierarchy clusters; `--focus <hier-path | file:line | cone(net)>`; never renders > N nodes without a focus |
| CIRCT MLIR | later | yes | hw/comb/seq |

## 10. Verification

- `simulate`: cycle-based event-free simulator over the netlist view using op-registry semantics; hard blocks simulate via their registry function; random or file vectors; VCD out.
- `equiv`: ABC `cec` (combinational) / `dsec` (sequential) between any two IRs or BLIFs; Verilator harness comparing emitted Verilog against source.
- `check`: structural invariants (no dangling pins, driver counts, width consistency, clock-domain attribute consistency, granularity view) — runs around every pass in debug.

## 11. Testing and benchmarks

- **Oracle**: Parmys (and Odin II via `WITH_ODIN=ON`) golden BLIFs, archived per VTR release.
- **Microbenchmarks** (VTR `odin_ii/regression_test/benchmark/...`): required *identical* canonicalized netlist across Verilog, SV (converted), and VHDL (converted) front ends, and equivalent to Parmys.
- **VTR 19**: full flow to P&R; QoR table (min W, critical path, LUTs, FFs, hard blocks) vs. Parmys (Phase 3 exit; after Phase 3 QoR parity is not an exit criterion, D10; Phase 4 sweeps several archs for routability and equivalence).
- **Titan** (removed from v1.0, D10): smallest designs first; VQM path and source path both; must match Quartus-synthesized BLIF functionally.
- **Altera/LPM** (v1.0, Phase 5 item 5.1, D11): written, read back and linted; equivalence to the RTL using the LPM library's simulation models when Peter needs that step; an optional Quartus Lite compile is by hand, not CI.
- **Fuzzing**: random Verilog generator with Yosys as differential oracle, from Phase 2.
- **Reverse engineering** (removed from v1.0, D10): round-trip test — Verilog → lower → BLIF → read → raise → Verilog → equiv, plus structure-recovery scorecard.
- CI runs lint gate + unit + micro + fuzz on every push; VTR 19 nightly; Titan weekly (Titan removed from v1.0, D10).

## 12. Development path

| Phase | Deliverable | Exit test |
|---|---|---|
| 0 | WSL2, builds of VTR/Yosys/Parmys/ABC/GHDL; golden BLIFs; `netlist-compare`, `equiv-check`; lint gate; skills; `docs/DESIGN.md`; CI | Oracles run green on all goldens; lint gate green (see `odin3-phase0-setup.md`) |
| 1 | `util/`, core IR, op registry, pass manager, `check`, provenance, C ABI v0, BLIF read/write, dot/JSON/Verilog writers, simulator, tech-library format + reader + generic gate library | BLIF→IR→BLIF bit-identical on goldens; sim matches ABC on goldens; a Python plugin can walk the IR |
| 2 | Verilog-2005 front end + preprocessor + elaboration; project input (§4.0: `.o3proj`, `-f` file lists, Quartus `.qsf`/`.qpf` import, top selection); `proc`, `opt`; VTR benchmark set elaborated (PHASE2 #2) | Micros identical/equivalent to Parmys; every project fixture (`tests/golden/projects`) parses to its expected record from `.o3proj`, `-f` and `.qsf`, and every phase-2 fixture elaborates to the same netlist from each of those formats, equivalent to its oracle (or fails with its located error); the Odin II XML format is excluded until its reader lands |
| 3 | `lower`, linked ABC, VTR flow hookup | VTR 19 through P&R; QoR table |
| 4 | VTR architectures and FSM (D11): VPR-XML import into the tech library (4.1); generic hard blocks by exact port signature (4.2); multiplier splitting from the arch (4.3); memory depth/width splitting (4.4); carry chains from the arch model (4.5); FF feature matching (4.6); memory port merge (4.7); FSM detect and recode, one-hot below 32 states as Yosys, run after `proc` as Parmys (4.8); arch-sweep test (4.13). Dropped (D10): mux collapsing (4.9, done by `opt`), matcher v1 (4.10), project mapping rules (4.11), per-bit `opt_dff` parity (4.12). Plan: `docs/PHASE4.md` | The tool works on VTR architectures: the VTR designs on several VTR archs (k6_frac_N10, one DSP arch, one plain-LUT arch) route in VPR, pass the vtr-qor fcheck, and the micros are equivalent; 4.8 passes its test. QoR parity is not an exit criterion (D10) |
| 5 | Outputs and read-back (D11): LPM via structural Verilog, light, family-neutral (5.1); Verilog read-back at word, gate, LUT and LPM level with `read_verilog -icells` (5.2); Yosys JSON reader (5.3, was 7.2); HDL locations through write and read for BLIF, Verilog and JSON (5.4). Plan: `docs/PHASE5.md` | Every BLIF, Verilog and JSON output reads back equivalent with its HDL locations; LPM micros write, read back and lint |
| 6 | Analysis flow and provenance (D11): gate-level netlist ingest (6.1); `write_dot --color-by` and provenance labels (6.2); graph export as Yosys JSON plus Odin III keys (6.3); `levelize` (6.4); provenance coverage gate (6.5). Plan: `docs/PHASE6.md` | 6.1 to 6.4 pass their tests; 6.5 resolves every final-netlist object (BLIF, LPM, VPR blocks) to the HDL apart from named exception classes |
| 7 | CIRCT writer (7.1), visual tooling polish (7.3), docs including the extension-API review (7.4), release packaging and the v1.0 tag (7.6). The Yosys JSON reader (7.2) moved to 5.3 (D11); the EDIF writer (7.5) is dropped (D10) | Release |

Agentic working rules: one pass per PR; golden test per pass; `check` on in debug; lint gate must pass before commit; agent reads `docs/DESIGN.md` and the op-registry file for any task; human reviews IR/invariant changes and all mapping algorithms; no PR merges red. Per-pass workflow: `superpowers:brainstorming` → `superpowers:writing-plans` → human approves → `superpowers:executing-plans` → `/run-micro` → PR.

### 12.1 After v1.0 (D10, D11, 2026-10-19)

Peter re-scoped v1.0 on 2026-10-19 (`docs/ADR/D10.md`). Nothing below is built in v1.0; it is
kept so the plan is not lost. Original phase 4 items dropped are listed in `docs/PHASE4.md`.
D11 renumbered the v1.0 phases: "Phase 5" and "Phase 6" below are the old phases, now
post-v1.0 work, not today's Phases 5 (outputs and read-back) and 6 (analysis flow).
The research plugins of D10 follow-up 2 (Gate-Breaker redo, Bayesian classifier, graphlets,
graph sandwich, GNNs, placement study) are also after v1.0; they read Phase 6's outputs.

**Old Phase 5, after v1.0** (reason: Verilog is close to good enough). How each step would be done:

- 5.1 slang adapter: a C++ shim in `adapters/slang/` links slang, walks its elaborated AST
  (post-parameter, post-generate) into the `odin3_ast_*` builders, exports exactly one C function
  (`odin3_read_slang`), and is built as a separate shared library so the core stays pure C
  (§4.2).
- 5.2 GHDL path: run `ghdl --synth --out=verilog` per design unit, then our Verilog front end,
  with a provenance name map from GHDL's names back to VHDL lines. Fallback if `--out=verilog`
  coverage is short (§13 Q2): link libghdl in a separately licensed optional module (§4.3).
- 5.3 Cross-language test: the same micro in Verilog, SV and VHDL gives an identical
  canonicalised netlist.
- 5.4 Fixtures: SV, VHDL and mixed-language project fixtures elaborate from each project format
  and are equivalent to their oracle.

**Old Phase 6, removed from v1.0.** What it would have been:

- 6.1 `raise`: `lower` run backwards via the matcher (§8), bit-level cones to word-level cells.
- 6.2 RE scorecard and round trip: Verilog, `lower`, BLIF, read, `raise`, Verilog, equivalence
  check, plus a structure-recovery scorecard (§11).
- 6.3 VQM and EDIF readers (§4.4).
- 6.4 Altera `bind` output: bind/unbind between generic cells and device atoms (D6 original).
- 6.5 Primitive library v0 (`lpm_*`, `alt*`, `stratixiv_*`).
- 6.6 Titan subset: smallest design parsed from its `.qsf` first (PHASE2 #2).

Reverse engineering is now approached "from Altera (LPM structural Verilog) up", which Phase
5 item 5.1 starts: the LPM library reads LPM-structured Verilog back with LPM instances as hard
cells.

**Also out of v1.0:** matcher v1 (4.10), project mapping rules (4.11), per-bit `opt_dff` parity (4.12), the EDIF
writer (7.5). Mux collapsing (4.9) is done: `opt_muxtree` and `opt_reduce` in `opt`.

## 13. Open questions

1. ~~Matcher pattern DSL and where patterns live.~~ **Resolved 2026-10-08:** tech-library (`.o3lib`) cell functions compiled to IR fragments, living in the tech library (PHASE1 #7/#8).
2. GHDL `--out=verilog` coverage; fallback is linking libghdl in a separately-licensed optional module. **Deferred (D10):** the old Phase 5 is after v1.0 (D11).
3. Primitive library v0 scope: exactly which `lpm_*`/`alt*`/`stratixiv_*` the smallest Titan designs need. **Removed (D10)** as framed (Titan and `stratixiv_*` are out of v1.0). Replaced by the fixed LPM set of Phase 5 item 5.1 (D11; `lpm_add_sub`, `lpm_mult`, `lpm_compare`, `altsyncram`; `docs/PHASE4.md` #3).
4. Memory layout details (arena vs. SoA) — decide in Phase 1 with a 2M-node synthetic benchmark.
5. Which Altera generation for `bind` output first (Cyclone IV/V for Quartus Lite vs. Stratix IV for Titan). **Answered for v1.0 (D10; Peter, 2026-10-19, PHASE4 #2):** no `bind` output; Cyclone V (Quartus Lite), via LPM. Family-neutral LPM with a family option; Stratix reachable, untested (PHASE4 #4, PHASE5 #1).
6. Name-style policy for VTR: provenance names are long; measure VPR runtime impact. 3A's part (PHASE3 #30): `flatten` names flat objects with dotted instance paths (`--separator` changes the dot), leaves unnamed objects unnamed (the path is in provenance) and labels the children of an unnamed instance `$c<ID>`; which style a writer prints is still open (3E-6/3H).
7. Whether `raise` should also target Yosys's internal cell names for interop. **Removed (D10):** `raise` is out of v1.0.
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
- Errors: functions return `odin3_status` (an enum); no `exit()`/`_exit()`/`_Exit()`/`quick_exit()` outside `cli/`; no `abort()`, `raise(SIGABRT)` or `__builtin_trap()` outside `check` in debug builds, except the ABC adapter's off-thread stop, which is `__builtin_trap()` (PHASE3 #28). `assert()` is allowed (it compiles out of release builds). Checked by `tools/check_rules.py` and, at link level, `tools/check-symbols.sh`.
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
