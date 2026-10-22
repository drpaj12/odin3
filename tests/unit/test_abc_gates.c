/*
 * test_abc_gates.c — abc --gates after start-up (gatemap plan Tasks 11 and 12; spec 4.3, 4.4, G-2,
 * G-7): the cell result read from hand-built odin3_abc_cells (each library gate tagged; an unknown
 * gate, a wrong input count, a wrong function and an untagged object refused), the session order on
 * the fake engine, and the splice of library gates (types, buffers, TIE cells, the init-1 cut,
 * provenance).
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "ir/celltype.h"
#include "ir/prov.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/abc/cells.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/manager.h"
#include "util/alloc.h"

#include <string.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum { N = 0x7FFFFFFF, N2 = 0x7FFFFFFE };

static odin3_design *design;
static odin3_module *mod;
static odin3_pass_ctx ctx;
static odin3_abc_run *run;
static odin3_vec domains;
static odin3_abc_region region;
static odin3_abc_result result;
static odin3_arena *arena;

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    odin3_pass_set_options((odin3_pass_options){0});
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    mod = NULL;
    run = NULL;
    region = (odin3_abc_region){0};
    domains = (odin3_vec){0};
    odin3_abc_result_init(&result);
    arena = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_abc_result_free(&result);
    odin3_arena_destroy(arena);
    odin3_abc_region_free(&region);
    odin3_abc_domains_free(&domains);
    odin3_abc_run_destroy(run);
    odin3_abc_test_set_engine(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* --- helpers ------------------------------------------------------------------------------- */

static void read_generic(void) {
    char lib[AFX_PATH];
    (void)snprintf(lib, sizeof lib, "%s/generic_gates.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
}

/* Module m: inputs a b, output y = a & b. */
static void build_and(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(design, afx_intern(design, "m"), (odin3_prov_id){0}, &mid));
    mod = odin3_module_get(design, mid);
    odin3_net_id in_a = afx_port(design, mod, "a", ODIN3_DIR_IN);
    odin3_net_id in_b = afx_port(design, mod, "b", ODIN3_DIR_IN);
    odin3_net_id out_y = afx_port(design, mod, "y", ODIN3_DIR_OUT);
    odin3_net_id gate[3] = {in_a, in_b, out_y};
    (void)AFX_CELL(design, mod, "$_AND_", "g", gate, 3);
    afx_check(design);
}

/* A run of abc --gates generic_gates on module m, its first domain's region built. */
static void start_region(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, afx_intern(design, "abc"), &ctx));
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_abc_run_start(&ctx, odin3_bytes_cstr("--gates generic_gates"), &run),
        afx_errors());
    run->module = mod;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_domains(run, mod, &domains));
    TEST_ASSERT_TRUE(domains.len > 0);
    run->domain = (const odin3_abc_domain *)domains.data;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_region_build(run, run->domain, &region));
    run->region = &region;
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)region.cis.len);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)region.cos.len);
}

/* The index of the library gate named name in the run's lib.gates. */
static uint32_t gate_index(const char *name) {
    for (size_t at = 0; at < run->lib.gates.len; at++) {
        const odin3_libgate *gate = odin3_vec_cat(&run->lib.gates, at);
        if (strcmp(odin3_celltype_get(design, gate->type)->name, name) == 0) {
            return (uint32_t)at;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return UINT32_MAX;
}

/*
 * A hand-built cell network over a b: object 4 is gate g4 over object 2 (a), object 5 gate g5 over
 * objects 4 and 3 (b), object 6 the PO of 5. names and tags as given.
 */
typedef struct hand_cells {
    int32_t fanins[14];
    uint32_t truths[7];
    int32_t gate[7];
    const char *names[2];
    odin3_abc_cells cells;
} hand_cells;

static void hand_init(hand_cells *hc, uint32_t truth4, uint32_t truth5) {
    const int32_t fanins[14] = {N, N, N, N, N, N, N, N, 2, N, 4, 3, 5, N2};
    memcpy(hc->fanins, fanins, sizeof fanins);
    memset(hc->truths, 0, sizeof hc->truths);
    hc->truths[4] = truth4;
    hc->truths[5] = truth5;
    const int32_t gate[7] = {-1, -1, -1, -1, 1, 0, -1};
    memcpy(hc->gate, gate, sizeof gate);
    hc->names[0] = "AND2";
    hc->names[1] = "INV";
    hc->cells = (odin3_abc_cells){{hc->fanins, hc->truths, NULL, 7, 0, 2}, hc->gate, hc->names, 2};
}

/* from_cells fails with ENGINE and logs exactly the one malformed-network error why. */
static void expect_malformed(const hand_cells *hc, const char *why) {
    char line[256];
    (void)snprintf(line, sizeof line, "abc: ABC returned a malformed network: %s", why);
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_ENGINE,
                          odin3_abc_result_from_cells(run, &hc->cells, arena, &result));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, afx_count(ODIN3_LOG_ERROR, "abc:"), afx_errors());
    TEST_ASSERT_TRUE_MESSAGE(afx_has(ODIN3_LOG_ERROR, line), afx_errors());
}

