/*
 * guard.c — the escape guard, the --wrap wrappers and the adapter assertion (guard.h; 3D spec
 * §4.2, AB-6; plan point (a)).
 *
 * One guarded call runs at a time, on the session's thread. Its state is static: the jmp_buf, the
 * running thread, and the escape a wrapper recorded before longjmp. Whether a call is running and
 * where the off-thread line goes are atomics, because ABC's own threads read them.
 */
#include "guard.h"

#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

enum {
    ADP_FIRST_PRIVATE_FD = 3, /* saved descriptors stay clear of 0, 1 and 2 */
    ADP_REFUSED = -1,         /* what the refused process-control calls return */
    ADP_ESCAPED = 1           /* setjmp's value after an escape */
};

/* The off-thread lines (§4.2, §11). */
static const char adp_line_exit[] =
    "odin3: linked ABC called exit outside its session thread; stopping\n";
static const char adp_line_abort[] =
    "odin3: linked ABC called abort outside its session thread; stopping\n";
static const char adp_line_assert[] =
    "odin3: linked ABC called assert outside its session thread; stopping\n";

typedef struct adp_guard_state {
    jmp_buf env;            /* where an escape on the guarded thread returns */
    odin3_guard_fn fn;      /* the guarded call (kept here, not in setjmp's frame) */
    void *arg;              /* its argument */
    pthread_t thread;       /* the thread running the guarded call */
    atomic_bool active;     /* a guarded call is running on thread */
    atomic_bool disabled;   /* an escape or an adapter assertion happened: for the process */
    atomic_int err_fd;      /* the original stderr, where the off-thread line goes */
    odin3_abc_fail escape;  /* the escape that longjmp'd; kind NONE otherwise */
    odin3_abc_fail pending; /* the last adapter assertion, until an engine call takes it */
    bool has_pending;       /* pending holds a failure */
    odin3_capture *cap;     /* the attached session's capture, or NULL */
    odin3_abc_text_fn text; /* its output callback (NULL: dropped) */
    void *user;             /* the callback's first argument */
} adp_guard_state;

static adp_guard_state adp_guard = {.err_fd = STDERR_FILENO};

void odin3_abc_fail_reset(odin3_abc_fail *fail, uint32_t kind) {
    if (fail != NULL) {
        *fail = (odin3_abc_fail){.kind = kind};
    }
}

odin3_status odin3_abc_fail_set(odin3_abc_fail *fail, uint32_t kind, const char *fmt, ...) {
    if (fail != NULL) {
        odin3_abc_fail_reset(fail, kind);
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(fail->text, sizeof fail->text, fmt, args);
        va_end(args);
        if (len < 0) {
            fail->text[0] = '\0';
        }
    }
    return ODIN3_ERR_ENGINE;
}

bool odin3_guard_disabled(void) {
    return atomic_load(&adp_guard.disabled);
}

bool odin3_guard_active_here(void) {
    return atomic_load(&adp_guard.active) && pthread_equal(pthread_self(), adp_guard.thread);
}

void odin3_guard_attach(odin3_capture *cap, odin3_abc_text_fn text, void *user) {
    adp_guard.cap = cap;
    adp_guard.text = text;
    adp_guard.user = user;
    int fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, ADP_FIRST_PRIVATE_FD);
    int old = atomic_exchange(&adp_guard.err_fd, fd >= 0 ? fd : STDERR_FILENO);
    if (old != STDERR_FILENO) {
        (void)close(old);
    }
}

void odin3_guard_detach(void) {
    int old = atomic_exchange(&adp_guard.err_fd, STDERR_FILENO);
    if (old != STDERR_FILENO) {
        (void)close(old);
    }
    adp_guard.cap = NULL;
    adp_guard.text = NULL;
    adp_guard.user = NULL;
}

odin3_status odin3_adapter_assert_fail(const char *file, int line, const char *expr) {
    odin3_abc_fail_reset(&adp_guard.pending, ODIN3_ABC_FAIL_ASSERT);
    int len = snprintf(adp_guard.pending.text, sizeof adp_guard.pending.text, "adapter: %s:%d: %s",
                       file != NULL ? file : "?", line, expr != NULL ? expr : "?");
    if (len < 0) {
        adp_guard.pending.text[0] = '\0';
    }
    adp_guard.has_pending = true;
    atomic_store(&adp_guard.disabled, true);
    return ODIN3_ERR_ENGINE;
}

