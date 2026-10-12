/*
 * test_abc_adapter.c — the linked engine on the real ABC (3D spec §13.2, AB-6, AB-8; plan Task 5,
 * point (a)): sessions, the fd capture, a failing command, the refused process control, the
 * plugin entry, the escapes and the adapter assertion.
 *
 * Built from the adapter's sources with the test hooks (ODIN3_ABC_TEST_HOOKS) and linked with the
 * adapter's --wrap set, so the exit() and __assert_fail calls of the ABC commands this file adds
 * (o3test_exit, o3test_assert, o3test_thread_exit) reach the guard exactly as ABC's own would.
 * Each escape, and the adapter assertion, runs in a child process (this program with --escape
 * <case>, ASan's detect_leaks=0): each disables the engine for its process and leaks the session's
 * frame, as the engine contract says. The parent process leaks nothing.
 */
#include "odin3/odin3.h"

#include "bridge.h"
#include "guard.h"
#include "odin3_abc.h"
#include "session.h"
#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* glibc's assertion entry, by its symbol: the test link wraps it like ABC's (point (w)). */
_Noreturn void o3test_assert_fail(const char *expr, const char *file, unsigned int line,
                                  const char *func) __asm__("__assert_fail");

enum {
    MAX_LINES = 64,
    LINE_CAP = 256,
    OUT_CAP = 8192,
    SESSIONS = 100,
    EXIT_CODE = 3,
    UNDERSCORE_EXIT_CODE = 4,
    THREAD_EXIT_CODE = 5,
    ASSERT_LINE = 7,
    DECIMAL = 10,
    REFUSED_CALLS = 8,
    GENERATORS_EACH = 4,   /* AB-4's 32-bit and 64-bit generators */
    LIBC_SEED = 1,         /* bridge.c's srand seed */
    DESCRIPTOR_LIMIT = 256 /* the descriptor cap of the no-descriptor case */
};

static const char MARKER[] = "o3test-marker: fd 1 at the child's end";

/* --- helpers -------------------------------------------------------------------------------- */

typedef struct lines {
    char text[MAX_LINES][LINE_CAP];
    size_t count;
} lines;

/* The text callback: keeps every line (cut to LINE_CAP - 1 bytes). */
static void collect(void *user, const char *line) {
    lines *got = user;
    if (got->count == MAX_LINES) {
        return;
    }
    size_t len = strlen(line);
    if (len >= LINE_CAP) {
        len = LINE_CAP - 1;
    }
    memcpy(got->text[got->count], line, len);
    got->text[got->count][len] = '\0';
    got->count++;
}

static bool has_line(const lines *got, const char *line) {
    for (size_t i = 0; i < got->count; i++) {
        if (strcmp(got->text[i], line) == 0) {
            return true;
        }
    }
    return false;
}

static void *self(void) {
    return odin3_abc_session_self();
}

static char log_line[LINE_CAP];

static void log_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t len = strlen(msg);
    if (len >= LINE_CAP) {
        len = LINE_CAP - 1;
    }
    memcpy(log_line, msg, len);
    log_line[len] = '\0';
}

void setUp(void) {
}

void tearDown(void) {
}

/* --- sessions and capture --------------------------------------------------------------------- */

static void test_sessions_100(void) {
    odin3_abc_fail fail;
    for (int i = 0; i < SESSIONS; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
        odin3_abc_session_close(self());
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    odin3_abc_fail nested;
    odin3_status st = odin3_abc_session_open(self(), NULL, NULL, &nested);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, st);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_BUSY, nested.kind);
    TEST_ASSERT_FALSE(odin3_guard_disabled());
    /* The nested open left the open session alone: a new one opens. */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    odin3_abc_session_close(self());
}

/* What the test's own pipes on fds 1 and 2 saw: during the call, and after it. */
typedef struct pipe_view {
    ssize_t during_out, during_err;
    ssize_t after_out, after_err;
    char out_byte, err_byte;
} pipe_view;

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* The test's fds 1 and 2 put on fresh non-blocking pipes, and the originals kept. */
typedef struct pipes {
    int out[2], err[2];
    int keep_out, keep_err;
} pipes;

