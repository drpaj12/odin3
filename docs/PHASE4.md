# Odin III: Phase 4: VTR architectures and FSM

Spec: `docs/DESIGN.md` §6 step 7 (`read_arch`), §7 (partial mapping), §9 (writers), §12, §12.1,
§13 Q5; ADRs D10 (the scope; amends D5 and D6), D1, D3 (subprocess tools), D9 (provenance).
Inputs from Phase 3: the 3C `hardmap` rules (the fixed Parmys-default binding, PHASE3
decision 4), `lib/vtr.o3lib` (1G), the `tools/vtr-qor` harness and its `fcheck`
(functional check), `tools/hardmap-check`, `tools/equiv-check`. Oracle: VTR `3c9a4d23`
(`docs/ORACLES.md`). External trees are read-only.

**Goal (Peter, 2026-10-19): the tool works on VTR architectures, with FSM recoding.** QoR
parity is no longer an exit criterion. Altera via LPM is Phase 5 and the analysis flow is
Phase 6 (ADR D11, decision 8); their items left this file.

**Exit test** (DESIGN §12 row 4, 4.8 and 4.13):

1. **Arch sweep (4.13).** The VTR designs (the Phase 3 set) on several VTR archs:
   `k6_frac_N10` (the Phase 3 arch), one DSP arch and one plain-LUT arch (which ones: open
   item 1). Pass = VPR routes (`vpr_status=success`), the `vtr-qor` fcheck passes, and the
   micros are equivalent (`equiv-check` against the Parmys golden for that arch).
2. **FSM (4.8).** Its acceptance test passes.

Each sub-project gets its own spec (`docs/specs/`), plan and PRs. Model and effort follow
`docs/PHASE0.md` §9: mapping algorithms Opus `xhigh` to `high` with the advisor; importers,
writers and harnesses Opus `high`; bulk configs Sonnet. Peter reviews every mapping algorithm
(DESIGN §12).

## Checklist (kept items)

Each item has a one-line acceptance test.

- [x] 4.1 VPR-XML import: `read_arch` reads a VPR arch's `<models>` and pb_types into a
  `.o3lib` tech library. Test: the imported library for the Phase 3 arch yields the same hard
  cells and ports as `lib/vtr.o3lib`; each sweep arch imports without error.
- [ ] 4.2 Generic hard blocks matched by exact port signature (Odin II style). Test: a
  design instantiating a model of an arch binds to that model; a port-signature mismatch is a
  located error or soft logic, as specified in the 4.2 spec.
