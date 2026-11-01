# Round 6 gaps: evidence (PR #317, fix/r6-gaps)

Date 2026-11-01. Commands run on the head of this branch (debug build for tests, release for timing).

## boundtop-style latch loop (scoap --sigprob)

`tests/tools/data/latch_loop.v` is a 12-line always @* with an incomplete assignment (a latch whose D
reads its own Q). `tests/tools/test_scoap.py::test_sigprob_skipped_for_a_boundtop_style_latch_loop_in_rtl`
runs `read_verilog; proc; opt; lower; opt; scoap --sigprob 100` on it and checks: exit 0, the WARNING
`--sigprob skipped: the simulator refused the netlist (combinational loop; ...)`, no `error:` line, JSON
`sigprob_ran` false, `sigprob_vectors` 0, `rare` null, `p1_ppm` -1 on every net.

## ch_intrinsics against iverilog

Odin III reads the benchmark with the VTR invocation (`read_arch` k6_frac_N10_frac_chain_mem32K_40nm,
`read_verilog --vtr-compat`, `unmap`). Files for the comparison: `tools/sim-check/ch_intrinsics_tb.v`
(memset under test, 600 cycles) and `tools/sim-check/single_port_ram_model.v` (the VTR primitive
without its specify block, which Icarus rejects). Commands (X = external/vtr-verilog-to-routing;
skip when external/ is absent):

    iverilog -g2005 -o ch.vvp tools/sim-check/ch_intrinsics_tb.v \
        $X/vtr_flow/benchmarks/verilog/ch_intrinsics.v tools/sim-check/single_port_ram_model.v
    vvp ch.vvp > ref.txt
    odin3 -p "read_arch $X/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml; \
        read_verilog --vtr-compat $X/vtr_flow/benchmarks/verilog/ch_intrinsics.v; proc; unmap; opt; lower; \
        sim_tb --out out.txt tools/sim-check/ch_intrinsics_tb.v"

Result of this reproduction: both run, but the two outputs do not agree line for line, so this is NOT
a pass record. Every reference line has `x` fields (Icarus starts regs at x; Odin III starts at 0),
and memory_controller's `memory_controller_out` is a level latch (`case(prevTag)` with no default,
sensitivity list `tag or address or write_enable or in`, not `@*`), which Icarus evaluates only on a
change of the listed signals while Odin III evaluates it as a latch on every settle. The lane's earlier
verdict (2922 vectors, 0 mismatches against iverilog when the RTL uses `@*`) was reached on a netlist
comparison that is not in the repo; it is not reproduced here. Treat the Odin III side as unverified
until a TB with defined initial state and `@*` is committed.

## b19 scoap timing

Release build, `read_verilog b19.v; proc; opt; lower; opt; flatten; opt; scoap --sequential`:
read-to-scoap 5.2 s wall in total, 329 MB peak RSS; scoap alone 1097 ms on 5 modules, 400076 nets,
398947 cells, 774 sweeps (without flatten: 95 ms, 49355 nets). The 33-44 min on 638552 gates in the
brief was not reproduced with this pipeline at this head (hierarchy below b18 stays opaque after one
`flatten`, so the gate count differs); the pipeline that gave 638552 gates is not recorded.
