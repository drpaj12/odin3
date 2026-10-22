# `unmap`: hard cells back to word-level cells (design, for review)

Status: v1 (2026-10-21), written for Peter's review. Not implemented. Plan:
`docs/plans/2026-10-21-unmap-plan.md`. Rules have IDs so they can be approved or changed line by
line: **S-n** structure, **N-n** net classes, **C-n** carry chains, **M-n** multipliers, **R-n**
RAMs, **P-n** provenance and attributes, **E-n** errors, **T-n** tests. Open questions are in
section 10, each with a recommendation (agent defaults unless Peter overrides).

## 0. Purpose and scope

Loki (the reverse-engineering tool, `docs/RE-ARCHITECTURE.md`) starts from FPGA netlists whose
soft logic is LUTs and whose arithmetic and memories are VTR hard blocks ("floor F1": the output of
`partial_map` and `lower`/`abc`, or a VTR BLIF read with `read_blif`). `unmap` is the inverse of
`partial_map`: it turns the hard cells of `lib/vtr.o3lib` back into word-level cells, with
provenance, so that Loki's first hypotheses on them are exact (probability 1).

| Hard cell (`lib/vtr.o3lib`) | Becomes | Evidence that groups pieces |
|---|---|---|
| `adder` chain (`cout` to `cin`) | one `$add`, or one `$sub` | the carry wiring itself |
| `multiply` | one `$mul` (signedness and widths recovered) | none for one cell |
| `multiply` pieces plus the `$add` tree `partial_map` built (`mul_split.c`) | the original `$mul` | the exact algebraic pattern, section 5 |
| `single_port_ram` / `dual_port_ram` | one `$mem` | none for one cell |
| RAM slices of one memory (width slices, depth slices, read replicas) | the original `$mem` | one shared provenance record (section 6) |

The word-level memory cell of Odin III is `$mem` (`src/ir/cells/mem.c`: parameters `MEMID SIZE
OFFSET ABITS WIDTH RD_PORTS WR_PORTS RD_CLK_ENABLE RD_CLK_POLARITY RD_TRANSPARENT WR_CLK_ENABLE
WR_CLK_POLARITY INIT`, ports `RD_CLK RD_EN RD_ADDR RD_DATA WR_CLK WR_EN WR_ADDR WR_DATA`). `unmap`
does not make `altsyncram` or `lpm_*` (those are `lpmmap`'s blackbox targets; `lpmmap` can run
after `unmap`).

Not in v1: flip-flop hard cells (4.6 was dropped), generic hard blocks (no function to recover),
`$mem` INIT (the arch RAMs have none), signedness of `$add`/`$sub` from names (only from nets, C-6).

## 1. Facts about `partial_map` that `unmap` inverts (verified by reading the code)

All in `src/passes/partial_map/`.

- **adder** (`adder.c`): cell type `adder`, ports in order `a b cin cout sumout`, all 1 bit. A
  `$add` of `n = max(A_WIDTH, B_WIDTH)` becomes a head `adder` (`a = vcc, b = gnd, cin = gnd`, so
  its `cout` is 0; its `sumout` is never read), stages `0 .. n-1`, and a top stage `n` (a
  `$add` marked "nocarry" by the multiplier split has no top stage). Stage `i` has
  `a = ext(A)[i]`, `b = ext(B)[i]` (`ext` is the operand bit, then the sign net for a signed cell
  or `gnd`), `cin` = the previous `cout`. `Y[i]` is the `sumout` of stage `i`. A `$sub` is
  `a + ~b + 1`: the head has `a = b = vcc` and `cin` = the pad, so its `cout` is 1; each stage `b`
  is inverted (a constant folds, any other net goes through one shared 1-bit `$not` per distinct
  net inside the chain, role `nb<bit>`).
- **multiply** (`mul.c`): `multiply WIDTH=W` (36 on k6), ports `a` (W), `b` (W), `out` (2W). The
  operand bits the `$mul` has go to the low pins and every other pin is the pad. A both-signed
  `$mul` is sign-extended to `Y_WIDTH` before (limit = `Y_WIDTH`, at most W; bits above are pad).
  `out[k]` drives `Y[k]`; the unused outputs have no reader.
- **multiplier split** (`mul_split.c`, spec X-4): an unsigned `$mul` with an operand above W is
  rewritten into `$mul` and `$add` cells and each piece is mapped by the rules above. Three
  shapes, all with `W` the arch width and `*` meaning a `$mul`:
  - **one side, A above W:** `m00 = A[0,W) * B`, `m10 = A[W,a) * B`, `add0 = m00.out[W, W+b) +
    m10.out`; `Y = m00.out[0,W) ++ add0.out`.
  - **one side, B above W:** `m00 = A * B[0,b-W)`, `m01 = A * B[b-W,b)` (W bits), `add0 =
    m00.out[b-W, b-W+a) + m01.out`; `Y = m00.out[0,b-W) ++ add0.out`. (`split_b` in the source.)
  - **both above W:** `m00 = A[0,W)*B[0,W)`, `m11 = A[W,a)*B[W,b)`, `m01 = A[0,W)*B[W,b)`,
    `m10 = A[W,a)*B[0,W)`, `add0 = m10 + m01` (each operand with one extra `gnd` bit), `add1 = add0 +
    (m00.out[W,2W) ++ m11.out)`; `Y = m00.out[0,W) ++ add1.out`.
  A piece still above W is split again (so the pattern nests). With `Y_WIDTH <= W` (both above) or
  `Y_WIDTH <= W` / `<= b-W` (one side) only `m00` is built. The adders have no top stage.
  Option `--parmys-split-ext` sign-extends the low product; `unmap` does not recognise that form.
