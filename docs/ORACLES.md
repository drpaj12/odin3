# Oracles

The upstream tools Odin III is measured against. Every golden in `odin3-golden` records the VTR
commit that produced it (`<leaf>.<tool>.prov`, layout in the odin3-golden README); this file records what that commit contains.
Update it whenever `external/` is moved to a new commit, and regenerate the goldens in the same
change.

Recorded 2026-10-08 (Phase 0 §4).

| Oracle | Source | Commit / version | Binary |
|---|---|---|---|
| VTR | github.com/verilog-to-routing/vtr-verilog-to-routing | `3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b` (2026-10-07) | `$VTR_ROOT/build/...` |
| Odin II | in VTR, built with `-DWITH_ODIN=on` | `9.0.0-dev+v8.0.0-18416-g3c9a4d23b2` | `$VTR_ROOT/odin_ii/odin_ii` |
| Yosys + Parmys | VTR submodule `libs/EXTERNAL/yosys` | Yosys 0.55, `60f126cd00c94892782470192d6c9f7abebe7c05` | `$VTR_ROOT/build/bin/yosys` |
| yosys-slang | VTR submodule `libs/EXTERNAL/yosys-slang` | `55e3a2ad5b9dcdc8cf86918fb0a110fb6356def8` | (Yosys plugin) |
| ABC (VTR's) | in VTR tree `abc/` | ABC 1.01 at VTR `3c9a4d23` | `$VTR_ROOT/build/abc/abc` |
| Yosys (standalone) | github.com/YosysHQ/yosys | 0.69+270, `c4a0a2c486303a49c826f1f49f3a8cad24528cf5` (2026-10-08) | `external/yosys/build/yosys` |
| ABC (Odin III's) | `third_party/abc` submodule, berkeley-abc/abc | `a3001b72edc5de22442e942165487fddf150e3d0` (2026-10-05) | `build/<preset>/third_party/abc/abc` |

`$VTR_ROOT` = `~/odin3-ws/external/vtr-verilog-to-routing`.

## Notes

- **Two Yosys versions.** Parmys runs inside VTR's pinned Yosys 0.55; the standalone Yosys
  (differential oracle for fuzzing, Phase 2+) is current upstream. Do not mix their outputs in one
  comparison.
- **Two ABCs.** Goldens come from VTR's ABC; `tools/equiv-check` uses Odin III's own ABC by default
  (`$ODIN3_ABC` overrides). Equivalence verdicts must not depend on which one runs — if they ever
  disagree, that is a bug report.
- **Odin III's ABC is pinned and built once** (3D spec AB-5). `cmake/abc_pin.cmake` holds the
  commit above as `ODIN3_ABC_COMMIT`; configure stops with `third_party/abc is at <sha>;
  ORACLES.md pins <pin>` when the submodule is elsewhere, so move both together. The build makes
  ABC's `libabc-pic` (PIC static archive, linked into the adapter from 3D-2) with `-g -O2` in
  Debug and `-O2 -DNDEBUG` in Release, `ABC_NO_HISTORY` (no `abc.history`), and never
  `odin3_flags` or the sanitisers. The `abc` tool is the target `odin3_abc_exe` (ABC's `main.c`
  linked to `libabc-pic`, output `build/<preset>/third_party/abc/abc`); it is what `ODIN3_ABC`
  points at in CTest; build it alone with `--target odin3_abc_exe` (or `abc_tool`). ABC's own
  `abc` and `libabc` targets are not built: `--target abc` is ABC's upstream tool (output
  `abc-upstream`, on the non-PIC `libabc`), which compiles all of ABC a second time and does not
  update `third_party/abc/abc`.
  The tool's `main()` is `adapters/abc/child_main.c` (ABC's `main.c` plus one line, below), and
  the build copies it to `build/<preset>/odin3-abc` beside `odin3` (an install puts it in `bin/`
  as `odin3-abc`): that copy is the program of the subprocess path for non-default ABC scripts
  (3D spec §7.9, AB-33). `ODIN3_ABC_EXE` names another program (Python hosts, tests). The core
  asks it `-c "version"` once per process; ABC's own version text does not name the commit, so
  `odin3-abc` prints `odin3-abc: commit <40 hex>` first for that exact query, and the core
  refuses a program whose commit is not the pin above.
- **Yosys now builds with CMake** (`cmake -B build -G Ninja && ninja -C build`); its old
  `make -j` instructions no longer apply.
