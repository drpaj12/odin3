# Offload: the runs shipped out of the local machine

PHASE3 decision 54 (Peter, 2026-10-20). The local Phase 3 gate (`vtr-qor --set gate`, milestones
M2 and M3) is the 15 group A designs. The runs below are "untested here: too big for this machine,
shipped out". `run-offload` runs them on a bigger machine and packs the results to send back.

| Shipped out | What | Why |
|---|---|---|
| `mcml`, `LU32PEEng`, `bgm`, `LU8PEEng` (the `offload` set in `tests/vtr/qor/sets.json`; `LU64PEEng` is not in the set) | M2/M3 legs P, O′, P′, O with the Verilator check | VTR flow 4,699 s (`mcml`), 3,237 s (`LU32PEEng`); a lane build stalls while they hold the heavy lock |
| `mcml`, `LU32PEEng` | 3D-8 H1 to H6 (linked ABC against the ABC executable, BLIF against AIG exchange, byte-identical reruns, equivalence) | hours each, up to a 9 GiB slice |
| `lower_gen_vs_yosys_long` | 5,000 seeds of the `--lower` generator (the local CTest `lower_gen_vs_yosys` keeps 200) | hours |
| `fuzz_lower` long run | libFuzzer, capped at 1 h | one hour of the only heavy lock |

Not shipped out, still local: byte-identical reruns (`rerun-check`, seed reruns) on group A only;
the 3D-8 calibration of `ODIN3_ABC_BYTES_PER_AIG_OBJ` from the local `bench-abc-set` table (it
already leaves out `mcml` and `LU32PEEng`).

## Machine limits

This machine is WSL2 on Windows: 12 cores, 23 GiB RAM for the VM, and a 472 GB `C:` drive that the
VM disk grows into. An out-of-memory kill or a full `C:` kills the whole VM, not one process. So
heavy runs use one lock (`tools/heavy-run`) and one memory-capped slice (6 GiB, 9 GiB with
`--xl`, no swap), and a heavy run stalls every lane's build (`tools/build-locked` waits for the
lock). A sweep of the four big designs, about 12 h for three legs, would block the machine for a
day. That is the reason for this directory.

The caps are not about the bigger machine's size: `tools/heavy-run` and `vtr-qor` apply the same
6 GiB and 9 GiB slices there, so the results are comparable. Do not raise them.

## What the bigger machine needs

Measured numbers where the repo has them; "not measured" where it does not.

