/*
 * test_json_parse.c — unit tests for the iterative JSON parser and its limits (spec 5.3 T-1).
 */
#include "frontends/json/json_parse.h"
#include "unity.h"
#include "util/alloc.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEEP_LEVELS = 200000,
    DEEP_BYTES = 2 * DEEP_LEVELS,
    SMALL_STACK = 256 * 1024,
    SAMPLE_BYTES = 2048,
    SWEEP_MAX = 4000,
    LIMIT_STRING = 8,
    LIMIT_BYTES = 16,
    DEPTH_OK = 32,
    DEPTH_BAD = 33,
    CASE_LINE = 3,
    CASE_COL = 5
};

static odin3_json_doc g_doc;

void setUp(void) {
    memset(&g_doc, 0, sizeof g_doc);
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_json_test_set_limits(NULL);
    odin3_json_doc_free(&g_doc);
}

static odin3_status parse_n(const char *text, size_t len) {
    odin3_json_doc_free(&g_doc);
    return odin3_json_parse((const uint8_t *)text, len, NULL, &g_doc);
}

static odin3_status parse_s(const char *text) {
    return parse_n(text, strlen(text));
}

static void expect_ok(const char *text) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, parse_s(text), text);
    TEST_ASSERT_NOT_NULL(g_doc.root);
}

/* The parse fails with ODIN3_ERR_PARSE at line:col and a message containing want. */
static void expect_err(const char *text, uint32_t line, uint32_t col, const char *want) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, parse_s(text), text);
    TEST_ASSERT_NULL(g_doc.root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(line, g_doc.error.line, text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(col, g_doc.error.col, text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_doc.error.message, want), g_doc.error.message);
}

static void expect_str(const char *json, const void *want, size_t want_len) {
    expect_ok(json);
    size_t len = 0;
    const char *got = odin3_json_string(g_doc.root, &len);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_size_t(want_len, len);
    TEST_ASSERT_EQUAL_MEMORY(want, got, want_len + 1); /* includes the NUL */
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)got[len]);
}

