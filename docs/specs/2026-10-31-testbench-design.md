# TB: Verilog testbench simulation (`sim_tb`, `testbench_gen`, `tools/tb-compare`)

Status: v1 spec (2026-10-31), written before code. Brief: controller briefs/odin3-testbench.md.
Purpose: simulate a design under a Verilog testbench (no `--vectors-in` file), so that VTR
benchmarks without testbenches, Leman Russ trigger testbenches and Loki recovered-RTL checks can
be run and compared. Measure: output equality with iverilog (the oracle), line by line.

## 1. Architecture

The testbench file is NOT elaborated or synthesised. A small self-contained front end in
`src/sim/tb_*.c` (lexer, parser, evaluator, scheduler) interprets the subset of section 2. The DUT
is whatever the design holds: the user runs `read_verilog dut.v; proc; opt; lower` (RTL) or
`read_blif dut.blif` (netlist) first. Each DUT instance in the testbench is a module of the design,
simulated by the existing cycle simulator (`odin3_sim`, S1/1E) built on that module. New code is in
new files; only `CMakeLists.txt`, `src/passes/builtin.c` and `docs/PASSES.md` are shared with the
other lanes (overlap noted in the PR; no change to `src/api` or to existing `src/sim` files).

Pass: `sim_tb [--top <tb_module>] [--max-time N] [--seed N] [--out <file>] [--vcd <file>] <tb.v>`.
`--out` also writes the `$display` text to a file (stdout/log otherwise). `--dut-netlist <file>` is
the same as a preceding `read_blif <file>` (agent default D-1: one mechanism, the design in hand;
the flag is sugar that runs the BLIF reader). Python API: `odin3.Design.run("sim_tb ...")` (the
pass is a script command, so the cffi binding needs no new function).

## 2. The supported subset

Module: `module name; ... endmodule` or `module name();` (no ports). `--top` picks it; default the
first module of the file. Other modules in the file are rejected ("only one testbench module").
`` `timescale u/p `` is parsed; delays are integers in units of `u`; `$time` is in units of `u`
(precision `p` is ignored: delays must be integers, `#1.5` is rejected).

Declarations: `reg|wire|integer [h:l] a, b;` (h,l constants, width at most 64), `reg [h:l] m [0:n];`
(array of reg), `parameter|localparam X = const;`, `reg x = const;`, `integer fd;`. `genvar`, `real`,
`time`, `string`, `event`, tasks, functions, generate, `defparam`, `specify`: rejected.

Continuous: `assign w = expr;` for a wire (re-evaluated whenever any TB signal changes).

DUT instances: `dut_mod [#(...)] inst (.p(expr), ...);` or ordered. Parameter overrides are
rejected (the DUT is already elaborated). Input port connection: any expression (re-evaluated
at every sync). Output port connection: an identifier of a wire or reg of the testbench (width
is truncated or zero-extended). Ports wider than 64 bits and inout ports: rejected. A DUT with
more than one clock bit is rejected (one clock: section 4). Unconnected inputs are 0.

Procedural: `initial stmt`, `always stmt` (an `always` must wait: begin with `#`, `@` or `wait`,
else rejected as a zero-delay loop). Statements: `begin..end` (names allowed), `=`, `<=`
(nonblocking; no intra-assignment delay), `#N stmt;`, `#N;`, `@(posedge|negedge|any a [or b ...])`,
`@(a, b)`, `@*` (rejected), `wait(expr)`, `if/else`, `case` (`casez`/`casex` rejected: no x),
`for`, `while`, `repeat`, `forever`, `disable` (rejected), system tasks. LHS: identifier, bit
select, part select (constant), `mem[i]`, concatenation of those.

Expressions (2-state; widths and signedness follow IEEE 1364-2005 5.4/5.5 for the operators
below): sized/unsized numbers in b/o/d/h (`x`/`z` digits rejected), string literals (only as
`$display` arguments), identifiers, `a[i]`, `a[h:l]`, `m[i]`, `{a,b}`, `{n{a}}`, unary
`+ - ~ ! & | ^ ~& ~| ~^`, binary `+ - * / % & | ^ ~^ << >> <<< >>> < <= > >= == != === !== && ||`,
`?:`, `$time`, `$random`, `$random(seed_var)`, `$urandom` (same stream as `$random`), `$signed`,
`$unsigned`, `$clog2`. Real numbers and `**` are rejected. `/` or `%` by 0 gives 0 (iverilog
gives x; documented difference D-2). Values are at most 64 bits.

