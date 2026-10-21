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
- [x] 7.3 `tools/flow/odin3_flow.py` (targets `vtr`, `lpm`; `--analysis`) and user docs
  (`docs/user/`, Sphinx + MyST, CI `-W`) (#214, #219). Done when a basic design goes from Verilog to a routed
  VPR result and to LPM Verilog with one command.
- [x] 7.4a `submod` pass (#216). Done when `flatten` after `submod` is equivalent on the micros.
- [x] 7.4b (#217) `odin3_design_copy` and `Design.copy()`.
- [x] 7.4c (#220) `cone_check` pass (combinational cones, linked ABC `cec`).
- [x] 7.4d (#222) `raise` pass (Peter reviews).
- [x] 7.4e (#223) ABI fitness review, `docs/RE-ARCHITECTURE.md` gaps, `plugins/python/re_round.py` in CTest.
- [x] 7.5 EDIF writer: dropped (D10).
- [x] 7.6 Release (#221 and this PR): tarball with submodules, `cmake --install`, pip package, CHANGELOG, docs,
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
8. (Agent default) The wheel keeps the plain `linux_x86_64` tag. `auditwheel repair` reaches only
   `manylinux_2_39_x86_64` from this host (glibc 2.39), too new for most users and PyPI. Release notes
   say so; a manylinux wheel needs a build in an older-glibc container (later work).
9. (Agent default) The wheel ships the flow tools under `odin3/data/` (`lib/*.o3lib`,
   `lib/abc/vtr.abc`, `tools/flow`, `tools/vtr-qor`) and the console commands `odin3` (bundled CLI)
   and `odin3-flow`. `import odin3` registers the in-process ABC engine by loading
   `libodin3_abc.so` beside `libodin3.so` as a plugin, so `abc` works from Python.

## Exit evidence

- 7.6 clean-machine test (2026-10-21, head of `feat/7.6-release`, `ODIN3_HEAVY_OK=1 tools/heavy-run
  tools/release/clean-check`, exit 0, 17 stages): fresh clone of HEAD into an empty temp dir,
  submodules from the local checkouts, `env -i` with `PATH=/usr/bin:/bin`; Release configure and
  build (`-j4`); `cmake --install` into a temp prefix; wheel built with scikit-build-core in one
  venv and installed in a second (`import odin3` reads a BLIF); the installed `odin3 --version`
  and `odin3-flow` run from outside the clone; the 4 shell blocks of `docs/user/getting-started.md`
  (build, version, one design, LPM) run in order, none skipped (none needs `VTR_ROOT`).
  Found by the first run: nothing; the doc's `odin3 --version` sample still said 0.0.0 and now
  says 1.0.0. Also CTest `release_clean` (sweep label, local only).
- Release text: `CHANGELOG.md`, `docs/RELEASE-NOTES-1.0.md` (known gaps and the wheel tag).
- The tag `v1.0.0` and the GitHub release are not done: Peter, or the controller after his go.
