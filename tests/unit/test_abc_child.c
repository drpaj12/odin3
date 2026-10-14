/*
 * test_abc_child.c — the child runner (3D spec §7.9, AB-33; plan Task 9a). Shell fixtures stand in
 * for odin3-abc: output order and the last line without a newline, exit status, a signal, the
 * wall clock with a grandchild that must die, the output cap, a missing program, the child's
 * environment, stdin and core limit, and an out-of-memory sweep with no child left behind.
 */
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/child.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/clock.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ODIN3_CHILD_DATA
#error "ODIN3_CHILD_DATA must name tests/unit/data/abc/child"
#endif

enum {
    MAX_LINES = 64,
    LINE_CAP = 256,
    PATH_CAP = 512,
    DIR_CAP = 64,
    SIGSEGV_NUMBER = 11,
    EXIT_CODE = 3,
    SMALL_CAP = 4096,
    LONG_LINE = 100000,
    ONE_SECOND = 1,
    TIMEOUT_SLACK_MS = 3000,
    GONE_WAIT_MS = 3000,
    POLL_MS = 20,
    OOM_MAX = 64,
    CORE_SOFT = 1 << 20
};

static char g_dir[DIR_CAP];
static char g_lines[MAX_LINES][LINE_CAP];
static size_t g_count;
static size_t g_total;

static void collect(void *user, const char *line) {
    (void)user;
    size_t len = strlen(line);
    g_total += len;
    if (g_count < MAX_LINES) {
        if (len >= LINE_CAP) {
            len = LINE_CAP - 1;
        }
        memcpy(g_lines[g_count], line, len);
        g_lines[g_count][len] = '\0';
    }
    g_count++;
}

void setUp(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/odin3-abc-child-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
    g_count = 0;
    g_total = 0;
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_test_set_child_limits(NULL);
    char pid_file[PATH_CAP];
    snprintf(pid_file, sizeof pid_file, "%s/grandchild.pid", g_dir);
    (void)unlink(pid_file);
    (void)rmdir(g_dir);
}

static odin3_abc_child_req request(const char *fixture, char *program, size_t cap) {
    snprintf(program, cap, "%s/%s", ODIN3_CHILD_DATA, fixture);
    odin3_abc_child_req req = {0};
    req.program = program;
    req.option = "-f";
    req.argument = "unused.cmd";
    req.run_dir = g_dir;
    req.run = 7;
    req.text = collect;
    return req;
}

static odin3_status run_fixture(const char *fixture, odin3_abc_child_res *res) {
    char program[PATH_CAP];
    odin3_abc_child_req req = request(fixture, program, sizeof program);
    return odin3_abc_child_run(&req, res);
}

/* True when this process has no child left, running or zombie. */
static bool no_children(void) {
    int status = 0;
    pid_t got = waitpid(-1, &status, WNOHANG);
    return got == -1 && errno == ECHILD;
}

/* True once pid is gone or only a zombie (its parent, init, reaps it in its own time). */
static bool process_gone(pid_t pid) {
    if (kill(pid, 0) != 0) {
        return errno == ESRCH;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return true;
    }
    char text[LINE_CAP];
    size_t got = fread(text, 1, sizeof text - 1, file);
    (void)fclose(file);
    text[got] = '\0';
    const char *close_paren = strrchr(text, ')');
    return close_paren != NULL && close_paren[1] == ' ' && close_paren[2] == 'Z';
}

static void test_ok_lines_in_order(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_fixture("ok.sh", &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
    TEST_ASSERT_EQUAL_INT(0, res.code);
    TEST_ASSERT_EQUAL_size_t(3, g_count);
    TEST_ASSERT_EQUAL_STRING("first line", g_lines[0]);
    TEST_ASSERT_EQUAL_STRING("second line", g_lines[1]);
    TEST_ASSERT_EQUAL_STRING("last line, no newline", g_lines[2]);
    TEST_ASSERT_EQUAL_UINT64(strlen("first line\nsecond line\nlast line, no newline"),
                             res.output_bytes);
    TEST_ASSERT_EQUAL_UINT32(7, res.run);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_CHILD_SECONDS, res.seconds);
    TEST_ASSERT_EQUAL_UINT64(ODIN3_ABC_CHILD_OUTPUT_BYTES, res.output_limit);
    TEST_ASSERT_TRUE(no_children());
}

