/*
 * test_arch_pass.c — read_arch and write_techlib through the pass manager (4.1 Task 5).
 */
#include "arch_test.h"
#include "arch_test_cmp.h"
#include "ir/design.h"
#include "passes/manager.h"
#include "techlib/reader.h"

#define XML ODIN3_ARCH_DATA_DIR "/pb_ok.xml"
#define OUT_DIR "/tmp/claude-1000/p4arch"

static odin3_design *g_design;

void setUp(void) {
    arch_test_reset();
    EQ_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    g_design = odin3_design_create();
    NOT_NULL(g_design);
}

void tearDown(void) {
    odin3_design_destroy(g_design);
    arch_test_done();
}

static odin3_status run(odin3_design *design, const char *pass, const char *args) {
    return odin3_pass_run(design, pass, odin3_bytes_cstr(args));
}

static bool has_type(odin3_design *design, const char *name) {
    odin3_celltype_id id = {0};
    return cmp_def(design, name, &id) != NULL;
}

static void test_read_arch_registers_models(void) {
    EQ_INT_MSG(ODIN3_OK, run(g_design, "read_arch", XML), g_msg);
    IS_TRUE(has_type(g_design, "adder"));
    IS_TRUE(has_type(g_design, "multiply"));
    IS_TRUE(has_type(g_design, "single_port_ram"));
    IS_FALSE(has_type(g_design, "dual_port_ram"));
    const odin3_arch *arch = odin3_design_arch(g_design);
    NOT_NULL(arch);
    EQ_U32(6, odin3_arch_lut_k(arch));
    EQ_STR("read_arch: pb_ok: 3 models (3 registered), 8 leaves, K=6", g_info);
}

static void test_write_techlib_round_trip(void) {
    EQ_INT(ODIN3_OK, run(g_design, "read_arch", XML));
    EQ_INT_MSG(ODIN3_OK, run(g_design, "write_techlib", OUT_DIR "/rt.o3lib"), g_msg);
    odin3_design *fresh = odin3_design_create();
    EQ_INT_MSG(ODIN3_OK, odin3_techlib_read(fresh, OUT_DIR "/rt.o3lib"), g_msg);
    arch_test_same_type(g_design, fresh, "adder");
    arch_test_same_type(g_design, fresh, "multiply");
    arch_test_same_type(g_design, fresh, "single_port_ram");
    odin3_design_destroy(fresh);
}

static void test_write_techlib_without_arch(void) {
    EQ_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "write_techlib", "-"));
    EQ_STR("write_techlib: no architecture has been read (read_arch first)", g_msg);
    EQ_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "write_techlib", ""));
}

static void test_o3lib_argument_is_read_techlib(void) {
    EQ_INT_MSG(ODIN3_OK, run(g_design, "read_arch", ODIN3_LIB_DIR "/vtr.o3lib"), g_msg);
    IS_TRUE(has_type(g_design, "dual_port_ram"));
    IS_NULL(odin3_design_arch(g_design));
}

static void test_bad_arguments(void) {
    EQ_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", "a.txt"));
    EQ_STR("read_arch: expected a .xml or .o3lib file, not 'a.txt'", g_msg);
    EQ_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", ""));
    EQ_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", "a.xml b.xml"));
    arch_test_reset();
    EQ_INT(ODIN3_ERR_IO, run(g_design, "read_arch", OUT_DIR "/missing.xml"));
    NOT_NULL(strstr(g_msg, "cannot read"));
    IS_NULL(odin3_design_arch(g_design));
}

static void test_failed_import_changes_nothing(void) {
    EQ_INT(ODIN3_ERR_PARSE,
           run(g_design, "read_arch", ODIN3_ARCH_DATA_DIR "/pb_unknown_model.xml"));
    IS_NULL(odin3_design_arch(g_design));
    IS_FALSE(has_type(g_design, "m"));
    /* registration fails: the design already has `adder` from vtr.o3lib */
    EQ_INT(ODIN3_OK, run(g_design, "read_techlib", ODIN3_LIB_DIR "/vtr.o3lib"));
    arch_test_reset();
    EQ_INT(ODIN3_ERR_PARSE, run(g_design, "read_arch", XML));
    NOT_NULL(strstr(g_msg, "pb_ok.xml:"));
    NOT_NULL(strstr(g_msg, "model 'multiply' has the name of a cell type"));
    IS_NULL(odin3_design_arch(g_design));
}

static void test_collision_on_a_later_model_registers_nothing(void) {
    odin3_design *design = g_design;
    const odin3_techlib_text lib = {
        "one.o3lib", odin3_bytes_cstr("library L\ncell adder blackbox\n  in a 1\nend\n")};
    EQ_INT_MSG(ODIN3_OK, odin3_techlib_read_text(design, &lib), g_msg);
    arch_test_reset();
    /* pb_ok.xml has multiply, adder, single_port_ram: adder collides, the others must not stay */
    EQ_INT(ODIN3_ERR_PARSE, run(design, "read_arch", XML));
    EQ_STR("pb_ok.xml:7: model 'adder' has the name of a cell type the design already has",
           strstr(g_msg, "pb_ok.xml:"));
    IS_NULL(odin3_design_arch(design));
    IS_FALSE(has_type(design, "multiply"));
    IS_FALSE(has_type(design, "single_port_ram"));
    IS_TRUE(has_type(design, "adder"));
}

static void test_second_import_is_refused(void) {
    EQ_INT(ODIN3_OK, run(g_design, "read_arch", XML));
    const odin3_arch *first = odin3_design_arch(g_design);
    arch_test_reset();
    EQ_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", XML));
    EQ_STR("read_arch: an architecture has already been read", g_msg);
    EQ_PTR(first, odin3_design_arch(g_design));
}

static void test_oom_sweep(void) {
    for (long limit = 0; limit < 6000; limit++) {
        odin3_design *design = odin3_design_create();
        odin3_util_set_alloc_fail_after(limit);
        odin3_status st = odin3_arch_read(design, XML);
        odin3_util_set_alloc_fail_after(-1);
        IS_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        EQ_INT(st == ODIN3_OK, odin3_design_arch(design) != NULL);
        odin3_design_destroy(design);
        if (st == ODIN3_OK) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("never succeeded");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_read_arch_registers_models);
    RUN_TEST(test_write_techlib_round_trip);
    RUN_TEST(test_write_techlib_without_arch);
    RUN_TEST(test_o3lib_argument_is_read_techlib);
    RUN_TEST(test_bad_arguments);
    RUN_TEST(test_failed_import_changes_nothing);
    RUN_TEST(test_collision_on_a_later_model_registers_nothing);
    RUN_TEST(test_second_import_is_refused);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
