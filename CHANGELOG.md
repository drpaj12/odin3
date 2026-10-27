# Changelog

All notable changes to Odin III. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Odin III has no API stability promise before a later release (`docs/PHASE7.md`, decision 4).
PR numbers are GitHub pull requests; the phase checklists in `docs/PHASE*.md` hold the evidence.

## [Unreleased]

### Added

- V1.2.0 Task 3 (`docs/V1.2.0.md` 4.3, `docs/ADR/D14.md`): the construct table (`docs/IR.md` IR-12a),
  check rule 12, and ABI 7: `odin3_level` and `odin3_prov_dir` public, `odin3_design_construct_count`,
  `odin3_design_get_construct`, `odin3_obj_get_prov`, `odin3_prov_get_info`,
  `odin3_prov_visit_parents`, `odin3_prov_visit_joined`, `odin3_prov_visit_chain`; Python
  `provenance()` and `chain()` on every IR object. The table is empty until the elaborator fills it
  (V1.2.0 Task 4). Plugins built for ABI 6 must be rebuilt. No behaviour change in any pass or writer.
- V1.2.0 Task 4 (`docs/V1.2.0.md` 4.2, 4.3): the construct snapshot at the end of `read_verilog` stage 2
  (Verilog and SystemVerilog), `read_verilog --prov-ast refs|constructs|full|off` (default `off`: the
  cost gate failed on two designs, `docs/V1.2.0-task4-gate.txt`; the Python `Design` and the
  `ODIN3_PROV_AST` environment variable turn it on), `dump_locs --constructs`, `tools/construct-check`.
  ABI stays 7. Every writer output is byte-identical (the whole micro set, 539 designs).
- V1.2.0 Task 6 (`docs/V1.2.0.md` 3.5): `names --lookup` prints the records and chain of an object of a
  version 2 map, `names --chain` the chain, `names --check` the version 2 invariants; Python
  `odin3.read_name_map`. Version 1 output is unchanged. No core, IR or ABI change.

- V1.2.0 Task 9 (`docs/V1.2.0.md` 4.5): `read_blif`, `read_json` and `read_verilog -netlist` restore a version 2
  `--name-map`: the pass runs, records (with their classes), construct rows and the `A`, `Q`, `K` lines of the file,
  bound to the objects the map names, with no restored-location record; the writers number the extra roots, mark the
  lines and post-Odin records `stale=t<n>` after a pass changed the netlist, and write the objective-only `Q` of a new
  join. IR addition (Peter reviews): the extra table with the mutating-run slot and the `restored` flag of a pass-run row
  (`docs/IR.md` IR-12a). ABI stays 7. A version 1 map reads as before.
- V1.2.0 Task 7 (`docs/V1.2.0.md` 6, 7.2, 8): `odin3-flow --prov` (hash-name BLIF plus the version 2
  name map), `--timing-legacy`, and `odin3-timing` reading the map (full span, construct chain, the
  `DECLARATOR` word); `tools/flow/name_map_v2.py`, a stdlib reader that agrees with
  `odin3.read_name_map`. `attribution.json` stays schema 1 (fields added only). No core, IR or ABI
  change.

### Changed

- **`odin3-flow --timing-attr` now implies `--prov`** (V1.2.0 Task 7): VPR reads the BLIF with hash
  names, so net names in VPR's own output files (`.net`, `.place`, `.route`, reports, logs) are hash
  names where they were short names, and no second provenance BLIF is written. CPD, LUTs, channel
  width and wirelength are unchanged. Use `--timing-legacy` for the version 1.1.1 behaviour (short
  names for VPR, a second BLIF `timing/prov.blif`, line-level spans, no chain).

### Fixed