static void pipes_install(pipes *fds) {
    bool ok = pipe(fds->out) == 0 && pipe(fds->err) == 0;
    ok = ok && set_nonblocking(fds->out[0]) && set_nonblocking(fds->err[0]);
    (void)fflush(stdout);
    (void)fflush(stderr);
    fds->keep_out = dup(STDOUT_FILENO);
    fds->keep_err = dup(STDERR_FILENO);
    ok = ok && fds->keep_out >= 0 && fds->keep_err >= 0;
    ok = ok && dup2(fds->out[1], STDOUT_FILENO) >= 0 && dup2(fds->err[1], STDERR_FILENO) >= 0;
    TEST_ASSERT_TRUE(ok);
}

static void pipes_remove(pipes *fds) {
    (void)dup2(fds->keep_out, STDOUT_FILENO);
    (void)dup2(fds->keep_err, STDERR_FILENO);
    int all[] = {fds->keep_out, fds->keep_err, fds->out[0], fds->out[1], fds->err[0], fds->err[1]};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        (void)close(all[i]);
    }
}

/* Runs command with fds 1 and 2 on fresh pipes, reads them during and after, restores them. */
static odin3_status exec_on_pipes(const char *command, odin3_abc_fail *fail, pipe_view *view) {
    pipes fds;
    pipes_install(&fds);
    odin3_status st = odin3_abc_session_exec(self(), command, fail);
    char byte = 0;
    view->during_out = read(fds.out[0], &byte, 1);
    view->during_err = read(fds.err[0], &byte, 1);
    ssize_t wrote_out = write(STDOUT_FILENO, "x", 1);
    ssize_t wrote_err = write(STDERR_FILENO, "y", 1);
    view->after_out = wrote_out == 1 ? read(fds.out[0], &view->out_byte, 1) : -1;
    view->after_err = wrote_err == 1 ? read(fds.err[0], &view->err_byte, 1) : -1;
    pipes_remove(&fds);
    return st;
}

static void test_capture_echo(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    pipe_view view = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    odin3_status st = exec_on_pipes("echo hello", &fail, &view);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_size_t(1, got.count);
    TEST_ASSERT_EQUAL_STRING("hello", got.text[0]);
    /* Nothing reached the test's pipes during the call; after it they are fds 1 and 2 again. */
    TEST_ASSERT_TRUE(view.during_out < 0);
    TEST_ASSERT_TRUE(view.during_err < 0);
    TEST_ASSERT_EQUAL_INT(1, (int)view.after_out);
    TEST_ASSERT_EQUAL_INT(1, (int)view.after_err);
    TEST_ASSERT_EQUAL_CHAR('x', view.out_byte);
    TEST_ASSERT_EQUAL_CHAR('y', view.err_byte);
}

/* Output still in stdout's buffer before a call belongs to the original fd 1, not to ABC's lines:
 * capture_begin flushes it there before it moves fd 1. */
static void test_capture_flushes_pending_output(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    char seen[LINE_CAP] = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    pipes fds;
    pipes_install(&fds);
    (void)fputs("o3test-pending", stdout); /* no newline: still buffered, whatever the mode */
    odin3_status st = odin3_abc_session_exec(self(), "echo hello", &fail);
    ssize_t len = read(fds.out[0], seen, sizeof seen - 1);
    pipes_remove(&fds);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_STRING("o3test-pending", seen);
    TEST_ASSERT_EQUAL_size_t(1, got.count);
    TEST_ASSERT_EQUAL_STRING("hello", got.text[0]);
}

static void test_capture_partial_line_waits(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    odin3_status first = odin3_abc_session_exec(self(), "echo -n partial", &fail);
    size_t after_first = got.count;
    odin3_status second = odin3_abc_session_exec(self(), "echo end", &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, second);
    TEST_ASSERT_EQUAL_size_t(0, after_first);
    TEST_ASSERT_EQUAL_size_t(1, got.count);
    TEST_ASSERT_EQUAL_STRING("partial end", got.text[0]);
}

static void test_exec_failure(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    odin3_status st = odin3_abc_session_exec(self(), "strash", &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, st);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_COMMAND, fail.kind);
    TEST_ASSERT_NOT_EQUAL_INT(0, fail.code);
    TEST_ASSERT_TRUE(has_line(&got, "Error: Empty network."));
    TEST_ASSERT_FALSE(odin3_guard_disabled());
}

/* --- process control -------------------------------------------------------------------------- */

