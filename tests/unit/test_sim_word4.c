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

static odin3_w4 planes(word *w) {
    return (odin3_w4){w->v, w->u};
}
static odin3_w4c cplanes(const word *w) {
    return (odin3_w4c){w->v, w->u};
}

static word make1(uint32_t code) {
    word w;
    memset(&w, 0, sizeof w);
    odin3_w4_set_bit(planes(&w), 0, code);
    return w;
}

static uint32_t bit0(const word *w) {
    return odin3_w4_bit(cplanes(w), 0);
}

static bool is_unk(uint32_t c) {
    return (c & 2U) != 0;
}

/* reference per bit: 0 beats x for and, 1 beats x for or */
static uint32_t ref_and(uint32_t a, uint32_t b) {
    if ((!is_unk(a) && a == 0) || (!is_unk(b) && b == 0)) {
        return 0;
    }
    return is_unk(a) || is_unk(b) ? ODIN3_W4_X : 1U;
}
static uint32_t ref_or(uint32_t a, uint32_t b) {
    if ((!is_unk(a) && a == 1) || (!is_unk(b) && b == 1)) {
        return 1;
    }
    return is_unk(a) || is_unk(b) ? ODIN3_W4_X : 0U;
}
static uint32_t ref_xor(uint32_t a, uint32_t b) {
    return is_unk(a) || is_unk(b) ? ODIN3_W4_X : (a ^ b);
}
static uint32_t ref_resolve(uint32_t a, uint32_t b) {
    if (a == ODIN3_W4_Z) {
        return b;
    }
    if (b == ODIN3_W4_Z) {
        return a;
    }
    return (a == b && !is_unk(a)) ? a : ODIN3_W4_X;
}
static uint32_t ref_merge(uint32_t a, uint32_t b) {
    return (a == b && !is_unk(a)) ? a : ODIN3_W4_X;
}

static void binop(void (*fn)(const odin3_w4_bin *), uint32_t a, uint32_t b, word *out) {
    word wa = make1(a);
    word wb = make1(b);
    memset(out, 0, sizeof *out);
    odin3_w4_bin op = {planes(out), cplanes(&wa), cplanes(&wb), 1};
    fn(&op);
}

static void test_single_bit_tables(void) {
    for (uint32_t a = 0; a < 4; a++) {
        for (uint32_t b = 0; b < 4; b++) {
            word out;
            binop(odin3_w4_and, a, b, &out);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_and(a, b), bit0(&out), "and");
            binop(odin3_w4_or, a, b, &out);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_or(a, b), bit0(&out), "or");
            binop(odin3_w4_xor, a, b, &out);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_xor(a, b), bit0(&out), "xor");
            binop(odin3_w4_resolve, a, b, &out);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_resolve(a, b), bit0(&out), "resolve");
            binop(odin3_w4_merge, a, b, &out);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref_merge(a, b), bit0(&out), "merge");
            binop(odin3_w4_not, a, b, &out);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(is_unk(a) ? ODIN3_W4_X : (a ^ 1U), bit0(&out), "not");
            word wa = make1(a);
            word wb = make1(b);
            odin3_w4_bin op = {planes(&out), cplanes(&wa), cplanes(&wb), 1};
            uint32_t want_eq = is_unk(a) || is_unk(b) ? ODIN3_W4_X : (a == b ? 1U : 0U);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_eq, odin3_w4_eq(&op), "eq");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(a == b ? 1U : 0U, odin3_w4_case_eq(&op), "case_eq");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(is_unk(a) || is_unk(b) ? ODIN3_W4_X
                                                                    : (a < b ? 1U : 0U),
                                             odin3_w4_lt(&op, false), "lt");
        }
    }
}

/* a width-N word from a 64-bit value in v and an unknown mask (x bits) in the low limb */
static word make(uint64_t v, uint64_t xmask) {
    word w;
    memset(&w, 0, sizeof w);
    w.v[0] = v & ~xmask;
    w.u[0] = xmask;
    return w;
}

