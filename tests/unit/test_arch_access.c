/*
 * test_arch_access.c — the read-only accessors partial_map reads from the arch record (4.2 spec
 * section 9, plan Task 4): the model port widths, the template flag, the RAM address limit and the
 * RAM modes. Hand-made XML always; the three sweep archs with $VTR_ROOT.
 */
#include "arch_test.h"

static odin3_arch *g_arch;

void setUp(void) {
    arch_test_reset();
    g_arch = odin3_arch_create();
    NOT_NULL(g_arch);
}

void tearDown(void) {
    odin3_arch_destroy(g_arch);
    arch_test_done();
}

static void load(const char *file) {
    EQ_INT_MSG(ODIN3_OK, arch_test_parse_file(g_arch, file), g_msg);
    EQ_INT_MSG(ODIN3_OK, odin3_arch_make_library(g_arch), g_msg);
}

static uint32_t width_of(const char *model, const char *port) {
    return odin3_arch_model_port_width(odin3_arch_model_find(g_arch, model), port);
}

static void test_widths_27(void) {
    load("acc_27.xml");
    EQ_U32(27, width_of("multiply", "a"));
    EQ_U32(27, width_of("multiply", "b"));
    EQ_U32(54, width_of("multiply", "out"));
    EQ_U32(0, width_of("multiply", "nope"));
    EQ_U32(0, width_of("nomodel", "a"));
}

static void test_widths_25x18(void) {
    load("acc_25x18.xml");
    EQ_U32(25, width_of("multiply", "a"));
    EQ_U32(18, width_of("multiply", "b"));
}

static void test_template_flag(void) {
    load("acc_27.xml");
    IS_TRUE(odin3_arch_model_is_template(g_arch, "multiply"));
    IS_TRUE(odin3_arch_model_is_template(g_arch, "adder"));
    IS_TRUE(odin3_arch_model_is_template(g_arch, "single_port_ram"));
    IS_TRUE(odin3_arch_model_is_template(g_arch, "dual_port_ram"));
    IS_FALSE(odin3_arch_model_is_template(g_arch, "nomodel"));
}

static void test_renamed_model_is_not_a_template(void) {
    load("acc_renamed.xml");
    NOT_NULL(odin3_arch_model_find(g_arch, "multiply"));
    IS_FALSE(odin3_arch_model_is_template(g_arch, "multiply"));
}

static void test_no_models(void) {
    load("acc_none.xml");
    IS_FALSE(odin3_arch_model_is_template(g_arch, "multiply"));
    EQ_U32(0, odin3_arch_max_addr_pins(g_arch));
    EQ_U32(0, odin3_arch_modes_of(g_arch, "single_port_ram", NULL, 0));
}

static void test_max_addr_pins(void) {
    load("acc_27.xml");
    EQ_U32(11, odin3_arch_max_addr_pins(g_arch)); /* sp 9, 11 and dp 10 */
}

static void test_modes(void) {
    load("acc_27.xml");
    odin3_arch_ram_mode modes[4];
    EQ_U32(2, odin3_arch_modes_of(g_arch, "single_port_ram", modes, 4));
    EQ_U32(9, modes[0].addr);
    EQ_U32(40, modes[0].data);
    EQ_U32(11, modes[1].addr);
    EQ_U32(10, modes[1].data);
    EQ_U32(1, odin3_arch_modes_of(g_arch, "dual_port_ram", modes, 4));
    EQ_U32(10, modes[0].addr);
    EQ_U32(20, modes[0].data);
    /* the count is the number of modes, whatever the capacity */
    EQ_U32(2, odin3_arch_modes_of(g_arch, "single_port_ram", modes, 1));
    EQ_U32(9, modes[0].addr);
}

/* --- the sweep archs (skip without VTR_ROOT) -------------------------------------------------- */

static bool read_vtr(const char *rel) {
    const char *root = getenv("VTR_ROOT");
    if (root == NULL) {
        return false;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/vtr_flow/arch/%s", root, rel);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *buf = malloc(1U << 22);
    TEST_ASSERT_NOT_NULL(buf);
    size_t got = fread(buf, 1, (1U << 22) - 1U, fp);
    buf[got] = '\0';
    (void)fclose(fp);
    EQ_INT_MSG(ODIN3_OK, arch_test_parse(g_arch, path, buf), g_msg);
    free(buf);
    EQ_INT_MSG(ODIN3_OK, odin3_arch_make_library(g_arch), g_msg);
    return true;
}

static void test_sweep_k6(void) {
    if (!read_vtr("timing/k6_frac_N10_frac_chain_mem32K_40nm.xml")) {
        TEST_IGNORE_MESSAGE("VTR_ROOT not set");
    }
    EQ_U32(36, width_of("multiply", "a"));
    EQ_U32(36, width_of("multiply", "b"));
    EQ_U32(15, odin3_arch_max_addr_pins(g_arch));
    IS_TRUE(odin3_arch_model_is_template(g_arch, "adder"));
    IS_TRUE(odin3_arch_model_is_template(g_arch, "single_port_ram"));
    IS_TRUE(odin3_arch_model_is_template(g_arch, "dual_port_ram"));
}

static void test_sweep_coffe(void) {
    if (!read_vtr("COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml")) {
        TEST_IGNORE_MESSAGE("VTR_ROOT not set");
    }
    EQ_U32(27, width_of("multiply", "a"));
    EQ_U32(27, width_of("multiply", "b"));
    EQ_U32(11, odin3_arch_max_addr_pins(g_arch));
    odin3_arch_ram_mode modes[8];
    EQ_U32(3, odin3_arch_modes_of(g_arch, "single_port_ram", modes, 8));
    EQ_U32(9, modes[0].addr);
    EQ_U32(40, modes[0].data);
    EQ_U32(10, modes[1].addr);
    EQ_U32(20, modes[1].data);
    EQ_U32(11, modes[2].addr);
    EQ_U32(10, modes[2].data);
}

static void test_sweep_k4(void) {
    if (!read_vtr("timing/k4_N4_90nm.xml")) {
        TEST_IGNORE_MESSAGE("VTR_ROOT not set");
    }
    IS_FALSE(odin3_arch_model_is_template(g_arch, "multiply"));
    IS_FALSE(odin3_arch_model_is_template(g_arch, "adder"));
    EQ_U32(0, odin3_arch_max_addr_pins(g_arch));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_widths_27);
    RUN_TEST(test_widths_25x18);
    RUN_TEST(test_template_flag);
    RUN_TEST(test_renamed_model_is_not_a_template);
    RUN_TEST(test_no_models);
    RUN_TEST(test_max_addr_pins);
    RUN_TEST(test_modes);
    RUN_TEST(test_sweep_k6);
    RUN_TEST(test_sweep_coffe);
    RUN_TEST(test_sweep_k4);
    return UNITY_END();
}