/* Calls every refused wrapper once; *refused counts those that returned their refusal and EPERM. */
static odin3_status call_refused(void *arg) {
    int *refused = arg;
    static char true_path[] = "/bin/true";
    char *const argv[] = {true_path, NULL};
    errno = 0;
    *refused += odin3_wrap_system("true") == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_popen("true", "r") == NULL && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_fork() == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_execv(true_path, argv) == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_execvp("true", argv) == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_execl(true_path, true_path, (char *)NULL) == -1 && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_signal(SIGINT, SIG_IGN) == SIG_ERR && errno == EPERM;
    errno = 0;
    *refused += odin3_wrap_dlopen("libabc_o3test.so", 0) == NULL && errno == EPERM;
    return ODIN3_OK;
}

static void test_refused_calls(void) {
    lines got = {.count = 0};
    odin3_abc_fail fail;
    int refused = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), collect, &got, &fail));
    got.count = 0;
    odin3_status st = odin3_guard_run(call_refused, &refused, &fail);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_INT(REFUSED_CALLS, refused);
    static const char *const expected[REFUSED_CALLS] = {
        "ABC tried to system; refused", "ABC tried to popen; refused",
        "ABC tried to fork; refused",   "ABC tried to execv; refused",
        "ABC tried to execvp; refused", "ABC tried to execl; refused",
        "ABC tried to signal; refused", "ABC tried to dlopen; refused"};
    TEST_ASSERT_EQUAL_size_t(REFUSED_CALLS, got.count);
    for (size_t i = 0; i < REFUSED_CALLS; i++) {
        TEST_ASSERT_EQUAL_STRING(expected[i], got.text[i]);
    }
    TEST_ASSERT_FALSE(odin3_guard_disabled());
}

/* --- the exports ------------------------------------------------------------------------------ */

static void test_plugin_init(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ABI_MISMATCH, odin3_plugin_init(ODIN3_ABI_VERSION + 1));
    /* The registry validates the table: version, every function, the 40-digit commit. */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_plugin_init(ODIN3_ABI_VERSION));
    log_line[0] = '\0';
    odin3_log_set_sink(log_sink, NULL);
    odin3_status again = odin3_abc_adapter_init();
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, again);
    TEST_ASSERT_EQUAL_STRING("odin3_abc_engine_register: abc engine already registered (linked)",
                             log_line);
}

/* --- the generators (AB-8) ------------------------------------------------------------------- */

/* AB-4's generators by symbol (this test links libabc-pic): every open resets them. */
extern unsigned Abc_Random(int reset);
extern uint64_t Abc_RandomW(int reset);
extern unsigned Aig_ManRandom(int reset);
extern uint64_t Aig_ManRandom64(int reset);
extern unsigned Gia_ManRandom(int reset);
extern uint64_t Gia_ManRandomW(int reset);
extern unsigned Random_Int(int reset);
extern uint64_t Random_Word(int reset);

/* The next value of every generator, libc's rand() included. */
typedef struct draw {
    unsigned u32[GENERATORS_EACH];
    uint64_t u64[GENERATORS_EACH];
    int libc;
} draw;

/* rand() through a pointer: cert-msc30-c refuses direct rand() calls (weak randomness), while
 * here the repeatable sequence after srand(1) is what is tested (AB-8), as in bridge.c. */
static int (*const libc_rand)(void) = rand;

/* bridge.c's resets, in its order; the seed is a parameter for the same reason as there. */
static void reset_as_open(unsigned int seed) {
    (void)Abc_Random(1);
    (void)Abc_RandomW(1);
    (void)Aig_ManRandom(1);
    (void)Aig_ManRandom64(1);
    (void)Gia_ManRandom(1);
    (void)Gia_ManRandomW(1);
    (void)Random_Int(1);
    (void)Random_Word(1);
    srand(seed);
}

static void draw_next(draw *next) {
    next->u32[0] = Abc_Random(0);
    next->u64[0] = Abc_RandomW(0);
    next->u32[1] = Aig_ManRandom(0);
    next->u64[1] = Aig_ManRandom64(0);
    next->u32[2] = Gia_ManRandom(0);
    next->u64[2] = Gia_ManRandomW(0);
    next->u32[3] = Random_Int(0);
    next->u64[3] = Random_Word(0);
    next->libc = libc_rand();
}

