# Testbench simulation

`sim_tb` runs a Verilog testbench against designs held by Odin III. The testbench is interpreted
(a small Verilog subset); the design under test (DUT) is simulated by the existing cycle simulator.
`testbench_gen` writes a testbench for a design that has none; `tools/tb-compare` runs one
testbench on two designs and reports the first mismatch. The full specification is
`docs/specs/2026-10-31-testbench-design.md`.

## Running a testbench

```
odin3 -p "read_verilog dut.v; proc; opt; lower; sim_tb --out run.txt tb.v"
```

For a netlist DUT use `read_blif dut.blif` (or `read_json`) instead of the first four commands.
Values have four states (0, 1, x, z) as in iverilog: regs, integers and memories start x, wires z,
and the DUT's registers are x until reset. When a testbench prints the DUT before or around reset,
read it with `lower --keep-sel` (`read_verilog dut.v; proc; opt; lower --keep-sel; sim_tb ...`):
an `if` or `case` with an x select then takes its else or default branch as in the RTL, where plain
`lower` turns the select into gates that propagate x.
A testbench that loads or reads a memory of the DUT by hierarchical path (`$readmemh("rom.hex", uut.imem.rom);`,
`uut.dmem.ram[i]`) needs the memory kept: `lower --keep-mem` gives every word of every `$mem` a wire named
`<memory>[<index>]` (`rom[3]`) over its storage, and makes a ROM (a memory nothing writes) real storage that starts x
(it is a constant otherwise). See "DUT memories by hierarchical path" below.
Options: `--top <module>` (the testbench module, default the first), `--max-time N` (default
10,000,000 time units), `--seed N` (the seed of `$random`), `--out <file>` (the `$display` text;
default stdout), `--vcd <file>` (a waveform, below), `--2state` (the old 2-state
interpreter: every value starts 0 and the DUT is simulated with 0 and 1 only; the default, since
2026-11-07, is 4-state: x and z values, regs start x, the DUT is a 4-state simulator, as iverilog,
best with `lower --keep-sel`; `--4state` is accepted and changes nothing). A register with no reset whose hold or enable is computed from its own value (`HLT ? X : next` with `HLT` depending on `X`) stays x in 4-state mode, as in iverilog, and the old 2-state mode (Odin III 1.4.0 and earlier) hides it as 0; compare two traces in one mode, and treat x as a don't-care or reset that state first. Python: `design.run("sim_tb --out run.txt tb.v")`
after the design is built.

The time limit and progress. A run ends at `$finish`, or when the next event lies beyond `--max-time`. When the limit ends
it, `sim_tb` prints a WARNING that names the limit and `--max-time`, then the summary line `sim_tb: stopped at time T (time
limit)` (a run that reaches `$finish` prints `sim_tb: finished at time T`). A testbench with a free-running clock and no
`$finish` always ends that way; the warning is then expected and `--max-time` is the length of the run. The default stays
10,000,000: `run_testbenches` shares it as its hang limit, and a longer default would make a hung testbench slower to
report. `--progress SEC` prints an INFO line every SEC seconds of wall time, `sim_tb: progress: time 2844569 of 3000000
(94.8% of --max-time), 2844569 time steps, 1 s, 2844569 time units/s`, so a long run can be told from a hang; it is off by
default and prints nothing for a run shorter than SEC seconds.

