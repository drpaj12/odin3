/*
 * test_opt_ffbits.c — `opt_merge -ffbits` (ffbits.c; PHASE3 #48): bits of word FFs with the same
 * clock, D bit, enable, sync reset, polarities, reset value and init merge (Yosys 0.55 gets this
 * from techmap and opt_merge); a differing init, enable, reset, reset value or polarity keeps them
 * apart. Also: a duplicate bit leaves a narrower word cell, cross-node bits, cascades, `keep`,
 * -keepdc, provenance, the option being off by default, and out of memory.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/ffinit.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 4000 };

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_module *module;
} fixture;

static fixture fx;

void setUp(void) {
    memset(&fx, 0, sizeof fx);
    fx.opts.ffbits = true;
    odin3_log_set_level(ODIN3_LOG_ERROR);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
    odin3_log_set_level(ODIN3_LOG_INFO);
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void build(const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx.res));
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.ctx));
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fx.res.design, opts));
}

static void run_merge(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_merge_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts));
    check_full();
}

static odin3_net_id wbit(const char *name, uint32_t idx) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, idx);
}

static bool same_net(const char *left, uint32_t li, const char *right, uint32_t ri) {
    return wbit(left, li).v == wbit(right, ri).v;
}

static uint32_t count_type(const char *type) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.module, node)) {
            const odin3_celltype_def *def =
                odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
            hits += strcmp(def->name, type) == 0 ? 1U : 0U;
        }
    }
    return hits;
}

/* The Q width of the first live node of type `type`, 0 when none. */
static uint32_t width_of(const char *type) {
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(fx.module, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
        for (uint32_t port = 0; strcmp(def->name, type) == 0 && port < def->n_ports; port++) {
            if (def->ports[port].dir == ODIN3_DIR_OUT) {
                return odin3_node_port(fx.module, node, port).count;
            }
        }
    }
    return 0;
}

static odin3_node_id node_named(const char *name) {
    return odin3_module_find_node(fx.module, intern(name));
}

static uint32_t live_named(const char *name) {
    return odin3_node_live(fx.module, node_named(name)) ? 1U : 0U;
}

#define HEAD                                                                                       \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\a\n  wire input 3 \\b\n"                   \
    "  wire input 4 \\en\n  wire input 5 \\en2\n  wire input 6 \\rst\n  wire input 7 \\rst2\n"
#define DFFE(name, pol, en, d, q)                                                                  \
    "  cell $dffe \\" name "\n    parameter \\WIDTH 2\n    parameter \\CLK_POLARITY " pol          \
    "\n    parameter \\EN_POLARITY 1\n    connect \\CLK \\clk\n    connect \\EN \\" en "\n"        \
    "    connect \\D " d "\n    connect \\Q " q "\n  end\n"
#define SDFFE(name, rv, rst, en, d, q)                                                             \
    "  cell $sdffe \\" name "\n    parameter \\WIDTH 2\n    parameter \\CLK_POLARITY 1\n"          \
    "    parameter \\EN_POLARITY 1\n    parameter \\SRST_POLARITY 1\n"                             \
    "    parameter \\SRST_VALUE 2'" rv "\n    connect \\CLK \\clk\n    connect \\SRST \\" rst      \
    "\n    connect \\EN \\" en "\n    connect \\D " d "\n    connect \\Q " q "\n  end\n"

/* --- the merge ---------------------------------------------------------------------------------
 */

static void test_equal_bits_in_one_word(void) {
    build(HEAD "  wire width 4 output 8 \\q\n"
               "  cell $dffe \\f\n    parameter \\WIDTH 4\n    parameter \\CLK_POLARITY 1\n"
               "    parameter \\EN_POLARITY 1\n    connect \\CLK \\clk\n    connect \\EN \\en\n"
               "    connect \\D { \\a \\b \\a \\b }\n    connect \\Q \\q\n  end\nend\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.merge_removed);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$dffe"));
    TEST_ASSERT_EQUAL_UINT32(2, width_of("$dffe"));
    TEST_ASSERT_TRUE(same_net("q", 0, "q", 2));
    TEST_ASSERT_TRUE(same_net("q", 1, "q", 3));
    TEST_ASSERT_FALSE(same_net("q", 0, "q", 1));
}

static void test_cross_node_bits(void) {
    build(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" DFFE(
        "f1", "1", "en", "{ \\a \\b }", "\\y") DFFE("f2", "1", "en", "{ \\b \\a }", "\\z") "end\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.merge_removed);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$dffe"));
    TEST_ASSERT_TRUE(same_net("y", 0, "z", 1));
    TEST_ASSERT_TRUE(same_net("y", 1, "z", 0));
    TEST_ASSERT_EQUAL_UINT32(1, live_named("f1") + live_named("f2"));
    TEST_ASSERT_EQUAL_UINT32(1, live_named("f1"));
}

static void test_sdffe_reset_value_per_bit(void) {
    /* f1 resets both bits to 0, f2 to 1: the same D and controls, other reset values */
    build(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" SDFFE(
        "f1", "00", "rst", "en", "{ \\a \\a }", "\\y")
              SDFFE("f2", "11", "rst", "en", "{ \\a \\a }", "\\z") "end\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.merge_removed); /* a pair in each word */
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$sdffe"));
    TEST_ASSERT_EQUAL_UINT32(1, width_of("$sdffe"));
}