- [ ] 4.3 Multiplier splitting generalised from the arch: split size and small-multiplier
  threshold from the XML; signed multipliers (light). Test: on the DSP arch the micros with
  `$mul` of several widths and signs are equivalent; on `k6_frac_N10` the output equals the 3C
  fixed k6 rules (which stay Parmys's).
- [ ] 4.4 Memory depth/width splitting to whatever RAM blocks the arch has. Test: memory
  micros are equivalent on every sweep arch; `k6_frac_N10` output equals the 3C k6 result.
- [ ] 4.5 Carry chains read from the arch model (light). Test: adder micros equivalent on the
  arch with a carry model; 3C maps to k6's `adder` only and stays unchanged.
- [ ] 4.6 FF feature matching: enable and sync/async reset mapped onto what the arch's FF
  supports, soft glue for the rest. Test: FF micros (enable, sync reset, async reset) are
  equivalent on an arch with a featureful FF and on a plain one.
- [ ] 4.7 Memory port merge: registered reads and writes merged into `$mem` ports; no
  `ram_style` attribute overrides. Test: a registered-read RAM micro yields one `$mem` with the
  read register merged, and is equivalent.
- [x] 4.8 FSM detect and recode (restored, decision 6): a port of Yosys 0.55's `fsm` as its
  default script runs it (`fsm_detect`, `fsm_extract`, `fsm_opt`, `fsm_recode`, `fsm_map`;
  `-expand` and `-export` are not in v1.0). It runs where Parmys runs it, right after `proc`
  (Parmys: `proc -norom; fsm; opt; wreduce`, `parmys.cc`). Encoding follows Yosys: the
  `fsm_encoding` attribute (`none`, `one-hot`, `binary`, `auto`) is honoured, and `auto` is
  one-hot below 32 states and binary from 32; `-encoding one-hot` forces one-hot for every FSM.
  Provenance: the recoded state register and next-state logic get `DERIVED` records whose
  parents are the original state register and its logic, and each FSM gets attributes (state
  count, encoding) that Phase 6 items 6.2 and 6.3 export. Test: FSM micros are equivalent to
  the RTL after `fsm`; detection and encoding match Yosys 0.55 `fsm` on the micros (same FSMs,
  state counts and encodings); the 19 VTR designs with `fsm` in the script still route and
  pass fcheck.
- [ ] 4.13 Arch-sweep exit test (above). Test: the exit test item 1.

## Dropped from Phase 4 (D10)

| Old item | Reason |
|---|---|
| 4.9 mux collapsing | Done: `opt` already does it (`opt_muxtree`, `opt_reduce`) |
| 4.10 matcher v1 | The matcher is not in v1.0 (D5 amended); port signature and fixed rules instead |
| 4.11 project mapping rules | Not needed to make the tool work; the project file still reads and stores them (DESIGN §4.0) |
| 4.12 per-bit `opt_dff` parity | Not needed for "the tool works"; QoR parity is no longer a goal |

Moved out by D11: 4.14 (Altera via LPM) is Phase 5 item 5.1; 4.15 to 4.19 are Phase 6 items
6.1 to 6.5. The old Phases 5 and 6 (SystemVerilog/VHDL, `raise`) are after v1.0, recorded in
DESIGN §12.1.

## Dependencies on Phase 3

- 3C `hardmap` and its rules are the base: 4.2 to 4.6 generalise them from the arch, and must
  reproduce the fixed k6 result on `k6_frac_N10`. 4's `partial_map` replaces `hardmap` (PHASE3
  sub-project 3C).
  The identity gate (`tools/partial-map-check/identity`): tier 1 (`partial_map_identity`, CI)
  is a wiring/alias test, not a regression test (`hardmap` and `partial_map` share one entry
  point in one binary); tier 3 (two binaries, main against the head, over the micros and the 19
  VTR designs, `tools/partial-map-check/tier3`) is the merge gate of every refactor PR.
- `tools/vtr-qor` and its `fcheck` are the harness for 4.13; its caps and smallest-first order
  apply (heavy-run lock: sweeps run when lanes are idle).
- `lib/vtr.o3lib` and the `.o3lib` format (1G) are the target of 4.1.
- `write_blif --vtr` (3E), `lower` (3B) and the linked `abc` (3D) are unchanged.

## Decisions log

1. **v1.0 scope** (Peter, 2026-10-19): "since our goal is to make Odin III ... I just want the
   tool to work"; "go with all the recommendations (we will be looking reverse engineering from
   a different perspective of the goal of going from altera all the way up) I would like to
   target Altera devices with LPM and structured verilog, but I'm not interested in their
   low-level formats." Phase 4 is "the tool works on VTR architectures and on Altera via LPM";
   Phase 5 is delayed; Phase 6 is removed from v1.0; Phase 7 is kept without the EDIF writer.
   Recorded in `docs/ADR/D10.md` and DESIGN §12, §12.1.
2. **LPM device family** (Peter, 2026-10-19): "Cyclone V is fine, go ahead just a basic set of
   memory and simple items is fine - it's just a basic path". Cyclone V (Quartus Lite) is the
   device family; device-independent LPM where no device parameter is needed. This supersedes
   the earlier agent default and answers DESIGN §13 Q5 for v1.0.
