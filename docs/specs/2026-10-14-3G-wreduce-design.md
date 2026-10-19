# 3G — `wreduce`: design

Status: v1 (2026-10-14), **approved by Peter 2026-10-14** with the plan (PHASE3 #31; `$mem` ABITS 0 allowed)
(`docs/plans/2026-10-14-3G-wreduce-plan.md`). Phase 3, sub-project 3G (PHASE3 decision 7 = (b);
light review; Opus `high`). Spec: `docs/DESIGN.md` §5, §6 step 4. Inputs: `docs/IR.md` (IR-2, IR-6,
IR-8 and the mixed-sign rule, IR-12/13); PHASE2 #19 (no compaction inside a pass), #25 (mixed-sign
cells); PHASE3 decisions 3, 4, 7, 17, and the rulings of 2026-10-14 (a faithful Yosys 0.55 `wreduce`;
signed adders are sign-extended in 3C, not copied from Parmys); the 2F spec (OP-2 kit, OP-12 caps,
OP-13 refinement, OP-14 determinism, OP-15 provenance, OP-16 diagnostics, §20 tools); the 2H splice
report (stereovision0 eq-x review, `exit-repros/sx_add`). **Oracle: Yosys 0.55 as pinned by VTR**
(`$VTR_ROOT/libs/EXTERNAL/yosys`, `60f126cd`, `passes/opt/wreduce.cc`, 653 lines; binary
`$VTR_ROOT/build/bin/yosys`), and Parmys's `vtr_flow/misc/yosys/synthesis.tcl` at VTR `3c9a4d23`.
Decisions are numbered **WR-n**. Where this file and `docs/IR.md` disagree, IR.md wins.

## 1. Purpose and scope (WR-1)

`wreduce` narrows word cells to the widths their inputs and readers need, exactly as Yosys 0.55's
`wreduce` does, so that `hardmap` (3C) sees Parmys's operand widths. Widths decide hard-block
counts: an adder chain has one `adder` per operand bit, and the multiplier threshold looks at
operand widths (decision 7). Without `wreduce`, `num_clb` and the adder count drift for reasons
unrelated to the mapping.

| Delivered by 3G | Where |
|---|---|
| The pass `wreduce` with Yosys's options `-memx`, `-keepdc`, `-mux_undef` | `src/passes/opt/wreduce.c` (+ `wreduce_ff.c`) |
| Its place in the Phase 3 script (§9) | `docs/PASSES.md`, `tools/opt-check/phase3.script` |
| Per-rule goldens, refinement after the call, the same-input differential, the `sx_add` repro | `tests/opt/rules/wreduce/`, `tools/opt-check/` (extended, not forked) |

Not in 3G: `peepopt`, `share`, `memory*`, `opt_dff -sat`; the cells Yosys's list has but the
registry does not (`$pos`, `$sshl`, `$shift`, `$eqx`, `$nex`, `$divfloor`, `$modfloor`,
`$memrd_v2`, `$memwr_v2`, `$meminit*`); Yosys's final wire-narrowing step (WR-8, divergence 3).

## 2. How Yosys's concepts map onto the IR (WR-2)

`wreduce` is a member of the opt family: it uses 2F's kit (`src/passes/opt/kit.[ch]`) unchanged.

