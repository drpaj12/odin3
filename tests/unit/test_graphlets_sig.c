/*
 * test_graphlets_sig.c — signatures, similarity and the Bayesian posterior of the graphlets pass
 * (spec sections 13 and 14): identical subgraphs agree fully, disjoint support gives 0, the
 * posterior rises with the similarity under the test calibration, and an uncalibrated mode gives
 * no posterior.
 */
#include "graph/glet.h"
#include "passes/graphlets/gl_int.h"
#include "unity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_GL_ROOT
#define ODIN3_GL_ROOT "."
#endif

void setUp(void) {
}
void tearDown(void) {
}

static void make_sig(glet_sig *sig, uint32_t n, const glet_edge *ed, size_t ne) {
    glet_graph gr;
    int64_t *counts = calloc((size_t)n * 73, sizeof *counts);
    TEST_ASSERT_NOT_NULL(counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&gr, n, ed, ne));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&gr, &(glet_spec){false, 5, 0}, counts, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_sig_make(sig, &(glet_rows){counts, n, 73}, NULL));
    glet_graph_free(&gr);
    free(counts);
}

static void test_identical_subgraphs(void) {
    glet_edge ring[6] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}};
    glet_sig xa;
    glet_sig xb;
    make_sig(&xa, 6, ring, 6);
    make_sig(&xb, 6, ring, 6);
    TEST_ASSERT_TRUE(fabs(glet_gdd_agreement(&xa, &xb, NULL) - 1.0) < 1e-12);
    TEST_ASSERT_TRUE(fabs(glet_cosine_log(&xa, &xb) - 1.0) < 1e-12);
    glet_sig_free(&xa);
    glet_sig_free(&xb);
}

static void test_different_graphs_agree_less(void) {
    glet_edge ring[6] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}};
    glet_edge star[5] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}};
    glet_sig xa;
    glet_sig xb;
    make_sig(&xa, 6, ring, 6);
    make_sig(&xb, 6, star, 5);
    double agree = glet_gdd_agreement(&xa, &xb, NULL);
    TEST_ASSERT_TRUE(agree < 1.0 && agree >= 0.0);
    TEST_ASSERT_TRUE(glet_cosine_log(&xa, &xb) < 1.0);
    glet_sig_free(&xa);
    glet_sig_free(&xb);
}

static gl_cal *load_test_cal(void) {
    gl_cal *cal = gl_cal_load(ODIN3_GL_ROOT "/tests/cli/graphlet_calibration_test.json");
    TEST_ASSERT_NOT_NULL(cal);
    return cal;
}

static double gdd_posterior(const gl_cal *cal, double value) {
    double post = 0;
    TEST_ASSERT_TRUE(gl_cal_posterior(cal, "undirected-5", "gdd_agreement", value, &post));
    return post;
}

static void test_posterior_moves_with_similarity(void) {
    gl_cal *cal = load_test_cal();
    double low = gdd_posterior(cal, 0.2);
    double mid = gdd_posterior(cal, 0.85);
    double high = gdd_posterior(cal, 1.0);
    TEST_ASSERT_TRUE(low < mid && mid < high);
    TEST_ASSERT_TRUE(low > 0.0 && high < 1.0);
    gl_cal_free(cal);
}

static void test_combined_and_uncalibrated(void) {
    gl_cal *cal = load_test_cal();
    double both[2] = {0.95, 0.95};
    double comb = 0;
    TEST_ASSERT_TRUE(gl_cal_posterior_both(cal, "undirected-5", both, &comb));
    TEST_ASSERT_TRUE(comb > 0.5);
    double none = 0;
    TEST_ASSERT_FALSE(gl_cal_posterior(cal, "directed-4", "gdd_agreement", 0.9, &none));
    gl_cal_free(cal);
}

static void test_no_calibration_is_uncalibrated(void) {
    double none = 0;
    TEST_ASSERT_FALSE(gl_cal_posterior(NULL, "undirected-5", "gdd_agreement", 0.9, &none));
}

static void test_bad_calibration_file(void) {
    TEST_ASSERT_NULL(gl_cal_load(ODIN3_GL_ROOT "/tests/cli/graphlets_cli.sh"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identical_subgraphs);
    RUN_TEST(test_different_graphs_agree_less);
    RUN_TEST(test_posterior_moves_with_similarity);
    RUN_TEST(test_combined_and_uncalibrated);
    RUN_TEST(test_no_calibration_is_uncalibrated);
    RUN_TEST(test_bad_calibration_file);
    return UNITY_END();
}