static void test_sdffe_equal_bits(void) {
    build(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" SDFFE(
        "f1", "11", "rst", "en", "{ \\a \\a }", "\\y")
              SDFFE("f2", "11", "rst", "en", "{ \\a \\a }", "\\z") "end\n");
    run_merge();
    /* opt_merge joins the identical words (1), then the two equal bits of the word (1) */
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.merge_removed);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$sdffe"));
    TEST_ASSERT_EQUAL_UINT32(1, width_of("$sdffe"));
}

/* --- what must stay apart ----------------------------------------------------------------------
 */

static void expect_apart(const char *text) {
    build(text);
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.merge_removed);
}

static void test_other_enable_stays_apart(void) {
    expect_apart(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" DFFE(
        "f1", "1", "en", "{ \\a \\b }", "\\y")
                     DFFE("f2", "1", "en2", "{ \\a \\b }", "\\z") "end\n");
}

static void test_other_reset_stays_apart(void) {
    expect_apart(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" SDFFE(
        "f1", "11", "rst", "en", "{ \\a \\b }", "\\y")
                     SDFFE("f2", "11", "rst2", "en", "{ \\a \\b }", "\\z") "end\n");
}

static void test_other_polarity_stays_apart(void) {
    expect_apart(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" DFFE(
        "f1", "1", "en", "{ \\a \\b }", "\\y") DFFE("f2", "0", "en", "{ \\a \\b }", "\\z") "end\n");
}

static void test_other_init_stays_apart(void) {
    expect_apart(HEAD "  attribute \\init 2'01\n  wire width 2 output 8 \\y\n"
                      "  attribute \\init 2'10\n  wire width 2 output 9 \\z\n" DFFE(
                          "f1", "1", "en", "{ \\a \\b }", "\\y")
                          DFFE("f2", "1", "en", "{ \\a \\b }", "\\z") "end\n");
}

static void test_equal_init_merges_and_keeps_it(void) {
    build(HEAD "  attribute \\init 2'11\n  wire width 2 output 8 \\y\n"
               "  attribute \\init 2'11\n  wire width 2 output 9 \\z\n" DFFE("f1", "1", "en",
                                                                             "{ \\a \\b }", "\\y")
                   DFFE("f2", "1", "en", "{ \\b \\a }", "\\z") "end\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.merge_removed);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("y", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wbit("y", 1)));
}

static void test_keepdc_and_x_init(void) {
    fx.opts.keepdc = true;
    expect_apart(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" DFFE(
        "f1", "1", "en", "{ \\a \\b }", "\\y") DFFE("f2", "1", "en", "{ \\a \\b }", "\\z") "end\n");
}

static void test_option_off_by_default(void) {
    fx.opts.ffbits = false;
    build(HEAD "  wire width 4 output 8 \\q\n"
               "  cell $dffe \\f\n    parameter \\WIDTH 4\n    parameter \\CLK_POLARITY 1\n"
               "    parameter \\EN_POLARITY 1\n    connect \\CLK \\clk\n    connect \\EN \\en\n"
               "    connect \\D { \\a \\b \\a \\b }\n    connect \\Q \\q\n  end\nend\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.merge_removed);
    TEST_ASSERT_EQUAL_UINT32(4, width_of("$dffe"));
}

/* --- cascade, keep, provenance -----------------------------------------------------------------
 */

static void test_cascade(void) {
    /* f1 and f2 merge; then g1 (D = f1.Q) and g2 (D = f2.Q) read one net and merge */
    build(HEAD "  wire output 8 \\y\n  wire output 9 \\z\n  wire \\m1\n  wire \\m2\n"
               "  cell $dff \\f1\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
               "    connect \\CLK \\clk\n    connect \\D \\a\n    connect \\Q \\m1\n  end\n"
               "  cell $dff \\f2\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
               "    connect \\CLK \\clk\n    connect \\D \\a\n    connect \\Q \\m2\n  end\n"
               "  cell $dff \\g1\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
               "    connect \\CLK \\clk\n    connect \\D \\m1\n    connect \\Q \\y\n  end\n"
               "  cell $dff \\g2\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
               "    connect \\CLK \\clk\n    connect \\D \\m2\n    connect \\Q \\z\n  end\nend\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, count_type("$dff"));
    TEST_ASSERT_TRUE(same_net("y", 0, "z", 0));
}

static void test_keep_node_survives(void) {
    build(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" DFFE(
        "f1", "1", "en", "{ \\a \\b }",
        "\\y") "  attribute \\keep 1\n" DFFE("f2", "1", "en", "{ \\b \\a }", "\\z") "end\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(2, fx.counts.merge_removed);
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, node_named("f1")));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node_named("f2")));
}

static bool has_parent(odin3_prov_id rec, const odin3_prov_id *parent) {
    odin3_prov_list list = odin3_prov_parents(fx.res.design, rec);
    for (uint32_t i = 0; i < list.count; i++) {
        if (list.ids[i].v == parent->v) {
            return true;
        }
    }
    return false;
}