static void test_open_resets_generators(void) {
    draw expected;
    draw skipped;
    draw after_open;
    odin3_abc_fail fail;
    reset_as_open(LIBC_SEED);
    draw_next(&expected);
    draw_next(&skipped); /* every generator moves past its first value */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_session_open(self(), NULL, NULL, &fail));
    draw_next(&after_open);
    odin3_abc_session_close(self());
    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected.u32, after_open.u32, GENERATORS_EACH);
    TEST_ASSERT_EQUAL_MEMORY(expected.u64, after_open.u64, sizeof expected.u64);
    TEST_ASSERT_EQUAL_INT(expected.libc, after_open.libc);
}

/* --- child processes, parent side ------------------------------------------------------------- */

typedef struct child {
    int status;
    char out[OUT_CAP]; /* the child's fds 1 and 2 */
} child;

static char asan_env[LINE_CAP];
static char extra_env[LINE_CAP];

/* ASAN_OPTIONS=<old>:detect_leaks=0 into asan_env (an escape leaks ABC's frame). */
static void set_asan_env(const char *old) {
    static const char key[] = "ASAN_OPTIONS=";
    int len = snprintf(asan_env, sizeof asan_env, "%s%s%sdetect_leaks=0", key, old,
                       old[0] != '\0' ? ":" : "");
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof asan_env);
}

/* environ without its ASAN_OPTIONS, then asan_env, then extra (NULL: none); malloc'd. */
static char **child_env(const char *extra) {
    static const char key[] = "ASAN_OPTIONS=";
    size_t count = 0;
    while (environ[count] != NULL) {
        count++;
    }
    char **env = (char **)calloc(count + 3, sizeof *env);
    const char *old = "";
    size_t kept = 0;
    for (size_t i = 0; env != NULL && i < count; i++) {
        if (strncmp(environ[i], key, sizeof key - 1) == 0) {
            old = environ[i] + sizeof key - 1;
        } else {
            env[kept++] = environ[i];
        }
    }
    TEST_ASSERT_NOT_NULL(env);
    set_asan_env(old);
    env[kept++] = asan_env;
    if (extra != NULL && strlen(extra) < sizeof extra_env) {
        memcpy(extra_env, extra, strlen(extra) + 1);
        env[kept++] = extra_env;
    }
    env[kept] = NULL;
    return env;
}

/* One child run: the --escape case, and one extra environment entry (NULL: none). */
typedef struct child_req {
    const char *which;
    const char *extra;
} child_req;

/* Runs this program with --escape <req->which> and req->extra in its environment, its fds 1 and
 * 2 on one file, which *res receives. */
static void run_child(const child_req *req, child *res) {
    static char exe[] = "/proc/self/exe";
    static char flag[] = "--escape";
    char name[LINE_CAP];
    TEST_ASSERT_TRUE(strlen(req->which) < sizeof name);
    memcpy(name, req->which, strlen(req->which) + 1);
    char *argv[] = {exe, flag, name, NULL};
    FILE *out = tmpfile();
    TEST_ASSERT_NOT_NULL(out);
    posix_spawn_file_actions_t actions;
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&actions));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&actions, fileno(out), 1));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&actions, fileno(out), 2));
    char **env = child_env(req->extra);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe, &actions, NULL, argv, env);
    (void)posix_spawn_file_actions_destroy(&actions);
    free((void *)env);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &res->status, 0));
    TEST_ASSERT_EQUAL_INT(0, fseek(out, 0, SEEK_SET));
    size_t got = fread(res->out, 1, OUT_CAP - 1, out);
    res->out[got] = '\0';
    (void)fclose(out);
}

/* The child exited 0 (every check passed) and printed MARKER at its end. */
static void expect_child_ok(const char *which, const char *extra) {
    static child res;
    const child_req req = {.which = which, .extra = extra};
    run_child(&req, &res);
    TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(res.status), res.out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(res.status), res.out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(res.out, MARKER), res.out);
}

/* The case name and the expected off-thread line of one escaping call. */
typedef struct offthread_names {
    char which[LINE_CAP];
    char line[LINE_CAP];
} offthread_names;

static bool offthread_names_fill(offthread_names *names, const char *call) {
    int which = snprintf(names->which, sizeof names->which, "thread_%s", call);
    int line = snprintf(names->line, sizeof names->line,
                        "odin3: linked ABC called %s outside its session thread; stopping\n", call);
    return which > 0 && (size_t)which < sizeof names->which && line > 0 &&
           (size_t)line < sizeof names->line;
}

