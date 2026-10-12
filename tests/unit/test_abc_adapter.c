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
    THREAD_EXIT_CODE = 5,
    ASSERT_LINE = 7,
    DECIMAL = 10,
    REFUSED_CALLS = 6
};

static const char OFFTHREAD_EXIT_LINE[] =
    "odin3: linked ABC called exit outside its session thread; stopping\n";
static const char MARKER[] = "o3test-marker: fd 1 after the escape";

/* --- helpers -----------------------------------------------------------------------------------
 */

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
    *refused += odin3_wrap_signal(SIGINT, SIG_IGN) == SIG_ERR && errno == EPERM;
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
        "ABC tried to execvp; refused", "ABC tried to signal; refused"};
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

/* --- the escapes, parent side ---------------------------------------------------------------- */

typedef struct child {
    int status;
    char out[OUT_CAP]; /* the child's fds 1 and 2 */
} child;

static char asan_env[LINE_CAP];

/* environ with ASAN_OPTIONS extended by detect_leaks=0 (an escape leaks ABC's frame); malloc'd. */
static char **child_env(void) {
    static const char key[] = "ASAN_OPTIONS=";
    size_t count = 0;
    while (environ[count] != NULL) {
        count++;
    }
    char **env = (char **)calloc(count + 2, sizeof *env);
    TEST_ASSERT_NOT_NULL(env);
    const char *old = "";
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(environ[i], key, sizeof key - 1) == 0) {
            old = environ[i] + sizeof key - 1;
        } else {
            env[kept++] = environ[i];
        }
    }
    int len = snprintf(asan_env, sizeof asan_env, "%s%s%sdetect_leaks=0", key, old,
                       old[0] != '\0' ? ":" : "");
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof asan_env);
    env[kept++] = asan_env;
    env[kept] = NULL;
    return env;
}

/* Runs this program with --escape <which>, its fds 1 and 2 on one file, which *res receives. */
static void run_child(const char *which, child *res) {
    static char exe[] = "/proc/self/exe";
    static char flag[] = "--escape";
    char name[LINE_CAP];
    TEST_ASSERT_TRUE(strlen(which) < sizeof name);
    memcpy(name, which, strlen(which) + 1);
    char *argv[] = {exe, flag, name, NULL};
    FILE *out = tmpfile();
    TEST_ASSERT_NOT_NULL(out);
    posix_spawn_file_actions_t actions;
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&actions));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&actions, fileno(out), 1));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_adddup2(&actions, fileno(out), 2));
    char **env = child_env();
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

/* The child exited 0 (every check passed), and printed MARKER after the escape. */
static void expect_child_ok(const child *res) {
    TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(res->status), res->out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(res->status), res->out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(res->out, MARKER), res->out);
}

static void test_escape_exit(void) {
    static child res;
    run_child("exit", &res);
    expect_child_ok(&res);
}

static void test_escape_assert(void) {
    static child res;
    run_child("assert", &res);
    expect_child_ok(&res);
}

/* exit() on a thread ABC started: the line, then the process stops by a signal. Which signal
 * (SIGILL for a trap, SIGKILL otherwise) is pinned once Peter rules on point (a). */
static void test_escape_thread_exit(void) {
    static child res;
    run_child("thread_exit", &res);
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(res.status), res.out);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(res.out, OFFTHREAD_EXIT_LINE), res.out);
}

/* The adapter assertion disables the engine, and its session's frame is leaked as after an escape,
 * so it runs in a child too. */
static void test_adapter_assert_clean(void) {
    static child res;
    run_child("invariant", &res);
    expect_child_ok(&res);
}

/* --- the escapes, child side ------------------------------------------------------------------ */

static int child_failures;

static void child_check(bool ok, const char *what) {
    if (!ok) {
        printf("o3test: FAILED: %s\n", what);
        child_failures++;
    }
}

static int o3test_exit(int argc, char **argv) {
    long code = argc > 1 ? strtol(argv[1], NULL, DECIMAL) : 1;
    printf("o3test_exit: calling exit(%ld)\n", code);
    exit((int)code);
}