static void test_every_kind(void) {
    expect_ok("{\"a\": null, \"b\": true, \"c\": false, \"d\": 7, \"e\": \"s\", \"f\": [1, 2],"
              " \"g\": {}}");
    const odin3_json_node *root = g_doc.root;
    TEST_ASSERT_EQUAL_INT(ODIN3_JSON_OBJECT, odin3_json_kind_of(root));
    TEST_ASSERT_EQUAL_size_t(7, odin3_json_count(root));
    TEST_ASSERT_EQUAL_INT(ODIN3_JSON_NULL, odin3_json_kind_of(odin3_json_member(root, "a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_JSON_TRUE, odin3_json_kind_of(odin3_json_member(root, "b")));
    TEST_ASSERT_EQUAL_INT(ODIN3_JSON_FALSE, odin3_json_kind_of(odin3_json_member(root, "c")));
    TEST_ASSERT_EQUAL_INT64(7, odin3_json_int(odin3_json_member(root, "d")));
    TEST_ASSERT_EQUAL_STRING("s", odin3_json_string(odin3_json_member(root, "e"), NULL));
}

static void test_navigation(void) {
    expect_ok("{\"f\": [1, 2], \"g\": {}}");
    const odin3_json_node *root = g_doc.root;
    const odin3_json_node *arr = odin3_json_member(root, "f");
    TEST_ASSERT_EQUAL_INT(ODIN3_JSON_ARRAY, odin3_json_kind_of(arr));
    TEST_ASSERT_EQUAL_size_t(2, odin3_json_count(arr));
    TEST_ASSERT_EQUAL_INT64(2, odin3_json_int(odin3_json_next(odin3_json_first(arr))));
    TEST_ASSERT_NULL(odin3_json_next(odin3_json_next(odin3_json_first(arr))));
}

static void test_navigation_misses(void) {
    expect_ok("{\"f\": [1, 2], \"g\": {}}");
    const odin3_json_node *root = g_doc.root;
    TEST_ASSERT_EQUAL_size_t(0, odin3_json_count(odin3_json_member(root, "g")));
    TEST_ASSERT_NULL(odin3_json_first(odin3_json_member(root, "g")));
    TEST_ASSERT_NULL(odin3_json_member(root, "zz"));
}

static void test_accessors_on_wrong_kind(void) {
    expect_ok("[1, 2]");
    const odin3_json_node *arr = g_doc.root;
    TEST_ASSERT_NULL(odin3_json_member(arr, "a"));
    TEST_ASSERT_NULL(odin3_json_string(arr, NULL));
    TEST_ASSERT_EQUAL_INT64(0, odin3_json_int(arr));
}

static void test_member_order_and_keys(void) {
    expect_ok("{\"z\":1,\"a\":2,\"m\":3}");
    const char *want[] = {"z", "a", "m"};
    size_t idx = 0;
    for (const odin3_json_node *kid = odin3_json_first(g_doc.root); kid != NULL;
         kid = odin3_json_next(kid)) {
        TEST_ASSERT_EQUAL_STRING(want[idx++], odin3_json_key(kid));
    }
    TEST_ASSERT_EQUAL_size_t(3, idx);
    TEST_ASSERT_NULL(odin3_json_key(g_doc.root));
}

static void test_scalar_roots(void) {
    expect_ok("  42  ");
    TEST_ASSERT_EQUAL_INT64(42, odin3_json_int(g_doc.root));
    expect_ok("\"x\"");
    expect_ok("null");
    expect_ok("[]");
    expect_ok("{}");
}

static void test_string_escapes(void) {
    expect_str("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"", "a\"b\\c/d\b\f\n\r\t", 12);
    expect_str("\"\"", "", 0);
    expect_str("\"\xFF\xFE\"", "\xFF\xFE", 2);
}

static void test_unicode_escapes(void) {
    expect_str("\"\\u0041\"", "A", 1);
    expect_str("\"\\u00e9\"", "\xC3\xA9", 2);
    expect_str("\"\\u20AC\"", "\xE2\x82\xAC", 3);
    expect_str("\"\\uD83D\\uDE00\"", "\xF0\x9F\x98\x80", 4);
    expect_str("\"\\u007f\\u0001\"", "\x7F\x01", 2);
}

static void test_unicode_errors(void) {
    expect_err("\"\\uD83D\"", 1, 2, "lone high surrogate");
    expect_err("\"\\uD83Dx\"", 1, 2, "lone high surrogate");
    expect_err("\"\\uD83D\\u0041\"", 1, 2, "lone high surrogate");
    expect_err("\"\\uDE00\"", 1, 2, "lone low surrogate");
    expect_err("\"\\u0000\"", 1, 2, "\\u0000");
    expect_err("\"\\u12G4\"", 1, 6, "4 hex digits");
    expect_err("\"\\x\"", 1, 2, "bad escape");
}

static void test_raw_control_bytes(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_n("\"a\0b\"", 5));
    TEST_ASSERT_EQUAL_UINT32(1, g_doc.error.line);
    TEST_ASSERT_EQUAL_UINT32(3, g_doc.error.col);
    TEST_ASSERT_NOT_NULL(strstr(g_doc.error.message, "0x00"));
    expect_err("\"a\nb\"", 1, 3, "control byte");
    expect_err("\"a\tb\"", 1, 3, "control byte");
}

static void test_integers(void) {
    expect_ok("9223372036854775807");
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, odin3_json_int(g_doc.root));
    expect_ok("-9223372036854775808");
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, odin3_json_int(g_doc.root));
    expect_ok("-0");
    TEST_ASSERT_EQUAL_INT64(0, odin3_json_int(g_doc.root));
    expect_ok("[-1, 0, 10]");
    expect_err("9223372036854775808", 1, 1, "int64 range");
    expect_err("-9223372036854775809", 1, 1, "int64 range");
    expect_err("99999999999999999999999999", 1, 1, "int64 range");
    expect_err("1.5", 1, 1, "fraction or exponent");
    expect_err("1e5", 1, 1, "fraction or exponent");
    expect_err("1E+5", 1, 1, "fraction or exponent");
    expect_err("-", 1, 1, "digit");
    expect_err("01", 1, 1, "leading zero");
    expect_err("-x", 1, 1, "digit");
}

static void test_duplicate_key(void) {
    expect_err("{\n\"a\": 1,\n  \"b\": 2,\n  \"a\": 3}", 4, 3,
               "duplicate key \"a\" (first at line 2)");
    expect_err("{\"k\":{\"x\":1,\"x\":2}}", 1, 13, "duplicate key \"x\"");
    expect_ok("{\"k\":{\"x\":1},\"j\":{\"x\":2},\"x\":3}");
}

