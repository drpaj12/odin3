/*
 * test_util_u64map.c — unit tests for the Robin Hood integer map.
 */
#include "unity.h"
#include "util/alloc.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    COLLIDE_N = 1000,
    GROW_N = 100000,
    RAND_OPS = 100000,
    RAND_KEYS = 4096,
    LCG_SEED = 12345,
    OOM_FILL = 200,
    DISP_N = 60,
    WRAP_N = 6,
    WRAP_HOME = 15,
    LARGE_CAP = 100
};

void setUp(void) {
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_u64map_test_set_hash(NULL);
}

static uint64_t hash_const(uint64_t key) {
    (void)key;
    return 42;
}

static void put_ok(odin3_u64map *map, uint64_t key, uint64_t value) {
    odin3_kv entry = {key, value};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_u64map_put(map, entry));
}

static void expect_entry(const odin3_u64map *map, odin3_kv expected) {
    uint64_t val = 0;
    TEST_ASSERT_TRUE(odin3_u64map_get(map, expected.key, &val));
    TEST_ASSERT_EQUAL_UINT64(expected.value, val);
}

static void test_put_get_overwrite(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    TEST_ASSERT_NOT_NULL(map);
    TEST_ASSERT_FALSE(odin3_u64map_get(map, 5, NULL));
    put_ok(map, 5, 50);
    expect_entry(map, (odin3_kv){5, 50});
    put_ok(map, 5, 51);
    expect_entry(map, (odin3_kv){5, 51});
    TEST_ASSERT_EQUAL_UINT(1, odin3_u64map_count(map));
    odin3_u64map_destroy(map);
}

static void test_key_zero_valid(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    put_ok(map, 0, 7);
    expect_entry(map, (odin3_kv){0, 7});
    TEST_ASSERT_EQUAL_UINT(1, odin3_u64map_count(map));
    odin3_u64map_destroy(map);
}

static void test_remove_then_reinsert(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    put_ok(map, 9, 1);
    TEST_ASSERT_TRUE(odin3_u64map_remove(map, 9));
    TEST_ASSERT_FALSE(odin3_u64map_remove(map, 9));
    TEST_ASSERT_FALSE(odin3_u64map_get(map, 9, NULL));
    TEST_ASSERT_EQUAL_UINT(0, odin3_u64map_count(map));
    put_ok(map, 9, 2);
    expect_entry(map, (odin3_kv){9, 2});
    odin3_u64map_destroy(map);
}

static void test_collisions(void) {
    odin3_u64map_test_set_hash(hash_const);
    odin3_u64map *map = odin3_u64map_create(0);
    for (uint64_t i = 0; i < COLLIDE_N; i++) {
        put_ok(map, i, i * 3);
    }
    for (uint64_t i = 0; i < COLLIDE_N; i += 2) {
        TEST_ASSERT_TRUE(odin3_u64map_remove(map, i));
    }
    TEST_ASSERT_EQUAL_UINT(COLLIDE_N / 2, odin3_u64map_count(map));
    for (uint64_t i = 0; i < COLLIDE_N; i++) {
        uint64_t val = 0;
        bool found = odin3_u64map_get(map, i, &val);
        TEST_ASSERT_EQUAL(i % 2 == 1, found);
        if (found) {
            TEST_ASSERT_EQUAL_UINT64(i * 3, val);
        }
    }
    odin3_u64map_destroy(map);
}

static uint64_t hash_two_buckets(uint64_t key) {
    return key & 1;
}

static uint64_t hash_wrap(uint64_t key) {
    (void)key;
    return WRAP_HOME;
}

static void test_displacement_two_buckets(void) {
    odin3_u64map_test_set_hash(hash_two_buckets);
    odin3_u64map *map = odin3_u64map_create(0);
    for (uint64_t i = 0; i < DISP_N; i++) {
        put_ok(map, i, i + 100);
    }
    for (uint64_t i = 0; i < DISP_N; i++) {
        expect_entry(map, (odin3_kv){i, i + 100});
    }
    for (uint64_t i = 0; i < DISP_N; i += 3) {
        TEST_ASSERT_TRUE(odin3_u64map_remove(map, i));
    }
    for (uint64_t i = 0; i < DISP_N; i++) {
        TEST_ASSERT_EQUAL(i % 3 != 0, odin3_u64map_get(map, i, NULL));
        if (i % 3 != 0) {
            expect_entry(map, (odin3_kv){i, i + 100});
        }
    }
    odin3_u64map_destroy(map);
}

static void test_wraparound(void) {
    odin3_u64map_test_set_hash(hash_wrap); /* home = last slot of the default 16-slot table */
    odin3_u64map *map = odin3_u64map_create(0);
    for (uint64_t i = 0; i < WRAP_N; i++) {
        put_ok(map, i, i + 1);
    }
    TEST_ASSERT_TRUE(odin3_u64map_remove(map, 0));
    put_ok(map, 0, 1);
    for (uint64_t i = 0; i < WRAP_N; i++) {
        expect_entry(map, (odin3_kv){i, i + 1});
    }
    odin3_u64map_destroy(map);
}

