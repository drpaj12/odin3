# Gate mapping (`abc --gates`, `dfflibmap`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `abc --gates L` maps soft logic onto the gate cells of a `.o3lib` library `L` read with
`read_techlib`, `dfflibmap L` maps the bit flip-flops and latches onto its state cells, and the
result stays in the provenance IR, writes as structural Verilog of library cells, and is proven
equivalent to the RTL with `write_blif --gates-as-names` and `tools/equiv-check`.

**Architecture:** the core derives ABC's genlib text from the library (ISOP of each cell's truth
table, explicit pins in port order) and hands it to the linked engine through a new ABI 5 call
(`load_genlib`); the script ends in a gate mapper (`map` or `&nf`); a second new call
(`get_cells`) returns the mapped network in the MiniLUT layout plus a gate name per object, which
the core reads with the existing MiniLUT reader, tags with library cells, and splices as library
cell instances with cone provenance. `dfflibmap` is its own pass. `write_blif --gates-as-names`
writes every gate-like cell as `.names` and every library state cell as `.latch`, so equiv-check
sees plain logic on both sides.

**Tech Stack:** C17 core (`src/passes/abc`, `src/techlib`, `src/sim`, `src/backends/blif`), the
linked-ABC adapter (`adapters/abc`, C17 with ABC's headers), ABC at pin `a3001b72` (`Mio_*`,
`Abc_NtkFromCellMappedGia`, `Abc_NtkDfsIter`), Unity unit tests, stdlib Python tools.

**Spec:** `docs/specs/2026-10-21-gatemap-design.md` (sections cited as spec 3, 4, ...; decisions
G-1 to G-9). Executors read both. Where this plan deviates from the spec it says so in
"Spec gaps and agent defaults" below; those rows go into `docs/V1.0.2.md` (Task 15).

## Global Constraints

- Code standard of `CLAUDE.md`: C17 only in the core and the adapter; `-Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wvla -Werror`; functions
  CCN <= 15, <= 60 lines, <= 5 parameters; no VLAs; no `goto` except one `cleanup:`; no recursion
  in IR traversals (explicit worklists); errors as `odin3_status`; IR objects only through the
  module API; `odin3_` prefix; named constants (no magic numbers outside `tests/`).
- Never add `NOLINT`, never weaken `.clang-tidy` or a gate. `tools/lint.sh` passes before every
  commit (run `/lint`).
- Every function in `include/odin3/odin3.h` has a paragraph: purpose, ownership of returned
  memory, failure modes. Struct fields added to the ABI are described in the same comment block.
- `ODIN3_ABI_VERSION` goes from 4 to 5 and `ODIN3_ABC_ENGINE_VERSION` from 1 to 2 (spec 4); no
  stability promise (CHANGELOG says so).
- One pass per PR; every pass has a unit test, a golden or equivalence test and a
  `docs/PASSES.md` row.
- Libraries are `.o3lib` only (G-1); scripts never read files: `read_genlib`, `read_lib`,
  `read_library`, `read_liberty` stay refused (G-4); `amap` is not accepted (G-3).
- `ODIN3_ABC_GATE_MAX_INPUTS` (6) bounds a genlib gate's inputs (spec 3).
- Gate provenance is the cone form (`odin3_abc_records` without a name map), G-2.
- Branches from `origin/main`, merged with `git merge origin/main` (never rebase, never force
  push). `git -C <worktree>` and absolute paths, no `cd X && git ...`.
- Heavy runs (micro sweeps) only through `tools/heavy-run` with label `sweep`, smallest first.
- PR 6 (`abc --gates`) and PR 5 (`dfflibmap`) are mapping algorithms: Peter reviews them (G-9).

## Review Focus

1. A library cell whose `fn` ignores one of its inputs, or has a port named `CONST0`/`CONST1` or
   holding `$`: skipped with a note, never a genlib ABC misreads (Task 3,
   `test_genlib_skips_non_support_input`, `test_genlib_skips_reserved_port_name`).
2. An output driven by a constant, or two output ports carrying one signal, with a library that
   has no `TIE0`/`TIE1`/`BUF` (cmos6): `$_CONST0_`/`$_CONST1_`/`$_BUF_` cells, written as a
   literal or an `assign`, still equivalent (Task 12 `test_gates_buffer_without_buf_cell`, Task 13
   `test_const_output_tie_vs_literal`).
3. A flip-flop with INIT 1 under `--gates`: a cut point (not sent to ABC), then `dfflibmap` maps it
   and warns when the library cell's init is x (Task 12 `test_gates_init1_flop_is_cut`, Task 9
   `test_pass_init_lost_warns`).
