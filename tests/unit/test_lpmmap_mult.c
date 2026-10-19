/*
 * test_lpmmap_mult.c — lpmmap, plan Task 6: `$mul` to `lpm_mult` (5.1 spec M-1..M-5, H-6, H-8).
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "lpmmap_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 8192, TEXT_MAX = 8192, OOM_LIMIT = 4000, MAXW = 40 };

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

typedef struct mulcase {
    uint32_t a_width, b_width, y_width;
    bool a_signed, b_signed;
    const char *a_sig; /* NULL: the input wire \a */
    const char *b_sig; /* NULL: the input wire \b */
} mulcase;

static void build_one(mulcase mc, pb_result *res) {
    char text[TEXT_MAX];
    size_t at = 0;
    at += (size_t)snprintf(text + at, sizeof text - at, "module \\m\n");
    uint32_t port = 1;
    if (mc.a_sig == NULL) {
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "  wire width %" PRIu32 " input %" PRIu32 " \\a\n", mc.a_width,
                               port++);
    }
    if (mc.b_sig == NULL) {
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "  wire width %" PRIu32 " input %" PRIu32 " \\b\n", mc.b_width,
                               port++);
    }
    at += (size_t)snprintf(text + at, sizeof text - at,
                           "  wire width %" PRIu32 " output %" PRIu32 " \\y\n", mc.y_width, port);
    (void)snprintf(text + at, sizeof text - at,
                   "  cell $mul \\u0\n    parameter \\A_SIGNED %d\n    parameter \\B_SIGNED %d\n"
                   "    parameter \\A_WIDTH %" PRIu32 "\n    parameter \\B_WIDTH %" PRIu32
                   "\n    parameter \\Y_WIDTH %" PRIu32
                   "\n    connect \\A %s\n    connect \\B %s\n    connect \\Y \\y\n  end\nend\n",
                   mc.a_signed ? 1 : 0, mc.b_signed ? 1 : 0, mc.a_width, mc.b_width, mc.y_width,
                   mc.a_sig != NULL ? mc.a_sig : "\\a", mc.b_sig != NULL ? mc.b_sig : "\\b");
    lm_build_ok(text, res);
}

static bool is_const(const odin3_module *mod, odin3_net_id net, odin3_const value) {
    return odin3_net_valid(net) && odin3_net_const_value(mod, net) == value;
}

typedef struct before {
    odin3_net_id a[MAXW], b[MAXW], y[MAXW];
} before;

static void port_nets(const odin3_module *mod, odin3_node_id node, uint32_t port,
                      odin3_net_id *out) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    for (uint32_t i = 0; i < pins.count && i < MAXW; i++) {
        out[i] = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + i});
    }
}

static void snapshot(const pb_result *res, before *bf) {
    memset(bf, 0, sizeof *bf);
    odin3_node_id src = lm_nth(res->module, "$mul", 0);
    port_nets(res->module, src, 0, bf->a);
    port_nets(res->module, src, 1, bf->b);
    port_nets(res->module, src, 2, bf->y);
}

/* The attribute `lpmmap` of blk is expected. */
static void expect_lpmmap_attr(const pb_result *res, odin3_node_id blk, const char *expected) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(res->design), odin3_bytes_cstr("lpmmap"), &key));
    const odin3_value *val =
        odin3_attr_get(res->module, (odin3_objref){ODIN3_OBJ_NODE, blk.v}, key);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING(expected,
                             odin3_strtab_get(odin3_design_strtab(res->design), val->str));
}

/* --- M-1 ------------------------------------------------------------------------------------ */

