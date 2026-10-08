/*
 * test_util_hash.c — unit tests for util hashing against fixed vectors.
 */
#include "unity.h"
#include "util/hash.h"

#include <stdint.h>

void setUp(void) {
}
void tearDown(void) {
}

static void test_bytes_vectors_with_fixed_seed(void) {
    TEST_ASSERT_EQUAL_HEX64(0xdf2d2a779b7e2325ULL,
                            odin3_hash_bytes(odin3_bytes_cstr(""), ODIN3_HASH_SEED));
    TEST_ASSERT_EQUAL_HEX64(0xcecb8d4d295357faULL,
                            odin3_hash_bytes(odin3_bytes_cstr("a"), ODIN3_HASH_SEED));
    TEST_ASSERT_EQUAL_HEX64(0x952238a8a1375f22ULL,
                            odin3_hash_bytes(odin3_bytes_cstr("odin3"), ODIN3_HASH_SEED));
}

static void test_bytes_vector_with_zero_seed(void) {
    TEST_ASSERT_EQUAL_HEX64(0xe77a0e3cbf70455eULL, odin3_hash_bytes(odin3_bytes_cstr("odin3"), 0));
}

static void test_u64_vectors(void) {
    TEST_ASSERT_EQUAL_HEX64(0xb456bcfc34c2cb2cULL, odin3_hash_u64(1));
    TEST_ASSERT_EQUAL_HEX64(0, odin3_hash_u64(0));
}

static void test_combine_order_sensitive_and_defined(void) {
    const uint64_t first = 0x1234;
    const uint64_t second = 0x9abc;
    TEST_ASSERT_EQUAL_HEX64(odin3_hash_u64(first ^ second), odin3_hash_combine(first, second));
    TEST_ASSERT_NOT_EQUAL(odin3_hash_combine(odin3_hash_combine(0, first), second),
                          odin3_hash_combine(odin3_hash_combine(0, second), first));
}

static void test_null_empty_equals_empty_string(void) {
    const odin3_bytes none = {NULL, 0};
    TEST_ASSERT_EQUAL_HEX64(odin3_hash_bytes(odin3_bytes_cstr(""), ODIN3_HASH_SEED),
                            odin3_hash_bytes(none, ODIN3_HASH_SEED));
    TEST_ASSERT_EQUAL_HEX64(odin3_hash_bytes(odin3_bytes_cstr(""), 7), odin3_hash_bytes(none, 7));
}

static void test_cstr_null_is_empty(void) {
    const odin3_bytes none = odin3_bytes_cstr(NULL);
    TEST_ASSERT_NULL(none.ptr);
    TEST_ASSERT_EQUAL_UINT(0, none.len);
    TEST_ASSERT_EQUAL_UINT(5, odin3_bytes_cstr("odin3").len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bytes_vectors_with_fixed_seed);
    RUN_TEST(test_bytes_vector_with_zero_seed);
    RUN_TEST(test_u64_vectors);
    RUN_TEST(test_combine_order_sensitive_and_defined);
    RUN_TEST(test_null_empty_equals_empty_string);
    RUN_TEST(test_cstr_null_is_empty);
    return UNITY_END();
}
