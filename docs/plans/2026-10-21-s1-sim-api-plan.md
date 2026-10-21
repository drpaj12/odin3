# S1 Simulation API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Commit and push after every task and after each sub-step that turns a test green (commit incrementally). Never use `cd X && git ...`; use `git -C <worktree>` and absolute paths. Merge `origin/main` into a branch; never rebase or force-push.

**Goal:** a program steps the existing netlist simulator one cycle at a time through the C ABI and
Python (open a top, find signals by HDL name, set inputs, step, read values, reset, close); the
`sim` pass replays an input vector file and writes a VCD.

**Architecture:** the internal simulator (`src/sim/`) gains an exposed settle / clock edge / reset
and keeps its instance frames for names; a new signal table (`src/sim/signals.c`) lists and
resolves names and reads and writes values; the ABI (`src/api/sim_abi.c`) and the VCD writer
(`src/sim/vcd.c`) both sit on it; the Python class wraps the ABI; the `sim` pass gains
`--vectors-in` (a reader in `src/sim/vectors_in.c`) and `--vcd`.

**Tech Stack:** C17 core, Unity unit tests, cffi binding (`plugins/python/odin3.py`), stdlib
Python tests, bash CLI tests, Icarus Verilog (optional cross-check only).

**Spec:** `docs/specs/2026-10-21-s1-sim-api-design.md` (rule IDs SC, SA, SP, SV, SW are binding;
section 10 is the reference trace).

## Global Constraints

- `CLAUDE.md` code standard: C17; functions <= 60 lines, CCN <= 15, <= 5 parameters; no recursion
  (explicit stacks); no VLAs; `odin3_status` errors; only `src/util/` defines containers; named
  constants outside `tests/`; clang-format; `tools/lint.sh` clean (never add NOLINT or relax a gate).
- Every function in `include/odin3/odin3.h` gets a doc paragraph: purpose, ownership of returned
  memory, failure modes. The cdef block (between `ODIN3_CDEF_BEGIN` and `ODIN3_CDEF_END`) has no
  preprocessor lines and no attributes. Every new public function is `ODIN3_EXPORT` (from
  `src/util/attr.h`) so the `abi_exports` test passes.
- `ODIN3_ABI_VERSION` stays `4` (SA-12); only the history comment gains the S1 line.
- Python: stdlib plus cffi only, `ruff check` and `mypy --strict` clean, `unittest`.
- Hierarchical separator `.`; signal IDs start at 1, 0 = none.
- No change to existing vector output: every existing `sim` CLI and unit test stays green
  unchanged, except the one message text of SC-1 (`tests/unit/test_ir_cells.c:783`).
- Build and test (from the worktree root): `cmake --preset debug`;
  `/home/jamiespa/odin3-ws/work/controller-scripts/build-mem.sh --preset debug`;
  `env -u ODIN3_HEAVY_OK ctest --preset debug -R '<regex>' -j6 --output-on-failure`; before the last
  push of each PR: the full `env -u ODIN3_HEAVY_OK ctest --preset debug -j6` and `tools/lint.sh`.
  Nothing runs under `tools/heavy-run`.
- Commits end with the attribution lines the session's harness gives; no PR is opened by a lane
  (the controller reviews and opens it).

## Review Focus

1. A pass started through a **script** (`odin3_design_run_script`), not only `run_pass`, while a
   simulation is open: refused the same way (Task 3, `test_script_refused_while_open`).
2. Python frees a `Design` and its `Simulation` in either order, or in one garbage cycle: no
   use-after-free under ASan (Task 5, `test_gc_order_is_safe`).
3. A port whose bits are partly clocks (`input [1:0] ck`, `ck[0]` a clock): `set` of the port with
   the clock bit 1 is refused, with it 0 is accepted; `ck[0]` is CLOCK (Task 2,
   `test_write_rules`; Task 4, `test_mixed_clock_port`).
4. A vector file written for a different design (other port names or widths, or no header and
   the wrong bit count): a located parse error and no output file (Task 6 error tests; Task 7
   `sim_vectors_in` case "no output on error").
5. Names after `flatten` (a top wire literally named `u0.inner`) and after `opt` deleted a
   register: found the same as unflattened / not found (0, Python `KeyError`) (Task 2,
   `test_flattened_names`, `test_find`).

## PRs, branches and lanes

| PR | Branch | Tasks | Content | Depends on |
|---|---|---|---|---|
| S1a | `feat/s1a-sim-core` | 1, 2 | fixtures, settle/edge/reset, SC-1 hint, frames kept, signal table | nothing |
| S1b | `feat/s1b-sim-abi` | 3, 4 | open-simulation counter and pass refusal, the ABI (Peter reviews) | S1a merged |
| S1c | `feat/s1c-sim-python` | 5 | `Simulation` class, `python_sim`, manual section | S1b merged |
| S1d | `feat/s1d-sim-vectors-in` | 6, 7 | vector reader, `sim --vectors-in`, CLI case, PASSES row | nothing |
| S1e | `feat/s1e-sim-vcd` | 8, 9 | VCD writer, `sim --vcd`, CLI case, Icarus cross-check, docs | S1a and S1d merged |

Lane 1: S1a, S1b, S1c in order. Lane 2 (in parallel): S1d. Then S1e (either lane). S1a and S1d
both append to `CMakeLists.txt` and S1d edits `src/sim/vectors.{h,c}` that S1a does not touch;
the later of the two merges `origin/main` and resolves the CMake list by keeping both entries.
Each branch starts from `origin/main` after its dependencies merged:
`git -C /home/jamiespa/odin3-ws/odin3 fetch -q && git -C /home/jamiespa/odin3-ws/odin3 worktree add -b <branch> /home/jamiespa/odin3-ws/work/odin3-<branch-suffix> origin/main`.

## File map

| File | Task | Responsibility |
|---|---|---|
| `tests/sim/{traffic.v,hier.v,netnames.blif,wide.v,mixclk.v}` | 1 | fixtures (text below) |
| `src/sim/sim.h`, `src/sim/eval.c` | 1 | `odin3_sim_settle`, `odin3_sim_clock_edge`, `odin3_sim_reset` |
| `src/sim/build_names.c` | 1 | SC-1 hint |
| `src/sim/sim_internal.h`, `src/sim/build_internal.h`, `src/sim/build.c` | 2 | frames and net map kept in `odin3_sim` |
| `src/sim/signals.h`, `src/sim/signals.c` (new) | 2 | scopes, listing, lookup, read, write |
| `tests/unit/test_sim_signals.c` (new) | 1, 2 | internal tests |
| `src/ir/ir_internal.h`, `src/ir/design.h`, `src/ir/design.c`, `src/passes/manager.c` | 3 | open-simulation counter, refusal |
| `include/odin3/odin3.h`, `src/api/sim_abi.c` (new) | 4 | the ABI |
| `tests/unit/test_sim_abi.c` (new) | 3, 4 | ABI tests through `libodin3.so` |
| `plugins/python/odin3.py`, `tests/tools/test_python_sim.py` (new), `docs/user/python-api.md` | 5 | Python |
| `src/sim/vectors.h`, `src/sim/vectors.c`, `src/sim/vectors_in.c` (new), `tests/unit/test_sim_vectors_in.c` (new) | 6 | line writer, reader |
| `src/passes/sim_pass.c`, `tests/cli/test_cli.sh`, `docs/PASSES.md` | 7, 9 | pass syntax, replay, VCD |
| `src/sim/vcd.h`, `src/sim/vcd.c` (new), `tests/unit/test_sim_vcd.c` (new) | 8 | VCD writer |
| `tests/tools/test_sim_vcd.py` (new), `docs/user/outputs.md`, `src/sim/README.md` | 9 | VCD cross-check, docs |
| `CMakeLists.txt` | all | sources and tests |