static void test_m1_candidates(void) {
    static const mulcase cases[] = {{2, 2, 4, false, false, NULL, NULL},
                                    {1, 1, 2, false, false, NULL, NULL},
                                    {40, 40, 80, false, false, NULL, NULL},
                                    {8, 7, 15, false, false, NULL, "7'1010101"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pb_result res;
        build_one(cases[i], &res);
        odin3_lpmmap_stats stats;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
        TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_mult"));
        TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$mul"));
        TEST_ASSERT_EQUAL_UINT64(1, stats.mult);
        pb_free(&res);
    }
}

/* --- M-2 ------------------------------------------------------------------------------------ */

static void test_m2_full_product_width(void) {
    pb_result res;
    build_one((mulcase){8, 7, 15, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_mult", 0);
    TEST_ASSERT_EQUAL_INT64(8, lm_param_int(res.module, blk, "lpm_widtha"));
    TEST_ASSERT_EQUAL_INT64(7, lm_param_int(res.module, blk, "lpm_widthb"));
    TEST_ASSERT_EQUAL_INT64(15, lm_param_int(res.module, blk, "lpm_widthp"));
    TEST_ASSERT_EQUAL_UINT32(8, lm_port_width(res.module, blk, "dataa"));
    TEST_ASSERT_EQUAL_UINT32(7, lm_port_width(res.module, blk, "datab"));
    TEST_ASSERT_EQUAL_UINT32(15, lm_port_width(res.module, blk, "result"));
    for (uint32_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.a[i].v, lm_pin_net(res.module, blk, "dataa", i).v);
    }
    for (uint32_t i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.b[i].v, lm_pin_net(res.module, blk, "datab", i).v);
    }
    for (uint32_t i = 0; i < 15; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.y[i].v, lm_pin_net(res.module, blk, "result", i).v);
    }
    pb_free(&res);
}

static void test_m2_short_y_dangling_result_bits(void) {
    pb_result res;
    build_one((mulcase){8, 7, 10, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_mult", 0);
    TEST_ASSERT_EQUAL_INT64(15, lm_param_int(res.module, blk, "lpm_widthp"));
    for (uint32_t i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.y[i].v, lm_pin_net(res.module, blk, "result", i).v);
    }
    for (uint32_t i = 10; i < 15; i++) {
        odin3_net_id net = lm_pin_net(res.module, blk, "result", i);
        TEST_ASSERT_TRUE(odin3_net_valid(net));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_sinks(res.module, net).count);
    }
    pb_free(&res);
}

/* --- M-3 ------------------------------------------------------------------------------------ */

static void test_m3_signedness(void) {
    static const struct {
        bool a_sig, b_sig;
        const char *rep;
    } rows[] = {{true, true, "SIGNED"},
                {false, false, "UNSIGNED"},
                {true, false, "UNSIGNED"},
                {false, true, "UNSIGNED"}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        pb_result res;
        build_one((mulcase){4, 3, 7, rows[i].a_sig, rows[i].b_sig, NULL, NULL}, &res);
        before bf;
        snapshot(&res, &bf);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
        odin3_node_id blk = lm_nth(res.module, "lpm_mult", 0);
        TEST_ASSERT_EQUAL_STRING(rows[i].rep, lm_param_str(res.module, blk, "lpm_representation"));
        /* the operands are not extended: the block takes their own widths */
        TEST_ASSERT_EQUAL_UINT32(4, lm_port_width(res.module, blk, "dataa"));
        TEST_ASSERT_EQUAL_UINT32(3, lm_port_width(res.module, blk, "datab"));
        pb_free(&res);
    }
}

/* --- M-4 ------------------------------------------------------------------------------------ */

static void test_m4_y_wider_stays_soft(void) {
    pb_result res;
    build_one((mulcase){4, 4, 20, false, false, NULL, NULL}, &res);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "lpm_mult"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: '$mul' 'u0' has Y wider than A + B; left soft "
                                          "(run wreduce first)"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "1 cells left soft"));
    pb_free(&res);
}

/* --- M-5 ------------------------------------------------------------------------------------ */

/* Bit 0 of port `port` of blk is the constant `value`. */
static void expect_tied(const pb_result *res, odin3_node_id blk, const char *port,
                        odin3_const value) {
    TEST_ASSERT_TRUE_MESSAGE(is_const(res->module, lm_pin_net(res->module, blk, port, 0), value),
                             port);
}

