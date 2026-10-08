---
name: oracle
description: Use when the user asks to generate or refresh golden netlists with the upstream oracles (Yosys+Parmys and/or Odin II) for one design, a glob, or a list of designs, on EArch and/or the k6_frac arch. Wraps tools/run-oracle.sh and summarizes ok/failed.
---

# oracle

Arguments: design(s) (path, glob, or list of `.v`), arch (`EArch`, `k6`, or `both`; default both), optional tool (`parmys|odin|both`, default both), optional `--name` group.

## Setup
- `VTR_ROOT` defaults to `~/odin3-ws/external/vtr-verilog-to-routing`; `ls $VTR_ROOT/vtr_flow/scripts/run_vtr_flow.py` must exist. VTR checkout must be clean (`vtr_dirty_files=0` in provenance).
- Arch files, both under `$VTR_ROOT/vtr_flow/arch/timing/`: `EArch.xml` and `k6_frac_N10_frac_chain_mem32K_40nm.xml`.
- Goldens go to `~/odin3-ws/golden/<arch>/<name>/` (the default `--golden`). Never hand-edit files there.

## Run
Expand globs yourself, then for each design and each arch:
```
tools/run-oracle.sh [--tool parmys|odin|both] --name <group>/<leaf> <design.v> "$VTR_ROOT"/vtr_flow/arch/timing/<arch>.xml
```
- `--name`: use `micro/<leaf>` for `tests/micro` designs (leaf = basename without `.v`); omit for the default (basename). Names may contain `/`.
- Exit 0 = all requested tools ok, 1 = a tool failed, 2 = usage error (stop and fix the call). Keep going after exit 1; failures are data, recorded as `status=failed` plus a `.log`.
- Runs are slow; run the batch sequentially, in the background if long, and do not run two against the same `ODIN3_WORK`.

## Summarize
Print a table `design | arch | parmys | odin` with ok/failed, then totals (`N ok, M failed`). For each failure give the path `~/odin3-ws/golden/<arch>/<name>/<leaf>.<tool>.log`, plus the last error line from it. Check each `ok` has both `.blif` and `.prov`.

## Reminders (tell the human at the end)
- Goldens are only real once committed and pushed to the odin3-golden repo: `git -C ~/odin3-ws/golden add -A && git -C ~/odin3-ws/golden commit -m "goldens: <what> @ VTR <short sha>" && git -C ~/odin3-ws/golden push`. `*.blif` is stored with Git LFS (`git -C ~/odin3-ws/golden lfs ls-files` should list the new blifs). Push after each batch (decision #16).
- `~/odin3-ws/external/` is read-only: never edit or write there; scratch runs go to `ODIN3_WORK` (`~/odin3-ws/work/oracle`).
- Do not overwrite existing goldens with a different VTR commit without telling the human (check `vtr_commit` in the `.prov` and `docs/ORACLES.md`).
