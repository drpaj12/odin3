/*
 * test_arch_vtr.c — the real VTR architectures (4.1 Task 6; spec T-1, T-3, T-4). Skips (exit 77)
 * without $VTR_ROOT.
 */
#include "arch_test.h"
#include "arch_test_cmp.h"
#include "ir/design.h"
#include "techlib/reader.h"

#define ARCH_K6 "timing/k6_frac_N10_frac_chain_mem32K_40nm.xml"
#define ARCH_DSP "COFFE_22nm/k6_frac_N10_4add_2chains_depop50_mem20K_22nm.xml"
#define ARCH_K4 "timing/k4_N4_90nm.xml"

static char g_path[1024];

void setUp(void) {
    arch_test_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
}

void tearDown(void) {
    arch_test_done();
}

static const char *arch_path(const char *rel) {
    (void)snprintf(g_path, sizeof g_path, "%s/vtr_flow/arch/%s", getenv("VTR_ROOT"), rel);
    return g_path;
}

static odin3_design *import(const char *rel) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_arch_read(design, arch_path(rel)), g_msg);
    TEST_ASSERT_NOT_NULL(odin3_design_arch(design));
    return design;
}

static const odin3_arch_mport *port_of(const odin3_arch *arch, const char *model, const char *port) {
    const odin3_arch_model *m = odin3_arch_model_find(arch, model);
    TEST_ASSERT_NOT_NULL_MESSAGE(m, model);
    for (uint32_t i = 0; i < m->n_ports; i++) {
        if (strcmp(m->ports[i].name, port) == 0) {
            return &m->ports[i];
        }
    }
    TEST_FAIL_MESSAGE(port);
    return NULL;
}

static uint32_t count_leaves(const odin3_arch *arch, const char *model) {
    uint32_t count = 0;
    uint32_t idx = 0;
    uint32_t from = 0;
    while (odin3_arch_next_leaf_of(arch, model, from, &idx)) {
        count++;
        from = idx + 1;
    }
    return count;
}