static void test_create_large_initial_cap(void) {
    odin3_u64map *map = odin3_u64map_create(LARGE_CAP);
    TEST_ASSERT_NOT_NULL(map);
    for (uint64_t i = 0; i < LARGE_CAP; i++) {
        put_ok(map, i, i);
    }
    TEST_ASSERT_EQUAL_UINT(LARGE_CAP, odin3_u64map_count(map));
    for (uint64_t i = 0; i < LARGE_CAP; i++) {
        expect_entry(map, (odin3_kv){i, i});
    }
    odin3_u64map_destroy(map);
}

static void test_growth_keeps_entries(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    for (uint64_t i = 0; i < GROW_N; i++) {
        put_ok(map, i, i + 1);
    }
    TEST_ASSERT_EQUAL_UINT(GROW_N, odin3_u64map_count(map));
    for (uint64_t i = 0; i < GROW_N; i++) {
        expect_entry(map, (odin3_kv){i, i + 1});
    }
    odin3_u64map_destroy(map);
}

static void test_iteration_visits_each_once(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    for (uint64_t i = 0; i < COLLIDE_N; i++) {
        put_ok(map, i, i);
    }
    static unsigned char seen[COLLIDE_N];
    size_t cursor = 0;
    size_t visits = 0;
    odin3_kv entry;
    while (odin3_u64map_next(map, &cursor, &entry)) {
        TEST_ASSERT_TRUE(entry.key < COLLIDE_N);
        TEST_ASSERT_EQUAL_UINT64(entry.key, entry.value);
        TEST_ASSERT_EQUAL_UINT8(0, seen[entry.key]);
        seen[entry.key] = 1;
        visits++;
    }
    TEST_ASSERT_EQUAL_UINT(COLLIDE_N, visits);
    TEST_ASSERT_FALSE(odin3_u64map_next(map, &cursor, &entry));
    odin3_u64map_destroy(map);
}

static void test_empty_iteration(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    size_t cursor = 0;
    odin3_kv entry;
    TEST_ASSERT_FALSE(odin3_u64map_next(map, &cursor, &entry));
    odin3_u64map_destroy(map);
}

static void test_randomized_against_reference(void) {
    static bool present[RAND_KEYS];
    static uint64_t ref[RAND_KEYS];
    odin3_u64map *map = odin3_u64map_create(0);
    uint64_t state = LCG_SEED;
    size_t expect_count = 0;
    for (int op = 0; op < RAND_OPS; op++) {
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        uint64_t key = (state >> 33) % RAND_KEYS;
        uint64_t kind = (state >> 20) % 3;
        uint64_t val = 0;
        if (kind == 0) {
            put_ok(map, key, state);
            expect_count += present[key] ? 0 : 1;
            present[key] = true;
            ref[key] = state;
        } else if (kind == 1) {
            bool found = odin3_u64map_get(map, key, &val);
            TEST_ASSERT_EQUAL(present[key], found);
            if (found) {
                TEST_ASSERT_EQUAL_UINT64(ref[key], val);
            }
        } else {
            TEST_ASSERT_EQUAL(present[key], odin3_u64map_remove(map, key));
            expect_count -= present[key] ? 1 : 0;
            present[key] = false;
        }
        TEST_ASSERT_EQUAL_UINT(expect_count, odin3_u64map_count(map));
    }
    odin3_u64map_destroy(map);
}

static void test_oom_put(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    uint64_t next = 0;
    /* Fill to just under the growth threshold of the default 16 slots, then force growth. */
    while (next < OOM_FILL) {
        put_ok(map, next, next);
        next++;
    }
    /* Keep inserting with an injected failure until one put hits growth. */
    odin3_status status = ODIN3_OK;
    while (status == ODIN3_OK) {
        odin3_util_set_alloc_fail_after(0);
        odin3_kv entry = {next, next};
        status = odin3_u64map_put(map, entry);
        if (status == ODIN3_OK) {
            next++;
        }
    }
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, status);
    TEST_ASSERT_EQUAL_UINT(next, odin3_u64map_count(map));
    for (uint64_t i = 0; i < next; i++) {
        expect_entry(map, (odin3_kv){i, i});
    }
    odin3_u64map_destroy(map);
}

static void test_mod_counter_changes_on_put_and_remove(void) {
    odin3_u64map *map = odin3_u64map_create(0);
    uint64_t before = odin3_u64map_test_modcount(map);
    put_ok(map, 1, 1);
    uint64_t after_put = odin3_u64map_test_modcount(map);
    TEST_ASSERT_NOT_EQUAL(before, after_put);
    TEST_ASSERT_TRUE(odin3_u64map_remove(map, 1));
    TEST_ASSERT_NOT_EQUAL(after_put, odin3_u64map_test_modcount(map));
    odin3_u64map_destroy(map);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_put_get_overwrite);
    RUN_TEST(test_key_zero_valid);
    RUN_TEST(test_remove_then_reinsert);
    RUN_TEST(test_collisions);
    RUN_TEST(test_displacement_two_buckets);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_create_large_initial_cap);
    RUN_TEST(test_growth_keeps_entries);
    RUN_TEST(test_iteration_visits_each_once);
    RUN_TEST(test_empty_iteration);
    RUN_TEST(test_randomized_against_reference);
    RUN_TEST(test_oom_put);
    RUN_TEST(test_mod_counter_changes_on_put_and_remove);
    return UNITY_END();
}
