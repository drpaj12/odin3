/*
 * test_abc_pass.c — the abc pass as a whole (3D plan Task 19; spec §13.1 to §13.3, §13.6): the
 * fake engine's identity mapping on every data and golden BLIF (simulation equal), the views
 * refusal, the real engine through the odin3 CLI (a second pass on the result is still
 * equivalent), an out-of-memory sweep over the whole pass, the end state of a pass (no deleted
 * slots, no temporary directory left), the `$lut` round trip, and every engine failure text.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "abc_sim.h"
#include "abc_spawn.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/manager.h"
#include "util/alloc.h"

#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif
#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository"
#endif

enum { OOM_LIMIT = 30000, MAX_LISTED = 64, MAX_DESIGNS = 128 };

static odin3_design *design;
static char start_dir[PATH_MAX];

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
    TEST_ASSERT_NOT_NULL(getcwd(start_dir, sizeof start_dir));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_fake_fail_on(NULL, (odin3_abc_fail){0});
    odin3_abc_test_set_engine(NULL);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_AUTO);
    odin3_log_set_sink(NULL, NULL);
    odin3_log_mute(false);
    TEST_ASSERT_EQUAL_INT(0, chdir(start_dir));
    odin3_design_destroy(design);
}

/* --- reading a design ------------------------------------------------------------------------ */

static void fresh_design(void) {
    odin3_design_destroy(design);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

static void read_vtr_techlib(void) {
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
}

static odin3_status read_design(const char *path) {
    fresh_design();
    read_vtr_techlib();
    return odin3_blif_read(design, path);
}

/* The data BLIF called name (without .blif) of tests/unit/data/abc. */
static void data_path(char *path, size_t cap, const char *name) {
    (void)snprintf(path, cap, "%s/tests/unit/data/abc/%s.blif", ODIN3_SOURCE_DIR, name);
}

static void read_data(const char *name) {
    char path[AFX_JOIN];
    data_path(path, sizeof path, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_design(path), path);
}

static odin3_status run_abc(const char *args) {
    return odin3_pass_run(design, "abc", odin3_bytes_cstr(args));
}

/* True when the top module simulates (muted: a derived clock or a loop does not). */
static bool can_simulate(void) {
    odin3_sim *sim = NULL;
    odin3_log_mute(true);
    odin3_status st = odin3_sim_build(design, odin3_design_top(design), &sim);
    odin3_log_mute(false);
    odin3_sim_destroy(sim);
    return st == ODIN3_OK;
}

/* --- every data and golden BLIF through the fake engine -------------------------------------- */

typedef struct listed {
    char names[MAX_LISTED][AFX_PATH];
    uint32_t count;
} listed;

/* True for a line of the list that names a design (not a comment or a blank). */
static bool names_a_design(const char *line) {
    return line[0] != '#' && line[0] != '\n';
}

/* The paths of tests/abc/equiv_expect_fail.txt (the first word of each line). */
static void read_expect_list(listed *out) {
    char path[AFX_JOIN];
    (void)snprintf(path, sizeof path, "%s/tests/abc/equiv_expect_fail.txt", ODIN3_SOURCE_DIR);
    FILE *file = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char line[AFX_PATH * 2];
    out->count = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        if (!names_a_design(line)) {
            continue;
        }
        line[strcspn(line, " \t\n")] = '\0';
        TEST_ASSERT_TRUE(out->count < MAX_LISTED);
        (void)snprintf(out->names[out->count++], AFX_PATH, "%s", line);
    }
    (void)fclose(file);
}