3. **LPM set** (Peter, 2026-10-19, same message; the exact list is the coordinator's):
   `lpm_add_sub`, `lpm_mult`, `lpm_compare`, `altsyncram` (single- and simple-dual-port, Cyclone
   V). Flip-flops: plain DFF in soft logic rather than `lpm_ff` (agent default, Peter may
   override; it needs no extra library cell or simulation model). Everything else is soft logic
   as generic structural Verilog gates or assigns.

4. **Altera family** (Peter, 2026-10-19): "we just want a path to stratix (nothing deep) so an
   LPM family neutral is completely reasonable". The LPM output is family-neutral, with a
   family option (default Cyclone V, tested in Quartus Lite). Stratix is set by the option and
   untested; Quartus Lite has no Stratix support. Amends decision 2.
5. **The generalized analysis flow** (Peter, 2026-10-19): "yes to all the recommendations and
   build the generalized flow in OdinIII to hit on this". Items 4.15 to 4.19 serve the goals
   in D10 follow-up 2 (netlist reverse engineering, graph methods, provenance back to HDL).
   The experiments (Gate-Breaker dataset, Bayesian classifier, graphlets, graph sandwich, GNNs,
   the placement study) are designed after v1.0, as plugins over these outputs. Phase 3
   parity scope: PHASE3 decision 53.
6. **FSM restored** (Peter, 2026-10-19): "reinclude the FSM detection to map to one-hot
   ecoding for FPGAs". 4.8 is back in Phase 4 (it had been dropped by D10). Agent defaults:
   port Yosys `fsm` and run it right after `proc`, as Parmys does, so `--vtr-compat` matches
   Parmys; Yosys's `auto` rule (one-hot below 32 states) is the default, `-encoding one-hot`
   forces one-hot. The pass is a mapping algorithm: Peter reviews it (DESIGN §12).
7. **Provenance coverage target** (Peter, 2026-10-19): "4.19 using the Provenance path we
   should expect a very good report". 4.19 is a gate, not only a measurement: full coverage
   apart from named exception classes; an unexplained unresolved object is a bug.
8. **Phase split** (Peter, 2026-10-19, ADR D11): "yes to all recommendations". Phase 4 keeps
   4.1 to 4.8 and 4.13. Altera via LPM (4.14, decisions 2 to 4) becomes Phase 5 item 5.1, in a
   Phase 5 of outputs and read-back (`docs/PHASE5.md`). The analysis flow (4.15 to 4.19,
   decisions 5 and 7) becomes Phase 6 items 6.1 to 6.5 (`docs/PHASE6.md`). Decisions 2 to 5
   and 7 stay here as history; PHASE5 and PHASE6 restate them.

9. **4.1 design choices** (Peter, 2026-10-19: "go with your recommendations"). The arch record is
   `odin3_design_get_arch` in `src/ir/design.[ch]`, shaped like the project record; it is an IR
   container change Peter reviewed and approved, kept minimal and documented in `docs/IR.md`.
   Agent defaults (spec `docs/specs/2026-10-20-4.1-arch-import-design.md` §12 questions 1, 3 to 8):
   reuse `src/frontends/project/xml.[ch]` in place, no new dependency; `write_techlib` dumps only
   the library `read_arch` generated; the `multiply` template stays the square VTR primitive;
   models with non-identifier names are skipped with one warning; only `<complexblocklist>` is
   read; generic cells are `blackbox`; no script changes before 4.2.
10. **Sweep archs** (Peter, 2026-10-19, same message; closes open item 1). DSP arch:
   `COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml` (fallback
   `timing/k6_N10_mem32K_40nm.xml`). Plain-LUT arch: `timing/k4_N4_90nm.xml` (fallback
   `timing/k6_N10_40nm.xml`). Each needs Parmys goldens from `tools/run-oracle.sh`, scheduled by
   the coordinator (not run by the 4.1 lane).

11. **CI coverage of 4.1** (agent default). `.github/workflows/ci.yml` does not set `VTR_ROOT`, so
   `test_arch_vtr` and `arch_corpus` skip (exit 77) in CI and run locally only; the
   template-versus-`lib/vtr.o3lib` and fixture tests run everywhere. CI is not changed.

12. **4.2 to 4.7 design: the eleven questions** (Peter, 2026-10-19: "Yes to all"). Spec
   `docs/specs/2026-10-20-4.2-partial-map-design.md` section 12, plan
   `docs/plans/2026-10-20-4.2-partial-map-plan.md`. Approved: (a) `hardmap` is renamed
   `partial_map`, with `hardmap` kept as an alias; (b) Parmys's rule is the default and every
   improvement is a named option that is off by default; (c) the 3C rulings D1, D2b, D3, D4, D5
   stay RTL-correct and warned; (d) `--min-mult 3` and `--min-add 1`, so 4.3's "threshold from the
   XML" is corrected: the XML has none; (e) the multiplier WIDTH is square and comes from the
   arch, and a non-square model stays soft with a warning; (f) an oversize multiplier is split as
   Parmys does, after a probe; (g) signed multipliers sign-extend when Y fits, else stay soft;
   (h) the RAM depth limit comes from the arch, slices are 1 bit by default, and `--mem-width
   arch` selects the arch's modes; (i) the 4.2 errors and warnings are as specified (B-3 to B-7);
   (j) 4.6 is a `--ff-file` table on `read_arch`, off by default, built last and droppable from
   v1.0; (k) 4.7 is read-side only (`memory_merge`), `ram_style` is ignored with a warning.
13. **Spec questions not put to Peter** (agent defaults, he may override): the spec's other seven
   open questions (8 fracturing deferred, 10 one depth limit D for SP and DP, 12 PR order A to H,
   14 no name-based FF classification, 16 new tools `partial-map-diff` and
   `partial-map-check/identity` with `hardmap-diff` kept for the alias, 17 K from the record for
   `abc` and `write_blif` in a small 4.13 PR, 18 goldens for the new archs are a controller step)
   take the spec's recommendations.
14. **4.8 FSM design questions** (Peter, 2026-10-19, on `docs/specs/2026-10-20-4.8-fsm-design.md`
    section 20: "Yes to all"). That adopts the recommended answer to each of the ten questions:
    1. no `$fsm` registry cell; the FSM table lives inside the pass (no IR change);
    2. `fsm_opt` finds unused outputs with a native "unread and not a port" test, `opt_clean`
       unchanged;
    3. `keep` on a state register follows Yosys (detected and recoded; `fsm_encoding = "none"`
       excludes);
    4. 1,000,000-evaluation cap per FSM, then skip with a warning;
    5. no pre-`proc` `opt_expr; opt_clean; opt -nodffe -nosdff` lines in the Phase 3 script;
    6. `fsm` runs only in scripts that name it (the VTR scripts do), not gated on `--vtr-compat`;
    7. attributes `odin3_fsm_states`, `odin3_fsm_encoding`, `odin3_fsm_orig_bits`,
       `odin3_fsm_orig_name`;
    8. the RAM write-enable 2-state registers are ported faithfully (recoded as Parmys does);
    9. micro equivalence uses a bounded Yosys miter after reset (`fsm-equiv`);
    10. stereovision3's unexplained flop is not a blocker for 4.8.
    The plan (`docs/plans/2026-10-20-4.8-fsm-plan.md`) called this "decision 11"; main already
    held 9 to 13, so it is 14.
    **Result (2026-10-20, branch `feat/4.8-fsm`; agent defaults, Peter may override).**
    - Micros: `fsm_equiv_micro` and `fsm_diff_micro` pass (32 `yes` rows equivalent from reset;
      Odin III's `fsm` matches Yosys 0.55 on the same RTLIL). 9 VTR designs through `fsm-diff`
      (logs `/tmp/claude-1000/fsm/diff/*.log`): ch_intrinsics, stereovision3, raygentop, boundtop,
      LU8PEEng, LU32PEEng ok; stereovision1 and mcml differ only in that Odin III logs an alias
      wire of a rejected register as a second rejection; blob_merge differs only in the no-fsm
      baseline (5 `$dffe`); all in `tests/fsm/diff_expect.txt`.
    - `hardmap-diff` on the 19 (`/tmp/claude-1000/fsm/hm/sweep.log`): ch_intrinsics is now
      equivalent to Parmys; stereovision3's latch count equals Parmys's (the unexplained flop of
      PHASE3 #43 is gone) and it is `unproven` (two clock domains); blob_merge latch count equals
      Parmys's and dsec says NOT equivalent from the all-zero start (D16 in
      `tests/hardmap/vtr_expect.txt`). D16 is isolated (outcome A, 2026-10-20): `timeout 900
      tools/fsm-check/fsm-equiv --odin build/release/odin3 blob_merge.v --top RLE_BlobMerging
      --rst iReset:1 --depth N` (Odin III's `proc` against `proc; fsm; opt`, a Yosys miter with the
      reset held in steps 1 and 2) exits 0 (equivalent) at depth 3, 8 and 30 (depth 60 stopped with
      exit 3 after 60 s, no verdict); the same run without `--rst`, depth 8, exits 1. So the two
      netlists agree from reset and differ only in the illegal all-zero start state, which the
      one-hot register of a sync-reset FSM has and the binary original did not (decision text
      below; plan Deviations 12 and 15). The sweep stays red on the 11 big designs that
      have no expect line (as before this change, listed in the vtr_expect comment).
    - **Behaviour change, all-zero start.** A one-hot register without async reset (sync-reset FSMs:
      blob_merge, ch_intrinsics) powers up all-zero, an illegal state with no outgoing transition;
      the binary original powered up in state 0. This is Yosys and Parmys behaviour, so parity
      holds, but anything simulating from the all-zero start (ABC dsec, equiv-check) sees another
      machine; equivalence holds from reset. `fsm_opt` also removes a no-reset state that has no
      incoming transition, as Yosys does. RAM write-enable registers (ruling 8): nothing to port,
      Odin III's proc makes no `$memwr` registers (plan Deviation 16).
    - **Summary tables** (from the logs under `/tmp/claude-1000/fsm`). `fsm-diff` on 9 VTR designs:
      ok LU32PEEng (3 FSMs), LU8PEEng (3), boundtop (6), ch_intrinsics (1), raygentop (6),
      stereovision3 (1); differs, all in diff_expect: blob_merge (1), mcml (1), stereovision1 (2).
      `hardmap-diff` on the 19: equivalent diffeq1, diffeq2, mkPktMerge, ch_intrinsics; listed
      stereovision3 (unproven), sha (unproven), blob_merge (D16), bgm (D2b); counts-differ boundtop
      (latch +6), mkSMAdapter4B (latch +4), stereovision1 (adder +24), LU8PEEng and LU32PEEng
      (adder +53), mcml (adder -2, latch -1); differs raygentop, or1200, stereovision0; unproven
      mkDelayWorker32B; unpaired stereovision2 (19 designs: 6 counts-differ, 3 differs, 4
      equivalent, 4 listed, 1 unpaired, 1 unproven). `vtr-qor run --leg O` on 21: 17 ok, 3 failed,
      1 capped (LU32PEEng) in 3:36:30.
    - `vtr-qor run --leg O --fcheck` (`/tmp/claude-1000/fsm/qor_run.log`, `qor2_run.log`): 17 of 21
      designs `vpr_status=success`; fcheck pass on 14; fail on raygentop, boundtop, stereovision0
      (the same designs fail on the P leg for `dropped-latches` and `x-state`, PHASE3 #50; no
      no-fsm control run was made). arm_core: pre-existing hang in linked ABC (also without `fsm`,
      controller's rerun; not rerun here). LU32PEEng: capped at the 6 GiB scope in the front end.
      **bgm and mcml: `abc` fails at `strash`**, new with `fsm`: the O-leg script has no `opt`
      after `flatten`, so the mapped decoders of `LogCalc.r_indexFirstOne` (32 states, binary) get
      constant inputs from the instance and `lower` writes `.names vcc vcc n` (XOR 10/01); the
      `abc` adapter's exchange deduplicates the fanin and ABC rejects the empty cover ("SOP has a
      mismatch between its cover size (0) and its fanin number (1)"). With `opt -full` after
      `flatten` (as `phase3_pre_abc.script` has) mcml passes `strash`. Not fixed here: either add
      that `opt` to `qor_legs.py` or make the adapter fold equal-fanin cells; controller's call.
15. **Read-enable RAMs and big soft memories** (Peter, 2026-10-20): read-enable RAMs map hard with
   exact soft emulation; soft-memory warning above 50k FFs; ABC time cap deferred. A `$mem` read
   port whose enable is not constant 1 no longer stays soft (`MEM_SOFT_EN` is gone): the RAM reads
   every cycle, the enable goes through a flip-flop, and a hold register and a `$mux` per data bit
   keep the output while the enable is 0 (Yosys `Mem::emulate_rden`, as called by `memory_libmap`
   `emu_en`). One located WARNING per such memory port. A memory left soft whose width x depth is
   above 50,000 bits draws a located WARNING that names it, its bits and why it stayed soft (a
   warning only). 4.2 plan PR D, Tasks 11 and 12. Peter approved 2026-10-20: `--mem-width arch`
   pads the address to the chosen mode's width; DATA_WIDTH is the slice's bit count.
16. **D17 and D18** (Peter, 2026-10-20). D17, Parmys's soft memory has no read register: keep
   RTL-correct, a Parmys defect, no code change. D18, an explicit `single_port_ram`,
   `dual_port_ram`, `multiply` or `adder` instance on an arch without that model: under
   `read_verilog --vtr-compat` it is built as soft logic from `lib/soft_primitives/<name>.v` (our
   own behavioural bodies, read as library units on demand) with a located WARNING, as Parmys
   does; without `--vtr-compat` the unresolved-module error stays. Including `adder` in D18 is an
   agent default, Peter may override. 4.2 plan PR D.

## Open items

(none open. Item 1, the sweep archs, is closed by decision 10.)
