/*
 * test_util_idindex.c — unit tests for the ID-only hash-cons index.
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/idindex.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    NAME_LEN = 12,
    GROW_N = 200000,
    RAND_OPS = 50000,
    RAND_KEYS = 4096,
    LCG_SEED = 987654321,
    DEFAULT_CAP = 16,
    DEFAULT_MAX = 13 /* entries a default-capacity index holds before growing */
};

/* The fixture "store": name of ID n is "k<n>". */
static char g_names[GROW_N][NAME_LEN];

void setUp(void) {
    for (uint32_t i = 0; i < GROW_N; i++) {
        (void)snprintf(g_names[i], NAME_LEN, "k%u", (unsigned)i);
    }
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static bool store_eq(const void *ctx, uint32_t id, odin3_bytes probe) {
    (void)ctx;
    size_t len = strlen(g_names[id]);
    return len == probe.len && memcmp(g_names[id], probe.ptr, len) == 0;
}

static odin3_bytes name_bytes(uint32_t id) {
    return odin3_bytes_cstr(g_names[id]);
}

static uint64_t real_hash(uint32_t id) {
    return odin3_hash_bytes(name_bytes(id), ODIN3_HASH_SEED);
}

/* Lookup of ID's name using an explicit hash. */
static bool find_with(const odin3_idindex *ix, uint64_t hash, uint32_t probe_id, uint32_t *out) {
    odin3_idcmp cmp = {hash, name_bytes(probe_id), store_eq, NULL};
    return odin3_idindex_find(ix, &cmp, out);
}

static void insert_ok(odin3_idindex *ix, uint64_t hash, uint32_t id) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_idindex_insert(ix, (odin3_identry){hash, id}));
}

static void expect_found(const odin3_idindex *ix, uint64_t hash, uint32_t id) {
    uint32_t got = UINT32_MAX;
    TEST_ASSERT_TRUE(find_with(ix, hash, id, &got));
    TEST_ASSERT_EQUAL_UINT32(id, got);
}

static void test_insert_find(void) {
    odin3_idindex *ix = odin3_idindex_create(0);
    TEST_ASSERT_NOT_NULL(ix);
    TEST_ASSERT_FALSE(find_with(ix, real_hash(5), 5, NULL));
    insert_ok(ix, real_hash(5), 5);
    insert_ok(ix, real_hash(6), 6);
    expect_found(ix, real_hash(5), 5);
    expect_found(ix, real_hash(6), 6);
    TEST_ASSERT_FALSE(find_with(ix, real_hash(7), 7, NULL));
    TEST_ASSERT_EQUAL_UINT(2, odin3_idindex_count(ix));
    odin3_idindex_destroy(ix);
}

static void test_id_zero_valid(void) {
    odin3_idindex *ix = odin3_idindex_create(0);
    insert_ok(ix, real_hash(0), 0);
    expect_found(ix, real_hash(0), 0);
    TEST_ASSERT_TRUE(odin3_idindex_remove(ix, (odin3_identry){real_hash(0), 0}));
    TEST_ASSERT_FALSE(find_with(ix, real_hash(0), 0, NULL));
    TEST_ASSERT_EQUAL_UINT(0, odin3_idindex_count(ix));
    odin3_idindex_destroy(ix);
}

static void test_same_hash_different_content(void) {
    const uint64_t forced = 0x1234;
    odin3_idindex *ix = odin3_idindex_create(0);
    insert_ok(ix, forced, 1);
    insert_ok(ix, forced, 2);
    expect_found(ix, forced, 1);
    expect_found(ix, forced, 2);
    TEST_ASSERT_FALSE(find_with(ix, forced, 3, NULL));
    odin3_idindex_destroy(ix);
}

static void expect_remove(odin3_idindex *ix, odin3_identry entry, bool expected) {
    TEST_ASSERT_EQUAL(expected, odin3_idindex_remove(ix, entry));
}

static void test_remove_specific_entry(void) {
    const uint64_t forced = 77;
    odin3_idindex *ix = odin3_idindex_create(0);
    insert_ok(ix, forced, 1);
    insert_ok(ix, forced, 2);
    expect_remove(ix, (odin3_identry){forced, 3}, false);
    expect_remove(ix, (odin3_identry){forced + 1, 1}, false);
    expect_remove(ix, (odin3_identry){forced, 1}, true);
    TEST_ASSERT_FALSE(find_with(ix, forced, 1, NULL));
    expect_found(ix, forced, 2);
    expect_remove(ix, (odin3_identry){forced, 1}, false);
    TEST_ASSERT_EQUAL_UINT(1, odin3_idindex_count(ix));
    odin3_idindex_destroy(ix);
}

