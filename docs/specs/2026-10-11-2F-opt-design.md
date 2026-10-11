# 2F — `opt`: design

Status: draft v1 (2026-10-11), for review. Phase 2, sub-project 2F (PHASE2 #1; light review;
Opus `high`). Spec: `docs/DESIGN.md` §5, §6 step 4, §10. Inputs: PHASE2 #4, #5, #13; `docs/IR.md`;
the op registry (`src/ir/cells/` plus 2D's IR change A); the 2D spec (EL-14 constants, EL-33
`$proc` and the `processes` flag, EL-40 `init`, EL-48 `write_rtlil`, §14.3 bridge, §14.6 x plan);
the 1E and 1B specs. **Oracle: Yosys 0.55 as pinned by VTR** (`$VTR_ROOT/libs/EXTERNAL/yosys`,
`60f126cd`; binary `$VTR_ROOT/build/bin/yosys`): `passes/opt/*.cc`, `kernel/ff.cc`,
`passes/proc/proc.cc`, and Parmys's `vtr_flow/misc/yosys/synthesis.tcl` at VTR `3c9a4d23`.
Decisions are numbered **OP-n**. Where this file and `docs/IR.md` disagree, IR.md wins, except
**IR change D** (OP-9), which is for Peter (§24).

**Oracle version note.** The brief's `~/odin3-ws/external/yosys` is the standalone Yosys
**0.69+270** (`c4a0a2c4`), whose `opt` family differs (`opt_balance_tree`, `opt_hier`, new
`peepopt` rules, `opt_clean`/`opt_dff` split into directories). Everything here was read in, and
every probe run on, 0.55; 0.69 serves only the nightly drift check (§20.4), as in 2D.

## 1. Purpose and scope (OP-1)

`opt` turns the word-level IR that `proc` (2E) leaves into the IR that Yosys 0.55's `opt` family
leaves on the same input, cell for cell where possible, so that the Phase 2 exit comparison
(PHASE2 #4) can reach **identical** netlists, not merely equivalent ones.

| Delivered by 2F | Where |
|---|---|
| `opt_expr`, `opt_merge`, `opt_muxtree`, `opt_reduce`, `opt_share`, `opt_dff`, `opt_clean` and the driver `opt`, with Yosys's names, options, loop and modes | `src/passes/opt/*.c` |
| The rewrite kit (connect, constant nets, rebuild with provenance), the FF model (`FfData`) and init values (`FfInitVals`) | `kit.c`, `ff.c`, `ffinit.c` |
| Folding through 2D's evaluator (`odin3_cval`, EL-14 = Yosys `const_*`) | `fold.c` |
| IR change D: the word FF cells (shared with 2E) | `src/ir/cells/ff.c` |
| The Phase 2 opt script and the 2H splice (OP-3) | §3, `docs/PASSES.md` |
| Per-rule goldens, per-sub-pass refinement, `opt-diff`, sweeps, fuzzing, `bench_opt` | `tests/opt/`, `tests/unit/test_opt_*.c`, `tools/opt-check/` |

Not in 2F: `wreduce`, `peepopt`, `share`, `fsm`, `memory*`/`opt_mem*`, `flatten`, `techmap`,
`dffunmap`, `opt_lut*`, `opt_demorgan`, `opt_ffinv`, `muxpack`, `pmux2shiftx`, `opt_hier`,
`opt_dff -sat` (a SAT solver; Parmys never passes it), and cells the registry does not have
(`$alu`, `$pos`, `$sshl`, `$shift`, `$eqx`, `$nex`, `$bmux`, `$demux`, `$bwmux`, `$bweqx`,
`$divfloor`, `$modfloor`, `$_MUX4_`…, the fine FF family `$_DFFE_*`/`$_SDFF*_`…). Rules on those
cells are not ported; a rule that would *create* one is written in registry cells (§17).

Success: every rule of §4–§11 has a unit golden; every sub-pass invocation on every corpus design
is a refinement of its input (§20.2); on the micros and the VTR set, Odin III's `opt` and Yosys
0.55's `opt` on the **same input** (Odin III's post-`proc` IR through `write_rtlil`) give equal
per-module cell-type histograms and identical netlists after a common Yosys lowering (§20.3),
exceptions listed with reasons; mcml.v and lenet.v meet §19; the gate passes.

## 2. How Yosys's concepts map onto the IR (OP-2)

Yosys works on multi-bit SigSpecs with a `SigMap` of `connect`ed aliases; Odin III has one-bit
nets that are merged (IR-1, IR-2, IR-15). The port is mechanical once these are fixed:

| Yosys | Odin III |
|---|---|
| `SigMap assign_map` (canonical bit of an alias class) | nothing: a merged net *is* the class; `odin3_net_driver` is O(1) |
| constant bit `State::S0/S1/Sx/Sz` in a SigSpec | a pin on the module's canonical constant net for that value (IR-4); the predicate is `odin3_net_const_value` (so a constant `$sop` counts) |
| `module->connect(Y, sig)` after `module->remove(cell)` | `kit_connect`: delete the driver, then per bit `odin3_net_merge(keep = source net, drop = Y net)` (a constant source: the canonical constant net); `keep == drop` (output fed back to its own input) leaves the net undriven |
| `$pos` (identity with extension) | wiring: Y bit k merges into A bit k, or into A's top bit (signed) or the 0 net (unsigned) beyond A's width; no cell |
| in-place `setPort` / `setParam` / `type = …` | `kit_rebuild`: the new node on fresh output nets, then the old one deleted and each old output net kept with the new node's merged into it (names stay); widths are fixed per node (IR §3) |
| `NEW_ID` wires and cells | unnamed nets and nodes (writers print `$n<ID>`/`$c<ID>`, IR §6) |
| wire attribute `init` (FfInitVals) | the same: wire attribute `init` (BITS), read per net through its primary and alias memberships (§9.4) |
| `keep` on a cell, wire or module | attribute `keep` (non-zero) on node, wire or module; an instance of a module that has `keep` or contains a `keep` node or wire counts as `keep` (Yosys `keep_cache`, iterative) |
| "unknown" cell (user module, black box) | granularity `module`, `blackbox`, `hard`: every pin is treated as both input and output, as `ct_all.cell_known` false makes Yosys do |
| `design->scratchpad "opt.did_something"` | a `changed` flag in the opt context, set by every mutating kit call |

**Constant canonicalization.** Each sub-pass call starts by leaving one `$_CONST0_`,
`$_CONST1_`, `$_CONSTX_`, `$_CONSTZ_` node per module (made on demand; lowest ID kept, duplicates
merged in), so keys and rules compare constants by value. Constant `$sop`s are left alone (BLIF round trip, IR-4) but read as
constants through `odin3_net_const_value`.

**Atomicity.** Each rewrite builds everything it needs (new nodes on fresh nets, the provenance
record) before its first destructive step. `NO_MEMORY` during that phase leaves the rewrite
undone; the pass returns `NO_MEMORY` with a valid IR in which every completed rewrite preserved
the module's function (refinement, §13). OOM tests inject a failure at every allocation (§20.1).

## 3. Which opt passes Parmys runs, and where Odin III runs them (OP-3)

**Parmys's invocations** (VTR `synthesis.tcl`, `PARSER=default`; `parmys -nopass` skips the
plugin's own pass list):

| # | Parmys line | Notes |
|---|---|---|
| 1 | `opt_expr` | before `proc`: works on cells outside processes |
| 2 | `opt_clean` | before `proc`: a no-op on every module that has processes (`opt_clean` skips them) |
| 3 | `opt -nodffe -nosdff` | before `proc`: `opt_muxtree`/`opt_clean` skip modules with processes |
| 4 | `procs -norom` | Tcl spelling of `proc -norom` (Yosys renames `proc`, a Tcl keyword); `proc` ends with **`opt_expr -keepdc`** |
| 5 | `fsm` | calls `opt_clean` twice internally |
| 6 | `opt` | the main post-`proc` run; `opt_dff` makes `$dffe`/`$sdff*` here |
| 7 | `wreduce; peepopt; opt_clean; share` | `opt_clean` alone |
| 8 | `opt -full` | `-full` = `opt_expr -mux_undef -mux_bool -undriven -fine`, `opt_reduce -fine`, `opt_share` |
| 9 | `memory -nomap; flatten; opt -full` | `memory` calls `opt_clean` |
| 10 | `techmap -map +/parmys/{adff,adffe,aldff,aldffe}2dff.v; opt -full` | async FFs become sync FFs + logic |
| 11 | `parmys -a … -nopass …; opt -full` | hard-block mapping |
| 12 | `techmap; opt -fast` | gate level |
| 13 | `dffunmap; opt -fast -noff` | enables and sync resets become logic |

