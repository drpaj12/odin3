/*
 * test_lpmmap_addsub.c — lpmmap, plan Task 5: `$add` / `$sub` to `lpm_add_sub` (5.1 spec A-1..A-5,
 * H-6, H-8). Every test builds flat IR from RTLIL text with lib/lpm.o3lib read, runs lpmmap with
 * check FULL around it, and asserts the pins and parameters the rule fixes.
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

enum { LOG_TEXT = 8192, TEXT_MAX = 8192, OOM_LIMIT = 4000 };

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

/* One `$add` / `$sub` cell on wires a, b, y (or the given signal texts). */
typedef struct addcase {
    const char *type; /* "$add" or "$sub" */
    uint32_t a_width, b_width, y_width;
    bool a_signed, b_signed;
    const char *a_sig; /* NULL: the input wire \a */
    const char *b_sig; /* NULL: the input wire \b */
} addcase;

static void module_text(char *out, size_t size, const char *name, const addcase *ac) {
    size_t at = 0;
    at += (size_t)snprintf(out + at, size - at, "module \\%s\n", name);
    uint32_t port = 1;
    if (ac->a_sig == NULL) {
        at += (size_t)snprintf(out + at, size - at,
                               "  wire width %" PRIu32 " input %" PRIu32 " \\a\n", ac->a_width,
                               port++);
    }
    if (ac->b_sig == NULL) {
        at += (size_t)snprintf(out + at, size - at,
                               "  wire width %" PRIu32 " input %" PRIu32 " \\b\n", ac->b_width,
                               port++);
    }
    at += (size_t)snprintf(out + at, size - at, "  wire width %" PRIu32 " output %" PRIu32 " \\y\n",
                           ac->y_width, port);
    (void)snprintf(out + at, size - at,
                   "  cell %s \\u0\n    parameter \\A_SIGNED %d\n    parameter \\B_SIGNED %d\n"
                   "    parameter \\A_WIDTH %" PRIu32 "\n    parameter \\B_WIDTH %" PRIu32
                   "\n    parameter \\Y_WIDTH %" PRIu32
                   "\n    connect \\A %s\n    connect \\B %s\n    connect \\Y \\y\n  end\nend\n",
                   ac->type, ac->a_signed ? 1 : 0, ac->b_signed ? 1 : 0, ac->a_width, ac->b_width,
                   ac->y_width, ac->a_sig != NULL ? ac->a_sig : "\\a",
                   ac->b_sig != NULL ? ac->b_sig : "\\b");
}

static void build_one(addcase ac, pb_result *res) {
    char text[TEXT_MAX];
    module_text(text, sizeof text, "m", &ac);
    lm_build_ok(text, res);
}

/* The nets on the pins of port `port` of node, as a copy (the node may be deleted by the run). */
static void port_nets(const odin3_module *mod, odin3_node_id node, uint32_t port, odin3_net_id *out,
                      uint32_t max) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    for (uint32_t i = 0; i < pins.count && i < max; i++) {
        out[i] = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + i});
    }
}

enum { MAXW = 16 };

typedef struct before {
    odin3_net_id a[MAXW], b[MAXW], y[MAXW];
} before;

static void snapshot(const pb_result *res, const char *type, before *bf) {
    memset(bf, 0, sizeof *bf);
    odin3_node_id src = lm_nth(res->module, type, 0);
    port_nets(res->module, src, 0, bf->a, MAXW);
    port_nets(res->module, src, 1, bf->b, MAXW);
    port_nets(res->module, src, 2, bf->y, MAXW);
}

static bool is_const(const odin3_module *mod, odin3_net_id net, odin3_const value) {
    return odin3_net_valid(net) && odin3_net_const_value(mod, net) == value;
}

/* The first `have` bits of port `port` of blk are the nets src[0..have), the rest up to `width`
 * are gnd. */