static bool is_listed(const listed *list, const char *path) {
    size_t len = strlen(path);
    for (uint32_t i = 0; i < list->count; i++) {
        size_t nlen = strlen(list->names[i]);
        if (nlen <= len && strcmp(path + len - nlen, list->names[i]) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct design_list {
    char paths[MAX_DESIGNS][AFX_JOIN];
    uint32_t count;
} design_list;

static bool is_blif_name(const char *name) {
    size_t len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".blif") == 0;
}

static void add_directory(design_list *out, const char *dir) {
    DIR *handle = opendir(dir);
    TEST_ASSERT_NOT_NULL_MESSAGE(handle, dir);
    for (struct dirent *ent = readdir(handle); ent != NULL; ent = readdir(handle)) {
        if (is_blif_name(ent->d_name)) {
            TEST_ASSERT_TRUE(out->count < MAX_DESIGNS);
            (void)snprintf(out->paths[out->count++], AFX_JOIN, "%s/%s", dir, ent->d_name);
        }
    }
    (void)closedir(handle);
}

static int compare_paths(const void *lhs, const void *rhs) {
    return strcmp(lhs, rhs);
}

static void all_designs(design_list *out) {
    char dir[AFX_JOIN];
    out->count = 0;
    (void)snprintf(dir, sizeof dir, "%s/tests/unit/data/abc", ODIN3_SOURCE_DIR);
    add_directory(out, dir);
    (void)snprintf(dir, sizeof dir, "%s/tests/golden/blif", ODIN3_SOURCE_DIR);
    add_directory(out, dir);
    qsort(out->paths, out->count, sizeof out->paths[0], compare_paths);
}

/* One design through `abc --lut 6` on the fake engine: valid, and simulation-equal when it
 * simulates. */
static void expect_identity_mapping(const char *path) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_design(path), path);
    asim_trace before = {0};
    bool simulated = can_simulate();
    if (simulated) {
        asim_record(design, &before);
    }
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_abc("--lut 6"), path);
    afx_check(design);
    if (simulated) {
        asim_expect_equal(design, &before);
    }
    asim_free(&before);
}

static void test_every_design_maps_to_an_equal_netlist(void) {
    listed expect;
    design_list designs;
    read_expect_list(&expect);
    all_designs(&designs);
    uint32_t mapped = 0;
    for (uint32_t i = 0; i < designs.count; i++) {
        if (is_listed(&expect, designs.paths[i])) {
            continue;
        }
        expect_identity_mapping(designs.paths[i]);
        mapped++;
    }
    TEST_ASSERT_TRUE_MESSAGE(mapped >= 5, "too few designs were mapped");
}

/* pow.parmys.blif has a $pow word cell: the views error, nothing changed (ruling I8). */
static void test_views_refusal_on_pow(void) {
    char path[AFX_JOIN];
    (void)snprintf(path, sizeof path, "%s/tests/golden/blif/pow.parmys.blif", ODIN3_SOURCE_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_design(path));
    char *before = afx_blif_text(design);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, run_abc("--lut 6"));
    const char *line =
        afx_only(ODIN3_LOG_ERROR, "abc: module 'pow' is not in the netlist view: $pow '");
    TEST_ASSERT_NOT_NULL(strstr(line, "'; run lower first"));
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_abc_fake_open_count());
}

/* --- the end state of a pass ----------------------------------------------------------------- */

static uint32_t left_over_directories(void) {
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    DIR *handle = opendir(tmp);
    TEST_ASSERT_NOT_NULL(handle);
    uint32_t left = 0;
    for (struct dirent *ent = readdir(handle); ent != NULL; ent = readdir(handle)) {
        left += strncmp(ent->d_name, "odin3-abc-", 10) == 0 ? 1U : 0U;
    }
    (void)closedir(handle);
    return left;
}

/* Counts the dead slots of a module: nodes, nets and wires. */
static uint32_t dead_slots(const odin3_module *mod) {
    uint32_t dead = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        dead += odin3_node_live(mod, (odin3_node_id){id}) ? 0U : 1U;
    }
    for (uint32_t id = 1; id < odin3_module_net_end(mod); id++) {
        dead += odin3_net_live(mod, (odin3_net_id){id}) ? 0U : 1U;
    }
    for (uint32_t id = 1; id < odin3_module_wire_end(mod); id++) {
        dead += odin3_wire_live(mod, (odin3_wire_id){id}) ? 0U : 1U;
    }
    return dead;
}