static void test_no_output(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_fixture("noout.sh", &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
    TEST_ASSERT_EQUAL_size_t(0, g_count);
    TEST_ASSERT_EQUAL_UINT64(0, res.output_bytes);
}

static void test_null_text_drops_output(void) {
    char program[PATH_CAP];
    odin3_abc_child_req req = request("ok.sh", program, sizeof program);
    req.text = NULL;
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_run(&req, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
    TEST_ASSERT_EQUAL_size_t(0, g_count);
}

static void test_exit_status(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("exit3.sh", &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_EXIT, res.kind);
    TEST_ASSERT_EQUAL_INT(EXIT_CODE, res.code);
    TEST_ASSERT_EQUAL_size_t(1, g_count);
    TEST_ASSERT_EQUAL_STRING("about to fail", g_lines[0]);
    TEST_ASSERT_TRUE(no_children());
}

static void test_signal(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("segv.sh", &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_SIGNAL, res.kind);
    TEST_ASSERT_EQUAL_INT(SIGSEGV_NUMBER, res.code);
    TEST_ASSERT_TRUE(no_children());
}

/* The grandchild's pid, from the file the fixture wrote in the run directory; -1 if unreadable. */
static pid_t read_grandchild(void) {
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/grandchild.pid", g_dir);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    long pid = -1;
    if (fscanf(file, "%ld", &pid) != 1) {
        pid = -1;
    }
    (void)fclose(file);
    return (pid_t)pid;
}

static void test_timeout_kills_the_group(void) {
    char program[PATH_CAP];
    odin3_abc_child_req req = request("sleep.sh", program, sizeof program);
    req.seconds = ONE_SECOND;
    odin3_abc_child_res res;
    double start = odin3_now_ms();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_run(&req, &res));
    TEST_ASSERT_TRUE(odin3_now_ms() - start < TIMEOUT_SLACK_MS);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_TIMEOUT, res.kind);
    TEST_ASSERT_EQUAL_UINT32(ONE_SECOND, res.seconds);
    TEST_ASSERT_TRUE(no_children());
    pid_t grandchild = read_grandchild();
    TEST_ASSERT_TRUE(grandchild > 1);
    for (int waited = 0; waited < GONE_WAIT_MS && !process_gone(grandchild); waited += POLL_MS) {
        struct timespec nap = {0, POLL_MS * 1000000L};
        (void)nanosleep(&nap, NULL);
    }
    TEST_ASSERT_TRUE(process_gone(grandchild));
}

static void test_test_hook_lowers_limits(void) {
    odin3_abc_child_limits lowered = {ONE_SECOND, 0};
    odin3_abc_test_set_child_limits(&lowered);
    odin3_abc_child_res res;
    double start = odin3_now_ms();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("sleep.sh", &res));
    TEST_ASSERT_TRUE(odin3_now_ms() - start < TIMEOUT_SLACK_MS);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_TIMEOUT, res.kind);
    TEST_ASSERT_EQUAL_UINT32(ONE_SECOND, res.seconds);
}

static void test_output_cap(void) {
    char program[PATH_CAP];
    odin3_abc_child_req req = request("flood.sh", program, sizeof program);
    req.output_bytes = SMALL_CAP;
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_run(&req, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OUTPUT, res.kind);
    TEST_ASSERT_EQUAL_UINT64(SMALL_CAP, res.output_limit);
    TEST_ASSERT_TRUE(res.output_bytes > SMALL_CAP);
    TEST_ASSERT_TRUE(no_children());
}

static void test_output_cap_by_hook(void) {
    odin3_abc_child_limits lowered = {0, SMALL_CAP};
    odin3_abc_test_set_child_limits(&lowered);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("flood.sh", &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OUTPUT, res.kind);
    TEST_ASSERT_EQUAL_UINT64(SMALL_CAP, res.output_limit);
}

static void test_long_line_is_cut(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_fixture("longline.sh", &res));
    TEST_ASSERT_TRUE(g_count > 2);
    TEST_ASSERT_EQUAL_STRING("after", g_lines[g_count - 1]);
    TEST_ASSERT_EQUAL_size_t((size_t)LONG_LINE + strlen("after"), g_total);
}

static void test_missing_program(void) {
    char program[PATH_CAP];
    odin3_abc_child_req req = request("no-such-program", program, sizeof program);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_run(&req, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_SPAWN, res.kind);
    TEST_ASSERT_EQUAL_INT(ENOENT, res.code);
    TEST_ASSERT_EQUAL_STRING(program, res.program);
    TEST_ASSERT_TRUE(no_children());
}

/* The value of KEY=value in the collected lines, or NULL. */
static const char *env_value(const char *key) {
    size_t len = strlen(key);
    for (size_t i = 0; i < g_count && i < MAX_LINES; i++) {
        if (strncmp(g_lines[i], key, len) == 0 && g_lines[i][len] == '=') {
            return g_lines[i] + len + 1;
        }
    }
    return NULL;
}

