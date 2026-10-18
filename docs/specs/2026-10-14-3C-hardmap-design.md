# 3C — `hardmap`: design (spec only, for Peter's review)

Status: v1 (2026-10-14), **approved by Peter 2026-10-14** ("All 10 yes"; PHASE3 #31). **Spec only.** 3C is a mapping algorithm, which Peter reviews (DESIGN §12,
CLAUDE.md rule 10). The plan follows his approval. Each rule has an ID (**M-n** multipliers, **A-n**
adders, **R-n** memories, **S-n** soft, **H-n** general) so that it can be approved or changed line by
line. The questions are in §12.

Phase 3, sub-project 3C (Opus `xhigh` → `high`, advisor on). Spec: `docs/DESIGN.md` §5, §6 steps
7–9, §7 (only the fixed subset of PHASE3 decision 4). Inputs:
- `docs/IR.md`: IR-7b black boxes and hard cells, IR-8 and the mixed-sign rule, IR-9 views, IR-12/13;
- PHASE2 #25;
- PHASE3:
  - decisions 3 (bands), 4 (the fixed Parmys-default binding), 5 (arch model names as `.o3lib`
    cells), 6 (memories), 16 (functional check), 19 (FF normalisation in `lower`);
  - the 2026-10-14 rulings: sign-extend signed adders rather than copy Parmys; 3G is a faithful
    `wreduce`; 3C spec first;
- `lib/vtr.o3lib`: `multiply` is VTR's square `WIDTH` cell;
- the 2D spec: §3.1a library reads, EL-27/EL-39 (the `$mem` form), §17 #66 (two-write-port race);
- the 3G spec (`docs/specs/2026-10-14-3G-wreduce-design.md`);
- the 2H splice report (stereovision0 eq-x review, `exit-repros/sx_add`).

**Oracle:** Parmys at VTR `3c9a4d23`, read only:
- `parmys-plugin/parmys.cc`: `elaborate`, `optimization`, `from_yosys_type`;
- `parmys_resolve.cc`;
- `core/adder.cc`, `core/subtractor.cc`, `core/multiplier.cc`, `core/block_memory.cc`, `core/memory.cc`;
- `netlist/netlist_utils.cc` (`reduce_input_ports`, `equalize_ports_size`);
- `utils/read_xml_config_file.cc`;
- `vtr_flow/misc/basic_odin_config_split.xml` (with `vtr/parmys/parmys.py` filling `MMM=3`,
  `AAA=1`, `PPP=15`);
- `vtr_flow/misc/yosys/synthesis.tcl`;
- the arch `k6_frac_N10_frac_chain_mem32K_40nm.xml`.

The arch's shapes:
- `mult_36` has modes 36×36, 2× 18×18 and 4× 9×9, all `.subckt multiply`.
- `memory` is 32 Kbit: single-port modes from 512×64 to 32768×1, dual-port modes from 1024×32 to
  32768×1; address width 15.
- `adder` is 1-bit, with `a b cin → cout sumout`.

## 1. Purpose and scope (H-1)

`hardmap` binds word cells to the arch's hard blocks exactly where Parmys's defaults do, so that the
O′/O legs differ from P only by the declared divergences (§10). It is the fixed Phase 3 subset of
DESIGN §7; Phase 4's `partial_map` replaces it.

**Parmys's effective configuration**, as `run_vtr_flow.py` sets it:

| Setting | Value |
|---|---|
| `min_hard_multiplier` | 3 |
| `fixed_hard_multiplier` | 1: pad to the model's full size |
| `split_hard_multiplier` (`fracture`) | 0: never use the 18×18 or 9×9 modes |
| `mult_padding` | −1: pad with `unconn` |
| `min_threshold_adder` | 1 |
| `min_add` | 0 |
| `fixed_hard_adder` | 1 |
| `adder_cin_global` | false: a dummy head feeds the first `cin` |
| `split_memory_width` | 1: 1-bit data slices |
| `split_memory_depth` | 15 |
| soft-memory thresholds | 0: every memory is hard |

| In 3C | Not in 3C (Phase 4 or later) |
|---|---|
| `$mul` → `multiply` (§4) | multiplier splitting above 36, fracturing (18×18, 9×9), soft-multiplier construction |
| `$add`/`$sub` → `adder` chains (§5) | comparator chains, `$neg` and `$alu`-style chains, mixed soft/hard adders |
| RAM instances → 1-bit slices with depth split (§6.1) | width/depth trading, LUTRAM |
| inferred `$mem` → FF merge + slices, Parmys's port patterns (§6.2) | `memory_share`, multiplexed port modes |

## 2. Survey of the 19 (evidence for every rule below)

**Method** (2026-10-14; light, one Yosys at a time beside a heavy sweep, each under `prlimit
--as=4 GiB` in `mktemp -d /tmp/claude-1000/3C/run.XXXX`):
- Yosys 0.55 ran Parmys's `synthesis.tcl` up to, but not including, the `parmys` pass. That is
  `vtr_primitives.v`, `read_verilog -sv -nolatches`, the opt trio, `proc -norom; fsm; opt;
  wreduce; peepopt; opt_clean; share; opt -full; memory -nomap; flatten; opt -full`, the four
  `*2dff` techmaps and `opt -full`. The `$mul`/`$add`/`$sub`/`$neg`/`$mem_v2` cells were dumped
  with their parameters, and `stat` was taken per module.
