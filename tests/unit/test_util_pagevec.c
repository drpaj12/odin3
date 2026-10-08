/*
 * test_util_pagevec.c — unit tests for the paged array with stable addresses.
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/pagevec.h"

#include <stdint.h>

enum {
    MANY = 10000,
    LAST_OF_PAGE = ODIN3_PAGEVEC_PAGE_ELEMS - 1,
    THREE_PAGES = 3 * ODIN3_PAGEVEC_PAGE_ELEMS
};

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

/* Pushes count elements, each holding its index plus one. */
static void fill(odin3_pagevec *pv, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t *slot = odin3_pagevec_push(pv, NULL);
        TEST_ASSERT_NOT_NULL(slot);
        *slot = i + 1;
    }
}

static void test_push_across_pages(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    for (size_t i = 0; i < MANY; i++) {
        size_t index = SIZE_MAX;
        uint64_t *slot = odin3_pagevec_push(pv, &index);
        TEST_ASSERT_NOT_NULL(slot);
        TEST_ASSERT_EQUAL_UINT(i, index);
        TEST_ASSERT_EQUAL_UINT64(0, *slot);
        *slot = i;
    }
    TEST_ASSERT_EQUAL_UINT(MANY, odin3_pagevec_len(pv));
    for (size_t i = 0; i < MANY; i++) {
        TEST_ASSERT_EQUAL_UINT64(i, *(uint64_t *)odin3_pagevec_at(pv, i));
        TEST_ASSERT_EQUAL_UINT64(i, *(const uint64_t *)odin3_pagevec_cat(pv, i));
    }
    odin3_pagevec_destroy(pv);
}

static void test_addresses_stable(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    fill(pv, ODIN3_PAGEVEC_PAGE_ELEMS);
    uint64_t *first = odin3_pagevec_at(pv, 0);
    uint64_t *last = odin3_pagevec_at(pv, LAST_OF_PAGE);
    fill(pv, THREE_PAGES - ODIN3_PAGEVEC_PAGE_ELEMS);
    TEST_ASSERT_EQUAL_PTR(first, odin3_pagevec_at(pv, 0));
    TEST_ASSERT_EQUAL_PTR(last, odin3_pagevec_at(pv, LAST_OF_PAGE));
    TEST_ASSERT_EQUAL_UINT64(1, *first);
    TEST_ASSERT_EQUAL_UINT64(ODIN3_PAGEVEC_PAGE_ELEMS, *last);
    odin3_pagevec_destroy(pv);
}

static void test_page_boundary_4095_4096(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    fill(pv, ODIN3_PAGEVEC_PAGE_ELEMS + 1);
    uint64_t *before = odin3_pagevec_at(pv, LAST_OF_PAGE);
    uint64_t *after = odin3_pagevec_at(pv, ODIN3_PAGEVEC_PAGE_ELEMS);
    TEST_ASSERT_TRUE(before != after);
    TEST_ASSERT_EQUAL_UINT64(ODIN3_PAGEVEC_PAGE_ELEMS, *before);
    TEST_ASSERT_EQUAL_UINT64(ODIN3_PAGEVEC_PAGE_ELEMS + 1, *after);
    odin3_pagevec_destroy(pv);
}

static void test_bytes_reserved_grows_by_pages(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    size_t page_bytes = ODIN3_PAGEVEC_PAGE_ELEMS * sizeof(uint64_t);
    TEST_ASSERT_EQUAL_UINT(0, odin3_pagevec_bytes_reserved(pv));
    fill(pv, 1);
    TEST_ASSERT_EQUAL_UINT(page_bytes, odin3_pagevec_bytes_reserved(pv));
    fill(pv, ODIN3_PAGEVEC_PAGE_ELEMS - 1);
    TEST_ASSERT_EQUAL_UINT(page_bytes, odin3_pagevec_bytes_reserved(pv));
    fill(pv, 1);
    TEST_ASSERT_EQUAL_UINT(2 * page_bytes, odin3_pagevec_bytes_reserved(pv));
    odin3_pagevec_destroy(pv);
}

enum { TABLE_PAGES = 8, TABLE_FULL_ELEMS = TABLE_PAGES * ODIN3_PAGEVEC_PAGE_ELEMS };

