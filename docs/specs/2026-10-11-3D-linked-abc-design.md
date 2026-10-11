# 3D — Linked ABC: design

Status: draft v1 (2026-10-11). Peter reviews the IR and ABI pieces: the `$lut` cell (§2), the
IR-12 amendment (§8.5), the engine ABI (§4.3) and the adapter build (§4.1–4.2). The rest has light
review. Phase 3, sub-project 3D; model Opus `xhigh` → `high`.

This version includes the three requirements of the 3E/3H review:

1. Hard-block inputs driven by `unconn` or x pass through `abc` unchanged (§5.2).
2. The exchange follows P3-9, P3-12 and P3-13 (§6, with the one justified deviation stated in
   §6.3).
3. 3D owns the `$lut` interface, defined in §2 and landed first by the small PR 3D-1, so that 3E's
   ISOP/`$lut` PR can follow immediately.

Inputs:

- PHASE3 decisions 1 and 8–14, approved by Peter 2026-10-11 (cited as **P3-n**), plus P3-10 and
  P3-16 to P3-18;
- DESIGN §4.0, §5.2, §6, §15;
- IR.md IR-4, IR-6, IR-8 to IR-13 and IR-15 to IR-17;
- the op registry;
- the 1C, 1D, 1E and 1G specs;
- 2E §16 (the byte budget and OOM injection);
- the 2B project reader.

Oracles:

- ABC as pinned in ORACLES.md: `third_party/abc` at `a3001b72edc5de22442e942165487fddf150e3d0`
  (2026-10-05). Every claim about ABC below was read in that tree.
- VTR `3c9a4d23`'s ABC stage: `vtr_flow/scripts/python_libs/vtr/abc/abc.py`,
  `vtr_flow/scripts/blackbox_latches.pl` and `abc/abc.rc`.

Decisions are numbered **AB-n** and the PRs are 3D-0 … 3D-8 (§15). Where this file and IR.md
disagree, IR.md wins, except for the two IR changes proposed here.

Two findings reword approved decisions without changing their substance (§18):

- P3-8's adapter "registers the `abc` pass". Here the pass is in the core and the adapter
  registers an **ABC engine** (AB-7).
- P3-9 (a) names `Abc_FrameGiaOutputMiniLutObj` as ABC's object correspondence. At the pin that
  call returns the timing manager's box map. The per-LUT correspondence is
  `Abc_FrameReadMiniLutNameMapping` (AB-16).

## 1. Purpose and scope (AB-1)

`abc` takes each module in the netlist view (after `lower`, or as `read_blif` leaves it). It maps
the soft logic to K-input LUTs with the pinned ABC and re-stitches the result into the IR with
exact provenance and byte-identical reruns. ABC's `exit()` calls and global symbols stay outside
`libodin3` (P3-8).

| Delivered by 3D | Where |
|---|---|
| `libabc-pic` from the submodule; the adapter `libodin3_abc.so`; its symbol checks | `CMakeLists.txt`, `adapters/abc/`, `tools/check-symbols.sh` |
| The `$lut` interface and registry cell, first and alone (3D-1); `$lut` in the Verilog and JSON writers | `src/ir/cells/lut.c`, `src/backends/{verilog,json}` |
| Engine ABI and `ODIN3_ERR_ENGINE`; pass-run arguments and run information (IR-12) with accessors | `odin3.h`, `src/api/`, `src/ir/prov.[ch]`, `src/passes/manager.c` |
| The `abc` pass: arguments, script checker, regions, domains, both exchanges, splice, latch restore, provenance | `src/passes/abc/` |
| Default script; SHA-256 | `lib/abc/vtr.abc` (embedded); `src/util/sha256.[ch]` |
| `abc_script` (`.o3proj`), `--abc-script` (CLI), `dump_project` field, fixtures | `src/frontends/project/`, `src/cli/main.c`, `tools/project-fixtures/`, `tests/golden/projects/` |

Not in 3D:

- `write_blif --vtr`, which is 3E's (it uses the run information);
- the ISOP cover and `$lut`'s BLIF forms: 3E's ISOP/`$lut` PR, right after 3D-1. 3D calls them
  (AB-3);
