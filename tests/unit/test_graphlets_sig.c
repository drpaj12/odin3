/*
 * test_graphlets_sig.c — signatures, similarity and the Bayesian posterior of the graphlets pass
 * (spec sections 13 and 14): identical subgraphs agree fully, disjoint support gives 0, the
 * posterior rises with the similarity under the test calibration, and an uncalibrated mode gives
 * no posterior.
 */
#include "graph/glet.h"
#include "ir/design.h"
#include "ir/module.h"
#include "passes/graphlets/gl_int.h"
#include "unity.h"
#include "util/alloc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* --- hostile --compare and calibration inputs (PR #304 review) ------------------------------- */

/* Writes text to a fresh temp file; the path is returned in path (cap 64). */
static void temp_file(char *path, const char *text) {
    (void)snprintf(path, 64, "/tmp/odin3_glet_XXXXXX");
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(text), (int)write(fd, text, strlen(text)));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

/* A graphlets file with two orbits; mode, group name, totals and the orbit-0 pairs are inserted. */
static odin3_status load_sig(const char *mode, const char *name, const char *totals,
                             const char *pairs) {
    char text[1024];
    char path[64];
    (void)snprintf(text, sizeof text,
                   "{\"schema\": \"odin3-graphlets\", \"mode\": %s, \"n_orbits\": 2, \"groups\": "
                   "[{%s \"reported\": 3, \"orbit_totals\": %s, \"gdd\": [%s, []]}]}",
                   mode, name, totals, pairs);
    temp_file(path, text);
    glet_sig sig;
    odin3_strbuf md;
    odin3_strbuf gp;
    odin3_strbuf_init(&md);
    odin3_strbuf_init(&gp);
    odin3_status st = gl_sig_load(path, &sig, &md, &gp);
    if (st == ODIN3_OK) {
        glet_sig_free(&sig);
    }
    odin3_strbuf_free(&md);
    odin3_strbuf_free(&gp);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    return st;
}

#define GOOD_NAME "\"name\": \"g\","

static void test_sig_file_accepts_good(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          load_sig("\"undirected-5\"", GOOD_NAME, "[3, 0]", "[[1, 2], [4, 1]]"));
}

static void test_sig_file_rejects_bad_mode_and_name(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, load_sig("\"\"", GOOD_NAME, "[3, 0]", "[[1, 2]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, load_sig("7", GOOD_NAME, "[3, 0]", "[[1, 2]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, load_sig("\"m\"", "", "[3, 0]", "[[1, 2]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", "\"name\": \"\",", "[3, 0]", "[[1, 2]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", "\"name\": 5,", "[3, 0]", "[[1, 2]]"));
}

static void test_sig_file_rejects_bad_gdd(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[0, 2]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[-2, 2]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[1, 0]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[1, -1]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[4, 1], [2, 1]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[2, 1], [2, 1]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, load_sig("\"m\"", GOOD_NAME, "[3, 0]", "[[1]]"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          load_sig("\"m\"", GOOD_NAME, "[-3, 0]", "[[1, 2]]"));
}

static gl_cal *cal_from(const char *modes) {
    char text[2048];
    char path[64];
    (void)snprintf(text, sizeof text,
                   "{\"schema\": \"odin3-graphlet-calibration\", \"version\": 1, \"modes\": {%s}}",
                   modes);
    temp_file(path, text);
    gl_cal *cal = gl_cal_load(path);
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    return cal;
}

static void test_calibration_validated_on_load(void) {
    static const char *const k_bad[] = {
        "\"m\": {\"prior\": {\"same\": -1, \"different\": 5}}",
        "\"m\": {\"prior\": {\"same\": \"x\", \"different\": 5}}",
        "\"m\": {\"metrics\": {\"a\": {\"edges\": [0, 500, 400], \"same\": [1, 2], \"different\": "
        "[1, 2]}}}",
        "\"m\": {\"metrics\": {\"a\": {\"edges\": [0, 500, 900], \"same\": [1, -2], \"different\": "
        "[1, 2]}}}",
        "\"m\": {\"metrics\": {\"a\": {\"edges\": [0, 500, 900], \"same\": [1], \"different\": [1, "
        "2]}}}",
        "\"m\": {\"metrics\": {\"a\": {\"edges\": [0], \"same\": [], \"different\": []}}}",
        "\"m\": {\"metrics\": {\"a\": {\"edges\": [0, \"x\"], \"same\": [1], \"different\": "
        "[1]}}}"};
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        TEST_ASSERT_NULL_MESSAGE(cal_from(k_bad[i]), k_bad[i]);
    }
}

static void test_calibration_good_and_lookup(void) {
    gl_cal *good = cal_from(
        "\"m\": {\"prior\": {\"same\": 1, \"different\": 3}, \"metrics\": {\"a\": {\"edges\": [0, "
        "500, 1000], \"same\": [1, 2], \"different\": [3, 4]}}}");
    TEST_ASSERT_NOT_NULL(good);
    double post = 0;
    TEST_ASSERT_TRUE(gl_cal_posterior(good, "m", "a", 0.9, &post));
    TEST_ASSERT_TRUE(post > 0.0 && post < 1.0);
    gl_cal_free(good);
}

