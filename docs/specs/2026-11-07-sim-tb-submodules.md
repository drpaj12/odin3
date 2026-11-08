# Design note: testbench-side submodules for sim_tb (2026-11-07)

Status: design, no code. Brief: `work/controller-scripts/briefs/odin3-sim-tb-features.md` Part 2. Purpose: Bifrost
activity and simulator parity need Odin III to run each design's own testbench (Odin III S1, Bifrost B Q1.2).
Measure: Mimir `opencores-tb` designs whose testbench runs to the end under `sim_tb` with output text equal to
iverilog 12 (INFO lines excluded). Targets after this work: simple_spi first, then vga_lcd, usbhostslave, uart2spi,
sdram_16bit.

## 1. Problem

A real testbench instantiates behavioural helper modules next to the DUT: a Wishbone master model with tasks
(`u0.wb_write(...)`), a SPI slave model, a flash model, a bus monitor. They use `#` delays, `<= #1`, tasks, `force`,
x literals, `$display`, and are not synthesizable. Today `sim_tb` treats every instance in the testbench as a DUT: it
looks the module up in the design and simulates it with the cycle simulator (`odin3_sim`), which ignores delays and
tasks. A tb-side module is therefore refused (`wb_master_model #(8, 32) u0 (...)`: "parameter override on a DUT
instance") or, with no parameters, is looked up in the design and not found. Other modules of the testbench files are
skipped at parse time (only the `--top` module is parsed).

## 2. Decision: flatten into the testbench interpreter

Each tb-side instance is expanded at parse time into the single testbench module the interpreter already runs: the
body of the instantiated module is parsed again, in a name scope with the instance's prefix, and its items
(variables, `assign`, `initial`, `always`, tasks, nested instances) are added to the one `tb_module`. There is no new
run-time object, no second scheduler, and no change to the evaluation engine for the plain case. The alternatives
were rejected:

- Run the helper through the DUT path (`proc; opt; lower`, `odin3_sim`). Rejected: the models are behavioural
  (delays, tasks, `<= #n`, x literals, `$display`, `force`); the cycle simulator ignores all of them.
- A second interpreter instance per module with a port-exchange protocol. Rejected: two schedulers need a
  co-simulation protocol and still break the iverilog event order between a model and the bench; flattening keeps one
  event queue, which is what iverilog does.

## 3. Classification: tb-side or DUT

A module instantiated in the testbench is tb-side when its `module` header is in the testbench token stream (the
files given to `sim_tb`, after the preprocessor) and no module of that name is in the design. Otherwise it is a DUT as
today. A name in both places is a DUT (the design wins) and an INFO line says the testbench copy is ignored. A name in
neither place keeps today's error ("no module `m` in the design"). No new option: the file list is the user's choice
(`sim_tb tb.v wb_master_model.v spi_slave_model.v`); a helper file passed with `--top` naming only the bench works as
before when the bench has no tb-side instance.

A module index is built once after lexing: `module <name>` token position to the matching `endmodule` position, so
a module can be parsed from any place, and a bench of several files may order its modules freely.

## 4. Mechanism

The parser gets a stack of instance frames, no recursion (the repository rule), and a current scope prefix.

1. At an instance item whose module is tb-side, the parent-scope parts are parsed first: `#(...)` parameter
   overrides (positional or named) and the port connections (named, or ordered: the header of a tb-side module is
   known, so ordered connections are accepted here, unlike for a DUT). Overrides are constant-evaluated in the parent
   scope with the existing `tb_const`.
2. A frame is pushed: the prefix (`u0.`, `u0.u1.` when nested), the return token position, the overrides, the
   connection list. `pr->pos` jumps to the module's header and the same item loop parses the body, then the frame is
   popped at `endmodule` and parsing continues after the instance statement. Frame depth is capped (16): a module
   that instantiates itself, directly or not, gives `unsupported: recursive module instance`.
3. Names. A variable, task or parameter declared in the body is added as `<prefix><name>` (`u0.adr`). A lookup in the
   body resolves `<prefix><name>` only: module scoping, no fallback to the parent, so a helper's `clk` never captures
   the bench's `clk`. `TB_NAME` (64) bounds a full name; a longer one is an error naming the instance.
4. Parameters. `parameter` and `localparam` in the body go through the existing declaration code; for `parameter`, an
   override by name or by position (declaration order) replaces the default. The ANSI header form
   `module m #(parameter a = 1)(...)` is accepted. A parameter is a constant of the scope, so ranges such as
   `[awidth:1]` and `[dwidth/8:1]` evaluate as today. `defparam` stays refused here (its own PR).
5. Ports. The header (non-ANSI `module m(clk, rst, ...)` with `input/output/inout` items in the body, or ANSI
   `input wire csn`) declares the ports. A port is a variable of the scope; an `output reg x` or an `output x;`
   followed by `reg x;` is one variable (the later declaration refines the first). After the body is parsed, each
   connection becomes a continuous assignment: input port `assign <prefix>port = <parent expr>`, output port
   `assign <parent lvalue> = <prefix>port` (the parent expression must be a whole wire or reg, as for a DUT output),
   so a wire with several drivers uses the existing resolution (`tb_drv`). An empty connection `.sel()` leaves the
   port undriven (z on a wire, x on a reg); a constant connection (`.err(1'b0)`) is an input assign. `inout` ports
   are refused (`unsupported: inout port of a testbench module`) until a design needs one.
6. Items. Processes (`initial`, `always`) of the instance are appended at the position of the instance item in the
   source order, which fixes the order the scheduler runs them in; iverilog orders them the same way for the cases
   the existing `tb race:` warning covers. The assignment `wire debug = 1'b1;` (net declaration with a value) is
   supported for a tb-side module (a driver on the new wire). `specify`, `generate`, `function` are refused with the
   same message as in the bench (functions and `generate` have their own PRs).
7. Hierarchical names inside the testbench. In an expression or statement, `a.b.c` whose first part is a tb-side
   instance of the current scope resolves to the variable `<prefix>a.b.c` (arrays: `u0.mem[3]`, with a variable index
   through the existing array code), exactly like a local name; a task call `u0.wb_write(0, a, d)` links to the task
   `<prefix>u0.wb_write`. A first part that is a DUT instance goes through the DUT code (and the new DUT array reads)
   as now. Resolution happens in the second parse pass, where every instance is known.
8. `%m` in `$display` prints the scope path (`tst_bench_top.u0`); today it prints the bench module name.
9. Timescale. A flattened module takes the bench's timescale; a module with another `` `timescale `` is a later PR
   (mixed timescales, with `real`).

## 5. Interaction with what exists

- VCD (`--vcd`): flattened scalars of an instance are dumped under a nested `$scope module <instance>`; arrays stay
  left out, as for the bench. The prefix splits at the dots.
- 4-state and 2-state: nothing new; the new items are ordinary variables.
- DUT detection (`tb_inst`) is unchanged for DUT instances. A tb-side instance never creates a `tb_inst`.
- Errors keep the file and line of the helper's own source (the location map already maps joined lines), so an error
  in `wb_master_model.v:120` says that.

## 6. Errors (every refusal is a located `unsupported:`)

recursive module instance; `inout` port of a testbench module; instance array (`m u[3:0] (...)`); a connection to a
port the module does not have (`module m has no port p`); more connections than ports; a parameter override that is not
a constant; `defparam`; a name longer than 63 characters; a module that is both tb-side and not defined (neither in the
design nor in the files).

## 7. Tests

Micros in `tests/tb/micro/sub*_tb.v` with a `_lib.v` file holding the helper (the runner already passes `<n>_lib.v` as
a second testbench file), expected output from iverilog: positional and named overrides; a port list in both header
styles; two instances of one module with their own state; a nested instance; `u0.x` read and `u0.task(args)` call;
`output reg`; a helper `clk` next to the bench `clk`; an ordered connection; an empty connection; a module in both the
design and the files (the DUT wins); the refusals of section 6 under `tests/tb/errors`.

## 8. Plan and estimate

| PR | content | lines |
|---|---|---|
| A | module index; frames and prefix; parameters (override); ports and connections as assigns; processes and variables of the instance | 700-900 C |
| B | hierarchical names and task calls into instances, nested instances, `%m`, VCD scopes | 400-600 C |
| C | simple_spi's other constructs, one PR each, survey rerun after each merge: `@(a, posedge b)` and `@*`, `force`/`release` | 150-300 C each |

After simple_spi: `real` with timescale-precision ticks and mixed timescales (vga_lcd, uart2spi, sdram_16bit);
`fork`/`join`, events, `disable`, functions (uart2spi's flash model); `defparam` and macros leaking from RTL files
(usbhostslave). Everything iverilog itself refuses is skipped.

## 9. Questions for the controller

1. Classification (section 3): tb-side when the module is in the testbench files and not in the design. Accept?
2. Ports as continuous assigns (section 4.5) change the order of same-time updates by at most the active-region
   iterations the bench already has; a bench that depends on the exact delta order of a port may differ from iverilog.
   Accept, with the `tb race:` warning as the guard?
3. The processes of an instance are appended at the instance item (section 4.6). Accept?
4. Ordered port connections allowed for tb-side modules only. Accept?