- Separately, each k6 Parmys golden (`golden/k6_frac_N10_frac_chain_mem32K_40nm/vtr/<d>/<d>.parmys.blif`)
  was parsed for its hard-block instances and the pins it ties to `unconn` or constants.
- The whole probe took under 80 s. The largest run was `mcml` (40 s, 249 MiB).

| design | `$mul` (A×B: count) | signed `$add`/`$sub` (of all) | RAM instances (we pattern) | golden `adder` / `multiply` / RAM slices |
|---|---|---|---|---|
| `ch_intrinsics` | — | 0/0 (0, 0) | 1 SP (we constant) | 0 / 0 / 8 SP |
| `stereovision3` | — | 0/0 (3, 0) | — | 28 / 0 / 0 |
| `diffeq2` | 32×32: 3, 3×32: 1, 2×32: 1 | 0/0 (2, 2) | — | 132 / 5 / 0 |
| `diffeq1` | 32×32: 3, 32×2: 2 | 0/0 (2, 2) | — | 132 / 5 / 0 |
| `mkPktMerge` | — | 0/0 (9, 0) | 3 DP (we2 constant: 1 write + 1 read) | 42 / 0 / 459 DP |
| `raygentop` | 8×7: 9, 16×16: 6 | 0/0 (25, 7) | 1 SP | 413 / 15 / 21 SP |
| `sha` | — | 0/0 (10, 1) | — | 309 / 0 / 0 |
| `boundtop` | — | 0/0 (12, 3) | 1 SP | 151 / 0 / 32 SP |
| `mkSMAdapter4B` | — | 0/0 (16, 8) | 3 DP (2 with we2 constant, 1 with both constant) | 344 / 0 / 153 DP |
| `or1200` | 32×32: 1 | 0/0 (10, 5) | 2 DP (we1 constant) | 496 / 1 / 64 DP |
| `stereovision0` | — | **170**/0 (244, 11) | — | 2,659 / 0 / 0 |
| `stereovision1` | 8×8: 152 | **50**/0 (202, 24) | — | 2,294 / 152 / 0 |
| `mkDelayWorker32B` | — | 0/0 (52, 8) | 9 DP (we2 constant) | 768 / 0 / 1,336 DP |
| `stereovision2` | 8×5: 72, 16×8: 54, 8×8: 54, 16×7: 48, 16×5: 48, 16×4: 48, … (468) | **102/12** (496, 18) | — | 12,081 / 468 / 0 |
| `blob_merge` | — | 0/0 (31, 268) | — | 3,516 / 0 / 0 |
| `LU8PEEng` | 24×24: 8 | 1/1 (104, 300) | 9 DP (5 with we2 constant, **4 with two live write ports**) | 4,560 / 8 / 1,409 DP (353 slices with two live `we`) |
| `bgm` | 24×24: 11 | 0/0 (102, 91) | — | 2,270 / 11 / 0 |
| `mcml` | 32×32: 26, 5×27: 1 | 1/0 (146, 261) | 7 DP (we2 constant; 3 of them `ADDR_WIDTH` 16) + 3 SP (we constant) | 25,137 / 27 / 252 DP + 96 SP |
| `LU32PEEng` | 24×24: 32 | 1/1 (248, 996) | DP (two live write ports on 1,123 slices) | 13,470 / 32 / 5,251 DP |

**Findings:**
1. **No `$mul` is wider than 36.** The widest operand is 32 (`diffeq1/2`, `or1200`, `mcml`).
   No `$mul` is signed, and none has a constant operand. Every `$mul` became exactly one
   `multiply` in the golden: Yosys's `$mul` count equals the golden's `multiply` count on all 10
   designs that have one.
2. **Narrow multiplies go hard.** The 32×2 and 2×32 `$mul` of `diffeq1/2` are hard `multiply`
   blocks in the golden. That contradicts decision 4's "both widths ≥ 3" (M-2, question 1).
3. **Every golden `multiply` has 36 `a` and 36 `b` pins**, the unused ones on `unconn`. Its `a`
   carries `$mul`'s A and its `b` carries B (`diffeq2`'s 3×32 has 3 used `a` pins; `mcml`'s 5×27 has
   5).
4. **No design has an inferred memory.** `stat` finds `$mem_v2` only inside the
   `$paramod…single_port_ram`/`dual_port_ram` primitive modules. Every RAM in the 19 is an
   explicit instance of a VTR primitive. The FF-merge path (§6.2) is exercised only by micros.
5. **RAM port patterns**, from the instances' `we` pins:
   - SP with `we` live: `raygentop`, `boundtop`.
   - SP with `we` constant (read-only): `ch_intrinsics`, `mcml`.
   - DP with one write and one read port: `mkPktMerge`, `mkSMAdapter4B`, `or1200`,
     `mkDelayWorker32B`, `LU*`, `mcml`.
   - DP with both `we` constant: one instance in `mkSMAdapter4B`.
   - DP with **two live write ports**: `LU8PEEng` (4 instances, 353 slices) and `LU32PEEng`
     (1,123 slices). These are §17 #66's race case.
6. **Only `mcml` needs a depth split.** It has three DP instances with `ADDR_WIDTH` 16: 36-, 18- and
   8-bit, i.e. 124 slices of 15 address bits each.
7. **Signed adders** after `wreduce` occur in 6 designs (bold above; 3G §5). All of them have an
   operand narrower than Y, and none is mixed-sign.