- Smoke test passed: `run_vtr_flow.py blink.v EArch.xml --route_chan_width 100` → `EArch/blink OK`.
  Odin II on `blink.v` exits 0. `tools/run-oracle.sh` runs each front end alone with
  `run_vtr_flow.py -start <tool> -end <tool>`.
- **Includes and macros.** VTR's flow copies the design into a scratch directory and has no `-I` or
  `-D` option, so a design that `include`s a header beside it, or whose macros come from a parent file,
  fails in Parmys before synthesis. `tools/run-oracle.sh --tool parmys --include-dir DIR --define
  NAME[=VALUE]` (both repeatable) handle this: the script copies VTR's own `misc/yosys/synthesis.tcl`
  into the run directory, adds `-I DIR ... -D NAME[=VALUE] ...` to its `read_verilog -sv -nolatches`
  line, and passes it to `run_vtr_flow.py -yosys_script` (VTR is not modified). The options need
  `--tool parmys` (Odin II is not run with them). The `.prov` records them as `include_dirs=`
  (`:`-separated, relative to `$VTR_ROOT`) and `defines=` (space-separated), and writes neither line when
  the option is absent. Only use a value from the design's own parent file or its VTR task config; for
  the `.generic` templates, which only `.vh` wrappers instantiate, there is none (`tests/exit/micro_nogolden.txt`).
  `exit-check` reads `defines=` from a golden's `.prov` and gives Odin III the same macros (a `+define+` file
  list through `sp-splice --odin3-pre`); a header beside the design needs nothing there.
- **The VPR-read oracle** (3E §16.4). `$VTR_ROOT/vpr/vpr <arch> <netlist> --circuit_format blif
  --exit_before_pack on` makes VPR read the BLIF, clean it (R14, R15), verify it and print its
  netlist statistics, then stop ("Exiting before packing as requested", `VPR succeeded`, exit 0).
  It is VPR's whole reader at a fraction of a route's cost, and it is what `tools/vtr-read-check`
  runs on every `status=ok` k6 golden G and on every `write_blif --vtr` file W (arch
  `vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml`). A rejected netlist exits 1 with
  `Message: <text>` in stdout (a `vpr_throw`); an assertion failure aborts (exit 134) with
  ` Assertion '…' failed` on stderr, so the tool searches both streams. It checks reading only:
  whether a hard block can be packed is 3C's and 3H's to show. It runs only through
  `tools/heavy-run` (the sweep CTest `vtr_read_check_k6`); the 18-file negative corpus
  (`vtr-read-check --bad-corpus tests/vtr/blif-bad`, CTest `vtr_read_check_bad_corpus`) is small
  enough to run anywhere `$VTR_ROOT` is built.
- **The functional oracle** (3H §8.1, decision 16a). `$VTR_ROOT/mosaic/verilator_check/run_random_check.py
  --rtl <design>.v --post-synth-blif <netlist> --work-dir <dir> --vectors 50000 --seed 1
  --verilator-j <n>` simulates the RTL and the post-synthesis BLIF with Verilator on random vectors
  and compares the outputs: exit 0 pass, 1 mismatch, 2 error (it cannot parse a port, or Yosys or
  Verilator is missing or fails to build). It finds Yosys at `$VTR_ROOT/build/bin/yosys`. `vtr-qor
  fcheck` and `vtr-qor run --fcheck` run it read-only from VTR's tree with `--work-dir` under the
  job directory, on `<J>/run/<d>.pre-vpr.blif` (P: the kept gzip, decompressed). A mismatch on
  the O leg passes the QoR gate only with a row in `tests/vtr/qor/divergences.tsv` (class
  `mem-init-dropped`, `async-reset-made-sync`, or `baseline-fails` when P's own check fails too).
- **The VTR pin check.** `tools/vtr-qor/vtr-qor doctor` checks that `$VTR_ROOT` is at the commit
  in the table above and has no modified tracked files
  (`git status --porcelain --untracked-files=no`). `vtr-qor run` refuses either case (E-QH2),
  except that `--allow-vtr-drift` lets a run at another commit go ahead as report-only. The pin
  is also `VTR_PIN` in `tools/vtr-qor/qor_legs.py`: moving VTR updates both, and every P job's
  key changes with it.

## Rebuilding

```bash
cd ~/odin3-ws/external/vtr-verilog-to-routing
git fetch && git checkout <commit> && git submodule update
make env && source .venv/bin/activate && pip install -r requirements.txt
make CMAKE_PARAMS="-DWITH_ODIN=on" -j$(nproc)
cd ../yosys && git fetch && git checkout <commit> && git submodule update --init
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release . && ninja -C build
```
