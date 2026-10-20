/*
 * test_abc_report.c - the per-run report of the abc pass (3D plan Task 22; spec §12): phase times,
 * counts, name-map coverage and provenance edges, read through abc_test.h's report hook as
 * bench_abc reads them. The fake engine runs both exchanges; the figures that need real ABC are
 * checked by bench_abc_smoke.
 */
#include "abc_fake_engine.h"
#include "abc_fixture.h"
#include "passes/abc/abc.h"
#include "passes/abc/abc_test.h"
#include "passes/manager.h"

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the repository"
#endif
#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif

enum { MAX_REPORTS = 8 };

typedef struct collected {
    odin3_abc_run_report reports[MAX_REPORTS];
    uint32_t count;
} collected;

static odin3_design *design;
static collected got;

static void on_report(const odin3_abc_run_report *report, void *user) {
    collected *into = user;
    if (into->count < MAX_REPORTS) {
        into->reports[into->count++] = *report;
    }
}

void setUp(void) {
    afx_log_reset();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
    odin3_abc_fake_reset();
    odin3_abc_test_set_engine(odin3_abc_fake_engine());
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
    got = (collected){0};
    odin3_abc_test_set_report(on_report, &got);
    char tmp[AFX_PATH];
    afx_path(tmp, "tmp");
    afx_mkdir(tmp);
    TEST_ASSERT_EQUAL_INT(0, setenv("TMPDIR", tmp, 1));
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_abc_test_set_report(NULL, NULL);
    odin3_abc_test_set_engine(NULL);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
}

/* tests/unit/data/abc/<name>.blif, with the VTR tech library read first. */
static void read_data(const char *name) {
    char lib[AFX_PATH];
    char path[AFX_JOIN];
    (void)snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR);
    (void)snprintf(path, sizeof path, "%s/tests/unit/data/abc/%s.blif", ODIN3_SOURCE_DIR, name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(lib)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(design, path), path);
}

static void run_abc(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_pass_run(design, "abc", odin3_bytes_cstr("--lut 6")), afx_errors());
}

static void assert_times_are_numbers(const odin3_abc_run_report *rep) {
    const double times[] = {rep->classify_ms, rep->aig_ms, rep->check_ms,  rep->splice_ms,
                            rep->prov_ms,     rep->abc_ms, rep->namemap_ms};
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        TEST_ASSERT_TRUE(times[i] >= 0.0 && times[i] < 3600000.0);
    }
}

static void assert_blif_counts(const odin3_abc_run_report *rep);

static void test_blif_exchange_report(void) {
    read_data("keep_net");
    run_abc();
    TEST_ASSERT_EQUAL_UINT32(1, got.count);
    const odin3_abc_run_report *rep = &got.reports[0];
    assert_blif_counts(rep);
    assert_times_are_numbers(rep);
}

static void assert_blif_counts(const odin3_abc_run_report *rep) {
    TEST_ASSERT_EQUAL_STRING("keep_net", rep->module);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_EXCHANGE_BLIF, rep->exchange);
    TEST_ASSERT_EQUAL_UINT32(0, rep->run_no);
    TEST_ASSERT_TRUE(rep->cells >= 2);
    TEST_ASSERT_TRUE(rep->luts >= 1 && rep->depth >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, rep->aig_objs + rep->namemap_total); /* no AIG, no name map */
    TEST_ASSERT_TRUE(rep->edges > 0);
}

static void test_aig_exchange_report(void) {
    TEST_ASSERT_EQUAL_INT(0, setenv("ODIN3_ABC_EXCHANGE", "aig", 1));
    read_data("keep_net");
    run_abc();
    TEST_ASSERT_EQUAL_UINT32(1, got.count);
    const odin3_abc_run_report *rep = &got.reports[0];
    TEST_ASSERT_EQUAL_UINT32(ODIN3_ABC_EXCHANGE_AIG, rep->exchange);
    TEST_ASSERT_TRUE(rep->aig_objs >= 2);
    TEST_ASSERT_TRUE(rep->namemap_hit <= rep->namemap_total);
    TEST_ASSERT_TRUE(rep->edges > 0);
    assert_times_are_numbers(rep);
    TEST_ASSERT_EQUAL_INT(0, unsetenv("ODIN3_ABC_EXCHANGE"));
}

static void test_one_report_per_clock_domain_with_a_run_number(void) {
    read_data("derived_clock");
    run_abc();
    TEST_ASSERT_TRUE(got.count >= 2);
    for (uint32_t i = 0; i < got.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, got.reports[i].run_no);
        TEST_ASSERT_TRUE(got.reports[i].domain[0] != '\0');
    }
}

enum { BLIF_TEXT = 1 << 16 };

/* keep_net through `abc`, then odin3_blif_write to name in the test's tmp: the file's text. */
static void abc_then_blif(const char *file, char *text) {
    char path[AFX_JOIN];
    afx_path(path, file);
    read_data("keep_net");
    run_abc();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(design, path));
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t len = fread(text, 1U, BLIF_TEXT - 1U, f);
    (void)fclose(f);
    text[len] = '\0';
    TEST_ASSERT_TRUE(len > 0);
}

/* The hook is only a reader: the mapped design is byte-identical with it set and unset. */
static void test_the_hook_does_not_change_the_result(void) {
    static char with_hook[BLIF_TEXT];
    static char without_hook[BLIF_TEXT];
    abc_then_blif("with_hook.blif", with_hook);
    TEST_ASSERT_EQUAL_UINT32(1, got.count);
    odin3_design_destroy(design);
    design = odin3_design_create();
    odin3_abc_fake_reset();
    odin3_abc_test_set_report(NULL, NULL);
    abc_then_blif("without_hook.blif", without_hook);
    TEST_ASSERT_EQUAL_UINT32(1, got.count); /* the hook is off: nothing new arrived */
    TEST_ASSERT_EQUAL_STRING(with_hook, without_hook);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_blif_exchange_report);
    RUN_TEST(test_aig_exchange_report);
    RUN_TEST(test_one_report_per_clock_domain_with_a_run_number);
    RUN_TEST(test_the_hook_does_not_change_the_result);
    return UNITY_END();
}
