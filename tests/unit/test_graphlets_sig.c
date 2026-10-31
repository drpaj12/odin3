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

static void make_sig(glet_sig *sig, uint32_t n, const glet_edge *e, size_t ne) {
    glet_graph g;
    int64_t *counts = calloc((size_t)n * 73, sizeof *counts);
    TEST_ASSERT_NOT_NULL(counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&g, n, e, ne));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&g, &(glet_spec){false, 5, 0}, counts, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_sig_make(sig, counts, n, 73, NULL));
    glet_graph_free(&g);
    free(counts);
}

static void test_identical_subgraphs(void) {
    glet_edge ring[6] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}};
    glet_sig a;
    glet_sig b;
    make_sig(&a, 6, ring, 6);
    make_sig(&b, 6, ring, 6);
    TEST_ASSERT_TRUE(fabs(glet_gdd_agreement(&a, &b, NULL) - 1.0) < 1e-12);
    TEST_ASSERT_TRUE(fabs(glet_cosine_log(&a, &b) - 1.0) < 1e-12);
    glet_sig_free(&a);
    glet_sig_free(&b);
}

static void test_different_graphs_agree_less(void) {
    glet_edge ring[6] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}};
    glet_edge star[5] = {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}};
    glet_sig a;
    glet_sig b;
    make_sig(&a, 6, ring, 6);
    make_sig(&b, 6, star, 5);
    double agree = glet_gdd_agreement(&a, &b, NULL);
    TEST_ASSERT_TRUE(agree < 1.0 && agree >= 0.0);
    TEST_ASSERT_TRUE(glet_cosine_log(&a, &b) < 1.0);
    glet_sig_free(&a);
    glet_sig_free(&b);
}

static void test_posterior_moves_with_similarity(void) {
    gl_cal *cal = gl_cal_load(ODIN3_GL_ROOT "/tests/cli/graphlet_calibration_test.json");
    TEST_ASSERT_NOT_NULL(cal);
    double lo = 0;
    double mid = 0;
    double hi = 0;
    TEST_ASSERT_TRUE(gl_cal_posterior(cal, "undirected-5", "gdd_agreement", 0.2, &lo));
    TEST_ASSERT_TRUE(gl_cal_posterior(cal, "undirected-5", "gdd_agreement", 0.85, &mid));
    TEST_ASSERT_TRUE(gl_cal_posterior(cal, "undirected-5", "gdd_agreement", 1.0, &hi));
    TEST_ASSERT_TRUE(lo < mid && mid < hi);
    TEST_ASSERT_TRUE(lo > 0.0 && hi < 1.0);
    double both[2] = {0.95, 0.95};
    double comb = 0;
    TEST_ASSERT_TRUE(gl_cal_posterior_both(cal, "undirected-5", both, &comb));
    TEST_ASSERT_TRUE(comb > 0.5);
    double none = 0;
    TEST_ASSERT_FALSE(gl_cal_posterior(cal, "directed-4", "gdd_agreement", 0.9, &none));
    TEST_ASSERT_FALSE(gl_cal_posterior(NULL, "undirected-5", "gdd_agreement", 0.9, &none));
    gl_cal_free(cal);
}

static void test_bad_calibration_file(void) {
    TEST_ASSERT_NULL(gl_cal_load(ODIN3_GL_ROOT "/tests/cli/graphlets_cli.sh"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identical_subgraphs);
    RUN_TEST(test_different_graphs_agree_less);
    RUN_TEST(test_posterior_moves_with_similarity);
    RUN_TEST(test_bad_calibration_file);
    return UNITY_END();
}
