# Front-end triage from Leman Russ round 4 (2026-10-31)

Purpose: front-end and elaboration correctness (Peter's priority) so that Leman Russ III can read the
designs it needs. RQ: none (tool correctness). Measure: designs that elaborate and lower. Branch
`fix/frontend-triage`, worktree `work/odin3-fe`. Oracles: Yosys 0.55 as built in VTR (the Parmys
front end, `external/vtr-verilog-to-routing/build/bin/yosys`) and iverilog.

## Issue 1: ITC-99 b15, b17, b18, b19 refused, `'TEMPORARY' is assigned in two always blocks`

Reproduced on the current main with `read_verilog b15.v` (error at `elab2_claim.c:428`, rule T10).

Source: the vhd2vl output declares `reg signed [31:0] TEMPORARY;` once ("added to be used when
assignments need to be split") and blocking-assigns it in three `always @(posedge CLOCK, posedge RESET)`
blocks (b15.v lines 125, 296, 724). In every use the block assigns TEMPORARY in a begin/end sequence and
reads it in a later statement of the same sequence (`TEMPORARY = rEIP / 4; Address = TEMPORARY [29:0];`).
No block reads a value another block wrote, and nothing outside the blocks reads it.

Minimal reproducer: `tests/verilog/proc-bridge/scratch.v` (two clocked blocks, one scratch `T`).

What the oracles do with the same input:
- Yosys 0.55 (Parmys front end): `read_verilog -nolatches; proc; opt; check` exits 0 on b15, b17, b18, b19
  and on the reproducer. `proc` gives each block its own `$procdff` on `T`; the flip-flops have no reader
  (reads of `T` after a blocking assignment use the block's own value, Yosys `ProcessGenerator` `rv`), so
  `opt_clean` removes them. Standalone Yosys 0.69 without `opt` warns `multiple conflicting drivers for T`
  from `check` and still continues. Result: the design is accepted, `T` leaves no logic.
- iverilog: simulates the reproducer, each block sees its own assignment (`x=6 y=0` for a=5, b=9).

Odin III rule: spec 2D §8.9 and divergence row 12 made T10 an error ("T10 legal in simulation", accepted
by Yosys, flagged later by `check`). The refusal is correct when two blocks truly drive one bit that is
read elsewhere. It is wrong for a scratch variable whose every read is dominated by a whole-variable
blocking assignment in the same block: nothing is shared, so there is no multi-driver. The spec already
applies the same idea to loop variables ("Yosys makes loop counters outputs (a dead register) ... 2D's rule
avoids that false T10 conflict", §8.3 and divergence 5).

Verdict: Odin III bug (false T10 against Yosys/Parmys). Fix (own commit, regression micro): a variable is a
block scratch variable when (a) it is a plain variable (not a port, not an array), (b) at least two always
blocks assign it, (c) every assignment to it anywhere is a whole-variable blocking assignment inside an
always block, (d) every read is inside an always block and follows, in an enclosing begin/end sequence, a
whole-variable blocking assignment of it. Each always block then gets a private IR wire for it (the
variable's wire is swapped for the duration of the block), so the blocks drive different nets, there is no
T10 and no shared state. Anything else keeps the error. IR invariants are unchanged (every net still has
one driver); no ABI change. The private flip-flops have no reader and `opt` removes them, as in Yosys.

## Issue 2: VTR ch_intrinsics, mkPktMerge, mkDelayWorker32B, mkSMAdapter4B, boundtop refused

Reproduced: `read_verilog X.v` and `read_verilog --vtr-compat X.v` both say
`module 'single_port_ram' is not defined` (`dual_port_ram` for the others).

Leman Russ call (`lemanruss/htl/build.py:73`): `read_verilog "<file>"; proc; opt; lower`. There is no
`read_arch`.

Odin III rule (`src/frontends/verilog/soft_blocks.h`, decision D18, `docs/user/outputs.md`): the VTR
primitives `single_port_ram`, `dual_port_ram`, `multiply`, `adder` are supplied by `--vtr-compat`, and only
for a design that has an architecture loaded ("0 without a record"). In the VTR flow the architecture is
always present, and the models in it decide whether a primitive is a hard block (the k6_frac arch has
`single_port_ram` and `dual_port_ram` models, so they stay hard blocks) or soft logic (the arch has no
such model).

Verified with the corrected call on all five designs:
`read_arch <k6_frac_N10_frac_chain_mem32K_40nm.xml>; read_verilog --vtr-compat X.v; proc; opt; unmap; lower`
elaborates and lowers; `unmap` turns the hard RAM cells (and `multiply`/`adder` hard cells) back to `$mem`
(`$mul`, `$add`) which `lower` makes into flip-flops and muxes, so the netlist is gate level with no
hard-block cell left.

Verdict: invocation issue, not an Odin III bug. Documented in `docs/user/inputs.md`; the error for these
four names now says what to do (small code change, own commit, with a test). What Leman Russ should pass is
in the PR description.

## Issue 3: asynchronous resets made synchronous

Behaviour: `lower` turns `$adff` into `$_DFF_P_` plus a reset mux (PHASE3 decision 19; Parmys `adff2dff`)
and warns once ("made synchronous ... the netlist differs from the RTL on reset timing").

Target: the VTR arch k6_frac_N10_frac_chain_mem32K_40nm has only `.latch` flip-flops (`<pb_type name="ff"
blif_model=".latch">`) with D, Q and clock pins: no set, reset or enable pin. VPR rejects an async flip-flop
for this arch. So the conversion is the only netlist VPR can take, matches Parmys, and is correct for the
stated target. `--async error` is the option for a caller that wants a refusal instead.

Simulation: `sim`/`sim_tb` on the pre-`lower` IR stops (`cannot simulate $adff ... run lower first`); on the
lowered netlist the reset is synchronous, so a testbench that raises reset between clock edges sees the
difference from iverilog (reproduced: `q` stays 5 after `rst` rises with no clock edge; iverilog gives 0).
That is the same decision-19 divergence, not a second bug: the simulator models the netlist, which is what
the target hardware runs. `testbench_gen` holds reset for four clock cycles so generated testbenches agree.
Round 5 point: `sim_tb` is 2-state (documented); x and z literals in a testbench are refused with a located
error, and a DUT starts at 0 where iverilog starts at x.

Verdict: intended difference, documented in `docs/user/testbench.md` (what is not modelled) and
`docs/user/flow.md`. No code change: modelling async reset in the simulator would simulate a netlist the
target cannot hold.

## Agent defaults logged

1. Issue 1 fix is limited to the strict dominated-scratch pattern; wider cases keep T10.
2. Issue 3 stays a documented divergence (no `--async` default change).

## Known, not fixed (critique round, LOW)

- The scratch rule is narrow by design: only the strict dominated pattern is accepted; wider cases keep the
  T10 error (safe refusals).
- Scratch flip-flops that nothing reads are removed by `opt`; without `opt` they remain as dead cells.
- The scratch walk and the writer-list scans are O(n^2) in the worst case (many scratch variables or
  blocks per module); not a problem at the sizes seen (ITC-99 b15 to b19).