8. **`$neg` occurs** in `stereovision1` (12), `LU8PEEng` (9), `bgm` (17), `mcml` (50) and
   `LU32PEEng` (33). Parmys leaves it soft (`from_yosys_type` returns `SKIP` for a known Yosys
   cell), so it is S-1's.

## 3. Position, inputs and IR interactions (H-2 … H-7)

- **H-2 Script position** (3G WR-9): `…; wreduce; opt_clean; opt -full; flatten; opt -full;
  hardmap; opt -full; lower; abc; write_blif --vtr`. `hardmap` needs a flat design (3A), as Parmys
  needs `flatten`. A module other than the top that still holds word cells is mapped the same way,
  but the VTR flow has only the top.
- **H-3 Library cells come from the techlib** (decision 5). `read_techlib lib/vtr.o3lib` registers:
  - `adder`: hard, 1-bit `a b cin → cout sumout`;
  - `multiply`: hard, parameter `WIDTH`, `a`/`b` of `WIDTH` bits, `out` of `2*WIDTH`;
  - `single_port_ram` / `dual_port_ram`: hard, parameters `ADDR_WIDTH` (default 15) and
    `DATA_WIDTH` (default 1).

  `hardmap` finds them by name. If a needed type is missing or not `hard`, it refuses with a
  located error (`hardmap: cell type 'multiply' is not registered; read_techlib lib/vtr.o3lib
  first`). It never creates a type.
