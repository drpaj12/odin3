/*
 * test_arch_record.c — the architecture record and its design hook (4.1 Task 1).
 */
#include "frontends/arch/arch.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"

#include <string.h>

void setUp(void) {}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void test_empty_record_queries(void) {
    odin3_arch *arch = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(arch);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_lut_k(arch));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_lut_k_min(arch));
    TEST_ASSERT_FALSE(odin3_arch_has_adder(arch));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_model_count(arch));
    TEST_ASSERT_NULL(odin3_arch_model_at(arch, 0));
    TEST_ASSERT_NULL(odin3_arch_model_find(arch, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_leaf_count(arch));
    TEST_ASSERT_NULL(odin3_arch_leaf_at(arch, 0));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_ic_count(arch));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_direct_count(arch));
    TEST_ASSERT_EQUAL_STRING("", odin3_arch_library_text(arch));
    uint32_t idx = 0;
    TEST_ASSERT_FALSE(odin3_arch_next_leaf_of(arch, "adder", 0, &idx));
    odin3_arch_destroy(arch);
    odin3_arch_destroy(NULL);
}

static void test_hand_built_record(void) {
    odin3_arch *arch = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(arch);
    odin3_arch_leaf *leaf = odin3_vec_push(&arch->leaves);
    TEST_ASSERT_NOT_NULL(leaf);
    memset(leaf, 0, sizeof *leaf);
    leaf->kind = ODIN3_ARCH_LEAF_SUBCKT;
    leaf->model = "adder";
    arch->lut_k = 6;
    arch->lut_k_min = 5;
    arch->has_adder = true;
    TEST_ASSERT_EQUAL_UINT32(6, odin3_arch_lut_k(arch));
    TEST_ASSERT_EQUAL_UINT32(5, odin3_arch_lut_k_min(arch));
    TEST_ASSERT_TRUE(odin3_arch_has_adder(arch));
    uint32_t idx = 9;
    TEST_ASSERT_TRUE(odin3_arch_next_leaf_of(arch, "adder", 0, &idx));
    TEST_ASSERT_EQUAL_UINT32(0, idx);
    TEST_ASSERT_FALSE(odin3_arch_next_leaf_of(arch, "adder", 1, &idx));
    TEST_ASSERT_FALSE(odin3_arch_next_leaf_of(arch, "multiply", 0, &idx));
    TEST_ASSERT_EQUAL_PTR(leaf, odin3_arch_leaf_at(arch, 0));
    odin3_arch_destroy(arch);
}

static void test_design_owns_record(void) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_NULL(odin3_design_arch(design));
    odin3_arch *first = NULL;
    odin3_arch *second = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_arch(design, &first));
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_arch(design, &second));
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_PTR(first, odin3_design_arch(design));
    odin3_design_drop_arch(design);
    TEST_ASSERT_NULL(odin3_design_arch(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_arch(design, &first));
    odin3_design_destroy(design); /* frees the record: ASan checks the leak */
}

static void test_create_oom(void) {
    for (long n = 0; n < 4; n++) {
        odin3_util_set_alloc_fail_after(n);
        odin3_arch *arch = odin3_arch_create();
        odin3_util_set_alloc_fail_after(-1);
        if (arch == NULL) {
            continue;
        }
        odin3_arch_destroy(arch);
    }
    odin3_design *design = odin3_design_create();
    odin3_util_set_alloc_fail_after(0);
    odin3_arch *arch = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_design_get_arch(design, &arch));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_NULL(odin3_design_arch(design));
    odin3_design_destroy(design);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_record_queries);
    RUN_TEST(test_hand_built_record);
    RUN_TEST(test_design_owns_record);
    RUN_TEST(test_create_oom);
    return UNITY_END();
}
