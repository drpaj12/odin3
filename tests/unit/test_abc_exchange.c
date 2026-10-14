/*
 * test_abc_exchange.c — the BLIF exchange of one run through the whole pass, on the fake engine and
 * on shell fixtures that stand in for odin3-abc (3D plan Task 16; spec §6.2, §7.6, §7.9, §11):
 * the files of a run, the commands ABC is given, ABC's lines in the logs, the temporary
 * directory, every failure text, and the 3D-5a carry-over: a child that exits 0 without an output
 * file is a diagnosed error, never a success. The splice is Task 17's; these tests do not look at
 * what it does.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/child.h"
#include "passes/manager.h"
#include "util/alloc.h"

#include <dirent.h>
#include <unistd.h>

static odin3_design *design;

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    odin3_pass_set_options((odin3_pass_options){0});
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXE"));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id in_b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id gate[3] = {in_a, in_b, out_y};
    (void)AFX_CELL(design, mod, "$_AND_", "g", gate, 3);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_fake_fail_on(NULL, (odin3_abc_fail){0});
    odin3_abc_test_set_engine(NULL);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

static odin3_status run_abc(const char *args) {
    return odin3_pass_run(design, "abc", odin3_bytes_cstr(args));
}

/* A fresh directory name under the test directory. */
static void run_dir(char *dir, const char *name) {
    afx_path(dir, name);
    afx_mkdir(dir);
}

static void expect_file(const char *dir, const char *name) {
    char path[AFX_JOIN];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    TEST_ASSERT_TRUE_MESSAGE(afx_exists(path), path);
}

/* --- the linked engine ----------------------------------------------------------------------- */

static void test_files_commands_and_logs(void) {
    char dir[AFX_PATH];
    char log_file[AFX_PATH];
    char args[3 * AFX_PATH];
    run_dir(dir, "run1");
    afx_path(log_file, "abc.log");
    (void)snprintf(args, sizeof args, "--via-blif %s --lut 6 --log %s", dir, log_file);
    (void)run_abc(args);
    expect_file(dir, "abc0.in.blif");
    expect_file(dir, "abc0.cmd");
    expect_file(dir, "abc0.log");
    expect_file(dir, "abc0.out.blif");
    char want[4 * AFX_PATH];
    (void)snprintf(want, sizeof want,
                   "read \"%s/abc0.in.blif\"\nstrash\nifraig -v\nscorr -v\ndc2 -v\ndch -f\nif -K 6 "
                   "-v\nmfs2 -v\nprint_stats\nwrite_hie \"%s/abc0.in.blif\" "
                   "\"%s/abc0.out.blif\"\n",
                   dir, dir, dir);
    TEST_ASSERT_EQUAL_STRING(want, odin3_abc_fake_last_commands());
    TEST_ASSERT_EQUAL_UINT32(1, odin3_abc_fake_open_count());
    char path[AFX_JOIN];
    (void)snprintf(path, sizeof path, "%s/abc0.cmd", dir);
    TEST_ASSERT_EQUAL_STRING(want, afx_slurp(path));
}

/* --log starts with the run header and holds ABC's raw lines. */
static void expect_log_file(const char *log_file) {
    const char *log_text = afx_slurp(log_file);
    TEST_ASSERT_EQUAL_INT(0, strncmp(log_text, "# abc run 0: module 'm', no flip-flops\n", 39));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "fake: if -K 6 -v\n"));
}

/* ABC's lines: DEBUG, except the printing command's, which is INFO; the run's log file and --log.
 */
static void test_abc_lines_in_logs(void) {
    char dir[AFX_PATH];
    char log_file[AFX_PATH];
    char args[3 * AFX_PATH];
    run_dir(dir, "run3");
    afx_path(log_file, "abc2.log");
    (void)snprintf(args, sizeof args, "--via-blif %s --lut 6 --log %s", dir, log_file);
    (void)run_abc(args);
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_DEBUG, "abc| fake: strash"));
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO, "abc| fake: print_stats"));
    char path[AFX_JOIN];
    (void)snprintf(path, sizeof path, "%s/abc0.log", dir);
    TEST_ASSERT_NOT_NULL(strstr(afx_slurp(path), "fake: strash\n"));
    expect_log_file(log_file);
}

/* Without --via-blif the files live in a mkdtemp directory the pass removes. */
static void test_temp_directory_is_removed(void) {
    (void)run_abc("--lut 6");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_abc_fake_open_count());
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    DIR *dir = opendir(tmp);
    TEST_ASSERT_NOT_NULL(dir);
    uint32_t left = 0;
    for (struct dirent *ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        left += strncmp(ent->d_name, "odin3-abc-", 10) == 0 ? 1U : 0U;
    }
    (void)closedir(dir);
    TEST_ASSERT_EQUAL_UINT32(0, left);
}

/* --- failures -------------------------------------------------------------------------------- */

/* The arguments of a failing run and the one `abc:` error it must log. */
typedef struct failure {
    const char *args;
    const char *line;
} failure;

#define EXPECT_FAILURE(args, line) expect_engine_failure(&(failure){(args), (line)})

