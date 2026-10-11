# Odin III — Phase 3: `lower`, linked ABC, VTR flow

Spec: `docs/DESIGN.md` §5.4 (views), §6 steps 9–11 (`lower`, `abc`, `write_*`), §7 (only the
fixed subset in decision 4), §9 (BLIF, VTR dialect), §11 ("VTR 19"), §12, §13 #6 (name style);
ADRs D1, D3 (ABC is linked), D9 (provenance). Inputs: PHASE0 #7/#8 (latch init, `equiv-check`
models), #13, #29 (WSL caps), #30; PHASE1 #15/#16/#20/#21 (BLIF rules), 1G (`lib/vtr.o3lib`), 1E
(no RAM simulation yet); PHASE2 #2, #4, #13 (2D: `$mem` form EL-27/EL-39, x written as `unconn`,
`--nolatches` for VTR runs). Oracle: VTR `3c9a4d23` (`docs/ORACLES.md`): Yosys 0.55 + Parmys,
VTR's in-tree ABC, VPR. External trees are read-only; every VTR script is used, never edited.

**Exit test** (DESIGN §12 "VTR 19 through P&R; QoR table", made precise; the choices are
decisions 2–3 and 16 below):

1. **Designs: the 19 VTR 7.0 benchmarks**: `bgm`, `blob_merge`, `boundtop`, `ch_intrinsics`,
   `diffeq1`, `diffeq2`, `LU8PEEng`, `LU32PEEng`, `mcml`, `mkDelayWorker32B`, `mkPktMerge`,
   `mkSMAdapter4B`, `or1200`, `raygentop`, `sha`, `stereovision0`–`3`. Evidence:
   `$VTR_ROOT/doc/src/vtr/benchmarks.rst`, table "The VTR 7.0 Benchmarks", lists exactly these
   19. VTR's own QoR task `vtr_flow/tasks/regression_tests/vtr_reg_nightly_test3/vtr_reg_qor_chain`
   runs them plus `arm_core` and `spree` (21). `LU64PEEng` is in neither. The goldens'
   `vtr/` group (PHASE0 §7, there called "VTR-19") is the whole `benchmarks/verilog` directory:
   31 files, including micros such as `single_ff` and the `*_nolatches` variants. Every one of
   the 19 has a Parmys golden with `status=ok` on both arches.
2. **Architecture**: `vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml`, the arch of
   `vtr_reg_qor_chain`. Its `<models>` are `adder`, `multiply`, `single_port_ram` and
   `dual_port_ram`. VTR's own readers give LUT K = 6 (the largest `.names` pb_type) and a RAM
   address width of 15.
3. **Through P&R**: Odin III alone produces each netlist. The script is `read_verilog
   --nolatches; proc; opt; flatten; [wreduce;] hardmap; lower; abc; write_blif --vtr`
   (`wreduce` per decision 7). Then
   `run_vtr_flow.py <design>.blif <arch> -start vpr -track_memory_usage
   -crit_path_router_iterations 100` (the `vtr_reg_qor_chain` options) must end with
   `vpr_status=success`. That means VPR packs and places, finds the minimum channel width by
   binary search, and routes legally at 1.3 × that width (`relax_w_factor`) with timing
   analysis. Two further conditions: the functional check passes (decision 16), and two Odin
   III runs produce byte-identical BLIF.