System tasks: `$display $write $strobe $monitor $monitoroff/on $fdisplay $fwrite $fopen $fclose
$finish $stop $readmemh $readmemb $dumpfile $dumpvars` (the last two are accepted and ignored: use
`--vcd`). Format specifiers: `%d %0d %Nd %b %0b %Nb %h %x %o %c %s %m %%` with an optional width or
`0`; `%t %e %f %g %v` are rejected. `%d` pads to the digit count of the width unless `%0d`
(LRM 17.1.1.2); signed regs print a sign. `$fopen(name, mode)` returns a descriptor with
`$fdisplay(fd, ...)`; `fd` 1 is the output. `$readmemh/b(file, mem [, start [, end]])` fills a reg
array (no `@addr` lines beyond the first form; `//` comments and `_` allowed); a missing file is an
error. `$finish` ends the run; without it the run ends when no event is left or at `--max-time`
(default 10,000,000 units). Any unsupported construct stops the pass with a located error
`tb.v:LINE: unsupported: <construct>`.

## 3. Time and scheduling

Event wheel by integer time. A time step is: (1) wake delayed processes at t; (2) active region:
run runnable processes until all block (a blocking write wakes `@` waiters at once); after each
quiet point the DUTs are synced (section 4) and their outputs are published to the testbench wires,
which can wake processes again; repeat until nothing is runnable; (3) NBA region: apply `<=`
updates, then back to (2) if anything woke; (4) postponed: `$strobe` and `$monitor` print. This
reproduces the iverilog-visible order for race-free testbenches: a process waking on
`@(posedge clk)` reads the DUT's outputs from before that edge (as with a non-blocking flop).
Testbenches whose result depends on active-region order between processes are racy in iverilog too
and are not a gate.

## 4. DUT clocking and what is not modelled

The DUT is a posedge/negedge-triggered netlist with a settle. At every sync the connected input
expressions are evaluated; changed non-clock inputs are applied and the DUT is settled
(`odin3_sim_settle`); if the clock input went 0 to 1 the DUT gets `odin3_sim_clock_edge(POSEDGE)`
(sampling the inputs as settled just before), 1 to 0 the NEGEDGE. The outputs after the settle are
published at the end of the active region. The clock must be a primary input of the DUT (a derived
clock is rejected by the simulator, S1 rule). Not modelled: gate or `#` delays inside the DUT
(`#` in DUT RTL are not seen: the DUT is the elaborated design), x and z (2-state: the DUT powers
on with all state 0, where iverilog has x before the first reset, so a testbench must reset before
it prints; the generator does), strengths, `timescale` inside the DUT, multiple clock domains
with independent phases, asynchronous glitches.

## 5. `testbench_gen`

`testbench_gen [--cycles N] [--seed N] [--out tb.v] [--vec vectors.hex] [--dut-name n]` writes,
for the design's top: module `tb_<top>`, regs for inputs, wires for outputs, the DUT instance, a
clock `always #5 clk = ~clk;`, a reset sequence, N vectors and an output capture. Detection
(stated, agent default D-3): a clock is an input bit that drives an edge-triggered cell clock pin
(structure, netlist view; `odin3_sim_input_is_clock`), else an input named, case-insensitively,
`clk`, `clock`, `ck` or containing `clk`/`clock`. A reset is a 1-bit input named `rst`, `reset`,
`arst`, `rstn`, `rst_n`, `reset_n`, `resetn`, `nrst`, `nreset`, `clr`, `clear` (case-insensitive);
active low when the name ends in `n`/`_n`/`b`/`_b` after the stem or starts with `n`, else high.
Sequence: all inputs 0, reset asserted for 4 clock cycles, released; then N vectors. Vectors are
written to a hex file, one line per cycle, one hex word per non-clock non-reset input in port
order (so the vector file is a plain stimulus file and also feeds other tools), read with
`$readmemh` into per-input arrays. Each cycle: apply the vector at the falling edge, sample the
outputs just before the next rising edge and `$display("%0d %h %h ...", cycle, outs...)`, so the
capture is one line per cycle with the cycle number first. Ports wider than 64 bits are an error
(D-4; next step: wide values). Random vectors come from the C `odin3_prng` in the generator, not
from `$random`, so the stimulus is identical for every simulator.

## 6. `tools/tb-compare`

`tools/tb-compare/tb-compare --tb tb.v --a <script-a> --b <script-b>` (Python, stdlib): runs the
odin3 CLI twice with the same testbench, each script building one design (`read_verilog ..; proc;
opt; lower` golden RTL, or a synthesised/Trojan netlist), then compares the two `--out` files line
by line and reports the first mismatch: line number, simulation time/cycle (first field of a
generated-testbench line, else the line text), and both lines with the differing fields named.
Exit 0 identical, 1 mismatch, 2 error.

## 7. Gates (oracle: iverilog)

