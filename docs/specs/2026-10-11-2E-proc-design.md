# 2E — `proc`: design

Status: draft v1, for review (2026-10-11). Phase 2, sub-project 2E (PHASE2 #1; review: light;
model Opus `xhigh` → `high`). Spec: `docs/DESIGN.md` §5.2, §6 step 3, §12, §15. Inputs: PHASE2
#4 (exit bar), #6 (D1 `full_case`/`parallel_case`, D2 initial values), #13 (the 2D rulings: the
`$proc` cell with `I`/`O` pins and an opaque `BODY` (IR change B), the `processes` pass flag so
that `opt` runs only after `proc`, decision 4 `--nolatches` off by default, decision 13
don't-cares kept explicit); the 2D spec v4 (`docs/specs/2026-10-11-2D-elaboration-design.md`:
EL-33 the cell and its body, EL-34 syncs, EL-35 the process generator, EL-36 switches and case
temporaries, EL-39 memory-write temporaries and the removal rule, EL-40 initial values, EL-45
provenance, §13 `write_rtlil`, §14.3 the bridge, §14.6 x) and its plan (Task 3: body layout,
`odin3_proc_body_pack`/`_view`/`_verify`, `odin3_proc_print`; Task 5: `write_rtlil`; Tasks
21–24: the generator and the bridge `elab-equiv`); `docs/IR.md`; the op registry
(`src/ir/cells/`); 1E (simlib semantics). **Oracle: Yosys 0.55 as pinned by VTR**
(`$VTR_ROOT/libs/EXTERNAL/yosys/passes/proc/*.cc`, `kernel/bitpattern.h`, `kernel/celltypes.h`;
binary `$VTR_ROOT/build/bin/yosys`, `Yosys 0.55 (git sha1 60f126cd0)`, verified 2026-10-11) and
the Parmys script (`vtr_flow/misc/yosys/synthesis.tcl`: `read_verilog -sv -nolatches`, then
`procs -norom`, which is `proc -norom` under Tcl's renaming). The standalone Yosys (0.69+270,
`external/yosys/build/yosys`) is used only for the nightly drift check, never mixed with 0.55 in
one comparison (ORACLES.md). Each decision is numbered **PR-n**; pull requests are named 2E-1 …
2E-4. Where this file and `docs/IR.md` or the 2D spec disagree, they win and this file is
corrected, except **IR change D** (§13: three registry cells), proposed for Peter (§24 item 1).

Every behavioural claim about Yosys below was read in the 0.55 sources and, where marked
"verified", run on 0.55 in `/tmp/claude-1000/2E-probe/` on 2026-10-11.

## 1. Purpose and scope (PR-1)

`proc` replaces every `$proc` node (EL-33) by ordinary cells: decision trees become `$mux` /
`$pmux` trees with `$eq` / `$reduce_or` selects, edge syncs become `$dff` / `$adff` / `$aldff` /
`$dffsr`, combinational syncs become wires or `$dlatch` cells, exactly as Yosys 0.55 `proc
-norom -noopt` does on the same process. It is DESIGN §6 step 3. The front end has already done
blocking/non-blocking substitution, `if`/`case` structure, case temporaries, `full_case` and
`parallel_case` flags, memories and initial values (2D EL-35–EL-40); `proc` judges only what the
body says.

| Delivered by 2E | Where |
|---|---|
| The `proc` pass: per-module schedule, the working form of a process, the decision-tree phases (clean, rmdead, prune, arst), the clock pre-check, mux trees, latches, flip-flops, connections and merges, orphan cleanup, diagnostics, caps | `src/passes/proc/*.c` |
| **IR change D** (proposed, §13): `$dlatch`, `$aldff`, `$dffsr` registry cells with Yosys's ports, parameters and simlib meaning | `src/ir/cells/ff.c`, `builtin.c`, `cells.h` |
| `proc --stop-after clean\|rmdead\|prune\|arst`: re-encodes the remaining processes as `$proc` nodes, for bisecting against Yosys | `src/passes/proc/pw_encode.c` |
| Tools: `proc-diff` (2E against Yosys 0.55 `proc` on the same RTLIL, structural), its `--bisect` mode, `netlist-compare --implicit-models`, `elab-equiv --pass proc` | `tools/proc-check/`, `tools/netlist-compare/`, `tools/elab-check/` |
| Tests: phase unit tests on hand-built processes, ~45 construct rows with goldens in both latch modes, proc-diff and bridge sweeps over the micros and the 64 VTR/Koios files (smallest first), a differential generator, caps, OOM, `bench_proc` | `tests/unit/test_proc_*.c`, `tests/proc/`, `tests/bench/bench_proc.c` |

Not in 2E: `opt` and every x resolution (2F, decision 13); the trailing `opt_expr -keepdc` of
Yosys's `proc` (§24 item 5); `proc_rom` (Parmys runs `-norom`); `fsm`, `memory_infer` (Phase
4); `lower` of the new cells (Phase 3); SystemVerilog `always_ff`/`always_comb`/`always_latch`
checks and `$global_clock` (Phase 5); Yosys's `-global_arst`, `-ifx` and `-nomux` options.

Success: on every construct row, micro and VTR/Koios file that 2D elaborates, `proc-diff`
reports **identical** (2E's cells equal Yosys 0.55's `proc -norom -noopt` output on the same
RTLIL, after both pass through Yosys `opt_clean -purge`; listed exceptions only) and, in the
`--nolatches` mode, the bridge reports **equivalent** to `read_verilog -sv -nolatches`; every
error and warning of §15 has a test with its exact text; §19's targets hold on mcml.v and
lenet.v; the gate passes.

## 2. The pass

### 2.1 Phases and their order (PR-2)

2E runs Yosys's sub-passes in Yosys's order, as internal phases of one pass:

| # | Yosys 0.55 (`proc -norom`, Parmys) | 2E | What it does | § |
|---|---|---|---|---|
| 1 | `proc_clean` | clean | drop empty actions; fold switches on a constant; inline single-case switches; drop empty cases | 5.1 |
| 2 | `proc_rmdead` | rmdead | drop unreachable cases and compare patterns; mark fully covered switches `full_case` | 5.2 |
| 3 | `proc_prune` | prune | drop assignments always overwritten later; turn unconditional root assignments into connections | 5.3 |
| 4 | `proc_init` | — | 2D never emits an init sync: initial values are already `init` attributes (EL-40) | 10 |
| 5 | `proc_arst` | arst | find asynchronous resets; turn their syncs into level syncs with constant values; remove the reset branch | 5.4 |
| — | (in `proc_dff`) | clock pre-check | the user errors `proc_dff` would raise, found before anything is changed | 5.5 |
| 6 | `proc_rom` | — | not run: Parmys passes `-norom` | 20 |
| 7 | `proc_mux` | mux | each decision tree → `$mux`/`$pmux` trees per snippet of assigned bits | 6 |
| 8 | `proc_dlatch` | latch | combinational syncs → connections, or `$dlatch` with a hold condition | 8 |
| 9 | `proc_dff` | ff | edge and level syncs → `$dff`, `$adff`, `$aldff`, `$dffsr` | 9 |
| 10 | `proc_memwr` | — | 2D collects `$mem` at elaboration; write-port temporaries are ordinary combinational outputs | 11 |
| 11 | `proc_clean` | — | nothing is left: the mux phase deletes the `$proc` node | — |
| 12 | `opt_expr -keepdc` | — | 2F (§24 item 5) | — |
| — | — | cleanup | delete the unnamed nets the deleted nodes leave without pins | 12 |

### 2.2 Per-module schedule and atomicity (PR-3)

Modules are independent; 2E visits them in creation order and skips one whose
`odin3_module_proc_count` is 0. Within a module the steps are:

- **A. Analysis (pure: nothing in the IR changes).** For each `$proc` node in node-ID order:
  decode (§4), clean, rmdead, prune (its root promotions are *recorded*, §5.3), arst, clock
  pre-check. User errors are collected (located, §15).
- If any process of the module has an error, the module is **left untouched** (every `$proc`
  node stays), the error count grows, and from then on 2E only analyses: later modules are
  analysed for their diagnostics but not changed. The pass returns `ODIN3_ERR_PARSE` (2D's
  status for rejected input, always with a located diagnostic) after the last module or at the
  error cap. So a module is lowered completely or not at all, and a failed `proc` leaves
  `$proc` nodes that keep every `REFUSE` pass away (EL-33).
- **B. Mux.** For each process: delete its `$proc` node (its `O` nets are now undriven), build
  its mux trees (§6), then apply its merges (§7, point M1).
- **C. Latch.** One latch database for the module, then each process's combinational syncs,
  then the mux fix-up, then the deferred no-latch merges (§8, point M2).
- **D. Flip-flops.** Each process's edge and level syncs (§9).
- **E. Cleanup.** Orphaned nets of the deleted nodes (§12); the working forms are freed.

**Why this equals Yosys's pass-at-a-time order.** Yosys runs each sub-pass over all processes
of a module before the next sub-pass. Step A runs clean … arst for every process before any mux
exists, as Yosys does, and arst reads the module's cells before any are added. In step B a
process's merges join its own temporaries (prune promotions, snippet connections, whose left
side is always a temporary of that process) to a value, so for every other process they are
renames that change no equality between its signals. Step C defers its connections to the end
of the module because Yosys's `proc_dlatch` takes one `SigMap` snapshot for the whole module
before its loop and never updates it (§8). Step D sees every earlier connection, as Yosys's
`ConstEval` (built after `proc_dlatch`) does.

Failure modes: `ODIN3_ERR_NO_MEMORY` and the fatal byte budget (§16) stop at once and may leave
the module being lowered valid but incomplete (undriven nets at worst; IR rule 4 warns only), as
2D's stage 2 does; the design is then fit only for destruction (2D EL-2). `ODIN3_ERR_CHECK` is a
2E bug.

### 2.3 Options, entry points and the `processes` flag (PR-4)

- **Pass:** `proc [--stop-after clean|rmdead|prune|arst]`, a built-in with
  `processes = ODIN3_PASS_PROCESSES_OK` (EL-33), no `wants_ast` (2E reads provenance, never the
  AST). `--stop-after X` runs step A up to and including phase X for every module, then
  re-encodes each process as a new `$proc` node (§17, `odin3_pw_encode`) and applies the prune
  promotions as merges; the design still has processes and `opt` still refuses it. It exists for
  the bisect test (§18.4).
- **Messages:** `proc: unknown option '<opt>'`; `proc: --stop-after needs one of: clean,
  rmdead, prune, arst` (both `INVALID_ARG`, logged by the pass).