4. **QoR table**: each design gets one row with Odin III (O), Parmys (P) and the ratio O/P, plus
   a geomean row:
   - **Netlist** (from the pre-VPR BLIF): LUTs (`.names`, constant generators excluded), FFs
     (`.latch`), `adder`, `multiply`, RAM slices (`single_port_ram` + `dual_port_ram`), ABC
     depth.
   - **Packed** (`vpr.out`): `num_clb`, `num_memories`, `num_mult`, device grid.
   - **Routed**: `min_chan_width`; `critical_path_delay` at the relaxed width, and
     Fmax = 1/CPD; `crit_path_routed_wirelength`.
   - **Cost**: front-end wall time and peak RSS (Odin III including ABC, against Parmys plus
     VTR's ABC stage); VPR `total_runtime` and `max_vpr_mem`.
5. **Baseline**: the Parmys flow, i.e. `run_vtr_flow.py <design>.v <arch>` with its defaults:
   `parmys`, then VTR's ABC stage (iterative per-clock black-boxing), then VPR. It runs at the
   same VTR commit, with the same VPR binary and options, on the same machine. `tools/vtr-qor`
   reruns it. VTR's `golden_results.txt` (other machines, another revision) is only a cross-check.
   Two **control legs** separate the causes of a gap:
   - **O′**: Odin III's pre-ABC netlist through VTR's ABC stage (`-start abc`);
   - **P′**: Parmys's BLIF through Odin III's `read_blif; abc; write_blif --vtr`.

   Tolerance bands are decision 3.

Each sub-project gets its own spec (`docs/specs/`), plan, and PRs. Model and effort follow
`docs/PHASE0.md` §9: mapping algorithms Opus `xhigh` → `high` with the advisor; readers, writers
and harnesses Opus `high`; bulk configs Sonnet. The agent says when to switch.

## Sub-projects

| # | Sub-project | Depends on | Review | Model/effort |
|---|---|---|---|---|
| 3A | `flatten`: hierarchy → one flat top module; instance-path names, with IR-2 aliases for the joined port nets and the path kept in provenance; black boxes and library cells untouched | Phase 1 | light | Opus `high` |
| 3B | `lower`: every registry word cell → `bit` cells, using Yosys `simplemap`/`techmap.v` structures; the `$dff*` family → `$_DFF_P_` plus enable/reset logic, with async resets made synchronous as Parmys's `adff2dff` does (decision 19); a leftover `$mem` → FFs + muxes; the netlist view (IR-9) holds after it | Phase 1 registry; 2D IR change A cells | light (Peter: decision 19) | Opus `xhigh` → `high` |
| 3C | `hardmap`: the fixed Parmys-default binding (decision 4). `$mul` → `multiply`; `$add`/`$sub` → `adder` chains; `$mem` → FF merge, then `single_port_ram`/`dual_port_ram` slices (decision 6). Library cells come from the `.o3lib` (decision 5). Phase 4's `partial_map` replaces it | 3A; 2D `$mem` form | **Peter** (mapping algorithm, DESIGN §12) | Opus `xhigh` → `high`, advisor on |
| 3D | Linked ABC: `libabc-pic` in a C adapter (decision 8); the `abc` pass, covering exchange, cut points, clock domains, latches, determinism and provenance (decisions 9–14); the script from project, CLI or pass (decision 1) | Phase 1 only (BLIF-read netlists) | Peter for the IR/ABI pieces (pass-run args, `$lut`, adapter build); light otherwise | Opus `xhigh` → `high` |
| 3E | `write_blif --vtr`: VPR-legal BLIF (below) | 1C | light | Opus `high` |
| 3F | VTR flow hookup: the `tools/vtr-flow` wrapper and Odin III's own task configs, run through VTR's scripts read-only (decision 15) | 3E | light | Opus `high`; Sonnet for configs |
| 3G | `wreduce` (only if decision 7 = b) | 2F | light | Opus `high` |
| 3H | QoR harness and exit run: `tools/vtr-qor` (legs P, O, O′, P′; smallest first; caps), the table, the functional check, CI scope | grows alongside; the P leg runs now | light | Opus `high`; Sonnet for bulk |

**Lanes** (detail in "Order of work relative to Phase 2"):

- **Lane 1, back end** (needs nothing from Phase 2): 3E → 3D → 3F. Milestone **M1**: the P′ leg
  routes all 19.
- **Lane 2, front end**: 3A ‖ 3B → (3G) → 3C. Developed now on hand-built IR; it meets real
  designs once 2D/2E/2F land. Milestones **M2** (the O′ leg routes all 19) and **M3** (the O
  leg, which is the exit).
- **Lane 3**: 3H, starting with the P baseline today.

**3E `write_blif --vtr` rules** (the spec owns the detail): top model first, then a
`.model … .blackbox` stub per hard cell used, with the arch's port names; only `.names`,
`.latch`, `.subckt`, `.inputs`, `.outputs`, `.end` (no Yosys `.attr`/`.param`/`.cname`); at most
K inputs per `.names`; `.latch D Q re <clk> <init>` with init 2 for unknown (Parmys writes 2;
ABC's reader rejects 3); constants on `gnd`, `vcc` and `unconn` (`.names unconn` with no rows,
2D §14.6 item 3: VPR deletes that net and leaves its pins open, which is how Parmys pads RAM
addresses); short names by default. A falling-edge or level latch, a latch without a clock, a
tri-state or a multi-driver net is a located error.

## Checklist

- [ ] Decisions 2–20 answered (or kept as agent defaults); DESIGN §4.0 amended for decision 1
- [ ] 3H: Parmys baseline (P) on the 19, `-temp_dir ~/odin3-ws/work/vtr-qor`, table generator
- [ ] 3E `write_blif --vtr`: Parmys goldens round-trip through it and route via `-start abc`
- [ ] 3D linked ABC: adapter builds; `abc` pass; script ruling (#1); deterministic; **M1**: P′
  leg routes all 19
- [ ] 3F hookup: Odin III task configs run through `run_vtr_task.py` read-only
- [ ] 3A `flatten`
- [ ] 3B `lower` (simulator-equal before/after on hand-built and micro designs)
- [ ] 3G `wreduce` (if decision 7 = b)
- [ ] 3C `hardmap` (Peter reviews); **M2**: O′ leg routes all 19
- [ ] **M3 / exit**: O leg routes all 19; functional check; byte-identical reruns; QoR table
  within decision 3's bands, recorded here verbatim; DESIGN §13 #6 answered (name style)

## Decisions log

1. **The project file names the ABC script; the CLI can load one** (Peter, 2026-10-11: "the
   project file should describe which abc script to run (which also means there is a loading
   of an ABC script in the command line)"). 3D owns it. The details below are agent defaults
   for his review:
   - **`.o3proj`**: `abc_script <file>`, beside `arch` and the mapping rules in DESIGN §4.0.
     - It is a path, never inline text. ABC scripts are `;`/newline command lists, and inline
       text would need a second quoting layer.
     - The path resolves against the project file's directory, like every `.o3proj` path.
     - It may appear once ("`abc_script` given twice" is `syntax`). A missing file is
       `missing_file`. Any extension is accepted.
     - The record keeps the path as given and resolved. An `import`ed `.qsf` cannot set it,
       but the importing `.o3proj` can.
   - **Not in other formats**: `-f` lists (`unknown_option`), Quartus `.qsf`/`.qip` (no such
     assignment), Odin II XML (never had one).
   - **CLI**: `--abc-script <file>`, resolved against the working directory like other CLI
     paths.
   - **Precedence**, mirroring `--top` and `--revision`: a pass argument `abc --script <file>`
     beats the CLI's `--abc-script`, which beats the project's `abc_script`, which beats the
     built-in default (decision 10). `read_project` applies `--abc-script` to the record with
     the info line `<entry>: --abc-script <new> replaces the project abc_script <old>` when
     they differ. The pass argument affects that run only.
   - **Recording**: `dump_project` gains `"abc_script": <resolved path> | null`, right after
     `"flow"`. The corpus README key table and a fixture `o3proj_abc_script` get the row; its
     negatives cover "given twice", a missing file, and `-abc_script` in a `-f` list.
   - **Provenance**: the `abc` pass run records its resolved script path, the script's
     SHA-256, the ABC commit and K (decision 14). `write_blif --vtr` repeats the same facts in
     a header comment.
   - **Script language**: ABC command syntax (`;` or newline between commands, `#` to end of
     line), with a single substitution, `{K}` = the LUT size. K comes from `abc --lut <K>`;
     the VTR wrapper passes VTR's own `determine_lut_size(arch)`. Aliases (`resyn2`, `b`, `rw`,
     …) expand from a table copied from the pinned ABC's `abc.rc`, which linked ABC never reads
     (without it `resyn2` is unknown).
   - **Safety**: the whole script is read and checked before ABC starts. Errors are located
     `file:line`. The caps are `ODIN3_ABC_MAX_SCRIPT_BYTES` = 64 KiB, at most 1,024 commands
     after alias expansion, and a line of at most 4 KiB. Every command must be on the
     allow-list (decision 11). Each command runs through `Cmd_CommandExecute`, and a nonzero
     return is `abc: <file>:<line>: command '<cmd>' failed`.

## Decisions 2–20 (approved)

Peter approved every recommendation below on 2026-10-11 ("For all the questions - sounds good
and your ideas decisions seem fine"), together with the agent defaults in decision 1. The
numbers continue the log above. Each item gives the recommendation (now the decision) first,
then the options considered.

2. **Set and arch.** Recommend (a), because it is what DESIGN §11 names and what VTR itself
   tracks for QoR.
   - (a) The gate is the 19 on `k6_frac_N10_frac_chain_mem32K_40nm`. `arm_core` and `spree`
     are extra, non-gating rows, since `vtr_reg_qor_chain` runs them and they cost little.
     EArch runs the 5 smallest designs as a nightly smoke test.
   - (b) The 21 of `vtr_reg_qor_chain` as the gate.
   - (c) Start on `k6_frac_N10_mem32K_40nm`, the same arch without the `adder` model, so that
     Phase 3 needs no carry chains; move to (a) in Phase 4.
3. **Baseline and tolerance.** Recommend (b). Phase 4's exit is parity, so Phase 3 should
   catch a broken mapping without demanding parity. The O′/P′ legs say whether a miss comes
   from the front end or from ABC.
   - (a) Report only: the gate is P&R success plus the functional check.
   - (b) A loose gate. The geomean of O/P is at most 1.20, and no single design exceeds 1.50,
     on each of `num_clb`, `min_chan_width`, `critical_path_delay` and
     `crit_path_routed_wirelength`. `num_mult` and `num_memories` must be equal, or the
     difference is explained in the table. Everything else is report-only.
   - (c) VTR's `pass_requirements` bands (`num_clb` 0.9–1.1, CPD 0.5–1.4, min W 0.25–1.3,
     wirelength 0.6–1.5), made for one tool's drift across commits; too tight here.
4. **What "lower" covers in Phase 3 versus Phase 4.** Recommend (a). Of the 19, 10 use RAM
   slices, 10 `multiply` and 18 `adder`; soft multipliers and memories would blow up device
   size and runtime. Phase 4 keeps everything arch-driven: VPR-XML import, arch thresholds,
   project `map`/`split`/`limit` rules, cost models, width/depth trading, multiplier splitting,
   comparator chains, FSM, the matcher.
   - (a) Phase 3 has a fixed `hardmap` that copies Parmys's defaults and nothing more. `$mul`
     with both widths ≥ 3 (`min_hard_mult_size`) becomes one `multiply`, inputs extended to
     36 bits (sign-extended when signed). Every `$add`/`$sub` (`min_hard_adder_size` 1)
     becomes an `adder` chain in Parmys's form: a dummy head `a=vcc b=gnd cin=gnd` whose
     `cout` starts the chain, and subtraction as `a + ~b + 1`. `$mem` becomes RAM slices
     (decision 6). A `$mul` wider than 36×36 stays soft in Phase 3, with a warning; the 3C
     survey checks whether any of the 19 has one.
   - (b) Phase 3 maps only RAMs; adders and multipliers stay soft. `stereovision2` alone has
     468 multipliers.
   - (c) Phase 4's mapper comes first, which delays Phase 3 by a phase.
5. **How hard blocks are named.** Recommend (a). It is what VPR reads, the BLIF reader/writer
   already handles it, and Phase 4 only swaps the hand-written library for an imported one.
   - (a) Use the arch's model names (`adder`, `multiply`, `single_port_ram`, `dual_port_ram`)
     as `.o3lib` cells: `lib/vtr.o3lib` today (36×36 `multiply`, 15-bit RAMs), generated by
     Phase 4's VPR-XML importer later. 3H checks the library against the arch with VTR's own
     `determine_*` readers, so Phase 3 needs no C XML reader.
   - (b) Generic `$mul`/`$add`/`$mem` black boxes that the writer renames from a table.
   - (c) Custom arch files with `$mul` models; VTR's arch files have none.
6. **Memories before Phase 4 memory inference.** Recommend (a): the Parmys way. 2D emits one
   `$mem` per array, with asynchronous read ports and raw indices (EL-27). 2D gives "merge FFs
   into sync ports and map" to Phase 4 (§6.5); this moves a fixed subset of that into Phase 3.
   - (a) The steps:
     1. A `memory_dff`-style merge folds the output or address register into a sync read port.
     2. Single- or dual-port by port pattern, as Parmys's `block_memory.cc` (1r, 1rw, 2r,
        r+w, 2rw, …); more than two ports: one replica per read port.
     3. 1-bit data slices, depth split at the arch's 15 address bits, unused address bits on
        `unconn`.
     4. A read that cannot become synchronous leaves the `$mem` to `lower` (FFs + muxes),
        with a warning.
     5. INIT contents are dropped with a warning naming the memory, as Parmys drops them
        (`ch_intrinsics` is the "Memory Init" benchmark); the functional check lists these.
   - (b) All memories soft: infeasible for `LU32PEEng` (5,251 slices in Parmys) and `mcml`.
   - (c) Pass `$mem` through as a black box: VPR has no such model.
7. **Must `opt` reach Parmys's level first?** Recommend (b). Widths decide hard-block counts:
   `wreduce` narrows multipliers below the 3-bit threshold, shortens adder chains and RAM
   widths, so without it `num_mult`/`num_memories` and the CLB count drift for reasons
   unrelated to the mapping. The rest only moves soft-logic QoR, which ABC partly absorbs.
   - (a) 2F's `opt` as it is.
   - (b) Add `wreduce` (3G) before `hardmap`; leave `share`, `peepopt`, `fsm`,
     `opt_dff -sdff/-dffe` and `memory_share` to Phase 4 and report their absence as a cause.
   - (c) Reach Parmys parity (`share`, `fsm`, `peepopt`, …) in Phase 3.
8. **How ABC is linked, and the pin.** Recommend (a). It keeps ABC's `exit()` calls (34 files)
   and global symbols out of libodin3, so `symbol_rules` and the exports check stay unchanged
   (no gate weakening). The shape is the slang adapter's (DESIGN §4.2).
   - (a) Build ABC from the `third_party/abc` submodule (upstream `a3001b72`, already pinned
     in ORACLES.md) as `libabc-pic`, linked into a separate C adapter `libodin3_abc.so`
     (`adapters/abc/`) that registers the `abc` pass. ABC's symbols are hidden with
     `--exclude-libs`. The adapter gets its own allowance for ABC's `exit`/`abort`, and the
     `odin3` CLI links it.
   - (b) Link libabc into libodin3, which needs an allowance in `check-symbols.sh` and export
     filtering: a gate change.
   - (c) Use VTR's in-tree ABC: a 2023-11 upstream snapshot plus VTR patches (613 `diff -rq`
     entries against ours under `src/`); linking from `external/` is fragile, copying forks a
     fork.

   The pin bumps only with an ORACLES change. The version gap to VTR's ABC is a QoR confound,
   which the P′ leg measures.
