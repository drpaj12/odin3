# Changelog

All notable changes to Odin III. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Odin III has no API stability promise before a later release (`docs/PHASE7.md`, decision 4).
PR numbers are GitHub pull requests; the phase checklists in `docs/PHASE*.md` hold the evidence.

## [Unreleased]

### Added

- `cone_check` and `raise` take `-const PORT=V` (a template input port tied to a constant;
  v1.0.5, `docs/V1.0.5.md`).

### Changed

- ABI 5: the ABC engine table gains load_genlib and get_cells (gate mapping); ODIN3_ABC_ENGINE_VERSION 2.
  No API stability promise (docs/PHASE7.md decision 4).

### Fixed

- `odin3_flow` retries the VPR route at fixed wider channel widths when the final route at 1.3 x
  the minimum width fails after a finished minimum-width search (tiny devices such as
  `bm_DL_2_4_encoder` on k6); `summary.json` gets `route_retry`.

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
