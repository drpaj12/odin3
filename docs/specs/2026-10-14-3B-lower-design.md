# 3B — `lower`: design

Status: v1 (2026-10-14), **approved by Peter 2026-10-14** (all recommendations; PHASE3 #32). Phase 3, sub-project 3B (PHASE3
sub-projects table; light review, with Peter on decision 19; Opus `xhigh` → `high`). Written by
the lane-2 agent. Spec: `docs/DESIGN.md` §5.2 (cell sets), §5.4 (views), §6 step 9, §12 (Phase 3
exit). Inputs: `docs/IR.md` IR-1, IR-2, IR-4, IR-6, IR-8, IR-9, IR-12, IR-13, IR-15, IR-16, IR-18,
IR-18a, IR-19; PHASE2 #25 (the both-signed rule); the 2D spec (EL-14, EL-27, EL-39, EL-40, EL-48);
the 2E spec §13 (IR change D); the 2F spec (OP-3, OP-13, OP-14, OP-19, §17, §20); the 3A spec (FL-2,
FL-17, §14); the 3E spec (E10 and the constant convention); PHASE3 decisions 3, 4, 6, 7, 16, 17,
19, 28 and 29 (29 is on branch `docs/phase3-decisions`, PR in flight; this spec cites its rulings
3, 4, 5 and 8). **Oracle: Yosys 0.55 as pinned by VTR** (`$VTR_ROOT/libs/EXTERNAL/yosys`,
`60f126cd`; binary `$VTR_ROOT/build/bin/yosys`): `passes/techmap/simplemap.cc`,
`share/techmap.v`, `passes/techmap/alumacc.cc`, `maccmap.cc`, `dffunmap.cc`, `kernel/ff.cc`,
`passes/memory/memory_map.cc`; and Parmys's `parmys/parmys-plugin/techlibs/{adff2dff,adffe2dff,
aldff2dff,aldffe2dff}.v` plus `vtr_flow/misc/yosys/synthesis.tcl` at VTR `3c9a4d23`. Decisions are
numbered **LW-n**. Where this file and `docs/IR.md` disagree, IR.md wins. Every default that is
not Peter's is marked **agent default (Peter may override)**; §18 lists what is Peter's.

## 1. Purpose and scope (LW-1)

`lower` turns the word-level cells that are left after `proc`, `opt` and (in the Phase 3 script)
`hardmap` into bit-level cells, so that the module satisfies the **netlist view** (DESIGN §5.4,
IR-9): every live node is `bit`, `hard`, `blackbox`, `module` or `port`. It is DESIGN §6 step 9.
It is the step between `hardmap` and `abc` in the Phase 3 exit script (PHASE3 exit test item 3).

| Delivered by 3B | Where |
|---|---|
| The pass `lower` and its options | `src/passes/lower/*.c` |
| A rule per registry word cell (§4–§8): bitwise and logic cells, `$eq`/`$ne`, `$mux`, `$pmux`, shifts, `$neg`/`$add`/`$sub`, comparisons, `$mul`, `$div`/`$mod`; the whole word FF and latch family; `$mem` | §4–§8 |
| The async-to-sync reset conversion of decision 19, with its warning and report | §6 |
| Evidence: unit tests, per-cell goldens, a same-input differential against Yosys 0.55, simulator-equal before/after, refinement checks, fuzzing, determinism and out-of-memory tests | §14, `tools/lower-check/` |

**Not in 3B.**
- Hard mapping: `$mul` to `multiply`, `$add`/`$sub` to `adder` chains, `$mem` to RAM slices. That is
  3C. `lower` knows no architecture and no `.o3lib`; it lowers whatever word cell it finds (§2).
- Any optimisation. `lower` copies Yosys's structures with their constants in place, as `simplemap`
  and `techmap.v` do; `opt` cleans up afterwards (§2.2). `wreduce` is 3G, `flatten` is 3A.
- `$sop` and `$lut`. They are `bit` cells already (DESIGN §5.2) and are left alone. Yosys's
  `techmap` expands them into gates (`_90_lut`); here a `.names` cover is already what ABC and the
  BLIF writers want (§16 row 3).
- Cells the registry does not have: Yosys's `$alu`, `$lcu`, `$fa`, `$macc`, `$pos`, `$sshl`, `$shift`,
  `$eqx`, `$nex`, `$bmux`, `$bwmux`, `$divfloor`, `$modfloor`, `$demux`, the fine FF family
  `$_DFFE_*`, `$_SDFF*_`, `$_DFF_PN0_` …, and `$_TBUF_`. Where a Yosys structure goes through one
  of them (`$alu` inside `$add`), `lower` builds its bit-level result directly (§3.2).
- Anything in `src/ir/` (no IR change; §15).

**Success.** Every rule of §4–§8 has a unit test and a golden; on every micro and on the hand-built
corpus, the lowered module is simulator-equal to the original where the original can be simulated
(§14.3) and is equivalent to Yosys 0.55's own lowering of the same input otherwise (§14.2, §14.4);
the netlist view holds after the pass on every run; two runs are byte-identical; the gate passes.

## 2. Where it runs (LW-2)

The Phase 3 script (PHASE3 exit test item 3, with 3A's and this spec's recommended additions marked
`+`):

```
read_verilog --nolatches; proc; opt; flatten; +opt; [wreduce;] hardmap; lower; +opt -fast; +compact; abc; write_blif --vtr
```

**Ordering rules** (each is enforced or checked by something, in the right-hand column):

| # | Rule | Enforced by |
|---|---|---|
| O1 | `proc` before `lower`: a `$proc` node has no bit form | `lower` is a `REFUSE` pass: `pass 'lower': module 'm' has processes; run proc first` (2D EL-33) |
| O2 | **`hardmap` before `lower`.** A hard cell (`multiply`, `adder`, `single_port_ram`, `dual_port_ram`, any `.o3lib`/`.blackbox` cell) is `hard` or `blackbox`: `lower` never opens, rewrites or counts it as an operand. A word `$mul`/`$add`/`$sub`/`$mem` that `hardmap` left soft (below the thresholds, wider than 36, a read that cannot become synchronous; decisions 4 and 6) is lowered here, to soft logic | the granularity test in §3.1; `lower` logs `lower: lowered N $mul, M $add, K $mem as soft logic` (INFO) so a script that forgot `hardmap` shows it at once; a script test (`cli_lower_after_hardmap`, Task 9) runs the Phase 3 script on a hand-built design with a `$mul` and a `$mem` and asserts that the hard cells survive and the leftover soft ones are gone |
| O3 | `wreduce` (3G) before `hardmap`, so before `lower`. `lower` does not need it (it works at any widths, including the mixed-width signed cells 3G leaves) but it lowers fewer gates when widths were narrowed first | none needed |
| O4 | `flatten` (3A) is independent. `lower` treats each module separately, and instances (`module` granularity) are left alone, so `lower` is correct on a hierarchical design; 3E and `abc` need the flat one | none needed |
| O5 | `abc` (3D) after `lower`: the `abc` pass reads the netlist view. If a word cell is still live it is 3D's error to report, not `lower`'s to prevent | 3D's view check (IR-9) |
| O6 | `lower` after `opt`, and an `opt -fast; compact` after `lower` (§2.2) | the script |

### 2.1 `lower` on a design with no word cells

It is a no-op that still takes a pass run and the `check` before and after. `lower` on a netlist read
by `read_blif` changes nothing and logs `lower: nothing to lower`.

### 2.2 `opt -fast; compact` after `lower` (agent default; Peter's question 2)

Parmys runs `techmap; opt -fast` and `dffunmap; opt -fast -noff` after it maps (synthesis.tcl). Yosys
`techmap` also cleans some structures inside its own maps (`opt_muxtree; opt_expr -mux_undef
-mux_bool -fine` in the shift maps). `lower` here does neither: it emits Yosys's gates with their
constant inputs (`$alu` with `CI = 0`, a shift stage on a constant bit, `Y[k] = 0` for the upper bits
of a reduction). Without a cleanup those go to ABC, which removes them but spends memory and time on
them (the 19 designs have cells with millions of bits: PHASE3 risk 8, decision 17). 2F's `opt` passes
already take bit gates and `$_DFF_*` (2F spec §4.2, §9). So the recommended script adds `opt -fast;
compact` after `lower`. This edits PHASE3's exit test item 3 and the 3H spec and plan script
strings; Task 9 makes the edits if Peter agrees. If he does not, `lower`'s output is still correct;
only ABC's input is larger.

## 3. How a word cell becomes bit cells (LW-3)

### 3.1 What is lowered

`lower` visits every module of the design in module-ID order, and in each module every live node in
ID order (IR-16). For each node it looks at the cell type's **granularity** (IR-9):

| Granularity | Action |
|---|---|
| `word` | lowered by the rule for its built-in type (§4–§8); a word type with no rule (a plugin's) is `lower: no lowering rule for cell type '<t>' ('<cell>')` (`ODIN3_ERR_INVALID_ARG`) |
| `bit`, `hard`, `blackbox`, `module`, `port` | untouched; `$_CONST*_`, `$sop`, `$lut`, `$_BUF_` and `$_DFF_*` are `bit`, so a netlist read by `read_blif` passes through unchanged |

`$proc` is `word` but cannot reach the pass (O1). The check in §12 asserts the view afterwards.

### 3.2 Shape of every rule

Every rule is a **builder**: a straight-line function over small vectors of net IDs (the operand
bits, widened per §3.3) that appends bit nodes and returns the output bits. There are no temporary
word cells and no recursion (CLAUDE.md "no recursion in IR traversals"). Yosys's structures that go
through `$alu`, `$lcu`, `$fa` or `$pos` (`$add` is `$alu`, which is `$lcu`) are written as builders
that call each other: `alu_build` calls `lcu_build`; `lt_build` calls `alu_build`. Each builder is
small (CLAUDE.md: ≤ 60 lines, complexity ≤ 15) and tested alone against a closed-form node count.

The nodes of one word cell are built on **fresh nets**, then committed (§12.2): the word node is
deleted, and each of its output nets is merged with the net the builder returned for that bit
(`kit_connect`'s rule in the 2F spec OP-2: `odin3_net_merge(keep = the builder's net, drop = the Y
net)`, so the output net keeps its name, its wire membership and its sinks, and a constant source
is the module's canonical constant net, IR-4). A bit that Yosys would `connect` instead of
gating (a 1-bit reduction, `Y[k] = 0` above bit 0) is that merge and no node. `lower_connect` does
this in `src/passes/lower/connect.c`; it mirrors the 2F kit and does not call it (the kit is private
to `src/passes/opt`; a shared helper is a refactor for later, §16 row 22).

### 3.3 Extension and the both-signed rule (LW-4)

Yosys's `sig.extend_u0(width, signed)`: widen with the top bit (signed) or `0` (unsigned), or
truncate to `width`. `lower` does the same on a vector of nets. The **signedness it uses is the
registry's, not `calc.cc`'s** (IR-8, PHASE2 #25, Peter's ruling of 2026-10-13): for a binary cell of
the 15 ruled types (`$and $or $xor $xnor $lt $le $gt $ge $eq $ne $add $sub $mul $div $mod`; `$pow`
and the shifts read their own flags) both operands are extended as signed **only when both
`A_SIGNED` and `B_SIGNED` are 1**; otherwise both are zero-extended.

| Cell | Operand extension | Source of the flag |
|---|---|---|
| unary `$not`, `$neg` | `A` to the result width | `A_SIGNED` |
| ruled binary cells | `A` and `B` to the working width | `A_SIGNED && B_SIGNED` |
| `$shl $shr $sshr` | `A` to the working width; `B` is never extended (an amount) | `A_SIGNED` (sign fill only for `$sshr`) |
| `$shiftx` | `A` padded with x; `B` read as signed when `B_SIGNED` | `B_SIGNED` |
| `$mux $pmux $reduce_* $logic_*` | none (equal widths, or width-free) | — |

A mixed cell (`A_SIGNED != B_SIGNED`) is lowered as unsigned and warns once per cell: `lower: module
'm': cell '<type>' '<cell>' has A_SIGNED != B_SIGNED: treated as unsigned (both-signed rule, Yosys
simlib; PHASE2 #25)` (the text of 2F's warning, with `lower:` for `opt:`). The Verilog front end
never makes one; 3G's `wreduce` makes mixed-*width* signed cells, not mixed-sign ones (decision 29,
ruling 4), and those are an ordinary signed cell here. Where Yosys's `simplemap`/`alumacc` would
extend per operand, `lower` differs on a mixed cell; it is divergence #1 of §16, and a test pins it
for every one of the 15 types.

Widths follow the cell's `*_WIDTH` parameters and its `Y` port width; no port has width 0 (IR-1),
so the `extend_u0`-of-nothing corners of Yosys do not arise.

## 4. Gates and comparisons: the `simplemap` family (LW-5)

These rules are `simplemap.cc` transcribed. The gate-for-gate **structure** is the contract
(tier S of §14.2): the same gate types, the same pairing order, the same number of gates.

| Cell | Result (`Y` is `Y_WIDTH` wide) | Yosys |
|---|---|---|
| `$not` | `Y[i] = $_NOT_(ext(A)[i])` for all i | `simplemap_not` |
| `$and $or $xor $xnor` | `Y[i] = $_AND_/_OR_/_XOR_/_XNOR_(ext(A)[i], ext(B)[i])`; both operands extended to `Y_WIDTH` | `simplemap_bitop` |
| `$mux` | `Y[i] = $_MUX_(A[i], B[i], S)`; `S` is the one select net | `simplemap_mux` |
| `$reduce_and`, `$reduce_or`, `$reduce_xor`, `$reduce_bool` | a pairing tree: while more than one signal is left, pair adjacent signals `(0,1), (2,3), …` with `$_AND_`/`$_OR_`/`$_XOR_`/`$_OR_`; an odd last signal is carried up unpaired. The root drives `Y[0]`. `Y[k] = 0` for `k > 0` (merged with the `$_CONST0_` net) | `simplemap_reduce` |
| `$reduce_xnor` | the `$_XOR_` tree, then one `$_NOT_` as the root | likewise |
| `$logic_not` | the `$_OR_` tree of `A` (a one-bit `A` is itself), then `$_NOT_` into `Y[0]`; `Y[k>0] = 0` | `simplemap_lognot`, `logic_reduce` |
| `$logic_and`, `$logic_or` | `$_OR_` tree of `A`, `$_OR_` tree of `B`, then `$_AND_`/`$_OR_` into `Y[0]`; `Y[k>0] = 0` | `simplemap_logbin` |
| `$eq`, `$ne` | `W = max(A_WIDTH, B_WIDTH)`; per bit `$_XOR_(ext(A)[i], ext(B)[i])` (the `$xor` rule at width `W`); the `$_OR_` tree of those (the `$reduce_or` rule); `$ne`: the root is `Y[0]`; `$eq`: one more `$_NOT_` into `Y[0]`. `Y[k>0] = 0` | `simplemap_eqne` |

Details that decide gate counts, each with a test:
- A reduction of a **one-bit** `A` is a merge of `Y[0]` with `A[0]` and no node (`$reduce_xnor`: one
  `$_NOT_`). Zero-width `Y` or `A` cannot occur.
- The tree is **level by level**: level 0 pairs the operand bits in order; level 1 pairs the level-0
  results in order, and so on. The root of an odd-sized level keeps its place (carried bit appended
  after the pair results), exactly as `simplemap_reduce` appends `sig_a[i]` to `sig_t`.
- `$eq` and `$ne` extend with the both-signed flag (§3.3). Yosys's `simplemap_eqne` passes
  `A_SIGNED` only; the two agree on every cell the Verilog front end makes.
- `$tribuf` has no bit form in the registry (no `$_TBUF_`): a located error (§9, Peter's question 4).

## 5. `$pmux`, shifts, arithmetic, comparisons: the `techmap.v` family (LW-6)

The structure is Yosys 0.55's after its `techmap` (`share/techmap.v`, with `alumacc` and `maccmap`
for the arithmetic). The contract is **tier T** (§14.2): the same algorithm and the same cell-type
histogram after the common `opt` cleanup; gate-for-gate identity is a target, not a gate, because
Yosys runs `proc; opt_muxtree; opt_expr -fine` inside its shift and divider maps and because it
shares nothing across cells while Odin III's later `opt_merge` shares everything.

### 5.1 `$pmux` (`_90_pmux`)

`WIDTH = W`, `S_WIDTH = n`. Per select `i` and bit `j`: `B_AND_S[i][j] = $_AND_(B[i·W+j], S[i])`.
Per output bit `j`: the `$_OR_` tree (§4's reduction) over `B_AND_S[0..n−1][j]` gives `Y_B[j]`. One
`any = OR-tree(S)` is built once (the `$reduce_or` rule over the `n` select nets). Then `Y[j] =
$_MUX_(A[j], Y_B[j], any)`. With several selects high the result is the OR of the selected words
(Yosys's `techmap.v`; simlib says x there). The sim and refinement checks drive `$pmux` selects
one-hot or all zero, and treat the x as a don't-care (§14).

### 5.2 `$shl`, `$shr`, `$sshr` (`_90_shift_ops_shr_shl_sshl_sshr`)

`shift_left = (type == $shl)`; `sign_extend = (type == $sshr) && A_SIGNED`. `WIDTH = max(A_WIDTH,
Y_WIDTH)`; `BB_WIDTH = min(clog2(shift_left ? Y_WIDTH : (A_SIGNED ? WIDTH : A_WIDTH)) + 1,
B_WIDTH)`.
1. `overflow = OR-tree(B[BB_WIDTH..B_WIDTH−1])` (the constant 0 when `B_WIDTH == BB_WIDTH`).
2. `buffer[0..WIDTH−1] = overflow ? fill : ext(A, WIDTH, A_SIGNED)`, bitwise `$_MUX_(ext(A)[k],
   fill, overflow)` where `fill` is the sign bit `A[A_WIDTH−1]` if `sign_extend`, else 0.
3. For `i = 0 .. BB_WIDTH−1` (low to high): `buffer = B[i] ? shift(buffer, 2^i) : buffer`, one
   `$_MUX_` per bit with select `B[i]`. `shift` moves toward the high end for `$shl` (low bits
   filled with 0) and toward the low end for `$shr`/`$sshr` (high bits filled with 0, or with
   `buffer[WIDTH−1]` when `sign_extend`; a shift by `2^i ≥ WIDTH` fills everything).
4. `Y[k] = buffer[k]` for `k < Y_WIDTH`.

### 5.3 `$shiftx` (`_90_shift_shiftx`, the `$shiftx` case)

`extbit = x` (a `$_CONSTX_`), `a_padding = x`, `BB_WIDTH = min(clog2(max(A_WIDTH, Y_WIDTH)) +
(B_SIGNED ? 2 : 1), B_WIDTH)`, `WIDTH = max(A_WIDTH, Y_WIDTH) + (B_SIGNED ? 2^(BB_WIDTH−1) : 0)`. The
buffer starts as `{WIDTH{x}}` with `A` in its low `A_WIDTH` bits; a `B` wider than `BB_WIDTH`
turns the whole buffer to x when the extra bits are not all equal to the sign (signed) or not all
zero (unsigned); a signed `B[BB_WIDTH−1]` first shifts toward the high end by `2^(BB_WIDTH−1)`;
then the stages `i < (B_SIGNED ? BB_WIDTH−1 : BB_WIDTH)` shift toward the low end by `2^i`,
filling with x. This is the only rule that writes `$_CONSTX_` into a netlist; it is the "x" of the
x-bearing selects Verilog `[i +: w]` produces (2D EL-31). `write_blif` writes it as `unconn`.

### 5.4 `$alu` and `$lcu` (the kernel of `$add`, `$sub`, `$neg`, comparisons)

`alu_build(A, B, CI, BI, Y_WIDTH, s)` is `_90_alu`: `AA = ext(A, Y_WIDTH, s)`; `BB = BI ? ~ext(B,
Y_WIDTH, s) : ext(B, Y_WIDTH, s)` (`BI` is a build-time constant here, so a `$_NOT_` row or nothing);
`P[i] = $_XOR_(AA[i], BB[i])` (Yosys's `X`), `G[i] = $_AND_(AA[i], BB[i])`;
`CO = lcu_build(P, G, CI)`; `Y[i] = $_XOR_(P[i], i == 0 ? CI : CO[i−1])`. `CI` is a net (constant 0
or 1 for every use here; the constant is kept, §2.2).

`lcu_build` is `_90_lcu_brent_kung`: `g[0] = g[0] | (p[0] & CI)` (`$_AND_` then `$_OR_`); the main
tree for `i = 1 .. clog2(W)` and `j = 2^i − 1, step 2^i`: `g[j] = g[j] | (p[j] & g[j − 2^(i−1)])`,
`p[j] = p[j] & p[j − 2^(i−1)]`; then the inverse tree for `i = clog2(W) .. 1` and `j = 2^i + 2^(i−1)
− 1, step 2^i`: the same two updates. `CO = g`. The loops are bounded by `clog2(W) ≤ 32`; nothing
recurses.

| Cell | Rule (`s` = both-signed flag, §3.3) |
|---|---|
| `$add` | `alu_build(A, B, CI = 0, BI = 0, Y_WIDTH, s)`; `Y` is the sum |
| `$sub` | `alu_build(A, B, CI = 1, BI = 1, Y_WIDTH, s)` |
| `$neg` | `alu_build(A = 0, B = A, CI = 1, BI = 1, Y_WIDTH, A_SIGNED)` (`alumacc` turns a lone negation into `0 − A`) |
| `$lt $le $gt $ge` | `W = max(A_WIDTH, B_WIDTH)`; `alu_build(A, B, CI = 1, BI = 1, W, s)`. Flags (from `alumacc.cc`): `cf = $_NOT_(CO[W−1])`; `of = $_XOR_(CO[W−1], CO[W−2])` (`CO[W−2]` is `CI` when `W = 1`); `sf = Y[W−1]`; `lt = s ? $_XOR_(of, sf) : cf`; `eq = AND-tree(P)` (the `X` bits); `le = $_OR_(lt, eq)`; `gt = $_NOT_($_OR_(lt, eq))`; `ge = $_OR_(gt, eq)`. `Y[0]` is the one asked for; `Y[k>0] = 0` |

The `Y` outputs of the ALU that the comparison does not read are dropped, not built: a builder
returns lazily the bits its caller asks for (`alu_build` has a `need_sum` flag), because Yosys's
`opt_clean` would remove them and the cap in §11 counts built nodes.

### 5.5 `$mul` (`maccmap`)

`alumacc` turns a lone `$mul` into a `$macc` with one product term, and `maccmap` expands it:
- width `W = Y_WIDTH`; the partial products of the extended operands (`A`, `B` extended to `W` by
  `A_SIGNED && B_SIGNED`): for each bit `i` of `B`, the row `(A << i) & {W{B[i]}}`, built as
  `$_AND_` of `A[k]` and `B[i]` into column `i + k < W`;
- a **bit heap**: one set of nets per column; a net added to a column that already holds that same
  net cancels and carries into the next column (`add(bit, position)` in `maccmap.cc`);
- the heap is read out as summands (one per row of columns) and reduced three at a time with
  full adders (`$fa`: `t1 = a^b; t2 = a&b; t3 = c&t1; sum = t1^c; carry = t2|t3`) until two remain,
  in the order of `maccmap.cc` lines 215–236 (`i` stepping by 3, an incomplete triple carried);
- the two summands go to `alu_build(..., CI = 0, BI = 0, W, unsigned)`.
Task 5 reads `maccmap.cc` again line by line before writing it; the contract above is what the
tests pin (closed-form counts of `$_AND_` and of full adders, then the node histogram against
Yosys, §14.2).

### 5.6 `$div`, `$mod` (`_90_div`, `_90_mod`, `$__div_mod_u`, `$__div_mod_trunc`)

Truncating division as `techmap.v` builds it: take absolute values of signed operands (conditional
negation, `alu_build`), run the unsigned **restoring divider** of `$__div_mod_u` (`WIDTH` stages;
stage `i` shifts one more dividend bit in, subtracts the divisor with `alu_build`, and keeps the
difference when it does not borrow, a `$_MUX_` row), then negate the quotient (signs differ) or the
remainder (dividend negative) back. `$div` returns the quotient, `$mod` the remainder, at
`Y_WIDTH` (extension and truncation as in §3.3). The size is `Θ(W²)`, capped in §11. Division by
zero yields what the divider yields (all ones quotient), which refines simlib's x. Tests pin the
divider against the registry evaluator on random operands; the structure against Yosys is tier T.

### 5.7 `$pow`

Yosys's `_90_pow` is `_TECHMAP_FAIL_`: `techmap` leaves the cell, and Parmys's flow never sees one
because `opt_expr` rewrites constant and power-of-two cases first. `lower` does the same: a `$pow`
that is still live is `lower: module 'm': '$pow' '<cell>' has no bit-level form (run opt first;
Yosys leaves it unmapped)` (`ODIN3_ERR_INVALID_ARG`). Peter's question 4.

## 6. Flip-flops (LW-7; decision 19)

The registry has exactly these bit storage cells: `$_DFF_P_`, `$_DFF_N_`, `$_FF_`, `$_DLATCH_P_`,
`$_DLATCH_N_` (IR-8, `dff.c`), each with an `INIT` parameter (0, 1, 2 = don't care, 3 = unknown;
default 3). It has no async-reset, enable or sync-reset bit cell. So the whole word family
reduces to **one `$_DFF_P_` per bit plus logic in front of D**, and an asynchronous control can
only be made synchronous. This is decision 19 (a) as approved: "mirror Parmys's
`adff2dff`/`adffe2dff`/`aldff*2dff` and `dffunmap`", and it is also forced by the registry (§16
row 8).

### 6.1 The rule

For a word FF with width `W`, each bit `b` gets one storage node. Let `D`, `Q`, `CLK` be the cell's
nets and `c = CLK_POLARITY`. The **next-state function** `N[b]` (what `Q[b]` takes at the active
edge) is built from the cell's controls, in this priority, with `$_MUX_(a, b, s) = s ? b : a`
and the polarity of each control folded in (an active-low control swaps the mux arms; no inverter
node is made):

| Cell | `N[b]` | Parmys / Yosys it mirrors |
|---|---|---|
| `$dff` | `D[b]` | `simplemap_ff` |
| `$dffe` | `EN ? D[b] : Q[b]` | `dffunmap` `unmap_ce` |
| `$sdff` | `SRST ? SRST_VALUE[b] : D[b]` | `unmap_srst` |
| `$sdffe` | `SRST ? SRST_VALUE[b] : (EN ? D[b] : Q[b])` (reset over enable) | `unmap_ce_srst` (`ce_over_srst = 0`) |
| `$sdffce` | `EN ? (SRST ? SRST_VALUE[b] : D[b]) : Q[b]` (enable over reset) | `ce_over_srst = 1` |
| `$adff` | `ARST ? ARST_VALUE[b] : D[b]` | `adff2dff.v` |
| `$adffe` | `ARST ? ARST_VALUE[b] : (EN ? D[b] : Q[b])` **(reset over enable, agent default; Peter's question 1)** | `adffe2dff.v` has enable over reset (see §6.3) |
| `$aldff` | `ALOAD ? AD[b] : D[b]` | `aldff2dff.v` |
| `$aldffe` | `ALOAD ? AD[b] : (EN ? D[b] : Q[b])` (load over enable) | `aldffe2dff.v` has enable over load |
| `$dffsr` | `CLR[b] ? 0 : (SET[b] ? 1 : D[b])` (clear wins, simlib) | `dffunmap`-style, `$_DFFSR_` unmapped; Parmys does not convert it |
| `$dffsre` | `CLR[b] ? 0 : (SET[b] ? 1 : (EN ? D[b] : Q[b]))` | likewise |

A constant control disappears into the constant it is (the mux selects a constant; it is built,
then `opt` removes it, §2.2). The `Q[b]` fed back in the `EN ? D : Q` arm is the storage node's own
output net. `CLK_POLARITY` is a parameter of every clocked family cell (`$dffsr` and `$dffsre`
included); the latches have `EN_POLARITY` instead.

The storage node is `$_DFF_P_(C = CLK, D = N[b], Q = Q[b])` when `c = 1`. For `c = 0` (**a falling
edge**) decision 19 (a) says: a clock inverter and a warning (Odin II's way; none of the 19 needs it).
`--neg-edge invert` (the default) builds **one** `$_NOT_` on `CLK` per word FF cell (shared by its
bits) and a `$_DFF_P_` on its output, and warns `lower: module 'm': falling-edge '<type>' '<cell>'
gets a clock inverter`. `--neg-edge keep` (agent default for non-VTR targets; Peter may override)
builds `$_DFF_N_` and does not warn. 3E's VTR writer rejects `$_DFF_N_` (PHASE3 "3E rules"), and
1E rejects a clock that is not a primary input, so a netlist with an inverter is simulated through
`keep` (§14.3).

**Init values.** `INIT` of each storage node = `odin3_ffinit_get(module, Q[b])` (the `init` wire
attribute, 2D EL-40; 2E's `ffinit`): 0 or 1 when defined, else 3. Nothing else carries init. An async
reset value is not an init.

**The declared divergence.** Decision 19: an async set, reset or load becomes synchronous. The
netlist differs from the RTL on reset *timing*: Q follows the control at the next active edge, not at
once; at every active edge where the control is active the two agree. This is the same divergence
Parmys makes, listed in the PHASE3 risks (6) and in the exit table. `lower` **warns once per design**
(not per cell): `lower: asynchronous reset/set/load made synchronous on N flip-flop bits in M cells
(PHASE3 decision 19); the netlist differs from the RTL on reset timing; see --report`, and
`--report <file>` writes the list (§13). `--async error` (agent default; decision 19 option (b) as a
switch) turns each such cell into `lower: module 'm': asynchronous control on '<type>' '<cell>'`
(`ODIN3_ERR_INVALID_ARG`) for flows that must not change timing.

### 6.2 Latches (LW-8)

`$dlatch` (`EN`, `D`, `Q`, `EN_POLARITY`): per bit `$_DLATCH_P_` (`EN_POLARITY = 1`) or `$_DLATCH_N_`
with `E = EN`, `D = D[b]`. The other latch cells **keep their meaning exactly** (no divergence): a
latch whose control is level-sensitive and transparent can absorb an asynchronous control into its
enable and data, as Yosys's `dfflegalize` does:
- `$adlatch` (async reset `ARST`): `E' = EN_active | ARST_active`, `D' = ARST_active ? ARST_VALUE[b] :
  D[b]`;
- `$dlatchsr` (per-bit `SET`, `CLR`): `E'[b] = EN_active | SET[b] | CLR[b]`, `D'[b] = CLR[b] ? 0 :
  (SET[b] ? 1 : D[b])` (clear wins, simlib);
- `$sr` (no enable): `E'[b] = SET[b] | CLR[b]`, `D'[b] = !CLR[b]`.
The latch is then `$_DLATCH_P_` on `E'`. `E'` is an OR of nets (`$_OR_` nodes), so a latch enable that
comes from logic is legal (IR-8: "a latch enable may come from logic"). Level latches stay
rejected by 3E's VTR writer; `--nolatches` keeps them out of VTR runs (PHASE3 header). Latch INIT:
as §6.1.

### 6.3 Why the enable priority is a question (Peter's question 1)

Parmys's `adffe2dff.v` and `aldffe2dff.v` put the enable *around* the whole next-state function:
`if (EN) Q <= ARST ? VALUE : D`. A real `$adffe` resets regardless of `EN`; after decision 19 makes
the reset synchronous, the closest synchronous behaviour is `Q <= ARST ? VALUE : (EN ? D : Q)`
(reset over enable). Parmys's map is `$sdffce` semantics, not `$sdffe`, so with `EN = 0` and the
reset active it holds its value, which the RTL never does. The same shape as decision 29, ruling 3
(signed adders: correct, not Parmys-exact). The recommendation is the same: **correct**, declared in
the exit table and the functional-check list. `$aldffe` and `$dffsre` are treated alike. If Peter
prefers Parmys-exact, the table rows for `$adffe` and `$aldffe` change to the `$sdffce` shapes and
nothing else does; the tests carry both variants behind `--async-enable-first` (hidden,
test-only).

## 7. Memories (LW-9)

`hardmap` (3C) maps what it can to RAM slices (decision 6). A `$mem` that is still live, because a
read cannot become synchronous, because the arch has no RAM, or because `lower` runs without
`hardmap`, becomes **flip-flops and multiplexers**, as Yosys's `memory_map` does it. 2D emits
exactly the form EL-27/EL-39 fix: one `$mem` per array, async read ports (`RD_CLK_ENABLE` 0,
`RD_EN` 1, `RD_TRANSPARENT` 0), clocked write ports in source order, raw (unsubtracted) addresses,
`OFFSET`, and `INIT`. `hardmap`'s "FF merge" may have turned a read port synchronous.

| Part | Rule |
|---|---|
| Storage | for word `k` in `0 .. SIZE−1` (address `k + OFFSET`) and bit `j`: one `$_DFF_P_` (`$_DFF_N_` or inverted clock per §6.1's edge rule) `Q[k][j]`; its `INIT` is `INIT[k·WIDTH + j]` (`x` → 3) |
| Write decode | per write port `p` and word `k`: `sel[p][k] = AND-tree over ABITS of (WR_ADDR[p][i] ? (a_i) : !(a_i))`, `a = (k + OFFSET) mod 2^ABITS` (the address compare `addr_decode` builds; inverters shared per port) |
| Write data | per bit `j`, `N[k][j] = Q[k][j]` hold, then for `p = 0 .. WR_PORTS−1` in order: `N = $_MUX_(N, WR_DATA[p][j], $_AND_(sel[p][k], WR_EN[p][j]))` (the per-bit enable of `$mem` is `WIDTH` wide); the later port wins a same-address conflict (EL-27: "a later write port wins"), which is the rule Yosys uses and what a later statement does. Two ports writing the same address in one cycle is undefined behaviour in Verilog (§17 #66, race micros #99); `lower` keeps the port order and does not warn |
| Async read | per read port `r` and data bit `j`: a binary `$_MUX_` tree over the words, selected from the low address bit up (`abits` levels, as `memory_map`'s `$rdmux`); a word index `≥ SIZE` reads `$_CONSTX_` |
| Sync read, not transparent | the same tree, registered: `$_DFF_P_` per data bit on `RD_CLK` (polarity `RD_CLK_POLARITY`); when `RD_EN` is not the constant 1 the register's D is `$_MUX_(Q, tree, RD_EN)` |
| Dropped | a `$mem` with no read port is deleted; a write port whose `WR_EN` bits are all constant 0 is skipped |
| Errors | an **async write port** (`WR_CLK_ENABLE` 0), write ports on different clocks or polarities (one FF has one clock), and a **transparent** read port (`RD_TRANSPARENT` 1) are `lower: module 'm': '$mem' '<cell>' <reason>` (2D never emits them; Yosys's `memory_map` refuses the first two as well). Standalone `$memrd`/`$memwr` cells (a Yosys form, 2D collects into `$mem`) are `lower: standalone $memrd/$memwr '<cell>' (use $mem)` |
| Cap | `SIZE × WIDTH` above `ODIN3_LOWER_MAX_MEM_BITS` (2^22 = 4,194,304) is `lower: module 'm': '$mem' '<cell>' has N bits (cap 4194304); hardmap it or raise --max-mem-bits`. The VTR RAMs are 5,251 slices for `LU32PEEng` (decision 6) and must never reach here |

Memory contents `INIT` are kept (unlike `hardmap`, which drops them with a warning, decision 6.5).
RAM correctness has no oracle in our simulator (PHASE3 risk 5), so `$mem` is checked by SAT
refinement against Yosys's own `memory_map` only (§14.4).

## 8. Other cells and special cases

| Cell | Treatment |
|---|---|
| `$sop`, `$lut` | `bit`: untouched (§1) |
| `$tribuf` | located error: `lower: '$tribuf' '<cell>' has no bit-level cell in the registry` (3E also rejects a tri-state; `$_TBUF_` is Phase 6 Altera work). Peter's question 4 |
| `$proc` | refused by the manager (O1) |
| `$_CONST*_`, `$_BUF_`, all bit gates | untouched |
| An `inout` or a net driven by a tristate type | not examined; `check` rules 4/E stay as they were |
| A cell with an unconnected input pin | its missing operand bit reads as a fresh undriven net (a bit gate with an open pin, rule 11 W). `opt` warns on these as well. It is not an error here |

## 9. Provenance (LW-10; IR-12, IR-13)

One provenance operation per lowered word cell (`odin3_prov_begin_op`, named `lower`). Every node
the rule creates shares one DERIVED record whose single parent is the word cell's record, so
`odin3_prov_index_by_loc` on `foo.v:42` reaches the dead `$add` (slot or tombstone, IR-6) and every
gate made from it, and a provenance name leads with the word cell's source. Merged nets
(`lower_connect`) are rewiring (IR-15): they keep their records. Memory nodes get the `$mem`'s
record; the clock inverter of §6.1 gets the FF's. Bit nodes are **unnamed**; the output nets keep
their names and wires (agent default; naming each bit node `<name>[i]` costs memory for nothing
the writers use, and DESIGN §13 #6 is open).

**Attributes.** Only `keep` is copied from the word cell to every node it creates (so `opt_clean`
still honours it). `src`-style attributes live in provenance.

## 10. Diagnostics (LW-11)

Style as 2D §9 and 2F §16 (lower case, names in `'…'`, no final period). `<cell>` is the node's name
or `$c<ID>`.

| Level | Exact text | When |
|---|---|---|
| INFO | `lower: module 'm': lowered N word cells to B bit cells` | per module with at least one word cell |
| INFO | `lower: lowered N $mul, M $add, K $mem as soft logic` (only the types present, sorted by name) | end of the pass |
| INFO | `lower: nothing to lower` | no word cell in the design |
| WARN | `lower: asynchronous reset/set/load made synchronous on N flip-flop bits in M cells (PHASE3 decision 19); the netlist differs from the RTL on reset timing; see --report` | once per design, if N > 0 |
| WARN | `lower: module 'm': falling-edge '<type>' '<cell>' gets a clock inverter` | per cell, `--neg-edge invert` |
| WARN | `lower: module 'm': cell '<type>' '<cell>' has A_SIGNED != B_SIGNED: treated as unsigned (both-signed rule, Yosys simlib; PHASE2 #25)` | per mixed cell, once |
| WARN | `lower: module 'm': '$mem' '<cell>' lowered to N flip-flops` | per `$mem`, so a missed `hardmap` is visible |
| DEBUG | `lower: module 'm': <type> '<cell>' -> B bit cells` | per cell |
| ERROR | `lower: no lowering rule for cell type '<t>' ('<cell>')`; `lower: module 'm': '$pow' '<cell>' has no bit-level form (run opt first; Yosys leaves it unmapped)`; `lower: '$tribuf' '<cell>' has no bit-level cell in the registry`; `lower: standalone $memrd/$memwr '<cell>' (use $mem)`; the three `$mem` errors of §7; `lower: module 'm': asynchronous control on '<type>' '<cell>'` (`--async error`) | `ODIN3_ERR_INVALID_ARG` |
| ERROR | `lower: module 'm': '<type>' '<cell>' would create N nodes (cap C)`; `... '$mem' ... has N bits (cap 4194304) ...` | the caps of §11 (`ODIN3_ERR_INVALID_ARG`) |
| ERROR | `lower needs more than N bytes (the design's budget is 60% of physical memory)` | the shared byte budget (`ODIN3_ERR_PARSE`, as 2F) |
| ERROR | `lower: module 'm': node '<type>' '<cell>' is not in the netlist view after lowering` | the post-condition of §12.3 (`ODIN3_ERR_CHECK`; a bug) |
| ERROR | `lower: unknown argument '<arg>'` | `INVALID_ARG`, the 1D style |

`--report <file>` writes JSON (written atomically, as the 1F writers do): `{"async_to_sync":
[{"module": …, "cell": …, "type": …, "bits": …, "controls": ["ARST"]}, …], "neg_edge": […],
"mixed_sign": […], "mem": […]}`, in node-ID order. 3H's functional check (decision 16) reads
`async_to_sync` to know which designs the reset divergence affects (PHASE3 risk 6).

## 11. Options, caps, and the byte budget (LW-12)

Pass line: `lower [--neg-edge invert|keep] [--async sync|error] [--report <file>] [--max-mem-bits N]`
(`processes = REFUSE`). The defaults are the agent defaults of §6; `docs/PASSES.md` gets its row in
Task 1.

**Caps** (named constants in `src/passes/lower/lower.h`, lowered in tests through a hidden
`lower_test.h`, as 2D/2F/3A do; **agent default (Peter may override)**):

| Cap | Value | Why |
|---|---|---|
| `ODIN3_LOWER_MAX_CELL_NODES` | 2^24 (16,777,216) | bit nodes one word cell may create; a `$div` of width 2,000 would be 2^28; the 19 designs' largest word cell is a multiplier of at most 36×36 after `hardmap` (hard) and a few hundred bits otherwise |
| `ODIN3_LOWER_MAX_MODULE_NODES` | 2^26 | live nodes in a module after lowering (same as 3A's `ODIN3_FLATTEN_MAX_NODES`) |
| `ODIN3_LOWER_MAX_MEM_BITS` | 2^22 (option `--max-mem-bits`) | §7 |

**Pre-flight estimate.** Before building a word cell, `lower` computes an **upper bound** on the
nodes the rule will create from the widths alone (closed formulas, one per rule, in
`src/passes/lower/size.c`; their constants are **measured** in Tasks 3–5 from the unit tests'
real node counts and rounded up; the test asserts `built ≤ estimate` for every width in the sweep).
It checks the estimate against the per-cell and per-module caps and, with `odin3_budget_check`
(60% of physical memory, shared with `proc` and the opt family, 2E §16), against the design's real
`odin3_design_bytes_reserved` every 2^16 created nodes. A cell that would exceed a cap is the
located error above and **nothing is built for it**.

## 12. Out of memory, `check`, determinism (LW-13)

### 12.1 Order and determinism

- Modules in module-ID order, nodes in ID order (IR-16), new nodes appended; each rule is a pure
  function of the cell's parameters and the order of its operand bits. No hash-map iteration
  reaches the IR, no pointer, address, time or randomness decides anything. The net a gate takes is
  the net of its operand bit; nets are created in the builder's order.
- A module built in reverse creation order lowers to a `netlist-compare`-identical result.
- Two runs give byte-identical `write_blif` and `write_rtlil` (the latter on the mixed view).
- `lower` never compacts (IR-6): the script runs `compact` after it (§2.2).

### 12.2 Transactions and out-of-memory

Per word cell: **build** (all new nodes on fresh nets; a vector of them is kept) then **commit**
(delete the word node, then `lower_connect` each output bit). A failure during *build*
(`ODIN3_ERR_NO_MEMORY`, a cap, a budget failure) deletes the nodes created so far and returns the
status: the word cell is intact, the cells lowered before it are complete, and the module is in
the legal mixed state of IR-9 and **refines its input** (2F's OP-13, here as equivalence). A
failure during *commit* is the one place a cell can be left partial; the commit allocates only for
merges that grow an alias table. Task 1 measures whether `odin3_net_merge` can fail; if it cannot,
the contract is full per-cell atomicity, and if it can, the failing cell's undriven output nets
are legal (check rule 4 W) and the pass reports the status. **This matches 2F's `kit_rebuild`
contract and is stronger than 3A's** (agent default; Peter may override). The OOM test injects a
failure at every allocation of a small fixture for each rule family and requires: no crash, no
leak (ASan), `ODIN3_ERR_NO_MEMORY` from a build-phase failure with the design equal to its
input module-for-module (`write_rtlil` text equal for the cells not yet lowered) and `check` FULL
clean, and, from a commit-phase failure, check rules 1, 2, 3, 5, 6, 8, 9, 10, 11 clean.

### 12.3 `check`

`check` runs before and after the pass in Debug (CLAUDE.md rule 3). Post-conditions `lower` adds,
asserted in Debug and tested: every live node of every module is `bit`, `hard`, `blackbox`, `module`
or `port` (the netlist view, IR-9); the number of live word nodes is zero; every wire still names
live nets; every output net of a lowered cell has exactly one driver (or none, after a commit
failure); the pass's INFO totals equal the node counts that `stats` reports before and after.

## 13. Interfaces (LW-14)

```c
/* src/passes/lower/lower.h */
typedef struct odin3_lower_opts {
    bool keep_neg_edge;          /* --neg-edge keep */
    bool async_is_error;         /* --async error */
    uint64_t max_mem_bits;       /* 0 = ODIN3_LOWER_MAX_MEM_BITS */
    const char *report_path;     /* NULL = none */
} odin3_lower_opts;