(a) micros with testbenches in `tests/tb/micro/` (combinational, counter, shift reg, FSM, memory
`$readmemh`, `$random`, `$monitor`, nonblocking): `$display` output of `sim_tb` equals
`iverilog -g2005 && vvp` line for line. (b) generated testbenches for the 14 small VTR designs:
the generated `tb.v` run by `vvp` against the DUT RTL equals `sim_tb` on the lowered netlist.
(c) VerilogEval testbenches (`external/Gate-Breaker-verilog-eval`): count of benchmarks whose
reference testbench runs unchanged in the subset, and the number whose output equals iverilog's.
Results are in the PR description and `docs/user/testbench.md`.

## 8. Agent defaults, deviations from sections 1 to 7 as built, next step

Agent defaults: D-1 pass named `sim_tb`, generator `testbench_gen`, compare is a Python tool (not a
pass). D-2 divide by zero is 0. D-3 detection heuristics as in section 5. D-4 64-bit value limit
(testbench values, DUT ports, generator ports). D-5 the testbench file holds exactly one module.

Deviations from the text above, as built (2026-10-31):
1. `--dut-netlist` is not a flag: read the netlist first (`read_blif f.blif; sim_tb tb.v`).
2. `--vcd <file>` (added 2026-10-31, `src/sim/tb_vcd.c`): dumps the testbench's scalar regs, wires and
   integers and the connected ports of each DUT instance (scope per instance), 2-state, with the
   end-of-time-step value of each change; `$timescale` from the testbench (`1ns` when absent).
   Gate `tb_vcd` against iverilog's `$dumpvars` VCD (`tools/vcd-compare`).
3. Ranges must be `[h:0]` (and arrays `[0:N]`); ordered port connections are rejected (named only).
4. `testbench_gen` writes one stimulus file per data input, `<out>.<input>.hex`, not one file with
   a word per input per line.
5. `$random` is a Park-Miller generator; its stream does NOT equal iverilog's (iverilog updates
   the seed as `seed*69069+1` but derives the value differently; not matched). Testbenches whose
   output depends on `$random` values are not an oracle gate; `$random(seed)` is deterministic.