static bool line_is(const char *text) {
    for (size_t i = 0; i < g_count && i < MAX_LINES; i++) {
        if (strcmp(g_lines[i], text) == 0) {
            return true;
        }
    }
    return false;
}

static void test_environment_stdin_core_cwd(void) {
    struct rlimit core;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_CORE, &core));
    struct rlimit saved = core;
    if (core.rlim_max == RLIM_INFINITY || core.rlim_max >= CORE_SOFT) {
        core.rlim_cur = CORE_SOFT;
        TEST_ASSERT_EQUAL_INT(0, setrlimit(RLIMIT_CORE, &core));
    }
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_CHILD_SECRET", "leak", 1));
    odin3_abc_child_res res;
    odin3_status st = run_fixture("env.sh", &res);
    (void)unsetenv("ODIN3_CHILD_SECRET");
    struct rlimit after;
    TEST_ASSERT_EQUAL_INT(0, getrlimit(RLIMIT_CORE, &after));
    TEST_ASSERT_EQUAL_UINT64(core.rlim_cur, after.rlim_cur); /* the parent's limit is restored */
    (void)setrlimit(RLIMIT_CORE, &saved);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    for (size_t i = 0; i < g_count && i < MAX_LINES; i++) {
        const char *eq = strchr(g_lines[i], '=');
        if (eq == NULL) {
            continue;
        }
        size_t key_len = (size_t)(eq - g_lines[i]);
        bool known = (key_len == 4 && strncmp(g_lines[i], "PATH", 4) == 0) ||
                     (key_len == 4 && strncmp(g_lines[i], "HOME", 4) == 0) ||
                     (key_len == 6 && strncmp(g_lines[i], "TMPDIR", 6) == 0) ||
                     (key_len == 6 && strncmp(g_lines[i], "LC_ALL", 6) == 0) ||
                     (key_len == 3 && strncmp(g_lines[i], "PWD", 3) == 0); /* the shell's own */
        if (strncmp(g_lines[i], "args:", 5) == 0 || strncmp(g_lines[i], "pwd:", 4) == 0) {
            known = true;
        }
        TEST_ASSERT_TRUE_MESSAGE(known, g_lines[i]);
    }
    TEST_ASSERT_NOT_NULL(env_value("PATH"));
    TEST_ASSERT_NOT_NULL(env_value("HOME"));
    TEST_ASSERT_NOT_NULL(env_value("TMPDIR"));
    TEST_ASSERT_EQUAL_STRING("C", env_value("LC_ALL"));
    TEST_ASSERT_TRUE(line_is("stdin-eof"));
    TEST_ASSERT_TRUE(line_is("core:0"));
    char want[PATH_CAP + 8];
    snprintf(want, sizeof want, "pwd:%s", g_dir);
    TEST_ASSERT_TRUE(line_is(want));
    TEST_ASSERT_TRUE(line_is("args:-f unused.cmd"));
}

static void test_invalid_requests(void) {
    char program[PATH_CAP];
    odin3_abc_child_req req = request("ok.sh", program, sizeof program);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(NULL, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(&req, NULL));
    odin3_abc_child_req bad = req;
    bad.program = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(&bad, &res));
    bad = req;
    bad.program = "relative/odin3-abc"; /* the working directory would change what it names */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(&bad, &res));
    bad = req;
    bad.run_dir = "";
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(&bad, &res));
    bad = req;
    bad.option = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(&bad, &res));
    bad = req;
    bad.argument = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_run(&bad, &res));
    TEST_ASSERT_TRUE(no_children());
}

/* Every allocation of a run fails once in turn: NO_MEMORY, nothing spawned, nothing leaked. */
static void test_oom_sweep(void) {
    bool done = false;
    for (long n = 0; n < OOM_MAX && !done; n++) {
        odin3_abc_child_res res;
        g_count = 0;
        odin3_util_set_alloc_fail_after(n);
        odin3_status st = run_fixture("ok.sh", &res);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_size_t(3, g_count);
            done = true;
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_size_t(0, g_count);
        }
        TEST_ASSERT_TRUE(no_children());
    }
    TEST_ASSERT_TRUE(done);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ok_lines_in_order);
    RUN_TEST(test_no_output);
    RUN_TEST(test_null_text_drops_output);
    RUN_TEST(test_exit_status);
    RUN_TEST(test_signal);
    RUN_TEST(test_timeout_kills_the_group);
    RUN_TEST(test_test_hook_lowers_limits);
    RUN_TEST(test_output_cap);
    RUN_TEST(test_output_cap_by_hook);
    RUN_TEST(test_long_line_is_cut);
    RUN_TEST(test_missing_program);
    RUN_TEST(test_environment_stdin_core_cwd);
    RUN_TEST(test_invalid_requests);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
