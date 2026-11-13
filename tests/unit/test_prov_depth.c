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

static void read_tiny(void) {
    FILE *file = fopen(BLIF_PATH, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(1, (int)fwrite(TINY, strlen(TINY), 1, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_blif", BLIF_PATH));
}

static void test_parse_level(void) {
    bool autolevel = true;
    uint32_t level = 99;
    for (uint32_t i = 0; i <= 4; i++) {
        char text[2] = {(char)('0' + i), '\0'};
        TEST_ASSERT_TRUE(odin3_prov_depth_parse_level(text, &autolevel, &level));
        TEST_ASSERT_FALSE(autolevel);
        TEST_ASSERT_EQUAL_UINT32(i, level);
    }
    TEST_ASSERT_TRUE(odin3_prov_depth_parse_level("auto", &autolevel, &level));
    TEST_ASSERT_TRUE(autolevel);
    const char *const bad[] = {"", "5", "-1", "01", "10", "Auto", "full", " 2", "2 "};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(odin3_prov_depth_parse_level(bad[i], &autolevel, &level), bad[i]);
    }
    TEST_ASSERT_FALSE(odin3_prov_depth_parse_level(NULL, &autolevel, &level));
}

static void test_parse_size(void) {
    uint64_t bytes = 0;
    TEST_ASSERT_TRUE(odin3_prov_depth_parse_size("1", &bytes));
    TEST_ASSERT_EQUAL_UINT64(1, bytes);
    TEST_ASSERT_TRUE(odin3_prov_depth_parse_size("500K", &bytes));
    TEST_ASSERT_EQUAL_UINT64(500U * KIB, bytes);
    TEST_ASSERT_TRUE(odin3_prov_depth_parse_size("2M", &bytes));
    TEST_ASSERT_EQUAL_UINT64(2U * MIB, bytes);
    TEST_ASSERT_TRUE(odin3_prov_depth_parse_size("3G", &bytes));
    TEST_ASSERT_EQUAL_UINT64(3ULL * MIB * KIB, bytes);
    const char *const bad[] = {"",    "0",   "0K",  "K",    "1.5G",  "1g", "1T",
                               "-1",  "1KB", "1 K", "G1",   "99999999999999999999",
                               "17179869184G"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(odin3_prov_depth_parse_size(bad[i], &bytes), bad[i]);
    }
    TEST_ASSERT_FALSE(odin3_prov_depth_parse_size(NULL, &bytes));
}

static void test_knobs(void) {
    odin3_prov_knobs k0 = odin3_prov_depth_knobs(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_OFF, k0.ast);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_EDGES_BULK, k0.edges);
    TEST_ASSERT_FALSE(k0.map);
    TEST_ASSERT_FALSE(k0.cut);
    TEST_ASSERT_FALSE(k0.steps);
    odin3_prov_knobs k1 = odin3_prov_depth_knobs(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONSTRUCT_MODE_REFS, k1.ast);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_EDGES_BULK, k1.edges);
    TEST_ASSERT_TRUE(k1.map);
    TEST_ASSERT_FALSE(k1.cut);
    odin3_prov_knobs k2 = odin3_prov_depth_knobs(2);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_EDGES_DEFAULT, k2.edges);
    TEST_ASSERT_FALSE(k2.cut);
    TEST_ASSERT_FALSE(k2.steps);
    odin3_prov_knobs k3 = odin3_prov_depth_knobs(3);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_EDGES_DEFAULT, k3.edges);
    TEST_ASSERT_TRUE(k3.cut);
    TEST_ASSERT_FALSE(k3.steps);
    odin3_prov_knobs k4 = odin3_prov_depth_knobs(4);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_EDGES_UNLIMITED, k4.edges);
    TEST_ASSERT_TRUE(k4.cut);
    TEST_ASSERT_TRUE(k4.steps);
    TEST_ASSERT_TRUE(k4.map);
}