- **RAM** (`ram.c`, `ram_split.c`, `mem.c`, `mem_plan.c`): `single_port_ram` ports `clk data addr
  we out`; `dual_port_ram` ports `clk data2 data1 addr2 addr1 we2 we1 out2 out1`; parameters
  `ADDR_WIDTH` (default 15) and `DATA_WIDTH` (default 1). Both are read-first, synchronous on the
  rising edge of `clk`. A `$mem` becomes slices of one data bit each (`--mem-width arch` allows
  wider), addresses padded with the pad up to `ADDR_WIDTH`, data pins of a port without a write
  padded with the pad and its `we` tied to `gnd`, a port without a reader leaves `out` open. More
  address bits than the depth limit D cause a depth split: `2^k` leaves, each leaf's `we` is an
  `$and` tree of the write enable with address bits or their `$not`, each read port has one
  registered select (`$dff`) per level and each data bit has a `$mux` tree over the leaves. A write
  port is replicated per read port in some patterns (D8), and a read port with an enable other than
  constant 1 gets hold logic (`$mux`, `$dff`) around the RAM output (R-10).
- **Names and attributes left behind:** every node a transaction creates carries the string node
  attribute `hardmap` = `<cell type>:<role>` (`hm_kit.c`, `set_hardmap_attr`) and is named
  `<source cell>.<role>`; the first non-glue node (the namesake) has the source's name and
  attributes, then `hardmap` is set on it too. The nocarry marker `partial_map_nocarry` is removed
  at the end of the pass. Roles: adder `head s<i> top`, glue `nb<i>`, multiply `mul`, split pieces
  `m00 m01 m10 m11 add0 add1`, RAM `b<first bit>[.<path>]`, glue `we[<p>].<path>`,
  `sel[<p>].l<L>`, `nad[<p>].l<L>`, `mux[<p>].b<bit>[.<path>]`, `rd<p>.enq|enn|sel<i>|hff<i>`.
- **Provenance:** one operation and one DERIVED record per source cell (`hm_begin_cell`), parent
  the source's record; every node of the transaction shares that record. This is the evidence R-6
  uses. A multiplier split is several transactions (one for the split, one per piece), so its
  pieces do not share a record; section 5 therefore uses algebra, not provenance.

## 2. Structure (S-1 to S-7)

- **S-1 One pass `unmap`.** Registered as `odin3_pass_unmap` (`src/passes/unmap/register.c`),
  `processes` REFUSE. Runs on every module in module-ID order, cells in node-ID order. Input: RTLIL
  view, netlist view (hard cells plus `$lut`/`$sop`/latches) or a mix; the output is the same
  design with word-level cells instead of hard cells (a mixed design; `lower` accepts it).
- **S-2 Two input situations, one algorithm.** (a) After `partial_map` in the same session:
  provenance, names and the `hardmap` attribute exist. (b) After `read_blif` of a VTR BLIF with
  `read_techlib lib/vtr.o3lib`: IMPORTED provenance (one record per `.subckt` line), names from the
  file, no `hardmap` attribute. Every recovery rule uses nets and parameters only; the one rule
  that also needs evidence of origin (R-6, RAM slice merge) uses provenance and, absent that, only
  records a candidate attribute. Names and the `hardmap` attribute are never read by a rule; they
  are used for ordering a tie (R-6) and are checked in tests.
- **S-3 Phases per module.** A: adder chains (C). B: `multiply` to `$mul` (M-1 to M-3). C:
  multiplier regroup (M-4 to M-6). D: RAM cells to `$mem` cells, grouped by origin before the
  conversion (R-1 to R-7). F: depth glue recovery on the `$mem` cells of D (R-8, only if word-level
  glue exists). C, the grouping inside D, and F are skipped with `--no-regroup`.
- **S-4 Option.** `unmap [--no-regroup]`. Anything else is `ODIN3_ERR_INVALID_ARG` with
  `unmap: unknown argument '<x>' (known: --no-regroup)`. Parsing with `odin3_pass_arg_next` and
  `odin3_pass_arg_is`, as `register.c` of `partial_map`.
