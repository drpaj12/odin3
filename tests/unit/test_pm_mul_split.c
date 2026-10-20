/*
 * test_pm_mul_split.c — partial_map 4.3 oversize multiplier split (4.2 spec X-4, X-6, X-8, X-10;
 * plan Task 14). A `$mul` wider than W is rewritten into smaller `$mul` and `$add` cells that the
 * ordinary rules map. The multiply and adder-cell counts and the chain lengths equal the Parmys
 * probe table of tests/partial_map/probes/README.md; the result simulates equal to the product
 * (1E); a signed oversize cell stays soft; Y_WIDTH below a + b is respected.
 */
#include "frontends/arch/arch.h"
#include "hardmap_fixture.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hardmap.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LOG_TEXT = 65536,
    NAME_CAP = 1024,
    W27 = 27,
    W36 = 36,
    MAX_CHAINS = 16,
    SIM_CYCLES = 24,
    SIM_SEED = 7,
    BIG_A = 4000,
    BIG_B = 3,
    OOM_LIMIT = 6000,
    OOM_QUIET = 4
};

#ifndef ODIN3_ARCH_DATA_DIR
#error "ODIN3_ARCH_DATA_DIR must name tests/unit/data/arch"
#endif

static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

/* --- builders ------------------------------------------------------------------------------ */

typedef struct mulcase {
    uint32_t a;
    uint32_t b;
    uint32_t y;
    bool is_signed;      /* both operands signed */
    const char *b_const; /* NULL: B is the input wire; else its RTLIL constant */
} mulcase;

static void mul_text(odin3_strbuf *out, mulcase mc) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(out, "module \\m\n  wire width %" PRIu32 " input 1 \\a\n", mc.a));
    if (mc.b_const == NULL) {
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(out, "  wire width %" PRIu32 " input 2 \\b\n", mc.b));
    }
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(out,
                             "  wire width %" PRIu32 " output 3 \\y\n  cell $mul \\u\n"
                             "    parameter \\A_SIGNED %d\n    parameter \\B_SIGNED %d\n"
                             "    parameter \\A_WIDTH %" PRIu32 "\n    parameter \\B_WIDTH %" PRIu32
                             "\n    parameter \\Y_WIDTH %" PRIu32
                             "\n    connect \\A \\a\n    connect \\B %s\n    connect \\Y \\y\n"
                             "  end\nend\n",
                             mc.y, mc.is_signed ? 1 : 0, mc.is_signed ? 1 : 0, mc.a, mc.b, mc.y,
                             mc.b_const != NULL ? mc.b_const : "\\b"));
}

static void explicit_text(odin3_strbuf *out, uint32_t width) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(
                      out,
                      "module \\m\n  wire width %" PRIu32 " input 1 \\a\n  wire width %" PRIu32
                      " input 2 \\b\n  wire width %" PRIu32
                      " output 3 \\y\n  cell \\multiply \\u\n    parameter \\WIDTH %" PRIu32
                      "\n    connect \\a \\a\n    connect \\b \\b\n    connect \\out \\y\n"
                      "  end\nend\n",
                      width, width, 2U * width, width));
}

/* The RTLIL text and the fixture record to read (fixture NULL: vtr.o3lib). */
typedef struct text_in {
    const char *text;
    const char *fixture;
} text_in;

static odin3_status build_text(text_in in, pb_result *res) {
    char path[NAME_CAP];
    odin3_status st = ODIN3_OK;
    if (in.fixture == NULL) {
        st = hm_build(in.text, res);
    } else {
        (void)snprintf(path, sizeof path, "%s/%s", ODIN3_ARCH_DATA_DIR, in.fixture);
        st = pb_build_lib((pb_source){in.text, path}, res); /* read_arch: library and record */
    }
    return st == ODIN3_OK ? odin3_design_set_top(res->design, odin3_module_id_of(res->module)) : st;
}

static void build_mul(mulcase mc, const char *fixture, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_text(&text, mc);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_text((text_in){text.data, fixture}, res));
    odin3_strbuf_free(&text);
}

/* The case and fixture hm_sim_equal's builder reads. */
static mulcase g_case;
static const char *g_fixture;
static bool g_explicit;

static odin3_status build_global(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    if (g_explicit) {
        explicit_text(&text, g_case.a);
    } else {
        mul_text(&text, g_case);
    }
    odin3_status st = build_text((text_in){text.data, g_fixture}, out);
    odin3_strbuf_free(&text);
    return st;
}

static bool sim_equal(mulcase mc, const char *fixture) {
    g_case = mc;
    g_fixture = fixture;
    g_explicit = false;
    return hm_sim_equal(build_global, SIM_CYCLES, SIM_SEED);
}