---

### Task 1: fixtures, settle / clock edge / reset, word-cell hint (S1a)

**Files:**
- Create: `tests/sim/traffic.v`, `tests/sim/hier.v`, `tests/sim/netnames.blif`, `tests/sim/wide.v`, `tests/sim/mixclk.v`, `tests/unit/test_sim_signals.c`
- Modify: `src/sim/sim.h`, `src/sim/eval.c`, `src/sim/build_names.c:194-200` (`odin3_sim_err_unsupported`), `tests/unit/test_ir_cells.c:783`, `CMakeLists.txt` (`odin3_core` list unchanged; tests near line 1646)

**Interfaces:**
- Produces (in `src/sim/sim.h`, which now also includes `sim/cell.h` for `odin3_sim_event`):
  - `void odin3_sim_settle(odin3_sim *sim);` one settle (every cell's COMB in level order). Cannot fail.
  - `void odin3_sim_clock_edge(odin3_sim *sim, odin3_sim_event event);` event is `ODIN3_SIM_POSEDGE` or `ODIN3_SIM_NEGEDGE` (assert): set every clock input to 1 / 0, send the event to every edge-triggered cell, then settle.
  - `void odin3_sim_reset(odin3_sim *sim);` zero `values` (all `n_values`) and `state` (all `n_state`), then `odin3_sim_start(sim)` (SC-4). Allocates nothing.
  - `odin3_sim_cycle` becomes `odin3_sim_settle; odin3_sim_clock_edge(POSEDGE); odin3_sim_clock_edge(NEGEDGE)` (identical work and order to today).

Fixture texts (exact; verified with the release build and Icarus on 2026-10-21):

`tests/sim/traffic.v`
```verilog
// S1 fixture: a two-way traffic light, one clock, two car sensors, two 2-bit lights.
module traffic(input clk, input rst, input car_ns, input car_ew,
               output [1:0] light_ns, output [1:0] light_ew);
    localparam RED = 2'd0, YELLOW = 2'd1, GREEN = 2'd2;
    reg [1:0] state; // 0 NS green, 1 NS yellow, 2 EW green, 3 EW yellow
    reg [2:0] timer;
    always @(posedge clk) begin
        if (rst) begin
            state <= 2'd0;
            timer <= 3'd0;
        end else if (timer != 3'd0) begin
            timer <= timer - 3'd1;
        end else begin
            case (state)
                2'd0: if (car_ew) begin state <= 2'd1; timer <= 3'd1; end
                2'd1: begin state <= 2'd2; timer <= 3'd3; end
                2'd2: if (car_ns) begin state <= 2'd3; timer <= 3'd1; end
                default: begin state <= 2'd0; timer <= 3'd3; end
            endcase
        end
    end
    assign light_ns = state == 2'd0 ? GREEN : state == 2'd1 ? YELLOW : RED;
    assign light_ew = state == 2'd2 ? GREEN : state == 2'd3 ? YELLOW : RED;
endmodule
```

`tests/sim/hier.v`
```verilog
// S1 fixture: two instances of one stage, an internal wire, a register opt removes, a kept one.
module stage(input clk, input d, output reg q);
    reg inner;
    always @(posedge clk) begin
        inner <= d;
        q <= inner;
    end
endmodule
module hier(input clk, input [3:0] a, output [3:0] y, output z);
    wire mid;
    reg dead_r;
    (* keep *) reg kept_r;
    stage u0(.clk(clk), .d(a[0]), .q(mid));
    stage u1(.clk(clk), .d(mid), .q(z));
    always @(posedge clk) begin
        dead_r <= a[1];
        kept_r <= a[2];
    end
    assign y = ~a;
endmodule
```

`tests/sim/netnames.blif`
```
.model netnames
.inputs clk a b
.outputs y
.names a b n1
11 1
.latch n1 q re clk 0
.names q y
1 1
.end
```

`tests/sim/wide.v`
```verilog
module wide(input clk, input [69:0] d, output reg [69:0] q);
    always @(posedge clk) q <= d;
endmodule
```

`tests/sim/mixclk.v`
```verilog
module mixclk(input [1:0] ck, output reg q);
    always @(posedge ck[0]) q <= ck[1];
endmodule
```

Test helpers in `test_sim_signals.c` (file-local): `load_v(const char *file, const char *script_tail)`
runs `odin3_pass_run(design, "read_verilog", <ODIN3_SIM_FIXTURES "/" file>)` then each pass of the
tail (`"proc"`, `"opt"`, `"lower"`, and `"flatten"` where a test says) with empty args;
`load_blif(file)` runs `read_blif`; `build()` returns `odin3_sim_build(design, odin3_design_top(design), &sim)`.
Log capture as in `tests/unit/test_sim_cycle.c` (`capture_sink`, `log_text`).

- [ ] **Step 1: Write the fixtures and the failing tests**

```c
/* traffic lowered; A and B built twice; PRNG seed 11 each; 64 cycles. */
static void test_settle_edges_equal_cycle(void);
    /* each cycle: odin3_sim_drive_random(A, &pa); odin3_sim_drive_random(B, &pb);
       odin3_sim_cycle(A); odin3_sim_settle(B); odin3_sim_clock_edge(B, ODIN3_SIM_POSEDGE);
       odin3_sim_clock_edge(B, ODIN3_SIM_NEGEDGE);
       every output bit of A equals B's (odin3_sim_get_output);
       at the end TEST_ASSERT_EQUAL_UINT64(A->n_hook_calls, B->n_hook_calls) */
static void test_reset_equals_fresh_build(void);
    /* A: 20 cycles of odin3_sim_vectors_step seed 9, then odin3_sim_reset(A);
       every non-clock input bit of A reads false (odin3_sim_get_input);
       B fresh; 32 cycles of odin3_sim_vectors_step with seed 4 on each into two strbufs;
       TEST_ASSERT_EQUAL_STRING(a.data, b.data) */
static void test_word_cell_hint(void);
    /* traffic with "proc", "opt" only: odin3_sim_build == ODIN3_ERR_INVALID_ARG;
       strstr(log_text, "the cell type has no simulate hook; run `lower` first") != NULL */
```

Register in `CMakeLists.txt` after `odin3_add_unit_test(test_sim_word)`:
`odin3_add_unit_test(test_sim_signals)` and
`target_compile_definitions(test_sim_signals PRIVATE ODIN3_SIM_FIXTURES="${PROJECT_SOURCE_DIR}/tests/sim")`.

- [ ] **Step 2: Build and run; expect compile failure** (`odin3_sim_settle` undeclared).
Run: `ctest --preset debug -R '^test_sim_signals$'`.

- [ ] **Step 3: Implement the three functions in `src/sim/eval.c`** (make the static `settle` and `edge` the bodies of the public ones; `odin3_sim_reset` uses `memset` on `values` and `state` when non-NULL). In `odin3_sim_err_unsupported` the why becomes `"the cell type has no simulate hook; run `lower` first"` when `def->gran == ODIN3_GRAN_WORD`, unchanged otherwise. Update the expected text at `tests/unit/test_ir_cells.c:783` (`$dlatch` is a word cell) to end in `; run `lower` first`.

- [ ] **Step 4: Run** `ctest --preset debug -R '^(test_sim_signals|test_sim_cycle|test_sim_build|test_sim_cells|test_sim_word|test_ir_cells|test_pass_outputs|cli_sim)$'`. Expected: all pass.

- [ ] **Step 5: Commit and push** (`feat(sim): settle, clock edge and reset; lower hint for word cells`).

### Task 2: frames kept, the signal table (S1a)

**Files:**
- Create: `src/sim/signals.h`, `src/sim/signals.c`
- Modify: `src/sim/sim_internal.h`, `src/sim/build_internal.h`, `src/sim/build.c` (`allocate`, `odin3_sim_destroy`), `tests/unit/test_sim_signals.c`, `CMakeLists.txt` (append `src/sim/signals.c` to `odin3_core`), `src/sim/README.md`

**Interfaces:**
- Consumes: Task 1's `odin3_sim_reset`, `odin3_sim_settle`.
- Produces, `src/sim/sim_internal.h`: `odin3_sim_frame` and `ODIN3_SIM_UNSET` move here from
  `build_internal.h` unchanged; `struct odin3_sim` gains `odin3_sim_frame *frames; uint32_t n_frames;
  uint32_t *netmap; size_t n_netmap;` (doc: `netmap[frames[f].map + net]` is the final slot of net
  `net` of frame f, or `ODIN3_SIM_UNSET` for a net with none). `allocate` remaps the builder's net map
  through `uf` (UNSET entries stay UNSET) and takes `frames` and `netmap` with `take()`;
  `odin3_sim_destroy` frees both.
- Produces, `src/sim/signals.h` (each with a doc comment):

```c
typedef enum odin3_sim_kind {           /* same values as the ABI's odin3_simulation_kind */
    ODIN3_SIM_KIND_INPUT = 0, ODIN3_SIM_KIND_CLOCK = 1,
    ODIN3_SIM_KIND_OUTPUT = 2, ODIN3_SIM_KIND_INTERNAL = 3
} odin3_sim_kind;

typedef struct odin3_sim_scope {
    uint32_t name;   /* table string ID of the instance path ("" for scope 0) */
    uint32_t leaf;   /* table string ID: instance name, "#<node ID>" unnamed, top module name for 0 */
    uint32_t parent; /* 0 for scope 0 */
} odin3_sim_scope;   /* module and instance node: sim->frames[scope] */

typedef struct odin3_sim_signal {
    uint32_t name, leaf;     /* table string IDs (full name, leaf) */
    uint32_t scope;
    uint32_t kind;           /* odin3_sim_kind */
    uint32_t width;
    int32_t msb, lsb;
    uint32_t obj_kind;       /* ODIN3_OBJ_WIRE or ODIN3_OBJ_NET */
    uint32_t obj_id;         /* wire or net ID in the scope's module */
    uint32_t first_bit;
    uint32_t slot_first;     /* index of bit 0's slot in the table's slot array */
    uint32_t in_port;        /* top input port for INPUT/CLOCK, else UINT32_MAX */
    uint32_t in_bit;         /* that port's bit for the signal's bit 0 */
} odin3_sim_signal;

typedef struct odin3_sim_signals odin3_sim_signals; /* opaque, defined in signals.c */

odin3_status odin3_sim_signals_create(odin3_sim *sim, odin3_sim_signals **out);
void odin3_sim_signals_destroy(odin3_sim_signals *sig);
uint32_t odin3_sim_signals_listed(const odin3_sim_signals *sig);
uint32_t odin3_sim_signals_total(const odin3_sim_signals *sig);
const odin3_sim_signal *odin3_sim_signals_at(const odin3_sim_signals *sig, uint32_t id);
uint32_t odin3_sim_signals_scope_count(const odin3_sim_signals *sig);
const odin3_sim_scope *odin3_sim_signals_scope(const odin3_sim_signals *sig, uint32_t scope);
const char *odin3_sim_signals_text(const odin3_sim_signals *sig, uint32_t str);
odin3_status odin3_sim_signals_find(odin3_sim_signals *sig, odin3_bytes name, uint32_t *id);
odin3_status odin3_sim_signals_read(const odin3_sim_signals *sig, uint32_t id, uint8_t *bits,
                                    uint32_t count);
odin3_status odin3_sim_signals_write(odin3_sim_signals *sig, uint32_t id, const uint8_t *bits,
                                     uint32_t count, const char **why);
```

Semantics: `create` builds scopes (SA-3) and the listed signals (SA-4) of `sim` (borrowed; it must
outlive the table); `ODIN3_ERR_NO_MEMORY` with nothing left allocated. `at`: IDs 1..total, else
NULL. `scope`: 0..count-1, else NULL. `text`: the string of a table string ID. `find`: SA-5, `*id`
0 when not found; `ODIN3_ERR_NO_MEMORY` (table unchanged) when an append fails. `read`: copies the
`count` bits (0/1) of signal `id`; `ODIN3_ERR_INVALID_ARG` (not logged) for an unknown ID or
`count != width`. `write`: SA-7 for `set_bits`; on `ODIN3_ERR_INVALID_ARG` (not logged; nothing
changed) `*why` is one of the exact strings `"unknown signal"`, `"not an input"`, `"count is not
the width"`, `"a bit is not 0 or 1"`, `"a clock bit set to 1"`; writes go through
`odin3_sim_set_input(sim, (odin3_sim_bit){in_port, in_bit + k}, ...)`, clock bits skipped.

Storage inside `signals.c`: a private `odin3_strtab` (from `odin3_strtab_create`) holds every name
(its strings never move, which SA-10 needs); a `odin3_u64map` maps a name's string ID to its signal
ID (first listed wins; appended names are inserted too, which gives step 2 of SA-5); `odin3_vec`s
of `odin3_sim_signal`, of `uint32_t` slots and of `odin3_sim_scope`. Scope paths and full names are
built with `odin3_strbuf` and `.`. Public-name rule: name string non-empty and not starting with
`$`. Wire bit slots: `sim->netmap[frames[f].map + odin3_wire_net(mod, wire, bit).v]`; top input and
output port signals use `sim->in_bits` / `sim->out_bits` from `in_ports[p].first` /
`out_ports[p].first`. Kind of a top input port: CLOCK when every bit has `in_clock` set. Instance
lookup for SA-5 step 5: `odin3_strtab_find` on the design's strtab (a text not in it names
nothing; never intern), `odin3_module_find_node`, then a scan of `sim->frames` for
`parent == cur && inst == node`. Wire and net lookup: `odin3_module_find_wire` / `odin3_module_find_net` after
`odin3_strtab_find`. Keep every function within the size limits (split listing, lookup and
bit-select parsing into helpers).

