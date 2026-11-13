# Changelog

All notable changes to Odin III. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Odin III has no API stability promise before a later release (`docs/PHASE7.md`, decision 4).
PR numbers are GitHub pull requests; the phase checklists in `docs/PHASE*.md` hold the evidence.

## Unreleased

### Added
- Provenance gaps for per-LUT cell attribution (additive; netlists and the name map version are unchanged). (1) The records
  `lower` makes for a word cell now carry the cell type, its width and, for muxes, the number of arms, written in the `flags`
  field of the `R` line as `cell=<type>`, `cw=<width>` and `ca=<arms>` (`$mux` has 2 arms, `$pmux` its selects plus the default);
  restore reads them back. Two cells from one HDL statement have two such records (one `lower` operation per cell). (2) `abc
  --prov-cut on` gives each LUT a record whose parents are the records of the old gates its function covers, flagged `cut=exact`;
  a LUT whose inputs or output have no old equivalent net, or whose proof fails, gets a one-parent record on its region record,
  flagged `cut=approx`. The old region is built as an AIG and simulated on 256 random patterns to find old nets with the same
  function as the LUT's inputs and output; a cut is accepted only after a local proof that the old function of the root net over
  the leaf nets is the LUT's truth table (LUTs up to 12 inputs). It works with either ABC exchange and changes no netlist. The
  abc INFO line `LUT cuts: N exact, M approximate; approximate: ...` gives the counts and the reasons. LUTs ABC built from a
  restructured function that no old net has stay approximate. Default behaviour of
  `--prov fine|cone` is unchanged. `odin3-flow --prov-cut` passes it on. Docs: `docs/user/provenance.md`. Tests: `prov_cut`
  (`tests/tools/test_prov_cut.py`).
- `odin3-flow --keep-results` after a route retry keeps one usable run directory: `vpr/run` is now the routed run and the failed
  first attempt moves to `vpr/run-failed-W<W>` (before, `vpr/run` held the failed attempt, which `vpr --analysis` cannot use, and
  the routed files sat in `vpr/retry-<W>/run`). Test: `RouteRetry.test_keep_results_after_retry_leaves_one_usable_run_dir`.
- `sim_tb --progress SEC` prints an INFO line every SEC seconds of wall time (simulated time against `--max-time`, time steps,
  wall seconds, simulated time units per second), so a long run can be told from a hang. Off by default.
- `sim_tb` now says so when the time limit ends a run: a WARNING names the limit (`--max-time`, default 10,000,000) and the
  option to raise it, then the summary line `sim_tb: stopped at time T (time limit)`; before, the run ended silently with
  `finished at time T`. The default limit is unchanged (`run_testbenches` shares it as its hang limit). Tests: `sim_tb_progress`.
