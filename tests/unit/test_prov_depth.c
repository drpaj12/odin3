/*
 * test_prov_depth.c — unit tests for the provenance depth: option parsing, the depth-to-knobs
 * table, the request, the estimate and the budget (docs/specs/2026-11-13-prov-levels.md).
 */
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/prov_depth.h"
#include "unity.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_CAP = 1 << 15, KIB = 1024, MIB = 1024 * 1024 };

static odin3_design *design;
static char log_text[LOG_CAP];
static size_t log_len;
static odin3_pass_options saved_opts;

static const char *const BLIF_PATH = "odin3_prov_depth_test.blif";
static const char *const TINY = ".model top\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n";

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%d %s\n", (int)level, msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

static bool log_has(const char *needle) {
    return strstr(log_text, needle) != NULL;
}

void setUp(void) {
    log_text[0] = '\0';
    log_len = 0;
    odin3_log_set_sink(capture_sink, NULL);
    saved_opts = odin3_pass_get_options();
    odin3_prov_depth_reset();
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_pass_set_options(saved_opts);
    odin3_prov_depth_reset();
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    (void)remove(BLIF_PATH);
}

typedef struct depth_req {
    const char *level;
    const char *budget;
} depth_req;

/* Both setters; the first failure's status. */
static odin3_status set_depth(depth_req req) {
    odin3_status st = odin3_pass_set_prov_level(req.level);
    return st == ODIN3_OK ? odin3_pass_set_prov_budget(req.budget) : st;
}

static void read_tiny(void) {
    FILE *file = fopen(BLIF_PATH, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(1, (int)fwrite(TINY, strlen(TINY), 1, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_blif", BLIF_PATH));
}

static void expect_level(const char *text, bool want_auto, uint32_t want_level) {
    bool autolevel = !want_auto;
    uint32_t level = 99;
    TEST_ASSERT_TRUE_MESSAGE(odin3_prov_depth_parse_level(text, &autolevel, &level), text);
    TEST_ASSERT_EQUAL_INT(want_auto, autolevel);
    TEST_ASSERT_EQUAL_UINT32(want_level, level);
}

static void expect_bad_level(const char *text) {
    bool autolevel = false;
    uint32_t level = 0;
    TEST_ASSERT_FALSE_MESSAGE(odin3_prov_depth_parse_level(text, &autolevel, &level), text);
}

static void test_parse_level(void) {
    expect_level("0", false, 0);
    expect_level("1", false, 1);
    expect_level("2", false, 2);
    expect_level("3", false, 3);
    expect_level("4", false, 4);
    expect_level("auto", true, ODIN3_PROV_DEPTH_FULL);
}

static void test_parse_level_rejects(void) {
    const char *const bad[] = {"", "5", "-1", "01", "10", "Auto", "full", " 2", "2 ", NULL};
    for (size_t at = 0; at < sizeof bad / sizeof bad[0]; at++) {
        expect_bad_level(bad[at]);
    }
}

static void expect_size(const char *text, uint64_t want) {
    uint64_t bytes = 0;
    TEST_ASSERT_TRUE_MESSAGE(odin3_prov_depth_parse_size(text, &bytes), text);
    TEST_ASSERT_EQUAL_UINT64(want, bytes);
}

static void test_parse_size(void) {
    expect_size("1", 1);
    expect_size("500K", (uint64_t)500 * KIB);
    expect_size("2M", (uint64_t)2 * MIB);
    expect_size("3G", 3ULL * MIB * KIB);
    expect_size("500k", (uint64_t)500 * KIB);
    expect_size("2m", (uint64_t)2 * MIB);
    expect_size("3g", 3ULL * MIB * KIB);
}

static void test_parse_size_rejects(void) {
    const char *const bad[] = {"",
                               "0",
                               "0K",
                               "K",
                               "1.5G",
                               "1kk",
                               "1T",
                               "-1",
                               "1KB",
                               "1 K",
                               "G1",
                               "99999999999999999999",
                               "17179869184G",
                               NULL};
    uint64_t bytes = 0;
    for (size_t at = 0; at < sizeof bad / sizeof bad[0]; at++) {
        TEST_ASSERT_FALSE(odin3_prov_depth_parse_size(bad[at], &bytes));
    }
}

static void expect_knobs(uint32_t depth, odin3_prov_knobs want) {
    odin3_prov_knobs got = odin3_prov_depth_knobs(depth);
    TEST_ASSERT_EQUAL_INT(want.ast, got.ast);
    TEST_ASSERT_EQUAL_INT(want.edges, got.edges);
    TEST_ASSERT_EQUAL_INT(want.cut, got.cut);
    TEST_ASSERT_EQUAL_INT(want.map, got.map);
    TEST_ASSERT_EQUAL_INT(want.steps, got.steps);
}

static void test_knobs_off_and_statement(void) {
    expect_knobs(0, (odin3_prov_knobs){ODIN3_CONSTRUCT_MODE_OFF, ODIN3_PROV_EDGES_BULK, false,
                                       false, false});
    expect_knobs(1, (odin3_prov_knobs){ODIN3_CONSTRUCT_MODE_REFS, ODIN3_PROV_EDGES_BULK, false,
                                       true, false});
}

static void test_knobs_ancestry_and_cut(void) {
    expect_knobs(2, (odin3_prov_knobs){ODIN3_CONSTRUCT_MODE_REFS, ODIN3_PROV_EDGES_DEFAULT, false,
                                       true, false});
    expect_knobs(3, (odin3_prov_knobs){ODIN3_CONSTRUCT_MODE_REFS, ODIN3_PROV_EDGES_DEFAULT, true,
                                       true, false});
}

static void test_knobs_full(void) {
    odin3_prov_knobs full = {ODIN3_CONSTRUCT_MODE_REFS, ODIN3_PROV_EDGES_UNLIMITED, true, true,
                             true};
    expect_knobs(ODIN3_PROV_DEPTH_FULL, full);
    expect_knobs(9, full);
}

static void test_request_level_and_budget(void) {
    TEST_ASSERT_FALSE(odin3_prov_depth_request(NULL).set);
    TEST_ASSERT_EQUAL_INT(-1, odin3_prov_depth_requested(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"3", "2M"}));
    odin3_prov_request req = odin3_prov_depth_request(design);
    TEST_ASSERT_TRUE(req.set);
    TEST_ASSERT_FALSE(req.autolevel);
    TEST_ASSERT_EQUAL_UINT32(3, req.level);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)2 * MIB, req.budget);
    TEST_ASSERT_EQUAL_INT(3, odin3_prov_depth_requested(design));
}