- the VTR wrapper and K (3F passes `--lut`);
- `lower` and `hardmap` (3B, 3C);
- retiming (P3-12 b);
- ABC as a subprocess (PHASE3 risk 1's fallback; a second engine, AB-7);
- an `equiv` pass.

**Success:**

1. **M1**: with 3E and 3F, `read_blif <Parmys golden>; abc --lut 6; write_blif --vtr` routes all
   19.
2. `equiv-check` before against after `abc` passes on every micro and every single-clock RAM-free
   golden.
3. Byte-identical reruns, across processes and within one.
4. `symbol_rules` and `abi_exports` unchanged and green, with the adapter added to `symbol_rules`.
5. Every new LUT and flip-flop has a `DERIVED` record that reaches its source.
6. §12's targets.

## 2. The `$lut` interface: an IR change owned by 3D (AB-2, AB-3)

LUTs come back from ABC as one new registry cell with Yosys `$lut` semantics. This is DESIGN
§5.2's `$_LUT_K_` (P3-9). 3D owns the interface below.

The cell lands **first and alone** in 3D-1: definition, `verify`, `const_value`, `simulate`, the
IR.md and DESIGN text, and a unit test. 3E's ISOP/`$lut` PR builds on it immediately (AB-3).

**The interface (AB-2)**, the contract that 3E, 1E, the writers and `abc` share:

| Item | Definition |
|---|---|
| name, file | `$lut`; `src/ir/cells/lut.c`, added to `cells/builtin.c` (IR-8, IR-11) |
| granularity, flags | `bit` (netlist view); no flags |
| parameter 0 | `WIDTH`: `ODIN3_VAL_INT`, default 1; valid values 1 to 16 (`ODIN3_LUT_MAX_WIDTH`, `src/ir/cells/cells.h`) |
| parameter 1 | `LUT`: `ODIN3_VAL_BITS`, default `LUT[0]` = 0, `LUT[1]` = 1 (`2'b10`, the identity); valid when it has exactly 2^`WIDTH` bits and each is 0 or 1 (x and z refused) |
| port 0 | `A`: input, vector (`scalar` false), width parameter `WIDTH`; pin *j* of `A` is `A[j]`, LSB first (IR-1) |
| port 1 | `Y`: output, scalar, width 1 |
| function | `Y = LUT[i]`, where `i = Σ_j A[j]·2^j` (`A[0]` least significant) and `LUT[i]` is the parameter's bit *i*, counted from bit 0, the least significant bit of the `odin3_value`, as for every BITS value (IR-10). Yosys `$lut` semantics |
| text forms | Yosys JSON and RTLIL write `LUT` MSB first: the string's first character is `LUT[2^WIDTH−1]`. The BLIF `.names` form has inputs in `A[0] … A[WIDTH−1]` order |
| `const_value` | `ODIN3_CONST_0` when every `LUT` bit is 0; `ODIN3_CONST_1` when every bit is 1; `ODIN3_CONST_NONE` otherwise |
| `simulate` (1E) | `Y = LUT[i]`, *i* from the `A` input bytes; `sim_scratch_bytes` NULL |
| ABC tables | MiniLUT truth variable *k* is fanin *k*, created as `A[k]`; for `WIDTH` ≤ 5, the low 2^`WIDTH` bits of the first 32-bit word (ABC replicates the table above them); otherwise words in increasing order. Bit *i* = `LUT[i]` in both cases |

**The split with 3E (AB-3).** 3E owns the irredundant cover (ISOP) and `$lut`'s BLIF forms, in
both `write_blif` and `write_blif --vtr`. 3D relies on two functions of that PR, whose final names
are 3E's:

- `lut_cover(width, lut) → COVER`: an irredundant, deterministic sum of products, on-set or
  off-set, rows in `A` order.
- `truth_isop(tt, nvars ≤ 16) → cubes`: the same on a raw truth table.

The BLIF exchange uses them to write gates and `$lut`s (AB-15), and the AIG builder to decompose
them (AB-16). 3D adds `$lut` to the Verilog writer (that cover, through the `FORM_SOP` path) and
to the JSON writer (Yosys `$lut`, the MSB-first string), in 3D-3. A BLIF round trip turns a `$lut`
into the `$sop` with that cover.

**IR.md text**, after the "BLIF cells (used by 1C)" paragraph:

> **LUT cells (3D).** `$lut` (`src/ir/cells/lut.c`, granularity `bit`) has parameters `WIDTH`
> (int, 1 to 16, `ODIN3_LUT_MAX_WIDTH`) and `LUT` (bits: exactly 2^`WIDTH` bits, each 0 or 1), and
> ports `A` (in, `WIDTH` bits, LSB first) and `Y` (out, 1 bit). `Y = LUT[i]` with `i = Σ A[j]·2^j`,
> and `LUT[i]` is bit *i* of the parameter counted from its least significant bit: Yosys `$lut`
> semantics (text forms write `LUT` MSB first). Its `const_value` is 0 or 1 when every `LUT` bit
> is, and it simulates (1E). Writers print it as an irredundant cover (BLIF `.names`, a sum of
> products in Verilog; the cover is 3E's) and as Yosys `$lut` in JSON; a BLIF round trip turns it
> into the `$sop` with that cover. `abc` is its only producer in Phase 3; DESIGN §5.2's `$_LUT_K_`
> is this cell.

**DESIGN §5.2**: "`$sop` (BLIF `.names` covers) and `$_LUT_K_`" becomes "`$sop` (BLIF `.names`
covers) and `$lut` (a K-input LUT, Yosys semantics; `docs/IR.md` LUT cells)".

## 3. The pinned ABC, read at the source (AB-4)

| Fact (at `a3001b72`) | Where | Consequence |
|---|---|---|
| `abcapis.h` declares `Abc_Start`/`Abc_Stop` (a lazy global frame), `Cmd_CommandExecute`, the MiniAIG/MiniLUT calls, and the explicit frames `Abc_FrameCreate/Destroy/Enter/Leave/Current` ("strict LIFO order on one thread") | `base/main/abcapis.h`, `mainFrame.c:654` | One fresh frame per run (AB-8) |
| The frame pointer is `ABC_THREAD_LOCAL`; `Cmd_CommandExecute` enters the frame it is given | `mainFrame.c:38`, `cmd/cmdApi.c:193` | Other statics are process-wide |
| Random generators with static state: `Abc_Random`, `Abc_RandomW`, `Aig_ManRandom`, `Aig_ManRandom64`, `Gia_ManRandom`, `Gia_ManRandomW`, `Random_Int`, `Random_Word` | `misc/util/utilSort.c:1012`, `aig/aigUtil.c:1175`, `gia/giaUtil.c:49`, `opt/rar/rewireRng.c` | Reset at every session (AB-8) |
| `Cmd_Init` reads `abc.history` from the working directory, and every command appends to it, unless batch mode or `ABC_NO_HISTORY` | `cmd/cmd.c:93`, `cmdHist.c:65–175` | Both are set (AB-5, AB-8) |
| An unknown one-word command containing `.` runs as `read <word>`; a leading `!` is a shell escape; `CmdSplitLine` splits at `;`, `#` and blanks and strips quotes; aliases exist only through `abc.rc`, which linked ABC never reads | `cmd/cmdUtils.c:96–270` | The checker refuses quotes, `!` and unlisted words and expands aliases itself (AB-21) |
| Debug `libabc.a` imports (objects): `exit` 162, `abort` 555, `__assert_fail` 1,091, `_exit` 4, `system` 14, `popen` 2, `fork` 3, `execv`/`execvp` 1 each, `signal` 1. No `NDEBUG` | `nm -u` | Link-time `--wrap` (AB-6) |
| Printing goes through `printf`/`vprintf`/`puts`/`putchar` (`Abc_Print` ignores the frame's `Out`); `ABC_ALLOC` is an unchecked `malloc`; 16 objects call `pthread_create` | `misc/util/abc_global.h:490` | fd capture (AB-8); a pre-run budget (AB-24); one session at a time |
| `Abc_FrameGiaInputMiniAig` loads the `&` space and keeps `pGiaMiniAig`/`vCopyMiniAig`; `Abc_NtkInputMiniAig` does neither | `gia/giaMini.c:151`, `abci/abcMini.c:175` | Input via `&`, then `&put` (AB-22) |
| `Abc_FrameGiaOutputMiniLut` returns the `&` mapping. CIs come first, then LUTs in topological order, then COs; a CO is constant 0/1 or a node; complemented COs share inverter LUTs; `nRegs` is set | `giaMini.c:410–545` | The result form (AB-14) |
| `Abc_FrameGiaOutputMiniLutObj` is `Gia_ManDeriveBoxMapping`, the timing boxes | `giaMini.c:558`, `giaTim.c:1209` | Not a per-LUT map (P3-9 a, corrected) |
| `Abc_FrameReadMiniLutNameMapping` returns, for each MiniLUT object, the MiniAIG literal ABC proves equivalent (SAT sweeping, 1,000 conflicts) or −1. It runs on `Gia_ManDup2`, which asserts equal CI and CO counts. It is exported but not in `abcapis.h` | `giaMini.c:704`, `gia/giaDup.c:786` | The anchor source, fed an aligned MiniLUT through `Abc_FrameGiaInputMiniLut2` (AB-16) |
| `&get -m` (`Abc_NtkAigToGia`) keeps the mapping and sets `RegNum`; init values are not carried | `abci/abc.c:35112`, `base/abc/abcFunc.c` | In-memory registers enter at init 0 (AB-13); the spike checks (AB-17) |
| DAR (behind `scorr`) complements init-1 registers and sets don't-cares to 0 ("assumed to be 0") | `abci/abcDar.c:134–331` | AB-13 follows ABC's convention |
| 1,247 C and 124 C++ sources; the CMake target `libabc-pic` exists; PUBLIC flags are `-Wall -Wno-unused-function -Wno-write-strings -Wno-sign-compare -DLIN64 -DSIZEOF_VOID_P=8 -DSIZEOF_LONG=8 -DSIZEOF_INT=4 -DABC_USE_CUDD=1 -DABC_USE_PTHREADS` | `CMakeLists.txt`, `make cmake_info` | AB-5 |
| Licence: UC's permissive notice; the one GPL mention, the GipSAT port, is under ABC's licence by the author's permission | `copyright.txt`, `proof/pdr/gipsat/gipMain.c` | AB-9 |

## 4. The adapter boundary

### 4.1 Build (AB-5)

With `ODIN3_WITH_ABC` on (the default), the build makes `libabc-pic` and the adapter.
Without it there is no adapter, and `abc` fails with "not available" (§11).

Inside the existing `odin3_add_abc()` scope:

- `CMAKE_C_FLAGS_DEBUG` and `CMAKE_CXX_FLAGS_DEBUG` are `-g -O2` for ABC only. An unoptimised ABC
  would stretch mcml's run of about an hour into several. Debug keeps ABC's assertions, which the
  guard catches; Release uses `-O2 -DNDEBUG`.
- ABC never gets `odin3_flags` or sanitisers. ASan still intercepts its mallocs, so ABC's known
  leaks are suppressed by function name in `adapters/abc/lsan.supp`, and none of them is the
  adapter's.
- `set_property(TARGET libabc-pic APPEND PROPERTY COMPILE_DEFINITIONS ABC_NO_HISTORY)`.
- **Agent default:** `odin3_abc_exe` is `add_executable(third_party/abc/src/base/main/main.c)`
  linked to `libabc-pic`, with `OUTPUT_NAME abc`, in `${PROJECT_BINARY_DIR}/third_party/abc/`. It
  replaces ABC's own `abc` as the dependency of `abc_tool` and as `ODIN3_ABC`, so ABC compiles
  once per build directory.
- `cmake/abc_pin.cmake` sets `ODIN3_ABC_COMMIT` to `a3001b72…`. When `git -C third_party/abc
  rev-parse HEAD` gives another commit, configure stops with `third_party/abc is at <sha>;
  ORACLES.md pins <pin>`. The adapter gets the pin as `ODIN3_ABC_COMMIT_STR`.

The adapter target is `odin3_abc`: `SHARED`, `OUTPUT_NAME odin3_abc`, `C_VISIBILITY_PRESET hidden`,
linked `PRIVATE` to `libodin3` and `libabc-pic`, through the C++ driver.

- `adapters/abc/bridge.c` is the only file that includes ABC headers (`abc_global.h`,
  `abcapis.h`, `main.h`, `miniaig.h`, `minilut.h`), through `target_include_directories(... SYSTEM
  ...)`. It uses `odin3_flags` minus `-Wstrict-prototypes`, because ABC declares `Abc_Start()`.
- `session.c`, `guard.c`, `capture.c` and `adapter.c` include only `odin3.h` and `bridge.h` and use
  the full `odin3_flags`.
- `tools/lint.sh` covers `adapters/abc/*.c` as it does `src/`.

### 4.2 Symbols and process control (AB-6)

The adapter links with:

```
-Wl,--exclude-libs,ALL -Wl,--version-script=adapters/abc/exports.map -Wl,-z,defs
-Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=abort -Wl,--wrap=__assert_fail
-Wl,--wrap=system -Wl,--wrap=popen -Wl,--wrap=fork -Wl,--wrap=execv -Wl,--wrap=execvp
-Wl,--wrap=signal
```

`exports.map` is `{ global: odin3_plugin_init; odin3_abc_adapter_init; local: *; };`.

`--wrap=sym` resolves every undefined reference to `sym` in the linked objects, including the
members taken from `libabc-pic.a`, to `__wrap_sym`. `guard.c` defines the wrappers and never names a
`__real_` symbol, so **the `.so` imports none of the banned names**.

Wrapping replaces the "own allowance" P3-8 foresaw. `tools/check-symbols.sh` runs **unchanged** on
the adapter, which only adds an argument to `symbol_rules`: the gate becomes stronger.

The wrappers:

- **Escapes:** `__wrap_exit(int)`, `__wrap__exit(int)`, `__wrap_abort(void)` and
  `__wrap___assert_fail(expr, file, line, fn)`.
  - They are `_Noreturn`. Each records its kind, its code, and for an assertion `"<file>:<line>:
    <expr>"` (≤ 255 bytes) in the active guard, then `longjmp`s to it.
  - Every ABC entry point runs inside a guard (AB-8). Outside one, which cannot happen by
    construction, they execute `__builtin_trap()`: PHASE3 risk 1's "a crash kills odin3", and an
    import of nothing.
- **Process control:**
  - `__wrap_system`, `__wrap_fork`, `__wrap_execv` and `__wrap_execvp` return −1;
  - `__wrap_popen` returns NULL;
  - `__wrap_signal` returns `SIG_ERR`.

  All of them set `errno = EPERM` and add `ABC tried to <call>; refused` to the session's output.
  The allow-list makes them unreachable; they are kept as defence in depth.

Checks:

- `symbol_rules` is `check-symbols.sh libodin3 example_plugin odin3_abc`.
- The new `abc_adapter_exports` is `check-symbols.sh --only odin3_plugin_init,odin3_abc_adapter_init
  --forbid exit,_exit,_Exit,quick_exit,abort,system,popen,fork,execv,execvp,signal
  <libodin3_abc.so>`. `--only` and `--forbid` are a new mode: the exported defined functions must
  equal `--only`, and nothing in `--forbid` may be imported.
- `abi_exports` is unchanged.

### 4.3 The engine seam (AB-7)

**Decision.** `abc` is a core built-in (`src/passes/abc/`). The adapter supplies an **engine**: a
table of seven functions that touch ABC and nothing else, registered through the public ABI.

The alternative is P3-8's wording: the adapter registers the pass and writes the IR. 1D's ABI is
read-mostly, so that needs about 30 IR-builder functions, and it moves the largest Phase 3 pass out
of `src/` and out of fault injection. With the seam, every IR write stays inside IR-15's API, under
`check`, lint, ASan and OOM injection, and the adapter stays at about 400 lines. P3-8's intent is
kept: ABC's symbols and `exit()` stay out of `libodin3`, the gate is unchanged, and the shape is the
slang adapter's. The seam also hosts risk 1's subprocess fallback and the tests' ABC-free double.

ABI, a new section of `odin3.h`. There is one version bump for 3D, made by whichever of 3D-2 and
3D-3 merges first.

```c
/* --- the ABC engine (3D) -------------------------------------------------------------------- */
enum { ODIN3_ABC_ENGINE_VERSION = 1 };

typedef enum odin3_abc_fail_kind {
    ODIN3_ABC_FAIL_NONE = 0,
    ODIN3_ABC_FAIL_COMMAND = 1,  /* a command returned non-zero (code = its value) */
    ODIN3_ABC_FAIL_EXIT = 2,     /* ABC called exit(code) or _exit(code) */
    ODIN3_ABC_FAIL_ABORT = 3,    /* ABC called abort() */
    ODIN3_ABC_FAIL_ASSERT = 4,   /* an ABC assertion failed (text = "file:line: expr") */
    ODIN3_ABC_FAIL_RESULT = 5,   /* no network, or a malformed one, where one was asked for */
    ODIN3_ABC_FAIL_DISABLED = 6, /* an earlier escape disabled the engine for this process */
    ODIN3_ABC_FAIL_BUSY = 7      /* a session is already open (one at a time, process-wide) */
} odin3_abc_fail_kind;

typedef struct odin3_abc_fail {
    uint32_t kind; /* odin3_abc_fail_kind */
    int32_t code;
    char text[256]; /* NUL-terminated detail, "" when none */
} odin3_abc_fail;

/* One line of ABC's output (stdout and stderr merged), without its newline; valid during the
 * call only, and called only from inside an engine call. */
typedef void (*odin3_abc_text_fn)(void *user, const char *line);

/* ABC's MiniAIG layout (aig/miniaig/miniaig.h): two literals per object; object 0 is constant
 * 0; a PI is (0x7FFFFFFF, 0x7FFFFFFF), an AND (lit0, lit1) with lit0 < lit1 < 2 * index, a PO
 * (lit, 0x7FFFFFFF); the last n_regs PIs and POs are register outputs and inputs, in order. */
typedef struct odin3_abc_aig {
    const int32_t *objs;
    uint32_t n_objs;
    uint32_t n_regs;
} odin3_abc_aig;

/* ABC's MiniLUT layout (minilut.h): lut_size fanin slots per object (0x7FFFFFFF unused; a PO has
 * its driver in slot 0 and 0x7FFFFFFE in slot 1); objects 0 and 1 are constants, then PIs, LUTs
 * in topological order, POs; (lut_size > 5 ? 1 << (lut_size - 5) : 1) 32-bit truth-table words
 * per object, variable k = fanin k. */
typedef struct odin3_abc_luts {
    const int32_t *fanins;
    const uint32_t *truths;
    uint32_t n_objs;
    uint32_t n_regs;
    uint32_t lut_size;
} odin3_abc_luts;

/*
 * An ABC engine; one session per ABC run (§4.4). All but close return ODIN3_OK, ODIN3_ERR_ENGINE
 * (*fail filled; any call may fail EXIT, ABORT or ASSERT) or ODIN3_ERR_NO_MEMORY.
 * open: fresh ABC state, generators reset, output to text (NULL: dropped). BUSY, DISABLED.
 * load_aig: aig (copied) becomes the & space's network and the name map's reference. RESULT.
 * exec: one already-checked command line, run as given. COMMAND.
 * get_luts: the & space's mapping, owned by the session until close or the next call. RESULT.
 * name_map: aligned (the loaded AIG's PI, PO, register counts) as ABC's LUT side; *lits gets one
 *   MiniAIG literal or -1 per object, valid until close. RESULT.
 * close: frees all the session returned; after an escape, leaks ABC's state and disables.
 */
typedef struct odin3_abc_engine {
    uint32_t version;       /* ODIN3_ABC_ENGINE_VERSION */
    const char *name;       /* "linked"; a test double names itself */
    const char *abc_commit; /* 40 lower-case hex digits */
    void *self;
    odin3_status (*open)(void *self, odin3_abc_text_fn text, void *user, odin3_abc_fail *fail);
    odin3_status (*load_aig)(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail);
    odin3_status (*exec)(void *self, const char *command, odin3_abc_fail *fail);
    odin3_status (*get_luts)(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail);
    odin3_status (*name_map)(void *self, const odin3_abc_luts *aligned, const int32_t **lits,
                             odin3_abc_fail *fail);
    void (*close)(void *self);
    void *reserved[4]; /* NULL */
} odin3_abc_engine;

/*
 * Makes *engine (copied; self and the strings must outlive the process) the process's ABC engine.
 * ODIN3_ERR_INVALID_ARG (logged) for NULL, another version, a NULL function, a non-NULL reserved
 * slot, an abc_commit that is not 40 lower-case hex digits, or an engine already registered
 * ("abc engine already registered (<name>)"). Process-wide, not thread-safe, final.
 */
odin3_status odin3_abc_engine_register(const odin3_abc_engine *engine);
```

`odin3_status` gains `ODIN3_ERR_ENGINE = 8`, meaning an external engine failed or returned an
invalid result. `ODIN3_STATUS_COUNT` becomes 9.

The adapter exports exactly two functions, declared in `adapters/abc/odin3_abc.h`:

- `odin3_status odin3_abc_adapter_init(void)` registers the linked engine.
- `odin3_plugin_init(uint32_t)` checks the ABI version and then calls the first, so `--plugin`
  and Python's `odin3_plugin_load` work too.

`odin3` links the adapter (P3-8). Under the compile definition `ODIN3_HAVE_ABC`, `main.c` calls
`odin3_abc_adapter_init()` before any `--plugin`. A failure is the warning `odin3: linked ABC
unavailable: <status>`.

### 4.4 Sessions (AB-8)

The linked engine (`session.c`, `bridge.c`) has one static session.

1. **open**: `pthread_mutex_trylock` (fail: `BUSY`; a disabled engine: `DISABLED`); then, inside a
   guard, `frame = Abc_FrameCreate()`, `prev = Abc_FrameEnter(frame)`, `Abc_FrameSetBatchMode(1)`,
   `Cmd_CommandExecute(frame, "set check")` (VTR's `abc.rc` setting; it changes no result), and
   every generator of AB-4 called with `(1)` (declared `extern` in `bridge.c`; the list is part of
   the pin). The frame stays entered, so ABC never creates its lazy global frame.
2. **Every call** runs under `guard_run`: `setjmp`; `fflush` stdout and stderr, then `dup` and
   `dup2` fds 1 and 2 onto the session's `tmpfile()`; the ABC call; `fflush` and restore; then the
   new bytes go to `text` line by line (a partial last line waits) and the file is truncated. An
   escape restores the fds first, then disables the engine and fills `fail`. The core never logs
   during a call (IR-17), so only ABC's output is captured.
3. **load_aig** checks a copy (literal ranges, AND order, `n_regs`), then calls
   `Abc_FrameGiaInputMiniAig`. **exec** is `Cmd_CommandExecute`. **get_luts** is
   `Abc_FrameGiaOutputMiniLut`. **name_map** checks the counts, then calls
   `Abc_FrameGiaInputMiniLut2(aligned)` and `Abc_FrameReadMiniLutNameMapping`.
4. **close**: `Abc_FrameLeave(prev)`, then `Abc_FrameDestroy(frame)` unless an escape happened,
   then `fclose` and unlock.

**Why a frame per run.** PHASE3 risk 1 planned one session per process, with
`Abc_FrameDeleteAllNetworks` between runs. That call leaves behind `pGiaMini*`, `vCopyMini*`, the
LUT library `if -K` sets, stored networks and command flags. A fresh frame has none of them, as
determinism needs (AB-26). The spike measures the `Abc_FrameInit` cost; risk 1's plan is the
fallback (AB-17).

### 4.5 Licence (AB-9)

`adapters/abc/NOTICE` reproduces `copyright.txt` and the notices of the third-party code compiled
into `libabc-pic`, found by 3D-2's scan of the licence headers under `third_party/abc/src`. A
licence that cannot be combined stops the PR for Peter.

`libodin3` stays MIT and does not link ABC. `odin3 --version` adds the line `linked ABC <commit>
(adapters/abc/NOTICE)`.

## 5. What ABC sees

### 5.1 Soft cells, registers and cut cells (AB-10)

**Soft** nodes of module *M* are nodes without a non-zero `keep` whose type is one of:

- `$sop` or `$lut`;
- a parameterless bit type with a `simulate` hook, no `SEQ_*`, tristate or `ANYVIEW` flag, at most
  `ODIN3_ABC_GATE_MAX_INPUTS` = 6 input bits and one 1-bit output. Today these are `$_BUF_ $_NOT_
  $_AND_ $_OR_ $_XOR_ $_NAND_ $_NOR_ $_XNOR_ $_MUX_`, and gates 3B adds join automatically. The
  pass runs the 1E hook once per type over all 2^n inputs (input *j* = the *j*-th input pin) and
  caches the truth table. The hook contract (no allocation, logging or failure) makes this safe.

Not soft:

- **constant cells**: the node's `const_value` is 0, 1, x or z. Their nets are literals, except
  for undefined nets (§5.2);
- **registers**: the run's domain's flip-flops (§5.3);
- **cut cells**, everything else: port nodes, hard and black-box cells, instances, level latches,
  other domains' flip-flops, tristate cells, `keep` cells and unknown bit types. A cut cell is never
  changed. Its outputs are ABC inputs (P3-13). Its inputs are ABC outputs when soft logic drives
  them (P3-13). An input on a cut, literal or undefined net is not part of the run and stays as it
  is (§5.2).

`abc` needs the netlist view (IR-9). A word cell gives `abc: module '<m>' is not in the netlist
view: <type> '<cell>'; run lower first` (`ODIN3_ERR_CHECK`, nothing changed). Modules run in
creation order; black-box modules are skipped.

### 5.2 Inputs and outputs (AB-11)

For soft cells *S* and registers *R*:

- **Undefined nets** are driven by `$_CONSTX_` or `$_CONSTZ_`, or are a net named `unconn`
  driven by a zero-input, zero-row `$sop`. The last is VTR's convention (2D §14.6 item 3): Parmys
  writes `.names unconn` with no rows, and VPR deletes that net and leaves its pins open. An
  undefined net is **never a literal and never a CO**. Soft cells read it as a CI, a free input that
  ABC cannot tie to 0 or 1. The constant cell driving it is never deleted, and no undefined net is
  ever merged into a constant.

  So a hard-block input on `unconn` or x **passes through `abc` unchanged** (the 3E/3H
  requirement). Read directly, its driver is outside the run. Read through soft logic that reduces
  to it, the CO comes back driven by that CI, and phase B keeps the undefined net (§6.5). The P′
  leg never ties a RAM pin to `gnd`. The cost: where soft logic reads `unconn`, VTR's ABC folds a
  0 and ours cannot (§17 risk 9). Info line: `abc: module '<m>': N soft-logic inputs are undefined
  (x, z or unconn) and stay free`.
- **Literal nets** are driven by the other constant cells: the literal 0 or 1.
- **CIs** are the nets a data pin of *S* ∪ *R* reads (a soft input, or a register's `D`) whose
  driver is outside *S* ∪ *R* and which are not literal nets. That covers undefined nets, undriven
  nets (free inputs too) and multi-driver nets. **Register outputs** are the `Q` nets of *R*.
- **COs** are the nets driven by *S* ∪ *R* that satisfy any of:
  - a cut cell's pin reads it (this covers `$port_out`/`$port_inout`);
  - a register's clock pin reads it (a derived clock keeps its net);
  - the net or one of its wires has a non-zero `keep`. As in Yosys, `keep` makes an output, and
    soft readers still read the signal.

  **Register inputs** are the `D` nets. A net driven by *S* that nothing reads is dead.
- **Order.** CIs in net-ID order, registers in node-ID order, COs in net-ID order (IR-16). One
  sweep in ID order builds all three, in O(nodes + pins).
- **Loops.** Kahn's algorithm sorts *S*, breaking ties by node ID. When it stalls, the remaining
  cell with the lowest ID has its output net **cut**: the net becomes a CO (its function kept) and
  a CI (readers see a free input). The warning is `abc: module '<m>': combinational loop through
  <type> '<cell>'; its output <net> is a cut point`. The loop survives the splice, structurally and
  functionally.

### 5.3 Clock domains: one run each (AB-12)

- **Key.** `$_DFF_P_`/`$_DFF_N_` give (clock net ID, edge); `$_FF_` gives the global domain.
- **Order.** By the clock net's name bytes (own name, else primary wire bit, else empty), then P
  before N, then net ID; the global domain last. VTR sorts `latch|<type>|<clk>|<init>`, which gives
  the same order when the latches share one init value, as in all 19 (P3-12).
- **Runs.** One run per domain, in that order. A run sees every soft cell of *M*, including the
  `$lut`s that earlier runs made, plus that domain's flip-flops; other domains' flip-flops are cut
  cells. A module without flip-flops gets one run. This is VTR's `iterative_bb` (P3-13 a), with the
  IR re-stitching, so `restore_outputs_from_blif` and `CLOCK_GATING` are not needed.
- **Stability.** Domains are computed once per module, before the first run. Later runs see run
  *k*'s new flip-flops as cut cells. Nothing is compacted between runs, so held IDs stay valid
  (IR-6).
- **Cap.** At most `ODIN3_ABC_MAX_DOMAINS` = 64 per module; `stereovision3` is the 19's only
  two-clock design.

### 5.4 Latches and init values (AB-13)

| `INIT` | In-memory (MiniAIG has no inits) | BLIF exchange |
|---|---|---|
| 0 | as is | `.latch D Q 0` |
| 1 | **inverted**: ABC holds ¬Q, gets ¬D, init 0 (P3-12) | `.latch D Q 1` (DAR complements it the same way) |
| 2, 3 | 0 | `.latch D Q 2` (ABC: "assumed to be 0"; its reader refuses 3) |

Retiming is refused (AB-21), and ABC never adds registers: more registers out than in is a
`RESULT` failure. `scorr` may merge registers, or replace one with a constant or with logic.

- **Exact correspondence.** New register *j* corresponds exactly to input register *k* when, in
  every reachable state, they hold the same value in ABC's encoding. In-memory, *k*'s observation
  output (AB-16) is driven by *j*'s output; in the BLIF exchange, the latch is named `o3q<k>`. All
  registers start at 0 in that encoding, so an exact correspondence is never complemented.
- **Representative** *k\**: the lowest-indexed input register that corresponds to *j*. A *j*
  without one is a `RESULT` failure.
- **The new flip-flop.**
  - Type and clock are the domain's.
  - `INIT` is 2 when `INIT(k*)` is 2 or 3, and `INIT(k*)` otherwise.
  - Its node name is *k\**'s.
  - *k\**'s old `Q` net merges into the new one (§6.5 phase C).
- **Restore** (in-memory, `INIT(k*)` = 1). *j* holds ¬Q and is turned back without adding LUTs
  where possible:
  - `INIT` 1;
  - every new LUT reading *j* has its truth table flipped in that variable;
  - a CO reading *j* directly gets a 1-input NOT `$lut`, but a CO read through MiniLUT's inverter
    LUT turns that LUT into a buffer, which becomes a merge;
  - a `D` source LUT that feeds only *j* is complemented, and any other `D` source gets a NOT (a
    constant flips).

  This is exact: Q_k\*,ir = ¬Q_j,abc, and *j* restored stores exactly that, with the same register
  count.
- **Other registers merged into *j*** keep their names when not inverted. A complemented one loses
  its names; its readers were replaced or re-stitched anyway.

## 6. Exchange

### 6.1 One result form (AB-14)

Both exchanges give the splice and provenance code the same `abc_result` (`result.[ch]`), numbered
like MiniLUT:

- 0 and 1 are the constants;
- then the CIs as sent: real inputs, then register outputs (*n_regs_out*);
- then the LUTs in topological order, each with up to 16 lower-numbered fanins and a truth table.

The COs (the real outputs; then, in-memory, *n_regs_in* observation outputs; then *n_regs_out*
register inputs) each have a driver object. An optional array maps each object to an old AIG
literal or −1 (§8.2).

The result is checked: real I/O counts as sent, *n_regs_out* ≤ *n_regs_in*, fanins precede, at most
16 inputs per LUT and at most K with `--lut K`. A failed check is `abc: ABC returned a malformed
network: <why>` (`ODIN3_ERR_ENGINE`, §11). Building and checking cost O(objects + fanins).

### 6.2 BLIF exchange, `--via-blif` (AB-15)

This is P3-9 (b): proven code for bring-up and, afterwards, the differential and debug path.
Until 3D-7 lands it is also the default, in a `mkdtemp` directory `odin3-abc-XXXXXX` under
`$TMPDIR`, else `/tmp`, which the pass removes afterwards.

- **Directory.** `<dir>` is created when missing; files are overwritten. An absolute path with
  `"`, `'`, a newline or NUL is refused, because ABC cannot quote it.
- **Files of run *r*** (numbered from 0 across the invocation): `abc<r>.in.blif`, `abc<r>.cmd`
  (the commands as run), `abc<r>.log` and `abc<r>.out.blif`.
- **Input.** `.model o3abc`, `.inputs`, `.outputs`, then the cells in topological order, the
  `.latch`es, `.end`. Each net has one exchange name:

  | Net | Name |
  |---|---|
  | CI *i* | `o3ci<i>` |
  | register output *j* | `o3q<j>` |
  | CO *i* | `o3co<i>` |
  | constants | `o3c0` (no rows), `o3c1` (row `1`) |
  | other | `o3n<net ID>` |

  - A `$sop` keeps its rows; gates and `$lut` write 3E's cover (AB-3).
  - A register is `.latch <D name> o3q<j> <init>`, with no type or clock (ABC ignores both).
  - A CO that is also a register output gets the buffer `.names o3q<j> o3co<i>` / `1 1`.

  Writing costs O(region pins + rows).
- **Commands** (AB-22): `read "<abs in>"`, the script, `&put` if the script ends in the `&`
  space, then `write_hie "<abs in>" "<abs out>"`. These are VTR's stage's commands (P3-9 b), and
  for a flat model `write_hie` writes the mapped network. Under this exchange the `&` space starts
  empty, so the checker refuses an `&` command, other than `&get`, before the first `&get` (AB-21,
  step 11).
- **Undefined CIs** (§5.2) are ordinary `.inputs`. `unconn` and x never enter the file as
  constants, so ABC cannot tie them.
- **Read-back.** 1C's reader reads the output into a scratch design (`odin3_design_create`, then
  destroyed).
  - The inputs and outputs must be exactly the `o3ci*`/`o3co*` names sent, in any order.
  - Latches are identified by their outputs `o3q<k>`.
  - `$sop` becomes a truth table, evaluated from its cover; more than 16 inputs is `RESULT`.
  - A one-input `1 1` `.names` is a buffer and becomes a connection; a `.subckt` is `RESULT`.
  - Kahn's algorithm (ties by node ID) orders the result.
  - ABC's latch inits are ignored; AB-13 decides.

  Read-back costs O(pins + rows · 2^k/64).
- **Anchors**: CIs and COs by name, registers by `o3q<k>` (§8.2).

### 6.3 In-memory exchange: MiniAIG in, MiniLUT out (AB-16)

This is P3-9 (a), the production path: no files, no 100 MB BLIF parse, and the name map. It lands
in 3D-7, after the spike says go (AB-17).

**AIG** (`aig.c`), in MiniAIG's layout:

- **Objects.** Object 0; one PI per CI; one PI per register output; the ANDs in topological order;
  then the POs: the COs, then one **observation output** per register (driven by that register's
  PI literal, not complemented), then the register inputs.
- **Literals.**
  - A CI gets its PI; a register `Q` gets its PI, complemented when inverted (AB-13); a literal net
    gets 0 or 1.
  - A soft output is built from its input literals.
    - Gates and `$lut` use the ISOP cover of their truth table (an off-set cover complements the
      output); `$sop` uses its rows.
    - Each cube is a balanced AND over its literals in input order.
    - The cover is a balanced OR (an AND of complements) over its cubes in row order. "Balanced"
      means pairwise, left to right, level by level, as `Mini_AigAndMulti` does.
  - A register input gets `D`'s literal, complemented when inverted.
- **Hashing.** A `u64map` keyed by `lit0 << 32 | lit1` (`lit0 < lit1`) applies a·0 = 0, a·1 = a,
  a·a = a and a·¬a = 0. Every AND records its **owner**, the first soft cell whose decomposition
  made it. Every soft output net records its literal.
- **Cost.** O(Σ cover literals) expected. Above `ODIN3_ABC_MAX_AIG_OBJS` = 2^26 objects (an int32
  array of 512 MB) the run fails; MiniAIG's own ceiling is 2^29.

**Session** (AB-22): `open`; `load_aig`; `&put`; the script; `&get -m` if the script ends in the
network space; `get_luts`; `name_map` (fine provenance only); `close`. The MiniLUT becomes an
`abc_result`, whose outputs include the observation outputs.

*Consistency with P3-9, P3-12, P3-13.* P3-9 (a) names `Abc_NtkInputMiniAig`. Entering through
`Abc_FrameGiaInputMiniAig` and then `&put` puts the same AIG into the network space, where VTR's
script runs unchanged, and it keeps the `pGiaMiniAig` reference the name map needs (AB-4). Spike
criterion 7 checks that both entries give identical MiniLUTs; otherwise `Abc_NtkInputMiniAig` is
used, with cone provenance. P3-12 holds as written (init-1 inverted, don't-cares 0, written back
as 2), and so does P3-13 (§5: cut points, one run per clock net and edge).

**Register correspondence.** ABC keeps the PO count and order, which the spike checks.
Observation output *k* tells what input register *k* became:

- new register *j*'s output: an exact correspondence;
- a constant: *k* was constant;
- a LUT: *k* merged into logic, and the LUT anchors *k*'s old `Q` net.

This uses only the public API. Its cost: a register that becomes unobservable during the run stays
alive with its `D` cone. Phase D removes them, so the FF count is right, but the mapping near that
logic can differ (§17 risk 3). The fallback, reading latch names from ABC's internal `gia.h`
(`&get -m -n`, `vNamesIn`), is AB-17's.

**The name map.** `Abc_FrameReadMiniLutNameMapping` needs equal CI and CO counts, and `scorr`
changes the register count. So the core builds an **aligned MiniLUT** with the AIG's interface:

- the PIs as sent, and one register output per *input* register *k*;
- the new LUTs, with each fanin from new register *j* remapped to *k\**(*j*);
- the real and observation outputs as in the result;
- register inputs: *j*'s `D` driver for *k* with an exact correspondence to *j*, constant 0 for any
  other *k*.

`name_map` then gives, per aligned object, the MiniAIG literal ABC proved equivalent, or −1.

- It is skipped above `ODIN3_ABC_NAMEMAP_MAX_OBJS` = 2^23 AIG objects and under `--prov cone`.
- A `RESULT` or `COMMAND` failure there is not fatal: provenance falls back to cones.

### 6.4 The spike gate (AB-17)

3D-0 is a throwaway program in `~/odin3-ws/work/spike-abc/`, linked to `libabc-pic`. Its findings
go into PHASE3. It runs on `ch_intrinsics`, `stereovision3`, `diffeq2`, `diffeq1` and `mkPktMerge`,
smallest first, and on two micros (small inputs, no heavy run). 3D-7 goes ahead only if all of
these hold:

1. **`&get -m` keeps latches and mapping** (P3-9). `Abc_FrameGiaInputMiniAig; &put; <default
   script>; &get -m; Abc_FrameGiaOutputMiniLut` gives the input's PI and PO counts, at most its
   registers and K-bounded LUTs, and is equivalent (`dsec` on BLIF).
2. **PO order is preserved**: the observation outputs identify the same registers as `&get -m -n`
   with `vNamesIn` (read in the spike only).
3. **The name map works** on the aligned MiniLUT. Coverage (anchored LUTs over LUTs) is recorded,
   and its time must be ≤ 15% of the script's.
4. **Frames are clean**: 1,000 create/destroy cycles with no LSan growth and identical results. The
   per-frame cost is recorded.
5. **The guard works**: a test command calling `exit(3)` returns through it, with fds restored.
6. **Determinism**: two processes, and two frames in one process, give identical MiniLUTs, so the
   generator list is complete.
7. **The two entries agree**: `Abc_NtkInputMiniAig` and `Abc_FrameGiaInputMiniAig` + `&put` give
   identical MiniLUTs.

Fallbacks:

- 1 or 2 fails: the output side reads the mapped `Abc_Ntk_t` through `base/abc/abc.h` (P3-9 c,
  output only; for Peter).
- 3 fails: cone provenance only.
- 4 fails: one frame per process, `Abc_FrameDeleteAllNetworks` between runs, and determinism
  checked again.
- 7 fails: `Abc_NtkInputMiniAig` (P3-9 a as written), with cone provenance.

### 6.5 Splice (AB-18)

Records come first, because creating an object needs its provenance ID (IR-13).

**Phase A** (allocates; changes no existing object):

1. Compute anchors, regions and records (§8), and apply the restore flips (AB-13).
2. Create fresh nets for the LUT outputs and the new `Q`s.
3. Create the `$lut` nodes. Their `A` pins go to CI nets, new nets, or the canonical
   `$_CONST0_`/`$_CONST1_` net: the lowest-ID net driven by a constant 0 or 1 cell that is not
   undefined (§5.2), or a new one, as 2F does. A CO that ABC drives with an undefined CI merges
   into that undefined net (phase B), so a hard pin on `unconn` stays on `unconn`.
4. Create the new flip-flops: `C` the domain's clock, `D` the new driver, `INIT` as in AB-13.

`NO_MEMORY` deletes what phase A created, in reverse order (records are immutable and stay), and
the IR is as before.

**Phase B** (re-stitch; every step is atomic and keeps the function). For each CO *c* in order,
with new driver *d*: first disconnect *c*'s old driver pin, then merge.

| *d* | Merge |
|---|---|
| a fresh net (LUT or `Q`) | `merge(keep = c, drop = d)` |
| already merged into an earlier CO *c′* | `merge(keep = c′, drop = c)` |
| a CI or constant net | `merge(keep = d, drop = c)` |

Names survive as aliases (IR-2). If a merge fails with `NO_MEMORY`, the old driver pin goes back on
*c*. That needs no allocation, since *c* just lost a pin. Unvisited COs still have their old
drivers, so the IR stays valid and equivalent. Observation outputs are never stitched.

**Phase C.**

1. Delete the old soft cells and the domain's old flip-flops (slots stay until `compact`).
2. In old-net-ID order, merge each old internal net that has an uncomplemented anchor into the net
   its anchor's object now drives, so names and wire memberships survive.
3. Merge each *k\**'s old `Q` net into the new one, unless phase B already did.

`NO_MEMORY` here leaves the function complete with fewer names moved.

**Phase D** (cleanup, no allocation):

- a worklist removes new LUTs and flip-flops with no readers (observation-only registers and their
  private cones);
- then non-port nets with no pins, no wire membership and no aliases;
- then non-port wires whose nets all have no pins.

A wire with mixed live and pinless bits keeps them, and `check` warns, as after 2F's `opt_clean`.

**Cost:** O(new objects + their pins + moved pins + old cells).

A failure in run *k* leaves the runs before *k* applied, and the IR valid and equivalent. The pass
manager compacts after `abc` (IR-6's "after each ABC read-back"), using 2F-5's hook for `opt`.

## 7. The `abc` pass

### 7.1 Arguments (AB-19)

```
abc [--script <file>] [--lut <K>] [--via-blif <dir>] [--log <file>] [--prov fine|cone]
```

The help line, exactly: `abc [--script FILE] [--lut K] [--via-blif DIR] [--log FILE] [--prov
fine|cone]: map each module's soft logic to K-input LUTs with linked ABC (one run per clock
domain)`.

Arguments are split by `odin3_pass_arg_next`. Each option may be given once. Paths resolve against
the working directory.

| Option | Meaning | Errors |
|---|---|---|
| `--script <file>` | this run's script (AB-20) | no value; unreadable (`ODIN3_ERR_IO`) |
| `--lut <K>` | replaces `{K}`; every result LUT must have ≤ K inputs | not an integer from 2 to 16 |
| `--via-blif <dir>` | BLIF exchange in `<dir>` (AB-15) | a quote or newline in the path; cannot create (`IO`) |
| `--log <file>` | ABC's raw output, truncated at pass start, each run headed `# abc run <r>: module '<m>', <domain>` (agent default) | cannot open (`IO`) |
| `--prov fine\|cone` | `cone` skips the name map; the default is `fine` (agent default) | another value |

`abc` is in the built-in table (`src/passes/builtin.c`) with `wants_ast` false. PASSES.md gets this
row:

| Pass | Phase | Input view → output view | Source | Golden test | Notes |
|---|---|---|---|---|---|
| `abc [--script F] [--lut K] [--via-blif D] [--log F] [--prov fine\|cone]` | 3 (3D) | netlist → netlist | `src/passes/abc/*.c`, `adapters/abc/` | `test_abc_*`, `abc_equiv_micro`, `cli_abc` | One ABC run per clock domain per module. Hard cells, black boxes, instances, level latches and other domains' FFs are cut points, and inputs on `unconn`/x pass unchanged. Script: `--script` > `--abc-script` > the project's `abc_script` > `lib/abc/vtr.abc`, checked before ABC starts. Soft cells become `$lut`. Provenance from ABC's correspondence. Compacts after. |

### 7.2 Script precedence (AB-20)

This implements P3-1. The first that applies wins:

1. `abc --script` (this run only).
2. The project's `abc_script`. `read_project` has already replaced it with the CLI's
   `--abc-script`, logging `<entry>: --abc-script <new> replaces the project abc_script <old>`
   when the resolved paths differ.
3. The CLI's `--abc-script` when no project was read (`odin3_pass_options.abc_script`, set by the
   new ABI call `odin3_pass_set_abc_script`).
4. `lib/abc/vtr.abc`, displayed as `builtin:lib/abc/vtr.abc`.

The pass keeps the display path, the resolved path and the SHA-256 of the bytes read. Its first
line: `abc: script <display> (sha256 <16 hex>…), K=<K|none>, exchange <aig|blif>, ABC <12 hex>`.

### 7.3 Script language and checker (AB-21)

ABC's syntax: `;` or a newline between commands, `#` to the end of the line, and one substitution,
`{K}` (P3-1). The whole script is checked once per invocation, before ABC starts, by `script.c`,
pure C with no ABC. Every error is `ODIN3_ERR_PARSE`, located `<display>:<line>`, and nothing
changes.

1. **Bytes.**
   - `<file>: abc script is larger than 65536 bytes`.
   - LF or CRLF.
   - `<file>:<line>: abc script contains a NUL byte`.
   - Outside comments, printable ASCII and tab only: `<file>:<line>: byte 0x<hh> is not allowed
     outside a comment`.
   - `<file>:<line>: line longer than 4096 bytes`.
2. **Comments.** `#` drops the rest of its line, as `CmdSplitLine` does.
3. **Quotes and shell.**
   - `"` or `'`: `<file>:<line>: quotes are not allowed in abc scripts`. ABC strips quotes, which
     would let a word carry `;`, `#` or blanks; `echo` takes plain words.
   - A word starting with `!`: `<file>:<line>: '!' (shell escape) is not allowed in abc scripts`.
4. **Commands.** Split at `;` and line ends; blank ones are skipped.
5. **`{K}`.**
   - It becomes K in decimal.
   - Without `--lut`: `<file>:<line>: the script uses {K} but abc was given no --lut`.
   - Any other `{` or `}`: `<file>:<line>: unknown substitution '<text>' (only {K} is defined)`.
6. **Words.** Split at spaces and tabs. More than 64: `<file>:<line>: command has more than 64
   words`.
7. **Aliases** (§7.4).
   - Word 0 naming an alias is replaced by the alias text, with the remaining words appended at its
     end, and the result is re-split at `;`. These are `CmdApplyAlias`'s semantics: extra words go
     to the expansion's last command.
   - Expansion repeats, at most 8 levels: `<file>:<line>: alias '<a>' expands more than 8 levels
     deep`.
   - Each command keeps its line and its outermost alias.
8. **Allow-list** (§7.5). The text is `<file>:<line>: abc command '<cmd>'[ (from alias '<a>')]`
   followed by its class's ending:
   - `is not allowed: abc scripts cannot read or write files`;
   - `is not allowed: abc scripts cannot control the ABC process`;
   - `is not allowed: it retimes or changes initial values (PHASE3 #12)`;
   - `is not an allowed ABC command (3D spec §7.5)`.
9. **Arguments.**
   - Each argument must be an option `-[A-Za-z][A-Za-z0-9]*` or a number
     `[-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?`.
   - `echo` takes any printable word without `{`, `}` or `!`.
   - Else: `<file>:<line>: argument '<w>' of '<cmd>' is neither an option nor a number`. File
     arguments are closed by this rule.
10. **Count.** More than 1,024 after expansion: `<file>:<line>: script expands to more than 1024
    commands`.
11. **Spaces and mapping.** `ntk` (the `Abc_Ntk` space) and `gia` (the `&` space) are each
    *empty*, *aig* or *mapped*.
    - At the start: both *aig* in-memory; under the BLIF exchange `ntk` is *aig* and `gia`
      *empty*.
    - §7.5 gives each command's effect. The current space is that of the last command that changes
      or moves a network, starting at `ntk`.
    - An `&` command other than `&get` on an empty `gia`: `<file>:<line>: '<cmd>' needs a network
      in the & space, which the BLIF exchange starts empty; add '&get'`.
    - The script must end with its current space *mapped*: `<file>: the script leaves the network
      unmapped; end it with a LUT mapper such as 'if -K {K}'`. An empty script ends unmapped.
12. **Output.** An `odin3_abc_script`: the expanded lines (NUL-terminated, each with its line and
    alias), `uses_k`, the final space and the SHA-256.

The cost is O(bytes + expanded words).

**Checked is what runs.** The expanded lines hold no quote, `!`, `;`, `#`, `{`, `}` or alias, so
ABC's `CmdSplitLine` sees the checker's words. §13.2 tests this through a hook in `bridge.c`.

### 7.4 The alias table (copied from `abc.rc`)

`src/passes/abc/alias.c` holds `k_abc_aliases`: every `alias` line of `third_party/abc/abc.rc` at
`a3001b72`, in file order. VTR's `abc.rc` is the same less `&resyn2rs` and `&compress2rs`.
`test_abc_alias_table` parses the submodule's file and asserts equality, so a pin bump fails CI
until the table follows.

**One-word aliases** (→ expansion):

- Usable: `b` balance, `cl` cleanup, `f` fraig, `fs` fraig_sweep, `lp` lutpack, `pcon`
  print_cone, `pd` print_dsd, `pex` print_exdc -d, `pf` print_factor, `pfan` print_fanio, `pg`
  print_gates, `pl` print_level, `plat` print_latch, `pio` print_io, `pk` print_kmap, `pm`
  print_miter, `ps` print_stats, `psb` print_stats -b, `psu` print_supp, `psy` print_symm, `pun`
  print_unate, `rw` rewrite, `rwz` rewrite -z, `rf` refactor, `rfz` refactor -z, `rs` resub, `rsz`
  resub -z, `scl` scleanup, `sif` if -s, `st` strash, `sw` sweep.
- Refused (expansions as in `abc.rc`), files: `r`, `ra`, `rh`, `ri`, `rl`, `rb`, `rp`, `rt`, `rv`,
  `rvl`, `rsup`, `rlib`, `rlibc`, `rty`, `rlut`, `so`, `w`, `wa`, `wb`, `wc`, `wh`, `wl`, `wp`, `wv`.
  Process: `hi`, `q`, `sa`. Latches: `fr`, `r3`, `r3f`, `r3b`, `ret`, `dret`. Other: `cg`, `clp`,
  `cs`, `el`, `esd`, `fsto`, `fres`, `ft`, `ic`, `ren`, `re`, `rez`, `ssw`, `tr0`, `tr1`, `u`.

**Usable script aliases:**

| Alias | Expansion |
|---|---|
| `resyn` | `b; rw; rwz; b; rwz; b` |
| `resyn2` | `b; rw; rf; b; rw; rwz; b; rfz; rwz; b` |
| `resyn2a` | `b; rw; b; rw; rwz; b; rwz; b` |
| `resyn3` | `b; rs; rs -K 6; b; rsz; rsz -K 6; b; rsz -K 5; b` |
| `compress` | `b -l; rw -l; rwz -l; b -l; rwz -l; b -l` |
| `compress2` | `b -l; rw -l; rf -l; b -l; rw -l; rwz -l; b -l; rfz -l; rwz -l; b -l` |
| `rwsat` | `st; rw -l; b -l; rw -l; rf -l` |
| `drwsat2` | `st; drw; b -l; drw; drf; ifraig -C 20; drw; b -l; drw; drf` |
| `src_rw` | `st; rw -l; rwz -l; rwz -l` |
| `src_rs` | `st; rs -K 6 -N 2 -l; rs -K 9 -N 2 -l; rs -K 12 -N 2 -l` |
| `src_rws` | `st; rw -l; rs -K 6 -N 2 -l; rwz -l; rs -K 9 -N 2 -l; rwz -l; rs -K 12 -N 2 -l` |
| `resyn2rs`, `r2rs` | `b; rs -K 6; rw; rs -K 6 -N 2; rf; rs -K 8; b; rs -K 8 -N 2; rw; rs -K 10; rwz; rs -K 10 -N 2; b; rs -K 12; rfz; rs -K 12 -N 2; rwz; b` |
| `compress2rs`, `c2rs` | `b -l; rs -K 6 -l; rw -l; rs -K 6 -N 2 -l; rf -l; rs -K 8 -l; b -l; rs -K 8 -N 2 -l; rw -l; rs -K 10 -l; rwz -l; rs -K 10 -N 2 -l; b -l; rs -K 12 -l; rfz -l; rs -K 12 -N 2 -l; rwz -l; b -l` |
| `&resyn2rs`, `&compress2rs` | `&put; resyn2rs; &get`; `&put; compress2rs; &get` |
| `&sw_` | `&put; sweep; st; &get` |

Refused script aliases, each with the command that stops it:

- `choice`, `choice2`: `fraig_store`;
- `share`: `multi`;
- `addinit`: `read_init`;
- `blif2aig`, `fix_blif`: `undc`;
- `fix_aig`: `logic`;
- `v2p`, `g2p`: `&vta_gla`, `&gla_derive`;
- `&fx_`: `sop`;
- `&dc3`, `&dc4`: `&b`;
- `trt`: `read`;
- `recadd3`: `rec_add3`.

Their expansions are as in `abc.rc`.

### 7.5 The allow-list

This is P3-11 in real command names, each verified at the pin. "Space" and "effect" drive step 11
of AB-21.

| Commands (aliases) | P3-11 group | Space | Effect |
|---|---|---|---|
| `strash` (`st`), `balance` (`b`), `rewrite` (`rw`, `rwz`), `refactor` (`rf`, `rfz`), `resub` (`rs`, `rsz`), `dc2`, `drw`, `drf`, `dch`, `ifraig`, `fraig` (`f`), `fraig_sweep` (`fs`), `scorr` | optimisation | ntk | ntk := aig |
| `sweep` (`sw`), `cleanup` (`cl`), `scleanup` (`scl`) | optimisation | ntk | keeps |
| `if` (`sif`) | mapping | ntk | ntk := mapped |
| `mfs`, `mfs2`, `lutpack` (`lp`) | mapping | ntk | keeps (ABC fails at run time if unmapped) |
| `&get` | `&` space | gia | gia := ntk's state with `-m`, else aig |
| `&put` | `&` space | ntk | ntk := gia's state |
| `&st`, `&synch2`, `&dch` | `&` space | gia | gia := aig |
| `&if` | `&` space | gia | gia := mapped |
| `&mfs` | `&` space | gia | keeps |
| the 32 `print_*` commands at the pin except `print_constr` and `print_status` (which take files); `&ps`; `time`; `echo` | printing | — | none (output logged at INFO) |

Agent defaults: the `print_*` set as listed, and `&ps` as the `&`-space print.

Refused, among others:

- **files:** `read*`, `write*`, `&r`, `&w`, `source`, `load`, `save`;
- **process:** `quit`, `exit`, `cd`, `ls`, `alias`, `unalias`, `set`, `unset`, `history`,
  `load_plugin`;
- **latches:** `retime`, `dretime`, `fretime`, `zero`, `init`, `undc`, `cycle` (P3-12).

The classes are a table in `allow.c`; an unlisted name is "other".

### 7.6 Command protocol (AB-22)

| Exchange | Calls and commands, in order |
|---|---|
| in-memory | `open`; `load_aig`; `&put`; the script; `&get -m` if the final space is ntk; `get_luts`; [`name_map`]; `close` |
| BLIF | `open`; `read "<in>"`; the script; `&put` if the final space is gia; `write_hie "<in>" "<out>"`; `close` (VTR's stage, P3-9 b) |

The core builds the prologue and epilogue commands; scripts never supply them. A failure reads `abc:
internal command '<cmd>' failed`. A script command failure reads P3-1's `abc: <file>:<line>: command
'<cmd>' failed`, with ` (from alias '<a>')` appended when an alias produced it.

### 7.7 The default script (AB-23)

`lib/abc/vtr.abc` is P3-10 (a):

```
# lib/abc/vtr.abc: the default script of Odin III's abc pass (PHASE3 #10).
# The logic part of VTR's ABC stage at VTR 3c9a4d23
# (vtr_flow/scripts/python_libs/vtr/abc/abc.py, run(), "Logic Opt + Techmap"),
# without its file, echo, time and print commands. {K} is abc --lut K.
strash
ifraig -v
scorr -v
dc2 -v
dch -f
if -K {K} -v
mfs2 -v
print_stats
```

The final `print_stats` logs the LUT count and depth, as VTR's stage does.

- CMake embeds the file (`file(READ … HEX)` generates `default_script.c`), so nothing depends on an
  install path. `test_abc_default_script` checks that the hashes match.
- The default uses `{K}`, so without `--lut` it fails at line 10, its `if -K {K} -v`.
- Agent default: no built-in K. Without an architecture no K is right, and 6 would hide a missing
  `--lut`. 3F passes VTR's `determine_lut_size`.

### 7.8 Caps and memory (AB-24)

These constants live in `src/passes/abc/abc.h`. Tests lower them through
`odin3_abc_test_set_limits` (`abc_test.h`).

| Cap | Value | On reaching it |
|---|---|---|
| `ODIN3_ABC_MAX_SCRIPT_BYTES`, `_LINE_BYTES`, `_COMMANDS` | 65,536; 4,096; 1,024 (P3-1) | script error |
| `ODIN3_ABC_MAX_WORDS`, `_ALIAS_DEPTH` | 64; 8 (the table's deepest is 3) | script error |
| `--lut` | 2 to 16 (`ODIN3_LUT_MAX_WIDTH`, MiniLUT's limit) | argument error |
| `ODIN3_ABC_MAX_DOMAINS` | 64 per module | `abc: module '<m>' has N clock domains; at most 64` (`PARSE`) |
| `ODIN3_ABC_MAX_AIG_OBJS` | 2^26 | `abc: module '<m>', <domain>: the AIG needs more than 67108864 objects` (`PARSE`) |
| `ODIN3_ABC_NAMEMAP_MAX_OBJS` | 2^23 | name map skipped (INFO) |
| `ODIN3_ABC_GATE_MAX_INPUTS` | 6 | wider gate types are cut cells |
| provenance edge budget | 16 × (old cells + new objects) + 65,536 per run | one record per run (WARN, §8.3) |
| design byte budget | 60% of physical memory (2E §16) | `abc needs more than N bytes (the design's budget is 60% of physical memory)` (`PARSE`) |

**ABC's memory.** ABC's allocations are unchecked (AB-4), so before each run the pass calls 2E-1's
`odin3_budget_check(ctx, design, extra)`. If 3D-5 lands first, it lands `src/passes/budget.[ch]`
exactly as 2E §16 specifies. The term `extra` is:

- `ODIN3_ABC_BYTES_PER_AIG_OBJ` × AIG objects (512 to start; `bench_abc` calibrates it to 1.5 × the
  worst measured ratio on the 19, and it only goes up);
- plus the exchange arrays;
- plus, with the name map, two more AIGs.

Inside ABC an OOM kills the process, and on WSL the VM: hence the heavy-run wrapper (§13).

**OOM in the core.** Any allocation can return `NO_MEMORY`. The pass then frees what it holds and
returns a valid, equivalent IR (AB-18). Tests inject a failure at every allocation.

## 8. Provenance (AB-25)

### 8.1 The pass-run record

P3-14 makes every run record its arguments. 3D adds a second field, **run information**: facts the
run established. `abc` writes `script=<path> sha256=<hex> abc=<commit> K=<K|none>
exchange=<aig|blif>`; `exchange=` is an agent addition. §8.5 has the exact rules.

```c
/* src/ir/prov.h */
odin3_status odin3_pass_run_begin_args(odin3_design *design, uint32_t pass_name_str,
                                       uint32_t args_str, odin3_pass_ctx *ctx);  /* 0: none */
uint32_t odin3_passrun_args(const odin3_design *design, odin3_passrun_id run);  /* 0: none */
uint32_t odin3_passrun_info(const odin3_design *design, odin3_passrun_id run);  /* 0: none */
/* INVALID_ARG (logged): already set, run 0 or unknown, or info_str 0 or not a strtab ID. */
odin3_status odin3_passrun_set_info(const odin3_pass_ctx *ctx, uint32_t info_str);

/* odin3.h (the AB-7 version bump) */
odin3_status odin3_design_get_passrun_count(const odin3_design *design, uint32_t *count);
/* Name, arguments and run information of run (1 .. count - 1); "" for none. INVALID_ARG for run 0
 * or past the end. Strings as the conventions say. */
odin3_status odin3_design_get_passrun(const odin3_design *design, uint32_t run, const char **name,
                                      const char **args, const char **info);
```

- `odin3_pass_run_begin` keeps its meaning, with args 0.
- The manager's `open_run` interns the trimmed arguments; empty arguments give 0.
- A run's row becomes `{name, args, info}`.
- 3E writes `# odin3 abc: <run information>` once per `abc` run, read through `odin3_passrun_info`
  (P3-1's header comment).

### 8.2 Anchors

An anchor pairs a result object with an old net that computes the same function. There are three
kinds:

1. **Identity.** Each CI and each CO with its own net.
2. **Registers.** Each new register with the old `Q` nets of the registers that correspond to it
   exactly. A LUT that drives an observation output is anchored to that register's `Q`.
3. **Name map** (in-memory, `--prov fine`, within the cap). Each LUT whose aligned object ABC maps
   to a literal ℓ, with the lowest-ID old net *n* such that `lit(n)` is ℓ or ¬ℓ. A complemented
   anchor counts for provenance but never moves names.

Without a name map the info line is `abc: module '<m>', <domain>: no name map (<reason>);
provenance uses cut cones`. The reason is `--prov cone`, `BLIF exchange`, `the AIG has more than
8388608 objects`, or `ABC: <fail text>`.

### 8.3 Regions, records and cost

- **Walks.** Old and new regions are found by iterative walks with an explicit stack and
  per-anchor visit stamps, so no mark array is cleared between anchors.
- **Records.** Each anchor is one `odin3_prov_begin_op` and one `odin3_prov_derive`, whose parents
  are its old region's cells' records in node-ID order. The IR deduplicates them, so the hundred
  gates that lowering one `$add` made add one parent.
- **Which record an object carries.**
  - A LUT in one new region carries that region's record.
  - A LUT in several carries one more operation's record, whose parents are those records in
    anchor order (CO order, then register order, then object order).
  - A new flip-flop carries its register anchor's record.
  - Output nets carry their driver's record.
- **Cost.** O(Σ old regions + Σ new regions + new objects). The name map's anchors keep regions
  small. Identity anchors alone give output cones, P3-14's `--via-blif` rule, which can overlap
  quadratically.
- **Budget.** The edge count is summed before any record is made. Over the budget, the run makes
  one record (parents: all its old cells and flip-flops, in node-ID order) and warns.

### 8.4 Compact and tombstones

Old cells die in phase C. Their slots, then their tombstones after `abc`'s compact, keep them
reachable from the new objects' records (IR-6). An old cell that no anchor reaches had no effect
on any output, and it dies without children.

### 8.5 The IR-12 amendment (exact text for `docs/IR.md`)

**In the IR-12 table**, the `run` row:

> | `run` | the pass run that created it (the pass-run table below: pass name, arguments, run
> information, run number); readers and front ends are runs too |

**After "Why `run` and `op`":**

> **The pass-run table (3D, PHASE3 #14).** One row per run, in run order: the pass name; the
> **arguments**, the text after the pass name as the script gave it, trimmed (a strtab ID; 0 when
> there were none or when a reader opened the run itself); and the **run information**, the facts
> the pass established while running, as blank-separated `key=value` words in an order the pass
> fixes, a value with a blank, `"` or `\` written in double quotes with `\"` and `\\` (a strtab
> ID; 0 = none). The pass manager records the arguments when it opens the run
> (`odin3_pass_run_begin_args`); a pass sets its information at most once
> (`odin3_passrun_set_info`); rows never change afterwards and survive `compact`. `abc` records
> `script=<path> sha256=<64 hex> abc=<40 hex> K=<K or none> exchange=<aig|blif>`: the script it
> ran (its resolved path, `builtin:lib/abc/vtr.abc` for the default), the SHA-256 of the bytes it
> read, the commit of the linked ABC and the LUT size; `write_blif --vtr` repeats it in a header
> comment.
>
> **Provenance across ABC (3D, PHASE3 #14).** `abc` replaces a region of soft cells and flip-flops
> by LUTs and flip-flops that ABC made, and keeps lineage through **anchors**: pairs of a new
> object and an old net that compute the same function. They are every input and output of the run
> (by identity); every new flip-flop with the old flip-flops ABC proved it equal to (sequential
> correspondence); and, with the in-memory exchange, every new LUT whose output ABC's name map
> proves equivalent to an old net, up to complement. An anchor's **old region** is the set of old
> soft cells in the fan-in cone of its old net, up to other anchors' nets and the run's inputs (for
> a flip-flop anchor, its old flip-flops); its **new region** is the set of new LUTs in the fan-in
> cone of its new object, up to other anchored objects and the run's inputs. Each anchor is one
> operation, whose `DERIVED` record has as parents the records of its old region's cells, in
> node-ID order. A new LUT in one new region carries that record; one in several carries the
> record of one more operation, whose parents are those regions' records in anchor order. When a
> run's parent edges would exceed 16 × (old cells + new objects) + 65,536, the run makes one record
> whose parents are all its old cells and flip-flops, every new object carries it, and a warning
> says so. With input and output anchors only (the BLIF exchange, `abc --prov cone`, or no name
> map) a region is the cut-bounded cone of an output. An old cell no anchor reaches had no effect on
> any output; it dies without children.

**In §11** add: "Under PHASE3 #9 and #14 (3D spec): the `$lut` cell, the pass-run table's arguments
and run information, and provenance across ABC (IR-12)."

## 9. Determinism (AB-26)

Two processes, and two runs in one process, write byte-identical netlists from the same input,
script and build.

| Source of variation | Guard |
|---|---|
| IR iteration | ID order only (IR-16); CIs, COs, registers and domains sorted by ID or by name then ID; Kahn ties broken by node ID; no hash-map iteration reaches the AIG, a file or the IR |
| AIG shape | 3E's deterministic cover; balanced trees, left to right; the first owner wins |
| ABC state between runs | a frame per run (AB-8) |
| ABC randomness | every generator reset at `open`; spike criterion 6; §13.8 (b) |
| ABC time limits and threads | none in the default script (the spike checks; a test build counts `pthread_create`); a user script that sets `-T` or threads is documented as not reproducible |
| Splice and provenance | fixed phases; merges in CO order, then old-net-ID order; records in anchor order |
| Debug against Release ABC (assertions on or off) | §13.8 (d) |

## 10. The project grammar amendment (AB-27)

### 10.1 DESIGN text

1. **§4.0, "It holds:"**, after the flow item:

   > - optionally the **ABC script** the `abc` pass runs (§6 step 10), so that soft-logic
   >   optimisation is part of what one file reproduces;

2. **§4.0, the `.o3proj` example list**: add `abc_script vtr.abc` after `flow synth.o3`.
3. **§4.0, *`.o3proj`.*, the `flow` bullet** becomes:

   > - `flow <script.o3>` (once); `abc_script <file>` (once): the ABC script of the `abc` pass, a
   >   path (never inline text), any extension; `import <file.qpf|file.qsf> [revision <name>]` reads
   >   a Quartus project into this one (its top counts as this project's `top`; an imported `.qsf`
   >   cannot set `abc_script`, the importing `.o3proj` can).

4. **§4.0, after *Odin II XML*:**

   > *The ABC script.* Only `.o3proj` names one; a `-f` list has no form for it (`-abc_script` is
   > `unknown_option`), Quartus has no such assignment (an `ABC_SCRIPT` assignment is ignored and
   > listed like any other), Odin II XML never had one. The CLI loads one with `--abc-script
   > <file>`, resolved against the working directory. Precedence, as for `--top` and `--revision`:
   > `abc --script <file>` (that run only) beats `--abc-script`, which beats the project's
   > `abc_script`, which beats the built-in `lib/abc/vtr.abc`. `read_project` puts `--abc-script`
   > into the record, with the info line `<entry>: --abc-script <new> replaces the project
   > abc_script <old>` when they differ. The language, `{K}`, the allowed commands and the caps are
   > in the 3D spec §7.

5. **§6, step 10** becomes:

   > 10. `abc`: linked ABC (`adapters/abc`, an engine behind the C ABI) maps each module's soft
   >     logic to `$lut` cells, one ABC run per clock domain, with hard cells, black boxes,
   >     instances and other domains' flip-flops as cut points (inputs on `unconn` or x pass
   >     unchanged); the script is `abc --script`, else `--abc-script`, else the project's
   >     `abc_script`, else `lib/abc/vtr.abc`; results are re-stitched with provenance from ABC's
   >     correspondence (3D spec)

The error kinds do not change: "`abc_script` given twice" is `syntax`, and a missing file is
`missing_file`.

### 10.2 Record, reader, CLI, dump

- **Record.** `odin3_project` gains `odin3_proj_path abc_script;` after `flow` (`given` 0 means
  none; the CLI's value has `at` `{0}`).
- **`.o3proj` reader.** The key table gains `{"abc_script", 1, 1, "abc_script <file>",
  key_abc_script}`, which mirrors `key_flow`:
  - a second use is `syntax`, `abc_script given twice`, naming `abc_script`;
  - `arg_path(..., ODIN3_PROJ_WANT_FILE, ...)` makes a missing file `missing_file` at its line.
- **Request.** `odin3_project_request` gains `const char *abc_script`. `odin3_project_read` applies
  it beside `top`: it must be a regular file, else an unlocated `missing_file`, and the info line
  is logged when it differs from the project's.
- **CLI.**
  - `--abc-script FILE` joins `k_value_options` and `cli_state`. At scan time a non-file is
    `odin3: --abc-script FILE: no such file` (`bad_value`, exit 2).
  - `main.c` calls `odin3_pass_set_abc_script`, with `odin3_pass_set_top_name`'s contract, and
    puts the path into every `read_project` request.
  - Usage line: `  --abc-script FILE the ABC script of every abc pass (below abc --script)`.
- **Dump.** `"abc_script": <resolved> | null` right after `"flow"`. The README key order becomes
  `… overrides flow abc_script output ignored duplicates`.
- **Reference parser.** `tools/project-fixtures/project_fixtures.py` gets the same rules.

### 10.3 Fixtures

- **Every positive `expected.json`** gains `"abc_script": null` after `"flow"`, because the README
  says every key is always present. One mechanical commit, checked by `project-fixtures check`.
- **`o3proj_abc_script`** (phase 2, `o3proj`, oracle `yosys`):
  - `src/top.v`;
  - `abc/map.abc` (`strash; dch -f; if -K {K}`);
  - `project.o3proj` with `file`, `top` and `abc_script abc/map.abc`;
  - expected `"abc_script": "abc/map.abc"`;
  - `ref.cmd`/`ref.blif` as in `o3proj_arch_rules` (the script is stored, not applied).
- **Negatives:**

  | Case | Format | Content | Error |
  |---|---|---|---|
  | `neg_abc_script_twice` | o3proj | two `abc_script` lines | `syntax` at the second, names `["abc_script"]` |
  | `neg_abc_script_missing` | o3proj | `abc_script abc/none.abc` | `missing_file`, names `["abc/none.abc"]` |
  | `neg_abc_script_flist` | -f | `src/top.v` then `-abc_script x.abc` | `unknown_option` at `files.f:2`, names `["-abc_script"]` |

- **README.** The matrix gains these four rows. The key table gains `abc_script`: the ABC script
  path or null (stored; `abc` uses it, 3D §7.2).
- **CTest `cli_abc_script_precedence`.** `abc` runs on a two-gate BLIF with four scripts that
  differ only in a comment, and the test asserts the logged hash for every combination of
  `--script`, `--abc-script` and project `abc_script`.

## 11. Diagnostics (AB-28)

Style follows 2D §9. `<domain>` is one of `clock '<net>' posedge`, `clock '<net>' negedge`,
`global clock` or `no flip-flops`.

| Level | Exact text | Status / when |
|---|---|---|
| INFO | `abc: script <display> (sha256 <16 hex>…), K=<K\|none>, exchange <aig\|blif>, ABC <12 hex>` | start |
| INFO | `abc: module '<m>': cut points: <h> hard or black-box cells, <i> instances, <l> level latches, <t> tri-state cells, <o> other cells` | any count non-zero |
| INFO | `abc: module '<m>': N soft-logic inputs are undefined (x, z or unconn) and stay free` | N > 0 |
| INFO | `abc: module '<m>', <domain>: <c> cells and <f> flip-flops -> <l> LUTs (at most <k> inputs) and <g> flip-flops, <ms> ms in ABC` | end of each run |
| INFO | `abc: module '<m>', <domain>: no name map (<reason>); provenance uses cut cones` | §8.2 |
| INFO / DEBUG | `abc\| <line>` | ABC output: INFO for `print_*`, `&ps`, `time`, `echo`; DEBUG otherwise |
| WARN | `abc: module '<m>': combinational loop through <type> '<cell>'; its output <net> is a cut point` | §5.2 |
| WARN | `abc: module '<m>', <domain>: provenance needs N parent edges (budget M); the run's LUTs share one record` | §8.3 |
| WARN | `odin3: linked ABC unavailable: <status>` | CLI start |
| ERROR | `abc: unknown argument '<arg>'`; `abc: <option> given twice`; `abc: <option> needs a value`; `abc: --lut must be an integer from 2 to 16, not '<v>'`; `abc: --prov must be fine or cone, not '<v>'`; `abc: --via-blif directory '<dir>' contains a quote or newline; ABC cannot quote it` | `INVALID_ARG` |
| ERROR | `abc: <path>: cannot read\|create\|open …: <strerror>` | `IO` |
| ERROR | `abc: module '<m>' is not in the netlist view: <type> '<cell>'; run lower first` | `CHECK` |
| ERROR | AB-21's script errors; AB-24's cap errors | `PARSE` |
| ERROR | `abc: linked ABC is not available (no engine registered; build with ODIN3_WITH_ABC=ON)` | `ENGINE` |
| ERROR | `abc: <file>:<line>: command '<cmd>' failed`[` (from alias '<a>')`]; `abc: internal command '<cmd>' failed` | `ENGINE`, `COMMAND` |
| ERROR | `abc: ABC called exit(<code>) during '<cmd>'; linked ABC is disabled until odin3 restarts`; likewise `ABC aborted during …` and `ABC assertion failed during '<cmd>': <text>; …` | `ENGINE`, `EXIT`/`ABORT`/`ASSERT` |
| ERROR | `abc: linked ABC is disabled by an earlier failure in this process`; `abc: linked ABC is busy (another session is open)` | `ENGINE`, `DISABLED`/`BUSY` |
| ERROR | `abc: ABC returned a malformed network: <why>`, where `<why>` is one of: `N primary inputs, expected M`; `N outputs, expected M`; `N registers, more than the M it was given`; `register J corresponds to no input register`; `a LUT with N inputs (at most 16)`; `a LUT with N inputs exceeds --lut K`; `an unknown output '<name>'`; `a .subckt`; `no mapping in the & space` | `ENGINE`, `RESULT` |
| ERROR / INFO | the project and CLI texts of §10.2 | §10 |

## 12. Performance (AB-29)

**Reference.** VTR's ABC stage at `3c9a4d23` (golden results, other machines): mcml 3,414 s,
LU32PEEng 1,064 s, LU8PEEng 117 s, bgm 95 s, every other design under 15 s; peak 365 MB (mcml).
ABC dominates these times, so 3D's targets bound only what 3D adds.

| Measure (Release; `tests/bench/bench_abc.c`; smallest first; mcml, LU32PEEng, LU8PEEng and bgm under the heavy-run rule) | Target |
|---|---|
| Time outside ABC per run (classification, AIG, checks, splice, provenance) | ≤ 5% of the run's ABC time, and ≤ 10 s on mcml |
| Linked ABC against the pinned `abc` executable running the same expanded script on the same exchange BLIF | ≤ 1.05× in time; equal LUT, FF and level counts |
| Name map | ≤ 15% of ABC time on each of the 19 (else the cap) |
| `--via-blif` overhead | reported; ≤ 25% of ABC time on mcml |
| Peak RSS during `abc` | ≤ RSS before + 1.2 × the executable's peak + 64 MB |
| `open` + `close` | ≤ 20 ms |
| Script checker | ≤ 1 ms on 64 KiB |

The bench's per-run table (counts, depth, phase times, name-map coverage, edges, RSS, VTR's row)
goes verbatim into PHASE3. A missed target is investigated, never relaxed.

## 13. Testing (AB-30)

**Rules for every test:**

- Debug, ASan and UBSan, with `check` around every pass.
- Sweeps run smallest first.
- Implementers run unit tests, the CTest gate and small samples.
- **Heavy runs** are VTR-set sweeps, mcml, LU32PEEng, LU8PEEng, bgm, fuzzing past 10k cases or 10
  minutes, and ASan on large inputs. Each is wrapped as `flock /tmp/claude-1000/odin3-heavy.lock
  systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 <cmd>` and reported as deferred to
  the controller.

### 13.1 Unit tests (CI) and the fake engine

**The fake engine** (`tests/unit/abc_fake_engine.c`) implements the table without ABC, so the
core runs fast and deterministically under ASan and fault injection. `exec` knows `&put`, `&get
-m` and `strash` (no-ops), `if -K k` (each AND a 2-input LUT, inverter LUTs for complemented
outputs) and `scorr` (registers with equal `D` merge; `D` = own `Q` or 0 makes a constant).
`name_map` anchors each LUT to its AND.

| Test | Covers |
|---|---|
| `test_lut_cell` (3D-1) | the AB-2 interface: parameter order and kinds, `verify` (width range, LUT length, x refused), `const_value`, `simulate` exhaustively for WIDTH 1–6 and randomly up to 16, the bit order (`LUT[i]`, `A[0]` LSB) |
| `test_lut_writers` (3D-3) | Verilog and JSON forms; JSON's MSB-first string; `writer-check` with Yosys and Icarus |
| `test_sha256` | FIPS 180-4 vectors (`""`, `"abc"`, 448 bits, a million `a`); streamed in odd chunks |
| `test_passrun` | arguments recorded (trimmed; empty gives 0); info set once; readers' runs 0; the ABI accessors; `compact` keeps the rows |
| `test_abc_script` | every AB-21 text at its line, CRLF included; `{K}`; aliases (extra words, depth); each refusal class; arguments; mapping states (`strash` last, `&if` last, the BLIF `&` rule); caps at and past their limits |
| `test_abc_alias_table`, `test_abc_default_script` | the table equals the submodule's `abc.rc`; the usable/refused lists of §7.4; the embedded default's hash; the default with K = 6, and without `--lut` (line 10) |
| `test_abc_region` | soft, cut, constant and **undefined** classification for every registry type; CI/CO rules (clock pins, `keep`, undriven, multi-driver, CO = register output); domain order; loop cuts |
| `test_abc_unconn` | **the 3E/3H requirement**: a fixture with a `single_port_ram` whose address bits are on `unconn` directly, through a buffer, and through `and(unconn, x)`, plus a `$_CONSTX_` input. After `abc` (fake and real engine, both exchanges) every such pin is still on `unconn`/x and none is on `gnd`; `write_blif` shows it |
| `test_abc_aig` | MiniAIG layout; hashing rules; owners; inverted registers; observation outputs; the AIG equal to the IR region under 1E simulation (256 vectors × 3 seeds, sequential) |
| `test_abc_splice` | the three phase-B merges; constants; a CO driven by a CI; restore (flips, inverter to buffer, NOT for a shared `D`); DC → 2; names; cleanup; every `<why>` of §11; before equal to after under simulation |
| `test_abc_prov` | anchors and regions; every new object DERIVED; forward and backward navigation through the index; a LUT in several regions; coarse mode at a lowered budget |
| `test_abc_engine_abi` | the register contract; `ODIN3_ERR_ENGINE`'s string; "not available" without an engine |

### 13.2 Adapter tests (CI, real ABC)

`test_abc_adapter.c` uses hooks compiled only for this test (`ODIN3_ABC_TEST_HOOKS` in
`bridge.c`): the commands `o3test_exit <n>` and `o3test_assert`, added with `Cmd_CommandAdd`, and
access to `CmdSplitLine`.

- **Sessions.** 100 open/close cycles; one script twice in one process gives identical MiniLUTs;
  `BUSY` on a nested open.
- **Capture.** `echo hello` reaches the callback. fds 1 and 2, as pipes, receive nothing while ABC
  runs and are restored after.
- **No files.** In an empty working directory, every allowed command runs on a small network and
  the directory stays empty. An existing `abc.history` keeps its size and mtime.
- **Escapes**, each in its own process with `detect_leaks=0`. `o3test_exit 3` gives `EXIT`, code
  3, with fds restored, and a later `open` gives `DISABLED`. `o3test_assert` gives `ASSERT`.
- **Checked is what runs.** For every command the checker accepts (the test scripts, the usable
  aliases, the fuzz corpus), `CmdSplitLine`'s argv equals the checker's words.
- **Symbols.** `symbol_rules` with the adapter added, and `abc_adapter_exports`.

### 13.3 Round trips (CI)

1. A design with `$lut`s: `write_blif`, `read_blif`, `write_blif`. The two texts are identical
   under `netlist-compare`, with equal truth tables (once 3E's PR lands).
2. The fake engine's identity mapping in both exchanges, on every BLIF fixture and micro: before and
   after are equivalent under `equiv-check`.
3. Real ABC with `strash; if -K 6` on the same set: equivalent. Running `abc` again on that result
   is still equivalent.
4. `project-fixtures compare`, with the new cases.

### 13.4 `equiv-check` around `abc` (`tools/abc-check/abc-equiv`)

Per design, smallest first: `odin3 -p "read_blif X; write_blif before.blif; abc --lut 6 [--via-blif
D]; write_blif after.blif"`, then `equiv-check before.blif after.blif`. The tool also asserts that
the hard-block pins on `unconn` are the same set before and after.

| Where | Set |
|---|---|
| CI (`abc_equiv_micro`) | `tests/micro`, `tests/golden/blif` |
| Nightly | every single-clock RAM-free golden of the `vtr/` and regression groups, on both arches |
| Heavy (controller) | bgm, mcml, LU8PEEng, LU32PEEng |

`equiv-check` refuses several clocks and RAMs. `stereovision3` and the RAM designs are left to
P3-16's Verilator check in 3H, and are listed as such.

### 13.5 The exchange differential (`tools/abc-check/abc-diff`)

From 3D-7 on, nightly and smallest first, both exchanges run on each design. They must be
equivalent to the input and to each other. LUT, FF and depth ratios are printed, and a difference
over 5% is explained in PHASE3: the observation outputs, the AIG shape, or the entry path.

### 13.6 Out of memory

- **Fake engine.** A failure is injected at every allocation (`odin3_util_set_alloc_fail_after`)
  in the checker, region, AIG, exchange writer and read-back, splice (phases A–D) and provenance.
  Each case must give `NO_MEMORY`, no leak, a passing FULL `check`, and simulation equal to before.
- **Adapter.** A hook fails its copies and `tmpfile()`.
- **Budget.** A hook lowers the budget, and the exact error text is checked.
- **ABC's own OOM** cannot be tested in-process (§17 risk 8).

### 13.7 Fuzzing

- **`fuzz_abc_script`** (libFuzzer). Input: a K byte, then script bytes.
  - It must not crash or leak.
  - When the script is accepted: every word 0 is allowed, the arguments pass, there are ≤ 1,024
    commands, the end is mapped, and no command contains a quote, `!`, `;`, `#`, `{`, `}` or an
    alias.
  - When the script is rejected: exactly one located error.
  - Seeds: VTR's script, every alias, the refused examples.
  - CI: 60 s. Nightly long runs are heavy.
- **`fuzz_abc_splice`** (fake engine). Input: a small BLIF, K and fake choices. The result must
  have a valid IR, simulate equal, and have complete provenance. CI: 2,000 cases. More is heavy.

### 13.8 Determinism

| Test | Where |
|---|---|
| (a) two processes: identical `write_blif` and `--via-blif` files | micros in CI; the VTR set nightly |
| (b) one process: A, then B, then a fresh A; the two A results identical | CI |
| (c) two runs a minute apart, identical (catches time-seeded randomness) | nightly |
| (d) Debug against Release on the micros | nightly |

### 13.9 The 19 and M1

The P′ leg is 3F's and 3H's. 3D's runs on the 19 are `abc-equiv` on the RAM-free single-clock
subset, `abc-diff` and `bench_abc`, all heavy and all left to the controller.

## 14. Files (AB-31)

| File | Content |
|---|---|
| `src/ir/cells/lut.c`, `cells/builtin.c`, `cells/cells.h` | `$lut` (AB-2); `ODIN3_LUT_MAX_WIDTH` |
| `include/odin3/odin3.h`, `src/api/abc.c`, `src/api/pass.c`, `src/api/design.c` | engine ABI, `ODIN3_ERR_ENGINE`, pass-run accessors, `odin3_pass_set_abc_script` |
| `src/ir/prov.[ch]`, `src/passes/manager.c` | pass-run rows; args in `open_run`; compact after `abc` |
| `src/util/sha256.[ch]` | `odin3_sha256_init/update/final/hex` (FIPS 180-4, streaming, no allocation) |
| `src/backends/{verilog,json}/*` | `$lut` (AB-3) |
| `src/passes/abc/abc.h`, `abc_test.h` | caps; `odin3_abc_opts`; `odin3_abc_pass_run(odin3_pass_ctx *, odin3_design *, odin3_bytes args, void *user)`; `odin3_abc_test_set_limits`, `odin3_abc_test_set_engine` |
| `script.[ch]`, `alias.c`, `allow.c`, `default_script.c` (generated) | AB-21 to AB-23: `odin3_abc_script_parse(const odin3_abc_script_src *src, uint32_t lut_k, uint32_t exchange, odin3_abc_script *out)`, `odin3_abc_script_free`, `odin3_abc_script_cmd` |
| `args.c`, `run.c`, `engine.c` | AB-19, AB-20, AB-22: the module and domain loop, logs, budget, run information; the registered engine |
| `region.[ch]`, `aig.[ch]`, `xblif.[ch]`, `result.[ch]`, `splice.[ch]`, `prov.[ch]` | AB-10 to AB-18 and AB-25 |
| `src/frontends/project/*`, `src/cli/main.c` | AB-27 |
| `adapters/abc/{adapter,session,guard,capture,bridge}.c`, `bridge.h`, `odin3_abc.h`, `exports.map`, `lsan.supp`, `NOTICE`, `README.md` | AB-5 to AB-9 |
| `cmake/abc_pin.cmake`, `CMakeLists.txt`, `tools/check-symbols.sh` | AB-5, AB-6 |
| `lib/abc/vtr.abc`; `tools/abc-check/{abc-equiv,abc-diff}` (stdlib Python); §13's tests | |

Side arrays indexed by ID come from `util/vec`, sized at the store length and freed per run. Per-run
scratch comes from one arena. lizard's limits hold (≤ 60 lines, CCN ≤ 15, ≤ 5 parameters). There
is no recursion: the cone walks and the cleanup use explicit stacks.

## 15. Implementation split (AB-32)

Nine PRs. Each PR's plan fixes its tasks. Each starts tests-first, passes `/lint`, and gets a
read-only critique (PHASE0 §A.1). Model and effort follow PHASE0 §9.

| PR | Content | Depends on | Review | Model / effort |
|---|---|---|---|---|
| **3D-0** spike (not merged) | AB-17's seven criteria; frame cost; name-map coverage | — | light | Opus `xhigh` |
| **3D-1** `$lut` (small, first) | the AB-2 interface: `lut.c`, the builtin entry, `ODIN3_LUT_MAX_WIDTH`, `test_lut_cell`, IR.md and DESIGN §5.2 text. **3E's ISOP/`$lut` PR follows directly** | — | **Peter** (IR) | Opus `high` |
| **3D-2** adapter | CMake (`libabc-pic`, pin, `-O2`, `ABC_NO_HISTORY`, the PIC `abc` tool); `adapters/abc` (bridge, session, guard and wraps, capture, exports, NOTICE and licence scan); engine ABI, `ODIN3_ERR_ENGINE`, CLI init; symbol and adapter tests | — | **Peter** (adapter build, ABI) | Opus `high` |
| **3D-3** run records | pass-run args and info, accessors, IR-12 text; `sha256`; `$lut` in Verilog and JSON | 3D-1; 3E's ISOP PR for Verilog | **Peter** (IR, ABI) | Opus `high` |
| **3D-4** script | checker, caps, alias table, allow-list, embedded default, `fuzz_abc_script` | 3D-3 (sha256) | light | Opus `high` |
| **3D-5** pass, BLIF exchange | args and precedence; region, undefined nets and domains; exchange writer and read-back; `abc_result`; splice and restore; cone provenance; run loop, logs, budget, compact; fake engine, `abc-equiv`, `test_abc_unconn`, `fuzz_abc_splice`; PASSES row; DESIGN §6 | 3D-2, 3D-3, 3D-4, 3E's ISOP PR | light | Opus `xhigh` → `high` once the splice settles |
| **3D-6** project and CLI | `abc_script`, `--abc-script`, request, dump, reference parser, DESIGN §4.0, fixtures (with the bulk `null` edit), precedence test | 3D-4 | light (P3-1) | Opus `high`; Sonnet `medium` for the bulk |
| **3D-7** in-memory exchange | AIG builder; observation outputs; MiniLUT result; aligned MiniLUT and name map; in-memory as the default; `abc-diff` | 3D-5, the 3D-0 go | light; **Peter** if P3-9 (c) | Opus `xhigh` → `high` |
| **3D-8** evidence | determinism suite, `bench_abc` and calibration, nightly sweeps, the PHASE3 entry | 3D-5 (3D-7 for its rows) | light | Opus `high`; Sonnet `high` for bulk runs |

**Lanes:**

- 3D-1 first: a small PR that unblocks 3E.
- 3D-0 and 3D-2 in parallel with it.
- Then 3D-3 and 3D-4.
- Then 3D-5, which gives **M1** with 3E and 3F.
- 3D-6 after 3D-4.
- 3D-7 after the spike.
- 3D-8 alongside.

**Model cues:** Opus `xhigh` for 3D-0 and for the splice and provenance tasks of 3D-5 and 3D-7;
`high` otherwise; Sonnet for the bulk edit.

## 16. Amendments to other documents

| Document | Change | PR |
|---|---|---|
| IR.md | LUT cells (§2) | 3D-1 |
| IR.md | IR-12 (§8.5) and the §11 entry | 3D-3 |
| DESIGN | §5.2 (`$lut`) in 3D-1; §6 step 10 in 3D-5; §4.0 in 3D-6 (§10.1) | 3D-1, 3D-5, 3D-6 |
| PHASE3 | notes for Peter: P3-8 "registers the `abc` pass" becomes "registers the ABC engine (AB-7)"; P3-9 (a): correspondence through the name map on an aligned MiniLUT plus observation outputs, entering through `&` + `&put` (AB-16); "spike (3D-0)" in the checklist | with this spec |
| PASSES.md, ORACLES.md, CLAUDE.md | the `abc` row; the `abc` tool built from `libabc-pic`; Layout `adapters/{slang,abc}` (the ABC adapter is C, and ABC's C++ stays in `third_party/`) | 3D-5, 3D-2 |
| corpus README | key row, key order, four matrix rows | 3D-6 |
| 3E spec (its author) | builds on 3D-1's interface; provides the cover functions of AB-3; writes `# odin3 abc:` (§8.1); keeps `unconn` pins unchanged | 3E |

## 17. Risks

1. **ABC kills the process**: a segfault, a C++ `std::terminate` (124 C++ sources), or stack
   exhaustion; the wraps cover only `exit`, `abort` and assertions. The fallback is a subprocess
   engine.
2. **The spike fails.** AB-17's fallbacks apply. M1 runs on the BLIF exchange either way.
3. **Observation outputs perturb QoR** near registers that become unobservable. `abc-diff` and
   P′ measure it; the alternative is `vNamesIn`.
4. **The name map is slow.** Covered by the cap, `--prov cone` and the bench.
5. **Overlapping cones.** Covered by the edge budget and the single record.
6. **Hidden nondeterminism**: `srand` (33 objects), threads, a missing generator. Covered by §13.8
   and the spike.
7. **fd capture is process-global.** A host's other threads printing during ABC are captured.
   Documented.
8. **ABC's memory is unbounded.** Covered by the budget, the calibration and the heavy-run rule; an
   OOM on WSL kills the VM.
9. **Undefined nets stay free** (the 3E/3H requirement), so QoR is lost where soft logic reads
   `unconn` or x and VTR folds 0. The P′ leg reports it.
10. **Pin coupling**: the name map and generators are outside `abcapis.h`. Covered by the pin
    check, the alias test and a spike rerun per bump.
11. **Build time and licence.** The PIC `abc` tool and ccache; 3D-2's licence scan.
12. **Partly pinless wires** give `check` warnings. If they are frequent, a `check` change for
    Peter.
13. **The ABC version gap** with VTR's ABC. The P′ leg attributes it (PHASE3 risk 4).

## 18. Decisions for Peter

1. **The engine seam** (AB-7). *Recommended:* the pass in the core and an engine in the adapter.
   This rewords P3-8 and keeps its intent.
2. **`$lut`** (AB-2). *Recommended:* the interface as in §2, landed alone as 3D-1, with 3E's ISOP PR
   right after.
3. **Pass-run arguments and run information** (§8). *Recommended:* both fields, two accessors, and
   the IR-12 text verbatim; `exchange=` added.
4. **`ODIN3_ERR_ENGINE`** and one ABI bump. *Recommended:* yes.
5. **Wrap, not allow** (AB-6). *Recommended:* the adapter passes the unchanged `symbol_rules`.
6. **The correspondence** (AB-16). *Recommended:* the name map on an aligned MiniLUT, observation
   outputs, and entry through `&` + `&put`. This corrects P3-9 (a)'s `…MiniLutObj`, and the spike
   gates it.
7. **A frame per run** (AB-8). *Recommended:* yes, instead of risk 1's single session.

Agent defaults:

- `--log` and `--prov`;
- no built-in K;
- domain order by clock name;
- `&ps` allowed; `print_constr` and `print_status` left out;
- quotes refused;
- loops cut;
- 512 bytes per AIG object until calibrated;
- the PIC `abc` tool;
- `-O2` ABC in Debug;
- `set check`;
- `"abc_script": null` in every positive record.