9. **IR ↔ ABC exchange.** Recommend (b) as the bring-up path first, then (a) after a spike.
   (b) reaches M1 with proven code and stays as `abc --via-blif <dir>`, the differential and
   debug mode. (a) is the production path: no temporary files, no 100 MB BLIF parses, exact
   provenance.
   - (a) MiniAIG in, MiniLUT out, through ABC's public `abcapis.h` (header-only structs with
     registers). `Abc_NtkInputMiniAig` loads the network space, so VTR's script runs as is;
     `Abc_FrameGiaOutputMiniLut`/`…MiniLutObj` return the LUTs with an object correspondence.
     The spike must show `&get -m` after `mfs2` keeps latches and mapping, else (c).
   - (b) BLIF text through a per-run temporary directory: `read`, the script, then
     `write_hie`, exactly VTR's stage, read back by 1C's reader.
   - (c) Build `Abc_Ntk_t` through ABC's internal `base/abc/abc.h`: full control, but it
     couples to ABC internals.

   Either way, LUTs come back as a new `$lut` registry cell (DESIGN §5.2's `$_LUT_K_`, Yosys
   `$lut` semantics), which `--vtr` writes as an irredundant `.names` cover. It is an IR
   addition for Peter's review.
10. **Default ABC script when neither project nor CLI names one.** Recommend (a), shipped as
    `lib/abc/vtr.abc` with the VTR commit cited. It is exactly what the Parmys baseline runs,
    so the script is not a confound and only the ABC version is. (Parmys's own Yosys script
    has no `abc` call; the "Parmys script" is VTR's ABC stage.)
    - (a) VTR's current ABC-stage script: `strash; ifraig -v; scorr -v; dc2 -v; dch -f; if -K
      {K} -v; mfs2 -v`.
    - (b) VTR's old script (`-use_old_abc_script`): `resyn; resyn2; if -K {K}; scleanup`.
    - (c) Yosys's built-in `abc -lut` script, which retimes (`dretime`).
11. **Allowed ABC commands.** Recommend (a). A deny-list misses aliases and new commands, and
    `quit` inside linked ABC calls `exit()` on Odin III.
    - (a) An allow-list, checked after alias expansion:
      - optimisation (`strash b rw rwz rf rfz rs rsz dc2 drw drf dch ifraig fraig
        fraig_sweep scorr sweep cleanup scleanup`);
      - mapping (`if mfs mfs2 lutpack`);
      - the `&` space (`&get &put &st &synch2 &dch &if &mfs`);
      - printing (`print_* time echo`).

      Everything else is refused: files (`read*`, `write*`, `r`, `w`, `source`, `load`,
      `save`), the shell and the process (`!`, `quit`/`q`, `exit`, `cd`, `ls`, `alias`,
      `unalias`, `set`, `history`), and latch semantics (decision 12).
    - (b) A deny-list of the dangerous commands only.
    - (c) No check; scripts are trusted.
12. **Latches across ABC (retiming and init values).** Recommend (a): retiming would break init
    values, latch provenance and FF-count parity with a baseline that never retimes (all
    172,301 latches in the 31 `vtr/` Parmys goldens are `re <clk> 2`).
    - (a) Retiming commands are refused (`retime dretime fretime fr r3*`, and `zero init undc
      cycle`). In-domain latches enter ABC with init 0, 1 or DC. ABC treats DC as 0
      (`abcDar.c`: "the don't-care are assumed to be 0"), as in VTR's flow, and they are
      written back as 2, as Parmys writes them. An exchange without init values (MiniAIG)
      inverts init-1 latches around the boundary and restores them after.
    - (b) Opt-in `abc --retime`, with init values recomputed or set to DC. Phase 4 or later.
    - (c) Cut every latch, so ABC runs combinationally: no `scorr`, worse QoR.
13. **Black boxes and clocks across ABC.** Recommend (a). It matches the baseline and is
    correct for multiple clocks. ABC knows one implicit clock, and `stereovision3`
    (`tm3_clk_v0`, `tm3_clk_v2`) is the only two-clock design of the 19.
    - (a) Hard-block outputs become ABC primary inputs, hard-block inputs ABC primary outputs.
      ABC runs once per clock domain (clock net, edge), with other domains' latches cut the
      same way: VTR's `iterative_bb`. Re-stitching from the IR avoids the output stripping VTR
      patches with `restore_outputs_from_blif`.
    - (b) One run with every latch cut (`blanket_bb`).
    - (c) One run that ignores clocks (`once_bb`), which is wrong for multi-clock designs.
14. **Provenance across ABC (D9).** Recommend (a). Source-line to VPR-block tracing (D9) is the
    point of provenance, and ABC is where it would otherwise end.
    - (a) Each new LUT or latch gets a `DERIVED` record. Its parents are the old cells in its
      cone, from ABC's object correspondence (decision 9a), or for `--via-blif` from the
      cut-bounded cone of its named output. IR-12 is amended so that every pass run records
      its argument string (strtab ID) in the pass-run table. `abc` adds `script=<path>
      sha256=<hex> abc=<commit> K=<K>`.
    - (b) Run-only records with no source parents.
    - (c) Per-primary-output cones: coarse.
15. **Hooking Odin III into the VTR flow.** Recommend (a). It uses VTR's flow, parsers, QoR
    configs and pass requirements unmodified, as the read-only rule requires.
    - (a) Wrap; do not modify: `run_vtr_flow.py <odin3 BLIF> <arch> -start vpr …`. Task configs
      live in our repo (`tests/vtr/tasks/<task>/config/config.txt`, absolute `circuits_dir`)
      and run as `run_vtr_task.py <abs path> -temp_dir ~/odin3-ws/work/vtr-qor`; VTR accepts
      absolute task paths, and `-temp_dir` keeps run directories out of `external/`.
    - (b) Add an `odin3` stage to `run_vtr_flow.py`. That edits `external/`; an upstream PR is
      outward-facing and is Peter's call, and could come later.
    - (c) Abuse `-yosys_script` to run Odin III inside the `parmys` stage.
16. **Functional check in the exit.** Recommend (a). VTR ships the check with RAM, multiplier
    and adder models (`models/sim_hardblocks.v`); `equiv-check` has no RAM model (PHASE0 #8)
    and 1E excludes RAMs.
    - (a) Two checks:
      - VTR's own random check, `mosaic/verilator_check/run_random_check.py --rtl <design>.v
        --post-synth-blif <odin3 BLIF>` (Verilator; 50,000 vectors, seed 1), run read-only;
      - inside Odin III, `equiv-check` before and after `abc` on every design without RAMs.

      Declared divergences are listed per design: dropped memory init (decision 6) and async
      reset made synchronous (decision 19).
    - (b) `equiv-check` only.
    - (c) P&R success only.
17. **Runtime and memory on the WSL VM** (24 GB, 12 threads; PHASE0 #29). Recommend (a). It
    keeps the parallel total at or below 60% of RAM, and the four largest designs carry most
    of the risk (VTR's golden `max_vpr_mem`: `LU32PEEng` 2.3 GB, `mcml` 2.2 GB; `LU32PEEng`
    min-W routing alone 1,140 s).
    - (a) Run smallest first, by Parmys golden size:
      1. `ch_intrinsics`, `stereovision3`, `diffeq2`, `diffeq1`, `mkPktMerge`, `raygentop`,
         `sha`, `boundtop`, `mkSMAdapter4B`, `or1200`, `stereovision0`, `stereovision1`,
         `mkDelayWorker32B`, `stereovision2`, `blob_merge` at `-j3`;
      2. then `LU8PEEng`, `bgm`, `mcml` and `LU32PEEng` at `-j1`.

      Each job is capped (`prlimit` RSS 6 GB; `run_vtr_flow -limit_memory_usage`;
      `-timeout` 2 h). Successful run directories are deleted down to their logs and QoR
      files, and the run pauses below 20 GB free on `C:`. The P leg is cached per VTR commit.
    - (b) Everything at `-j1` (about 4–5 h per set of legs). (c) A cloud runner.
18. **CI scope.** Recommend (a). VTR takes about an hour to build per cache miss, which does not
    fit `ci`.
    - (a) `ci` runs:
      - the unit and fixture tests of 3A–3E (ABC is already built in CI);
      - a VPR-free structural checker for `--vtr` output (K-bounded `.names`, `re` latches,
        model and port names checked against a committed copy of the arch's `<models>`);
      - `equiv-check` before and after `abc` on micros.

      VPR legs run nightly on the WSL machine through `tools/vtr-qor`. The QoR table is
      committed to `odin3-golden` and summarized here.
    - (b) Also build VPR in GitHub CI (cached) and route the 5 smallest designs nightly.
    - (c) A self-hosted runner on the WSL machine (Peter's hands; outward-facing).
19. **FF normalization in `lower`.** Recommend (a). The arch's FF has no async reset or enable
    (`.latch … re` only), and the baseline does exactly this, so parity and the functional
    check both need it.
    - (a) Mirror Parmys's `adff2dff`/`adffe2dff`/`aldff*2dff` and `dffunmap`. Async
      set/reset/load becomes a mux in front of a `$_DFF_P_`, which makes the reset synchronous.
      This is a declared semantic divergence, warned once per design and listed. A falling
      edge gets a clock inverter LUT with a warning (Odin II's way; none of the 19 needs it).
    - (b) An async reset is a located error.
    - (c) Keep async FFs, which VPR rejects on this arch.
20. **Phase 2 carry-overs that block Phase 3.** Recommend (a). Lanes 1 and 3 need nothing from
    Phase 2, and nobody owned `flatten` before.
    - (a) Only the front-end lane waits, at M2: 2D, 2E and 2F merged, and 2H's 19 VTR rows
      identical or equivalent (PHASE2 #4).
      - The x → `unconn` writer convention (2D §14.6 item 3) lands in 3E if 2F has not done it.
      - `flatten` moves into Phase 3 (3A).
      - Not blocking: fuzzing, micro exceptions, the `--x-report` miter, the Odin II XML reader,
        the 2A plugin `wants_ast` ABI, the 1C writer follow-ups.
    - (b) Phase 2 must be fully closed before any Phase 3 work starts.
    - (c) Feed the front-end lane Yosys word-level BLIF (`write_blif -param`, read by 1C as
      registered `$add`… cells) until 2E/2F land; a spike at most (`$mem_v2`, `$sdffe` don't
      match our registry).

## Risks

1. **ABC global state.** `Abc_Start` runs once per process. The frame is thread-local in this
   ABC (`ABC_THREAD_LOCAL`), but other statics are not. ABC prints to stdout, and some error
   paths `exit()`. Mitigations: one ABC session, serialized; `Abc_FrameDeleteAllNetworks`
   between runs; stdout redirected to the run's `abc.log`; `quit` refused (decision 11). A
   crash in ABC kills `odin3`; if that is seen, the fallback is ABC as a subprocess, which is a
   change to D3.
2. **The symbol gate.** libabc imports `exit`/`abort` and exports its symbols. Linked naively,
   it fails `symbol_rules` and the exports check (decision 8).
3. **VPR rejects our BLIF.** VPR accepts only rising-edge latches that have a clock
   (`read_blif.cpp`). Other causes: more than K inputs on a `.names`, model or port names
   absent from the arch, Yosys extension directives, names containing `#` or blanks,
   multi-driver nets. Mitigations: 3E's checker, then M1 (Parmys goldens through `--vtr` and
   VPR) before any Odin III front-end netlist reaches VPR.