static void test_eq_known_mismatch_wins(void) {
    word a = make(0x1, 0x2); /* 0b?1 */
    word b = make(0x0, 0x0); /* 0b00 */
    word out;
    odin3_w4_bin op = {planes(&out), cplanes(&a), cplanes(&b), 2};
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_0, odin3_w4_eq(&op));
    word c = make(0x1, 0x2); /* 0b?1 against 0b01 */
    word d = make(0x1, 0x0);
    op = (odin3_w4_bin){planes(&out), cplanes(&c), cplanes(&d), 2};
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_eq(&op));
}

static void test_arith_known_and_unknown(void) {
    word a;
    word b;
    word out;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.v[0] = UINT64_MAX;
    a.v[1] = 1; /* 65-bit all ones in low 65 bits */
    b.v[0] = 1;
    odin3_w4_bin op = {planes(&out), cplanes(&a), cplanes(&b), 70};
    odin3_w4_add(&op);
    TEST_ASSERT_EQUAL_UINT64(0, out.v[0]);
    TEST_ASSERT_EQUAL_UINT64(2, out.v[1]);
    TEST_ASSERT_TRUE(odin3_w4_known(out.u, 70));
    a.u[1] = 4; /* one x bit anywhere: all x */
    odin3_w4_add(&op);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, out.u[0]);
    TEST_ASSERT_EQUAL_UINT64(0x3f, out.u[1]);
    TEST_ASSERT_EQUAL_UINT64(0, out.v[0]);
}

static void test_shifts_move_state(void) {
    word a = make(0x1, 0x2); /* bits: 0=1, 1=x */
    word amt = make(1, 0);
    word out;
    memset(&out, 0, sizeof out);
    odin3_w4_shift sh = {planes(&out), cplanes(&a), cplanes(&amt), 70, 8};
    odin3_w4_shl(&sh);
    TEST_ASSERT_EQUAL_UINT64(0x2, out.v[0]);
    TEST_ASSERT_EQUAL_UINT64(0x4, out.u[0]);
    amt.u[0] = 1; /* unknown amount */
    odin3_w4_shl(&sh);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, out.u[0]);
}

static void test_sshr_fills_with_sign_code(void) {
    word a;
    memset(&a, 0, sizeof a);
    odin3_w4_set_bit(planes(&a), 3, ODIN3_W4_X); /* 4-bit 0b?000 */
    word amt = make(2, 0);
    word out;
    memset(&out, 0, sizeof out);
    odin3_w4_shift sh = {planes(&out), cplanes(&a), cplanes(&amt), 4, 8};
    odin3_w4_sshr(&sh);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_bit(cplanes(&out), 3));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_bit(cplanes(&out), 2));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_bit(cplanes(&out), 1));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_bit(cplanes(&out), 0));
}

static void test_reductions_and_truth(void) {
    word a = make(0x7, 0x8); /* 0b?111 */
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_reduce_and(cplanes(&a), 4));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_w4_reduce_or(cplanes(&a), 4));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_reduce_xor(cplanes(&a), 4));
    word b = make(0x6, 0x8); /* 0b?110 */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_reduce_and(cplanes(&b), 3));
    word c = make(0x0, 0x1); /* 0b000? */
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_X, odin3_w4_truth(cplanes(&c), 4));
    word d = make(0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_truth(cplanes(&d), 4));
}

static void test_extend_copies_unknown_sign(void) {
    word a;
    memset(&a, 0, sizeof a);
    odin3_w4_set_bit(planes(&a), 3, ODIN3_W4_Z);
    odin3_w4_extend(planes(&a), &(odin3_word_resize){4, WIDE, true});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, odin3_w4_bit(cplanes(&a), 100));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_W4_Z, odin3_w4_bit(cplanes(&a), WIDE - 1));
    word b;
    memset(&b, 0, sizeof b);
    odin3_w4_set_bit(planes(&b), 3, ODIN3_W4_X);
    odin3_w4_extend(planes(&b), &(odin3_word_resize){4, 70, false});
    TEST_ASSERT_EQUAL_UINT32(0, odin3_w4_bit(cplanes(&b), 69));
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