- [ ] **Step 1: Write the failing tests** (in `test_sim_signals.c`; `id(name)` = find helper asserting OK):

```c
static void test_hier_scopes(void);
    /* hier lowered; create; scope_count == 3;
       scope 0: name "", leaf "hier", parent 0; scope 1: name "u0", leaf "u0", parent 0;
       scope 2: name "u1", leaf "u1", parent 0 */
static void test_hier_listed_names(void);
    /* listed == 14; names of IDs 1..14, in order:
       "clk","a","y","z","mid","kept_r","u0.clk","u0.d","u0.q","u0.inner",
       "u1.clk","u1.d","u1.q","u1.inner";
       kinds: clk CLOCK, a INPUT, y OUTPUT, z OUTPUT, the rest INTERNAL;
       "a": width 4, msb 3, lsb 0, obj_kind ODIN3_OBJ_WIRE, scope 0;
       "u0.inner": scope 1, leaf "inner", width 1 */
static void test_find(void);
    /* id("u0.inner") == 10; id("a[2]") == 15, its width 1, kind INPUT, msb 2, lsb 2,
       first_bit 2, leaf "a[2]"; id("a[2]") again == 15; id("u1.q[0]") == 16 (INTERNAL);
       id("dead_r") == 0; id("nope") == 0; id("u2.q") == 0; id("a[4]") == 0; id("a[x]") == 0;
       total == 16, listed == 14 */
static void test_netnames_listing_blif(void);
    /* netnames.blif: listed == 6, names "clk","a","b","y","n1","q";
       n1: obj_kind ODIN3_OBJ_NET, width 1, kind INTERNAL; clk kind CLOCK */
static void test_flattened_names(void);
    /* hier with "proc","opt","lower","flatten": scope_count == 1; id("u0.inner") != 0;
       drive as test_values_follow_pipeline: z reads 1 after cycle 4 */
static void test_values_follow_pipeline(void);
    /* hier lowered; write "a" bits {1,0,0,0}; odin3_sim_cycle; write a {0,0,0,0};
       after cycle 1: u0.inner 1, mid 0, z 0; after 2: mid 1; after 4: z 1; after 5: z 0.
       Then write a {1,0,1,0} (a = 5): y still reads {1,1,1,1} (settled for a = 0);
       odin3_sim_settle; y reads {0,1,0,1} */
static void test_write_rules(void);
    /* hier: write y -> INVALID_ARG why "not an input"; write clk -> "not an input"
       (kind CLOCK); write a with count 3 -> "count is not the width"; a byte 2 ->
       "a bit is not 0 or 1"; id 999 -> "unknown signal".
       mixclk lowered: "ck" listed kind INPUT; write ck {0,1} OK and odin3_sim_get_input bit 1
       true; write ck {1,0} -> "a clock bit set to 1"; id("ck[0]") kind CLOCK, id("ck[1]") INPUT */
static void test_signals_oom(void);
    /* hier lowered; for k = 0 .. 100000: odin3_util_set_alloc_fail_after(k);
       st = odin3_sim_signals_create(sim, &sig); reset fail-after to -1;
       st is OK or NO_MEMORY; on OK destroy and stop; TEST_ASSERT reached OK.
       Same loop for odin3_sim_signals_find(sig, "a[2]") on a fresh table: NO_MEMORY leaves
       total unchanged; OK gives 15 */
```