static void test_duplicate_key_in_wide_object(void) {
    enum { KEYS = 5000 };
    size_t cap = (size_t)KEYS * 16 + 64;
    char *buf = malloc(cap);
    TEST_ASSERT_NOT_NULL(buf);
    size_t len = 0;
    buf[len++] = '{';
    for (int i = 0; i < KEYS; i++) {
        len += (size_t)snprintf(buf + len, cap - len, "%s\"k%d\":%d", i ? "," : "", i, i);
    }
    buf[len] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_n(buf, len)); /* unterminated */
    (void)snprintf(buf + len, cap - len, ",\"k%d\":0}", KEYS / 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_s(buf));
    TEST_ASSERT_NOT_NULL(strstr(g_doc.error.message, "duplicate key \"k2500\""));
    (void)snprintf(buf + len, cap - len, ",\"kx\":0}");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_s(buf));
    TEST_ASSERT_EQUAL_size_t(KEYS + 1, odin3_json_count(g_doc.root));
    free(buf);
}

static void test_syntax_errors_located(void) {
    expect_err("[1,]", 1, 4, "trailing comma");
    expect_err("{\"a\":1,}", 1, 8, "trailing comma");
    expect_err("[,1]", 1, 2, "unexpected character ','");
    expect_err("{,}", 1, 2, "expected a string key");
    expect_err("{\"a\" 1}", 1, 6, "expected ':'");
    expect_err("{\"a\":}", 1, 6, "unexpected character '}'");
    expect_err("{1:2}", 1, 2, "expected a string key");
    expect_err("\"abc", 1, 1, "unterminated string");
    expect_err("[1, 2", 1, 6, "unexpected end of file");
    expect_err("[\n1,\n2", CASE_LINE, 2, "unexpected end of file");
    expect_err("{\"a\":", 1, 6, "unexpected end of file");
    expect_err("", 1, 1, "unexpected end of file");
    expect_err("   \n  ", 2, 3, "unexpected end of file");
    expect_err("[1 2]", 1, 4, "expected ',' or ']'");
    expect_err("{\"a\":1 \"b\":2}", 1, 8, "expected ',' or '}'");
    expect_err("[1] x", 1, 5, "after the root value");
    expect_err("[1][2]", 1, 4, "after the root value");
    expect_err("tru", 1, 1, "unexpected character 't'");
    expect_err("nul]", 1, 1, "unexpected character 'n'");
    expect_err("truex", 1, 5, "after the root value");
    expect_err("[\n  [\n    @]]", CASE_LINE, CASE_COL, "unexpected character '@'");
}

static void expect_loc(odin3_json_loc got, uint32_t line, uint32_t col) {
    TEST_ASSERT_EQUAL_UINT32(line, got.line);
    TEST_ASSERT_EQUAL_UINT32(col, got.col);
}

static void test_value_positions(void) {
    expect_ok("{\n  \"a\": [\n    7,\n    \"x\"\n  ]\n}");
    expect_loc(odin3_json_pos(g_doc.root), 1, 1);
    const odin3_json_node *arr = odin3_json_member(g_doc.root, "a");
    expect_loc(odin3_json_pos(arr), 2, 8);
    expect_loc(odin3_json_key_pos(arr), 2, 3);
    expect_loc(odin3_json_pos(odin3_json_next(odin3_json_first(arr))), 4, 5);
}

static void test_bom_skipped(void) {
    expect_ok("\xEF\xBB\xBF[1]");
    TEST_ASSERT_EQUAL_size_t(1, odin3_json_count(g_doc.root));
    expect_loc(odin3_json_pos(g_doc.root), 1, 1);
    expect_err("\xEF\xBB\xBF[1,]", 1, 4, "trailing comma");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_s("\xEF\xBB"));
}

static char *nested(size_t depth, size_t *len) {
    char *buf = malloc(2 * depth + 1);
    TEST_ASSERT_NOT_NULL(buf);
    memset(buf, '[', depth);
    memset(buf + depth, ']', depth);
    buf[2 * depth] = '\0';
    *len = 2 * depth;
    return buf;
}

static void test_depth_limit(void) {
    size_t len = 0;
    char *ok = nested(DEPTH_OK, &len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_n(ok, len));
    free(ok);
    char *bad = nested(DEPTH_BAD, &len);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_n(bad, len));
    TEST_ASSERT_EQUAL_UINT32(1, g_doc.error.line);
    TEST_ASSERT_EQUAL_UINT32(DEPTH_BAD, g_doc.error.col);
    TEST_ASSERT_NOT_NULL(strstr(g_doc.error.message, "nested deeper than 32"));
    free(bad);
    expect_err(
        "{\"a\":{\"a\":{\"a\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}}}", 1,
        45, "nested deeper");
}