- **S-5 Replacement transaction (`um_replace`).** For a group of old nodes and one new cell: (1)
  one provenance operation, `odin3_prov_begin_op`, then `odin3_prov_derive` with the old nodes'
  records as parents in node-ID order (a repeated record once; `0` when no old node has any;
  this is the clump rule of `docs/IR.md` section 6 and what `raise` does in `make_prov`); (2)
  `odin3_node_create_connected` of the new cell directly on the old boundary nets (raise does the
  same: the output nets have two drivers until step 4); (3) copy the namesake's attributes with
  `odin3_kit_copy_attrs`, remove `hardmap`, set `unmap` (P-2); (4) `odin3_node_delete` of the old
  nodes (a failure on the first delete deletes the new cell again, as `replace_group` in
  `raise.c`); (5) `odin3_node_rename` of the new cell to the namesake's name. Nets that only the
  old nodes touched (carry nets, the head's nets, pad and constant nets) stay in the module with no
  pin; `opt_clean` removes them. Each group is a unit: after any failure the IR is valid and
  earlier groups stay replaced.
- **S-6 Determinism.** Node-ID order everywhere; no pointers, hash-order or time in any decision.
  Two runs on equal designs give byte-identical `write_rtlil`.
- **S-7 Idempotent.** A second `unmap` finds no hard cell of a recovered kind and changes nothing.
  Declined cells (P-3) are declined again with the same note.

## 3. Net classes (N-1 to N-3)

- **N-1 `um_net_class(module, net)`** returns one of `UM_NONE` (invalid net: pin unconnected),
  `UM_PAD`, `UM_GND`, `UM_VCC`, `UM_SIG`. Order of tests: invalid net is `UM_NONE`; **pad** first:
  `odin3_net_const_value` is `ODIN3_CONST_X`, or the net is named `unconn` and driven by exactly
  one zero-input `$sop` with empty cover (that is how `read_blif` and `write_blif --vtr` spell the
  pad; `odin3_abc_net_undefined` in `src/passes/abc/region.c` is the same test; copy it, do not
  include the ABC header). Then `ODIN3_CONST_0` is `UM_GND`, `ODIN3_CONST_1` is `UM_VCC`, anything
  else `UM_SIG`. (The `unconn` net is constant 0 to `odin3_net_const_value`, so the pad test must
  come before the `UM_GND` test.)
- **N-2 `um_observed(module, net)`** is `odin3_net_sinks(module, net).count > 0` (port nodes are
  nodes, so a module output counts; Task 2 pins this with a test). Used to find the used width of
  an output.
- **N-3 Zero-extension nets.** In adder operands and multiplier operands, a trailing `UM_PAD`,
  `UM_NONE` (multiplier only, C-3 for adders) or `UM_GND` bit is a width marker, never data.

## 4. Carry chains (C-1 to C-8)

Algorithm (explicit worklist, no recursion):

- **C-1 Discovery.** Let `adders` be the live nodes whose type is the registered hard `adder`, in
  node-ID order. For each, look at the net on its `cout` pin and count its sinks: `cins` = sinks
  that are the `cin` pin of an `adder`, `others` = all other sinks. Link `x` to `y` when `cins = 1`
  (that sink is on `y`) and `others = 0`; `succ[x] = y`, `pred[y] = x`. No link is made out of a cell with a tap or a branch (it
  ends its walk; the cells after it start their own walks and are declined by C-3 with reason
  `cin_carry`, which Loki can read as "this chain continues that one"). Start cells are those with
  no `pred`; walk `succ` with a loop and a visited flag per node. Cells never visited belong to a
  cycle: declined, reason `cycle`. A walk gives the ordered cell list `cells[0..m)`.
- **C-2 Head.** `cells[0]` is a **head** when its `a`, `b`, `cin` classes are all constants (`UM_GND`,
  `UM_VCC`, with `UM_PAD` allowed only on `cin` when `a` and `b` are both `UM_VCC`) and its `sumout`
  is not observed. Its carry `c0 = majority(a, b, cin)`: 0 for the `$add` head, 1 for the `$sub`
  head. A head is part of the group and the chain's stages are `cells[1..m)`. If `m = 1` (a head
  alone) the cell is left (note `lone_head`).
- **C-3 No head.** Otherwise `cells[0]` is stage 0 and `c0` is the class of its `cin`: `UM_GND`
  gives 0, `UM_VCC` gives 1; `UM_PAD`, `UM_NONE` or `UM_SIG` decline the chain (reasons `cin_x`,
  `cin_open`, `cin_carry` when the net is the `cout` of an `adder` that C-1 did not link,
  `cin_signal` otherwise). (A user `adder` instance with a real carry-in is declined; see open
  question 2.)
- **C-4 Result width.** `stages = m - 1` with a head, else `m`. `k` is the highest stage index
  whose `sumout` net is observed, plus 1 (`k = 1` if none is). Stages at or above `k` are dropped
  with their cells (their sums are unobserved and nothing above depends on them being kept). If
  no stage was dropped and the last stage's `cout` is observed (it has sinks), then `k = stages + 1`
  and the extra bit `Y[stages]` is that `cout` net (it equals the sum of a stage with `a = b = gnd`,
  and `A`, `B` get no bit there).