/* Two home buckets (hash = id & 1): later arrivals with hash 0 displace earlier hash-1 runs. */
static void test_forced_displacement(void) {
    enum { N = 8 };
    odin3_idindex *ix = odin3_idindex_create(0);
    for (uint32_t id = 1; id < N; id += 2) {
        insert_ok(ix, id & 1U, id);
    }
    for (uint32_t id = 0; id < N; id += 2) {
        insert_ok(ix, id & 1U, id);
    }
    for (uint32_t id = 0; id < N; id++) {
        expect_found(ix, id & 1U, id);
    }
    for (uint32_t id = 0; id < N; id += 3) {
        TEST_ASSERT_TRUE(odin3_idindex_remove(ix, (odin3_identry){id & 1U, id}));
    }
    for (uint32_t id = 0; id < N; id++) {
        TEST_ASSERT_EQUAL(id % 3 != 0, find_with(ix, id & 1U, id, NULL));
    }
    odin3_idindex_destroy(ix);
}

/* Constant home at the last slot: the run wraps past the end of the table. */
static void test_wraparound(void) {
    const uint64_t last = DEFAULT_CAP - 1;
    enum { N = 6, MID = 2 };
    odin3_idindex *ix = odin3_idindex_create(0);
    for (uint32_t id = 0; id < N; id++) {
        insert_ok(ix, last, id);
    }
    TEST_ASSERT_TRUE(odin3_idindex_remove(ix, (odin3_identry){last, MID}));
    for (uint32_t id = 0; id < N; id++) {
        TEST_ASSERT_EQUAL(id != MID, find_with(ix, last, id, NULL));
    }
    insert_ok(ix, last, MID);
    for (uint32_t id = 0; id < N; id++) {
        expect_found(ix, last, id);
    }
    TEST_ASSERT_EQUAL_UINT(N, odin3_idindex_count(ix));
    odin3_idindex_destroy(ix);
}

static void test_growth(void) {
    odin3_idindex *ix = odin3_idindex_create(0);
    for (uint32_t id = 0; id < GROW_N; id++) {
        insert_ok(ix, real_hash(id), id);
    }
    TEST_ASSERT_EQUAL_UINT(GROW_N, odin3_idindex_count(ix));
    for (uint32_t id = 0; id < GROW_N; id++) {
        expect_found(ix, real_hash(id), id);
    }
    odin3_idindex_destroy(ix);
}

static void test_randomized_against_reference(void) {
    static bool present[RAND_KEYS];
    memset(present, 0, sizeof present);
    odin3_idindex *ix = odin3_idindex_create(0);
    uint64_t state = LCG_SEED;
    size_t expected = 0;
    for (int op = 0; op < RAND_OPS; op++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t id = (uint32_t)(state >> 33) % RAND_KEYS;
        bool coin = ((state >> 20) & 1U) != 0;
        TEST_ASSERT_EQUAL(present[id], find_with(ix, real_hash(id), id, NULL));
        if (coin && !present[id]) {
            insert_ok(ix, real_hash(id), id);
            present[id] = true;
            expected++;
        } else if (!coin && present[id]) {
            TEST_ASSERT_TRUE(odin3_idindex_remove(ix, (odin3_identry){real_hash(id), id}));
            present[id] = false;
            expected--;
        }
    }
    TEST_ASSERT_EQUAL_UINT(expected, odin3_idindex_count(ix));
    for (uint32_t id = 0; id < RAND_KEYS; id++) {
        TEST_ASSERT_EQUAL(present[id], find_with(ix, real_hash(id), id, NULL));
    }
    odin3_idindex_destroy(ix);
}

static void test_oom_insert(void) {
    odin3_idindex *ix = odin3_idindex_create(0);
    for (uint32_t id = 0; id < DEFAULT_MAX; id++) {
        insert_ok(ix, real_hash(id), id);
    }
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_NO_MEMORY,
        odin3_idindex_insert(ix, (odin3_identry){real_hash(DEFAULT_MAX), DEFAULT_MAX}));
    TEST_ASSERT_EQUAL_UINT(DEFAULT_MAX, odin3_idindex_count(ix));
    for (uint32_t id = 0; id < DEFAULT_MAX; id++) {
        expect_found(ix, real_hash(id), id);
    }
    TEST_ASSERT_FALSE(find_with(ix, real_hash(DEFAULT_MAX), DEFAULT_MAX, NULL));
    insert_ok(ix, real_hash(DEFAULT_MAX), DEFAULT_MAX); /* works once memory returns */
    expect_found(ix, real_hash(DEFAULT_MAX), DEFAULT_MAX);
    odin3_idindex_destroy(ix);
}

static void test_oom_create(void) {
    odin3_util_set_alloc_fail_after(0); /* the header allocation fails */
    TEST_ASSERT_NULL(odin3_idindex_create(0));
    odin3_util_set_alloc_fail_after(1); /* the slot array fails; the header is freed (ASan) */
    TEST_ASSERT_NULL(odin3_idindex_create(0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_insert_find);
    RUN_TEST(test_id_zero_valid);
    RUN_TEST(test_same_hash_different_content);
    RUN_TEST(test_remove_specific_entry);
    RUN_TEST(test_forced_displacement);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_growth);
    RUN_TEST(test_randomized_against_reference);
    RUN_TEST(test_oom_insert);
    RUN_TEST(test_oom_create);
    return UNITY_END();
}