- **Info line** after a successful run: `proc: P processes in M modules: D $dff, A $adff, L
  $aldff, S $dffsr, T $dlatch, X $mux, Y $pmux; N ms`; with no processes, `proc: no processes`;
  after `--stop-after X`, `proc: stopped after X; P processes kept`.
- **Postcondition:** `odin3_design_has_processes(design)` is false after a successful run
  without `--stop-after`; a second `proc` is a no-op (`proc: no processes`). The pass manager
  runs `check` before and after it in Debug (1D). `opt` and the other `REFUSE` passes keep
  refusing a design whose `proc` failed (EL-33's `pass 'opt': module 'm' has processes; run
  proc first`).
- **ABI:** nothing new. `proc` runs through `odin3_pass_run`; its cells are read through the
  existing node accessors; structured body accessors stay deferred (§24 item 8).

## 3. The working form (PR-5)

A process is decoded once into a mutable C structure that mirrors Yosys's `RTLIL::Process`; every
phase of step A works on it and nothing else. Declared in `src/passes/proc/pw.h`:

```c
/* A signal bit: a module-local net ID, or a constant (RTLIL's State) when the top bit is set. */
typedef uint32_t odin3_pw_bit;
enum {
    ODIN3_PW_CONST = 1U << 31,
    ODIN3_PW_S0 = ODIN3_PW_CONST | 0U, /* 0 */
    ODIN3_PW_S1 = ODIN3_PW_CONST | 1U, /* 1 */
    ODIN3_PW_SX = ODIN3_PW_CONST | 2U, /* x */
    ODIN3_PW_SZ = ODIN3_PW_CONST | 3U, /* z */
    ODIN3_PW_SA = ODIN3_PW_CONST | 4U, /* '-' (RTLIL Sa): any value; compare patterns only */
    ODIN3_PW_SM = ODIN3_PW_CONST | 5U  /* marked (RTLIL Sm): arst's "not known yet" */
};
typedef struct odin3_pw_sig { uint32_t first, len; } odin3_pw_sig;   /* run in pw->bits */

typedef struct odin3_pw_action { odin3_pw_sig lhs, rhs; odin3_prov_id prov; } odin3_pw_action;
typedef struct odin3_pw_case {
    odin3_vec compares;      /* odin3_pw_sig, each the switch signal's width; empty = default */
    odin3_vec actions;       /* odin3_pw_action, in order */
    odin3_vec switches;      /* uint32_t indices into pw->switches, in order */
    odin3_prov_id prov;
} odin3_pw_case;
typedef struct odin3_pw_switch {
    odin3_pw_sig signal;     /* may become empty (len 0): arst and clean clear it */
    odin3_vec cases;         /* uint32_t indices into pw->cases, in order */
    uint16_t flags;          /* ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE (2D) */
    odin3_prov_id prov;
} odin3_pw_switch;
typedef struct odin3_pw_sync {
    uint8_t type;            /* odin3_sync_type (EL-33); arst turns edges into LEVEL0/LEVEL1 */
    odin3_pw_bit signal;     /* 0 for ALWAYS */
    odin3_vec actions;       /* odin3_pw_action */
} odin3_pw_sync;
typedef struct odin3_pw {
    odin3_node_id node;      /* the $proc node, until step B deletes it */
    odin3_prov_id prov;      /* the node's SOURCE record (EL-45: its ALWAYS) */
    odin3_vec bits;          /* odin3_pw_bit: storage of every sig, append-only */
    odin3_vec cases;         /* odin3_pw_case; [0] is the root; removed cases are unlinked */
    odin3_vec switches;      /* odin3_pw_switch; removed switches are unlinked */
    odin3_vec syncs;         /* odin3_pw_sync, in order */
    odin3_vec promoted;      /* odin3_pw_action: prune's root promotions, in promotion order */
    odin3_vec in_nets;       /* odin3_net_id: the nets of the I and O pins at decode (cleanup) */
} odin3_pw;
```

Signals are runs of `bits`; an operation that changes a signal appends a new run (old runs become
garbage until the working form is freed). The signal operations are Yosys's, in
`src/passes/proc/pw_sig.c`, each O(length) with a per-module scratch index (a `vec` of
`uint32_t` positions indexed by net ID, reset through a touched list):
`odin3_pw_sig_replace` (`SigSpec::replace(pattern, with, &other)`), `_remove2`, `_extract`,
`_equal` (bit by bit, as `SigSpec::operator==`), `_is_fully_const` (no net bits), `_is_fully_def`
(only `S0`/`S1`), `_is_fully_undef` (only `SX`/`SZ`), `_as_bool` (any `S1`), `_sort_unify`
(by the bit key of §4). The tree is a forest of indices, so a removal is an unlink and an inline
(§5.1) is an index splice; no phase recurses (CLAUDE.md): every walk uses an explicit stack of
`(case or switch index, position)` frames.

Module state lives in `odin3_proc_mctx` (`src/passes/proc/proc_internal.h`): the pass context,
the module, the options, the working forms of the module's processes (`odin3_vec` of
`odin3_pw`), the net map (§7), the pending merges, the four shared constant nets (found on first
use: the first live `$_CONST0_/1_/X_/Z_` node in ID order, else created with the module's
provenance, IR-4), the scratch index, the latch database (§8), the statistics and the error
count. It is freed after each module, so 2E's memory is bounded by the largest module's bodies.

## 4. Decoding and the net view (PR-6)

**Decode** (`odin3_pw_decode`): `odin3_proc_body_view` of the node's `BODY` (2D Task 3), then
each position becomes a bit: `IN | k` → the net on `I` pin k, `OUT | k` → the net on `O` pin k,
`ANY` → `ODIN3_PW_SA`; a net whose `odin3_net_const_value` is 0, 1, x or z becomes `S0`, `S1`,
`SX`, `SZ` (IR-4: constants are pins on the shared constant nets, and a wire merged into one
reads the same way). A sync signal may decode to a constant (`always @(posedge 1'b1)`), as in
Yosys. Cases, switches, actions and syncs are copied with their provenance; flags are copied.
Cost O(body).

**Signals are nets, not names.** Yosys's `proc_clean`, `proc_rmdead`, `proc_arst` and `proc_mux`
compare process `SigSpec`s literally, wire name by wire name; only `proc_prune`, `proc_arst`'s
value substitution, `proc_dlatch` and `proc_dff` use a `SigMap`. In Odin III every connection
2D made is already a single net (EL-28 merges), so 2E sees each process the way Yosys would see
it after `opt_clean` had renamed every signal to one name per net. Consequences (each listed in
§20):

- a switch on a wire tied to a constant (`wire k = 1'b1; case (k)`) is folded by clean, where
  Yosys keeps it and its `$eq`s until `opt`;
- an asynchronous reset tested through a renaming wire (`wire r = rst; … if (r)`) is found,
  where Yosys reports `Multiple edge sensitive events found for this signal!`;
- a mux whose two inputs are one net under two names is not built.

So 2E accepts a superset of what Yosys accepts and builds a subset of its cells; where both
accept, the results are equivalent, and `proc-diff` (§18.3) is exact because it feeds Yosys the
RTLIL that `write_rtlil` prints, one name per net. **Parmys is stricter still** (verified): its
pre-`proc` `opt_expr` and `opt_merge` run on modules with processes (`opt_clean` and
`opt_muxtree` skip them), replace cells such as `$eq(rst, 1'1)` by connections, and so hide the
reset from `proc_arst`'s literal driver test. Of the nine micros Parmys rejects with `Multiple
edge sensitive events found for this signal!`, plain Yosys 0.55 `proc` lowers four (verified;
2E must match, §18.6 asserts it): `large/paj_boundtop_hierarchy_no_mem`, `paj_framebuftop_…`,
`paj_raygentop_…` and `syntax/diffeq_f_systemC`; four are real two-clock blocks
(`keywords/always/always_posedge_negedge`, `syntax/bm_function_1`, `bm_function_2`,
`matrix_multiplication`) and fail in all three; `large/paj_top_hierarchy_no_mem` fails in
Yosys and 2E with a non-constant reset value (§5.4). All nine stay Parmys-rejected exceptions
of the exit bar (PHASE2 #4).

**Three views of a signal**, each the one Yosys uses at that point: the **raw** view (decoded
nets, as renamed by merges already applied, §7) in clean, rmdead, prune and mux; the **assign**
view (raw, then the process's own recorded promotions) in arst, as Yosys's `assign_map` sees
every `proc_prune` connection; the **live** view (after every merge so far) in latch and ff.

**Bit order.** Where Yosys sorts bits (`sort_and_unify` in `proc_dff`, `latches_out_in.sort()`
in `proc_dlatch`) it orders by wire (by `IdString` index, which is name-interning order) and
then by offset. 2E orders by `odin3_proc_bit_key(net)` = (primary wire ID, primary bit) for a
net with a primary wire, then (UINT32_MAX, net ID) for one without; constants first, as in
Yosys. Within one wire both orders are the offset order; across wires Yosys's order cannot be
reproduced (it depends on which names were interned first), and it decides only the order in
which registers and latches are created, never their structure (§20).

## 5. Decision-tree phases

### 5.1 clean (PR-7)

`odin3_pw_clean(pw, case_index, max_depth)` is `proc_clean_case` (Yosys `proc_clean.cc`), with
`max_depth` −1 for the whole tree and 1 when arst calls it. Repeated until a sweep changes
nothing. In each case, in order:

1. Remove every action whose `lhs` is empty (2E never decodes one; arst and the ff phase create
   them).
2. For each switch in order: a switch with no cases is unlinked; otherwise `clean_switch`:
   - **Constant signal.** If the signal is non-empty and fully defined (only 0/1): walk the cases
     in order until a default case (no compares) or a match. In each case, a compare that is not
     fully defined (x, z or `-`) is kept and skipped; a fully defined compare equal to the signal
     clears the case's compares and makes it the match; any other fully defined compare is
     removed, and a case left with none (and no match yet) is removed. After a match every later
     case is removed, and if the match is the first case the signal is cleared.
   - **Inline.** If the switch is its parent case's **first** switch, has exactly one case, and
     its signal is empty or that case has no compares: append the case's actions to the parent's
     actions, insert its switches at the front of the parent's switches, unlink the switch.
   - **Otherwise** recurse (depth permitting) into each case, then remove empty cases (no actions,
     no switches): under `FULL_CASE` only when *all* are empty (then all go); under
     `PARALLEL_CASE` without `FULL_CASE`, every empty case; otherwise only trailing empty cases.
3. A sync action whose `lhs` is empty is removed; a sync with no actions is removed.

The "first switch" condition keeps order: appended actions run before every later switch only
when no switch precedes the inlined one. Cost O(body) per sweep; every change removes or merges
an object, so sweeps ≤ objects + 1 (1–3 in practice). Recorded counts feed only the statistics.

### 5.2 rmdead (PR-8)

`odin3_pw_rmdead` is `proc_rmdead` on every switch of the root case, depth first, cases in order.
For each switch a pool of still-reachable values is built:

- **Fully defined pool** (`odin3_fdpool`) when the signal has no constant bits and every compare
  is fully defined: a set of the values taken so far (hashed in a `u64map`, each value stored
  once) plus `default_reached`; empty when the default was reached or `2^width` values were
  taken (width < 32).
- **Bit-pattern pool** (`odin3_ppool`, Yosys's `BitPatternPool`) otherwise: a deduplicated set of
  patterns over {0, 1, `-`}, two bits per position packed into `uint64_t` words, starting from the
  signal (its 0/1 bits fixed, every other bit `-`). `take(p)` (x and z in `p` read as `-`)
  removes every pattern `p` matches and inserts, for each position where the removed pattern has
  `-` and `p` has 0 or 1, the pattern with that position set to the opposite value; `has_all(p)`
  is true if some pattern matches `p` and has `-` wherever `p` has `-` (it holds every value `p`
  covers); `take_all` empties it.

Per case, in order: the case is a **default** if it has no compares and the pool is not empty
(or the signal is empty); each fully constant compare (no net bits) that the pool does not take
is removed; a non-default case left without compares is removed (dead); the case's own switches
are processed; a default case then takes all. After the last case, a switch whose pool is empty
gets `FULL_CASE` (so a `case` that lists every value marks itself full, and a default after
complete coverage is removed as dead).

A pool past `ODIN3_PROC_MAX_PATTERN_BITS` (§16) stops this switch's analysis: its remaining cases
and compares are kept, `FULL_CASE` is not set, and the warning of §15 (W5a) is given once for
the switch. The result is equivalent (dead cases cost cells, never meaning). Cost: O(compares ×
width) for the defined pool; the pattern pool is O(patterns × width) per operation and can grow
with the number of overlapping wildcard items, hence the cap.

### 5.3 prune (PR-9)

`odin3_pw_prune` is `proc_prune`'s `do_case`/`do_switch` on the root with `root = true`:

- `do_case(c, assigned)`: first the case's switches **in reverse order**, each adding to
  `assigned` what it assigns on every path; then its actions **in reverse order**: for each `lhs`
  bit (raw view) not yet in `assigned`, the bit is kept; at the root, if it is not in `affected`
  either, it is **promoted** instead; every kept or promoted bit joins `assigned` and `affected`.
  An action left with no kept bits is removed; one with some keeps only those. Promoted bits form
  one promotion (`lhs ← rhs`, in bit order) appended to `pw->promoted`, and are removed from the
  action.
- `do_switch(s, assigned)`: each case runs with a copy of `assigned`; the bits newly assigned in
  *every* case are added to the caller's set when the switch is `FULL_CASE` or has a default
  case.
- `affected` is shared by the whole walk: a bit assigned anywhere later in program order is never
  promoted.

A promotion is a connection (Yosys `module->connect`): it is recorded (§7) and applied as an IR
merge only in step B (or by `--stop-after`), and it is visible to arst through the assign view.
In 2D's bodies the typical promotion is a clocked block's root `t0(q) = d`, which is how a
register gets `D = d` without a mux (verified). Implementation: per-net mark arrays with an undo
log instead of Yosys's copied sets (`assigned` copies become save/restore marks; the "every case"
intersection is a per-net counter of cases), so the cost is O(body + Σ lhs bits), not O(depth ×
bits).

### 5.4 Asynchronous resets: arst (PR-10)

`odin3_pw_arst` is `proc_arst` without `-global_arst`. Syncs are split into level
(`LEVEL0`/`LEVEL1`), edge (`POSEDGE`/`NEGEDGE`) and other (`ALWAYS`).

**`check_signal(s, ref, polarity)`** decides whether a 1-bit switch signal `s` is the sync signal
`ref` seen through simple logic, and flips `polarity` on each inversion. It follows drivers
(`odin3_net_driver`, O(1), where Yosys scans every cell of the module for each test — the reason
its `proc_arst` takes 15.7 s on lenet.v, §19):

| `s` is driven by (a 1-bit `Y` port) | then |
|---|---|
| — (`s` is `ref`) | true |
| `$reduce_or`, `$reduce_bool` | continue with `A` |
| `$logic_not`, `$not` | flip, continue with `A` |
| `$eq` with a fully constant `A` (or `B`) | flip unless that constant has a 1 bit; continue with the other port |
| `$ne` with a fully constant `A` (or `B`) | flip if that constant has a 1 bit; continue with the other port |
| anything else, or `s` wider than 1 bit | false |

(Yosys also follows `$eqx`/`$nex`, which 2D never emits: EL-30 lowers `===` to `$eq`.) The walk
stops as "false" after `ODIN3_PROC_MAX_SIGNAL_WALK` hops, which only a combinational cycle of
inverters reaches (Yosys recurses until its stack overflows there).

**The loop.** While the root case has exactly one switch: try each edge sync in order with
`polarity` = (the sync is `POSEDGE`). On the first whose `check_signal` holds:

- **More than one edge sync: an asynchronous reset.** The sync becomes `LEVEL1` (from
  `POSEDGE`) or `LEVEL0` (from `NEGEDGE`) and moves to the level list. Each of its actions gets
  `rhs = apply_reset(rhs)`. Then `eliminate_const(root, polarity)`.
- **One edge sync: a "VHDL-style" edge test** (`always @(posedge c) if (c) …`):
  `eliminate_const(root, !polarity)`.

Then loop (a second reset nested in the `else` of the first is found on the next turn). If
anything was found, the syncs are reordered: level, then edge, then other. The level syncs'
order is their priority in the ff phase (first = highest).

- **`apply_reset(sig)`** computes the value the reset forces: `rspec` = the assign view of `sig`;
  `rval` starts all `SM` except `rspec`'s constant bits. Repeat: apply the root case to it
  (`apply_const`), map `rval` through the assign view, stop when fully constant or unchanged,
  else `rspec = rval`; after `ODIN3_PROC_MAX_ARST_ROUNDS` (100, Yosys's) rounds it is error E3.
  `apply_const(case)`: every action replaces matching `rspec` bits in `rval` (by `rhs`, or by
  `SM` when the case is "unknown"); then for each switch: one with an empty signal descends into
  every case with the current flag and then, because an empty signal never passes
  `check_signal`, once more as unknown (Yosys's fall-through, kept); one whose signal passes
  `check_signal` against the reset descends, as known, only into the first case with a compare
  equal to the 1-bit `polarity` value, or the first default; any other switch descends into
  every case as unknown. `SM` left in the result is error E2. The
  result may be a non-constant signal without `SM` (a register not reset by the block, whose root
  default `t0(q) = q` survives prune): the ff phase handles it (§9).
- **`eliminate_const(case, polarity)`**: in each switch of the case whose signal passes
  `check_signal`, the cases that match `polarity` are removed, the first other case keeps its body
  with its compares cleared, every case after it is removed, and the signal is cleared; switches
  that do not pass are descended into. Then the case is cleaned with depth 1 (§5.1) until stable,
  which inlines the surviving branch.

| Source (2D bodies, EL-34/EL-36) | Found | Sync after arst | Cells (§9) |
|---|---|---|---|
| `@(posedge clk or posedge rst) if (rst) q <= 0; else q <= d;` | yes | `LEVEL1 rst` | `$adff` (`ARST` = `$reduce_or(rst)`, polarity 1) — verified |
| `@(negedge clk or negedge rst_n) if (!rst_n) …` | yes (`$logic_not`) | `LEVEL0 rst_n` | `$not(rst_n)` → `$reduce_or` → `$adff`, polarity 1, `CLK_POLARITY` 0 — verified |
| `if (rst == 1'b1)`, `if (rst_n == 0)`, `if (~rst_n)` | yes | as above | as above |
| `if (r1) … else if (!r2) … else …` with three events | both | `LEVEL1 r1`, `LEVEL0 r2` | `$dffsr` when the two values differ — verified |
| a statement before the `if` that adds a root switch, or a reset that is not the root switch | no | two edges remain | error E1 (§5.5) |
| a register assigned outside the reset `if` and promoted by prune (`paj_top`'s `dataout <= data;`) | yes | — | error E2 — verified (Yosys: `Async reset \we yields non-constant value 1'm`) |

Memory writes cannot occur in a block with an asynchronous reset: such arrays are registers
(2D EL-27 rule 2). 2E still checks it (E4, §11). Cost: O(chain) per `check_signal`; `apply_reset`
O(rounds × body) per sync action (1–2 rounds in practice); `eliminate_const` O(body).

### 5.5 Clock pre-check: edges, dual edges, several clocks (PR-11)

After arst, `odin3_pw_check_clocks` raises, before anything changes, every error the ff phase
would raise (Yosys raises them in `proc_dff`, after the muxes are built):

- a bit assigned by two different edge syncs is **E1**: same net, opposite edges
  (`always @(posedge clk or negedge clk)`) → E1b, "dual-edge registers are not supported";
  different nets → E1a, naming both;
- a bit assigned only by level syncs (possible only for a body that arrives with level syncs:
  arst always leaves one edge sync) → E5.

So: one clock per register, `POSEDGE` → `CLK_POLARITY` 1, `NEGEDGE` → 0; any further edge
events of the block must be resets found by arst. Different blocks may use different clocks (the
`$dff`s simply carry different `CLK` nets; multi-clock netlists are Phase 3's). A block clocked
by a net it assigns (`always @(posedge a) a <= ~a;`, EL-33 re-review N1) is a `$dff` whose `CLK`
is its own `Q`, as in Yosys. A constant clock is a `$dff` with `CLK` on a constant net, as in
Yosys. Yosys's other `proc_dff` errors cannot arise from a 2D body and are not mirrored: `Mixed
always event with edge and/or level sensitive events!` (the latch phase empties every `ALWAYS`
sync first), `Event with any-edge sensitivity` and `$global_clock` (SystemVerilog). Cost
O(sync actions).

## 6. Mux trees (PR-12)

`odin3_proc_mux` is `proc_mux` without `-ifx`, for one process, after its `$proc` node is
deleted. All signals are raw (§4).

**Snippets.** Every action `lhs` in the case tree (root first, then each switch's cases, depth
first) is inserted into `odin3_proc_snips`: a partition of the assigned bits into runs, split
where an `lhs` overlaps an existing run partly (Yosys `SigSnippets::insert`, made iterative with
a worklist; identical runs are stored once). Snippets are processed in creation order (Yosys
iterates a hash pool; the order changes only creation order). A **switch cache** records, per
switch, which snippets any action below it assigns (a `u64map` keyed `switch << 32 | snippet`),
so a snippet visits only switches that can change it.

**One tree per snippet** `sig`, from the root with default value x
(`signal_to_mux_tree(case, sig, defval)`, an explicit stack of case and switch frames):

1. `result = defval`; each action of the case, in order, replaces the bits of `result` where its
   `lhs` meets `sig` (later actions win).
2. For each switch of the case that the cache says touches `sig`:
   - **Parallel groups.** If the switch is `PARALLEL_CASE`, all cases are one group. Else, if
     every compare of every case is fully defined and no value repeats ("simple parallel",
     computed once per switch), all cases are one group. Else groups are formed in case order
     with a fresh pattern pool per group: a case starts a new group when the previous case had a
     non-constant compare, when one of its own compares is not fully constant or not `has_all`
     in the pool, or when it is the default; each case's constant compares are then taken into
     the pool.
   - **`full_case` mask.** If the switch is `FULL_CASE`, the bits assigned in *every* case
     (directly or by a nested `FULL_CASE` switch; memoised per switch) read x in `result`, since
     some case always overrides them.
   - **Cases in reverse order** (the first case gets top priority): `value` = the case's own
     tree with default `initial` (the masked `result`). If a mux is open and this case is in the
     same group as the case after it, **append** `(value, select)` to that mux unless `value`
     equals its `A`; otherwise **gen_mux**: no compares (the default) or `value` equal to
     `result` → `result = value`, no cell; else a select (below) and a new open mux `$mux(A =
     result, B = value, S = select)`, `result` = its output. A group of k appended cases becomes
     one `$pmux` (B slices and S bits in append order; A from the first); one entry stays `$mux`.
3. Return `result`.

**Selects** (`gen_cmp`): for each compare of the case, the bits where it is `-` are dropped from
both the compare and the switch signal; a compare that is then empty makes the case always taken
(no select, no mux); a 1-bit signal against constant 1 is that bit itself (no cell, so an `if`
costs no `$eq`); anything else is `$eq(A = signal bits, B = compare bits, A_SIGNED = B_SIGNED =
0, Y_WIDTH = 1)`, where an x compare bit stays a pin on the x constant net and a non-constant
item (2D C8) is its nets. Several compares are ORed by `$reduce_or` (`A_WIDTH` = their count).
Yosys's quirks are kept on purpose, so that `proc-diff` is exact: an `$eq` made before a later
compare turned out empty stays (dead, removed by `opt`); a mux stays open across a group
boundary when the boundary case built no cell, so the next case appends to it; an all-`-`
compare in an append (Yosys asserts) cannot follow rmdead and is an internal error.

**Emission.** Muxes are records until the process's trees are done (so an append never resizes a
node); then each becomes one `odin3_node_create_connected` (`$mux` with `WIDTH`, or `$pmux` with
`WIDTH`, `S_WIDTH`). Every cell this phase creates drives fresh nets; each snippet's final value
is a **connection** `sig ← value` (Yosys `module->connect`), applied with the process's
promotions at merge point M1 (§7). Constant inputs are pins on the shared constant nets.

| Switch (2D, EL-36) | Cells per snippet |
|---|---|
| `if (c) a else b` | one `$mux(A = b, B = a, S = c)`, no `$eq` (verified) |
| `if` without `else`, clocked | `$mux(A = q, B = d, S = en)`: the root default `t0 = q` is A |
| `case` with distinct constant items (simple parallel) | one `$pmux`: one `$eq` per item value, a `$reduce_or` per multi-value item, A = default (verified) |
| `casez`/`casex`, overlapping or non-constant items | a priority chain of `$mux`/`$pmux` stages, one per group; `-` bits omitted from each `$eq` |
| `parallel_case` | one `$pmux` whatever the overlap (Yosys `$pmux` semantics; Odin III's simulator ORs, 1E) |
| `full_case`, or every value listed (rmdead marks it) | as above, with x for the bits every item assigns, so the default leg is x (2D adds an x default under `full_case`, EL-36) |
| case items that assign the same value as the default | no mux stage for them (`value == result`) |

Cost: O(cells created + Σ over snippets of the cases on their paths); the pattern pools of the
grouping step are capped as in §5.2 (past the cap the remaining cases start new groups, W5b,
equivalent).

## 7. Connections, merges and the net map (PR-13)

Connections come from prune promotions (§5.3), snippet values (§6) and no-latch outputs (§8).
`odin3_proc_connect(m, lhs, rhs)` records one per bit; `odin3_proc_apply_merges` turns them into
IR merges (`odin3_net_merge`, IR-15) at two points only:

- **M1**, at the end of each process's mux step: its promotions, then its snippet connections;
- **M2**, at the end of the module's latch step: the no-latch connections, after the mux
  fix-up.

(`--stop-after` applies the promotions after re-encoding.) **Net map** (`odin3_proc_netmap`):
a union-find over net IDs with path compression, stored as a `vec` of `odin3_pw_bit` indexed by
net ID (grown as nets are created), where a class may also hold one constant. Every net a working
form stores is resolved through it at use (`odin3_proc_find`), so merges never invalidate a
working form. A merge resolves both sides; equal → nothing (a self-connection, e.g. `y ← y`); a
constant side → the bit is merged into that shared constant net (created on first use); else
`keep` is chosen by name, never by driver (pins move either way): the net whose primary wire is
a port, else the one with a primary wire, else the lower ID. Invariant, asserted in Debug: at
most one net of a class has a driver (every `lhs` of a connection is a temporary or output of a
deleted `$proc`, so it is undriven); a violation is `ODIN3_ERR_CHECK`.

Merges keep every name (IR-2), so `write_rtlil` prints the user's names where Yosys's `opt_clean`
would. The cost of a merge is O(pins of `drop`).

## 8. Latches (PR-14)

`odin3_proc_latches(m)` is `proc_dlatch` for a whole module, after step B.

**Database** (`odin3_proc_ldb`), built once, from the IR as it stands (Yosys builds it after
`proc_mux` and never refreshes its `SigMap`): `mux_drv[net]` = (node, bit) for each `Y` bit of a
live `$mux`/`$pmux` (2D's `?:` muxes included); `mux_src[node]` = a **snapshot** of the distinct
non-constant nets on its `A` and `B`; `users[net]` = the number of reading pins (sinks and
`inout`s of non-port nodes) plus one when a `$port_in`/`$port_inout` drives it (Yosys counts
input-port wires, not output ports); a hash-consed rule table (`odin3_proc_rule {odin3_pw_bit
signal, match; uint32_t first_child, n_children;}`, IDs from 3; 1 = true, 2 = false), a memo
from rule to hold net, the generated latches and the deferred connections.

Per process (node-ID order), per `ALWAYS` sync action `lhs ← rhs` (live view):

1. **quickcheck(rhs, lhs)**: breadth-first from the `rhs` nets through `mux_drv` and the
   `mux_src` snapshots; true when a source net is one of `lhs`. False → no latch for the action.
2. Otherwise each bit becomes a candidate `out ← in`; candidates are sorted by the bit key (§4).
3. **find_mux_feedback(in, out, set_undef = true)**, exactly Yosys's walk, as an explicit
   stack in Yosys's visiting order (A first, then the B slices in order): `set_undef` turns false
   below any net with `users > 1`; `in == out` → true; no mux driver → false; at a mux bit, the
   `A` branch's result (when not false) becomes, for each select bit, an inner rule `(s, 0,
   child)`, and with `set_undef` an `A` pin on `out` is moved to the x constant net; each `B`
   slice j's result becomes `(s_j, 1, child)` and likewise moves a `B` pin on `out` to x; no
   children → false; else an "or" rule over the sorted children. Ports are read live (so a pin
   moved earlier reads x), the snapshot is used only by quickcheck: both as in Yosys. A walk past
   `ODIN3_PROC_MAX_FEEDBACK_VISITS` is error E6.
4. False → no latch for that bit; else a latch bit with hold rule n.
5. **No-latch bits** are recorded connections `out ← in` (merged at M2); their `init` bits are
   removed (§10).
6. **Latch bits**, in key order, are grouped while the next bit has the same wire and the same
   rule; each group is one `$dlatch(EN = $not(hold(n)), D = in bits, Q = out bits, EN_POLARITY
   1)` and warning W1. `hold(n)` builds, memoised per rule: true → constant 1; false → 0; a rule
   whose signal differs from its match gives a term, the signal for `match` 1 and `$not(signal)`
   for `match` 0 (the "or" rule, signal = match = 0, gives none); children give `$reduce_or` of
   their holds (even of one); two terms give their `$and`.

**Fix-up** (`fixup_muxes`): from every generated `$dlatch`, breadth-first upstream through
drivers of input nets, visiting only Yosys's "internal" combinational types (in the registry:
`$not $neg $and $or $xor $xnor $reduce_* $logic_* $shl $shr $sshr $shiftx $lt $le $eq $ne $ge
$gt $add $sub $mul $div $mod $pow $mux $pmux $tribuf $sop`), each `$mux`/`$pmux` is rewritten:
`B` slices that are all x/z are dropped with their select bits; none left → `B = A`, `S` =
constant 0; `A` all x/z with some valid slice → `A` = the last valid slice; one select → `$mux`,
more → `$pmux`. A changed mux is deleted and re-created on the same `Y` nets, keeping its
provenance and attributes (IR-12: a rewiring keeps records). Then M2.

Verified example (default mode): `always @* if (en) y = a;` gives `$mux(A = a, B = a, S = en)`
(the `y` input moved to x, then `A` := the valid `B` slice), `$dlatch(D = mux, Q = y, EN =
$not($reduce_or($and($not(en), $reduce_or(1'1)))))` — the cells Yosys builds, all simplified by
`opt` later.

**`--nolatches`.** It stays a `read_verilog` option (2D decision 4). With it, 2D gives an
unassigned path x and no root hold action (EL-35 step 3), so a combinational process has no
feedback and gets no latch unless the user wrote one through other logic. Without it (the
default), every path that does not assign an output holds it and becomes a latch with W1. `proc`
has no option of its own: whether a block holds is the front end's statement.

Cost: database O(module pins); quickcheck O(mux cone) per action (Yosys's own bound: chains of
combinational processes can make it O(actions × muxes); 9.0 s of Yosys's 34.6 s on lenet.v);
find_mux_feedback O(cone) per latch bit on the tree-shaped cones `proc` builds, capped on shared
cones.

## 9. Flip-flops (PR-15)

`odin3_proc_ffs(m, pw)` is `proc_dff`, after the latch step (live view throughout).

**Groups** (`find_any_lvalue`): take the first action `lhs` of the first sync that has one,
sorted and deduplicated; for each sync, intersect it with that sync's assigned bits when the
intersection is not empty. That group is one register; its bits are removed from every action;
repeat until no sync assigns anything. With 2D's bodies (one sync action per output wire chunk,
§23) a group is one wire's chunk, as in Yosys.

**Per group** `sig`, from each sync action that meets it: level syncs give `(value, sync)` rules
in sync order; the edge sync gives `insig` (bits it does not assign stay z). Then:

1. **Collapse.** When there are rules and all their values are equal, they become one `LEVEL1`
   rule whose signal is `$reduce_or` of the triggers (`signal` for `LEVEL1`, `$not(signal)` for
   `LEVEL0`) — so even one reset gets a `$reduce_or`, and `ARST_POLARITY` is always 1 (verified).
2. **Self reset.** One rule whose value is `sig` itself (a register the reset does not touch):
   `insig = $mux(A = insig, B = sig, S = trigger)`, and the rule is dropped (verified).
3. **Several rules with different values: `$dffsr`** (warning W4). From the last rule to the
   first: `set = $mux(set, value, trig)`, `clr = $mux(clr, $not(value), trig)` starting from
   constant 0, where `trig` is the signal or its `$not`; then `$dffsr(CLK, SET = set, CLR = clr, D
   = insig, Q = sig)`, `CLK_POLARITY` from the edge, `SET_POLARITY` = `CLR_POLARITY` = 1. (The
   `$not` of a constant value is built as Yosys builds it.)
4. **One rule with a value that is not fully constant: `$aldff`** (warning W3): `$aldff(CLK,
   ALOAD = trigger, AD = value, D = insig, Q = sig)`, `ALOAD_POLARITY` 1. Yosys first tries
   `ConstEval` on the value; 2E counts only constant nets (2D folds constant cells, EL-30, so the
   two agree on 2D's output; §20).
5. **Otherwise** `$adff(CLK, ARST = trigger, D, Q, ARST_VALUE = the constant, ARST_POLARITY 1)`
   when there is a rule, else `$dff(CLK, D, Q)`; `WIDTH` = group width, `CLK_POLARITY` 1 for
   `POSEDGE`, 0 for `NEGEDGE`.

Registers drive the group's own output nets (undriven since the node was deleted), so no merge is
needed. The cell gets the `$proc` node's attributes (Yosys copies `proc->attributes`). Cost
O(groups × sync actions) per process, Yosys's bound; groups ≈ output wires.

## 10. Initial values (PR-16)

- **Representation.** An initial value stays where 2D puts it (EL-40): the `init` attribute (BITS,
  x where unknown) on the output wire, Yosys's convention, which `write_rtlil` prints and Yosys's
  `opt_dff`/`write_blif` read. 2E adds no `INIT` parameter to `$dff`-family cells, which mirror
  simlib (§24 item 4); a register's `Q` drives the wire's nets, so the attribute stays attached
  to the register's output. `lower` (Phase 3) moves it to the bit-level `$_DFF_*`'s `INIT`, which
  the BLIF writer prints as `.latch` init.
- **No `proc_init`.** 2D runs `initial` blocks through its interpreter and never emits an init
  sync (EL-33's sync types have none), so there is nothing to extract.
- **Combinational outputs** (no-latch bits, §8 step 5): as Yosys, each `(wire, bit)` membership of
  the net loses its `init` bit (set to x; the attribute is removed when all x), with warning W2
  once per wire per process when a non-x bit was removed. Latch and register outputs keep it.
- **`-global_arst`** (initial values as an asynchronous reset) is not provided: Parmys does not
  use it.

## 11. Memory writes (PR-17)

2D collects every memory into one `$mem` cell at elaboration (EL-27) and wires each write port's
`WR_ADDR`/`WR_DATA`/`WR_EN` to process temporaries that the block assigns and no sync action
updates (EL-39, v4: `addr = x`, `data = x`, `en = 0` at the block's start, so every path assigns
them). To 2E these are ordinary snippets: the mux phase drives them with mux trees, and since no
sync action names them, the latch phase never sees them. There is no `$memwr` cell and no
`proc_memwr`. Yosys's `proc_memwr` gates a write enable with the block's asynchronous resets and
`proc_arst` rejects a reset that would enable a write; 2D makes every array written in a block
with an asynchronous reset a set of registers (EL-27 rule 2), so neither can arise. 2E checks it
anyway: a process with a level sync after arst whose combinational outputs reach a `$mem` `WR_*`
pin is error E4.

## 12. Dead and unused outputs (PR-18)

- **Dead structure** goes as in Yosys: rmdead removes unreachable cases, clean removes empty
  actions, cases and switches, prune removes overwritten assignments.
- **Unused outputs are lowered like any other**, as in Yosys: T7's `t` in `t = a & b; r <= t;`
  gets a `$dff` with an unread `Q`, a loop counter read elsewhere gets its register, a write-port
  temporary whose memory is never read keeps its mux tree. Removing dead cells is `opt`'s (2F's
  `opt_clean`); doing it here would make `proc-diff` inexact and duplicate 2F (§24 item 7).
- **Orphan nets.** After step D, every net that was on a pin of a deleted `$proc` node and is now
  live, pinless, unnamed, without a primary wire and without aliases is deleted
  (`odin3_net_delete`), so `check` does not warn about nets 2E stranded (typically a temporary
  read only in a pruned branch). Wire bits are never deleted.

## 13. IR change D: three registry cells (PR-19)

`proc_dff` and `proc_dlatch` emit three Yosys cells the registry lacks (`$dff $dffe $adff $sdff`
exist, `ff.c`). Proposed (§24 item 1): add them with simlib's ports, parameters and meaning, word
granularity, flags 0 like the existing word flip-flops, `simulate` NULL (1E simulates the netlist
view only), appended after 2D's `$proc` (2D point (f): built-in IDs 0–62 do not move; the count
becomes 66).

| Cell | Ports (order) | Parameters | Meaning (simlib) |
|---|---|---|---|
| `$dlatch` | `EN` (1), `D` (`WIDTH`), `Q` (`WIDTH`) | `WIDTH` ≥ 1, `EN_POLARITY` 0/1 | `Q` follows `D` while `EN == EN_POLARITY`, else holds |
| `$aldff` | `CLK` (1), `ALOAD` (1), `AD` (`WIDTH`), `D` (`WIDTH`), `Q` (`WIDTH`) | `WIDTH`, `CLK_POLARITY`, `ALOAD_POLARITY` | while `ALOAD` is active `Q = AD`; else `Q = D` at the clock edge |
| `$dffsr` | `CLK` (1), `SET` (`WIDTH`), `CLR` (`WIDTH`), `D`, `Q` (`WIDTH`) | `WIDTH`, `CLK_POLARITY`, `SET_POLARITY`, `CLR_POLARITY` | per bit: `CLR` active → 0, else `SET` active → 1, else `D` at the edge |

Polarities are INT 0/1 as on the existing `$dff` family (Yosys stores 1-bit constants; the
comparison tools normalise, §18.3). `verify` checks the INT ranges as `ff.c` does. Writers:
`write_rtlil` prints them generically (cell, parameters, connections); `write_verilog` gets a
refusal row for each (`module 'm': cell '<name>' of type '$dlatch' has no structural Verilog
form`, 2D Task 1's pattern) until a later phase writes them; the netlist-view writers already
refuse word cells. Consumers to come: 2F's `opt_dff`-style rules and Phase 3's `lower` (Parmys
maps `$adff`/`$aldff` to `$dff` with `techmap` before its own passes).

*Alternatives:* (a) lower `$aldff` and `$dffsr` in 2E into `$dff` plus logic (no registry
change, but `proc-diff` could not be exact and 2E would do Phase 3's job early); (b) reject
non-constant and multi-valued resets (Parmys accepts both). Neither is recommended.

## 14. Provenance and attributes (PR-20)

- **One operation per process** (`odin3_prov_begin_op`), since lowering a process is one
  decomposition (IR-12). Records (`odin3_prov_derive`, hash-consed, IR-13):
  - a `$mux`/`$pmux` and the `$eq`/`$reduce_or` cells of its selects: parents = [the switch's
    record, then the record of each case it selects, in case order];
  - a register, latch, and the helper cells of §8 and §9 (`$not`, `$and`, `$reduce_or`, `$mux`):
    parents = [the `$proc` node's record];
  - a body element without a record (provenance 0) falls back to the node's record.
  Every net a cell creates takes its cell's record. So "what did this `always` become" is the
  forward index from its SOURCE record, and each mux points back to its `case` item.
- **Merges** create no records (names are kept, IR-2). The deleted `$proc` node's tombstone keeps
  its `BODY` after `compact` (EL-33, IR-6).
- **Attributes**: registers and latches copy the `$proc` node's attributes (the `always`'s `(*
  *)`, EL-45), as Yosys copies `proc->attributes`; muxes and their selects get `full_case` and
  `parallel_case` (INT 1) from the switch flags, including a `full_case` rmdead set, as Yosys's
  `apply_attrs` copies the switch's attributes. A re-created mux (§8) keeps its attributes.

## 15. Diagnostics (PR-21)

Through `odin3_diag` (2A §3.4): `file:line:col: error|warning: message` at the first location of
the record named, lower case, names in `'…'`, no final period (2B–2D style). A record with no
location (hand-built tests) prints `odin3: <level>: proc: module 'm': <message>`. **Names**
(`odin3_proc_name_bits`): a group covering a whole wire is `'q'`; a contiguous part is
`'q[7:4]'` in declared indices (EL-23); otherwise the first bit, `'q[3]'`; a net without a wire
uses its first alias, else `'$n<ID>'`. A sync signal is named the same way.

**Errors** (each at the `$proc` node's record, the `always`, unless stated; the module is not
lowered, PR-3):

| # | When | Message |
|---|---|---|
| E1a | a bit assigned under two edge events of different nets (§5.5) | `'q' is clocked by both 'a' and 'b'; a second edge event must be an asynchronous reset tested by the block's only top-level 'if'` |
| E1b | the same net, both edges | `'q' is clocked by both edges of 'clk'; dual-edge registers are not supported` |
| E2 | `SM` left by `apply_reset` (§5.4) | `asynchronous reset 'rst' does not give 'q' a constant value` |
| E3 | `apply_reset` past 100 rounds | `asynchronous reset 'rst' does not settle the value of 'q' after 100 substitutions` |
| E4 | a memory write under an asynchronous reset (§11) | `memory 'm' is written in an always block with asynchronous reset 'rst'` |
| E5 | a bit under level events only (§5.5) | `'q' is assigned only under level events; a register needs a clock edge` |
| E6 | latch walk past its cap (§8) | `latch analysis of 'y' visits more than 1048576 multiplexer inputs` |
| E7 | the byte budget (§16), fatal | `proc needs more than N bytes (the budget is 60% of physical memory)` |
| E8 | the 20th error | `too many errors; proc stopped after 20` |

**Warnings** (each once per object named):

| # | When | Message |
|---|---|---|
| W1 | each `$dlatch` (§8) | `latch inferred for 'y': the always block does not assign it on every path` |
| W2 | `init` removed from a combinational output (§10) | `initial value of 'y' is ignored: 'y' is assigned in a combinational always block` |
| W3 | `$aldff` (§9) | `'q' takes a non-constant value while asynchronous reset 'rst' is active; it is built as an asynchronous-load register ($aldff)` |
| W4 | `$dffsr` (§9) | `'q' has asynchronous resets with different values ('r1', 'r2'); it is built as a set/reset register ($dffsr)` (more than three resets: the first three, then `…`) |
| W5a | rmdead pool cap, at the switch's record | `case analysis needs more than 16777216 pattern bits; no more dead cases are removed from this case statement` |
| W5b | mux grouping pool cap, at the switch's record | `case analysis needs more than 16777216 pattern bits; the remaining items of this case statement are not grouped into a $pmux` |

Yosys gives none of W1, W2 as warnings (it logs `Latch inferred for signal …` and `Removing init
bit …`); W3 and W4 match its `Async reset value … is not constant!` and `Complex async reset for
dff …`. D1's `full_case`/`parallel_case` warnings are 2D's (EL-36). **Infos**: the summary line
of §2.3.

## 16. Caps and out-of-memory behaviour (PR-22)

Named constants in `src/passes/proc/proc.h`, checked before the work they bound, lowered in tests
through `odin3_proc_test_set_limits` (`proc_test.h`, as 2A–2D do):

| Cap | Value | Why / on reaching it |
|---|---|---|
| `ODIN3_PROC_MAX_ERRORS` | 20 | as 2C/2D; E8 |
| `ODIN3_PROC_MAX_PATTERN_BITS` | 2^24 per pattern pool | `BitPatternPool` can grow with overlapping wildcard items; ≤ 4 MB per pool; past it W5a/W5b and an equivalent, coarser result |
| `ODIN3_PROC_MAX_ARST_ROUNDS` | 100 | Yosys's limit; E3 |
| `ODIN3_PROC_MAX_SIGNAL_WALK` | 1024 driver hops | only a cycle of inverters reaches it; `check_signal` answers false |
| `ODIN3_PROC_MAX_FEEDBACK_VISITS` | 2^20 per latch bit | find_mux_feedback is exponential on shared cones (Yosys too); E6 |
| `ODIN3_PROC_MAX_BYTES` | 60% of physical memory (2D's formula) | one run-level budget, checked at each process boundary and every 2^16 created pins against the running estimate (48 B per node + parameters, 28 B per pin, 48 B per net, the working forms' vectors); E7, fatal (memory rule: a host OOM kills the WSL VM) |

**No mux-depth or case-arm cap.** Every algorithm is iterative, so depth threatens no stack;
the body's size is already bounded by 2D's caps (elaborated nodes 2^25, loop iterations
2^24, widths 2^20); a deep priority chain is legal hardware that `opt` and ABC reduce; and the
byte budget bounds what a large `case` can build (S snippets × C items muxes). The
super-linear steps are the ones capped above.

**Out of memory.** Every IR call reserves before it mutates (IR-15); 2E's own vectors are freed
on every path. `ODIN3_ERR_NO_MEMORY` stops at once (PR-3). Unit tests inject a failure at every
allocation (`odin3_util_set_alloc_fail_after`) and assert `NO_MEMORY`, no leak, and a passing
`odin3_check_design` FULL.

## 17. Files, data structures and API (PR-23)

| File | Contents |
|---|---|
| `src/passes/proc/proc.h` | `odin3_proc_opts`, `odin3_proc_stop` (`NONE CLEAN RMDEAD PRUNE ARST`), `odin3_proc_stats`, caps, `odin3_proc_run` |
| `src/passes/proc/proc_internal.h` | `odin3_proc_mctx`, phase entry points, cell helpers, net map, diagnostics |
| `src/passes/proc/proc_test.h` | `odin3_proc_limits`, `odin3_proc_test_set_limits`, phase hooks for unit tests |
| `src/passes/proc/pw.h`, `pw.c`, `pw_sig.c`, `pw_encode.c` | the working form, decode, signal operations, bit key, re-encoding |
| `proc_pass.c` | the pass definition, options, module schedule (PR-3), statistics, info line |
| `proc_clean.c`, `proc_rmdead.c`, `ppool.c`, `proc_prune.c`, `proc_arst.c`, `proc_clock.c` | step A |
| `proc_mux.c`, `proc_snip.c`, `proc_latch.c`, `proc_ff.c` | steps B–D |
| `proc_emit.c`, `proc_diag.c` | cells, constant nets, net map, merges, orphans, provenance; names and messages |
| `src/ir/cells/ff.c`, `builtin.c`, `cells.h` | IR change D |
| `src/passes/builtin.c`, `builtin.h` | the `proc` row (`processes` OK) |

```c
typedef struct odin3_proc_opts { odin3_proc_stop stop_after; } odin3_proc_opts;
odin3_status odin3_proc_run(odin3_pass_ctx *ctx, odin3_design *design,
                            const odin3_proc_opts *opts, odin3_proc_stats *stats);

/* proc_internal.h — one module's lowering */
odin3_status odin3_pw_decode(odin3_proc_mctx *m, odin3_node_id node, odin3_pw *out);
odin3_status odin3_pw_encode(odin3_proc_mctx *m, odin3_pw *pw);   /* --stop-after */
void odin3_pw_free(odin3_pw *pw);
odin3_status odin3_pw_clean(odin3_pw *pw, uint32_t case_index, int32_t max_depth);
odin3_status odin3_pw_rmdead(odin3_proc_mctx *m, odin3_pw *pw);
odin3_status odin3_pw_prune(odin3_proc_mctx *m, odin3_pw *pw);
odin3_status odin3_pw_arst(odin3_proc_mctx *m, odin3_pw *pw);
odin3_status odin3_pw_check_clocks(odin3_proc_mctx *m, const odin3_pw *pw);
odin3_status odin3_proc_mux(odin3_proc_mctx *m, odin3_pw *pw);
odin3_status odin3_proc_latches(odin3_proc_mctx *m);
odin3_status odin3_proc_ffs(odin3_proc_mctx *m, odin3_pw *pw);
odin3_status odin3_proc_connect(odin3_proc_mctx *m, odin3_pw_bit lhs, odin3_pw_bit rhs);
odin3_status odin3_proc_apply_merges(odin3_proc_mctx *m);
odin3_pw_bit odin3_proc_find(odin3_proc_mctx *m, odin3_pw_bit bit);
odin3_status odin3_proc_const_net(odin3_proc_mctx *m, odin3_pw_bit code, odin3_net_id *out);

typedef struct odin3_proc_cell {          /* one cell to create; outputs on fresh nets */
    odin3_celltype_id type;
    const odin3_value *params; uint32_t n_params;
    const odin3_pw_sig *inputs; uint32_t n_inputs;   /* one per input port, in port order */
    odin3_prov_id prov;
} odin3_proc_cell;
odin3_status odin3_proc_add_cell(odin3_proc_mctx *m, const odin3_proc_cell *cell,
                                 odin3_pw_sig *out);
```

`odin3_pw_encode` builds a body in 2D's canonical layout (breadth-first case and switch
numbering, contiguous actions and compares, sync actions last; 2D point (e)) with `O` = every net
that is an `lhs`, `I` = every other net read plus the shared constant nets, both in net-ID order,
then deletes the old node and creates the new one with `odin3_proc_body_pack` and
`odin3_node_create_connected`; `verify` runs on it. Decode then encode with no phase in between
gives a byte-identical `BODY` and the same pin nets (a unit test). All containers are `src/util`'s;
every function stays within the lint limits (≤ 60 lines, CCN ≤ 15, ≤ 5 parameters, request
structs for more).

## 18. Testing (PR-24)

Under ASan/UBSan in Debug; every IR a test builds passes `odin3_check_design` FULL before and
after `proc`; every located diagnostic is asserted as the whole line through a capturing sink.
**Oracle:** every Yosys run is 0.55 (`$VTR_ROOT/build/bin/yosys`, found by 2D's
`ODIN3_YOSYS055` CMake block; without it no 0.55 CTest is registered); 0.69 only nightly, for
drift. Every golden and generated table records `yosys -V` in its header.

### 18.1 Unit tests (Unity)

Inputs are hand-built processes, so 2E-1…2E-3 need only 2D-1 (the `$proc` cell), not the process
generator. `tests/unit/proc_build.[ch]` (test code, may recurse) parses a subset of RTLIL — `wire`
lines, an RTLIL `process` block (`assign`, `switch`, `case`, `attribute \full_case 1`, `end`,
`sync posedge|negedge|low|high|always`, `update`) and `cell` lines — into a module with
`odin3_proc_body_pack` and `odin3_node_create_connected`. The same text is valid input to Yosys,
so each expected result is re-derived from Yosys 0.55 by `tools/proc-check/yosys-phase` (runs
`read_rtlil; proc_clean; …` up to the phase, prints the process) and the command is recorded in
the test. Expected bodies are compared as `odin3_proc_print` text after `odin3_pw_encode`;
expected cells as `write_rtlil` text of the module.

- `test_proc_pw.c`: decode of every position kind (`IN`, `OUT`, `ANY`, constant nets, a wire
  merged into a constant, a constant sync signal); decode → encode identity; the signal
  operations against hand-computed results; the bit key across merged and aliased nets; the net
  map (chains, constants, path compression, keep policy).
- `test_proc_clean.c`, `test_proc_rmdead.c`: every rule of §5.1 (constant signal with a match
  first/later, with `-` and x compares kept, the inline's first-switch condition, the three
  empty-case rules); rmdead with both pools (a `case` listing every value gains `FULL_CASE` and
  loses its default; `casez 2'b1?` then `2'b10` removes the second; a non-constant item stops
  nothing), the pattern cap (W5a, equivalent result).
- `test_proc_prune.c`: promotion of a root assignment, no promotion when a later switch assigns
  the bit, removal of overwritten actions, the intersection over cases with and without a
  default, `FULL_CASE`.
- `test_proc_arst.c`: each row of §5.4's table (polarity through `$not`, `$logic_not`,
  `$reduce_bool`, `$eq`/`$ne` with a constant on either side, two nested resets, VHDL-style),
  `apply_reset` with a self value, `SM` (E2), the round cap (E3 via the hook), the walk cap on a
  cycle; the clock pre-check (E1a, E1b, E5).
- `test_proc_mux.c`: snippet splitting (overlapping `lhs`), each row of §6's table, groups for
  `casez` overlap, `parallel_case`, the `full_case` mask, the open-mux quirks, `$eq` per item with
  x bits, non-constant items, emission and M1 merges, provenance parents of each mux.
- `test_proc_latch.c`: the verified example, quickcheck through a 2D `?:` mux, set-undef
  rewrites and `users > 1`, rule hash-consing and hold memo, grouping by wire and rule, every
  fix-up rule, deferred M2 merges (two processes whose feedback crosses: no latch, as Yosys),
  `init` removal (W2), E6 via the hook.
- `test_proc_ff.c`: grouping, `$dff`/`$adff` with both polarities, collapse with one and two
  equal resets, the self-reset mux, `$dffsr` (W4), `$aldff` (W3), attributes copied.
- `test_proc_pass.c`: no processes; options and their errors; module atomicity (an error in one
  process leaves its module's every `$proc` node and lowers the module before it, later modules
  analysed only); `--stop-after` for each phase; the `processes` flag (`opt` refused after a
  failed `proc`, accepted after a good one); orphan nets; statistics and the info line;
  determinism (two runs, byte-identical `write_rtlil`); caps (E7 through the budget hook); an OOM
  sweep over a module using every construct.
- `test_ir_cells.c` gains the three cells (ports, parameters, `verify`).

### 18.2 Construct rows and goldens

`tests/proc/rows/<slug>.v`, about 45 small modules, one construct family each: `if`/`else`
chains, nested `if`, `case` with and without `default`, multi-value items, `casez`/`casex`,
constant switch, `full_case`, `parallel_case`, both, items equal to the default, blocking temps
across nested switches, prune promotions, unreset registers in a clocked block, every reset form
of §5.4, two resets (same and different values), a non-constant reset value, an unreset register
in a reset block, VHDL-style, `negedge` clocks, self-clocked, constant clock, several clocks in
one module, latches (simple, `case` without `default`, nested, through a `?:`, partial bits of a
vector), `initial` on registers and on combinational outputs, memory writes (conditional,
constant index, partial word), function and task calls in processes, loops, mem2reg reads, the
five error cases. Each has `<slug>.il` (Odin III `write_rtlil` after `read_verilog <slug>.v;
proc`) and, when it has a combinational block, `<slug>.nolatches.il`. Goldens are generated by
`proc-diff --write` only when `proc-diff` reports identical, and reviewed; error rows have
`<slug>.err`. CTests: `proc_rows` (goldens), `proc_diff_rows` (§18.3), `proc_bridge_rows`
(§18.5, `--nolatches` mode).

### 18.3 `proc-diff`: 2E against Yosys `proc` on the same RTLIL

`tools/proc-check/proc-diff --odin3 O --yosys Y [--top T] [--nolatches] [--mem-cap B] [--keep
DIR] [--write] <file>…` (Python, stdlib; every child through 2D's `memcap.py`):

1. `O [--top T] -p "read_verilog [--nolatches] F; write_rtlil in.il; proc; write_rtlil a.il"`;
2. `Y -p "read_rtlil in.il; hierarchy -top T; proc -norom -noopt; opt_clean -purge; write_blif
   -icells -param b.blif"`;
3. `Y -p "read_rtlil a.il; hierarchy -top T; opt_clean -purge; write_blif -icells -param a.blif"`
   (and `a.il` holds no `process`);
4. both files are normalised (INT-kind parameters, whose names the tool lists from the registry,
   to decimal: Yosys writes some polarities as 1-bit constants) and compared with
   `netlist-compare --implicit-models a.blif b.blif`.

Exit 0 identical, 1 differ (diff kept), 2 error. Yosys's `proc` here sees exactly the processes
2E sees, one name per net (§4), so a difference is 2E's. `opt_clean -purge` on both sides only
removes unused cells and wires and joins connected names. `netlist-compare --implicit-models`
(new, 2E) treats a `.subckt` of an undefined `$`-model as a black box whose formals are those
used, outputs being `Y`, `Q`, `X`, `CO`, `RD_DATA`. Known exceptions go to
`tests/proc/diff_expect.txt` with a reason (expected: none; §20 items 8–9 can only arise with
latches sharing a 2D mux).

### 18.4 Bisect

`proc-diff --bisect`: for X in clean, rmdead, prune, arst, Odin III runs `proc --stop-after X;
write_rtlil aX.il`, and Yosys finishes it with the remaining sub-passes (after `clean`:
`proc_rmdead; proc_prune; proc_arst; proc_mux; proc_dlatch; proc_dff; proc_memwr; proc_clean`,
and so on), then step 3–4 above. The first X whose result differs from b.blif names the phase at
fault. Run by the rows CTest on failure and by hand.

### 18.5 The bridge (end to end)

2D's `elab-equiv` gains `--pass proc`: side A becomes `O -p "read_verilog --nolatches F; proc;
write_rtlil a.il"`, then 2D's Yosys back end (whose `proc` is now a no-op); side B is unchanged
(`read_verilog -sv -nolatches F` and the same back end); `equiv-check a.blif b.blif`. Always
`--nolatches` (`equiv-check` cannot model latches, PHASE0 #7): latch rows rely on §18.2–18.3.
`--a-only` keeps `a.blif` for 2H.

### 18.6 Sweeps (label `sweep`, `RUN_SERIAL`, smallest first, under 2D's memory caps)

- `proc_diff_micro` and `proc_bridge_micro`: every micro that 2D elaborates (539 files, the
  multi-file ones through their includer; `--top` from the Parmys golden, 2D point (o)); expected
  failures in `tests/proc/micro_expect_fail.txt` with reasons: the five micros 2E rejects
  (`always_posedge_negedge`, `bm_function_1`, `bm_function_2`, `matrix_multiplication` with E1;
  `paj_top_hierarchy_no_mem` with E2), asserted to fail with those exact errors, plus 2D's own
  list.
- `proc_diff_vtr` and `proc_bridge_vtr`: the 31 VTR and 33 Koios files as one set sorted by size
  (lenet.v last, 8 GiB cap alone), when `VTR_ROOT` is set; nightly.
- `proc_gen_vs_yosys`: 2D's `tools/verilog-gen` gains a `--proc` profile (`always` blocks with
  nested `if`/`case`/`casez`/`casex`, `full_case`/`parallel_case`, random items with x and `?`,
  every reset form, `negedge`, latches in default mode, blocking temporaries, memories) and each
  file runs through `proc-diff` (both latch modes) and the bridge (`--nolatches`): 200 seeds in
  CTest when Yosys 0.55 is found, 5,000 nightly; a difference fails with its seed and file kept.
  Nightly only, the same files run `proc-diff` with 0.69; 0.55 against 0.69 disagreement is
  oracle drift, never a 2E failure.

### 18.7 Python tool tests

`tests/tools/test_proc_diff.py` (fake `odin3`/`yosys` scripts that log their argv: the exact
command strings, parameter normalisation, exit mapping, `--write` refused on a difference,
bisect's remaining-pass lists) and `test_netlist_compare.py` cases for `--implicit-models`.

## 19. Performance (PR-25)

**Measured oracle** (Yosys 0.55, `plugin -i parmys; read_verilog -nomem2reg
+/parmys/vtr_primitives.v; read_verilog -sv -nolatches F; hierarchy -check -auto-top
-purge_lib; proc -norom -noopt`, `yosys -d`, dev machine i7-13700 WSL2, 2026-10-11):

| | mcml.v (650 KB) | lenet.v (11 MB) |
|---|---|---|
| processes (summed over modules) | 131 | 7,049 |
| cells before → after `proc` (summed over modules) | 2,401 → 9,206 | 186,176 → 784,457 |
| created | 4,881 `$mux`, 47 `$pmux`, 303 `$eq`, 1,555 `$dff`, 3 `$adff`, 3 `$reduce_or`, 3 `$not` | 586,643 `$mux`, 10 `$pmux`, 7,884 `$eq`, 3,728 `$dff` |
| wire bits before → after | 0.35M → 0.48M | 0.73M → 7.10M |
| `proc_*` time | 1.88 s (`proc_mux` 1.21, `proc_dff` 0.50, `proc_dlatch` 0.13, rest 0.04) | 34.6 s (`proc_arst` 15.7, `proc_dlatch` 9.0, `proc_mux` 7.1, `proc_dff` 2.0, rest 0.8) |
| whole script, peak RSS | 2.1 s, 137 MB | 57.2 s, 5.22 GB |

The 31 VTR files take 3.9 s for the whole script, the 33 Koios files 78.5 s (lenet.v 57.2 s);
no VTR or Koios file infers a latch in `-nolatches` mode, and only `$dff`/`$adff` are created.

**Targets** (Release, `tests/bench/bench_proc.c`: per file, smallest first, `read_verilog
--nolatches` untimed, then the time of steps A–E separately, cells and nets created, peak RSS):

| Measure | Target |
|---|---|
| mcml.v, `proc` | ≤ 0.2 s |
| lenet.v, `proc` | ≤ 5 s |
| lenet.v, `read_verilog --nolatches; proc`, peak RSS | ≤ 3.0 GB |
| the 31 VTR files, total `proc` | ≤ 0.5 s |
| the 33 Koios files, total `proc` | ≤ 8 s |
| complexity | O(body + cells and pins created) per process, plus Yosys's own super-linear points: pattern pools (capped), quickcheck O(actions × mux cone), groups O(outputs × sync actions) |

**lenet.v estimate.** 2D's stage-2 peak is ≈ 1.5–1.6 GB (2D §15). `proc` adds ≈ 0.59M nodes:
586k `$mux` of ≈ 10.8 bits on average (6.4M new wire bits / 587k muxes) have ≈ 3 × 10.8 + 1 ≈ 33
pins, ≈ 19.4M pins × 28 B × 1.3 (pin-array size classes) ≈ **0.71 GB**; fresh output nets ≈ 6.4M
× 48 B ≈ **0.31 GB**; nodes and parameters ≈ 0.05 GB; the deleted `$proc` nodes' pins stay
until `compact` (IR-6, already in 2D's figure); working forms of the largest module ≤ 0.1 GB.
Total ≈ **2.7 GB**, inside 3.0 GB. The bench prints each term so a miss is explainable; a missed
target is investigated and reported, never relaxed (2A's rule). Odin III is expected to beat
Yosys mainly on `proc_arst` (O(1) driver lookup instead of a scan of every cell per switch) and
on the hash-pool overheads of `proc_dlatch` and `proc_mux`.

## 20. Divergences from Yosys 0.55 and Parmys

| # | 2E | Yosys 0.55 `proc` | Parmys | Why |
|---|---|---|---|---|
| 1 | signals compared as nets (§4) | literal `SigSpec`s in clean, rmdead, arst, mux | same, after pre-`proc` `opt_expr`/`opt_merge` hid some resets | 2D's merged nets; a superset is accepted, equivalent where both accept |
| 2 | no `proc_rom` | runs it | `-norom` | the exit bar is Parmys |
| 3 | no trailing `opt_expr -keepdc` | runs it | runs it | 2F's (§24 item 5) |
| 4 | no `proc_init` | runs it | runs it | 2D emits `init` attributes directly |
| 5 | `$aldff` when the reset value is not made of constant nets | `ConstEval` may prove it constant (`$adff`) | same | 2D folds constant cells; equivalent |
| 6 | `check_signal` stops after 1024 hops | recurses until the stack overflows on a cycle | same | robustness |
| 7 | pattern pools capped (W5a/W5b) | unbounded | same | memory rule; equivalent, coarser |
| 8 | processes in node-ID order | process dictionary order | same | determinism; differs only when two latches share a mux (set-undef rewrites) |
| 9 | bits ordered by (primary wire ID, bit) | by name-interning order, then offset | same | reproducible; creation order only |
| 10 | W1, W2 are warnings | logged | logged | DESIGN §6 "latch detection (warn)" |
| 11 | errors E1–E5 located, house style | `ERROR: Multiple edge sensitive events found for this signal!` etc. | same | same conditions, better text |
| 12 | no `$memwr`, no `proc_memwr` | `$memwr_v2` | same | 2D collects `$mem` (2D §17 #15); E4 guards the reset case |
| 13 | SV attribute checks (`always_ff`, …), `$global_clock`, any-edge, `-global_arst`, `-ifx`, `-nomux` not provided | provided | — | not producible by Verilog-2005 bodies; Phase 5 revisits |
| 14 | cells unnamed (`$c<ID>` when written) | `$procmux$N`, `$procdff$N` | same | names carry no meaning; provenance does |

## 21. Review focus

1. **`proc_mux`'s grouping and append quirks** (§6): the open mux across a group boundary, the
   `value == A` skip, the `full_case` mask with nested switches. `proc-diff` on the rows and the
   generator is the guard; reviewers should trace `test_proc_mux.c`'s `casez` case by hand.
2. **prune's `assigned`/`affected`** (§5.3) with the counter implementation replacing Yosys's
   copied sets: an off-by-one in the "every case" intersection promotes a bit a later branch
   assigns.
3. **arst's `apply_reset`** (§5.4): `SM` initialisation, the assign view including the process's
   own promotions, `eliminate_const` followed by depth-1 clean.
4. **The latch phase's two views** (§8): the snapshot for quickcheck, live ports for the walk,
   merges deferred to M2. Getting this wrong changes which crossed feedback becomes a latch.
5. **Merge points and the net map** (§7): a working-form net used without `odin3_proc_find`
   after a merge points at a dead net; Debug asserts every resolved net is live.

## 22. Implementation split (PR-26)

Four pull requests; the plan (next) fixes tasks, files and interfaces. Model and effort per
PHASE0 §9 and PHASE2's 2E row (Opus `xhigh` → `high`); the agent says when to switch.

| PR | Tasks | Content | Waits for | Model/effort |
|---|---|---|---|---|
| 2E-1 core | 1. IR change D (three cells, writer refusal rows, `docs/IR.md`/DESIGN §5.2) · 2. working form: `pw.h`, decode, signal operations, bit key, encode, `proc_build` test helper · 3. pass skeleton: options, schedule and atomicity, net map and merges, constant nets, cell helper, provenance, diagnostics, caps and hooks, orphans, `docs/PASSES.md` row | 2D-1 merged (the `$proc` cell, `proc.h`, `write_rtlil`, the pass flag) | 1: Opus `high`; 2–3: Opus `xhigh` |
| 2E-2 decision trees | 4. clean, rmdead, pattern pools · 5. prune · 6. arst and the clock pre-check | 2E-1 | 4, 6: Opus `xhigh`; 5: Opus `high` |
| 2E-3 lowering | 7. mux (snippets, switch cache, groups, selects, emission, M1) · 8. latch (database, quickcheck, walk, holds, fix-up, M2, `init`) · 9. ff (groups, the four cell kinds, attributes), summary | 2E-2 | 7–8: Opus `xhigh`; 9: Opus `high` |
| 2E-4 evidence | 10. tools: `proc-diff` and `--bisect`, `yosys-phase`, `netlist-compare --implicit-models`, `elab-equiv --pass proc`, Python tests · 11. rows and goldens, `proc_rows`/`proc_diff_rows`/`proc_bridge_rows` · 12. sweeps and expect lists, the generator profile · 13. `bench_proc`, the targets, the PHASE2 entry, the amendments of §23 | 2E-3; 2D-6 (process generator, `elab-equiv`) and 2D-7's sweep preset for 11–13 | Opus `high`; Sonnet `high` for bulk goldens and bench runs |

Lanes: 2E-1 → 2E-2 → 2E-3 serially (they share the working form); 2E-4's tools (Task 10) can
start alongside 2E-2 on hand-written RTLIL. 2E-1…2E-3 run in parallel with 2D-2…2D-6, since unit
tests build processes by hand (§18.1). 2E and 2F proceed in parallel (PHASE2 order 2E ‖ 2F).
Effort cues: `/model opus` + `/effort xhigh` at Task 2; `/effort high` for Tasks 1, 5, 9–13;
`/model sonnet` + `/effort high` for bulk goldens and bench runs; `/clear` between tasks. Each
task gets a read-only critique (Opus `high`; `xhigh` for 6–8) and each PR a whole-branch review
(Opus `xhigh`, or Fable if `/model` offers it) before it opens (PHASE0 §A.1); tests first,
`/lint` before each commit, gates `cmake --build --preset debug -j6 && ctest --preset debug
-j4`.

## 23. Amendments to other documents

Made when the pull requests land (not by this spec):

- `docs/IR.md`: IR-8's registry gains the three cells of IR change D (2E-1); IR-19 (2D's)
  notes that `proc` deletes `$proc` nodes and that their tombstones keep `BODY`.
- `docs/DESIGN.md`: §5.2 word list (+ `$dlatch $aldff $dffsr`); §6 step 3 reads "`proc` —
  Yosys-equivalent lowering of `$proc` cells: decision trees to `$mux`/`$pmux`, syncs to
  `$dff`/`$adff`/`$aldff`/`$dffsr`, latches to `$dlatch` (warned)" (blocking/non-blocking
  analysis is 2D's).
- `docs/PASSES.md`: the `proc [--stop-after X]` row (2E, RTLIL view with processes → RTLIL view).
- 2D spec, EL-35 step 4: sync actions are **one action per output wire chunk** (Yosys's
  `addChunkActions` over the sorted lvalue), as are the root defaults, so `proc`'s register
  groups (§9) are Yosys's; EL-33's "structured body accessors wait for 2E" becomes "deferred"
  (§24 item 8). To be checked against 2D-6 before 2E-4.
- `tools/netlist-compare`: `--implicit-models`; `tools/elab-check/elab-equiv`: `--pass proc`.
- `tests/proc/micro_expect_fail.txt` and 2H's exceptions list: the five micros of §18.6 and why
  four other Parmys-rejected micros pass in 2E (§4).
- `docs/PHASE2.md`: 2E's entry (the controller writes the decision log).

## 24. Decisions for Peter

Agent defaults under the autonomy rule unless marked; Peter may override any.

1. **IR change D — three registry cells** (§13). *Recommended:* add `$dlatch`, `$aldff`,
   `$dffsr` with Yosys's ports, parameters and simlib meaning (INT polarities, `simulate` NULL,
   `write_verilog` refusal rows). It is the registry, a design rule, hence Peter's.
2. **Fidelity bar and the net view** (§4, §18.3). *Recommended:* 2E's acceptance test is
   structural identity with Yosys 0.55 `proc -norom -noopt` run on the same RTLIL (`proc-diff`),
   plus bridge equivalence; signals are compared as nets, so 2E (like plain Yosys `proc`)
   lowers the four micros Parmys rejects only because its pre-`proc` `opt_expr` hides `$eq`
   resets, and they stay Parmys-rejected exceptions.
3. **Latches** (§8, §15). *Recommended:* one located warning per `$dlatch` (Yosys only logs); no
   error mode; `--nolatches` stays a `read_verilog` option (2D decision 4), so `proc` has no
   latch option.
4. **Initial values** (§10). *Recommended:* keep Yosys's `init` attribute on wires; no `INIT`
   parameter on word flip-flops (`lower` moves it to `$_DFF_*` in Phase 3); a combinational
   output loses its `init` with a warning.
5. **The `proc` boundary** (§2.1). *Recommended:* phases in Yosys's order without `proc_rom`
   (Parmys's `-norom`) and without `proc_init`; one `proc` pass with `--stop-after` for
   bisecting, no separate `proc_*` passes; Yosys's trailing `opt_expr -keepdc` is called from
   `proc` once 2F provides it (`proc --noopt` then skips it, as Yosys), wired by whichever of 2E
   and 2F lands second; until then `proc` equals `proc -norom -noopt`.
6. **Failure atomicity** (PR-3). *Recommended:* per module, all or nothing: a module with any
   error keeps every `$proc` node; up to 20 located errors are reported; `ODIN3_ERR_PARSE`.
7. **Dead outputs** (§12). *Recommended:* lower unused outputs like Yosys and leave dead cells to
   2F's `opt`; 2E deletes only the unnamed nets it strands.
8. **ABI.** *Recommended:* no structured `$proc` body accessors yet (the text form through
   `odin3_node_get_param_text` exists); add them when a plugin or the Python walk needs them.
