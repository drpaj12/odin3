/*
 * test_abc_init1.c — registers that start at 1 (3D plan Task 19; spec §5.4, AB-13, finding C1,
 * Review Focus 2) on the real ABC with the default script, `--lut 6` and the BLIF exchange: a
 * toggling register and two INIT 1 registers that scorr merges each come out as one `$_DFF_P_`
 * with INIT 1, and `equiv-check --strict-init` proves the result equal to the input. The odin3 CLI
 * runs the pass (it registers the adapter); the files are read back in process.
 */
#include "abc_fixture.h"
#include "abc_spawn.h"
#include "passes/manager.h"

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif
#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository"
#endif

static odin3_design *design;

void setUp(void) {
    design = NULL;
}

void tearDown(void) {
    odin3_design_destroy(design);
}

typedef struct flops {
    uint32_t count;
    int64_t init;
} flops;

static flops count_flops(const odin3_module *mod) {
    flops found = {0, -1};
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) &&
            strcmp(odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node))->name,
                   "$_DFF_P_") == 0) {
            found.count++;
            found.init = odin3_node_param(mod, node, 0)->i;
        }
    }
    return found;
}

/* Maps data/<name>.blif with the real ABC and checks the result. */
static void expect_one_flop_at_one(const char *name, uint32_t want_flops) {
    char data[AFX_JOIN];
    char before[AFX_PATH];
    char after[AFX_PATH];
    char file[AFX_PATH];
    (void)snprintf(data, sizeof data, "%s/tests/unit/data/abc/%s.blif", ODIN3_SOURCE_DIR, name);
    (void)snprintf(file, sizeof file, "%s_before.blif", name);
    afx_path(before, file);
    (void)snprintf(file, sizeof file, "%s_after.blif", name);
    afx_path(after, file);
    char script[4 * AFX_PATH];
    (void)snprintf(script, sizeof script,
                   "read_techlib %s/vtr.o3lib; read_blif %s; write_blif %s; abc --lut 6; "
                   "write_blif %s",
                   ODIN3_LIB_DIR, data, before, after);
    TEST_ASSERT_EQUAL_INT(0, afx_run_odin3(script));
    design = odin3_design_create();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, after));
    afx_check(design);
    flops found = count_flops(afx_top(design));
    TEST_ASSERT_EQUAL_UINT32(want_flops, found.count);
    TEST_ASSERT_EQUAL_INT64(1, found.init);
    TEST_ASSERT_EQUAL_INT(0, afx_run_equiv(before, after, "--strict-init"));
}

static void test_toggle_init1(void) {
#ifdef ODIN3_HAVE_REAL_ABC
    expect_one_flop_at_one("toggle_init1", 1);
#else
    TEST_IGNORE_MESSAGE("built without ODIN3_WITH_ABC");
#endif
}

static void test_scorr_merge_init1(void) {
#ifdef ODIN3_HAVE_REAL_ABC
    expect_one_flop_at_one("scorr_merge_init1", 1);
#else
    TEST_IGNORE_MESSAGE("built without ODIN3_WITH_ABC");
#endif
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_toggle_init1);
    RUN_TEST(test_scorr_merge_init1);
    return UNITY_END();
}