static void test_page_table_growth_keeps_addresses(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    fill(pv, ODIN3_PAGEVEC_PAGE_ELEMS);
    uint64_t *first = odin3_pagevec_at(pv, 0);
    uint64_t *last = odin3_pagevec_at(pv, LAST_OF_PAGE);
    fill(pv, TABLE_FULL_ELEMS + ODIN3_PAGEVEC_PAGE_ELEMS);
    TEST_ASSERT_EQUAL_UINT(TABLE_FULL_ELEMS + 2 * ODIN3_PAGEVEC_PAGE_ELEMS, odin3_pagevec_len(pv));
    TEST_ASSERT_EQUAL_PTR(first, odin3_pagevec_at(pv, 0));
    TEST_ASSERT_EQUAL_PTR(last, odin3_pagevec_at(pv, LAST_OF_PAGE));
    TEST_ASSERT_EQUAL_UINT64(1, *first);
    TEST_ASSERT_EQUAL_UINT64(ODIN3_PAGEVEC_PAGE_ELEMS, *last);
    odin3_pagevec_destroy(pv);
}

/* Fills 8 pages (table full), then fails the n-th allocation of the push that needs page 9. */
static void check_oom_at_ninth_page(long fail_after) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    fill(pv, TABLE_FULL_ELEMS);
    size_t bytes = odin3_pagevec_bytes_reserved(pv);
    odin3_util_set_alloc_fail_after(fail_after);
    size_t index = 0;
    TEST_ASSERT_NULL(odin3_pagevec_push(pv, &index));
    TEST_ASSERT_EQUAL_UINT(TABLE_FULL_ELEMS, odin3_pagevec_len(pv));
    TEST_ASSERT_EQUAL_UINT(bytes, odin3_pagevec_bytes_reserved(pv));
    TEST_ASSERT_NOT_NULL(odin3_pagevec_push(pv, &index));
    TEST_ASSERT_EQUAL_UINT(TABLE_FULL_ELEMS, index);
    TEST_ASSERT_EQUAL_UINT(TABLE_FULL_ELEMS + 1, odin3_pagevec_len(pv));
    odin3_pagevec_destroy(pv);
}

static void test_oom_table_grow_at_ninth_page(void) {
    check_oom_at_ninth_page(0);
}

static void test_oom_page_alloc_after_table_grow(void) {
    check_oom_at_ninth_page(1);
}

static void test_oom_push_returns_null_and_len_unchanged(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    odin3_util_set_alloc_fail_after(0);
    size_t index = 42;
    TEST_ASSERT_NULL(odin3_pagevec_push(pv, &index));
    TEST_ASSERT_EQUAL_UINT(0, odin3_pagevec_len(pv));
    TEST_ASSERT_EQUAL_UINT(0, odin3_pagevec_bytes_reserved(pv));
    TEST_ASSERT_NOT_NULL(odin3_pagevec_push(pv, &index));
    TEST_ASSERT_EQUAL_UINT(0, index);
    TEST_ASSERT_EQUAL_UINT(1, odin3_pagevec_len(pv));
    odin3_pagevec_destroy(pv);
}

static void test_oom_page_alloc_on_first_push(void) {
    odin3_pagevec *pv = odin3_pagevec_create(sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(pv);
    odin3_util_set_alloc_fail_after(1); /* page table succeeds, page calloc fails */
    TEST_ASSERT_NULL(odin3_pagevec_push(pv, NULL));
    TEST_ASSERT_EQUAL_UINT(0, odin3_pagevec_len(pv));
    TEST_ASSERT_EQUAL_UINT(0, odin3_pagevec_bytes_reserved(pv));
    TEST_ASSERT_NOT_NULL(odin3_pagevec_push(pv, NULL));
    TEST_ASSERT_EQUAL_UINT(1, odin3_pagevec_len(pv));
    odin3_pagevec_destroy(pv);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_push_across_pages);
    RUN_TEST(test_addresses_stable);
    RUN_TEST(test_page_boundary_4095_4096);
    RUN_TEST(test_bytes_reserved_grows_by_pages);
    RUN_TEST(test_page_table_growth_keeps_addresses);
    RUN_TEST(test_oom_table_grow_at_ninth_page);
    RUN_TEST(test_oom_page_alloc_after_table_grow);
    RUN_TEST(test_oom_page_alloc_on_first_push);
    RUN_TEST(test_oom_push_returns_null_and_len_unchanged);
    return UNITY_END();
}