/* The case thread_<call> printed "odin3: linked ABC called <call> outside its session thread;
 * stopping", then died by SIGKILL: the off-thread stop that ships until Peter rules on point (a)
 * (a trap would make it SIGILL). */
static void expect_offthread_stop(const char *call) {
    static child res;
    static offthread_names names;
    TEST_ASSERT_TRUE(offthread_names_fill(&names, call));
    const child_req req = {.which = names.which, .extra = NULL};
    run_child(&req, &res);
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(res.status), res.out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIGKILL, WTERMSIG(res.status), res.out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(res.out, names.line), res.out);
}

static void test_escape_exit(void) {
    expect_child_ok("exit", NULL);
}

static void test_escape_underscore_exit(void) {
    expect_child_ok("_exit", NULL);
}

static void test_escape_abort(void) {
    expect_child_ok("abort", NULL);
}

static void test_escape_assert(void) {
    expect_child_ok("assert", NULL);
}

static void test_escape_thread_exit(void) {
    expect_offthread_stop("exit");
}

static void test_escape_thread_abort(void) {
    expect_offthread_stop("abort");
}

static void test_escape_thread_assert(void) {
    expect_offthread_stop("assert");
}

/* The adapter assertion disables the engine and leaks the session's frame as an escape does, so
 * it runs in a child too. */
static void test_adapter_assert_clean(void) {
    expect_child_ok("invariant", NULL);
}

/* Review finding C1: a libabc_*.so in the working directory, or in $ABC_LIB_PATH, is never
 * loaded (its constructor would exit 9). */
static void test_planted_library_in_cwd(void) {
    expect_child_ok("planted_cwd", NULL);
}

static void test_planted_library_on_abc_lib_path(void) {
    expect_child_ok("planted_env", "ABC_LIB_PATH=" ODIN3_ABC_PLANTED_DIR);
}

static void test_close_without_descriptors(void) {
    expect_child_ok("no_descriptors", NULL);
}

/* --- child processes, child side ---------------------------------------------------------------
 */

static int child_failures;

static void child_check(bool ok, const char *what) {
    if (!ok) {
        printf("o3test: FAILED: %s\n", what);
        child_failures++;
    }
}

static long code_arg(int argc, char **argv) {
    return argc > 1 ? strtol(argv[1], NULL, DECIMAL) : 1;
}

static int o3test_exit(int argc, char **argv) {
    long code = code_arg(argc, argv);
    printf("o3test_exit: calling exit(%ld)\n", code);
    exit((int)code);
}

static int o3test_underscore_exit(int argc, char **argv) {
    long code = code_arg(argc, argv);
    printf("o3test__exit: calling _exit(%ld)\n", code);
    (void)fflush(stdout); /* _exit flushes nothing */
    _exit((int)code);
}

static int o3test_abort(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("o3test_abort: calling abort()\n");
    abort();
}

static int o3test_assert(int argc, char **argv) {
    (void)argc;
    (void)argv;
    o3test_assert_fail("o3test assertion", "o3test.c", ASSERT_LINE, "o3test_assert");
}

/* The new thread's escape, chosen by the command's name (o3test_thread_<exit|abort|assert>). */
static void *o3test_thread_body(void *arg) {
    const char *name = arg;
    if (strcmp(name, "o3test_thread_abort") == 0) {
        abort();
    }
    if (strcmp(name, "o3test_thread_assert") == 0) {
        o3test_assert_fail("o3test thread assertion", "o3test.c", ASSERT_LINE, "thread");
    }
    exit(THREAD_EXIT_CODE);
}

static int o3test_thread(int argc, char **argv) {
    (void)argc;
    pthread_t thread;
    if (pthread_create(&thread, NULL, o3test_thread_body, argv[0]) != 0) {
        return 1;
    }
    (void)pthread_join(thread, NULL);
    return 0;
}

static odin3_status call_invariant(void *arg) {
    const bool *holds = arg;
    return odin3_abc_test_invariant(*holds);
}

/* odin3_abc_test_invariant inside a guarded call: OK while it holds; then {ASSERT, 0, "adapter:
 * adapters/abc/guard.c:<line>: o3test invariant"}, a clean return, and DISABLED afterwards. */
