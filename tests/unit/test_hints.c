/*
 * test_hints.c — unit tests for the timing hint table: the file format, its errors, the worst-wins
 * merge, the lookups and the match counts, and the read_hints pass (docs/specs/
 * 2026-11-14-timing-hints.md).
 */
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/hints.h"
#include "unity.h"
#include "util/log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { LOG_CAP = 1 << 14 };

static odin3_design *design;
static char log_text[LOG_CAP];
static size_t log_len;

static const char *const PATH = "odin3_hints_test.tsv";
static const char *const HEAD = "odin3-timing-hints\t1\n"
                                "meta\tcpd_ps\t2000\n"
                                "meta\tdepth\t4\n"
                                "meta\tslack_min_ps\t0\n";

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    int wrote = snprintf(log_text + log_len, LOG_CAP - log_len, "%d %s\n", (int)level, msg);
    if (wrote > 0 && log_len + (size_t)wrote < LOG_CAP) {
        log_len += (size_t)wrote;
    }
}

void setUp(void) {
    log_text[0] = '\0';
    log_len = 0;
    odin3_log_set_sink(capture_sink, NULL);
    odin3_hints_reset();
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_hints_reset();
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
    (void)remove(PATH);
}

static odin3_hints *parse_ok(const char *text) {
    odin3_hints *hints = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hints_parse(text, strlen(text), "t.tsv", &hints));
    TEST_ASSERT_NOT_NULL(hints);
    return hints;
}

static void expect_error(const char *text, const char *needle) {
    odin3_hints *hints = (odin3_hints *)1;
    log_len = 0;
    log_text[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_hints_parse(text, strlen(text), "t.tsv", &hints));
    TEST_ASSERT_NULL(hints);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), log_text);
}

static void test_parse_reads_meta_cells_and_nets(void) {
    char text[512];
    (void)snprintf(text, sizeof text,
                   "%s# a comment\n\ncell\t7\t120.5\t0.9\t2\t3\n"
                   "cell\t3\t40\t0.1\t4\t1\nnet\ttop/a\t3000\t-\n"
                   "meta\tarch\tk6\n",
                   HEAD);
    odin3_hints *hints = parse_ok(text);
    TEST_ASSERT_TRUE(fabs((2000.0) - (hints->cpd_ps)) < 1e-9);
    TEST_ASSERT_EQUAL_UINT32(4, hints->depth);
    TEST_ASSERT_TRUE(fabs((0.5) - (hints->ns_per_level)) < 1e-9); /* 2000 ps / 1000 / depth 4 */
    TEST_ASSERT_EQUAL_size_t(2, hints->n_cells);
    TEST_ASSERT_EQUAL_UINT32(3, hints->cells[0].uid); /* sorted by uid */
    TEST_ASSERT_EQUAL_UINT32(7, hints->cells[1].uid);
    const odin3_hint_cell *hit = odin3_hints_find_cell(hints, 7);
    TEST_ASSERT_NOT_NULL(hit);
    TEST_ASSERT_TRUE(fabs((120.5) - (hit->slack_ps)) < 1e-9);
    TEST_ASSERT_EQUAL_UINT32(2, hit->depth);
    TEST_ASSERT_NULL(odin3_hints_find_cell(hints, 8));
    const odin3_hint_net *net = odin3_hints_find_net(hints, "top/a");
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(fabs((3000.0) - (net->arrival_ps)) < 1e-9);
    TEST_ASSERT_TRUE(isnan(net->required_ps));
    TEST_ASSERT_NOT_NULL(strstr(hints->source, "arch=k6"));
    TEST_ASSERT_EQUAL_size_t(1, odin3_hints_cells_used(hints));
    TEST_ASSERT_EQUAL_size_t(1, odin3_hints_nets_used(hints));
    odin3_hints_free(hints);
}

