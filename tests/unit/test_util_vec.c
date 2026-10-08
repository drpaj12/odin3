/*
 * test_util_vec.c — unit tests for the growable contiguous array.
 */
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <stdint.h>

enum { MANY = 10000, FIRST_CAP = 8 };

typedef struct big {
    uint64_t a, b, c;
} big;

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void test_push_get_10000_ints(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(int));
    for (int i = 0; i < MANY; i++) {
        int *slot = odin3_vec_push(&vec);
        TEST_ASSERT_NOT_NULL(slot);
        *slot = i;
    }
    TEST_ASSERT_EQUAL_UINT(MANY, vec.len);
    TEST_ASSERT_TRUE(vec.cap >= MANY);
    TEST_ASSERT_EQUAL_UINT(0, vec.cap & (vec.cap - 1));
    for (int i = 0; i < MANY; i++) {
        TEST_ASSERT_EQUAL_INT(i, *(int *)odin3_vec_at(&vec, (size_t)i));
        TEST_ASSERT_EQUAL_INT(i, *(const int *)odin3_vec_cat(&vec, (size_t)i));
    }
    odin3_vec_free(&vec);
}

static void test_push_is_zeroed(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(big));
    big *first = odin3_vec_push(&vec);
    TEST_ASSERT_NOT_NULL(first);
    first->a = first->b = first->c = UINT64_MAX;
    odin3_vec_clear(&vec);
    big *again = odin3_vec_push(&vec);
    TEST_ASSERT_NOT_NULL(again);
    TEST_ASSERT_EQUAL_UINT64(0, again->a);
    TEST_ASSERT_EQUAL_UINT64(0, again->b);
    TEST_ASSERT_EQUAL_UINT64(0, again->c);
    odin3_vec_free(&vec);
}

static void test_reserve_overflow(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(big));
    TEST_ASSERT_NOT_NULL(odin3_vec_push(&vec));
    void *data = vec.data;
    size_t cap = vec.cap;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_vec_reserve(&vec, SIZE_MAX));
    TEST_ASSERT_EQUAL_PTR(data, vec.data);
    TEST_ASSERT_EQUAL_UINT(cap, vec.cap);
    TEST_ASSERT_EQUAL_UINT(1, vec.len);
    odin3_vec_free(&vec);
}

static void test_pop_and_clear_keep_capacity(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(int));
    for (int i = 0; i < FIRST_CAP + 1; i++) {
        TEST_ASSERT_NOT_NULL(odin3_vec_push(&vec));
    }
    size_t cap = vec.cap;
    odin3_vec_pop(&vec);
    TEST_ASSERT_EQUAL_UINT(FIRST_CAP, vec.len);
    TEST_ASSERT_EQUAL_UINT(cap, vec.cap);
    odin3_vec_clear(&vec);
    TEST_ASSERT_EQUAL_UINT(0, vec.len);
    TEST_ASSERT_EQUAL_UINT(cap, vec.cap);
    odin3_vec_free(&vec);
}

static void test_oom_on_growth(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(int));
    for (int i = 0; i < FIRST_CAP; i++) {
        int *slot = odin3_vec_push(&vec);
        TEST_ASSERT_NOT_NULL(slot);
        *slot = i + 1;
    }
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_NULL(odin3_vec_push(&vec));
    TEST_ASSERT_EQUAL_UINT(FIRST_CAP, vec.len);
    for (int i = 0; i < FIRST_CAP; i++) {
        TEST_ASSERT_EQUAL_INT(i + 1, *(int *)odin3_vec_at(&vec, (size_t)i));
    }
    odin3_vec_free(&vec);
}

static void test_free_twice_safe(void) {
    odin3_vec vec;
    odin3_vec_init(&vec, sizeof(int));
    TEST_ASSERT_NOT_NULL(odin3_vec_push(&vec));
    odin3_vec_free(&vec);
    TEST_ASSERT_NULL(vec.data);
    TEST_ASSERT_EQUAL_UINT(0, vec.len);
    TEST_ASSERT_EQUAL_UINT(0, vec.cap);
    odin3_vec_free(&vec);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_push_get_10000_ints);
    RUN_TEST(test_push_is_zeroed);
    RUN_TEST(test_reserve_overflow);
    RUN_TEST(test_pop_and_clear_keep_capacity);
    RUN_TEST(test_oom_on_growth);
    RUN_TEST(test_free_twice_safe);
    return UNITY_END();
}
