/*
 * test_arch_record.c — the architecture record and its design hook (4.1 Task 1).
 */
#include "arch_test.h"
#include "frontends/arch/arch.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"

#include <string.h>

void setUp(void) {
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
}

static void test_empty_record_queries(void) {
    odin3_arch *arch = odin3_arch_create();
    NOT_NULL(arch);
    EQ_U32(0, odin3_arch_lut_k(arch));
    EQ_U32(0, odin3_arch_lut_k_min(arch));
    IS_FALSE(odin3_arch_has_adder(arch));
    EQ_U32(0, odin3_arch_model_count(arch));
    IS_NULL(odin3_arch_model_at(arch, 0));
    IS_NULL(odin3_arch_model_find(arch, "multiply"));
    EQ_U32(0, odin3_arch_leaf_count(arch));
    IS_NULL(odin3_arch_leaf_at(arch, 0));
    EQ_U32(0, odin3_arch_ic_count(arch));
    EQ_U32(0, odin3_arch_direct_count(arch));
    EQ_STR("", odin3_arch_library_text(arch));
    uint32_t idx = 0;
    IS_FALSE(odin3_arch_next_leaf_of(arch, "adder", 0, &idx));
    odin3_arch_destroy(arch);
    odin3_arch_destroy(NULL);
}

static void test_hand_built_record(void) {
    odin3_arch *arch = odin3_arch_create();
    NOT_NULL(arch);
    odin3_arch_leaf *leaf = odin3_vec_push(&arch->leaves);
    NOT_NULL(leaf);
    memset(leaf, 0, sizeof *leaf);
    leaf->kind = ODIN3_ARCH_LEAF_SUBCKT;
    leaf->model = "adder";
    arch->lut_k = 6;
    arch->lut_k_min = 5;
    arch->has_adder = true;
    EQ_U32(6, odin3_arch_lut_k(arch));
    EQ_U32(5, odin3_arch_lut_k_min(arch));
    IS_TRUE(odin3_arch_has_adder(arch));
    uint32_t idx = 9;
    IS_TRUE(odin3_arch_next_leaf_of(arch, "adder", 0, &idx));
    EQ_U32(0, idx);
    IS_FALSE(odin3_arch_next_leaf_of(arch, "adder", 1, &idx));
    IS_FALSE(odin3_arch_next_leaf_of(arch, "multiply", 0, &idx));
    EQ_PTR(leaf, odin3_arch_leaf_at(arch, 0));
    odin3_arch_destroy(arch);
}

static void test_design_owns_record(void) {
    odin3_design *design = odin3_design_create();
    NOT_NULL(design);
    IS_NULL(odin3_design_arch(design));
    odin3_arch *first = NULL;
    odin3_arch *second = NULL;
    EQ_INT(ODIN3_OK, odin3_design_get_arch(design, &first));
    NOT_NULL(first);
    EQ_INT(ODIN3_OK, odin3_design_get_arch(design, &second));
    EQ_PTR(first, second);
    EQ_PTR(first, odin3_design_arch(design));
    odin3_design_drop_arch(design);
    IS_NULL(odin3_design_arch(design));
    EQ_INT(ODIN3_OK, odin3_design_get_arch(design, &first));
    odin3_design_destroy(design); /* frees the record: ASan checks the leak */
}

static void test_create_oom(void) {
    for (long limit = 0; limit < 4; limit++) {
        odin3_util_set_alloc_fail_after(limit);
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
    EQ_INT(ODIN3_ERR_NO_MEMORY, odin3_design_get_arch(design, &arch));
    odin3_util_set_alloc_fail_after(-1);
    IS_NULL(odin3_design_arch(design));
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