static void test_explicit_ns_per_level_wins(void) {
    char text[256];
    (void)snprintf(text, sizeof text, "%smeta\tns_per_level\t0.25\ncell\t1\t0\t1\t1\t1\n", HEAD);
    odin3_hints *hints = parse_ok(text);
    TEST_ASSERT_TRUE(fabs((0.25) - (hints->ns_per_level)) < 1e-9);
    odin3_hints_free(hints);
}

static void test_tighten_levels_meta(void) {
    odin3_hints *hints = parse_ok("odin3-timing-hints\t1\n");
    TEST_ASSERT_TRUE(fabs(1.0 - hints->tighten_levels) < 1e-9); /* the default */
    odin3_hints_free(hints);
    char text[256];
    (void)snprintf(text, sizeof text, "%smeta\ttighten_levels\t2.5\n", HEAD);
    hints = parse_ok(text);
    TEST_ASSERT_TRUE(fabs(2.5 - hints->tighten_levels) < 1e-9);
    odin3_hints_free(hints);
    (void)snprintf(text, sizeof text, "%smeta\ttighten_levels\t-1\n", HEAD);
    expect_error(text, "tighten_levels must be a number from 0");
}

static void test_duplicates_keep_the_worst(void) {
    char text[512];
    (void)snprintf(text, sizeof text,
                   "%scell\t5\t100\t0.2\t3\t1\ncell\t5\t20\t0.8\t2\t2\n"
                   "net\tm/x\t100\t900\nnet\tm/x\t400\t700\n",
                   HEAD);
    odin3_hints *hints = parse_ok(text);
    TEST_ASSERT_EQUAL_size_t(1, hints->n_cells);
    TEST_ASSERT_TRUE(fabs((20.0) - (hints->cells[0].slack_ps)) < 1e-9);
    TEST_ASSERT_TRUE(fabs((0.8) - (hints->cells[0].crit)) < 1e-9);
    TEST_ASSERT_EQUAL_UINT32(2, hints->cells[0].depth);
    TEST_ASSERT_EQUAL_UINT32(3, hints->cells[0].luts);
    TEST_ASSERT_EQUAL_size_t(1, hints->n_nets);
    TEST_ASSERT_TRUE(fabs((400.0) - (hints->nets[0].arrival_ps)) < 1e-9);
    TEST_ASSERT_TRUE(fabs((700.0) - (hints->nets[0].required_ps)) < 1e-9);
    odin3_hints_free(hints);
}

static void test_errors_name_file_and_line(void) {
    expect_error("", "t.tsv:1: empty file");
    expect_error("hello\n", "t.tsv:1: not a timing hint file");
    expect_error("odin3-timing-hints\t2\n", "t.tsv:1: not a timing hint file");
    expect_error("odin3-timing-hints\t1\nbogus\t1\n", "t.tsv:2: unknown record kind: bogus");
    expect_error("odin3-timing-hints\t1\nmeta\tcpd_ps\n", "t.tsv:2: a meta line has 3 fields");
    expect_error("odin3-timing-hints\t1\nmeta\tcpd_ps\tabc\n", "t.tsv:2: not a number: abc");
    expect_error("odin3-timing-hints\t1\nmeta\tdepth\t-3\n", "t.tsv:2: not an unsigned number");
    char text[256];
    (void)snprintf(text, sizeof text, "%scell\t1\t2\t3\n", HEAD);
    expect_error(text, "t.tsv:5: a cell line has 6 fields");
    (void)snprintf(text, sizeof text, "%scell\t0\t1\t1\t1\t1\n", HEAD);
    expect_error(text, "t.tsv:5: not a record ID: 0");
    (void)snprintf(text, sizeof text, "%scell\t1\tx\t1\t1\t1\n", HEAD);
    expect_error(text, "t.tsv:5: not a number: x");
    (void)snprintf(text, sizeof text, "%snet\tnoslash\t1\t1\n", HEAD);
    expect_error(text, "t.tsv:5: a net name is <module>/<net>");
    (void)snprintf(text, sizeof text, "%snet\tm/a\t1\t1\textra\n", HEAD);
    expect_error(text, "t.tsv:5: a net line has 4 fields");
    /* records without the meta lines that give the units */
    expect_error("odin3-timing-hints\t1\ncell\t1\t0\t1\t1\t1\n", "need meta lines");
    expect_error("odin3-timing-hints\t1\nmeta\tcpd_ps\t1000\nmeta\tdepth\t0\n"
                 "meta\tslack_min_ps\t0\ncell\t1\t0\t1\t1\t1\n",
                 "ns_per_level");
    /* a cell with neither depth nor LUTs */
    (void)snprintf(text, sizeof text, "%scell\t1\t1\t1\t0\t0\n", HEAD);
    expect_error(text, "t.tsv:5: a cell hint with depth 0 and luts 0 together");
    /* a slack below the file's own slack_min_ps */
    (void)snprintf(text, sizeof text, "%scell\t1\t-5\t1\t1\t1\n", HEAD);
    expect_error(text, "cell 1 has slack -5.0 ps, below the file's slack_min_ps 0.0");
    /* ns_per_level below the floor (it would print as inf in the ABC timing file) */
    (void)snprintf(text, sizeof text, "%smeta\tns_per_level\t1e-300\ncell\t1\t1\t1\t1\t1\n",
                   HEAD);
    expect_error(text, "ns_per_level 1e-300 is below the floor");
}