- `read_verilog -P NAME=VALUE` overrides a parameter of the top module at elaboration, as a project `param` line does (and ahead
  of the project's lines): a Verilog number (`16`, `-3`, `8'hFF`), else a string (`'12'` forces one); several allowed; an unknown
  name is the located error `param '-P Q' does not name a parameter`; not for SystemVerilog files. With it a testbench `dut #(.W(12))`
  runs: `read_verilog -P W=12 dut.v; proc; opt; lower; sim_tb tb.v` (the `sim_tb` check compares the `#()` value with the
  elaborated one; its refusal now says `read the DUT with 'read_verilog -P W=16'`). A project file's `param` key already existed.
  Tests: four cases in `test_read_verilog_lib`, `tests/tb/micro/dutparamp_tb.v` (iverilog), the two `dutparam*_differ` error tests.
- The simulator's `$mem` cell (`lower --keep-mem-cells`) takes ports on different clocks and edges: one edge cell per clock net
  and polarity, all sharing the array; a dual-port RAM written on `wclk` and read on `rclk`, mixed rising and falling edges, and
  write ports on different clocks (which `lower` refuses) simulate. The edges of one instant read the memory as it was before the
  instant and the writes then apply in write-port order (later port wins). A transparent read sees the writes on its own clock
  and edge only, as in `lower`. Still refused: an asynchronous write port (a write with no clock), over 16 clock/edge pairs.
  Tests: `test_lower_mem` (cell against the lowered netlist, random clock subsets, every polarity mix), micros `mem2clk`,
  `mem2clkm`, `mem2pol` (4-state, 2-state, lowered, cell; iverilog), `tests/tb/run_memclk.sh` (`tb_memclk`: two write clocks vs
  iverilog).

### Fixed
- Front-end gaps found on VTR and OpenCores sources (purpose: Odin III goal 1, front-end and elaboration correctness, VTR comparable).
  (1) A macro name may start with a digit (`` `define 2_X 5 `` and `` `2_X ``; Yosys accepts it, the VTR `adder_tree` files use it).
  Formal argument names still must start with a letter or `_`. A backtick followed by a digit is now an undefined-macro error
  (before, a stray-backtick error). Tests: `test_pp_define` (`test_digit_leading_macro_name`).
  (2) `read_verilog -D` and `-I` now work for SystemVerilog (`.sv`) files: they reach slang's preprocessor after the project's own
  defines and include directories (before, the read was refused with "use a project"). `-P` is still refused for `.sv`. Test:
  `test_read_slang` (`test_sv_command_line_config`).
  (3) `SYNTHESIS` is predefined for every Verilog and SystemVerilog read, as Yosys does (so `ifndef SYNTHESIS` simulation-only code is
  skipped); a project or `-D` define of the name wins. Tests: `test_read_verilog_lib` (`test_synthesis_*`), `test_read_slang`
  (`test_sv_synthesis_predefined`).
  (4) slang adapter: explicit cast `W'(x)` (same width: no change; narrower: part select; wider: zero or sign padding), `unique`/
  `unique0`/`priority` on `case` and `if` (qualifier dropped), and a simple assignment pattern `'{...}` for a packed array or
  struct. An assignment pattern for an unpacked array or struct is still refused with a clearer message (VTR `hdmi` stays failed).
  The VerilogEval references Prob151 and Prob156 now read (`tests/cli/veval/manifest.tsv`: refused to ok). Tests: `test_read_slang`
  (`test_sv_gap4_constructs`), `cli_veval_corpus`.
  (7) An instance of an implicit black box (`--auto-blackbox`; for example `ROM32X1 u (a, b, c)`) may use positional connections: the
  ports are named `$1`, `$2`, ... in order, as Yosys names the ports of a cell of an unknown module, with a WARNING; a mix of named
  and positional connections is an error as for a module. Before, any positional connection was `needs named connections`. Test:
  `test_elab_stage1_inst` (`test_implicit_positional`, `test_implicit_mixed_connections`).
  (9) `write_json --skip-flattened` leaves out the modules `flatten` marked as no longer instantiated (they stay in the design, as
  `flatten` says in its INFO line). Off by default, so the file readers see does not change. Test: `test_json_odin3`
  (`test_skip_flattened`).
  (8) A concatenation target with a select whose index is not constant (`{a[j+3], b[j +: 4]} = ...` inside nested loops, picorv32
  `ENABLE_MUL`) is split per part as a memory word already was, instead of the error `non-constant index into ... in a
  concatenation target`. Test: `test_elab_stage2_case` (`test_variable_index_in_concatenation_*`).

## 1.6.1 (released 2026-11-11, tag v1.6.1; release notes `docs/V1.6.1.md`)

### Added
- `lpmmap` maps a ROM to `altsyncram` in `ROM` mode (`--target lpm`, `write_verilog --quartus`): an inferred `$mem` with one read
  port, no write port, every `INIT` bit 0 or 1 (`reg [W-1:0] m [0:N-1]; initial $readmemh(...)`), a synchronous read on a
  rising edge and no read enable (the read register is merged as for a RAM). Before this the ROM stayed a behavioural `reg`
  array (2.7 MB of per-word `initial` statements for the AI-digit I3 P=1 design); now the instance carries
  `.operation_mode("ROM")` and `.init_file("<netlist>_rom<N>.mif")`, and `write_verilog` writes the contents to that
  Memory Initialization File beside the netlist (`ODIN3_ATTR_ROM_INIT` node attribute; no writer prints it). Port a only
  (`address_a`, `clock0`, `q_a`). A ROM with an asynchronous read, a falling edge, a read enable or unknown INIT bits stays soft
  with a warning. A RAM with `INIT` still stays soft. `lib/lpm.o3lib` gains `init_file` (default `"UNUSED"`). The VTR leg is
  unchanged. Tests: `tests/lpm/altsyncram_rom.v` (contents checked against the `.mif`), `altsyncram_rom_soft.v`,
  `test_lpmmap_ram`, `test_lpm_check`.
- `lower --keep-mem-cells`: leaves each `$mem` as a cell, and the simulator (`sim_tb`, `sim`) models it directly instead of
  one flip-flop per stored bit: one byte of state per bit, an edge cell for the write ports and synchronous read ports and
  a combinational cell for the asynchronous read ports, with the semantics (and x rules) of the lowered netlist. Async and
  sync reads, `RD_TRANSPARENT`, `RD_EN`, several write ports, per-bit write enables, OFFSET, `INIT`, `$readmemh` into a DUT
  memory by path (`uut.mem[i]`, no `--keep-mem` needed). One clock net and one edge per memory; several clocks or an
  asynchronous write port are a located error. The AI-digit layer `l_fc2` unit test (30,395 cycles) runs 13 times faster than
  through `lower`. Netlist-view passes still need the default `lower`. Design: `docs/specs/2026-11-11-sim-mem-hook-design.md`.
- `read_verilog -D NAME[=VALUE]` and `-I DIR` (repeatable): command-line preprocessor defines and include directories for a Verilog read, after the project record's. A `sim_tb` run on a design that needs defines no longer needs an `.o3proj` (`sim_tb` already took `-D`/`-I` for its testbench files).
- `odin3-flow --project X.o3proj`: the design files, include directories, defines and `top` come from the project file, so the top is declared and never guessed (a different `--top` is an error; several uninstantiated modules without a top is still exit 2 naming them).
- `write_verilog --quartus [--sim-file F]` and the LPM target `odin3-flow --target lpm` write a netlist Quartus can read: no
  Odin-internal cell type and no module definition of a library name. Every flip-flop and latch cell type (`$sdffe`, `$adffe`,
  `$sdffce`, `$aldff`, `$aldffe`, `$dffsr`, `$dffsre`, `$dlatch`, `$adlatch`, `$dlatchsr`, `$sr` and the 68 gate-level
  `$_DFFE_`, `$_SDFF_`, ... types) is plain Verilog-2001 `always` blocks on a `reg`, the cell type and name in a comment; a
  `$mem` lpmmap leaves soft is a behavioural `reg` array that Quartus infers as RAM (INFO line per memory); `$shiftx` and
  `$pow` are `assign`s. The stubs of the LPM and black-box modules go to `<top>_lpm_sim.v` (read-back and iverilog only).
  An implicit black box (`--read-opt=--auto-blackbox`, e.g. `scfifo`) keeps its parameter overrides as `#(.NAME(value))` and
  its unconnected ports as `.port()`. Tests: `tools/quartus-check`, `tests/quartus`, `tests/tools/test_quartus_check.py`.
- `sim_tb` accepts a `#()` on the DUT instance when every value equals the DUT's default parameter (named or ordered, constants, strings); a different value, an unknown name, too many ordered values or a non-constant value is a located error that says to pass it to `read_verilog (-P, not yet)`. Elaboration now records each module parameter as the module attribute `odin3 param default:<NAME>` (bookkeeping, not written by the JSON, Verilog or RTLIL writers).

### Changed
- An instance of an implicit black box now carries its parameter overrides as `parameter:<NAME>` node attributes (as a module
  instance does); `write_verilog` writes them as `#(.NAME(value))`.

### Fixed
- A module read by an earlier `read_verilog` can be instantiated by a later one (`unresolved_module: module 'TreeLUT' is not defined` with two `read_verilog` commands). It resolves to the elaborated module with its default parameters; a `#()` override of it is an error that says to read it with `--lib`.
- `abc` no longer refuses a large gate-level design for memory it would not use. Its pre-run estimate of AIG objects charged a 2-input gate 6 (and an inverter 2); it now charges the ANDs of the gate's irredundant cover (AND and OR 1, XOR 3, inverter 0). The NeuraLUT designs nl-A and nl-C (2 M gates after `lower`) estimated 6.9 M objects, 3.6 GB at 520 B each, and hit the 4 GiB budget (`abc needs more than 4294967296 bytes`); they now estimate about a third of that.
- `write_verilog --quartus` keeps the power-up value of a word flip-flop: the `init` attribute of the nets its Q bits drive is written as `initial <reg> = N'b...;` (one word when every bit is known, else one line per known bit), so Quartus starts the register where the RTL did. `$aldff` stays an edge-only `always @(posedge clk, posedge aload)` (Quartus infers an asynchronous load from it; a change of AD while ALOAD is high is not followed in simulation).
- A variable index into every dimension of a multi-dimensional register array or net array (`unit_out[i_col][i_row]`,
  `mem[i][j]`, as a read in a process or in `assign`, and as the target of a process) is a nested case over the words, one
  switch per dimension, instead of the error `non-constant index into the multi-dimensional register array`. IEEE 1364-2005
  allows it; Yosys 0.55 elaborates it. An index outside a dimension reads x, as for one dimension.
- A function or task with a local array (`reg [7:0] m [7:0]` in `lfsr_mask`, verilog-ethernet) no longer fails with `'m' is a
  memory; select a word`: the array is registers, its words made per call like the callee's other locals; a bit of a word
  (`m[i][i] = 1'b1`) can be written.
- `tests/micro/verilog/syntax/matrix_multiplication.v` now elaborates (it had the multi-dimensional error); its Yosys 0.55
  bridge still stops at the dual-edge rejection that Parmys also hits.

## 1.6.0 (2026-11-09; release notes `docs/V1.6.0.md`)

The C ABI stays 7.

### Added
- `activity` pass: vectorless static probability and transition density of every net on the netlist view, ACE 2.0 comparable;
  VPR `.act` output under the `write_blif --vtr` names, JSON `odin3-activity`, module attributes `odin3:activity_*`, Python
  `Design.activity`. Spec `docs/specs/2026-11-08-activity-design.md`, user page `docs/user/activity.md`, test `activity_unit`,
  comparison tool `tools/activity/act_check.py`.
- `odin3-prov-steps FLOW_DIR --activity F.act`: `attrs.act_p1`, `attrs.act_density`, `attrs.act_source` on the `luts` objects (no schema change).

### Fixed
- `sim_tb` 4-state: a tech-library hard cell with an unknown input bit made every output x, so a testbench that passes on
  the lowered gates failed on a `hardmap`'d netlist (mmuart, verilog_cordic_core, wb_lcd on the abc-mapped netlist, carry
  chains of `adder`). The generic wrapper now runs the cell on every 0/1 completion of up to 6 unknown input bits and keeps an
  output bit known when all completions agree (as the gate netlist does); beyond the caps all outputs are x. Test `tb_hard4`.
  A testbench that relies on the RTL meaning of `if (x)` (wb_lcd) still fails in the default 4-state mode on an `abc`-mapped
  netlist, because `abc` drops the RTL select attribute (`lower --keep-sel`) and the LUT cover propagates x; run such a
  netlist with `sim_tb --2state` (wb_lcd: 307 checks pass on the full flow plus `abc --lut 6`; mmuart and verilog_cordic_core pass in both modes).

