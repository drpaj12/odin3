/*
 * test_abc_route.c — the route, the program lookup, the pin check and the six diagnostics of the
 * subprocess path (3D spec §7.9, AB-33; plan Task 9b). Shell fixtures stand in for odin3-abc.
 */
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/child.h"
#include "passes/abc/script.h"
#include "unity.h"
#include "util/sha256.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ODIN3_CHILD_DATA
#error "ODIN3_CHILD_DATA must name tests/unit/data/abc/child"
#endif

enum { PATH_CAP = 512, MSG_CAP = 2048, DIR_CAP = 64, RUN = 5, SIGSEGV_NUMBER = 11, EXIT_CODE = 3 };

static char g_dir[DIR_CAP];
static char g_made[PATH_CAP]; /* the fixture written by make_pin_fixture, or "" */

void setUp(void) {
    (void)snprintf(g_dir, sizeof g_dir, "/tmp/odin3-abc-route-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_dir));
    g_made[0] = '\0';
}

void tearDown(void) {
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    odin3_abc_test_set_child_limits(NULL);
    (void)unsetenv("ODIN3_ABC_EXE");
    if (g_made[0] != '\0') {
        (void)unlink(g_made);
    }
    (void)rmdir(g_dir);
}

static void digest_of(const void *bytes, size_t len, uint8_t digest[ODIN3_SHA256_DIGEST_BYTES]) {
    odin3_sha256 sha;
    odin3_sha256_init(&sha);
    odin3_sha256_update(&sha, bytes, len);
    odin3_sha256_final(&sha, digest);
}

/* --- the route ------------------------------------------------------------------------------ */

static void test_default_script_is_linked(void) {
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    digest_of(odin3_abc_default_script, odin3_abc_default_script_len, digest);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_LINKED, odin3_abc_route(digest));
    uint8_t embedded[ODIN3_SHA256_DIGEST_BYTES];
    odin3_abc_default_sha256(embedded);
    TEST_ASSERT_EQUAL_MEMORY(embedded, digest, sizeof digest);
}

static void test_other_bytes_go_to_the_child(void) {
    unsigned char changed[512];
    TEST_ASSERT_TRUE(odin3_abc_default_script_len <= sizeof changed);
    memcpy(changed, odin3_abc_default_script, odin3_abc_default_script_len);
    changed[0] = (unsigned char)(changed[0] ^ 1U);
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    digest_of(changed, odin3_abc_default_script_len, digest);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_CHILD, odin3_abc_route(digest));
    digest_of("", 0, digest);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_CHILD, odin3_abc_route(digest));
    digest_of(changed, odin3_abc_default_script_len - 1, digest); /* one byte shorter */
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_CHILD, odin3_abc_route(digest));
}

static void test_same_bytes_from_another_path_stay_linked(void) {
    /* The route sees digests, never paths: the same bytes read from any file hash the same. */
    char path[PATH_CAP];
    (void)snprintf(path, sizeof path, "%s/copy.abc", g_dir);
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(
        odin3_abc_default_script_len,
        fwrite(odin3_abc_default_script, 1, odin3_abc_default_script_len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    unsigned char back[512];
    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    size_t got = fread(back, 1, sizeof back, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    (void)unlink(path);
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    digest_of(back, got, digest);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_LINKED, odin3_abc_route(digest));
}

static void test_route_hook(void) {
    uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
    odin3_abc_default_sha256(digest);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_ALWAYS_CHILD);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_CHILD, odin3_abc_route(digest));
    digest[0] = (uint8_t)(digest[0] ^ 1U);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_ALWAYS_LINKED);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_LINKED, odin3_abc_route(digest));
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_ROUTE_CHILD, odin3_abc_route(digest));
}

/* --- the program lookup --------------------------------------------------------------------- */

static void test_env_wins_over_the_sibling(void) {
    char want[PATH_CAP];
    (void)snprintf(want, sizeof want, "%s/ok.sh", ODIN3_CHILD_DATA);
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXE", want, 1));
    char got[PATH_CAP];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_program(got, sizeof got));
    TEST_ASSERT_EQUAL_STRING(want, got);
}

static void test_relative_env_is_made_absolute(void) {
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXE", "fixtures/odin3-abc", 1));
    char got[PATH_CAP];
    char cwd[PATH_CAP];
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_program(got, sizeof got));
    char want[2 * PATH_CAP];
    (void)snprintf(want, sizeof want, "%s/fixtures/odin3-abc", cwd);
    TEST_ASSERT_EQUAL_STRING(want, got);
}

