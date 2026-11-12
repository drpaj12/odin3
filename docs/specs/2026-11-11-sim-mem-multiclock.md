# sim: `$mem` on several clocks (v1.6.2; extends 2026-11-11-sim-mem-hook-design.md section 3)

Purpose: Odin III goal 2 (dual-port RAMs written and read on different clocks simulate without `lower`).
Measure: a `$mem` whose ports are on different clock nets or edges gives the output of the lowered netlist
(where `lower` accepts it) and of iverilog, 4- and 2-state.

## Cell model

The engine sends an edge to a cell through its one clock pin (`ODIN3_CT_CLOCK_PIN0`), so one edge cell per
(clock net, polarity) used by a live write port or a synchronous read port. Groups are made in the order
write ports (index order), then read ports. Every edge cell has the full port spans of the node (inputs are
read at the edge) plus `WR_CLK` and `RD_CLK` spans: for port p the cell's clock slot when p belongs to the cell,
else the zero slot. The hook finds its own ports by comparing those pins with its `CLK` pin; the builder
decides membership once, so slot merging done later cannot make two cells claim a port. `RD_DATA` of a cell
holds its own synchronous ports; its state is their registers. The asynchronous read cell is unchanged.

Shared block (`odin3_sim_cell.aux`, same for every cell of the node, owned by the first): the array
(`words * WIDTH`, one byte per bit) followed by one record per write port (pending flag, WR_EN, WR_DATA,
WR_ADDR codes of its last edge). The first cell sets the array from INIT and clears the records.

## Same-instant semantics

All edges of one instant (`odin3_sim_clock_edges`, derived clocks of one round) see the memory as it was
before the instant, as RTL non-blocking assignments do. With one group a write is applied at its edge,
after that edge's reads (as before). With several groups (`ODIN3_SIM_CELL_MEM_DEFER`) an edge copies the
write inputs into the record and the writes are applied at the next settle, in write-port-index order (the
later port wins a same-address conflict, as `lower` for one clock). The edge cells come first in the settle
order, so the array is final before any asynchronous read.

A transparent synchronous read sees the write ports of its own clock net and edge only, exactly `lower`
(`sees_write`); a port on another clock reads the array as it is.

## What `lower` does not do

`lower` refuses write ports on different clocks or edges. The cell simulates them; the reference there is
iverilog (`tests/tb/run_memclk.sh`), and the same-instant rule above is the RTL's.

## Refused

An asynchronous write port; more than 16 (clock net, edge) pairs; over 4 GiB of state.