static void expect_engine_failure(const failure *want) {
    char *before = afx_blif_text(design);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_abc(want->args));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, afx_count(ODIN3_LOG_ERROR, "abc:"), afx_errors());
    TEST_ASSERT_TRUE_MESSAGE(afx_has(ODIN3_LOG_ERROR, want->line), afx_errors());
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    afx_check(design);
    odin3_abc_fake_fail_on(NULL, (odin3_abc_fail){0});
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
}

static void test_failure_texts(void) {
    char dir[AFX_PATH];
    char args[2 * AFX_PATH];
    char line[4 * AFX_PATH];
    run_dir(dir, "run2");
    (void)snprintf(args, sizeof args, "--lut 6 --via-blif %s", dir);
    odin3_abc_fake_fail_on("strash", (odin3_abc_fail){ODIN3_ABC_FAIL_COMMAND, 1, ""});
    EXPECT_FAILURE(args, "abc: builtin:lib/abc/vtr.abc:5: command 'strash' failed");
    odin3_abc_fake_fail_on("scorr", (odin3_abc_fail){ODIN3_ABC_FAIL_EXIT, 3, ""});
    EXPECT_FAILURE(args, "abc: ABC called exit(3) during 'scorr -v'; linked ABC is "
                         "disabled until odin3 restarts");
    odin3_abc_fake_fail_on("dc2", (odin3_abc_fail){ODIN3_ABC_FAIL_ABORT, 0, ""});
    EXPECT_FAILURE(args, "abc: ABC aborted during 'dc2 -v'; linked ABC is disabled until odin3 "
                         "restarts");
    odin3_abc_fake_fail_on("dch", (odin3_abc_fail){ODIN3_ABC_FAIL_ASSERT, 0, "abc.c:1: x"});
    EXPECT_FAILURE(args, "abc: ABC assertion failed during 'dch -f': abc.c:1: x; linked ABC "
                         "is disabled until odin3 restarts");
    odin3_abc_fake_fail_on("write_hie", (odin3_abc_fail){ODIN3_ABC_FAIL_COMMAND, 1, ""});
    (void)snprintf(line, sizeof line,
                   "abc: internal command 'write_hie \"%s/abc0.in.blif\" "
                   "\"%s/abc0.out.blif\"' failed",
                   dir, dir);
    EXPECT_FAILURE(args, line);
    odin3_abc_fake_fail_on("read", (odin3_abc_fail){ODIN3_ABC_FAIL_RESULT, 0, "no network"});
    EXPECT_FAILURE(args, "abc: ABC returned a malformed network: no network");
    expect_file(dir, "abc0.in.blif"); /* a failed run keeps its files in a given directory */
}

/* --- the child (3D-5a carry-over) --------------------------------------------------------------
 */

/* A shell fixture that answers `-c version` with the pin and runs `body` for -f. */
typedef struct child_fixture {
    const char *name;
    const char *body;
} child_fixture;

#define MAKE_CHILD(name, body, path) make_child(&(child_fixture){(name), (body)}, (path))

static void make_child(const child_fixture *fixture, char *path) {
    afx_path(path, fixture->name);
    char text[2048];
    (void)snprintf(
        text, sizeof text,
        "#!/bin/sh\nif [ \"$1\" = \"-c\" ]; then\n  echo 'odin3-abc: commit %s'\n  exit 0\n"
        "fi\n%s\n",
        odin3_abc_child_pin(), fixture->body);
    AFX_WRITE(path, text);
    TEST_ASSERT_EQUAL_INT(0, chmod(path, 0755));
}

static void use_child(const char *path) {
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXE", path, 1));
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_ALWAYS_CHILD);
}

/* Abc_RealMain returns 0 even when a command fails: no output file is the failure. */
static void test_child_exit_zero_without_output(void) {
    char exe[AFX_PATH];
    MAKE_CHILD("child_noout.sh", "echo 'Error: boom'\nexit 0", exe);
    use_child(exe);
    EXPECT_FAILURE("--lut 6", "abc: ABC returned a malformed network: no output network (last ABC "
                              "line: 'Error: boom')");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

static void test_child_failure_kinds(void) {
    char exe[AFX_PATH];
    MAKE_CHILD("child_exit3.sh", "exit 3", exe);
    use_child(exe);
    EXPECT_FAILURE("--lut 6", "abc: ABC subprocess exited with status 3 during run 0");
    MAKE_CHILD("child_segv.sh", "kill -SEGV $$", exe);
    use_child(exe);
    EXPECT_FAILURE("--lut 6", "abc: ABC subprocess killed by signal 11 during run 0");
}

static void test_child_missing_program(void) {
    char exe[AFX_PATH];
    afx_path(exe, "no-such-abc");
    use_child(exe);
    char line[4 * AFX_PATH];
    (void)snprintf(line, sizeof line,
                   "abc: ABC subprocess '%s' cannot start: No such file or "
                   "directory",
                   exe);
    EXPECT_FAILURE("--lut 6", line);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_files_commands_and_logs);
    RUN_TEST(test_abc_lines_in_logs);
    RUN_TEST(test_temp_directory_is_removed);
    RUN_TEST(test_failure_texts);
    RUN_TEST(test_child_exit_zero_without_output);
    RUN_TEST(test_child_failure_kinds);
    RUN_TEST(test_child_missing_program);
    return UNITY_END();
}
