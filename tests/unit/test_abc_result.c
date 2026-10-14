/*
 * test_abc_result.c — the abc_result form's checks and constant folding (3D plan Task 16; spec
 * §6.1, AB-14, I10), on hand-made results.
 */
#include "ir/lut.h"
#include "odin3/odin3.h"
#include "passes/abc/result.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <stdint.h>
#include <string.h>

enum { CHAIN = 100000, OOM_LIMIT = 2000 };

void setUp(void) {
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void push32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    TEST_ASSERT_NOT_NULL(slot);
    *slot = value;
}

/* Appends a LUT (fanins, table word 0) to res; returns its object number. */
static uint32_t add_lut(odin3_abc_result *res, const uint32_t *fanins, uint32_t k, uint64_t table) {
    if (res->fanin_start.len == 0) {
        push32(&res->fanin_start, 0);
        push32(&res->truth_start, 0);
    }
    for (uint32_t v = 0; v < k; v++) {
        push32(&res->fanins, fanins[v]);
    }
    push32(&res->lut_k, k);
    uint64_t *word = odin3_vec_push(&res->truth);
    TEST_ASSERT_NOT_NULL(word);
    *word = table;
    push32(&res->fanin_start, (uint32_t)res->fanins.len);
    push32(&res->truth_start, (uint32_t)res->truth.len);
    return odin3_abc_result_first_lut(res) + res->n_luts++;
}

static uint32_t u32(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

/* Two CIs (objects 2, 3), no registers. */
static void init_two_cis(odin3_abc_result *res) {
    odin3_abc_result_init(res);
    res->n_cis = 2;
}

/* A constant-1 LUT read by an AND LUT: the reader becomes the identity of its other input. */
static void test_fold_constant_one_reader(void) {
    odin3_abc_result res;
    init_two_cis(&res);
    uint32_t in_one[1] = {2};
    uint32_t one = add_lut(&res, in_one, 1, 0x3); /* both table bits 1: constant 1 over 2^1 bits */
    uint32_t in_and[2] = {one, 3};
    uint32_t and_obj = add_lut(&res, in_and, 2, 0x8);
    res.n_cos = 1;
    push32(&res.co_driver, and_obj);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_result_fold(&res));
    TEST_ASSERT_EQUAL_UINT32(1, res.n_luts);
    TEST_ASSERT_EQUAL_UINT32(1, u32(&res.lut_k, 0));
    TEST_ASSERT_EQUAL_UINT32(3, u32(&res.fanins, 0));                    /* the other input */
    TEST_ASSERT_EQUAL_HEX64(0x2, ((const uint64_t *)res.truth.data)[0]); /* identity */
    TEST_ASSERT_EQUAL_UINT32(odin3_abc_result_first_lut(&res), u32(&res.co_driver, 0));
    odin3_abc_result_free(&res);
}

/* A CO driven by an all-0 LUT has driver object 0; a LUT that becomes constant through its reader's
 * cofactor is folded in the same pass. */
static void test_fold_constant_zero_and_cascade(void) {
    odin3_abc_result res;
    init_two_cis(&res);
    uint32_t in0[2] = {2, 3};
    uint32_t zero = add_lut(&res, in0, 2, 0x0);
    uint32_t in_and[2] = {zero, 3};
    uint32_t and_obj = add_lut(&res, in_and, 2, 0x8); /* 0 & x: constant 0 after the cofactor */
    res.n_cos = 2;
    push32(&res.co_driver, zero);
    push32(&res.co_driver, and_obj);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_result_fold(&res));
    TEST_ASSERT_EQUAL_UINT32(0, res.n_luts);
    TEST_ASSERT_EQUAL_UINT32(0, u32(&res.co_driver, 0));
    TEST_ASSERT_EQUAL_UINT32(0, u32(&res.co_driver, 1));
    odin3_abc_result_free(&res);
}

static odin3_status check_text(const odin3_abc_result *res, const odin3_abc_result_expect *exp,
                               char *text, size_t cap) {
    odin3_abc_fail fail;
    memset(&fail, 0, sizeof fail);
    odin3_status st = odin3_abc_result_check(res, exp, &fail);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_FAIL_RESULT, fail.kind);
        (void)snprintf(text, cap, "%s", fail.text);
    }
    return st;
}

