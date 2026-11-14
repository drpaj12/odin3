# Timing hints: implementation plan

Spec: `docs/specs/2026-11-14-timing-hints.md`. One PR; each step is one commit, tested before the next.

1. Hint store: `src/passes/hints.{h,c}` (parser, sorted tables, lookups, per-design cache), the
   `read_hints` pass, the project key `timing_hints` (`o3proj.c`, `project.h`, `dump.c`), CMake,
   `builtin.{c,h}`. Test: `tests/unit/test_hints.c`, pass list in `test_pass_manager.c`.
2. abc consumer: `src/passes/abc/timing.{h,c}` (times per region, `abc<r>.timing`), the command list
   (`xblif.c`: `o3_timing` before `if`, `o3_timing -c` before `write_hie`), `exchange.c`, `run.c`,
   the ABC command in `adapters/abc/bridge.c`. Tests: `test_abc_xblif.c` (command file), the micro in
   `tests/tools/test_timing_hints.py` (a late input moves in the mapping; no-match hint file leaves the
   netlist as it was; the required time formula).
3. Flow: `tools/flow/timing_hints.py` (back-annotation), `odin3-flow --hints` and `--two-pass`
   (`tools/flow/two_pass.py`). Tests: the annotator on a real name map, the keys across runs, a
   `--two-pass` run on one design (needs VTR).
4. Docs: `docs/user/timing-hints.md`, `docs/PASSES.md`, `CHANGELOG.md`.
5. Gates: `/lint`, `/run-micro`, `ctest --preset debug`, the Python tools tests with `ODIN3_CLI`.
6. Pilot (local, not in the repo): pass 1 and two pass-2 variants on eight designs, five VPR seeds.