Several files and directives: `sim_tb a.v b.v ...` joins the files (after the Verilog preprocessor,
run once over all of them, so a `` `define`` reaches the files after it). `` `include`` is searched
in the directories given by `-I <dir>` (repeatable), and `-D name[=value]` (repeatable) predefines a
macro. `--top` picks the testbench module among the joined files; the other modules are skipped.
`` `timescale`` is read; `` `default_nettype``, `` `celldefine`` and similar are ignored. Messages
give line numbers of the joined, preprocessed text of the first file's name.

Tasks: `task name; input/output/inout ...; begin ... end endtask` (and the ANSI form `task name(input [7:0] a, output b);`) can be declared and called as statements, also with delays inside. Arguments are static variables of the testbench: inputs are copied in at the call and outputs copied back when the task ends; there is no recursion or `automatic` storage, and no `function`.

Plusargs: `--plusarg name=value` (repeatable, up to 16; a leading `+` is dropped) is read by
`$test$plusargs("name")` (1 when a plusarg starts with the text) and `$value$plusargs("name=%d", var)`
(conversions `d`, `h`/`x`, `b`, `o`, `s`; the call is 1 and `var` is set when a plusarg starts with the text
before the `%` and its rest parses in that conversion, else 0 and `var` is unchanged). A `%s` value keeps its
first 8 bytes. `run_testbenches` takes `--plusarg`
too. A file name from a variable (`$readmemh(firmware_file, ...)`) is not supported.

A DUT whose RTL writes `3'bxxx` as a "don't care" case item (mips_16's ALU) simulates differently from its netlist
unless `read_verilog --case-xz-never` is used (`flow.md`): the item then matches nothing, as in iverilog. Flow for such a
bench: `read_verilog --case-xz-never ...; proc; lower --keep-sel --keep-mem; sim_tb ...`.

## Memories as cells: `lower --keep-mem-cells`

```
odin3 -p "read_verilog cpu.v; proc; opt; lower --keep-mem-cells; sim_tb tb.v"
```

`lower` normally turns every `$mem` into one flip-flop per stored bit plus read multiplexers, which is why a design
with large RAMs or ROMs simulates slowly (a 2,048-word memory is 16,384 flip-flops). With `--keep-mem-cells` the
`$mem` stays a cell and `sim_tb` (and `sim`) simulate it directly: one byte of state per stored bit, no gates. The
output is the same as through `lower`, line for line (`tests/tb/run_micros.sh` with `LOWER_EXTRA=--keep-mem-cells`,
`tests/tb/run_memx.sh`, and the memory cases of `test_lower_mem`); on the AI-digit layer `l_fc2` (an 84 x 120 weight ROM
and a 256-byte activation RAM, 30,395 cycles) it runs 13 times faster. The netlist still holds a `$mem`, so this is for
simulation: `abc`, `write_blif` and the other netlist-view passes need the default `lower`.

Supported: asynchronous and synchronous read ports (RD_EN, `RD_TRANSPARENT`), several write ports (the later port wins),
per-bit write enables, OFFSET, ABITS above or below log2(SIZE), `INIT`, either clock edge, and ports on different clock
nets or edges (below). The rules for x and z are those
of the lowered netlist: an address that reaches no word reads x (0 with `--2state`) and a write to it is lost; an
address with x or z bits reads x unless every word it may reach holds the same known value, and a write through it
makes the words it may reach x (iverilog drops that write); an x write enable makes the written bit x unless the data
equals the old value. A synchronous read with RD_EN x merges the new word with the old one bit by bit (the lowered
netlist registers the pieces of a transparent read separately, so the two can differ there).

Several clocks: every live write port and every synchronous read port runs on its own clock net and edge
(`WR_CLK`/`RD_CLK` per port, with `WR_CLK_POLARITY`/`RD_CLK_POLARITY`), so a dual-port RAM written on `wclk` and read on
`rclk`, or written on the rising and read on the falling edge, simulates as the RTL does. All the edges of one instant
(`odin3_sim_clock_edges`, a testbench that raises two clocks in one time step) see the memory as it was before the
instant, and then the writes are applied in write-port-index order, the later port winning a same-address conflict, as
for one clock. A transparent read port sees the write ports on its own clock net and edge only (what `lower` builds). Two
differences from `lower`: write ports on different clocks simulate (`lower` refuses them, so there is no gate-level
reference; `tests/tb/run_memclk.sh` compares them with iverilog), and the cell state is one shared array. Limit: 16
different (clock net, edge) pairs per memory.

Refused, with a located error `cannot simulate `$mem` (node ...)`: an asynchronous write port (a write with no clock has
no event to apply it on; `lower` refuses it too) and a memory over 4 GiB of state. A `$mem` with no read port is ignored.

`$readmemh` / `$readmemb` into a DUT memory works as with `--keep-mem` (above) and needs no extra flag: the words of a
`$mem` are named `<memory>[<index>]`, the index being the declared one (OFFSET added). `--keep-mem` is not needed with
`--keep-mem-cells`; a ROM keeps its `INIT` contents and starts x without one.

## DUT memories by hierarchical path

```
odin3 -p "read_verilog cpu.v; proc; lower --keep-sel --keep-mem; sim_tb tb.v"
```

With `lower --keep-mem` (no `opt` first when a word may be a constant: `opt` folds a register that only ever
takes 0, such as register 0 of a register file, and removes a memory nothing reads) the testbench may:

- `$readmemh` / `$readmemb` into a DUT memory: `$readmemb("prog.bin", uut.imem.rom);` (optional start and end
  addresses as for a testbench array). The words are stored in the DUT's flip-flops (new internal function
  `odin3_sim_poke_state`, not part of the C ABI), the DUT settles at once and its outputs are published, so a read
  or a `$display` on the next line sees the loaded program.