static void test_a_nul_byte_is_an_error(void) {
    static const char text[] = "odin3-timing-hints\t1\nmeta\tcpd_ps\t1\0\nmeta\tdepth\t1\n";
    odin3_hints *hints = (odin3_hints *)1;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_hints_parse(text, sizeof text - 1, "t.tsv", &hints));
    TEST_ASSERT_NULL(hints);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "t.tsv:2: a NUL byte in the file"));
}

static void test_header_only_file_is_fine(void) {
    odin3_hints *hints = parse_ok("odin3-timing-hints\t1\n");
    TEST_ASSERT_EQUAL_size_t(0, hints->n_cells);
    TEST_ASSERT_EQUAL_size_t(0, hints->n_nets);
    odin3_hints_free(hints);
}

static void write_file(const char *text) {
    FILE *file = fopen(PATH, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(1, (int)fwrite(text, strlen(text), 1, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void test_read_hints_pass_loads_and_caches_per_design(void) {
    char text[256];
    (void)snprintf(text, sizeof text, "%scell\t9\t10\t0.5\t2\t1\n", HEAD);
    write_file(text);
    TEST_ASSERT_NULL(odin3_hints_of(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_run_pass(design, "read_hints", PATH));
    odin3_hints *hints = odin3_hints_of(design);
    TEST_ASSERT_NOT_NULL(hints);
    TEST_ASSERT_EQUAL_size_t(1, hints->n_cells);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "read_hints: odin3_hints_test.tsv: 1 cell hints"));
    odin3_design *other = odin3_design_create();
    TEST_ASSERT_NULL(odin3_hints_of(other)); /* another design has none */
    TEST_ASSERT_NOT_NULL(strstr(log_text, "loaded for an older design"));
    odin3_design_destroy(other);
}

static void test_read_hints_pass_errors(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_run_pass(design, "read_hints", ""));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "usage: read_hints <file>"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_run_pass(design, "read_hints", "a.tsv b.tsv"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO,
                          odin3_design_run_pass(design, "read_hints", "no_such_hint_file.tsv"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "cannot read the file"));
    write_file("not a hint file\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_design_run_pass(design, "read_hints", PATH));
    TEST_ASSERT_NULL(odin3_hints_of(design));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_reads_meta_cells_and_nets);
    RUN_TEST(test_explicit_ns_per_level_wins);
    RUN_TEST(test_tighten_levels_meta);
    RUN_TEST(test_duplicates_keep_the_worst);
    RUN_TEST(test_errors_name_file_and_line);
    RUN_TEST(test_a_nul_byte_is_an_error);
    RUN_TEST(test_header_only_file_is_fine);
    RUN_TEST(test_read_hints_pass_loads_and_caches_per_design);
    RUN_TEST(test_read_hints_pass_errors);
    return UNITY_END();
}