typedef struct deep_job {
    char *text;
    size_t len;
    odin3_status status;
    odin3_json_doc doc;
} deep_job;

static void *deep_thread(void *arg) {
    deep_job *job = arg;
    job->status = odin3_json_parse((const uint8_t *)job->text, job->len, NULL, &job->doc);
    return NULL;
}

/* A 200,000-deep [[[ is refused in a 256 KiB stack: depth costs heap, not stack. */
static void test_deep_input_bounded_stack(void) {
    deep_job job = {.text = malloc(DEEP_BYTES), .len = DEEP_LEVELS};
    TEST_ASSERT_NOT_NULL(job.text);
    memset(job.text, '[', DEEP_LEVELS);
    pthread_attr_t attr;
    pthread_t tid;
    TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
    TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstacksize(&attr, SMALL_STACK));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, &attr, deep_thread, &job));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, NULL));
    pthread_attr_destroy(&attr);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, job.status);
    TEST_ASSERT_EQUAL_UINT32(DEPTH_BAD, job.doc.error.col);
    TEST_ASSERT_NOT_NULL(strstr(job.doc.error.message, "nested deeper than 32"));
    odin3_json_doc_free(&job.doc);
    /* a raised limit still never recurses */
    odin3_json_limits lim = {ODIN3_JSON_MAX_BYTES, ODIN3_JSON_MAX_STRING, DEEP_LEVELS + 1};
    odin3_json_test_set_limits(&lim);
    memset(job.text, '[', DEEP_LEVELS);
    memset(job.text + DEEP_LEVELS, ']', DEEP_LEVELS);
    job.len = DEEP_BYTES;
    TEST_ASSERT_EQUAL_INT(0, pthread_attr_init(&attr));
    TEST_ASSERT_EQUAL_INT(0, pthread_attr_setstacksize(&attr, SMALL_STACK));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, &attr, deep_thread, &job));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, NULL));
    pthread_attr_destroy(&attr);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, job.status);
    odin3_json_doc_free(&job.doc);
    free(job.text);
}

static void test_string_limit(void) {
    odin3_json_limits lim = {ODIN3_JSON_MAX_BYTES, LIMIT_STRING, ODIN3_JSON_MAX_DEPTH};
    odin3_json_test_set_limits(&lim);
    expect_ok("\"12345678\"");
    expect_err("[\"123456789\"]", 1, 2, "string longer than 8");
    expect_err("{\"123456789\":1}", 1, 2, "string longer than 8");
    expect_err("\"1234567\\u00e9\"", 1, 1, "string longer than 8"); /* decoded length counts */
    expect_ok("\"1234\\n\\n\\n\\n\"");
}

static void test_file_limit(void) {
    odin3_json_limits lim = {LIMIT_BYTES, ODIN3_JSON_MAX_STRING, ODIN3_JSON_MAX_DEPTH};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_json_parse((const uint8_t *)"[1,2,3,4,5,6,7]",
                                                     LIMIT_BYTES - 1, &lim, &g_doc));
    odin3_json_doc_free(&g_doc);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_json_parse((const uint8_t *)"[1,2,3,4,5,6,7,8]",
                                                            LIMIT_BYTES + 1, &lim, &g_doc));
    TEST_ASSERT_NOT_NULL(strstr(g_doc.error.message, "file larger than 16 bytes"));
    TEST_ASSERT_EQUAL_UINT32(1, g_doc.error.line);
    odin3_json_test_set_limits(&lim);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_s("[1,2,3,4,5,6,7,8,9,10]"));
}

static void test_invalid_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_json_parse(NULL, 1, NULL, &g_doc));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_json_parse((const uint8_t *)"1", 1, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_json_parse(NULL, 0, NULL, &g_doc));
    odin3_json_doc_free(NULL);
    odin3_json_doc_free(&g_doc);
    odin3_json_doc_free(&g_doc);
}

/* A 2 KB nested object with every kind and escapes; no trailing newline. */
static size_t build_sample(char *buf, size_t cap) {
    size_t len = (size_t)snprintf(buf, cap, "{\"creator\": \"t\\u00e9st\", \"modules\": {");
    for (int mod = 0; len < SAMPLE_BYTES - 200; mod++) {
        len +=
            (size_t)snprintf(buf + len, cap - len,
                             "%s\n  \"m%d\": {\"attributes\": {\"top\": 1, \"s\": \"a\\n\\\"b\"},"
                             " \"ports\": {\"a\": {\"direction\": \"input\", \"bits\": [2, 3,"
                             " \"x\"]}}, \"flag\": [true, false, null, -5]}",
                             mod ? "," : "", mod);
    }
    len += (size_t)snprintf(buf + len, cap - len, "}}");
    return len;
}