## 1.5.1 (2026-11-08; release notes `docs/V1.5.1.md`)

The C ABI stays 7. Two performance fixes for provenance runs of a real CPU (a-z80-modified); output unchanged.

### Fixed
- `odin3-prov-steps` held one dict per link of every object (up to 30k links each) and was OOM-killed at 10 GB; it now
  streams the `luts` step: peak RSS 340 MB (#407).
- `write_blif --prov-map` sorted the whole join set of every coarsen record, and again for each data parent; now once per
  distinct anchor list, with stamp arrays for the scratch sets: 1423 s to 64 s, name map byte identical (#408).
  `odin3_prov_label.joined` (internal) holds the first 16 rows; `n_joined` is still the total.

## 1.5.0 (2026-11-08; release notes `docs/V1.5.0.md`)

The C ABI stays 7. This section holds everything since v1.3.0; the entries headed 1.4.0 shipped in v1.4.0, the rest is new since then (see `docs/V1.5.0.md`).

### Added (sim_tb: DUT memories by hierarchical path)

- `lower --keep-mem`: each `$mem` word gets a wire `<memory>[<index>]` and a ROM keeps flip-flop storage (x until
  loaded). `sim_tb`: `$readmemh`/`$readmemb` into a DUT memory by hierarchical path, DUT array words with a variable
  index (`uut.m.ram[i]`), bit and part selects of an array word (`mem[i][7:4]`, also testbench arrays). Internal
  functions (not C ABI, used by `sim_tb`): `odin3_sim_poke_state`, `odin3_sim_signals_poke`. Micro `poke`, unit test `test_sim_poke`.

### Added (graphlet instances and metrics)

- `graphlets --instances G [--max N] --instances-out FILE`: the instances of one graphlet type, from the same ESU run
  as the counts (schema `odin3-graphlet-instances`). C ABI (additive, stays 7): `odin3_graphlets_instances`,
  `odin3_graphlet_nodes`, `odin3_graphlets_similarity` (GDD agreement and cosine of log1p at full double precision for
  two sets of orbit rows). Python: `graphlet_instances`, `graphlet_similarity`. Spec:
  `docs/specs/2026-11-06-odin3-graphlet-instances.md`.

### Added (1.4.0, silicon flow and provenance step files)

- Project files: `schema 2` and the statements `tool`, `flowdef`, `step`, `provlink`, `flow-var` describe named flows,
  ordered steps and the provenance relationship of each step; `dump_project` adds `schema`, `tools`, `flows` for schema 2
  files only. Schema 1 files load and dump unchanged. New error kind `unsupported_schema`. Spec:
  `docs/specs/2026-11-03-v1.4-silicon-provenance.md`; manual: `docs/user/project-flows.md`.
- Silicon flow (`tools/silicon`, `odin3-flow --target silicon`): Odin III to a routed SKY130 layout and back, no Yosys.
  `lib/sky130hd.o3lib` (84 cells, generated from Liberty by `tools/silicon/liberty_to_o3lib.py`). OpenROAD, Magic, Netgen,
  KLayout and the PDK are optional user-space tools; tests skip without them. Manual: `docs/user/silicon-flow.md`.
- Provenance step files (`tools/flow/prov_steps.py`, `docs/schemas/*.json`, `tools/silicon/schema_check.py`,
  `odin3-prov-steps`): one manifest per step at every stage; `odin3-flow --prov` writes them for the VTR flow too.
  Manual: `docs/user/provenance-files.md`. The C ABI stays 7.

### Added

- `fsm` provenance: the new register and decoders, the logic into each state (the case items that set it) and each control
  output carry their own records instead of one derived from every folded cell, so a LUT below traces to the part of the
  machine it implements.
- `scoap --prov`: every net row that has a wire names it (`wire`) with the bit (`bit`), also when the wire is only an alias
  of the net or the net is a bit of a port wire (output ports and registers after mapping); on ch_intrinsics the viewer found
  a wire on 107 of 319 rows.
- The version 2 name map writes `uid=<store ID>` on `R` lines when `ODIN3_PROV_UID=1` is set. The `r<n>` numbers count the
  records of one file, so two maps from one run disagree on them; the store ID is the same in both, so a record can be
  followed from the proc dump to the final one. Off by default (a restored design numbers its store again). New CTest
  `lineage` (`tests/lineage/case_fsm.v`): the LUTs of a case, of an independent statement and of a state machine trace to
  their own source lines.
- `abc --prov-edges N|unlimited` sets the parent-edge budget of the precise per-LUT provenance records; the default budget
  is now at least 4,194,304 edges (16 MiB of parent IDs; peak memory on sha unchanged at 57 MB), where 16 x (cells + objects)
  made sha (725,231 edges) share one record for all its LUTs. The WARNING names the option.
- `write_json --net-ids` adds `odin3_nets`, the Odin III net ID of each bit, to every netname (the "bits" codes are not net
  IDs); `read_json` accepts it. Additive.
- `proc` provenance: the compare cells of a case item ($eq, $reduce_or) carry the record [switch, case item, and the statement
  or signal of the snippet], and a $mux/$pmux the cases that give it a slice, instead of one record derived from every case
  of the switch. A mux traces only to the items that assign its signal.
- `tristate` pass (post-1.4; `docs/PASSES.md`): tri-state nets that reach no port become logic and the inout ports of the
  top split into `p_i`/`p_o`/`p_oe`, a WARNING for each; elaboration keeps a `$tribuf` on a port of a module below the top
  instead of the D3 error, for the pass to resolve after `flatten`. `write_blif` leaves out the shell of a split port.
- `sim_tb`: a DUT instance may have several independent clock inputs (up to 16; each clock port is observed on its
  own and its edges reach only the cells it clocks; new `odin3_sim_clock_edges` in `src/sim/sim.h`, an
  internal simulator call, not the public ABI). More than 16 is an error naming the extra port. A clock bit in a multi-bit DUT port, and a cell with no clock pin
  (a global-clock `$_FF_` such as a BLIF `.latch` with no control) in a DUT with two or more clock inputs, are
  located errors (the cell would otherwise never be clocked).
- `sim_tb`: read-only hierarchical references into a DUT, `dut.u_regs.r` and `dut.u.v[3]` (literal indices), resolved
  through the DUT's signal table; the DUT instance may be declared after the use. A write (including `$readmemh` into a path), a variable
  index, a part-select and a memory word (no names after `lower`) are located errors, reported at the line of the read
  in the testbench file. Spec: `docs/specs/2026-10-31-testbench-design.md` 7a, 7b.

### Fixed

- `lower` of two chained `$tribuf` cells refused only one of the two orders: lowering the first hid the chain from the second. The chain is now found before any cell of the module is lowered (an allocation failure there is logged as such). A constant z enable (`$_CONSTZ_`) is refused like 0 and x.

## 1.3.0 (2026-11-02)

**C ABI stays 7**: the public header changed since 1.2.0 only by additive exports (`odin3_edit_*`,
`odin3_simulation_set_toggle_counting`, `_get_toggles`, `_run_vectors`, `odin3_graphlets_count`), which need no bump
(repo precedent). Version strings are 1.3.0.

### Added

- `lower` of `$tribuf` (the tinyriscv gpio `inout`). When the `$tribuf` is the sole driver of its output bits the
  enable is dropped and the output is driven by the data input, with one WARNING per cell. This is the case Yosys
  `opt_expr -mux_undef` collapses before Parmys maps anything (Parmys maps `$tribuf` itself to SKIP); the function
  differs while the enable is low (the output follows the data instead of z), and on an inout port of the top the
  internal read sees the data, not the external pin. A bus (several drivers on one net), an inout port of a module
  below the top, a constant 0 or x enable and a `$tribuf` fed by a `$tribuf` stay a located error (`lower` changes
  nothing). `tools/lower-check/lower_sim.py` excludes `$tribuf`, so the cells are not compared.
  Spec: `docs/specs/2026-10-14-3B-lower-design.md` (§4 text, rule table, §9 ERROR and WARN tables, decision 11).
- `write_blif` and `sim` take an inout port of the top module that every bit has a driver (besides the port's own
  pin) as an output; any other inout is refused as before. Child modules keep the refusal in both.
- `sim_tb`: up to 4096 named port connections on a DUT instance (was 16); `%t` in `$display`/`$write` prints the
  time in the precision of the testbench's `` `timescale `` (units come from that timescale; without one, 1 ns); a
  value that does not fit 64 bits in that precision is a run error.

### Fixed

- Hierarchical references into another module (`u_tinyriscv.u_regs.regs[26]` in tinyriscv_soc_top.v) are no
  longer an error: as Yosys does, the dotted path is an undriven net of that name (a bit select past its one bit
  is x), with one WARNING per reference (a write to one drives that local net only). The same holds in a port
  connection (`leaf v(.a(u.w), ...)`; Yosys accepts it too), for a first component that is a function (`f.x`) and
  for the module's own name before an undeclared name (`top.nope`); the module's own name before a declared name
  is popped (`top.a` is `a`, as Yosys). A net declared with the escaped name (`wire \u.w ;`) is used, not a second
  net. After `flatten` the foreign net keeps the name `u.w` and a child wire of that name becomes `u.w_1`; the
  foreign net stays undriven and unconnected (as Yosys). Generate-block and named-block references resolve as
  before. Still errors: an index on a path that names no block (`u[0].w`; Yosys makes a wire), a call (`u.f(1)`),
  a parameter override (`#(.P(u.w))`), a constant expression (localparam, range bound, generate condition), a
  path through a generate block that lacks the member (`g.sub.x`; Yosys makes a wire) and any reference under
  `default_nettype none`, tri0, tri1 or trireg. Notes: `docs/notes/2026-11-02-hierarchical-references.md`.
- `loc-check`: the plain BLIF listing leaves out the modules flatten left unused, which plain `write_blif`
  no longer writes (#318). The `loc_micros_*` and `loc_vtr14_*` gate and LUT sweeps failed on those modules, and
  the totals of the designs that did compare looked like improvements; with the fix every pin holds in Release.

- Processor netlists for Seeker-Proc (Leman Russ III round 7). `flatten` marks the modules it leaves unused
  (module attribute `odin3 flattened away`, not printed by the other writers) and plain `write_blif` no longer writes them (Yosys deletes them): the 28
  `.model` blocks after `proc; opt; flatten; opt; lower` were these 27 leftovers, and ABC cannot resolve their
  `.subckt` boxes. Plain `write_blif` writes `?` for `=` in model and `.subckt` type names, as Yosys does, so a
  hierarchical netlist with specialised modules (`m$P=V`) is readable by ABC (`#`, `<` and `>` too; the model and
  the `.subckt` of a black box get the same name). The INFO line names the modules not written. A system function or hierarchical
  reference in a dead generate branch is no longer an error at the module scan (Yosys reads only the branches
  the parameters select; a live branch still errors), which lets `servile_mux.v` read. A module that is never
  elaborated (no instance reaches it) therefore no longer errors on such calls in its generate bodies, as in Yosys.
  A second `flatten` with another top removes the mark from the modules that are roots then. 2026-11-01.

- Leman Russ III round 6 gaps. `write_blif --reachable` (plain dialect) writes the top and the modules reachable from
  it and skips the rest with a WARNING, as Parmys writes only the top's netlist; without it every module is
  written, as before, so multi-model BLIF still round-trips (one WARNING counts the unreachable modules written;
  `--reachable` with no top is ignored, with a WARNING) (`flatten` leaves the flattened modules in the
  design; `--vtr` already writes the top alone). `scoap --sigprob` no longer fails when
  the simulator refuses the netlist (for example a loop through a transparent latch, as boundtop's inferred
  latches give) or a net with two drivers: the scoap values are still computed, a WARNING names the refusal
  (the simulator's own line is downgraded to a WARNING for these two refusals; any other simulator failure still
  fails the pass), `p1_ppm` is -1 on every net, and the JSON (schema version 3) says `"sigprob_ran": false`,
  `"sigprob_vectors": 0` and `"rare": null`.
  Python `Odin3Error` can be pickled, so a `multiprocessing` pool no longer hangs on one. 2026-11-01.

### Added

- `sim_tb` and `run_testbenches` take `--plusarg name=value` (repeatable); the testbench subset gains
  `$test$plusargs` and `$value$plusargs` (a value is decimal, hex, binary or octal digits with an optional `-`; no
  whitespace, `+` or `0x` prefix; a value that overflows 64 bits or an unknown conversion such as `%c` is not found;
  at most 16 plusargs of 255 bytes). 2026-11-01.

### Documented

- `opt` latch fold. A latch whose D is constant (read in as `.latch` with a constant data net) is treated as an asynchronous load of that constant (Yosys `opt_dff`): with INIT unknown (2 or 3, the default) `opt` folds it to the constant (D = 1, INIT 3 gives constant 1), with INIT 0 or 1 it stays a latch. `-keepdc` keeps the latch when INIT is unknown (verified: `opt -keepdc` leaves `.latch one q al en 3`); it does not matter for a defined INIT. 2026-11-01.

### Added

- `$readmemh`/`$readmemb` take any constant string expression as the file name (a string parameter, a
  localparam, a concatenation), as Yosys does. `tests/mcu/` runs the Arduino-on-RISC-V microcontroller
  (`docs/notes/2026-10-31-mcu-testbench.md`): iverilog and `sim_tb` agree cycle for cycle on the ports, RTL and
  BLIF netlist. 2026-10-31.
- Netlist-edit gaps (Leman Russ III round 5). Edit events (added, param, rewired, removed) per node and net,
  exported on request: `write_json --odin3` member `odin3_edit`, `write_blif --edit-tags` line
  `.attr odin3_edit KIND:TAG,...` (plain dialect); `read_json` and `read_blif` restore them. `odin3_edit_rewire` and
  `_disconnect` now mark the node whose pin moved (`_tagged` forms take a tag); `odin3_edit_get_events`.
  `odin3_edit_add_node` takes initial integer parameters (`odin3_edit_new` grew: size 56 to 80, old callers
  unaffected), so a flip-flop starts at INIT 0 without a twin. Python: `Design(top=...)`, `run_script(top=...)`,
  `run_pass(top=...)` set the requested top before `read_verilog`; `Module.add_node(params=)`,
  `Pin.rewire(tag=)`, `Pin.disconnect(tag=)`, `obj.edit_events`. Default BLIF, JSON, Verilog and RTLIL output is
  unchanged. Spec `docs/specs/2026-10-31-edit-gaps-design.md`, manual `docs/user/edit.md`. ABI stays 7. 2026-10-31.
  Behaviour change: the creation calls (`add_net`, `add_node`, `add_sop`, `set_cover`, `set_param_int`) now refuse
  a tag with a newline, as the tagged connection calls do. `odin3_edit_new` sizes other than 56 or at least the
  current size are refused, and a non-zero `n_params` outside `add_node`. The events attribute key is `odin3 edit`
  (a user attribute named `odin3_edit` no longer collides). Python top: the binding serialises runs with a lock
  and restores the previous top.
- `scoap` pass: SCOAP testability measures (CC0, CC1, CO; with `--sequential` SC0, SC1, SO) of every net, signal
  probability from random simulation (`--sigprob`), module attributes `odin3:scoap_*`, versioned JSON, Python
  `Design.scoap()`. Spec `docs/specs/2026-10-31-scoap-design.md`, manual `docs/user/scoap.md`. 2026-10-31.
- `odin3 --version` prints a second line `commit <sha>[-dirty]`; line 1 (`odin3 1.2.0 (ABI 7)`) is unchanged.
  The commit is recorded at CMake configure time (`cmake/odin3_commit.cmake`: `git -C`, else the stamp file
  `.odin3-commit` that `tools/release/make-tarball` writes, else `unknown`) so run records can name the Odin III
  commit (provenance completeness). New C API `odin3_version_commit()` and `odin3_version_commit_dirty()`
  (`include/odin3/odin3.h`; Python `Odin3.commit()`). Purely additive, so the ABI stays 7 (no removed or changed
  symbol or struct; callers built for ABI 7 run unchanged). 2026-10-29.

### Fixed

- `sim_tb`: an unsized testbench `parameter N = 3` was one bit wide. The simulator accepts a clock that passes
  through an identity buffer (a BLIF `.names i o / 1 1`). 2026-10-31.
- `odin3-flow` runs `odin3` with the working directory set to the common parent directory of the design
  files and passes each design relative to it, so hash names, the version 2 name map and the BLIF are
  identical for one design in any directory (the known gap of `docs/V1.2.0.md`). The run log (`odin3/odin3.log`)
  records `name_root=`. No change under `src/` or `include/`. Tools only.
- Tests no longer leave temporary directories in `/tmp`.

## 1.2.0 (unreleased, tag on Peter's go)

Release notes: [docs/V1.2.0.md](docs/V1.2.0.md). Provenance as a two-way granularity DAG (ADR D14).
**C ABI 7** (6 in 1.1.1): plugins built for ABI 6 must be rebuilt. Version strings are 1.2.0.

### Added

- V1.2.0 Task 8 (`docs/V1.2.0.md` 6; #284): `odin3-prov-post FLOW_DIR` adds BLE, CLB and site provenance records
  and `A` lines to a version 2 map from VPR's `.net` and `.place`; `odin3-flow --keep-results` keeps those files.
  Tools only.
- V1.2.0 Task 5 (`docs/V1.2.0.md` 3.2; #281): the version 2 name map writers. `--prov-map [refs|constructs|full]` on
  `write_blif --vtr`, `write_json` and `write_verilog` write the lines `O N R C F T A` atomically with the netlist
  (`src/backends/common/prov_export.c`, `name_map.c`). ABI stays 7.
- V1.2.0 Task 2 (`docs/V1.2.0.md` 3.1; #276): the provenance operation-class table (`src/ir/opclass.{h,c}`,
  `docs/IR.md` IR-12a, `check` rule 13), `odin3_prov_begin_op_class`, and a lint rule that rejects a raw
  `odin3_prov_begin_op` outside the readers and `src/ir/`. No behaviour change in any output.
- V1.2.0 Task 3 (`docs/V1.2.0.md` 4.3, `docs/ADR/D14.md`): the construct table (`docs/IR.md` IR-12a),
  check rule 12, and ABI 7: `odin3_level` and `odin3_prov_dir` public, `odin3_design_construct_count`,
  `odin3_design_get_construct`, `odin3_obj_get_prov`, `odin3_prov_get_info`,
  `odin3_prov_visit_parents`, `odin3_prov_visit_joined`, `odin3_prov_visit_chain`; Python
  `provenance()` and `chain()` on every IR object. The table is empty until the elaborator fills it
  (V1.2.0 Task 4). Plugins built for ABI 6 must be rebuilt. No behaviour change in any pass or writer.
- V1.2.0 Task 4 (`docs/V1.2.0.md` 4.2, 4.3): the construct snapshot at the end of `read_verilog` stage 2
  (Verilog and SystemVerilog), `read_verilog --prov-ast refs|constructs|full|off` (default `off`: the
  cost gate failed on two designs, `docs/V1.2.0-task4-gate.txt`; the Python `Design` and the
  `ODIN3_PROV_AST` environment variable turn it on), `dump_locs --constructs`, `tools/construct-check`.
  ABI stays 7. Every writer output is byte-identical (the whole micro set, 539 designs).
- V1.2.0 Task 6 (`docs/V1.2.0.md` 3.5): `names --lookup` prints the records and chain of an object of a
  version 2 map, `names --chain` the chain, `names --check` the version 2 invariants; Python
  `odin3.read_name_map`. Version 1 output is unchanged. No core, IR or ABI change.

- V1.2.0 Task 11 (`docs/V1.2.0.md` 9): the provenance user docs. New page `docs/user/provenance.md` (what a
  record is, the levels and operation classes, the construct table, joins and their labels, the version 2 map
  line by line with the full flag list, `odin3-prov-post`, the `Q` and `K` lines with the pair classes and
  the recorded/inferred table, restore from a map with its stale marks and round-trip results, `names
  --lookup/--chain/--check`, the Python readers, a worked example and the limits). The flag list moved out of
  `docs/user/outputs.md`, which links to the page, as does `odin3-flow`. Every example is written by
  `docs/user/examples/provenance/make_examples.py` from the current build. Documentation only; no code,
  ABI or output change.

- V1.2.0 Task 10 (`docs/V1.2.0.md` 3.6): the `Q` and `K` lines. `odin3-prov-post` writes the N17 pair classes (shares by
  largest-remainder rounding), gain terms and pack-time criticality (with a Norns census), post-route `rho`, `slack` and
  near-critical flags and counts, and `basis` letters; the Odin III writers write an objective-only `Q` for every join
  (the `abc` objective is `area` or `depth` from the script's last mapping command, or the goal a `# objective:` comment
  states; it reaches the exporter through a memo in the abc pass, so the `abc` run information and the `T` row are
  unchanged); `odin3.read_name_map` and `tools/flow/name_map_v2.py` parse `Q` and `K`. `odin3-prov-post` takes the hard
  blocks' output pins from the architecture (`--arch`) or the `.net`, the OR word labels from a Norns
  `words/members.json` (`--words`), and the census `.net` SHA-256 from the `metrics.json` next to the census. ABI stays 7.
  Only `Q` lines differ in the writers' output.

- V1.2.0 Task 9 (`docs/V1.2.0.md` 4.5): `read_blif`, `read_json` and `read_verilog -netlist` restore a version 2
  `--name-map`: the pass runs, records (with their classes), construct rows and the `A`, `Q`, `K` lines of the file,
  bound to the objects the map names, with no restored-location record; the writers number the extra roots, mark the
  lines and post-Odin records `stale=t<n>` after a pass changed the netlist, and write the objective-only `Q` of a new
  join. IR addition (Peter reviews): the extra table with the mutating-run slot and the `restored` flag of a pass-run row
  (`docs/IR.md` IR-12a). ABI stays 7. A version 1 map reads as before.
- V1.2.0 Task 7 (`docs/V1.2.0.md` 6, 7.2, 8): `odin3-flow --prov` (hash-name BLIF plus the version 2
  name map), `--timing-legacy`, and `odin3-timing` reading the map (full span, construct chain, the
  `DECLARATOR` word); `tools/flow/name_map_v2.py`, a stdlib reader that agrees with
  `odin3.read_name_map`. `attribution.json` stays schema 1 (fields added only). No core, IR or ABI
  change.

### Changed

- **`odin3-flow --timing-attr` now implies `--prov`** (V1.2.0 Task 7): VPR reads the BLIF with hash
  names, so net names in VPR's own output files (`.net`, `.place`, `.route`, reports, logs) are hash
  names where they were short names, and no second provenance BLIF is written. CPD, LUTs, channel
  width and wirelength are unchanged. Use `--timing-legacy` for the version 1.1.1 behaviour (short
  names for VPR, a second BLIF `timing/prov.blif`, line-level spans, no chain).

### Fixed

- Hash names (and provenance names) no longer depend on the build. Two causes, found by comparing independent builds
  of one commit on the micros. (1) `read_verilog --vtr-compat` gave the soft body it reads from
  `lib/soft_primitives/<name>.v` the absolute path of the directory the build found (`<exe>/../../lib`, the install
  tree or `ODIN3_SOFT_PRIMITIVES_DIR`) as its file name, so every object of the body had a location, a full name and
  a hash name that depended on where the build lived (`adder_hard_block`, `hard_adder_cin_propagation`). The body is
  still read from that directory, but its file name is now `<odin3>/soft_primitives/<name>.v` (a form no design path has; `lib/soft_primitives/<name>.v` is a legal user path)
  (`src/frontends/verilog/read_verilog_lib.c`, `soft_source`). Test `cli_hash_name_soft_dir` (and `cli_hash_name_lib_path`: a user file at `lib/soft_primitives/<name>.v` gets other hash names). The `// file:line` comments `write_verilog` writes for library cells, and the `F` lines of the name map, carry the fixed name now (documented in `docs/user/outputs.md`). (2) The Debug-only
  full sweep in `opt_expr` `settle` (`src/passes/opt/expr.c`) re-ran the rules on cells a Release build never
  revisits; the `fine.group` and `fine.addsub` rules made the canonical constants 0 and 1 before knowing they would
  rewrite anything, and the sweep dropped them again, but IDs are never reused (IR-6), so a Debug build numbered the
  nets and nodes after `opt_expr` higher than a Release build (two per dropped constant) and every `$n<ID>` name
  built on them, hence the hash names, differed (522 of 1224 writer files on the micros, Debug against Release; the count is `python3 tools/flow/hash_name_compare.py build/debug/odin3 build/release/odin3`, which runs both on every micro in 9 writer/map variants and compares every output and name map byte for byte, and gives 1224 of 1224 identical after the fix). The
  rules (`src/passes/opt/expr_fine.c`) now make a constant only when they rewrite. Test `cli_hash_name_ids`. IDs, and
  so names, of objects made after such a rule change in every build (fewer IDs used). Goldens regenerated for that
  (IDs only): `tests/opt/rules/expr/fine.group.il`, `tests/tools/data/names_v2/{ansiportlist_2.blif,v1_ansiportlist_2}.txt`,
  `tests/tools/data/restore/roundtrip.txt` (the BLIF rows' `map_identical` count rises). ABI stays 7.
- The source tarball (`odin3-<ver>.tar.gz`) now configures, builds, passes `ctest` and installs
  without git: `tools/release/make-tarball` writes `third_party/abc/.odin3-commit` and
  `third_party/unity/.odin3-commit` (the submodule HEADs; generated, not committed) and
  `cmake/abc_pin.cmake` reads the ABC stamp when there is no `.git`, still failing on a mismatch
  with the pin. `tools/release/clean-check` gains tarball stages. This lifts the known limit in
  the 1.1.0 and 1.1.1 release notes (`docs/V1.1.1.md`, kept as written).

## [1.1.1] - 2026-10-25

Release notes: [docs/V1.1.1.md](docs/V1.1.1.md). Tools and Python only; the C core, IR and ABI (6) are
unchanged since 1.1.0.

### Added

- Critical-path attribution to HDL (v1.1.1, `docs/V1.1.1.md`, `docs/user/timing-attribution.md`):
  `odin3-flow --timing-attr [--timing-paths N]` joins VPR's setup timing report to the Odin III
  provenance and writes `timing/attribution.json` (schema_version 1) and `timing/summary.txt`
  (which statements, modules and words set the critical path and Fmax); `odin3-timing FLOW_DIR`
  does the same for an existing flow directory and `odin3.timing_attribution()` returns the data
  to Python. Tools and Python only: the core, IR and ABI are unchanged.

### Documentation

- D14 provenance granularity DAG: ADR `docs/ADR/D14.md` and the v1.2.0 spec `docs/V1.2.0.md`
  (design only, nothing built; #275).

## [1.1.0] - 2026-10-24

Release notes: [docs/V1.1.0.md](docs/V1.1.0.md). ADR D13; spec `docs/DESIGN.md` §4.2/§12.1.
The patch versions v1.0.1 to v1.0.5 were never released on their own; their changes ship here and
keep their version notes.

### Added

- SystemVerilog (v1.1.0, `docs/V1.1.0.md`; DESIGN §4.2/§12.1): slang v12.0 is vendored in
  `third_party/slang` with fmt, boost.regex and tomlplusplus (offline build), and `adapters/slang`
  is the C++20 shim that emits the elaborated Odin AST; `read_verilog` routes `.sv`/`.svh` and a
  project's SYSTEMVERILOG files to it, and the gate flow's `write_verilog` writes the mapped
  netlist as structural Verilog of library gate cells, which `read_verilog` reads back (an
  instance of a registered bit-granularity cell type binds to it).
- Simulation API (v1.0.3, ABI 5, `docs/specs/2026-10-21-s1-sim-api-design.md`): `odin3_simulation_*`
  in the C ABI and a `Simulation` class in the Python binding step a design one clock at a time;
  `sim --vectors-in` replays an input vector file and `sim --vcd` writes VCD waveforms.
- VerilogEval corpus test `cli_veval_corpus` and `tools/veval/veval-coverage`: 146 of the 156
  references go through the SystemVerilog front end and the cmos6 gate flow, 10 are refused with a
  pinned reason (#265).
- `unmap` pass (v1.0.4, `docs/specs/2026-10-21-unmap-design.md`): the inverse of `partial_map`; turns the
  VTR hard cells `adder`, `multiply`, `single_port_ram` and `dual_port_ram` back into `$add`/`$sub`,
  `$mul` and `$mem` with provenance, regrouping carry chains, split multipliers and sliced RAMs;
  `tools/unmap-check/unmap-vtr` is its VTR sweep.
- Gate mapping (v1.0.2, `docs/V1.0.2.md`): `abc --gates L` maps soft logic onto the gate cells of
  an `.o3lib` library read with `read_techlib`, `dfflibmap L` maps bit flip-flops and latches onto
  its state cells, and `write_blif --gates-as-names` writes gate cells as `.names` and library
  state cells as `.latch` so `tools/equiv-check` compares the result with the RTL.
- `lib/cmos6.o3lib` (BUF, INV, NAND2, NOR2, DFF, DFFSR with the areas of Yosys's `cmos_cells.lib`)
  and the default gate script `lib/abc/gates.abc`.
- `tools/gatemap-check/gatemap-run` and the sweeps `gatemap_micro_generic` and
  `gatemap_micro_cmos6`: every micro through the gate flow, equivalent to the RTL, with a re-map leg.
- `submod -bus BUSKEY`: vector ports from `NAME[BIT]` net attributes (v1.0.5, `docs/V1.0.5.md`).
- `cone_check` and `raise` take `-const PORT=V` (a template input port tied to a constant;
  v1.0.5, `docs/V1.0.5.md`).

### Changed

- ABI 6 (v1.1.0): `odin3_sv_reader_register` and `odin3_sv_read_fn`, the seam the CLI uses to make
  the slang adapter the read_verilog pass's SystemVerilog reader. Version is 1.1.0.
- ABI 5: the ABC engine table gains load_genlib and get_cells (gate mapping); ODIN3_ABC_ENGINE_VERSION 2.
  No API stability promise (docs/PHASE7.md decision 4).

### Fixed

- `odin3_flow` retries the VPR route (v1.0.1) at fixed wider channel widths when the final route at 1.3 x
  the minimum width fails after a finished minimum-width search (tiny devices such as
  `bm_DL_2_4_encoder` on k6); `summary.json` gets `route_retry`.
- `read_verilog --vtr-compat` reads `lib/soft_primitives/<name>.v` for an instance whose `#()`
  does not fit the registered hard cell of a VTR primitive (`adder #(W)` on the 1-bit `adder`),
  so the micros `adder_hard_block` and `hard_adder_cin_propagation` parse; an instance that fits
  keeps the hard cell, with the existing info line (`#262`).
- The SystemVerilog adapter lowers compound assignments, a bit-select target included
  (`q_next[21] ^= q[0];`), and names the kind in its unsupported-expression diagnostic instead
  of printing the enum number; VerilogEval Prob082/086 pass (`#264`).

## [1.0.0] - 2026-10-21

First release. Source tarball with submodules, `cmake --install`, and a pip wheel
(see `docs/RELEASE-NOTES-1.0.md`).

### Phase 0: environment and gate

- Repository skeleton, oracle tooling (Yosys 0.55 + Parmys from VTR `3c9a4d23`), goldens repo,
  lint gate (clang-format, clang-tidy, cppcheck, lizard, ruff, mypy) and CI (`docs/PHASE0.md`).

### Phase 1: core IR and netlist round trip

- Hierarchical, provenance-tracked IR with typed IDs, paged storage and an op registry; `check`
  pass; C ABI; generic and VTR technology libraries (`.o3lib`).
- BLIF read and write (round trip identical on the goldens), dot, JSON and Verilog writers.
- Bit-level simulator matching ABC on the goldens; a Python plugin walks the IR through the C ABI.

### Phase 2: Verilog-2005 front end

- Project readers (`-f`, Quartus, `.o3proj`, Odin II XML), preprocessor, Flex/Bison parser (#36,
  #44, #49).
- Two-stage elaboration to word-level IR (#94), `proc` (#92), the `opt` family (#95) and the
  `--vtr-compat` mode. Micros and the VTR benchmark set are identical or equivalent to Parmys
  (#98, #113, #114, #134, #135).

### Phase 3: lower, linked ABC, VTR flow

- `flatten` (#112), `wreduce` (#116), `hardmap` (#125, #128), `lower` (PHASE3 3B).
- ABC linked as a library with an in-memory exchange and a script checker (PHASE3 3D; in-memory exchange #148); the
  `$lut` cell.
- `write_blif --vtr` that VPR accepts, with provenance names (#144); the QoR harness, `fcheck`
  and the VTR-19 QoR table (#63, #68, #126, #140). Parmys-default behaviour with warnings.

### Phase 4: VTR architectures and FSM

- `read_arch` imports VPR architectures (#156); generic hard blocks by port signature (#164);
  multiplier splitting (#189); memory splitting, merge and read-enable emulation (#162, #179);
  carry chains from the arch (#174); FSM detect and one-hot recoding (#165).
- Dropped from v1.0: FF feature matching (4.6, to a later plugin).

### Phase 5: outputs and read-back

- Altera LPM structural Verilog target, `--target lpm` (#154); `read_verilog -icells` read-back at
  every level (#155); Yosys JSON reader (#173, #175, #182, #183); HDL locations survive write and
  read, hashed names with a name map (#180, #192, #200).

### Phase 6: analysis flow and provenance

- Gate-level netlist ingest (#195); `write_dot --color-by` (#190); graph export with versioned
  Odin III JSON keys and a Python loader (#197); `levelize` (#191); provenance coverage gate
  (#193, #205, #207, #209, #215).

### Phase 7: release

- `tools/flow/odin3_flow.py` and `odin3-flow` (targets `vtr` and `lpm`, `--analysis`) (#214).
- User manual in Sphinx and MyST, built with `-W` in CI (#219).
- Reverse-engineering support: `submod` (#216), `odin3_design_copy` and `Design.copy()` (#217),
  `cone_check` (#220), `raise` (#222), `plugins/python/re_round.py`, ABI fitness review and
  `docs/RE-ARCHITECTURE.md` (#223).
- Version 1.0.0, `cmake --install`, source tarball with submodules (`tools/release/make-tarball`)
  and the pip wheel (#221). Clean-machine test `tools/release/clean-check` (CTest `release_clean`).

### Removed

- CIRCT writer (D12) and EDIF and VQM writers (D10) are not part of v1.0.