static void test_check_texts(void) {
    odin3_abc_result res;
    char text[ODIN3_ABC_FAIL_TEXT_BYTES];
    init_two_cis(&res);
    res.n_cos = 1;
    odin3_abc_result_expect exp = {3, 1, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, check_text(&res, &exp, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("2 primary inputs, expected 3", text);
    exp = (odin3_abc_result_expect){2, 4, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, check_text(&res, &exp, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("1 outputs, expected 4", text);
    res.n_regs_out = 2;
    push32(&res.reg_from, 0);
    push32(&res.reg_from, 1);
    exp = (odin3_abc_result_expect){2, 1, 1, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, check_text(&res, &exp, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("2 registers, more than the 1 it was given", text);
    res.reg_from.len = 0;
    push32(&res.reg_from, 0);
    push32(&res.reg_from, ODIN3_ABC_NO_REG);
    exp = (odin3_abc_result_expect){2, 1, 2, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, check_text(&res, &exp, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("register 1 carries no input register's name", text);
    res.reg_from.len = 0;
    res.n_regs_out = 0;
    uint32_t fan[17];
    for (uint32_t i = 0; i < 17; i++) {
        fan[i] = 2;
    }
    (void)add_lut(&res, fan, 5, 0x1);
    exp = (odin3_abc_result_expect){2, 1, 0, 4};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, check_text(&res, &exp, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("a LUT with 5 inputs exceeds --lut 4", text);
    ((uint32_t *)res.lut_k.data)[0] = 17;
    exp.lut_k_max = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE, check_text(&res, &exp, text, sizeof text));
    TEST_ASSERT_EQUAL_STRING("a LUT with 17 inputs (at most 16)", text);
    ((uint32_t *)res.lut_k.data)[0] = 5;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, check_text(&res, &exp, text, sizeof text));
    odin3_abc_result_free(&res);
}

/* A 100,000-LUT chain whose first LUT is constant 1: every AND folds into the identity of the
 * previous one, with no recursion. */
static void test_fold_chain_is_linear(void) {
    odin3_abc_result res;
    init_two_cis(&res);
    uint32_t in_one[1] = {2};
    uint32_t prev = add_lut(&res, in_one, 1, 0x3);
    for (uint32_t i = 0; i < CHAIN; i++) {
        uint32_t fan[2] = {prev, 3};
        prev = add_lut(&res, fan, 2, 0x8);
    }
    res.n_cos = 1;
    push32(&res.co_driver, prev);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_result_fold(&res));
    TEST_ASSERT_EQUAL_UINT32(CHAIN, res.n_luts);
    TEST_ASSERT_EQUAL_UINT32(odin3_abc_result_first_lut(&res) + CHAIN - 1U, u32(&res.co_driver, 0));
    odin3_abc_result_free(&res);
}

static void test_fold_oom_sweep(void) {
    uint32_t failures = 0;
    for (long at = 0; at < OOM_LIMIT; at++) {
        odin3_abc_result res;
        init_two_cis(&res);
        uint32_t in_one[1] = {2};
        uint32_t one = add_lut(&res, in_one, 1, 0x3);
        uint32_t fan[2] = {one, 3};
        uint32_t reader = add_lut(&res, fan, 2, 0x8);
        res.n_cos = 1;
        push32(&res.co_driver, reader);
        odin3_util_set_alloc_fail_after(at);
        odin3_status st = odin3_abc_result_fold(&res);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_UINT32(1, res.n_luts);
            odin3_abc_result_free(&res);
            break;
        }
        failures++;
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(2, res.n_luts); /* unchanged */
        odin3_abc_result_free(&res);
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0, failures);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fold_constant_one_reader);
    RUN_TEST(test_fold_constant_zero_and_cascade);
    RUN_TEST(test_check_texts);
    RUN_TEST(test_fold_chain_is_linear);
    RUN_TEST(test_fold_oom_sweep);
    return UNITY_END();
}
