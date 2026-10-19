/*
 * test_lpmmap_compare.c — lpmmap, plan Task 7: the six comparisons to `lpm_compare` (5.1 spec
 * C-1..C-4, H-6, H-8).
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

enum { LOG_TEXT = 8192, TEXT_MAX = 4096, OOM_LIMIT = 4000, MAXW = 16 };

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

typedef struct cmpcase {
    const char *type;
    uint32_t a_width, b_width, y_width;
    bool a_signed, b_signed;
    const char *a_sig; /* NULL: the input wire \a */
    const char *b_sig; /* NULL: the input wire \b */
} cmpcase;

static void build_one(cmpcase cc, pb_result *res) {
    char text[TEXT_MAX];
    size_t at = 0;
    at += (size_t)snprintf(text + at, sizeof text - at, "module \\m\n");
    uint32_t port = 1;
    if (cc.a_sig == NULL) {
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "  wire width %" PRIu32 " input %" PRIu32 " \\a\n", cc.a_width,
                               port++);
    }
    if (cc.b_sig == NULL) {
        at += (size_t)snprintf(text + at, sizeof text - at,
                               "  wire width %" PRIu32 " input %" PRIu32 " \\b\n", cc.b_width,
                               port++);
    }
    at += (size_t)snprintf(text + at, sizeof text - at,
                           "  wire width %" PRIu32 " output %" PRIu32 " \\y\n", cc.y_width, port);
    (void)snprintf(text + at, sizeof text - at,
                   "  cell %s \\u0\n    parameter \\A_SIGNED %d\n    parameter \\B_SIGNED %d\n"
                   "    parameter \\A_WIDTH %" PRIu32 "\n    parameter \\B_WIDTH %" PRIu32
                   "\n    parameter \\Y_WIDTH %" PRIu32
                   "\n    connect \\A %s\n    connect \\B %s\n    connect \\Y \\y\n  end\nend\n",
                   cc.type, cc.a_signed ? 1 : 0, cc.b_signed ? 1 : 0, cc.a_width, cc.b_width,
                   cc.y_width, cc.a_sig != NULL ? cc.a_sig : "\\a",
                   cc.b_sig != NULL ? cc.b_sig : "\\b");
    lm_build_ok(text, res);
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

static void snapshot(const pb_result *res, const char *type, before *bf) {
    memset(bf, 0, sizeof *bf);
    odin3_node_id src = lm_nth(res->module, type, 0);
    port_nets(res->module, src, 0, bf->a);
    port_nets(res->module, src, 1, bf->b);
    port_nets(res->module, src, 2, bf->y);
}

static bool is_const(const odin3_module *mod, odin3_net_id net, odin3_const value) {
    return odin3_net_valid(net) && odin3_net_const_value(mod, net) == value;
}

static const struct {
    const char *type;
    const char *out;
} k_table[] = {{"$lt", "alb"},  {"$le", "aleb"}, {"$gt", "agb"},
               {"$ge", "ageb"}, {"$eq", "aeb"},  {"$ne", "aneb"}};

static const char *const k_all_outs[] = {"alb", "aeb", "agb", "aleb", "aneb", "ageb"};

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

/* --- C-1 ------------------------------------------------------------------------------------ */

/* Exactly the output `wanted` of blk is connected, to net `y0`; the other five are open. */
static void expect_one_output(const pb_result *res, odin3_node_id blk, const char *wanted,
                              odin3_net_id y0) {
    for (size_t idx = 0; idx < sizeof k_all_outs / sizeof k_all_outs[0]; idx++) {
        if (strcmp(k_all_outs[idx], wanted) == 0) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(y0.v, lm_pin_net(res->module, blk, wanted, 0).v,
                                             wanted);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(lm_port_open(res->module, blk, k_all_outs[idx]),
                                     k_all_outs[idx]);
        }
    }
}

/* The control ports are open. */
static void expect_ctrl_open(const pb_result *res, odin3_node_id blk) {
    TEST_ASSERT_TRUE(lm_port_open(res->module, blk, "clock"));
    TEST_ASSERT_TRUE(lm_port_open(res->module, blk, "clken"));
    TEST_ASSERT_TRUE(lm_port_open(res->module, blk, "aclr"));
}

