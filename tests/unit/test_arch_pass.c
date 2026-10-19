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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
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
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(g_design, "read_arch", XML), g_msg);
    TEST_ASSERT_TRUE(has_type(g_design, "adder"));
    TEST_ASSERT_TRUE(has_type(g_design, "multiply"));
    TEST_ASSERT_TRUE(has_type(g_design, "single_port_ram"));
    TEST_ASSERT_FALSE(has_type(g_design, "dual_port_ram"));
    const odin3_arch *arch = odin3_design_arch(g_design);
    TEST_ASSERT_NOT_NULL(arch);
    TEST_ASSERT_EQUAL_UINT32(6, odin3_arch_lut_k(arch));
    TEST_ASSERT_EQUAL_STRING("read_arch: pb_ok: 3 models (3 registered), 8 leaves, K=6", g_info);
}

static void test_write_techlib_round_trip(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(g_design, "read_arch", XML));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, run(g_design, "write_techlib", OUT_DIR "/rt.o3lib"), g_msg);
    odin3_design *fresh = odin3_design_create();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(fresh, OUT_DIR "/rt.o3lib"), g_msg);
    arch_test_same_type(g_design, fresh, "adder");
    arch_test_same_type(g_design, fresh, "multiply");
    arch_test_same_type(g_design, fresh, "single_port_ram");
    odin3_design_destroy(fresh);
}

static void test_write_techlib_without_arch(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "write_techlib", "-"));
    TEST_ASSERT_EQUAL_STRING("write_techlib: no architecture has been read (read_arch first)",
                             g_msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "write_techlib", ""));
}

static void test_o3lib_argument_is_read_techlib(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run(g_design, "read_arch", ODIN3_LIB_DIR "/vtr.o3lib"),
                                  g_msg);
    TEST_ASSERT_TRUE(has_type(g_design, "dual_port_ram"));
    TEST_ASSERT_NULL(odin3_design_arch(g_design));
}

static void test_bad_arguments(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", "a.txt"));
    TEST_ASSERT_EQUAL_STRING("read_arch: expected a .xml or .o3lib file, not 'a.txt'", g_msg);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", ""));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", "a.xml b.xml"));
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_IO, run(g_design, "read_arch", OUT_DIR "/missing.xml"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "cannot read"));
    TEST_ASSERT_NULL(odin3_design_arch(g_design));
}

static void test_failed_import_changes_nothing(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          run(g_design, "read_arch", ODIN3_ARCH_DATA_DIR "/pb_unknown_model.xml"));
    TEST_ASSERT_NULL(odin3_design_arch(g_design));
    TEST_ASSERT_FALSE(has_type(g_design, "m"));
    /* registration fails: the design already has `adder` from vtr.o3lib */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(g_design, "read_techlib", ODIN3_LIB_DIR "/vtr.o3lib"));
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run(g_design, "read_arch", XML));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "pb_ok.xml (generated .o3lib):"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "cell type 'multiply' already exists"));
    TEST_ASSERT_NULL(odin3_design_arch(g_design));
}

static void test_second_import_is_refused(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run(g_design, "read_arch", XML));
    const odin3_arch *first = odin3_design_arch(g_design);
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run(g_design, "read_arch", XML));
    TEST_ASSERT_EQUAL_STRING("read_arch: an architecture has already been read", g_msg);
    TEST_ASSERT_EQUAL_PTR(first, odin3_design_arch(g_design));
}

static void test_oom_sweep(void) {
    for (long n = 0; n < 6000; n++) {
        odin3_design *design = odin3_design_create();
        odin3_util_set_alloc_fail_after(n);
        odin3_status st = odin3_arch_read(design, XML);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        TEST_ASSERT_EQUAL_INT(st == ODIN3_OK, odin3_design_arch(design) != NULL);
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
    RUN_TEST(test_second_import_is_refused);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