static void test_calibration_nan_value_is_uncalibrated(void) {
    gl_cal *good =
        cal_from("\"m\": {\"metrics\": {\"a\": {\"edges\": [0, 500, 1000], \"same\": [1, 2], "
                 "\"different\": [3, 4]}}}");
    TEST_ASSERT_NOT_NULL(good);
    double post = 0;
    TEST_ASSERT_FALSE(gl_cal_posterior(good, "m", "a", NAN, &post));
    gl_cal_free(good);
}

/* Appends ", 0" until the array holds count zeros (the first is already written). */
static void zeros(odin3_strbuf *text, int count) {
    for (int i = 1; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(text, odin3_bytes_cstr(", 0")));
    }
}

static void test_calibration_bin_cap(void) {
    enum { BINS = 4100 };
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_append(
                      &text, odin3_bytes_cstr(
                                 "{\"schema\": "
                                 "\"odin3-graphlet-calibration\", \"modes\": {\"m\": {\"metrics\": "
                                 "{\"a\": {\"edges\": [0")));
    for (int i = 1; i <= BINS; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&text, ", %d", i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(&text, odin3_bytes_cstr("], \"same\": [0")));
    zeros(&text, BINS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_append(&text, odin3_bytes_cstr("], \"different\": [0")));
    zeros(&text, BINS);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&text, odin3_bytes_cstr("]}}}}}")));
    char path[64];
    temp_file(path, text.data);
    TEST_ASSERT_NULL(gl_cal_load(path));
    TEST_ASSERT_EQUAL_INT(0, unlink(path));
    odin3_strbuf_free(&text);
}

static void test_compare_names_are_escaped(void) {
    glet_edge ring[3] = {{0, 1}, {1, 2}, {2, 0}};
    glet_sig xa;
    make_sig(&xa, 3, ring, 3);
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    gl_cmp_in in = {"a\"b\\c", "d\"e", &xa, &xa, NULL, "undirected-5"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, gl_compare_json(&out, &in, false));
    TEST_ASSERT_NOT_NULL(strstr(out.data, "{\"a\": \"a\\\"b\\\\c\", \"b\": \"d\\\"e\""));
    odin3_strbuf_free(&out);
    glet_sig_free(&xa);
}

static void test_sig_make_alloc_failure_is_clean(void) {
    glet_edge ring[6] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 0}};
    glet_graph gr;
    int64_t *counts = calloc((size_t)6 * 73, sizeof *counts);
    TEST_ASSERT_NOT_NULL(counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_graph_build(&gr, 6, ring, 6));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, glet_count(&gr, &(glet_spec){false, 5, 0}, counts, NULL));
    for (long k = 4; k < 8; k++) {
        glet_sig sig;
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = glet_sig_make(&sig, &(glet_rows){counts, 6, 73}, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            glet_sig_free(&sig);
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
    }
    glet_graph_free(&gr);
    free(counts);
}

/* --partition: 4096 groups are accepted, one more is a clear error (and no leak under ASan). */
static void partition_run(unsigned nodes, odin3_status want) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("m"), &name));
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_create(design, name, (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_celltype_id type = {0};
    uint32_t tname = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr("$and"), &tname));
    TEST_ASSERT_TRUE(odin3_celltype_find(design, tname, &type));
    for (unsigned i = 0; i < nodes; i++) {
        odin3_node_spec spec = {type, 0, {0}, NULL, 0};
        odin3_node_id node = {0};
        char val[16];
        (void)snprintf(val, sizeof val, "g%u", i);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create(mod, &spec, &node));
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_attr_set_string(design, (odin3_obj){mid.v, ODIN3_OBJ_NODE, node.v}, "k", val));
    }
    gl_opts opts;
    memset(&opts, 0, sizeof opts);
    odin3_strbuf_init(&opts.partition);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&opts.partition, odin3_bytes_cstr("k")));
    gl_groups groups;
    TEST_ASSERT_EQUAL_INT(want, gl_groups_make(design, &opts, &groups));
    if (want == ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT64(nodes, groups.count);
        gl_groups_free(&groups);
    }
    odin3_strbuf_free(&opts.partition);
    odin3_design_destroy(design);
}

static void test_partition_group_cap(void) {
    partition_run(4096, ODIN3_OK);
    partition_run(4097, ODIN3_ERR_INVALID_ARG);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_identical_subgraphs);
    RUN_TEST(test_different_graphs_agree_less);
    RUN_TEST(test_posterior_moves_with_similarity);
    RUN_TEST(test_combined_and_uncalibrated);
    RUN_TEST(test_no_calibration_is_uncalibrated);
    RUN_TEST(test_bad_calibration_file);
    RUN_TEST(test_sig_file_accepts_good);
    RUN_TEST(test_sig_file_rejects_bad_mode_and_name);
    RUN_TEST(test_sig_file_rejects_bad_gdd);
    RUN_TEST(test_calibration_validated_on_load);
    RUN_TEST(test_calibration_good_and_lookup);
    RUN_TEST(test_calibration_nan_value_is_uncalibrated);
    RUN_TEST(test_calibration_bin_cap);
    RUN_TEST(test_compare_names_are_escaped);
    RUN_TEST(test_sig_make_alloc_failure_is_clean);
    RUN_TEST(test_partition_group_cap);
    return UNITY_END();
}