static void test_sibling_of_the_executable(void) {
    (void)unsetenv("ODIN3_ABC_EXE");
    char self[PATH_CAP];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    TEST_ASSERT_TRUE(len > 0);
    self[len] = '\0';
    char *slash = strrchr(self, '/');
    TEST_ASSERT_NOT_NULL(slash);
    (void)snprintf(slash, sizeof self - (size_t)(slash - self), "/odin3-abc");
    char got[PATH_CAP];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_program(got, sizeof got));
    TEST_ASSERT_EQUAL_STRING(self, got);
#ifdef ODIN3_HAVE_ABC_EXE
    TEST_ASSERT_EQUAL_INT(0, access(got, X_OK)); /* the build copies odin3_abc_exe next to us */
#endif
}

static void test_program_argument_checks(void) {
    char got[PATH_CAP];
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_program(NULL, sizeof got));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_program(got, 0));
    char tiny[4];
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_program(tiny, sizeof tiny));
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXE", "/a/long/enough/path", 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_program(tiny, sizeof tiny));
}

/* --- the six texts, from real runs ---------------------------------------------------------- */

static void fixture_path(const char *name, char *path, size_t cap) {
    (void)snprintf(path, cap, "%s/%s", ODIN3_CHILD_DATA, name);
}

static void nop_text(void *user, const char *line) {
    (void)user;
    (void)line;
}

static odin3_status run_fixture(const char *name, odin3_abc_child_res *res) {
    char program[PATH_CAP];
    fixture_path(name, program, sizeof program);
    odin3_abc_child_req req = {0};
    req.program = program;
    req.option = "-f";
    req.argument = "unused.cmd";
    req.run_dir = g_dir;
    req.run = RUN;
    req.text = nop_text;
    return odin3_abc_child_run(&req, res);
}

static void check_message(const odin3_abc_child_res *res, const char *want) {
    char out[MSG_CAP];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_message(res, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void test_message_spawn(void) {
    odin3_abc_child_res res;
    char program[PATH_CAP];
    fixture_path("no-such-program", program, sizeof program);
    odin3_abc_child_req req = {0};
    req.program = program;
    req.option = "-f";
    req.argument = "unused.cmd";
    req.run_dir = g_dir;
    req.run = RUN;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_run(&req, &res));
    TEST_ASSERT_EQUAL_INT(ENOENT, res.code);
    char want[MSG_CAP];
    (void)snprintf(want, sizeof want, "abc: ABC subprocess '%s' cannot start: %s", program,
                   "No such file or directory");
    check_message(&res, want);
}

static void test_message_exit(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("exit3.sh", &res));
    check_message(&res, "abc: ABC subprocess exited with status 3 during run 5");
}

static void test_message_signal(void) {
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("segv.sh", &res));
    check_message(&res, "abc: ABC subprocess killed by signal 11 during run 5");
}

static void test_message_timeout(void) {
    odin3_abc_child_limits lowered = {1, 0};
    odin3_abc_test_set_child_limits(&lowered);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("sleep.sh", &res));
    check_message(&res, "abc: ABC subprocess exceeded 1 s wall clock during run 5");
}

static void test_message_output(void) {
    odin3_abc_child_limits lowered = {0, 4096};
    odin3_abc_test_set_child_limits(&lowered);
    odin3_abc_child_res res;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_fixture("flood.sh", &res));
    check_message(&res, "abc: ABC subprocess wrote more than 4096 bytes during run 5");
}

static void test_message_pin(void) {
    char program[PATH_CAP];
    fixture_path("badpin.sh", program, sizeof program);
    odin3_abc_child_res res = {0};
    res.run = RUN;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_check_pin(program, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_PIN, res.kind);
    TEST_ASSERT_EQUAL_STRING("0123456789ab", res.found);
    char want[MSG_CAP];
    (void)snprintf(want, sizeof want, "abc: ABC subprocess '%s' is 0123456789ab, expected %.12s",
                   program, odin3_abc_child_pin());
    check_message(&res, want);
}

