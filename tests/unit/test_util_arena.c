/*
 * test_util_arena.c — unit tests for the bump-allocator arena.
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    ALLOC_COUNT = 100,
    SMALL_CHUNK = 1024,
    LARGE_REQUEST = 10000,
    ONE_MIB = 1024 * 1024,
    PIECE = 64,
    PATTERN_BYTES = 256,
    OOM_ROUNDS = 4,
    OOM_ALLOCS = 5,
    OOM_BYTES = 100 * 1024,
    TINY_ALLOCS = 3
};

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void test_alloc_is_aligned_and_zeroed(void) {
    odin3_arena *arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    for (size_t size = 1; size <= ALLOC_COUNT; size++) {
        unsigned char *ptr = odin3_arena_alloc(arena, size);
        TEST_ASSERT_NOT_NULL(ptr);
        TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)ptr % _Alignof(max_align_t));
        for (size_t i = 0; i < size; i++) {
            TEST_ASSERT_EQUAL_UINT8(0, ptr[i]);
        }
        memset(ptr, 0xAB, size);
    }
    odin3_arena_destroy(arena);
}

static void test_alloc_zero_returns_distinct_non_null(void) {
    odin3_arena *arena = odin3_arena_create(0);
    void *first = odin3_arena_alloc(arena, 0);
    void *second = odin3_arena_alloc(arena, 0);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first != second);
    odin3_arena_destroy(arena);
}

static void test_large_request_gets_own_chunk(void) {
    odin3_arena *arena = odin3_arena_create(SMALL_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, LARGE_REQUEST));
    TEST_ASSERT_TRUE(odin3_arena_bytes_reserved(arena) >= LARGE_REQUEST + SMALL_CHUNK &&
                     odin3_arena_bytes_used(arena) >= LARGE_REQUEST);
    odin3_arena_destroy(arena);
}

static void test_pointers_stable(void) {
    odin3_arena *arena = odin3_arena_create(SMALL_CHUNK);
    unsigned char *first = odin3_arena_alloc(arena, PATTERN_BYTES);
    TEST_ASSERT_NOT_NULL(first);
    memset(first, 0x5A, PATTERN_BYTES);
    for (size_t done = 0; done < ONE_MIB; done += PIECE) {
        TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, PIECE));
    }
    for (size_t i = 0; i < PATTERN_BYTES; i++) {
        TEST_ASSERT_EQUAL_UINT8(0x5A, first[i]);
    }
    odin3_arena_destroy(arena);
}

static void test_overflow_returns_null(void) {
    odin3_arena *arena = odin3_arena_create(0);
    TEST_ASSERT_NULL(odin3_arena_alloc(arena, SIZE_MAX));
    TEST_ASSERT_NULL(odin3_arena_strndup(arena, "x", SIZE_MAX));
    TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, 1));
    odin3_arena_destroy(arena);
}

static void test_strndup(void) {
    odin3_arena *arena = odin3_arena_create(0);
    char *copy = odin3_arena_strndup(arena, "abcdef", 3);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("abc", copy);
    odin3_arena_destroy(arena);
}

static void test_tiny_chunk_still_aligned(void) {
    odin3_arena *arena = odin3_arena_create(1);
    TEST_ASSERT_NOT_NULL(arena);
    for (size_t i = 0; i < TINY_ALLOCS; i++) {
        void *ptr = odin3_arena_alloc(arena, 1);
        TEST_ASSERT_NOT_NULL(ptr);
        TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)ptr % _Alignof(max_align_t));
    }
    odin3_arena_destroy(arena);
}

static void test_oom_every_allocation(void) {
    for (long fail = 0; fail < OOM_ROUNDS; fail++) {
        odin3_util_set_alloc_fail_after(fail);
        odin3_arena *arena = odin3_arena_create(0);
        if (arena != NULL) {
            for (size_t i = 0; i < OOM_ALLOCS; i++) {
                (void)odin3_arena_alloc(arena, OOM_BYTES);
            }
        }
        odin3_arena_destroy(arena);
        odin3_util_set_alloc_fail_after(-1);
    }
}

static void test_destroy_null_is_noop(void) {
    odin3_arena_destroy(NULL);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_alloc_is_aligned_and_zeroed);
    RUN_TEST(test_alloc_zero_returns_distinct_non_null);
    RUN_TEST(test_large_request_gets_own_chunk);
    RUN_TEST(test_pointers_stable);
    RUN_TEST(test_overflow_returns_null);
    RUN_TEST(test_strndup);
    RUN_TEST(test_tiny_chunk_still_aligned);
    RUN_TEST(test_oom_every_allocation);
    RUN_TEST(test_destroy_null_is_noop);
    return UNITY_END();
}