static void expect_truncation_fails(const char *sample, size_t cut) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, parse_n(sample, cut),
                                  "truncated file must fail");
    TEST_ASSERT_NULL(g_doc.root);
    TEST_ASSERT_TRUE(g_doc.error.line >= 1 && g_doc.error.col >= 1);
    TEST_ASSERT_TRUE(g_doc.error.message[0] != '\0');
}

static void test_truncation_at_every_byte(void) {
    char sample[SAMPLE_BYTES + 512];
    size_t len = build_sample(sample, sizeof sample);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_n(sample, len));
    TEST_ASSERT_TRUE(len >= SAMPLE_BYTES - 200);
    for (size_t cut = 0; cut < len; cut++) {
        expect_truncation_fails(sample, cut);
    }
}

static void test_byte_flip_never_crashes(void) {
    char sample[SAMPLE_BYTES + 512];
    size_t len = build_sample(sample, sizeof sample);
    static const char flips[] = {'"', '\\', '[', ']', '{', '}', ',', ':', '-', '0', 'u', '\0'};
    for (size_t pos = 0; pos < len; pos++) {
        char saved = sample[pos];
        for (size_t idx = 0; idx < sizeof flips; idx++) {
            sample[pos] = flips[idx];
            odin3_status st = parse_n(sample, len);
            TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_PARSE);
        }
        sample[pos] = saved;
    }
}

typedef struct fail_plan {
    long after;
    bool persist;
} fail_plan;

/* Parses with an injected failure; returns the status, leaving the hook off. */
static odin3_status parse_with_failure(const char *sample, size_t len, fail_plan plan) {
    odin3_json_doc_free(&g_doc);
    if (plan.persist) {
        odin3_util_set_alloc_fail_from(plan.after);
    } else {
        odin3_util_set_alloc_fail_after(plan.after);
    }
    odin3_status st = odin3_json_parse((const uint8_t *)sample, len, NULL, &g_doc);
    odin3_util_set_alloc_fail_after(-1);
    return st;
}

/* Fails the n-th allocation for n = 0, 1, ... until the parse succeeds; returns n. */
static long sweep_single_failures(const char *sample, size_t len) {
    long fails = 0;
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    while (st == ODIN3_ERR_NO_MEMORY && fails < SWEEP_MAX) {
        fail_plan plan = {fails, false};
        st = parse_with_failure(sample, len, plan);
        TEST_ASSERT_TRUE((st == ODIN3_OK) == (g_doc.root != NULL));
        fails += st == ODIN3_ERR_NO_MEMORY ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return fails;
}

static void test_alloc_failure_sweep(void) {
    char sample[SAMPLE_BYTES + 512];
    size_t len = build_sample(sample, sizeof sample);
    long fails = sweep_single_failures(sample, len);
    TEST_ASSERT_TRUE(fails > 0);
    /* persistent failure, as real out-of-memory */
    for (long from = 0; from < fails; from += 7) {
        fail_plan plan = {from, true};
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, parse_with_failure(sample, len, plan));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_kind);
    RUN_TEST(test_navigation);
    RUN_TEST(test_navigation_misses);
    RUN_TEST(test_accessors_on_wrong_kind);
    RUN_TEST(test_member_order_and_keys);
    RUN_TEST(test_scalar_roots);
    RUN_TEST(test_string_escapes);
    RUN_TEST(test_unicode_escapes);
    RUN_TEST(test_unicode_errors);
    RUN_TEST(test_raw_control_bytes);
    RUN_TEST(test_integers);
    RUN_TEST(test_duplicate_key);
    RUN_TEST(test_duplicate_key_in_wide_object);
    RUN_TEST(test_syntax_errors_located);
    RUN_TEST(test_value_positions);
    RUN_TEST(test_bom_skipped);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_deep_input_bounded_stack);
    RUN_TEST(test_string_limit);
    RUN_TEST(test_file_limit);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_truncation_at_every_byte);
    RUN_TEST(test_byte_flip_never_crashes);
    RUN_TEST(test_alloc_failure_sweep);
    return UNITY_END();
}
