# adapters/abc — the linked-ABC adapter (`libodin3_abc.so`)

The adapter puts the pinned ABC (`third_party/abc`, built as `libabc-pic`) behind the ABC engine
of `odin3.h` (3D spec §4.3). The `abc` pass lives in the core (`src/passes/abc/`); this library
only runs ABC. Spec: `docs/specs/2026-10-11-3D-linked-abc-design.md` §4 (AB-5 to AB-8).

## The boundary

- **Two exports.** `exports.map` leaves exactly `odin3_plugin_init` and `odin3_abc_adapter_init`
  (`odin3_abc.h`) in the dynamic table. `--exclude-libs,ALL` keeps every ABC symbol local, so none
  of ABC's global names reaches `libodin3` or the process. CTest `abc_adapter_exports` checks it.
- **`libodin3` never links ABC.** The adapter links `libodin3` and `libabc-pic` (objects only:
  none of ABC's compile flags reach the adapter's C).
- **One file sees ABC.** `bridge.c` alone includes ABC's headers (as SYSTEM headers, with ABC's
  `-D` flags and without `-Wstrict-prototypes`). `session.c`, `guard.c`, `capture.c` and
  `adapter.c` use only `odin3.h` and the adapter's headers, with the full `odin3_flags`.
- **No `assert()` here.** The link-wide `--wrap=__assert_fail` would report the adapter's own
  failures as ABC's. Invariants use `ODIN3_ADAPTER_ASSERT` (`guard.h`), which records
  `{ASSERT, 0, "adapter: <file>:<line>: <expr>"}`, disables the engine and returns
  `ODIN3_ERR_ENGINE`. It never traps (plan point (a): a trap is Peter's call). CTest
  `abc_adapter_no_assert` and `tools/check_rules.py` check it.
- **Sanitizers** reach the adapter's C, never ABC's objects. `lsan.supp` names only ABC functions.
  Its `^Abc_`/`^Gia_` patterns would also hide a leak of an ABC-allocated object the adapter owns
  (the MiniLUT from `Abc_FrameGiaOutputMiniLut`, the name map), so `close` frees both by hand.
- **Every bridge call runs under the guard.** Each `odin3_bridge_*` function starts with
  `ODIN3_ADAPTER_ASSERT(odin3_guard_active_here())`: one called outside `odin3_guard_run` is an
  adapter assertion (`ODIN3_ERR_ENGINE`, engine disabled) before ABC runs.

## Sessions (AB-8)

One static session at a time, process-wide (`session.c`). `open` makes a fresh frame
(`Abc_FrameCreate`, `Abc_FrameEnter`, batch mode, `set check`), resets every AB-4 generator and
calls `srand(1)`; `close` leaves the frame and destroys it. A frame costs about 15 ms, almost all
of it `Dar_LibStart` (PHASE3 decision 22). Every ABC call runs under `odin3_guard_run`: `setjmp`,
fds 1 and 2 on the session's capture file, the call, the fds restored, and ABC's lines handed to
the engine's `text` callback (without the newline and trailing blanks; a partial line waits).

The exchange calls (AB-16, spec §4.4 steps 3, 5 and 6):

- **`load_aig`** checks a copy of the MiniAIG before ABC sees it: object 0 is the constant, then
  the PIs, the ANDs with `0 <= lit0 < lit1 < 2 * index`, and the POs on earlier PIs or ANDs;
  `n_regs` is at most the PI and the PO count. The first fault is `RESULT`, naming the object. The
  copy then goes in through `Abc_FrameGiaInputMiniAig`, and CI *n* is named `c<n>`.
- **`get_luts`** is `Abc_FrameGiaOutputMiniLut`, with `ci_from` parsed from the GIA's CI names.
  It refuses first, with `RESULT`, when the `&` space has no mapping (`no mapping in the & space`)
  or a LUT wider than 16, because ABC would assert there, or crash without assertions. MiniLUT's
  `lut_size` is the widest LUT (at least 2), not the K of the mapping.
- **`name_map`** checks a copy of the aligned MiniLUT (every fanin on an earlier PI or LUT, and
  the loaded AIG's PI, PO and register counts), then calls `Abc_FrameGiaInputMiniLut2` and
  `Abc_FrameReadMiniLutNameMapping`.
- What `get_luts` and `name_map` return stays valid until the next such call or `close`.

The gate-mapping calls (ABI 5, `docs/specs/2026-10-21-gatemap-design.md` 4.2 and 4.3):

- **`load_genlib`** reads only the canonical genlib that Odin III's own writer
  (`src/passes/abc/genlib.c`) emits, and refuses everything else before ABC sees it, because ABC's
  reader is not total (it dereferences missing words, runs past a comment on the last line, joins
  lines at a `\`, and reads a `PIN *` formula past its fixed name table). The subset:
  - printable ASCII, single spaces and `\n` only: no `#` comment, tab, `\r`, `\`, other control
    character or byte outside ASCII, and no empty line;
  - `GATE <name> <area> <out>=<formula>;` lines, each followed by zero or more
    `PIN <name> UNKNOWN|INV|NONINV <n> <n> <n> <n> <n> <n>` lines (no `LATCH`, `.end` or other
    statement);
  - a name is `[A-Za-z_$][A-Za-z0-9_$]*` (so it never starts with `0` or `1`, which ABC reads as a
    constant), a number `[-+]?` digits with an optional fraction and exponent; each at most 128
    bytes;
  - at most 6 `PIN`s per gate (the core's `ODIN3_ABC_GATE_MAX_INPUTS`), all distinct and explicit
    (`PIN *` is refused); the formula names each of them and no other name; a gate without `PIN`s
    has the formula `CONST0` or `CONST1`;
  - no two gates share a name (ABC would pair them into a two-output gate).

  A text outside it is `RESULT` `ABC cannot read the gate library (line N: reason)` (or `(no
  gates)`). Then, inside the guarded call, every gate's formula is parsed over its pins: a grammar
  check, then `Mio_ParseFormula`. ABC's `Mio_GateParseFormula` hands the `NULL` that
  `Mio_ParseFormula` returns for a formula it cannot parse to `Mio_LibDeriveSop`
  (`mioFunc.c:264`), a segmentation fault the guard cannot catch, and `Mio_ParseFormulaOper`
  asserts on some `!!` and on an operator with no operand. So an unclosed or extra parenthesis
  (`Y=(A;`), a leading or dangling operator (`Y=*A;`, `Y=A+;`) and `!!` are `RESULT` `ABC cannot
  read the gate library (gate "name")`, and the engine stays usable. A library that passes both
  checks and still breaks an ABC assumption would be caught by the guard (escape, engine
  disabled). A copy of the text with a `\n` appended then goes to `Mio_LibraryReadBuffer`, not
  `Mio_LibraryRead`, which dereferences the library it failed to read (`mioRead.c:96`); a `NULL`
  library is the plain `RESULT`. The library is named `odin3.genlib` (no file is read) and made the
  frame's with `Mio_UpdateGenlib`, which frees the previous one. So load the library before the
  mapping script: once any `exec` has run in the session (even one that failed), `load_genlib` is
  `RESULT` `load the gate library before the mapping script`, because a mapped network, the backup
  network a later command keeps of it (`mainFrame.c:469`), an `&save` copy or an undo would point
  into the freed library. A new session can load another. Other texts: `no gate library (text is
  NULL)`.
- **`get_cells`** reads the network space's mapped network (`Abc_NtkIsMappedLogic`, after `map`)
  or converts the `&` space's cell mapping (`Gia_ManHasCellMapping`, after `&nf`) with
  `Abc_NtkFromCellMappedGia`, deleted again before the call returns. Objects follow MiniLUT's
  layout: the constants 0 and 1 (a constant node is one of them, by its gate's truth), the PIs and
  register outputs, the gates in `Abc_NtkDfsIter` order (fanins in pin order, the 64-bit
  `Mio_GateReadTruth` as two words when a gate has 6 inputs), the POs and register inputs; `gate`
  and `names` give each gate object's library gate (names point into the library). `RESULT` texts:
  `no gate-mapped network in the network space`, `no cell mapping in the & space`, `a box that is
  not a latch`, `a node without a library gate`, `a gate with N inputs (at most 6)`, `a fanin that
  is not a gate, an input or a constant`, `no network space N` (a space that is neither), and
  `nowhere to put the cells (cells is NULL)`.
- What `get_cells` returns stays valid until the next `get_cells`, `load_genlib` or `close`.
- Bounded leaks. Memory the adapter allocated and ABC's own error paths leave behind are not
  freed after an escape (the engine is disabled, so nothing more is allocated): `load_genlib`'s
  text copies (the genlib copy and the formula check's) and `get_cells`' object map
  (`build.obj_of`) and order vector (`build.order`). What `get_cells` returned is freed at close.
  A `Mio_ParseFormula` that returns `NULL` also leaves its parser stacks and expressions (tens of bytes
  per refused gate, more for a long formula; `lsan.supp` names `Mio_ParseFormula` and, for stacks
  the unwinder cuts at the inlined builder, `Exp_And`). Each is bounded by one call.

## Start-up and licence

`odin3` links the adapter and calls `odin3_abc_adapter_init()` before any `--plugin`. A failure is
the warning `linked ABC unavailable: <status>`, and `odin3` runs without linked ABC. With the
engine registered, `odin3 --version` adds `linked ABC <commit> (adapters/abc/NOTICE)`.

Only a failed *registration* gives that warning. `odin3` links `libodin3_abc.so` as an ordinary
dynamic dependency (P3-8), so when the library is missing the dynamic loader stops `odin3` before
`main`: there is no soft fallback. Build with `ODIN3_WITH_ABC=OFF` for an `odin3` without it.

`NOTICE` reproduces ABC's `copyright.txt` and the notice of every third-party component that the
licence scan of `third_party/abc/src` found compiled into `libabc-pic` (spec §4.5, AB-9).

## Process control (AB-6) and what it covers, honestly

The link wraps `exit`, `_exit`, `abort`, `__assert_fail`, `system`, `popen`, `fork`, `execv`,
`execvp`, `execl`, `signal` and `dlopen` (`guard.c`; no wrapper names a `__real_` symbol).

- **Escapes on the session thread** (`exit`, `_exit`, `abort`, a failed assertion) `longjmp` back
  to the guard, which restores fds 1 and 2, reports `EXIT` (with the code), `ABORT` or `ASSERT`
  (with `"<file>:<line>: <expr>"`), and disables the engine until the process ends. The frame's
  state is leaked, never reused. The `longjmp` skips the destructors of any live C++ frames
  between the guard and the escape (ABC has 124 C++ sources). That is acceptable only because
  the engine never runs again in that process.
- **Escapes on any other thread**, or with no guarded call running, write
  `odin3: linked ABC called <exit|abort|assert> outside its session thread; stopping` to the
  original stderr and stop the process, because a `longjmp` across threads is undefined.
  - **The stop is `__builtin_trap()`** (`SIGILL`, exit status 132, which an OOM kill's 137 cannot
    be mistaken for; under ASan it prints a backtrace). Peter's ruling (a), PHASE3 #28, replaced
    the interim `raise(SIGKILL)`. It is the one trap outside `check` that CLAUDE.md and
    `tools/check_rules.py` allow (`adp_offthread_stop` in `guard.c` only), and
    `test_abc_adapter` pins `SIGILL`.
  - ABC started no thread in the spike's runs (PHASE3 decision 22).
  - The same applies at process exit. ABC's C++ objects have 8 static initialisers
    (`_GLOBAL__sub_I_Glucose*.cpp`, `UfarCmd.cpp`), whose destructors `__cxa_atexit` runs during
    `exit()`. An `exit`, `abort` or failed assertion inside one of them runs with no guard
    active, so it would stop `odin3` there, losing the exit status and any unflushed stdout. By
    reading, they are trivial option tables that call none of these.
- **Process control and code loading are refused.**
  - `system`, `fork`, `execv`, `execvp` and `execl` return −1; `popen` and `dlopen` return NULL;
    `signal` returns `SIG_ERR`.
  - Each sets `errno = EPERM` and adds the line `ABC tried to <call>; refused` to the session's
    output.
  - **`dlopen` is reachable:** every frame's creation runs ABC's `Libs_Init`
    (`base/main/libSupport.c`). It would `dlopen` each `libabc_*.so` in the working directory, or
    in `$ABC_LIB_PATH`, and run its `abc_init` outside every wrap. Refused, it loads nothing.
    ABC then prints its own "failed to load ABC library" warning, whose second line is whatever
    `dlerror()` holds (`(null)`, or a stale message).
  - The others are unreachable behind the AB-21 checker and are kept as defence in depth.

The wraps catch these calls **only when ABC's own objects make them**. They do not catch:

- `std::terminate` (it reaches `abort` inside `libstdc++.so`);
- glibc's fatal paths (heap corruption, `__libc_fatal`);
- signals, notably the SIGSEGV an unchecked `ABC_ALLOC` gives when memory runs out.

Each of those still ends `odin3` (spec §17, risk 1). The fallback, if they are seen, is ABC as a
subprocess engine.

**The adapter's other process-affecting imports** (from ABC's objects, `nm -D`) are unreachable
from a guarded call, by reading the pinned source:

- `execl` is used only in a `fork` child (`base/sn/snCom.c`); both are wrapped anyway.
- `prctl` and `raise` are used only in signal handlers or `fork` children (`opt/util/util.cpp`,
  `sat/cadical`, `sat/kissat`), which the `signal` and `fork` wraps refuse. The adapter's own
  stop also uses `raise`, so it cannot be forbidden.
- `setrlimit` is used only in `Abc_RealMain` and bsat2's `main`, never by a command.
- `pthread_exit` runs only on worker threads.
- `dlsym`, `dlerror` and `dlclose` act only on the handles `dlopen` would have returned; there are
  none (ABC's warning prints whatever `dlerror()` holds, which is cosmetic). All three stay
  unwrapped for the same reason: clang's static sanitizer runtime, linked into the test
  executable, calls the `dl*` functions itself, and a link-wide wrap would redirect its calls too.
- `pipe`, `waitpid` and `pclose` serve `fork` and `popen`, which are refused.

CTest `abc_adapter_exports` forbids the rest of the exec, spawn and `dl*open` families, so a pin
bump that brings one in fails.

## Tests

- `test_abc_adapter` (real ABC, built with `ODIN3_ABC_TEST_HOOKS`):
  - 100 sessions, and `BUSY` for a nested open;
  - the capture: `echo hello` reaches the callback while the test's own pipes on fds 1 and 2 get
    nothing and are restored; output still buffered in stdout goes to the original fd 1; a partial
    line waits for the next call;
  - a failing command (`COMMAND`), the refused calls, the generators and `srand(1)` reset at
    every open, and the plugin entry;
  - the exchange: a toggle (two PIs and a register) through `load_aig`, `&put`, `if -K 6`,
    `&get -m -n` and `get_luts`; every malformed AIG and aligned network refused with its text;
    `get_luts` without a mapping; the name map; and each of the adapter's own allocations
    failed in turn (`NO_MEMORY`, the engine still usable);
  - gate mapping on the cmos6 genlib: `map` and `&nf` results through `get_cells` (names, truths,
    a register's `ci_from`, a constant output), every refused genlib text, `get_cells` without a
    mapped network, and each allocation of the gate flow failed in turn;
  - on a 40-AND network: no thread started by the default script (the test link counts
    `pthread_create`), and two runs in one process byte-identical for the default script and for
    `strash; fraig; if -K 6`;
  - ABC's command splitting (`odin3_abc_test_split`, for 3D Task 11);
  - each in a child process:
    - `o3test_exit 3`, `o3test__exit 4`, `o3test_abort` and `o3test_assert`;
    - off-thread `exit`, `abort` and `assert` (the line, then `SIGILL`);
    - the adapter assertion;
    - a planted `libabc_o3test.so` in the working directory and on `$ABC_LIB_PATH`, never loaded
      (its constructor would exit 9);
    - `close` with no descriptor left, after which the next open still works.
- `abc_adapter_exports`, `abc_adapter_exports_refuses_libodin3`, `symbol_rules` (with the adapter)
  and `abc_adapter_no_assert`.
- `cli_version_abc` and `cli_abc_unavailable_warning` (a test CLI linked to an adapter variant
  whose table the registry refuses).