static bool has_pack(const odin3_arch *arch, const char *name) {
    for (uint32_t i = 0; i < odin3_arch_ic_count(arch); i++) {
        const odin3_arch_ic *ic = odin3_arch_ic_at(arch, i);
        for (uint32_t p = 0; p < ic->n_packs; p++) {
            if (strcmp(ic->packs[p].name, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* T-1: the Phase 3 arch gives exactly the four hard cells of lib/vtr.o3lib. */
static void test_k6_frac_has_the_vtr_o3lib_cells(void) {
    odin3_design *da = import(ARCH_K6);
    odin3_design *db = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_techlib_read(db, ODIN3_LIB_DIR "/vtr.o3lib"));
    const odin3_arch *arch = odin3_design_arch(da);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_arch_model_count(arch));
    TEST_ASSERT_EQUAL_UINT32(4, arch->n_registered);
    static const char *const k_names[] = {"multiply", "single_port_ram", "dual_port_ram", "adder"};
    for (size_t i = 0; i < 4; i++) {
        arch_test_same_type(da, db, k_names[i]);
        TEST_ASSERT_NOT_NULL(odin3_arch_model_find(arch, k_names[i]));
    }
    TEST_ASSERT_EQUAL_UINT(0, g_warnings);
    odin3_design_destroy(da);
    odin3_design_destroy(db);
}

/* T-3: the facts of the Phase 3 arch. */
static void test_k6_frac_facts(void) {
    odin3_design *design = import(ARCH_K6);
    const odin3_arch *arch = odin3_design_arch(design);
    TEST_ASSERT_EQUAL_UINT32(6, odin3_arch_lut_k(arch));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_arch_lut_k_min(arch)); /* the arithmetic mode's lut4 */
    TEST_ASSERT_TRUE(odin3_arch_has_adder(arch));
    TEST_ASSERT_EQUAL_UINT32(3, count_leaves(arch, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(7, count_leaves(arch, "single_port_ram"));
    TEST_ASSERT_EQUAL_UINT32(6, count_leaves(arch, "dual_port_ram"));
    TEST_ASSERT_EQUAL_UINT32(1, count_leaves(arch, "adder"));
    TEST_ASSERT_EQUAL_UINT32(36, port_of(arch, "multiply", "a")->max_width);
    TEST_ASSERT_EQUAL_UINT32(9, port_of(arch, "multiply", "a")->min_width);
    TEST_ASSERT_EQUAL_UINT32(72, port_of(arch, "multiply", "out")->max_width);
    TEST_ASSERT_EQUAL_UINT32(15, port_of(arch, "single_port_ram", "addr")->max_width);
    TEST_ASSERT_EQUAL_UINT32(9, port_of(arch, "single_port_ram", "addr")->min_width);
    TEST_ASSERT_EQUAL_UINT32(64, port_of(arch, "single_port_ram", "data")->max_width);
    uint32_t idx = 0;
    TEST_ASSERT_TRUE(odin3_arch_next_leaf_of(arch, "multiply", 0, &idx));
    const odin3_arch_leaf *first = odin3_arch_leaf_at(arch, idx);
    TEST_ASSERT_EQUAL_STRING("mult_36", first->levels[0].pb);
    TEST_ASSERT_EQUAL_STRING("two_divisible_mult_18x18", first->levels[1].in_mode);
    TEST_ASSERT_TRUE(has_pack(arch, "ble5"));
    TEST_ASSERT_TRUE(has_pack(arch, "ble6"));
    TEST_ASSERT_TRUE(has_pack(arch, "chain"));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_arch_direct_count(arch));
    TEST_ASSERT_EQUAL_STRING("adder_carry", odin3_arch_direct_at(arch, 0)->name);
    TEST_ASSERT_EQUAL_INT32(-1, odin3_arch_direct_at(arch, 0)->y_offset);
    odin3_design_destroy(design);
}

/* T-3: the recommended DSP arch (27-bit model, 18x19 and 27x27 leaves). */
static void test_dsp_arch_facts(void) {
    odin3_design *design = import(ARCH_DSP);
    const odin3_arch *arch = odin3_design_arch(design);
    TEST_ASSERT_EQUAL_UINT32(27, port_of(arch, "multiply", "a")->max_width);
    TEST_ASSERT_EQUAL_UINT32(54, port_of(arch, "multiply", "out")->max_width);
    TEST_ASSERT_EQUAL_UINT32(2, count_leaves(arch, "multiply"));
    uint32_t idx = 0;
    TEST_ASSERT_TRUE(odin3_arch_next_leaf_of(arch, "multiply", 0, &idx));
    const odin3_arch_leaf *small = odin3_arch_leaf_at(arch, idx);
    TEST_ASSERT_EQUAL_STRING("b", small->pins[1].name);
    TEST_ASSERT_EQUAL_UINT32(18, small->pins[0].num_pins);
    TEST_ASSERT_EQUAL_UINT32(19, small->pins[1].num_pins);
    TEST_ASSERT_EQUAL_UINT32(37, small->pins[2].num_pins);
    TEST_ASSERT_EQUAL_UINT32(6, odin3_arch_lut_k(arch));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_arch_lut_k_min(arch));
    TEST_ASSERT_TRUE(odin3_arch_has_adder(arch));
    TEST_ASSERT_TRUE(has_pack(arch, "chain"));
    TEST_ASSERT_EQUAL_UINT32(40, port_of(arch, "single_port_ram", "data")->max_width);
    TEST_ASSERT_EQUAL_UINT32(10, port_of(arch, "single_port_ram", "data")->min_width);
    TEST_ASSERT_EQUAL_UINT32(11, port_of(arch, "single_port_ram", "addr")->max_width);
    TEST_ASSERT_EQUAL_UINT32(2, count_leaves(arch, "dual_port_ram"));
    TEST_ASSERT_EQUAL_UINT32(3, count_leaves(arch, "single_port_ram"));
    odin3_design_destroy(design);
}

/* T-3: the recommended plain-LUT arch (K 4, no models, no hard blocks). */
static void test_plain_lut_arch_facts(void) {
    odin3_design *design = import(ARCH_K4);
    const odin3_arch *arch = odin3_design_arch(design);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_arch_lut_k(arch));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_arch_lut_k_min(arch));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_arch_model_count(arch));
    TEST_ASSERT_FALSE(odin3_arch_has_adder(arch));
    TEST_ASSERT_TRUE(odin3_arch_leaf_count(arch) > 0);
    uint32_t idx = 0;
    for (uint32_t i = 0; i < odin3_arch_leaf_count(arch); i++) {
        TEST_ASSERT_NOT_EQUAL(ODIN3_ARCH_LEAF_SUBCKT, odin3_arch_leaf_at(arch, i)->kind);
    }
    TEST_ASSERT_FALSE(odin3_arch_next_leaf_of(arch, "multiply", 0, &idx));
    odin3_design_destroy(design);
}

/* T-4: the fallbacks import too. */
static void test_fallback_archs_import(void) {
    odin3_design *a = import("timing/k6_N10_mem32K_40nm.xml");
    TEST_ASSERT_FALSE(odin3_arch_has_adder(odin3_design_arch(a)));
    odin3_design_destroy(a);
    odin3_design *b = import("timing/k6_N10_40nm.xml");
    TEST_ASSERT_EQUAL_UINT32(6, odin3_arch_lut_k(odin3_design_arch(b)));
    odin3_design_destroy(b);
}

int main(void) {
    const char *root = getenv("VTR_ROOT");
    if (root == NULL || root[0] == '\0') {
        (void)puts("skip: VTR_ROOT not set");
        return 77;
    }
    UNITY_BEGIN();
    RUN_TEST(test_k6_frac_has_the_vtr_o3lib_cells);
    RUN_TEST(test_k6_frac_facts);
    RUN_TEST(test_dsp_arch_facts);
    RUN_TEST(test_plain_lut_arch_facts);
    RUN_TEST(test_fallback_archs_import);
    return UNITY_END();
}
