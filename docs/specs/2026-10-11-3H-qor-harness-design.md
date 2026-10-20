# 3H — QoR harness and exit run: design

Status: v2.1 (2026-10-11), approved in re-review 2 with touch-ups applied (exit estimate ≈ 20–30 h
with its breakdown, a starvation guard on the group B slot, hand starts as a transient user
service, the `stereovision0` equiv fallback, wording). v2: v2 applies review 1 (`3EH-review1`: 7 important, 13 minor; dispositions
in §21) and the controller's rulings: the nightly rotates the four biggest *designs* with a
realistic budget and a 06:00 admission guard, and runs start by hand until Peter approves the
timer (H1); a missing gated metric fails the gate (H2); the run directory is wiped before each
attempt (H3); P's `pre-vpr.blif` is kept (H4); `equiv` is in the exit gate, with the Verilator
check and a recorded exception for stereovision3 and the multiplier designs, and fcheck exit
codes mapped (H5/H6); a first measurement of the unmeasured group B stages under the 6 GB scope
(H7). v1 was the agent draft for light review (PHASE3 3H row: light; Opus `high`, Sonnet for
bulk). Phase 3, sub-project 3H, lane 3: it starts now with the Parmys baseline (the P leg) and
grows alongside 3A–3G until it runs the exit (M3). Spec: `docs/DESIGN.md` §11 ("VTR 19 … QoR
table"), §12 (Phase 3 exit), §13 #6 (name style). Inputs: `docs/PHASE3.md` in full, especially the
exit test (items 1–5) and decisions 2, 3, 5, 15, 16, 17, 18 (all approved 2026-10-11); PHASE0 #29
(WSL caps) and §9 (model/effort); `docs/ORACLES.md`; the golden layout (odin3-golden README); the
3E spec (`write_blif --vtr`, `blif-vtr-check`, `vtr-models`); `tools/run-oracle.sh` (the capped-run
precedent); the standing rules' heavy-run rule (Peter, 2026-10-11). **Oracle: VTR `3c9a4d23`**:
`vtr_flow/scripts/run_vtr_flow.py`, `run_vtr_task.py`, `python_libs/vtr/{flow,task,util,
parse_vtr_flow,parse_vtr_task,log_parse}.py`, `vtr_flow/parse/{parse_config,qor_config,
pass_requirements}/`, the task `vtr_reg_nightly_test3/vtr_reg_qor_chain`, and
`mosaic/verilator_check/run_random_check.py`. External trees are read-only; every VTR script is
used, never edited (decision 15). Decisions are numbered **QH-n**.

## 1. Purpose and scope (QH-1)

`tools/vtr-qor` runs Odin III and the Parmys baseline through VTR on the 19 VTR 7.0 benchmarks,
collects VTR's own QoR metrics, and produces the table and verdict of the Phase 3 exit (PHASE3
exit item 4, decision 3), under the WSL limits of decision 17 and the heavy-run rule.

| Delivered by 3H | Where |
|---|---|
| The harness: legs P, O, O′, P′ (and R, Oprov), job keys and cache, VTR parse, netlist statistics | `tools/vtr-qor/` |
| Heavy-run scheduling: one lock, one capped scope, smallest first, `-j3` then `-j1`, OOM and timeout handling, disk pause, cleanup | `tools/vtr-qor/qor_sched.py`, `qor_heavy.py` |
| The table (Markdown and CSV, geomean, gate, attribution) and the publish step | `tools/vtr-qor/qor_table.py` |
| The functional check (decision 16), rerun determinism, `equiv-check` collection | `tools/vtr-qor/` |
| Design sets, order, bands, explanations, declared divergences | `tests/vtr/qor/` |
| Nightly local runs (started by hand; systemd timer files for Peter to install when he approves) | `tools/vtr-qor/systemd/` |
| CI scope (decision 18) | `.github/workflows/ci.yml` (unittest only), `tests/tools/test_vtr_qor*.py` |
| The first P-leg run of the 19, now | §12 |

Not in 3H: Odin III's flow script and task configs (3F, decision 15; 3H defines the leg commands
in one module that 3F imports, §3.7); the `--vtr` writer and its checker (3E); building VTR; any
VTR change; a cloud or self-hosted runner (decision 17c, 18c); VPR in GitHub CI (decision 18).

## 2. Designs, order and arches (QH-2)

**Gate set** (decision 2a): the 19. **Extra** (non-gating rows): `arm_core`, `spree`. **Smoke**
(EArch, nightly): the 5 smallest. **Order** (decision 17a): ascending size of the Parmys golden
BLIF on k6_frac (`~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm/vtr/<d>/<d>.parmys.blif`,
odin3-golden `3931cf4`); the extras slot in by the same rule. Group A runs at `-j3`, group B at
`-j1`. Reference costs are VTR's own `golden_results.txt` for `vtr_reg_qor_chain` (VTR's CI
runner, VPR revision `85125f5`; a cross-check only, never a baseline).

| Rank | Design | Group | Golden BLIF (bytes) | VTR flow time (s) | max VPR mem (MiB) | num_clb | min W |
|---|---|---|---|---|---|---|---|
| 1 | `ch_intrinsics` | A | 59,473 | 3.0 | 70 | 68 | 40 |
| 2 | `stereovision3` | A | 113,057 | 2.3 | 67 | 15 | 26 |
| 3 | `diffeq2` | A | 127,006 | 9.7 | 75 | 23 | 58 |
| 4 | `diffeq1` | A | 163,388 | 8.9 | 77 | 35 | 62 |
| 5 | `mkPktMerge` | A | 384,705 | 13.6 | 93 | 34 | 40 |
| — | `spree` (extra) | A | 431,216 | 12.7 | 83 | 63 | 58 |
| 6 | `raygentop` | A | 689,323 | 19.3 | 100 | 141 | 58 |
| 7 | `sha` | A | 697,688 | 14.1 | 98 | 131 | 66 |
| 8 | `boundtop` | A | 1,110,434 | 18.0 | 79 | 52 | 38 |
| 9 | `mkSMAdapter4B` | A | 1,385,687 | 28.0 | 103 | 176 | 46 |
| 10 | `or1200` | A | 1,497,595 | 38.9 | 125 | 265 | 78 |
| 11 | `stereovision0` | A | 2,651,591 | 78.5 | 281 | 706 | 46 |
| 12 | `stereovision1` | A | 2,757,780 | 121.5 | 351 | 806 | 74 |
| — | `arm_core` (extra) | A | 3,376,843 | 226.8 | 343 | 853 | 96 |
| 13 | `mkDelayWorker32B` | A | 4,467,574 | 71.9 | 310 | 481 | 40 |
| 14 | `stereovision2` | A | 4,977,084 | 433.3 | 1,162 | 1,940 | 86 |
| 15 | `blob_merge` | A | 6,723,686 | 84.6 | 186 | 502 | 64 |
| 16 | `LU8PEEng` | B | 16,900,911 | 532.9 | 766 | 2,144 | 88 |
| 17 | `bgm` | B | 23,271,051 | 548.1 | 947 | 2,777 | 84 |
| 18 | `mcml` | B | 51,063,332 | 4,699.4 | 2,186 | 6,773 | 120 |
| 19 | `LU32PEEng` | B | 59,566,477 | 3,236.8 | 2,256 | 7,483 | 100 |

The smoke set is ranks 1–5. `tests/vtr/qor/sets.json` holds this table's first five columns
(`{"schema": 1, "order": [...], "group_b": [...], "gate": [...], "extra": [...], "smoke": [...],
"sizes_from": "odin3-golden 3931cf4"}`); the harness never re-derives the order at run time, so
runs are comparable.

**Arches**: `k6` = `vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml` (gate);
`earch` = `vtr_flow/arch/timing/EArch.xml` (smoke). K = `determine_lut_size(arch)` = 6 for both,
read once per run through VTR's Python (3E §15.2).

## 3. The legs (QH-3)

Notation: `$V` = `$VTR_ROOT` (default `~/odin3-ws/external/vtr-verilog-to-routing`); `$PY` =
`$V/.venv/bin/python`; `<arch>` = the arch XML's absolute path; `<d>` the design; `<J>` the job
directory (§5); `<K>` = 6; `$O3` = the `odin3` binary of `--odin3` (default
`<repo>/build/release/src/cli/odin3`); `$LIB` = `<repo>/lib/vtr.o3lib`. Every VTR invocation
appends the **common VPR parameters**, those of `vtr_reg_qor_chain` plus the harness's
housekeeping:

```
-track_memory_usage -crit_path_router_iterations 100        # vtr_reg_qor_chain script_params
-temp_dir <J>/run -timeout <T> -delete_intermediate_files -delete_result_files   # harness
```

