# 1G Tech library (and the tombstone follow-up) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the `.o3lib` tech-library format (reader, expression language, parametric widths), ship `generic_gates.o3lib` and `vtr.o3lib`, make the BLIF reader type the goldens' `adder`/`multiply`/RAM cells from `vtr.o3lib` with inferred parameters while the round trip stays identical; and first, the IR-6 tombstone follow-up (PHASE1 #14).

**Architecture:** `src/techlib/` holds an expression lexer/parser (Verilog-2005 operator set and precedence) producing a small expression AST in the design arena, an integer evaluator for width expressions, and a `.o3lib` reader that registers each cell as a design-local cell type. The cell-type registry gains a fourth width rule (a compiled width expression). Black-box declarations matching a parametric registered type infer its parameters; the declared-model list records them so the BLIF writer reproduces the exact formals.

**Tech Stack:** C17, `src/util`, `src/ir`, `src/frontends/blif`, `src/backends/blif`, Unity.

**Spec:** `docs/specs/2026-10-08-1G-techlib-design.md` (approved, PHASE1 #7/#8); IR semantics `docs/IR.md` (IR-6, IR-7b, IR-8..IR-11); PHASE1 decisions #10, #14, #16.

## Global Constraints

- All 1A/1B/1C constraints apply (C17 `-Werror`, function limits, no adjacent convertible params, no abort/exit/goto/VLA/recursion, no NOLINT, `odin3_` snake_case, util-only containers, IR changes only through `src/ir`, reserve-before-mutate, deterministic, located `file:line` errors as `ODIN3_ERR_PARSE`).
- Expression evaluation and parsing are iterative (explicit stacks), never recursive (spec §15.1).
- The 1C round trip must stay identical on all goldens when `vtr.o3lib` is loaded.

## Review Focus

1. A golden declaring `.model multiply` with 36-bit `a`/`b` and 72-bit `out` resolves to the library `multiply` with `A_WIDTH=36, B_WIDTH=36`; its instances get those parameters; the writer prints the same formals — test in Task 4.
2. A declared black box whose widths contradict the library cell's expressions (e.g. `out` 70 bits for 36×36) → located parse error — test in Task 4.
3. An expression with operator precedence traps (`a + b << 1`, `a == b & c`, `~a[2]`, `{2{a}}`) parses with Verilog precedence — test in Task 2.
4. A `.o3lib` cell with an `out` port driven by no `fn`/`seq`/`memory` and kind not `blackbox` → located error — test in Task 3.
5. Implicit black boxes (`$pow`) keep working when a library does not define them, and a parametric registered type used via `.subckt` gets parameters inferred from its formals — test in Task 4.

---

### Task 1: Tombstones keep parameters and pin net names (PHASE1 #14)
**Files:** Modify `src/ir/prov.h`, `src/ir/prov.c`, `src/ir/compact.c`, `tests/unit/test_ir_compact.c`, `tests/unit/test_ir_prov.c`, `docs/IR.md` (IR-6 already amended — verify wording).
**Produces:** `odin3_tombstone` gains `const odin3_value *params; uint32_t n_params;` (copied into a design-owned arena) and `const uint32_t *pin_nets; uint32_t n_pins;` (strtab IDs of the nets each pin was last connected to, 0 = unconnected; for a node only; net/wire tombstones leave both empty). `odin3_tombstone_add` deep-copies them. `compact` fills them for dead nodes from the dead node's parameter values and its pins' nets' names (a dead node's pins are disconnected at delete time — so `odin3_node_delete` must record the names it disconnected; store them per dead node in the module until compact, cheaply: a side table keyed by node ID holding the strtab IDs). Tests: delete a node with params and 3 connected pins, compact, the tombstone has the params (equal values) and the three net names in pin order; net/wire tombstones unchanged; OOM sweep; memory stays linear.

### Task 2: Expression language
**Files:** Create `src/techlib/expr.h`, `src/techlib/expr.c`, `tests/unit/test_techlib_expr.c`.
**Produces:** `odin3_expr` AST (node kinds: ident, int literal, sized literal with 4-state bits, unary `~ ! -`, binary `& | ^ + - * << >> == != < <= > >= && ||`, ternary `?:`, slice `[msb:lsb]`, bit select `[i]`, concat `{…}`, replication `{n{…}}`, power `**` for integer expressions); `odin3_status odin3_expr_parse(odin3_expr_parser *p, odin3_bytes text, const odin3_expr **out)` (located errors via a caller-supplied file/line); `odin3_status odin3_expr_eval_int(const odin3_expr *e, const odin3_expr_env *env, int64_t *out)` for width/parameter expressions (identifiers = INT parameters; overflow → INVALID_ARG). Iterative shunting-yard parser and iterative evaluator. Tests: precedence (Review Focus 3), every operator, sized literals with x/z, errors with column, overflow, depth 10k without recursion.

### Task 3: `.o3lib` reader and width-expression rule
**Files:** Create `src/techlib/reader.h`, `src/techlib/reader.c`, `tests/unit/test_techlib_reader.c`; Modify `src/ir/celltype.h/.c` (fourth width rule: `const odin3_width_expr *width_expr` — an opaque compiled expression plus an evaluation hook; `odin3_celltype_port_width_checked` evaluates it against the node's INT parameters; validation at registration), `CMakeLists.txt`.
**Produces:** `odin3_status odin3_techlib_read(odin3_design *design, const char *path)` registering each cell as a design-local type: kind `gate`→BIT, `hard`→HARD, `blackbox`→BLACKBOX; params (int with default); ports (`in`/`out`/`inout`, width expression, optional `signed`, `clock` flag stored as an attribute or flag); `area`/`delay` as cell attributes; `fn`/`seq`/`memory` statements parsed and kept (AST) in a per-design function table keyed by cell type for 1E/Phase 4; every `out` needs a driver statement unless kind is `blackbox` (Review Focus 4); duplicate cell/port/param names, unknown identifiers in width expressions, `;`-separated statements, comments. Tests per construct and per error.

### Task 4: Libraries, parametric black boxes, BLIF integration
**Files:** Create `lib/generic_gates.o3lib`, `lib/vtr.o3lib` (adder, multiply with A_WIDTH/B_WIDTH, single_port_ram/dual_port_ram with ADDR_WIDTH/DATA_WIDTH — port names and order exactly as the goldens declare them: check `~/odin3-ws/golden` `.model … .blackbox` stanzas of both oracles), `tests/unit/test_techlib_libs.c`; Modify `src/ir/celltype.{h,c}` (`declare_blackbox` against a parametric registered type: infer each INT parameter from a port whose width expression is exactly that parameter, evaluate every port's expression with the inferred values and require equality with the declared widths; the declared-model list entry stores the inferred parameter values — API `odin3_design_declared_model_params`), `src/frontends/blif/reader.c` (instances of a declared parametric model get the inferred params; a `.subckt` of a registered parametric type with no `.model` declaration infers params from its formals' maximum bit indices; option to load a tech library before reading: `odin3_blif_read_opts{techlib path list}` or the driver loads it first), `src/backends/blif/writer.c` (declared-model stanzas expand ports with the stored params), `tools/blif-roundtrip` (flag `--techlib lib/vtr.o3lib`).
Tests: Review Focus 1, 2, 5; every golden `.model … .blackbox` stanza (collect the distinct ones with a script into fixtures) resolves against `vtr.o3lib`; full golden round trip with `--techlib lib/vtr.o3lib` stays identical (record counts in `docs/PHASE1.md`).
