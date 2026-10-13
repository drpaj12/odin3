/*
 * test_bench_opt_columns.c — bench_opt prints every column (2F plan Task 15): the measuring core
 * of tests/bench/bench_opt.c on a tiny RTLIL-built design and on a tiny Verilog file. The heavy
 * smoke run over the VTR files is Task 19's.
 */
#include "bench_opt.h"
#include "ir/check.h"
#include "odin3/odin3.h"
#include "proc_build.h"
#include "unity.h"
#include "util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PATH_LEN = 512 };

static pb_result res;

void setUp(void) {
    memset(&res, 0, sizeof res);
}

void tearDown(void) {
    pb_free(&res);
    odin3_log_set_sink(NULL, NULL);
}

static char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    char *text = calloc(1, 4096);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, 4095, file);
    TEST_ASSERT_TRUE(got > 0);
    (void)fclose(file);
    return text;
}

/* Every key of the line, in order. Each is followed by '=' and a value. */
static const char *const k_keys[] = {"bench_opt file=",
                                     " cells0=",
                                     " nets0=",
                                     " pins0=",
                                     " cellsp=",
                                     " netsp=",
                                     " pinsp=",
                                     " cells1=",
                                     " nets1=",
                                     " pins1=",
                                     " iters=",
                                     " t_total=",
                                     " t_expr=",
                                     " t_merge=",
                                     " t_muxtree=",
                                     " t_reduce=",
                                     " t_share=",
                                     " t_dff=",
                                     " t_clean=",
                                     " t_setup=",
                                     " setup_rounds=",
                                     " rw_rewritten=",
                                     " rw_folded=",
                                     " rw_merge=",
                                     " rw_muxtree=",
                                     " rw_reduce=",
                                     " rw_share=",
                                     " rw_dff=",
                                     " rw_dff_bits=",
                                     " rw_clean=",
                                     " rules=",
                                     " t_compact=",
                                     " dead_nodes=",
                                     " dead_nets=",
                                     " dead_wires=",
                                     " full_iters=",
                                     " full_t_total=",
                                     " full_t_expr=",
                                     " full_t_merge=",
                                     " full_t_muxtree=",
                                     " full_t_reduce=",
                                     " full_t_share=",
                                     " full_t_dff=",
                                     " full_t_clean=",
                                     " full_t_setup=",
                                     " full_setup_rounds=",
                                     " full_rw_rewritten=",
                                     " full_rw_folded=",
                                     " full_rw_merge=",
                                     " full_rw_muxtree=",
                                     " full_rw_reduce=",
                                     " full_rw_share=",
                                     " full_rw_dff=",
                                     " full_rw_dff_bits=",
                                     " full_rw_clean=",
                                     " full_rules=",
                                     " rss_proc_kib=",
                                     " rss_proc_cur_kib=",
                                     " rss_opt_kib=",
                                     " rss_opt_growth_pct=",
                                     " rss_compact_kib=",
                                     " rss_full_kib=",
                                     " rss_peak_kib=",
                                     " rss_src="};

/* The value text of `key` (up to the next blank) into out. */
static void value_of(const char *line, const char *key, char *out, size_t cap) {
    const char *at = strstr(line, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, key);
    at += strlen(key);
    size_t len = strcspn(at, " ");
    TEST_ASSERT_TRUE(len > 0 && len < cap);
    memcpy(out, at, len);
    out[len] = '\0';
}

/* key occurs in text after `from`, and once only; returns its place. */
static const char *find_once(const char *from, const char *key) {
    const char *at = strstr(from, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, key);
    TEST_ASSERT_NULL_MESSAGE(strstr(at + 1, key), key);
    return at;
}

static void assert_columns(const char *line) {
    const char *from = line;
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
        from = find_once(from, k_keys[i]);
    }
    TEST_ASSERT_NULL(strchr(line, '\n'));
}

static unsigned long number(const char *line, const char *key) {
    char text[64];
    value_of(line, key, text, sizeof text);
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    TEST_ASSERT_TRUE(end != text);
    return value;
}

/* Builds tiny.il (an AND with a constant 0, and a dead NOT) into res. */
static void build_tiny(void) {
    char path[PATH_LEN];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/tiny.il", ODIN3_BENCH_DATA) > 0);
    char *text = slurp(path);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res));
    free(text);
}

/* The values of the line of tiny.il. Before: the two cells and the constant nodes; the AND folds to
 * a constant and the dead NOT goes; compact then drops their slots, and the peak (before compact)
 * still holds them. */
static void assert_at_least(const char *line, const char *key, unsigned long least) {
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)least, (uint32_t)number(line, key));
}

static void assert_tiny_values(const char *line) {
    assert_at_least(line, " cells0=", 2);
    assert_at_least(line, " dead_nodes=", 1);
    assert_at_least(line, " iters=", 1);
    assert_at_least(line, " setup_rounds=", 1);
    assert_at_least(line, " rss_peak_kib=", number(line, " rss_proc_kib="));
    assert_at_least(line, " rss_peak_kib=", number(line, " rss_opt_kib="));
    assert_at_least(line, " rss_proc_cur_kib=", 1);
    TEST_ASSERT_LESS_THAN_UINT32((uint32_t)number(line, " cells0="),
                                 (uint32_t)number(line, " cells1="));
}

static void test_bench_opt_columns(void) {
    build_tiny();
    char line[BENCH_OPT_LINE];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bench_opt_design(res.design, "tiny.il", line, sizeof line));
    assert_columns(line);
    TEST_ASSERT_NOT_NULL(strstr(line, "bench_opt file=tiny.il "));
    assert_tiny_values(line);
    TEST_ASSERT_NOT_NULL(strstr(line, " rules=const:"));
    TEST_ASSERT_NULL(strstr(line, "nan"));
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
}

static void test_bench_opt_small_buffer(void) {
    build_tiny();
    char line[64];
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          bench_opt_design(res.design, "tiny.il", line, sizeof line));
}

static void test_bench_opt_one_verilog(void) {
    char path[PATH_LEN];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/tiny.v", ODIN3_BENCH_DATA) > 0);
    char line[BENCH_OPT_LINE];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, bench_opt_one(path, line, sizeof line));
    assert_columns(line);
    TEST_ASSERT_NOT_NULL(strstr(line, "bench_opt file=tiny.v "));
    TEST_ASSERT_TRUE(number(line, " cells0=") >= 1);
}

static void test_bench_opt_missing_file(void) {
    char line[BENCH_OPT_LINE];
    TEST_ASSERT_TRUE(bench_opt_one("/nonexistent/none.v", line, sizeof line) != ODIN3_OK);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bench_opt_columns);
    RUN_TEST(test_bench_opt_small_buffer);
    RUN_TEST(test_bench_opt_one_verilog);
    RUN_TEST(test_bench_opt_missing_file);
    return UNITY_END();
}