- Hash names (and provenance names) no longer depend on the build. Two causes, found by comparing independent builds
  of one commit on the micros. (1) `read_verilog --vtr-compat` gave the soft body it reads from
  `lib/soft_primitives/<name>.v` the absolute path of the directory the build found (`<exe>/../../lib`, the install
  tree or `ODIN3_SOFT_PRIMITIVES_DIR`) as its file name, so every object of the body had a location, a full name and
  a hash name that depended on where the build lived (`adder_hard_block`, `hard_adder_cin_propagation`). The body is
  still read from that directory, but its file name is now `lib/soft_primitives/<name>.v`
  (`src/frontends/verilog/read_verilog_lib.c`, `soft_source`). Test `cli_hash_name_soft_dir`. (2) The Debug-only
  full sweep in `opt_expr` `settle` (`src/passes/opt/expr.c`) re-ran the rules on cells a Release build never
  revisits; the `fine.group` and `fine.addsub` rules made the canonical constants 0 and 1 before knowing they would
  rewrite anything, and the sweep dropped them again, but IDs are never reused (IR-6), so a Debug build numbered the
  nets and nodes after `opt_expr` higher than a Release build (two per dropped constant) and every `$n<ID>` name
  built on them, hence the hash names, differed (522 of 1224 writer files on the micros, Debug against Release). The
  rules (`src/passes/opt/expr_fine.c`) now make a constant only when they rewrite. Test `cli_hash_name_ids`. IDs, and
  so names, of objects made after such a rule change in every build (fewer IDs used). Goldens regenerated for that
  (IDs only): `tests/opt/rules/expr/fine.group.il`, `tests/tools/data/names_v2/{ansiportlist_2.blif,v1_ansiportlist_2}.txt`,
  `tests/tools/data/restore/roundtrip.txt` (the BLIF rows' `map_identical` count rises). ABI stays 7.
- The source tarball (`odin3-<ver>.tar.gz`) now configures, builds, passes `ctest` and installs
  without git: `tools/release/make-tarball` writes `third_party/abc/.odin3-commit` and
  `third_party/unity/.odin3-commit` (the submodule HEADs; generated, not committed) and
  `cmake/abc_pin.cmake` reads the ABC stamp when there is no `.git`, still failing on a mismatch
  with the pin. `tools/release/clean-check` gains tarball stages. This lifts the known limit in
  the 1.1.0 and 1.1.1 release notes (`docs/V1.1.1.md`, kept as written).

## [1.1.1] - 2026-10-25

Release notes: [docs/V1.1.1.md](docs/V1.1.1.md). Tools and Python only; the C core, IR and ABI (6) are
unchanged since 1.1.0.

### Added

- Critical-path attribution to HDL (v1.1.1, `docs/V1.1.1.md`, `docs/user/timing-attribution.md`):
  `odin3-flow --timing-attr [--timing-paths N]` joins VPR's setup timing report to the Odin III
  provenance and writes `timing/attribution.json` (schema_version 1) and `timing/summary.txt`
  (which statements, modules and words set the critical path and Fmax); `odin3-timing FLOW_DIR`
  does the same for an existing flow directory and `odin3.timing_attribution()` returns the data
  to Python. Tools and Python only: the core, IR and ABI are unchanged.

### Documentation

- D14 provenance granularity DAG: ADR `docs/ADR/D14.md` and the v1.2.0 spec `docs/V1.2.0.md`
  (design only, nothing built; #275).

## [1.1.0] - 2026-10-24

Release notes: [docs/V1.1.0.md](docs/V1.1.0.md). ADR D13; spec `docs/DESIGN.md` §4.2/§12.1.
The patch versions v1.0.1 to v1.0.5 were never released on their own; their changes ship here and
keep their version notes.

### Added

- SystemVerilog (v1.1.0, `docs/V1.1.0.md`; DESIGN §4.2/§12.1): slang v12.0 is vendored in
  `third_party/slang` with fmt, boost.regex and tomlplusplus (offline build), and `adapters/slang`
  is the C++20 shim that emits the elaborated Odin AST; `read_verilog` routes `.sv`/`.svh` and a
  project's SYSTEMVERILOG files to it, and the gate flow's `write_verilog` writes the mapped
  netlist as structural Verilog of library gate cells, which `read_verilog` reads back (an
  instance of a registered bit-granularity cell type binds to it).
- Simulation API (v1.0.3, ABI 5, `docs/specs/2026-10-21-s1-sim-api-design.md`): `odin3_simulation_*`
  in the C ABI and a `Simulation` class in the Python binding step a design one clock at a time;
  `sim --vectors-in` replays an input vector file and `sim --vcd` writes VCD waveforms.
- VerilogEval corpus test `cli_veval_corpus` and `tools/veval/veval-coverage`: 146 of the 156
  references go through the SystemVerilog front end and the cmos6 gate flow, 10 are refused with a
  pinned reason (#265).
- `unmap` pass (v1.0.4, `docs/specs/2026-10-21-unmap-design.md`): the inverse of `partial_map`; turns the
  VTR hard cells `adder`, `multiply`, `single_port_ram` and `dual_port_ram` back into `$add`/`$sub`,
  `$mul` and `$mem` with provenance, regrouping carry chains, split multipliers and sliced RAMs;
  `tools/unmap-check/unmap-vtr` is its VTR sweep.
- Gate mapping (v1.0.2, `docs/V1.0.2.md`): `abc --gates L` maps soft logic onto the gate cells of
  an `.o3lib` library read with `read_techlib`, `dfflibmap L` maps bit flip-flops and latches onto
  its state cells, and `write_blif --gates-as-names` writes gate cells as `.names` and library
  state cells as `.latch` so `tools/equiv-check` compares the result with the RTL.
- `lib/cmos6.o3lib` (BUF, INV, NAND2, NOR2, DFF, DFFSR with the areas of Yosys's `cmos_cells.lib`)
  and the default gate script `lib/abc/gates.abc`.
- `tools/gatemap-check/gatemap-run` and the sweeps `gatemap_micro_generic` and
  `gatemap_micro_cmos6`: every micro through the gate flow, equivalent to the RTL, with a re-map leg.
- `submod -bus BUSKEY`: vector ports from `NAME[BIT]` net attributes (v1.0.5, `docs/V1.0.5.md`).
- `cone_check` and `raise` take `-const PORT=V` (a template input port tied to a constant;
  v1.0.5, `docs/V1.0.5.md`).

### Changed

- ABI 6 (v1.1.0): `odin3_sv_reader_register` and `odin3_sv_read_fn`, the seam the CLI uses to make
  the slang adapter the read_verilog pass's SystemVerilog reader. Version is 1.1.0.
- ABI 5: the ABC engine table gains load_genlib and get_cells (gate mapping); ODIN3_ABC_ENGINE_VERSION 2.
  No API stability promise (docs/PHASE7.md decision 4).

### Fixed

- `odin3_flow` retries the VPR route (v1.0.1) at fixed wider channel widths when the final route at 1.3 x
  the minimum width fails after a finished minimum-width search (tiny devices such as
  `bm_DL_2_4_encoder` on k6); `summary.json` gets `route_retry`.
- `read_verilog --vtr-compat` reads `lib/soft_primitives/<name>.v` for an instance whose `#()`
  does not fit the registered hard cell of a VTR primitive (`adder #(W)` on the 1-bit `adder`),
  so the micros `adder_hard_block` and `hard_adder_cin_propagation` parse; an instance that fits
  keeps the hard cell, with the existing info line (`#262`).
- The SystemVerilog adapter lowers compound assignments, a bit-select target included
  (`q_next[21] ^= q[0];`), and names the kind in its unsupported-expression diagnostic instead
  of printing the enum number; VerilogEval Prob082/086 pass (`#264`).

## [1.0.0] - 2026-10-21

First release. Source tarball with submodules, `cmake --install`, and a pip wheel
(see `docs/RELEASE-NOTES-1.0.md`).

### Phase 0: environment and gate

- Repository skeleton, oracle tooling (Yosys 0.55 + Parmys from VTR `3c9a4d23`), goldens repo,
  lint gate (clang-format, clang-tidy, cppcheck, lizard, ruff, mypy) and CI (`docs/PHASE0.md`).

### Phase 1: core IR and netlist round trip

- Hierarchical, provenance-tracked IR with typed IDs, paged storage and an op registry; `check`
  pass; C ABI; generic and VTR technology libraries (`.o3lib`).
- BLIF read and write (round trip identical on the goldens), dot, JSON and Verilog writers.
- Bit-level simulator matching ABC on the goldens; a Python plugin walks the IR through the C ABI.

### Phase 2: Verilog-2005 front end

- Project readers (`-f`, Quartus, `.o3proj`, Odin II XML), preprocessor, Flex/Bison parser (#36,
  #44, #49).
- Two-stage elaboration to word-level IR (#94), `proc` (#92), the `opt` family (#95) and the
  `--vtr-compat` mode. Micros and the VTR benchmark set are identical or equivalent to Parmys
  (#98, #113, #114, #134, #135).

### Phase 3: lower, linked ABC, VTR flow

- `flatten` (#112), `wreduce` (#116), `hardmap` (#125, #128), `lower` (PHASE3 3B).
- ABC linked as a library with an in-memory exchange and a script checker (PHASE3 3D; in-memory exchange #148); the
  `$lut` cell.
- `write_blif --vtr` that VPR accepts, with provenance names (#144); the QoR harness, `fcheck`
  and the VTR-19 QoR table (#63, #68, #126, #140). Parmys-default behaviour with warnings.

### Phase 4: VTR architectures and FSM

- `read_arch` imports VPR architectures (#156); generic hard blocks by port signature (#164);
  multiplier splitting (#189); memory splitting, merge and read-enable emulation (#162, #179);
  carry chains from the arch (#174); FSM detect and one-hot recoding (#165).
- Dropped from v1.0: FF feature matching (4.6, to a later plugin).

### Phase 5: outputs and read-back

- Altera LPM structural Verilog target, `--target lpm` (#154); `read_verilog -icells` read-back at
  every level (#155); Yosys JSON reader (#173, #175, #182, #183); HDL locations survive write and
  read, hashed names with a name map (#180, #192, #200).

### Phase 6: analysis flow and provenance

- Gate-level netlist ingest (#195); `write_dot --color-by` (#190); graph export with versioned
  Odin III JSON keys and a Python loader (#197); `levelize` (#191); provenance coverage gate
  (#193, #205, #207, #209, #215).

### Phase 7: release

- `tools/flow/odin3_flow.py` and `odin3-flow` (targets `vtr` and `lpm`, `--analysis`) (#214).
- User manual in Sphinx and MyST, built with `-W` in CI (#219).
- Reverse-engineering support: `submod` (#216), `odin3_design_copy` and `Design.copy()` (#217),
  `cone_check` (#220), `raise` (#222), `plugins/python/re_round.py`, ABI fitness review and
  `docs/RE-ARCHITECTURE.md` (#223).
- Version 1.0.0, `cmake --install`, source tarball with submodules (`tools/release/make-tarball`)
  and the pip wheel (#221). Clean-machine test `tools/release/clean-check` (CTest `release_clean`).

### Removed

- CIRCT writer (D12) and EDIF and VQM writers (D10) are not part of v1.0.