- [ ] **Step 2: Run** `ctest --preset debug -R '^test_sim_signals$'`; expected: compile failure (no `signals.h`).
- [ ] **Step 3: Keep frames and net map in the build** (`sim_internal.h`, `build_internal.h`, `build.c`) as in Interfaces; run `ctest --preset debug -R '^(test_sim_build|test_sim_cycle)$'`: pass (no behaviour change).
- [ ] **Step 4: Implement `src/sim/signals.c`**; add it to `odin3_core` in `CMakeLists.txt`; add one line to `src/sim/README.md`: "signals.c: scopes and named signals over a built simulator (S1)."
- [ ] **Step 5: Run** `ctest --preset debug -R '^(test_sim_signals|test_sim_build|test_sim_cycle|cli_sim)$'`; expected: all pass, no ASan report.
- [ ] **Step 6: Full debug ctest and `tools/lint.sh`; commit and push** (`feat(sim): signal table: scopes, HDL names, lookup, read and write`). Hand S1a to the controller.

### Task 3: the open-simulation counter and the pass refusal (S1b)

**Files:**
- Modify: `src/ir/ir_internal.h` (`struct odin3_design` gains `uint32_t sim_holds; /* S1: open odin3_simulation handles (SA-11) */`), `src/ir/design.h`, `src/ir/design.c`, `src/passes/manager.c` (`run_def`)
- Modify: `tests/unit/test_sim_signals.c` (the counter is internal; Task 4 tests it again through the ABI)

**Interfaces:**
- Produces (`src/ir/design.h`):
  - `uint32_t odin3_design_sim_holds(const odin3_design *design);` 0 for NULL.
  - `void odin3_design_sim_hold(odin3_design *design);` +1.
  - `void odin3_design_sim_release(odin3_design *design);` -1; `assert(holds > 0)`.
  - `run_def` first: when `odin3_design_sim_holds(design) > 0`, log at ERROR
    `"pass %s: the design has %u open simulation(s); close them first (odin3_simulation_close)"` with
    `def->name` and the count, return `ODIN3_ERR_INVALID_ARG` (before the process refusal and before
    any provenance run opens).
  - `odin3_design_clone` leaves `sim_holds` 0 (it callocs; assert this in the test, change nothing
    if it already holds).

`test_sim_abi.c` links `libodin3` only (no `src/` includes), so the counter is driven through the
ABI in Task 4; in this task add the internal test to `test_sim_signals.c` instead:

- [ ] **Step 1: Failing tests in `test_sim_signals.c`:**

```c
static void test_pass_refused_while_held(void);
    /* empty design; odin3_design_sim_hold(design);
       odin3_pass_run(design, "stats", (odin3_bytes){0}) == ODIN3_ERR_INVALID_ARG;
       strstr(log_text, "pass stats: the design has 1 open simulation(s); close them first") */
static void test_script_refused_while_held(void);
    /* hold; odin3_design_run_script(design, "stats", (odin3_script_src){"-p",
       ODIN3_SCRIPT_BY_COMMAND}) != ODIN3_OK and the same log text; release; it is ODIN3_OK */
static void test_copy_is_not_held(void);
    /* hold; odin3_design_copy(design, &copy) == OK; odin3_design_sim_holds(copy) == 0;
       odin3_pass_run(copy, "stats", ...) == OK; release */
```
- [ ] **Step 2: Run, expect compile failure. Step 3: implement. Step 4: run** `ctest --preset debug -R '^(test_sim_signals|test_pass_outputs|test_design_copy|cli_script_roundtrip)$'`; all pass.
- [ ] **Step 5: Commit and push** (`feat(ir): count open simulations; refuse passes while one is open`).

### Task 4: the C ABI (S1b; Peter reviews this PR)

**Files:**
- Modify: `include/odin3/odin3.h` (history line; new group after `odin3_abc_engine_register`, before `/* ODIN3_CDEF_END */`)
- Create: `src/api/sim_abi.c`, `tests/unit/test_sim_abi.c`
- Modify: `CMakeLists.txt` (`src/api/sim_abi.c` in `odin3_core` after `src/api/abc.c`; test registration below)

**Interfaces:**
- Consumes: Tasks 1 to 3 (`odin3_sim_build`, `odin3_sim_destroy`, `odin3_sim_cycle`,
  `odin3_sim_settle`, `odin3_sim_reset`, the `odin3_sim_signals_*` functions, the hold functions,
  `odin3_design_refuse_processes(design, NULL)`, `odin3_api_invalid`, `odin3_api_invalid_why`).
