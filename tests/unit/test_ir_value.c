/*
 * test_ir_value.c — unit tests for IR typed IDs and parameter values.
 */
#include "ir/ids.h"
#include "ir/value.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"

#include <stdint.h>
#include <string.h>

enum { BITS_LEN = 4, COVER_ROW_BYTES = 3, COVER_ROWS = 2, STR_ID = 7, NEG = -42 };

static odin3_arena *arena;

void setUp(void) {
    arena = odin3_arena_create(0);
}
void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_arena_destroy(arena);
}

static const uint8_t k_bits[BITS_LEN] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_Z};
static const uint8_t k_bits_z[BITS_LEN] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_Z, ODIN3_BIT_Z};
static const uint8_t k_cover[COVER_ROWS * COVER_ROW_BYTES] = {'1', '-', '1', '0', '1', '0'};
static const uint8_t k_cover2[COVER_ROWS * COVER_ROW_BYTES] = {'1', '-', '1', '0', '0', '0'};

static odin3_value make_bits(const uint8_t *bits) {
    odin3_value val = odin3_value_int(0);
    val.kind = ODIN3_VAL_BITS;
    val.bits = bits;
    val.len = BITS_LEN;
    return val;
}

static odin3_value make_cover(const uint8_t *rows, uint32_t len) {
    odin3_value val = odin3_value_int(0);
    val.kind = ODIN3_VAL_COVER;
    val.bits = rows;
    val.len = len;
    return val;
}

static odin3_value with_inputs(odin3_value val, uint32_t inputs) {
    val.cover_inputs = inputs;
    return val;
}

static void test_ids(void) {
    odin3_node_id none = {0};
    odin3_net_id net = {3};
    TEST_ASSERT_FALSE(odin3_node_valid(none));
    TEST_ASSERT_TRUE(odin3_net_valid(net));
}

static void test_ids_global(void) {
    TEST_ASSERT_FALSE(odin3_pin_valid((odin3_pin_id){0}));
    TEST_ASSERT_TRUE(odin3_wire_valid((odin3_wire_id){1}));
}

static void test_ids_design(void) {
    TEST_ASSERT_TRUE(odin3_module_valid((odin3_module_id){1}));
}

static void test_ids_registry(void) {
    TEST_ASSERT_TRUE(odin3_celltype_valid((odin3_celltype_id){1}));
    TEST_ASSERT_TRUE(odin3_prov_valid((odin3_prov_id){1}));
    TEST_ASSERT_TRUE(odin3_passrun_valid((odin3_passrun_id){1}));
}

static void test_int_string_copy(void) {
    odin3_value src = odin3_value_int(NEG);
    odin3_value dst = odin3_value_int(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_value_copy(arena, &src, &dst));
    TEST_ASSERT_TRUE(odin3_value_equal(&src, &dst));
    TEST_ASSERT_EQUAL_INT64(NEG, dst.i);
    src.kind = ODIN3_VAL_STRING;
    src.str = STR_ID;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_value_copy(arena, &src, &dst));
    TEST_ASSERT_EQUAL_UINT32(STR_ID, dst.str);
    TEST_ASSERT_TRUE(odin3_value_equal(&src, &dst));
}

static void test_bits_copy(void) {
    odin3_value src = make_bits(k_bits);
    odin3_value dst = odin3_value_int(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_value_copy(arena, &src, &dst));
    TEST_ASSERT_TRUE(dst.bits != src.bits);
    TEST_ASSERT_EQUAL_MEMORY(k_bits, dst.bits, BITS_LEN);
    TEST_ASSERT_TRUE(odin3_value_equal(&src, &dst));
    TEST_ASSERT_TRUE(odin3_arena_bytes_used(arena) >= BITS_LEN);
}

static void test_cover_copy(void) {
    odin3_value src = with_inputs(make_cover(k_cover, sizeof k_cover), 2);
    odin3_value dst = odin3_value_int(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_value_copy(arena, &src, &dst));
    TEST_ASSERT_TRUE(dst.bits != src.bits);
    TEST_ASSERT_EQUAL_MEMORY(k_cover, dst.bits, sizeof k_cover);
    TEST_ASSERT_EQUAL_UINT32(2, dst.cover_inputs);
    TEST_ASSERT_TRUE(odin3_value_equal(&src, &dst));

    const uint8_t one[1] = {'1'};
    odin3_value zero = make_cover(one, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_value_copy(arena, &zero, &dst));
    TEST_ASSERT_EQUAL_UINT32(1, dst.len);
    TEST_ASSERT_EQUAL_UINT8('1', dst.bits[0]);
    TEST_ASSERT_TRUE(odin3_value_equal(&zero, &dst));
}

static void test_equal_distinguishes(void) {
    odin3_value lhs = make_bits(k_bits);
    odin3_value rhs = make_bits(k_bits_z);
    TEST_ASSERT_FALSE(odin3_value_equal(&lhs, &rhs)); /* X vs Z */
    rhs = make_bits(k_bits);
    rhs.len = BITS_LEN - 1;
    TEST_ASSERT_FALSE(odin3_value_equal(&lhs, &rhs)); /* length */
}

static void test_equal_cover(void) {
    odin3_value c1 = with_inputs(make_cover(k_cover, sizeof k_cover), 2);
    odin3_value c2 = with_inputs(make_cover(k_cover2, sizeof k_cover2), 2);
    TEST_ASSERT_FALSE(odin3_value_equal(&c1, &c2)); /* row difference */
    c2 = with_inputs(make_cover(k_cover, sizeof k_cover), 5);
    TEST_ASSERT_FALSE(odin3_value_equal(&c1, &c2)); /* inputs per row */
}

static void test_equal_kinds_and_ints(void) {
    odin3_value i0 = odin3_value_int(0);
    odin3_value s0 = i0;
    s0.kind = ODIN3_VAL_STRING;
    TEST_ASSERT_FALSE(odin3_value_equal(&i0, &s0)); /* kind */
}

static void test_equal_ints(void) {
    odin3_value i0 = odin3_value_int(0);
    odin3_value i1 = odin3_value_int(1);
    odin3_value lhs = make_bits(k_bits);
    TEST_ASSERT_FALSE(odin3_value_equal(&i0, &lhs));
    TEST_ASSERT_TRUE(odin3_value_equal(&i0, &i0));
    TEST_ASSERT_FALSE(odin3_value_equal(&i0, &i1));
}

static void test_copy_oom(void) {
    odin3_arena *tiny = odin3_arena_create(64);
    TEST_ASSERT_NOT_NULL(tiny);
    odin3_value src = make_bits(k_bits);
    odin3_value dst = odin3_value_int(NEG);
    odin3_util_set_alloc_fail_after(0);
    /* Exhaust the first 64-byte chunk so the next request needs a fresh (failing) chunk. */
    (void)odin3_arena_alloc(tiny, 64);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_value_copy(tiny, &src, &dst));
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, dst.kind); /* unchanged on failure */
    TEST_ASSERT_EQUAL_INT64(NEG, dst.i);
    odin3_arena_destroy(tiny);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ids);
    RUN_TEST(test_ids_global);
    RUN_TEST(test_ids_design);
    RUN_TEST(test_ids_registry);
    RUN_TEST(test_int_string_copy);
    RUN_TEST(test_bits_copy);
    RUN_TEST(test_cover_copy);
    RUN_TEST(test_equal_distinguishes);
    RUN_TEST(test_equal_cover);
    RUN_TEST(test_equal_kinds_and_ints);
    RUN_TEST(test_equal_ints);
    RUN_TEST(test_copy_oom);
    return UNITY_END();
}