4. **QoR gaps from what Phase 4 is missing:** no `share`/`fsm`/`peepopt`, fixed RAM slicing,
   no multiplier splitting, comparators kept soft. Also ABC's version: VTR's is a 2023
   snapshot. The O′ and P′ legs attribute each gap; decision 3's bands are loose for this
   reason.
5. **RAM correctness is not covered by our own oracles.** There is no RAM model in
   `equiv-check` or in 1E. Only the Verilator check (decision 16) sees RAMs. Dropped memory
   init is a declared divergence.
6. **The async-to-sync reset divergence** (decision 19) makes Odin III's netlist differ from the
   RTL on reset timing. The functional check must drive resets synchronously, or list the
   affected designs.
7. **The WSL VM.** An OOM or a full `C:` kills the VM (PHASE0 #29); VPR run directories reach
   hundreds of MB and `C:` is 74% full. Decision 17's caps and cleanup address this.
8. **Determinism.** Map-iteration order in Odin III passes, or ABC commands with internal
   randomness, could break byte-identical reruns. The exit asserts identical reruns, and ABC's
   input is in canonical node-ID order.
9. **Name style** (DESIGN §13 #6): long provenance names may cost VPR memory and time; 3H
   measures both styles on the 19, and short stays the default until then.
10. **VTR drift.** We depend on `-start vpr`, the parse configs and `run_random_check.py` at
    `3c9a4d23`; a VTR bump is an ORACLES change that reruns the P leg.

## Order of work relative to Phase 2

**Now, with no Phase 2 dependency:**

- **3H**: the Parmys baseline (P) on the 19, smallest first, and the table generator.
- **3E**: the Parmys goldens are already pre-ABC VTR netlists: read them, write `--vtr`, route
  through VTR's ABC stage (`-start abc`).
- **3D**: the adapter, then `abc` with BLIF exchange (decision 9), on the Parmys goldens:
  `read_blif; abc; write_blif --vtr`, then VPR. This is the P′ leg and **M1**: the back half
  of Phase 3 proven on real VTR netlists before 2E/2F land.
- **3A** on the hierarchical BLIF goldens (multi-model Odin II micros); **3B** on hand-built
  word IR, before/after checked by 1E's simulator on random vectors (the 2D cells join when
  2D-1 lands); **3C** on hand-built `$mul`/`$add`/`$mem` (EL-27/39 fix the `$mem` form).

**After 2D/2E/2F land, and 2H's VTR rows pass:** the front end runs on the 19 → 3G → 3C on real
designs. Then **M2** (the O′ leg routes) and **M3** (the O leg with linked ABC routes; the
functional check; the table), which is the exit.