/* Lowers every word cell of every module of design; see LW-1 .. LW-13. ODIN3_OK, or
 * ODIN3_ERR_INVALID_ARG / _NO_MEMORY / _PARSE / _CHECK as §10 and §12. */
odin3_status odin3_lower_run(odin3_pass_ctx *ctx, odin3_design *design, const odin3_lower_opts *opts);
```

Files (`src/passes/lower/`): `register.c` (pass, options, report), `lower.c` (driver: modules,
nodes, dispatch, transactions), `kit.c` (net vectors, extension, fresh nets, gate builders
`gate_and/or/xor/not/mux`, reduction trees, `lower_connect`), `bitwise.c` (§4), `pmux_shift.c`
(§5.1–5.3), `alu.c` (§5.4), `mul.c` (§5.5), `div.c` (§5.6), `ff.c` (§6), `mem.c` (§7), `size.c`
(§11), `lower.h`, `lower_test.h`. Dispatch is a table indexed by the built-in cell type ID
(`odin3_celltype_builtin_index`), so a type's rule is one table entry. The builders are the only
code that calls `odin3_node_create`; nothing outside `src/ir` writes an IR field (IR-15); IR objects
live in the module's storage (IR-18): the pass `malloc`s no IR object, and its scratch is `src/util`
vectors and the arena.

## 14. Evidence (LW-15)

Every test runs under ASan/UBSan in Debug with `check` around every pass. Sweeps run **smallest
input first** (memory rule). Heavy sweeps run through `tools/heavy-run` in the controller's announced
windows (decision 29, ruling 8; no nightly, decision 28), and lanes do unit work only during them.

The rules have three **tiers**, which set what "equal to Yosys" means:

| Tier | Cells | Contract against Yosys 0.55 |
|---|---|---|
| S | §4's cells; `$dff`, `$dffe`, `$sdff`, `$sdffe`, `$sdffce` after Yosys `dffunmap`; `$dlatch` | gate-for-gate: after the common cleanup, `netlist-compare` exit 0 (identical) |
| T | `$pmux`, shifts, `$shiftx`, `$neg`, `$add`, `$sub`, comparisons, `$mul`, `$div`, `$mod` | `equiv-check` equivalent; per-type histograms equal after Yosys `opt -fast` and ours; gate-for-gate identity wanted, differences listed with reasons |
| D | every async family, `$adlatch`, `$dlatchsr`, `$sr`, `$mem`, `--neg-edge invert` | equivalent to a **declared reference** (§14.2), not to Yosys's own lowering |

### 14.1 Unit tests (Unity; hand-built IR, `tests/unit/test_lower_*.c`)

- `test_lower_kit.c`: extension (signed, unsigned, truncate, mixed), the pairing tree for sizes
  1..40 against the closed form (nodes and depth), `lower_connect` (alias kept, port net, the
  canonical constant), the transaction (build failure rolls back), OOM at every allocation.
- One file per family. For **every cell type, every width in {1, 2, 3, 4, 5, 8, 17, 33, 64, 65}**
  (cells with two operands: the unequal pairs of those too), signed, unsigned and mixed:
  node count equals the closed form for tier S and the measured table for tier T; the module
  satisfies the netlist view; output names and wire membership are unchanged.
- **Simulator-equal** (§14.3) inside each file: exhaustive for total input width ≤ 12, 4,096 seeded
  random vectors above.
- `test_lower_ff.c`: every family × polarity of every control × `CLK_POLARITY`: the structure of
  §6.1's table, init propagation (0, 1, x), `--neg-edge` both ways, `--async error`, the one-per-design
  warning, the `--report` file. Sequential behaviour against an **independent reference stepper**
  in the test file (the simlib text of `ff.c`'s comment table written as C, sharing no code with the
  pass): exhaustive over all input sequences of length ≤ 5 at `W = 1`, random for `W > 1`.
- `test_lower_mem.c`: every §7 row, writing then reading all addresses with the 1E simulator on the
  lowered netlist against a C array model; overlapping write ports (later wins); `OFFSET` ≠ 0;
  `ABITS` larger and smaller than `clog2(SIZE)`; out-of-range reads give x; the cap and every error. `ABITS` 0 (a one-word `$mem` after `wreduce`, 3G): the write decode is constant 1 (empty AND-tree), the read mux tree has no levels, `a = k + OFFSET mod 2^0 = 0`.
- `test_lower_order.c` (O2): the Phase 3 script on a hand-built design that has a `multiply`, an
  `adder`, a `single_port_ram` (hard cells), plus a soft `$mul`, `$add` and `$mem`: the hard ones survive, the
  soft ones go, the INFO counts are right.
- `test_lower_pass.c`: option parsing, the `REFUSE` refusal, an empty design, a netlist from
  `read_blif` unchanged byte for byte, diagnostics text.

### 14.2 Goldens and the same-input differential (`lower-diff`)

`tools/lower-check/lower-diff <files> [--top T]`, per design and per cell family:

1. Odin III: `read_verilog -nolatches; proc; opt; write_rtlil in.il; lower; write_blif lo.blif`.
2. Yosys 0.55 on `in.il`: `read_rtlil in.il; techmap -map <the four Parmys maps, with the
   enable-order variant of §6.3>; techmap; dffunmap; opt_clean -purge; write_blif -undef + unconn
   yo.blif`. (Tier S needs no `opt`; tier T also runs `opt -fast` on both sides.)
3. Tier S: `netlist-compare lo.blif yo.blif` exit 0. Tier T: `equiv-check` exit 0 and the cell-type
   histograms printed and compared, differences allowed only if `tests/lower/diff_expect.txt`
   lists them with a reason. Tier D: `equiv-check lo.blif ref.blif` where `ref` is the **declared
   reference** of the family: for the async FFs the Yosys lowering with the maps above (so reset
   over enable is the *declared* semantics), for `$adlatch`/`$dlatchsr`/`$sr` Yosys
   `dfflegalize -cell $_DLATCH_?_ 01`, for `$mem` Yosys `memory_map`, for `--neg-edge invert` Yosys's
   own `$_DFF_N_` with the inverter replaced on the reference by a hand-written Verilog model.

**Per-cell goldens** `tests/lower/cells/<type>_<variant>.v` + `.il` + `.blif` (about 150: one tiny
module per rule and variant, as 2F's `tests/opt/rules`): the `.blif` is the committed Odin III
output, compared by `netlist-compare` (exit 0) in CTest `lower_cells`; the Yosys run is the second
CTest `lower_cells_yosys` (skipped without Yosys 0.55, as the other YOSYS055 tests are).

### 14.3 Simulator-equal before and after (`lower-sim`)

`tools/lower-check/lower-sim <files> [--top T] [--vectors N] [--seed S]` runs 1E's `sim` on the
design **before** `lower` and **after**, with the same seeded random vectors and the reset rules of
`sim-check`, and requires equal outputs on every cycle. 1E simulates the netlist view; the RTLIL-view
word cells are simulated by the registry hooks (IR-8) where they have them. So:

| Design contents | Before | After | Verdict |
|---|---|---|---|
| word combinational cells with hooks (everything in §4, §5 except `$shiftx` and `$pow`) | the registry hooks | the lowered gates | **sim-equal** (the checklist's "simulator-equal before/after") |
| `$shiftx`; x-bearing designs | no hook / 2-state sim cannot judge x | — | `skipped`, the cell named, never `pass`; the tier T equivalence check (§14.4) covers them |
| word FFs and latches (no hooks, IR change D) | the **independent reference stepper** of `test_lower_ff.c` driven by the tool, and for micros the Yosys-lowered netlist (`yo.blif` of §14.2) simulated by 1E | the lowered gates | **sim-equal to the reference**; "before" is not a 1E run (Peter's question 5) |
| `$mem` | no hook | the lowered netlist | the C array model of `test_lower_mem.c` (hand-built only); micros: `skipped`, covered by §14.4 |
| `--neg-edge invert` | — | 1E rejects a clock that is not a primary input | run through `--neg-edge keep` for 1E; the inverter variant by §14.4 |

Async-to-sync is a declared divergence, so `lower-sim` samples outputs **at the end of each cycle**
(after the active edge settles), as 1E does. At those points a design whose async controls change
only between cycles behaves identically before and after; mid-cycle reset timing is exactly what
decision 19 gives up and is not sampled. The tool prints the sample points it used.

It runs on: the hand-built corpus `tests/lower/` (every §14.1 fixture), `tests/micro/verilog` (every
micro with no RAM; RAM micros are covered by §14.4), the Phase 2 elaboration fixtures that are
process-free after `proc`, and the fuzz corpus.

### 14.4 Refinement checks (`lower-equiv`, 2F's `opt-equiv` style)

`tools/lower-check/lower-equiv <files> [--top T]`: runs the script with `--lower-snapshot <dir>` (a
Debug option on `lower`, `src/passes/lower/snapshot.c`) which writes `write_rtlil` of each module
**before** and `write_blif` **after**, per design; then, in Yosys 0.55: gold = `read_rtlil before.il`
with the declared sync-conversion maps applied (so the divergence of §6 is the *specification*,
not a failure); gate = `read_blif after.blif`; `equiv_make gold gate eq; equiv_simple -undef -seq 4;
equiv_induct -undef; equiv_status -assert`, with 2F's clock-aware family (`async2sync`, `techmap`,
`memory`, `clk2fflogic`) and its exit codes: 0 refines, 1 does not, 2 tool error, 3 hard (crash,
cap kill, timeout; never absorbed). x in the gold is a don't-care, so `$shiftx`, out-of-range
`$mem` reads and `$pmux` multi-hot are judged correctly. The mutants of 2F's review (flipped edge,
wrong clock net, wrong function, wrong reset value and polarity, tied output, missing port) are
regression tests (`tests/tools/test_lower_equiv.py`), plus the new ones this pass needs: reset and
enable swapped, a bit-swapped `$pmux`, a signed comparison read unsigned, a shifted-by-one
`$shr`, an off-by-one `$mem` `OFFSET`. Runs on every micro, every `tests/lower/` fixture, the
`lower_diff` set, and the VTR set via `heavy-run` (smallest first; designs whose proof runs past
60 s are `unproven`, not `pass`, as in 2F §20.2).

### 14.5 Corpus, caps and fuzzing

- `lower_micro` / `lower_vtr`: the Phase 3 script minus `abc` on every micro and, as 3C lands, on
  the 19 VTR designs: `ODIN3_OK`, the netlist view holds, node counts logged. **Before 3C** the
  VTR runs lower `$mul` and `$mem` as soft logic and some designs hit the caps: the expected
  outcome of those is `cap` and is listed in `tests/lower/vtr_expect.txt`; after 3C the same
  list must be empty (M2).
- Differential generator: `tools/verilog-gen --elab` gains a `--lower` weighting (mixed widths and
  signs, every operator, enable and reset nests, shifts by variable amounts, `$pmux` through
  `case`) through `lower-diff` and `lower-equiv`: 200 seeds in CTest when Yosys is found, 5,000 in
  a heavy-run window.
- `tests/fuzz/fuzz_lower.c` (libFuzzer): bytes → a small word-cell builder (types, widths, signs,
  random wiring) → `lower` with lowered caps and injected `NO_MEMORY` → `ODIN3_OK` or the status,
  then `check` FULL. Corpus seeded from the unit fixtures.
- `bench_lower` (Release): nodes created per second and peak bytes on a synthetic design of
  1M mixed word cells; the number goes in the PHASE3 entry. Target: not slower than 2F's
  `opt_expr` per node, within the 60% budget on the 24 GB VM (decision 17).

### 14.6 Determinism, provenance and the 19

- Two runs → byte-identical `write_rtlil` and `write_blif`; reverse-created module →
  `netlist-compare`-identical (§12.1).
- After `lower`, `odin3_prov_index_by_loc` on a Verilog line reaches the dead word cell and its
  gates.
- Exit hand-over: `lower` is one step of 3H's O and O′ legs; 3H reads `--report` for the
  functional-check list (decision 16).

## 15. Files touched outside `src/passes/lower/`

`src/passes/builtin.c`/`builtin.h` (one table entry, `&odin3_pass_lower`); `docs/PASSES.md` (one
row, and the `Processes` paragraph already says `lower` is `REFUSE`); `CMakeLists.txt` (append-only
hunks); `tests/lower/`; `tools/lower-check/`; `tools/verilog-gen` (`--lower`, append-only);
`docs/PHASE3.md` (the checklist line and a decisions entry, Task 9); DESIGN §6 step 9 (one
sentence, Task 9). No `src/ir/` change: `lower` uses only existing creation, deletion, merge and
provenance primitives.

## 16. Divergences from Yosys 0.55 / Parmys (§17-style)

| # | Odin III | Yosys 0.55 / Parmys | Why / effect |
|---|---|---|---|
| 1 | a mixed-sign binary cell is unsigned (both-signed rule) | `simplemap`/`alumacc`/`_90_alu` extend per operand or use `A_SIGNED` alone | IR-8, PHASE2 #25. Rare (the front end unifies the flags); warned once per cell |
| 2 | **asynchronous reset, set and load become synchronous** | Yosys keeps `$_DFF_PN0_` …; Parmys's `adff2dff` does the same conversion as here for `$adff` and `$aldff` | decision 19. Differs from the RTL on reset *timing* only; warned once per design; `--report`; `--async error` |
| 3 | `$sop` and `$lut` are left alone | `techmap` expands them (`_90_lut`) | they are `bit` cells; a `.names` cover is what the back end wants |
| 4 | `$adffe`, `$aldffe`, `$dffsre`: **reset/load over enable** | Parmys's `adffe2dff.v`/`aldffe2dff.v`: enable over reset (the `$sdffce` shape). `$dffsre` is not converted by Parmys | correct against the RTL, like decision 29 ruling 3; Peter's question 1. The hidden `--async-enable-first` is the Parmys shape |
| 5 | `$dffsr`, `$dffsre`, `$adlatch`, `$dlatchsr`, `$sr` are lowered | Parmys sends `$dffsr` to its own mapper; Yosys `techmap` gives `$_DFFSR_*`/`$_DLATCHSR_*` which VPR rejects | the registry has no such bit cell; same declared divergence (FFs) or exact (latches, §6.2) |
| 6 | falling-edge FFs get a clock inverter by default | Yosys keeps `$_DFF_N_` | decision 19 (a); `--neg-edge keep` gives Yosys's |
| 7 | no `$alu`/`$lcu`/`$fa`/`$macc`/`$pos`/`$sshl`/`$shift`/`$eqx`… rules | those cells exist in Yosys | not in the registry (2F §17 #5); the structures that pass through them are built directly (§3.2) |
| 8 | the fine FF family `$_DFFE_*`, `$_SDFF*_`, `$_DFF_PN0_`, `$_DFFSR_*` is never produced | `techmap` produces them; `dffunmap`/`opt -fast` re-infers `$_SDFFE_` | the registry has no such bit cell; the unmapped shape is `opt`'s input anyway |
| 9 | each cell is built alone; no sharing between a comparison and a subtraction of the same operands | `alumacc` merges `$alu` nodes with equal operands when run on a whole module; `techmap`'s wrapper runs it per cell, so Yosys's `techmap` does not share either | `opt_merge` shares them afterwards in both |
| 10 | constants are left in (`CI = 0`, shift stages on constants, `Y[k>0] = 0`) | `techmap` runs `opt_muxtree; opt_expr -fine` inside the shift and divider maps and `opt -fast` after | §2.2; the script adds `opt -fast` |
| 11 | `$pow` and `$tribuf` are errors | `$pow` is left unmapped (`_TECHMAP_FAIL_`); `$tribuf` becomes `$_TBUF_` | no bit cells for them; Peter's question 4 |
| 12 | `$mem`: transparent read ports, async write ports and write ports on different clocks are errors; reads past `SIZE` give x | `memory_map` lowers transparent reads with a bypass and rejects the other two | 2D never emits them; hand-built only |
| 13 | `$mem` `INIT` is kept | `hardmap` drops it with a warning (decision 6.5) | soft memories have somewhere to put it |
| 14 | bit nodes are unnamed; output nets keep their names | `techmap` makes `$techmap<N>$…` names | names never reach `netlist-compare`; DESIGN §13 #6 |
| 15 | `$pmux` multi-hot gives the OR of the selected words | `techmap.v` does too; simlib says x | refinement; checks drive one-hot |
| 16 | a division by zero gives the divider's result | simlib: x | refinement |
| 17 | `$reduce_*`, `$logic_*`, `$eq`, `$ne`: `Y[k>0]` merged with the constant-0 net | `connect Y[k] 1'0` | the same thing in the IR (IR-1, IR-4) |
| 18 | one `$_NOT_` on the clock per falling-edge FF cell, shared by its bits | per-bit `$_DFF_N_` | `--neg-edge invert` only |
| 19 | caps and a pre-flight estimate (§11) | none | safety; decision 17 |
| 20 | a failure while building a cell rolls that cell back (§12.2) | aborts the run | stronger than 3A's |
| 21 | `lower` never compacts or optimises | `techmap` is followed by `opt -fast` in the same Parmys script | the Phase 3 script spells them (§2.2) |
| 22 | `lower_connect` duplicates the 2F kit's merge rule | — | the kit is private to `src/passes/opt`; a shared `src/passes/netkit` is a later refactor (not in 3B) |
| 23 | Odin III log text (§10) | Yosys text | — |

