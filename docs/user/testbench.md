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
Options: `--top <module>` (the testbench module, default the first), `--max-time N` (default
10,000,000 time units), `--seed N` (the seed of `$random`), `--out <file>` (the `$display` text;
default stdout), `--vcd <file>` (a waveform, below). Python: `design.run("sim_tb --out run.txt tb.v")`
after the design is built.

## Waveform (`--vcd`)

`sim_tb --vcd wave.vcd tb.v` writes an IEEE 1364 VCD of the run. It holds every scalar `reg`,
`wire` and `integer` of the testbench (arrays and parameters are left out) under `$scope module
<testbench>`, and the ports of each DUT instance that the testbench connects, under a nested
`$scope module <instance>`. Values are 2-state (0 and 1). The `$timescale` is the testbench's
`` `timescale `` unit (`1ns` when it has none), and the time stamps are the testbench time units.
The file starts with a `#0` `$dumpvars` block, then one `#<time>` block for each time step in
which a dumped signal changed; a signal is written with the value it has when the step ends, so a
glitch inside one time step does not show. Open it with GTKWave. Odin III checks it against
iverilog (`$dumpvars(0, tb)` on the same testbench) with `tools/vcd-compare/vcd-compare`.

## What is supported

One testbench module without ports per file, with `reg`/`wire`/`integer`/array declarations,
`parameter`, `assign`, `initial`, `always` (starting with `#`, `@` or `wait`), blocking and
nonblocking assignments, `#N`, `@(posedge|negedge|change ...)`, `wait`, `if`, `case`, `for`,
`while`, `repeat`, `forever`, DUT instances with named port connections, `$display`, `$write`,
`$strobe`, `$monitor`, `$fopen`, `$fdisplay`, `$fwrite`, `$fclose`, `$readmemh`, `$readmemb`,
`$finish`, `$stop`, `$time`, `$random`, `$signed`, `$unsigned`, `$clog2`. Values are at most 64
bits wide. Anything else stops the run with `tb.v:LINE: unsupported: <construct>`: x/z literals,
real numbers, tasks and functions, generate, hierarchical names, parameter overrides on the DUT
instance, ordered port connections, several modules in the testbench file, compiler directives
other than `` `timescale ``.

## What is not modelled

The simulator is 2-state and cycle-based. The DUT powers on with all state 0 (iverilog starts at
x), so reset the DUT before printing its outputs. Gate or `#` delays in the DUT are not seen. The
DUT must have at most one clock input, a primary input of the DUT; its registers may be clocked by
nets derived from it (divided, gated or muxed clocks: `tests/tb/micro/dclk.v`, equal to iverilog), which
the simulator evaluates after each input change and each edge. Divide by zero gives 0.

Asynchronous resets: `lower` makes them synchronous, as Parmys does (PHASE3 decision 19), because the
VTR architecture's flip-flop (`.latch`) has no set or reset pin. `sim_tb` runs the lowered netlist, so
a reset raised between clock edges takes effect at the next edge; iverilog applies it at once. On the
IR before `lower`, `sim` and `sim_tb` stop at the `$adff` cell. `testbench_gen` holds reset for four
clock cycles, so its testbenches agree with iverilog. `lower --async error` refuses an async reset
instead. A testbench cannot use x or z literals (they are refused with a located error).

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