odin3_status odin3_guard_finish(odin3_status st, odin3_abc_fail *fail) {
    if (st != ODIN3_ERR_ENGINE) {
        return st;
    }
    if (adp_guard.has_pending && fail != NULL && fail->kind == ODIN3_ABC_FAIL_NONE) {
        *fail = adp_guard.pending;
    }
    adp_guard.has_pending = false;
    return st;
}

/*
 * The guarded call itself: setjmp, then fn(arg). An escape on this thread comes back here with
 * ADP_ESCAPED. No local lives across setjmp (fn and arg are in adp_guard), so nothing can be
 * clobbered by the longjmp.
 */
static odin3_status adp_guard_call(void) {
    if (setjmp(adp_guard.env) != 0) {
        return ODIN3_ERR_ENGINE;
    }
    return adp_guard.fn(adp_guard.arg);
}

/* The guarded call between the fds' capture and their restore. captured false: no capture (fds
 * 1 and 2 stay as they are), for odin3_guard_run_always only. */
static odin3_status adp_guard_body(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail,
                                   bool captured) {
    odin3_abc_fail_reset(&adp_guard.escape, ODIN3_ABC_FAIL_NONE);
    adp_guard.fn = fn;
    adp_guard.arg = arg;
    adp_guard.thread = pthread_self();
    atomic_store(&adp_guard.active, true);
    odin3_status st = adp_guard_call();
    atomic_store(&adp_guard.active, false);
    /* The fds first, on every path: an escape left them on the capture file. */
    if (captured) {
        odin3_capture_end(adp_guard.cap, adp_guard.text, adp_guard.user);
    }
    if (adp_guard.escape.kind != ODIN3_ABC_FAIL_NONE) {
        *fail = adp_guard.escape;
        atomic_store(&adp_guard.disabled, true);
        return ODIN3_ERR_ENGINE;
    }
    return st;
}

static odin3_status adp_guard_run(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail,
                                  bool uncaptured_ok) {
    ODIN3_ADAPTER_ASSERT(fn != NULL && fail != NULL && adp_guard.cap != NULL);
    ODIN3_ADAPTER_ASSERT(!atomic_load(&adp_guard.active));
    odin3_status st = odin3_capture_begin(adp_guard.cap);
    if (st == ODIN3_ERR_NO_MEMORY && uncaptured_ok) {
        return adp_guard_body(fn, arg, fail, false);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    return adp_guard_body(fn, arg, fail, true);
}

odin3_status odin3_guard_run(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_guard_run(fn, arg, fail, false), fail);
}

odin3_status odin3_guard_run_always(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail) {
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_NONE);
    return odin3_guard_finish(adp_guard_run(fn, arg, fail, true), fail);
}

/* --- the escapes ----------------------------------------------------------------------------- */

/*
 * Stops the process after an off-thread escape, which has no caller to return to (a longjmp
 * across threads is undefined behaviour).
 *
 * PENDING PETER (plan point (a), question 1): the body is his answer, either __builtin_trap()
 * with a matching check_rules.py allowance, or the non-trap stop raise(SIGKILL). Until he rules,
 * SIGKILL ships (a _Noreturn function needs a body), so the adapter never returns into ABC after
 * such an escape; test_abc_adapter pins SIGKILL, and 3D-2 does not merge before the ruling.
 */
static _Noreturn void adp_offthread_stop(void) {
    for (;;) {
        (void)raise(SIGKILL);
    }
}

/* The off-thread path: the line to the original stderr, then the stop. */
static _Noreturn void adp_offthread(uint32_t kind) {
    const char *line = adp_line_assert;
    if (kind == ODIN3_ABC_FAIL_EXIT) {
        line = adp_line_exit;
    } else if (kind == ODIN3_ABC_FAIL_ABORT) {
        line = adp_line_abort;
    }
    ssize_t written = write(atomic_load(&adp_guard.err_fd), line, strlen(line));
    (void)written; /* nothing more can be done: the process stops either way */
    adp_offthread_stop();
}