**`opt`'s loop** (0.55 `opt.cc`), mirrored exactly (OP-12):

```
default:  opt_expr X; opt_merge -nomux M
          do { opt_muxtree; opt_reduce R; opt_merge M; [opt_share if -full];
               [opt_dff D unless -noff]; opt_clean C; opt_expr X } while (changed)
-fast:    do { opt_expr X; opt_merge M; [opt_dff D unless -noff];
               if (!dff changed) break; opt_clean C } ; opt_clean C
```

`X` = `[-mux_undef] [-mux_bool] [-undriven] [-noclkinv] [-fine] [-full] [-keepdc]`, `R` =
`[-fine] [-full]`, `M` = `[-keepdc]`, `D` = `[-nodffe] [-nosdff] [-keepdc]`, `C` = `[-purge]`.
"changed" is any change by any sub-pass in the iteration (Yosys's `opt.did_something`); in
`-fast` mode only `opt_dff`'s changes loop.

**Odin III's Phase 2 opt script, two modes.** 2D ruled every `opt*` pass `processes = REFUSE`
(EL-33), so as ruled Odin III cannot run lines 1–3 before `proc`. **Mode R** (as ruled) runs them
right after `proc`, which already ended with `opt_expr -keepdc` (§4.8):

```
proc; opt_expr; opt_clean; opt -nodffe -nosdff; opt          # mode R
opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt          # mode P (Parmys's order)
```

**Mode P** (recommended, §24 item 2) runs Parmys's lines verbatim. The opt passes then accept
modules with processes as Yosys does: `opt_expr`, `opt_merge`, `opt_reduce`, `opt_share` and
`opt_dff` treat every `$proc` node as **opaque** (never evaluated, merged or deleted; `O` pins are
drivers of unknown function, `I` pins readers, as EL-33 requires of an `OK` pass), and
`opt_muxtree` and `opt_clean` **skip** a module with processes ("only operates on … modules
without processes"). Body positions are pin indices, so merges never touch a body (EL-33). Both
modes omit `fsm` (Phase 4); the later `opt -full`/`-fast` runs belong to Phase 3/4 pipelines
(`wreduce`, `share`, `memory`, `flatten`, `techmap`), and §20.3 tests those modes already.

*Evidence* (probes, Yosys 0.55, 2026-10-11; 2F-6 commits them as `tools/opt-check/order-probe.sh`;
each variant is the rest of `synthesis.tcl`, without the `parmys` plugin pass, compared by
`netlist-compare` with Parmys's order):

| Variant (inside Yosys) | Micros (331 that compile both ways) | VTR, 24 smallest |
|---|---|---|
| mode R's order (`proc` first, then the trio) | 318 identical; 13 others hold `$pow`/`$mem_v2` (unmapped without the plugin) or multi-driver `inout`s | 8 identical (the smallest); 13 differ — 7 by `netlist-compare` (`sha` 6,785 vs 6,786 `.names`, `dsec`-equivalent; `stereovision3` 955 vs 942), 6 by `.names` count where unmapped `$mem_v2` stops `netlist-compare` (`raygentop` 22,949 vs 19,321: without hard-block mapping arithmetic becomes gates, magnifying word-level differences); 3 undecided (`$mem_v2`, equal counts) |
| `proc`, then nothing or only `opt_expr; opt_clean`, then the rest | — | differs on all 4 designs tried |
| mode P's order with the main `opt` moved **before** `fsm` | 316 of 318 identical (2 `dsec`-equivalent) | `diffeq1/2` identical; `stereovision3` 955 vs **1,288**, `sha`, `blob_merge` differ |

Two conclusions: the opt passes are not confluent (`opt -nodffe -nosdff` run to its fixed point
first changes which enables and resets `opt_dff` absorbs later), so only Parmys's own order
reproduces Parmys on the VTR set; and `fsm` must see raw `proc` output.

**2H splice (recommended, §24 item 3).** Phase 2 has no `fsm`, `wreduce`, `memory`, `flatten`,
`techmap` or hard-block mapping, so the exit comparison runs the rest of Parmys's script in Yosys
0.55 on Odin III's `write_rtlil` output (2D §14.3). **S-P** (mode P): Odin III runs lines 1–4
(`read_verilog -nolatches; hierarchy; opt_expr; opt_clean; opt -nodffe -nosdff; proc`), Yosys
line 5 (`fsm`) onwards; identity is possible wherever 2C–2E match, but only 2F's pre-`proc` work
is exercised, so 2F's main `opt` is gated by the same-input differential `opt-diff` (§20.3).
**S-R** (mode R): Odin III runs `proc` and the trio, Yosys from `fsm`; identical on the micros,
not on most of the VTR set (equivalence verified where `equiv-check` reads the result, e.g.
`sha`). **S-D** (Odin III through the main `opt`, Yosys from `fsm`) is reported for information:
`fsm_detect` takes only `$dff`/`$adff`, so it misses the `$dffe`/`$sdff` registers `opt` made.

## 4. `opt_expr`: constant folding and expression rewriting (OP-4)

### 4.1 Driver

Per module, mirroring `opt_expr.cc` `execute`:

1. with `-undriven` (or `-full`): `replace_undriven` (§4.6);
2. `do { do { rcc(consume_x = false) } while (changed); if (!keepdc) rcc(consume_x = true) }
   while (changed)`, where `rcc` = `replace_const_cells`;
3. `replace_const_connections` has nothing to do: a constant is a net (OP-2).

One `rcc` round: snapshot the **invert map** (Y → A of every 1-bit `$not`, `$logic_not`,
`$_NOT_`; Y → S of every 1-bit `$mux`/`$_MUX_` with A = 1, B = 0); unless `-noclkinv`, fold
inverters into FF polarities (§4.4); then visit the evaluable cells in topological order (ties by
node ID, §17 #4; ID order with §16's info line when a loop prevents sorting) and apply **the first
matching rule** of §4.2–4.5 in the order listed (Yosys's `goto next_cell`).

### 4.2 Rules on logic cells (Yosys order; names are Yosys's `cover` points)

"x" means a bit on the `$_CONSTX_` net only, "undef" x or z,
exactly as each Yosys rule tests (`== State::Sx` versus "not 0/1").

| Rule | Cells | Fires when | Result |
|---|---|---|---|
| `const_and` | `$reduce_and`, `$_AND_`, 1-bit unsigned `$and`/`$logic_and` | an input bit is 0; or a bit and its inverse (invert map) are both inputs; or `consume_x` and an undef input | Y = 0 |
| `const_or` | `$reduce_or`, `$reduce_bool`, `$_OR_`, 1-bit unsigned `$or`/`$logic_or` | a bit is 1; bit and inverse; or `consume_x` and undef | Y = 1 |
| `and_or_buffer` | same | exactly one non-constant input bit, no undef | Y = that bit |
| `const_xor`/`const_xnor` | `$_XOR_`, `$_XNOR_`, 1-bit unsigned `$xor`/`$xnor` | `!keepdc` and (A = B or either is x or z) | Y = 0 (`xor`), all-ones at Y's width (`xnor`) |
| `xor_buffer`/`xnor_buffer` | same | one side constant 0/1 | Y = the other side, or a new `$not`/`$_NOT_` of it; `$xnor` pads Y with 1s |
| `reduce_xnor_not` | `$reduce_xnor` 1-bit A, 1-bit Y | always | becomes `$not` |
| `unary_buffer` | `$reduce_and/or/bool/xor`, `$neg`, 1-bit A, 1-bit Y | always | Y = A |
| `bitwise_logic_one_const` | `$and $or $xor $xnor` | exactly one side fully constant (with `-keepdc`: and not all-undef) | per Y bit by the constant bit (sign/zero-extended to Y): `and` 0→0, 1→other, x→0; `or` 0→other, 1→1, x→1; `xor` 0→other, 1→new `$not`, x→**x**; `xnor` mirrored (`-keepdc`: x bits keep a cell with the x) |
| `invert.double` | 1-bit-Y `$not`, `$logic_not`, `$_NOT_` | A is an invert-map output | Y = the inverter's input |
| `invert.muxsel` | `$mux`, `$_MUX_` | S is an invert-map output | swap A/B, S = the inverter's input |
| bit-gate tables | `$_NOT_ $_AND_ $_OR_ $_XOR_ $_MUX_` | the patterns of `opt_expr.cc` 1100–1190 (`$_AND_`: a 0 → 0, 11 → 1, x with 1 or x → x, `consume_x`: x with a signal → 0, 1 with a signal → buffer; `$_MUX_`: A = B or constant S → that input, A = 1, B = 0 → `$_NOT_` of S, `-mux_undef` drops x inputs) | as Yosys |
| `$tribuf` | `$tribuf` | EN = 1 → Y = A; EN = 0 and A not all-undef → A becomes all x | — |

`$_NAND_`/`$_NOR_` (and `$_XNOR_` beyond `const_xnor`) are not folded, as in Yosys.

### 4.3 Rules on comparisons, shifts and arithmetic

| Rule | Cells | Fires when | Result |
|---|---|---|---|
| `xbit` | `$reduce_xor $reduce_xnor $lt $le $ge $gt` | an **x** bit in A or B | Y = 1-bit x (zero-extended) |
| `xbit` | `$neg $add $sub $mul $div $mod $pow`; `$shl $shr $sshr $shiftx` (B only) | an x bit | Y = all x |
| `trim` | `$shiftx` | A has x MSBs above bit 0 | A narrowed (rebuild) |
| `eqneq.isneq` | `$eq $ne` | after extending A, B to a common width (signed iff both signed), a position holds two different 0/1 constants | Y = 0 (`$eq`) / 1 (`$ne`) |
| `eqneq.empty` | `$eq $ne` | every position has identical bits (same net, or the same constant, x included) | Y = 1 / 0 |
| `eqneq.resize` | `$eq $ne` | some positions identical | those positions dropped (rebuild) |
| `eqneq.swapconst` | 1-bit `$eq`/`$ne` | A constant, B not | A, B swapped |
| (`action`) | 1-bit `$eq`/`$ne`, B constant | B all undef → Y = x; B = 1 (`$eq`) or 0 (`$ne`) → Y = A; else `eqneq.isnot` → `$not` of A | |
| `eqneq.cmpzero` | `$eq $ne` | A or B all zero | `$logic_not` / `$reduce_bool` of the other side |
| `constshift` | `$shl $shr $sshr $shiftx` | B fully constant (`-keepdc`: fully defined) | wiring: Y bit i = A bit (i + shift) (A extended to Y first except `$shiftx`), fill 0, the sign bit (`$sshr` signed) or x (`$shiftx`); shift saturates at A's width |
| `identwrt.a/b` (only `consume_x`) | `$add $or $xor` (A or B zero), `$sub` (B zero); shifts (B zero); `$mul` by 1 or −1; `$div` by 1 | the identity holds | Y = the other operand extended to Y (wiring; Yosys makes `$pos`) or `$neg` of it for −1 |
| `mux_bool` / `mux_invert` (`-mux_bool`) | 1-bit `$mux`, `$_MUX_` | A = 0, B = 1 / A = 1, B = 0 | Y = S / `$not` of S |
| `mux_and` / `mux_or` (`-mux_bool`, `consume_x`) | 1-bit `$mux`, `$_MUX_` | A = 0 / B = 1 | `$and(S, B)` / `$or(A, S)` |
| `mux_undef`, `mux_empty`, `mux_sel01`, `mux_reduce` (`-mux_undef`) | `$mux $pmux` | (A and B all undef) or S all undef → Y = A; B slices that are all undef or whose S bit is undef are dropped; an all-undef A takes the last remaining B slice; none left → Y = A; A = 0, B = 1 → Y = S; else rebuilt as `$pmux` (≥ 2 selects) or `$mux` | |
| `const.$<type>` | every evaluable registry cell: `$not $and $or $xor $xnor $reduce_* $logic_* $shl $shr $sshr $shiftx $lt $le $eq $ne $gt $ge $add $sub $mul $div $mod $pow $neg $mux $pmux` | every input bit constant | Y = the 4-state fold (OP-5) |
| (`$mux` conservative) | `$mux` | S constant (`-keepdc`: and defined) → Y = B if S is 1, else A (so **S = x picks A**); A = B → Y = A | |
| `$pow` strength reduction | `$pow`, A constant one-hot, B unsigned | A = 2 → `$shl(1, B)`; A = 2^k → `$mul(k, B)` feeding `$shl(1, ·)` | |
| `mul_shift.zero` / `.swapped` / `.unswapped`, `mul_low_zeros` (`!keepdc`) | `$mul` | a fully defined constant side: 0 → Y = 0; one-hot (not the signed MSB) → `$shl` by its index; low zero bits on either side → a narrower `$mul` and Y's low bits 0 | |
| `divmod_zero` | `$div $mod` | B fully defined and zero | Y = all x |
| `div_shift` / `mod_mask` (`!keepdc`) | `$div` / `$mod` | B one-hot (not the signed MSB) | `$sshr` by the index (signed truncating: plus `$reduce_or`/`$and`/`$add` correction) / `$and` mask (signed truncating: the remainder bits plus a sign fix) |
| `fine` `$lt $le $gt $ge` (with `-fine`) | | one side fully defined constant | `X<0` unsigned → 0, `X>=0` → 1, `X>~0` → 0, `X<=~0` → 1; one-hot constant 2^k → `$logic_not`/`$reduce_or` of `X[w−1:k]`; a constant above X's range → 0/1; signed `X<0` → MSB, `X>=0` → `$logic_not(MSB)` | |

### 4.4 FF polarity folding

Unless `-noclkinv`: a `CLK`, `EN`, `ARST`, `SRST`, `ALOAD`, `SET`, `CLR` input of a word FF
(`$dff $dffe $adff $adffe $aldff $aldffe $sdff $sdffe $sdffce $dffsr $dffsre $dlatch $adlatch
$dlatchsr $sr`) or a `$memrd`/`$memwr` `CLK` driven by an invert-map output takes the inverter's
input and flips its `*_POLARITY` parameter (rebuild); `$_DFF_P_`↔`$_DFF_N_` and
`$_DLATCH_P_`↔`$_DLATCH_N_` swap types the same way.

### 4.5 `-fine` rules

`fine.group` (`$not $and $or $xor $xnor`): the Y bits are split into groups — both inputs
non-constant, A constant, B constant, both constant (after per-bit normalisation: an `$or` bit with
a 1 → both 1, an `$and` bit with a 0 → both 0, `!keepdc` `$xor`/`$xnor` bits with A = B → 0/1)
— and each group becomes its own unsigned cell; constant-A `$and`/`$or` groups become wiring
(x → 0 for `$and`, → 1 for `$or`), constant-A `$xor`/`$xnor` groups become wiring or `$not`s.
Nothing happens when one group already holds every bit. `fine.neutral_A/B` drop neutral bits (0;
1 for `$reduce_and`) from the inputs of `$logic_not $logic_and $logic_or $reduce_or $reduce_and
$reduce_bool`; `fine.$reduce_and`, `fine.A`, `fine.B` collapse an all-constant input to its one-bit
4-state reduction; `fine` (`$add`/`$sub`) strips low result bits that are copies of an operand
bit (B bit 0; `$add` A bit 0; `$sub` 1 − 1 = 0); `$eq`/`$ne` drop redundant bit pairs and fold
when a union-find over the constant-equal positions merges 0 with 1.

### 4.6 `replace_undriven` (`-undriven`)

Every undriven wire bit (for `$`-named wires only bits something reads) is connected to its
`init` value when that is 0/1, else to x; the init bits used are cleared. 2D drives unconnected
cell inputs with x already (2D §17 #16); this catches wires declared and never assigned.

### 4.7 The `keepdc` flag

`-keepdc` disables every rule marked `!keepdc` or `consume_x` above, makes `constshift` and the
`$mux` rule require *defined* constants, and keeps x bits in `bitwise_logic_one_const` as cells.
`proc`'s trailing `opt_expr -keepdc` (§4.8) is the only `-keepdc` run in Parmys's flow.

### 4.8 Interface with 2E

Yosys's `proc` ends with `opt_expr -keepdc` unless `-noopt`. 2F provides it as
`odin3_opt_expr_run(ctx, module, &opts)`; 2E's `proc` calls it once 2F-2 has landed (§24 item 6).
Until then the Phase 2 script writes `proc; opt_expr -keepdc` explicitly.

## 5. Constant folding semantics and x (OP-5)

Folding a cell whose inputs are all constant evaluates **Yosys `RTLIL::const_<type>`** on 4-state
values (`kernel/calc.cc`), which 2D's evaluator already implements bit-exactly (EL-14, tested
against a Yosys 0.55 table): `fold.c` loads the cell's input pins as an `odin3_cval` (aval/bval
planes; x and z per bit), calls the matching `odin3_cval_*` operator with the cell's `A_SIGNED`,
`B_SIGNED` and `Y_WIDTH`, and connects Y to the result's constant nets. Per cell type:

| Cells | Fold (Yosys `const_*`) | x/z in an input |
|---|---|---|
| `$not $and $or $xor $xnor` | bitwise, operands extended to Y (sign iff `*_SIGNED`) | IEEE 4-state tables: `0 & x = 0`, `1 \| x = 1`, otherwise x; z reads as x |
| `$reduce_and/or/xor/xnor/bool`, `$logic_not/and/or` | one bit, zero-extended to Y | and/or: dominated by 0/1, else x; xor/xnor: any undef → x |
| `$eq $ne` | one bit | a definite mismatch decides; else any undef → x |
| `$lt $le $gt $ge` | one bit | any undef → x |
| `$add $sub $mul $div $mod $neg $pow` | two's complement at Y's width; `$div`/`$mod` truncate; division by zero → x | any undef → all x |
| `$shl $shr $sshr $shiftx` | Yosys `const_shift_worker`; `$shiftx` fills x | an undef bit in B → all x |
| `$mux` | S = 0 → A, 1 → B | S undef: per bit, equal A/B bits kept, others x |
| `$pmux` | Yosys `const_pmux`: S all 0 → A; one bit set → that slice | S with an undef bit, or several bits set → x (simlib); the 1E simulator's OR (PHASE2 #5) is the gate-level reading and is not used for folding |

**Where x is consumed** (PHASE2 #13): exactly where Yosys 0.55 consumes it — the `consume_x`
round (`const_and`/`const_or`, the gate tables, `identwrt`, `mux_and`/`mux_or`),
`bitwise_logic_one_const` and `fine.group` (x with `and` → 0, `or` → 1, `xor` → 0 or x as
listed), `const_xor`/`const_xnor`, the conservative `$mux` rule (S = x picks A), `-mux_undef`,
`replace_undriven`, and `opt_dff` (x controls count as active, x init yields to any constant, x D
in constbits). `opt_merge` compares x as a constant, so cells equal up to x never merge. Every
other rule propagates x (`xbit`, `divmod_zero`, folding). Surviving x stays a `$_CONSTX_` pin,
printed `1'x` by `write_rtlil`, so over the 2H bridge Yosys's own `opt -full` and `write_blif
-undef + unconn` resolve it as in Parmys; Odin III's BLIF writer adopts `unconn` when `lower`
lands (PHASE2 #13, 2D §14.6; not 2F's change).

## 6. `opt_merge`: CSE keying (OP-6)

**Candidates**: live word and bit nodes, except `$mem` (Yosys skips it), `$tribuf`, `$proc`,
port and constant nodes (constants are canonical, OP-2), `$mux`/`$pmux` under `-nomux`, and
`module`/`blackbox`/`hard` nodes (Yosys merges user cells only with `-share_all`, which Parmys
never passes); with `-keepdc`, FFs whose init is not fully defined.

**Key** of a node: its cell type ID; its parameters in declaration order (values compared
exactly: INT, BITS bit by bit, STRING by strtab ID); and its input pins as a sequence of
**operands** — a net ID, or a constant value (0/1/x/z) when `odin3_net_const_value` says the net
is constant, so a constant `$sop` and a `$_CONST1_` pin agree. Normalisations, from
`hash_cell_inputs`/`compare_cell_parameters_and_connections`:

- `$and $or $xor $xnor $add $mul $logic_and $logic_or $_AND_ $_OR_ $_XOR_`: the A and B operand
  vectors ordered (lexicographic on (kind, value)) — the commutative swap;
- `$reduce_xor $reduce_xnor`: A's operands sorted (a multiset);
- `$reduce_and $reduce_or $reduce_bool`: A sorted and de-duplicated (a set);
- `$pmux`: the (S bit, B slice) pairs sorted, then A;
- FFs (the word family of OP-9 and `$_DFF_*`/`$_DLATCH_*`/`$_FF_`): plus the init value of Q
  (§9.4), so registers with different init never merge.

Outputs are not part of the key. A 64-bit hash of the encoded key indexes a `u64map` with a
collision chain; equality is checked on the full key, so a collision costs time, never correctness.

**Procedure** (per module, Yosys's `while (did_something)`): visit candidates in **ID order**;
look the node up; on a hit, the earlier node is the survivor unless only the later one has
`keep` (then they swap); each output bit of the dropped node is connected to the survivor's
(`kit_connect`), Q init is moved as Yosys does, the dropped node is deleted, and the class is
remembered for provenance (OP-15). Repeat until a sweep merges nothing (a merge can make two
downstream keys equal). Count: `opt_merge: removed N cells`.

**Provenance.** After the sweeps each class with drops is closed with one operation: the survivor
is rebuilt (same type, parameters and nets) with a DERIVED record whose parents are the
survivor's record followed by each dropped node's record in ID order (IR-12 "clumping"), so a
provenance name still leads with the survivor's source (§24 item 4).

## 7. `opt_muxtree` (OP-7)

A faithful iterative port of `OptMuxtreeWorker`. Per module: index every `$mux`/`$pmux` port
(each B slice with its S bit, plus A); a net is "seen non-mux" when a non-mux cell, a mux S input,
a `$port_out` or a `keep` wire uses it; a mux is a **root** when an output bit is seen non-mux
(a "pure" root) or feeds more than one mux. From each root, evaluate the tree with **knowledge**
(reference-counted known-active / known-inactive control nets, a visited mark per mux): a
constant-1 S port is the only live one; a port whose S is known active wins; otherwise every port
not constant-0 and not known-inactive is evaluated, which marks the other ports' S nets inactive
and its own active while its input muxes are visited; inputs of a non-root mux whose bits are
known (an S net of an enclosing port, or a control net of the mux itself) are replaced by 0/1
(`replace_known`); a non-pure root met below another root is evaluated in place up to three
levels deep, else re-queued as non-pure. Ports never enabled are dead: a mux with one live port
becomes wiring, with several a narrower `$pmux`/`$mux`, with none it is deleted. The recursion of
`eval_mux`/`eval_mux_port` becomes an explicit frame stack (CLAUDE.md: no recursion); a global
budget of **100,000** evaluations per module, as in Yosys, stops the analysis with the warning of
§16 and changes nothing in that module. Count: `opt_muxtree: removed N multiplexer ports`.

## 8. `opt_reduce` (OP-8)

Per module, until a worker run changes nothing (Yosys re-runs the worker while its count is
non-zero):

- `$reduce_or`, `$reduce_and` (in that type order): inputs sorted and de-duplicated; a 1 (or) /
  0 (and) input makes the input that constant alone; neutral constants are dropped; an input bit
  driven by a reduction of the **same type** is replaced by that reduction's inputs, whatever the
  child's fanout (the child is processed first — Yosys's recursion becomes a post-order worklist —
  and stays until `opt_clean` finds it dead). Empty → the neutral constant. Rebuild when the
  input changed.
- `$mux`/`$pmux`: with `-fine` (or when the mux feeds a memory write enable, Yosys's
  `mem_wren_sigs`), `opt_mux_bits` merges Y bits whose input tuples are equal and removes bits whose
  tuple is one repeated value; then `opt_pmux` merges B slices equal to A (dropped) or to each
  other (their S bits ORed by a new `$reduce_or`, in first-occurrence order); no S left → Y = A;
  one left → `$mux`. Yosys compares every slice with every later one (O(S_WIDTH²) per cell; 140 s
  of lenet.v's 334 s, §19); Odin III groups slices through a hash of their operand vectors, O(B
  pins), with the same grouping and order.

Count: `opt_reduce: performed N changes`.

## 9. `opt_dff` and the FF cells (OP-9)

### 9.1 The FF model

`ff.c` ports `kernel/ff.cc` `FfData`: width; `has_clk/ce/srst/arst/aload/sr`, `ce_over_srst`;
polarities; the nets of `CLK EN SRST ARST ALOAD AD SET CLR D Q`; `val_arst`, `val_srst`,
`val_init` (4-state); `ff_load`, `ff_slice`, and `ff_emit`, which picks the type as
`FfData::emit` does (no clock and no load: an async reset becomes a load, nothing left becomes
Q = init; else `$sr`, `$dlatch`/`$adlatch`/`$dlatchsr`, `$dffsr(e)`, `$adff(e)`, `$aldff(e)`,
`$sdff`/`$sdffe`/`$sdffce`, `$dff(e)`) and rebuilds (OP-2). Bit-level `$_DFF_P_/N_`, `$_FF_`,
`$_DLATCH_P_/N_` load as fine FFs; the other fine types are not in the registry, so a fine FF only
gets the type-preserving rules (removal, constant bits; §17 #5).

### 9.2 IR change D — the FF cells (for Peter, §24 item 1)

The registry has `$dff $dffe $adff $sdff`. `proc` (2E, Yosys `proc_dff`/`proc_dlatch`) emits `$dff`,
`$adff`, `$aldff` (non-constant async value), `$dffsr` (several async rules), `$dlatch`; `opt_dff`
emits `$dffe`, `$adffe`, `$aldffe`, `$sdff`, `$sdffe`, `$sdffce`, `$adlatch`, and
`$dffsre`/`$dlatchsr`/`$sr` from SR forms. **IR change D adds eleven cells with Yosys's simlib ports
and parameters**: `$adffe $sdffe $sdffce $aldff $aldffe $dffsr $dffsre $dlatch $adlatch $dlatchsr
$sr`, one family file (`src/ir/cells/ff.c`, as `dff.c` groups the bit FFs), each with `verify`
(widths, polarities 0/1, `*_VALUE` BITS of `WIDTH`) and no `const_value`. The purely synchronous
ones (`$sdffe`, `$sdffce`, and the existing `$dff $dffe $sdff`) get `simulate` hooks (`SEQ_EDGE |
CLOCK_PIN0`); 1E's cycle model has no asynchronous event, so the others stay `simulate = NULL` and
are checked through the bridge (§20.2). 2E needs three first (`$aldff`, `$dffsr`, `$dlatch`);
whichever of 2E-1 and 2F-1 lands first adds the family. DESIGN §5.2's word list is amended.

### 9.3 Rules, per FF (Yosys `OptDffWorker::run`, in order)

Each FF from a work list (initially every FF in ID order; FFs emitted by a rule are pushed):

1. width 0 → removed.
2. SR: per bit, CLR always active (or x, `!keepdc`) → Q bit = 0; SET always active → Q bit =
   `~CLR`-gated; never-active CLR (or SET) with a uniform other side → `$adff` with all-1 (all-0)
   reset; CLR and SET of one polarity on complementary bits → `$adff` with the derived value.
3. ALOAD never active (or x) → dropped; always active → combinational (mux/and/or of AD with the
   SR/ARST terms); constant AD without ARST/SR → ARST with value AD.
4. ARST never active → dropped; always active (or x) → Q = the reset value; the FF is removed.
5. SRST never active → dropped; always active (or x) → D = the reset value, SRST dropped (EN too
   unless `ce_over_srst`).
6. EN never active (or x) → with SRST and not `ce_over_srst`: EN := SRST, D := reset value; else
   (unless `keepdc` with an undefined init) the D path is dropped (latch, SR or constant); EN
   always active → dropped.
7. constant CLK → the D path dropped (or D := Q under `keepdc` with undefined init).
8. D = Q → with SRST: EN := SRST (ANDed with an existing `ce_over_srst` EN), D := reset value;
   else the D path dropped.
9. no clock, ALOAD with AD = Q → ALOAD dropped.
10. **SRST merging** (not `-nosdff`; clocked, D ≠ Q, no ARST/SR): per bit, follow D down
    single-user one-select `$mux` bits that have a constant (consistent with the reset value so far)
    on one side and not Q on the other; bits are grouped by their set of (S, polarity) controls;
    each group becomes a sliced FF with `SRST` = the controls combined (`$reduce_or`/
    `$reduce_and` with `$not`s for mixed polarity) and the collected reset value.
11. **EN merging** (not `-nodffe`): per bit, eat single-user one-select muxes with Q on one side;
    then `find_muxtree_feedback_patterns` (iterative) collects the select patterns under which a
    deeper mux tree feeds Q back, **replacing those Q feedback inputs of the muxes by x**; groups
    by (patterns, controls) become sliced FFs with `EN` = `$reduce_and` of `$ne(pattern)`s and
    controls.
12. a changed FF is re-emitted.

Then **constbits** (`run_constbits`, no `-sat`): per bit, `combine_const` (x yields unless
`keepdc`; 0 against 1 → not constant) folds init, the ARST and SRST values, the SR constants and a
constant D (clocked) or AD (load); a bit that stays constant, x included, gets Q tied to it and is
sliced off. Constant register pipelines thus collapse one stage per `opt` iteration (31 on
mcml.v, §19).

### 9.4 Init values

2D stores initial values as the BITS attribute `init` on wires (EL-40, Yosys's convention).
`ffinit.c` is `FfInitVals`: the init of a net is its bit in the `init` attribute of its primary
wire or of any alias membership (first defined value; a conflict is opt_clean's warning, §10);
`set_init`/`remove_init` rewrite those attribute bits (an attribute that becomes all x is
removed). FFs read the init of their Q nets. This binds 2E: `proc` must leave init on the wires
(as Yosys does), not move it into a parameter (§24 item 7).

Counts: `opt_dff: N flip-flops changed, M constant bits`.

## 10. `opt_clean` (OP-10)

Per module, as `rmunused_module`:

1. `$_BUF_` nodes without `keep` become wiring (Yosys removes `$pos`/`$_BUF_` first).
2. **Roots**: `$port_out` and `$port_inout` pins; nets of wires with `keep`; nodes with `keep`
   (OP-2, including the module-type rule). **Liveness** spreads backwards from root nets to their
   drivers and from a live node to the drivers of every input pin, with "unknown" nodes (module,
   black box, hard) treated as Yosys does: all their pins count as both driver and reader, so an
   unknown node that only *reads* a live net is live too. `$mem`: a live read port keeps the
   memory. Mark-and-sweep with an explicit worklist, O(pins).
3. Dead nodes are deleted in ID order; a dead FF's Q init bits are removed.
4. Nets left with no pins, no wire membership and no aliases are deleted.
5. A wire is deleted when every one of its nets has no pins, unless it is a port wire, has `keep`
   or a defined `init` bit; with `-purge` also a wire whose name does not start with `$`
   (Yosys's public names) — its nets stay, only the name goes. Yosys also drops `$`-named wires
   that are still used; Odin III keeps them as names (IR-2; §17 #3).
6. `init` cleanup (`rmunused_module_init`): an init bit on a net that no FF drives is removed; on
   a constant net whose value differs it is removed with the warning of §16.

Count: `opt_clean: removed N unused cells, M unused nets and W unused wires`. Yosys's
"driver-driver conflict" warning has no counterpart (check rule 4).

## 11. `opt_share` (OP-11, `-full` only)

A port of `opt_share.cc`: for each `$mux`/`$pmux` whose input ports are each driven entirely by
one cell of one mergeable class (`$add`/`$sub` together; otherwise equal type; the operation's
output used only by that mux port) and where the cells share one operand (same nets, sign and
port semantics), the cells are replaced by one cell on the shared operand and a new mux over the
non-shared operands (`$sub` operands of an `$add`/`$sub` mix are negated as Yosys does). Yosys
orders candidate operands in a `std::map` keyed by SigSpec, i.e. by **wire name**; Odin III
orders by (net ID) — deterministic, but where several merges compete the choice can differ
(§17 #6). Count: `opt_share: merged N cells`.

## 12. The loop, the fixed point and termination (OP-12)

**Loop.** As §3, per design (all modules per sub-pass, modules in creation order), never on a
module twice per sub-pass call. In mode P a module with live `$proc` nodes is skipped by
`opt_muxtree` and `opt_clean` and seen by the others with every `$proc` opaque (§3). In Debug the
pass manager's `check` runs before and after `opt`; `opt` itself runs `check --fast` after every
sub-pass call (agent default; Release: off).

**Termination of each sub-pass invocation.** Every inner loop is over a finite work list that only
shrinks or is refilled by strictly simplifying rewrites: `opt_expr`'s rounds stop when a round
rewrites nothing, which the ranking below bounds; `opt_merge` sweeps strictly decrease the number of
candidates; `opt_muxtree` is bounded by its evaluation budget; `opt_reduce`'s worker runs strictly
decrease the total reduction-input count or the mux port count; `opt_dff`'s work list receives only
sliced FFs, whose bit count is smaller than the FF they came from, or FFs whose feature set strictly
grew (≤ 6 features); `opt_clean` is one sweep.

**Termination of the loop — ranking argument.** Measured at iteration boundaries (after
`opt_clean`), let Φ = ⟨W, F, D⟩ lexicographically, where

- W = Σ over live nodes other than ports, constants and FFs of `cost(type) × output bits`, where `cost`
  strictly decreases along every strength reduction of §4, in the order `$pow`, `$mul`, `$div` and
  `$mod`, `$add` and `$sub`, shifts, comparisons, `$pmux`, `$mux`, bitwise, `$logic_*` and
  `$reduce_*`, `$not` (every cell costs more than wiring);
- F = Σ over FF bits of their controls (EN, SRST, ARST, ALOAD, SR, CLK) at a constant or x level;
- D = Σ over FF D bits of the depth of D's single-user mux cone.

Folding, identities, wiring, CSE, mux-port removal, dead-cell removal and constbits decrease W.
The rules that **create** cells (`xor_buffer`, `fine.group`, `div_shift`, `mod_mask`, `$pow`,
`opt_reduce`'s `$reduce_or`, `opt_share`'s mux, `opt_dff`'s control logic) consume their pattern
and produce cells on which they cannot fire again (a group is homogeneous, `$not` is not `$xor`,
`$sshr` is not `$div`), so each fires at most once per node lineage: at most K × (initial node
bits) firings, and W (which leaves FFs out) decreases between them. Removing a constant control
decreases F; SRST/EN merging decreases D (a D bit moves up a finite acyclic mux cone). So Φ
cannot decrease forever. This is a sketch over the rule table, not a mechanised proof, hence:

**Cap.** `ODIN3_OPT_MAX_ITERATIONS` = 10,000 iterations per `opt` invocation (Yosys has no cap; the
probe's worst case is 31, mcml.v). Reaching it logs the warning of §16 and returns `ODIN3_OK` with
the current IR (valid, a refinement of the input); a corpus test asserts it is never reached
(§20.4). Further caps: `opt_muxtree`'s 100,000 evaluations per module (Yosys's), and `opt_merge`'s
collision chain is unbounded but linear in candidates. A test hook lowers each cap.

## 13. Correctness notion: refinement (OP-13)

Every sub-pass call must produce a module that **refines** its input: for every input sequence and
every reachable state, each output bit that is defined (not x) in the input module has the same
value in the output module; x may become anything (Yosys's `opt` is a refinement in exactly this
sense; `-keepdc` makes it an equivalence). Initial values follow the same rule (x init may become
a value). This is what §20.2 checks after every sub-pass.

## 14. Determinism (OP-14)

- Iteration in ID order (IR-16), topological ties by node ID, new objects appended; no hash-map
  iteration reaches the IR (`opt_merge` only looks up; Yosys's `pool` orders become ID order); no
  pointer, address, time or randomness decides anything.
- Tested (§20.5): two runs give byte-identical `write_rtlil`; reordered input gives a
  `netlist-compare`-identical result.
- The pass manager compacts after each top-level `opt` (IR-6's "after `opt`"), not after
  sub-passes.

## 15. Provenance (OP-15)

Each rewrite is one operation (`odin3_prov_begin_op`); the objects it creates share one DERIVED
record (IR-12/13):

| Rewrite | New objects | Parents |
|---|---|---|
| fold to a constant, identity, buffer, wiring, dead removal | none: Y's names become aliases of the source net (IR-2); the deleted node keeps its record and gets a tombstone at `compact` | — |
| rebuild of one node (type, width, parameter or port change; FF re-emit) | the new node and its fresh nets | the old node |
| a rule that adds helper cells (`$not`, `$reduce_or`, FF control logic, `$pow` → `$mul` + `$shl`) | the helpers | the cell(s) the rule consumed: the FF and every mux it absorbed, in ID order |
| CSE class (OP-6) | the rebuilt survivor | survivor, then the dropped nodes in ID order |
| FF slicing | each slice | the FF, then absorbed muxes |
| `opt_share` merge | the shared cell and the new mux | the merged cells in ID order, then the old mux |

Forward navigation from `foo.v:42` thus finds the dead `$add` (slot or tombstone) and everything
made from it; a provenance name leads with the first parent's source. `kit_connect`'s merges are
rewiring (IR-15): nets keep their records, the dropped net dies with its own.

## 16. Diagnostics (OP-16)

Style as 2D §9 (lower case, names in `'…'`, no final period); `<cell>` is a node's name or
`$c<ID>`, `<net>` a net's name, wire bit `w[k]` or `$n<ID>`. Pass lines are INFO, per-rewrite
lines DEBUG.

| Level | Exact text | When |
|---|---|---|
| INFO | `opt_expr: N cells rewritten, M cells folded to constants` | end of each `opt_expr` call (design totals) |
| INFO | `opt_merge: removed N cells` | each call |
| INFO | `opt_muxtree: removed N multiplexer ports` | each call |
| INFO | `opt_reduce: performed N changes` | each call |
| INFO | `opt_share: merged N cells` | each call |
| INFO | `opt_dff: N flip-flops changed, M constant bits` | each call |
| INFO | `opt_clean: removed N unused cells, M unused nets and W unused wires` | each call |
| INFO | `opt: fixed point after K iterations` / `opt: -fast: done after K iterations` | end of `opt` |
| INFO | `opt_expr: module 'm': cells could not be sorted topologically; visiting them in ID order` | a combinational loop (Yosys: "Couldn't topologically sort cells…") |
| INFO | `opt_muxtree: module 'm' has processes; skipped` (likewise `opt_clean`) | mode P (Yosys warns: "…contains processes") |
| DEBUG | `opt_expr: module 'm': <rule> on <type> '<cell>'` (rule = §4 name) | every `opt_expr` rewrite |
| DEBUG | `opt_merge: module 'm': '<cell>' is identical to '<cell>'` | every merge |
| DEBUG | `opt_muxtree: module 'm': dead port K/N on <type> '<cell>'` | every dead port |
| DEBUG | `opt_dff: module 'm': <action> on <type> '<cell>'`; `<action>` ∈ `removing never-active <port>`, `handling always-active <port>`, `handling const clock`, `handling D = Q`, `adding SRST`, `adding EN`, `setting constant <v> at bit K` | every `opt_dff` change |
| DEBUG | `opt_clean: module 'm': removing unused <type> '<cell>'` | every removed node |
| WARN | `opt: module 'm': no fixed point after 10000 iterations; stopping` | the cap (OP-12) |
| WARN | `opt_muxtree: module 'm': giving up after 100000 evaluations` | Yosys's "Giving up (too many iterations)" |
| WARN | `opt_clean: module 'm': initial value conflict for <net>: driven by 1'<v>, init 1'<i>` | Yosys's "Initial value conflict" |
| ERROR | `pass 'opt': module 'm' has processes; run proc first` | pass manager, EL-33, mode R only (each `opt*` pass `REFUSE`); in mode P the passes are `OK` |
| ERROR | `<pass>: unknown argument '<arg>'` | an unknown option (`INVALID_ARG`, the 1D style) |

Options are spelled as in Yosys (`-full`, `-nodffe`, …) so Parmys's lines run verbatim; `--full`
is accepted too (agent default). `--opt-stats` prints one `opt: rule <name> N` line per non-zero
rule or `opt_dff` action counter, sorted by name: the counterpart of Yosys's `YOSYS_COVER_DIR`.

## 17. Divergences (from Yosys 0.55 / Parmys)

| # | Odin III | Yosys/Parmys | Why / effect |
|---|---|---|---|
| 1 | mode R only: the pre-`proc` trio runs after `proc` | before `proc` | EL-33's `REFUSE`; identical on 318/331 micros, not on ≥ 13 of the 24 smallest VTR designs (§3); mode P removes it |
| 2 | identities are wiring; no `$pos` | `$pos` until `techmap` | IR-1; same netlist after lowering |
| 3 | used `$`-named wires keep their names | dropped by `opt_clean` | IR-2; names never reach `netlist-compare` |
| 4 | ties and candidate order by node ID | by name or container order | determinism without names; may change which of two equal rewrites fires first |
| 5 | rules on cells outside the registry are not ported; fine FFs keep their type | `$alu`, `$eqx`, `$_DFFE_*`… rules | those cells do not occur in Odin III's Phase 2 IR |
| 6 | `opt_share` operand order by net ID | `std::map` by wire name | competing merges may differ; equivalent |
| 7 | CSE survivor = lowest ID (unless `keep`) | first in the cell dict | names only |
| 8 | x-with-init substitution only in `replace_undriven` | also in `opt_clean` | rare (2D folds `initial`-only regs, EL-40); x stays x, a refinement |
| 9 | loop cap 10,000 | none | safety net |
| 10 | `compact` after `opt` | — | IR-6 |
| 11 | Odin III log text (§16) | Yosys text | — |
| 12 | no `opt_dff -sat` | available | Parmys does not use it |

## 18. Files, interfaces and data structures (OP-17)

| File | Content |
|---|---|
| `src/passes/opt/opt.h` | `odin3_opt_opts` (§3 flags), `odin3_opt_run` (the loop), `odin3_opt_<sub>_run(ctx, design, &opts, &counts)` per sub-pass, `odin3_opt_expr_run` (module scope, for 2E) |
| `src/passes/opt/kit.c/.h` | opt context (`changed`, counters, caps), constant nets, `kit_connect`, `kit_rebuild`, `kit_new_cell`, fanout counts, invert map, topological order (Kahn, explicit queue), dirty sets |
| `src/passes/opt/fold.c` | pins ↔ `odin3_cval`, dispatch to 2D's operators |
| `src/passes/opt/expr*.c` | §4, one function per rule, a rule table per cell type (lizard: ≤ 60 lines, CCN ≤ 15) |
| `src/passes/opt/{merge,muxtree,reduce,share,clean}.c` | §6, §7 (explicit frame stack), §8, §11, §10 |
| `src/passes/opt/{ff,ffinit,dff}.c` | §9 |
| `src/passes/opt/register.c` | the eight pass definitions and option parsing |
| `src/ir/cells/ff.c` | IR change D |
| `tools/opt-check/` | `opt-rules`, `opt-equiv`, `opt-diff`, `gen-fold-table`, `order-probe.sh` |
| `tests/unit/test_opt_*.c`, `tests/opt/`, `tests/bench/bench_opt.c` | §20 |

No public ABI change (passes are reached through the pass manager); `docs/PASSES.md` gains eight
rows. Side arrays indexed by net or node ID (fanout counts, liveness marks, order, mux port
tables) come from `util/vec`, sized at the store length and freed after each call; the CSE table
is a `u64map` plus a chain `vec`; `fold.c` uses a per-call arena.

## 19. Performance (OP-18)

**Yosys 0.55 measured** (i7-13700 WSL2, 2026-10-11; `synthesis.tcl` lines 1–9 with
`vtr_primitives.v`; `yosys -d` per-pass totals):

- **mcml.v**: 14,817 cells after `proc` → 5,560 after `opt` → 5,335 → 4,154 after the last
  `opt -full`; 34.9 s wall, 259 MB peak. The `opt` family is **32.7 s** of it: `opt_clean`
  13.5 s (73 calls), `opt_dff` 9.3 s (68), `opt_expr` 4.0 s (74), `opt_muxtree` 2.7 s,
  `opt_merge` 1.6 s, `opt_reduce` 0.9 s, `opt_share` 0.7 s. The post-`fsm` `opt` alone iterates
  **31 times**, each late iteration removing one constant register stage (`opt_dff` constbits).
- **lenet.v** (koios, 11 MB): 661,069 cells after `proc` → 73,479 after `opt` → 67,700 →
  14,341; **6 min 49 s** wall, **7.1 GB** peak. The `opt` family is **334 s**: `opt_reduce`
  139.6 s (21 calls; its `$pmux` slice matching is quadratic in `S_WIDTH`), `opt_clean` 72.0 s,
  `opt_expr` 49.7 s, `opt_muxtree` 30.0 s, `opt_dff` 25.1 s, `opt_merge` 13.9 s, `opt_share`
  3.9 s. Iterations: 3 (post-`fsm` `opt`), 5 and 12 (`opt -full` runs).

Yosys's cost is whole-module rescans, with a SigMap rebuilt per call, times iterations. Odin III
keeps the loop (same results) and makes each call cheap: no SigMap, O(1) drivers, contiguous
fanout (IR-18), ID-indexed side arrays, and after the first iteration `opt_expr`, `opt_dff` and
`opt_merge` start from a **dirty set** (nodes whose input nets were connected, merged or
re-driven since that sub-pass last ran, plus their readers); `opt_muxtree`, `opt_reduce` and
`opt_clean` stay O(pins) sweeps. A rule depends only on a node's inputs, their drivers and fanout
counts, so a clean node cannot newly match; the Debug option `--opt-full-rescan` asserts that
full sweeps give the same result.

| Measure (Release, dev machine, `tests/bench/bench_opt.c`, smallest first) | Target |
|---|---|
| mcml.v, the opt passes of the Phase 2 script (the trio and the main `opt`, either mode) | ≤ 1.0 s |
| mcml.v, `opt -full` on that result | ≤ 0.3 s |
| lenet.v, the opt passes of the Phase 2 script (≈ 661k cells after `proc`) | ≤ 15 s |
| lenet.v, peak RSS growth over the post-`proc` IR (side arrays, CSE table, dead objects before `compact`) | ≤ 25% |
| the 31 VTR files, opt passes of the Phase 2 script, total | ≤ 3 s |
| per sub-pass call | O(pins) full sweep; O(dirty pins) incremental; `opt_muxtree` O(mux ports × budget) worst case; `opt_merge` O(candidate pins) expected |
| per `opt` invocation | O(iterations × pins) worst case; iterations ≤ sequential depth of constant-register chains + a small constant on the corpus |

The bench prints per file cells/nets/pins before and after, iterations, time per sub-pass, rewrites
per rule family and peak RSS; the output goes verbatim into `docs/PHASE2.md`. A missed target is
investigated and reported, never relaxed.

## 20. Testing (OP-19)

Every test runs under ASan/UBSan in Debug with `check` around every pass. Sweeps run **smallest
input first** (memory rule); every Yosys run is 0.55 unless the line says 0.69.

### 20.1 Unit tests (Unity)

- `test_opt_kit.c`: constant canonicalization (duplicate constant nodes, a constant `$sop`),
  `kit_connect` (aliases kept, to a constant, the `keep == drop` loop, a port on the dropped net),
  `kit_rebuild` (names, attributes, provenance), the invert map, topological order with a loop;
  OOM at every allocation of every kit operation (`NO_MEMORY`, valid IR, rewrite undone).
- `test_opt_fold.c`: every foldable type at widths 1, 7, 64, 65, 200, signed and unsigned, with x
  and z, against 2D's Yosys 0.55 constant table and a cell table from `tools/opt-check/
  gen-fold-table` (random constant cells through Yosys `opt_expr`, committed with the version);
  `$pmux` with several hot bits and x selects.
- **Per-rule goldens** `tests/opt/rules/<subpass>/<rule>.v` + `.il` (≈ 140): one tiny module
  per rule of §4–§11 and per `opt_dff` action, built so that rule fires first; `read_verilog
  -nolatches; proc; opt_expr -keepdc; <sub-pass>; write_rtlil` is golden-compared, and the runner
  `tools/opt-check/opt-rules` asserts the rule's `--opt-stats` counter is ≥ 1, that Yosys 0.55 on
  the same input fires the same `cover` point (`YOSYS_COVER_DIR`, verified on 0.55) with the
  same cell histogram, and that the output refines the input (§20.2). Bit-gate, `$_DFF_*` and
  `$sop` rules start from `read_blif`.
- `test_opt_cse.c`: every key normalisation, FF init, `-nomux`, `-keepdc`, the keep swap, forced
  hash collisions, cascaded merges, the provenance parents.
- `test_opt_muxtree.c`: Yosys's `opt_muxtree` test patterns, a tree deeper than a 1 MB stack
  would allow recursively, the 100,000 budget (test hook).
- `test_opt_dff.c`: `ff_emit` for all feature combinations against `FfData::emit`, every §9.3
  action, constbits with x init, init conflicts.
- `test_opt_loop.c`: the call sequences of each mode, the iteration cap (warning, `ODIN3_OK`,
  valid IR), mode R's refusal and mode P's skips with their exact messages.

### 20.2 Refinement after every sub-pass (`opt-equiv`)

`tools/opt-check/opt-equiv <files> [--top T]`: runs the Phase 2 script with `--opt-snapshot <dir>`,
a debug option that writes `write_rtlil` before and after **every sub-pass call** that changed
something; each pair goes through Yosys 0.55 `read_rtlil before.il; rename -top gold; read_rtlil
after.il; rename -top gate; equiv_make gold gate eq; hierarchy -top eq; flatten; equiv_simple -undef
-seq 4; equiv_induct -undef; equiv_status -assert` — a **refinement** check (x in the gold is a
don't-care; probe: `s ? a : 4'bx` → `a` passes, the reverse and a wrong enable fail). Inputs holding
processes (mode P) get Yosys `proc` on both sides first. A failing pair names the sub-pass call and
rule counters. A cheaper in-process check runs alongside when the module has no x constant, no
asynchronous FF and no cell without a `simulate` hook (`$shiftx`, `$pow`): 1E's simulator, 64 cycles
× 3 seeds, before/after copies. Run on: every per-rule golden (CTest), the micros (CTest, label
`opt_equiv`, smallest first), the VTR set (nightly).

### 20.3 Differential against Yosys on the same input (`opt-diff`)

`tools/opt-check/opt-diff <files> [--top T] [--script S]`, per design:

1. Odin III: `read_verilog -nolatches; hierarchy; proc; opt_expr -keepdc; write_rtlil in.il; S;
   write_rtlil odin.il` with `--opt-stats` (for mode P's trio, `in.il` is written before `proc`);
2. Yosys 0.55: `read_rtlil in.il; S; write_rtlil yosys.il` with `YOSYS_COVER_DIR`;
3. **per-module cell-type histograms** equal (Yosys `stat`, Odin III `stats`): the structural
   count target;
4. `yosys.il` refined by `odin.il` (§20.2's check with Yosys's result as gold), so Odin III
   resolved no x differently where Yosys kept it defined;
5. **identity after a common lowering**: both through Yosys `flatten; techmap; opt -fast;
   dffunmap; opt -fast -noff; opt_clean -purge; write_blif -undef + unconn`, then
   `netlist-compare` (exit 0);
6. the per-rule counters of both, printed for diagnosis (not a gate).

`S` ∈ {the Phase 2 script, `opt -full`, `opt -fast`, `opt -fast -noff`, `opt -nodffe -nosdff`}.
CTest `opt_diff_micro` runs every micro with the first two; nightly runs the VTR set, mcml.v last,
with all five. A difference fails unless `tests/opt/diff_expect.txt` lists it with a reason.

### 20.4 Corpus, caps and fuzzing

- `opt_micro` / `opt_vtr`: the Phase 2 script on every micro and VTR file: `ODIN3_OK`, `check`
  FULL, iterations below the cap (the maximum is logged).
- Differential generator: 2D's `tools/verilog-gen --elab` with an `--opt` weighting (constants and
  x in operands, `case` with and without `default`, repeated sub-expressions, enable and reset
  nests, constant shifts, power-of-two multiplies) through `opt-diff` steps 3–5: 200 seeds in
  CTest when Yosys is found, 5,000 nightly; the same files through 0.69 nightly as a drift report.
- Crash fuzz `tests/fuzz/fuzz_opt.c` (libFuzzer): bytes → BLIF or a small word-cell builder →
  every `opt` mode with lowered caps; `ODIN3_OK` or injected `NO_MEMORY`, then `check` FULL.

### 20.5 Determinism and provenance

Two runs → byte-identical `write_rtlil`; a module built in reverse creation order →
`netlist-compare`-identical output; after `opt` on a duplicated expression,
`odin3_prov_index_by_loc` on its line reaches the rebuilt survivor and the dead duplicate.

### 20.6 The 2H handover

2F hands 2H the S-P splice (S-R if mode R stays; §3): Odin III's output through `write_rtlil` and
Yosys 0.55's remainder of `synthesis.tcl` from `fsm`, compared with the Parmys golden by
`netlist-compare`, then `equiv-check` (with `--x-report` from 2D §14.6 where needed); S-D is
reported as information. 2F's own gate is §20.3; the exit-bar table is 2H's. In mode P,
`opt-equiv` (§20.2) and `opt-diff` also run on the pre-`proc` calls (inputs with `$proc` nodes
go to Yosys as RTLIL processes, EL-48).

## 21. Review focus and risks

1. **Pass order** (§3, §17 #1): `opt` is not confluent, so identity with Parmys on the VTR set
   needs Parmys's own order (mode P, which reopens EL-33's `REFUSE` for the opt passes) and a
   splice before `fsm`; 2F's main `opt` is then proven by `opt-diff`, not by the exit test.
2. **`opt_dff`'s feedback patterns** write x into mux inputs (§9.3 item 11); the refinement check
   must see Q's init.
3. **`opt_muxtree`'s re-entrant evaluation** (pure/non-pure roots, depth 3) is the hardest code to
   make iterative faithfully; its tests replay Yosys's cases.
4. **Dirty sets** (§19) could miss a rewrite; `--opt-full-rescan` guards them.
5. **2E coupling**: IR change D, `init` on wires, and the trailing `opt_expr -keepdc` (§24 items
   1, 6, 7) must match the 2E spec written in parallel.

## 22. Implementation split (OP-20)

Six PRs; the plan fixes tasks and interfaces. PHASE2's 2F row: Opus `high` (IR change D: Opus
`xhigh` for its review, then `high`).

| PR | Content | Depends on | Model/effort |
|---|---|---|---|
| 2F-1 kit | IR change D (if 2E has not landed it), opt context and kit, constant canonicalization, FF model and init, `opt_clean`, the eight pass registrations with options and `REFUSE`, `--opt-stats`, `--opt-snapshot` | 2D-1 (cells, `$proc` flag, `write_rtlil`) | Opus `xhigh` → `high` |
| 2F-2 expr | `fold.c` on 2D's evaluator, `opt_expr` §4 all rules, `replace_undriven`, polarity folding; `odin3_opt_expr_run` for 2E; per-rule goldens for §4 | 2F-1, 2D-2 | Opus `high` |
| 2F-3 merge/reduce/muxtree | §6, §7, §8; goldens | 2F-1 | Opus `high` |
| 2F-4 dff | §9: all actions, SRST/EN merging, feedback patterns, constbits; goldens | 2F-1, 2F-2 | Opus `high` |
| 2F-5 loop | `opt` driver and modes, `opt_share`, dirty sets, caps, logs, compact point; mode P (process-opaque sub-passes, the two skips; dropped if §24 item 2 is declined); the Phase 2 script; `bench_opt` | 2F-2…4 | Opus `high` |
| 2F-6 evidence | `opt-equiv`, `opt-diff`, `order-probe.sh`, fold table, corpus sweeps, fuzzers, the PHASE2 entry with bench output | grows from 2F-2; needs 2E for corpus runs | Opus `high`; Sonnet `high` for bulk goldens and bench runs |

Lanes: 2F-1; 2F-2 ‖ 2F-3; 2F-4; 2F-5; 2F-6 alongside. Before 2E lands, tests start from hand-built
IR, `read_blif`, or process-free Verilog. Each PR: tests first, `/lint`, a read-only critique
(PHASE0 §A.1).

## 23. Amendments to other documents (made when the PRs land)

- `docs/IR.md` §5: the FF cells of IR change D; DESIGN §5.2 word list.
- `docs/DESIGN.md` §6 step 4: `opt` is Yosys 0.55's `opt` family; arithmetic simplification is
  `opt_expr`'s, mux collapsing `opt_muxtree` + `opt_reduce` + `opt_dff`'s.
- `docs/PASSES.md`: eight rows (`opt` and the seven sub-passes; `REFUSE` or, in mode P, `OK`).
- 2D spec EL-33 (mode P only): the opt passes move from the `REFUSE` list to `OK`, with the
  process-opaque rules of OP-3; `docs/PHASE2.md` #13's "`opt` runs only after `proc`" is amended.
- 2E spec: `proc` calls `odin3_opt_expr_run` with `keepdc` (or the script does), keeps `init` on
  wires, emits the IR change D cells.
- `docs/PHASE2.md`: 2F's entry (the controller writes the decision log).

## 24. Decisions for Peter

1. **IR change D — eleven word FF cells** (`$adffe $sdffe $sdffce $aldff $aldffe $dffsr $dffsre
   $dlatch $adlatch $dlatchsr $sr`, Yosys simlib ports and parameters, `verify`, simulate hooks for
   the synchronous ones; §9.2). *Recommended:* add the whole family in one PR (2E-1 or 2F-1,
   whichever lands first); without them `opt_dff` cannot produce what Parmys's `opt` produces.
2. **Opt before `proc` (mode P) — amends EL-33.** Mark the opt passes `processes = OK` and run
   them as Yosys does on modules with processes (`$proc` opaque; `opt_muxtree`/`opt_clean` skip
   such modules), so Odin III runs Parmys's `opt_expr; opt_clean; opt -nodffe -nosdff; proc; …`
   verbatim (OP-3). *Recommended:* yes. Keeping `REFUSE` (mode R: the trio after `proc`) is
   identical on 318/331 micros but changes at least 13 of the 24 smallest VTR designs (8
   identical, 3 undecided), because the opt passes are not confluent; mode P costs two module skips and the opacity EL-33 already
   requires of an `OK` pass.
3. **2H splice point.** *Recommended:* **S-P** — Odin III runs Parmys's lines 1–4 (pre-`proc`
   trio + `proc`), Yosys 0.55 runs `synthesis.tcl` from `fsm` on; S-P counts toward PHASE2 #4, and
   2F's main `opt` is gated by the same-input `opt-diff` (§20.3). Splitting after the main `opt`
   (S-D) is information only: it changes netlists even inside Yosys (`stereovision3` 955 →
   1,288 `.names`) because `fsm` must see raw `proc` output. With mode R, the splice is S-R.
4. **Provenance of CSE survivors and rebuilt nodes** (OP-6, OP-15). *Recommended:* rebuild the
   node (`odin3_node_create` + `odin3_node_replace`, a DERIVED record with ordered parents) — no
   IR API change; the alternative `odin3_node_set_prov` would be an IR-15 change.
5. **Loop cap.** *Recommended:* 10,000 iterations per `opt` invocation, a warning and `ODIN3_OK`
   when reached (Yosys has none; worst seen 31).
6. **`proc`'s trailing `opt_expr -keepdc`.** *Recommended:* 2F exports `odin3_opt_expr_run`; 2E's
   `proc` calls it once 2F-2 has landed, and until then the Phase 2 script spells
   `proc; opt_expr -keepdc`.
7. **Where FF init values live.** *Recommended:* keep 2D's `init` wire attribute through `proc`
   and `opt` (Yosys's convention, read per net by `ffinit.c`); no `INIT` parameter on word FFs.