`<T>` is 7,200 s for group A (decision 17) and 14,400 s for group B (review H20: mcml's ABC
alone takes ≈ 57 min on VTR's runner, ≈ 85 min at × 1.5, and a timeout costs a 2 h+ rerun). Each
attempt runs `safe_rmtree(<J>/run)`, `mkdir -p <J>/run`, then `cd <J> && $PY
$V/vtr_flow/scripts/run_vtr_flow.py … > <J>/run/vtr_flow.out 2>&1`, with `PATH=$V/.venv/bin:$PATH`
(§7.4 item 0). In `flow.py`, `delete_intermediate_files()` runs only with
`-delete_intermediate_files` (`keep_intermediate_files` defaults to true); with that flag it
deletes the last-stage netlist and the copied `.xml/.sdf/.v/.sv`, and with
`-delete_result_files` also `.net/.place/.route`. It keeps `<d>.pre-vpr.blif` (the netlist VPR
read), which §4.1 reads and §7.5 keeps for P (gzipped) and deletes for the other legs.

### 3.1 P — the Parmys baseline (decision 3, exit item 5)

```
$PY run_vtr_flow.py $V/vtr_flow/benchmarks/verilog/<d>.v <arch> <common>
```

VTR's default flow: `parmys`, then VTR's ABC stage (iterative per-clock black-boxing, the script
`strash; ifraig -v; scorr -v; dc2 -v; dch -f; if -K 6 -v; mfs2 -v`), then VPR (min-W binary
search, then routing at 1.3 × min W with timing). Front-end cost = the `parmys` stage plus every
`abc<i>` stage (§4.4). Cached per job key (§5.3): it reruns only when VTR, the arch, the design
source or the parameters change (decision 17: "cached per VTR commit").

### 3.2 O — Odin III alone (exit item 3)

```
/usr/bin/time -v -o <J>/frontend.time $O3 --diag-paths resolved -p "read_techlib $LIB; \
  read_verilog --nolatches $V/vtr_flow/benchmarks/verilog/<d>.v; proc; opt; flatten; wreduce; \
  hardmap; lower; opt -fast; compact; abc --lut <K>; write_blif --vtr --lut-k <K> <J>/<d>.blif" > <J>/frontend.log 2>&1
$PY run_vtr_flow.py <J>/<d>.blif <arch> -start vpr <common>
```

The script is PHASE3 exit item 3's, with `wreduce` (decision 7b) and the ABC script left to the
default (`lib/abc/vtr.abc`, decision 10). Front-end cost = `frontend.time`. Available once
`odin3 --help` lists every pass in the script (§9, leg availability).

### 3.3 O′ — Odin III's front end, VTR's ABC (control leg)

```
/usr/bin/time -v -o <J>/frontend.time $O3 … hardmap; lower; write_blif --vtr --lut-k <K> <J>/<d>.blif"
$PY run_vtr_flow.py <J>/<d>.blif <arch> -start abc <common>
```

The same front end as O up to `lower`, then VTR's ABC stage. Front-end cost = `frontend.time` +
the `abc<i>` stages. Available once `flatten`, `hardmap`, `lower` (and the 2D–2F passes) are
registered.

### 3.4 P′ — Parmys's front end, Odin III's ABC (control leg; M1)

```
G=~/odin3-ws/golden/<arch base>/vtr/<d>/<d>.parmys.blif   # verified, below
/usr/bin/time -v -o <J>/frontend.time $O3 --diag-paths resolved -p "read_techlib $LIB; \
  read_blif $G; abc --lut <K>; write_blif --vtr --lut-k <K> <J>/<d>.blif"
$PY run_vtr_flow.py <J>/<d>.blif <arch> -start vpr <common>
```

`G` is used only when its `.prov` says `status=ok`, `vtr_commit` equals ORACLES.md's pin, `arch`
names the same XML and `blif_sha256` matches the file; otherwise the job is `skipped` with E-QH6.
Front-end cost = Parmys's (from the P leg's `parmys.out`) + `frontend.time`; the table shows both
parts. Available once `abc` is registered (3D).

### 3.5 R — the 3E round trip (evidence, not a gate)

`read_techlib $LIB; read_blif $G; write_blif --vtr --lut-k <K> <J>/<d>.blif`, then `-start abc`.
It shows that `--vtr` does not perturb the baseline (3E §16.5): the same netlist as P's input to
ABC up to names and line order.

### 3.6 Oprov — the name-style variant (DESIGN §13 #6, PHASE3 risk 9)

O with `write_blif --vtr --name-style provenance`. Reported only in the table's "Name style"
section (VPR time and memory, provenance / short). Run once at M3 and on request.

### 3.7 One definition of the commands

`tools/vtr-qor/qor_legs.py` builds every command above (`build_commands(leg: str, design: str,
ctx: LegContext) -> list[Command]`, `odin3_script(leg: str, design_v: Path, out_blif: Path, k: int,
name_style: str) -> str`). 3F's `tools/vtr-flow` imports `odin3_script` so the wrapper and the
harness cannot drift; 3F's task configs (`tests/vtr/tasks/…`) are checked against the harness by
`vtr-qor crosscheck` (§15).

## 4. Metrics (QH-4)

Every job ends with `<J>/metrics.json` (§5.2). Sources:

### 4.1 Netlist (from `<J>/run/<d>.pre-vpr.blif`; `qor_netstats.py`)

