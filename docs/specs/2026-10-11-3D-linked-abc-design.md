# 3D — Linked ABC: design

Status: v2 (2026-10-11), Peter's review for the IR and ABI pieces: `$lut` (§2), IR-12 (§8.5), the
engine ABI (§4.3) and the adapter build (§4.1–4.2); light review otherwise. Phase 3, sub-project
3D; Opus `xhigh` → `high`.

v2 applies Fable's `3D-review1` (3 critical, 10 important, 12 minor) and the coordinator's rulings.
Every finding is applied and no minor is rejected (§19). The rulings:

- **C1:** one init encoding on both exchanges.
- **C2:** `unconn`/x is 0 at soft readers; a hard pin keeps its direct net.
- **C3:** register inputs are identity anchors.
- **I4:** `tools/heavy-run`.
- **I7:** the script path as given.
- **I8:** in-memory is the measured exchange.
- **I11:** ABC's names give the register correspondence.
- **Q1–Q4:** confirmed, no ABI bump.
- **`$lut`:** approved as an agent default (P3-9 approved 2026-10-11), so 3D-1 proceeds once this
  spec merges.

v1 included the 3E/3H review's three requirements: hard pins on `unconn`/x pass unchanged; the
exchange follows P3-9, P3-12 and P3-13; 3D owns `$lut` and lands it first.

**Inputs:**

- PHASE3 decisions 1 and 8–14, approved by Peter 2026-10-11 (cited as **P3-n**), plus P3-10 and
  P3-16–18;
- DESIGN §4.0, §5.2, §6, §15; IR.md IR-4, IR-6, IR-8 to IR-13, IR-15 to IR-17; the op registry;
- the 1C, 1D, 1E and 1G specs;
- 2E §16 and the 2E plan's point (p) (heavy runs);
- the 3E spec v2.1 (`docs/specs/2026-10-11-3E-blif-vtr-design.md`: §4.2, §5, §9, §14.1) and 3H;
- the 2B project reader.

**Oracles:**

- ABC as pinned in ORACLES.md: `third_party/abc` at `a3001b72edc5de22442e942165487fddf150e3d0`
  (2026-10-05); every claim about ABC was read in that tree.
- VTR `3c9a4d23`'s ABC stage: `vtr_flow/scripts/python_libs/vtr/abc/abc.py`,
  `vtr_flow/scripts/blackbox_latches.pl` and `abc/abc.rc`.

Decisions are **AB-n**; the PRs are 3D-0 … 3D-8 (§15). Where this file and IR.md disagree, IR.md
wins, except for the two IR changes proposed here.

Two findings reword approved decisions without changing their substance. PHASE3 notes them (§16):

- P3-8's adapter "registers the `abc` pass". Here the pass is in the core, and the adapter
  registers an **ABC engine** (AB-7).
- P3-9 (a) names `Abc_FrameGiaOutputMiniLutObj` as ABC's object correspondence. At the pin that
  call returns the timing manager's box map (`Gia_ManDeriveBoxMapping`). The per-LUT
  correspondence is `Abc_FrameReadMiniLutNameMapping` (AB-16).

## 1. Purpose and scope (AB-1)

`abc` takes each module in the netlist view (after `lower`, or as `read_blif` leaves it). It maps
its soft logic to K-input LUTs with the pinned ABC and re-stitches the result into the IR, with
exact provenance and byte-identical reruns. ABC's `exit()` calls and global symbols stay outside
`libodin3` (P3-8).

| Delivered by 3D | Where |
|---|---|
| `libabc-pic` from the submodule; the adapter `libodin3_abc.so`; its symbol checks | `CMakeLists.txt`, `adapters/abc/`, `tools/check-symbols.sh` |
| The `$lut` interface and registry cell with its JSON form, first and alone (3D-1); `$lut` in the Verilog writer | `src/ir/cells/lut.c`, `src/backends/{json,verilog}` |
| Engine ABI and `ODIN3_ERR_ENGINE`; pass-run arguments and run information (IR-12) with accessors | `odin3.h`, `src/api/`, `src/ir/prov.[ch]`, `src/passes/manager.c` |
| The `abc` pass: arguments, script checker, regions, domains, both exchanges, splice, latch restore, provenance | `src/passes/abc/` |
| Default script; SHA-256 | `lib/abc/vtr.abc` (embedded); `src/util/sha256.[ch]` |
| `abc_script` (`.o3proj`), `--abc-script` (CLI), the `dump_project` field, fixtures | `src/frontends/project/`, `src/cli/main.c`, `tools/project-fixtures/`, `tests/golden/projects/` |
| `equiv-check --blackbox` (hard blocks as boxes) | `tools/equiv-check/` |

**Not in 3D:**

