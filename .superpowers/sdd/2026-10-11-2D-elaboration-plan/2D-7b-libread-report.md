# 2D-7b: library-unit read (`read_verilog --lib`) - report

Branch feat/2D-evidence-b. Spec: 2D design section 3.1a, v4.29, section 17 #78 (rewritten) and #80; PHASE2 #21.

## What was built
- `read_verilog --lib [--nolatches] [--nomem2reg] FILE...` parses the files (errors at that read), makes no IR,
  and keeps the parsed store in the design marked with the read's flags (`odin3_ast_mark_units`, `ODIN3_UNIT_*`).
  `--auto-blackbox` is refused with `--lib`.
- Every later `read_verilog` re-reads the kept files as library UNITs (after the design files, before -v/-y), each
  UNIT carrying its read's flags. Library UNITs are never top candidates; unused ones are dropped silently.
- Per-unit flags: stage 1 writes the elaborated MODULE of such a unit with the value-less ATTRs `odin3_lib`,
  `odin3_lib_nolatches`, `odin3_lib_nomem2reg`; stage 2 (`odin3_elab2_enter`) uses them for that module in place
  of the run's options. The design read's flags never reach a unit; the unit's never reach the design.
- Precedence against a hard cell of the same name: the unit is exempt from the "has the name of cell type" error;
  per instance the hard cell wins when every `#()` value fits a parameter of the cell (info line once per name),
  else the unit wins (warning per INSTANTIATION). A shadowing unit cannot be `--top`. Clash names take `$2`.
- `elab-equiv --lib FILE [--lib-nomem2reg]`, `elab-sweep --lib FILE`; `tests/elab/lib/vtr_primitives.v` is a
  copy of Parmys's file, trailing blanks stripped (the micro sweeps in CMake pass it).

## Evidence
- Unit tests `test_read_verilog_lib` (15), RED first (12 failed before the change, 3 controls passed), now green.
- elab-equiv vs Yosys 0.55 (techlib lib/vtr.o3lib + `--lib` vtr_primitives.v + `--lib-nomem2reg`, `--top top_module`):
  `adder_hard_block.v: equivalent`, `hard_adder_cin_propagation.v: equivalent`.
- elab sweep over all 539 micros (techlib + --lib): 469 passed, 70 expected failures, 0 unexpected, 0 listed but
  passing, 0 stale (the two adder entries removed from tests/elab/micro_expect_fail.txt).
- The equivalence sweep over tests/micro/verilog/micro was started and stopped at 52 of 68 files (all ok) because
  one file (`bm_match2_str_arch`) sat several minutes in abc `dsec`; not a --lib effect (no adder in it).
- Structure against the Parmys goldens: not compared. By design (#80) ours is the soft module's `$add`, the golden a
  chain of 1-bit hard adders; equivalent, not identical, until 3E.

## S-P splice
`tools/opt-check/sp-splice` lives on feat/2F-perf (PR 2F-5), not on this branch, so it is not edited here. The change
for the controller is one line of `ODIN3_HALF`:
`"read_verilog --nomem2reg {primitives}"` becomes `"read_verilog --lib --nomem2reg {primitives}"`.
Run from a scratch copy of 2F-5's sp_splice.py with that line and this branch's odin3 (the `opt -nodffe -nosdff`
line dropped, `opt` is not on this branch), memset = ch_intrinsics' golden top:
- with `--primitives vtr_primitives.v` alone, and again with `--odin3-pre "read_techlib lib/vtr.o3lib"`:
  the Odin III half reads vtr_primitives.v + ch_intrinsics.v and writes `a.il` (no `ambiguous_top`). Before: stop at
  `ambiguous_top`.
- The Yosys half then stops in `hierarchy -check`: without the techlib on
  `Module single_port_ram$ADDR_WIDTH?5$DATA_WIDTH?8 ... is not part of the design` (our specialization name is not
  Yosys's `$paramod` name, and Parmys's `setattr ... keep_hierarchy single_port_ram` names the unsuffixed module);
  with it on `Module single_port_ram ... does not have a parameter named 'ADDR_WIDTH'`. The 2F-5 review already
  recorded that ch_intrinsics.v fails on the Yosys side. So out.blif is NOT reached; that is 2H's S-P work
  (naming of specializations and hard-block stubs), not the library read.

## Concerns
- out.blif not reached (above). Re-parse of kept files uses the later read's defines/include dirs.
- Soft RAM wrappers: with a tech library read, `single_port_ram #(...)` with the cell's own parameters stays the
  hard cell, so Parmys's `Mem`-array soft wrapper is not built in that setup (only without the techlib).

## Gate
`tools/build-locked --preset debug -j4` then `ODIN3_REQUIRE_CFFI=1 ctest --preset debug -j2`: 213/213 passed (this
configure registers 213 tests, the new `test_read_verilog_lib` among them); `tools/lint.sh` in full: all PASS.
The python tests (`tests/tools/test_elab_equiv.py`, 43) pass. clangdbg not rebuilt (known ASan runtime mismatch).
