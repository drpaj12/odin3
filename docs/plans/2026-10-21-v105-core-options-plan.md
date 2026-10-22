# v1.0.5 core options (`-const` for `cone_check`/`raise`, `submod -bus`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `cone_check` and `raise` accept `-const PORT=V` (a template input port tied to a
constant), and `submod` accepts `-bus BUSKEY` (boundary nets tagged `NAME[BIT]` become one vector
port), so Loki III L1 can raise `A == 13` and `Q + 1` cones to word cells and commit buses as
vector ports, through the unchanged Python binding.

**Architecture:** `-const` is one more option in the shared parser `cc_parse_args`; the template
(`cc_tmpl`) records a constant per flat bit; the binding check skips those bits; the proof's
temporary design drives them from `$_CONST0_`/`$_CONST1_` nodes; `raise` connects them to the
module's constant nets, created only in its run phase. `-bus` adds a per-group bus table, built in
a new file `src/passes/submod/busattr.c` during `analyze`, which `add_boundary_port` consults
before the wire-bus rule.

**Tech Stack:** C17 core (`src/passes/cone_check`, `src/passes/raise`, `src/passes/submod`), Unity
unit tests, the stdlib Python golden scripts in `tests/cli/`, the cffi binding
`plugins/python/odin3.py` (unchanged), CTest.

**Spec:** `docs/specs/2026-10-21-v105-core-options-design.md` (sections cited as spec 3.2 K-1 ...,
4.2 B-1 ...; agent defaults V105-1 to V105-10). Executors read both.

## Global Constraints

- Code standard of `CLAUDE.md`: C17 only; `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wstrict-prototypes -Wmissing-prototypes -Wvla -Werror`; functions CCN <= 15, <= 60 lines,
  <= 5 parameters; no VLAs; no `goto` except one `cleanup:`; no recursion in IR traversals;
  errors as `odin3_status`; IR objects only through the module API; named constants outside
  `tests/`.
- Never add `NOLINT`, never weaken `.clang-tidy` or a gate; `tools/lint.sh` passes before every
  commit (`/lint`).
- No C ABI change: `include/odin3/odin3.h` and `ODIN3_ABI_VERSION` are not touched; no change to
  `plugins/python/odin3.py` (spec 2 fact 9).
- `odin3_submod_extract` and `odin3_submod_extract_ctx` keep their signatures and behaviour
  (spec B-7).
- Without `-const`, `odin3:cone_cell` text is byte-identical to today (spec K-5); without `-bus`,
  `submod` output is identical to today.
- `-const` values: decimal (<= 2^64 - 1) or `0b` binary, zero-extended, never truncated, no
  `x`/`z` (spec 3.1).
- Decisions go into `docs/V1.0.5.md` (created by Task 4, rows V105-1 to V105-10 from spec 7);
  `CHANGELOG.md` entries under `## [Unreleased]`; no version bump (V105-10).
- Branches from `origin/main`; update with `git merge origin/main` (never rebase, never force
  push); `git -C <worktree>` and absolute paths. PR 1 is reviewed by Peter (spec 9 question 1).
- Golden micro runs are small (seconds); no heavy-run needed. Smallest micro first in each list.

## Review Focus

1. A binary `-const` longer than the port but with leading zeros (`-const B=0b00001101` on a
   4-bit `B`) is accepted; `0b10000` on a 4-bit port is refused (Task 1
   `test_cone_check_const_value_widths`).
2. `raise` across two modules where the second fails the check phase: the first module gets no
   new `$_CONST*_` node (constants are made only in the run phase) (Task 3
   `test_raise_const_all_or_nothing`).
3. A group that itself contains a constant driver (a zero-input `.names`, a `$sop`): `raise` must
   not tie the new cell to it, since the group's nodes are deleted (Task 3
   `test_raise_const_ignores_group_constant`).
4. A bus name equal to an internal net's name, or to another port's name: the port gets a unique
   name and the internal net is renamed `$submod$<ID>` as today (Task 6
   `test_submod_busattr_name_unique`).
5. Nested commits: a second `submod -bus` whose group holds the instance made by the first, its
   boundary nets tagged again: vector ports at both levels, `flatten` equivalent (Task 7
   `bus_nested` micro).

## PRs, order and parallelism