- Produces: exactly the types and 15 functions of spec 3.1 and 3.2 (copy the declarations
  verbatim), each with the SA rule's text as its doc paragraph. `struct odin3_simulation {
  odin3_design *design; odin3_sim *sim; odin3_sim_signals *signals; uint64_t cycle; }` in
  `sim_abi.c`. `_Static_assert` that each `ODIN3_SIMULATION_*` equals its `ODIN3_SIM_KIND_*`.
  History comment line: `ABI v4 (cont., S1): simulations (odin3_simulation_*): open a top, find
  signals, set inputs, step, read values, reset, close; passes are refused while a simulation is
  open.` Every failure logs through `odin3_api_invalid(__func__)` or `odin3_api_invalid_why(__func__,
  why)` (why from `odin3_sim_signals_write`, or `"width over 64"`, `"value wider than the signal"`,
  `"unknown signal"`, `"count is not the width"`, `"unknown scope"`). `open`: NULL checks; module 0 ->
  `odin3_design_top`, none -> log `"odin3_simulation_open: the design has no top module"`; module not
  in the design -> invalid; `odin3_design_refuse_processes(design, NULL)`; `odin3_sim_build`;
  `odin3_sim_signals_create`; `odin3_design_sim_hold`. On any failure free what was built, `*out`
  NULL. `close`: destroy signals, simulator, `odin3_design_sim_release`, free.

Registration (after the `test_ast_abi` lines):
```cmake
add_executable(test_sim_abi tests/unit/test_sim_abi.c)
target_link_libraries(test_sim_abi PRIVATE libodin3 unity odin3_flags)
target_compile_definitions(test_sim_abi PRIVATE ODIN3_SIM_FIXTURES="${PROJECT_SOURCE_DIR}/tests/sim"
    ODIN3_TEST_TMP="${PROJECT_BINARY_DIR}/test_sim_abi.vec")
add_test(NAME test_sim_abi COMMAND test_sim_abi)
set_tests_properties(test_sim_abi PROPERTIES LABELS unit)
```

Helpers in the test: `lowered(file)` creates a design and runs `read_verilog <path>`, `proc`, `opt`,
`lower` with `odin3_design_run_pass`; `sig(sim, name)` asserts `find_signal` OK and nonzero;
`get(sim, name)` returns the `uint64_t`; log capture through `odin3_log_set_sink`.

- [ ] **Step 1: Write the failing tests:**

```c
static void test_null_arguments(void);
    /* open(NULL, 0, &s) INVALID_ARG and s == NULL; open(d, 0, NULL) INVALID_ARG;
       close(NULL) returns; get_signal_count(NULL, &n), get_cycle(sim, NULL),
       step(NULL, 1), find_signal(sim, NULL, &id) all INVALID_ARG */
static void test_open_needs_a_top(void);
    /* empty design: open(d, 0, &s) INVALID_ARG, s NULL,
       log contains "the design has no top module"; open(d, 99, &s) INVALID_ARG */
static void test_open_refuses_processes(void);
    /* read_verilog traffic.v only: INVALID_ARG; log contains "has processes; run proc first" */
static void test_open_needs_lower(void);
    /* traffic after proc, opt: INVALID_ARG; log contains "run `lower` first" */
static void test_traffic_reference_trace(void);
    /* traffic lowered, open(d, 0): light_ns 2, light_ew 0, cycle 0.
       Then the 10 rows of spec section 10: set rst, car_ns, car_ew; step(sim, 1);
       assert light_ns, light_ew, "state", "timer" as in the table; get_cycle == 10 */
static void test_matches_sim_pass_vectors(void);
    /* traffic lowered; run_pass("sim", "16 3 " ODIN3_TEST_TMP) before open;
       read the file; open; for each vector line "c iii oooo": set rst, car_ns, car_ew from
       inputs[0], [1], [2]; step 1; light_ns == outputs[0..1] (MSB first), light_ew ==
       outputs[2..3] */
static void test_settle_shows_combinational(void);
    /* hier lowered: y == 15 after open; set a = 5; y still 15; settle; y == 10; cycle 0 */
static void test_reset(void);
    /* traffic: 3 rows of the trace, reset: get_cycle 0, light_ns 2, rst reads 0, state 0;
       repeat the full trace: same values */
static void test_set_and_get_rules(void);
    /* traffic: set(car_ns, 2) INVALID_ARG; set(light_ns, 1) INVALID_ARG; set(clk, 0)
       INVALID_ARG; wide lowered: get(q) INVALID_ARG ("width over 64"); set_bits(d, 70 bytes
       of i % 3 == 0 ? 1 : 0) OK; step 1; get_bits(q, 70) equals them; get_bits(q, 69)
       INVALID_ARG */
static void test_mixed_clock_port(void);
    /* mixclk lowered: signal "ck" kind INPUT; set(ck, 2) OK; set(ck, 1) INVALID_ARG;
       find "ck[0]" kind CLOCK; step 1; q == 1 */
static void test_scopes_and_signal_info(void);
    /* hier lowered: scope count 3; scope 1 name "u0", leaf "u0", parent 0, module == the
       module ID of "stage" (odin3_design_lookup_module), inst.id != 0;
       signal count 14; signal 1 name "clk" kind CLOCK; info of find("u0.inner"): leaf "inner",
       scope 1, obj.kind ODIN3_OBJ_WIRE, obj.module == stage's ID */
static void test_names_stay_valid(void);
    /* hier: keep info.name of signal 10; find every "<name>[i]" of the 14 listed signals for
       each declared index i (20 appends, IDs 15..34); TEST_ASSERT_EQUAL_STRING("u0.inner",
       kept pointer); signal 34's info is valid */
static void test_passes_refused_while_open(void);
    /* hier open: run_pass("stats", "") INVALID_ARG, log "open simulation(s)"; two sims open:
       log says "has 2 open"; close both: run_pass("stats", "") OK */
static void test_two_simulations_independent(void);
    /* traffic: open A and B; A follows the trace for 2 rows; B stays at light_ns 2 */
```
- [ ] **Step 2: Run** `ctest --preset debug -R '^(test_sim_abi|abi_exports)$'`; expected: link failure.
- [ ] **Step 3: Write the header block and `src/api/sim_abi.c`.**
- [ ] **Step 4: Run** `ctest --preset debug -R '^(test_sim_abi|abi_exports|symbol_rules|test_abi_core|test_abi_ext|python_walk|python_copy|wheel)'`; expected: pass (the cdef still parses in cffi: `python_walk` loads it).
- [ ] **Step 5: Full debug ctest and `tools/lint.sh`; commit and push** (`feat(abi): odin3_simulation: per-cycle simulation of a top`). PR description lists the agent defaults D-1 to D-6 and the six questions of spec section 9 for Peter.

### Task 5: Python `Simulation` (S1c)

**Files:**
- Modify: `plugins/python/odin3.py` (module docstring gains a 4-line simulation example; classes after `Design`), `docs/user/python-api.md` (new section `## Simulation` before `## Plugins`), `CMakeLists.txt` (CTest `python_sim` after `python_copy`, same form)
- Create: `tests/tools/test_python_sim.py`

**Interfaces:**
- Consumes: the ABI of Task 4.
- Produces: SP-1 exactly: `SignalInfo`, `ScopeInfo` (NamedTuples, field order as in SP-1),
  `Simulation(design: Design, top: str | None = None)` with `close`, `__enter__`, `__exit__`,
  `signals`, `scopes`, `signal`, `get`, `set`, `__getitem__`, `__setitem__`, `settle`,
  `step(cycles: int = 1)`, `reset`, property `cycle`; `Design.simulate(top: str | None = None) ->
  Simulation`; `Design.__init__` adds `self._sims: weakref.WeakSet[Simulation]`; `Design.close()`
  closes them first. Kind strings via `Odin3.enum_name("odin3_simulation_kind", v)`. The handle:
  `self.ptr = ffi.gc(raw, lambda p, _d=design.ptr: lib.odin3_simulation_close(p))` (SP-4). Errors
  per SP-3 (exact `KeyError` text of SP-3). `step` with `cycles` < 0 or > 2**32-1 is `ValueError`.

