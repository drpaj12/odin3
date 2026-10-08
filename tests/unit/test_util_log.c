/*
 * test_util_log.c — unit tests for the log levels, counts, sink, truncation and re-entrancy.
 */
#include "unity.h"
#include "util/log.h"

#include <stddef.h>
#include <string.h>

enum { LONG_MSG = 2000, CAPTURE = 4096, TRUNC_LEN = ODIN3_LOG_BUF - 1 };

static char captured[CAPTURE];
static odin3_log_level captured_level;
static int delivered;
static int nested;

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    delivered++;
    captured_level = level;
    strncpy(captured, msg, sizeof captured - 1);
}

static void reentrant_sink(odin3_log_level level, const char *msg, void *user) {
    capture_sink(level, msg, user);
    if (nested == 0) {
        nested++;
        odin3_log(ODIN3_LOG_ERROR, "inner");
    }
}

void setUp(void) {
    odin3_log_set_level(ODIN3_LOG_INFO);
    odin3_log_set_sink(capture_sink, NULL);
    odin3_log_reset_counts();
    captured[0] = '\0';
    captured_level = ODIN3_LOG_LEVEL_COUNT;
    delivered = 0;
    nested = 0;
}
void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
}

static void test_default_level_info_filters_debug(void) {
    odin3_log(ODIN3_LOG_DEBUG, "hidden %d", 1);
    TEST_ASSERT_EQUAL_INT(0, delivered);
    TEST_ASSERT_EQUAL_UINT(1, odin3_log_count(ODIN3_LOG_DEBUG));
    odin3_log(ODIN3_LOG_INFO, "shown %d", 2);
    TEST_ASSERT_EQUAL_INT(1, delivered);
    TEST_ASSERT_EQUAL_STRING("shown 2", captured);
    TEST_ASSERT_EQUAL_INT(ODIN3_LOG_INFO, captured_level);
}

static void test_counts_per_level_and_reset(void) {
    odin3_log(ODIN3_LOG_ERROR, "e");
    odin3_log(ODIN3_LOG_WARN, "w");
    odin3_log(ODIN3_LOG_WARN, "w");
    odin3_log(ODIN3_LOG_DEBUG, "d");
    TEST_ASSERT_EQUAL_UINT(1, odin3_log_count(ODIN3_LOG_ERROR));
    TEST_ASSERT_EQUAL_UINT(2, odin3_log_count(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_UINT(0, odin3_log_count(ODIN3_LOG_INFO));
    TEST_ASSERT_EQUAL_UINT(1, odin3_log_count(ODIN3_LOG_DEBUG));
    TEST_ASSERT_EQUAL_UINT(0, odin3_log_count(ODIN3_LOG_LEVEL_COUNT));
    odin3_log_reset_counts();
    TEST_ASSERT_EQUAL_UINT(0, odin3_log_count(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_UINT(0, odin3_log_count(ODIN3_LOG_ERROR));
}

static void test_set_level_debug_delivers_debug(void) {
    odin3_log_set_level(ODIN3_LOG_ERROR);
    odin3_log(ODIN3_LOG_WARN, "no");
    TEST_ASSERT_EQUAL_INT(0, delivered);
    odin3_log_set_level(ODIN3_LOG_DEBUG);
    odin3_log(ODIN3_LOG_DEBUG, "yes");
    TEST_ASSERT_EQUAL_INT(1, delivered);
    TEST_ASSERT_EQUAL_STRING("yes", captured);
}

static void test_truncation(void) {
    char text[LONG_MSG + 1];
    memset(text, 'a', LONG_MSG);
    text[LONG_MSG] = '\0';
    odin3_log(ODIN3_LOG_ERROR, "%s", text);
    TEST_ASSERT_EQUAL_INT(1, delivered);
    TEST_ASSERT_EQUAL_UINT(TRUNC_LEN, strlen(captured));
    TEST_ASSERT_EQUAL_STRING("...", captured + TRUNC_LEN - 3);
    TEST_ASSERT_EQUAL_CHAR('a', captured[TRUNC_LEN - 4]);
}

static void test_exact_fit_not_truncated(void) {
    char text[TRUNC_LEN + 1];
    memset(text, 'b', TRUNC_LEN);
    text[TRUNC_LEN] = '\0';
    odin3_log(ODIN3_LOG_ERROR, "%s", text);
    TEST_ASSERT_EQUAL_STRING(text, captured);
}

static void test_sink_null_restores_stderr(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_log(ODIN3_LOG_DEBUG, "filtered, nothing printed");
    TEST_ASSERT_EQUAL_UINT(1, odin3_log_count(ODIN3_LOG_DEBUG));
    TEST_ASSERT_EQUAL_INT(0, delivered);
}

static void test_reentrant_sink_dropped(void) {
    odin3_log_set_sink(reentrant_sink, NULL);
    odin3_log(ODIN3_LOG_WARN, "outer");
    TEST_ASSERT_EQUAL_INT(1, delivered);
    TEST_ASSERT_EQUAL_STRING("outer", captured);
    TEST_ASSERT_EQUAL_UINT(1, odin3_log_count(ODIN3_LOG_WARN));
    TEST_ASSERT_EQUAL_UINT(1, odin3_log_count(ODIN3_LOG_ERROR));
    odin3_log(ODIN3_LOG_WARN, "again");
    TEST_ASSERT_EQUAL_INT(2, delivered);
    TEST_ASSERT_EQUAL_STRING("again", captured);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_level_info_filters_debug);
    RUN_TEST(test_counts_per_level_and_reset);
    RUN_TEST(test_set_level_debug_delivers_debug);
    RUN_TEST(test_truncation);
    RUN_TEST(test_exact_fit_not_truncated);
    RUN_TEST(test_sink_null_restores_stderr);
    RUN_TEST(test_reentrant_sink_dropped);
    return UNITY_END();
}
