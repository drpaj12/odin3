# Plan: Arduino-on-RISC-V microcontroller under Odin III testbench simulation (2026-10-31)

Purpose: first Seeker-Proc v2.0 base for Leman Russ III. Measure: Odin III runs a real
processor program, on the RTL and on the netlist, matching iverilog cycle for cycle on the ports
(PC, PORTB_data_out, PORTD_data_out).

Design: github.com/qiany21/Arduino-on-RISC-V (MIT), `quartus_project_revert_timer`, top
`single_cycle_BRAM_top`. Upstream stays outside this repo (read-only clone under `external/`); the
test skips when it is absent and a small patch set is applied to a scratch copy at run time. Only
the patches and the port-only testbench are committed, so the upstream licence text is not
needed in the tree (nothing of the upstream is vendored).

Steps
1. Front end: `$readmemh/$readmemb` first argument may be any constant string expression
   (string parameter, localparam, concatenation of string constants). Resolved at
   elaboration with the constant evaluator. As Yosys (`AstNode::readmem`, simplify.cc): the
   string is the constant's bytes with NULs dropped (so `{"0"+i, BASE}` works). Relative
   paths: Yosys tries the working directory, then the source file's directory; Odin III
   keeps its documented rule (source file's directory only, spec 17).
2. Regression micro + golden for the string parameter case.
3. Patch set for the design (PLL stub, `byte` identifier, `$time()`), port-only testbench,
   iverilog reference for 2-3 programs.
4. sim_tb on the RTL and on the netlist (proc; opt; lower; k6_frac mem32K with --vtr-compat
   and unmap if the RAM maps hard). Report cycles, wall time, memory, DFF count, first mismatch.
5. Fix only Odin III bugs found on the way; log Quartus-specific items as patches.
6. Lint, full debug ctest, CI, PR.
