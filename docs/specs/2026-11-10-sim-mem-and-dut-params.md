# sim_tb: a `$mem` simulate hook and DUT `#()` overrides (scope, v1.6.1 gaps 5 and 8)

Status: scope and estimate only. No code. Purpose: Odin III goal 2 (the AI-digit corpus and other
memory-heavy designs simulated through Odin III without bit-blasting every memory word into
flip-flops). Measure: `sim_tb` on `read_verilog; proc; opt` of a design with `$mem` cells gives
the same result as iverilog, and runs without `lower`.

## 1. `$mem` simulate hook (gap 5)

Today `sim_tb` refuses a design that still holds a `$mem` (`cannot simulate $mem ...; run lower
first`). `lower` turns a memory of N words of W bits into N*W flip-flops plus read multiplexers,
which is why a 3 M-cell netlist appears for the TPU of the AI-digit lane.

Why a plain hook does not fit the engine (read: `src/sim/cell.h`, `eval.c`, `levelize.c`,
`build.c`):

1. State size. The simulator sizes a sequential cell's state as one byte per output bit
   (`build.c` `add_pins`). A memory needs `SIZE * WIDTH` bytes (one byte per bit in 2-state, two
   bits per bit in 4-state: `sim4.c`). The builder needs a per-type state-size hook, or `$mem`
   state kept in the scratch/`type_data` of the cell, sized from its parameters.
2. One cell cannot be both edge-triggered and combinational. An edge cell is cut from the
   levelization graph and writes its outputs from state only, at the start of each settle. A
   `$mem` has asynchronous read ports (output = f(RD_ADDR, state)) and synchronous write ports
   (state changes on an edge). The engine therefore needs the node split into two flat cells that
   share one state block: a write half (`SEQ_EDGE`, ports WR_*) and a read half (combinational,
   reads the shared state; for a synchronous read port, a registered half). That is a change to
   `build.c` (`add_pins`, `state_first`) and `levelize.c` (the read half depends on the write
   half's state, not on its inputs).
3. Clocks. `ODIN3_CT_CLOCK_PIN0` and `clk_in[]` assume the clock is a scalar port 0. `$mem`'s
   `RD_CLK` and `WR_CLK` are vectors with one bit per port and a polarity and enable bit per port
   (`RD_CLK_ENABLE`, `RD_CLK_POLARITY`, `WR_CLK_ENABLE`, `WR_CLK_POLARITY`). First step: all
   synchronous ports on one clock net with one polarity (what `read_verilog` produces from a single
   `always @(posedge clk)`); several clocks refused with a located error.
4. Four states. `sim_tb` defaults to 4-state, so the hook needs a `sim4` twin (an x address reads
   x, an x write enable or address makes the written word x: same rules as `lower`'s mux tree,
   `src/passes/lower/mem.c`, so the two paths agree).
5. Poking. `$readmemh` into a DUT memory by path (`odin3_sim_poke_state`, docs/user/testbench.md)
   works on flip-flops today; a `$mem` needs a word-addressed poke and the `INIT` parameter as the
   initial state.

Semantics to copy: `lower/mem.c` (read-during-write per `RD_TRANSPARENT`, write order by port
index, out-of-range address reads x and writes nothing) and `rtl_sel`-free; test against
iverilog with the existing `tests/tb` memory benches and the AI-digit TPU quick tier.

Estimate: 2 to 3 working days for single-clock 2-state and 4-state with the read/write split
(points 1, 2, 4), plus 0.5 day for poke and `INIT` (point 5); multi-clock memories (point 3,
second step) another day. Risk: points 1 and 2 touch the builder and levelizer that every
simulation uses; the full `sim_*`, `tb_*` and `equiv` suites must stay byte-identical. Recommended
as its own PR (plus a design review by Peter, since it changes the simulator's cell model).

Not a workaround: `lower --keep-mem` keeps `$mem` as flip-flop words for hierarchical poke but
still bit-blasts the storage.

## 2. DUT instance `#()` overrides (gap 8)

`sim_tb` refuses `dut #(.W(8)) u(...)` (`unsupported: parameter overrides on the DUT instance`).
The DUT was already elaborated by `read_verilog` with its default parameters, so the override
cannot be applied to the netlist. Ways, cheapest first:

- (a) Elaborate with the override: the testbench is parsed first (`tb_parse.c` first pass), the
  constant overrides are collected, and `read_verilog` is re-run with them as project-style
  parameter lines (`ODIN3_OVR_PROJECT`, elab1_params.c). The pass would need the DUT sources
  again (the pass does not keep them): `sim_tb` would take `--dut-param NAME=VALUE`, and the
  override written in the testbench is checked against it. About 1 day: option parsing, a
  `read_verilog -P NAME=VALUE` option (the same plumbing as the `-D` option of this release),
  tests with iverilog.
- (b) Refuse only when it differs: accept the override when it equals the default parameter of
  the elaborated top (read from the module's `parameter:` attributes) and refuse otherwise with
  the value to pass to `read_verilog -P`. Half a day; covers testbenches that repeat the default.
- (c) Re-elaboration inside `sim_tb` from the kept AST (`--lib`-style keep_streams): 2 to 3 days,
  and it changes what a pass may keep; not recommended.

Recommendation: (b) now, (a) when the first real testbench needs a non-default override.

## 3. Hierarchical references

Hierarchical reads of DUT signals and memories are supported (docs/user/testbench.md, "DUT
memories by hierarchical path"). Not supported and not planned here: assigning a DUT register
or memory word from the testbench, `force` on a DUT signal.