static void test_request_auto_and_none(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"auto", NULL}));
    TEST_ASSERT_TRUE(odin3_prov_depth_request(design).autolevel);
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_requested(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){NULL, NULL}));
    TEST_ASSERT_FALSE(odin3_prov_depth_request(design).set);
}

static void test_setters_reject_and_keep(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"2", "5K"}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_set_prov_level("7"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_set_prov_budget("0"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_set_prov_budget("12X"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_prov_depth_request(design).level);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)5 * KIB, odin3_prov_depth_request(design).budget);
    TEST_ASSERT_TRUE(log_has("prov-level must be"));
    TEST_ASSERT_TRUE(log_has("prov-budget must be"));
}

/* Depth 1 shares one bulk record per abc run, which can be bigger than the per-LUT records of
 * depth 2, so the estimate is not asked to rise from 1 to 2 (docs/specs/2026-11-13-prov-levels.md).
 */
static void expect_not_smaller(const odin3_prov_estimate *est, uint32_t depth) {
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(odin3_prov_estimate_total(est, depth - 1),
                                        odin3_prov_estimate_total(est, depth));
}

static void test_estimate_is_monotone(void) {
    odin3_prov_shape shape = {{1000, 5000, 2000, 100, 300, 300, 300, 10, 10, 500}};
    odin3_prov_estimate est;
    odin3_prov_depth_estimate(&shape, &est);
    TEST_ASSERT_EQUAL_UINT64(0, odin3_prov_estimate_total(&est, 0));
    TEST_ASSERT_GREATER_THAN_UINT64(0, odin3_prov_estimate_total(&est, 1));
    expect_not_smaller(&est, 3);
    expect_not_smaller(&est, ODIN3_PROV_DEPTH_FULL);
    TEST_ASSERT_GREATER_THAN_UINT64(0, est.map_bytes[1]);
    TEST_ASSERT_EQUAL_UINT64(0, est.step_bytes[3]);
    TEST_ASSERT_GREATER_THAN_UINT64(0, est.step_bytes[ODIN3_PROV_DEPTH_FULL]);
}