static int o3test_assert(int argc, char **argv) {
    (void)argc;
    (void)argv;
    o3test_assert_fail("o3test assertion", "o3test.c", ASSERT_LINE, "o3test_assert");
}

static void *o3test_thread_body(void *arg) {
    (void)arg;
    exit(THREAD_EXIT_CODE);
}

static int o3test_thread_exit(int argc, char **argv) {
    (void)argc;
    (void)argv;
    pthread_t thread;
    if (pthread_create(&thread, NULL, o3test_thread_body, NULL) != 0) {
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
 * 2 restored; then the engine refuses every later call with DISABLED. */
static void child_escape(const char *command, const odin3_abc_fail *expected, lines *got) {
    odin3_abc_fail fail;
    struct stat out_before;
    struct stat err_before;
    child_check(odin3_abc_session_open(self(), collect, got, &fail) == ODIN3_OK, "open");
    child_check(fstat(1, &out_before) == 0 && fstat(2, &err_before) == 0, "fstat");
    odin3_status st = odin3_abc_session_exec(self(), command, &fail);
    child_check(same_file(1, &out_before) && same_file(2, &err_before), "fds 1 and 2 restored");
    child_check(st == ODIN3_ERR_ENGINE, "the escape gives ODIN3_ERR_ENGINE");
    child_check(fail.kind == expected->kind && fail.code == expected->code, "kind and code");
    child_check(strcmp(fail.text, expected->text) == 0, fail.text);
    child_check(odin3_guard_disabled(), "the engine is disabled");
    st = odin3_abc_session_exec(self(), "echo late", &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "exec after");
    odin3_abc_session_close(self());
    st = odin3_abc_session_open(self(), NULL, NULL, &fail);
    child_check(st == ODIN3_ERR_ENGINE && fail.kind == ODIN3_ABC_FAIL_DISABLED, "next open");
}

static int run_escape_case(const char *which) {
    static lines got;
    (void)odin3_abc_test_add_command("o3test_exit", o3test_exit);
    (void)odin3_abc_test_add_command("o3test_assert", o3test_assert);
    (void)odin3_abc_test_add_command("o3test_thread_exit", o3test_thread_exit);
    if (strcmp(which, "exit") == 0) {
        const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_EXIT, .code = EXIT_CODE};
        child_escape("o3test_exit 3", &expected, &got);
        child_check(has_line(&got, "o3test_exit: calling exit(3)"), "the command's line");
    } else if (strcmp(which, "assert") == 0) {
        const odin3_abc_fail expected = {.kind = ODIN3_ABC_FAIL_ASSERT,
                                         .text = "o3test.c:7: o3test assertion"};
        child_escape("o3test_assert", &expected, &got);
    } else if (strcmp(which, "invariant") == 0) {
        child_invariant();
    } else if (strcmp(which, "thread_exit") == 0) {
        odin3_abc_fail fail;
        child_check(odin3_abc_session_open(self(), NULL, NULL, &fail) == ODIN3_OK, "open");
        (void)odin3_abc_session_exec(self(), "o3test_thread_exit", &fail);
        child_check(false, "the process outlived an off-thread exit");
    } else {
        child_check(false, "unknown --escape case");
    }
    printf("%s\n", MARKER);
    return child_failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--escape") == 0) {
        return run_escape_case(argv[2]);
    }
    UNITY_BEGIN();
    RUN_TEST(test_sessions_100);
    RUN_TEST(test_capture_echo);
    RUN_TEST(test_capture_flushes_pending_output);
    RUN_TEST(test_capture_partial_line_waits);
    RUN_TEST(test_exec_failure);
    RUN_TEST(test_refused_calls);
    RUN_TEST(test_plugin_init);
    RUN_TEST(test_escape_exit);
    RUN_TEST(test_escape_assert);
    RUN_TEST(test_escape_thread_exit);
    RUN_TEST(test_adapter_assert_clean);
    return UNITY_END();
}