static void child_invariant(void) {
    static const char prefix[] = "adapter: adapters/abc/";
    static const char suffix[] = ": o3test invariant";
    odin3_abc_fail fail;
    bool holds = true;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    child_check(odin3_guard_run(call_invariant, &holds, &fail) == ODIN3_OK, "a holding invariant");
    holds = false;
    odin3_status st = odin3_guard_run(call_invariant, &holds, &fail);
    child_check(st == ODIN3_ERR_ENGINE, "a failing invariant gives ODIN3_ERR_ENGINE");
    child_check(fail.kind == ODIN3_ABC_FAIL_ASSERT && fail.code == 0, "kind ASSERT, code 0");
    size_t len = strlen(fail.text);
    bool framed = len > sizeof prefix + sizeof suffix &&
                  strncmp(fail.text, prefix, sizeof prefix - 1) == 0 &&
                  strcmp(fail.text + len - (sizeof suffix - 1), suffix) == 0;
    child_check(framed, fail.text);
    printf("o3test: adapter assertion text: %s\n", fail.text);
    child_check(odin3_guard_disabled(), "the engine is disabled");
    odin3_abc_session_close(self());
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "next open");
}

static bool same_file(int fd, const struct stat *before) {
    struct stat now;
    return fstat(fd, &now) == 0 && now.st_dev == before->st_dev && now.st_ino == before->st_ino;
}

/* Runs command in a fresh session: it must escape as expected (kind, code, text), with fds 1 and
 * 2 restored and the command's own line (said, if not NULL) delivered; then the engine refuses
 * every later call with DISABLED. */
static void child_escape(const char *command, const odin3_abc_fail *expected, const char *said) {
    static lines got;
    odin3_abc_fail fail;
    struct stat out_before;
    struct stat err_before;
    child_check(odin3_abc_session_open(self(), collect, &got, &fail) == ODIN3_OK, "open");
    child_check(fstat(1, &out_before) == 0 && fstat(2, &err_before) == 0, "fstat");
    odin3_status st = odin3_abc_session_exec(self(), command, &fail);
    child_check(same_file(1, &out_before) && same_file(2, &err_before), "fds 1 and 2 restored");
    child_check(st == ODIN3_ERR_ENGINE, "the escape gives ODIN3_ERR_ENGINE");
    child_check(fail.kind == expected->kind && fail.code == expected->code, "kind and code");
    child_check(strcmp(fail.text, expected->text) == 0, fail.text);
    child_check(said == NULL || has_line(&got, said), "the command's line");
    child_check(odin3_guard_disabled(), "the engine is disabled");
    st = odin3_abc_session_exec(self(), "echo late", &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "exec after");
    odin3_abc_session_close(self());
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "next open");
}

static void case_exit(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_EXIT, .code = EXIT_CODE};
    child_escape("o3test_exit 3", &expected, "o3test_exit: calling exit(3)");
}

static void case_underscore_exit(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_EXIT, .code = UNDERSCORE_EXIT_CODE};
    child_escape("o3test__exit 4", &expected, "o3test__exit: calling _exit(4)");
}

static void case_abort(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_ABORT};
    child_escape("o3test_abort", &expected, "o3test_abort: calling abort()");
}

static void case_assert(void) {
    const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_ASSERT,
                                     .text = "o3test.c:7: o3test assertion"};
    child_escape("o3test_assert", &expected, NULL);
}

/* An escape on a thread of its own: the process must stop inside exec. */
static void child_offthread(const char *command) {
    odin3_abc_fail fail;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    (void)odin3_abc_session_exec(self(), command, &fail);
    child_check(false, "the process outlived an off-thread escape");
}

static void case_thread_exit(void) {
    child_offthread("o3test_thread_exit");
}

static void case_thread_abort(void) {
    child_offthread("o3test_thread_abort");
}

static void case_thread_assert(void) {
    child_offthread("o3test_thread_assert");
}

/* A session opened beside libabc_o3test.so (in the cwd or on $ABC_LIB_PATH): the dlopen is
 * refused, nothing is loaded (the library's constructor would exit 9), the session runs. */
static void child_planted(void) {
    static lines got;
    odin3_abc_fail fail;
    child_check(odin3_abc_session_open(self(), collect, &got, &fail) == ODIN3_OK, "open");
    child_check(has_line(&got, "ABC tried to dlopen; refused"), "the dlopen was refused");
    child_check(!odin3_guard_disabled(), "the engine stays enabled");
    odin3_status st = odin3_abc_session_exec(self(), "echo alive", &fail);
    child_check(st == ODIN3_OK && has_line(&got, "alive"), "the session runs");
    odin3_abc_session_close(self());
    for (size_t i = 0; i < got.count; i++) {
        printf("o3test: line: %s\n", got.text[i]);
    }
}