static void test_provenance(void) {
    build(HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" DFFE(
        "f1", "1", "en", "{ \\a \\b }", "\\y") DFFE("f2", "1", "en", "{ \\b \\a }", "\\z") "end\n");
    odin3_prov_id own = odin3_node_prov(fx.module, node_named("f1"));
    odin3_prov_id gone = odin3_node_prov(fx.module, node_named("f2"));
    run_merge();
    odin3_prov_id now = odin3_node_prov(fx.module, node_named("f1"));
    TEST_ASSERT_TRUE(now.v != own.v);
    TEST_ASSERT_TRUE(has_parent(now, &own));
    TEST_ASSERT_TRUE(has_parent(now, &gone));
}

static void test_partial_drop_keeps_word_cell(void) {
    build(HEAD "  wire width 3 output 8 \\q\n"
               "  cell $dffe \\f\n    parameter \\WIDTH 3\n    parameter \\CLK_POLARITY 1\n"
               "    parameter \\EN_POLARITY 1\n    connect \\CLK \\clk\n    connect \\EN \\en\n"
               "    connect \\D { \\a \\b \\a }\n    connect \\Q \\q\n  end\nend\n");
    run_merge();
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.merge_removed);
    TEST_ASSERT_EQUAL_UINT32(2, width_of("$dffe"));
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, node_named("f")));
    TEST_ASSERT_TRUE(same_net("q", 0, "q", 2));
}

/* --- `opt -ffbits` ------------------------------------------------------------------------------
 */

static void test_opt_option(void) {
    odin3_opt_args args;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_parse_args("opt", odin3_bytes_cstr("-full -ffbits"), &args));
    TEST_ASSERT_TRUE(args.opts.ffbits);
    TEST_ASSERT_TRUE(args.opts.full);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_parse_args("opt", odin3_bytes_cstr("-full"), &args));
    TEST_ASSERT_FALSE(args.opts.ffbits);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_parse_args("opt_expr", odin3_bytes_cstr("-ffbits"), &args));
}

static void test_opt_loop_merges_bits(void) {
    build(HEAD "  wire width 4 output 8 \\q\n"
               "  cell $dffe \\f\n    parameter \\WIDTH 4\n    parameter \\CLK_POLARITY 1\n"
               "    parameter \\EN_POLARITY 1\n    connect \\CLK \\clk\n    connect \\EN \\en\n"
               "    connect \\D { \\a \\b \\a \\b }\n    connect \\Q \\q\n  end\nend\n");
    odin3_opt_args args;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_parse_args("opt", odin3_bytes_cstr("-ffbits"), &args));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_run(&fx.ctx, fx.res.design, &args, &fx.counts));
    check_full();
    TEST_ASSERT_EQUAL_UINT32(2, width_of("$dffe"));
    TEST_ASSERT_TRUE(same_net("q", 0, "q", 2));
}

/* --- out of memory -----------------------------------------------------------------------------
 */

#define OOM_TEXT                                                                                   \
    HEAD "  wire width 2 output 8 \\y\n  wire width 2 output 9 \\z\n" SDFFE(                       \
        "f1", "11", "rst", "en", "{ \\a \\b }", "\\y")                                             \
        SDFFE("f2", "11", "rst", "en", "{ \\b \\a }", "\\z") "end\n"

/* One run with the nth allocation failing; true when it ran through. */
static bool oom_once(long fail) {
    pb_free(&fx.res);
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_merge_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    check_full();
    return st == ODIN3_OK;
}

static void test_oom(void) {
    long failures = 0;
    long fail = 0;
    while (fail < OOM_LIMIT && !oom_once(fail)) {
        failures++;
        fail++;
    }
    TEST_ASSERT_TRUE(fail < OOM_LIMIT);
    TEST_ASSERT_TRUE(failures > 5);
    TEST_ASSERT_EQUAL_UINT32(1, count_type("$sdffe"));
    printf("opt_merge -ffbits OOM: %ld failing allocation points\n", failures);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_equal_bits_in_one_word);
    RUN_TEST(test_cross_node_bits);
    RUN_TEST(test_sdffe_reset_value_per_bit);
    RUN_TEST(test_sdffe_equal_bits);
    RUN_TEST(test_other_enable_stays_apart);
    RUN_TEST(test_other_reset_stays_apart);
    RUN_TEST(test_other_polarity_stays_apart);
    RUN_TEST(test_other_init_stays_apart);
    RUN_TEST(test_equal_init_merges_and_keeps_it);
    RUN_TEST(test_keepdc_and_x_init);
    RUN_TEST(test_option_off_by_default);
    RUN_TEST(test_cascade);
    RUN_TEST(test_keep_node_survives);
    RUN_TEST(test_provenance);
    RUN_TEST(test_partial_drop_keeps_word_cell);
    RUN_TEST(test_opt_option);
    RUN_TEST(test_opt_loop_merges_bits);
    RUN_TEST(test_oom);
    return UNITY_END();
}