| PR | Branch | Tasks | Depends on | Parallel with |
|---|---|---|---|---|
| 1 | `feat/cone-const` | 1, 2, 3, 4 | main | 2 |
| 2 | `feat/submod-bus` | 5, 6, 7 | main | 1 |

Files are disjoint except `docs/PASSES.md` (different rows), `CHANGELOG.md`, `docs/V1.0.5.md`
(PR 2 appends; whichever merges second runs `git merge origin/main`) and `CMakeLists.txt` (PR 1
adds the `python_core_options` test, PR 2 extends its module list only if it adds a second test
class, which it does not: it adds test methods to the same file).

Every task ends with `cmake --preset debug && cmake --build --preset debug`, the task's
`ctest --preset debug -R '<names>'`, `tools/lint.sh`, a commit and
`git -C $W push -u origin <branch>` ($W = the lane's worktree). Each PR ends with the full
`ctest --preset debug` green, a read-only critique review (CLAUDE.md rule 12), and a PR whose
head SHA equals the reviewed SHA.

---

## PR 1 `feat/cone-const`

### Task 1: `-const` option, template constants, binding and cell text

**Files:**
- Modify: `src/passes/cone_check/cone_check_int.h` (`cc_opts` at line 67, `cc_tmpl` at line 80,
  declarations near lines 121-171), `src/passes/cone_check/register.c` (`k_usage` line 21,
  `take_option` line 83), `src/passes/cone_check/tmpl.c` (`cc_tmpl_build` line 179,
  `cc_tmpl_bind` line 290, `cc_cell_text` line 331), `src/passes/raise/register.c` (`k_usage`
  line 18)
- Test: `tests/unit/test_cone_check.c` (now also includes `passes/cone_check/cone_check_int.h`
  for the direct `cc_const_bits` and `cc_cell_text` cases)

**Interfaces:**
- Produces in `cone_check_int.h`:
  - `typedef struct cc_const { const char *port; const char *value; } cc_const;` (texts in
    `opts->arena`) and a field `odin3_vec consts; /* cc_const, in option order */` in `cc_opts`
    (callers `odin3_vec_init` it beside `params`: both pass-run functions).
  - `enum { CC_BIT_NET = 0, CC_BIT_CONST0 = 1, CC_BIT_CONST1 = 2 };` and a field
    `odin3_vec cbit; /* uint8_t per flat bit: CC_BIT_* */` in `cc_tmpl` (init/free in
    `cc_tmpl_init`/`cc_tmpl_free`).
  - `odin3_status cc_const_bits(const char *text, uint32_t width, uint8_t *bits);` parses a
    `-const` value into `width` bits, LSB first (0 or 1 each); logged `INVALID_ARG` on a parse or
    width error (spec K-2). `cc_tmpl_build` calls it per `-const` after `layout_port`.
- Consumed by Task 2 (`tmpl->cbit`), Task 3 (`tmpl->cbit`, `opts->consts`).

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_cone_check.c` (reuse `read_blif`,
  `mark_all`, `bind`, `run`, `FAILS_WITH`, `text_attr`, `expect_same`). Add a BLIF macro `EQ13`:
  `.model m`, inputs `a0..a3`, output `y`, one `.names a0 a1 a2 a3 y` with row `1011 1`
  (a0=1, a1=0, a2=1, a3=1, i.e. A == 13). A helper `load_eq13()` reads it, marks all, binds
  `a<i>=A[<i>]`, `y=Y[0]`.
  - `test_cone_check_const_errors`: `FAILS_WITH(EQ4 " -const C=1", "has no port 'C'")`,
    `FAILS_WITH(EQ4 " -const Y=1", "is not an input")`, `FAILS_WITH(EQ4 " -const B=1 -const B=2",
    "twice")`, `FAILS_WITH(EQ4 " -const B=16", "does not fit port 'B' (4 bits)")`,
    `FAILS_WITH(EQ4 " -const B=1x", "not a constant")`, `FAILS_WITH(EQ4 " -const B=", "needs
    NAME=VALUE")`, where `EQ4` is `"-attr grp=add4 -cell $eq -param A_WIDTH=4 -param B_WIDTH=4
    -param Y_WIDTH=1"`; each leaves the module unchanged (`expect_same`) and sets no
    `odin3:cone_check`.
  - `test_cone_check_const_net_bound_refused`: `load_eq13()`, then rebind net `a3` to `B[3]`
    (`bind("a3=B[3]")` overwrites the text); `FAILS_WITH(EQ4 " -const B=13", "port 'B' is
    constant")` (the `A[3] is not bound` error is logged too; the test checks only the constant one).
  - `test_cone_check_const_cell_text`: after `load_eq13()`, `run(EQ4 " -const B=13 -timeout 0")`
    is `ODIN3_OK` (unproven, no ABC needed) and `text_attr("odin3:cone_cell")` equals
    `"$eq A_WIDTH=4 B_WIDTH=4 Y_WIDTH=1 -const B=13"`; a second run with `-const B=0b1101` gives
    `"$eq A_WIDTH=4 B_WIDTH=4 Y_WIDTH=1 -const B=0b1101"`. `cc_cell_text` on a hand-filled
    `cc_opts` with constants `B=1` then `A=2` gives `"$add -const A=2 -const B=1"` (sorted by
    port).
  - `test_cone_check_const_value_widths`: unit-test `cc_const_bits` directly: `"13"`, width 4
    gives bits `{1,0,1,1}`; `"0b00001101"`, width 4, gives the same; `"0b10000"`, width 4, is
    `INVALID_ARG`; `"18446744073709551615"`, width 64, is OK; `"18446744073709551616"` is
    `INVALID_ARG`; `"0b"` and `"-1"` are `INVALID_ARG`.
  - `test_cone_check_no_const_text_unchanged`: the existing `ADD4` run's `odin3:cone_cell` is
    `"$add A_WIDTH=4 B_WIDTH=4 Y_WIDTH=4"` (pins V105 Global Constraint).
- [ ] **Step 2: Run** `ctest --preset debug -R test_cone_check --output-on-failure`. Expected:
  FAIL (`unknown option '-const'`, `cc_const_bits` undefined).
- [ ] **Step 3: Implement.** `take_option`: `-const` takes a pair through `take_pair` and pushes
  a `cc_const`; usage strings of both passes gain `[-const PORT=V ...]`. `cc_tmpl_build`: after
  the layout loop, fill `cbit` with `CC_BIT_NET` for every flat bit, then for each `cc_const`
  find the port (K-2 errors: unknown, not `ODIN3_DIR_IN`, duplicate), call `cc_const_bits` and
  set the port's bits. `cc_tmpl_bind`: a bit with `cbit != CC_BIT_NET` counts as bound; a net
  whose binding lands on such a bit is the K-1 error. `cc_cell_text`: after the params, append
  ` -const PORT=V` per constant, sorted by port name with the same `sort_in` insertion.
- [ ] **Step 4: Run** the test again. Expected: PASS (the proof cases of Task 2 are not written
  yet). Also `ctest --preset debug -R 'test_cone_check|test_raise'`: the old cases still pass.
- [ ] **Step 5: Commit** `cone_check: -const PORT=V option, template constants, cell text` and push.

### Task 2: Constants in the proof (miter and counterexample)

**Files:**
- Modify: `src/passes/cone_check/miter.c` (`make_ports` line 61), `src/passes/cone_check/prove.c`
  (`cex_text` line 313)
- Test: `tests/unit/test_cone_check.c`, `tests/cli/cone_check_micros.py`

**Interfaces:**
- Consumes: `cc_tmpl.cbit` (Task 1).
- Produces: in the temporary module, `job->in_ids[k]` for a constant input bit `k` is a
  `$_CONST0_`/`$_CONST1_` node whose one pin carries a fresh net, so `cc_port_net` and
  `template_port` stay unchanged (spec K-3). `tests/cli/cone_check_micros.py` gains
  `incrementer(width: int) -> tuple[str, dict[str, str]]` and
  `eq_const(width: int, value: int) -> tuple[str, dict[str, str]]` (BLIF text and binding), used
  by Task 4's raise micros.

- [ ] **Step 1: Write the failing tests.** In `test_cone_check.c` (each calls `use_real_abc()`):
  - `test_cone_check_const_increment_equivalent`: BLIF `INC4` (inputs `a0..a3`, outputs
    `s0..s3`, `s = a + 1 mod 16` as half-adder `.names` with an internal carry chain: `s0 = !a0`,
    `s1 = a1 ^ a0`, `c2 = a1 & a0`, `s2 = a2 ^ c2`, `c3 = a2 & c2`, `s3 = a3 ^ c3`), all marked,
    `a<i>=A[<i>]`, `s<i>=Y[<i>]`; `run("-attr grp=add4 -cell $add -param A_WIDTH=4 -param
    B_WIDTH=1 -param Y_WIDTH=4 -const B=1")` gives `equivalent`, no `odin3:cone_cex`,
    `expect_same`.
  - `test_cone_check_const_eq13_equivalent`: `load_eq13()`, `run(EQ4 " -const B=13")` gives
    `equivalent`.
  - `test_cone_check_const_eq13_differs`: `run(EQ4 " -const B=12")` gives `differs`;
    `odin3:cone_cex` contains `A[0]=` and does not contain `B[` (spec K-4).
  In `tests/cli/cone_check_micros.py` `micros()`, after the existing entries: `inc4_vs_add_const`
  (`$add`, params `A_WIDTH=4 B_WIDTH=1 Y_WIDTH=4` plus `-const B=1` in `params`, expect
  `equivalent`), `eq13_vs_eq_const` (`$eq`, expect `equivalent`), `eq13_wrong_const` (`-const
  B=12`, expect `differs`), `const_output_refused` (`-const Y=1`, expect `refused`, refusal
  `is not an input`).
- [ ] **Step 2: Run** `ctest --preset debug -R 'test_cone_check|cone_check_micros'
  --output-on-failure`. Expected: FAIL (the template's constant bits read an unconnected input).
- [ ] **Step 3: Implement.** `make_ports`: walk the flat template bits in order; for each input
  bit `b` (`cc_tmpl_dir == ODIN3_DIR_IN`) push either an input port `i<ord[b]>` (as today) or, when
  `cbit[b] != CC_BIT_NET`, a `$_CONST0_`/`$_CONST1_` node made with `odin3_net_create` +
  `odin3_node_create_connected` in `job->tmp` (type found by `odin3_celltype_find` on the
  interned name). Output ports as today. `cex_text`: skip bits with `cbit != CC_BIT_NET`.
- [ ] **Step 4: Run** the same `ctest`. Expected: PASS; `cone_check_micros` prints
  `cone_check_micros: 10 micros, 0 failed`.
- [ ] **Step 5: Commit** `cone_check: constant template inputs in the proof` and push.

### Task 3: `raise` ties constant ports to the module's constant nets

**Files:**
- Modify: `src/passes/raise/raise.c` (`rs_job`, `make_ports` line 157, `create_cell` line 214,
  `replace_group` line 291)
- Test: `tests/unit/test_raise.c`

**Interfaces:**
- Consumes: `cc_tmpl.cbit`, `cc_opts.consts` (Task 1).
- Produces (static in `raise.c`): `static odin3_status tie_constants(rs_job *job,
  odin3_node_id made[2]);` fills `bit_net` for every constant bit with the net of a live
  `$_CONST0_`/`$_CONST1_` node not in the group (lowest node ID), else of a node it creates
  (recorded in `made[0]`/`made[1]`, 0 when none); called from `replace_group` before
  `create_cell`, never from `prepare` (spec K-6). New nodes get the raised cell's provenance:
  `make_prov` runs once and its result is passed to both.

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_raise.c` (helpers `read_blif`,
  `mark_all`, `bind`, `REFUSED`, `check_clean`, `use_real_abc`, `design_reset`):
  - `test_raise_const_increment`: `INC4` (same text as Task 2), `cone_check` then `raise` with
    `INC_ARGS` (`-attr grp=add4 -cell $add -param A_WIDTH=4 -param B_WIDTH=1 -param Y_WIDTH=4
    -const B=1`): `ODIN3_OK`; the module has exactly one live non-port node of type `$add` plus
    one `$_CONST1_`; the `$add`'s `B[0]` pin's net has `odin3_net_const_value == ODIN3_CONST_1`;
    `check_clean()`.
  - `test_raise_const_reuses_shared_constant`: `INC4`, then the test creates one `$_CONST1_`
    node on a fresh net with `odin3_net_create` and `odin3_node_create_connected` (a BLIF
    zero-input `.names` would be a `$sop`, not a `$_CONST1_`); `cone_check` and `raise`: the
    count of `$_CONST1_` nodes is still 1 and the `$add`'s `B[0]` is on that node's net.
  - `test_raise_const_refuses_other_constant`: prove with `INC_ARGS`, then `raise` with the same
    text but `-const B=0b1`: `REFUSED(..., "cone_check proved group")` (V105-4: the spelling is
    part of the record).
  - `test_raise_const_all_or_nothing`: two modules each holding an `INC4` group (reuse the
    second-module helper of `test_raise_all_or_nothing_across_modules`), only the first proved:
    `raise` fails and neither module gained a `$_CONST*_` node.
  - `test_raise_const_ignores_group_constant`: an `EQ13` variant whose group contains a
    zero-input `.names one\n1\n` read by the cone (`y = a == 13` written as `.names a0 a1 a2 a3
    one y` with row `10111 1`), bound and proved with `-const B=13`; after `raise` the `$eq`'s
    `B` bits sit on `$_CONST0_`/`$_CONST1_` nets, and the `$sop` constant was deleted with the
    group.
- [ ] **Step 2: Run** `ctest --preset debug -R test_raise --output-on-failure`. Expected: FAIL
  (constant bits left on net 0).
- [ ] **Step 3: Implement** `tie_constants` and call it from `replace_group`; on the rollback path
  (`deleted == 0`) delete `made[]` nodes and their nets after the cell (`odin3_node_delete`,
  `odin3_net_delete`).
- [ ] **Step 4: Run** `ctest --preset debug -R 'test_raise|test_cone_check'`. Expected: PASS.
- [ ] **Step 5: Commit** `raise: tie -const ports to the module's constant nets` and push.

### Task 4: Golden raise micros, Python test, docs for `-const`

**Files:**
- Modify: `tests/cli/raise_micros.py` (`micros()`, a new `const_mismatch()` beside
  `never_proved()`), `CMakeLists.txt` (after the `python_re_round` test, around line 3016),
  `docs/PASSES.md` (rows `cone_check` line 49, `raise` line 50), `CHANGELOG.md`
  (`## [Unreleased]`), `docs/user/re-workflow.md` ("Notes for a tool built on this"),
  `docs/RE-ARCHITECTURE.md` (section 5 gap 7: add "Partly closed in v1.0.5: `-const`")
- Create: `tests/tools/test_core_options.py`, `docs/V1.0.5.md`

**Interfaces:**
- Consumes: `cc.incrementer`, `cc.eq_const` (Task 2).
- Produces: `tests/tools/test_core_options.py` class `ConstOptionTest(unittest.TestCase)`;
  Task 7 adds `BusOptionTest` to the same file. CTest name `python_core_options`, registered
  inside `if(ODIN3_WITH_ABC)` with the same environment as `python_re_round`.

- [ ] **Step 1: Write the failing tests.** `raise_micros.py`: `inc4_raised` and `eq13_raised`
  (expect `equivalent`: raised, lowered, `abc --lut 4`, equiv-check against the reference);
  `const_mismatch()` proves `eq13` with `-const B=13` and raises with `-const B=12`; expects a
  non-zero exit and `cone_check proved group` in the output, called from `main` like
  `never_proved`. `test_core_options.py::ConstOptionTest.test_eq_const_raise_from_python`
  (skips without cffi/`ODIN3_LIB` unless `ODIN3_REQUIRE_CFFI=1`, as `test_re_round.py`): write the
  EQ13 BLIF to a temp dir, `Design.read_blif(path)`, `odin3.default().load_abc()`, set `grp=1` on
  every non-port node (`Node.set_attr`), `odin3:bind` on the nets (`Module.net(name).set_attr`),
  `run_pass("cone_check", ARGS + " -timeout 60")`, assert `top().attr("odin3:cone_check") ==
  "equivalent"` and `top().attr("odin3:cone_cell")` ends with `-const B=13`;
  `run_pass("raise", ARGS)`; assert exactly one node with `type_name == "$eq"` and that
  `odin3.Odin3Error` is raised by a second `raise` (group gone).
- [ ] **Step 2: Run** `ctest --preset debug -R 'raise_micros|python_core_options'
  --output-on-failure`. Expected: `raise_micros` passes its new micros (Tasks 1-3 are in);
  `python_core_options` is not found (not registered yet).
- [ ] **Step 3: Implement** the CMake entry and the docs: PASSES.md rows get the `-const` syntax
  and spec K-1 to K-6 in one or two sentences each; CHANGELOG `### Added` line "`cone_check` and
  `raise` take `-const PORT=V` (a template input port tied to a constant; v1.0.5)";
  `docs/V1.0.5.md` follows `docs/PHASE7.md`'s header form: title `# Odin III: v1.0.5 core options
  for Loki III L1`, spec and plan paths, a checklist (`-const` PR, `-bus` PR) and a
  `## Decisions log` with rows V105-1 to V105-10 copied from spec 7, each `(Agent default)`.
- [ ] **Step 4: Run** `ctest --preset debug -R 'raise_micros|python_core_options|cone_check'`
  and `tools/lint.sh`. Expected: PASS.
- [ ] **Step 5: Commit** `v1.0.5: -const golden micros, Python test, docs` and push. Full
  `ctest --preset debug`, critique review, open PR 1 (Peter reviews).

---

## PR 2 `feat/submod-bus`

### Task 5: The `NAME[BIT]` parser and the per-group bus table

**Files:**
- Create: `src/passes/submod/busattr.c`
- Modify: `src/passes/submod/submod_int.h` (move `enum { SM_NONE, SM_INTERNAL, SM_IN, SM_OUT }`
  from `submod.c:26`; add the declarations below), `src/passes/submod/submod.c:26` (keep only
  `SM_INT_TEXT`), `CMakeLists.txt:111` (add `src/passes/submod/busattr.c` beside `submod.c`)
- Test: `tests/unit/test_submod.c`

**Interfaces:**
- Produces in `submod_int.h`:
  - `bool odin3_submod_bus_parse(const char *text, size_t *name_len, uint32_t *bit);` true for
    `NAME[BIT]` (spec 4.1).
  - `typedef struct odin3_submod_bus { uint32_t name; uint8_t dir; uint32_t first; uint32_t
    width; bool ok; } odin3_submod_bus;` (`name` a strtab ID, `dir` `SM_IN`/`SM_OUT`, `first`
    index into `nets`, `ok` false for a B-3 fallback).
  - `typedef struct odin3_submod_busset { odin3_vec buses; /* odin3_submod_bus */ odin3_vec nets;
    /* odin3_net_id, each bus's bits LSB first */ odin3_vec of_net; /* uint32_t per net ID: bus
    index + 1, 0 none */ } odin3_submod_busset;` with `void odin3_submod_busset_init(...)` and
    `void odin3_submod_busset_free(...)`.
  - `odin3_status odin3_submod_busset_build(const odin3_module *module, const odin3_vec *state,
    const char *key, const char *group, odin3_submod_busset *out);` reads `key` on every net whose
    `state` entry is `SM_IN` or `SM_OUT`; B-4 errors are located (`INVALID_ARG`); B-3 sets get
    `ok = false` and one WARN each naming `group` and the bus; `of_net` is set only for nets of
    `ok` buses.
- Consumed by Task 6.

- [ ] **Step 1: Write the failing tests** in `test_submod.c`:
  `test_submod_busattr_parse`: `"h12[3]"` gives true, `name_len == 3`, `bit == 3`; `"a[0]"` true;
  false for `"h12"`, `"h12[]"`, `"[3]"`, `"$x[1]"`, `"h[3]x"`, `"h[-1]"`, `"h[2147483648]"`,
  `"a[b][1]"`. `test_submod_busset_groups`: a BLIF with four AND gates `y<i> = a<i> & b<i>`, all
  four in group `sub`, nets `y0..y3` tagged `loki_bus = "y4[<i>]"`; call `analyze`-equivalent
  setup through the pass is Task 6, so here build `state` by hand (`SM_OUT` for `y<i>`, `SM_IN`
  for `a<i>`, `b<i>`) and call `odin3_submod_busset_build`: one bus, `ok`, `width == 4`, nets in
  bit order; with `y2` untagged: `ok == false` and the log has `has no bit 2`; with `y1` and `y2`
  both `y4[1]`: `ok == false`, log `bit 1 twice`; with `y0` tagged `"y4"`: `INVALID_ARG`, log
  `is not NAME[BIT]`.
- [ ] **Step 2: Run** `ctest --preset debug -R test_submod --output-on-failure`. Expected: FAIL
  (undefined functions).
- [ ] **Step 3: Implement** `busattr.c`: one pass over nets in ID order collecting `(name, dir,
  bit, net)` entries, a stable insertion sort by `(name, dir, bit)` (no recursion), then one pass
  that cuts the sorted run per `(name, dir)` and decides `ok` (bits exactly `0..W-1`) and the
  mixed-direction case (same `name`, both directions: both sets `ok = false`).
- [ ] **Step 4: Run** the test. Expected: PASS.
- [ ] **Step 5: Commit** `submod: NAME[BIT] bus attribute table` and push.

### Task 6: `submod -bus BUSKEY`

**Files:**
- Modify: `src/passes/submod/submod_int.h` (`odin3_submod_group` line 10: add
  `const char *bus_key; /* NULL: no -bus */`), `src/passes/submod/submod.c` (`sm_run`, `analyze`,
  `full_bus` line 432, `add_boundary_port` line 492, `run_free`), `src/passes/submod/register.c`
  (`sm_opts`, `take_value` usage text line 39, `parse_args` line 46 and its unknown-option text,
  `extract_groups` line 164, the pass usage line 254)
- Test: `tests/unit/test_submod.c`

**Interfaces:**
- Consumes: Task 5's functions.
- Produces: `sm_run` gains `odin3_submod_busset buses;` built by `analyze` when
  `group->bus_key != NULL`; a static `add_attr_bus_port(sm_run *run, uint32_t bus)` that adds the
  port per B-1/B-2 and maps every bit (`map_port_bit`, `record_port`); `add_boundary_port` checks
  `of_net` first; `full_bus` returns false when any bit's net has `of_net != 0` (B-5).
  Pass usage: `submod [-attr KEY] [-name N] [-bus BUSKEY]`.

- [ ] **Step 1: Write the failing tests** in `test_submod.c` (helpers `read_blif`, `mark_driver`,
  `run`, `module_named`, `port_count`, `port_width`, `port_dir`, `check_clean`; a new
  `tag_net(name, key, text)` using `odin3_attr_set` on `ODIN3_OBJ_NET`):
  - `test_submod_busattr_vector_in_and_out`: four ANDs in `sub`, `a<i>` tagged `a4[<i>]`, `y<i>`
    tagged `y4[<i>]`: `run("-bus loki_bus")` OK; `sub` has 6 ports (`a4` in, width 4; four
    scalar `b<i>`; `y4` out, width 4); `check_clean()`; the instance's pins of port `y4` are on
    the old `y0..y3` nets in order.
  - `test_submod_busattr_off_by_default`: same tags, `run("")`: 12 scalar ports.
  - `test_submod_busattr_gap_stays_scalar`: `y2` untagged: `y0`, `y1`, `y3` are scalar ports,
    log contains `bus 'y4' has no bit 2`.
  - `test_submod_busattr_mixed_dir_stays_scalar`: `a0` tagged `y4[4]` (an input with the output
    bus's name): every `y4` net and `a0` are scalar; WARN names `y4`.
  - `test_submod_busattr_malformed_refused`: `y0` tagged `"y4"`: `run("-bus loki_bus")` is
    `INVALID_ARG`, the design is unchanged (same module count, no `sub`), log `is not NAME[BIT]`.
  - `test_submod_busattr_beats_wire_bus`: reuse `BUS_JSON` (a 4-bit wire `a` of boundary nets)
    with its nets tagged `q[<3-i>]` (reversed): the port is `q`, not `a`, and bit 0 of `q` is the
    net of `a[3]`.
  - `test_submod_busattr_name_unique`: a scalar boundary net named `y4` and the bus `y4`: ports
    `y4` and `y4_1` (whichever comes first by net ID keeps `y4`); an internal net named like the
    bus port is renamed `$submod$<ID>`.
  - `test_submod_options` gains `run("-bus")` is a usage error (`-bus needs a value`).
- [ ] **Step 2: Run** `ctest --preset debug -R test_submod --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement** the option (`take_value` into a new `odin3_strbuf bus`,
  `has_bus`), pass `bus_key` through `odin3_submod_group` in `extract_groups`, build the table in
  `analyze` after `classify_nets` (so the check phase refuses B-4 before any move), free it in
  `run_free`, and the port logic above.
- [ ] **Step 4: Run** `ctest --preset debug -R 'test_submod|submod_equiv_micros'`. Expected: PASS.
- [ ] **Step 5: Commit** `submod: -bus BUSKEY makes vector ports from NAME[BIT] attributes` and
  push.

### Task 7: Golden bus micros, Python test, docs for `-bus`

**Files:**
- Modify: `tests/cli/submod_equiv.py` (a `bus` mode and `bus_nested` check, called from `main`),
  `tests/tools/test_core_options.py` (add `BusOptionTest`), `docs/PASSES.md` (row `submod` line
  48: also correct "one scalar port each" to name the wire-bus rule that exists), `CHANGELOG.md`,
  `docs/V1.0.5.md` (tick the `-bus` item), `docs/user/re-workflow.md`, `docs/RE-ARCHITECTURE.md`
  (the "new" gap: scalar ports, closed by `-bus`)

**Interfaces:**
- Consumes: the `-bus` option (Task 6).

- [ ] **Step 1: Write the failing tests.** `submod_equiv.py`: `bus_micro(binary, equiv, work)`
  generates a BLIF with eight 2-input ANDs `y<i> = a<i> & b<i>` (i < 8) feeding a 3-input OR per
  output `z<j>`; reads it, writes the reference BLIF and `--odin3` JSON; in the JSON puts the
  ANDs in group `g_and` (`submod` attribute) and tags `y<i>` with `sbus = "y8[<i>]"`, `a<i>` with
  `"a8[<i>]"`; runs `read_json; submod -bus sbus; write_verilog v.v; flatten; write_blif out.blif`;
  requires `equiv-check ref out` exit 0 and `v.v` to match the regexes `input\s*\[7:0\]\s*a8` and
  `output\s*\[7:0\]\s*y8`. `bus_nested`: the same, then a second JSON round with the OR cells
  and the `g_and` instance in group `g_top`, `y8` nets untouched and `z<j>` tagged
  `"z[<j>]"`, a second `submod -bus sbus`, `flatten`, equivalent. `BusOptionTest.
  test_vector_ports_from_python`: four ANDs read with `Design.read_blif`, `loki_sm = "sub"` on the
  AND nodes, `loki_bus` on `y<i>` via `Module.net(...).set_attr`, `run_pass("submod", "-attr
  loki_sm -bus loki_bus")`; `design.module("sub").ports()` has one port whose
  `node.ports()[0].width == 4`.
- [ ] **Step 2: Run** `ctest --preset debug -R 'submod_equiv_micros|python_core_options'
  --output-on-failure`. Expected: FAIL until the micros and the test class exist; then PASS.
- [ ] **Step 3: Implement** the docs: PASSES.md `submod` row gets `-bus BUSKEY` and B-1 to B-6 in
  short form; CHANGELOG `### Added` "`submod -bus BUSKEY`: vector ports from `NAME[BIT]` net
  attributes (v1.0.5)".
- [ ] **Step 4: Run** the full `ctest --preset debug` and `tools/lint.sh`. Expected: PASS.
- [ ] **Step 5: Commit** `v1.0.5: -bus golden micros, Python test, docs` and push. Critique
  review, open PR 2.

---

## Self-review

- Spec coverage: 3.1 syntax (T1), K-1/K-2/K-5 (T1), K-3/K-4 (T2), K-6 (T3), K-7 (no change),
  3.3 uses (T2, T4 micros), 4.1/B-4 (T5), B-1/B-2/B-3 (T5, T6), B-5/B-6 (T6), B-7 (Global
  Constraints, untouched functions), section 5 calls (T4, T7 Python tests), section 6 tests
  (T1-T7), section 7 defaults logged (T4), Review Focus 1-5 pinned in T1, T3, T3, T6, T7.
- Types: `cc_const`, `cc_tmpl.cbit`, `CC_BIT_*`, `cc_const_bits` (T1) used in T2/T3;
  `odin3_submod_busset*`, `odin3_submod_bus_parse` (T5) used in T6; `odin3_submod_group.bus_key`
  (T6) set only in `register.c`.
- Proportion: signatures, test names and assertions; bodies only for the busset algorithm shape.
