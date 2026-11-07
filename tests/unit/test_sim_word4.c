/*
 * test_sim_word4.c — the 4-state word kernels (sim/word4.h) against a per-bit reference model on
 * the codes 0, 1, x, z: every code pair for the bitwise operations, net resolution, merge and
 * ==, and multi-limb words (70 and 130 bits) for the arithmetic, shifts, comparisons and
 * extension.
 */
#include "sim/word4.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { LIMBS = 3, WIDE = 130 };

void setUp(void) {
}
void tearDown(void) {
}

typedef struct word {
    uint64_t v[LIMBS];
    uint64_t u[LIMBS];
} word;

static odin3_w4 planes(word *wd) {
    return (odin3_w4){wd->v, wd->u};
}
static odin3_w4c cplanes(const word *wd) {
    return (odin3_w4c){wd->v, wd->u};
}

static word make1(uint32_t code) {
    word wd;
    memset(&wd, 0, sizeof wd);
    odin3_w4_set_bit(planes(&wd), (odin3_w4_bit_at){0, code});
    return wd;
}

static uint32_t bit0(const word *wd) {
    return odin3_w4_bit(cplanes(wd), 0);
}

static bool is_unk(uint32_t code) {
    return (code & 2U) != 0;
}

/* reference per bit: 0 beats x for and, 1 beats x for or */
static uint32_t ref_and(uint32_t lhs, uint32_t rhs) {
    if ((!is_unk(lhs) && lhs == 0) || (!is_unk(rhs) && rhs == 0)) {
        return 0;
    }
    return is_unk(lhs) || is_unk(rhs) ? ODIN3_W4_X : 1U;
}
static uint32_t ref_or(uint32_t lhs, uint32_t rhs) {
    if ((!is_unk(lhs) && lhs == 1) || (!is_unk(rhs) && rhs == 1)) {
        return 1;
    }
    return is_unk(lhs) || is_unk(rhs) ? ODIN3_W4_X : 0U;
}
static uint32_t ref_xor(uint32_t lhs, uint32_t rhs) {
    return is_unk(lhs) || is_unk(rhs) ? ODIN3_W4_X : (lhs ^ rhs);
}
static uint32_t ref_resolve(uint32_t lhs, uint32_t rhs) {
    if (lhs == ODIN3_W4_Z) {
        return rhs;
    }
    if (rhs == ODIN3_W4_Z) {
        return lhs;
    }
    return (lhs == rhs && !is_unk(lhs)) ? lhs : ODIN3_W4_X;
}
static uint32_t ref_merge(uint32_t lhs, uint32_t rhs) {
    return (lhs == rhs && !is_unk(lhs)) ? lhs : ODIN3_W4_X;
}

static void binop(void (*fn)(const odin3_w4_bin *), uint32_t lhs, uint32_t rhs, word *out) {
    word wa = make1(lhs);
    word wb = make1(rhs);
    memset(out, 0, sizeof *out);
    odin3_w4_bin op = {planes(out), cplanes(&wa), cplanes(&wb), 1};
    fn(&op);
}

static uint32_t ref_flag(bool unknown_in, uint32_t known_code) {
    if (unknown_in) {
        return ODIN3_W4_X;
    }
    return known_code;
}

static void check_bitwise(uint32_t lhs, uint32_t rhs) {
    word out;
    binop(odin3_w4_and, lhs, rhs, &out);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_and(lhs, rhs), bit0(&out), "and");
    binop(odin3_w4_or, lhs, rhs, &out);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_or(lhs, rhs), bit0(&out), "or");
    binop(odin3_w4_xor, lhs, rhs, &out);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_xor(lhs, rhs), bit0(&out), "xor");
    binop(odin3_w4_resolve, lhs, rhs, &out);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_resolve(lhs, rhs), bit0(&out), "resolve");
    binop(odin3_w4_merge, lhs, rhs, &out);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_merge(lhs, rhs), bit0(&out), "merge");
    binop(odin3_w4_not, lhs, rhs, &out);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_flag(is_unk(lhs), lhs ^ 1U), bit0(&out), "not");
}

static void check_compare(uint32_t lhs, uint32_t rhs) {
    word out;
    word wa = make1(lhs);
    word wb = make1(rhs);
    odin3_w4_bin op = {planes(&out), cplanes(&wa), cplanes(&wb), 1};
    bool any_unknown = is_unk(lhs) || is_unk(rhs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_flag(any_unknown, lhs == rhs ? 1U : 0U), odin3_w4_eq(&op),
                                     "eq");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(lhs == rhs ? 1U : 0U, odin3_w4_case_eq(&op), "case_eq");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_flag(any_unknown, lhs < rhs ? 1U : 0U),
                                     odin3_w4_lt(&op, false), "lt");
}

static void test_single_bit_tables(void) {
    for (uint32_t lhs = 0; lhs < 4; lhs++) {
        for (uint32_t rhs = 0; rhs < 4; rhs++) {
            check_bitwise(lhs, rhs);
            check_compare(lhs, rhs);
        }
    }
}