/* --- chain lengths -------------------------------------------------------------------------- */

typedef struct chains {
    uint32_t count;
    uint32_t cells[MAX_CHAINS];
} chains;

/* The adder whose cin is net, or none (a linear scan: the test designs are small). */
static odin3_node_id adder_with_cin(const pb_result *res, odin3_net_id net) {
    for (uint32_t i = 0;; i++) {
        odin3_node_id node = hm_nth(res->module, "adder", i);
        if (!odin3_node_valid(node)) {
            return node;
        }
        odin3_pinslice pins = odin3_node_port(res->module, node, 2);
        if (odin3_pin_net(res->module, pins.first).v == net.v) {
            return node;
        }
    }
}

/* The cells of the chain that starts at the head `node`: through cout -> cin. */
static uint32_t chain_cells(const pb_result *res, odin3_node_id node) {
    uint32_t len = 0;
    while (odin3_node_valid(node)) {
        len++;
        odin3_net_id cout = odin3_pin_net(res->module, odin3_node_port(res->module, node, 3).first);
        node = adder_with_cin(res, cout);
    }
    return len;
}

static void sort_longest_first(chains *out) {
    for (uint32_t i = 0; i < out->count; i++) {
        for (uint32_t j = i + 1; j < out->count; j++) {
            if (out->cells[j] > out->cells[i]) {
                uint32_t tmp = out->cells[i];
                out->cells[i] = out->cells[j];
                out->cells[j] = tmp;
            }
        }
    }
}

/* The lengths in cells of the chains of the module: a head is an adder whose cin is a constant.
 * Sorted, longest first. */
static void chain_lengths(const pb_result *res, chains *out) {
    out->count = 0;
    for (uint32_t i = 0; odin3_node_valid(hm_nth(res->module, "adder", i)); i++) {
        odin3_node_id node = hm_nth(res->module, "adder", i);
        odin3_net_id cin = odin3_pin_net(res->module, odin3_node_port(res->module, node, 2).first);
        if (odin3_net_const_value(res->module, cin) != ODIN3_CONST_NONE) {
            TEST_ASSERT_TRUE(out->count < MAX_CHAINS);
            out->cells[out->count++] = chain_cells(res, node);
        }
    }
    sort_longest_first(out);
}

/* --- the probe table (tests/partial_map/probes/README.md) ----------------------------------- */

typedef struct probe_row {
    uint32_t a;
    uint32_t b;
    const char *fixture; /* NULL: k6, W = 36; else COFFE-like, W = 27 */
    uint32_t multiplies;
    uint32_t adder_cells;
    uint32_t n_chains;
    uint32_t chain[6];
} probe_row;

static const probe_row k_probe[] = {
    {40, 40, NULL, 4, 87, 2, {45, 42}},
    {40, 8, NULL, 2, 13, 1, {13}},
    {8, 40, NULL, 2, 45, 1, {45}},
    {80, 80, NULL, 9, 424, 6, {125, 82, 73, 53, 46, 45}},
    {40, 40, "acc_27.xml", 4, 96, 2, {54, 42}},
    {40, 8, "acc_27.xml", 2, 22, 1, {22}},
    {8, 40, "acc_27.xml", 2, 36, 1, {36}},
    {80, 80, "acc_27.xml", 9, 460, 6, {134, 82, 80, 55, 55, 54}},
};