/* On the guarded thread, records *what and returns to the guard; anywhere else, stops. */
static _Noreturn void adp_escape(const odin3_abc_fail *what) {
    if (atomic_load(&adp_guard.active) && pthread_equal(pthread_self(), adp_guard.thread)) {
        adp_guard.escape = *what;
        longjmp(adp_guard.env, ADP_ESCAPED);
    }
    adp_offthread(what->kind);
}

_Noreturn void odin3_wrap_exit(int code) {
    const odin3_abc_fail what = {.kind = ODIN3_ABC_FAIL_EXIT, .code = code};
    adp_escape(&what);
}

_Noreturn void odin3_wrap__exit(int code) {
    const odin3_abc_fail what = {.kind = ODIN3_ABC_FAIL_EXIT, .code = code};
    adp_escape(&what);
}

_Noreturn void odin3_wrap_abort(void) {
    const odin3_abc_fail what = {.kind = ODIN3_ABC_FAIL_ABORT};
    adp_escape(&what);
}

_Noreturn void odin3_wrap_assert_fail(const char *expr, const char *file, unsigned int line,
                                      const char *func) {
    odin3_abc_fail what = {.kind = ODIN3_ABC_FAIL_ASSERT};
    (void)func;
    int len = snprintf(what.text, sizeof what.text, "%s:%u: %s", file != NULL ? file : "?", line,
                       expr != NULL ? expr : "?");
    if (len < 0) {
        what.text[0] = '\0';
    }
    adp_escape(&what);
}

/* --- the refused process control -------------------------------------------------------------- */

/* "ABC tried to <call>; refused" into the session's output (fds 1 and 2 share the capture file
 * during a call, so the line lands after what ABC printed before it), then errno = EPERM. */
static void adp_refused(const char *call) {
    (void)fflush(stdout);
    (void)fprintf(stderr, "ABC tried to %s; refused\n", call);
    (void)fflush(stderr);
    errno = EPERM;
}

int odin3_wrap_system(const char *command) {
    (void)command;
    adp_refused("system");
    return ADP_REFUSED;
}

/* type is const-qualified in this definition only: popen's two string parameters are fixed by
 * libc, and clang-tidy's easily-swappable check counts differently qualified types as distinct. */
FILE *odin3_wrap_popen(const char *command, const char *const type) {
    (void)command;
    (void)type;
    adp_refused("popen");
    return NULL;
}

pid_t odin3_wrap_fork(void) {
    adp_refused("fork");
    return (pid_t)ADP_REFUSED;
}

int odin3_wrap_execv(const char *path, char *const argv[]) {
    (void)path;
    (void)argv;
    adp_refused("execv");
    return ADP_REFUSED;
}

int odin3_wrap_execvp(const char *file, char *const argv[]) {
    (void)file;
    (void)argv;
    adp_refused("execvp");
    return ADP_REFUSED;
}

/* ABC's Libs_Init (base/main/libSupport.c) dlopens every libabc_*.so in the working directory or
 * in $ABC_LIB_PATH at each frame's creation, and runs its abc_init: code outside every wrap. It
 * is refused, so ABC prints its own "failed to load" warning and loads nothing. */
void *odin3_wrap_dlopen(const char *file, int mode) {
    (void)file;
    (void)mode;
    adp_refused("dlopen");
    return NULL;
}

/* arg is const-qualified in this definition only (see odin3_wrap_popen). */
int odin3_wrap_execl(const char *path, const char *const arg, ...) {
    (void)path;
    (void)arg;
    adp_refused("execl");
    return ADP_REFUSED;
}

odin3_sighandler_fn odin3_wrap_signal(int sig, odin3_sighandler_fn handler) {
    (void)sig;
    (void)handler;
    adp_refused("signal");
    return SIG_ERR;
}

#ifdef ODIN3_ABC_TEST_HOOKS
odin3_status odin3_abc_test_invariant(bool ok) {
    if (!ok) {
        return odin3_adapter_assert_fail(__FILE__, __LINE__, "o3test invariant");
    }
    return ODIN3_OK;
}
#endif