- [ ] **Step 1: Failing tests (`tests/tools/test_python_sim.py`, header and skip decorators as `test_python_copy.py`; fixtures `REPO_ROOT / "tests" / "sim"`; designs made with `Design()` and `run_script("read_verilog \"<path>\"; proc; opt; lower")`):**

```python
def test_traffic_reference_trace(self) -> None:   # spec section 10 table, via sim["rst"] = ...
def test_signals_and_scopes(self) -> None:
    # hier: [s.name for s in sim.signals()] == the 14 names of Task 2; signals()[0].kind == "clock";
    # [s.leaf for s in sim.scopes()] == ["hier", "u0", "u1"]; sim.signal("a[2]").width == 1
def test_unknown_name_raises_key_error(self) -> None:
    # with self.assertRaisesRegex(KeyError, "dead_r: not in the simulated netlist"): sim["dead_r"]
def test_value_range(self) -> None:
    # traffic: sim.set("car_ns", 2) -> ValueError; sim.set("car_ns", -1) -> ValueError;
    # sim.set("light_ns", 1) -> Odin3Error
def test_wide_signal_roundtrip(self) -> None:
    # wide: v = (1 << 69) | 0x2AAAAAAAAAAAAAAAA; sim["d"] = v; sim.step(); sim["q"] == v
def test_run_pass_refused_while_open(self) -> None:
    # open; design.run_pass("stats") raises Odin3Error; sim.close(); run_pass("stats") succeeds
def test_design_close_closes_simulations(self) -> None:
    # sim = design.simulate(); design.close(); sim.step() raises RuntimeError("simulation is closed")
def test_gc_order_is_safe(self) -> None:
    # three rounds: (del sim; del design), (del design; del sim), and a reference cycle;
    # gc.collect() after each; no crash (ASan preloaded)
def test_two_simulations_one_design(self) -> None:
    # a, b = design.simulate(), design.simulate(); drive a 2 trace rows; b["light_ns"] == 2
def test_reset_and_cycle(self) -> None:
    # 3 steps -> cycle == 3; reset() -> cycle == 0, sim["light_ns"] == 2
def test_steps_fast_enough(self) -> None:
    # traffic: 600 steps, each sets car_ns, car_ew (alternating) and reads light_ns, light_ew;
    # elapsed (time.perf_counter) < 10.0 s (60 steps/s with margin, Debug + ASan)
```
For `test_gc_order_is_safe` round 3, `Design` gets no new attribute: put both objects in a list
that references itself (`box = [design, sim]; box.append(box)`), then `del design, sim, box`.

- [ ] **Step 2: Run** `ctest --preset debug -R '^python_sim$' --output-on-failure`; expected: failures (no `Simulation`).
- [ ] **Step 3: Implement**; manual section: the flow line (`read_verilog; proc; opt; lower`), a traffic-light example (open, set sensors, step, read lights), naming rules (SA-5, `.`), what a step is (SC-2), the clock rule (clocks must be top inputs; use clock enables), names removed by `opt` (`(* keep *)`), and that passes are refused while a simulation is open.
- [ ] **Step 4: Run** `ctest --preset debug -R '^(python_sim|python_walk|python_copy|wheel)$'` and `.venv/bin/ruff check plugins/python tests/tools/test_python_sim.py` and `.venv/bin/mypy --strict plugins/python/odin3.py`; expected: pass, clean. `sphinx-build -W -b html docs/user build/docs` succeeds.
- [ ] **Step 5: Full ctest, `tools/lint.sh`; commit and push** (`feat(python): Simulation: step a design from Python`).

### Task 6: vector line writer and the input-vector reader (S1d)

**Files:**
- Modify: `src/sim/vectors.h`, `src/sim/vectors.c`
- Create: `src/sim/vectors_in.c`, `tests/unit/test_sim_vectors_in.c`
- Modify: `CMakeLists.txt` (`src/sim/vectors_in.c` in `odin3_core` after `src/sim/vectors.c`; `odin3_add_unit_test(test_sim_vectors_in)` with the `ODIN3_SIM_FIXTURES` definition)

**Interfaces:**
- Produces (`src/sim/vectors.h`):

