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
    OOM_ROUNDS = 7,
    CREATE_ALLOCS = 2,
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

/* Allocates ONE_MIB in small pieces; returns how many allocations failed. */
static size_t count_failed_pieces(odin3_arena *arena) {
    size_t failed = 0;
    for (size_t done = 0; done < ONE_MIB; done += PIECE) {
        if (odin3_arena_alloc(arena, PIECE) == NULL) {
            failed++;
        }
    }
    return failed;
}

static void test_pointers_stable(void) {
    odin3_arena *arena = odin3_arena_create(SMALL_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    unsigned char *first = odin3_arena_alloc(arena, PATTERN_BYTES);
    TEST_ASSERT_NOT_NULL(first);
    memset(first, 0x5A, PATTERN_BYTES);
    TEST_ASSERT_EQUAL_UINT(0, count_failed_pieces(arena));
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

static size_t count_misaligned(void *const *ptrs, size_t count) {
    size_t bad = 0;
    for (size_t i = 0; i < count; i++) {
        if (ptrs[i] == NULL || (uintptr_t)ptrs[i] % _Alignof(max_align_t) != 0) {
            bad++;
        }
    }
    return bad;
}

static void test_tiny_chunk_still_aligned(void) {
    odin3_arena *arena = odin3_arena_create(1);
    TEST_ASSERT_NOT_NULL(arena);
    void *ptrs[TINY_ALLOCS];
    for (size_t i = 0; i < TINY_ALLOCS; i++) {
        ptrs[i] = odin3_arena_alloc(arena, 1);
    }
    TEST_ASSERT_EQUAL_UINT(0, count_misaligned(ptrs, TINY_ALLOCS));
    TEST_ASSERT_TRUE(ptrs[0] != ptrs[1] && ptrs[1] != ptrs[2] && ptrs[0] != ptrs[2]);
    odin3_arena_destroy(arena);
}

/* Allocates OOM_ALLOCS big blocks; returns how many came back NULL, checking the rest. */
static size_t oom_alloc_round(odin3_arena *arena) {
    size_t failures = 0;
    for (size_t i = 0; i < OOM_ALLOCS; i++) {
        unsigned char *ptr = odin3_arena_alloc(arena, OOM_BYTES);
        if (ptr == NULL) {
            failures++;
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8(0, ptr[0]);
        TEST_ASSERT_EQUAL_UINT8(0, ptr[OOM_BYTES - 1]);
        memset(ptr, 0xCD, OOM_BYTES);
    }
    return failures;
}

static size_t oom_round_or_zero(odin3_arena *arena) {
    return arena == NULL ? 0 : oom_alloc_round(arena);
}

static void test_oom_every_allocation(void) {
    /* create performs two allocations (arena, first chunk); each 100 KiB request is a third. */
    for (long fail = 0; fail < OOM_ROUNDS; fail++) {
        odin3_util_set_alloc_fail_after(fail);
        odin3_arena *arena = odin3_arena_create(0);
        TEST_ASSERT_EQUAL_UINT(fail < CREATE_ALLOCS ? 0 : 1, oom_round_or_zero(arena));
        TEST_ASSERT_EQUAL(fail < CREATE_ALLOCS, arena == NULL);
        odin3_arena_destroy(arena);
        odin3_util_set_alloc_fail_after(-1);
    }
}

static void test_small_alloc_after_oversize_uses_head(void) {
    odin3_arena *arena = odin3_arena_create(SMALL_CHUNK);
    TEST_ASSERT_NOT_NULL(arena);
    TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, LARGE_REQUEST));
    size_t reserved = odin3_arena_bytes_reserved(arena);
    TEST_ASSERT_NOT_NULL(odin3_arena_alloc(arena, PIECE));
    TEST_ASSERT_EQUAL_UINT(reserved, odin3_arena_bytes_reserved(arena));
    odin3_arena_destroy(arena);
}

static void test_strndup_null_empty(void) {
    odin3_arena *arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    char *copy = odin3_arena_strndup(arena, NULL, 0);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("", copy);
    odin3_arena_destroy(arena);
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
    RUN_TEST(test_small_alloc_after_oversize_uses_head);
    RUN_TEST(test_strndup_null_empty);
    RUN_TEST(test_destroy_null_is_noop);
    return UNITY_END();
}