static void case_planted_cwd(void) {
    child_check(chdir(ODIN3_ABC_PLANTED_DIR) == 0, "chdir to the planted library");
    child_planted();
}

static void case_planted_env(void) {
    child_planted();
}

/* With no descriptor left, exec reports NO_MEMORY, and close still leaves and destroys the
 * frame: the engine stays usable once descriptors are back (review finding 6). */
static void case_no_descriptors(void) {
    odin3_abc_fail fail;
    struct rlimit limit;
    child_check(getrlimit(RLIMIT_NOFILE, &limit) == 0, "getrlimit");
    if (limit.rlim_cur > DESCRIPTOR_LIMIT) {
        limit.rlim_cur = DESCRIPTOR_LIMIT;
        child_check(setrlimit(RLIMIT_NOFILE, &limit) == 0, "setrlimit");
    }
    static int held[DESCRIPTOR_LIMIT];
    size_t count = 0;
    child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
    for (int fd = dup(STDERR_FILENO); fd >= 0 && count < DESCRIPTOR_LIMIT;
         fd = dup(STDERR_FILENO)) {
        held[count++] = fd;
    }
    odin3_status st = odin3_abc_session_exec(self(), "echo starved", &fail);
    child_check(st == ODIN3_ERR_NO_MEMORY, "exec without descriptors gives NO_MEMORY");
    odin3_abc_session_close(self());
    child_check(!odin3_guard_disabled(), "the engine stays enabled");
    for (size_t i = 0; i < count; i++) {
        (void)close(held[i]);
    }
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_OK, "the next open works");
    odin3_abc_session_close(self());
}

static const struct {
    const char *name;
    void (*run)(void);
} CHILD_CASES[] = {
    {"exit", case_exit},
    {"_exit", case_underscore_exit},
    {"abort", case_abort},
    {"assert", case_assert},
    {"invariant", child_invariant},
    {"thread_exit", case_thread_exit},
    {"thread_abort", case_thread_abort},
    {"thread_assert", case_thread_assert},
    {"planted_cwd", case_planted_cwd},
    {"planted_env", case_planted_env},
    {"no_descriptors", case_no_descriptors},
};

static int run_child_case(const char *which) {
    (void)odin3_abc_test_add_command("o3test_exit", o3test_exit);
    (void)odin3_abc_test_add_command("o3test__exit", o3test_underscore_exit);
    (void)odin3_abc_test_add_command("o3test_abort", o3test_abort);
    (void)odin3_abc_test_add_command("o3test_assert", o3test_assert);
    (void)odin3_abc_test_add_command("o3test_thread_exit", o3test_thread);
    (void)odin3_abc_test_add_command("o3test_thread_abort", o3test_thread);
    (void)odin3_abc_test_add_command("o3test_thread_assert", o3test_thread);
    bool known = false;
    for (size_t i = 0; i < sizeof CHILD_CASES / sizeof CHILD_CASES[0]; i++) {
        if (strcmp(which, CHILD_CASES[i].name) == 0) {
            CHILD_CASES[i].run();
            known = true;
        }
    }
    child_check(known, "unknown --escape case");
    printf("%s\n", MARKER);
    return child_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--escape") == 0) {
        return run_child_case(argv[2]);
    }
    UNITY_BEGIN();
    RUN_TEST(test_sessions_100);
    RUN_TEST(test_capture_echo);
    RUN_TEST(test_capture_flushes_pending_output);
    RUN_TEST(test_capture_partial_line_waits);
    RUN_TEST(test_exec_failure);
    RUN_TEST(test_refused_calls);
    RUN_TEST(test_open_resets_generators);
    RUN_TEST(test_plugin_init);
    RUN_TEST(test_escape_exit);
    RUN_TEST(test_escape_underscore_exit);
    RUN_TEST(test_escape_abort);
    RUN_TEST(test_escape_assert);
    RUN_TEST(test_escape_thread_exit);
    RUN_TEST(test_escape_thread_abort);
    RUN_TEST(test_escape_thread_assert);
    RUN_TEST(test_adapter_assert_clean);
    RUN_TEST(test_planted_library_in_cwd);
    RUN_TEST(test_planted_library_on_abc_lib_path);
    RUN_TEST(test_close_without_descriptors);
    return UNITY_END();
}
