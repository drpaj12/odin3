/*
 * test_util_alloc.c — unit tests for the util allocation wrapper and its failure hook.
 */
#include "unity.h"
#include "util/alloc.h"

#include <stddef.h>
#include <string.h>

enum { BLOCK_BYTES = 64, SMALL_BYTES = 8, FAIL_AFTER_TWO = 2 };

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void test_malloc_and_free_roundtrip(void) {
    char *block = odin3_util_malloc(BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(block);
    memset(block, 'x', BLOCK_BYTES);
    TEST_ASSERT_EQUAL_CHAR('x', block[BLOCK_BYTES - 1]);
    odin3_util_free(block);
}

static void test_fail_after_zero_fails_next(void) {
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_NULL(odin3_util_malloc(SMALL_BYTES));
    void *block = odin3_util_malloc(SMALL_BYTES);
    TEST_ASSERT_NOT_NULL(block);
    odin3_util_free(block);
}

static void test_fail_after_two(void) {
    odin3_util_set_alloc_fail_after(FAIL_AFTER_TWO);
    void *first = odin3_util_malloc(SMALL_BYTES);
    void *second = odin3_util_calloc(SMALL_BYTES);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NULL(odin3_util_malloc(SMALL_BYTES));
    odin3_util_free(first);
    odin3_util_free(second);
}

static void test_realloc_failure_keeps_block(void) {
    char *block = odin3_util_malloc(SMALL_BYTES);
    TEST_ASSERT_NOT_NULL(block);
    memset(block, 'y', SMALL_BYTES);
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_NULL(odin3_util_realloc(block, BLOCK_BYTES));
    TEST_ASSERT_EQUAL_CHAR('y', block[SMALL_BYTES - 1]);
    odin3_util_free(block);
}

static void test_calloc_is_zeroed(void) {
    unsigned char *block = odin3_util_calloc(BLOCK_BYTES);
    TEST_ASSERT_NOT_NULL(block);
    for (size_t i = 0; i < BLOCK_BYTES; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, block[i]);
    }
    odin3_util_free(block);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_malloc_and_free_roundtrip);
    RUN_TEST(test_fail_after_zero_fails_next);
    RUN_TEST(test_fail_after_two);
    RUN_TEST(test_realloc_failure_keeps_block);
    RUN_TEST(test_calloc_is_zeroed);
    return UNITY_END();
}