4. A user script ending in `&nf` (the & space, read back through ABC's own conversion) gives the
   same kind of result as `map` (Task 2 `test_get_cells_gia`, Task 13 `test_nf_script`).
5. ABC returns a gate whose input count or function does not match the library cell of that
   name (pin order drift): `abc: ABC returned a malformed network: ...`, IR unchanged (Task 11
   `test_from_cells_wrong_function`, `test_from_cells_wrong_arity`).

## Spec gaps and agent defaults (decided here; logged as GM-1..GM-13 in Task 15)

| # | Gap found in the code | Decision |
|---|---|---|
| GM-1 | `Mio_LibraryRead(NULL, text, NULL, 0, 0)` (spec 4.2) crashes: with a buffer it calls `Extra_FileNameGenericAppend(FileName, ".genlib")`, which `strlen`s the NULL name (mioRead.c:81). It also edits the buffer in place. | The adapter passes the name `"odin3.genlib"` (no file is opened when a buffer is given) and a writable copy of the text. |
| GM-2 | With `PIN *` Mio numbers pins in the order names first appear in the formula and drops an input the formula does not mention (mioFunc.c:172-200), so "pins in the cell's input order" (spec 4.4) would not hold. | One `PIN <port>` line per input, in port order. A cell whose function does not depend on every input is skipped with a note. |
| GM-3 | Spec 3 puts the ISOP (`Kit_TruthIsop`) in the adapter, but the text must be hashed and golden-tested, and the core already has `odin3_blif_isop_set`. | The core derives the text (`src/passes/abc/genlib.c`, on-set ISOP from `odin3_blif_isop_set`); `load_genlib` takes the finished text. |
| GM-4 | The default exchange is still BLIF and every non-default script runs in the `odin3-abc` subprocess (AB-33), which has no engine calls. | `--gates` always uses the linked engine and the in-memory exchange (`ODIN3_ABC_EXCHANGE` is ignored); `--via-blif` with `--gates` is refused. A user `--script` with `--gates` runs in-process (no subprocess isolation): Peter may overrule. |
| GM-5 | A constant node in a mapped network has a gate only when the genlib has constant gates (abcMap.c:454). Spec 3 admits only cells with 1 to 6 inputs. | The text always ends with `GATE $odin3_const0 0 Y=CONST0;` and `GATE $odin3_const1 0 Y=CONST1;` (`$` cannot start an `.o3lib` name); the adapter reads such nodes as objects 0 and 1. Library TIE cells are never in the genlib; the splice uses them (G-7). |
| GM-6 | The init-1 restore (restore.c) flips LUT tables; a library gate cannot be flipped. | Under `--gates`, an in-domain flip-flop with INIT 1 is a cut point (counted as "other cells"). |
| GM-7 | `.o3lib`'s `seq` has no asynchronous set or reset, so cmos6's DFFSR cannot be written "from its function" (spec 8). | `DFFSR` is a `blackbox` cell with ports C, D, S, R, Q and a comment giving its function. |
| GM-8 | A library state cell with init x keeps no INIT 0/1. equiv-check reads init x as 0, so an INIT 1 register then differs. | `dfflibmap` follows G-6 and warns with the count; the sweep lists such micros with reason `init-lost`. |
| GM-9 | cmos6 has no latch; `lower` makes `$_DLATCH_*` for latches. | `dfflibmap cmos6` refuses them (no match error); the micro sweep's front uses `read_verilog --nolatches` as the other sweeps do. |
| GM-10 | "the RTL's BLIF" (spec 9) is not defined; the plain BLIF writer writes `$_AND_` and the other bit gates as `.subckt`, which equiv-check cuts. | `--gates-as-names` writes every gate-like cell (built-in or library) as `.names`; the reference is `write_blif --gates-as-names` right after `lower`. |
| GM-11 | AB-20's script precedence would pick a project's or `--abc-script`'s LUT script for `--gates`. | With `--gates` only `--script` overrides; otherwise `builtin:lib/abc/gates.abc`. An INFO line says a project or CLI script was not used. |
| GM-12 | The log has no NOTE level. | Spec "NOTE" lines are INFO lines whose text starts `abc: note:`. |
| GM-13 | Spec 8 gave every cmos6 gate area 1. | cmos6 areas follow Yosys cmos_cells.lib (review finding) so `map -a` matches Gate-Breaker's flow: BUF 6, INV 3, NAND2 4, NOR2 4, DFF 18, DFFSR 18. |

## PRs, order and parallelism

| PR | Branch | Tasks | Depends on | Runs in parallel with |
|---|---|---|---|---|
| 1 | `feat/gatemap-abi` | 1, 2 | main | 2, 3 |
| 2 | `feat/gatemap-lib` | 3, 4 | main | 1, 3 |
| 3 | `feat/gatemap-script` | 5, 6 | main | 1, 2 |
| 4 | `feat/write-blif-gates` | 7 | 2 | 5, 6 |
| 5 | `feat/dfflibmap` | 8, 9 | 2 | 4, 6 |
| 6 | `feat/abc-gates` | 10, 11, 12, 13 | 1, 2, 3 (Task 13 also 4 and 5) | 4, 5 until Task 13 |
| 7 | `feat/gatemap-micros` | 14, 15 | 4, 5, 6 | none |

Files are disjoint between parallel PRs except `CMakeLists.txt` (each PR adds its own lines in
different places), `src/passes/builtin.c` (PR 4 the option, PR 5 the registration) and
`docs/PASSES.md` (each PR its own row): `git merge origin/main` resolves them.
`CHANGELOG.md` is touched by PR 1 (creates `## [Unreleased]` with the ABI line) and PR 7 only.

Every task ends with: `cmake --preset debug && cmake --build --preset debug`, the task's
`ctest --preset debug -R '<names>'`, then `tools/lint.sh`, then a commit and
`git -C $W push -u origin <branch>` ($W = the lane's worktree). Each PR ends with the full
`ctest --preset debug` green, the critique agent's review (CLAUDE.md rule 12), and a PR whose
head SHA equals the reviewed SHA.

---

## PR 1 `feat/gatemap-abi`: engine ABI 5

### Task 1: ABI 5 header, registration, fake engine

**Files:**
- Modify: `include/odin3/odin3.h` (lines 52, 1721-1810), `src/api/abc.c`,
  `tests/unit/abc_fake_engine.{c,h}`, `tests/unit/test_abc_engine_abi.c`,
  `tests/unit/test_abi_ir.c:156-157,809`, `tests/unit/test_api.c:28-30`,
  `tests/unit/test_ast_abi.c:126-127,781`, `tests/tools/test_python_walk.py:284`,
  `CMakeLists.txt:2171` (the `(ABI 4)` regex), `docs/user/getting-started.md:27`,
  `docs/user/python-api.md:26`, `CHANGELOG.md` (new `## [Unreleased]` above `## [1.0.0]`).

**Interfaces:**
- Produces (odin3.h, inside `ODIN3_CDEF_BEGIN/END`):
  - `enum { ODIN3_ABI_VERSION = 5 };` and `enum { ODIN3_ABC_ENGINE_VERSION = 2, ODIN3_ABC_FAIL_TEXT_BYTES = 256 };`
  - `typedef enum odin3_abc_cells_space { ODIN3_ABC_CELLS_NTK = 0, ODIN3_ABC_CELLS_GIA = 1 } odin3_abc_cells_space;`
  - ```c
    /* A gate-mapped network (ABI 5): net in odin3_abc_luts' layout with lut_size the widest gate
     * (at least 2): objects 0 and 1 the constants (a constant node of ABC's network is object 0
     * or 1), the PIs (real inputs, then register outputs), the gates in topological order, the
     * POs (real outputs, then register inputs); net.truths holds each gate's function over its
     * fanins in pin order, net.ci_from as for get_luts. gate has net.n_objs entries: the index
     * into names of a gate object's library gate, -1 for every other object. names holds n_names
     * NUL-terminated gate names, each once. */
    typedef struct odin3_abc_cells {
        odin3_abc_luts net;
        const int32_t *gate;
        const char *const *names;
        uint32_t n_names;
    } odin3_abc_cells;
    ```
  - Two engine fields after `name_map`, before `close` (`reserved[4]` stays):
    `odin3_status (*load_genlib)(void *self, const char *text, odin3_abc_fail *fail);` and
    `odin3_status (*get_cells)(void *self, uint32_t space, odin3_abc_cells *cells, odin3_abc_fail *fail);`
  - Engine comment additions (same style as the existing lines): `load_genlib: text (a
    NUL-terminated genlib, copied) becomes the session's gate library (ABC's Mio_LibraryRead,
    then Mio_UpdateGenlib). RESULT when ABC cannot read it.` / `get_cells: the gate-mapped
    network of space (an odin3_abc_cells_space), owned by the session until close or the next
    call. RESULT "no gate-mapped network in the network space", "no cell mapping in the & space",
    "a node without a library gate", "a box that is not a latch".`
- Produces (`src/api/abc.c`): `ABC_ENGINE_FUNCTIONS = 8`; `missing_function` order `open,
  load_aig, exec, get_luts, name_map, load_genlib, get_cells, close`.
- Produces (fake engine, `abc_fake_engine.h`):
  - `const char *odin3_abc_fake_genlib(void);` the last text `load_genlib` got ("" none; reset clears).
  - `void odin3_abc_fake_gate_names(const char *and2, const char *inv);` names the fake's gates
    (default `"AND2"`, `"INV"`; reset restores them).
  - fake `get_cells` (in-memory mode only, any space): each loaded AND becomes a gate object
    named `and2` with truth `0x8`; a complemented fanin or complemented PO driver gets one `inv`
    object (truth `0x1`) per complemented source; constants are objects 0 and 1; `ci_from` by
    loaded PI name, as `get_luts`. Without a loaded AIG: RESULT "the fake engine has no cells".

- [ ] **Step 1: Write the failing tests.** In `test_abc_engine_abi.c`:
  `test_register_refuses_missing_load_genlib` (table with `load_genlib = NULL` -> `ODIN3_ERR_INVALID_ARG`, log line `odin3_abc_engine_register: engine 'fake' has no 'load_genlib' function`),
  same for `get_cells` (`test_register_refuses_missing_get_cells`), and
  `test_engine_version_is_2` (`TEST_ASSERT_EQUAL_UINT32(2, ODIN3_ABC_ENGINE_VERSION)`).
  Change the three `test_abi_version_is_4` to `test_abi_version_is_5` asserting 5; `test_api.c`
  asserts 5; `test_python_walk.py:284` asserts 2.
- [ ] **Step 2:** build; run `ctest --preset debug -R 'test_abc_engine_abi|test_abi_ir|test_api|test_ast_abi'`; expect compile errors (fields missing).
- [ ] **Step 3: Implement** the header, `src/api/abc.c`, the fake engine functions above, the version strings in the CMake regex and the two user docs. CHANGELOG `## [Unreleased]` / `### Changed`: `- ABI 5: the ABC engine table gains load_genlib and get_cells (gate mapping); ODIN3_ABC_ENGINE_VERSION 2. No API stability promise (docs/PHASE7.md decision 4).`
- [ ] **Step 4:** build; `ctest --preset debug -R 'test_abc_engine_abi|test_abi_ir|test_api|test_ast_abi|cli_version|python_tools'` pass. The linked engine's table has no `load_genlib`/`get_cells` until Task 2, so registration fails and the real-ABC tests fail: do not push between Task 1 and Task 2. Lint.
- [ ] **Step 5: Commit** (local only) `feat(abi): ABI 5, engine load_genlib and get_cells (gatemap spec 4)`.

### Task 2: the linked engine's load_genlib and get_cells

**Files:**
- Modify: `adapters/abc/bridge.{c,h}`, `adapters/abc/session.{c,h}`, `adapters/abc/adapter.c`,
  `adapters/abc/README.md` (the engine table section), `tests/unit/test_abc_adapter.c`.

**Interfaces:**
- Consumes: Task 1's `odin3_abc_cells`, `ODIN3_ABC_CELLS_*`.
- Produces (bridge.h, same contract style as `odin3_bridge_get_luts`):
  - `odin3_status odin3_bridge_load_genlib(const char *text, odin3_abc_fail *fail);`
    writable copy (`odin3_abc_alloc`), `Mio_Library_t *lib = Mio_LibraryRead(ADP_GENLIB_NAME, copy, NULL, 0, 0)` with `ADP_GENLIB_NAME "odin3.genlib"`, free the copy; NULL -> RESULT `ABC cannot read the gate library`; else `Mio_UpdateGenlib(lib)`. Add `#include "map/mio/mio.h"` after `base/main/main.h`.
  - `odin3_status odin3_bridge_get_cells(uint32_t space, odin3_abc_cells *cells, odin3_abc_fail *fail);`
  - session.h: `odin3_abc_session_load_genlib(void *self, const char *text, odin3_abc_fail *fail)` and `odin3_abc_session_get_cells(void *self, uint32_t space, odin3_abc_cells *cells, odin3_abc_fail *fail)`, built like `odin3_abc_session_get_luts` (`adp_ready`, NULL argument -> RESULT, `odin3_guard_run`, `odin3_guard_finish`).
- `get_cells` algorithm (bridge.c; arrays from `odin3_abc_alloc`; a converted network is kept in `adp_cells_ntk`; a new `static void adp_free_cells(bool delete_ntk)` frees the arrays and, when `delete_ntk`, `Abc_NtkDelete`s `adp_cells_ntk`: `get_cells` calls it with true first, `odin3_bridge_frame_close` with `destroy`, so after an escape the network leaks with the frame):
  1. NTK: `ntk = Abc_FrameReadNtk(adp_frame)`; `ntk == NULL || !Abc_NtkIsMappedLogic(ntk)` -> RESULT `no gate-mapped network in the network space`. GIA: `gia = Abc_FrameReadGia(adp_frame)`; `gia == NULL || !Gia_ManHasCellMapping(gia)` -> RESULT `no cell mapping in the & space`; `ntk = adp_cells_ntk = Abc_NtkFromCellMappedGia(gia, 0)` (declare it `extern`, as `Abc_FrameReadMiniLutNameMapping` is; abcDar.c:1152).
  2. `Abc_NtkBoxNum(ntk) != Abc_NtkLatchNum(ntk)` -> RESULT `a box that is not a latch`.
  3. `Vec_Ptr_t *order = Abc_NtkDfsIter(ntk, 1)`. A node with `Abc_ObjFaninNum == 0` is a constant: its `pData` gate's `Mio_GateReadTruth` 0 gives object 0, else object 1; `pData == NULL` -> RESULT `a node without a library gate`. A node with fanins and `pData == NULL` -> the same RESULT. Other nodes are gates.
  4. Objects: 0, 1; PIs (`Abc_NtkForEachPi`); register outputs (`Abc_ObjFanout0(latch)` per `Abc_NtkForEachLatch`); gates in `order`; POs (`Abc_NtkForEachPo`, driver `Abc_ObjFanin0(po)`); register inputs (driver `Abc_ObjFanin0(Abc_ObjFanin0(latch))`). `int32_t` map by `Abc_ObjId`, size `Abc_NtkObjNumMax(ntk)`.
  5. `lut_size = max(2, widest gate)`; fanins `0x7FFFFFFF` unused, a PO `0x7FFFFFFE` in slot 1; truths `W = lut_size > 5 ? 2 : 1` words per object from the 64-bit `Mio_GateReadTruth` (low word first).
  6. `names`: `Mio_GateReadName` of each distinct gate in first-appearance order (pointers into the library, valid until close); `gate[obj]` its index.
  7. `ci_from`: NTK from `adp_ci_index(Abc_ObjName(ci))` over the PIs, then the register outputs; GIA from `adp_ci_from_names(gia)`.
- `adapter.c`: `.load_genlib = odin3_abc_session_load_genlib, .get_cells = odin3_abc_session_get_cells`.

- [ ] **Step 1: Write the failing tests** in `test_abc_adapter.c` (real ABC; a static `k_cmos6_genlib` equal to Task 4's cmos6 golden):
  - `test_load_genlib_and_map`: open; `load_genlib(k_cmos6_genlib)`; `load_aig` of y = a AND b; `&put`; `strash`; `map`; `get_cells(NTK)` -> OK, `net.n_regs == 0`, every `names[i]` in {BUF, INV, NAND2, NOR2}, PO driver is a gate object, and for each gate object named NAND2 `(truths[W*obj] & 0xF) == 0x7`.
  - `test_get_cells_gia`: the same AIG, `&st`, `&nf`, `get_cells(GIA)` -> OK, same name set.
  - `test_get_cells_registers`: an AIG with one register; after `strash; map`, `net.n_regs == 1` and `ci_from` of the register PI is the loaded register CI's index.
  - `test_get_cells_constant`: PO = constant 1; after `strash; map` the PO driver (slot 0) is 1.
  - `test_load_genlib_rejects`: text `"GATE"` -> `ODIN3_ERR_ENGINE`, kind RESULT, text `ABC cannot read the gate library`.
  - `test_get_cells_unmapped`: no `map` -> RESULT `no gate-mapped network in the network space`; GIA -> `no cell mapping in the & space`.
  - `test_adapter_oom` gains an OOM pass over `get_cells` (fail allocation n for n = 0.. until OK; every failure is `ODIN3_ERR_NO_MEMORY`, no leak under ASan).
- [ ] **Step 2:** build; `ctest --preset debug -R test_abc_adapter` fails (functions missing).
- [ ] **Step 3:** implement as above; README's engine table lists the two calls.
- [ ] **Step 4:** `ctest --preset debug -R 'test_abc'` passes (Debug ASan/LSan; `adapters/abc/lsan.supp` must not grow). Lint.
- [ ] **Step 5: Commit** `feat(abc-adapter): load_genlib and get_cells (gatemap spec 4.2, 4.3)`; push; PR 1.

---

## PR 2 `feat/gatemap-lib`: library view, cmos6, genlib text

### Task 3: gate truth helper, library cells, cmos6

**Files:**
- Create: `src/sim/gate_truth.{c,h}`, `src/techlib/libcells.{c,h}`, `lib/cmos6.o3lib`,
  `tests/unit/test_libcells.c`.
- Modify: `src/passes/abc/region.c:197-260` (use the helper), `CMakeLists.txt` (sources to
  `odin3_core`, `odin3_add_unit_test(test_libcells)` with `ODIN3_LIB_DIR`),
  `tests/unit/test_techlib_libs.c` (add cmos6).

**Interfaces:**
- Produces `src/sim/gate_truth.h`:
  - `enum { ODIN3_GATE_TRUTH_MAX_INPUTS = 6 };`
  - `bool odin3_gate_truth_eligible(const odin3_celltype_def *def);` region.c's `gate_eligible`, moved unchanged (BIT granularity, no parameters, a simulate hook, none of SEQ_EDGE/SEQ_LEVEL/TRISTATE/ANYVIEW, 1-bit non-inout ports, one output, 1 to 6 inputs).
  - `uint32_t odin3_gate_truth_inputs(const odin3_celltype_def *def);`
  - `uint64_t odin3_gate_truth(const odin3_celltype_def *def);` region.c's `gate_truth` (bit i = output for input assignment i, input j = j-th input port in port order); only for an eligible def.
  - `bool odin3_gate_const_value(const odin3_celltype_def *def, int *value);` true for a BIT, parameterless type with a simulate hook and exactly one 1-bit OUT port and no other port; *value = its simulated output (0 or 1).
  - region.c: `_Static_assert(ODIN3_GATE_TRUTH_MAX_INPUTS == ODIN3_ABC_GATE_MAX_INPUTS, ...)`.
- Produces `src/techlib/libcells.h`:
  ```c
  typedef enum odin3_libcell_skip {
      ODIN3_LIBCELL_NOT_GATE = 1, /* kind hard or blackbox */
      ODIN3_LIBCELL_STATE,        /* a seq cell (dfflibmap's) */
      ODIN3_LIBCELL_CONSTANT,     /* no input: a TIE cell (the splice's, G-7) */
      ODIN3_LIBCELL_SHAPE,        /* not one fn output and 1 to 6 1-bit inputs without parameters */
      ODIN3_LIBCELL_NO_SUPPORT,   /* the function ignores input `port` */
      ODIN3_LIBCELL_NAME          /* port `port` is not a genlib name */
  } odin3_libcell_skip;
  typedef struct odin3_libgate {
      odin3_celltype_id type;
      uint32_t n_inputs;
      uint64_t table;                                /* odin3_gate_truth */
      uint32_t in_port[ODIN3_GATE_TRUTH_MAX_INPUTS]; /* port index of input j */
      uint32_t out_port;
      uint32_t area, delay;                          /* strtab IDs of the texts, 0 absent */
  } odin3_libgate;
  typedef struct odin3_libstate {
      odin3_celltype_id type;
      odin3_techlib_trigger trigger;
      int init;                                      /* 0, 1, or -1 for x */
      uint32_t clock_port, data_port, q_port;
      uint32_t area;
  } odin3_libstate;
  typedef struct odin3_libskip { odin3_celltype_id type; uint8_t why; uint32_t port; } odin3_libskip;
  typedef struct odin3_libcells {
      uint32_t library;                 /* strtab ID of the name, 0 when no cell has it */
      uint32_t n_cells;                 /* cells of the library */
      odin3_vec gates;                  /* odin3_libgate, library order */
      odin3_vec states;                 /* odin3_libstate, library order */
      odin3_vec skips;                  /* odin3_libskip: every cell not in gates, library order */
      odin3_celltype_id buf, inv, tie0, tie1;   /* {0} when none */
  } odin3_libcells;
  odin3_status odin3_libcells_collect(const odin3_design *design, const char *library, odin3_libcells *out);
  void odin3_libcells_free(odin3_libcells *cells);
  bool odin3_libcells_area(const odin3_design *design, uint32_t area_str, double *out);
  bool odin3_libstate_of(const odin3_design *design, odin3_celltype_id type, odin3_libstate *out);
  const char *odin3_libcell_skip_text(uint8_t why);
  ```
  Rules (libcells.c header comment says them): library cells are the types with
  `odin3_techlib_cell_get(...)->library` equal to the name's strtab ID, in type-ID order (= file
  order). Gate: techlib kind GATE, `n_fns == 1`, `n_seqs == 0`, `memory == NULL`,
  `odin3_gate_truth_eligible`, every input in the support (the two cofactors of `table` on input
  j differ), every port name matches `[A-Za-z_][A-Za-z0-9_]*` and is not `CONST0`/`CONST1`. State
  cell (`odin3_libstate_of`): kind GATE, `n_seqs == 1`, `n_fns == 0`, no memory, no parameters,
  exactly three 1-bit ports: the seq's clock input, one more input whose name is the seq's data
  expression (`ODIN3_EXPR_IDENT`), the seq's output; init NULL is -1, else
  `odin3_expr_eval_int_quiet` gives 0 or 1 (anything else: not a state cell, reason SHAPE).
  `buf` = gate with 1 input and table `0x2`, `inv` = table `0x1`, `tie0`/`tie1` = CONSTANT cells
  with `odin3_gate_const_value` 0/1; each the smallest area (`odin3_libcells_area`: `strtod` of
  the area text, false when absent), absent area after every numeric one, ties to library order.
  `collect`: ODIN3_OK with `n_cells == 0` for an unknown name; NO_MEMORY; never logs. Skip texts:
  `not a gate cell`, `a flip-flop or latch`, `a constant cell (used for constants)`, `not one
  output and 1 to 6 one-bit inputs without parameters`, `its function ignores input '%s'`,
  `port '%s' is not a genlib name`.
- `lib/cmos6.o3lib` (exact cells, header comment: "the six cells of Yosys's
  examples/cmos/cmos_cells.lib under Gate-Breaker's names (gatemap spec 8); DFFSR is a black box:
  .o3lib's seq has no asynchronous set or reset (GM-7); its function: Q <= D on posedge C, S high
  sets Q, R high resets Q, both asynchronous, S wins over R"):
  `library cmos6`; `cell BUF gate area 6` (in A, out Y, `fn Y = A`); `cell INV gate area 3`
  (`fn Y = ~A`); `cell NAND2 gate area 4` (in A, B; `fn Y = ~(A & B)`); `cell NOR2 gate area 4`
  (`fn Y = ~(A | B)`); `cell DFF gate area 18` (`in C 1 clock ; in D 1 ; out Q 1`,
  `seq Q <= D @ posedge C init x`); `cell DFFSR blackbox area 18` (`in C 1 clock ; in D 1 ; in S 1 ; in R 1 ; out Q 1`).

- [ ] **Step 1: Write the failing tests** (`test_libcells.c`, reading `ODIN3_LIB_DIR` files with `odin3_techlib_read`):
  - `test_collect_generic_gates`: 13 gates in order BUF INV AND2 AND3 AND4 OR2 OR3 OR4 NAND2 NOR2 XOR2 XNOR2 MUX2; tables `0x2 0x1 0x8 0x80 0x8000 0xE 0xFE 0xFFFE 0x7 0x1 0x6 0x9 0xCA`; 4 states (DFFP POSEDGE, DFFN NEGEDGE, DLATCHP HIGH, DLATCHN LOW, all init -1, data_port 0, clock_port 1, q_port 2); skips = the 4 state cells (STATE) and TIE0, TIE1 (CONSTANT); `buf`, `inv`, `tie0`, `tie1` are BUF, INV, TIE0, TIE1.
  - `test_collect_cmos6`: gates BUF INV NAND2 NOR2; one state DFF (clock_port 0, data_port 1, q_port 2); skips DFF (STATE), DFFSR (NOT_GATE); `tie0`/`tie1` none.
  - `test_collect_unknown_library`: OK, `n_cells == 0`.
  - `test_genlib_skips_non_support_input` (Review Focus 1): inline library `cell G gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = A ; end` -> skip NO_SUPPORT with port 1, text `its function ignores input 'B'`.
  - `test_genlib_skips_reserved_port_name`: ports `CONST0` and `A$1` -> NAME skips.
  - `test_buf_smallest_area_then_order`: two BUF-function cells, areas 3 and 2 -> the area-2 one; equal areas -> the first.
  - `test_libstate_init_constant`: `seq Q <= D @ posedge C init 1'b1` -> init 1; `init 2` -> not a state cell.
  - `test_techlib_libs`: `lib/cmos6.o3lib` reads with 6 cell types.
- [ ] **Step 2:** build fails (headers missing).
- [ ] **Step 3:** implement; move `gate_eligible`/`gate_truth` out of region.c (region.c keeps its callers' behaviour: `type_kind`, `type_entry` call the helper).
- [ ] **Step 4:** `ctest --preset debug -R 'test_libcells|test_techlib|test_abc_region|test_abc_splice|test_abc_pass'` pass. Lint.
- [ ] **Step 5: Commit** `feat(techlib): gate truth helper, library cell view, lib/cmos6.o3lib`.

### Task 4: the genlib text

**Files:** Create `src/passes/abc/genlib.{c,h}`, `tests/unit/test_abc_genlib.c`,
`tests/unit/data/genlib/generic_gates.genlib`, `tests/unit/data/genlib/cmos6.genlib`.
Modify `CMakeLists.txt` (source, test with `ODIN3_LIB_DIR` and `ODIN3_GENLIB_DIR`).

**Interfaces:**
- Consumes: Task 3's `odin3_libcells`, `odin3_libcells_area`; `odin3_blif_isop_set`
  (`src/backends/blif/isop.h`); `odin3_sha256_*` (`src/util/sha256.h`).
- Produces `genlib.h`:
  - `#define ODIN3_ABC_GENLIB_CONST0 "$odin3_const0"` and `..._CONST1 "$odin3_const1"`.
  - `odin3_status odin3_abc_genlib_check(const odin3_design *design, const odin3_libcells *cells, const char *library);` logs one ERROR and returns `ODIN3_ERR_INVALID_ARG`: `abc: --gates: library '<L>' is not loaded; read it with read_techlib first` (`n_cells == 0`); `abc: --gates: library '<L>': cell '<C>' has no numeric area` (first gate without one); `abc: --gates: library '<L>' has no inverter and 2-input AND, NAND, OR or NOR gate; ABC cannot map to it` (no gate with 1 input and table 0x1, or none with 2 inputs and table 0x8, 0x7, 0xE or 0x1).
  - `odin3_status odin3_abc_genlib_text(const odin3_design *design, const odin3_libcells *cells, odin3_strbuf *out);` appends the text; NO_MEMORY.
  - `void odin3_abc_genlib_sha256(const odin3_strbuf *text, char hex[ODIN3_SHA256_HEX_CHARS]);`
- Text format (exact): per gate in library order `GATE <name> <area text> <out port>=<formula>;\n`, then per input j in port order `PIN <input name> UNKNOWN 1 999 <d> 0 <d> 0\n` with `<d>` the delay text or `1`; after the gates `GATE $odin3_const0 0 Y=CONST0;\nGATE $odin3_const1 0 Y=CONST1;\n`. Formula: the rows of `odin3_blif_isop_set(&table, n, false, ...)` in emitted order joined by `+`; a row's literals for j ascending, plane `1` -> name, `0` -> `!name`, `-` -> none, joined by `*`; no blanks.

- [ ] **Step 1: Write the failing tests:**
  - `test_genlib_cmos6_golden`: text equals `cmos6.genlib`, exactly:
    ```
    GATE BUF 1 Y=A;
    PIN A UNKNOWN 1 999 1 0 1 0
    GATE INV 1 Y=!A;
    PIN A UNKNOWN 1 999 1 0 1 0
    GATE NAND2 1 Y=!B+!A;
    PIN A UNKNOWN 1 999 1 0 1 0
    PIN B UNKNOWN 1 999 1 0 1 0
    GATE NOR2 1 Y=!A*!B;
    PIN A UNKNOWN 1 999 1 0 1 0
    PIN B UNKNOWN 1 999 1 0 1 0
    GATE $odin3_const0 0 Y=CONST0;
    GATE $odin3_const1 0 Y=CONST1;
    ```
  - `test_genlib_generic_golden`: equals `generic_gates.genlib`, whose GATE lines are (each followed by its PIN lines as above): `BUF 1 Y=A`, `INV 1 Y=!A`, `AND2 1 Y=A*B`, `AND3 2 Y=A*B*C`, `AND4 2 Y=A*B*C*D`, `OR2 1 Y=B+A`, `OR3 2 Y=C+B+A`, `OR4 2 Y=D+C+B+A`, `NAND2 1 Y=!B+!A`, `NOR2 1 Y=!A*!B`, `XOR2 2 Y=A*!B+!A*B`, `XNOR2 2 Y=!A*!B+A*B`, `MUX2 2 Y=A*!S+B*S` (pins of MUX2: A, B, S), then the two constant gates.
  - `test_genlib_delay_text`: a gate with `delay 2.5` -> `PIN A UNKNOWN 1 999 2.5 0 2.5 0`.
  - `test_genlib_check_errors`: unknown library, a gate without area, a library of only NAND2 -> the three texts above, each `ODIN3_ERR_INVALID_ARG`.
  - `test_genlib_sha256_stable`: two derivations of generic_gates give the same 64-digit hex.
- [ ] **Step 2:** build fails.
- [ ] **Step 3:** implement; write the two golden files from the texts above (do not regenerate them from the code).
- [ ] **Step 4:** `ctest --preset debug -R test_abc_genlib` passes. Lint.
- [ ] **Step 5: Commit** `feat(abc): genlib text from an .o3lib library (gatemap spec 3)`; push; PR 2.

---

## PR 3 `feat/gatemap-script`: checker and `gates.abc`

### Task 5: the cell-mapped state, `map` and `&nf`

**Files:** Modify `src/passes/abc/script.{c,h}`, `src/passes/abc/allow.c`,
`tests/unit/test_abc_script.c` (and any test asserting the old end message: grep
`leaves the network unmapped` under `tests/`).

**Interfaces:**
- Produces (script.h):
  - Effects appended after `ODIN3_ABC_FX_GIA_KEEP`: `ODIN3_ABC_FX_NTK_CELLMAPPED`, `ODIN3_ABC_FX_GIA_CELLMAPPED`.
  - `typedef struct odin3_abc_script_mode { uint32_t lut_k; uint32_t exchange; bool gates; } odin3_abc_script_mode;`
  - `odin3_status odin3_abc_script_parse_mode(const odin3_abc_script_src *src, const odin3_abc_script_mode *mode, odin3_abc_script *out);` `odin3_abc_script_parse` becomes a wrapper with `gates = false` (no caller changes).
- script.c: state `ABC_CELLMAPPED = 4`; `abc_parse` gains `bool gates`.
- allow.c rows (in the mapping and & groups): `{"map", ODIN3_ABC_CLASS_MAP, ODIN3_ABC_FX_NTK_CELLMAPPED, "", <Task 6>}`, `{"&nf", ODIN3_ABC_CLASS_GIA, ODIN3_ABC_FX_GIA_CELLMAPPED, "MZ", <Task 6>}` with header citations: `&nf -Z abc.c:47206 ZFile`, `&nf -M nMaxMatches: Nf_ManDumpMatchesBin writes the matches (giaNf.c:2279-2292)`; `map` has no file option (getopt `"DABFSGMarspfuovh"`, abc.c:21453).
- Rules (exact messages, `<from>` as the existing `from.pre/name/post`):
  - not gates, `map`/`&nf`: `'<cmd>'<from> maps to a gate library, which only abc --gates loads`.
  - `map` when `ntk != ABC_AIG`: `'map'<from> needs a strashed network; add 'strash'`.
  - `&nf` joins `&if`'s rules (`if_gia` true for both GIA mapper effects; `ABC_CELLMAPPED` counts as mapped there): the existing texts with `'&nf'`.
  - `mfs`, `mfs2`, `lutpack` when `ntk == ABC_CELLMAPPED`: `'<cmd>'<from> works on LUT networks, not on the gate-mapped network here; add 'strash'`; `&mfs` when `gia == ABC_CELLMAPPED`: same with `add '&st'`.
  - `get_state`: a CELLMAPPED network gives `ABC_AIG` (`&get` drops a gate mapping); `&put` copies `gia` as today (CELLMAPPED through `Abc_NtkFromCellMappedGia`, abc.c:35267).
  - `check_end`: gates and end state != CELLMAPPED: `the script does not end gate-mapped; with --gates end it with a gate mapper ('map' or '&nf')`; not gates and != MAPPED: `the script leaves the network unmapped; end it with a LUT mapper such as 'if -K {K}' (with --gates: a gate mapper, 'map' or '&nf')`.

- [ ] **Step 1: Write the failing tests** (`test_abc_script.c`, mode `{0, ODIN3_ABC_EXCHANGE_AIG, true}` unless said):
  `test_map_needs_gates` (`strash\nmap\n` without gates -> PARSE, first message above);
  `test_nf_needs_gates`; `test_gates_end_map` (`strash\nmap\n` -> OK, `final_space` NTK);
  `test_gates_end_nf` (`&st\n&nf\n` -> OK, GIA); `test_gates_end_nf_put` (`&st\n&nf\n&put\n` -> OK, NTK);
  `test_gates_refuses_lut_ending` (`strash\nif -K 4\n` -> the gates end message);
  `test_map_needs_strashed` (`strash\nif -K 4\nmap\n` -> strash message);
  `test_nf_needs_unmapped_gia` (`&st\n&nf\n&nf\n` -> `needs an unmapped network in the & space`);
  `test_mfs_refuses_cellmapped` (`strash\nmap\nmfs\n`);
  `test_unmapped_end_message_names_both` (`strash\n` without gates);
  `test_nf_file_letters` (`&st\n&nf -Z x\n` and `-M 3` refused as file options).
- [ ] **Step 2:** fails. **Step 3:** implement. **Step 4:** `ctest --preset debug -R 'test_abc_script|test_abc_default_script|fuzz_abc_script'` pass. Lint.
- [ ] **Step 5: Commit** `feat(abc): checker state for gate mapping, map and &nf rows (gatemap spec 5)`.

### Task 6: `lib/abc/gates.abc`, its embedding, and the crash audit of `map`/`&nf`

**Files:** Create `lib/abc/gates.abc`, `cmake/abc_gates_script.c.in`. Modify `CMakeLists.txt`
(next to lines 445-454 and the install line 541), `src/passes/abc/script.h`,
`src/passes/abc/allow.c` (crash letters and their citations), `tests/unit/test_abc_script.c`.

**Interfaces:**
- `lib/abc/gates.abc` (exact):
  ```
  # lib/abc/gates.abc: the default script of abc --gates (gatemap spec G-5): the optimisation
  # steps of lib/abc/vtr.abc, then area-oriented mapping to the library abc --gates loaded.
  strash
  ifraig -v
  scorr -v
  dc2 -v
  dch -f
  map -a
  print_stats
  ```
- script.h: `#define ODIN3_ABC_GATES_SCRIPT_NAME "builtin:lib/abc/gates.abc"`,
  `extern const unsigned char odin3_abc_gates_script[]; extern const uint32_t odin3_abc_gates_script_len;`
  generated as `generated/abc/gates_script.c` from the new template (a copy of
  `cmake/abc_default_script.c.in` with these names and `@ODIN3_ABC_GATES_SCRIPT_BYTES@`).
  `install(FILES lib/abc/vtr.abc lib/abc/gates.abc ...)`.
- Crash audit (procedure fixed; the result is data): with the Release `odin3-abc` binary, in a
  scratch directory under the build tree, the cmos6 golden text of Task 4 saved as `g.genlib`, designs
  `tests/golden/blif/ff.odin.blif` (sequential) and `tests/unit/data/abc/dup_fanin.blif`
  (combinational): run `odin3-abc -c "read_genlib g.genlib; read <d>; strash; map <opts>"` and
  `odin3-abc -c "read_genlib g.genlib; read <d>; &get -n; &st; &nf <opts>"` under `timeout 60`,
  for each valued letter alone at `0 1 64 -1 3 1000` and every pair of flag letters; plus
  `dch -f; map` and `&get -n; &st; &dch -f; &nf`. A signal exit (>= 128) or a timeout puts the
  letter in `crash_opts` with a citation line in allow.c's header (`map -X abc.c:<line> <field>:
  <crash site>`), as the 3D Task 11 audit did; a crash of `&dch -f; &nf` makes `&nf` refuse native
  choices with the `&if` choices message. Record the audit table in the PR description.

- [ ] **Step 1: Write the failing tests:** `test_gates_script_checks` (parse `odin3_abc_gates_script` with mode `{0, AIG, true}` -> OK, `n_cmds == 7`, `cmds[5].line == "map -a"`, final NTK); `test_gates_script_needs_gates` (same bytes without gates -> PARSE); `test_map_nf_crash_letters` (each audited crash letter refused; if the audit finds none, assert `crash_opts` is `""` for both rows).
- [ ] **Step 2:** fails (symbols missing). **Step 3:** embed, run the audit, fill the rows.
- [ ] **Step 4:** `ctest --preset debug -R 'test_abc_script|test_abc_default_script|python_tools'` pass. Lint.
- [ ] **Step 5: Commit** `feat(abc): lib/abc/gates.abc and the map/&nf option audit`; push; PR 3.

---

## PR 4 `feat/write-blif-gates`: `write_blif --gates-as-names`

### Task 7: the option

**Files:** Modify `src/backends/blif/writer.{c,h}`, `src/passes/builtin.c` (`wb_option`,
`wb_check_options`, the pass help text), `docs/PASSES.md:34`, `tests/unit/test_blif_writer.c`,
`CMakeLists.txt` (CTest `cli_write_blif_gates_vtr_refused`).

**Interfaces:**
- Consumes: `odin3_gate_truth_eligible`, `odin3_gate_truth`, `odin3_gate_const_value`
  (Task 3), `odin3_libstate_of` (Task 3), `odin3_blif_isop`, the writer's `write_constant`,
  `write_cover`, `pin_name`.
- Produces: `odin3_blif_write_opts.gates_as_names` (`bool`, PLAIN only). In `write_cell`, after
  the `vtr_const_cell` check and before the built-in dispatch, when set:
  - an eligible gate type (`$_AND_` and the other bit gates, library gates): `.names <input pins
    in port order> <output>` with `odin3_blif_isop`'s cover (constant tables as `write_lut`
    writes them);
  - a type with `odin3_gate_const_value`: the zero-input generator (`.names Y`, plus row `1`);
  - a library state cell: `.latch <D> <Q> re|fe|ah|al <C> <init>` (POSEDGE re, NEGEDGE fe,
    HIGH ah, LOW al; init 0, 1, or 2 for x);
  - everything else as today (`$_DFF_*` `.latch`, other library cells `.subckt`).
- `--gates-as-names` with `--vtr`: `write_blif: --gates-as-names is for the plain dialect`
  (INVALID_ARG). Without the option the bytes are unchanged.

- [ ] **Step 1: Write the failing tests** (`test_blif_writer.c`, designs read with
  `read_techlib` + `read_blif` of inline text as the file's other tests do):
  `test_gates_as_names_library_gate` (NAND2 A=a B=b Y=y -> lines `.names a b y` and `11 0`);
  `test_gates_as_names_builtin_gate` (`$_AND_` -> `.names` with `11 1`);
  `test_gates_as_names_state_cell` (generic DFFP D=d C=c Q=q -> `.latch d q re c 2`);
  `test_gates_as_names_tie` (TIE1 Y=y -> `.names y` then `1`);
  `test_gates_as_names_off_keeps_subckt` (no option: `.subckt NAND2 A=a B=b Y=y`);
  `test_gates_as_names_blackbox_stays_subckt` (cmos6 DFFSR -> `.subckt DFFSR`);
  CTest `cli_write_blif_gates_vtr_refused` (exit non-zero, the message).
- [ ] **Step 2:** fails. **Step 3:** implement; PASSES row: add the option and its rule.
- [ ] **Step 4:** `ctest --preset debug -R 'test_blif_writer|blif_|cli_write_blif'` pass. Lint.
- [ ] **Step 5: Commit** `feat(write_blif): --gates-as-names (gatemap spec 7, G-8)`; push; PR 4.

---

## PR 5 `feat/dfflibmap`: the pass

### Task 8: choosing a state cell

**Files:** Create `src/passes/dfflibmap/{dfflibmap.h,choose.c}`, `tests/unit/test_dfflibmap.c`.
Modify `CMakeLists.txt`.

**Interfaces:**
- Consumes: `odin3_libcells`, `odin3_libstate`, `odin3_libcells_area` (Task 3).
- Produces (`dfflibmap.h`, internal):
  ```c
  typedef enum odin3_dlm_kind { ODIN3_DLM_DFF_P = 0, ODIN3_DLM_DFF_N, ODIN3_DLM_DLATCH_P,
                                ODIN3_DLM_DLATCH_N, ODIN3_DLM_KINDS } odin3_dlm_kind;
  typedef struct odin3_dlm_choice { uint32_t state; bool invert_clock; } odin3_dlm_choice;
  bool odin3_dlm_choose(const odin3_design *design, const odin3_libcells *cells,
                        odin3_dlm_kind kind, int64_t init, odin3_dlm_choice *out);
  ```
  Rule (spec 6): trigger P POSEDGE, N NEGEDGE, LATCH_P HIGH, LATCH_N LOW; compatible when the
  cell's init is -1, or `init` is 2 or 3, or they are equal; among compatible cells the smallest
  numeric area, a cell without area after every numeric one, then library order; `state` =
  index into `cells->states`. DFF_N with no NEGEDGE match: the POSEDGE choice with
  `invert_clock = true`, only when `cells->inv` is set. Latches never invert. False: no match.

- [ ] **Step 1: Write the failing tests:** `test_choose_posedge_generic` (DFF_P INIT 3 -> DFFP);
  `test_choose_area_tie_break` (inline library: posedge cells areas 5 then 3 -> the 3; equal ->
  first); `test_choose_init_rules` (cell init 0: node INIT 1 -> false, INIT 2 -> true);
  `test_choose_negedge_fallback_inverts` (cmos6, DFF_N -> DFF, `invert_clock`);
  `test_choose_no_inverter_no_fallback` (posedge-only library without INV -> false);
  `test_choose_no_latch_in_cmos6` (DLATCH_P -> false).
- [ ] **Step 2:** fails. **Step 3:** implement. **Step 4:** `ctest -R test_dfflibmap` passes. Lint.
- [ ] **Step 5: Commit** `feat(dfflibmap): state cell choice (gatemap spec 6)`.

### Task 9: the pass

**Files:** Create `src/passes/dfflibmap/dfflibmap.c`, `tests/golden/dfflibmap/{input.blif,expected.blif,README}`.
Modify `src/passes/builtin.{c,h}` (register `odin3_pass_dfflibmap` after `odin3_pass_lpmmap`),
`docs/PASSES.md` (new row after `lpmmap`), `CMakeLists.txt` (CTest `cli_dfflibmap_golden`),
`tests/unit/test_dfflibmap.c`.

**Interfaces:**
- Produces: `extern const odin3_pass_def odin3_pass_dfflibmap;` = `{"dfflibmap", "dfflibmap
  <library>: map $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and $_DLATCH_N_ onto the library's flip-flops
  and latches", dlm_pass_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE}`.
- Order: parse (exactly one word, else `dfflibmap: expects one library name`); collect
  (`n_cells == 0`: `dfflibmap: library '<L>' is not loaded; read it with read_techlib first`);
  plan every module (module-ID order, node-ID order) before any change; then apply.
- Planning errors (first one logged, INVALID_ARG, IR untouched):
  `dfflibmap: module '<m>': no cell of library '<L>' implements <type> '<cell>' (<posedge|negedge|high|low>, INIT <n>)`;
  `dfflibmap: module '<m>': cell '<cell>' of type <T> is a flip-flop or latch dfflibmap cannot map (only $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and $_DLATCH_N_)`
  for a type with `ODIN3_CT_SEQ_EDGE|ODIN3_CT_SEQ_LEVEL`, granularity BIT or WORD, no techlib data.
  `<cell>` is the node name or `$c<ID>`.
- Apply per node: ports of the old cell C(0), D(1), Q(2) (INIT parameter 0); when
  `invert_clock`, one inverter per clock net per module (a `u64map` clock net -> new net; the
  INV instance and its net take the first flip-flop's provenance); `odin3_node_create_connected`
  with the library cell's ports (clock_port, data_port, q_port), name 0, prov = the old node's;
  then `odin3_node_delete(old)`; then `odin3_node_rename(new, old name)`. A failed create leaves
  the old node; a failed delete deletes the new node. Attributes are not copied.
- Logs: INFO `dfflibmap: library '<L>': <n> cells mapped (<cell> x<k>, ... in library order), <i> clock inverters`;
  WARNING when any INIT 0 or 1 went to an init-x cell: `dfflibmap: <n> flip-flops or latches with INIT 0 or 1 became cells whose init is x; their initial value is lost`.

- [ ] **Step 1: Write the failing tests:** `test_pass_maps_and_keeps_name_and_prov` (`$_DFF_P_`
  named `q_reg` -> DFFP named `q_reg`, same prov ID, same D/C/Q nets, `check` OK);
  `test_pass_shares_clock_inverter` (two `$_DFF_N_` on one clock, cmos6 -> one INV, two DFF);
  `test_pass_no_match_changes_nothing` (cmos6 + `$_DLATCH_P_` -> INVALID_ARG, live node count
  unchanged, the message); `test_pass_refuses_other_state_type` (a `$dff`);
  `test_pass_library_not_loaded`; `test_pass_init_lost_warns` (INIT 1 + generic -> the WARNING);
  `test_pass_args` (no word, two words). Golden `cli_dfflibmap_golden`: `read_techlib
  lib/generic_gates.o3lib; read_blif tests/golden/dfflibmap/input.blif; dfflibmap generic_gates;
  write_blif out.blif`, `tools/netlist-compare/netlist-compare out.blif expected.blif` exit 0;
  input has one `.latch` of each kind `re fe ah al`; expected.blif is written by hand from the
  rule (`.subckt DFFP D=.. C=.. Q=..` etc.).
- [ ] **Step 2:** fails. **Step 3:** implement; PASSES row: `dfflibmap <library>` | post-1.0
  (gatemap) | netlist -> netlist | `src/passes/dfflibmap/{dfflibmap,choose}.c` | `test_dfflibmap`,
  `cli_dfflibmap_golden` | the rules above.
- [ ] **Step 4:** `ctest --preset debug -R 'dfflibmap|test_pass_manager'` pass. Lint.
- [ ] **Step 5: Commit** `feat(dfflibmap): the pass (gatemap spec 6, G-6)`; push; PR 5 (Peter reviews).

---

## PR 6 `feat/abc-gates`: `abc --gates`

### Task 10: option and start-up

**Files:** Modify `src/passes/abc/{abc.h,args.c,run.h,run.c}`, `tests/unit/test_abc_args.c`.

**Interfaces:**
- Consumes: Tasks 3-6 (`odin3_libcells_collect`, `odin3_abc_genlib_check/_text/_sha256`,
  `odin3_abc_script_parse_mode`, `odin3_abc_gates_script`).
- Produces:
  - `odin3_abc_opts` gains `const char *gates;` and `bool prov_given;`; option `--gates` (value
    required: `abc: --gates needs a value`).
  - `struct odin3_abc_run` gains `bool gates; odin3_libcells lib; odin3_strbuf genlib; char genlib_sha256_hex[ODIN3_SHA256_HEX_CHARS];` (freed in `odin3_abc_run_destroy`).
  - Start-up with `--gates`, before the script choice, in this order: `--lut` given ->
    `abc: --lut does not apply with --gates`; `--via-blif` given -> `abc: --gates runs ABC in
    memory; --via-blif cannot be used with it` (both INVALID_ARG); collect; genlib check; one INFO
    `abc: note: library '<L>': cell '<C>' is not used for gate mapping: <skip text>` per skip;
    text and hash; INFO `abc: --gates library '<L>': <n> gates, genlib sha256 <16 hex>…`;
    `prov` forced CONE, with INFO `abc: note: --prov fine is not available with --gates;
    provenance uses cut cones (gatemap spec G-2)` when `prov_given` and fine; exchange AIG; script:
    `--script` else `ODIN3_ABC_GATES_SCRIPT_NAME` (resolved "embedded"); when the project or CLI
    option would have chosen one: INFO `abc: note: --gates does not use the project's or
    --abc-script's script; give --script to choose one`; parse with
    `{0, ODIN3_ABC_EXCHANGE_AIG, true}`; route LINKED.
  - `log_start`: `K=none, exchange aig, gates <L>`. `finish_info` adds `gates=<L>` and
    `genlib=<sha256>` after `exchange`. Help text adds `[--gates LIB]`.

- [ ] **Step 1: Write the failing tests** (fake engine): `test_gates_needs_library` (no
  read_techlib -> the not-loaded text); `test_gates_unknown_library`; `test_gates_with_lut_refused`;
  `test_gates_with_via_blif_refused`; `test_gates_lut_script_refused` (`--script` file
  `strash\nif -K 4\n` -> the gates end message); `test_gates_default_script_is_gates_abc` (run
  info `script=builtin:lib/abc/gates.abc`, `gates=generic_gates`); `test_gates_prov_fine_note`;
  `test_gates_logs_skipped_cells` (generic: six note lines); `test_gates_no_inverter_refused`;
  `test_gates_genlib_loaded` (fake's `odin3_abc_fake_genlib()` equals Task 4's generic golden).
- [ ] **Step 2:** fails. **Step 3:** implement (the exchange part of the last test lands in Task 11; mark it `TEST_IGNORE` until then and remove the ignore in Task 11).
- [ ] **Step 4:** `ctest --preset debug -R 'test_abc_args|test_abc_pass|test_abc_route'` pass. Lint.
- [ ] **Step 5: Commit** `feat(abc): --gates option and start-up (gatemap spec 3, 4, G-4, G-5)`.

### Task 11: exchange and the cell result

**Files:** Create `src/passes/abc/cells.{c,h}`, `tests/unit/test_abc_gates.c`. Modify
`src/passes/abc/{exchange.c,result.h,result.c}`, `CMakeLists.txt` (add `test_abc_gates` to the
fake-engine `foreach` near line 1387).

**Interfaces:**
- Produces:
  - `odin3_abc_result` gains `odin3_vec gate;` (uint32_t per LUT: index into `run->lib.gates`;
    empty outside `--gates`), initialised and freed with the others.
  - `odin3_status odin3_abc_result_from_cells(const odin3_abc_run *run, const odin3_abc_cells *cells, odin3_arena *arena, odin3_abc_result *out);`
    `odin3_abc_result_from_luts(&cells->net, expect, ...)` with expect from `run->region`
    (`lut_k_max` 0), then LUT k = the k-th MiniLUT object whose slot 0 is not `0x7FFFFFFF` and
    slot 1 not `0x7FFFFFFE`; its name `cells->names[cells->gate[obj]]` is looked up among
    `run->lib.gates` by cell-type name. Failures, each logged `abc: ABC returned a malformed
    network: <why>` with `ODIN3_ERR_ENGINE`: `a gate object without a gate name`; `gate '<g>' is
    not a cell of library '<L>'`; `gate '<g>' has <n> inputs; library cell '<g>' has <m>`;
    `gate '<g>' does not compute library cell '<g>''s function` (first 2^n bits of the truth
    differ from `table`).
  - exchange.c in gates mode: `load_genlib(run->genlib.data)` right after `open` (failure
    reported as `load_genlib`, like `load_aig`); no `&get -m -n`; `get_cells(final_space == GIA ?
    ODIN3_ABC_CELLS_GIA : ODIN3_ABC_CELLS_NTK)`; `from_cells`; no name map, `nomap_reason`
    `--gates`.

- [ ] **Step 1: Write the failing tests** (`test_abc_gates.c`; hand-built `odin3_abc_cells`
  static arrays against a run with generic_gates collected): `test_from_cells_tags_gates`
  (AND2 and INV objects -> `gate` holds their indices, `n_luts` 2);
  `test_from_cells_unknown_gate` (`"NAND3"`); `test_from_cells_wrong_arity` (`"INV"` with two
  fanins); `test_from_cells_wrong_function` (`"AND2"` with truth `0xE`) (Review Focus 5);
  `test_from_cells_untagged_object` (`gate[obj] == -1` on a gate object); and through the pass on
  the fake engine: `test_gates_session_order` (fake command record: `&put`, the script; no
  `&get -m -n`; `odin3_abc_fake_genlib()` non-empty).
- [ ] **Step 2:** fails. **Step 3:** implement. **Step 4:** `ctest -R 'test_abc'` passes. Lint.
- [ ] **Step 5: Commit** `feat(abc): read a gate-mapped result (gatemap spec 4.3)`.

### Task 12: region and splice

**Files:** Modify `src/passes/abc/{region.c,splice.c,splice_a.c,splice_bc.c,run.c}`,
`tests/unit/test_abc_gates.c`.

**Interfaces:**
- region.c `classify_all`: with `run->gates`, a flip-flop of the domain whose INIT parameter
  (index 0) is the integer 1 is not marked REG; it is counted as `ODIN3_ABC_CUT_OTHER` (GM-6).
- run.c `map_region`: no `odin3_abc_result_fold` in gates mode; the INFO line says `-> <n>
  gates` instead of `-> <n> LUTs (at most <w> inputs, depth <d>)`. A result whose `gate.len !=
  n_luts` in gates mode: `abc: internal: the gate result lost its gate tags` (INVALID_ARG).
- splice_a.c: `create_luts` makes, in gates mode, an instance of `lib.gates[gate[k]].type` with
  input j on `in_port[j]` and the object's net on `out_port` (other ports none), prov from
  `obj_prov`. `make_buffer`: gates mode -> `lib.buf` when set, else `$_BUF_` (found as
  `make_const` finds `$_CONST0_`); its prov as today.
- splice.c `sp_canon`: gates mode with `lib.tie<v>` set -> the lowest-ID live net whose single
  driver is a node of that type, else a new instance on a new net with the run's coarse record
  (`odin3_abc_coarse_record`) as prov; without a TIE cell, as today (`$_CONST<v>_`).
- splice_bc.c: the buffer's output port is the output port of its type (first `ODIN3_DIR_OUT`
  port), not the constant `PORT_Y`.

- [ ] **Step 1: Write the failing tests** (fake engine, designs from inline BLIF + `read_techlib`):
  `test_gates_splice_types` (after `abc --gates generic_gates` every soft cell is AND2 or INV;
  no `$lut`; `check` OK); `test_gates_buffer_without_buf_cell` (inline library `INV`, `AND2`
  only; `y = a & b` on two output ports -> one `$_BUF_`) (Review Focus 2);
  `test_gates_const_uses_tie` (`sp_canon` in gates mode with generic -> a TIE1 node, prov != 0);
  `test_gates_init1_flop_is_cut` (`$_DFF_P_` INIT 1 keeps its node ID; the cut line counts it)
  (Review Focus 3); `test_gates_prov_every_gate` (every new library-cell node has a prov record of
  kind DERIVED).
- [ ] **Step 2:** fails. **Step 3:** implement. **Step 4:** `ctest -R 'test_abc'` passes (the LUT
  path's tests unchanged). Lint.
- [ ] **Step 5: Commit** `feat(abc): splice library gates, TIE/BUF or assign (gatemap spec 4.4, G-7)`.

### Task 13: real ABC end to end, PASSES row

**Files:** Create `tests/cli/abc_gates_cli.py`. Modify `CMakeLists.txt` (CTest
`abc_gates_cli` under `ODIN3_WITH_ABC`, env `ODIN3_ABC=$<TARGET_FILE:odin3_abc_exe>`,
`ODIN3_REQUIRE_ABC=1`, labels `tools`, args `$<TARGET_FILE:odin3>`, `tools/equiv-check/equiv-check`,
`${PROJECT_BINARY_DIR}/abc_gates_cli`), `docs/PASSES.md:42`.

Needs PR 4 and PR 5 merged (`git -C $W merge origin/main` first).

**Interfaces:** `write_verilog` needs no change: `write_instance` writes library cells by type
and pin name, `$_CONST0_`/`$_CONST1_` become literals and `$_BUF_` an `assign` (writer.c
BUILTINS). Each case writes its Verilog to the work dir and runs
`odin3 -p "read_techlib <lib>; read_verilog --nolatches t.v; hierarchy -auto; proc; opt; lower;
write_blif --gates-as-names rtl.blif; abc --gates <L>; dfflibmap <L>; write_blif
--gates-as-names gates.blif; write_verilog gates.v; dump_locs --types locs.txt"` and
`equiv-check rtl.blif gates.blif` (exit 0).

- [ ] **Step 1: Write the tests** (unittest; each for `generic_gates` and `cmos6` unless said):
  `test_comb_equivalent` (`y = (a & b) ^ c; z = ~a`); `test_seq_equivalent` (posedge register);
  `test_negedge_cmos6_inverter` (`lower --neg-edge keep`; cmos6; one INV on the clock in locs);
  `test_nf_script` (`--script` file `&st\n&nf\n`) (Review Focus 4);
  `test_const_output_tie_vs_literal` (`y = a & ~a` with `lower` but no `opt`: generic has a
  `node:TIE0`, cmos6 none and `1'b0` in gates.v) (Review Focus 2);
  `test_verilog_only_library_cells` (every instance type in gates.v is a library cell);
  `test_every_gate_has_location` (every `node:<library cell>` line of locs.txt has a non-empty
  SRC); errors: `test_no_techlib_error`, `test_unknown_library_error`,
  `test_lut_script_error` (texts of Tasks 4, 5).
- [ ] **Step 2:** run `ctest --preset debug -R abc_gates_cli`; fix what fails (no test edits to pass).
- [ ] **Step 3:** PASSES abc row: options gain `[--gates LIB]`; notes: "`--gates L`: map to
  library L's gates (gatemap spec): genlib derived from L (sha256 in the run info), linked engine,
  in-memory exchange, `lib/abc/gates.abc` unless `--script`, which must end in `map` or `&nf`;
  library cells, TIE/BUF or `$_CONST*_`/`$_BUF_`; cone provenance; INIT-1 flip-flops are cut
  points." Tests column adds `test_abc_gates`, `test_abc_genlib`, `abc_gates_cli`.
- [ ] **Step 4:** full `ctest --preset debug` green. Lint.
- [ ] **Step 5: Commit** `test(abc): --gates end to end with real ABC`; push; PR 6 (Peter reviews).

---

## PR 7 `feat/gatemap-micros`: micro sweeps and docs

### Task 14: `gatemap-run`, smoke, sweeps, re-map

**Files:** Create `tools/gatemap-check/{gatemap_run.py,gatemap-run}` (sh wrapper as
`tools/lower-check/lower-run`), `tests/tools/test_gatemap_run.py`,
`tests/gatemap/{expect_fail_generic.txt,expect_fail_cmos6.txt}`. Modify
`tools/elab-check/elab_sweep.py` (register the tool), `pyproject.toml` (ruff/mypy paths),
`CMakeLists.txt`.

**Interfaces:**
- `gatemap-run --odin3 O --equiv-check E --lib NAME=FILE [--top T] [--work DIR] [--remap] FILE...`,
  stdlib only, ruff and `mypy --strict` clean. Front: `lower_common.read_front(files, top,
  FILE)` (read_techlib first, `read_verilog --nolatches`, hierarchy), then the Task 13 script.
  Checks in order: odin3 exit and output files; equiv-check exit (0 ok, 1 `differs`, 4
  `unproven`, else `error`); every `node:TYPE` of locs.txt in the library's cells, the dump's
  module names, `$port_in/$port_out/$port_inout`, `$_CONST0_ $_CONST1_ $_CONSTX_ $_CONSTZ_
  $_BUF_` (else `foreign-cell` naming the first); every library-cell node has a non-empty SRC
  (else `noprov`). `--remap`: then `read_techlib FILE; read_verilog -netlist gates.v; hierarchy
  -auto; proc; lower; abc --gates NAME; dfflibmap NAME; write_blif --gates-as-names remap.blif`
  and equiv-check gates.blif remap.blif. Prints `gatemap-run: <outcome> <design>` and
  `gatemap-run: compared <0|1> of 1 designs`; exits 0 ok, 1 differs/foreign-cell/noprov,
  2 usage, 3 odin3 error, 4 unproven.
- elab_sweep.py: `"gatemap-run"` in `OPT_TOOLS`, `LOWER_TOOLS`, `NO_YOSYS_TOOLS`; flag
  `--gatemap-run`; `LOWER_COMPARED` accepts `gatemap-run: compared`. `--opt-arg` carries
  `--lib` and `--equiv-check`.
- CTests: `gatemap_smoke` (CI, `ODIN3_WITH_ABC`, labels `tools`): gatemap-run with `--remap`
  on `micro/bm_DL_logic_w_Dff.v`, `micro/bm_DL_2_1_mux.v`, `micro/bm_DL_4_bit_comparator.v`,
  `micro/bm_DL_4_bit_shift_register.v` for both libraries; `gatemap_micro_generic` and
  `gatemap_micro_cmos6` (`odin3_add_sweep_test`, label `sweep`, the `_lower_micro_expect`
  lists plus the library's list, `--min-compared-total 200` until Task 15 sets the measured floor).

- [ ] **Step 1: Write the failing tests** (`test_gatemap_run.py`): `test_allowed_types_ok`,
  `test_foreign_cell_named` (a dump with `node:$lut`), `test_noprov_named`, `test_outcome_exit_codes`
  (fixture dumps under `tests/tools/data/gatemap/`).
- [ ] **Step 2:** fails. **Step 3:** implement and register.
- [ ] **Step 4:** `ctest --preset debug -R 'python_tools|gatemap_smoke'` pass. Lint.
- [ ] **Step 5:** run the sweeps through heavy-run (Release), smallest first:
  `ODIN3_HEAVY_OK=1 ctest --preset release -R 'gatemap_micro_(generic|cmos6)'`; every failure is
  either fixed (a bug: fresh agent, new commit) or listed in the library's expect-fail file with
  a reason from {`front`, `init-lost`, `no-latch-cell`, `unproven`, `multiclock`} and the
  evidence line. Set each `--min-compared-total` to the measured count minus 5 percent.
- [ ] **Step 6: Commit** `test(gatemap): micro sweeps, smoke and re-map (gatemap spec 9)`.

### Task 15: docs and decisions

**Files:** Modify `docs/user/flow.md` (section 9: a "Gate mapping" subsection with the spec 1
flow and the `dfflibmap` step), `CHANGELOG.md` (`## [Unreleased]` / `### Added`: `abc --gates`,
`dfflibmap`, `write_blif --gates-as-names`, `lib/cmos6.o3lib`, `lib/abc/gates.abc`),
`docs/V1.0.2.md` (create it: title `Odin III v1.0.2: gate mapping`; decisions log from `1.`, one per
GM-1..GM-13 as "(Agent default, gatemap)", plus the sweep counts and floors under "Exit evidence").

- [ ] **Step 1:** write the docs; `ctest --preset debug -R docs` (the Sphinx build, if present) and lint.
- [ ] **Step 2: Commit** `docs: gate mapping (flow, CHANGELOG, V1.0.2 decisions GM-1..13)`; push; PR 7.

## Release v1.0.2 (Peter 2026-10-21: post-v1.0 core features are patch releases)

Gate mapping is Odin III **v1.0.2**. Its log is `docs/V1.0.2.md` (create it: title `Odin III v1.0.2: gate mapping`,
a checklist, and a decisions log numbered from 1 holding this document's decisions and agent defaults).
The release bumps `project(odin3 VERSION 1.0.2)`, adds `docs/RELEASE-NOTES-1.0.2.md` and a CHANGELOG section,
and is tagged only on Peter's explicit go.
