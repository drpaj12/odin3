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

## Start-up and licence

`odin3` links the adapter and calls `odin3_abc_adapter_init()` before any `--plugin`. A failure is
the warning `linked ABC unavailable: <status>`, and `odin3` runs without linked ABC. With the
engine registered, `odin3 --version` adds `linked ABC <commit> (adapters/abc/NOTICE)`.

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
  - **Until Peter rules on plan point (a), the stop is `raise(SIGKILL)`.** That is what ships,
    and `test_abc_adapter` pins `SIGKILL`.
  - The alternative under review is a trap (`__builtin_trap()`, `SIGILL`).
  - ABC started no thread in the spike's runs (PHASE3 decision 22).
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
  - on a 40-AND network: no thread started by the default script (the test link counts
    `pthread_create`), and two runs in one process byte-identical for the default script and for
    `strash; fraig; if -K 6`;
  - ABC's command splitting (`odin3_abc_test_split`, for 3D Task 11);
  - each in a child process:
    - `o3test_exit 3`, `o3test__exit 4`, `o3test_abort` and `o3test_assert`;
    - off-thread `exit`, `abort` and `assert` (the line, then `SIGKILL`);
    - the adapter assertion;
    - a planted `libabc_o3test.so` in the working directory and on `$ABC_LIB_PATH`, never loaded
      (its constructor would exit 9);
    - `close` with no descriptor left, after which the next open still works.
- `abc_adapter_exports`, `abc_adapter_exports_refuses_libodin3`, `symbol_rules` (with the adapter)
  and `abc_adapter_no_assert`.
- `cli_version_abc` and `cli_abc_unavailable_warning` (a test CLI linked to an adapter variant
  whose table the registry refuses).
