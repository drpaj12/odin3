# 2F — `opt`: design

Status: v2.1 (2026-10-11): v2.1 aligns the hand-offs with 2E v2 (`5185ecc`: shared
`src/passes/ffinit.[ch]` and `budget.[ch]`, `odin3_opt_expr_counts`, the step-F call site). v2
applies the joint 2E/2F Fable review (`2EF-review1`: 5 important, 9 minor, all applied) and the
controller's rulings T1 (2E-1 owns IR change D), T2 (mode P; 2D EL-33 amended, v4.2), T3 (S-P is the
exit splice), T5 (counts, `--noopt`). Phase 2, sub-project 2F (PHASE2 #1; light review; Opus
`high`). Spec: `docs/DESIGN.md` §5, §6 step 4, §10. Inputs: PHASE2 #4, #5, #13; `docs/IR.md`; the op
registry (`src/ir/cells/` plus 2D's IR change A); the 2D spec (EL-14 constants, EL-33 `$proc` and
the `processes` flag, EL-40 `init`, EL-48 `write_rtlil`, §14.3 bridge, §14.6 x plan); the 1E and 1B
specs. **Oracle: Yosys 0.55 as pinned by VTR** (`$VTR_ROOT/libs/EXTERNAL/yosys`, `60f126cd`; binary
`$VTR_ROOT/build/bin/yosys`): `passes/opt/*.cc`, `kernel/ff.cc`, `passes/proc/proc.cc`, and Parmys's
`vtr_flow/misc/yosys/synthesis.tcl` at VTR `3c9a4d23`. Decisions are numbered **OP-n**. Where this
file and `docs/IR.md` disagree, IR.md wins. IR change D (the word FF family, §9.2) is specified and
landed by 2E-1; 2F-1 depends on it.

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
| The rewrite kit (connect, constant nets, rebuild with provenance) and the FF model (`FfData`); init values (`FfInitVals`) through 2E-1's shared helpers | `kit.c`, `ff.c`; `src/passes/ffinit.[ch]` (2E-1) |
| Folding through 2D's evaluator (`odin3_cval`, EL-14 = Yosys `const_*`) | `fold.c` |
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
| cell types and port directions | every registry type, every module of the design (port nodes, IR-7) and every black box or hard cell (declared ports, IR-7b) has port directions: `opt_clean` uses them all, as Yosys's `ct_all.setup(design)` does; `opt_merge` and `opt_expr` act only on registry word/bit types (Yosys's internals + stdcells); a type with no definition (none exist after `hierarchy`) would be "unknown", every pin both ways |
| `design->scratchpad "opt.did_something"` | a `changed` flag in the opt context, set by every mutating kit call |

**Constant canonicalization.** Each sub-pass call starts by leaving one `$_CONST0_`, `$_CONST1_`,
`$_CONSTX_`, `$_CONSTZ_` node per module (made on demand; lowest ID kept, duplicates merged in), so
keys and rules compare constants by value. Constant `$sop`s are left alone (BLIF round trip, IR-4)
but read as constants through `odin3_net_const_value`.

**Atomicity.** Each rewrite builds everything it needs (new nodes on fresh nets, the provenance
record) before its first destructive step. `NO_MEMORY` during that phase leaves the rewrite
undone; the pass returns `NO_MEMORY` with a valid IR in which every completed rewrite preserved
the module's function (refinement, §13). OOM tests inject a failure at every allocation (§20.1).

## 3. Which opt passes Parmys runs, and where Odin III runs them (OP-3)

**Parmys's invocations** (VTR `synthesis.tcl`, `PARSER=default`, after `plugin -i parmys`,
`parmys_arch -a <arch>`, `read_verilog -nomem2reg +/parmys/vtr_primitives.v`, `setattr -mod -set
keep_hierarchy 1 single_port_ram` / `dual_port_ram`, `read_verilog -sv -nolatches <files>` and
`hierarchy -check -auto-top -purge_lib`; `parmys -nopass` skips the plugin's own pass list):

| # | Parmys line | Notes |
|---|---|---|
| 1 | `opt_expr` | before `proc`: works on cells outside processes |
| 2 | `opt_clean` | before `proc`: a no-op on every module that has processes (`opt_clean` skips them) |
| 2a | `scc -select; select -assert-none %; select -clear` | a combinational loop is a Parmys rejection |
| 2b | `check` | design check (warnings) |
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

**Odin III's Phase 2 opt script (mode P).** Odin III runs Parmys's lines in Parmys's order:

```
opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt
```

(`proc` ends with `opt_expr -keepdc`, §4.8.) The opt passes therefore accept modules with
processes as Yosys's do (2D EL-33 as amended in v4.2, ruling T2): `opt_expr`, `opt_merge`,
`opt_reduce`, `opt_share` and `opt_dff` treat every `$proc` node as **opaque** (never evaluated,
merged or deleted; `O` pins drive unknown values, `I` pins read), and `opt_muxtree` and
`opt_clean` **skip** a module with live `$proc` nodes with §16's info line (Yosys's
`has_processes_warn`). Body positions are pin indices, so merges never touch a body (EL-33).
Every opt pass registers `processes = OK`. `fsm` is omitted (Phase 4); the later `opt
-full`/`-fast` runs belong to Phase 3/4 pipelines (`wreduce`, `share`, `memory`, `flatten`,
`techmap`), and §20.3 tests those modes already. Running the trio after `proc` instead (v1's
"mode R") is dropped: the evidence below shows it loses identity on the VTR set.