static void test_estimate_grows_with_the_design(void) {
    odin3_prov_shape shape = {{1000, 5000, 2000, 100, 300, 300, 300, 10, 10, 500}};
    odin3_prov_shape bigger = {{2000, 10000, 4000, 200, 600, 600, 600, 20, 20, 1000}};
    odin3_prov_estimate small_est;
    odin3_prov_estimate big_est;
    odin3_prov_depth_estimate(&shape, &small_est);
    odin3_prov_depth_estimate(&bigger, &big_est);
    TEST_ASSERT_GREATER_THAN_UINT64(odin3_prov_estimate_total(&small_est, ODIN3_PROV_DEPTH_FULL),
                                    odin3_prov_estimate_total(&big_est, ODIN3_PROV_DEPTH_FULL));
}

static void test_measure_counts_cells(void) {
    read_tiny();
    odin3_prov_shape shape = odin3_prov_depth_measure(design);
    TEST_ASSERT_GREATER_THAN_UINT64(0, shape.f[ODIN3_PROV_F_CELLS]);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(shape.f[ODIN3_PROV_F_CELLS], shape.f[ODIN3_PROV_F_PINS]);
}

static void test_no_request_is_today(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(-1, odin3_prov_depth_effective(design));
    TEST_ASSERT_FALSE(odin3_prov_depth_wants_map(design));
    size_t before = log_len;
    odin3_prov_depth_after_proc(design);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)before, (uint64_t)log_len);
}

static void test_fixed_depth_without_budget(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"3", NULL}));
    TEST_ASSERT_EQUAL_INT(3, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(odin3_prov_depth_wants_map(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"0", NULL}));
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design));
    TEST_ASSERT_FALSE(odin3_prov_depth_wants_map(design));
}

static void test_budget_drops_a_fixed_depth_with_a_warning(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"4", "1"}));
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(log_has("provenance depth 4 (full) is estimated at"));
    TEST_ASSERT_TRUE(log_has("over the budget of 1 bytes; using depth 0 (off)"));
    size_t before = log_len;
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design)); /* resolved once */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)before, (uint64_t)log_len);
}

static void test_auto_takes_the_highest_that_fits(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"auto", "1G"}));
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(log_has("prov-level auto: depth 4 (full)"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"auto", "1"}));
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design));
}

static void test_budget_alone_is_auto_and_auto_alone_warns(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){NULL, "1G"}));
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_effective(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"auto", NULL}));
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(log_has("prov-level auto needs --prov-budget"));
}

static void test_prov_estimate_pass(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "prov_estimate", ""));
    TEST_ASSERT_TRUE(log_has("prov_estimate: depth 0 (off): 0 records"));
    TEST_ASSERT_TRUE(log_has("prov_estimate: depth 4 (full)"));
    TEST_ASSERT_FALSE(log_has("in force"));
}

static void test_prov_estimate_pass_arguments_and_depth(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_run_pass(design, "prov_estimate", "x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, set_depth((depth_req){"2", NULL}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "prov_estimate", ""));
    TEST_ASSERT_TRUE(log_has("prov_estimate: in force: depth 2 (ancestry)"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_level);
    RUN_TEST(test_parse_level_rejects);
    RUN_TEST(test_parse_size);
    RUN_TEST(test_parse_size_rejects);
    RUN_TEST(test_knobs_off_and_statement);
    RUN_TEST(test_knobs_ancestry_and_cut);
    RUN_TEST(test_knobs_full);
    RUN_TEST(test_request_level_and_budget);
    RUN_TEST(test_request_auto_and_none);
    RUN_TEST(test_setters_reject_and_keep);
    RUN_TEST(test_estimate_is_monotone);
    RUN_TEST(test_estimate_grows_with_the_design);
    RUN_TEST(test_measure_counts_cells);
    RUN_TEST(test_no_request_is_today);
    RUN_TEST(test_fixed_depth_without_budget);
    RUN_TEST(test_budget_drops_a_fixed_depth_with_a_warning);
    RUN_TEST(test_auto_takes_the_highest_that_fits);
    RUN_TEST(test_budget_alone_is_auto_and_auto_alone_warns);
    RUN_TEST(test_prov_estimate_pass);
    RUN_TEST(test_prov_estimate_pass_arguments_and_depth);
    return UNITY_END();
}