- **H-4 Parameters.** Each new hard node carries its parameters as node parameters: `multiply
  WIDTH=36`; RAM slices `ADDR_WIDTH=15 DATA_WIDTH=1`. The plain BLIF writer prints them as `.param`
  lines. `write_blif --vtr` drops `.param` (PHASE3 #21) and writes every formal bit, so VPR sees
  the widths as pin counts, exactly as in Parmys's BLIF. The IR-7b table rule is satisfied, because
  these values are inside the library's width expressions.
- **H-5 Constants.**
  - gnd and vcc are the canonical `$_CONST0_`/`$_CONST1_` nets.
  - Parmys's pad pin is the `$_CONSTX_` net, which both writers emit as `unconn` (3E-3).
  - A pad on an operand bit is x, never a value. Where correctness needs a value, the rule says
    gnd, vcc or a sign bit.
- **H-6 Views and glue.**
  - `hardmap` runs in the RTLIL view: word cells plus `hard` cells (IR-9).
  - Any soft glue it adds is made of word cells of width 1: `$not` for a subtrahend bit, `$and`
    for a write-enable decode, `$mux` for a depth-split output, `$dff` for a registered select
    (R-7).
  - `lower` (3B) lowers that glue.
- **H-7 Provenance** (IR-12/13). Each source cell is mapped as one operation. Every node it creates
  (hard cells and glue) shares one DERIVED record whose parent is the source cell. A RAM instance's
  slices have the instance as their parent. The source cell is killed, and its output nets survive
  as aliases where they are reused (IR-2).

## 4. `$mul` → `multiply` (M-1 … M-7)

- **M-1 Candidates.** Every live `$mul`. `$mul` with a constant operand is not special-cased:
  Parmys's constant-multiply path runs only when the arch has no hard multiplier.
- **M-2 Hard or soft.** Let `a = A_WIDTH`, `b = B_WIDTH`. The cell goes hard unless one of these
  holds:
  - `a = b = 2`: Parmys builds a soft multiplier there, from AND partial products and hard adders
    (`split_soft_multiplier`). 3C leaves the cell soft for `lower` instead (S-2; question 3).
  - `max(a, b) > 36`: Parmys splits; 3C leaves it soft and warns (decision 4).
  - The cell is signed with `Y_WIDTH > 36` (M-5).

  Parmys's test is `max(a,b) >= 3 || a == 1 || b == 1`, so 1×N goes hard too. It is **not**
  decision 4's "both widths ≥ 3" (survey finding 2; question 1).
- **M-3 Shape** (Parmys `pad_multiplier`, fixed size, no fracturing). One `multiply` with `WIDTH =
  36`, whatever the operand widths (question 2):
  - `a[i] = A[i]` for `i < a`, `b[i] = B[i]` for `i < b` (survey finding 3);
  - every other `a`/`b` bit is the pad (M-4);
  - `out[i]` drives `Y[i]` for `i < Y_WIDTH`;
  - `out[Y_WIDTH..71]` drive fresh unread nets (Parmys's `unconnected_multiplier_output~N`).

  After 3G, `Y_WIDTH <= a + b`. An unsigned `$mul` with a `Y_WIDTH` above `a + b`, possible without
  3G, stays soft with a warning ("run wreduce first"): the gnd merges Parmys's
  `check_multiplier_port_size` gives would allocate after the source is deleted, which H-9 forbids.
  (Amended 2026-10-17, review finding M-2; agent default.)
- **M-4 Padding.** For an unsigned cell, the pad is x (`unconn`), as Parmys's `mult_padding = -1`.
  `multiply` is an unsigned primitive (`fn out = a * b`), and VPR leaves an `unconn` pin open. ABC
  sees `.names unconn` with no rows as 0, and the functional check's hard-block model reads an open
  pin as 0. A pad of x is therefore the value Parmys relies on. Question 2a: should 3C use gnd so
  that the IR's own semantics is exact?
- **M-5 Signed `$mul`** (both flags set; a mixed cell is unsigned by PHASE2 #25):
  - When `Y_WIDTH <= 36`, both operands are **sign-extended to `Y_WIDTH` bits**: `a[i] = A[a-1]` for
    `a <= i < Y_WIDTH`, and likewise for `b`. Bits above `Y_WIDTH` are the pad. The low `Y_WIDTH`
    bits of the unsigned product are then the signed product.
  - When `Y_WIDTH > 36`, the cell stays soft with a warning.

  Parmys pads with `unconn` whatever the sign, which is wrong for a negative operand. This is
  divergence D3, consistent with the adder ruling. None of the 19 has a signed `$mul`.
- **M-6 Explicit `multiply` instances** (a design that instantiates the hard cell, 2D §3.1a)
  are treated like the equivalent `$mul`, as Parmys resolves the primitive to `$mul`: `WIDTH = 2`
  becomes a `$mul` cell for `lower` (the 2×2 rule, M-2), any other `WIDTH < 36` is padded to a
  36-wide `multiply` exactly as a `$mul` of that shape (M-3, M-4), `WIDTH = 36` is left as it is.
  Each instance so treated gets a warning: `hardmap: explicit 'multiply' instance 'u' (WIDTH 2)
  treated as '$mul' as Parmys does`. Above 36 they are a located error: `hardmap: 'multiply'
  instance 'u' has WIDTH 40; the arch's multiplier is 36 wide`. Parmys would split it, which is
  Phase 4. None of the 19 has such an instance. Amended 2026-10-17, agent default under Peter's
  Parmys-default rule (it replaces "untouched when `WIDTH <= 36`"; the setting `--vtr-compat` does
  not reach passes, so the rule applies always).
- **M-7 Order and naming.** `$mul` cells are mapped in node-ID order. Each new node's name is the
  source cell's name. Its output net names follow 3E's short or provenance style.

## 5. `$add` / `$sub` → `adder` chains (A-1 … A-9)

- **A-1 Candidates.** Every live `$add` and `$sub` with `max(A_WIDTH, B_WIDTH) >= 1`, since
  `min_threshold_adder = 1` and `min_add = 0`. In effect every adder and subtractor goes hard, with
  constant operands too. `$neg` stays soft (S-1). Parmys's `reduce_operations` merges identical
  adders, which is CSE that `opt_merge` already did, so 3C has no such step.
- **A-2 Signedness.** `s = odin3_opt_both_signed(cell)`, the PHASE2 #25 rule. For an operand `X` of
  width `w`, `ext(X)[i]` is:
  - `X[i]` for `i < w`;
  - `X[w-1]` for `i >= w` when `s`;
  - gnd for `i >= w` otherwise.
- **A-3 Chain length** (Parmys `iterate_adders`: `count = max(a,b) + 1 + 1`). Let `n = max(A_WIDTH,
  B_WIDTH)`. The chain has `n + 2` `adder` cells:
  - a **head**;
  - **stages** `0 … n-1`;
  - a **top stage** `n`.

  The count does not depend on `Y_WIDTH`, so adder counts match Parmys (decision 3's Netlist row).
- **A-4 `$add` chain:**
  - Head: `a = vcc, b = gnd, cin = gnd`, so its `cout` is 0 and its `sumout` is left unread
    (Parmys: `vcc`/`gnd` on `a`/`b`; `cin` gets the gnd pin `check_missing_ports` adds).
  - Stage `i` (0 ≤ i ≤ n): `a = ext(A)[i]`, `b = ext(B)[i]`, `cin` = the previous stage's `cout`.
  - `Y[i] = sumout_i` for `i <= min(n, Y_WIDTH-1)`. The top stage's `cout` is left unread.

  For an unsigned cell the top stage gets `a = b = gnd`, exactly Parmys's form, so the chain is
  Parmys's chain bit for bit.
- **A-5 `$sub` chain** (`a + ~b + 1`, decision 4):
  - Head: `a = vcc, b = vcc`, `cin` = the pad (Parmys `split_adder_for_sub`), so its `cout` is 1.
  - Stage `i`: `a = ext(A)[i]`, `b = ~ext(B)[i]`.
    - The inversion folds a constant: `~gnd` is vcc and `~vcc` is gnd.
    - Otherwise it is a 1-bit `$not` (Parmys's NOT nodes). One `$not` is made per distinct B
      bit, so a repeated sign bit gets one inverter.
  - `Y` as in A-4.
- **A-6 Sign extension, by ruling: divergence D1.** Parmys pads every bit at or above an operand's
  width with `unconn`:
  - `split_adder` (`adder.cc` line 700: "connect those pins to unconn");
  - `reduce_input_ports`, which equalises `$sub` operands with pad pins.

  That pad is 0, and for `$sub` its inverse is 1. Parmys is therefore right for unsigned cells and
  wrong for signed ones. 3C uses A-2's `ext`, so a signed narrow operand carries its sign into
  every higher stage. The netlists then differ from Parmys on the 6 designs in survey finding 7
  (`stereovision0/1/2`, `LU8PEEng`, `LU32PEEng`, `mcml`), at the stages above the narrow operand
  only:
  - adder pins carry the sign bit, or its `$not` for `$sub`, instead of `unconn`;
  - the adder count does not change.
- **A-7 Y wider than the chain** (only without 3G, which caps Y at `n + 1`).
  - **Amended 2026-10-17 (PHASE3 #39 (d), agent default):** a cell with `Y_WIDTH > n + 1` stays
    soft with a WARNING ("run wreduce first"); padding the extra Y bits would allocate after the
    delete and break H-9. The pad rule below applies only if that restriction is lifted.
  - Extra Y bits get the sign of the result (`Y[n]`) when `s` or the cell is a `$sub`, else gnd.
    This is Yosys's own pad rule (3G WR-4 §4.3 step 6).
  - Parmys gives gnd for `$add` (`check_missing_ports`) and `unconn` for `$sub`
    (`equalize_ports_size`). That is divergence D2: correct where Parmys is not.
- **A-8 Unsigned `$sub` with unequal widths.** With A-2, `ext(B)` zero-extends B, so every stage
  above B gets `b = ~gnd = vcc`. Parmys gives `~unconn`, which is also 1 after ABC, so the result
  matches.
  - Whether Parmys's top stage (`n`) of a subtraction is right is not proved here. Its `b` pin is
    `unconn` there, and the borrow bit `Y[n]` needs `~0 = 1`.
  - 3C follows A-5 (correct by construction).
  - The plan's first evidence task runs a `sub_pad` micro family through Parmys and iverilog. If
    Parmys differs there, the difference becomes divergence D2b in §10 (question 5).
- **A-9 Explicit `adder` instances.**
  - A 1-bit hard `adder` instance is untouched.
  - `adder #(W)` resolves to `vtr_primitives.v`'s soft module (2D §3.1a). After `flatten` that is
    `$add` cells, mapped by A-4.

## 6. Memories (R-1 … R-12)

### 6.1 Explicit RAM instances (the only RAMs in the 19)

With `read_verilog --vtr-compat` and the techlib read first, an instance `single_port_ram
#(.ADDR_WIDTH(A), .DATA_WIDTH(D)) u (…)` resolves to the hard cell with those parameters (2D §3.1a,
item 1). `hardmap` legalises it to the arch's shape, as Parmys's `iterate_memories` does.

- **R-1 Order.** Depth split first, then width split, then address padding (Parmys:
  `split_*_memory_depth`, then `split_*_memory_width`, then `pad_memory_input_port`).
- **R-2 Depth split** (`ADDR_WIDTH = A > 15`; `split_memory_depth = 15`). This is recursive on the
  **lowest** address bit, as Parmys's `split_sp_memory_depth`/`split_dp_memory_depth` do
  (`addr->pins[0]`). Each step makes two RAMs of `A-1` address bits, `addr[A-1:1]`, called `__S`
  (written when `addr[0] = 1`) and `__H` (written when `addr[0] = 0`):
  - `we_S = we & addr[0]` and `we_H = we & ~addr[0]`, both as `$and`/`$not` glue;
  - each output bit is `$mux(sel, out_H, out_S)`;
  - for a dual-port RAM, each port is split by its own address and select.

  The recursion stops at 15.
- **R-3 Width split** (`split_memory_width = 1`). One slice per data bit `j`, with
  `DATA_WIDTH = 1`:
  - `data[j]`/`out[j]` (or `data1[j]`, `data2[j]`, `out1[j]`, `out2[j]`);
  - the shared `addr*`, `we*` and `clk`.
- **R-4 Address padding.** Address bits above `A`, up to 15, are the pad (`unconn`). VPR deletes
  that net and leaves the pins open, which is how Parmys pads (PHASE3 3E rules).
- **R-5 Constant controls are kept.** A `we` on a constant stays wired to it, so a read-only RAM
  stays a RAM: survey finding 5 lists SP and DP instances with constant `we`, and Parmys keeps them.
  Explicit primitives have no INIT, so nothing is dropped.
- **R-6 Two live write ports** (`dual_port_ram` with `we1` and `we2` both live: `LU8PEEng`,
  `LU32PEEng`). Each slice gets both write ports, and no priority logic is added. What happens on a
  same-address write in one cycle is the primitive's; in the RTL it is an IEEE race (2D §17 #66).
  Odin III maps it exactly as Parmys does. The functional check lists these instances as
  "same-address dual write undefined" and must not drive both writes to one address (decision 16
  list). It is not a divergence.
- **R-7 The depth-split select: suspected Parmys defect, divergence D4** (question 6).
  - Parmys's output mux selects with the **live** `addr[0]`, but `out` is the RAM's registered
    read of the address at the last clock edge.
  - So when `addr[0]` changes after a read, the mux picks the other half.
  - 3C registers the select: `sel = $dff(clk, addr[0])`, one 1-bit FF per split level per read
    port. That costs one FF and no LUT level.
  - Only `mcml` is affected: 3 instances, 1 level, 2 ports, so 6 FFs.
  - The plan's first evidence task confirms Parmys's behaviour with a micro (a 16-address-bit
    `dual_port_ram` read at changing addresses, Verilator check against the RTL) before D4 is
    recorded.

### 6.2 Inferred `$mem` (decision 6; micros only, survey finding 4)

- **R-8 FF merge** (Yosys `memory_dff` subset, decision 6 step 1). For each read port of a `$mem`
  with an asynchronous read (`RD_CLK_ENABLE` bit 0):
  - **Data merge:** if every `RD_DATA` bit feeds exactly one `$dff` (or a `$dffe` whose enable
    becomes the port's `RD_EN`), and these FFs share one clock and polarity, then the FFs go, and
    the port becomes synchronous on that clock with `RD_TRANSPARENT = 0`.
  - **Address merge, otherwise:** if every `RD_ADDR` bit is the `Q` of a `$dff` on one clock, and
    each such `D` net feeds nothing else that would change, the address FFs are bypassed, and the
    port becomes synchronous with `RD_TRANSPARENT = 1` when a write port on the same clock exists.
  - Yosys's further cases (reset values, `$sdff` with init, mixed clocks) are not merged. The port
    stays asynchronous.
- **R-9 A read port that stays asynchronous**, or a memory whose ports use more than one clock
  domain: the whole `$mem` stays for `lower` (FFs and muxes), with the warning `hardmap: memory
  '<m>' has an asynchronous read port; left to lower as flip-flops`.
  - Parmys instead drops `RD_CLK`, takes `WR_CLK[0]` as the RAM clock, and maps the port as
    synchronous (`ymem2_to_bram`). That adds a cycle of latency, which is wrong.
  - Divergence D5, decision 6 step 4.
- **R-10 Port patterns** (Parmys `map_bram_to_mem_hardblocks` / `map_rom_to_mem_hardblocks`), with
  `r` read ports and `w` write ports after R-8:

  | r, w | Parmys | 3C |
  |---|---|---|
  | 1, 0 | SP, `we` = gnd, `data` = pad | same |
  | 2, 0 | DP, `we1` = `we2` = gnd | same |
  | > 2, 0 | SP with multiplexed read ports (`create_nr_single_port_ram`) | one SP replica per read port (decision 6 step 2; question 7) |
  | 1, 1 | SP if the read and write address nets are identical (`check_same_addrs`), else DP (port 1 write, port 2 read) | same |
  | 1, 2 | DP `r2w` | DP: each port is one write port, and the read goes on the port whose write address is identical; if neither is identical, soft (R-11) |
  | 2, 1 | DP `2rw` | DP: write and the read with the identical address on port 1, the other read on port 2; if neither read address is identical, two DP replicas (each with the write on port 1 and one read on port 2) |
  | 2, 2 | DP `2r2w` | DP: each port pairs a write with the read of identical address; otherwise soft (R-11). Two live write ports: R-6 |
  | > 2, ≥ 1 or ≥ 1, > 2 | DP with multiplexed ports (`create_nrmw_dual_port_ram`) | one DP replica per read port when w = 1; otherwise soft (R-11; question 7) |

  `wreduce` (3G) may leave a one-word `$mem` with `ABITS` 0 and empty address ports; R-1 pads that
  address to the RAM's minimum address width (zeros) like any other short address.

  Every hard mapping is then sliced and padded by R-1 … R-4 (`ABITS` as the address width, `WIDTH`
  as the data width). The read-enable `RD_EN` has no pin on the arch's RAMs:
  - Parmys drops it;
  - 3C maps it only when it is constant 1;
  - otherwise the port is treated as R-9 (`lower`), with the warning.
- **R-11 Soft fallback.** A `$mem` that R-10 sends soft is left for `lower` with the warning
  `hardmap: memory '<m>' (<r> read, <w> write ports) has no fixed-subset mapping; left to lower`.
- **R-12 INIT.** A `$mem` with a defined `INIT` that is mapped hard drops it, with the warning
  `hardmap: memory '<m>': initial contents dropped (the arch's RAMs have no init)` (decision 6
  step 5; Parmys drops it silently). The functional check lists it.

## 7. What stays soft (S-1 … S-4)

- **S-1** `$neg`, every comparison (`$lt $le $gt $ge $eq $ne`), `$div $mod $pow`, shifts, `$shiftx`,
  logic and reduction cells, muxes and every FF. Parmys's `from_yosys_type` maps only `$add $sub
  $mul $mem(_v2)` and the four primitives. Comparator chains are Phase 4.
- **S-2** `$mul` that M-2 leaves soft: `2×2`, wider than 36, or signed with `Y_WIDTH > 36`.
- **S-3** `$mem` that R-9 or R-11 leaves soft.
- **S-4** Everything `lower` (3B) lowers: the glue of H-6, and the soft cells above.

Each soft decision except S-1 logs one WARN line per cell. S-1 is the default and logs nothing.

## 8. Determinism, memory, diagnostics (H-8 … H-10)

- **H-8 Determinism.** Cells are mapped in node-ID order: `$mul`, then `$add`/`$sub`, then RAM
  instances and `$mem`, as Parmys's `optimization` orders them. New nodes are appended. Two runs
  give byte-identical BLIF (PHASE3 exit item 3).
- **H-9 Memory.**
  - Each mapping builds all its nodes and nets before it kills the source cell. On `NO_MEMORY` the
    IR is valid and the source cell is intact.
  - The design budget (`odin3_budget_check`, 60%) is checked per cell kind and every 2^16 created
    pins. `LU32PEEng` makes 5,251 RAM slices and about 13,500 adders, which is small.
  - `hardmap` never compacts; the script's `compact` after `opt -full` does (PHASE2 #19).
- **H-10 Diagnostics** (2F §16 style).
  - INFO `hardmap: N multiply, M adder in K chains, R single_port_ram + D dual_port_ram slices; S
    cells left soft`.
  - WARN lines as in M-2, M-5, R-9, R-11 and R-12.
  - ERROR lines as in H-3 and M-6.
  - DEBUG one line per mapped cell.

## 9. Evidence plan (identical or equivalent to Parmys except the declared divergences)

1. **Rule tests** (Unity, hand-built IR): one test per rule ID. They assert the exact pins and
   parameters, a FULL `check`, and the before/after simulation of 1E on cells without RAM.
   - `sx_add` (A-6): Y[9], Y[10] right against iverilog's trace.
   - `sub_pad` (A-8).
   - Signed and unsigned 2×N, 1×N, 2×2 and 37×4 `$mul` (M-2).
   - A 16-bit-address `dual_port_ram` (R-2, R-7).
   - The two-write-port slice (R-6).
2. **Parmys behaviour probes first** (the plan's first task, before code). Micros go through
   Parmys (`tools/run-oracle.sh` into a scratch golden root) and iverilog/Verilator, for questions
   5 and 6 and for M-4's open pins. They settle D2b and D4 before the rules are frozen.
3. **Micros** (`/run-micro`): every arithmetic and memory micro through the O′ script.
   `netlist-compare` with the Parmys golden must say identical, or `equiv-check` must say
   equivalent. Expected differences are listed in `tests/exit/` style expect files, each naming its
   divergence ID: signed micros (D1/D3) and memory micros with asynchronous reads (D5).
4. **The 19 through `tools/exit-check`** (heavy, controller, smallest first).
   - O′'s pre-ABC netlist against the Parmys golden: identical or equivalent, or a listed
     divergence. `equiv-check` cuts at hard blocks and pairs RAM slices by memory group and bit
     (PR #100). Designs listed in `vtr_unproven.txt` (multi-clock, the largest) fall back to the
     next two points.
   - Hard-block counts per design must equal the golden's (§2 table): `multiply`, `adder`, SP and
     DP slices. The exceptions are `mcml` (+6 FFs, D4) and anything a ruled divergence explains.
5. **The O′ leg** (3H): Odin III's pre-ABC netlist through VTR's ABC stage and VPR on the 19.
   - Every design must reach `vpr_status=success`.
   - `num_mult` and `num_memories` must equal P's (decision 3).
   - `num_clb`, W, CPD and wirelength must be within the bands.
6. **Functional check** (decision 16): Verilator random check on all 19. The declared-divergence
   list per design is:
   - D1: `stereovision0/1/2`, `LU8/32PEEng`, `mcml`. Here Odin III must be RTL-correct where
     Parmys is not. This is the positive test of the ruling.
   - D4: `mcml`.
   - R-6's dual-write instances.
   - Decision 19's async-to-sync resets.

## 10. Divergences from Parmys (declared)

| ID | Odin III | Parmys | Effect on the 19 |
|---|---|---|---|
| D1 | signed operands of `$add`/`$sub` are sign-extended into the chain (A-2, A-6) | `unconn` (0) pad | `stereovision0/1/2`, `LU8/32PEEng`, `mcml`: different adder pins above the narrow operand; same adder count; RTL-correct (Peter's ruling, 2026-10-14) |
| D2 | Y bits above the chain: sign of the result for signed or `$sub`, else gnd (A-7) | gnd (`$add`), `unconn` (`$sub`) | none after 3G (Y ≤ n + 1) |
| D2b | top stage of an unsigned `$sub` uses `~0 = vcc` (A-8) | `unconn` (confirmed by Peter's probe, 2026-10-14: the borrow bit is lost) | pin difference only; Parmys is wrong, Odin III is RTL-correct (`tests/hardmap/sub_pad_u.v`) |
| D3 | signed `$mul` sign-extended to `Y_WIDTH` (≤ 36), else soft (M-5) | `unconn` pad | none (no signed `$mul`) |
| D4 | depth-split output select registered (R-7) | live address bit (confirmed: 468 mismatches in 2000 cycles, 0 with `addr[0]` held) | `mcml`: 6 more FFs (`counts:+6 latch`) |
| D5 | an inferred `$mem` with an asynchronous read stays soft (R-9) | mapped as synchronous | none (no inferred memory) |
| D6 | `2×2` `$mul` soft (M-2) | AND gates + hard adders | none in the 19; micros: 4 fewer hard `adder` per 2×2 `$mul` (`counts:-4 adder`) |
| D7 | `$mul` wider than 36 soft, with a warning (M-2); explicit `multiply` above 36 is an error (M-6); an explicit `multiply` below 36 is treated like the equivalent `$mul` with a warning (PHASE3 #39a) | split into 36-bit pieces + adders | none |
| D7b | unsigned `$mul` with `Y_WIDTH > A_WIDTH + B_WIDTH` soft, with a warning (M-3) | hard, gnd on the extra Y bits | none after 3G |
| D8 | inferred `$mem` with more than two ports, or ports that cannot share a RAM port: replicas or soft (R-10, R-11) | multiplexed port modes | none |
| D9 | `RD_EN` mapped only when constant 1, else soft (R-10) | dropped | none in the 19; micro `common/bram.v` (`counts:-8 dual_port_ram`) |
| D10 | INIT dropped with a warning (R-12) | dropped silently | none |
| D11 | names follow 3E's styles | Parmys's `~` names | names only |
| D12 | an inferred `$mem` whose read address is registered stays asynchronous (R-9) when a write port is on the read clock; the address merge applies only to read-only memories (plan default d) | merged | none (no inferred memory in the 19) |
| D13 | an inferred `$mem` with `OFFSET` != 0 stays soft with a warning (R-11 class) | records the offset, ignores it | none |
| D14 | an inferred `$mem` with a port on the falling clock edge stays soft with a warning (R-11 class) | mapped as a rising-edge RAM | none |
| D15 | a memory with no write port and no INIT is mapped to RAM slices (`we` on gnd, R-10 pattern `p_1_0`) | deleted by Yosys's `opt_mem`: read data x, no RAM | `common/1r.v` and `2r.v` (+8 slices); none in the 19. Not a `hardmap` rule: the cure is an `opt_mem` rule in `opt` |

## 11. Amendments this spec needs (after approval)

Status 2026-10-17 (3C-3): all five are applied (PHASE3 #4/#5 notes, #42, `docs/PASSES.md`). Also
amended by the implementation, each logged in PHASE3 #39 and #42: M-3 and A-7 (a `$mul` with
`Y_WIDTH` above `A_WIDTH + B_WIDTH`, an `$add`/`$sub` with `Y_WIDTH` above `max + 1`, stay soft with
a warning: `wreduce` caps Y, and the alias merges would allocate after the delete, H-9), M-6 (an
explicit `multiply` is treated like the `$mul`), R-8 (D12), R-9..R-12 texts (the warnings are
verbatim in `test_hardmap_mem.c`), R-6 (a hard-mapped `$mem` with more than one write port logs a
warning: a same-address collision is undefined), and the ADDR bound of 31 (an instance above it is
a located error).

- PHASE3 decision 4: "`$mul` with both widths ≥ 3" becomes M-2. "Inputs extended to 36 bits
  (sign-extended when signed)" becomes M-3/M-4/M-5.
- PHASE3 decision 5 and the 3E spec (E11, `stub_width_max`): "36×36 `multiply`" becomes "`multiply`
  with `WIDTH` 36" (the 2D1 survey's note).
- PHASE3 exit item 3: the script of 3G WR-9.
- `docs/PASSES.md`: the `hardmap` row.

## 12. Questions for Peter (each with a recommendation)

**Ruled 2026-10-14 (Peter: "All 10 yes"):** every recommendation below is approved; question 6 (register the depth-split select bit) applies if the micro probe confirms Parmys selects on the live address bit (`.superpowers/sdd/3C-probes/`).

1. **Multiplier threshold.** Should a `$mul` go hard on Parmys's test (hard unless 2×2; M-2) or on
   decision 4's text ("both widths ≥ 3")? **Recommend Parmys's test.** The golden maps
   `diffeq1/2`'s 32×2 multiplies hard. Decision 4's text would make `num_mult` 3 instead of 5 on
   `diffeq1` and 4 instead of 5 on `diffeq2`, which breaks decision 3's "`num_mult` equal".
2. **`multiply` width.** Fixed `WIDTH = 36` (M-3), or `WIDTH = max(a, b)`, which lets VPR pack 9×9
   or 18×18 fractions? **Recommend 36.** It is what Parmys emits (fracture 0), and it keeps
   `num_mult` equal. Fracturing is a Phase 4 QoR lever.
   - 2a. Pad unsigned operands with `unconn` (Parmys; every tool reads it as 0) or with gnd (exact
     in the IR)? **Recommend `unconn`.** It keeps the netlist identical to Parmys's, and
     `equiv-check` and ABC already read it as 0.
3. **2×2 `$mul`**: leave it soft (D6), or build Parmys's AND-and-adder form? **Recommend soft.**
   None of the 19 has one, and the AND-and-adder form is Phase 4's soft-multiplier work.
4. **Signed `$mul`**: sign-extend to `Y_WIDTH` when it is at most 36, otherwise soft (M-5, D3)?
   **Recommend yes.** It follows the same principle as your adder ruling, and none of the 19 is
   affected.
5. **Subtraction top stage (A-8, D2b).** Should 3C build the mathematically correct chain and
   record any Parmys difference as a divergence once the first evidence task measures it?
   **Recommend yes**, rather than copying an unproven pad.
6. **Depth-split select (R-7, D4).** Should the select bit be registered (correct; `mcml` gains 6
   FFs) once a micro confirms Parmys's live-address mux is wrong? **Recommend register.** If the
   probe shows Parmys is right (for example, VTR's RAM model is transparent), 3C copies Parmys and D4
   is dropped.
7. **Inferred memories beyond the simple patterns** (R-10, D8): replicas for many reads with one
   write, and soft otherwise, instead of Parmys's multiplexed port modes? **Recommend yes.** No
   design in the 19 infers a memory, so this only touches micros, and the multiplexed modes are
   Phase 4 memory work.
8. **Asynchronous read ports** (R-9, D5): keep decision 6 step 4, which leaves them soft rather than
   mapping them synchronous as Parmys does? **Recommend keep.** Parmys's mapping adds a cycle, and
   no design in the 19 has one.
9. **Two live write ports** (R-6): map as Parmys does, with no priority logic, and exempt
   same-address dual writes from the functional check? **Recommend yes.** It is an IEEE race (§17
   #66), so no value is mandated.
10. **Plan shape after approval.** About 7 tasks in 3 PRs, smallest risk first:
    - 3C-1: Parmys probes (§9.2), then `$mul`;
    - 3C-2: adder chains;
    - 3C-3: RAM instances, then inferred `$mem`, then the O′ evidence.

    **Recommend this split.** Code waits for 3A (`flatten`) only for the 19; unit tests run on
    hand-built flat IR now.