| Yosys | Odin III |
|---|---|
| `ModIndex mi`; `mi.sigmap(bit)` | the bit's net (nets are one bit, IR-1; no SigMap) |
| `mi.query(bit)->ports` (how many cell ports touch the bit) | the net's pin count (driver and sinks) |
| `info->is_output` / `is_input` | the net is on an `output`/`inout` (resp. `input`/`inout`) port wire bit |
| `keep_bits`: bits of `(* keep *)` wires | nets of wires whose `keep` attribute is set |
| `FfInitVals` | 2E-1's `src/passes/ffinit.[ch]` (`odin3_ffinit_get`, `odin3_ffinit_remove`) |
| `module->connect(a, b)` | `kit_connect(keep, drop)`: the dropped net's names become aliases (IR-2) |
| `cell->setPort` + `fixup_parameters()` (narrowing) | `kit_rebuild_node`: a new node with the new widths and fresh output nets, then the old output nets that survive are merged back; the removed output nets are connected as the rule says |
| `module->remove(cell)` | the node is killed; its output nets keep their aliases (IR-6: no compaction, PHASE2 #19) |
| `State::Sx` / `S0` | the canonical x and 0 constant nets (`$_CONSTX_`, `$_CONST0_`) |
| `log("Removed top …")` | the DEBUG lines of §11 and the `--opt-stats` counters |

## 3. The pass (WR-3): what runs, in order

Per module, modules in creation order. A module that still has `$proc` nodes is **skipped** with
the info line of §11 (Yosys: `has_processes_warn`; `wreduce` registers `processes = OK` like the
opt passes and skips such a module itself, as `opt_muxtree` does).

1. **Pre-step, one sweep in node-ID order** (Yosys `execute`, before the worker):
   1. **One-bit results.** `$reduce_and $reduce_or $reduce_xor $reduce_xnor $reduce_bool $lt $le
      $eq $ne $ge $gt $logic_not $logic_and $logic_or` with `Y_WIDTH > 1` and no constant Y bit:
      Y keeps bit 0, `Y_WIDTH := 1`, and Y[1..] are connected to 0. (Yosys's list also has
      `$eqx`/`$nex`, which the registry lacks.)
   2. **`$div $mod $pow`**: trim A, then B. Unsigned port: drop top bits while the top bit is the
      constant 0. Signed port: drop the top bit while it **and the bit below it** are both 0 (one 0
      stays to carry the sign). The port's own flag decides here: for `$div`/`$mod` under PHASE2
      #25 the flag is the cell's both-signed value (WR-6); for `$pow`, A_SIGNED and B_SIGNED are
      each their own operand's flag (IR-8).
   3. **Memory address bits**, unless `-memx`: a `$mem` with `OFFSET >= 0` and `ABITS >
      ceil_log2(OFFSET + SIZE)` gets `ABITS := ceil_log2(OFFSET + SIZE)`, and every read and write
      port's address keeps its low `ABITS` bits. (Yosys does this per `$memrd`/`$memwr`; Odin III's
      `$mem` is the v1 form with one `ABITS`, 2D EL-27, so the narrowing is per memory. The result
      is the same after Yosys's `memory_collect`.) `ceil_log2(1) = 0`, so a one-word memory gets
      `ABITS 0` as in Yosys; the `$mem` registry entry must accept `ABITS 0` (WR-9, an IR question).
2. **The worker** (Yosys `WreduceWorker::run`):
   1. Collect `keep` nets; load init values.
   2. The work list is every node of the module, **in rank order** (WR-7, amended 2026-10-19:
      operand before consumer; the rank of a node is its place in `wreduce_order.c`'s post-order).
   3. Loop: clear the changed-net set; run §4's rule for each listed node in order; then the next
      work list is every node with a pin on a changed net (deduplicated, rank order). Stop when the
      list is empty.

The worker never recurses: Yosys's `run_cell` calls itself when it changed the cell; Odin III
loops on the same node until a pass over it changes nothing (bounded by its port widths).

## 4. Rules per cell (WR-4)

The supported types, each with the Yosys function that handles it:

| Types | Rule |
|---|---|
| `$mux $pmux` | §4.1 `run_cell_mux` |
| `$dff $dffe $adff $adffe $sdff $sdffe $sdffce $dlatch $adlatch` | §4.2 `run_cell_dff` |
| `$not $neg $and $or $xor $xnor $shl $shr $sshr $shiftx $lt $le $eq $ne $ge $gt $add $sub $mul` | §4.3 `run_cell` |

Every other type is untouched (`$div $mod $pow` only in the pre-step).

### 4.1 `$mux` / `$pmux`

Skip when any Y bit is on a constant net. From the top Y bit down:
- a Y bit that is **unused** (its net has only this pin, is not on an output port and is not
  `keep`) is removed and replaced by x;
- otherwise, if every data input agrees on that bit (A's bit and each B word's bit are the same
  net, or, with `-mux_undef` and without `-keepdc`, the same net apart from x bits), the bit is
  removed and replaced by that common net;
- the first bit that is neither stops the scan.

If every bit went, the cell is deleted and Y's nets connect to the replacements; otherwise the
cell is narrowed to its low bits and the removed Y nets connect to the replacements. The removed
A, B and Y nets join the changed set.

### 4.2 Flip-flops and latches

`D`, `Q`, init value `I` (per bit) and reset value `R` (`ARST_VALUE` or `SRST_VALUE`, if any).
`zero_ext` is true when D's top bit is the constant 0, `sign_ext` otherwise. From the top bit `i`
down:
1. `zero_ext`, `D[i] = 0`, `I[i]` is 0 (or x without `-keepdc`), and (no reset, or `R[i]` is 0, or
   x without `-keepdc`): Q[i] connects to 0, its init is removed, bit `i` goes;
2. `sign_ext`, `i > 0`, `D[i]` and `D[i-1]` are the same net, `I[i] = I[i-1]` (and with `-keepdc`
   not x), `R[i] = R[i-1]` likewise: Q[i] connects to Q[i-1], bit `i` goes;
3. Q[i] unused (as §4.1): its init is removed, bit `i` goes, and both `zero_ext` and `sign_ext`
   become false;
4. otherwise stop.

A cell with no bits left is deleted; otherwise it is narrowed (reset value truncated) and its D
and Q nets join the changed set.

### 4.3 Operators (`run_cell`)

Skip when any Y bit is on a constant net. Let `Wy` be Y's width, and `MA`, `MB` the current A and B
widths (−1 when absent).

1. **Port limits.** For `$not $neg $and $or $xor $add $sub`, `MA := min(MA, Wy)` and `MB := min(MB,
   Wy)` (bits of an operand above the result are invisible).
2. **Free choice of signedness** (`$mul $add $sub` with `MA = MB = Wy`): with A and B taken to `Wy`
   bits, compute the cost of each reading: for `$mul` the product, for `$add`/`$sub` the maximum,
   of each operand's reduced size, where the signed size drops repeated top bits and the unsigned
   size drops top constant-0 bits. If the signed cost is smaller, set `A_SIGNED = B_SIGNED = 1`.
   (Yosys's opposite branch, signed → unsigned, can never fire: it tests local flags that are still
   false at that point. Odin III mirrors this, divergence-free; §12 #6.)
3. **Trim A then B** (`run_reduce_inport`; A is skipped on `$shiftx`): first cut the port to its
   limit; then, if the port is signed, drop the top bit while it equals the bit below; if unsigned,
   drop the top bit while it is the constant 0 (one bit always stays). The port's signedness is
   WR-6's, and B of `$shl $shr $sshr` is always unsigned. The removed nets join the changed set.
4. **Signed to unsigned.** A binary cell whose ports are both signed and whose A and B top bits are
   both the constant 0 becomes unsigned; a unary signed cell whose A top bit is 0 likewise.
5. **Trim Y from the top** while the top Y bit is unused (as §4.1), except on a `$shr` with a signed
   A (Yosys keeps those).
6. **Cap Y** for `$add $sub $mul $and $or $xor` (and Yosys's `$pos`): the largest meaningful width is
   `max(|A|, |B|)` (+1 for `$add`/`$sub`; `|A| + |B|` for `$mul`; at least 1). Y bits above it are
   removed and connected to the **pad bit**: the new top Y bit when the cell is signed
   (WR-6) or is a `$sub`, else 0.
7. If Y is empty, the cell is deleted. If anything changed, the node is rebuilt with the new widths
   and flags and §4.3 runs again on it.

## 5. The signed rewrite and why 3C must sign-extend (WR-5)

Rule 2 of §4.3 is the "signed rewrite": an unsigned `$add` whose operands are explicit sign
extensions (`{x[8], x[8], x}` in an 11-bit context) is cheaper read as signed, so it becomes a
signed `$add` and rule 3 then drops the repeated sign bits: `A_SIGNED=1, A_WIDTH=10, B_WIDTH=9,
Y_WIDTH=11` in stereovision0's `combine_res` (2H splice report). The rewrite is sound: a signed
cell sign-extends its operands to Y's width, which is exactly what the dropped bits did.

What is not sound is Parmys's adder lowering after it: `split_adder` (`parmys-plugin/core/adder.cc`,
the "connect those pins to unconn" loop at line 700) pads the shorter operand of a chain with
`unconn` (0), whatever A_SIGNED says; `reduce_input_ports` pads `$sub` operands the same way. So the
golden is IEEE-wrong wherever `wreduce` produced a signed narrow adder. `exit-repros/sx_add/sx_add.v`
shows it: y[9], y[10] wrong in 153 of 298 cycles against iverilog, in Parmys's netlist.

**Ruling (Peter, 2026-10-14): 3G is faithful; 3C sign-extends.** 3G produces exactly Yosys's cells,
signed rewrite included. 3C extends every signed operand by its sign bit (3C spec §5). The O/P
comparison therefore legitimately differs on designs with signed narrow adders. The survey of the 19
(3C spec §2) counts them after Parmys's `wreduce`:

| design | signed `$add` | signed `$sub` |
|---|---|---|
| `stereovision0` | 170 of 244 | 0 of 11 |
| `stereovision1` | 50 of 202 | 0 of 24 |
| `stereovision2` | 102 of 496 | 12 of 18 |
| `LU8PEEng`, `LU32PEEng` | 1 of 104, 1 of 248 | 1 of 300, 1 of 996 |
| `mcml` | 1 of 146 | 0 of 261 |
| the other 13 | 0 | 0 |

Every one of those signed cells has an operand narrower than Y, so every one is a site where
Parmys drops the extension. None is mixed-sign.

## 6. Mixed-sign cells (PHASE2 #25) (WR-6)

A binary word cell of the 15 ruled types (`$add $sub $mul $div $mod $and $or $xor $xnor $lt $le $gt
$ge $eq $ne`) with `A_SIGNED != B_SIGNED` is unsigned (IR-8). Yosys's `wreduce` reads each port's own
flag (`run_reduce_inport`) and the cap pad reads `A_SIGNED` alone (`is_signed =
A_SIGNED || $sub`), which on a mixed cell trims a signed-looking operand as signed: unsound under
simlib's both-signed rule, in Yosys too.

**Odin III:** every place Yosys reads a flag of a ruled type reads `odin3_opt_both_signed(cell)`
instead: rule 3's port signedness, rule 4's test, rule 6's pad, the pre-step's `$div`/`$mod`
trimming. Rule 2 may set both flags on a mixed cell (both become 1: the cell is no longer mixed,
and since A, B and Y have one width the operands' extension does not matter). `$shl $shr $sshr`
ignore B's flag, as in Yosys; `$shiftx` and `$pow` read B_SIGNED as the amount's (exponent's) own
flag (IR-8). Every rule that builds or rebuilds a ruled cell gives it equal flags. The first visit
of a mixed cell logs 2F's warning (§16 there, unchanged text with `wreduce` as the pass name). The
Verilog front end never emits a mixed cell (PHASE2 #25 evidence), so this is a divergence on
hand-built or plugin IR only (§12 #2).

## 7. Determinism and work order (WR-7)

Yosys's `work_queue_cells` is a `std::set` ordered by `IdString` index, i.e. by when the cell's name
was first interned. The fixed point is **order dependent**: a narrowing connects nets that a later
cell in the same round then reads (the stereovision0 rewrite needs the first adder's result to
show bits 9 and 10 equal before the second adder is visited, which is why Odin III's and Yosys's
own front ends rewrite different adders there).

**Amendment (Peter, 2026-10-19; PHASE3 #51).** Odin III no longer orders by node ID. The elaborator
(2D) numbers a consumer before its operand, Yosys's front end creates an expression bottom-up, so an
operand's cell has the lower `IdString` index. The cases where the order matters are the
rule "free to choose the signedness" (Yosys `wreduce.cc` 316) and any other rule that reads a net an
earlier visit in the same round has just narrowed: `LUControl`'s
`counter == mdivk-1+5-2` gives `$add A3:B4:Y5` and `$sub A5:B3:Y6` (signed) in Yosys, and
`$add A32:B3`, `$sub A31:B1` in node-ID order (53 more adder stages in LU8PEEng and LU32PEEng).

The rule: **each node has a rank, and a round is sorted by (rank, node ID).**
1. *Initial rank.* The position of the node in a depth-first post-order of the net graph of its
   module (`src/passes/opt/wreduce_order.c`). Nodes are entered in ascending node-ID order; from a
   node, the driver of each input pin is entered first. A node is entered once.
2. *Pin order.* Ports in registry order, except `$mux`, which reads S, B, A (Yosys's ternary,
   `genrtlil.cc:1904`: the condition, then the true value, then the false value; its other binary
   and unary cells read A before B, left operand first); bits of a port LSB first (bit 0 first).
   The driver followed on a net is its first driver.
3. *Which nets are followed.* Only a net without a wire (an expression temporary). A net with a
   wire is a statement boundary, where Yosys's order is the order of the statements, which is node-ID
   order: a forward reference (the consumer's statement before the producer's) stays consumer first,
   as in Yosys (micro `tests/opt/order_forward/forward_reference.v`).
4. *Cuts.* Flip-flop, latch and `$mem` outputs are not followed (Yosys's front end creates a
   register after the logic of its D input, and the cut keeps every feedback loop out of the walk). A
   combinational loop is cut where the walk meets a node already entered. The cut types are the one
   list `wr_order_is_source`.
5. *Rewrites.* A rebuilt node inherits the rank of the node it descends from (unchanged from the
   earlier WR-7); a node that has no rank of its own (never made by `wr_rebuild`) has the rank
   UINT32_MAX, so it sorts after every ranked node, by node ID.
6. *Determinism.* No hash order reaches the IR; two runs give byte-identical `write_rtlil`. The walk
   uses an explicit stack (the IR can be 2M nodes deep), allocates one byte per node and the stack
   (counted in the module budget), and is linear in nodes plus pins. An allocation failure is
   `ODIN3_ERR_NO_MEMORY`.

**Known approximations** (the walk reproduces Yosys's creation order for an expression tree written
in statement order; these cases it does not, and they are not tolerances: each is measured by the
hardmap-diffs of LU8PEEng, LU32PEEng and mcml against the Parmys goldens, PHASE3 #51):
- *Cells made by `proc`* (`$mux`, `$pmux`, `$dff`, `$mem`). Yosys creates them after every front-end
  cell; the walk ranks them like any other node, so a `proc` `$mux` that feeds arithmetic goes before
  that arithmetic. Odin III's IR does not say which nodes `proc` made (no pass tag on a node), so
  this is not modelled.
- *A shared operand* is placed by the first consumer the walk enters; Yosys creates it at its first
  use in source order. They agree when node-ID order is source order; `opt_merge` keeps the cell
  with the lower ID, which is the first use.
- *Name interning is design-wide in Yosys.* In a file with several modules the order of module 2 is
  the order in which the names were first seen in the whole file; Odin III's `$c<ID>` names repeat
  from module to module. This affects only a file that Yosys reads (`opt-diff`'s oracle input), not
  Odin III.
- *Statement order across `always` blocks and `generate`* follows node-ID order, that is the
  elaborator's order, which is source order for the cases measured but is not specified.

On a `write_rtlil` file the cells are in node-ID order, not rank order. `opt-diff` hands Yosys a copy
of Odin III's file with the cells in rank order (`tools/opt-check/il_order.py`, the same walk in
Python over the cell table `tools/opt-check/il_celltypes.txt`, which the unit test
`test_il_celltypes` keeps equal to the registry and to the C file; an unknown cell type or port is an
error, exit 2). **That gate is circular on the order**: both sides use the same walk, so
`opt_diff_wreduce_order` shows that they agree and that wreduce refines Yosys's result for that order.
Fidelity to Yosys's front end is measured only by hardmap-diff against Parmys. Any difference that
remains is a finding, not a tolerance.

## 8. Wire narrowing (WR-8)

Yosys ends by giving an unused-top-bits wire a narrower replacement and swapping names. In Odin III
a wire is a name over per-bit nets (IR-1); unused nets cost nothing and `opt_clean` handles dead
cells, so the step is not ported (§12 #3). Names never reach `netlist-compare`.

## 9. Position in the pipeline (WR-9)

Parmys's lines (2F §3 table) around `wreduce`:
`fsm; opt; wreduce; peepopt; opt_clean; share; opt -full; memory -nomap; flatten; opt -full;
techmap …2dff; opt -full; parmys`. The opt family is not confluent (2F §3 evidence), so Odin III
keeps the **same relative order**, with the passes it does not have dropped:

```
read_techlib lib/vtr.o3lib
read_verilog --nolatches --vtr-compat [--lib vtr_primitives.v] <files>
opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt          # Phase 2 (2F OP-3)
wreduce; opt_clean; opt -full                                 # 3G (peepopt, share: Phase 4)
flatten; opt -full                                            # 3A (memory -nomap: none, WR-9.2)
hardmap; opt -full                                            # 3C
lower; abc; write_blif --vtr                                  # 3B, 3D, 3E
```

1. **Before `flatten`**, as Parmys: per-module `wreduce` treats port bits as used, so it narrows
   less than it would after `flatten`, and that is what Parmys's counts reflect. Running it after
   `flatten` instead would be a QoR choice for Phase 4, not parity.
2. **`memory -nomap`** has no Odin III counterpart. The survey (3C §2) found **no** `$mem_v2`
   outside the `single_port_ram`/`dual_port_ram` primitive modules in any of the 19: every RAM
   there is an explicit instance. The pre-step's address rule therefore matters only for micros.
3. **After `hardmap`, not before `lower` alone**: the cells 3C creates are hard; `opt -full` after
   it is Parmys's line 11.
4. PHASE3's exit script (`…; flatten; [wreduce;] hardmap; …`) is amended to this order (§13).

## 10. Correctness, memory, provenance (WR-10)

- **Refinement** (2F OP-13): every `wreduce` call refines its input (rule 1 of §4.2 and the x
  replacements of §4.1 resolve x; `-keepdc` makes the FF rules exact). Checked per call (§10.2 of
  the plan's Task 3, 2F's `opt-equiv`).
- **OOM.** Each rewrite builds everything before its first destructive step: the new node, fresh
  nets and the connection list are allocated first; a `NO_MEMORY` there leaves the node unchanged
  and the IR valid (2F IR discipline). The design budget (`odin3_budget_check`, 60%) is checked at
  the start of each module and every 2^16 created pins. Side arrays (net pin counts, keep marks,
  the changed set as a bitmap, the next work list) are `util/vec`, sized at the store length and
  freed after each module.
- **The dirty set.** `wreduce` is a pass of its own (a full first sweep per module), but it marks
  every net it connects or re-drives in the opt session's dirty set, so the `opt -full` after it
  starts `opt_expr` incrementally (2F §19).
- **Caps.** `ODIN3_WREDUCE_MAX_ROUNDS` = 1,000,000 rounds per module (each round removes at least
  one bit, so the real bound is the module's total port width; the cap is a safety net, warned,
  `ODIN3_OK` with valid IR, as 2F OP-12).
- **Provenance** (OP-15): a narrowing is one operation; the rebuilt node gets a DERIVED record with
  the old node as parent; connections are rewiring (IR-15); a deleted node keeps its record.

## 11. Diagnostics (WR-11)

2F §16 style. INFO `wreduce: N cells narrowed, M bits removed, K cells removed, S cells made
signed, U cells made unsigned` per call; INFO `wreduce: module 'm' has processes; skipped`; DEBUG
per rewrite: `wreduce: module 'm': removed top N bits (of W) from port P of <type> '<cell>'`,
`… from mux cell …`, `… from FF cell …`, `converting <type> '<cell>' from unsigned to signed` (and
back), `removed <type> '<cell>'`, `removed top N address bits (of W) from memory '<cell>'`; WARN the
mixed-sign line (§6) and the round cap. `--opt-stats` gains `wreduce.<rule>` counters, one per row
of §4 and the pre-step. Options `-memx -keepdc -mux_undef` as Yosys; an unknown one is `<pass>:
unknown argument '<arg>'`.

## 12. Divergences (from Yosys 0.55)

| # | Odin III | Yosys 0.55 | Why / effect |
|---|---|---|---|
| 1 | work order = rank: a post-order of the net graph, operand first, anonymous nets only (amended 2026-10-19) | `IdString` interning order (the front end creates operands first) | determinism without names (WR-7); reproduces Yosys's order for expression trees in statement order; known approximations: cells made by `proc`, shared operands, design-wide interning, statement order (section 7) |
| 2 | mixed-sign ruled cells read the both-signed flag everywhere | each port's own flag; pad by `A_SIGNED` | PHASE2 #25; Yosys is unsound there; the front end never makes such cells |
| 3 | no wire narrowing | narrower wire + `swap_names` | names only (WR-8) |
| 4 | `$mem` `ABITS` narrowed once per memory | per `$memrd`/`$memwr`/`$meminit` | v1 `$mem` form (2D EL-27); same after `memory_collect` |
| 5 | cells outside the registry are not ported | `$pos $sshl $shift $eqx $nex $divfloor $modfloor $*_v2` | not in Odin III's IR |
| 6 | the signed → unsigned branch of the free choice is dead, as in Yosys | dead code | mirrored, not "fixed": keeps cell-for-cell identity |
| 7 | loop instead of `run_cell` recursion | recursion | no recursion in `src/` |
| 8 | round cap 1,000,000 | none | safety net |
| 9 | Odin III log text | Yosys text | — |

Parmys-level divergence that 3G causes downstream: none of its own. The signed-adder difference is
3C's (3C §5, divergence D1), by ruling.

## 13. Amendments to other documents (WR-12)

- `docs/PHASE3.md` exit item 3: the script becomes §9's.
- `docs/PASSES.md`: one row, `wreduce`.
- **IR question (Peter, CLAUDE.md "stop and ask"):** the `$mem` registry entry's `ABITS` minimum.
  Yosys allows `ABITS 0` for a one-word memory. Recommendation: allow 0 in `src/ir/cells/mem.c`
  (port widths become 0), since 2D can already emit a one-word `reg [7:0] m [0:0]`. Until ruled,
  3G keeps `ABITS >= 1` and lists it as divergence 10.

## 14. Review focus and risks

1. **Order dependence** (WR-7). Pinned by a golden that needs two rounds and a cascade, and by the
   differential on the 19 (`stereovision0` is the known order-sensitive design).
2. **Unused-bit test** (`ports <= 1`, `is_output`, `keep`). A wrong count removes a used bit:
   refinement after every call catches it.
3. **FF init and reset widths.** Truncating `ARST_VALUE`/`SRST_VALUE` with the narrowed Q, init
   removal on removed bits.
4. **The mixed-sign departures** (WR-6): unit tests with hand-built mixed cells.