static void expect_zero_extended(const pb_result *res, odin3_node_id blk, const char *port,
                                 const odin3_net_id *src, uint32_t have) {
    uint32_t width = lm_port_width(res->module, blk, port);
    for (uint32_t i = 0; i < width; i++) {
        odin3_net_id net = lm_pin_net(res->module, blk, port, i);
        if (i < have) {
            TEST_ASSERT_EQUAL_UINT32(src[i].v, net.v);
        } else {
            TEST_ASSERT_TRUE(is_const(res->module, net, ODIN3_CONST_0));
        }
    }
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

/* --- A-1 ------------------------------------------------------------------------------------ */

static void test_a1_candidates(void) {
    static const char text[] =
        "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 4 output 3 \\y1\n  wire width 4 output 4 \\y2\n  wire width 4 output 5 \\y3\n"
        "  cell $add \\u0\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 4\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y1\n  end\n"
        "  cell $sub \\u1\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 4\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y2\n  end\n"
        "  cell $neg \\u2\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 4\n"
        "    parameter \\Y_WIDTH 4\n    connect \\A \\a\n    connect \\Y \\y3\n  end\nend\n";
    pb_result res;
    lm_build_ok(text, &res);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(2, lm_count(res.module, "lpm_add_sub"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$add"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$sub"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$neg"));
    TEST_ASSERT_EQUAL_UINT64(2, stats.add_sub);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: 2 lpm_add_sub, 0 lpm_mult, 0 lpm_compare, "
                                          "0 altsyncram; 0 cells left soft"));
    pb_free(&res);
}

/* --- A-2 ------------------------------------------------------------------------------------ */

static void test_a2_width_is_y_unsigned(void) {
    pb_result res;
    build_one((addcase){"$add", 8, 4, 9, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, "$add", &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    TEST_ASSERT_EQUAL_INT64(9, lm_param_int(res.module, blk, "lpm_width"));
    TEST_ASSERT_EQUAL_UINT32(9, lm_port_width(res.module, blk, "dataa"));
    expect_zero_extended(&res, blk, "dataa", bf.a, 8);
    expect_zero_extended(&res, blk, "datab", bf.b, 4);
    for (uint32_t i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.y[i].v, lm_pin_net(res.module, blk, "result", i).v);
    }
    pb_free(&res);
}

static void test_a2_truncation(void) {
    pb_result res;
    build_one((addcase){"$add", 8, 8, 4, false, false, NULL, NULL}, &res);
    before bf;
    snapshot(&res, "$add", &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    TEST_ASSERT_EQUAL_INT64(4, lm_param_int(res.module, blk, "lpm_width"));
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.a[i].v, lm_pin_net(res.module, blk, "dataa", i).v);
        TEST_ASSERT_EQUAL_UINT32(bf.b[i].v, lm_pin_net(res.module, blk, "datab", i).v);
        TEST_ASSERT_EQUAL_UINT32(bf.y[i].v, lm_pin_net(res.module, blk, "result", i).v);
    }
    pb_free(&res);
}

static void test_a2_signed_ext(void) {
    pb_result res;
    build_one((addcase){"$add", 3, 5, 6, true, true, NULL, NULL}, &res);
    before bf;
    snapshot(&res, "$add", &bf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    for (uint32_t i = 3; i < 6; i++) {
        TEST_ASSERT_EQUAL_UINT32(bf.a[2].v, lm_pin_net(res.module, blk, "dataa", i).v);
    }
    TEST_ASSERT_EQUAL_UINT32(bf.b[4].v, lm_pin_net(res.module, blk, "datab", 5).v);
    TEST_ASSERT_EQUAL_STRING("SIGNED", lm_param_str(res.module, blk, "lpm_representation"));
    pb_free(&res);
}

static void test_a2_mixed_sign_is_unsigned(void) {
    pb_result res;
    build_one((addcase){"$add", 3, 5, 6, true, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "dataa", 4), ODIN3_CONST_0));
    TEST_ASSERT_EQUAL_STRING("UNSIGNED", lm_param_str(res.module, blk, "lpm_representation"));
    pb_free(&res);
}

/* --- A-3, A-4, A-5 ---------------------------------------------------------------------------- */

/* Bit 0 of port `port` of blk is the constant `value`. */
static void expect_tied(const pb_result *res, odin3_node_id blk, const char *port,
                        odin3_const value) {
    TEST_ASSERT_TRUE_MESSAGE(is_const(res->module, lm_pin_net(res->module, blk, port, 0), value),
                             port);
}

static void test_a3_defaults_tied_and_unused_open(void) {
    pb_result res;
    build_one((addcase){"$add", 4, 4, 5, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    /* M1: the documented defaults are tied; add_sub, cout and overflow stay open */
    expect_tied(&res, blk, "cin", ODIN3_CONST_0);
    expect_tied(&res, blk, "clken", ODIN3_CONST_1);
    expect_tied(&res, blk, "aclr", ODIN3_CONST_0);
    expect_tied(&res, blk, "clock", ODIN3_CONST_0);
    static const char *const open_ports[] = {"add_sub", "cout", "overflow"};
    for (size_t i = 0; i < sizeof open_ports / sizeof open_ports[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(lm_port_open(res.module, blk, open_ports[i]), open_ports[i]);
    }
    pb_free(&res);
}

static void test_a4_params_add_and_sub(void) {
    static const struct {
        const char *type;
        bool sgn;
        const char *dir, *rep, *role;
    } rows[] = {{"$add", false, "ADD", "UNSIGNED", "lpm_add_sub:add"},
                {"$add", true, "ADD", "SIGNED", "lpm_add_sub:add"},
                {"$sub", false, "SUB", "UNSIGNED", "lpm_add_sub:sub"},
                {"$sub", true, "SUB", "SIGNED", "lpm_add_sub:sub"}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        pb_result res;
        build_one((addcase){rows[i].type, 4, 4, 5, rows[i].sgn, rows[i].sgn, NULL, NULL}, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
        odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
        TEST_ASSERT_EQUAL_STRING(rows[i].dir, lm_param_str(res.module, blk, "lpm_direction"));
        TEST_ASSERT_EQUAL_STRING(rows[i].rep, lm_param_str(res.module, blk, "lpm_representation"));
        TEST_ASSERT_EQUAL_INT64(0, lm_param_int(res.module, blk, "lpm_pipeline"));
        /* A-5: cin is the documented default: 1 for a subtract, 0 for an add */
        TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "cin", 0),
                                  strcmp(rows[i].dir, "SUB") == 0 ? ODIN3_CONST_1 : ODIN3_CONST_0));
        expect_lpmmap_attr(&res, blk, rows[i].role);
        pb_free(&res);
    }
}

/* --- constants, aliases, ports, hierarchy ---------------------------------------------------- */

static void test_a_const_operand(void) {
    pb_result res;
    build_one((addcase){"$add", 4, 4, 5, false, false, NULL, "4'0101"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    static const odin3_const bits[] = {ODIN3_CONST_1, ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_0};
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "datab", i), bits[i]));
    }
    pb_free(&res);
}