- read a word with a variable index: `uut.dmem.ram[i]`, and a bit or part of it: `uut.rom[i][7:4]`,
  `uut.regs[3][0]`. An index out of the range, or x, reads x.

A memory the DUT no longer has by that name (no `--keep-mem`, or `opt` removed it) is a located error:
`DUT 'uut' has no word array 'imem.rom'`. A word that `opt` folded to a constant is an error at the read
(`a word of a DUT array has no name`), never a silent x. Not supported: assigning a DUT register or memory word from
the testbench (`uut.x = v`), `force` on a DUT signal.

## force and release

`force v = expr;` and `release v;` on a whole reg, integer or wire (also `u0.x`, a variable of a testbench-side
instance). A forced variable ignores procedural and continuous writes; `release` gives a wire the value of its drivers
back and leaves a reg at the forced value until it is assigned. As in iverilog, a force whose expression is a plain
variable follows it; any other expression is evaluated once, when the statement runs. No `force` of a select, a
concatenation or a DUT signal, and no `assign`/`deassign` statements.

## Testbench-side modules

A real testbench instantiates behavioural helpers next to the DUT (a bus master model, a SPI slave model). A module
whose header is in the testbench files and which the design does not contain is **testbench-side**: its instance is
flattened into the testbench at parse time (its variables, tasks and processes become `<instance>.<name>` items of the
one interpreter, one event queue). If the design has a module of the same name, the design wins (the DUT) and an INFO
line says the testbench copy is ignored. List the helper files after the bench: `sim_tb tb.v helper.v`.