/* Point (y): nothing deleted is left in the module after the pass; point (l): no temporary
 * directory is left without --via-blif. */
static void test_no_deleted_slots_and_no_temp_directory(void) {
    read_data("derived_clock");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_abc("--lut 6"));
    afx_check(design);
    TEST_ASSERT_EQUAL_UINT32(0, dead_slots(afx_top(design)));
    TEST_ASSERT_EQUAL_UINT32(0, left_over_directories());
}

/* write_blif, read_blif, write_blif give the same text, and the tables are the same function. */
static void test_lut_round_trip(void) {
    read_data("t1");
    asim_trace after = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_abc("--lut 6"));
    asim_record(design, &after);
    char first_path[AFX_PATH];
    char second_path[AFX_PATH];
    afx_path(first_path, "round1.blif");
    afx_path(second_path, "round2.blif");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(design, first_path));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_design(first_path));
    asim_expect_equal(design, &after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(design, second_path));
    char *first = strdup(afx_slurp(first_path));
    TEST_ASSERT_EQUAL_STRING(first, afx_slurp(second_path));
    free(first);
    asim_free(&after);
}

/* --- the real engine, through the CLI -------------------------------------------------------- */

static void test_real_engine_script_and_second_pass(void) {
#ifdef ODIN3_HAVE_REAL_ABC
    char script[AFX_PATH];
    afx_path(script, "strash_if.abc");
    AFX_WRITE(script, "strash\nif -K 6\n");
    char data[AFX_JOIN];
    char one[AFX_PATH];
    char two[AFX_PATH];
    char base[AFX_PATH];
    data_path(data, sizeof data, "t1");
    afx_path(base, "real0.blif");
    afx_path(one, "real1.blif");
    afx_path(two, "real2.blif");
    char cmd[AFX_CMD_TEXT];
    (void)snprintf(cmd, sizeof cmd,
                   "read_techlib %s/vtr.o3lib; read_blif %s; write_blif %s; abc --script %s; "
                   "write_blif %s; abc --script %s; write_blif %s",
                   ODIN3_LIB_DIR, data, base, script, one, script, two);
    TEST_ASSERT_EQUAL_INT(0, afx_run_odin3(cmd));
    TEST_ASSERT_EQUAL_INT(0, afx_run_equiv(base, one, ""));
    TEST_ASSERT_EQUAL_INT(0, afx_run_equiv(base, two, ""));
#else
    TEST_IGNORE_MESSAGE("built without ODIN3_WITH_ABC");
#endif
}

/* --- out of memory --------------------------------------------------------------------------- */

/* Every allocation of the whole pass fails in turn on one design. Statuses allowed: NO_MEMORY, IO
 * (1C's reader reports a failed buffer as IO) and ENGINE (the fake engine reads the exchange file
 * with the same reader); the IR stays valid and simulation equal, and a success ends the sweep. */
static bool sweep_status_allowed(odin3_status st) {
    return st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY || st == ODIN3_ERR_IO ||
           st == ODIN3_ERR_ENGINE;
}

