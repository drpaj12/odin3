/*
 * guard.h — the escape guard, the link-time wrappers and the adapter's assertion (3D spec §4.1,
 * §4.2, AB-6; plan points (a) and (w)).
 *
 * libodin3_abc.so links with --wrap for exit, _exit, abort, __assert_fail, system, popen, fork,
 * execv, execvp, execl, signal and dlopen, so every call ABC's own objects make to them lands in a
 * wrapper below.
 * The escapes (exit, _exit, abort, an assertion) on the thread running a guarded call longjmp
 * back to odin3_guard_run, which restores fds 1 and 2, reports the escape and disables the engine
 * for the rest of the process. On any other thread, or with no guarded call running, they write
 * one line to the original stderr and stop the process. The process-control and code-loading
 * calls are refused.
 *
 * The wrappers never name a __real_ symbol, and their C names are bound to the --wrap symbols with
 * asm labels, because clang-tidy refuses C identifiers that start with "__" (point (w)).
 */
#ifndef ODIN3_ADAPTERS_ABC_GUARD_H
#define ODIN3_ADAPTERS_ABC_GUARD_H

#include "odin3/odin3.h"
#include "session.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* A guarded call. arg is the caller's; the result is the call's own status. */
typedef odin3_status (*odin3_guard_fn)(void *arg);

/*
 * The adapter's own invariant check (point (a)). The adapter never calls assert(): the link-wide
 * --wrap=__assert_fail would report its failures as ABC's (tools/check_rules.py refuses assert(
 * in adapters/abc/). A false condition records the pending failure {ASSERT, 0, "adapter:
 * <file>:<line>: <condition>"}, disables the engine and returns ODIN3_ERR_ENGINE from the
 * enclosing function, which must return odin3_status. It never traps (Peter's call, point (a)).
 */
#define ODIN3_ADAPTER_ASSERT(c)                                                                    \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            return odin3_adapter_assert_fail(__FILE__, __LINE__, #c);                              \
        }                                                                                          \
    } while (0)

/*
 * Records the pending failure {ODIN3_ABC_FAIL_ASSERT, 0, "adapter: <file>:<line>: <expr>"}
 * (truncated to 255 bytes), disables the engine as an escape does and returns ODIN3_ERR_ENGINE.
 * The engine call that reaches it hands the failure to its caller (odin3_guard_finish).
 */
odin3_status odin3_adapter_assert_fail(const char *file, int line, const char *expr);

/*
 * Runs fn(arg) under setjmp, with fds 1 and 2 captured into the attached session's capture and
 * delivered to its text callback after the call (session.h). An escape on this thread restores
 * the fds first, fills *fail with {EXIT, code}, {ABORT, 0} or {ASSERT, 0, "<file>:<line>:
 * <expr>"} (at most 255 bytes), disables the engine and returns ODIN3_ERR_ENGINE. Otherwise it
 * returns fn's status, with *fail filled from an adapter assertion when that status is
 * ODIN3_ERR_ENGINE. ODIN3_ERR_NO_MEMORY when the fds cannot be saved (fn is not run). Not
 * reentrant: a guarded call never starts another.
 */
odin3_status odin3_guard_run(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail);

/*
 * As odin3_guard_run, except that when fds 1 and 2 cannot be captured (no descriptor left) fn
 * still runs under the guard, its output going to the real fds 1 and 2. For the session's close,
 * which must leave its frame whatever happens: a frame left entered would disable the engine.
 */
odin3_status odin3_guard_run_always(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail);

/* True once an escape or an adapter assertion has disabled the engine (for the process). */
bool odin3_guard_disabled(void);

/*
 * Makes cap and text/user the capture and output of the following guarded calls, and saves a
 * close-on-exec copy of fd 2, the "original stderr" of the off-thread line, until
 * odin3_guard_detach. The session calls it at open.
 */
void odin3_guard_attach(odin3_capture *cap, odin3_abc_text_fn text, void *user);

/* Ends odin3_guard_attach: no capture, and the off-thread line goes to fd 2 again. */
void odin3_guard_detach(void);

/*
 * Ends an engine call: when st is ODIN3_ERR_ENGINE and *fail is still NONE, *fail gets the pending
 * adapter-assertion failure (if any), which is then cleared. Returns st. fail may be NULL.
 */
odin3_status odin3_guard_finish(odin3_status st, odin3_abc_fail *fail);

/* *fail = {kind, 0, ""}; NULL is ignored. */
void odin3_abc_fail_reset(odin3_abc_fail *fail, uint32_t kind);

/* --- the --wrap wrappers (§4.2; defined in guard.c, never called by the adapter itself) ------- */

/* A signal handler, as signal() takes and returns it. */
typedef void (*odin3_sighandler_fn)(int sig);

/* The escapes. */
_Noreturn void odin3_wrap_exit(int code) __asm__("__wrap_exit");
_Noreturn void odin3_wrap__exit(int code) __asm__("__wrap__exit");
_Noreturn void odin3_wrap_abort(void) __asm__("__wrap_abort");
_Noreturn void odin3_wrap_assert_fail(const char *expr, const char *file, unsigned int line,
                                      const char *func) __asm__("__wrap___assert_fail");

/* The refused process control and code loading: each sets errno to EPERM and adds "ABC tried to
 * <call>; refused" to the session's output. */
int odin3_wrap_system(const char *command) __asm__("__wrap_system");
FILE *odin3_wrap_popen(const char *command, const char *type) __asm__("__wrap_popen");
pid_t odin3_wrap_fork(void) __asm__("__wrap_fork");
int odin3_wrap_execv(const char *path, char *const argv[]) __asm__("__wrap_execv");
int odin3_wrap_execvp(const char *file, char *const argv[]) __asm__("__wrap_execvp");
int odin3_wrap_execl(const char *path, const char *arg, ...) __asm__("__wrap_execl");
void *odin3_wrap_dlopen(const char *file, int mode) __asm__("__wrap_dlopen");
odin3_sighandler_fn odin3_wrap_signal(int sig,
                                      odin3_sighandler_fn handler) __asm__("__wrap_signal");

#ifdef ODIN3_ABC_TEST_HOOKS
/*
 * Test builds only: ODIN3_OK when ok, else the adapter assertion "o3test invariant" (its file is
 * adapters/abc/guard.c), with everything odin3_adapter_assert_fail does.
 */
odin3_status odin3_abc_test_invariant(bool ok);
#endif

#endif