| Item | Need |
|---|---|
| OS | Linux with systemd `--user` and cgroups v2 (`tools/heavy-run` uses `systemd-run --user --scope` and `systemctl --user set-property`; it skips with exit 77 without them). `/tmp/claude-1000` must exist: it is `heavy-run`'s lock directory and an allowed `vtr-qor --out` parent |
| Cores | 4 are enough: one job at a time (`MAX_JOBS` 1). The wall times below are VTR's runner and this machine's, single-threaded stages |
| RAM | the 9 GiB `--xl` slice (`mcml`, `LU32PEEng` front end with linked ABC) plus 2 GiB for the OS: recommendation, not measured: **16 GiB is the least, 32 GiB comfortable**. Measured: VPR peak 2,264 MiB (`LU32PEEng`) and 2,224 MiB (`mcml`); Parmys 1,186 MiB (`LU32PEEng`), 973 MiB (`mcml`); VTR's ABC stage 364 MiB (`mcml`), 351 MiB (`LU32PEEng`), 121 MiB (`bgm`), 113 MiB (`LU8PEEng`). Not measured in the repo: the peak of `odin3` with linked ABC on `mcml` and `LU32PEEng` (3D-8's H2 measures it; `bench_abc` reports `rss_peak_mb`); a 9 GiB front-end slice for `LU32PEEng` is the `--xl` cap, not a measured peak |
| Disk | 30 GiB free. Up to about 2 GiB transient per group B job, several GiB for one failed `mcml` or `LU32PEEng` directory (kept for diagnosis, at most 2 GiB per leg), 1 to 20 MB per kept ok job, about 120 MB gzipped for the P netlists of all 21 designs; the Verilator build of RTL plus netlist is extra |
| Time (VTR flow, VTR's CI runner / this machine) | `LU8PEEng` 533 s, `bgm` 548 s, `mcml` 4,699 s (ABC alone 3,414 s; 2,028 s measured here in the first P run), `LU32PEEng` 3,237 s (min-W routing 1,141 s; 1,316 s measured here). Four legs (P, O′, P′, O) on the four designs: about 15 h (3H spec: P about 2.5 h, each of O′, P′, O about 4 h), the Verilator check not included. 3D-8 H1 to H6: about 2 h per design for H1 and H4 each (equivalence has a 7,200 s limit). `lower_gen` 5,000 seeds: hours, not measured. `fuzz_lower`: 1 h |
| Software | the odin3 repo at the commit to test, a release build (`cmake --preset release && cmake --build --preset release`: `odin3`, `bench_abc`, `third_party/abc/abc`); for `fuzz` a second build, `cmake -S . -B build/fuzz -DCMAKE_C_COMPILER=clang -DODIN3_FUZZ=ON` and `cmake --build build/fuzz --target fuzz_lower`; Python 3.11+; `verilator` |
| VTR | a built checkout at the pin `3c9a4d23b27d` (`VTR_ROOT`; with `vpr`, `abc`, `yosys` and the Python venv built), a clean tree. `vtr-qor doctor` checks them |
| Yosys 0.55 | for `lower_gen`: `YOSYS=...` or on `PATH`; VTR's own `build/bin/yosys` is 0.55 |
| Parmys goldens | `~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm/vtr/<design>/<design>.parmys.blif` for the four designs (the `odin3-golden` repo, commit `3931cf4` or later), or `GOLDEN=...`. The P′ leg and the 3D-8 runs read them |

Layout the scripts assume (set up once):

```
mkdir -p ~/odin3-ws/work /tmp/claude-1000
git clone <odin3 repo> ~/odin3-ws/odin3          # or any path; run-offload finds its own repo
git clone <odin3-golden repo> ~/odin3-ws/golden
export VTR_ROOT=~/odin3-ws/external/vtr-verilog-to-routing
```

`vtr-qor --out` must lie under `~/odin3-ws/work` or `/tmp/claude-1000`; the default is
`~/odin3-ws/work/vtr-qor` (`$ODIN3_QOR_DIR`).

## Run

```
cd <odin3 repo>
tools/vtr-qor/offload/run-offload check            # prerequisites; read-only
tools/vtr-qor/offload/run-offload --dry-run all    # the exact commands, run none
nohup tools/vtr-qor/offload/run-offload all > ~/odin3-ws/work/offload.out 2>&1 &
```

Steps (`run-offload STEP...`; each writes `$OFFLOAD_OUT/logs/<name>.log`; a failed step does not
stop the next; the exit status is 1 if any failed): `check`, `vtr`, `abc`, `lower`, `fuzz`,
`pack`, `all`. `--no-xl` runs `mcml` and `LU32PEEng` in the 6 GiB slice (they may then be
`capped`; that is a result, not a failure of the script).

The commands behind each step, for reading or running by hand (`R` = the release build,
`G` = the golden `vtr/` directory, `ODIN3_HEAVY_OK=1` throughout; `ODIN3_HEAVY_XL_OK=1` for `--xl`,
on an idle machine only):

1. `vtr`: `vtr-qor run --leg P,Oprime,Pprime,O --designs LU8PEEng,bgm --fcheck --keep-netlists`,
   then the same with `--designs mcml,LU32PEEng --xl`, then
   `vtr-qor fcheck --leg P --set offload --resume`, then
   `vtr-qor table --legs P,Oprime,Pprime,O --set offload --label offload-<date>`. This is
   `exit-run` step 1 and 2 on the offload set; the equivalence, rerun and seed steps are not run
   for these designs (PHASE3 #16: equivalence is for the small set; decision 54: reruns and seeds
   on group A only).
2. `abc`: 3D-8 H1 to H6 for `D` = `mcml`, then `LU32PEEng`, from
   `.superpowers/sdd/3D-8-report.md`, last section, with `E="ODIN3_HEAVY_OK=1
   ODIN3_ABC=$R/third_party/abc/abc ODIN3_REQUIRE_ABC=1 tools/heavy-run timeout 3h"` and
   `EQ="--odin3 $R/odin3 --equiv-check tools/equiv-check/equiv-check --lut 6"`:
   - H1: `python3 -I tools/abc-check/abc_equiv.py --step abc --via-blif W/h1/$D/x --keep W/h1/$D $EQ $G/$D/$D.parmys.blif`,
     then `--step equiv --timeout 7200` with the same options;
   - H2: `$R/bench_abc --one $G/$D/$D.parmys.blif --lut 6 --header --via-blif W/h2/$D --exe $R/third_party/abc/abc`;
   - H3: H1's `--step abc` again into `W/h3/$D`, then compare with H1: `cmp` of `after.blif` and
     `x/abc0.in.blif`, `diff` of `x/abc0.cmd` (directory name aside) and of `x/abc0.out.blif`
     without the dated `# Benchmark` line;
   - H4: `--step abc --exchange aig --keep W/h4/$D $EQ ...`, then `--step equiv --timeout 7200 --exchange aig`;
   - H5: `ODIN3_ABC_EXCHANGE=aig $R/bench_abc --one $G/$D/$D.parmys.blif --lut 6 --header`;
   - H6: H4's `--step abc` again into `W/h6/$D`, then `cmp` of `after.blif` with H4's.
3. `lower`: `tools/heavy-run tools/lower-check/lower-gen-diff --odin3 $R/odin3 --yosys $YOSYS
   --mem-cap 3G --expect tests/lower/gen_diff_expect.txt --seeds 1..5000 --min-compared 4500
   --tier-t report --work W/lower_gen_long_work` (the same as the CTest
   `lower_gen_vs_yosys_long`; also `ctest -R lower_gen_vs_yosys_long` in a configured build).
4. `fuzz`: `tools/heavy-run timeout 90m build/fuzz/fuzz_lower -max_total_time=3600 -timeout=30
   -rss_limit_mb=2048 -max_len=64 -print_funcs=0 -print_final_stats=1 <corpus dir>` (the same
   as CTest `fuzz_lower_long`; one hour is the cap).

`LU64PEEng` is not in `sets.json`, so `vtr-qor` cannot run it. Its Parmys golden exists and the
micro sweeps cover it (`exit_vtr`, `vtr_read_check_k6`, the latter capped by the 6 GiB slice).
Run them as sweeps if wanted: `ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^(exit_vtr|vtr_read_check_k6)$'`
from a configured release build.

## Expected outputs

- `check`: every line `ok`, then `vtr-qor doctor` passes (its log is `logs/doctor.log`).
- `vtr`: per design and leg a job directory
  `$ODIN3_QOR_DIR/3c9a4d23b27d/k6_frac_N10_frac_chain_mem32K_40nm/<leg>/<key12>/<design>/` with
  `status` `ok`, `metrics.json` (`routed.vpr_status` `success`, `verdicts.fcheck` `pass` or, for a
  design the RTL leaves undefined on a bit, `untested`), `job.json`, the kept logs, and
  `<design>.blif` for O and O′ (`--keep-netlists`). M2 is O′ `success` on the four, M3 is O
  `success` on the four. A `capped` or `timeout` status is a result: the harness prints the stage
  and the peak, and the job directory keeps its full `run/`. Then
  `tables/<stamp>-offload-<date>/{qor.md,runs.csv,ratios.csv,runs.json}`; `qor.md` says "Gate
  (decision 3): not gated" (the gate is the local 15) and lists the four designs with O/P
  ratios. They are report-only: the loose bands (geomean 1.20, 1.50 per design) are the reading
  guide, and a ratio over them is recorded, not a failure of the local gate.
- `abc`: for each design `logs/abc-<D>-h*.log`. H1, H4: the equivalence steps end `equivalent` (or
  `unproven` when ABC's `dsec` does not finish: record it). H3 and H6: `logs/abc-<D>-compare.log`
  shows `same` for every comparison; any difference is a determinism bug, report the diff. H2 and
  H5: `bench_abc` prints one header and one row (`abc_ms`, `outside_ms`, `rss_peak_mb`,
  `bytes_per_aig_obj`, ...). The 3D-8 targets: `outside_ms/abc_ms` at most 0.05 (at most 10 s on
  `mcml`), linked `abc_ms` over the executable's at most 1.05 with equal `abc_nodes`,
  `abc_latches`, `abc_levels`, `namemap_ms/abc_ms` at most 0.15, `rss_peak_mb` at most
  `rss_before_mb` + 1.2 × the executable's peak + 64.
- `lower`: the last line of the log reads `... compared N of 5000` with 0 `BUG` and 0 `differ`
  (the 200-seed run read `200 ok, 0 structural, 0 unproven`; the 5,000-seed run lists the tier-T
  histogram differences, about 4 % of the comb seeds, which are not failures with `--tier-t
  report`). Exit 0.
- `fuzz`: libFuzzer ends after 3,600 s with `Done N runs`; no `crash-*`, `leak-*`, `oom-*` or
  `timeout-*` file in the working directory. Exit 0.

## Sending the results back

`run-offload pack` writes `$OFFLOAD_OUT/offload-results-<date>.tar.gz` and prints its SHA-256. It
holds:

- `vtr-qor-<date>.tar.gz`: the whole `$ODIN3_QOR_DIR` except the `latest` files (so extracting it
  cannot move this machine's `latest`): the job directories (`status`, `metrics.json`,
  `job.json`, `frontend.log`, `fcheck.log`, `<d>.blif.sha256`, the kept `run/` logs: `vpr.out`,
  `parmys.out`, `abc*.out`, `vtr_flow.out`, `parse_results.txt`, `qor_results.txt`), `tables/`,
  `harness.log` and the `planned.json` files. The files that matter: `metrics.json`,
  `job.json`, the table's `qor.md` and `runs.json`, `harness.log`.
- `logs/`: every step's log, including `doctor.log` and the 3D-8 compare logs.
- the 3D-8 files: `after.blif`, `abc*.cmd`, `abc*.out.blif` and the `bench_abc` outputs.
- `lower_gen_long_work/` when `lower` ran: the per-seed work and the summary.

Also send: `git -C <odin3 repo> rev-parse HEAD`, `git -C $VTR_ROOT rev-parse HEAD`, the output of
`uname -a`, `nproc` and `free -g`, and `$OFFLOAD_OUT/../offload.out`. Copy the tarball back by
any means; on the local machine extract the `vtr-qor-*.tar.gz` into `~/odin3-ws/work/vtr-qor`
(`tar xzf ... --keep-old-files`). The job key includes the odin3 build id, so a build on another
machine has its own `<key12>` directories next to the local ones; read it with
`vtr-qor table --keys P=<key12>,O=<key12>,... --set offload --label ...`. The controller then
records the table and the 3D-8, `lower_gen` and `fuzz_lower` results in PHASE3 (decision 54 and the
3B, 3D-8 entries).