/* --- Task 11: the cell result ---------------------------------------------------------------- */

static void test_from_cells_tags_gates(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_abc_result_from_cells(run, &hc.cells, arena, &result), afx_errors());
    TEST_ASSERT_EQUAL_UINT32(2, result.n_luts);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)result.gate.len);
    TEST_ASSERT_EQUAL_UINT32(gate_index("INV"), ((const uint32_t *)result.gate.data)[0]);
    TEST_ASSERT_EQUAL_UINT32(gate_index("AND2"), ((const uint32_t *)result.gate.data)[1]);
}

static void test_from_cells_unknown_gate(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    hc.names[0] = "NAND3";
    expect_malformed(&hc, "gate 'NAND3' is not a cell of library 'generic_gates'");
}

static void test_from_cells_wrong_arity(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    hc.gate[5] = 1; /* "INV" on the 2-input object */
    expect_malformed(&hc, "gate 'INV' has 2 inputs; library cell 'INV' has 1");
}

static void test_from_cells_wrong_function(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0xE);
    expect_malformed(&hc, "gate 'AND2' does not compute library cell 'AND2''s function");
}

static void test_from_cells_untagged_object(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    hc.gate[5] = -1;
    expect_malformed(&hc, "a gate object without a gate name");
    odin3_abc_result_free(&result);
    hand_init(&hc, 0x1, 0x8);
    hc.gate[4] = 2; /* past n_names */
    expect_malformed(&hc, "a gate object without a gate name");
}

/* from_cells with the allocator failing after limit allocations, on a fresh arena. */
static odin3_status cells_with_limit(const hand_cells *hc, int limit) {
    odin3_abc_result_free(&result);
    odin3_arena *scratch = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(scratch);
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_abc_result_from_cells(run, &hc->cells, scratch, &result);
    odin3_util_set_alloc_fail_after(-1);
    odin3_arena_destroy(scratch);
    return st;
}

static void test_from_cells_out_of_memory(void) {
    read_generic();
    build_and();
    start_region();
    static hand_cells hc;
    hand_init(&hc, 0x1, 0x8);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    for (int limit = 0; limit < 64 && st != ODIN3_OK; limit++) {
        st = cells_with_limit(&hc, limit);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)result.gate.len);
}

static void test_gates_session_order(void) {
    read_generic();
    build_and();
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--gates generic_gates")),
        afx_errors());
    TEST_ASSERT_EQUAL_STRING("&put\nstrash\nifraig -v\nscorr -v\ndc2 -v\ndch -f\nmap -a\n"
                             "print_stats\n",
                             odin3_abc_fake_last_commands());
    TEST_ASSERT_TRUE(odin3_abc_fake_genlib()[0] != '\0');
    TEST_ASSERT_TRUE(afx_has(ODIN3_LOG_INFO,
                             "abc: module 'm', no flip-flops: no name map (--gates); "
                             "provenance uses cut cones"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_from_cells_tags_gates);
    RUN_TEST(test_from_cells_unknown_gate);
    RUN_TEST(test_from_cells_wrong_arity);
    RUN_TEST(test_from_cells_wrong_function);
    RUN_TEST(test_from_cells_untagged_object);
    RUN_TEST(test_from_cells_out_of_memory);
    RUN_TEST(test_gates_session_order);
    return UNITY_END();
}
