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
default stdout). Python: `design.run("sim_tb --out run.txt tb.v")` after the design is built.

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
DUT must have at most one clock input, a primary input of the DUT. Divide by zero gives 0.

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