Supported: `#(...)` parameter overrides (positional or `.name(value)`, constants), the header forms
`module m (a, b); input a; ...`, `module m (input wire a, output reg [7:0] b);` and `module m #(parameter W = 4) (...)`,
port connections named or ordered, an empty connection `.p()`, a constant or an expression on an input, `parameter`,
`localparam`, `reg`, `wire`, `integer`, arrays, `[h:l]` ranges with `l > 0` (whole-vector use only), `assign`,
`initial`, `always`, tasks (called from inside the module), `wire w = expr;`, nested instances (depth 16).
A port connected to a whole `reg` or `wire` of the parent with the same width is one net with it (as in iverilog: a clock
passed through a port reaches the helper's `@(posedge clk)` in the same delta as the bench). Other connections
(expressions, selects, constants, other widths) become continuous assignments. A name inside a module is its own: the
helper's `clk` is not the bench's `clk`.

Hierarchical names reach into instances: `u0.adr`, `u0.mem[i]` (read or write, any index expression), `u0.ia.v` (nested),
`u0.task(args)` as a statement, and the absolute form `tb.u0.x` from the testbench module; a name inside a helper
reaches its own scope and its own sub-instances only (no upward references). `$display("%m")` inside a helper prints
the scope path (`tb.u0`, with the task name inside a task). With `--vcd` each instance is a nested `$scope module`.
A DUT instance and a helper must not share a name. In the first parse pass a dotted name that starts with an instance
item written later in the file is taken for a helper instance (a DUT array read `uut.mem[i][3:0]` needs the instance
item before it).

Refused with `unsupported: <construct>` at the helper's own file and line: recursive module instance, `inout` port,
instance array, connection to a port the module does not have, more ordered connections than ports, an override of a
parameter the module does not have, a non-constant override, a name longer than 63 characters (instance path included),
a design module instantiated inside a testbench module, `defparam`, `function`, `generate`, `specify`. Tests:
`tests/tb/micro/sub_*_tb.v, xsub_*_tb.v` (with `sub_*_lib.v`), `tests/tb/errors/sub*_tb.v`.

## Waveform (`--vcd`)

`sim_tb --vcd wave.vcd tb.v` writes an IEEE 1364 VCD of the run. It holds every scalar `reg`,
`wire` and `integer` of the testbench (arrays and parameters are left out) under `$scope module
<testbench>`, and the ports of each DUT instance that the testbench connects, under a nested
`$scope module <instance>`. Values are 0, 1, x and z, vectors of any width. The `$timescale` is the testbench's
`` `timescale `` unit (`1ns` when it has none), and the time stamps are the testbench time units.
The file starts with a `#0` `$dumpvars` block, then one `#<time>` block for each time step in
which a dumped signal changed; a signal is written with the value it has when the step ends, so a
glitch inside one time step does not show. Open it with GTKWave. Odin III checks it against
iverilog (`$dumpvars(0, tb)` on the same testbench) with `tools/vcd-compare/vcd-compare`.

### Every net of the DUT (`--vcd-all`)

`sim_tb --vcd wave.vcd --vcd-all tb.v` adds, inside each DUT instance's scope, a nested `$scope module
<DUT module>` holding one 1-bit variable for every connected net of the design view the DUT was
simulated from, and a nested scope per instance below it. Run it after `abc --lut 6` and the
variables are the LUT, gate and flip-flop outputs of the netlist, each named as `write_blif --vtr`
names the net (a port bit `q[3]`, a generated name, a legalised name); the BLIF and the waveform of
one netlist then share names. On the RTL view (before `abc`) the variables are the nets of the IR with
their own names (`$n<ID>` for an unnamed one). Values are 0, 1, x and z, written at the end of each time step
as for the other variables. `--vcd-names short|provenance|hash` (only with `--vcd-all`; default
`short`) picks the naming style, the same as `write_blif --name-style`: use `hash` with a BLIF
written by `--name-style hash`. A module the writer cannot name (a refusal) logs a warning and keeps the IR's
net names. A design with N nets writes N variables, so a large design makes a large file; the file
grows with the number of nets that change, not with the run length alone.

## What is supported

One testbench module without ports per file, with `reg`/`wire`/`integer`/array declarations,
`parameter`, `assign`, `initial`, `always` (starting with `#`, `@` or `wait`), blocking and
nonblocking assignments, `#N`, `@(posedge|negedge|change ...)`, `wait`, `if`, `case`, `for`,
`while`, `repeat`, `forever`, `casez`, `casex`, x and z digits in numbers (`4'b1x0z`, `'bx`,
`?` in `casez` labels), `===` and `!==`, `**`, DUT instances with named port connections (up to 4096), `%t` in `$display`/`$write` (the time in
the `` `timescale `` precision, 20 columns), `$display`, `$write`,
`$strobe`, `$monitor`, `$fopen`, `$fdisplay`, `$fwrite`, `$fclose`, `$readmemh`, `$readmemb`,
`$finish`, `$stop`, `$time`, `$random`, `$signed`, `$unsigned`, `$clog2`. A DUT instance may have up to 16
independent clock inputs, and an expression may read a DUT signal by hierarchical path (`dut.u_regs.r`,
`dut.u.v[3]`, literal indices; read only; a memory word only after `lower --keep-mem`, see below; no part-selects of a DUT signal; a multi-bit DUT port may not carry a clock; with several clocks every register needs a clock pin, so a BLIF `.latch` needs a control). Values are at
most 1024 bits wide (signals, literals, concatenations, DUT ports, hierarchical reads). A wire
with several drivers (two `assign`s, an `assign` and a DUT output) takes the resolution of them: z
gives way, equal values stay, a 0 against a 1 or an x gives x. Anything else stops the run with
`tb.v:LINE: unsupported: <construct>`: real numbers, tasks and functions, generate, a parameter override on the DUT
instance that differs from the elaborated value (see "DUT parameter overrides"), ordered port connections, several modules in the testbench file, compiler directives
other than `` `timescale ``.

## DUT parameter overrides

The DUT was elaborated by `read_verilog` before `sim_tb` runs, so `sim_tb` cannot change its parameters; it checks the
testbench against them. A DUT instance may repeat the elaborated values: `dut #(.W(8), .N(3)) u (...)` and the ordered
`dut #(8, 3) u (...)` are accepted when each value equals the value the module was elaborated with, compared by value and
width-insensitively (`8`, `8'd8` and `32'd8` are the same; a string parameter compares as 8 bits per character). To use a
non-default value, give the same value to the read: `read_verilog -P W=12 -P N=9 dut.v; ...; sim_tb tb.v` with
`dut #(.W(12), .N(9)) u (...)` in the testbench (`-P NAME=VALUE` overrides a parameter of the top module, see `docs/user/inputs.md`;
a project's `param` lines do the same). A value that differs, a name the module does not have, more ordered values than
parameters, or a non-constant value stops the run with a located error, for example
`tb.v:8: unsupported: DUT instance u (module dut): parameter N is given 4 but the DUT was elaborated with 3; read the DUT
with 'read_verilog -P N=4'`. Elaboration records each parameter as the module attribute `odin3 param default:<NAME>`, holding
the value of the elaboration (not written by `write_json`, `write_verilog` or `write_rtlil`). A module without a
`#(parameter ...)` header list has its body `parameter` declarations counted, and its body `localparam`s with them (the
elaborated form keeps no difference). Tests: `tests/tb/micro/dutparams*_tb.v` (compared with iverilog; `dutparamp` reads the DUT
with `-P`, through `dutparamp.rflags`), `tests/tb/errors/dutparam*_tb.v`.

## What is not modelled

`$display` and other system tasks written inside the DUT's RTL do not run: `sim_tb` simulates the synthesized
DUT, so a bench that prints them under iverilog (mips_16 prints `ERROR: Unknown Instruction` at time 0) has those lines
only in the iverilog text. The DUT simulator is cycle-based. Gate or `#` delays in the DUT are not seen. A tri-state buffer
inside the DUT (`assign y = oe ? d : 'bz;`) is lowered to a plain driver of `d` by `lower` (as
Parmys does, with a warning), so its output is not z while `oe` is low; the testbench side of a
tri-state bus (several `assign`s) is resolved exactly. An x in the DUT after `opt` or `lower` may be
more pessimistic than iverilog's RTL (a `?:` or a lowered `if` merges both sides). The
DUT must have at most one clock input, a primary input of the DUT; its registers may be clocked by
nets derived from it (divided, gated or muxed clocks: `tests/tb/micro/dclk.v`, equal to iverilog), which
the simulator evaluates after each input change and each edge. Divide by zero gives x, as
in iverilog (it gave 0 before 2026-11-06).

Asynchronous resets: for a testbench that asserts and releases a reset between clock edges (or in one edge's time step), use `lower --async keep` (`proc; opt; lower --keep-sel --async keep`): the flip-flops keep their reset pin and the reset acts at once, as in iverilog (`tests/tb/micro/xasync`). Without it, `lower` makes them synchronous, as Parmys does (PHASE3 decision 19), because the
VTR architecture's flip-flop (`.latch`) has no set or reset pin. `sim_tb` runs the lowered netlist, so
a reset raised between clock edges takes effect at the next edge; iverilog applies it at once. On the
IR before `lower`, `sim` and `sim_tb` stop at the `$adff` cell. `testbench_gen` holds reset for four
clock cycles, so its testbenches agree with iverilog. `lower --async error` refuses an async reset
instead. This is a known simulator limit, also for a register on a derived clock with an
asynchronous reset: the `$adff` form of `tests/tb` is not simulated before `lower`, and after `lower` the
reset is seen at the next edge, so a design that resets between edges differs from iverilog (a counter
`always @(posedge clk or posedge rst)` with `rst` raised between edges shows its reset one clock late).

Known issue: the release build `build/release/odin3` cannot simulate the derived clock net `CLK` of darkriscv (the testbench run needs the main-branch binary, which can).

Other known limits of derived clocks. A clock that is constant 1 (`wire k = clk | 1'b1;`) never has an
edge in a 2-state simulator, while iverilog sees x become 1 at time 0 as a rising edge, so one extra
clocked update happens there only. A derived clock that never settles (a register that clocks itself,
for example a clock `clk ^ x` for a register `x` toggled by a rising edge of it) stops the run with the
error "a derived clock of the DUT does not settle" after 256 rounds; the `sim` and `simcheck` passes and
the simulation API (`step`, `settle`) report it too (ODIN3_ERR_CHECK). Large `case` statements over
registers (a memory held as registers) are faster with `proc --share-selects`; `proc` suggests it
in an info line when one case's select is made 64 times. A testbench cannot use x or z literals (they are refused with a located error).

## Generating a testbench

```
odin3 -p "read_verilog dut.v; proc; opt; lower; testbench_gen --out tb_dut.v --cycles 200 --seed 3"
```

Writes `tb_dut.v` and one `tb_dut.v.<input>.hex` stimulus file per data input (run from the same
directory). The clock is the input that drives an edge-triggered cell, else an input whose name
holds `clk` or `clock`. A reset is a 1-bit input named `rst`, `reset`, `arst`, `rstn`, `rst_n`,
`reset_n` and similar; names ending in `n`/`_n`/`b` are active low. The testbench holds reset for 4
clock cycles, then applies one vector per cycle at the falling clock edge and prints
`cycle out1 out2 ...` in hex 4 time units later. Ports wider than 64 bits are refused.

## Comparing two designs

```
tools/tb-compare/tb-compare --tb tb_dut.v --a "read_verilog golden.v; proc; opt; lower" \
    --b "read_blif synthesised.blif"
```

Exit 0 identical, 1 mismatch (the first differing line, its cycle and the differing fields), 2 error.

## Testbenches in the project

A project can declare its testbenches and how they run. In an `.o3proj` file:

```
file verilog counter.v
top counter
testbench good file tb_good.v top tb_good dut dut expect good.expect
testbench both file tb_good.v top tb_good on both expect good.expect
```

Syntax: `testbench <name> file <path> [top <module>] [dut <inst-or-module>] [max-time <T>]
[expect <path>] [on rtl|netlist|both]`. Paths are relative to the project file. `top` is the
testbench module (default the first in the file). `dut` checks that the testbench instantiates
that instance or module (an error otherwise). `max-time` is a count of testbench time units.
`expect` is a golden output file: the testbench passes only when its `$display` text equals it
line by line. `on` (default `rtl`) says which design the testbench drives: `rtl` is built from
the project itself (`read_project; read_verilog; proc; opt; lower`) and does not need a design in
hand; `netlist` is the design in hand when `run_testbenches` runs (read a BLIF or run synthesis
first); `both` runs the two and compares their output line by line.

A Quartus `.qsf` is imported too: `EDA_TEST_BENCH_NAME`, `_FILE`, `_MODULE_NAME`, `_RUN_SIM_FOR`
(converted to testbench time units with the file's `` `timescale ``), `EDA_DESIGN_INSTANCE_NAME`
and `EDA_NATIVELINK_SIMULATION_TEST_BENCH` map onto the same records. Anything else of the group
is a warning, never silently dropped.

```
odin3 -p "read_project project.o3proj; read_verilog; proc; opt; lower; run_testbenches --json r.json"
```

`run_testbenches [--only <name>] [--json <file>] [--report <file>] [--seed N]` runs every
testbench (or the one named) and prints one line each: name, pass, fail or error, `on`, wall time.
A failed comparison adds the first mismatch (`mismatch: expect line 5 time 4 field 1: want ...
got ...`: the line, the leading number of the line when it has one, the first differing field);
an error names the step (`run (rtl)`, `elaborate project (rtl)`, ...). The exit status is nonzero
when any testbench fails or errors. A project with no testbench prints that and passes. From
Python: `design.run("run_testbenches --json r.json")`. The specification is
`docs/specs/2026-10-31-project-testbench-design.md`.