A streaming reader (one pass for counts, then a Kahn pass for depth; no recursion; about 120 B per
net, so LU32PEEng's 380 k `.names` fit easily in the scope):

| Metric | Definition |
|---|---|
| `luts` | `.names` with at least one input, excluding constant generators (exit item 4) and identity buffers (one input, cover `1 1` or `0 0`), which VPR absorbs (`absorb_buffer_luts`) |
| `buffers` | the identity buffers excluded above (ABC's `write_hie` PO buffers, 1C's port buffers), reported separately |
| `ffs` | `.latch` lines |
| `adders`, `mults` | `.subckt adder`, `.subckt multiply` |
| `ram_slices` | `.subckt single_port_ram` + `.subckt dual_port_ram` |
| `depth` | the longest path counted in `.names` with ≥ 1 input that are not identity buffers, from primary inputs, latch outputs, hard-block outputs and constants to primary outputs, latch inputs and hard-block inputs (hard blocks cut, as VTR's iterative black-boxing does); `-1` with a warning on a combinational loop |
| `abc_depth` | VTR's own `abc_depth` (`abc0.out` `lev = N`) for P, O′ and R; for O and P′ the final depth line 3D is asked to log (3E §14.1 item 6), empty until it exists |
| `blif_sha256` | of the pre-VPR BLIF (and of `<J>/<d>.blif`, which equals it for `-start vpr`) |

`depth` is computed the same way for every leg, so it is the comparable column; `abc_depth` is
VTR's figure, reported for reference.

### 4.2 Packed, routed (VTR's parse)

```
PYTHONPATH=$V/vtr_flow/scripts/python_libs $PY \
  $V/vtr_flow/scripts/python_libs/vtr/parse_vtr_flow.py <J>/run \
  $V/vtr_flow/parse/parse_config/vpr_standard.txt arch=<arch>.xml circuit=<d>.<v|blif> \
  script_params=common > <J>/run/parse_results.txt
```

and the same with `$V/vtr_flow/parse/qor_config/qor_standard.txt` into `qor_results.txt`: exactly
the per-job parse `run_vtr_task.py` performs (`task.py` `create_cmd`), so values are VTR's.

| Metric | VTR key (file) |
|---|---|
| `vpr_status` | `vpr_status` (`output.txt`) |
| `num_clb`, `num_memories`, `num_mult` | same (`vpr.out`, `vtr_benchmarks.txt`) |
| `device_width`, `device_height` | same (`vpr.out`); the table prints `W×H` |
| `min_chan_width` | same (`vpr.out`) |
| `critical_path_delay_ns` | `critical_path_delay` (`vpr.crit_path.out`: the relaxed-width route, exit item 4) |
| `fmax_mhz` | 1000 / `critical_path_delay_ns` |
| `crit_path_routed_wirelength` | same (`vpr.crit_path.out`) |
| `max_vpr_mem_mib` | `max_vpr_mem` kB / 1024 (`vpr.out`; the min-W run, VTR's convention) |
| `vpr_time_s` | sum of `The entire flow of VPR took (.*) seconds` in `vpr.out` and `vpr.crit_path.out` (VTR's `total_runtime` covers `vpr.out` only; it is kept as `vpr_total_runtime_s`) |

### 4.3 Odin III's front end

`frontend.time` is GNU `time -v` output: `frontend_wall_s` from `Elapsed (wall clock) time`,
`frontend_peak_rss_mib` from `Maximum resident set size`.

### 4.4 VTR stages

`parmys.out`, every `abc<i>.out`, and the ABC stage's helper logs (`<i>_blackboxing_latch.out`,
`restore_latch<i>.out`, `report_clocks.abc.out`: the Perl passes VTR runs over the whole BLIF for
each clock domain) carry `time -v` output (`-track_memory_usage`). Wall times are summed over all
of them, peak RSS is the maximum; a log without `time -v` lines is reported as
`vtr-qor: <L> <d>: no time -v in <file>; stage cost incomplete` and the cost cell is marked `*`. P: parmys + abc; O′ and R: odin3 + abc; P′: parmys (from the P
job of the same key family) + odin3.

### 4.5 Functional and structural verdicts (§8)

`fcheck` (`pass|fail|error|skipped`, §8.1), `equiv` (`equivalent|differs|ram|multiclock|timeout|
error|skipped`, §8.2), `rerun` (`same|differs|skipped`).

## 5. Where results live (QH-5)

### 5.1 Three places

| Place | What | Who writes |
|---|---|---|
| **odin3 repo** | inputs only: `tests/vtr/qor/{sets.json,bands.json,explanations.tsv,divergences.tsv,equiv-exceptions.tsv}`, the harness, its tests; and **summaries** in `docs/PHASE3.md` (the P baseline once, M1, M2, and the M3 exit table verbatim, as the PHASE3 checklist requires) | agents, through PRs |
| **`~/odin3-ws/work/vtr-qor/`** (`$ODIN3_QOR_DIR`) | every job, cache, log, table draft, nightly report | the harness |
| **odin3-golden** (`~/odin3-ws/golden/qor/`) | each *published* table (decision 18: "the QoR table is committed to odin3-golden") | the controller only, by `vtr-qor publish`; implementer agents never write `~/odin3-ws/golden` (standing rules) |

### 5.2 Work layout

```
~/odin3-ws/work/vtr-qor/
  <vtr12>/<arch>/<leg>/<key12>/<d>/      one job (<vtr12> = 3c9a4d23b27d)
    job.json            the job: leg, design, key inputs, every command line, env, start/end
    status              one word: ok | failed | capped | timeout | skipped | aborted
    frontend.time  frontend.log  <d>.blif.sha256    (O, O′, P′, R, Oprov)
    run/                VTR's temp_dir, pruned (§7.6)
    metrics.json        §4, schema 1
    fcheck/             Verilator work dir (deleted on pass)
  <vtr12>/<arch>/<leg>/latest            text file: the newest complete key12
  tables/<YYYYMMDD-HHMM>-<label>/{qor.md,runs.csv,ratios.csv,runs.json}
  nightly/<YYYYMMDD>/{summary.md,nightly.log}
  harness.log                            one line per job start/end, append-only
```

No symlinks (standing rules); `latest` is a file. `metrics.json`:

```json
{"schema": 1, "leg": "P", "design": "sha", "arch": "k6_frac_N10_frac_chain_mem32K_40nm",
 "key": "<sha256>", "status": "ok", "gating": true,
 "vtr_commit": "3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b", "odin3_commit": null,
 "odin3_build": null, "abc_script_sha256": null, "started": "<UTC ISO-8601>", "elapsed_s": 14.2,
 "netlist": {"luts": 0, "ffs": 0, "adders": 0, "mults": 0, "ram_slices": 0, "depth": 0,
             "abc_depth": 0, "blif_sha256": "<hex>"},
 "packed": {"num_clb": 0, "num_memories": 0, "num_mult": 0, "device_width": 0,
            "device_height": 0},
 "routed": {"vpr_status": "success", "min_chan_width": 0, "critical_path_delay_ns": 0.0,
            "fmax_mhz": 0.0, "crit_path_routed_wirelength": 0},
 "cost": {"frontend_wall_s": 0.0, "frontend_peak_rss_mib": 0, "vpr_time_s": 0.0,
          "vpr_total_runtime_s": 0.0, "max_vpr_mem_mib": 0},
 "verdicts": {"fcheck": "skipped", "equiv": "skipped", "rerun": "skipped"},
 "error": ""}
```

### 5.3 Job keys and the cache

`key` = SHA-256 of the canonical JSON (sorted keys, no blanks) of: schema; leg; design; arch
basename and SHA-256; design source SHA-256 (`.v` for P/O/O′/Oprov, the golden BLIF for P′/R);
VTR commit; the common parameters; and, for every leg that runs `odin3`, the **odin3 build id**
(SHA-256 over the `odin3` binary and every `libodin3*.so` it loads), the script text, `$LIB`'s
SHA-256 and the ABC script's SHA-256. `key12` = its first 12 hex digits. A job whose
`metrics.json` exists with `status ok` is reused unless `--rerun`. The P leg's key has no odin3
part, so P runs once per VTR commit and arch.

### 5.4 Retention

Per `<vtr12>/<arch>/<leg>`: the 3 newest complete keys are kept; older ones are deleted by
`vtr-qor clean` (and automatically after a run completes). Failed and capped jobs keep their full
run directory for diagnosis, at most 5 per leg **and at most 2 GiB in total per leg** (oldest
pruned first; a single failed mcml or LU32PEEng directory can hold several GiB). Disk per kept
ok job: logs, parse files and `metrics.json`, about 1–20 MB; a P job also keeps
`<d>.pre-vpr.blif.gz` (review H4; P is cached per VTR commit, so the functional check of P and
P′'s cross-checks never need a 3–4 h rerun): about 120 MB gzipped for all 21.

## 6. The table (QH-6)

`vtr-qor table` reads the `latest` key of each requested leg (or `--keys leg=key12,…`) and
writes four files. Numbers: integers as is; `critical_path_delay_ns` 3 decimals; `fmax_mhz` 1;
times 1 (s); memory 0 (MiB); ratios and geomeans 3, or 4 when the value lies within 0.005 of a
gate bound (the gate compares unrounded values, so 1.2004 must not print as "1.200" next to a
FAIL); empty cell = not available. Rows follow the §2 order; extras after the 19. The header's
date and host come from the inputs (the newest job end time and the host recorded in
`job.json`), never from the clock, so two runs give identical bytes (T7).

### 6.1 Ratios and geomean

For a pair X/Y (pairs `O/P`, `O'/P`, `P'/P`, `O/O'`, `R/P`, `Oprov/O`) and a metric m, the ratio of
design d is `m(X,d) / m(Y,d)`. A value is **present** when its parse result is a finite number;
VTR's parser prints the pattern default or `-1` for a missing key (and the last match wins), so
`-1` and an absent key are "missing". For a **gated** metric (§6.2) the ratio is defined only when
both values are present and > 0; otherwise it is undefined and the gate fails (§6.2 rule 2). For
report-only metrics the ratio is defined when both are present, X ≥ 0 and Y > 0, and is 1 when
both are 0. The **geomean** over a design set S is `exp(mean_{d∈S} ln r_d)` over the
designs with a defined ratio, printed with `(n/|S|)` when n < |S|. S is the 19 for gate rows, the
2 extras separately, never mixed. Lower is better for every gated metric.

### 6.2 The gate (decision 3b; `tests/vtr/qor/bands.json`)

```json
{"schema": 1, "pair": ["O", "P"], "set": "gate",
 "metrics": ["num_clb", "min_chan_width", "critical_path_delay_ns", "crit_path_routed_wirelength"],
 "geomean_max": 1.20, "per_design_max": 1.50,
 "equal_or_explained": ["num_mult", "num_memories"]}
```

The verdict is one of **PASS**, **FAIL** or **INCOMPLETE** (review H13). It is INCOMPLETE (exit
3) when a *reference* job (P, or P′/O′ for the attribution rows) is not `ok` — capped, timed out
or failed — so a cap on the baseline is never reported as Odin III's failure; the table says
which jobs to rerun. Otherwise PASS iff all of:

1. every gate design's O job is `ok` with `vpr_status=success` (else FAIL);
2. every gated metric is present and > 0 for O and P on every gate design (§6.1); a missing or
   nonpositive value is `gate: FAIL: <metric> missing for <d> (<leg>)` (ruling H2);
3. for each gated metric, geomean ≤ 1.20 (inclusive) over all 19 (n = 19) and every ratio ≤ 1.50
   (inclusive), compared unrounded;
4. for `num_mult` and `num_memories`, O = P or a row `<design>\t<metric>\t<text>` exists in
   `tests/vtr/qor/explanations.tsv`;
5. the functional check of every gate design is `pass`, or `fail` with a declared row in
   `divergences.tsv` (§8.1); `error` fails;
6. `equiv` (decision 16a, §8.2) is `equivalent` or `ram` on every gate design not listed in
   `tests/vtr/qor/equiv-exceptions.tsv`; `differs` anywhere fails; a listed design's verdict is
   reported, and its functional check (rule 5) stands in for it;
7. every rerun is `same` (§8.3).

Everything else is report-only (decision 3). The verdict is printed by `vtr-qor gate` and as the
first section of `qor.md`.

### 6.3 `qor.md` (Markdown)

```markdown
# Odin III QoR — k6_frac_N10_frac_chain_mem32K_40nm — <label>

VTR 3c9a4d23b27d · odin3 <commit12> (build <id12>) · ABC script <sha12> · legs P <key12>, O <key12>, …
· host <cpu model>, <GiB> GiB, WSL2 · <UTC date>

## Gate (decision 3): PASS | FAIL

| design | CLB P | CLB O | O/P | W P | W O | O/P | CPD P | CPD O | O/P | WL P | WL O | O/P | ok |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ch_intrinsics | 68 | … | 1.029 | … | … | … | 1.934 | … | … | 1093 | … | … | ✓ |
| … 19 rows … |
| **geomean (19)** | | | **1.087** | | | … | | | … | | | … | |
| max (19) | | | 1.312 (sha) | | | … | | | … | | | … | |

FAIL lines, one per violated rule (E-QH texts of §10).

## Non-gating rows (arm_core, spree) — same columns, own geomean (2)
## Netlist — per design: luts, ffs, adders, mults, ram_slices, depth for each leg run, and O/P
## Packed — num_clb, num_memories, num_mult, device W×H per leg; hard-block equality column
## Routed — min W, CPD (ns), Fmax (MHz), wirelength per leg
## Cost — front-end wall (s) and peak RSS (MiB), VPR time (s) and memory (MiB) per leg, O/P
## Attribution — per gated metric, geomeans of O'/P (front end), P'/P and O/O' (ABC), O/P
## Noise floor — R/P per gated metric (the same netlist up to names and order: VPR's own spread)
## Functional check and determinism — fcheck O and P, equiv (with the recorded exceptions), rerun, per design
## Declared divergences — rows of divergences.tsv that apply
## Name style (when Oprov ran) — VPR time and memory, Oprov/O
```

The gate table is the one PHASE3 records verbatim at M3.

**Seeds** (review H12): one VPR seed per job, as VTR's own QoR task. At M3, every design whose
gated ratio lies within 10 % of a bound (≥ 1.08 geomean contributor or ≥ 1.35 per design) is
rerun with VPR seeds 2 and 3 (`--seed N` passed through `run_vtr_flow`) for O and P, and the
table shows the three-seed mean beside the gating single-seed value; the gate itself stays on seed
1 (decision 3 does not ask for seeds), and the spread goes to Peter with the exit table.

### 6.4 CSV

`runs.csv`, one row per (leg, design), header exactly:

```
arch,design,leg,key12,status,gating,vpr_status,luts,ffs,adders,mults,ram_slices,depth,abc_depth,
num_clb,num_memories,num_mult,device_width,device_height,min_chan_width,critical_path_delay_ns,
fmax_mhz,crit_path_routed_wirelength,frontend_wall_s,frontend_peak_rss_mib,vpr_time_s,
vpr_total_runtime_s,max_vpr_mem_mib,fcheck,equiv,rerun,blif_sha256,vtr_commit,odin3_commit
```

(one line in the file). `ratios.csv`, one row per (pair, design) plus one `GEOMEAN_GATE` and one
`GEOMEAN_EXTRA` row per pair, header exactly:

```
arch,pair,design,n,luts,ffs,adders,mults,ram_slices,depth,num_clb,num_memories,num_mult,
min_chan_width,critical_path_delay_ns,crit_path_routed_wirelength,frontend_wall_s,
frontend_peak_rss_mib,vpr_time_s,max_vpr_mem_mib
```

`n` is 1 for a design row and the count of defined ratios for a geomean row (per metric counts
are in `runs.json`). RFC 4180, `\n` line ends, no BOM, `.` decimal point. `runs.json` is the list
of `metrics.json` objects plus the table's provenance header and the gate verdict object
(`{"pass": bool, "violations": [str]}`). Two `table` runs on the same inputs give identical bytes
(§15 T7).

## 7. Heavy-run scheduling (QH-7)

### 7.1 The wrapper (standing rule, decision 17)

Every subcommand that starts VPR, VTR's ABC, Parmys, Verilator, or an `odin3` run on group B is
**heavy**. Unless the environment has `ODIN3_HEAVY=1`, `vtr-qor` re-executes itself as

```
mkdir -p /tmp/claude-1000
flock /tmp/claude-1000/odin3-heavy.lock \
  systemd-run --user --scope --quiet --unit=odin3-vtr-qor-<pid> \
    -p MemoryMax=6G -p MemorySwapMax=0 \
    env ODIN3_HEAVY=1 python3 -I <repo>/tools/vtr-qor/vtr_qor.py <same arguments>
```

so one heavy run holds the machine, and the whole run (all its jobs together) is capped at 6 GiB
with no swap. Before blocking it probes with `flock -n`; when the lock is busy it prints
`vtr-qor: waiting for the heavy lock /tmp/claude-1000/odin3-heavy.lock` (or exits 4 with
`--no-wait`). Inside, before any job, it **verifies the cap** from its own cgroup
(`/proc/self/cgroup` → `/sys/fs/cgroup/<path>/memory.max` ≤ 6,442,450,944 and
`memory.swap.max` = 0) and refuses otherwise (E-QH3). The job processes never call
`systemd-run` themselves: a new `--scope` is a sibling cgroup, not a child, and would escape the
cap.

**One convention for every heavy tool** (review H16). `qor_heavy.reexec_under_lock(argv)` and
`qor_heavy.verify_scope()` are the only implementation: 3E's `vtr-read-check` and `blif-roundtrip
--vtr` import them. A heavy tool that finds `ODIN3_HEAVY=1` does not call `flock` or
`systemd-run` again (that would deadlock under `vtr-qor nightly`, which already holds the lock); it
only verifies the scope and proceeds. Under a hand-started nightly service (`ODIN3_HEAVY_SERVICE=1`, §13) it takes the
lock with `flock` but creates no scope, since the service's cgroup already carries the cap.

This replaces decision 17's per-job `prlimit` RSS cap and `run_vtr_flow -limit_memory_usage`,
which are no-ops here: Linux ignores `RLIMIT_RSS`, and VTR applies its `ulimit -Sv` only when
`Path("ulimit").exists()` (`python_libs/vtr/util.py`, `check_cmd`), which is never true (recorded
in `tools/run-oracle.sh` in Phase 0). One 6 GiB scope for three concurrent jobs is also stricter
than 3 × 6 GiB, which would exceed decision 17's 60 % of 24 GiB. Group A's largest concurrent
peaks (stereovision2's VPR at 1.16 GiB plus two smaller jobs, §2) fit easily; each group B job
gets the whole 6 GiB alone (LU32PEEng's VPR peak is 2.26 GiB).

**What is not yet measured** (review H7; ruling: do not raise the cap). VTR's golden gives VPR's
peaks and the Parmys stage's; three group B steps have no figure: VTR's ABC stage on mcml and
LU32PEEng (`dch -f`, `mfs2`; `golden_results.txt` has no ABC-stage RSS), `odin3` with linked ABC
on the same two designs (the IR and ABC's network in one process), and the Verilator build of RTL
plus netlist. They are measured in this order, each solo under the scope:

1. VTR's ABC stage: the first P run (§12) records the peak RSS of every `abc*.out` for all 21;
2. the Verilator build: 3H-3 runs fcheck on `LU8PEEng` first, then `bgm`, then `mcml` and
   `LU32PEEng`, as a separate step after VPR with `--verilator-j 1` (§8.1);
3. `odin3`+ABC: the first P′ run (M1, 3H-3) on `LU8PEEng`, `bgm`, then `mcml` and `LU32PEEng`;
   the first O run (M3) repeats it with the front end.

If a step exceeds 6 GiB, the job is `capped` with the message `vtr-qor: <L> <d>: capped: stage
<stage> exceeded the 6 GiB scope (peak ≥ <n> MiB at the kill)` (the stage is the newest log file
being written; the peak is the scope's `memory.peak`), the gate cannot PASS (INCOMPLETE or FAIL,
§6.2), and the measured numbers go to Peter; the cap is not raised by the agent.

### 7.2 Order and parallelism

Jobs are generated design-major, leg-minor, in §2 order (`ch_intrinsics`: P, P′, O′, O; then
`stereovision3`: …), so the smallest designs finish every leg first. Group A runs through a pool
of `-j` workers (default 3, maximum 3, E-QH9 above); when group A has drained, group B runs one
job at a time. `--designs` and `--set` keep the §2 order whatever order they are given in.

### 7.3 Admission

A job starts only when all hold, re-checked every 60 s while waiting:

1. `C:` has ≥ 20 GiB free (`df -B1G --output=avail /mnt/c` reports GiB; 20 GiB ≥ decision 17's
   20 GB; PHASE0 #29: the ext4 VHDX grows into `C:`). Below: `vtr-qor: paused: C: has <n> GiB
   free (< 20 GiB); checking every 60 s`; resuming logs `vtr-qor: resumed: C: has <n> GiB free`.
   Deleting files inside ext4 does not return space to `C:` (the VHDX never shrinks by itself), so
   a pause rarely clears on its own: paused for 2 h in a row, the run stops with exit 5 (E-QH11)
   and prints the hint `vtr-qor: free C: by compacting the WSL disk (wsl --shutdown, then
   Optimize-VHD or diskpart compact vdisk; Peter's hands)`.
2. `MemAvailable` (`/proc/meminfo`) ≥ 7 GiB (the cap plus 1 GiB), so other sessions cannot push
   the VM into OOM while our scope is within its limit. Below: `vtr-qor: waiting: <n> GiB
   available (< 7)`.
3. A **hard floor**: if `C:` falls below 10 GiB while jobs run, the running jobs are terminated
   (`aborted`), and the run stops with exit 5.

### 7.4 Out of memory, timeouts

- **Fresh attempt** (review H3; ruling): before every attempt (first run, capped retry,
  `--rerun`) the harness deletes `<J>/metrics.json` and `<J>/status`, wipes the run directory with
  `safe_rmtree(<J>/run)` and recreates it, and records the attempt's start time; the parser
  accepts a log or parse file only if its mtime is ≥ that start time (else `vtr-qor: <L> <d>:
  stale <file> from an earlier attempt; job failed`), so a retry that dies early can never be
  scored with an earlier attempt's `vpr.out`.
- **Capped**: the scope's `memory.events` `oom_kill` counter is read before and after each job;
  a job that fails while the counter rose during its life is `capped` (the kernel kills the
  largest process in the scope, usually VPR, and `run_vtr_flow` then exits with an error, so the
  exit code alone does not tell). A capped group A job is retried once alone after group A
  drains; capped again (or capped in group B) it stays `capped`: no verdict, as in
  `run-oracle.sh`.
- **Timeouts**: VTR's `-timeout` per stage, 7,200 s in group A (decision 17) and 14,400 s in
  group B (§3); the harness's own wall limit per job is 3,600 s in both groups (`--job-timeout S`,
  shared by the job's commands; 0 is no cap; changed 2026-10-20 from 14,400/21,600 after one
  hung ABC held the heavy lock for hours). On expiry the job's process group (each job runs with
  `start_new_session=True`) gets SIGTERM, then SIGKILL after 30 s; status `timeout`.

### 7.5 Cleanup

After an `ok` job (and after §4.1 has read the netlist and §8 has used it): everything under
`<J>/run` is deleted except `vtr_flow.out output.txt parse_results.txt qor_results.txt vpr.out
vpr.crit_path.out parmys.out abc*.out *_blackboxing_latch.out restore_latch*.out
report_clocks.abc.out`, and, for **P always** (ruling H4), `<d>.pre-vpr.blif`, gzipped to
`<d>.pre-vpr.blif.gz`, so fcheck on P never needs a rerun; for the other legs the pre-VPR
netlist is deleted and `<J>/<d>.blif` is replaced by its `.sha256` (kept with `--keep-netlists`,
which the exit run sets for O and O′ so the functional check, `equiv` and reruns can use them).
Deletion goes through `safe_rmtree(path)`, which refuses (E-QH12) unless `realpath(path)` lies
strictly under `realpath(~/odin3-ws/work/vtr-qor)` and no component is a symlink: the standing
rule's "rm only under `~/odin3-ws/work`", enforced in code.

### 7.6 Progress and logs

One line per event on stderr and in `harness.log`:
`vtr-qor: [<i>/<n>] <leg> <design> <status> <elapsed> s (<group>, j<k>)`; at the end
`vtr-qor: done: <ok> ok, <failed> failed, <capped> capped, <timeout> timeout, <skipped> skipped
in <h:mm:ss>`.

## 8. Functional check, equivalence, determinism (QH-8)

### 8.1 Verilator random check (decision 16a)

```
$PY $V/mosaic/verilator_check/run_random_check.py --rtl $V/vtr_flow/benchmarks/verilog/<d>.v \
  --post-synth-blif <netlist> --work-dir <J>/fcheck \
  --vectors 50000 --seed 1 --verilator-j <n>
```

`<netlist>` is `<J>/run/<d>.pre-vpr.blif` (P: decompressed from the kept `.gz` into
`<J>/fcheck/`). It runs read-only from VTR's tree (`--work-dir` keeps every output in work; the
script's default would write into the working directory). Group A runs it inline after the leg's
VPR job when `--fcheck` is given, with `--verilator-j 2`; group B runs it as **its own step after
all VPR jobs**, one design at a time, with `--verilator-j 1` (review H7), heavy.

**Verdicts** from the script's exit code (review H6): 0 → `pass`; 1 → `fail` (vectors mismatch);
2 → `error` (Yosys or Verilator missing, a port it cannot parse, a build failure); a kill by the
cap → `capped`; anything else → `error`. (`missing` is the batch runner's CSV class, not this
script's.) It runs on O for the gate and on P as the reference.

`tests/vtr/qor/divergences.tsv` rows are `<design>\t<class>\t<text>`, with class:

- `mem-init-dropped`: decision 6 step 5 (`ch_intrinsics`, and any design 3C lists);
- `async-reset-made-sync`: decision 19;
- `baseline-fails`: P's own fcheck is `fail` on this design too, for a reason outside Odin III
  (RTL clock/reset heuristics of `port_parse.py`, uninitialised RAM); the row cites P's log.

**Gate rule** (with §6.2 rule 5): O `pass` passes; O `fail` passes only with a row whose class is
`mem-init-dropped` or `async-reset-made-sync` applying to the design, or `baseline-fails` **and**
P's verdict in the same table is `fail`; O `error` or `capped` fails (or is INCOMPLETE when the
cause is P's side); a P `error` makes the design's fcheck INCOMPLETE.

### 8.2 `equiv-check` before and after `abc` (decision 16a, second check; in the gate)

For each gate design without RAM slices: `tools/equiv-check/equiv-check --timeout 600 <O′ job's
<d>.blif> <O job's <d>.blif>` (pre-ABC vs post-ABC, both `--vtr` from the same front end), heavy
for group B. Verdicts: `equivalent` (exit 0), `differs` (exit 1), `ram` (the netlist has RAM
slices: no RAM model, PHASE0 #8), `multiclock` (equiv-check's dsec has one implicit clock and
exits 2 on two), `timeout`, `error` (any other exit 2).

The RAM-free gate designs (Parmys counts, 2026-10-11) are `stereovision3`, `diffeq2`, `diffeq1`,
`sha`, `stereovision0`, `stereovision1`, `stereovision2`, `blob_merge` and `bgm` (the other 10 gate designs and both
extras have RAMs). Of these, dsec cannot decide (ruling H5/H6):

| Design | Reason | Recorded exception (`tests/vtr/qor/equiv-exceptions.tsv`) |
|---|---|---|
| `stereovision3` | two clocks (`tm3_clk_v0`, `tm3_clk_v2`) | `multiclock`: the Verilator check stands in |
| `diffeq1`, `diffeq2` | 5 `multiply` each, inlined by equiv-check as 36×36 logic | `multiplier`: the Verilator check stands in |
| `stereovision1` | 152 `multiply` | `multiplier`: as above |
| `stereovision2` | 468 `multiply` | `multiplier`: as above |
| `bgm` | 11 `multiply` | `multiplier`: as above |

`equiv` still runs on the excepted designs (its verdict is reported; a `differs` there fails the
gate like anywhere else). `sha`, `stereovision0` and `blob_merge` must be `equivalent`; a
`timeout` or `error` on them fails until it is explained and added to the exceptions file in a
reviewed PR.

**`stereovision0`** (11,673 latches) is the likely exception: dsec within 600 s is doubtful. Its
stated fallback: if `equiv` gives `timeout` (or `error` from resource limits), the Verilator check
(§8.1) stands in for it, exactly as for the listed exceptions, and the case is recorded as an
`equiv-exceptions.tsv` row with class `timeout` and the measured time. It is measured on the first
O run (M3; the O′ and P′ netlists do not share a front end, so no earlier pair exists), and the
exceptions PR is expected. `sha` and `blob_merge` have no such fallback.

### 8.3 Byte-identical reruns (exit item 3)

`vtr-qor rerun-check --leg O` runs the O front end again into `<J>/rerun.blif` (a fresh process)
and compares SHA-256 with the job's `<d>.blif.sha256`: `same` or `differs`. The exit run does all
19; nightly does group A.

## 9. CLI (QH-9)

`tools/vtr-qor/vtr-qor` (bash shim: `exec python3 -I "$(dirname "$0")/vtr_qor.py" "$@"`).
Common options: `--vtr-root DIR` (default `$VTR_ROOT`, else `~/odin3-ws/external/vtr-verilog-to-routing`),
`--odin3 PATH`, `--out DIR` (`$ODIN3_QOR_DIR`, default `~/odin3-ws/work/vtr-qor`), `--arch
k6|earch`, `--no-wait`, `--allow-vtr-drift`, `-v`.

| Subcommand | Arguments | Heavy | Does |
|---|---|---|---|
| `doctor` | — | no | checks VTR commit = ORACLES pin and clean (`git status --porcelain --untracked-files=no` empty), binaries (`vpr`, `abc`, and `$V/build/bin/yosys`, which `run_random_check`'s `resolve_yosys` looks for), `verilator` on `PATH`, the venv, `/usr/bin/time -v`, a `systemd-run --user --scope` probe, `/mnt/c` free space, `MemAvailable`, the heavy lock (free/busy), and which legs `odin3` can run; prints a checklist, exit 0/1 |
| `run` | `--leg L[,L…]` (`P O Oprime Pprime R Oprov`; `O'`/`P'` accepted), `--set gate,extra,smoke` or `--designs d,…`, `-j N`, `--rerun`, `--fcheck`, `--keep-netlists`, `--job-timeout S`, `--stage-timeout S`, `--dry-run` | yes | §3, §7; `--dry-run` prints the job list with keys and cache hits and runs nothing (not heavy) |
| `table` | `--legs L,…`, `--keys L=key12,…`, `--label TEXT`, `--set …` | no | §6, into `tables/<stamp>-<label>/` |
| `gate` | `<table dir>` | no | prints the verdict and violations; exit 0 PASS, 1 FAIL, 3 INCOMPLETE |
| `fcheck` | `--leg L`, set/designs | yes | §8.1 on existing jobs with kept netlists |
| `equiv` | set/designs | yes for group B | §8.2 |
| `rerun-check` | `--leg O`, set/designs | yes | §8.3 |
| `crosscheck` | `--task DIR` | yes | runs 3F's task through `run_vtr_task.py <abs task> -temp_dir <out>/crosscheck -j 1` on the 3 smallest and requires `parse_results.txt` values equal to the harness's for every non-time key |
| `check-lib` | — | no | `tools/vtr-models check-lib lib/vtr.o3lib` against the arch (decision 5; 3E §15.2) |
| `exit-run` | `--label TEXT` | yes | M3: `run --leg P,Oprime,Pprime,O --set gate,extra --fcheck --keep-netlists` (group B fcheck as its own step), the P fcheck from the kept netlists, `equiv`, `rerun-check`, `run --arch earch --set smoke --leg P,Pprime,Oprime,O`, the M3 seed reruns (§6.3), `table`, `gate`. Estimated ≈ 20–30 h at WSL speed with P cached (breakdown below): 5–7 nights of ≈ 5 h each under the 06:00 admission deadline, resumed through the cache (`exit-run` again continues where it stopped) |
| `nightly` | `[--start-at HH:MM] [--now --budget H] [--slot N]` | yes | §13; `--slot` forces a rotation slot |
| `publish` | `<table dir>` | no | copies the four files to `~/odin3-ws/golden/qor/<vtr12>/<label>/` (controller only; refuses unless `ODIN3_QOR_PUBLISH=1`) and prints the `git -C ~/odin3-ws/golden add/commit` lines to run |
| `clean` | `[--keep N]` | no | §5.4 |
| `status` | — | no | per leg and arch: latest key, ok/failed counts, age |

**Exit codes** (as `run-oracle.sh` plus the lock and disk cases): 0 every job ok (or, for `gate`,
PASS); 1 a job failed or the gate failed; 2 usage, configuration or environment error; 3 no
failure but at least one `capped` or `timeout` (for `gate`: INCOMPLETE); 4 the heavy lock was busy with `--no-wait`; 5
stopped for disk space.

**Exit-run estimate** (VTR's runner times × 1.5, P cached; review 2). The first M1/M2 runs
replace these figures with measured ones:

| Part | Estimate | Basis |
|---|---|---|
| three legs (O′, P′, O) on 21 designs | ≈ 12–13 h | per leg: group A ≈ 16 min, `LU8PEEng` + `bgm` ≈ 0.5 h, `mcml` ≈ 2 h, `LU32PEEng` ≈ 1.5 h |
| fcheck on P and O | ≈ 3–6 h | group A ≈ 1–1.5 h; group B Verilator builds at `-j1`, 4 designs × 2 legs, unmeasured |
| `rerun-check` on the 19 O front ends | ≈ 3–4 h | `mcml` and `LU32PEEng` repeat linked ABC (≈ 1.5 h for `mcml`) |
| `equiv` | ≈ 1–1.5 h | 9 designs × ≤ 600 s |
| EArch smoke | ≈ 0.3 h | 5 designs × 4 legs |
| M3 seed reruns | 0–6+ h | 4 full runs per big design near a bound |
| **total** | **≈ 20–30 h** | **5–7 nights** with the 06:00 cutoff |

### Leg availability

A leg is available when `odin3 --help` lists every pass its script uses (`read_verilog proc opt
flatten wreduce hardmap lower abc write_blif` for O). `run` with an unavailable leg is E-QH5;
`nightly` and `exit-run` skip it with an info line.

## 10. Errors and messages (QH-10)

Prefix `vtr-qor: `; lower case; no final period; paths absolute.

| # | Exact text | Exit |
|---|---|---|
| E-QH1 | `VTR not found at <dir> (set VTR_ROOT or --vtr-root)` | 2 |
| E-QH2 | `VTR at <dir> is <sha12>, ORACLES.md pins <sha12> (use --allow-vtr-drift for a report-only run)` / `VTR at <dir> has <n> modified tracked files` | 2 |
| E-QH3 | `not inside a capped scope (memory.max <v>, memory.swap.max <s>); run through the heavy wrapper` / `cannot apply the memory cap (systemd-run --user --scope failed)` | 2 |
| E-QH4 | `heavy lock /tmp/claude-1000/odin3-heavy.lock is busy (--no-wait)` | 4 |
| E-QH5 | `leg <L> needs pass '<p>', which <odin3> does not register` | 2 |
| E-QH6 | `<d>: Parmys golden <path>: <reason>` with reason `missing`, `status=<s>`, `vtr_commit <a> is not the pin <b>`, `arch <a> is not <b>`, `blif_sha256 does not match` (the job is `skipped`) | 1 |
| E-QH7 | `unknown leg '<L>' (P, O, Oprime, Pprime, R, Oprov)` / `unknown design '<d>' (see tests/vtr/qor/sets.json)` / `unknown set '<s>' (gate, extra, smoke)` | 2 |
| E-QH8 | `<L> <d>: vpr_status=<s>; see <J>/run/vtr_flow.out` / `<L> <d>: front end exited <rc>; see <J>/frontend.log` / `<L> <d>: stale <file> from an earlier attempt; job failed` | 1 |
| E-QH9 | `-j <n> exceeds 3 (decision 17)` | 2 |
| E-QH10 | `<L> <d>: capped: stage <stage> exceeded the 6 GiB scope (peak ≥ <n> MiB at the kill)`; then `retrying alone` or `no verdict` / `<L> <d>: timeout after <s> s` / `nightly: <L> <d> not started: expected <m> min would pass 06:00` (exit 0 for that line alone) | 3 |
| E-QH11 | `C: stayed below 20 GiB free for 2 h; stopping (<n> jobs not started)` / `C: fell below 10 GiB free; terminating <n> running jobs` / `cannot read free space of /mnt/c` | 5 |
| E-QH12 | `refusing to delete <path>: not under <root>` / `refusing to delete <path>: <component> is a symlink` | 2 |
| E-QH13 | `table: no complete key for leg <L> on <arch>` / `table: <d> has no ok <L> job` (a gate design: the gate fails) | 1 |
| E-QH14 | `gate: FAIL: <metric> missing for <d> (<leg>)` / `gate: FAIL: <metric> geomean O/P <x> > 1.20` / `gate: FAIL: <metric> <d> O/P <x> > 1.50` / `gate: FAIL: <metric> <d> O <a> != P <b> (no row in explanations.tsv)` / `gate: FAIL: <d> O fcheck fail (no row in divergences.tsv)` / `gate: FAIL: <d> O fcheck error` / `gate: FAIL: <d> equiv differs` / `gate: FAIL: <d> equiv <timeout\|error> (not in equiv-exceptions.tsv)` / `gate: FAIL: <d> rerun differs` / `gate: FAIL: <d> O vpr_status=<s>` / `gate: INCOMPLETE: <d> <leg> is <status> (reference); rerun it` (exit 3) | 1 |
| E-QH14a | `gate: FAIL: <d> O fcheck <v>` (any O verdict other than `pass`, `fail`, `skipped`, e.g. `capped`) / `gate: FAIL: <d> equiv <v> (not in equiv-exceptions.tsv)` (any verdict other than `equivalent`, `ram`, `differs`, `skipped`) / `gate: FAIL: <d> rerun <v>` (any verdict other than `same`, `skipped`) / `gate: FAIL: <metric> missing for <d> (<leg>)` also for rule 4's `num_mult`/`num_memories` | 1 |
| E-QH14b | `gate: INCOMPLETE: <d> O <fcheck\|equiv\|rerun> <skipped\|missing> (not run); run it` / `gate: INCOMPLETE: <d> P fcheck is <error\|capped> (reference); rerun it` / `gate: INCOMPLETE: no <O\|P> leg in the table` / `gate: INCOMPLETE: <d> <leg> is missing (reference); rerun it` (no reference job at all) | 3 |
| E-QH15 | `publish: set ODIN3_QOR_PUBLISH=1 (controller only; agents never write ~/odin3-ws/golden)` | 2 |

**E-QH14a/E-QH14b (2026-10-12, agent default from the 3H Task 8 review, ruling D1; the controller may override).** A check that did not run (`skipped`, or no verdict) is unmeasured, so the gate is INCOMPLETE (exit 3), not FAIL: nightly runs fcheck and `rerun-check` on group A only, so FAIL here would make every nightly gate FAIL for good. The other texts generalise E-QH14's for verdicts §10 did not list, and name the cases where P's side, not Odin III, leaves a design undecided (§8.1).

## 11. Caps (QH-11)

| Cap | Value | Source |
|---|---|---|
| memory, the whole run | 6 GiB, no swap (one cgroup scope) | standing rules; decision 17 |
| concurrency | `-j3` group A, `-j1` group B, `-j` ≤ 3 | decision 17 |
| heavy runs machine-wide | 1 (`flock`) | standing rules |
| job admission | `MemAvailable` ≥ 7 GiB; `C:` ≥ 20 GiB free | PHASE0 #29, decision 17 |
| disk floor | 10 GiB on `C:` (terminate) | §7.3 |
| pause limit | 2 h | §7.3 |
| VTR stage timeout | 7,200 s group A (decision 17); 14,400 s group B | §3 |
| job wall timeout | 14,400 s group A; 21,600 s group B | §7.4 |
| nightly admission | no new job after 06:00 (or start + `--budget`) unless it fits | §13 |
| retries | 1, alone, for capped group A jobs | §7.4 |
| retention | 3 keys per (arch, leg); 5 failed run dirs and 2 GiB per leg; P's pre-VPR netlist always (gzipped) | §5.4 |
| Verilator | 50,000 vectors, seed 1, `--verilator-j 2` (group A), `1` (group B, own step) | decision 16, §8.1 |
| equiv-check | 600 s per design | §8.2 |

## 12. The first P-leg run, now (QH-12)

The P leg needs nothing from Odin III, so it runs as soon as 3H-1 lands (from its branch with the
controller's go-ahead if earlier). Solo: under the heavy lock, nothing else heavy on the machine.

1. **Preflight**: `tools/vtr-qor/vtr-qor doctor`. Expected on 2026-10-11: VTR `3c9a4d23b27d`
   clean, `vpr`/`abc`/`yosys` and the venv present, `/usr/bin/time -v` works, `C:` 117 GB free
   (`df`, 76 % used), `MemAvailable` ≈ 19 GiB, lock free.
2. **Run**: `tools/vtr-qor/vtr-qor run --arch k6 --leg P --set gate,extra` (re-executes under
   `flock … systemd-run … MemoryMax=6G`, §7.1).
3. **Schedule**: group A (17 jobs: the 15 + `spree` + `arm_core`) at `-j3`, then `LU8PEEng`,
   `bgm`, `mcml`, `LU32PEEng` one at a time.
4. **Estimate** from VTR's own `golden_results.txt` times (its CI runner):

   | Part | Serial (s) | Wall |
   |---|---|---|
   | group A, 17 designs | 1,185 | ≈ 11 min at `-j3` (the tail is `stereovision2`'s 433 s, which starts near the end) |
   | `LU8PEEng` | 533 | 9 min |
   | `bgm` | 548 | 9 min |
   | `mcml` | 4,699 (ABC alone 3,414) | 78 min |
   | `LU32PEEng` | 3,237 (min-W route 1,141) | 54 min |
   | **total** | 10,202 | **≈ 2 h 45 min** |

   On the WSL machine (i7-13700, 12 threads, 24 GiB) plan for **3–4 h** (× 1.5 margin, unmeasured
   so far). Group B runs with `-timeout 14400` per stage and a 21,600 s job limit from the start
   (§3, §7.4), so `mcml`'s ABC (≈ 57 min on VTR's runner, ≈ 85 min at × 1.5) has a 2.8× margin.
   Memory: peaks per VTR are 2.26 GiB (`LU32PEEng` VPR) and 1.19 GiB (`LU32PEEng` Parmys); the
   ABC stage's peak is not in VTR's golden, and this run is its **first measurement** (§7.1,
   "what is not yet measured", step 1): the `abc*.out` peaks of `mcml` and `LU32PEEng` are copied
   into the PHASE3 entry. A cap hit is reported with the stage and the peak, and goes to Peter;
   the cap is not raised. Disk: up to ≈ 2 GiB transient per group B job; ≈ 20 MB kept per design
   plus P's gzipped pre-VPR netlist (§5.4).
   The run session (Sonnet `high`) starts the command in the background and watches it with
   Monitor (until `harness.log` shows `done:`), rather than waiting 3–4 h in the foreground.
5. **Acceptance**: 21 jobs `ok`, `vpr_status=success`. Cross-check against `golden_results.txt`
   with VTR's own `pass_requirements.txt` bands as a report (another machine and VPR revision,
   PHASE3 exit item 5): a metric outside its band is investigated, never a failure of the run.
6. **Outputs**: `vtr-qor table --legs P --label P-baseline` (the P columns only, plus the
   cross-check column vs VTR's golden), `publish` by the controller, and one PHASE3 checklist line
   (`3H: Parmys baseline (P) on the 19 …`) with the geomeans of `num_clb`, min W, CPD and
   wirelength vs VTR's golden.
7. **Until 3H-2's table lands**, the run's `metrics.json` files are the record; the table is
   rendered later from them without rerunning anything.

## 13. Nightly local runs (QH-13; in scope)

**Starting.** Installing the systemd timer is Peter's call and is **not yet approved** (ruling
H1/H14). Until he approves it, nightly runs **start by hand**: the controller runs

```
systemd-run --user --unit=odin3-vtr-qor-nightly-<YYYYMMDD> --slice=odin3-heavy.slice --collect \
  -p MemoryMax=6G -p MemorySwapMax=0 --setenv=ODIN3_HEAVY_SERVICE=1 \
  <repo>/tools/vtr-qor/vtr-qor nightly --start-at 01:00
```

at the end of a session: a transient user service in the heavy slice, so it survives the Claude
session that started it (a plain background process may be killed with the session). It is not a
timer (no `--on-calendar`): the process itself sleeps until 01:00 holding nothing. With
`ODIN3_HEAVY_SERVICE=1`, `qor_heavy.reexec_under_lock` takes the heavy lock with `flock` only and
does not create a new scope: the service's own cgroup already carries the 6 GiB, no-swap cap,
which `verify_scope` checks as usual. Or `vtr-qor nightly --now --budget 4h` for a
daytime run on a free machine. The timer files ship in the repo
(`tools/vtr-qor/systemd/odin3-vtr-qor-nightly.{service,timer}`, `OnCalendar=*-*-* 01:00`,
`Persistent=false`); when Peter approves, he installs them with

```
mkdir -p ~/.config/systemd/user
cp tools/vtr-qor/systemd/odin3-vtr-qor-nightly.* ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now odin3-vtr-qor-nightly.timer
```

and removes them with `systemctl --user disable --now odin3-vtr-qor-nightly.timer`.

**Admission deadline** (ruling H1). Without `--now`, the deadline is 06:00 local: no new job
starts after it, and a job starts only if its expected duration ends before it. The expected
duration is the last measured elapsed time of the same (arch, leg, design), else VTR's golden flow
time × 1.5 (§2) plus the front end's last time. A refused job logs `vtr-qor: nightly: <L> <d> not
started: expected <m> min would pass 06:00`; jobs already running finish. With `--now` the
deadline is the start time plus `--budget` (default 4 h). The lock and the scope are therefore
released by about 06:00 even on a long night (a group B job admitted at 05:00 may run on; the
admission rule makes that rare because its expected time would not fit).

**Steps**, holding the heavy lock once (inner commands see `ODIN3_HEAVY=1` and do not
re-execute):

1. `doctor`; `vtr-models check --arch` (the committed models JSON is still the arch's) and
   `check-lib`.
2. **EArch smoke**: ranks 1–5 on `earch`, every available leg.
3. **k6, group A**: every available leg (P is a cache hit unless VTR moved), `--fcheck` on O,
   `equiv`, `rerun-check`.
4. **k6, group B: one design per night** (ruling H1), in a rotation of (design, leg) slots that
   splits the two longest designs by leg:

   | Night | Slot | Expected (WSL, × 1.5 of VTR's runner) |
   |---|---|---|
   | 1 | `LU8PEEng`: O, O′, P′ (+ fcheck O) | ≈ 1 h |
   | 2 | `bgm`: O, O′, P′ (+ fcheck O, equiv) | ≈ 1 h |
   | 3 | `mcml`: O | ≈ 2 h |
   | 4 | `mcml`: O′ | ≈ 2 h |
   | 5 | `mcml`: P′ | ≈ 2 h |
   | 6 | `LU32PEEng`: O | ≈ 1.5 h |
   | 7 | `LU32PEEng`: O′ | ≈ 1.5 h |
   | 8 | `LU32PEEng`: P′ | ≈ 1.5 h |

   An 8-night cycle; unavailable legs are skipped, and the rotation advances only when its slot
   ran. **Starvation guard**: if the slot was not admitted on the previous night (the 06:00
   deadline refused it after an overrunning step 3), the next night runs step 4 **before** step 3;
   a slot not run for two consecutive nights is listed at the top of `summary.md` as
   `vtr-qor: nightly: group B slot <n> (<d> <legs>) starved for <k> nights`, and group A then
   runs only the legs whose odin3 build changed. P on group B runs only when VTR moved (a ≈ 2.5–4 h night of its own). fcheck for `mcml`
   and `LU32PEEng` runs at M3 and on request, not nightly.
5. **3E sweeps** (`blif-roundtrip --vtr`, `vtr-read-check`, ≈ 2 h): when `src/backends/blif/`
   or `src/frontends/blif/` changed since the last sweep, else weekly (Saturday night). A sweep
   night skips step 4 and does not advance the rotation.
6. `table --label nightly-<date>` and a diff against the previous nightly: any (leg, design,
   gated metric) that moved by more than 2 % or changed status is listed in
   `nightly/<date>/summary.md`.

**Budget** (unmeasured; VTR's runner times × 1.5): steps 1–3 ≈ 2–2.5 h (group A ≈ 15–20 min per
leg for three legs; fcheck builds for 17 designs ≈ 30–45 min; `equiv` ≤ 3 × 600 s plus the
excepted runs; reruns ≈ 10 min; smoke ≈ 10 min); step 4 ≤ 2 h; total **≈ 3–4.5 h**, inside a
01:00–06:00 window. A sweep night is ≈ 4–4.5 h. The first hand-started nights measure the real
figures, and the rotation table is updated from them.

## 14. CI scope (QH-14; decision 18a)

`ci` (GitHub, every push) runs **no VPR, no VTR ABC stage, no Parmys, no Verilator**, and never
builds VTR (about an hour per cache miss, decision 18). It runs:

- the unit and fixture tests of 3A–3E (ABC is already built in CI, so 3D's `abc` tests and
  `equiv-check` before/after `abc` on micros run there);
- 3E's `blif-vtr-check` over the `--vtr` fixtures and `vtr-models check`/`check-lib` against the
  committed models JSON;
- 3H's `tests/tools/test_vtr_qor*.py` (unittest): parsers on committed log fixtures, netstats,
  keys, scheduler logic with fake jobs, cgroup/disk/meminfo readers on fake files, the table, the
  gate, `safe_rmtree`.

VPR legs, the functional check and the exit run run on the WSL machine (nightly and on demand).
The QoR table goes to odin3-golden and its summary to PHASE3.md (decision 18a).

## 15. Test plan and oracles (QH-15)

| # | Test | Oracle | Where |
|---|---|---|---|
| T1 | metric parsers on committed logs (`tests/tools/fixtures/vtr_qor/<case>/`: `vpr.out`, `vpr.crit_path.out`, `output.txt`, `parmys.out`, `abc0.out`, `abc1.out`, `frontend.time`, trimmed from a real `stereovision3` P run and a two-clock run) | the values VTR's `parse_vtr_flow.py` printed for the same directory, recorded in `expected.json` when the fixture was made | CI |
| T2 | `qor_netstats` | hand-counted fixtures; `depth` = ABC's `print_stats` `lev` on subckt-free single-clock fixtures; on the first P run, `depth` = VTR's `abc_depth` for the single-clock designs (a mismatch is investigated and explained in the PR) | CI; local |
| T3 | table and gate | golden files: synthetic `runs.json` → expected `qor.md`, `runs.csv`, `ratios.csv` bytes; hand-computed geomeans; boundaries (1.200 passes, 1.2001 fails and prints `1.2001`; 1.500 passes; a missing gate design fails; a gated value of `-1`, `0` or an absent key on O or P fails with `missing`; a non-`ok` P gives INCOMPLETE; report-only 0/0 → 1; explanations, divergences with `baseline-fails` only when P fails too, equiv exceptions honoured; fcheck exit 0/1/2 mapping) | CI |
| T4 | scheduler | fake jobs (`sleep`, scripted exit codes, fake `memory.events`, fake `df` via `ODIN3_QOR_DF_CMD`, fake meminfo): order, group boundary, `-j3` → `-j1`, capped detection and single retry, the fresh-attempt wipe and the mtime check (a retry that dies early must not be scored with the first attempt's `vpr.out`), timeout kill of a process group, pause/resume/stop, the 10 GiB floor, the 06:00 admission deadline with a fake clock, `ODIN3_HEAVY=1` skipping the re-exec, `safe_rmtree` refusals | CI |
| T5 | keys and cache | key changes with each input listed in §5.3 and with nothing else; a cached ok job is not rerun; `--rerun` reruns | CI |
| T6 | heavy guard | the re-exec command line (exact argv); refusal outside a capped scope (fake cgroup files) | CI; local probe in `doctor` |
| T7 | determinism | `table` twice → identical bytes; `run --dry-run` twice → identical job list | CI |
| T8 | end to end, smallest: `run --leg P --designs ch_intrinsics,stereovision3,diffeq2` | `vpr_status=success`; metrics equal the parse files'; cleanup left exactly the keep list | local (3H-1) |
| T9 | crosscheck: 3F's task via `run_vtr_task.py` on the 3 smallest | every non-time key equal to the harness's | local (after 3F) |
| T10 | baseline sanity: the first P run vs VTR's `golden_results.txt` | inside VTR's `pass_requirements.txt` bands, or explained (report-only) | local (§12) |
| T11 | M1: P′ on all 19 | `vpr_status=success` for all | local (3H-3) |
| T12 | exit: `exit-run` | `gate` PASS, recorded in PHASE3 | local (3H-4) |

## 16. Files

| File | Content |
|---|---|
| `tools/vtr-qor/vtr-qor` | bash shim |
| `tools/vtr-qor/vtr_qor.py` | CLI (argparse), subcommands of §9 |
| `tools/vtr-qor/qor_legs.py` | `LegContext`, `Command`, `build_commands`, `odin3_script`, `leg_available` (§3, §9) |
| `tools/vtr-qor/qor_sched.py` | `Job`, `job_key`, `plan_jobs`, `run_jobs` (pool, groups, admission, retries, timeouts) |
| `tools/vtr-qor/qor_heavy.py` | `reexec_under_lock`, `verify_scope`, `oom_kills`, `disk_free_gb`, `mem_available_gib`, `safe_rmtree` |
| `tools/vtr-qor/qor_parse.py` | `run_vtr_parse`, `read_parse_results`, `read_time_v`, `vpr_time_s`, `stage_costs` |
| `tools/vtr-qor/qor_netstats.py` | `netstats(path) -> NetStats` (§4.1) |
| `tools/vtr-qor/qor_table.py` | `load_runs`, `ratios`, `geomean`, `gate`, `write_md`, `write_csv` |
| `tools/vtr-qor/systemd/odin3-vtr-qor-nightly.{service,timer}` | §13 |
| `tests/vtr/qor/sets.json`, `bands.json`, `explanations.tsv`, `divergences.tsv`, `equiv-exceptions.tsv` | §2, §6.2, §8.1, §8.2 |
| `tests/tools/test_vtr_qor_{parse,netstats,table,sched,heavy}.py`, `tests/tools/fixtures/vtr_qor/` | §15 |

All Python is stdlib-only, `ruff` and `mypy --strict` clean (CLAUDE.md). No C code: 3H adds
nothing to libodin3 and no pass.

## 17. Implementation split (QH-17)

PHASE3's 3H row: light review, Opus `high`, Sonnet for bulk. Each PR: tests first, `/lint`, a
read-only critique (PHASE0 §A.1). Heavy runs are the controller's (standing rules); an
implementer runs at most T8 and says which sweeps it deferred.

| PR / task | Content | Depends on | Model/effort |
|---|---|---|---|
| 3H-1 harness + P | `vtr_qor.py` (`doctor run status clean`), `qor_legs` (P only), `qor_sched`, `qor_heavy`, `qor_parse`, `qor_netstats`, `sets.json`, T1 T2 T4 T5 T6 T8 | nothing | Opus `high` |
| 3H-run-P | the first P run (§12), controller, under the lock, started in the background and watched with Monitor | 3H-1 | Sonnet `high` (operating the run, the report) |
| 3H-2 table + gate | `qor_table`, `table gate publish`, `bands.json`, `explanations.tsv`, T3 T7; P-baseline table rendered and published; PHASE3 entry | 3H-1 | Opus `high` |
| 3H-3 control legs, checks, nightly | legs P′ and R, `fcheck` (on P from the kept netlists; O later), the group B Verilator and `odin3`+ABC measurements (§7.1), `equiv` and `equiv-exceptions.tsv`, `crosscheck`, `nightly` (hand-started) + timer files for Peter, EArch smoke, `divergences.tsv`; T9 T11; M1 run | 3E-1, 3E-5, 3D (P′), 3F (crosscheck) | Opus `high`; Sonnet `high` for runs (background + Monitor) and `divergences.tsv` research |
| 3H-4 exit | legs O′, O, Oprov, `rerun-check`, M3 seed reruns, `exit-run`; M2 and M3 runs (the exit run spans 5–7 nights, ≈ 20–30 h, resumed through the cache); PHASE3 exit table verbatim; DESIGN §13 #6 answered with the Oprov numbers | 2D, 2E, 2F (and 2H's VTR rows), 3A, 3B, 3C, 3G, 3D (`abc` for O), 3E-6 (Oprov), 3F | Opus `high`; Sonnet `high` for the runs (background + Monitor) |

Lanes: 3H-1 → 3H-run-P ‖ 3H-2 → 3H-3 (when 3D/3E land) → 3H-4 (when the front-end lane lands). The
agent says when to switch model: Opus `high` for 3H-1/2/3/4 code sessions, Sonnet `high` for run
sessions (PHASE0 §9).

## 18. Review focus and risks

1. **One scope for all jobs** (§7.1) is stricter than decision 17's text and replaces two caps
   that do not work; an `oom_kill` in group A takes down whichever job is largest, which the
   retry absorbs.
2. **Front-end cost comparability**: P's front end is two stages (`parmys`, `abc*`) measured by
   VTR's `time -v`, O's one process measured by ours; both are wall time and peak RSS, but VTR's
   ABC stage also pays BLIF write/read per clock domain. The table says so in the Cost section.
3. **`depth`** is ours, computed uniformly; VTR's `abc_depth` sits beside it.
4. **WSL wall time is unmeasured**: §12's estimate is VTR's runner's; the first P run measures it
   and fixes the nightly budget.
5. **Verilator heuristics** (clock and reset by port name, `port_parse.py`) may fail on designs for
   reasons outside Odin III; running fcheck on P too separates those.
6. **VTR drift** (PHASE3 risk 10): a VTR bump changes every P key; `doctor` refuses drift
   without `--allow-vtr-drift`.

## 19. Amendments to other documents (when the PRs land)

- `docs/PHASE3.md` decision 17: record (agent default) that the per-job `prlimit` RSS and
  `-limit_memory_usage` caps are replaced by one `MemoryMax=6G`, `MemorySwapMax=0` scope per run
  under the heavy lock, with the evidence of §7.1.
- `docs/PHASE3.md` checklist: the P baseline line (3H-run-P), M1 (3H-3), M2/M3 and the exit table
  (3H-4).
- `docs/ORACLES.md`: `vtr-qor doctor` as the VTR-pin check; `run_random_check.py` named as the
  functional oracle.
- odin3-golden README: a `qor/<vtr12>/<label>/` section describing the four files.

## 20. Decisions for Peter (agent defaults; none blocks 3H-1)

1. **Nightly timer installation** (ruled: Peter's call, not yet approved). Until he approves,
   nightly runs start by hand (§13). The unit files and the install commands are in §13; the
   agent does not install them.
2. **Big designs nightly in rotation** (ruled: one design per night, §13 step 4), with `mcml` and
   `LU32PEEng` split by leg so no night exceeds ≈ 2 h for group B.
3. **Where P′ reads Parmys's netlist.** *Recommended*: the verified golden (§3.4), not the P
   leg's own `parmys.blif`, so P′ does not depend on the P run's directory; the P run records
   whether its `parmys.blif` hash equals the golden's (Parmys determinism, report-only).

## 21. Review dispositions (v2, `3EH-review1`)

| # | Finding | Disposition |
|---|---|---|
| H1 | nightly rotation and budget | applied (ruling): one big design per night, `mcml`/`LU32PEEng` split by leg (8-night cycle), 06:00 admission deadline, budget ≈ 3–4.5 h, 3E sweeps only on change or weekly; hand-started until Peter approves the timer (§13) |
| H2 | missing metrics | applied (ruling): a missing or nonpositive gated metric fails (§6.1, §6.2 rule 2, T3) |
| H3 | stale run directories | applied (ruling): wipe and mtime check before every attempt (§7.4) |
| H4 | P netlist deleted | applied (ruling): P's `pre-vpr.blif` kept gzipped (§5.4, §7.5) |
| H5 | `equiv` not in the gate | applied (ruling): §6.2 rule 6, verdict set, recorded exceptions for `stereovision3` and the multiplier designs, the Verilator check standing in (§8.2) |
| H6 | fcheck verdict mapping | applied (ruling): exit 0/1/2 → pass/fail/error, `baseline-fails` only when P fails too (§8.1) |
| H7 | unmeasured group B stages | applied (ruling): measurement order and the capped message; the cap is not raised (§7.1, §12) |
| H8 | `-delete_result_files` alone does nothing | applied: `-delete_intermediate_files` added, text corrected (§3) |
| H9 | ABC-stage helper logs | applied: summed (§4.4) |
| H10 | identity buffers | applied: excluded from `luts`/`depth`, reported as `buffers` (§4.1) |
| H11 | ABC depth for O/P′ | applied: requested from 3D (3E §14.1 item 6; §4.1) |
| H12 | seeds and rounding | applied: R/P noise floor section, M3 seed reruns near a bound, 4 decimals near a bound (§6) |
| H13 | reference failures | applied: INCOMPLETE, exit 3 (§6.2) |
| H14 | timer installation | applied (ruling): Peter installs; commands given (§13, §20) |
| H15 | disk | applied: VHDX note and compaction hint, 2 GiB cap on failed dirs, GiB wording (§5.4, §7.3) |
| H16 | heavy-lock nesting | applied: one `qor_heavy` convention imported by 3E's tools (§7.1) |
| H17 | doctor, mkdir | applied: Verilator and VTR's Yosys checked; `mkdir -p <J>/run` (§3, §9) |
| H18 | split, estimates | applied: 3H-4 dependencies, exit-run ≈ 20–30 h (5–7 nights; restated in review 2), background + Monitor (§9, §17) |
| H19 | table determinism | applied: date and host from the inputs (§6) |
| H20 | mcml timeout margin | applied: group B `-timeout 14400`, job limit 21,600 s (§3, §7.4, §12) |

No minor finding was rejected.