static void test_request_and_setter(void) {
    TEST_ASSERT_FALSE(odin3_prov_depth_request(NULL).set);
    TEST_ASSERT_EQUAL_INT(-1, odin3_prov_depth_requested(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("3", "2M"));
    odin3_prov_request req = odin3_prov_depth_request(design);
    TEST_ASSERT_TRUE(req.set);
    TEST_ASSERT_FALSE(req.autolevel);
    TEST_ASSERT_EQUAL_UINT32(3, req.level);
    TEST_ASSERT_EQUAL_UINT64(2U * MIB, req.budget);
    TEST_ASSERT_EQUAL_INT(3, odin3_prov_depth_requested(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("auto", NULL));
    TEST_ASSERT_TRUE(odin3_prov_depth_request(design).autolevel);
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_requested(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth(NULL, NULL));
    TEST_ASSERT_FALSE(odin3_prov_depth_request(design).set);
}

static void test_setter_rejects_and_keeps(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("2", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_set_prov_depth("7", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_set_prov_depth("1", "0"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_set_prov_depth("1", "12X"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_prov_depth_request(design).level);
    TEST_ASSERT_TRUE(log_has("prov-level must be"));
    TEST_ASSERT_TRUE(log_has("prov-budget must be"));
}

static void test_estimate_is_monotone(void) {
    odin3_prov_shape shape = {1000, 5000};
    odin3_prov_estimate est;
    odin3_prov_depth_estimate(&shape, &est);
    TEST_ASSERT_EQUAL_UINT64(0, odin3_prov_estimate_total(&est, 0));
    for (uint32_t d = 1; d < ODIN3_PROV_DEPTH_COUNT; d++) {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64(odin3_prov_estimate_total(&est, d - 1),
                                            odin3_prov_estimate_total(&est, d));
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64(est.records[d - 1], est.records[d]);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT64(est.edges[d - 1], est.edges[d]);
    }
    TEST_ASSERT_GREATER_THAN_UINT64(0, est.map_bytes[1]);
    TEST_ASSERT_EQUAL_UINT64(0, est.step_bytes[3]);
    TEST_ASSERT_GREATER_THAN_UINT64(0, est.step_bytes[4]);
    odin3_prov_shape bigger = {2000, 10000};
    odin3_prov_estimate more;
    odin3_prov_depth_estimate(&bigger, &more);
    TEST_ASSERT_GREATER_THAN_UINT64(odin3_prov_estimate_total(&est, 4),
                                    odin3_prov_estimate_total(&more, 4));
}

static void test_measure_counts_cells(void) {
    read_tiny();
    odin3_prov_shape shape = odin3_prov_depth_measure(design);
    TEST_ASSERT_GREATER_THAN_UINT64(0, shape.nodes);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(shape.nodes, shape.pins);
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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("3", NULL));
    TEST_ASSERT_EQUAL_INT(3, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(odin3_prov_depth_wants_map(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("0", NULL));
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design));
    TEST_ASSERT_FALSE(odin3_prov_depth_wants_map(design));
}

static void test_budget_drops_a_fixed_depth_with_a_warning(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("4", "1"));
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(log_has("provenance depth 4 (full) is estimated at"));
    TEST_ASSERT_TRUE(log_has("over the budget of 1 bytes; using depth 0 (off)"));
    size_t before = log_len;
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design)); /* resolved once */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)before, (uint64_t)log_len);
}

static void test_auto_takes_the_highest_that_fits(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("auto", "1G"));
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(log_has("prov-level auto: depth 4 (full)"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("auto", "1"));
    TEST_ASSERT_EQUAL_INT(0, odin3_prov_depth_effective(design));
}

static void test_budget_alone_is_auto_and_auto_alone_warns(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth(NULL, "1G"));
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_effective(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("auto", NULL));
    TEST_ASSERT_EQUAL_INT(4, odin3_prov_depth_effective(design));
    TEST_ASSERT_TRUE(log_has("prov-level auto needs --prov-budget"));
}

static void test_prov_estimate_pass(void) {
    read_tiny();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "prov-estimate", ""));
    TEST_ASSERT_TRUE(log_has("prov-estimate: depth 0 (off): 0 records"));
    TEST_ASSERT_TRUE(log_has("prov-estimate: depth 4 (full)"));
    TEST_ASSERT_FALSE(log_has("in force"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, "prov-estimate", "x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_set_prov_depth("2", NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "prov-estimate", ""));
    TEST_ASSERT_TRUE(log_has("prov-estimate: in force: depth 2 (ancestry)"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_level);
    RUN_TEST(test_parse_size);
    RUN_TEST(test_knobs);
    RUN_TEST(test_request_and_setter);
    RUN_TEST(test_setter_rejects_and_keeps);
    RUN_TEST(test_estimate_is_monotone);
    RUN_TEST(test_measure_counts_cells);
    RUN_TEST(test_no_request_is_today);
    RUN_TEST(test_fixed_depth_without_budget);
    RUN_TEST(test_budget_drops_a_fixed_depth_with_a_warning);
    RUN_TEST(test_auto_takes_the_highest_that_fits);
    RUN_TEST(test_budget_alone_is_auto_and_auto_alone_warns);
    RUN_TEST(test_prov_estimate_pass);
    return UNITY_END();
}