/* a width-N word from a 64-bit value in v and an unknown mask (x bits) in the low limb */
static word make(uint64_t val, uint64_t xmask) {
    word wd;
    memset(&wd, 0, sizeof wd);
    wd.v[0] = val & ~xmask;
    wd.u[0] = xmask;
    return wd;
}

static void test_eq_known_mismatch_wins(void) {
    word lhs = make(0x1, 0x2); /* 0b?1 */
    word rhs = make(0x0, 0x0); /* 0b00 */
    word out;
    odin3_w4_bin op = {planes(&out), cplanes(&lhs), cplanes(&rhs), 2};
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_0, odin3_w4_eq(&op));
    word third = make(0x1, 0x2); /* 0b?1 against 0b01 */
    word fourth = make(0x1, 0x0);
    op = (odin3_w4_bin){planes(&out), cplanes(&third), cplanes(&fourth), 2};
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_eq(&op));
}

static void test_arith_known_and_unknown(void) {
    word lhs;
    word rhs;
    word out;
    memset(&lhs, 0, sizeof lhs);
    memset(&rhs, 0, sizeof rhs);
    lhs.v[0] = UINT64_MAX;
    lhs.v[1] = 1; /* 65-bit all ones in low 65 bits */
    rhs.v[0] = 1;
    odin3_w4_bin op = {planes(&out), cplanes(&lhs), cplanes(&rhs), 70};
    odin3_w4_add(&op);
    TEST_ASSERT_EQUAL_UINT64(0, out.v[0]);
    TEST_ASSERT_EQUAL_UINT64(2, out.v[1]);
    TEST_ASSERT_TRUE(odin3_w4_known(out.u, 70));
    lhs.u[1] = 4; /* one x bit anywhere: all x */
    odin3_w4_add(&op);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, out.u[0]);
    TEST_ASSERT_EQUAL_UINT64(0x3f, out.u[1]);
    TEST_ASSERT_EQUAL_UINT64(0, out.v[0]);
}

static void test_shifts_move_state(void) {
    word lhs = make(0x1, 0x2); /* bits: 0=1, 1=x */
    word amt = make(1, 0);
    word out;
    memset(&out, 0, sizeof out);
    odin3_w4_shift sh = {planes(&out), cplanes(&lhs), cplanes(&amt), 70, 8};
    odin3_w4_shl(&sh);
    TEST_ASSERT_EQUAL_UINT64(0x2, out.v[0]);
    TEST_ASSERT_EQUAL_UINT64(0x4, out.u[0]);
    amt.u[0] = 1; /* unknown amount */
    odin3_w4_shl(&sh);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, out.u[0]);
}

static void test_sshr_fills_with_sign_code(void) {
    word lhs;
    memset(&lhs, 0, sizeof lhs);
    odin3_w4_set_bit(planes(&lhs), (odin3_w4_bit_at){3, ODIN3_W4_X}); /* 4-bit 0b?000 */
    word amt = make(2, 0);
    word out;
    memset(&out, 0, sizeof out);
    odin3_w4_shift sh = {planes(&out), cplanes(&lhs), cplanes(&amt), 4, 8};
    odin3_w4_sshr(&sh);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_bit(cplanes(&out), 3));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_bit(cplanes(&out), 2));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_bit(cplanes(&out), 1));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_bit(cplanes(&out), 0));
}

static void test_reductions_and_truth(void) {
    word lhs = make(0x7, 0x8); /* 0b?111 */
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_reduce_and(cplanes(&lhs), 4));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_w4_reduce_or(cplanes(&lhs), 4));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_reduce_xor(cplanes(&lhs), 4));
    word rhs = make(0x6, 0x8); /* 0b?110 */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_reduce_and(cplanes(&rhs), 3));
    word third = make(0x0, 0x1); /* 0b000? */
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_truth(cplanes(&third), 4));
    word fourth = make(0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_truth(cplanes(&fourth), 4));
}

static void test_extend_copies_unknown_sign(void) {
    word lhs;
    memset(&lhs, 0, sizeof lhs);
    odin3_w4_set_bit(planes(&lhs), (odin3_w4_bit_at){3, ODIN3_W4_Z});
    odin3_w4_extend(planes(&lhs), &(odin3_word_resize){4, WIDE, true});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, odin3_w4_bit(cplanes(&lhs), 100));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, odin3_w4_bit(cplanes(&lhs), WIDE - 1));
    word rhs;
    memset(&rhs, 0, sizeof rhs);
    odin3_w4_set_bit(planes(&rhs), (odin3_w4_bit_at){3, ODIN3_W4_X});
    odin3_w4_extend(planes(&rhs), &(odin3_word_resize){4, 70, false});
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_bit(cplanes(&rhs), 69));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_bit_tables);
    RUN_TEST(test_eq_known_mismatch_wins);
    RUN_TEST(test_arith_known_and_unknown);
    RUN_TEST(test_shifts_move_state);
    RUN_TEST(test_sshr_fills_with_sign_code);
    RUN_TEST(test_reductions_and_truth);
    RUN_TEST(test_extend_copies_unknown_sign);
    return UNITY_END();
}