6. `$display` details matched to iverilog 12 (checked): no separator between arguments, extra
   arguments after a format string are appended in default decimal, a missing argument prints
   `<%d>`, `%0Nd` zero pads, `$finish` runs the postponed region (`$monitor`, `$strobe`) of its
   time step. `$finish` itself prints nothing here (vvp's "$finish called" line is not compared).
7. A DUT with more than one clock input, a DUT port wider than 64 bits and any hierarchical name
   are rejected, as listed in section 2.

Gate results (2026-10-31, debug build, iverilog 12.0; tests in `tests/tb/`):
(a) micros: comb, counter, fsm, mem (with `$readmemh`), misc (format, `$monitor`, `$strobe`,
signed shifts, concat): 5/5 identical to iverilog (`tests/tb/run_micros.sh`, ctest `tb_micros`).
(b) generated testbenches (`tests/tb/gen_gate.sh`, same `tb.v` on RTL under iverilog and on the
lowered netlist under `sim_tb`): PASS (exact): counter, fsm, comb, single_wire, single_ff,
and_latch, diffeq2. PASS-X (equal except hex digits that are x in iverilog because state is
uninitialised before the first write; the 2-state side prints 0): mem, ch_intrinsics,
ch_intrinsics_nolatches, sha, diffeq1. Not run, with reason: multiclock_output_and_latch (iverilog
rejects the design), multiclock_separate_and_latch, multiclock_reader_writer and stereovision3
(more than one clock input: unsupported), mkPktMerge and blob_merge (ports wider than 64 bits).
That is 12 of the 14 designs of the PHASE3 #16 set run (5 exact, 5 x-masked) with no mismatch;
the other 2 are limits of the subset, listed above. ch_intrinsics, mkPktMerge need the
`single_port_ram`/`dual_port_ram` models (`tests/tb/vtr_rams.v`, read by both tools).
(c) VerilogEval (`dataset_spec-to-rtl`, 156 `*_test.sv`): 0 of 156 run unchanged. Every file uses
`` `define `` and SystemVerilog (`logic`, `int`, `typedef struct`, tasks, several modules per
file, `$urandom` streams, `wavedrom_*` tasks), all outside the subset. A SystemVerilog testbench
interpreter would need the slang front end (`adapters/slang`) to parse them; not started.

Next step (for the resuming agent), FIRST: `tools/lint.sh` (clang-tidy) fails on the new files
(370 findings, 2026-10-31: 257 readability-identifier-length (names under 2 characters: `e`, `w`,
`c`, `b`, `g`, `o`, `n`, `r`, `t`), 52 readability-magic-numbers, 30 misc-no-recursion (the
recursive-descent parser in `tb_parse.c` and the recursive evaluator in `tb_expr.c`, `tb_eval` and
callees), 20 bugprone-multi-level-implicit-pointer-conversion and 11 bugprone-sizeof-expression
(the `tb_alloc` / array-of-pointer idiom), 10 easily-swappable-parameters, 7 cognitive-complexity,
4 nested conditional operators, and a few singles). All other gates pass (clang-format, cppcheck,
lizard, ruff, mypy, `--no-tidy`). Fix at the source, never NOLINT: rename short identifiers,
name the constants, and replace the recursion by explicit stacks (an operand stack with a
post-order node list built at parse time for `tb_eval`; an operator-precedence parser with
explicit stacks for expressions and a statement stack for the parser). Then CI. After that: (1) read DUT signals by hierarchical name in testbench
expressions through `odin3_sim_find_signal`; (2) values wider than 64 bits (arrays of words) so
mkPktMerge/blob_merge generate; (3) `` `define `` object macros and `` `include `` in the testbench
lexer; (4) multi-clock DUTs by per-clock edge calls in the simulator; (5) `$random` matching
iverilog; (6) `--vcd`; (7) the section 9 processor target.

## 9. Stretch target: BRISC-V timer core (Arduino-on-RISC-V, `quartus_project_revert_timer`)

Controller request (read-only external tree; the future processor benchmark, Seeker-Proc base).
Static analysis of `tb_single_cycle_BRAM_top_gcd.v` against this subset, 2026-10-31:
it does NOT run in the v1 subset. Blocking constructs, in order of source:
(1) `` `define REGISTER_FILE dut.core.ID.registers.register_file `` and `` `CURRENT_PC `` macros
(the lexer rejects every directive but `` `timescale ``); (2) hierarchical references into DUT
internals (`dut.core.FI.PC_reg`, `dut.memory.memory.BYTE_LOOP[byte].ELSE_INIT.BRAM_byte.ram[x] = ...`,
a hierarchical WRITE into the DUT's BRAM): the DUT is a separate elaborated netlist whose internals
only exist as flat names, so this needs a name-resolved peek/poke into the simulator (S1 signal
find/get exists; poke of state bits does not); (3) `generate for` with a genvar named `byte`, and
`parameter` overrides on the DUT instance (`#(.DATA_WIDTH(..))`: rejected, the DUT is elaborated
first with its own defaults); (4) `$time()` with empty parens and `$stop`; (5) a Windows path in
`PROGRAM` for `$readmemh` (the file itself would need `.vmh` words that `$readmemh` accepts, they
are plain hex words, so the reader copes); (6) DUT side: `pll_clock.v` instantiates `altpll`
(Quartus primitive, no model: treat as a black box or bypass with a clock buffer), the dual-port
BRAM uses `$readmemh(INIT_FILE, ram)` in an `initial` block (Odin III front end handles
`$readmemh` in RTL, the `.mif` is not read by the RTL path), and a 14-bit-address 32-bit word
memory (16K x 32 bits, so memory lowering size limits apply: `lower --max-mem-bits`).
iverilog check: `iverilog -g2005` fails at `$time()` ("Empty function argument list requires
SystemVerilog"); `-g2012` fails at `byte` used as a genvar (SV keyword). So iverilog cannot run
the testbench unchanged either; a patched copy (rename `byte`, `$time` without parens, PROGRAM
path) would be needed, plus an `altpll` model. Next steps for this target, in order: preprocessor
`` `define `` object macros in the testbench lexer; hierarchical read of DUT signals in testbench
expressions (S1 find_signal); poke into memory contents; parameter override on the DUT instance
by elaborating the DUT with `read_verilog` parameters; wide-value support. Not started in v1.

Fix round (PR #308 review, 2026-10-31): nonblocking assignments resolve their lvalue indices when
the statement runs; a DUT's clock edges are recorded at every write of the clock and replayed in
the NBA region (register outputs appear after the active processes ran, independent of process
order), and the active region runs again after any NBA or edge; the DUT clock starts at 0
(2-state), so `initial clk = 1` is a posedge at time 0; `#0` loops fail after 100000 steps at one
time; `$monitor` fires on non-`$time` argument changes; output lines are unbounded; a signed
negative `repeat` count is 0; evaluation errors fail the run. Regression tests: `tests/tb/micro`
(`nba_idx`, `nba_dut`, `ord_a`, `ord_b`, `pulse`, `seed`, `mon`, `longline`, `repeat`, `redxnor`),
`tests/tb/errors` (`delta`, `fopen`, ctest `tb_errors`), `tests/tools/test_testbench_gen.py`,
`tests/tools/test_tb_compare.py`.
