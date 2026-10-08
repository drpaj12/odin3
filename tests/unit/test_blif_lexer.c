/*
 * test_blif_lexer.c — unit tests for the BLIF lexer.
 */
#include "frontends/blif/lexer.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BIG_LINE = 2 * 1024 * 1024, MSG_MAX = 256, OOM_SWEEP = 60 };

static const char *const PATH = "odin3_lexer_test.blif";
static char last_error[MSG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
    }
}

void setUp(void) {
    last_error[0] = '\0';
    odin3_log_set_sink(sink, NULL);
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    (void)remove(PATH);
}

static void write_file(const char *data, size_t len) {
    FILE *file = fopen(PATH, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(len, fwrite(data, 1, len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void write_str(const char *data) {
    write_file(data, strlen(data));
}

static void expect_line(odin3_blif_lexer *lx, uint32_t line, const char *toks) {
    odin3_blif_line got;
    TEST_ASSERT_TRUE(odin3_blif_lexer_next(lx, &got));
    TEST_ASSERT_EQUAL_UINT32(line, got.line);
    char joined[MSG_MAX] = "";
    for (uint32_t i = 0; i < got.count; i++) {
        TEST_ASSERT_EQUAL_size_t(strlen(got.tokens[i].ptr), got.tokens[i].len);
        if (i > 0) {
            (void)strcat(joined, "|");
        }
        (void)strcat(joined, got.tokens[i].ptr);
    }
    TEST_ASSERT_EQUAL_STRING(toks, joined);
}

static void expect_eof(odin3_blif_lexer *lx) {
    odin3_blif_line got;
    TEST_ASSERT_FALSE(odin3_blif_lexer_next(lx, &got));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_lexer_status(lx));
}

static odin3_blif_lexer *open_ok(void) {
    odin3_blif_lexer *lx = odin3_blif_lexer_open(PATH);
    TEST_ASSERT_NOT_NULL(lx);
    return lx;
}

static void test_tokens_and_lines(void) {
    write_str(".model top\n\n.inputs a b\n   \n.names a b y\n11 1\n.end\n");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 1, ".model|top");
    expect_line(lx, 3, ".inputs|a|b");
    expect_line(lx, 5, ".names|a|b|y");
    expect_line(lx, 6, "11|1");
    expect_line(lx, 7, ".end");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_continuation_including_mid_subckt(void) {
    write_str(".inputs a \\\n b \\  \n c\n.subckt m x=1 \\\n y=2\\\n z=3\n.end\n");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 1, ".inputs|a|b|c");
    expect_line(lx, 4, ".subckt|m|x=1|y=2|z=3");
    expect_line(lx, 7, ".end");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_comments_anywhere(void) {
    write_str("# header\n.names a y # trailing\n1 1 # after a cover row\n#only\n.end#x\n");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 2, ".names|a|y");
    expect_line(lx, 3, "1|1");
    expect_line(lx, 5, ".end");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_comment_does_not_continue(void) {
    write_str(".a b # c \\\n.d\n");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 1, ".a|b");
    expect_line(lx, 2, ".d");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_tabs_and_crlf(void) {
    write_str(".names\ta\t b\r\n1\t1\r\n.inputs x \\\r\n y\r\n");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 1, ".names|a|b");
    expect_line(lx, 2, "1|1");
    expect_line(lx, 3, ".inputs|x|y");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_no_trailing_newline(void) {
    write_str(".end");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 1, ".end");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_literal_backslash_and_empty_file(void) {
    write_str("a\\b $auto$x:1:M$2\n");
    odin3_blif_lexer *lx = open_ok();
    expect_line(lx, 1, "a\\b|$auto$x:1:M$2");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
    write_str("");
    lx = open_ok();
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_two_megabyte_line(void) {
    char *data = malloc(BIG_LINE + 8);
    TEST_ASSERT_NOT_NULL(data);
    memset(data, 'x', BIG_LINE);
    for (size_t i = 7; i < BIG_LINE; i += 8) {
        data[i] = (i % 16 == 7) ? ' ' : '\t';
    }
    memcpy(data + BIG_LINE, "\n.end\n", 7);
    write_file(data, BIG_LINE + 6);
    free(data);
    odin3_blif_lexer *lx = open_ok();
    odin3_blif_line got;
    TEST_ASSERT_TRUE(odin3_blif_lexer_next(lx, &got));
    TEST_ASSERT_EQUAL_UINT32(BIG_LINE / 8, got.count);
    TEST_ASSERT_EQUAL_size_t(7, got.tokens[got.count - 1].len);
    expect_line(lx, 2, ".end");
    expect_eof(lx);
    odin3_blif_lexer_close(lx);
}

static void test_missing_file(void) {
    odin3_blif_lexer *missing = odin3_blif_lexer_open("/nonexistent/dir/none.blif");
    TEST_ASSERT_TRUE(missing == NULL);
    TEST_ASSERT_NOT_NULL(strstr(last_error, "/nonexistent/dir/none.blif"));
}

static bool read_all(void) {
    odin3_blif_lexer *lx = odin3_blif_lexer_open(PATH);
    if (lx == NULL) {
        return false;
    }
    odin3_blif_line got;
    while (odin3_blif_lexer_next(lx, &got)) {
    }
    bool ok = odin3_blif_lexer_status(lx) == ODIN3_OK;
    if (!ok) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_blif_lexer_status(lx));
    }
    odin3_blif_lexer_close(lx);
    return ok;
}

static void test_oom_sweep(void) {
    write_str(".model top\n.inputs a b c \\\n d e f g h i j k l m n o p q\n.end\n");
    bool completed = false;
    for (long i = 0; i < OOM_SWEEP && !completed; i++) {
        odin3_util_set_alloc_fail_after(i);
        completed = read_all();
        odin3_util_set_alloc_fail_after(-1);
    }
    TEST_ASSERT_TRUE(completed);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tokens_and_lines);
    RUN_TEST(test_continuation_including_mid_subckt);
    RUN_TEST(test_comments_anywhere);
    RUN_TEST(test_comment_does_not_continue);
    RUN_TEST(test_tabs_and_crlf);
    RUN_TEST(test_no_trailing_newline);
    RUN_TEST(test_literal_backslash_and_empty_file);
    RUN_TEST(test_two_megabyte_line);
    RUN_TEST(test_missing_file);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