```c
#define ODIN3_SIM_VECTORS_MAX_BYTES (1U << 28)

typedef struct odin3_sim_vectors_in {
    uint8_t *bits;   /* cycles x n_bits bytes, 0/1, line by line in file order (ports in order, MSB first) */
    uint32_t n_bits; /* the design's non-clock input bits */
    uint32_t cycles; /* vector lines */
    uint64_t seed;   /* the title line's seed; 0 without one */
} odin3_sim_vectors_in;

odin3_status odin3_sim_vectors_line(const odin3_sim *sim, uint32_t cycle, odin3_strbuf *buf);
odin3_status odin3_sim_vectors_parse(const odin3_sim *sim, const char *path, odin3_bytes text,
                                     odin3_sim_vectors_in *out);
odin3_status odin3_sim_vectors_read(const odin3_sim *sim, const char *path,
                                    odin3_sim_vectors_in *out);
void odin3_sim_vectors_in_free(odin3_sim_vectors_in *in);
void odin3_sim_vectors_drive(odin3_sim *sim, const odin3_sim_vectors_in *in, uint32_t cycle);
```
  - `line`: appends `"<cycle> <inputs> <outputs>\n"` for the current values (no drive, no cycle);
    `odin3_sim_vectors_step` becomes drive_random + `odin3_sim_cycle` + `line` (same text as today).
  - `parse`: SV-2 and SV-3 over `text`, messages prefixed `"<path>:<line>: "` (exact texts in
    SV-3), `ODIN3_ERR_PARSE`; `ODIN3_ERR_NO_MEMORY`; `*out` zeroed on failure. The expected port
    lines come from `odin3_sim_vectors_header` with a dummy title, dropping its first line.
  - `read`: `odin3_file_read_all(path, ODIN3_SIM_VECTORS_MAX_BYTES, ...)`; `ODIN3_ERR_IO` logged
    `"sim: cannot read `<path>`: <strerror(err)>"`; too big logged `"<path>: larger than 268435456
    bytes"`, `ODIN3_ERR_PARSE`; then `parse`.
  - `drive`: sets every non-clock input bit from line `cycle` (inverse of the writer's order).

- [ ] **Step 1: Failing tests** (`netnames.blif` via `read_blif` unless said; its port lines are
  `# clock clk 0`, `# input a 1`, `# input b 1`, `# output y 1`; `P(text)` = parse with path
  `"v.vec"`; error tests assert the status and `strstr(log_text, msg)`):

```c
static void test_parse_written_file(void);
    /* "# t seed 7 cycles 2\n# clock clk 0\n# input a 1\n# input b 1\n# output y 1\n0 10 0\n1 01 1\n"
       -> OK, cycles 2, seed 7, n_bits 2, bits {1,0,0,1} */
static void test_parse_bare_lines_crlf_comments(void);
    /* "\n0 11\r\n# note\n\t\n1 00 x\n" -> OK, cycles 2, seed 0, bits {1,1,0,0} */
static void test_parse_errors(void);
    /* each PARSE with exact message:
       "0 1\n"       -> "v.vec:1: expected 2 input bits of 0 and 1, found `1`"
       "0 1x\n"      -> "v.vec:1: expected 2 input bits of 0 and 1, found `1x`"
       "0 -\n"       -> "v.vec:1: expected 2 input bits of 0 and 1, found `-`"
       "1 11\n"      -> "v.vec:1: expected cycle 0, found `1`"
       "0 11 0 9\n"  -> "v.vec:1: expected `<cycle> <inputs> [<outputs>]`"
       "# input a 2\n0 11\n" -> "v.vec:1: port line `# input a 2` does not match the design: expected `# clock clk 0`"
       "# clock clk 0\n0 11\n" -> "v.vec:1: the header lists 1 port lines, the design has 4"
       "# t seed 1 cycles 3\n0 11\n" -> "v.vec:2: the title says 3 cycles, the file has 1" */
static void test_parse_no_input_design(void);
    /* BLIF ".model c\n.outputs y\n.names y\n1\n.end\n" written to a temp file and read:
       "0 1\n" -> "v.vec:1: expected `-`: the design has no non-clock input bit"; "0 -\n" -> OK, n_bits 0 */
static void test_read_missing_file(void);
    /* odin3_sim_vectors_read(sim, "/nonexistent/x.vec", &in) == ODIN3_ERR_IO;
       log contains "sim: cannot read `/nonexistent/x.vec`" */
static void test_drive_then_line_round_trips(void);
    /* traffic lowered: 16 lines from odin3_sim_vectors_step (seed 5) into text A with the header;
       parse A; reset the sim; for each cycle drive, odin3_sim_cycle, line -> text B (with the same
       header); strcmp(A, B) == 0 */
```
- [ ] **Step 2: Run** `ctest --preset debug -R '^test_sim_vectors_in$'`; expect compile failure.
- [ ] **Step 3: Implement** (`line` and the refactor in `vectors.c`; the reader in `vectors_in.c`, helpers per line kind).
- [ ] **Step 4: Run** `ctest --preset debug -R '^(test_sim_vectors_in|test_pass_outputs|cli_sim|sim_check_fixtures)$'`; pass (`sim_check_fixtures` only where registered).
- [ ] **Step 5: Commit and push** (`feat(sim): read odin3-sim-vectors input files`).

### Task 7: `sim --vectors-in` (S1d)

**Files:**
- Modify: `src/passes/sim_pass.c`, `tests/cli/test_cli.sh` (new case `sim_vectors_in`), `CMakeLists.txt` (add `sim_vectors_in` to the `foreach(_case ...)` list near line 2387), `docs/PASSES.md` (sim row)

**Interfaces:**
- Consumes: Task 6.
- Produces: SV-1 syntax and errors (exact texts), SV-4 output. `sim_args` gains `odin3_strbuf
  vectors_in; odin3_strbuf vcd;` (`vcd` is parsed and validated now and used by Task 9; until then a
  given `--vcd` logs `"sim: --vcd is not available yet"` and fails with `ODIN3_ERR_INVALID_ARG`, a
  branch Task 9 deletes). Help text (exact): `"sim [--vectors-in <file>] [--vcd <file>]
  <cycles> <seed> [<file>|-]: simulate the top with random inputs, or replay --vectors-in; write
  the vectors (odin3-sim-vectors format) or log a summary; --vcd writes a waveform"`. PASSES.md
  row: signature `` `sim [--vectors-in <file>] [--vcd <file>] <cycles> <seed> [<file>\|-]` ``; source
  column adds `src/sim/vectors_in.c`; tests add `test_sim_vectors_in`, `cli_sim_vectors_in`; notes
  add one sentence each for SV-1, SV-2 (header optional, outputs ignored) and SV-4.

- [ ] **Step 1: Failing CLI case** in `tests/cli/test_cli.sh` (fixtures `$fixtures/../../sim`):

```bash
sim_vectors_in)
    tv=$fixtures/../../sim/traffic.v
    pre="read_verilog $tv; proc; opt; lower"
    "$odin3" -p "$pre; sim 32 5 $work/a.vec; sim --vectors-in $work/a.vec $work/b.vec" \
        2>"$work/err" || fail "replay failed"
    cmp -s "$work/a.vec" "$work/b.vec" || fail "replay differs from the random run"
    "$odin3" -p "$pre; sim --vectors-in $work/a.vec" 2>"$work/err" || fail "summary run failed"
    grep -qF "sim: traffic: 32 cycles from $work/a.vec; last cycle: $(tail -1 "$work/a.vec")" \
        "$work/err" || fail "no replay summary"
    printf '0 101\n1 0x0\n' >"$work/bad.vec"
    expect_fail "bad.vec:2: expected 3 input bits of 0 and 1, found \`0x0\`" \
        -p "$pre; sim --vectors-in $work/bad.vec $work/out.vec"
    [[ ! -e $work/out.vec ]] || fail "output written despite a parse error"
    expect_fail "sim: --vectors-in replaces <cycles> <seed>" \
        -p "$pre; sim --vectors-in $work/a.vec 4 1"
    expect_fail "sim: expects <cycles> <seed>" -p "$pre; sim --vectors-in"
    expect_fail "sim: expects <cycles> <seed>" -p "$pre; sim 4 1 --vectors-in $work/a.vec --vectors-in $work/a.vec"
    ;;
```
- [ ] **Step 2: Run** `ctest --preset debug -R '^cli_sim_vectors_in$'`; expect failure.
- [ ] **Step 3: Implement the parser and the replay loop in `sim_pass.c`** (options in any position; one positional loop; the run loop takes either the PRNG or the parsed file; keep every function <= 60 lines).
- [ ] **Step 4: Run** `ctest --preset debug -R '^(cli_sim|cli_sim_vectors_in|test_pass_outputs|cli_help_runs_nothing|cli_usage|passes_ref)'`; pass (the old `cli_sim` usage text check still matches "sim: expects <cycles> <seed>").
- [ ] **Step 5: Full ctest, `tools/lint.sh`; commit and push** (`feat(sim): sim --vectors-in replays a vector file`). Hand S1d to the controller.

### Task 8: the VCD writer (S1e)

**Files:**
- Create: `src/sim/vcd.h`, `src/sim/vcd.c`, `tests/unit/test_sim_vcd.c`
- Modify: `CMakeLists.txt` (`src/sim/vcd.c` in `odin3_core`; `odin3_add_unit_test(test_sim_vcd)` with `ODIN3_SIM_FIXTURES`)

**Interfaces:**
- Consumes: Task 1 (`odin3_sim_settle`, `odin3_sim_clock_edge`), Task 2 (signal table).
- Produces (`src/sim/vcd.h`):

```c
enum { ODIN3_SIM_VCD_PERIOD = 10, ODIN3_SIM_VCD_INPUT_AT = 2, ODIN3_SIM_VCD_RISE_AT = 5,
       ODIN3_SIM_VCD_CODE_MAX = 8 };

typedef struct odin3_sim_vcd {
    const odin3_sim_signals *sig; /* borrowed */
    FILE *out;                    /* borrowed */
    uint8_t *last;                /* last written bit of every listed signal bit, in ID order */
    uint32_t n_last;
} odin3_sim_vcd;

void odin3_sim_vcd_code(uint32_t index, char code[ODIN3_SIM_VCD_CODE_MAX]);
odin3_status odin3_sim_vcd_begin(odin3_sim_vcd *vcd, const odin3_sim_signals *sig, FILE *out);
odin3_status odin3_sim_vcd_sample(odin3_sim_vcd *vcd, uint64_t time);
odin3_status odin3_sim_vcd_cycle(odin3_sim_vcd *vcd, odin3_sim *sim, uint64_t cycle);
void odin3_sim_vcd_free(odin3_sim_vcd *vcd);
```
  - `code`: SW-2 base 94, NUL-terminated (index 0 `"!"`, 93 `"~"`, 94 `"!\""`).
  - `begin`: the SW-2 header (explicit stack over scopes; children found by `parent`), then `#0`,
    `$dumpvars`, every listed signal's value, `$end`. `ODIN3_ERR_NO_MEMORY`; `ODIN3_ERR_IO` when
    `ferror(out)`.
  - `sample`: `#<time>` and the changed signals (SW-3 format) when any changed; nothing otherwise.
  - `cycle`: `settle; sample(10c+2); clock_edge(POS); sample(10c+5); clock_edge(NEG); sample(10c+10)`.

- [ ] **Step 1: Failing tests:**

```c
static void test_codes(void);
    /* 0 -> "!", 1 -> "\"", 93 -> "~", 94 -> "!\"", 95 -> "\"\"", 94*94 -> "!!\"" */
static void test_netnames_one_cycle_exact(void);
    /* netnames.blif; signals; vcd_begin into tmpfile(); write a=1, b=1 (signals_write);
       vcd_cycle(cycle 0); rewind and read; the text equals exactly: */
    "$version odin3 sim $end\n$timescale 1ns $end\n$scope module netnames $end\n"
    "$var wire 1 ! clk $end\n$var wire 1 \" a $end\n$var wire 1 # b $end\n"
    "$var wire 1 $ y $end\n$var wire 1 % n1 $end\n$var wire 1 & q $end\n"
    "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n0!\n0\"\n0#\n0$\n0%\n0&\n$end\n"
    "#2\n1\"\n1#\n1%\n#5\n1!\n1$\n1&\n#10\n0!\n"
static void test_hier_scopes_and_ranges(void);
    /* hier lowered, header only: contains "$scope module hier $end", then
       "$var wire 4 \" a [3:0] $end", "$scope module u0 $end" ... "$upscope $end" twice inside,
       and the u0 block closes before "$scope module u1 $end"; count of "$scope" == count of
       "$upscope" == 3 */
static void test_cycle_equals_sim_cycle(void);
    /* traffic: sim A with vcd (to tmpfile), sim B without; 32 cycles seed 6 random drive;
       A uses odin3_sim_vcd_cycle, B odin3_sim_cycle; output lines (odin3_sim_vectors_line) equal */
```
- [ ] **Step 2: Run** `ctest --preset debug -R '^test_sim_vcd$'`; expect compile failure.
- [ ] **Step 3: Implement `src/sim/vcd.c`.**
- [ ] **Step 4: Run** `ctest --preset debug -R '^(test_sim_vcd|test_sim_signals)$'`; pass.
- [ ] **Step 5: Commit and push** (`feat(sim): VCD writer`).

### Task 9: `sim --vcd`, cross-check, docs (S1e)

**Files:**
- Modify: `src/passes/sim_pass.c` (remove the Task 7 "not available yet" line; build a signal table and a `odin3_sim_vcd` on an `odin3_atomic_file` when `--vcd` is given; the run loop calls `odin3_sim_vcd_cycle` instead of `odin3_sim_cycle`), `tests/cli/test_cli.sh` (case `sim_vcd`), `CMakeLists.txt` (`sim_vcd` in the `foreach` list; CTest `sim_vcd_check` below), `docs/PASSES.md` (sim row: SW-1 to SW-3 sentence, tests `test_sim_vcd`, `cli_sim_vcd`, `sim_vcd_check`), `docs/user/outputs.md` (section `## Waveforms (VCD)`: command, timing of SW-3, which signals, viewing with GTKWave), `src/sim/README.md` (one line: vcd.c)
- Create: `tests/tools/test_sim_vcd.py`

CTest (next to `python_re_round`, outside any ABC condition):
```cmake
add_test(NAME sim_vcd_check
    COMMAND ${CMAKE_COMMAND} -E env "ODIN3_CLI=$<TARGET_FILE:odin3>"
            ${ODIN3_PLUGIN_PYTHON} -m unittest -v tests.tools.test_sim_vcd
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(sim_vcd_check PROPERTIES LABELS tools SKIP_REGULAR_EXPRESSION "skipped '")
```

- [ ] **Step 1: Failing tests.** CLI case:

```bash
sim_vcd)
    hv=$fixtures/../../sim/hier.v
    pre="read_verilog $hv; proc; opt; lower"
    "$odin3" -p "$pre; sim 8 3 $work/a.vec --vcd $work/a.vcd; sim 8 3 $work/b.vec" \
        2>"$work/err" || fail "vcd run failed"
    cmp -s "$work/a.vec" "$work/b.vec" || fail "vectors changed by --vcd"
    head -1 "$work/a.vcd" | grep -qxF '$version odin3 sim $end' || fail "bad first line"
    grep -qxF '$scope module u0 $end' "$work/a.vcd" || fail "no u0 scope"
    grep -qxF '$var wire 4 " a [3:0] $end' "$work/a.vcd" || fail "no a [3:0]"
    "$odin3" -p "$pre; sim 8 3 --vcd $work/c.vcd" 2>"$work/err" || fail "second vcd run failed"
    cmp -s "$work/a.vcd" "$work/c.vcd" || fail "vcd not deterministic"
    expect_fail "sim: --vcd needs a file name, not \`-\`" -p "$pre; sim 8 3 --vcd -"
    ;;
```
`tests/tools/test_sim_vcd.py` (stdlib; skips when `ODIN3_CLI` is unset; a 40-line VCD reader in the
test: header vars by code, then `#t` blocks into `{time: {code: bits}}` with carry-over):

```python
def test_vcd_matches_vectors(self) -> None:
    # traffic, `sim 24 8 v.vec --vcd w.vcd`: for every cycle c, light_ns and light_ew at time
    # 10c+10 (MSB first) equal line c's outputs field; clk is 0 at 10c+2, 1 at 10c+5, 0 at 10c+10
def test_vcd_matches_icarus(self) -> None:
    # skipUnless shutil.which("iverilog") and shutil.which("vvp");
    # in.vec: line 0 "0 100" (rst 1), lines 1..23 "c 0xy" with x, y from random.Random(8);
    # run `sim --vectors-in in.vec v.vec --vcd w.vcd` (rst first, so Icarus has no x after 10);
    # testbench generated from in.vec: `timescale 1ns/1ns; inputs set at 10c+2 from line c,
    # clk = 1 at 10c+5, 0 at 10c+10; $dumpfile("ref.vcd"); $dumpvars(0, dut); #(10*24+1) $finish;
    # compile with tests/sim/traffic.v; light_ns, light_ew at 10c+10 equal for c = 0..23
```
- [ ] **Step 2: Run** `ctest --preset debug -R '^(cli_sim_vcd|sim_vcd_check)$'`; expect failure.
- [ ] **Step 3: Implement** the pass changes and docs.
- [ ] **Step 4: Run** `ctest --preset debug -R '^(cli_sim|cli_sim_vectors_in|cli_sim_vcd|sim_vcd_check|test_sim_vcd|test_pass_outputs|passes_ref)'`; pass. `sphinx-build -W -b html docs/user build/docs` succeeds.
- [ ] **Step 5: Full ctest, `tools/lint.sh`; commit and push** (`feat(sim): sim --vcd writes a waveform`). Hand S1e to the controller.

## Self-review record

Spec coverage: SC-1 T1; SC-2..4 T1, T4; SA-1..12 T3, T4; SA-4/5 T2; SP-1..5 T5; SV-1..4 T6, T7;
SW-1..4 T8, T9; section 10 trace T4, T5. Review Focus lines 1 to 5 map to `test_script_refused_while_held`,
`test_gc_order_is_safe`, `test_write_rules` / `test_mixed_clock_port`, Task 6 errors and the Task 7
"no output on error" check, `test_flattened_names` / `test_find`.
