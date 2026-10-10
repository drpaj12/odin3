/*
 * test_pp_check.c — unit tests for odin3_pp_check (2B spec §7.8): the stream invariants on the
 * expansion probes and the preprocessor micros, the per-buffer reachability rule with the
 * DROP/F(a) example, and one corruption per rule through the hidden hook.
 */
#include "ast/srcman.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_internal.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ODIN3_PP_PROBES
#error "ODIN3_PP_PROBES must name tests/unit/data/pp/probes"
#endif
#ifndef ODIN3_PP_MICROS
#error "ODIN3_PP_MICROS must name tests/micro/verilog"
#endif

enum { PATH_BUF = 1024, MSG_MAX = 4096, MAX_MADE = 64 };

static odin3_design *design;
static odin3_srcman *sm;
static odin3_pp *pp;
static odin3_pp_stream stream;
static char tmp[PATH_BUF / 2];
static char captured[MSG_MAX];
static int errors;
static char made[MAX_MADE][PATH_BUF];
static int nmade;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors++;
        (void)snprintf(captured, sizeof captured, "%s", msg);
    }
}

void setUp(void) {
    odin3_log_set_sink(sink, NULL);
    captured[0] = '\0';
    errors = 0;
    nmade = 0;
    pp = NULL;
    memset(&stream, 0, sizeof stream);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3_ppchk_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
}

/* Cleans up before asserting: after TEST_IGNORE every assertion aborts at once. */
void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_pp_destroy(pp);
    odin3_design_destroy(design);
    int failed = 0;
    for (int i = nmade - 1; i >= 0; i--) {
        failed += remove(made[i]) != 0;
    }
    failed += rmdir(tmp) != 0;
    TEST_ASSERT_EQUAL_INT(0, failed);
}

/* --- helpers ------------------------------------------------------------------------------- */

static uint32_t id(const char *str) {
    uint32_t got = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(str), &got));
    return got;
}

static void fresh_pp(void) {
    odin3_pp_destroy(pp);
    odin3_pp_config none = {NULL, 0, NULL, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_create(design, &none, &pp));
}

static odin3_status run_path(const char *name, const char *path) {
    if (pp == NULL) {
        fresh_pp();
    }
    odin3_pp_source src = {id(name), id(path), id("work"), {0}};
    return odin3_pp_run(pp, &src, &stream);
}

static odin3_status run(const char *text) {
    static int serial;
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/c%d.v", tmp, serial++);
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    TEST_ASSERT_TRUE(nmade < MAX_MADE);
    (void)snprintf(made[nmade++], PATH_BUF, "%s", path);
    return run_path("c.v", path);
}

/* Runs text (a fresh preprocessor) and checks the stream explicitly. */
static void assert_checked(const char *text) {
    fresh_pp();
    odin3_status st = run(text);
    if (st != ODIN3_OK) {
        TEST_FAIL_MESSAGE(captured);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_check(pp, stream.stream));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

static bool not_supported_yet(void) {
    return strstr(captured, "not supported yet") != NULL;
}

/* A file to check: dir/file and the error it must stop at (NULL: none). */
typedef struct file_case {
    const char *dir;
    const char *file;
    const char *want;
} file_case;

/* Runs a file (a fresh preprocessor) and checks the stream; a PARSE error must contain want
 * or be a Task 7 stub's (skipped until Task 9). Returns true when the stream was checked. */
static bool check_file(file_case fc) {
    char path[PATH_BUF];
    (void)snprintf(path, sizeof path, "%s/%s", fc.dir, fc.file);
    fresh_pp();
    errors = 0;
    captured[0] = '\0';
    odin3_status st = run_path(fc.file, path);
    if (st == ODIN3_ERR_PARSE && not_supported_yet()) {
        return false;
    }
    if (fc.want != NULL) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, st, fc.file);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(captured, fc.want), captured);
        return false;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, captured);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_pp_check(pp, stream.stream), fc.file);
    return true;
}

static const odin3_pp_bufrec *bufrec(uint32_t n) {
    TEST_ASSERT_TRUE(n < pp->bufs.len);
    return odin3_vec_cat(&pp->bufs, n);
}

/* --- the invariants hold ------------------------------------------------------------------- */

