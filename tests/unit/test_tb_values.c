/*
 * test_tb_values.c — the testbench interpreter's 4-state values (sim/tb_val.c, sim/tb_ops.c):
 * based digits with x, z and ?, bit fields across limbs, width changes with sign and x fill, the
 * wide division and power, and the ?: merge that keeps z where both sides are z.
 */
#include "sim/tb_internal.h"
#include "unity.h"

#include <string.h>

void setUp(void) {
}
void tearDown(void) {
}

static bool parse(const char *text, unsigned base, tbv *out, uint32_t *bits) {
    tb_digits digits = {text, strlen(text), base};
    return tb_parse_digits(&digits, out, bits);
}

static void test_hex_digits_with_x_and_z(void) {
    tbv val;
    uint32_t bits = 0;
    TEST_ASSERT_TRUE(parse("x_5z", 16, &val, &bits));
    TEST_ASSERT_EQUAL_UINT32(12, bits);
    TEST_ASSERT_EQUAL_HEX64(0x05FU, val.v[0]); /* z bits have v = 1 */
    TEST_ASSERT_EQUAL_HEX64(0xF0FU, val.u[0]);
    TEST_ASSERT_FALSE(parse("5g", 16, &val, &bits));
    TEST_ASSERT_FALSE(parse("12", 2, &val, &bits));
}

static void test_decimal_digits(void) {
    tbv val;
    uint32_t bits = 0;
    /* 2^64 + 1 = 18446744073709551617: crosses into the second limb */
    TEST_ASSERT_TRUE(parse("18446744073709551617", 10, &val, &bits));
    TEST_ASSERT_EQUAL_HEX64(1U, val.v[0]);
    TEST_ASSERT_EQUAL_HEX64(1U, val.v[1]);
    TEST_ASSERT_TRUE(parse("x", 10, &val, &bits));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, tbv_code(&val, TB_MAX_WIDTH - 1U));
    TEST_ASSERT_FALSE(parse("1x", 10, &val, &bits));
}

static void test_field_across_limbs(void) {
    tbv dst;
    tbv src;
    tbv got;
    tbv_zero(&dst, 130);
    tbv_fill(ODIN3_W4_Z, &src, 10);
    tb_field_put((tb_planes){dst.v, dst.u, 130}, (tb_field){60, 10}, &src);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_0, tbv_code(&dst, 59));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, tbv_code(&dst, 60));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, tbv_code(&dst, 69));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_0, tbv_code(&dst, 70));
    tb_field_get(&got, (tb_cplanes){dst.v, dst.u, 130}, (tb_field){58, 14});
    TEST_ASSERT_EQUAL_HEX64(0xFFCU, got.v[0]);
    TEST_ASSERT_EQUAL_HEX64(0xFFCU, got.u[0]);
}

static void test_extend_sign_and_xfill(void) {
    tbv val;
    tbv_set_u64(0x8U, &val, 4);
    tbv_extend(&val, &(tb_resize){4, 70, true, false});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_1, tbv_code(&val, 69));
    tbv_zero(&val, 32);
    tbv_fill(ODIN3_W4_X, &val, 32);
    tbv_extend(&val, &(tb_resize){32, 64, false, true});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, tbv_code(&val, 63));
    tbv_fill(ODIN3_W4_X, &val, 32);
    tbv_extend(&val, &(tb_resize){32, 64, false, false});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_0, tbv_code(&val, 63));
}

static void test_wide_divide(void) {
    tbv num;
    tbv den;
    /* (2^69 + 1) / 3 = 196765270119568550571, remainder 0 */
    tbv_set_u64(1, &num, 70);
    odin3_word_set_bit(num.v, 69);
    tbv_set_u64(3, &den, 70);
    tbv quo = num;
    tb_binop div = {&quo, &den, 70, false};
    tb_op_arith("/", &div);
    TEST_ASSERT_EQUAL_HEX64(0xAAAAAAAAAAAAAAABU, quo.v[0]);
    TEST_ASSERT_EQUAL_HEX64(0xAU, quo.v[1]);
    tbv rem = num;
    tb_binop mod = {&rem, &den, 70, false};
    tb_op_arith("%", &mod);
    TEST_ASSERT_EQUAL_HEX64(0U, rem.v[0]);
    tbv zero;
    tbv_zero(&zero, 70);
    tb_binop by_zero = {&num, &zero, 70, false};
    tb_op_arith("/", &by_zero);
    TEST_ASSERT_FALSE(tbv_known(&num, 70));
}

static void test_power(void) {
    tbv base;
    tbv exp;
    tbv_set_u64(3, &base, 8);
    tbv_set_u64(5, &exp, 4);
    tb_powop pow = {&base, &exp, 8, 4, false, false};
    tb_op_pow(&pow);
    TEST_ASSERT_EQUAL_HEX64(243U, base.v[0]);
    tbv_set_u64(0, &base, 8);
    tbv_set_u64(0xFU, &exp, 4); /* -1 as a signed exponent: 0 ** -1 is x */
    tb_powop neg = {&base, &exp, 8, 4, false, true};
    tb_op_pow(&neg);
    TEST_ASSERT_FALSE(tbv_known(&base, 8));
}

static void test_merge_keeps_equal_codes(void) {
    tbv lhs;
    tbv rhs;
    tbv_fill(ODIN3_W4_Z, &lhs, 4);
    tbv_fill(ODIN3_W4_Z, &rhs, 4);
    rhs.v[0] &= ~1ULL;
    rhs.u[0] &= ~1ULL; /* bit 0 of rhs is 0 */
    tb_op_merge(&lhs, &rhs, 4);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, tbv_code(&lhs, 0));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, tbv_code(&lhs, 3));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hex_digits_with_x_and_z);
    RUN_TEST(test_decimal_digits);
    RUN_TEST(test_field_across_limbs);
    RUN_TEST(test_extend_sign_and_xfill);
    RUN_TEST(test_wide_divide);
    RUN_TEST(test_power);
    RUN_TEST(test_merge_keeps_equal_codes);
    return UNITY_END();
}