## 17. Review focus and risks

1. **Decision 19's divergence** is the one place `lower` is not equal to the RTL. The reset
   timing, the enable order (question 1) and the falling-edge inverter all land here. The
   functional check (decision 16) must read `--report` or drive resets synchronously (PHASE3 risk 6).
2. **Structure fidelity of tier T** (`$mul` via `maccmap`, the divider, the shifts): the contract
   is equivalence plus a histogram, not identity. If a VTR design's `num_clb` drifts from Parmys by
   more than decision 3's band because of soft arithmetic, the O′ and P′ legs attribute it; ABC
   re-synthesises these cones, so the expected effect is small.
3. **Soft arithmetic before 3C lands**: with `hardmap` absent, `lower` expands every multiplier and
   every memory and some of the 19 will hit the caps (§14.5). That is correct behaviour, listed,
   and goes away at M2.
4. **Size of the netlist handed to ABC**: constants left in until `opt -fast` (§2.2).
5. **The 1E gap for sequential cells**: the "before" side of an FF cell is a reference, not a 1E run
   (§14.3). RAMs have no simulation oracle at all (PHASE3 risk 5).
6. **`check` rule 11 / open pins**: a word cell with an open input pin lowers to gates with open pins.
   Not an error; `opt` has the same stance (2F §17 #16).

## 18. Decisions for Peter

**Ruled 2026-10-14 (Peter: general yes, "as close to the flow"):** every recommendation below is approved.

Each has a recommendation. 1 changes a semantic Peter ruled on (decision 19), 2 edits the exit
script text, 3 and 4 fix what is lowered and how, 5 confirms the evidence form.

1. **Enable versus async reset in `$adffe`/`$aldffe`/`$dffsre`** (§6.3). Parmys-exact makes the
   (now synchronous) reset conditional on the enable. *Recommended:* correct (reset or load wins over
   the enable, like the RTL), declared in the exit table and the functional-check list, the same
   ruling as decision 29 ruling 3. The alternative is Parmys-exact, one table row.
2. **Script text** (§2.2): add `opt -fast; compact` after `lower`. *Recommended:* yes. It edits
   PHASE3's exit item 3 and the 3H script strings (Task 9); Parmys runs `opt -fast` there too.
3. **Soft arithmetic structure** (§5): Yosys's `techmap.v` structures (Brent-Kung `$lcu` adders,
   the `maccmap` full-adder tree, the barrel shifter, the restoring divider), with equivalence and
   a histogram as the contract (tier T). *Recommended:* yes, as the PHASE3 3B row says. The
   alternative, ripple adders and array multipliers, is simpler to review and gives ABC the same
   function, but it is not "Yosys structures".
4. **Cells with no bit form** (§5.7, §7, §8): `$pow` (non-constant), `$tribuf`, standalone
   `$memrd`/`$memwr`, transparent or async-write or multi-clock `$mem`: located errors, no IR change.
   *Recommended:* yes. `$_TBUF_` would be an IR addition for Phase 6 (Altera); the other cells never
   occur in 2D's output.
5. **Evidence for sequential cells** (§14.3): 1E has no hooks for the word FF family (IR change D),
   so "simulator-equal before/after" holds literally for combinational cells and, for FFs and
   latches, against an independent reference stepper and Yosys's own lowering simulated by 1E, plus
   the formal check of §14.4. *Recommended:* accept, rather than adding `simulate` hooks to the
   eleven IR change D cells (an IR change that 2E deliberately did not make, and that could not
   model async controls anyway).

The remaining defaults are marked "agent default (Peter may override)" where they occur and are
logged in PHASE3's decisions by Task 9: the caps (§11), `--neg-edge keep`, `--async error`,
`--report`, the unnamed bit nodes, the copied `keep` attribute, the per-cell transaction, the
duplicated connect helper.