static void test_plain_file(void) {
    assert_checked("module m;\n  wire a; // c\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, stream.nsegments);
}

static void test_empty_file(void) {
    assert_checked("");
    TEST_ASSERT_EQUAL_UINT32(0, stream.nsegments);
}

static void test_2a_example(void) {
    assert_checked("`define INNER(p) (p + 1)\n`define OUTER(q) `INNER(q * 2)\n"
                   "assign y = `OUTER(xx);\n");
    for (uint32_t i = 1; i < pp->bufs.len; i++) {
        TEST_ASSERT_NOT_EQUAL_UINT32(0, bufrec(i)->reach); /* A1 counts through A2's def */
    }
}

static void test_inserted_bytes(void) {
    assert_checked("`define N(x) [x]\n`define M a/**/b\nq = `N(p /* c */ q) `M;\n"
                   "a`undef M`define Z 1\n");
}

/* §7.8: only F's buffer must be reachable; DROP's expansion and a's argument count zero. */
static void test_drop_example_reachability(void) {
    assert_checked("`define DROP(x)\n`define F(x) `DROP(x) z\nq = `F(a);\n");
    TEST_ASSERT_EQUAL_UINT32(4, pp->bufs.len); /* c.v, E(F), A(x <- a), E(DROP) */
    TEST_ASSERT_NOT_EQUAL_UINT32(0, bufrec(1)->reach);
    TEST_ASSERT_EQUAL_UINT32(0, bufrec(2)->reach);
    TEST_ASSERT_EQUAL_UINT32(0, bufrec(3)->reach);
}

static void test_probes(void) {
    static const char *const k_ok[] = {"t1.v", "t2.v", "t3.v", "t6.v", "u2.v", "v1.v",
                                       "w2.v", "w3.v", "w4.v", "w5.v", "t7.v", "t9.v"};
    uint32_t checked = 0;
    for (size_t i = 0; i < sizeof k_ok / sizeof k_ok[0]; i++) {
        checked += check_file((file_case){ODIN3_PP_PROBES, k_ok[i], NULL}) ? 1 : 0;
    }
    TEST_ASSERT_TRUE(checked >= 8);
    (void)check_file((file_case){ODIN3_PP_PROBES, "w1.v", "recursive macro 'N'"});
    (void)check_file((file_case){ODIN3_PP_PROBES, "rec.v", "recursive macro 'R'"});
}

static void test_preprocessor_micros(void) {
    static const char *const k_micros[] = {"default_net.v",
                                           "elsif_both_defined.v",
                                           "elsif_defined.v",
                                           "ifdef_defined.v",
                                           "ifdef_else_defined.v",
                                           "ifdef_else_undefined.v",
                                           "ifdef_undefined.v",
                                           "ifndef_defined.v",
                                           "ifndef_else_defined.v",
                                           "ifndef_else_undefined.v",
                                           "ifndef_undefined.v",
                                           "multiple_nested_define.v",
                                           "nested_define.v",
                                           "nested_ifdef_alldef.v",
                                           "nested_ifdef_middef.v",
                                           "nested_ifdef_nonedef.v",
                                           "nested_ifdef_outsidedef.v",
                                           "nested_ifdef_twodef.v",
                                           "nested_operation_define.v",
                                           "operation_define.v",
                                           "undef.v"};
    uint32_t checked = 0;
    for (size_t i = 0; i < sizeof k_micros / sizeof k_micros[0]; i++) {
        checked +=
            check_file((file_case){ODIN3_PP_MICROS "/preprocessor", k_micros[i], NULL}) ? 1 : 0;
    }
    TEST_ASSERT_TRUE(checked >= 5);
}

/* The Parmys-rejected splicing micros stop at `ADDITION_num (PP-6). */
static void test_syntax_define_micros(void) {
    static const char *const k_micros[] = {"preprocessor_complex_define.v", "preprocessor_define.v",
                                           "preprocessor_define_with_comment.v"};
    for (size_t i = 0; i < sizeof k_micros / sizeof k_micros[0]; i++) {
        (void)check_file((file_case){ODIN3_PP_MICROS "/syntax", k_micros[i],
                                     "syntax: undefined macro 'ADDITION_num'"});
    }
}

static void test_check_misuse(void) {
    assert_checked("q;\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_check(NULL, stream.stream));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pp_check(pp, (odin3_srcbuf_id){stream.stream.v + 1}));
}

/* --- one corruption per rule --------------------------------------------------------------- */

static const char k_rich[] = "`define DROP(x)\n`define F(x) `DROP(x) z\n`define N(y) [y]\n"
                             "q = `F(a) + `N(p /* c */ r);\n";

static void assert_corrupt(odin3_pp_test_target target, const char *want) {
    assert_checked(k_rich);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_test_corrupt(pp, target));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_pp_check(pp, stream.stream));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING(want, captured);
}

static void test_corrupt_segment_start(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_SEGMENT_START,
                   "odin3: error: pp_check: the first segment starts at output offset 1, not 0");
}

static void test_corrupt_segment_order(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_SEGMENT_ORDER,
                   "odin3: error: pp_check: segment 1 at output offset 0 does not follow output "
                   "offset 0");
}

static void test_corrupt_segment_loc(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_SEGMENT_LOC,
                   "odin3: error: pp_check: segment 0 has loc 4294967295, which does not decode");
}

static void test_corrupt_byte(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_BYTE,
                   "odin3: error: pp_check: output byte 0 differs from its spelling");
}

static void test_corrupt_loc0_blank(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_LOC0_BLANK,
                   "odin3: error: pp_check: output byte 15 has loc 0 and is not a blank");
}

static void test_corrupt_loc0_alone(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_LOC0_ALONE,
                   "odin3: error: pp_check: loc-0 segment 7 is 3 bytes, not 1");
}

static void test_corrupt_reach(void) {
    assert_corrupt(ODIN3_PP_CORRUPT_REACH, "odin3: error: pp_check: buffer 2 of the run is "
                                           "reached by emitted bytes but from no segment");
}

static void test_corrupt_needs_a_target(void) {
    assert_checked("q;\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pp_test_corrupt(pp, ODIN3_PP_CORRUPT_LOC0_BLANK));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pp_test_corrupt(pp, ODIN3_PP_CORRUPT_REACH));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pp_check(pp, stream.stream));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_file);
    RUN_TEST(test_empty_file);
    RUN_TEST(test_2a_example);
    RUN_TEST(test_inserted_bytes);
    RUN_TEST(test_drop_example_reachability);
    RUN_TEST(test_probes);
    RUN_TEST(test_preprocessor_micros);
    RUN_TEST(test_syntax_define_micros);
    RUN_TEST(test_check_misuse);
    RUN_TEST(test_corrupt_segment_start);
    RUN_TEST(test_corrupt_segment_order);
    RUN_TEST(test_corrupt_segment_loc);
    RUN_TEST(test_corrupt_byte);
    RUN_TEST(test_corrupt_loc0_blank);
    RUN_TEST(test_corrupt_loc0_alone);
    RUN_TEST(test_corrupt_reach);
    RUN_TEST(test_corrupt_needs_a_target);
    return UNITY_END();
}