static void check_probe_row(const probe_row *row) {
    pb_result res;
    build_mul((mulcase){row->a, row->b, row->a + row->b, false, NULL}, row->fixture, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$add"));
    TEST_ASSERT_EQUAL_UINT32(row->multiplies, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(row->adder_cells, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT64(row->multiplies, stats.multiply);
    TEST_ASSERT_EQUAL_UINT64(row->adder_cells, stats.adder);
    TEST_ASSERT_EQUAL_UINT64(row->n_chains, stats.chains);
    chains found;
    chain_lengths(&res, &found);
    TEST_ASSERT_EQUAL_UINT32(row->n_chains, found.count);
    for (uint32_t i = 0; i < found.count; i++) {
        TEST_ASSERT_EQUAL_UINT32(row->chain[i], found.cells[i]);
    }
    pb_free(&res);
}

static void test_probe_table(void) {
    for (size_t i = 0; i < sizeof k_probe / sizeof k_probe[0]; i++) {
        check_probe_row(&k_probe[i]);
    }
}

/* --- the product (1E simulation) ------------------------------------------------------------ */

/* Every pair of sizes around the multiplier width `width` of the fixture, simulated. */
static void sim_grid(uint32_t width, const char *fixture) {
    const uint32_t sizes[] = {width - 1U, width, width + 1U, 2U * width, 2U * width + 1U, 80U};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (size_t j = 0; j < sizeof sizes / sizeof sizes[0]; j++) {
            char msg[64];
            (void)snprintf(msg, sizeof msg, "W %" PRIu32 " a %" PRIu32 " b %" PRIu32, width,
                           sizes[i], sizes[j]);
            mulcase mc = {sizes[i], sizes[j], sizes[i] + sizes[j], false, NULL};
            TEST_ASSERT_TRUE_MESSAGE(sim_equal(mc, fixture), msg);
        }
    }
}

static void test_sim_grid(void) {
    sim_grid(W36, NULL);
    sim_grid(W27, "acc_27.xml");
}

static void test_sim_corners(void) {
    static const char ones40[] = "40'1111111111111111111111111111111111111111";
    static const char zero40[] = "40'0000000000000000000000000000000000000000";
    static const char one40[] = "40'0000000000000000000000000000000000000001";
    static const char top40[] = "40'1000000000000000000000000000000000000000";
    const char *consts[] = {ones40, zero40, one40, top40};
    for (size_t i = 0; i < sizeof consts / sizeof consts[0]; i++) {
        TEST_ASSERT_TRUE(sim_equal((mulcase){40, 40, 80, false, consts[i]}, NULL));
        TEST_ASSERT_TRUE(sim_equal((mulcase){40, 40, 80, false, consts[i]}, "acc_27.xml"));
    }
}

static void test_sim_y_narrower(void) {
    static const mulcase k_cases[] = {
        {40, 40, 50, false, NULL}, {40, 8, 20, false, NULL}, {8, 40, 30, false, NULL}};
    static const mulcase k_cases_27[] = {{40, 40, 30, false, NULL}, {80, 40, 100, false, NULL}};
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        TEST_ASSERT_TRUE(sim_equal(k_cases[i], NULL));
    }
    for (size_t i = 0; i < sizeof k_cases_27 / sizeof k_cases_27[0]; i++) {
        TEST_ASSERT_TRUE(sim_equal(k_cases_27[i], "acc_27.xml"));
    }
}

/* --- signed, Y_WIDTH, names ----------------------------------------------------------------- */

static void test_signed_oversize_stays_soft(void) {
    pb_result res;
    build_mul((mulcase){40, 40, 80, true, NULL}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "multiply"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "signed '$mul' 'u' needs Y_WIDTH 80 > 36; left to lower"));
    pb_free(&res);
}

static void test_signed_oversize_small_y(void) {
    pb_result res;
    build_mul((mulcase){40, 4, 30, true, NULL}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "signed '$mul' 'u' is 40x4, wider than the arch's"));
    pb_free(&res);
}

static void test_y_wider_oversize_stays_soft(void) {
    pb_result res;
    build_mul((mulcase){40, 40, 100, false, NULL}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "above A_WIDTH + B_WIDTH (80)"));
    pb_free(&res);
}

/* The cell type name of the node called `name`, NULL when there is none. */
static const char *type_of_node(const pb_result *res, const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_node_id node = odin3_module_find_node(res->module, str);
    if (!odin3_node_valid(node)) {
        return NULL;
    }
    const odin3_celltype_def *def =
        odin3_celltype_get(res->design, odin3_node_type(res->module, node));
    return def != NULL ? def->name : NULL;
}

/* X-8: the first piece keeps the source's name, the others are `<cell>.<role>`. */
static void test_names(void) {
    static const char *const k_names[] = {"u", "u.m01", "u.m10", "u.m11", "u.add0", "u.add1"};
    static const char *const k_types[] = {"multiply", "multiply", "multiply",
                                          "multiply", "adder",    "adder"};
    pb_result res;
    build_mul((mulcase){40, 40, 80, false, NULL}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        const char *type = type_of_node(&res, k_names[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(type, k_names[i]);
        TEST_ASSERT_EQUAL_STRING(k_types[i], type);
    }
    pb_free(&res);
}

/* D19: `--parmys-split-ext` changes the add0 port (sign extension), not the counts; without it a
 * B split gives one warning per design. */
static void test_parmys_split_ext_option(void) {
    pb_result res;
    build_mul((mulcase){8, 40, 48, false, NULL}, NULL, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, &stats));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "1 '$mul' cells split on operand B alone"));
    TEST_ASSERT_EQUAL_UINT64(2, stats.multiply);
    pb_free(&res);
    log_text[0] = '\0';
    build_mul((mulcase){8, 40, 48, false, NULL}, NULL, &res);
    odin3_partial_map_opts opts = {.parmys_split_ext = true};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_partial_map_run_opts(&res.ctx, res.design, &opts, &stats));
    TEST_ASSERT_NULL(strstr(log_text, "split on operand B alone"));
    TEST_ASSERT_EQUAL_UINT64(2, stats.multiply);
    TEST_ASSERT_EQUAL_UINT64(45, stats.adder);
    pb_free(&res);
}

