# Odin III: Phase 7: release (v1.0)

Spec: `docs/specs/2026-10-21-7-release-design.md`. Plan: `docs/plans/2026-10-21-7-release-plan.md`.
ADR: D12. Background for the next tool: `docs/RE-ARCHITECTURE.md`.

**Goal (Peter, 2026-10-21).** "Polish, write up, and completion of the tool such that a basic
design can go through": user docs like the VTR docs, a Python flow script, the core support for
the reverse-engineering tool, and the release.

**Exit test** (DESIGN section 12 row 7): every item below passes its "done when"; the 7.6
clean-machine test passes.

## Checklist

- [x] 7.1 CIRCT writer: dropped (D12).
- [x] 7.2 JSON reader: moved to 5.3, done.
- [ ] 7.3 `tools/flow/odin3_flow.py` (targets `vtr`, `lpm`; `--analysis`) and user docs
  (`docs/user/`, Sphinx + MyST, CI `-W`). Done when a basic design goes from Verilog to a routed
  VPR result and to LPM Verilog with one command.
- [ ] 7.4a `submod` pass. Done when `flatten` after `submod` is equivalent on the micros.
- [ ] 7.4b `odin3_design_copy` and `Design.copy()`.
- [ ] 7.4c `cone_check` pass (combinational cones, linked ABC `cec`).
- [ ] 7.4d `raise` pass (Peter reviews).
- [ ] 7.4e ABI fitness review, `docs/RE-ARCHITECTURE.md` gaps, `plugins/python/re_round.py` in CTest.
- [x] 7.5 EDIF writer: dropped (D10).
- [ ] 7.6 Release: tarball with submodules, `cmake --install`, pip package, CHANGELOG, docs,
  clean-machine test; tag `v1.0.0` pushed by Peter or after his go.

## Decisions log

1. (Peter, 2026-10-21) 7.1 dropped; Phase 7 = 7.3, 7.4, 7.6 (D12).
2. (Peter) One flow script, targets `vtr` (default) and `lpm`, `--analysis`.
3. (Peter) User docs in Sphinx with MyST Markdown under `docs/user/`.
4. (Peter) No API stability promise for v1.0; 7.4 checks fitness for the RE use.
5. (Peter) 7.4 builds `submod`, design copy, `cone_check` and `raise`; the pyramid, overlay and
   e-graph model is written down in `docs/RE-ARCHITECTURE.md` for the next tool.
6. (Peter) Release as source plus a pip package bundling `libodin3`.
7. (Peter) Benchmark runner, Python GUI, RTL viewer, Verilog Town (Pygame CE) after v1.0, each
   with its own spec on Peter's material.