/* One comparison type maps to one lpm_compare driving its output. */
static void check_one_type(size_t row) {
    pb_result res;
    build_one((cmpcase){k_table[row].type, 6, 6, 1, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, k_table[row].type, &bf);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_compare"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, k_table[row].type));
    TEST_ASSERT_EQUAL_UINT64(1, stats.compare);
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    expect_one_output(&res, blk, k_table[row].out, bf.y[0]);
    expect_ctrl_open(&res, blk);
    pb_free(&res);
}

static void test_c1_output_table(void) {
    for (size_t i = 0; i < sizeof k_table / sizeof k_table[0]; i++) {
        check_one_type(i);
    }
}

static void test_c1_y_wider_than_1(void) {
    pb_result res;
    build_one((cmpcase){"$lt", 4, 4, 3, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, "$lt", &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    TEST_ASSERT_EQUAL_UINT32(bf.y[0].v, lm_pin_net(res.module, blk, "alb", 0).v);
    /* Y[1], Y[2] are gnd: their nets now carry the constant */
    odin3_net_id y1 = bf.y[1];
    odin3_net_id y2 = bf.y[2];
    TEST_ASSERT_TRUE(!odin3_net_live(res.module, y1) || is_const(res.module, y1, ODIN3_CONST_0));
    TEST_ASSERT_TRUE(is_const(res.module, y2, ODIN3_CONST_0));
    pb_free(&res);
}

/* --- C-2 ------------------------------------------------------------------------------------ */

static void test_c2_width_is_max(void) {
    pb_result res;
    build_one((cmpcase){"$ge", 8, 4, 1, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, "$ge", &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    TEST_ASSERT_EQUAL_INT64(8, lm_param_int(res.module, blk, "lpm_width"));
    for (uint32_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.a[i].v, lm_pin_net(res.module, blk, "dataa", i).v);
        odin3_net_id net_b = lm_pin_net(res.module, blk, "datab", i);
        if (i < 4) {
            TEST_ASSERT_EQUAL_UINT32(bf.b[i].v, net_b.v);
        } else {
            TEST_ASSERT_TRUE(is_const(res.module, net_b, ODIN3_CONST_0));
        }
    }
    pb_free(&res);
}

static void test_c2_signed_vs_mixed(void) {
    pb_result res;
    build_one((cmpcase){"$lt", 4, 6, 1, true, true, NULL, NULL}, &res);
    before bf;
    snapshot(&res, "$lt", &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    TEST_ASSERT_EQUAL_STRING("SIGNED", lm_param_str(res.module, blk, "lpm_representation"));
    TEST_ASSERT_EQUAL_UINT32(bf.a[3].v, lm_pin_net(res.module, blk, "dataa", 4).v);
    TEST_ASSERT_EQUAL_UINT32(bf.a[3].v, lm_pin_net(res.module, blk, "dataa", 5).v);
    pb_free(&res);
    build_one((cmpcase){"$lt", 4, 6, 1, true, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    blk = lm_nth(res.module, "lpm_compare", 0);
    TEST_ASSERT_EQUAL_STRING("UNSIGNED", lm_param_str(res.module, blk, "lpm_representation"));
    TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "dataa", 5), ODIN3_CONST_0));
    pb_free(&res);
}

/* --- C-3, C-4 -------------------------------------------------------------------------------- */

static void test_c3_one_bit_eq_maps(void) {
    pb_result res;
    build_one((cmpcase){"$eq", 1, 1, 1, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_compare"));
    TEST_ASSERT_EQUAL_INT64(
        1, lm_param_int(res.module, lm_nth(res.module, "lpm_compare", 0), "lpm_width"));
    pb_free(&res);
}

static void test_c_const_operand(void) {
    pb_result res;
    build_one((cmpcase){"$eq", 4, 4, 1, false, false, NULL, "4'1001"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "datab", 0), ODIN3_CONST_1));
    TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "datab", 1), ODIN3_CONST_0));
    TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "datab", 3), ODIN3_CONST_1));
    pb_free(&res);
}

static void test_c_aliased(void) {
    pb_result res;
    build_one((cmpcase){"$eq", 4, 4, 1, false, false, NULL, "\\a"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(lm_pin_net(res.module, blk, "dataa", k).v,
                                 lm_pin_net(res.module, blk, "datab", k).v);
    }
    pb_free(&res);
}

static void test_c_prov_and_attr(void) {
    pb_result res;
    build_one((cmpcase){"$gt", 4, 4, 1, false, false, NULL, NULL}, &res);
    odin3_prov_id source = odin3_node_prov(res.module, lm_nth(res.module, "$gt", 0));
    TEST_ASSERT_TRUE(source.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_compare", 0);
    const odin3_prov_record *rec = odin3_prov_get(res.design, odin3_node_prov(res.module, blk));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(source.v, rec->parents.ids[0].v);
    expect_lpmmap_attr(&res, blk, "lpm_compare:cmp");
    pb_free(&res);
}

static void test_c_oom(void) {
    int mapped = 0;
    for (int fail_at = 0; fail_at < OOM_LIMIT && mapped == 0; fail_at++) {
        pb_result res;
        build_one((cmpcase){"$le", 6, 4, 2, true, true, NULL, NULL}, &res);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_lpmmap_run(&res.ctx, res.design, NULL, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            mapped = 1;
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_compare"));
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_NULL(strstr(log_text, "no longer matches"));
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$le"));
            TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "lpm_compare"));
            odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
        }
        pb_free(&res);
    }
    TEST_ASSERT_EQUAL_INT(1, mapped);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_c1_output_table);
    RUN_TEST(test_c1_y_wider_than_1);
    RUN_TEST(test_c2_width_is_max);
    RUN_TEST(test_c2_signed_vs_mixed);
    RUN_TEST(test_c3_one_bit_eq_maps);
    RUN_TEST(test_c_const_operand);
    RUN_TEST(test_c_aliased);
    RUN_TEST(test_c_prov_and_attr);
    RUN_TEST(test_c_oom);
    return UNITY_END();
}