/* --- X-6: an explicit multiply wider than W ------------------------------------------------- */

static void test_explicit_40(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, 40);
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_text((text_in){text.data, NULL}, &res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_NOT_NULL(strstr(
        log_text, "explicit 'multiply' instance 'u' (WIDTH 40) treated as '$mul' as Parmys does"));
    TEST_ASSERT_EQUAL_UINT32(4, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(87, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    pb_free(&res);
    odin3_strbuf_free(&text);
}

static void test_explicit_40_sim(void) {
    g_case = (mulcase){40, 40, 80, false, NULL};
    g_fixture = NULL;
    g_explicit = true;
    TEST_ASSERT_TRUE(hm_sim_equal(build_global, SIM_CYCLES, SIM_SEED));
    g_fixture = "acc_27.xml";
    TEST_ASSERT_TRUE(hm_sim_equal(build_global, SIM_CYCLES, SIM_SEED));
}

/* --- X-10: a worklist, not recursion ---------------------------------------------------------- */

static void test_big_operand(void) {
    pb_result res;
    build_mul((mulcase){BIG_A, BIG_B, BIG_A + BIG_B, false, NULL}, "acc_27.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$add"));
    TEST_ASSERT_EQUAL_UINT32((BIG_A + W27 - 1U) / W27, hm_count(res.module, "multiply"));
    pb_free(&res);
}

/* --- H-9: out of memory (the source cell is intact) --------------------------------------------
 */

typedef struct snapshot {
    odin3_strbuf text;
    uint32_t nodes;
    uint32_t nets;
} snapshot;

static void snap_take(const pb_result *res, snapshot *snap) {
    odin3_strbuf_init(&snap->text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(res, &snap->text));
    snap->nodes = 0;
    snap->nets = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(res->module); id++) {
        snap->nodes += odin3_node_live(res->module, (odin3_node_id){id}) ? 1U : 0U;
    }
    for (uint32_t id = 1; id < odin3_module_net_end(res->module); id++) {
        snap->nets += odin3_net_live(res->module, (odin3_net_id){id}) ? 1U : 0U;
    }
}

static bool oom_try(const char *text, long fail_at) {
    pb_result res;
    snapshot before;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_text((text_in){text, NULL}, &res));
    snap_take(&res, &before);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
    odin3_util_set_alloc_fail_after(-1);
    bool injected = st != ODIN3_OK;
    if (injected) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        snapshot now;
        snap_take(&res, &now);
        if (hm_count(res.module, "$mul") == 1 && hm_count(res.module, "multiply") == 0) {
            /* the failure was in the split transaction: nothing changed */
            TEST_ASSERT_EQUAL_STRING(before.text.data, now.text.data);
            TEST_ASSERT_EQUAL_UINT32(before.nodes, now.nodes);
            TEST_ASSERT_EQUAL_UINT32(before.nets, now.nets);
        }
        odin3_strbuf_free(&now.text);
    } else {
        TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "multiply"));
    }
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
    odin3_strbuf_free(&before.text);
    pb_free(&res);
    return injected;
}

static void test_oom_sweep(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_text(&text, (mulcase){40, 8, 48, false, NULL});
    long failed = 0;
    long quiet = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT && quiet < OOM_QUIET; fail_at++) {
        bool injected = oom_try(text.data, fail_at);
        failed += injected ? 1 : 0;
        quiet = injected ? 0 : quiet + 1;
    }
    TEST_ASSERT_TRUE(quiet == OOM_QUIET);
    TEST_ASSERT_TRUE(failed > 1);
    odin3_strbuf_free(&text);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_probe_table);
    RUN_TEST(test_sim_grid);
    RUN_TEST(test_sim_corners);
    RUN_TEST(test_sim_y_narrower);
    RUN_TEST(test_signed_oversize_stays_soft);
    RUN_TEST(test_signed_oversize_small_y);
    RUN_TEST(test_y_wider_oversize_stays_soft);
    RUN_TEST(test_names);
    RUN_TEST(test_parmys_split_ext_option);
    RUN_TEST(test_explicit_40);
    RUN_TEST(test_explicit_40_sim);
    RUN_TEST(test_big_operand);
    RUN_TEST(test_oom_sweep);
    return UNITY_END();
}