static void test_message_arguments(void) {
    char out[MSG_CAP] = "stale";
    odin3_abc_child_res res = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_message(&res, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("", out); /* kind OK: nothing to say */
    res.kind = ODIN3_ABC_CHILD_EXIT;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_message(NULL, out, sizeof out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_message(&res, NULL, sizeof out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_message(&res, out, 0));
    char small[16];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_message(&res, small, sizeof small));
    TEST_ASSERT_EQUAL_size_t(sizeof small - 1, strlen(small)); /* cut, still terminated */
}

/* --- the pin check -------------------------------------------------------------------------- */

static const char *pin_hex(void) {
    return odin3_abc_child_pin();
}

static void test_pin_is_forty_hex_digits(void) {
    const char *pin = pin_hex();
    TEST_ASSERT_EQUAL_size_t(40, strlen(pin));
    for (size_t i = 0; i < 40; i++) {
        TEST_ASSERT_NOT_NULL(strchr("0123456789abcdef", pin[i]));
    }
}

/* Writes a fixture into the temp directory that answers `-c version` with the pinned commit. */
static void make_pin_fixture(const char *body_commit, char *path, size_t cap) {
    (void)snprintf(path, cap, "%s/pin.sh", g_dir);
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    (void)fprintf(file,
                  "#!/bin/sh\n"
                  "echo 'UC Berkeley, ABC 1.01 (compiled today)'\n"
                  "echo 'odin3-abc: commit %s'\n",
                  body_commit);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
    (void)snprintf(g_made, sizeof g_made, "%s", path);
}

static void test_pin_check_passes_once_per_process(void) {
    char path[PATH_CAP];
    make_pin_fixture(pin_hex(), path, sizeof path);
    odin3_abc_child_res res = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_check_pin(path, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, res.kind);
    TEST_ASSERT_EQUAL_STRING(path, res.program);
    /* The second call runs nothing: the program is gone and the answer still stands. */
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    g_made[0] = '\0';
    odin3_abc_child_res again = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_child_check_pin(path, &again));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_OK, again.kind);
}

static void test_pin_check_without_a_commit_line(void) {
    odin3_abc_child_res res = {0};
    char program[PATH_CAP];
    fixture_path("noout.sh", program, sizeof program);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_check_pin(program, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_PIN, res.kind);
    TEST_ASSERT_EQUAL_STRING("unknown", res.found);
    char want[MSG_CAP];
    (void)snprintf(want, sizeof want, "abc: ABC subprocess '%s' is unknown, expected %.12s",
                   program, pin_hex());
    check_message(&res, want);
}

static void test_pin_check_failed_runs_keep_their_kind(void) {
    odin3_abc_child_res res = {0};
    res.run = RUN;
    char program[PATH_CAP];
    fixture_path("exit3.sh", program, sizeof program);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_check_pin(program, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_EXIT, res.kind);
    TEST_ASSERT_EQUAL_INT(EXIT_CODE, res.code);
    fixture_path("no-such-program", program, sizeof program);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, odin3_abc_child_check_pin(program, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ABC_CHILD_SPAWN, res.kind);
    TEST_ASSERT_EQUAL_INT(ENOENT, res.code);
}

static void test_pin_check_arguments(void) {
    odin3_abc_child_res res = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_check_pin(NULL, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_check_pin("relative", &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_abc_child_check_pin("/x", NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_script_is_linked);
    RUN_TEST(test_other_bytes_go_to_the_child);
    RUN_TEST(test_same_bytes_from_another_path_stay_linked);
    RUN_TEST(test_route_hook);
    RUN_TEST(test_env_wins_over_the_sibling);
    RUN_TEST(test_relative_env_is_made_absolute);
    RUN_TEST(test_sibling_of_the_executable);
    RUN_TEST(test_program_argument_checks);
    RUN_TEST(test_message_spawn);
    RUN_TEST(test_message_exit);
    RUN_TEST(test_message_signal);
    RUN_TEST(test_message_timeout);
    RUN_TEST(test_message_output);
    RUN_TEST(test_message_pin);
    RUN_TEST(test_message_arguments);
    RUN_TEST(test_pin_is_forty_hex_digits);
    RUN_TEST(test_pin_check_passes_once_per_process);
    RUN_TEST(test_pin_check_without_a_commit_line);
    RUN_TEST(test_pin_check_failed_runs_keep_their_kind);
    RUN_TEST(test_pin_check_arguments);
    return UNITY_END();
}