- **C-5 Operands.** `A[i]` = the net on `a` of stage `i`, `B[i]` = the net on `b` of stage `i`, for
  `i < k`. For a `$sub` (`c0 = 1`) every `b` must be `UM_GND`/`UM_VCC` (inverted: `UM_GND` becomes
  `UM_VCC` and the reverse) or the output of a 1-bit `$not` node (replaced by that node's input);
  otherwise the chain is declined (reason `carry_in_one`). A `$not` node whose output net has no
  sink outside this chain's `b` pins is put in the group and deleted, otherwise it stays. Then
  trailing `UM_GND`/`UM_PAD` bits are trimmed from `A` and from `B` independently (at least one bit
  stays). The new cell: `$add` when `c0 = 0`, `$sub` when `c0 = 1`; `A_WIDTH`, `B_WIDTH` the trimmed
  lengths, `Y_WIDTH = k`; ports `A`, `B`, `Y` with `Y[i]` = `sumout` net of stage `i`.
- **C-6 Signedness.** The top run of an operand of length `len` is the minimal `w <= len` such
  that bits `w-1 .. len-1` are all the same `UM_SIG` net (`w = len` when the top bit is alone). The
  cell is signed (`A_SIGNED = B_SIGNED = 1`) when no operand lost a constant tail in C-5 and at
  least one operand has a top run with `w < len`; each operand is then cut to its `w`. Otherwise it
  is unsigned and nothing is cut. A signed recovery is exact: bits above the sign net are the sign
  extension, which a signed `$add`/`$sub` supplies, and `partial_map` builds exactly that (`ext`).
  A signed cell whose operands show no repeated net is recovered unsigned (equal function).
- **C-7 Name and group.** The new cell takes the name and attributes of `cells[0]` (head or first
  stage). The group is `cells[0..stages)`, the absorbed `$not`s, and nothing else.
- **C-8 Cost.** Linear in cells and pins. A 100,000-cell chain is one loop.

## 5. Multipliers (M-1 to M-7)

- **M-1 One cell (phase B).** For each `multiply` of parameter `WIDTH = W`: `ua` = 1 + the highest
  index of `a` whose class is not `UM_PAD`/`UM_NONE` (0 if none, then 1), `ub` likewise; `y` = 1 +
  the highest `out` index with `um_observed`; none observed: `y = ua + ub`. New cell `$mul`:
  `A = a[0..ua)`, `B = b[0..ub)`, `Y_WIDTH = y`, `Y[k] = out[k]`, unsigned, plus node attribute
  `unmap_w = W` (integer).
- **M-2 Signed recovery.** If `ua = ub = u`, `y <= u`, and at least one operand has a top run of one
  `UM_SIG` net (as C-6, minimal `w` with `w < u`), the cell is signed: each operand is cut to its
  `w` (or stays `u`), `A_SIGNED = B_SIGNED = 1`. This is exactly `partial_map`'s signed form
  (operands sign-extended to `Y_WIDTH`, whose `u` equals `Y_WIDTH`); because only the low `y` bits
  are used and the run covers them, the signed and unsigned readings are equal. A signed multiplier
  with no repeated net is recovered unsigned (equal function, signedness not recoverable).
- **M-3 Hard cells left alone.** `W` not equal to the other piece's is irrelevant here; a `multiply`
  whose `WIDTH` parameter is missing or 0 is declined (reason `width`).
- **M-4 Regroup (phase C), what is matched.** A candidate root is an unsigned `$add` made by
  phase A (attribute `unmap` = `chain`), or by this phase. Patterns are tried in node-ID order of
  the root, P2 then P1. A pattern needs unsigned `$mul` pieces carrying `unmap_w`, all equal to `W`.
  `vec(cell, port)` is the list of nets of a port; `Y(cell)` is its `Y` vector.

  **P1 (two products, one adder; the one-side split).** Root `R` with operand lists `RA`, `RB`,
  `k = Y_WIDTH(R)`. Choose `(LO, HI)` among `{driver of RA[0], driver of RB[0]}` (try `RA` as LO
  first; the drivers are `$mul` cells found with `odin3_net_driver` and `odin3_pin_node`). Let the
  shared port be `S` (`B` for an A-split, `A` for a B-split; try `B` shared, then `A`) and the
  split port `X`. Required:
  1. `vec(LO,S) == vec(HI,S)` (same nets in the same order), `s = |vec(LO,X)|`, `h = |vec(HI,X)|`,
     `q = |vec(LO,S)|`.
  2. `W` equals `s` or `h` (the arch-width evidence), and both pieces carry `unmap_w = W`.
  3. `|RA| = min(k, q)` and `RA[i] = Y(LO)[s+i]`; `|RB| = min(k, h+q)` and `RB[i] = Y(HI)[i]`.
  4. Every net `Y(LO)[j]` for `j >= s` and every net of `Y(HI)` has exactly one sink and it is the
     `A` or `B` pin of `R` that step 3 named. The nets `Y(LO)[j]`, `j < s`, may have any sinks.
  5. `Y_WIDTH(LO) = s + |RA|` and `Y_WIDTH(HI) = |RB|` (these follow from step 4 when the nets are
     observed only by `R`; the check guards against trimmed or extra bits).
  The result: a `$mul`, `A` and `B` the concatenation of the two split-port vectors (`LO` first)
  on the split port and the shared vector on the other, `Y_WIDTH = s + k`, `Y = Y(LO)[0..s) ++
  Y(R)[0..k)`. Group: `LO`, `HI`, `R`. Namesake: `LO`. Attribute `unmap_w = W`.

  **P2 (four products, two adders; the both-sides split).** Root `R1` (the total adder), `k1 =
  Y_WIDTH(R1)`. `R1.A` must be exactly the `Y` list of an unsigned `$add` `R0`: `R1.A[i] = Y(R0)[i]` for
  all `i` and `|R1.A| = Y_WIDTH(R0)`. `R1.B[0..W)` must be `Y(M00)[W..2W)` for a `$mul` `M00` and
  `R1.B[W..)` an initial segment of `Y(M11)` for a `$mul` `M11`. `R0.A` and `R0.B` are initial
  segments of `Y(C1)` and `Y(C2)` for two `$mul` cells (try `C1 = driver of R0.A[0]` as the cell
  with ports `(M00.A, M11.B)`, else as the cell with ports `(M11.A, M00.B)`). Required:
  1. `|M00.A| = |M00.B| = W`, `|M00.Y| = 2W`; `a1 = |M11.A|`, `b1 = |M11.B|`, both `>= 1`.
  2. `C1` has ports `A = M00.A`, `B = M11.B`; `C2` has `A = M11.A`, `B = M00.B` (or the two cells
     swapped; the adder `R0` can take either order).
  3. With `k0 = Y_WIDTH(R0)`: `|R0.A| = min(k0, |Y(C1)|)`, `R0.A[i] = Y(C1)[i]`, `|R0.B| =
     min(k0, |Y(C2)|)`, `R0.B[i] = Y(C2)[i]`; `|R1.B| = min(k1, W + a1 + b1)`, `R1.B[W+i] =
     Y(M11)[i]`; and `|Y(C1)| = |R0.A|`, `|Y(C2)| = |R0.B|`, `|Y(M11)| = |R1.B| - W` (all trimmed
     to the observed width). The cross cells have `|A|+|B|` equal to `a1+W` and `W+b1` bits in some
     order; a cell with the sizes `(W, b1)` or `(a1, W)` is the one named in step 2.
  4. Sinks: `Y(M00)[W..2W)` and `Y(M11)` only on `R1.B`; `Y(C1)`, `Y(C2)` only on `R0`; `Y(R0)` only
     on `R1.A`. `Y(M00)[0..W)` may have any sinks.
  The result: `$mul`, `A = M00.A ++ M11.A`, `B = M00.B ++ M11.B`, `Y_WIDTH = W + k1`, `Y =
  Y(M00)[0..W) ++ Y(R1)[0..k1)`. Group: `M00 M11 C1 C2 R0 R1`. Namesake `M00`. `unmap_w = W`.
- **M-5 Fixpoint.** A scan visits every candidate root once; the scan repeats while it changed
  something (a nested split needs the inner group merged first). Every success removes at least two
  cells, so the loop ends; the scan count is at most the nesting depth plus one. A new `$mul`
  made by a merge carries `unmap_w` and is a piece for the next scan.
- **M-6 What is not matched.** `--parmys-split-ext` splits (the low product is extended), a piece
  whose intermediate net has an extra sink (it is observed elsewhere, so the merge would lose it),
  operand mismatch, signed pieces, and any other arrangement of the same `$mul` and `$add` cells
  stay separate word cells. These are not ambiguities (the structure is not the pattern), so no
  candidate attribute is written. When a merge was refused only because step 4 (sinks) failed, the
  root gets note `mul:shared` (P-3) so Loki can see it.
- **M-7 Exactness.** P1/P2 reconstruct the product from `a = |A|` and `b = |B|` bits with `Y_WIDTH`
  low bits; the merged cell computes the low `Y_WIDTH` bits of `A*B` exactly as the pieces compute
  `Y`. Dropped upper pieces (`Y_WIDTH <= W` cases) never existed, so a split that `partial_map`
  reduced to `m00` alone is a plain `W x W` product, equal in its low `Y_WIDTH` bits to the
  original (the low bits of a product depend only on the low operand bits); its operand widths are
  `W`, not the original `a`, `b`. This is the one place where the recovered widths differ from the
  source RTL; the function is equal.

## 6. RAMs (R-1 to R-8)

- **R-1 Shapes.** The hard types are found by name and checked as `hm_need` does (`hm_kit.c`
  `k_shapes`): granularity HARD, the port list above, parameters `ADDR_WIDTH` and `DATA_WIDTH`. A
  RAM type that is not registered means no RAM cell is touched; a registered type with a wrong shape
  is `E-2`.
- **R-2 One cell to a `$mem` description.** For port `p` (`p` in `{1}` for single, `{1,2}` for dual; the
  pin lists are `data2/data1`, `addr2/addr1`, `we2/we1`, `out2/out1`, `clk` shared):
  - `has_write(p)`: the class of `we` is not `UM_GND`.
  - `has_read(p)`: any `out` pin of the port is observed.
  - `ABITS` = 1 + the highest `addr` index of any used port (a port with `has_write` or `has_read`)
    whose class is not `UM_PAD`/`UM_NONE`; at least 1. A port with fewer real address bits is padded
    to `ABITS` with the `gnd` net.
  - `WIDTH` = max over used ports of (1 + the highest `data` index of a writing port that is not
    `UM_PAD`/`UM_NONE`) and (1 + the highest observed `out` index of a reading port); at least 1.
  - `RD_PORTS` = number of reading ports, at least 1 (a RAM nobody reads keeps one unconnected read
    port); `WR_PORTS` = number of writing ports. Order: port 1 then port 2 (the pins `*1` first).
  - Parameters: `SIZE = 2^ABITS` (the source `SIZE` is lost; reads above it were undefined, so the
    function is equal on defined behaviour), `OFFSET = 0`, `MEMID` = the new cell's name,
    `RD_CLK_ENABLE = 1` and `RD_CLK_POLARITY = 1` per read port, `RD_TRANSPARENT = 0`,
    `WR_CLK_ENABLE = 1`, `WR_CLK_POLARITY = 1`, `INIT` empty.
  - Ports: `RD_CLK`, `WR_CLK` = the `clk` net; `RD_EN` = the `vcc` net (find or create a constant
    node as `hm_const` does); `RD_ADDR`, `WR_ADDR` = the port's `addr` nets; `RD_DATA` = the port's
    `out` nets; `WR_DATA` = the `data` nets; `WR_EN` = `WIDTH` copies of the `we` net.
  Single and dual cells use the same rule (`ram_layout` of `ram.h` gives the pin order). A write
  data bit that is `UM_PAD` inside the used width is passed through as the pad.
- **R-3 Dead RAMs.** No port used: `WIDTH = 1`, `ABITS = 1`, one unconnected read port, nothing
  observable; the cell is converted all the same (`opt_clean` removes it).
- **R-4 Read enable and hold logic.** `partial_map` emulates a read enable other than constant 1 with
  `$mux` and `$dff` cells around `out`. They stay as word cells. The recovered `$mem` has `RD_EN =
  vcc`; the function is equal. They are not absorbed (open question 3).
- **R-5 Origin and grouping order.** Two hard cells have the same **origin** when
  `odin3_node_prov` is the same non-zero record. Slices of one `$mem` or one explicit RAM instance,
  and its glue, share one record (section 1). Cells from `read_blif` have a distinct IMPORTED record
  each: no common origin. Phase D works on the hard cells before any conversion (a converted cell
  has a new record, which would hide the origin): (1) summarise every RAM cell as in R-2 (a `um_ram`
  record); (2) unless `--no-regroup`, group by origin with R-6 then R-7; (3) convert each group
  with one `um_replace`. A group of one cell is the plain R-2 conversion. The new `$mem` carries
  the integer node attribute `unmap_origin` = the shared record ID (absent when the group's cells
  had none in common), which R-8 reads.
- **R-6 Width merge.** Cells with the same origin, the same `ABITS` and port structure, and
  identical nets on every `clk`, `addr` and `we` pin (nets, not values; the data and `out` pins may
  differ) are width slices of one memory. They merge into one `$mem` with `WIDTH` the sum, the data
  bits in slice order (the number after `b` in the `hardmap` attribute role when every member has
  it, else node-ID order; the test compares the two orders), `WR_EN` = `WIDTH` copies of the shared
  net. Same key but different or zero origin: **ambiguous** (two memories with equal control
  signals are equal in function to one wide memory), so nothing is merged and every member gets
  `unmap_cand` = `ram:<N>` (P-3), `N` the index of the candidate set in node-ID order of its lowest
  member.
- **R-7 Replica merge.** Cells (after R-6) with the same origin, `ABITS`, `WIDTH` and identical
  nets on every write pin (`clk we addr data`) are replicas of one memory built for several read
  ports (D8). They merge into one `$mem` with the union of the read ports (first cell's ports
  first) and one write port set. Cells with no write port merge only with the same origin (a ROM
  has no content to compare). Different origin and identical writes: ambiguous, `unmap_cand` =
  `ram:<N>` as in R-6.
- **R-8 Depth merge (phase F, needs word-level glue).** Applies to a set of `2^k` (`k >= 1`) `$mem`
  cells made by phase D, with the same `unmap_origin`, the same parameters and the same nets on
  every pin except `WR_EN` and `RD_DATA` (the leaf address is `addr[k..)` of the source, so the
  source `ABITS = k + leaf ABITS`). The glue cells must have the same provenance record as
  `unmap_origin`. The glue must be present as `$and`, `$not`, `$mux`,
  `$dff` cells of width 1 (RTLIL or mixed view; after `lower` it is LUTs and the rule does not
  fire, the leaves stay as separate `$mem` cells, each exact). Recovery by structure, no names:
  1. **Write enables.** For each leaf, `WR_EN[0]` is the output of a `$and` chain of depth `k`
     ending at a common net `we0`: each step is `$and(prev, t)` with `t` either an address net
     `ad[L]` or the output of a `$not` whose input is `ad[L]`; the chain from the leaf upward gives
     the path bits (`ad[L]` is 1 and `not` is 0) and the address nets `ad[0..k)`, one per level
     (the level is its distance from `we0`). All leaves must agree on `we0` and `ad[]`, and the
     `2^k` paths are all different.
  2. **Read side.** For each read port, `RD_DATA[j]` of every leaf is a leaf input to the `$mux`
     tree of data bit `j` (a `$mux` with `S` = `sel[L]` selects `A` for `sel = 0` and `B` for 1;
     depth `k`; the select of level `L` is the output of a `$dff` with `D = ad[L]` and `CLK` = the
     read clock, shared by all bits of that port). The tree's leaf order must match the paths of
     step 1 (level `L` chooses the leaf with path bit `L`).
  3. **Absorb** the `$and`, `$not`, `$mux`, `$dff` cells of the structure when every output net has
     sinks only inside it (a `$not` or `$dff` shared by several consumers is absorbed when all of
     them are in the group; otherwise it stays and the merge still happens because the net remains
     driven).
  4. **Result:** one `$mem`: `ABITS = k + leaf ABITS`, the address = `ad[0..k)` then the leaf
     address nets, `WR_EN = we0`, `RD_DATA[j]` = the mux tree root nets. Any mismatch in 1 or 2
     leaves the leaves as separate `$mem` cells and writes note `ram:depth` (P-3).

## 7. Provenance, attributes, diagnostics (P-1 to P-4)

- **P-1 Provenance of a new cell.** One DERIVED record, parents = the records of the old nodes in the
  group (S-5); the pass run is `unmap`, so "derivation record `unmap`" is the run recorded by the
  pass manager (`odin3_passrun_name`). With pieces from `partial_map` that share a record, the
  parents collapse to that one record, whose parent is the original source's record. With a BLIF, the
  parents are the IMPORTED records of the `.subckt` lines, so `odin3_prov_sources` reaches their
  file and line.
- **P-2 Node attributes on recovered cells.** `unmap` (string) = `<rule>`: `chain`, `mul`,
  `mul.p1`, `mul.p2`, `ram`, `ram.width`, `ram.replica`, `ram.depth`. `unmap_origin` (integer, R-5). `unmap_pieces` (integer) = the
  number of hard and glue cells absorbed. `unmap_w` (integer) on `$mul` cells (M-1). The attributes
  `hardmap` and `partial_map_nocarry` are never copied. Other attributes of the namesake are
  copied (`odin3_kit_copy_attrs`).
- **P-3 Cells left as they are.** A declined cell or group gets string attribute `unmap_note` =
  `<kind>:<reason>` on every hard cell of it (kind `adder`, `mul`, `ram`; reasons as named in the
  rules), and one WARNING per group: `unmap: module 'm': <kind> at '<name>' not recovered: <reason>`,
  located at the cell (as `hm_diag` does: the first source leaf of its provenance, else bare text).
  Candidate sets (R-6, R-7) get `unmap_cand` = `<kind>:<N>` on each member, and one INFO line per
  set. These attributes are for Loki's L1 clustering; the design stays valid and equal in function.
- **P-4 Log lines.** INFO at the end, one per run:
  `unmap: 14 adder cells to 2 $add + 1 $sub, 5 multiply to 3 $mul (2 regrouped), 6 RAM cells to 2
  $mem, 1 group left (see unmap_note)`. Counts are cells, not modules.

## 8. Errors (E-1 to E-4)

- **E-1** Bad option: `ODIN3_ERR_INVALID_ARG`, S-4 message.
- **E-2** A registered `adder`, `multiply`, `single_port_ram` or `dual_port_ram` that is not HARD or
  has other ports or parameters than `k_shapes`: located error `unmap: cell type 'multiply' does
  not have the expected ports`, `ODIN3_ERR_INVALID_ARG`, design unchanged.
- **E-3** A needed word cell (`$add`, `$sub`, `$mul`, `$mem`, and for R-8 `$and`, `$not`, `$mux`,
  `$dff`) or constant type (`$_CONST0_`, `$_CONST1_`) is not registered: `unmap: built-in cell
  type '<t>' is missing`, `ODIN3_ERR_INVALID_ARG`.
- **E-4** `ODIN3_ERR_NO_MEMORY` is returned as is. Declined groups are not errors (P-3). No
  design budget check is needed: every recovered cell has fewer pins than the cells it replaces.
  The manager's `check` runs before and after (CLAUDE.md rule 3).

## 9. Tests (T-1 to T-9)

- **T-1 Unit (`tests/unit/test_unmap.c`, Unity, hand-built IR).** Net classes and `unconn`; chain
  discovery (head, no head, branch, tap, cycle, lone head); `$add`/`$sub` recovery with widths,
  trimming, tail signedness, exposed `cout`; multiplier widths and signedness; P1 and P2 on cells
  built by hand to the shapes of section 1, including a nested split and a refused sink; RAM
  `$mem` parameters and ports for single, dual, ROM and unused cases; R-6 with and without a
  shared record; R-7; R-8 on a hand-built glue network; `um_replace` provenance (parents) and
  attribute copying; idempotence (S-7); `--no-regroup`.
- **T-2 Round trip with provenance (CLI test `cli_unmap_rtl`, script
  `tests/cli/test_cli.sh unmap_rtl`).** For every micro of `tests/unmap/micros.txt` (adder, sub,
  signed add and sub, `mult`, `mult_const`, 40x40, 40x8, 8x40 and 80x80 multipliers, signed
  multiplier, `spram`, `dpram`, `rom`, `mem`, `bram`, `1r2w`, `2r1w`, `adder_use` with an explicit
  `adder #(W)`): the Phase 3 script of `tools/hardmap-check/phase3_pre_abc.script` up to `hardmap`,
  then `unmap; opt_clean`, then `opt -full -ffbits; lower; opt -fast; write_blif`, versus the same
  script without `hardmap`/`unmap`; `tools/equiv-check/equiv-check` exit 0. Also: before `lower`
  the count of `$add $sub $mul $mem` cells equals the count before `hardmap` (counted by a
  `write_rtlil` grep in the test), except where the multiplier shape differs by M-7 (listed in
  `tests/unmap/expect.txt`, one reason per line, as `diff_expect.txt` of hardmap: a line that does
  not occur fails). RAM micros may end `unproven` (exit 4) with an entry in the same file. The combinational micros also compare `sim` vectors (same cycles and seed) before `hardmap` and after `unmap`, byte-identical (as `cli_hardmap_add`). The deep-memory micro `tests/unmap/ram_deep.v` (17 address bits, so a depth split of 2 levels) is too large for `equiv-check`; it is `equiv-skip:too-large` in `expect.txt` and checked structurally (one `$mem`, no glue).
- **T-3 Golden (`tests/unmap/expected/*.il`).** `write_rtlil` text after `hardmap; unmap; opt_clean`
  for the adder, sub, 8x7 multiplier, 40x40 multiplier and `spram` micros, byte-compared (the
  `flatten_golden` style). If `write_rtlil` refuses a design that still has declined hard cells,
  those micros are not goldens.
- **T-4 BLIF without provenance (`cli_unmap_blif`).** `read_techlib lib/vtr.o3lib; read_blif X;
  unmap; opt_clean; hardmap; compact; write_blif --vtr out`, then `equiv-check X out` (hard boxes
  are cut and paired by `bbmatch`) and the hard-block counts per model equal (`hardmap-diff` class
  1 logic). X: `tests/golden/blif-vtr/adder_hard_block.parmys.vtr.blif` (Parmys chain with
  `unconn` operands and `dummy_output` heads), `multiply_narrow.vtr.blif`, and the Parmys goldens
  of `~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm` for `common/` micros when the golden
  tree exists (skip 77 without it, as `hardmap_diff_micro`).
- **T-5 Soft check of the adder alone.** `read_blif X; unmap; lower; abc --lut 4; write_blif`
  versus X with `equiv-check` (the `adder` box is expanded to logic by equiv-check, so a chain-only
  BLIF is compared fully).
- **T-6 Ambiguity micros.** Hand BLIFs: a branching carry, a carry tap, two RAM cells with equal
  control and different origin (candidate attribute, no merge), two with one origin (merged).
- **T-7 VTR sweep (heavy, label `sweep`).** `tools/unmap-check/unmap-vtr`: the 19 designs of
  `tests/hardmap/vtr_designs.txt`, smallest first, `hardmap; unmap`, report counts of hard cells
  before and recovered/declined/candidate counts after; every design must finish and no group
  declined with reason `cycle` or `branch` without being listed in `tests/unmap/vtr_expect.txt`.
  Equivalence is not checked for the big designs (Phase 3 exit rule).
- **T-8 Idempotence and determinism.** Two runs equal; `write_rtlil` of two separate sessions equal.
- **T-9 Gates.** `tools/lint.sh` clean, `check` FULL before and after, ASan/UBSan debug build,
  PASSES.md row, `docs/V1.3.md` decision entry.

## 10. Decisions (Peter, 2026-10-21: all good)

Versioning (Peter): core features after v1.0 step the version so the v1.0 base stays clean:
v1.0.1 route-retry fix, v1.1 gate mapping, v1.2 S1 simulation API, v1.3 `unmap`. Each release has its
own log `docs/V1.<n>.md` (checklist + decisions log, as the PHASE files) and release notes, tagged
only on Peter's go. `unmap` is v1.3; question 1's Phase 7 placement is replaced by this.

The recommendations below are the decisions:

1. **Where does `unmap` sit in the phases?** Recommendation: a Phase 7 add-on (item 7.7 "unmap, for
   Loki"), allowed because it needs no core change; log it as an agent default.
2. **Chains with a real carry-in** (a user `adder` whose first `cin` is a signal) are declined in
   v1 (C-3). Option: recover as `$add` with both operands extended by the carry-in at bit 0
   (`{A, c} + {B, c}`), `Y` shifted by one. Recommendation: not in v1 (rare, and the form is not
   what a human wrote); Loki can treat the declined chain as an `adder` primitive.
3. **Read-enable hold logic** of `partial_map` R-10 stays as `$mux`/`$dff` cells next to the
   recovered `$mem` (R-4). Recommendation: leave; absorbing it needs a second pattern and the
   recovered `$mem` is already exact.
4. **Signed `$add`/`$sub` recovery** (C-6) rests on repeated top nets, so a mixed case is
   recovered as unsigned with explicit repeated bits. Recommendation: keep; both forms are exact.
5. **M-7: a split that `partial_map` reduced to `m00`** comes back as a `W x W` `$mul`, not the
   original `a x b`. Recommendation: accept (the function is equal); a Loki-side note is enough.
6. **`--no-regroup`** is the only option. Recommendation: enough; add `--keep-hardmap-attr` only if
   Loki asks.
7. **Depth merge (R-8) in v1 or later?** Recommendation: in v1 but as the last tasks of the plan
   (Tasks 15 and 16); the PR can merge without them if review time is short (the rule is isolated
   in `ram_depth.c`).