static void test_a_aliased_operands(void) {
    static const char *const types[] = {"$add", "$sub"};
    for (size_t i = 0; i < 2; i++) {
        pb_result res;
        build_one((addcase){types[i], 4, 4, 4, false, false, NULL, "\\a"}, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
        odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
        for (uint32_t k = 0; k < 4; k++) {
            TEST_ASSERT_EQUAL_UINT32(lm_pin_net(res.module, blk, "dataa", k).v,
                                     lm_pin_net(res.module, blk, "datab", k).v);
        }
        pb_free(&res);
    }
}

static void test_a_two_modules_no_flatten(void) {
    static const char text[] =
        "module \\sub1\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 5 output 3 \\y\n"
        "  cell $sub \\u0\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 5\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n  end\nend\n"
        "module \\top\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 5 output 3 \\y\n"
        "  cell \\sub1 \\i0\n    connect \\a \\a\n    connect \\b \\b\n    connect \\y \\y\n  end\n"
        "end\n";
    pb_result res;
    lm_build_ok(text, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_module *sub = pb_module(&res, "sub1");
    TEST_ASSERT_NOT_NULL(sub);
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(sub, "lpm_add_sub"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(sub, "$sub"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(pb_module(&res, "top"), "sub1"));
    pb_free(&res);
}

static void test_a_prov_and_attr(void) {
    pb_result res;
    build_one((addcase){"$add", 4, 4, 5, false, false, NULL, NULL}, &res);
    odin3_prov_id source = odin3_node_prov(res.module, lm_nth(res.module, "$add", 0));
    TEST_ASSERT_TRUE(source.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "lpm_add_sub", 0);
    const odin3_prov_record *rec = odin3_prov_get(res.design, odin3_node_prov(res.module, blk));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(res.ctx.run.v, rec->run.v);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(source.v, rec->parents.ids[0].v);
    TEST_ASSERT_EQUAL_STRING(
        "u0", odin3_strtab_get(odin3_design_strtab(res.design), odin3_node_name(res.module, blk)));
    expect_lpmmap_attr(&res, blk, "lpm_add_sub:add");
    pb_free(&res);
}

static void test_a_explicit_untouched(void) {
    static const char text[] =
        "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 4 output 3 \\y\n"
        "  cell \\lpm_add_sub \\u1\n    parameter \\lpm_width 4\n    connect \\dataa \\a\n"
        "    connect \\datab \\b\n    connect \\result \\y\n  end\nend\n";
    pb_result res;
    lm_build_ok(text, &res);
    odin3_node_id before_id = lm_nth(res.module, "lpm_add_sub", 0);
    TEST_ASSERT_TRUE(before_id.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_add_sub"));
    TEST_ASSERT_EQUAL_UINT32(before_id.v, lm_nth(res.module, "lpm_add_sub", 0).v);
    pb_free(&res);
}

static void test_a_idempotent(void) {
    pb_result res;
    build_one((addcase){"$add", 4, 4, 5, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.add_sub);
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_add_sub"));
    pb_free(&res);
}

static void test_a_oom(void) {
    int mapped = 0;
    for (int fail_at = 0; fail_at < OOM_LIMIT && mapped == 0; fail_at++) {
        pb_result res;
        build_one((addcase){"$sub", 8, 6, 9, true, true, NULL, NULL}, &res);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_lpmmap_run(&res.ctx, res.design, NULL, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            mapped = 1;
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_add_sub"));
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_NULL(strstr(log_text, "no longer matches"));
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$sub"));
            TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "lpm_add_sub"));
            odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
        }
        pb_free(&res);
    }
    TEST_ASSERT_EQUAL_INT(1, mapped);
}

/* M2: 256 bits maps, 257 stays soft with a warning naming the cell and the width. */
static void test_a_width_cap(void) {
    pb_result res;
    build_one((addcase){"$add", 256, 256, 256, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_add_sub"));
    pb_free(&res);
    log_text[0] = '\0';
    build_one((addcase){"$sub", 257, 257, 257, false, false, NULL, NULL}, &res);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$sub"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "lpm_add_sub"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: '$sub' 'u0' has width 257, above the 256 bits "
                                          "lpm_add_sub supports; left soft"));
    pb_free(&res);
}

static void test_a_missing_type_error(void) {
    pb_result res;
    char text[TEXT_MAX];
    addcase ac = {"$add", 4, 4, 5, false, false, NULL, NULL};
    module_text(text, sizeof text, "m", &ac);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res)); /* no lpm library */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_lpmmap_run(&res.ctx, res.design, NULL, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: cell type 'lpm_add_sub' is not registered; "
                                          "read_techlib lib/lpm.o3lib first"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$add"));
    pb_free(&res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a1_candidates);
    RUN_TEST(test_a2_width_is_y_unsigned);
    RUN_TEST(test_a2_truncation);
    RUN_TEST(test_a2_signed_ext);
    RUN_TEST(test_a2_mixed_sign_is_unsigned);
    RUN_TEST(test_a3_defaults_tied_and_unused_open);
    RUN_TEST(test_a_width_cap);
    RUN_TEST(test_a4_params_add_and_sub);
    RUN_TEST(test_a_const_operand);
    RUN_TEST(test_a_aliased_operands);
    RUN_TEST(test_a_two_modules_no_flatten);
    RUN_TEST(test_a_prov_and_attr);
    RUN_TEST(test_a_explicit_untouched);
    RUN_TEST(test_a_idempotent);
    RUN_TEST(test_a_oom);
    RUN_TEST(test_a_missing_type_error);
    return UNITY_END();
}
