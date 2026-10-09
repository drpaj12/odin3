# 1F Writers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** JSON (Yosys schema), structural Verilog and Graphviz dot writers, each validated against independent tools on the goldens.

**Architecture:** Each writer walks modules/nodes/nets in ID order through the `src/ir` API with buffered output (the BLIF writer's I/O pattern), shares one name-escaping/naming helper per language, and a bash checker drives Yosys, `equiv-check`, Icarus and `dot` over the fixtures and goldens.

**Tech Stack:** C17, `src/ir`, `src/backends/blif` (patterns), Yosys (standalone build), `tools/equiv-check`, Icarus, Graphviz `dot`.

**Spec:** `docs/specs/2026-10-09-1F-writers-design.md`. **Ordering:** after 1C merges (needs the BLIF reader for fixtures and goldens). Tasks 1–3 are independent of each other.

## Global Constraints
- All earlier constraints (C17 `-Werror`, function limits, no adjacent convertible params, no abort/exit/goto/VLA/recursion, no NOLINT, `odin3_` snake_case, util-only containers, IR via API, deterministic output, located errors, partial output removed on failure as in the BLIF writer).

## Review Focus
1. A BLIF net name needing Verilog escaping (`$add~5^ADD~5-1[0]`) round-trips through Yosys `read_verilog` — test in Task 2.
2. JSON bit numbering: constants use "0"/"1", every IR net gets one bit ≥ 2, ports and netnames agree — test in Task 1.
3. A `$sop` with an OFF-set cover becomes a correct complemented assignment in Verilog and a correct `$lut`/`$sop` in JSON — tests in Tasks 1–2.
4. dot `--focus file:line` selects exactly the objects the provenance forward index returns — test in Task 3.
5. A latch with INIT 2/3 is written without a defined initial value (JSON `init` = x; Verilog no `initial`) — tests in Tasks 1–2.

---

### Task 1: JSON writer
**Files:** Create `src/backends/json/writer.{h,c}`, `tests/unit/test_json_writer.c`. **Produces:** `odin3_status odin3_json_write(const odin3_design *design, const char *path)`. Per spec "JSON". Tests: schema shape per construct; Review Focus 2, 3 (`$lut` for ≤ 6 inputs, `$sop` DEPTH/TABLE beyond), 5; deterministic; I/O failure.

### Task 2: Structural Verilog writer
**Files:** Create `src/backends/verilog/{writer.h,writer.c,escape.c}`, `tests/unit/test_verilog_writer.c`. **Produces:** `odin3_status odin3_verilog_write(const odin3_design *design, const char *path)`. Per spec "Verilog". Tests: escaping (Review Focus 1), every cell kind, vector ports and concatenations MSB first, black-box stubs, INIT handling (Review Focus 5), `iverilog -o /dev/null` compiles each fixture's output (skip if iverilog missing).

### Task 3: dot writer
**Files:** Create `src/backends/dot/{writer.h,writer.c}`, `tests/unit/test_dot_writer.c`. **Produces:** `odin3_status odin3_dot_write(const odin3_design *design, const char *path, const odin3_dot_opts *opts)` with `opts = {focus kind, focus value, max_nodes}`. Tests: clusters per module; edges; budget refusal message with the count; focus by hierarchical path, `file:line` (Review Focus 4), `cone(net)` (iterative); `dot -Tsvg` accepts outputs (skip if dot missing).

### Task 4: Checker and the golden run
**Files:** Create `tools/writer-check/writer-check` (bash), a small C driver `odin3-write` (`in.blif --json out --verilog out --dot out`), CTest `writer_check_fixtures`; Modify `docs/PHASE1.md` (results).
Validations per spec table; black-box stubs for Yosys `read_verilog` generated from the design's declared models; RAM goldens excluded from the equivalence step where `equiv-check` cannot model memories (listed); `-j` ≤ 2; record counts and slowest file in `docs/PHASE1.md` verbatim; every failure reduced to a fixture and fixed.
