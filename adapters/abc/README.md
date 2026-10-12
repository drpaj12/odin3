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

## Process control (AB-6) and what it covers, honestly

The link wraps `exit`, `_exit`, `abort`, `__assert_fail`, `system`, `popen`, `fork`, `execv`,
`execvp` and `signal` (`guard.c`; no wrapper names a `__real_` symbol).

- **Escapes on the session thread** (`exit`, `_exit`, `abort`, a failed assertion) `longjmp` back
  to the guard, which restores fds 1 and 2, reports `EXIT` (with the code), `ABORT` or `ASSERT`
  (with `"<file>:<line>: <expr>"`), and disables the engine until the process ends. The frame's
  state is leaked, never reused.
- **Escapes on any other thread**, or with no guarded call running, write
  `odin3: linked ABC called <exit|abort|assert> outside its session thread; stopping` to the
  original stderr and stop the process: a `longjmp` across threads is undefined. How it stops is
  Peter's call (plan point (a)); until he rules, the stop is `raise(SIGKILL)`, not a trap. ABC
  started no thread in the spike's runs (PHASE3 decision 22).
- **Process control** is refused: `system`, `fork`, `execv` and `execvp` return −1, `popen` NULL,
  `signal` `SIG_ERR`, each with `errno = EPERM` and the line `ABC tried to <call>; refused` in the
  session's output. The AB-21 checker makes them unreachable; they are defence in depth.

The wraps catch these calls **only when ABC's own objects make them**. They do not catch:

- `std::terminate` (ABC has 124 C++ sources; it reaches `abort` inside `libstdc++.so`);
- glibc's fatal paths (heap corruption, `__libc_fatal`);
- signals, notably the SIGSEGV an unchecked `ABC_ALLOC` gives when memory runs out.

Each of those still ends `odin3` (spec §17, risk 1). The fallback, if they are seen, is ABC as a
subprocess engine.

## Tests

- `test_abc_adapter` (real ABC, built with `ODIN3_ABC_TEST_HOOKS`):
  - 100 sessions, and `BUSY` for a nested open;
  - the capture: `echo hello` reaches the callback while the test's own pipes on fds 1 and 2 get
    nothing and are restored; output still buffered in stdout goes to the original fd 1; a partial
    line waits for the next call;
  - a failing command (`COMMAND`), the refused calls and the plugin entry;
  - each in a child process: `o3test_exit 3`, `o3test_assert`, an off-thread `exit` and the
    adapter assertion.
- `abc_adapter_exports`, `abc_adapter_exports_refuses_libodin3`, `symbol_rules` (with the adapter)
  and `abc_adapter_no_assert`.