static void sweep_pass(const char *name) {
    char path[AFX_JOIN];
    data_path(path, sizeof path, name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_design(path));
    asim_trace before = {0};
    bool simulated = can_simulate();
    if (simulated) {
        asim_record(design, &before);
    }
    bool succeeded = false;
    for (long nth = 0; nth < OOM_LIMIT && !succeeded; nth++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, read_design(path));
        odin3_util_set_alloc_fail_after(nth);
        odin3_status st = run_abc("--lut 6");
        odin3_util_set_alloc_fail_after(-1);
        succeeded = st == ODIN3_OK;
        TEST_ASSERT_TRUE_MESSAGE(sweep_status_allowed(st), name);
        afx_check(design);
        if (simulated) {
            asim_expect_equal(design, &before);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(succeeded, name);
    TEST_ASSERT_EQUAL_UINT32(0, left_over_directories());
    asim_free(&before);
}

static void test_oom_sweep_t1(void) {
    sweep_pass("t1");
}

static void test_oom_sweep_derived_clock(void) {
    sweep_pass("derived_clock");
}

static void test_oom_sweep_toggle_init1(void) {
    sweep_pass("toggle_init1");
}

/* --- engine failures -------------------------------------------------------------------------- */

typedef struct failure {
    const char *script;
    const char *word;
    odin3_abc_fail fail;
    const char *line;
} failure;

static void expect_failure(const failure *want) {
    char dir[AFX_PATH];
    afx_path(dir, "failing");
    afx_mkdir(dir);
    TEST_ASSERT_EQUAL_INT(0, chdir(dir));
    AFX_WRITE("s.abc", want->script);
    odin3_abc_test_set_route(ODIN3_ABC_TEST_ROUTE_ALWAYS_LINKED);
    read_data("t1");
    char *before = afx_blif_text(design);
    asim_trace trace = {0};
    asim_record(design, &trace);
    odin3_abc_fake_fail_on(want->word, want->fail);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, run_abc("--lut 6 --script s.abc"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, afx_count(ODIN3_LOG_ERROR, "abc:"), afx_errors());
    TEST_ASSERT_TRUE_MESSAGE(afx_has(ODIN3_LOG_ERROR, want->line), afx_errors());
    char *after = afx_blif_text(design);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    afx_check(design);
    asim_expect_equal(design, &trace);
    asim_free(&trace);
    odin3_abc_fake_fail_on(NULL, (odin3_abc_fail){0});
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    TEST_ASSERT_EQUAL_INT(0, chdir(start_dir));
}

#define EXPECT_FAILURE(...) expect_failure(&(failure){__VA_ARGS__})

static void test_failure_texts(void) {
    EXPECT_FAILURE("resyn2\nif -K {K}\n", "balance", {ODIN3_ABC_FAIL_COMMAND, 1, ""},
                   "abc: s.abc:1: command 'balance' failed (from alias 'resyn2')");
    EXPECT_FAILURE("strash\n&get\n&if -K {K}\n", "&put", {ODIN3_ABC_FAIL_COMMAND, 1, ""},
                   "abc: internal command '&put' failed");
    EXPECT_FAILURE("strash\nif -K {K}\n", "strash", {ODIN3_ABC_FAIL_EXIT, 3, ""},
                   "abc: ABC called exit(3) during 'strash'; linked ABC is disabled until odin3 "
                   "restarts");
    EXPECT_FAILURE("strash\nif -K {K}\n", "strash", {ODIN3_ABC_FAIL_ABORT, 0, ""},
                   "abc: ABC aborted during 'strash'; linked ABC is disabled until odin3 "
                   "restarts");
    EXPECT_FAILURE("strash\nif -K {K}\n", "strash", {ODIN3_ABC_FAIL_ASSERT, 0, "abc.c:1: x"},
                   "abc: ABC assertion failed during 'strash': abc.c:1: x; linked ABC is disabled "
                   "until odin3 restarts");
    EXPECT_FAILURE("strash\nif -K {K}\n", "strash", {ODIN3_ABC_FAIL_DISABLED, 0, ""},
                   "abc: linked ABC is disabled by an earlier failure in this process");
    EXPECT_FAILURE("strash\nif -K {K}\n", "strash", {ODIN3_ABC_FAIL_BUSY, 0, ""},
                   "abc: linked ABC is busy (another session is open)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_design_maps_to_an_equal_netlist);
    RUN_TEST(test_views_refusal_on_pow);
    RUN_TEST(test_no_deleted_slots_and_no_temp_directory);
    RUN_TEST(test_lut_round_trip);
    RUN_TEST(test_real_engine_script_and_second_pass);
    RUN_TEST(test_oom_sweep_t1);
    RUN_TEST(test_oom_sweep_derived_clock);
    RUN_TEST(test_oom_sweep_toggle_init1);
    RUN_TEST(test_failure_texts);
    return UNITY_END();
}