*Evidence* (probes, Yosys 0.55, 2026-10-11; 2F-6 commits them as `tools/opt-check/order-probe.sh`;
each variant is the rest of `synthesis.tcl`, without the `parmys` plugin pass, compared by
`netlist-compare` with Parmys's order):

| Variant (inside Yosys) | Micros (331 that compile both ways) | VTR, 24 smallest |
|---|---|---|
| `proc` first, then the trio (v1's mode R) | 318 identical; 13 others hold `$pow`/`$mem_v2` (unmapped without the plugin) or multi-driver `inout`s | 8 identical (the smallest); 13 differ — 7 by `netlist-compare` (`sha` 6,785 vs 6,786 `.names`, `dsec`-equivalent; `stereovision3` 955 vs 942), 6 by `.names` count where unmapped `$mem_v2` stops `netlist-compare` (`raygentop` 22,949 vs 19,321: without hard-block mapping arithmetic becomes gates, magnifying word-level differences); 3 undecided (`$mem_v2`, equal counts) |
| `proc`, then nothing or only `opt_expr; opt_clean`, then the rest | — | differs on all 4 designs tried |
| Parmys's order with the main `opt` moved **before** `fsm` | 316 of 318 identical (2 `dsec`-equivalent) | `diffeq1/2` identical; `stereovision3` 955 vs **1,288**, `sha`, `blob_merge` differ |

Two conclusions: the opt passes are not confluent (`opt -nodffe -nosdff` run to its fixed point
first changes which enables and resets `opt_dff` absorbs later), so only Parmys's own order
reproduces Parmys on the VTR set; and `fsm` must see raw `proc` output.

**2H splice S-P (ruled, T3; §25 item 3).** Phase 2 has no `fsm`, `wreduce`, `memory`,
`flatten`, `techmap` or hard-block mapping, so the exit comparison runs the rest of Parmys's
script in Yosys 0.55 on Odin III's `write_rtlil` output (2D §13). The two halves, verbatim
from `synthesis.tcl` apart from the hand-over:

```
# Odin III (lines 1–4)
read_verilog --nomem2reg $VTR_ROOT/…/parmys/vtr_primitives.v      # 2D §17 #19
read_verilog -nolatches <files>
hierarchy --top <first .model of the golden>                       # 2D decision 6
opt_expr; opt_clean; opt -nodffe -nosdff; proc; write_rtlil a.il

# Yosys 0.55 (line 5 onwards)
plugin -i parmys; yosys -import; parmys_arch -a <arch>
read_rtlil a.il
setattr -mod -set keep_hierarchy 1 single_port_ram
setattr -mod -set keep_hierarchy 1 dual_port_ram
hierarchy -check -auto-top -purge_lib
fsm; opt; wreduce; peepopt; opt_clean; share; opt -full; memory -nomap; flatten; opt -full
techmap -map +/parmys/adff2dff.v; techmap -map +/parmys/adffe2dff.v
techmap -map +/parmys/aldff2dff.v; techmap -map +/parmys/aldffe2dff.v; opt -full
parmys -a <arch> -nopass -c <config> <args>; opt -full; techmap; opt -fast
dffunmap; opt -fast -noff; stat; hierarchy -check -auto-top -purge_lib
write_blif -true + vcc -false + gnd -undef + unconn -blackbox out.blif
```

**Why `--top` on one side and `-auto-top` on the other.** 2D elaborates only the modules
reachable from the top (2D decision 12), and its own automatic top rule differs from Yosys's
(2D §17 #9), so Odin III must be told the golden's top to elaborate the same hierarchy. Its
`write_rtlil` output then holds just that tree (plus the `vtr_primitives.v` modules it
instantiates), whose single root is that top, so Yosys's `hierarchy -check -auto-top -purge_lib`
picks the same module and purges nothing more; keeping Parmys's spelling there keeps the
remainder verbatim.

Lines 2a/2b run in Parmys on the *pre-`proc`* cell graph, where a loop through an `always @*` block
is not yet a cell loop, so they cannot move after `read_rtlil`. 2H runs them as a precondition on
the source (`read_verilog -nomem2reg vtr_primitives.v; read_verilog -sv -nolatches F; scc -select;
select -assert-none %`, Parmys's order, Yosys 0.55): a design that fails is a Parmys rejection
(PHASE2 #4 exception list); line 2b only warns. `out.blif` is compared with the Parmys golden by
`netlist-compare`, then `equiv-check` (with `--x-report`, 2D §14.6, where needed). S-P makes
identity possible wherever 2C–2E match, but it exercises only 2F's pre-`proc` work, so 2F's main
`opt` is gated by the same-input differential `opt-diff` (§20.3). 2H also reports **S-D** (Odin III
through the main `opt`, Yosys from `fsm`) for information: `fsm_detect` takes only `$dff`/`$adff`,
so it misses the `$dffe`/`$sdff` registers `opt` made. S-P is the only splice 2H uses for the exit
table (2E's `elab-equiv --pass proc` stays an equivalence oracle).

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
| `identwrt.a/b` (only `consume_x`) | `$add $or $xor` (A or B zero), `$sub` (B zero); shifts (B zero); `$mul` by 1 or −1; `$div` by 1 (by the 1-bit signed `1'1`, which is −1: `$neg`, only when A_SIGNED && B_SIGNED, §17 #30) | the identity holds | Y = the other operand extended to Y (wiring; Yosys makes `$pos`) or `$neg` of it for −1 |
| `mux_bool` / `mux_invert` (`-mux_bool`) | 1-bit `$mux`, `$_MUX_` | A = 0, B = 1 / A = 1, B = 0 | Y = S / `$not` of S |
| `mux_and` / `mux_or` (`-mux_bool`, `consume_x`) | 1-bit `$mux`, `$_MUX_` | A = 0 / B = 1 | `$and(S, B)` / `$or(A, S)` |
| `mux_undef`, `mux_empty`, `mux_sel01`, `mux_reduce` (`-mux_undef`) | `$mux $pmux` | (A and B all undef) or S all undef → Y = A; B slices that are all undef or whose S bit is undef are dropped; an all-undef A takes the last remaining B slice; none left → Y = A; A = 0, B = 1 → Y = S; else rebuilt as `$pmux` (≥ 2 selects) or `$mux` | |
| `const.$<type>` | every evaluable registry cell: `$not $and $or $xor $xnor $reduce_* $logic_* $shl $shr $sshr $shiftx $lt $le $eq $ne $gt $ge $add $sub $mul $div $mod $pow $neg $mux $pmux` | every input bit constant | Y = the 4-state fold (OP-5) |
| (`$mux` conservative) | `$mux` | S constant (`-keepdc`: and defined) → Y = B if S is 1, else A (so **S = x picks A**); A = B → Y = A | |
| `$pow` strength reduction | `$pow`, A constant one-hot (not the sign bit of a signed A), B unsigned | A = 2 → `$shl(1, B)` with `A_SIGNED` 0; A = 2^k → `$mul(k, B)` feeding `$shl(1, ·)` | Yosys 0.55 keeps `A_SIGNED` 1 on the 1-bit `1'1` (`-1 << B`) and also rewrites a signed A whose one-hot bit is the sign: §17 #21 |
| `mul_shift.zero` / `.swapped` / `.unswapped`, `mul_low_zeros` (`!keepdc`) | `$mul` | a fully defined constant side: 0 → Y = 0; one-hot (not the signed MSB) → `$shl` by its index; low zero bits on either side → a narrower `$mul` and Y's low bits 0 | |
| `divmod_zero` | `$div $mod` | B fully defined and zero | Y = all x |
| `div_shift` / `mod_mask` (`!keepdc`) | `$div` / `$mod` | B one-hot (not the signed MSB) | `$sshr` by the index (signed truncating: plus `$reduce_or`/`$and`/`$add` correction) / `$and` mask (signed truncating: the remainder bits plus a sign fix) |
| `fine` `$lt $le $gt $ge` (with `-fine`) | | one side fully defined constant | `X<0` unsigned → 0, `X>=0` → 1, `X>~0` → 0, `X<=~0` → 1; one-hot constant 2^k → `$logic_not`/`$reduce_or` of `X[w−1:k]`; a constant above X's range → 0/1; signed `X<0` → MSB, `X>=0` → `$logic_not(MSB)` | |

### 4.4 FF polarity folding

Unless `-noclkinv`: a `CLK`, `EN`, `ARST`, `SRST`, `ALOAD`, `SET`, `CLR` input of a word FF (`$dff
$dffe $adff $adffe $aldff $aldffe $sdff $sdffe $sdffce $dffsr $dffsre $dlatch $adlatch $dlatchsr
$sr`) driven by an invert-map output takes the inverter's input and flips its `*_POLARITY` parameter
(rebuild); likewise each bit of a `$mem`'s `RD_CLK`/`WR_CLK` (one `$mem` per array, 2D EL-27) flips
that port's bit of `RD_CLK_POLARITY`/`WR_CLK_POLARITY` (a `$memrd`/`$memwr`'s `CLK` likewise);
`$_DFF_P_`↔`$_DFF_N_` and `$_DLATCH_P_`↔`$_DLATCH_N_` swap types the same way. `SET` and `CLR` are
WIDTH-bit ports: only a 1-bit FF has an inverter's output there (the invert map is keyed by whole
signals, as Yosys's). The node is rebuilt once however many inputs change, its Q nets kept (so the
`init` on them stays); `DEBUG` line and `clkinv` count per input, as Yosys logs per input. An input
on a ring of inverters is left alone (§17 #28).

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
when a union-find over the constant-equal positions merges 0 with 1. Counter and `DEBUG` names:
`fine.group`, `fine.neutral_A`, `fine.neutral_B`, `fine.$reduce_and`, `fine.A`, `fine.B`, `fine`
(the `$add`/`$sub` strip), and, with no Yosys `cover` name, `fine.eqne` and `fine.cmp`. Every rule
is skipped without `-fine` (or `-full`). Where the rules sit in a cell type's list is Yosys's: the
group, neutral, reduce_and, A and B rules after the logic rules and before `xbit`, the strip first
for `$add`/`$sub`, the `$eq`/`$ne` and comparison rules after folding. `fine.neutral_*`, `fine.A/B`,
`fine.$reduce_and` and the strip go on with the rebuilt cell in the same visit (§4.1), the others
end it. Differences: §17 #25-27.

### 4.6 `replace_undriven` (`-undriven`)

Every undriven wire bit (for `$`-named wires only bits something reads) is connected to its
`init` value when that is 0/1, else to x; the init bits used are cleared. 2D drives unconnected
cell inputs with x already (2D §17 #16); this catches wires declared and never assigned.

### 4.7 The `keepdc` flag

`-keepdc` disables every rule marked `!keepdc` or `consume_x` above, makes `constshift` and the
`$mux` rule require *defined* constants, and keeps x bits in `bitwise_logic_one_const` as cells.
`proc`'s trailing `opt_expr -keepdc` (§4.8) is the only `-keepdc` run in Parmys's flow.

### 4.8 Interface with 2E (ruling T5; 2E v2 §2.2 step F)

Yosys's `proc` ends with `opt_expr -keepdc` unless `-noopt`. 2F exports, in
`src/passes/opt/opt.h`:

```c
odin3_status odin3_opt_expr_run(odin3_pass_ctx *ctx, odin3_module *module,
                                const odin3_opt_opts *opts, odin3_opt_expr_counts *counts);
```

`odin3_opt_opts` has one `bool` per §3 option (`keepdc` among them), so 2E's call
`odin3_opt_expr_run(ctx, module, &(odin3_opt_opts){.keepdc = true}, &counts)` is the whole
contract; `odin3_opt_expr_counts` holds the §16 counts (`rewritten`, `folded`, and the per-rule
counters of `--opt-stats`), which 2E stores in `odin3_proc_stats.opt_expr`. Module scope equals
Yosys's design-wide call (`opt_expr` is per module). In Debug it runs the same `check --fast`
2F's loop runs after a sub-pass. 2E's `proc` calls it for every module it lowered completely, not
for a module that failed (it still holds `$proc` nodes), and not under `--noopt` or
`--stop-after` (which implies `--noopt`); records and messages are then `proc`'s. Call site:
2E-3 Task 9 adds it in `proc_pass.c` if 2F-2 has merged; otherwise 2F-2 adds that one call
site. Until then `proc` behaves as with `--noopt`, and the Phase 2 script spells `proc;
opt_expr -keepdc`. `proc-diff` compares `proc --noopt` with Yosys `proc -norom -noopt`.

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

**Candidates**: live word and bit nodes, except `$mem` (Yosys skips it), `$tribuf` (`opt_merge.cc`
erases `$tribuf`/`$_TBUF_` from its cell set), `$proc`,
port and constant nodes (constants are canonical, OP-2), `$mux`/`$pmux` under `-nomux`, and
`module`/`blackbox`/`hard` nodes (Yosys merges user cells only with `-share_all`, which Parmys
never passes); with `-keepdc`, FFs whose init is not fully defined. Nodes with no output port or
with more than one are not candidates either, because `kit_connect` redirects exactly one output
port; this misses Yosys's `$memwr` merges (§17 row 13).

**Key** of a node: its cell type ID; its parameters in declaration order (values compared
exactly: INT, BITS bit by bit, STRING by strtab ID); and its input pins as a sequence of
**operands** — a net ID, or a constant value (0/1/x/z) when `odin3_net_const_value` says the net
is constant, so a constant `$sop` and a `$_CONST1_` pin agree. Normalisations, from
`hash_cell_inputs`/`compare_cell_parameters_and_connections`:

- `$and $or $xor $xnor $add $mul $logic_and $logic_or $_AND_ $_OR_ $_XOR_`: the A and B operand
  vectors ordered (lexicographic on (kind, value)) — the commutative swap — only when `A_SIGNED`
  equals `B_SIGNED` (or the type has neither; §17 row 14);
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
provenance name still leads with the survivor's source (§25 item 4).

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
§16 and changes nothing in that module (§17 row 18). Roots are evaluated from the last mux to the
first, then the re-queued ones last-in first-out: Yosys's cell dictionary and pool iterate newest
first, and the order is observable (a root promoted to pure prunes later evaluations; observed in a scratch
differential on several thousand random layered DAGs, most with re-queues, which agreed with Yosys
0.55 only in this order; the generator is not committed and lands with the corpus work of Task 15/18). The
known-bit replacements are kept aside and applied with the dead ports. Count:
`opt_muxtree: removed N multiplexer ports`.

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

### 9.2 IR change D — the FF cells (owned by 2E-1, ruling T1)

The registry has `$dff $dffe $adff $sdff`. `proc` (2E) emits `$dff`, `$adff`, `$aldff`, `$dffsr`,
`$dlatch`; `opt_dff`'s `FfData::emit` closure on that input adds `$dffe $adffe $aldffe $sdff
$sdffe $sdffce $dffsre $sr`; `$adlatch` and `$dlatchsr` are unreachable in Phase 2 but needed
for a closed `ff_emit` (later `read_blif`/tech-library inputs). **IR change D** — specified and
landed by **2E-1 Task 1**, one Peter review — adds the eleven cells `$adffe $sdffe $sdffce $aldff
$aldffe $dffsr $dffsre $dlatch $adlatch $dlatchsr $sr` in `src/ir/cells/ff.c` with Yosys simlib
ports and parameters, `verify` only, **`simulate` NULL for all** (the existing `$dff $dffe $sdff`
keep theirs NULL too; simulate hooks for word FFs are a 1E change in their own PR). The registry
count becomes 74. **2F-1 depends on 2E-1.** Within Parmys's own flow only `$dff` (as `.latch`)
and `$dffsr` reach the goldens (review evidence P4).

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
10. **SRST merging** (not `-nosdff`; clocked, D ≠ Q, no ARST/SR, no ALOAD (§17 #33)): per bit, follow D down
    single-user one-select `$mux` bits that have a constant (consistent with the reset value so far)
    on one side and not Q on the other; bits are grouped by their set of (S, polarity) controls;
    each group becomes a sliced FF with `SRST` = the controls combined (`$reduce_or`/
    `$reduce_and` with `$not`s for mixed polarity) and the collected reset value.
11. **EN merging** (not `-nodffe`): per bit, eat single-user one-select muxes with Q on one side;
    then `find_muxtree_feedback_patterns` (iterative) collects the select patterns under which a
    deeper mux tree feeds Q back, **replacing those Q feedback inputs of the muxes by x**; groups
    by (patterns, controls) become sliced FFs with `EN` = `$reduce_and` of `$ne(pattern)`s and
    controls. A mux with a constant x or z select ends the search for that bit (§17 #38).
12. a changed FF is re-emitted.

Then **constbits** (`run_constbits`, no `-sat`): per bit, `combine_const` (x yields unless
`keepdc`; 0 against 1 → not constant) folds init, the ARST and SRST values, the SR constants and a
constant D (clocked) or AD (load); a bit that stays constant, x included, gets Q tied to it and is
sliced off. Constant register pipelines thus collapse one stage per `opt` iteration (31 on
mcml.v, §19).

### 9.4 Init values

2D stores initial values as the BITS attribute `init` on wires (EL-40, Yosys's convention); 2E
keeps them there (no `INIT` parameter, 2E v2 §10). 2F uses the **shared helpers of
`src/passes/ffinit.[ch]`**, created by 2E-1 Task 3 with these rules (Yosys `FfInitVals`):

- `odin3_ffinit_set(module, net, v)` (`set_init`) writes **one** wire: the net's primary wire bit,
  else its first alias membership that is a wire bit, and clears the bit on every other
  membership;
- `odin3_ffinit_remove(module, net)` (`remove_init`) clears it on every membership; an attribute
  left all x is removed;
- a net with no wire membership has no init and nothing to write.

2F-1 adds the read side to the same file, `odin3_ffinit_get(module, net)`: the first defined bit
among the net's primary and alias memberships, x when none (a conflict is `opt_clean`'s warning,
§10). FFs read the init of their Q nets through it. There is no 2F-private `ffinit.c`.

Counts: `opt_dff: N flip-flops changed, M constant bits`.

## 10. `opt_clean` (OP-10)

Per module without live `$proc` nodes (others are skipped, §3), as `rmunused_module`:

1. `$_BUF_` nodes without `keep` become wiring (Yosys removes `$pos`/`$_BUF_` first).
2. **Roots**: `$port_out` and `$port_inout` pins; nets of wires with `keep`; nodes with `keep`,
   including every instance of a module that has `keep` or (transitively, computed iteratively)
   contains a `keep` node or wire (Yosys `keep_cache`). **Liveness** spreads backwards from root
   nets to their drivers and from a live node to the drivers of its input pins. Port directions
   come from the cell type for **every** node, as Yosys's `ct_all.setup(design)` knows every module
   of the design: registry types from their definitions, module instances from the module's port
   nodes (`$port_in` → input, `$port_out` → output, `$port_inout` → both; IR-7), black boxes and
   hard cells from their declared ports (IR-7b). So an instance or black box whose outputs feed
   nothing live is **removed** like any cell (a sub-block instantiated for an output the parent
   ignores disappears, as in Yosys). Only a type with no definition would be "unknown" (every pin
   both ways); none exists after `hierarchy`. `$mem`: a live read port keeps the memory.
   Mark-and-sweep with an explicit worklist, O(pins).
3. Dead nodes are deleted in ID order; a dead FF's Q init bits are removed.
4. Nets left with no pins, no wire membership and no aliases are deleted.
5. A wire is deleted when every one of its nets has no pins, unless it is a port wire, has `keep`
   or a defined `init` bit. **Without `-purge`** (Yosys 0.55, the `!purge_mode &&
   check_public_name && (... || s1 != s2)` branch) a public wire (name not starting with `$`) also
   stays when one of its nets has another wire membership (the net's primary is another wire bit,
   or an alias is another wire bit) or was constant-driven before step 3 (the same mark as step 6):
   `wire c; connect c 1'0` and two unused aliased wires keep their names. With `-purge` also a
   public wire that is not the name a live net depends on: for each bit, the net has no pins or
   the wire is not the net's primary. A used public wire that is its net's primary name stays
   (Yosys 0.55 probe: `w = a&b; y = ~w` keeps `w` under `-purge`; a wire that is only an alias is
   dropped). Its nets stay, only the name goes. `$`-named used wires stay (IR-2; §17 #3).
6. `init` normalisation (Yosys 0.55 `rmunused_module_signals`, which runs before the wire decision
   of step 5, so Odin III runs it between steps 3 and 5): (a) an init on a net that was
   constant-driven (decided before step 3 deletes the dead `$_CONST*_` node) is removed whatever
   its value, silently, and is no change; (b) when more than one wire membership of a net carries
   a defined bit, the net keeps exactly one (its primary first), also silently. There is no
   conflict warning: Yosys's "Initial value conflict" is unreachable after this step. Every other
   init stays (an unused wire with init, and a used non-FF net with init, keep it).

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
module twice per sub-pass call. A module with live `$proc` nodes is skipped by
`opt_muxtree` and `opt_clean` and seen by the others with every `$proc` opaque (§3). In Debug the
pass manager's `check` runs before and after `opt`; `opt` itself runs `check --fast` after every
sub-pass call (agent default; Release: off).

**Termination of each sub-pass invocation.** Every inner loop is over a finite work list that only
shrinks or is refilled by strictly simplifying rewrites: `opt_expr`'s rounds stop when a round
rewrites nothing, which the ranking below bounds; `opt_merge` sweeps strictly decrease the number of
candidates; `opt_muxtree` is bounded by its evaluation budget (the budget counts eval_mux entries, not their
cost: each entry on an S-way `$pmux` costs O(S²), O(W·S) more with `replace_known`, as in Yosys); `opt_reduce`'s worker runs strictly
decrease the number of reduction inputs driven by a same-type reduction (inlining adds inputs, so
the total count is not the measure) or the mux port count; `opt_dff`'s work list receives only
sliced FFs, whose bit count is smaller than the FF they came from, or FFs whose feature set strictly
grew (≤ 6 features); `opt_clean` is one sweep.

**Termination of the loop — ranking argument.** Measured at iteration boundaries (after
`opt_clean`), let Φ = ⟨W, F, D⟩ lexicographically, where

- W = Σ over live nodes other than ports, constants and FFs of `cost(type) × output bits`, where
  `cost` strictly decreases along every strength reduction of §4, in the order `$pow`, `$mul`,
  `$div` and `$mod`, `$add` and `$sub`, shifts, comparisons, `$pmux`, `$mux`, bitwise, `$logic_*`
  and `$reduce_*`, `$not` (every cell costs more than wiring);
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
| INFO | `opt_muxtree: module 'm' has processes; skipped` (likewise `opt_clean`) | before `proc` (Yosys's `has_processes_warn`) |
| DEBUG | `opt_expr: module 'm': <rule> on <type> '<cell>'` (rule = §4 name) | every `opt_expr` rewrite |
| DEBUG | `opt_expr: module 'm': undriven on net '<net>'` (`<net>`: the net's name, else its primary wire bit `w[k]` with `k` the declared index, else `$n<ID>`) | every net `replace_undriven` ties off |
| DEBUG | `opt_merge: module 'm': '<cell>' is identical to '<cell>'` | every merge |
| DEBUG | `opt_muxtree: module 'm': dead port K/N on <type> '<cell>'` | every dead port |
| DEBUG | `opt_dff: module 'm': <action> on <type> '<cell>'`; `<action>` ∈ `removing never-active <port>`, `handling always-active <port>`, `handling const clock`, `handling D = Q`, `adding SRST`, `adding EN`, `setting constant <v> at bit K`, and (agent default, no pattern above) `converting CLR/SET to ARST`, `changing const-value ALOAD to ARST`, `handling AD = Q` | every `opt_dff` change |
| DEBUG | `opt_clean: module 'm': removing unused <type> '<cell>'` | every removed node |
| WARN | `opt: module 'm': no fixed point after 10000 iterations; stopping` | the cap (OP-12) |
| WARN | `opt: snapshot skipped: <reason>` | a `--opt-snapshot` file that cannot be written (a cell with no RTLIL form, an I/O error); the call goes on and returns its own status |
| WARN | `opt_muxtree: module 'm': giving up after 100000 evaluations` | Yosys's "Giving up (too many iterations)" |
| ERROR | `<pass>: unknown argument '<arg>'` | an unknown option (`INVALID_ARG`, the 1D style) |
| ERROR | `opt needs more than N bytes (the design's budget is 60% of physical memory)` | the shared byte budget (§19; `ODIN3_ERR_PARSE`) |

Options are spelled as in Yosys (`-full`, `-nodffe`, …) so Parmys's lines run verbatim; `--full`
is accepted too (agent default). `--opt-stats` prints one `opt: rule <name> N` line per non-zero
rule or `opt_dff` action counter, sorted by name: the counterpart of Yosys's `YOSYS_COVER_DIR`.

## 17. Divergences (from Yosys 0.55 / Parmys)

| # | Odin III | Yosys/Parmys | Why / effect |
|---|---|---|---|
| 1 | — (v1's "trio after `proc`" is gone: Odin III runs Parmys's order, §3) | | |
| 2 | identities are wiring; no `$pos` | `$pos` until `techmap` | IR-1; same netlist after lowering |
| 3 | used `$`-named wires keep their names | dropped by `opt_clean` | IR-2; names never reach `netlist-compare` |
| 4 | ties and candidate order by node ID | by name or container order | determinism without names; may change which of two equal rewrites fires first |
| 5 | rules on cells outside the registry are not ported; fine FFs keep their type | `$alu`, `$eqx`, `$_DFFE_*`… rules | those cells do not occur in Odin III's Phase 2 IR |
| 6 | `opt_share` operand order by net ID | `std::map` by wire name | competing merges may differ; equivalent |
| 7 | CSE survivor = lowest ID (unless `keep`) | the later cell (its cell dict walks newest first) | names only |
| 8 | x-with-init substitution only in `replace_undriven` | also in `opt_clean` | rare (2D folds `initial`-only regs, EL-40); x stays x, a refinement |
| 9 | loop cap 10,000 | none | safety net |
| 10 | `compact` after `opt` | — | IR-6 |
| 11 | Odin III log text (§16) | Yosys text | — |
| 12 | no `opt_dff -sat`, `-simple-dffe`; no `-share_all` (`opt_dff`, `opt_merge`, `opt`) | available | Parmys uses none of them |
| 13 | `opt_merge` candidates have exactly one output port, so `$memwr` (no output) never merges | merges `$memwr` | `kit_connect` redirects one output port; a missed merge, never a wrong one |
| 14 | the commutative swap only when `A_SIGNED == B_SIGNED` | merges mixed-sign commutative cells with swapped operands (unsound) | OP-13: with mixed signs, sext(x)+zext(y) differs from sext(y)+zext(x) (Yosys's `calc.cc` extends each operand by its own sign; `simlib.v` reads mixed signs as unsigned) |
| 15 | `opt_reduce` sorts a reduction's inputs by key (constants by value, then net ID) and rebuilds the cell only when its input set changed (or had repeats) | sorts by wire name, and `setPort`s the sorted `A` of every reduction it visits, counted or not | same function and the same change count (observed in a scratch differential on a few thousand random designs, `-fine` or not, `$mem` write enables or not, compared with Yosys 0.55 over cell types, widths, constant inputs and change counts; the generator is not committed and lands with Task 15/18); only the order of a unique, constant-free input vector differs |
| 16 | `opt_expr`, `opt_reduce` and `opt_merge` leave a cell with an unconnected input pin alone (`opt_merge`: it is no candidate) | has no open pins (an undriven wire is a full-width signal) | a missed rewrite, never a wrong one; two cells with dangling inputs are not known to read the same value |
| 17 | `opt_muxtree`'s `replace_known` reads a data bit that is a select of the same mux as 1 in the slice of that select and 0 in the others and in A | compares an index into the distinct select nets (constants counting as one) with the slice number | the two agree unless a select repeats (or a constant repeats before it); Yosys 0.55 then writes 0 for a slice's own select (`B = { t s y }`, `S = { t s s }` gives `{ 0 0 y }`), which changes the function when only that select is set (OP-13) |
| 18 | `opt_muxtree` that gives up (100,000 evaluations in a module) changes nothing in that module | has already replaced known bits in place by then | the replacements are applied with the dead ports at the end; a run that finished is identical |
| 19 | `opt_muxtree` treats a `$mux`/`$pmux` with an unconnected pin, a second driver on a Y net or port widths that disagree with its parameters as a non-mux node | has no open pins | a missed rewrite, never a wrong one |
| 20 | `opt_muxtree` makes no known-bit replacement in a mux whose output reaches more than one (user, port) pair (nor in the non-root muxes below it); its dead-port analysis is unchanged | `replace_known` writes the first port's knowledge into the mux's data bits, the second port's opposite knowledge never wins | OP-13: Yosys 0.55 turns `C = $mux(A=s0,…)` feeding `$pmux(B={c c}, S={s1 s0})` into `A = 1'1`, and `eval` gives y = 0 before and y = 1 after (`tests/unit/data/opt/muxtree/multiport_*.yosys`); a few missed constants |
| 21 | `$pow` with a signed A whose one-hot bit is the sign bit is left alone; the `$shl` made from `2 ** B` has `A_SIGNED` 0 | Yosys 0.55 rewrites both, leaving `$shl` with `A_SIGNED` 1 on `1'1`, i.e. `-1 << B` (a miscompile) | follows later Yosys (`external/yosys`); `yosys -p "read_rtlil powsigned.il; opt_expr"` shows the 0.55 result |
| 22 | `mul_low_zeros`: an operand all of whose bits are 0 gives Y = 0 | Yosys strips its bits and leaves a 0-bit port (folded to 0 later) | same value; Odin III cells have no 0-width ports |
| 23 | `div_shift` / `mod_mask`: the low bits examined are cut at A's width | 0.55 extracts `exp` bits even when `exp` exceeds A | later Yosys does the same cut; avoids an out-of-range extract |
| 24 | one `opt_expr` call can reduce further than one Yosys `opt_expr` | Yosys rules that read `cell->getPort` see a connected constant only after the next `opt_clean` | constants are read through merged nets at once (IR-1); after Yosys's own `opt` loop the results converge (observed in an uncommitted scratch differential over some 19,000 modules, which compared the results of the full `opt` loop; undefined values may resolve differently) |
| 25 | `fine.group`: the new cells' bits are in (constant, then net ID) order of their operand pair | `std::map` by wire name | IR has no wire-name order (§17 #4); only the order of bits inside the new cells changes |
| 26 | `fine` on `$add`/`$sub`: a stripped cell with no result bit left is wiring; an operand with no bit left is a constant 0 (unsigned) or its last stripped bit (signed), not a 0-bit port; an `$eq`/`$ne` that loses every bit of a side becomes `$logic_not`/`$reduce_bool` of the other side at once | Yosys leaves 0-bit ports (the cell is rewritten in a later round, or stays under `-keepdc`) | Odin III cells have no 0-width ports (as #22); same value, and the strip goes on in the same run |
| 27 | `fine` `$add`/`$sub`, `$eq`/`$ne` and comparisons treat the operands as signed only when both `A_SIGNED` and `B_SIGNED` are set | Yosys tests `A_SIGNED` alone | Yosys 0.55 rejects mixed-sign cells (`check`: matched sign), whose constant evaluation is unsigned; the two agree on every valid cell |
| 28 | an FF input on a ring of inverters (two or more `$not`s in a cycle) is not folded into the polarity | Yosys flips it once (`handle_polarity_inv` sets no `did_something`, and `invert.double` removes the ring's inverters in the same round, so the next round finds no inverter): `yosys -q -p "read_rtlil ffring.il; opt_expr; write_rtlil -"` shows `CLK_POLARITY 0` and `CLK` on the ring's other net | a ring is a combinational loop with no defined value, so either result refines it; the guard stops the fold from depending on `invert.double` removing the ring first (without it the unit test `test_clkinv_ring` fails: the guard is reached). A missed rewrite, never a wrong one; the walk that finds the ring is capped at 1024 inverters |
| 29 | `$pow` of a one-hot `2^k` (k ≥ 2) and an unsigned B: the exponent product `k * B` is a `$mul` of `B_WIDTH + bitlen(k)` bits, and the `$shl(1, ·)` it feeds has that width for its B | the `$mul` is `Y_WIDTH` bits wide and the `$shl`'s B is too, so the product wraps (`4'd4 ** b` in 8 bits with b = 128 gives 1, not 0). Yosys 0.55 and later truncate the exponent product; Odin III widens it | OP-13: `yosys -p "read_rtlil pow.il; eval -set b 128 -show y m; opt_expr; eval -set b 128 -show y m"` (`$pow`, A = `8'00000100`, B and Y 8 bits) prints `y = 8'00000000`, then `8'00000001`; follows neither Yosys version (`test_pow_overflow`) |
| 30 | a `$div` by a constant 1-bit signed `1'1` (which is -1) is a `$neg` of A, not the identity (`identwrt`); so is a `$mul` by -1; both only when A_SIGNED && B_SIGNED. A mixed-sign cell is left unchanged: our registry evaluator (`operation_signed`) reads it unsigned (1'1 is +1) while the 2D fold reads each operand by its own sign, so no rewrite is right under both | `is_int_one` (`b.as_int() == 1`) ignores signedness, so Yosys rewrites A / -1 to `$pos` | OP-13: `yosys -p "read_rtlil div.il; eval -set a 4 -show y m; opt_expr; eval -set a 4 -show y m"` (A, B signed, B = `1'1`, A and Y 4 bits) prints `y = 4'1100`, then `4'0100`; the `$mul` by -1 case already builds `$neg` (`test_div_minus_one`) |
| 31 | `opt_dff` leaves an FF with an open pin, or two Q pins on one net, alone; rule 1 (width 0) never fires | has no open pins; a width-0 `FfData` is removed | the registry rejects `WIDTH` 0 and an FF whose nets are not known is no candidate; a missed rewrite, never a wrong one |
| 32 | `opt_dff` rules 10 and 11 do not merge a mux into a fine FF (`$_DFF_P_`, `$_DFF_N_`, `$_FF_`) | Yosys emits `$_SDFF_*` / `$_DFFE_*` | the registry has no such cell (the FF model's emit refuses a fine FF needing another fine type); a missed rewrite, never a wrong one |
| 33 | SRST merging (rule 10) skips an FF with ALOAD (`$aldff`, `$aldffe`) | merges anyway and `FfData::emit` then drops the reset: the result is an `$aldff` without the mux's reset | OP-13: `yosys -p "read_rtlil aldff_srst.il; opt_dff; write_rtlil"` on `$aldff(D = $mux(A d, B 1'0, S rst))` logs "Adding SRST signal" and writes an `$aldff` with D = d and no `SRST` (no `$aldff` + SRST cell exists) |
| 34 | the feedback-pattern search (rule 11) gives up on a bit, finding no pattern and marking no mux input x, when a path holds more than 64 selects or the search takes more than 4096 steps; a walk down muxes stops after as many steps as the module has mux bits | unbounded (it needs no cap: each step requires a single-reader mux, so it cannot cycle) | an iterative search must be bounded (OP-8 spirit); the bit keeps the enables it has: a missed rewrite, never a wrong one |
| 35 | the FFs a merge makes are appended to the work list and visited in order of creation; a merge's groups are made in the order of their first bit | Yosys pops `dff_cells` from the back (LIFO) and iterates the groups in `std::map` order of (patterns, controls) | OP-14 determinism (no hash or pointer order reaching the IR); the final cells are the same, only their IDs and the order of equal rewrites differ. The `mux` data case (all five flag variants) matches Yosys cell for cell |
| 36 | an empty feedback pattern (a bit whose D is Q after the enable muxes) makes the enable's `$ne` input a constant 0, not a zero-width `$ne` | `addNe` on empty signals (a width-0 cell) | the registry rejects width 0; the value is the same (an empty pattern never enables) |
| 37 | a merge's new FFs are built on the old FF's Q nets (a second driver) before the old node is shrunk with those Q nets handed on (`keep == drop` ties, init kept), so a NO_MEMORY before the shrink deletes them again and leaves the IR as it was | creates the cells and `module->remove(cell)` | OP-2 build before destroy; the group's gates stay as dead cells on a failure (opt_clean) |
| 38 | the feedback search gives up (the bit keeps the enables it has, as in #34) on a mux whose select is a constant x or z; 0 and 1 selects are searched as before | Yosys 0.55 searches through it, and the select is then read twice (in the `$ne` pattern and in the surviving mux) as two independent x values: `q <= b ? d : mux(S=1'x, A=1'1, B=q)` becomes a `$dffe` whose output differs from the input for x_sel=1, x_B=0, x_en=0 (q drops from 1 to 0), also under `-keepdc`; `async2sync; miter -equiv; sat -seq 2` fails on Yosys's own output (reviewer's 4 of 1,500 random cases, all with an x select; 0 failures in 12,000 runs without) | OP-13: soundness outranks parity; a missed enable is only an optimisation lost. Test `test_dff_feedback_x_select` |
| 39 | a top-level `opt` ends with `compact` (IR-6 amendment, point (h), for Peter's review): IDs of nodes, nets and wires are renumbered densely when it returns; the sub-passes called alone leave dead objects | Yosys has no IDs to renumber | §14 ("the pass manager compacts after each top-level `opt`") names the point; doing it in the `opt` driver keeps `opt` callable from scripts, `-p` and the tests alike, and holds the dead objects of one run only until its end |
| 40 | after its first call in an `opt` invocation, `opt_expr` visits only the nodes the kit marked (inputs connected, merged or re-driven since, plus the readers of their outputs), in the same topological order as a full sweep; `--opt-full-rescan` sweeps everything. Debug builds sweep in full after every incremental call and abort if that rewrites anything | a full sweep per call (Yosys rebuilds a SigMap and rescans) | §19 performance; the result is the full sweep's (the 20 files of `dirty_vs_full_rescan`, three option sets, byte-identical `write_rtlil`) |
| 41 | the iteration cap warns once per module that the last iteration changed (a module no kit rewrite touched in it is not named; when none was, the last module is) and returns `ODIN3_OK` | no cap | OP-12; `opt` runs design-wide, so the module in the §16 line is the changed ones |
| 42 | `opt -full` calls `opt_share`, which is a stub until Task 14: the run stops with `opt_share: not available yet (2F)` after the first loop `opt_merge`; the sequence tests accept it | runs `opt_share` | removed with the stub in Task 14 (this row too) |

## 18. Files, interfaces and data structures (OP-17)

| File | Content |
|---|---|
| `src/passes/opt/opt.h` | `odin3_opt_opts` (§3 flags), `odin3_opt_expr_counts`, `odin3_opt_run` (the loop), `odin3_opt_<sub>_run(ctx, design, &opts, &counts)` per sub-pass, `odin3_opt_expr_run` (module scope, for 2E, §4.8) |
| `src/passes/opt/opt_loop.c`, `dirty.c/.h` | the `opt` driver (`odin3_opt_run`: the mode tables, the cap, `compact`) and the opt session: per-module dirty marks the kit fills and `opt_expr` drains, per-iteration change flags |
| `src/passes/opt/kit.c/.h` | opt context (`changed`, counters, caps), constant nets, `kit_connect`, `kit_rebuild`, `kit_new_cell`, fanout counts, invert map, topological order (Kahn, explicit queue), dirty sets |
| `src/passes/opt/fold.c` | pins ↔ `odin3_cval`, dispatch to 2D's operators |
| `src/passes/opt/expr*.c` | §4, one function per rule, a rule table per cell type (lizard: ≤ 60 lines, CCN ≤ 15) |
| `src/passes/opt/{merge,muxtree,reduce,share,clean}.c` | §6, §7 (explicit frame stack), §8, §11, §10 |
| `src/passes/opt/{ff,dff}.c` | §9 (init through `src/passes/ffinit.[ch]`, 2E-1; 2F-1 adds `odin3_ffinit_get`) |
| `src/passes/budget.[ch]` (2E-1) | `odin3_budget_check`, used, not owned (§19) |
| `src/passes/opt/register.c` | the eight pass definitions (`processes = OK`, EL-33 v4.2) and option parsing |
| `src/ir/cells/ff.c` | IR change D |
| `tools/opt-check/` | `opt-rules`, `opt-equiv`, `opt-diff`, `gen-fold-table`, `order-probe.sh` |
| `tests/unit/test_opt_*.c`, `tests/opt/`, `tests/bench/bench_opt.c` | §20 |

One additive ABI v4 setter, `odin3_pass_set_opt_snapshot` (the passes are reached through the
pass manager); `docs/PASSES.md` gains eight rows. Side arrays indexed by net or node ID (fanout counts, liveness marks, order, mux port
tables) come from `util/vec`, sized at the store length and freed after each call; the CSE table
is a `u64map` plus a chain `vec`; `fold.c` uses a per-call arena.

## 19. Performance (OP-18)

**Yosys 0.55 measured** (i7-13700 WSL2, 2026-10-11; `synthesis.tcl` lines 1–9 with
`vtr_primitives.v` and `setattr`, plugin absent; `yosys -d` per-pass totals). Cell counts are
the top-level `stat` total over all modules after Parmys's lines 1–4 (the trio, then `proc -norom`
with its `opt_expr -keepdc`); 2E's 784,457 for lenet.v is `proc -norom -noopt` without the trio,
a different point of the same flow:

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
fanout (IR-18), ID-indexed side arrays. Only `opt_expr` is **incremental**: after the first
iteration it starts from a dirty set (nodes whose input nets were connected, merged or re-driven
since its last call, plus their readers), which is exact because every `opt_expr` rule reads only
a node's inputs, their drivers and the invert map (itself made of drivers), never fanout.
**Fanout-sensitive rules run as full sweeps** (review finding 2F-2): `opt_dff` (single-user muxes,
`bitusers`), `opt_share` (operands used only by one mux port), `opt_muxtree` (roots), `opt_reduce`
(`mem_wren_sigs`), and `opt_clean`; `opt_merge` is a full sweep too, since a clean node can newly
collide with a dirty one. All are O(pins) per call (on lenet.v Yosys spends 29 s in `opt_dff` and
`opt_share` together). The Debug option `--opt-full-rescan` runs `opt_expr` as a full sweep too
and asserts the same result.

**Memory.** Rebuilds and FF re-emission leave a dead node per rewrite until the pass manager
compacts after `opt` (IR-6); mcml.v's 31 iterations re-emit constant-register pipelines each
round. `bench_opt` reports dead objects at the peak; if the RSS target below slips, the remedy is
a compact point per `opt` iteration — an IR-6 amendment, for Peter — never a relaxed target. 2F
uses the design-wide byte budget of 2E-1's `src/passes/budget.[ch]` (2E v2 §16): it calls
`odin3_budget_check(ctx, design, extra_bytes)` at the start of each sub-pass call (`extra_bytes` =
its side arrays and CSE table) and every 2^16 created pins; the check measures what the design
holds (`odin3_design_bytes_reserved`, dead objects included), so 2D's, 2E's and 2F's checks share
one 60% limit. Exceeding it is fatal with `opt needs more than N bytes (the design's budget is
60% of physical memory)` (status `ODIN3_ERR_PARSE`, 2D's cap style), the IR left valid.

| Measure (Release, dev machine, `tests/bench/bench_opt.c`, smallest first) | Target |
|---|---|
| mcml.v, the opt passes of the Phase 2 script (the trio and the main `opt`) | ≤ 1.0 s |
| mcml.v, `opt -full` on that result | ≤ 0.3 s |
| lenet.v, the opt passes of the Phase 2 script (the trio, then the main `opt` on ≈ 661k cells) | ≤ 15 s |
| lenet.v, peak RSS growth over the post-`proc` IR (side arrays, CSE table, dead objects before `compact`) | ≤ 25% |
| the 31 VTR files, opt passes of the Phase 2 script, total | ≤ 3 s |
| per sub-pass call | O(pins) full sweep (O(dirty pins) for `opt_expr` after the first iteration); `opt_muxtree` O(budget × S²) worst case, each eval of an S-way `$pmux` costing O(S²) (O(W·S) more for `replace_known` on W-bit data), as Yosys; `opt_merge` O(candidate pins) expected |
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
  valid IR), the `opt_muxtree`/`opt_clean` skips on a module with a `$proc` (exact info line) and
  the opacity of `$proc` to the other sub-passes.
- `test_opt_kit.c` also asserts that a wire bit folded into a constant net prints, through 2D's
  `write_rtlil`, as Yosys's `connect \w [k] 1'0` (a constant net has no primary wire bit, and
  `opt_expr` makes such aliases in bulk).

### 20.2 Refinement after every sub-pass (`opt-equiv`)

`tools/opt-check/opt-equiv <files> [--top T]`: runs the Phase 2 script with `--opt-snapshot <dir>`,
a debug option that writes `write_rtlil` before and after **every sub-pass call** that changed
something; each pair goes through Yosys 0.55 `read_rtlil before.il; rename -top gold; read_rtlil
after.il; rename -top gate; equiv_make gold gate eq; hierarchy -top eq; flatten; equiv_simple -undef
-seq 4; equiv_induct -undef; equiv_status -assert` — a **refinement** check (x in the gold is a
don't-care; probe: `s ? a : 4'bx` → `a` passes, the reverse and a wrong enable fail). Inputs holding
processes (the pre-`proc` trio) get Yosys `proc` on both sides first. A failing pair names the
sub-pass call and rule counters. A cheaper in-process check runs alongside when the module has no x
constant, no FF and no cell without a `simulate` hook (`$shiftx`, `$pow`; word FFs have none, IR
change D): 1E's simulator, 64 cycles × 3 seeds, before/after copies. Run on: every per-rule golden
(CTest), the micros (CTest, label `opt_equiv`, smallest first), the VTR set (nightly).

### 20.3 Differential against Yosys on the same input (`opt-diff`)

`tools/opt-check/opt-diff <files> [--top T] [--script S]`, per design:

1. Odin III: `read_verilog -nolatches; hierarchy; proc; opt_expr -keepdc; write_rtlil in.il; S;
   write_rtlil odin.il` with `--opt-stats` (for the trio, `in.il` is written before `proc`);
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

2F hands 2H the S-P splice of §3 (the complete script there); S-D is reported as information.
2F's own gate is §20.3; the exit-bar table is 2H's. `opt-equiv` and `opt-diff` also run on the
pre-`proc` calls (inputs with `$proc` nodes go to Yosys as RTLIL processes, EL-48).

## 21. Review focus and risks

1. **Pass order** (§3): `opt` is not confluent, so identity with Parmys on the VTR set needs
   Parmys's own order (EL-33 v4.2) and a splice before `fsm`; 2F's main `opt` is then proven by
   `opt-diff`, not by the exit test.
2. **`opt_dff`'s feedback patterns** write x into mux inputs (§9.3 item 11); the refinement check
   must see Q's init.
3. **`opt_muxtree`'s re-entrant evaluation** (pure/non-pure roots, depth 3) is the hardest code to
   make iterative faithfully; its tests replay Yosys's cases.
4. **`opt_expr`'s dirty set** (§19) could miss a rewrite; `--opt-full-rescan` guards it.
5. **2E coupling**: IR change D (2E-1), `init` on wires, `odin3_opt_expr_run` and `--noopt`
   (§25 items 1, 6, 7) must match the 2E spec.

## 22. Implementation split (OP-20)

Six PRs; the plan fixes tasks and interfaces. PHASE2's 2F row: Opus `high`.

| PR | Content | Depends on | Model/effort |
|---|---|---|---|
| 2F-1 kit | opt context and kit, constant canonicalization, FF model, `odin3_ffinit_get` in the shared `ffinit.[ch]`, budget calls, `opt_clean` (design-wide port directions, instance removal, the process skip), the eight pass registrations (`OK`) with options, `--opt-stats`, `--opt-snapshot` | 2D-1 (cells, `$proc` flag, `write_rtlil`); **2E-1** (IR change D, `ffinit.[ch]`, `budget.[ch]`) | Opus `high` |
| 2F-2 expr | `fold.c` on 2D's evaluator, `opt_expr` §4 all rules, `replace_undriven`, polarity folding; `odin3_opt_expr_run` for 2E; per-rule goldens for §4 | 2F-1, 2D-2 | Opus `high` |
| 2F-3 merge/reduce/muxtree | §6, §7, §8; goldens | 2F-1 | Opus `high` |
| 2F-4 dff | §9: all actions, SRST/EN merging, feedback patterns, constbits; goldens | 2F-1, 2F-2 | Opus `high` |
| 2F-5 loop | `opt` driver and modes, `opt_share`, `opt_expr`'s dirty set, caps, logs, compact point, the run-level byte budget; `$proc` opacity across sub-passes; the Phase 2 script and the S-P splice script; `bench_opt` | 2F-2…4 | Opus `high` |
| 2F-6 evidence | `opt-equiv`, `opt-diff`, `order-probe.sh`, fold table, corpus sweeps, fuzzers, the PHASE2 entry with bench output | grows from 2F-2; needs 2E for corpus runs | Opus `high`; Sonnet `high` for bulk goldens and bench runs |

Lanes: 2F-1; 2F-2 ‖ 2F-3; 2F-4; 2F-5; 2F-6 alongside. Before 2E lands, tests start from hand-built
IR, `read_blif`, or process-free Verilog. Each PR: tests first, `/lint`, a read-only critique
(PHASE0 §A.1).

## 23. Amendments to other documents

- Made with v2: 2D spec EL-33 (v4.2 status note: the opt passes are `OK`, `$proc` opaque,
  `opt_muxtree`/`opt_clean` skip modules with processes) and `docs/PHASE2.md` #13 (one-line
  amendment).
- When the PRs land: DESIGN §6 step 4 (`opt` is Yosys 0.55's `opt` family; arithmetic
  simplification is `opt_expr`'s, mux collapsing `opt_muxtree` + `opt_reduce` + `opt_dff`'s);
  `docs/PASSES.md` (eight rows, `OK`); `docs/PHASE2.md` (2F's entry). IR change D's IR.md/DESIGN
  §5.2 amendments travel with 2E-1.
- 2E spec (its author; matched by 2E v2 `5185ecc`): `proc` calls `odin3_opt_expr_run` unless
  `--noopt`/`--stop-after`, owns `src/passes/ffinit.[ch]` and `budget.[ch]`, keeps `init` on wires,
  owns IR change D, feeds its oracles the trio's output.

## 24. Review dispositions (v2, `2EF-review1` §C)

| # | Finding | Disposition |
|---|---|---|
| I1 | `opt_clean` treats instances as unknown | applied: design-wide port directions, unused instances removed (§2, §10) |
| I2 | dirty sets miss fanout-sensitive rules | applied: only `opt_expr` is incremental; `opt_dff`, `opt_share`, `opt_merge` full sweeps (§19) |
| I3 | S-P under-specified | applied: complete two-sided script, `scc` precondition (§3) |
| I4 | two modes | applied: mode R dropped; EL-33 and PHASE2 #13 amended (§3, §23) |
| I5 | IR change D ownership and hooks | applied: 2E-1 owns it, `simulate` NULL, 2F-1 depends (§9.2, §22 table) |
| M6 | `$memrd`/`$memwr` polarity | applied: `$mem` `RD_CLK`/`WR_CLK` bits (§4.4) |
| M7 | include `$tribuf` in CSE | **rejected**: 0.55 `opt_merge.cc` erases `$tribuf` and `$_TBUF_` from its cell set, so Yosys never merges them |
| M8 | `opt_reduce` measure | applied (§12) |
| M9 | memory budget, dead objects | applied: shared run-level budget, compact-per-iteration as the escalation (§19) |
| M10 | `scc`/`check` rows | applied (§3 rows 2a/2b) |
| M11 | folded aliases in `write_rtlil` | applied: test asserts `connect \w [k] 1'0` (§20.1) |
| M12 | `odin3_opt_expr_run` counts, Debug check | applied (§4.8) |
| M13 | lenet count basis | applied: script stated (§19) |
| M14 | `set_init` target wire | applied (§9.4) |

## 25. Decisions for Peter

1. **IR change D** (ruled T1): eleven word FF cells, `simulate` NULL, specified and landed by
   2E-1 Task 1; 2F-1 depends on 2E-1. Peter reviews it once, in 2E's PR.
2. **Opt before `proc`** (ruled T2; agent default for Peter's review): the opt passes are
   `processes = OK` with `$proc` opaque and `opt_muxtree`/`opt_clean` skipping modules with
   processes, so Odin III runs Parmys's `opt_expr; opt_clean; opt -nodffe -nosdff; proc; opt`
   verbatim (2D EL-33 v4.2, PHASE2 #13 amended). Evidence: running the trio after `proc` changes at
   least 13 of the 24 smallest VTR designs (Fable: `stereovision3` 812 vs 792 cells).
3. **2H splice** (ruled T3): S-P, with §3's complete script; S-D is information only (it changes
   netlists even inside Yosys, `stereovision3` 955 → 1,288 `.names`, because `fsm` must see raw
   `proc` output).
4. **Provenance of CSE survivors and rebuilt nodes** (OP-6, OP-15). *Recommended:* rebuild the
   node (`odin3_node_create` + `odin3_node_replace`, a DERIVED record with ordered parents) — no
   IR API change; the alternative `odin3_node_set_prov` would be an IR-15 change.
5. **Loop cap.** *Recommended:* 10,000 iterations per `opt` invocation, a warning and `ODIN3_OK`
   when reached (Yosys has none; worst seen 31).
6. **`proc`'s trailing `opt_expr -keepdc`** (ruled T5): 2F exports `odin3_opt_expr_run`, which
   returns its counts; 2E's `proc` calls it for every completely lowered module unless `--noopt`;
   until 2F-2 lands the script spells `proc; opt_expr -keepdc`.
7. **Where FF init values live.** *Recommended:* keep 2D's `init` wire attribute through `proc`
   and `opt` (read per net, written to one wire, §9.4); no `INIT` parameter on word FFs.
