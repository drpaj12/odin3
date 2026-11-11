# sim: simulating `$mem` without `lower` (design, v1.6.1 gap 5)

Status: design for review. Scope doc: `docs/specs/2026-11-10-sim-mem-and-dut-params.md` section 1.
Purpose: Odin III goal 2 (memory-heavy designs, the AI-digit corpus, simulated without turning every
memory bit into a flip-flop). Measure: `read_verilog; proc; opt; sim_tb` on a design with `$mem`
prints what iverilog prints and what the `lower` path prints, and runs much faster than via `lower`.
Peter reviews this design before the PR merges (it changes the simulator's cell model).

## 1. Cell model: one `$mem` node, up to two flat cells, one shared state block

The engine cuts the levelization graph at edge-triggered cells (they write outputs from state only)
and orders everything else by its pin reads. One `$mem` is both: its synchronous parts are edge
cells, its asynchronous read ports are combinational reads of state. So `build.c` expands a `$mem`
node into two flat cells that share one state block (`add_mem`, new `src/sim/mem_sim.c`):

- **E (edge cell, `ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0`)**: all write ports and all
  synchronous read ports. Created when the memory has a write port that is not dead (WR_EN not
  constant 0) or a synchronous read port. Pins: CLK (one bit, the common clock), WR_EN, WR_ADDR,
  WR_DATA (the node's full ports, inputs: the graph ignores them, as for every edge cell), RD_EN and
  RD_ADDR (all read ports, inputs), and RD_DATA of the synchronous ports only (output). Its COMB
  writes RD_DATA from the registered read data; its POSEDGE / NEGEDGE (the one polarity of the
  memory) does the sync reads, then the writes.
- **A (async read cell, `ODIN3_CT_SEQ_LEVEL`, levelized like a combinational cell)**: the
  asynchronous read ports. Pins: RD_ADDR of the async ports only (input), RD_DATA of the async ports
  only (output). Its COMB reads the array at the address now. It has no clock. Level type so it gets
  the INIT event.

The split matters for ordering: a sync read port's address does not feed A, so a pointer-chase
(`RD_DATA -> logic -> RD_ADDR`, sync) is not a false combinational loop; a write port reading data
that another memory port produced is fine because E reads its inputs only at the edge.

State block (bytes, one per bit in both modes; codes 0/1 in 2-state, 0/1/x/z in 4-state):
`[ sync read registers: n_sync * WIDTH ][ array: words * WIDTH ]`, `words = min(SIZE, 2^ABITS)` as in
`lower/mem.c`. The block belongs to E (`state_first`, `n_state`); A's view points at the same bytes
(`state_first` equal, not counted twice); a ROM with only async reads has A alone, which owns it.
The sync registers come first so `build_poke_map` (output bit -> state byte, in order) stays right for
E. Exactly one cell has the flag `ODIN3_SIM_CELL_MEM_INIT` and performs the INIT event (E, else A).
This extends the contract in `sim/cell.h` ("state: one byte per output bit"): a `$mem` cell state is
sized from its parameters. A size past 2^32 bytes is a located error (`memory too large`).

Per-memory geometry (SIZE, OFFSET, ABITS, WIDTH, port flags, INIT) is read from the `$mem` params
(`view.params` is the node's, for both cells); nothing is cached beyond what the hook computes
per call from a handful of integers. Pin lists are built by `add_mem` as concatenated slices of the
node's ports, so the hooks see dense `[port][word-bit]` spans and need no port mapping.

## 2. Semantics (copied from `src/passes/lower/mem.c` so both paths agree)

- Word of raw address `a`: `w = (a - OFFSET) mod 2^ABITS`; it exists when `w < SIZE` (and the
  address reaches it). The word for index k has raw key `(k + OFFSET) & mask`.
- **Write**, at the memory's clock edge, ports in index order (later wins). Bit `b` of the word at
  the port's address takes `WR_DATA[b]` when `WR_EN[b]` is 1 and the address is an exact match; an
  address outside the words writes nothing. Dead ports (WR_EN all constant 0) are skipped. The
  5 rules per (word, bit) are the `$_MUX_` chain of `lower`: select = AND(word decode, WR_EN[b]) in
  4-state AND; 1 writes data, 0 keeps, x gives x unless old and new are the same known value (the
  `$_MUX_` merge). An x or z address bit is therefore handled as lower does: a word whose known
  address bits all match gets select x. Fast path: a fully known address indexes the word directly.
- **Async read**: the word at the address now. Out of range reads x (0 in 2-state, where `lower`'s x
  constant reads 0). An address with x/z bits reads x unless every word and every missing word the
  known bits allow holds the same known value (the nested `$_MUX_` merges, collapsed).
- **Sync read**, at the edge, before this edge's writes: if RD_EN is 1 the register takes the word as
  it was before the edge; if 0 it holds; RD_EN x/z gives x per bit unless unchanged. With
  `RD_TRANSPARENT`, bit `b` is then replaced by `WR_DATA[b]` of each write port (in order) whose
  `WR_EN[b]` is 1 and whose address equals the read address (x in either address gives the merge x).
  `lower` warns "Parmys reads the old value (PHASE3 #40)"; the simulator follows the RTL and prints
  no warning. Corner not copied exactly: with RD_EN x on a transparent port `lower` mixes registered
  pieces; the hook merges the whole fresh word with the old one. Documented in testbench.md.
- **INIT**: word k bit b = `INIT[k*WIDTH + b]`; 0 and 1 as given, anything else (x, or INIT shorter
  than the array) x in 4-state and 0 in 2-state, as a `$_DFF_` INIT 3.
- 2-state mode: all of the above with x read as 0; no x ever stored.

## 3. Clocks: refused cases

All live synchronous ports (non-dead write ports, sync read ports) must share one clock net and one
polarity. Anything else is a located error `cannot simulate '$mem' node 'm': <why>`:
several clocks or polarities, a live asynchronous write port (WR_CLK_ENABLE 0), a clock tied to a
constant (the existing "no clock connected" error). The clock net may be a primary input, an
identity-buffered input or a derived clock; the existing clock scan (`find_clocks`,
`build_clock.c`) takes pin 0 of E like any edge cell. Multi-clock memories are a later step (a state
block per clock domain would need E split by clock, and the sync read of a port on clock A seeing a
write on clock B has no defined order in the cycle engine).

## 4. Poke and INIT by hierarchical path

`$readmemh(file, uut.mem)` for a DUT memory today needs `lower --keep-mem` (words are named
wires). With a `$mem`, `odin3_sim_signals_find` resolves `<scope>.<MEMID>[<index>]` (MEMID as the
`$mem` params say, `\` stripped) to a word signal (`obj_kind ODIN3_OBJ_MEMWORD`, flat cell, index);
reads copy the state bytes, `odin3_sim_signals_poke` writes them (codes, validated like flop pokes).
`tb_inst_array` already walks `mem[0]`, `mem[1]`, ... until a word is missing, so the testbench
sees the same array API as with `--keep-mem`. A poked ROM word is visible at the next settle. INIT is
the initial state through the INIT event (also after `odin3_sim_reset`).

## 5. Risk and how it is bounded

Touches `build.c` (`add_flat`: a `$mem` branch before the "no simulate hook" refusal), `point_views`
(shared state), `eval.c` (`send` unchanged; INIT unchanged) and `signals.c` (name resolution). The
levelizer, the clock scan and the drivers check are unchanged: E and A are ordinary flat cells with
their own `odin3_celltype_def`s. No design without a `$mem` takes any new code path; the full
`sim_*`, `tb_*` and `equiv` suites must stay byte-identical (checked, reported in the PR).
Memory: one byte per stored bit, like the flip-flop state it replaces but without the 3 M cells, mux
trees and decode logic; packing bits is a later optimisation if a design needs it.

## 6. Out of scope here

Multi-clock memories; `$memrd` / `$memwr` (not produced by `read_verilog; proc`); a memory cap
option (the 2^32-byte limit stands); `force` or assignment to a DUT memory word from the testbench.