static void test_m5_defaults_tied_and_params(void) {
    pb_result res;
    build_one((mulcase){4, 4, 8, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_mult", 0);
    /* M1: sum 0, clken 1, aclr 0, clock 0 */
    expect_tied(&res, blk, "sum", ODIN3_CONST_0);
    expect_tied(&res, blk, "clken", ODIN3_CONST_1);
    expect_tied(&res, blk, "aclr", ODIN3_CONST_0);
    expect_tied(&res, blk, "clock", ODIN3_CONST_0);
    TEST_ASSERT_EQUAL_INT64(0, lm_param_int(res.module, blk, "lpm_pipeline"));
    TEST_ASSERT_EQUAL_INT64(1, lm_param_int(res.module, blk, "lpm_widths"));
    TEST_ASSERT_EQUAL_STRING("UNUSED", lm_param_str(res.module, blk, "lpm_hint"));
    TEST_ASSERT_EQUAL_STRING("lpm_mult", lm_param_str(res.module, blk, "lpm_type"));
    pb_free(&res);
}

/* M2: an operand of 256 bits maps, 257 stays soft with a warning. */
static void test_m_width_cap(void) {
    pb_result res;
    build_one((mulcase){256, 2, 258, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_mult"));
    pb_free(&res);
    log_text[0] = '\0';
    build_one((mulcase){3, 257, 260, false, false, NULL, NULL}, &res);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: '$mul' 'u0' has width 257, above the 256 bits "
                                          "lpm_mult supports; left soft"));
    pb_free(&res);
}

static void test_m_prov_and_attr(void) {
    pb_result res;
    build_one((mulcase){4, 4, 8, false, false, NULL, NULL}, &res);
    odin3_prov_id source = odin3_node_prov(res.module, lm_nth(res.module, "$mul", 0));
    TEST_ASSERT_TRUE(source.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_mult", 0);
    const odin3_prov_record *rec = odin3_prov_get(res.design, odin3_node_prov(res.module, blk));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(source.v, rec->parents.ids[0].v);
    expect_lpmmap_attr(&res, blk, "lpm_mult:mul");
    pb_free(&res);
}

static void test_m_explicit_untouched(void) {
    static const char text[] =
        "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 8 output 3 \\y\n"
        "  cell \\lpm_mult \\u1\n    parameter \\lpm_widtha 4\n    parameter \\lpm_widthb 4\n"
        "    parameter \\lpm_widthp 8\n    connect \\dataa \\a\n    connect \\datab \\b\n"
        "    connect \\result \\y\n  end\nend\n";
    pb_result res;
    lm_build_ok(text, &res);
    odin3_node_id id = lm_nth(res.module, "lpm_mult", 0);
    TEST_ASSERT_TRUE(id.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(id.v, lm_nth(res.module, "lpm_mult", 0).v);
    pb_free(&res);
}

static void test_m_oom(void) {
    int mapped = 0;
    for (int fail_at = 0; fail_at < OOM_LIMIT && mapped == 0; fail_at++) {
        pb_result res;
        build_one((mulcase){6, 5, 11, true, true, NULL, NULL}, &res);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_lpmmap_run(&res.ctx, res.design, NULL, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            mapped = 1;
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_mult"));
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_NULL(strstr(log_text, "no longer matches"));
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$mul"));
            TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "lpm_mult"));
            odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
        }
        pb_free(&res);
    }
    TEST_ASSERT_EQUAL_INT(1, mapped);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_m1_candidates);
    RUN_TEST(test_m2_full_product_width);
    RUN_TEST(test_m2_short_y_dangling_result_bits);
    RUN_TEST(test_m3_signedness);
    RUN_TEST(test_m4_y_wider_stays_soft);
    RUN_TEST(test_m5_defaults_tied_and_params);
    RUN_TEST(test_m_width_cap);
    RUN_TEST(test_m_prov_and_attr);
    RUN_TEST(test_m_explicit_untouched);
    RUN_TEST(test_m_oom);
    return UNITY_END();
}