- `write_blif --vtr` (3E, which prints the run information);
- the ISOP cover `odin3_blif_isop` and `$lut`'s BLIF forms (3E-5, right after 3D-1; AB-3);
- the VTR wrapper and K (3F passes `--lut`);
- `lower` and `hardmap` (3B, 3C);
- retiming (P3-12 b);
- ABC as a subprocess (PHASE3 risk 1's fallback, as a second engine, AB-7);
- an `equiv` pass.

**Success:**

1. **M1:** with 3E and 3F, `read_blif <Parmys golden>; abc --lut 6; write_blif --vtr` routes all
   19.
2. `equiv-check --blackbox` before against after `abc` passes on every micro and on the
   single-clock goldens (18 of the 19; §13.4).
3. Byte-identical netlists across processes and within one.
4. `symbol_rules` and `abi_exports` are unchanged and green, with the adapter added to
   `symbol_rules`.
5. Every new LUT and flip-flop has a `DERIVED` record that reaches its source, D cones included.
6. §12's targets.

## 2. The `$lut` interface: an IR change owned by 3D (AB-2, AB-3)

LUTs come back from ABC as one new registry cell with Yosys `$lut` semantics: DESIGN §5.2's
`$_LUT_K_` (P3-9). The cell lands **first and alone** in 3D-1, with:

- its definition, `verify`, `const_value`, `simulate` and JSON form;
- the IR.md and DESIGN text;
- `test_lut_cell`.

3E-5 builds on it immediately. Status: approved as an agent default (P3-9 approved 2026-10-11). The
3D-1 PR is still Peter's review, because it is an IR change.

**The interface (AB-2)**, the contract that 3E, 1E, the writers and `abc` share:

| Item | Definition |
|---|---|
| name, file | `$lut`; `src/ir/cells/lut.c`, entry in `cells/builtin.c` (IR-8, IR-11) |
| granularity, flags | `bit` (netlist view); no flags |
| parameter 0 | `WIDTH`: `ODIN3_VAL_INT`, default 1, valid 1 to 16 (`ODIN3_LUT_MAX_WIDTH`, `src/ir/cells/cells.h`) |
| parameter 1 | `LUT`: `ODIN3_VAL_BITS`, default `LUT[0]` = 0, `LUT[1]` = 1 (`2'b10`, the identity); valid when exactly 2^`WIDTH` bits, each 0 or 1 (x, z refused) |
| port 0 | `A`: input, vector (`scalar` false), width parameter `WIDTH`; pin *j* of `A` is `A[j]`, LSB first (IR-1) |
| port 1 | `Y`: output, scalar, width 1 |
| function | `Y = LUT[i]`, `i = Σ_j A[j]·2^j`; `LUT[i]` is bit *i* of the parameter counted from its least significant bit (bit 0), as for every BITS value (IR-10). Yosys `$lut` semantics |
| text forms | Yosys JSON and RTLIL write `LUT` MSB first: the first character is `LUT[2^WIDTH−1]`. BLIF `.names` inputs are `A[0] … A[WIDTH−1]`, so cover column *j* is `A[j]` |
| `const_value` | `ODIN3_CONST_0` when every `LUT` bit is 0, `ODIN3_CONST_1` when every bit is 1, else `ODIN3_CONST_NONE` |
| `simulate` (1E) | `Y = LUT[i]`, *i* from the `A` input bytes; `sim_scratch_bytes` NULL |
| constants | `abc` never makes a constant `$lut`: an all-0 or all-1 table becomes the canonical `$_CONST0_`/`$_CONST1_` net (AB-14; 3E §4.2 item 1) |
| engine tables | at the engine boundary (§4.3) an object's truth table is `W = (lut_size > 5 ? 1 << (lut_size − 5) : 1)` 32-bit words. For a *k*-input object, `LUT[i]` for *i* < 2^*k* is bit *i* of that word array: word 0 least significant, bit 0 of each word least significant, input variable *v* = fanin *v* = `A[v]`. Bits from 2^*k* up are ignored (ABC replicates the table there) |

**The split with 3E (AB-3).** 3E owns the irredundant cover and `$lut`'s BLIF forms, in both
dialects (3E-5). The function is `odin3_blif_isop(const uint64_t *tt, uint32_t k, odin3_strbuf
*cover, uint32_t *rows, int *constant)` in `src/backends/blif/isop.[ch]` (3E §4.1). 3D uses it:

- for gates and `$lut`s in the BLIF exchange (AB-15);
- in the AIG builder (AB-16);
- in the Verilog writer, which turns the cover into a sum of products (3D-3).

A BLIF round trip turns a `$lut` into the `$sop` with that cover.

**IR.md text**, after the "BLIF cells (used by 1C)" paragraph:

> **LUT cells (3D).** `$lut` (`src/ir/cells/lut.c`, granularity `bit`) has parameters `WIDTH`
> (int, 1 to 16, `ODIN3_LUT_MAX_WIDTH`) and `LUT` (bits: exactly 2^`WIDTH` bits, each 0 or 1), and
> ports `A` (in, `WIDTH` bits, LSB first) and `Y` (out, 1 bit). `Y = LUT[i]` with `i = Σ A[j]·2^j`,
> and `LUT[i]` is bit *i* of the parameter counted from its least significant bit: Yosys `$lut`
> semantics (text forms write `LUT` MSB first). Its `const_value` is 0 or 1 when every `LUT` bit
> is, and it simulates (1E). The writers today: `write_json` and `write_rtlil` write it as Yosys
> `$lut` (`LUT` MSB first; RTLIL spells the output of a constant one as the constant),
> `write_verilog` as a generic instance `\$lut #(.WIDTH(…), .LUT(…))` (`LUT` MSB first),
> `write_dot` as a node labelled `$lut`, and `write_blif` (3E-5, both dialects) as a BLIF
> `.names A[0] … A[WIDTH−1] Y` with an irredundant cover (3E's `odin3_blif_isop`: the shorter of
> the on-set and off-set ISOP, the on-set on a tie), a constant one as the zero-input `.names Y`,
> so a BLIF round trip turns it into the `$sop` with that cover. 3D Task 9 makes `write_verilog`
> print that cover as a sum of products. `abc` is its only producer in Phase 3 and never makes a
> constant one.
>
> *(Erratum, 3D-1 final review, 2026-10-12: the writer sentences now say what each writer does at 3D-1 and what 3E-5 and Task 9 change; `$_LUT_K_` no longer exists in DESIGN §5.2.)*
>
> *(Erratum, 3E-5, 2026-10-12: `write_blif` now writes the cover; 3D-1's interim refusal, and its `.subckt $lut` for the WIDTH-1 identity, are gone.)*

**DESIGN §5.2**: "`$sop` (BLIF `.names` covers) and `$_LUT_K_`" becomes "`$sop` (BLIF `.names`
covers) and `$lut` (a K-input LUT, Yosys semantics; `docs/IR.md` LUT cells)".

## 3. The pinned ABC, read at the source (AB-4)

| Fact (at `a3001b72`) | Where | Consequence |
|---|---|---|
| `abcapis.h` declares `Abc_Start`/`Abc_Stop` (a lazy global frame), `Cmd_CommandExecute`, the MiniAIG/MiniLUT calls and explicit frames `Abc_FrameCreate/Destroy/Enter/Leave/Current` ("strict LIFO order on one thread") | `base/main/abcapis.h`, `mainFrame.c:654` | A fresh frame per run; `Abc_Start`/`Abc_Stop` are never used (AB-8) |
| The frame pointer is `ABC_THREAD_LOCAL`; `Cmd_CommandExecute` enters the given frame; `Abc_FrameDeallocate` frees all frame state | `mainFrame.c:38, 222–280`, `cmdApi.c:193` | Frames are clean; other statics are process-wide |
| Static generators `Abc_Random`, `Abc_RandomW`, `Aig_ManRandom`, `Aig_ManRandom64`, `Gia_ManRandom`, `Gia_ManRandomW`, `Random_Int`, `Random_Word`; libc `rand()` in `fraig`/`fraig_sweep` (never seeded, `fraigMan.c:187`) and `Abc_InfoRandom` users | `utilSort.c:1012`, `aigUtil.c:1175`, `giaUtil.c:49`, `rewireRng.c` | All reset per session, with `srand(1)` (AB-8) |
| `Cmd_Init` reads `abc.history` from the working directory and every command appends to it, unless batch mode is on or `ABC_NO_HISTORY` is defined | `cmd/cmd.c:93`, `cmdHist.c:65–175` | Both are set (AB-5, AB-8) |
| A one-word unknown command with a `.` runs as `read <word>`; `!` is a shell escape; `CmdSplitLine` splits at `;`, `#`, blanks, strips quotes and turns `isspace` bytes in quotes into spaces; aliases come only from `abc.rc`; `quit` returns −1 (`cmd.c:386`) | `cmdUtils.c:96–270` | The checker refuses quotes, `!`, file options and unlisted words, and expands aliases (AB-21) |
| Debug `libabc.a` imports, counted in objects: `exit` 162, `abort` 555, `__assert_fail` 1,091, `_exit` 4, `system` 14, `popen` 2, `fork` 3, `execv`/`execvp` 1 each, `signal` 1. `pthread_create` is called in 18 files. No `NDEBUG` | `nm -u` | Link-time `--wrap` with a thread check (AB-6) |
| Printing goes through `printf`/`vprintf`/`puts`/`putchar` (`Abc_Print` ignores the frame's `Out`); `ABC_ALLOC` is an unchecked `malloc` | `misc/util/abc_global.h:490` | fd capture (AB-8); a pre-run budget (AB-24) |
| `Abc_FrameGiaInputMiniAig` loads the `&` space and keeps `pGiaMiniAig`/`vCopyMiniAig`; `Abc_NtkInputMiniAig` does neither | `gia/giaMini.c:151`, `abci/abcMini.c:175` | Input through `&`, then `&put` (AB-16) |
| `Abc_FrameGiaOutputMiniLut` returns the `&` mapping: CIs, then LUTs in topological order, then COs (constant 0/1 or a node; complemented COs share inverter LUTs); `nRegs` is set; a LUT's table may be constant | `giaMini.c:410–545` | The result form folds constants (AB-14) |
| `Abc_FrameReadMiniLutNameMapping`: per MiniLUT object, the MiniAIG literal ABC **proves** equivalent (1,000-conflict SAT sweep, `cecSolve.c:738`) or −1; combinational; `Gia_ManDup2` asserts equal CI/CO counts; not in `abcapis.h` | `giaMini.c:704`, `giaDup.c:786` | Sound anchors via an aligned MiniLUT and `Abc_FrameGiaInputMiniLut2` (AB-16) |
| `scorr` keeps each surviving latch's names (`abcDar.c:573–575`), unless its output name equals a PO name, which gets a dummy `l<i>` (`:563`). `&put` gives the network the `&` space's CI names (`abc.c:35302`); `&get -n` copies CI and CO names back (`:35150`); `&get -m` keeps the mapping and sets `RegNum` | `abci/abcDar.c`, `abci/abc.c` | Register correspondence by name on both exchanges (AB-13, AB-16) |
| DAR (behind `scorr`) complements init-1 registers on entry, and `Abc_NtkFromDarSeqSweep` writes every latch back at init 0 (`abcDar.c:512, 2440`): a `.latch … 1` returns as init 0, holding ¬Q, under the same name. `&get` without `-m` zeroes inits too (`abc.c:35129`) | `abci/abcDar.c:134–331` | Both exchanges send init-0 latches only, and restore themselves (AB-13) |
| 1,247 C and 124 C++ sources; CMake target `libabc-pic` (a static archive); PUBLIC flags `-Wall -Wno-unused-function -Wno-write-strings -Wno-sign-compare -DLIN64 -DSIZEOF_VOID_P=8 -DSIZEOF_LONG=8 -DSIZEOF_INT=4 -DABC_USE_CUDD=1 -DABC_USE_PTHREADS` | `CMakeLists.txt`, `make cmake_info` | AB-5 |
| Licence: UC's permissive notice; the one GPL mention, the GipSAT port, is under ABC's licence by the author's permission | `copyright.txt`, `proof/pdr/gipsat/gipMain.c` | AB-9 |

## 4. The adapter boundary

### 4.1 Build (AB-5)

With `ODIN3_WITH_ABC` on (the default), the build makes `libabc-pic` and the adapter. Without it
there is no adapter, and `abc` fails with "not available" (§11). Inside the existing
`odin3_add_abc()` scope:

- `CMAKE_C_FLAGS_DEBUG`/`CMAKE_CXX_FLAGS_DEBUG` are `-g -O2` for ABC only, because an unoptimised
  ABC would stretch mcml's hour into several. Debug keeps ABC's assertions; Release uses `-O2
  -DNDEBUG`.
- ABC never gets `odin3_flags` or sanitisers. ASan still intercepts ABC's mallocs, so ABC's known
  leaks are suppressed by function in `adapters/abc/lsan.supp`, and none of them is the adapter's.
- `set_property(TARGET libabc-pic APPEND PROPERTY COMPILE_DEFINITIONS ABC_NO_HISTORY)`.
- **Agent default:** `odin3_abc_exe` is an `add_executable` of
  `third_party/abc/src/base/main/main.c`, linked to `libabc-pic` (`OUTPUT_NAME abc`, in
  `${PROJECT_BINARY_DIR}/third_party/abc/`). It replaces ABC's own `abc` as `abc_tool`'s
  dependency and as `ODIN3_ABC`, so ABC compiles once per build directory.
- `cmake/abc_pin.cmake` sets `ODIN3_ABC_COMMIT` (`a3001b72…`). When `git -C third_party/abc
  rev-parse HEAD` gives another commit, configure stops with `third_party/abc is at <sha>;
  ORACLES.md pins <pin>`. The adapter gets it as `ODIN3_ABC_COMMIT_STR`.

The adapter target is `odin3_abc`: `SHARED`, `OUTPUT_NAME odin3_abc`, `C_VISIBILITY_PRESET hidden`,
linked `PRIVATE` to `libodin3` and `libabc-pic` through the C++ driver.

- `adapters/abc/bridge.c` is the only file that includes ABC headers: `abc_global.h`, `abcapis.h`,
  `main.h`, `miniaig.h`, `minilut.h`, and `aig/gia/gia.h` for the one field `vNamesIn` (AB-16).
  It includes them `SYSTEM`, and builds with `odin3_flags` minus `-Wstrict-prototypes`, because
  ABC declares `Abc_Start()`.
- `session.c`, `guard.c`, `capture.c` and `adapter.c` include only `odin3.h` and `bridge.h`, with
  the full `odin3_flags`.
- `tools/lint.sh` covers `adapters/abc/*.c`.
- The adapter's own files never call `assert()`: a link-wide `--wrap=__assert_fail` would report
  their failures as ABC's. They use `ODIN3_ADAPTER_ASSERT(c)` (`guard.h`: `if (!(c))
  __builtin_trap()`). `tools/check_rules.py` gains the rule "no `assert(` in `adapters/abc/`".
  `__builtin_trap` outside `check` is a documented exception for this directory (Peter, in 3D-2's
  review).

### 4.2 Symbols and process control (AB-6)

```
-Wl,--exclude-libs,ALL -Wl,--version-script=adapters/abc/exports.map -Wl,-z,defs
-Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=abort -Wl,--wrap=__assert_fail
-Wl,--wrap=system -Wl,--wrap=popen -Wl,--wrap=fork -Wl,--wrap=execv -Wl,--wrap=execvp
-Wl,--wrap=signal
```

`exports.map` is `{ global: odin3_plugin_init; odin3_abc_adapter_init; local: *; };`.

`--wrap=sym` rewrites the undefined references to `sym` **in the objects of this link**, which
includes the members taken from the static `libabc-pic.a`. `guard.c` defines the wrappers and never
names a `__real_` symbol. So the `.so` imports none of the banned names, `tools/check-symbols.sh`
runs on it unchanged, and `symbol_rules` only gains an argument (Q3, confirmed).

**What the wraps cover, honestly.** They catch `exit`, `_exit`, `abort` and `__assert_fail` when
**ABC's own objects** call them. They do **not** catch:

- `std::terminate` (124 C++ sources; it reaches `abort` inside `libstdc++.so`);
- glibc's fatal paths (heap corruption, `__libc_fatal`);
- signals, notably the SIGSEGV that an unchecked `ABC_ALLOC` gives on OOM.

Each of those kills `odin3` (§17 risk 1).

The wrappers:

- **Escapes** (`__wrap_exit`, `__wrap__exit`, `__wrap_abort`, `__wrap___assert_fail`) are
  `_Noreturn`.
  - On the **session thread**, when `pthread_equal(pthread_self(), session.thread)` holds and a
    guard is active, each records its kind, code and, for an assertion, `"<file>:<line>: <expr>"`
    (≤ 255 bytes), then `longjmp`s to the guard.
  - On **any other thread** (ABC spawns threads in 18 files), or with no guard active, it writes
    `odin3: linked ABC called <exit|abort|assert> outside its session thread; stopping` to the
    saved original stderr fd, then executes `__builtin_trap()`. A `longjmp` across threads would
    be undefined behaviour, and the trap imports nothing.
- **Process control:**
  - `__wrap_system`, `__wrap_fork`, `__wrap_execv` and `__wrap_execvp` return −1;
  - `__wrap_popen` returns NULL;
  - `__wrap_signal` returns `SIG_ERR`.

  All of them set `errno = EPERM` and add `ABC tried to <call>; refused` to the session's output.
  The allow-list makes them unreachable; they are kept as defence in depth.

**Checks:**

- `symbol_rules` is `check-symbols.sh libodin3 example_plugin odin3_abc`.
- `abc_adapter_exports` is `check-symbols.sh --only odin3_plugin_init,odin3_abc_adapter_init
  --forbid exit,_exit,_Exit,quick_exit,abort,system,popen,fork,execv,execvp,signal
  <libodin3_abc.so>`. This is a new mode: the exported functions must equal `--only`, and nothing
  in `--forbid` may be imported.
- `abi_exports` is unchanged.

### 4.3 The engine seam (AB-7)

**Decision** (Q1, confirmed). `abc` is a core built-in (`src/passes/abc/`). The adapter supplies an
**engine**: a table of seven functions that touch ABC and nothing else, registered through the
public ABI.

P3-8's wording instead has the adapter register the pass and write the IR through about 30 new
ABI functions, outside `src/` and fault injection. The seam keeps IR writes under IR-15, `check`,
ASan and OOM injection, keeps P3-8's intent (ABC symbols and `exit()` outside `libodin3`, gate
unchanged, the slang adapter's shape), and hosts risk 1's subprocess fallback and the test double.
The internal `odin3_abc_test_set_engine` is the only way past "final".

**No ABI version bump** (Q4, confirmed). ABI v4 has had no release and every consumer is in-tree.
The additions below are recorded under the existing "ABI v4" changelog line of `odin3.h`, together
with:

- `ODIN3_ERR_ENGINE = 8` and `ODIN3_STATUS_COUNT` = 9;
- `odin3_status_string`'s table;
- the Python cdef block, which is read verbatim;
- `test_abc_engine_abi`.

The rule "bump on the first incompatible change after a release" stands.

```c
/* --- the ABC engine (3D) -------------------------------------------------------------------- */
enum { ODIN3_ABC_ENGINE_VERSION = 1 };

typedef enum odin3_abc_fail_kind {
    ODIN3_ABC_FAIL_NONE = 0,
    ODIN3_ABC_FAIL_COMMAND = 1,  /* a command returned non-zero (code = its value) */
    ODIN3_ABC_FAIL_EXIT = 2,     /* ABC called exit(code) or _exit(code) on the session thread */
    ODIN3_ABC_FAIL_ABORT = 3,    /* ABC called abort() on the session thread */
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

/* Odin III's exchange layout, equal to ABC's MiniAIG at the pin (aig/miniaig/miniaig.h; the
 * adapter converts if a pin bump changes it). objs has 2 * n_objs entries: object 0 is constant
 * 0; a PI is (0x7FFFFFFF, 0x7FFFFFFF), an AND (lit0, lit1) with lit0 < lit1 < 2 * index, a PO
 * (lit, 0x7FFFFFFF); the last n_regs PIs and POs are register outputs and inputs, in order. */
typedef struct odin3_abc_aig {
    const int32_t *objs;
    uint32_t n_objs;
    uint32_t n_regs;
} odin3_abc_aig;

/* Odin III's exchange layout, equal to ABC's MiniLUT at the pin (minilut.h). fanins has
 * lut_size * n_objs entries (0x7FFFFFFF unused; a PO has its driver in slot 0 and 0x7FFFFFFE in
 * slot 1); objects 0 and 1 are constants, then PIs, LUTs in topological order, POs. truths has
 * W * n_objs 32-bit words, W = (lut_size > 5 ? 1 << (lut_size - 5) : 1); a k-input object's
 * function is bits 0 .. 2^k - 1 of its W words, word 0 and bit 0 least significant, variable v
 * = fanin v. ci_from has (number of PIs) entries: the index of the loaded AIG's PI each PI came
 * from (ABC's names, AB-16), or -1; NULL when no names came back. */
typedef struct odin3_abc_luts {
    const int32_t *fanins;
    const uint32_t *truths;
    const int32_t *ci_from;
    uint32_t n_objs;
    uint32_t n_regs;
    uint32_t lut_size;
} odin3_abc_luts;

/*
 * An ABC engine; one session per ABC run (§4.4). All but close return ODIN3_OK, ODIN3_ERR_ENGINE
 * (*fail filled; any call may fail EXIT, ABORT or ASSERT) or ODIN3_ERR_NO_MEMORY.
 * open: fresh ABC state, generators and libc rand() reset, output to text (NULL: dropped).
 *   BUSY, DISABLED.
 * load_aig: aig (copied) becomes the & space's network, its PIs named by index, and the name
 *   map's reference. RESULT.
 * exec: one already-checked command line, run as given. COMMAND.
 * get_luts: the & space's mapping with ci_from, owned by the session until close or the next
 *   call. RESULT.
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

The adapter exports exactly two functions (`adapters/abc/odin3_abc.h`):

- `odin3_status odin3_abc_adapter_init(void)` registers the engine.
- `odin3_plugin_init(uint32_t)` checks the ABI, then calls the first, for `--plugin` and Python's
  `odin3_plugin_load`.

`odin3` links the adapter (P3-8). Under `ODIN3_HAVE_ABC`, `main.c` calls `odin3_abc_adapter_init()`
before any `--plugin`; a failure is the warning `odin3: linked ABC unavailable: <status>`.

### 4.4 Sessions (AB-8)

The linked engine (`session.c`, `bridge.c`) has one static session.

1. **open.**
   - `pthread_mutex_trylock` (`BUSY`; a disabled engine gives `DISABLED`).
   - Record `session.thread = pthread_self()`.
   - Inside a guard: `frame = Abc_FrameCreate()`, `prev = Abc_FrameEnter(frame)`,
     `Abc_FrameSetBatchMode(1)`, then `Cmd_CommandExecute(frame, "set check")`, VTR's `abc.rc`
     setting, which changes no result.
   - Then every ABC generator of AB-4 called with `(1)` (declared `extern` in `bridge.c`; the list
     is part of the pin), and `srand(1)`, which also seeds `fraig` (Odin III never uses `rand()`).
   - The frame stays entered, so ABC never makes its lazy global frame.
2. **Every call** runs under `guard_run`:
   - `setjmp`;
   - `fflush` stdout and stderr, then `dup`/`dup2` fds 1 and 2 onto the session's `tmpfile()`;
   - the ABC call;
   - `fflush` and restore the fds;
   - the new bytes go to `text` line by line (a partial last line waits), and the file is
     truncated.

   An escape restores the fds first, then disables the engine and fills `fail`. The core never logs
   during a call (IR-17).
3. **load_aig** checks a copy (literal ranges, AND order, `n_regs`), calls
   `Abc_FrameGiaInputMiniAig`, then sets the GIA's `vNamesIn` to `c<n>` for PI *n* (AB-16).
4. **exec** is `Cmd_CommandExecute`.
5. **get_luts** is `Abc_FrameGiaOutputMiniLut`. It fills `ci_from` from the current GIA's
   `vNamesIn`, parsing `c<n>`; a missing or other name gives −1.
6. **name_map** checks the counts, then calls `Abc_FrameGiaInputMiniLut2(aligned)` and
   `Abc_FrameReadMiniLutNameMapping`.
7. **close** is `Abc_FrameLeave(prev)`, then `Abc_FrameDestroy(frame)` unless an escape happened,
   then `fclose`, then unlock.

**A frame per run** (Q4, confirmed) instead of PHASE3 risk 1's single session with
`Abc_FrameDeleteAllNetworks`. That call leaves frame state behind (AB-4); `srand` is the one reset
a frame cannot give. The spike measures `Abc_FrameInit`'s cost.

### 4.5 Licence (AB-9)

`adapters/abc/NOTICE` reproduces `copyright.txt` and the notices of the third-party code compiled
into `libabc-pic`, found by 3D-2's scan of the licence headers under `third_party/abc/src`. A
licence that cannot be combined stops the PR for Peter. `libodin3` stays MIT and does not link ABC.
`odin3 --version` adds `linked ABC <commit> (adapters/abc/NOTICE)`.

## 5. What ABC sees

### 5.1 Soft cells, registers and cut cells (AB-10)

**Soft** nodes of module *M* have no non-zero `keep`, and every output net they drive has them as
its only driver. A soft cell that drives a bus (tristate or another driver) is a cut cell. Their
type is:

- `$sop` or `$lut`; or
- a **parameterless** bit type with a `simulate` hook, no `SEQ_*`, tristate or `ANYVIEW` flag, at
  most `ODIN3_ABC_GATE_MAX_INPUTS` = 6 input bits and one 1-bit output. Today these are `$_BUF_
  $_NOT_ $_AND_ $_OR_ $_XOR_ $_NAND_ $_NOR_ $_XNOR_ $_MUX_`, and 3B's gates join automatically.
  The pass runs the 1E hook once per type over all 2^n inputs (input *j* = the *j*-th input pin)
  and caches the truth table per type. That cache is sound only because such types have no
  parameters, so a parameterised bit type is a cut cell even when it has a hook.

**Not soft:**

- **constant cells**, whose node `const_value` is 0, 1, x or z (§5.2);
- **registers**, the run's domain's flip-flops (§5.3);
- **cut cells**, everything else: port nodes, hard and black-box cells, instances, level latches,
  other domains' flip-flops, tristate and bus drivers, `keep` cells, unknown or parameterised bit
  types.

A cut cell is never changed. Its outputs are ABC inputs, and its inputs are ABC outputs when soft
logic drives them (P3-13). `abc` needs the netlist view (IR-9): a word cell gives `abc: module
'<m>' is not in the netlist view: <type> '<cell>'; run lower first` (`ODIN3_ERR_CHECK`, nothing
changed). Modules run in creation order; black-box modules are skipped.

### 5.2 Inputs and outputs (AB-11)

With soft cells *S* and registers *R*:

- **Constants, per reader pin** (C2). A net driven by a constant cell is a literal for every pin of
  *S* ∪ *R* that reads it:
  - **1** for value 1;
  - **0** for value 0, x and z, and for an **undefined** net: a net named `unconn` driven by a
    zero-input, zero-row `$sop` (Parmys's convention, 2D §14.6 item 3; its `const_value` is 0
    anyway).

  This is what VTR's ABC does with Parmys's `unconn`, and it is 3E §14.1 item 4: no `.names` or
  `.latch` pin is left on `unconn` after `abc`. Info line: `abc: module '<m>': N soft-logic pins
  read x, z or unconn as 0`.
- **The bypass** (3E §14.1 item 2). A cut-cell pin (hard block, black box, port) whose net is
  driven directly by a constant, x or `unconn` generator is outside every run. It keeps its net, so
  `ch_intrinsics`' `addr[5..14]=unconn` survive. Generator cells are never deleted by `abc`, and no
  undefined net is merged into another net.
- **CIs.** The nets that a data pin of *S* ∪ *R* (a soft input, a register's `D`) reads, whose
  driver is outside *S* ∪ *R* and is not a constant cell. Undriven nets are free inputs, and a bus
  net is read as a CI.
- **Register outputs** are the `Q` nets of *R*.
- **COs.** The nets driven by *S* ∪ *R* that are:
  - read by a cut cell's pin (this covers `$port_out` and `$port_inout`);
  - read by a register's clock pin (a derived clock keeps its net); or
  - carrying a non-zero `keep` on the net or one of its wires. As in Yosys, a `keep` net becomes
    an output and soft readers still read it.

  A net driven by *S* that nothing reads is dead.
- **Register inputs** are the `D` nets of *R*. They are identity anchors (§8.2).
- **Order.** CIs in net-ID order, registers in node-ID order, COs in net-ID order (IR-16). One sweep
  builds all three in O(nodes + pins).
- **Loops.** Kahn's algorithm orders *S*, ties by node ID. When it stalls, the remaining cell with
  the lowest ID has its output net **cut**: a CO (its function kept) and a CI (readers see a free
  input), with the warning `abc: module '<m>': combinational loop through <type> '<cell>'; its
  output <net> is a cut point`. The loop survives the splice, structurally and functionally.

### 5.3 Clock domains: one run each (AB-12)

- **Key.** `$_DFF_P_`/`$_DFF_N_` give (clock net ID, edge); `$_FF_` gives the global domain.
- **Order.** By the clock net's name bytes (own name, else primary wire bit, else empty), then P
  before N, then net ID; the global domain last. VTR's sorted `latch|<type>|<clk>|<init>` keys give
  the same order when the latches share one init value, as all 19 do (P3-12).
- **Runs.** One run per domain, in order. A run sees every soft cell of *M* (including earlier runs'
  `$lut`s) plus that domain's flip-flops; other domains' flip-flops are cut cells. A module without
  flip-flops gets one run. This is VTR's `iterative_bb` (P3-13 a); IR re-stitching makes
  `restore_outputs_from_blif` and `CLOCK_GATING` unnecessary.
- **Stability** (I13). Domains are computed once per module, before the first run, as **sets of
  flip-flop node IDs**, not by clock net ID. A new flip-flop takes its clock from its
  representative's `C` pin, read during the splice. Phase B never drops a clock net (§6.5). Later
  runs see run *k*'s new flip-flops as cut cells. Nothing is compacted between runs (IR-6).
- **Cap.** `ODIN3_ABC_MAX_DOMAINS` = 64 per module; `stereovision3` is the 19's only two-clock
  design.

### 5.4 Latches and init values (AB-13)

**One encoding on both exchanges** (C1). Every register handed to ABC has init 0, because DAR and
`&get` relabel any other init (AB-4).

| `INIT` | Sent to ABC (both exchanges) |
|---|---|
| 0 | as is |
| 1 | **inverted**: ABC holds ¬Q, reads ¬D, init 0 (P3-12) |
| 2, 3 | 0, as VTR does |

In the BLIF exchange, an inverted register *j* is written as `.names <D> o3d<j>` / `0 1`, then
`.latch o3d<j> o3q<j> 0`, and its readers read `o3r<j>` from `.names o3q<j> o3r<j>` / `0 1`. The
in-memory AIG complements the same literals (AB-16).

Retiming is refused (AB-21). ABC never adds registers: a result with more registers than were sent
is a `RESULT` failure. `scorr` may merge registers, or replace one with a constant or with logic.

- **Correspondence by name, on both exchanges** (I11). Result register *j* corresponds to input
  register *k* when its output carries *k*'s name:
  - BLIF: `o3q<k>`;
  - in-memory: `c<n>` for *k*'s PI, through `ci_from`.

  `scorr` keeps the names of the latches it keeps. The exchange names never equal a PO name, so
  `scorr`'s dummy-name branch (`abcDar.c:563`) cannot trigger. The named register *k\** is *j*'s
  **representative**. A result register with no input name is a `RESULT` failure: `register J
  carries no input register's name`.
- **The new flip-flop.**
  - Its type and clock come from *k\**'s domain and its `C` pin.
  - `INIT` is 2 when `INIT(k*)` is 2 or 3, and `INIT(k*)` otherwise (P3-12: written back as 2).
  - Its node name is *k\**'s, and *k\**'s old `Q` net merges into the new one (§6.5).
- **Restore** (both exchanges, when `INIT(k*)` = 1). *j* holds ¬Q. The splice:
  - sets `INIT` 1;
  - flips the truth table of every new LUT that reads *j*, in that variable;
  - drives a CO that reads *j* directly through a 1-input NOT `$lut`, unless it reads *j* through
    an inverter LUT, which then becomes a buffer and so a merge;
  - complements a `D` source LUT that feeds only *j*, and gives any other `D` source a NOT (a
    constant flips).

  It is exact: Q_k\*,ir = ¬Q_j,abc, and the restored *j* holds that, from INIT 1, with the same
  register count.
- **Test** (C1): `test_abc_init1` (§13.1).

## 6. Exchange

### 6.1 One result form (AB-14)

Both exchanges give the splice and provenance code the same `abc_result` (`result.[ch]`), numbered
like MiniLUT:

- objects 0 and 1 are the constants;
- then the CIs as sent: real inputs, then the register outputs (*n_regs_out*), each with its
  `from` index (AB-13);
- then the LUTs in topological order, each with up to 16 lower-numbered fanins and a truth table.

Each CO (the real outputs, any observation outputs, then the *n_regs_out* register inputs) has a
driver object. An optional array maps objects to old AIG literals (§8.2).

**Checks**, each failure being `abc: ABC returned a malformed network: <why>` (`ODIN3_ERR_ENGINE`,
§11):

- real I/O counts are as sent;
- *n_regs_out* ≤ *n_regs_in*;
- every register has a name;
- fanins precede their LUT;
- at most 16 inputs per LUT, and at most K with `--lut K`;
- in the BLIF exchange, every latch init is 0.

**Constant folding** (I10). A LUT whose table is all-0 or all-1 over its 2^*k* bits becomes object
0 or 1. Its readers are rewired, and a reader LUT gets that variable's cofactor of its table.

Cost: O(objects + fanins + LUT table words).

### 6.2 BLIF exchange, `--via-blif` (AB-15)

This is P3-9 (b): proven bring-up code, then the differential and debug path. Until 3D-7 lands it is
also the default, in a `mkdtemp` directory `odin3-abc-XXXXXX` under `$TMPDIR` (else `/tmp`) that the
pass removes afterwards.

- **Directory.** `<dir>` is created when missing; files are overwritten. Its absolute path is
  refused (I12) if it contains:
  - any byte below 0x20, or 0x7F (`CmdSplitLine` turns tabs into spaces even inside quotes);
  - `"` or `'`;
  - `;` or `#`.
- **Files of run *r*** (numbered from 0 across the invocation): `abc<r>.in.blif`, `abc<r>.cmd` (the
  commands as run), `abc<r>.log` (ABC's output, with `-v` timings: never compared, §9) and
  `abc<r>.out.blif`.
- **Input.** `.model o3abc`, `.inputs`, `.outputs`, the cells in topological order, the inverters
  and `.latch`es of AB-13, `.end`. One exchange name per net:

  | Net | Name |
  |---|---|
  | CI *i* | `o3ci<i>` |
  | register *j*'s ABC output; its IR value if inverted; its inverted input | `o3q<j>`; `o3r<j>`; `o3d<j>` |
  | CO *i* | `o3co<i>` |
  | constants as soft readers see them (0, x, z and `unconn` → `o3c0`) | `o3c0` (no rows), `o3c1` (row `1`) |
  | other | `o3n<net ID>` |

  - A `$sop` keeps its rows; gates and `$lut`s write `odin3_blif_isop`'s cover (AB-3).
  - A CO that is also a register's IR value gets a buffer `.names o3q<j>|o3r<j> o3co<i>` / `1 1`,
    so a PO name never equals a latch output name.
  - Writing costs O(region pins + rows).
- **Commands** (AB-22): `read "<abs in>"`, the script, `&put` if the script ends in the `&` space,
  `write_hie "<abs in>" "<abs out>"`. These are VTR's stage's commands (P3-9 b; 3E §14.1 item 1).
  The `&` space starts empty here, so the checker refuses an `&` command other than `&get` before
  the first `&get` (AB-21).
- **`restore_outputs_from_blif` is not needed.** VTR needs it because `write_hie` drops `.outputs`
  that only feed black-box inputs. Our model has no `.subckt`, so nothing is dropped, and the
  read-back's output check is the guard.
- **Read-back.** 1C's reader reads the output into a scratch design (`odin3_design_create`, then
  destroyed).
  - The inputs and outputs must be exactly the `o3ci*`/`o3co*` names sent, in any order.
  - Latches are identified by their output name `o3q<k>`.
  - Each `$sop` becomes a truth table, evaluated from its cover; more than 16 inputs is `RESULT`.
  - A one-input `1 1` `.names` is a buffer and becomes a connection; a `.subckt` is `RESULT`.
  - Kahn's algorithm (ties by node ID) gives the order.

  Read-back costs O(pins + rows · 2^k/64).
- **Anchors**: CIs and COs by name, registers by `o3q<k>`, and register inputs (§8.2).

### 6.3 In-memory exchange: MiniAIG in, MiniLUT out (AB-16)

This is P3-9 (a), the production path: no files, no 100 MB BLIF parse, and the name map. It lands in
3D-7 once the spike says go (AB-17).

**The AIG** (`aig.c`, MiniAIG layout):

- **Objects.**
  1. Object 0, then one PI per CI, then one PI per register output.
  2. The ANDs in topological order.
  3. The POs: the COs; then, only when the spike forces it, one observation output per register;
     then the register inputs.
- **Literals.**
  - A CI gets its PI. A register `Q` gets its PI, complemented when inverted (AB-13). A constant gets
    0 or 1 under AB-11's per-pin rule.
  - A soft output is built from its input literals:
    - gates and `$lut`s use `odin3_blif_isop`'s cover of their table (an off-set cover complements
      the output), and a `$sop` uses its rows;
    - each cube is a balanced AND over its literals in input order;
    - the cover is a balanced OR (an AND of complements) over its cubes in row order. "Balanced"
      means pairwise, left to right, level by level, as `Mini_AigAndMulti` does.
  - A register input gets `D`'s literal, complemented when inverted.
- **Hashing.** A `u64map` keyed by `lit0 << 32 | lit1` (`lit0 < lit1`) applies a·0 = 0, a·1 = a,
  a·a = a and a·¬a = 0. Every AND records its **owner**, the first soft cell that made it, and every
  soft output net records its literal.
- **Cost.** O(Σ cover literals) expected. Beyond `ODIN3_ABC_MAX_AIG_OBJS` = 2^26 objects (a 512 MB
  int32 array) the run fails; MiniAIG's own ceiling is 2^29.

**Session** (AB-22): `open`; `load_aig` (PIs named `c<n>`); `&put`; the script; `&get -m -n` if the
script ends in the network space; `get_luts` (with `ci_from`); `name_map` (fine provenance only);
`close`.

**Register correspondence** (I11): by ABC's names, as in the BLIF exchange. `&put` puts the GIA's PI
names on the network, `scorr` keeps surviving latches' names, and `&get -n` brings the CI names back
(AB-4), so `ci_from` names each result register's input register. This costs nothing in the network
ABC optimises.

**Observation outputs** (one PO per register, driven by its PI) are only the spike's fallback.
Every command sees them (`mfs2` loses the register outputs' ODCs, `if` and `dch -f` see extra
fanout), so they are used only if names fail and their measured cost is ≤ 1% (criterion 8).

**The name map** needs equal CI and CO counts, and `scorr` changes the register count. The core
therefore builds an **aligned MiniLUT** with the AIG's interface:

- the PIs as sent, with one register output per *input* register *k*;
- the new LUTs, with each fanin from result register *j* remapped to *k\**(*j*);
- the real outputs as in the result;
- register inputs: *j*'s `D` driver for *k* = *k\**(*j*), constant 0 for any other *k*.

`name_map` gives, per aligned object, the MiniAIG literal ABC **proved** equivalent, or −1. Anchors
are therefore sound. They are **combinational**: CIs are free, so logic equal only on reachable
states is not matched, and coverage after `scorr` is lower (minor 15). The name map is skipped
above `ODIN3_ABC_NAMEMAP_MAX_OBJS` = 2^23 AIG objects and under `--prov cone`. A `RESULT` or
`COMMAND` failure there falls back to cones; it is not fatal.

**Consistency with P3-9, P3-12 and P3-13.**

- P3-9 (a) names `Abc_NtkInputMiniAig`. Entering through `Abc_FrameGiaInputMiniAig` and then `&put`
  puts the same AIG into the network space, where VTR's script runs unchanged, and keeps the
  references the name map and names need. Spike criterion 7 checks that both entries agree.
- P3-12 holds: init-1 registers inverted, DC as 0, written back as 2.
- P3-13 holds through §5: cut points, and one run per clock domain.

**The two exchanges build different AIGs** (I8). ABC's `read` plus `strash` factors each BLIF cover
(`Dec_Factor`); the in-memory path builds balanced ISOP trees. Part of any P′–P gap is therefore the
entry path, not the ABC version. Ruling:

- **QoR is measured on the in-memory exchange**, the production path.
- `abc-diff` runs on the 19 (heavy, controller) **before** 3D-7 makes in-memory the default, and
  records per-design LUT and depth ratios in PHASE3.
- 3H's tables name the exchange each P′ row used.
- M1 is reached on the BLIF exchange, whose covers are Parmys's own, so it is the closest to VTR's
  stage.

### 6.4 The spike gate (AB-17)

3D-0 is a throwaway program in `~/odin3-ws/work/spike-abc/`, linked to `libabc-pic`. Its findings go
into PHASE3.

**Inputs**: `ch_intrinsics`, `stereovision3`, `diffeq2`, `diffeq1` and `mkPktMerge`, smallest
first, plus two micros. These are small, so no heavy run. Each golden's MiniAIG comes from ABC
itself, so hard blocks are cut as VTR's stage cuts them, independent of 3D-5's region code: `read`
with the hard models as `.blackbox`, then `strash`, then `Abc_NtkOutputMiniAig`.

3D-7 goes ahead only if:

1. **`&get -m -n` keeps latches, mapping and names** (P3-9). `Abc_FrameGiaInputMiniAig; &put;
   <default script>; &get -m -n; Abc_FrameGiaOutputMiniLut` gives the input's PI and PO counts, at
   most its registers, K-bounded LUTs, and is equivalent (`dsec` on BLIF).
2. **Names survive**: every result register's name parses back to an input register, through the
   default script, a `scorr` that merges registers, and each `&` command of §7.5 (`&st`, `&synch2`,
   `&dch`, `&if`, `&mfs`).
3. **The name map works** on the aligned MiniLUT. Its coverage is recorded separately for
   combinational and sequential designs, and its time must be ≤ 15% of the script's.
4. **Frames are clean**: 1,000 create/destroy cycles with no LSan growth and identical results; the
   per-frame cost is recorded.
5. **The guard works**: a test command calling `exit(3)` on the session thread returns through it,
   with the fds restored.
6. **Determinism**: two processes, and two frames in one process, give identical MiniLUTs, both for
   the default script and for `strash; fraig; if -K 6` (`srand`).
7. **The entries agree**: `Abc_NtkInputMiniAig` and `Abc_FrameGiaInputMiniAig` + `&put` give
   identical MiniLUTs.
8. **Observation outputs' cost**: LUT count and depth on the five, with and without them.

**Fallbacks:**

- 1 fails: the output side reads the mapped `Abc_Ntk_t` through `base/abc/abc.h` (P3-9 c, output
  only; for Peter).
- 2 fails for some `&` commands: the checker refuses them in scripts. If it fails for the default
  script, use observation outputs provided criterion 8 shows ≤ 1%, else ask Peter.
- 3 fails: cone provenance only.
- 4 fails: one frame per process with `Abc_FrameDeleteAllNetworks` between runs, and determinism
  checked again.
- 7 fails: `Abc_NtkInputMiniAig` (P3-9 a as written) with cone provenance; names are then set on
  the network through `abc.h`.

### 6.5 Splice (AB-18)

Records come first, because creating an object needs its provenance ID (IR-13).

**Phase A** (allocates; changes no existing object):

1. Fold constants (AB-14), compute anchors, regions and records (§8), and apply the restore flips
   (AB-13).
2. Create fresh nets for the LUT outputs and the new `Q`s.
3. Create the `$lut` nodes. Their `A` pins go to CI nets, new nets, or the **canonical constant
   net**: the lowest-ID net driven by `$_CONST0_`/`$_CONST1_` or a constant `$sop` that is not
   undefined, else a new `$_CONST0_`/`$_CONST1_` node.
4. Create the new flip-flops: `C` from the representative's `C` pin, `D` the new driver, `INIT` as
   in AB-13.

`NO_MEMORY` deletes what phase A created, in reverse order (records are immutable and stay); the IR
is as before.

**Phase B** (re-stitch; every step is atomic and keeps the function). For each CO *c* in order,
with new driver *d*: first disconnect *c*'s old driver pin, then:

| *d* | *c* ordinary | *c* **special** (a clock net, a port net, or `keep`) (I13) |
|---|---|---|
| a fresh net (LUT or `Q`) | `merge(keep = c, drop = d)` | the same |
| a net already carried by an earlier CO *c′* | `merge(keep = c′, drop = c)` | a 1-input buffer `$lut` from *c′* drives *c* |
| a CI net | `merge(keep = d, drop = c)` | a buffer `$lut` from *d* drives *c* |
| constant 0/1 | `merge(keep = canonical constant net, drop = c)` | a port or `keep` net: the same merge, and 3E writes the buffer from `gnd`/`vcc` (3E §9.2); a clock net: a buffer `$lut` from the constant net, so the net survives |

A special net is never dropped, so no domain loses its clock and no two port nets merge. VTR's own
output has these buffers too (`.names clk1 clk2` / `1 1`).

Names survive as aliases (IR-2). If a merge fails with `NO_MEMORY`, the old driver pin goes back on
*c*; that needs no allocation, because *c* just lost a pin. Unvisited COs keep their old drivers, so
the IR stays valid and equivalent. A buffer `$lut` is created in phase A, with the other new nodes.

**Phase C.**

1. Delete the old soft cells and the domain's old flip-flops (slots stay until `compact`).
2. In old-net-ID order, merge each old internal net that has an uncomplemented anchor into the net
   its anchor's object now drives (keep = new), so names and wire memberships survive.
3. Merge each *k\**'s old `Q` net into the new one, unless phase B already did.

`NO_MEMORY` here leaves the function complete, with fewer names moved.

**Phase D** (cleanup, no allocation):

- a worklist removes new LUTs and flip-flops with no readers;
- then non-port nets with no pins, wire memberships or aliases;
- then non-port wires whose nets all have no pins.

A wire with some live and some pinless bits keeps them, and `check` warns, as after 2F's
`opt_clean`.

**Cost:** O(new objects + their pins + moved pins + old cells). A failure in run *k* leaves the runs
before *k* applied and the IR valid and equivalent. The pass manager compacts after `abc` (IR-6),
using 2F-5's hook for `opt`.

## 7. The `abc` pass

### 7.1 Arguments (AB-19)

```
abc [--script <file>] [--lut <K>] [--via-blif <dir>] [--log <file>] [--prov fine|cone]
```

The help line, exactly: `abc [--script FILE] [--lut K] [--via-blif DIR] [--log FILE] [--prov
fine|cone]: map each module's soft logic to K-input LUTs with linked ABC (one run per clock
domain)`.

Split by `odin3_pass_arg_next`. Each option may be given once. Paths resolve against the working
directory.

| Option | Meaning | Errors |
|---|---|---|
| `--script <file>` | this run's script (AB-20) | no value; unreadable (`ODIN3_ERR_IO`) |
| `--lut <K>` | replaces `{K}`; every result LUT has ≤ K inputs | not an integer from 2 to 16 |
| `--via-blif <dir>` | BLIF exchange in `<dir>` (AB-15) | a byte refused by AB-15 in the path; cannot create (`IO`) |
| `--log <file>` | ABC's raw output, truncated at pass start, each run headed `# abc run <r>: module '<m>', <domain>` (agent default) | cannot open (`IO`) |
| `--prov fine\|cone` | `cone` skips the name map; default `fine` (agent default) | another value |

`abc` is in the built-in table (`src/passes/builtin.c`), with `wants_ast` false. PASSES.md gets
this row:

| Pass | Phase | Input view → output view | Source | Golden test | Notes |
|---|---|---|---|---|---|
| `abc [--script F] [--lut K] [--via-blif D] [--log F] [--prov fine\|cone]` | 3 (3D) | netlist → netlist | `src/passes/abc/*.c`, `adapters/abc/` | `test_abc_*`, `abc_equiv_micro`, `cli_abc` | One ABC run per clock domain per module. Hard cells, black boxes, instances, level latches and other domains' FFs are cut points; hard pins on `unconn`/x/constants pass unchanged, soft readers see 0. Script: `--script` > `--abc-script` > the project's `abc_script` > `lib/abc/vtr.abc`, checked before ABC starts. Soft cells become `$lut`. Provenance from ABC's correspondence. Compacts after. |

### 7.2 Script precedence and identity (AB-20)

This implements P3-1. The first that applies wins:

1. `abc --script` (this run only).
2. The project's `abc_script`, which `read_project` has already replaced with the CLI's
   `--abc-script`, logging `<entry>: --abc-script <new> replaces the project abc_script <old>` when
   the resolved paths differ.
3. The CLI's `--abc-script` when no project was read (`odin3_pass_options.abc_script`, set through
   the new ABI call `odin3_pass_set_abc_script`).
4. `lib/abc/vtr.abc`, named `builtin:lib/abc/vtr.abc`.

**Identity** (I7; 3E §14.1 item 5). The run information records the path **exactly as given** (the
CLI or project word, or `builtin:lib/abc/vtr.abc`), never a path resolved against the working
directory, together with the SHA-256 of the bytes read. The SHA-256 is the identity, so goldens and
3H's tables are byte-identical across checkouts. The resolved path is logged at INFO only, on the
first line: `abc: script <as given> (<resolved>), sha256 <16 hex>…, K=<K|none>, exchange
<aig|blif>, ABC <12 hex>`.

### 7.3 Script language and checker (AB-21)

The syntax is ABC's: `;` or a newline between commands, `#` to the end of the line, and one
substitution, `{K}` (P3-1). `script.c` (pure C, no ABC) checks the whole script once per
invocation, before ABC starts. Every error is `ODIN3_ERR_PARSE`, located `<file>:<line>` with the
file as given, and nothing changes.

1. **Bytes.**
   - `<file>: abc script is larger than 65536 bytes`.
   - LF or CRLF line ends.
   - `<file>:<line>: abc script contains a NUL byte`.
   - Outside comments only printable ASCII and tab: `<file>:<line>: byte 0x<hh> is not allowed
     outside a comment`.
   - `<file>:<line>: line longer than 4096 bytes`.
2. **Comments.** `#` drops the rest of its line.
3. **Quotes and shell.**
   - `"` or `'`: `<file>:<line>: quotes are not allowed in abc scripts`.
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
     end, and the result is re-split at `;` (`CmdApplyAlias`: extra words go to the last command).
   - Expansion repeats to at most 8 levels: `<file>:<line>: alias '<a>' expands more than 8 levels
     deep`.
   - Each command keeps its line and its outermost alias.
8. **Allow-list** (§7.5). The error is `<file>:<line>: abc command '<cmd>'[ (from alias '<a>')]`
   plus its class's ending:
   - `is not allowed: abc scripts cannot read or write files`;
   - `is not allowed: abc scripts cannot control the ABC process`;
   - `is not allowed: it retimes or changes initial values (PHASE3 #12)`;
   - `is not an allowed ABC command (3D spec §7.5)`.
9. **Arguments.**
   - Each argument is an option `-[A-Za-z][A-Za-z0-9]*` or a number
     `[-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?`; `echo` takes any printable word without `{`, `}` or
     `!`.
   - Otherwise: `<file>:<line>: argument '<w>' of '<cmd>' is neither an option nor a number`.
   - **File and string options** (I12). `allow.c` lists, per allowed command, the options whose
     value is a file or a string, read at the pin from each command's `Extra_UtilGetopt` string and
     usage text. Those are refused even with a numeric value (a numeric word can name a file in the
     working directory): `<file>:<line>: option '-<x>' of '<cmd>' takes a file or a string and is
     not allowed`.
10. **Count.** More than 1,024 commands after expansion: `<file>:<line>: script expands to more
    than 1024 commands`.
11. **Spaces and mapping.** `ntk` (the `Abc_Ntk` space) and `gia` (the `&` space) are each
    *empty*, *aig* or *mapped*.
    - At the start, in-memory: both *aig*. BLIF: `ntk` *aig* and `gia` *empty*.
    - §7.5 gives each command's effect. The current space is that of the last command that changes
      or moves a network, starting at `ntk`.
    - An `&` command other than `&get` on an empty `gia`: `<file>:<line>: '<cmd>' needs a network
      in the & space, which the BLIF exchange starts empty; add '&get'`.
    - The script must end with its current space *mapped*: `<file>: the script leaves the network
      unmapped; end it with a LUT mapper such as 'if -K {K}'`. An empty script ends unmapped.
12. **Output.** An `odin3_abc_script`: the expanded lines (each NUL-terminated, with its line and
    alias), `uses_k`, the final space and the SHA-256.

The cost is O(bytes + expanded words).

**Checked is what runs.** The expanded lines hold no quote, `!`, `;`, `#`, `{`, `}`, alias or file
option, so ABC's `CmdSplitLine` sees exactly the checker's words. §13.2 tests this through a hook in
`bridge.c`.

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

Agent defaults: the `print_*` set as listed, and `&ps` as the `&`-space print. Per command, options
whose value is a file or a string are refused too (AB-21 step 9).

Refused, among others:

- **files:** `read*`, `write*`, `&r`, `&w`, `source`, `load`, `save`;
- **process:** `quit` (which returns −1 and does not call `exit()`, `cmd.c:386`), `exit`, `cd`,
  `ls`, `alias`, `unalias`, `set`, `unset`, `history`, `load_plugin`;
- **latches:** `retime`, `dretime`, `fretime`, `zero`, `init`, `undc`, `cycle` (P3-12).

The classes are a table in `allow.c`; an unlisted name is "other".

### 7.6 Command protocol (AB-22)

| Exchange | Calls and commands, in order |
|---|---|
| in-memory | `open`; `load_aig` (PIs named); `&put`; the script; `&get -m -n` if the final space is ntk; `get_luts` (with `ci_from`); [`name_map`]; `close` |
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

These are constants in `src/passes/abc/abc.h`, lowered in tests through `odin3_abc_test_set_limits`
(`abc_test.h`).

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
| design byte budget | the smaller of 60% of physical memory (2E §16) and the cgroup's `memory.max`, when `/sys/fs/cgroup/<own cgroup>/memory.max` holds a number (minor 16) | `abc needs more than N bytes (the design's budget is <B> bytes)` (`PARSE`) |

**ABC's memory.** Its allocations are unchecked (AB-4), so before each run the pass calls 2E-1's
`odin3_budget_check(ctx, design, extra)`. 3D asks 2E-1's helper to apply the cgroup bound; if 3D-5
lands first, it lands `src/passes/budget.[ch]` as 2E §16 specifies, with that bound. The `extra`
term is:

- `ODIN3_ABC_BYTES_PER_AIG_OBJ` × AIG objects. It starts at 512; `bench_abc` calibrates it to 1.5
  × the worst measured ratio on the 19, and it only goes up.
- Plus the exchange arrays.
- Plus two AIGs' worth with the name map.

**OOM in the core.** Any allocation can return `NO_MEMORY`. The pass frees what it holds and returns
a valid, equivalent IR (AB-18). Tests inject a failure at every allocation.

## 8. Provenance (AB-25)

### 8.1 The pass-run record

P3-14 has every run record its arguments. 3D adds **run information**, the facts a run established.
`abc` writes:

`script=<path as given> sha256=<64 hex> abc=<40 hex> K=<K|none> exchange=<aig|blif> depth=<d>`

- `exchange=` and `depth=` are agent additions; `depth=` answers 3E §14.1 item 6, 3H's request.
- `<d>` is the largest number of `$lut` levels on any path between the top module's CIs and COs
  (ports, flip-flops, cut cells) after the pass. The core computes it from the IR, so 3H never
  parses ABC's text.

§8.5 has the exact format.

```c
/* src/ir/prov.h */
odin3_status odin3_pass_run_begin_args(odin3_design *design, uint32_t pass_name_str,
                                       uint32_t args_str, odin3_pass_ctx *ctx);  /* 0: none */
uint32_t odin3_passrun_args(const odin3_design *design, odin3_passrun_id run);  /* 0: none */
uint32_t odin3_passrun_info(const odin3_design *design, odin3_passrun_id run);  /* 0: none */
/* INVALID_ARG (logged): already set, run 0 or unknown, or info_str 0 or not a strtab ID. */
odin3_status odin3_passrun_set_info(const odin3_pass_ctx *ctx, uint32_t info_str);

/* odin3.h (under the ABI v4 changelog line; no bump) */
odin3_status odin3_design_get_passrun_count(const odin3_design *design, uint32_t *count);
/* Name, arguments and run information of run (1 .. count - 1); "" for none. INVALID_ARG for run 0
 * or past the end. Strings as the conventions say. */
odin3_status odin3_design_get_passrun(const odin3_design *design, uint32_t run, const char **name,
                                      const char **args, const char **info);
```

- `odin3_pass_run_begin` keeps its meaning, with args 0.
- The manager's `open_run` interns the trimmed arguments; empty arguments give 0.
- A run's row becomes `{name, args, info}`.
- 3E writes `# abc: <run information>` once per `abc` run (3E §5 item 1), read through
  `odin3_passrun_info`.

### 8.2 Anchors

An anchor pairs a result object with an old net that computes the same function.

1. **Identity** (C3). Every CI, every CO and **every register input** of the run, each with its own
   old net. A register input's old net is the old `D` net, and its new object is the new `D`'s
   driver, so every `D` cone gets parents.
2. **Registers.** Each new register with the old flip-flops whose names it carries (AB-13).
3. **Name map** (in-memory, `--prov fine`, within the cap). Each LUT whose aligned object ABC maps to
   a literal ℓ, with the lowest-ID old net *n* such that `lit(n)` is ℓ or ¬ℓ. A complemented
   anchor counts for provenance but never moves names.

Without a name map, the info line is `abc: module '<m>', <domain>: no name map (<reason>);
provenance uses cut cones`. The reason is one of `--prov cone`, `BLIF exchange`, `the AIG has more
than 8388608 objects`, or `ABC: <fail text>`.

### 8.3 Regions, records and cost

The rules are §8.5's. Implementation:

- **Walks.** Iterative, with an explicit stack and per-anchor visit stamps.
- **Records.** One `odin3_prov_begin_op` and one `odin3_prov_derive` per anchor. The IR deduplicates
  parents, so the hundred gates of one lowered `$add` add one parent.
- **Anchor order** for multi-region LUTs: COs, then register inputs, then registers, then objects.
  Output nets carry their driver's record.
- **Cost.** O(Σ old regions + Σ new regions + new objects). Identity anchors alone (output and `D`
  cones) can overlap quadratically, so the edge count is summed before any record is made, and
  over the budget the run makes one record.

### 8.4 Compact and tombstones

Old cells die in phase C. Their slots, then their tombstones after `abc`'s compact, keep them
reachable from the new records (IR-6). With register inputs as anchors, every old soft cell that
reaches a CO or a register input has a child. Only logic that reaches neither, which no output
observes, dies without children.

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
> `script=<path> sha256=<64 hex> abc=<40 hex> K=<K or none> exchange=<aig|blif> depth=<d>`: the
> script's path exactly as given (`builtin:lib/abc/vtr.abc` for the default; never resolved
> against the working directory, so that the record is the same on every machine), the SHA-256 of
> the bytes it read (its identity), the commit of the linked ABC, the LUT size and the top
> module's LUT depth after the pass; `write_blif --vtr` repeats it in a header comment.
>
> **Provenance across ABC (3D, PHASE3 #14).** `abc` replaces a region of soft cells and flip-flops
> by LUTs and flip-flops that ABC made, and keeps lineage through **anchors**: pairs of a new
> object and an old net that compute the same function. They are every input, output and register
> input of the run (by identity; a register input pairs the old `D` net with the new `D`'s
> driver); every new flip-flop with the old flip-flops whose names ABC kept on it (sequential
> correspondence); and, with the in-memory exchange, every new LUT whose output ABC's name map
> proves combinationally equivalent to an old net, up to complement. An anchor's **old region** is
> the set of old soft cells in the fan-in cone of its old net, up to other anchors' nets and the
> run's inputs (for a flip-flop anchor, its old flip-flops); its **new region** is the set of new
> LUTs in the fan-in cone of its new object, up to other anchored objects and the run's inputs.
> Each anchor is one operation, whose `DERIVED` record has as parents the records of its old
> region's cells, in node-ID order. A new LUT in one new region carries that record; one in several
> carries the record of one more operation, whose parents are those regions' records in anchor
> order. When a run's parent edges would exceed 16 × (old cells + new objects) + 65,536, the run
> makes one record whose parents are all its old cells and flip-flops, every new object carries
> it, and a warning says so. With identity anchors only (the BLIF exchange, `abc --prov cone`, or
> no name map) a region is the cut-bounded cone of an output or a register input. An old cell that
> reaches no output and no register input had no observable effect; it dies without children.

**In §11** add: "Under PHASE3 #9 and #14 (3D spec): the `$lut` cell, the pass-run table's arguments
and run information, and provenance across ABC (IR-12)."

## 9. Determinism (AB-26)

Two processes, and two runs in one process, write byte-identical netlists (`write_blif`) from the
same input, script and build. In the BLIF exchange, `abc<r>.in.blif`, `.cmd` and `.out.blif` are
identical too.

**Never compared:** `abc<r>.log`, `--log` and the INFO run line's milliseconds, which carry `-v`
timings (minors 14 and 25).

| Source of variation | Guard |
|---|---|
| IR iteration | ID order only (IR-16); sorted CIs, COs, registers, domains; Kahn ties by node ID; no hash-map iteration reaches the AIG, a file or the IR |
| AIG shape | 3E's deterministic cover; balanced trees, left to right; the first owner wins |
| ABC state between runs | a frame per run (AB-8) |
| ABC randomness | every ABC generator and libc `srand(1)` reset at `open` (I5); spike criterion 6; §13.8 (b) with a `fraig` script |
| ABC time limits and threads | none in the default script (the spike checks; a test build counts `pthread_create`); a user script that sets `-T` or threads is documented as not reproducible |
| Splice and provenance | fixed phases; merges in CO order, then old-net-ID order; records in anchor order |
| Debug against Release ABC (assertions on or off) | §13.8 (d) |
| Machine | the run information holds the script path as given and no absolute path (AB-20) |

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
   >     instances and other domains' flip-flops as cut points (hard-block inputs on `unconn`, x or a
   >     constant pass unchanged; soft logic reads them as 0); the script is `abc --script`, else `--abc-script`, else the project's
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

Style follows 2D §9. `<domain>` is `clock '<net>' posedge|negedge`, `global clock` or `no
flip-flops`. `<file>` is the script path as given.

| Level | Exact text | Status / when |
|---|---|---|
| INFO | `abc: script <as given> (<resolved>), sha256 <16 hex>…, K=<K\|none>, exchange <aig\|blif>, ABC <12 hex>` | start |
| INFO | `abc: module '<m>': cut points: <h> hard or black-box cells, <i> instances, <l> level latches, <t> tri-state or bus cells, <o> other cells` | any count non-zero |
| INFO | `abc: module '<m>': N soft-logic pins read x, z or unconn as 0` | N > 0 |
| INFO | `abc: module '<m>', <domain>: <c> cells and <f> flip-flops -> <l> LUTs (at most <k> inputs, depth <d>) and <g> flip-flops, <ms> ms in ABC` | end of each run |
| INFO | `abc: final: depth <d>` (3E §14.1 item 6) | end of the pass |
| INFO | `abc: module '<m>', <domain>: no name map (<reason>); provenance uses cut cones` | §8.2 |
| INFO / DEBUG | `abc\| <line>` | ABC output: INFO for `print_*`, `&ps`, `time`, `echo`; DEBUG otherwise |
| WARN | `abc: module '<m>': combinational loop through <type> '<cell>'; its output <net> is a cut point` | §5.2 |
| WARN | `abc: module '<m>', <domain>: provenance needs N parent edges (budget M); the run's LUTs share one record` | §8.3 |
| WARN | `odin3: linked ABC unavailable: <status>` | CLI start |
| ERROR | `abc: unknown argument '<arg>'`; `abc: <option> given twice`; `abc: <option> needs a value`; `abc: --lut must be an integer from 2 to 16, not '<v>'`; `abc: --prov must be fine or cone, not '<v>'`; `abc: --via-blif directory '<dir>' contains a control character, a quote, ';' or '#'; ABC cannot read it` | `INVALID_ARG` |
| ERROR | `abc: <path>: cannot read\|create\|open …: <strerror>` | `IO` |
| ERROR | `abc: module '<m>' is not in the netlist view: <type> '<cell>'; run lower first` | `CHECK` |
| ERROR | AB-21's script errors; AB-24's cap errors | `PARSE` |
| ERROR | `abc: linked ABC is not available (no engine registered; build with ODIN3_WITH_ABC=ON)` | `ENGINE` |
| ERROR | `abc: <file>:<line>: command '<cmd>' failed`[` (from alias '<a>')`]; `abc: internal command '<cmd>' failed` | `ENGINE`, `COMMAND` |
| ERROR | `abc: ABC called exit(<code>) during '<cmd>'; linked ABC is disabled until odin3 restarts`; likewise `ABC aborted during …` and `ABC assertion failed during '<cmd>': <text>; …` | `ENGINE`, `EXIT`/`ABORT`/`ASSERT` |
| ERROR | `abc: linked ABC is disabled by an earlier failure in this process`; `abc: linked ABC is busy (another session is open)` | `ENGINE`, `DISABLED`/`BUSY` |
| ERROR | `abc: ABC returned a malformed network: <why>`, `<why>` ∈ `N primary inputs, expected M`; `N outputs, expected M`; `N registers, more than the M it was given`; `register J carries no input register's name`; `a latch with init <v> (all were sent at 0)`; `a LUT with N inputs (at most 16)`; `a LUT with N inputs exceeds --lut K`; `an unknown output '<name>'`; `a .subckt`; `no mapping in the & space` | `ENGINE`, `RESULT` |
| FATAL (stderr, then trap) | `odin3: linked ABC called <exit\|abort\|assert> outside its session thread; stopping` | AB-6 |
| ERROR / INFO | the project and CLI texts of §10.2 | §10 |

## 12. Performance (AB-29)

**Reference.** VTR's ABC stage at `3c9a4d23` (golden results, other machines):

| Design | ABC stage time |
|---|---|
| mcml | 3,414 s (peak 365 MB, the largest) |
| LU32PEEng | 1,064 s |
| LU8PEEng | 117 s |
| bgm | 95 s |
| every other design | under 15 s |

| Measure (Release; `bench_abc`; smallest first; heavy designs per §13.9) | Target |
|---|---|
| Time outside ABC per run (classification, AIG, checks, splice, provenance) | ≤ 5% of the run's ABC time, and ≤ 10 s on mcml |
| Linked ABC against the pinned `abc` executable on the same expanded script and exchange BLIF | ≤ 1.05× time; equal LUT, FF and level counts |
| Name map | ≤ 15% of ABC time on each of the 19 (else the cap) |
| `--via-blif` overhead | reported; ≤ 25% of ABC time on mcml |
| Peak RSS during `abc` | ≤ RSS before + 1.2 × the executable's peak + 64 MB |
| `open` + `close`; script checker | ≤ 20 ms; ≤ 1 ms on 64 KiB |

The bench's per-run table (counts, depth, phase times, name-map coverage, edges, RSS, VTR's row)
goes verbatim into PHASE3. A missed target is investigated, never relaxed.

## 13. Testing (AB-30)

**Rules:**

- Debug, ASan and UBSan, with `check` around every pass. Sweeps run smallest first.
- Implementers run unit tests, the CTest gate and small samples.
- **Heavy runs** (I4) go through `tools/heavy-run [--xl] <cmd>` (2E-1 Task 2; the 2E plan's point
  (p)): `flock` on `/tmp/claude-1000/odin3-heavy.lock`, then `systemd-run --user --scope -q
  --slice=odin3-heavy.slice` (6G). One job at a time, machine-wide, never a bare nested
  `--scope`. They skip unless `ODIN3_HEAVY_OK=1`.
- Heavy CTests are registered with `odin3_add_sweep_test(<name> LABEL sweep COMMAND …)`. Heavy
  means VTR-set sweeps, the four large designs, fuzzing past 10k cases or 10 minutes, and ASan on
  large inputs.
- Every heavy command runs under `timeout 3h`. mcml and LU32PEEng are never run from a PR's CI or
  by an agent: the controller runs them by hand (§13.9), with `--xl` (9G, idle machine) only if
  `bench_abc` measures more than 6 GB.

### 13.1 Unit tests (CI) and the fake engine

**The fake engine** (`tests/unit/abc_fake_engine.c`) implements the table without ABC, so the core
runs fast and deterministically under ASan and fault injection. `exec` knows:

- `&put`, `&get -m -n` and `strash`: no-ops;
- `if -K k`: each AND becomes a 2-input LUT, with inverter LUTs for complemented outputs;
- `scorr`: registers with equal `D` merge, keeping the lowest's name; a `D` equal to its own `Q` or
  0 makes a constant;
- `const`: one LUT becomes constant (I10).

`get_luts` fills `ci_from` from its names. `name_map` anchors each LUT to its AND.

| Test | Covers |
|---|---|
| `test_lut_cell` (3D-1) | the AB-2 interface; `verify`; `const_value`; JSON form; `simulate` exhaustively for WIDTH 1–6, and on 1,000 random tables per WIDTH 7–16 × 64 random inputs from 1E's `odin3_prng` seed 0x3D1 (minor 24); bit order (`LUT[i]`, `A[0]` LSB) and the engine word layout for k < lut_size (I10) |
| `test_lut_verilog` (3D-3) | the Verilog form; `writer-check` with Yosys and Icarus |
| `test_sha256` | FIPS 180-4 vectors; streamed in odd chunks |
| `test_passrun` | args recorded (trimmed; empty gives 0); info set once; readers' runs 0; ABI accessors; `compact` keeps rows |
| `test_abc_script` | every AB-21 text at its line (CRLF too); `{K}`; aliases (extra words, depth); each refusal class; arguments; **every allowed command × each of its file/string options refused** (I12); mapping states (the BLIF `&` rule); caps at and past their limits |
| `test_abc_alias_table`, `test_abc_default_script` | the table equals the submodule's `abc.rc`; §7.4's lists; the embedded default's hash; K = 6, and the failure without `--lut` (line 10) |
| `test_abc_region` | soft, cut and constant classification for every registry type, including a parameterised bit type and a soft cell on a bus (cut); CI/CO rules; domains as FF sets, in order; loop cuts |
| `test_abc_unconn` | **3E §14.1 items 2 and 4.** A `single_port_ram` with address bits on `unconn` and on a `$_CONSTX_` directly: they stay on those nets. A `$lut` reading `unconn`, and a soft path from `unconn` to a RAM pin: folded to 0 (the pin ends on `gnd`, like VTR). Fake and real engine, both exchanges |
| `test_abc_init1` | **C1.** A toggle with INIT 1, and two INIT-1 registers that `scorr` merges, through both exchanges (real engine, default script). `equiv-check --strict-init` passes, and the result's INIT and polarity are as AB-13 says |
| `test_abc_aig` | MiniAIG layout; hashing rules; owners; inverted registers; PI names; equal to the IR region under 1E simulation (256 vectors × 3 seeds, sequential) |
| `test_abc_splice` | phase B's table, every row and both columns; a derived clock buffered by a run, whose domain still runs (I13); two port nets never merged; constant LUTs folded (I10); restore (flips, inverter to buffer, NOT for a shared `D`); DC → 2; names; cleanup; every `<why>` of §11; simulation equal before and after |
| `test_abc_prov` | anchors and regions; **in cone mode a register's `D` cone has the old cone's records as parents** (C3); forward and backward navigation; a LUT in several regions; coarse mode at a lowered budget |
| `test_abc_engine_abi` | the register contract; `ODIN3_ERR_ENGINE`'s string; the Python cdef; "not available" without an engine |

### 13.2 Adapter tests (CI, real ABC)

`test_abc_adapter.c` uses hooks compiled only for this test (`ODIN3_ABC_TEST_HOOKS` in `bridge.c`):
the commands `o3test_exit <n>`, `o3test_assert` and `o3test_thread_exit` (calls `exit` on a new
thread), added with `Cmd_CommandAdd`, and access to `CmdSplitLine`.

- **Sessions.** 100 open/close cycles. One script run twice in one process gives identical
  MiniLUTs, for the default script and for `strash; fraig; if -K 6` (I5). A nested open gives
  `BUSY`.
- **Capture.** `echo hello` reaches the callback. fds 1 and 2 (pipes) receive nothing during ABC and
  are restored after.
- **No files** (I12). In a working directory that holds only a file named `6`, every allowed command
  runs on a small network with its default options. Afterwards the directory holds only `6`, which
  has not been read (atime and `strace`-free check: `6` holds garbage that would fail any reader).
  An existing `abc.history` is untouched.
- **Escapes**, each in its own process with `detect_leaks=0`:
  - `o3test_exit 3` gives `EXIT` code 3 with fds restored, then `DISABLED`;
  - `o3test_assert` gives `ASSERT`;
  - `o3test_thread_exit` dies by `SIGILL` after the "outside its session thread" line (I6).
- **Checked is what runs.** `CmdSplitLine`'s argv equals the checker's words on every accepted test,
  alias and fuzz-corpus command.
- **Symbols.** `symbol_rules` with the adapter; `abc_adapter_exports`; `abc_adapter_no_assert`
  (§4.1).

### 13.3 Round trips (CI)

1. A design with `$lut`s: `write_blif`, then `read_blif`, then `write_blif` gives identical text
   with equal truth tables, once 3E-5 lands.
2. The fake engine's identity mapping in both exchanges, on every BLIF fixture and micro:
   `equiv-check` before against after.
3. Real ABC with `strash; if -K 6`, on the same set: equivalent. Running `abc` on the result again
   is still equivalent.
4. `test_abc_init1` (C1).
5. `project-fixtures compare` with the new cases.

### 13.4 `equiv-check` around `abc` (`tools/abc-check/abc-equiv`)

Per design, smallest first:

1. `odin3 -p "read_blif X; write_blif before.blif; abc --lut 6 [--via-blif D]; write_blif
   after.blif"`;
2. `equiv-check --blackbox adder,multiply,single_port_ram,dual_port_ram before.blif after.blif`;
3. assert that the hard-block pins on `unconn` are the same set before and after.

**`equiv-check --blackbox M[,M…]`** is new in 3D-5 (I9). Instances of those models are paired across
the two files by name, or by occurrence order per model when unnamed (cut cells keep their relative
order through `compact`). Equal counts are required. Their outputs become shared primary inputs and
their inputs compared outputs. This is sound for `abc`, which never touches a hard cell, and it
removes the 36×36 multiplier that `dsec` cannot finish. Several clocks stay excluded.

| Oracle per design | Designs |
|---|---|
| `equiv-check` without boxes (today) | `sha`, `stereovision0`, `blob_merge` |
| `equiv-check --blackbox` (3D) | the 18 single-clock designs: every design but `stereovision3`. A `timeout` (2 h) verdict is listed, not failed |
| Verilator random check (P3-16, 3H) | all 19 (the only oracle for `stereovision3`) |

Where: CI (`abc_equiv_micro`) runs `tests/micro` and `tests/golden/blif`; nightly (`sweep`) runs the
`vtr/` and regression goldens below 100k `.names`; the four large designs are in §13.9.

### 13.5 The exchange differential (`tools/abc-check/abc-diff`)

Both exchanges run on each design. Both results must be equivalent to the input and to each other.
LUT, FF and depth ratios are printed.

- Per I8, on the 19 **before 3D-7 flips the default**, recorded in PHASE3 as the entry-path effect.
- Then nightly, smallest first.

A difference over 5% is explained in PHASE3.

### 13.6 Out of memory

- **Fake engine.** A failure is injected at every allocation (`odin3_util_set_alloc_fail_after`) in
  the checker, region, AIG, exchange writer and read-back, splice (phases A–D) and provenance. Each
  must give `NO_MEMORY`, no leak, a passing FULL `check`, and simulation equal to before.
- **Adapter.** A hook fails its copies and `tmpfile()`.
- **Budget.** A hook lowers it (the cgroup bound included), and the exact text is checked.
- **ABC's own OOM** cannot be tested in-process (§17 risk 1).

### 13.7 Fuzzing

- **`fuzz_abc_script`** (libFuzzer). Input: a K byte, then script bytes.
  - It must not crash or leak.
  - When accepted: every word 0 is allowed, the arguments pass, there are ≤ 1,024 commands, the end
    is mapped, and no command contains a quote, `!`, `;`, `#`, `{`, `}`, an alias or a file option.
  - When rejected: exactly one located error.
  - Seeds: VTR's script, every alias, the refused examples. CI: 60 s; longer runs are heavy.
- **`fuzz_abc_splice`** (fake engine). Input: a small BLIF, K and fake choices (INIT 1 registers,
  `unconn` readers, derived clocks included). The result must have a valid IR, simulate equal, and
  have complete provenance. CI: 2,000 cases; more is heavy.

### 13.8 Determinism

| Test | Where |
|---|---|
| (a) two processes: identical `write_blif`, and identical `.in.blif`, `.cmd`, `.out.blif` (never `.log` or `--log`) | micros in CI; the VTR set nightly |
| (b) one process: A, then B, then a fresh A; the two A results identical; scripts: the default and one with `fraig` (I5) | CI |
| (c) two runs a minute apart, identical | nightly |
| (d) Debug against Release on the micros | nightly |

### 13.9 The large designs: an explicit schedule (I4)

The controller runs these by hand, one per heavy slot, each through `tools/heavy-run` with `timeout
3h`. Times are VTR's ABC stage plus `equiv-check`'s cap; LU8PEEng and bgm take minutes and run in
the nightly `sweep`.

| Run | After | What | mcml | LU32PEEng |
|---|---|---|---|---|
| H1 | 3D-5 | `abc-equiv --via-blif` (also determinism (a) run 1 and `abc-diff`'s BLIF side) | 1 h + ≤ 2 h equiv | 18 min + ≤ 2 h |
| H2 | 3D-5 | `bench_abc`, BLIF: linked ABC and the pinned executable on the same exchange BLIF | 2 h | 36 min |
| H3 | 3D-5 | determinism (a) run 2, compared with H1 | 1 h | 18 min |
| H4 | 3D-7, before the default flips | `abc-equiv` in-memory (also `abc-diff`'s in-memory side, with H1) | 1 h + ≤ 2 h equiv | 18 min + ≤ 2 h |
| H5 | 3D-7 | `bench_abc` in-memory (the executable row reused from H2) | 1 h | 18 min |
| H6 | 3D-7 | determinism (a) run 2, compared with H4 | 1 h | 18 min |

That is about 7 h of ABC plus ≤ 4 h of `equiv-check` for mcml, and about 2.5 h plus ≤ 4 h for
LU32PEEng, in roughly ten solo slots. A failed run is repeated only after a fix. 3H's P′ runs are
3H's.

## 14. Files (AB-31)

| File | Content |
|---|---|
| `src/ir/cells/lut.c`, `cells/builtin.c`, `cells/cells.h`; `src/backends/json/*` | `$lut` (AB-2) and its JSON form (3D-1) |
| `src/backends/verilog/*` | `$lut` through `odin3_blif_isop`'s cover (3D-3) |
| `include/odin3/odin3.h`, `src/api/abc.c`, `src/api/pass.c`, `src/api/design.c` | engine ABI, `ODIN3_ERR_ENGINE`, pass-run accessors, `odin3_pass_set_abc_script` (ABI v4 line) |
| `src/ir/prov.[ch]`, `src/passes/manager.c` | pass-run rows; args in `open_run`; compact after `abc` |
| `src/util/sha256.[ch]` | `odin3_sha256_init/update/final/hex` (FIPS 180-4) |
| `src/passes/abc/abc.h`, `abc_test.h` | caps; `odin3_abc_opts`; `odin3_abc_pass_run(odin3_pass_ctx *, odin3_design *, odin3_bytes args, void *user)`; `odin3_abc_test_set_limits`, `odin3_abc_test_set_engine` |
| `script.[ch]`, `alias.c`, `allow.c`, `default_script.c` (generated) | `odin3_abc_script_parse(const odin3_abc_script_src *src, uint32_t lut_k, uint32_t exchange, odin3_abc_script *out)`, `odin3_abc_script_free`, `odin3_abc_script_cmd`; the alias table; allow-list, classes, file options |
| `args.c`, `run.c`, `engine.c`, `region.[ch]`, `aig.[ch]`, `xblif.[ch]`, `result.[ch]`, `splice.[ch]`, `prov.[ch]`, `depth.c` | AB-10 to AB-25 |
| `src/frontends/project/*`, `src/cli/main.c` | AB-27 |
| `adapters/abc/{adapter,session,guard,capture,bridge}.c`, `bridge.h`, `guard.h`, `odin3_abc.h`, `exports.map`, `lsan.supp`, `NOTICE`, `README.md` | AB-5 to AB-9 |
| `cmake/abc_pin.cmake`, `CMakeLists.txt`, `tools/check-symbols.sh`, `tools/check_rules.py` | AB-5, AB-6 |
| `tools/equiv-check/equiv_check.py` | `--blackbox` (I9) |
| `lib/abc/vtr.abc`; `tools/abc-check/{abc-equiv,abc-diff}` (stdlib Python); §13's tests | |

Side arrays come from `util/vec`, and per-run scratch from one arena. lizard's limits hold. There is
no recursion.

## 15. Implementation split (AB-32)

Nine PRs, each tests-first, passing `/lint`, with a read-only critique (PHASE0 §A.1).

| PR | Content | Depends on | Review | Model / effort |
|---|---|---|---|---|
| **3D-0** spike (not merged) | AB-17's eight criteria | — | light | Opus `xhigh` |
| **3D-1** `$lut` (small, first) | `lut.c`, the builtin entry, `ODIN3_LUT_MAX_WIDTH`, the JSON form, `test_lut_cell`, the IR.md and DESIGN §5.2 text; **3E-5 follows directly** | this spec merged | **Peter** (IR; approved as an agent default) | Opus `high` |
| **3D-2** adapter | CMake (`libabc-pic`, pin, `-O2`, `ABC_NO_HISTORY`, PIC `abc` tool); `adapters/abc` (bridge, session, guard and thread check, capture, exports, NOTICE, licence scan, adapter assert rule); engine ABI, `ODIN3_ERR_ENGINE` (ABI v4 line), CLI init; symbol and adapter tests | — | **Peter** (adapter build, ABI) | Opus `high` |
| **3D-3** run records | pass-run args and info, accessors, IR-12 text; `sha256`; Verilog `$lut` | 3D-1; 3E-5 for Verilog | **Peter** (IR, ABI) | Opus `high` |
| **3D-4** script | checker, caps, alias table, allow-list with file options, embedded default, `fuzz_abc_script` | 3D-3 (sha256) | light | Opus `high` |
| **3D-5** pass, BLIF exchange | args and precedence; region, constants per pin, domains as FF sets; exchange writer with the init encoding, read-back; `abc_result` with folding; splice with special nets; restore; cone provenance with register-input anchors; depth; run loop, logs, budget (cgroup), compact; fake engine; `abc-equiv`; `equiv-check --blackbox`; `test_abc_unconn`/`_init1`; `fuzz_abc_splice`; PASSES row; DESIGN §6 | 3D-2, 3D-3, 3D-4, 3E-5 | light | Opus `xhigh` → `high` once the splice settles |
| **3D-6** project and CLI | `abc_script`, `--abc-script`, request, dump, reference parser, DESIGN §4.0, fixtures (bulk `null`), precedence test | 3D-4 | light (P3-1) | Opus `high`; Sonnet `medium` for the bulk |
| **3D-7** in-memory exchange | AIG builder; names through `ci_from` (observation outputs only if the spike says so); MiniLUT result; aligned MiniLUT and name map; `abc-diff` on the 19 (H1, H4), then the default flips | 3D-5, the 3D-0 go | light; **Peter** if P3-9 (c) or observation outputs | Opus `xhigh` → `high` |
| **3D-8** evidence | determinism suite, `bench_abc` and calibration, the §13.9 schedule (controller), the PHASE3 entry | 3D-5 (3D-7 for its rows) | light | Opus `high`; Sonnet `high` for bulk runs |

**Lanes:**

- 3D-1 first, once this spec merges (it unblocks 3E-5).
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
| PHASE3 | P3-8: "registers the ABC engine (3D AB-7)"; P3-9 (a): correspondence by ABC's names plus the name map on an aligned MiniLUT, entered through `&` + `&put` (AB-16); P3-11's reason for refusing `quit` corrected (it returns −1, `cmd.c:386`; still refused as process control); the checklist gains "spike (3D-0)" | with this spec |
| `odin3.h` | the ABI v4 changelog line lists the engine section, `ODIN3_ERR_ENGINE` and the accessors (no bump) | 3D-2, 3D-3 |
| PASSES.md, ORACLES.md, CLAUDE.md | the `abc` row; the `abc` tool built from `libabc-pic`; Layout `adapters/{slang,abc}` (C; ABC's C++ stays in `third_party/`) | 3D-5, 3D-2 |
| `tools/equiv-check` docstring | `--blackbox` | 3D-5 |
| corpus README | key row, key order, four matrix rows | 3D-6 |
| 2E-1's `budget.[ch]` | the cgroup `memory.max` bound (minor 16) | 2E-1 or 3D-5 |

## 17. Risks

1. **ABC kills the process**: unchecked `ABC_ALLOC` (SIGSEGV), `std::terminate`, glibc fatals, a
   worker-thread escape (AB-6). The fallback is a subprocess engine.
2. **The spike fails**: AB-17's fallbacks. M1 runs on the BLIF exchange either way.
3. **The entry path biases QoR** (I8): `abc-diff` measures it before the flip; 3H names the
   exchange.
4. **The name map is slow, or its coverage is low after `scorr`**: the cap, `--prov cone`, the bench;
   cone provenance still covers every `D` cone (C3).
5. **Overlapping cones**: the edge budget and the single record.
6. **Hidden nondeterminism**: an ABC generator missing from the list, threads. Covered by §13.8 and
   the spike.
7. **fd capture is process-global.** A host's other threads printing during ABC are captured.
   Documented.
8. **ABC memory and time**: the budget (cgroup-aware), the calibration, `tools/heavy-run`, and
   about 10 h of solo slots for mcml (§13.9).
9. **Pin coupling**: the name map, the generators and `vNamesIn` are outside `abcapis.h`. Covered by
   the pin check, the alias test and a spike rerun per bump.
10. **Build time and licence**: the PIC `abc` tool and ccache; 3D-2's scan.
11. **Partly pinless wires** give `check` warnings; if they are frequent, a `check` change is for
    Peter.
12. **The ABC version gap** with VTR's ABC: the P′ leg attributes it (PHASE3 risk 4).

## 18. Decisions

**Confirmed** (rulings, 2026-10-11):

- Q1, the engine seam (AB-7).
- Q2, the correspondence correction (AB-16), amended so that ABC's names come first.
- Q3, `--wrap` (AB-6), with honest coverage and the thread check.
- Q4, `ODIN3_ERR_ENGINE` and a frame per run, with **no ABI bump**.
- `$lut` (AB-2) is approved as an agent default: Peter approved P3-9's `$lut` on 2026-10-11, so
  3D-1 proceeds once this spec merges, and its PR is still Peter's review.
- Pass-run arguments and run information (IR-12, §8.5) go with 3D-3's review.

**Open for Peter, only if the spike forces it:** P3-9 (c) on the output side, or observation outputs
costing more than 1%.

**Agent defaults:**

- `--log` and `--prov`;
- no built-in K;
- domain order by clock name, domains keyed by FF sets;
- `&ps` allowed; `print_constr` and `print_status` left out;
- quotes refused;
- loops cut;
- 512 bytes per AIG object until calibrated;
- the PIC `abc` tool; `-O2` ABC in Debug; `set check`;
- `exchange=` and `depth=` in the run information;
- `"abc_script": null` in every positive record;
- the worker-thread escape traps rather than calling `exit_group`, so no process-ending import
  exists.

## 19. Review dispositions (v2, `3D-review1`)

| # | Finding | Disposition |
|---|---|---|
| C1 | BLIF init-1 latches come back relabelled | applied (ruling): one init encoding on both exchanges, inverters in the exchange BLIF, every latch sent at 0, shared restore, correspondence by name; `test_abc_init1` (§5.4, §6.2, §13.1) |
| C2 | free `unconn`/x contradicts 3E and VTR | applied (ruling): literal 0 per soft reader pin; direct bypass for cut-cell pins only; folded and bypass cases in `test_abc_unconn`; the info line and risk 9 replaced (§5.2) |
| C3 | `D` cones without parents | applied (ruling): register inputs are identity anchors; IR-12 text updated; `test_abc_prov` case (§8.2, §8.5) |
| I4 | heavy-run wrapper; mcml runs uncounted | applied (ruling): `tools/heavy-run` in `odin3-heavy.slice`, `timeout 3h`, the explicit H1–H6 schedule (§13, §13.9) |
| I5 | libc `rand()` not reset | applied: `srand(1)` at open; `fraig` in the determinism tests and spike criterion 6 |
| I6 | `--wrap` overstated; cross-thread `longjmp`; adapter asserts | applied: coverage stated, thread check (trap, not `exit_group`: no process-ending import), `ODIN3_ADAPTER_ASSERT` with a lint rule (§4.1, §4.2) |
| I7 | resolved script path in the record | applied (ruling): path as given plus SHA-256; resolved path at INFO only (§7.2, §8.5) |
| I8 | the exchanges build different AIGs | applied (ruling): documented; in-memory is the measured exchange; `abc-diff` on the 19 before the flip; 3H names the exchange (§6.3) |
| I9 | `equiv-check` covers ≤ 8 of 19 | applied (ruling): `equiv-check --blackbox`, per-design oracle table, 18 of 19 (§13.4) |
| I10 | constant LUTs; truth-word layout | applied (ruling): folding in the result, exact layout and array lengths in §2 and the ABI comments |
| I11 | observation outputs cost; names are cheaper | applied (ruling): names on both exchanges via `vNamesIn`/`&get -n`; observation outputs only as the spike's fallback (criteria 2, 8) |
| I12 | via-blif path bytes; numeric file options | applied: control bytes, quotes, `;`, `#` refused; per-command file/string options refused; "no files" test with a file named `6` |
| I13 | phase B drops clock or port nets | applied: special nets keep their identity behind a buffer `$lut`; domains keyed by FF sets (§5.3, §6.5) |
| m14 | `.log` timings in determinism | applied (§9, §13.8) |
| m15 | name map is combinational | applied (§6.3, criterion 3) |
| m16 | budget ignores the cgroup | applied (§7.8) |
| m17 | how the spike cuts hard blocks | applied: ABC's own `read` + `strash` with `.blackbox` models (§6.4) |
| m18 | depth for 3H | applied: `depth=` in the run information, the run line and `abc: final: depth <d>` |
| m19 | "one ABI bump" | applied: no bump, ABI v4 line; status table, cdef, test listed (§4.3) |
| m20 | layouts are ABC's | applied: "Odin III's exchange layout, equal to MiniAIG/MiniLUT at the pin" |
| m21 | `restore_outputs_from_blif` | applied (§6.2) |
| m22 | multi-driver CIs ambiguous | applied: a soft cell driving a bus is a cut cell; a bus read is a CI (§5.1, §5.2) |
| m23 | gate truth-table cache | applied: per type because parameterless; parameterised bit types are cut cells (§5.1) |
| m24 | random test seed and count | applied: seed 0x3D1, 1,000 tables per WIDTH 7–16 (§13.1) |
| m25 | INFO milliseconds and `--log` | applied with m14 |

No minor finding was rejected.
