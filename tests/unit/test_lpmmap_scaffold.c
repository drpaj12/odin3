/*
 * test_lpmmap_scaffold.c — lpmmap, plan Task 4: the pass registration, its options, the INFO line,
 * and the kit (spec P-2, H-2, H-3, H-6, H-7, H-8) through a transaction built here: one `$add`
 * becomes one `lpm_add_sub` by the rules' own machinery. The rules themselves are tested in
 * test_lpmmap_addsub.c and the files after it.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "lpmmap_fixture.h"
#include "passes/lpmmap/lm_kit.h"
#include "passes/lpmmap/lpmmap.h"
#include "passes/opt/kit.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 8192, OOM_LIMIT = 2000 };

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
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_DEBUG));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

static const char k_one_add[] =
    "module \\m\n  wire width 3 input 1 \\a\n  wire width 5 input 2 \\b\n"
    "  wire width 6 output 3 \\y\n"
    "  cell $add \\u0\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
    "    parameter \\A_WIDTH 3\n    parameter \\B_WIDTH 5\n    parameter \\Y_WIDTH 6\n"
    "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n  end\nend\n";

/* A design no rule fires on: one `$neg`. */
static const char k_neg[] =
    "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 output 2 \\y\n"
    "  cell $neg \\u0\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 4\n"
    "    parameter \\Y_WIDTH 4\n    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n";

static const char k_signed_add[] =
    "module \\m\n  wire width 3 input 1 \\a\n  wire width 5 input 2 \\b\n"
    "  wire width 6 output 3 \\y\n"
    "  cell $add \\u0\n    parameter \\A_SIGNED 1\n    parameter \\B_SIGNED 1\n"
    "    parameter \\A_WIDTH 3\n    parameter \\B_WIDTH 5\n    parameter \\Y_WIDTH 6\n"
    "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n  end\nend\n";

static void check_full(odin3_design *design) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

/* A transaction built here: the one `$add` becomes one lpm_add_sub, Y tied to its result. */
static odin3_status build_add(lm_ctx *lc, odin3_node_id src, odin3_vec *ties) {
    odin3_net_id da[6];
    odin3_net_id db[6];
    odin3_net_id res[6];
    odin3_status st = lm_begin_cell(lc, src);
    bool sgn = odin3_opt_both_signed(lc->module, src);
    st = st == ODIN3_OK ? lm_extend(lc, odin3_node_port(lc->module, src, 0), 6, sgn, da) : st;
    st = st == ODIN3_OK ? lm_extend(lc, odin3_node_port(lc->module, src, 1), 6, sgn, db) : st;
    st = st == ODIN3_OK ? lm_fresh_nets(lc, 6, res) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    const lm_pset params[] = {{"lpm_width", 6, NULL}, {"lpm_direction", 0, "ADD"}};
    const lm_pconn ports[] = {{"dataa", da, 6}, {"datab", db, 6}, {"result", res, 6}};
    odin3_node_id node = {0};
    st = lm_new_node(lc, &(lm_node_req){LM_ADD_SUB, params, 2, ports, 3, "add"}, &node);
    odin3_pinslice ypins = odin3_node_port(lc->module, src, 2);
    st = st == ODIN3_OK ? odin3_vec_reserve(ties, 6) : st;
    for (uint32_t i = 0; st == ODIN3_OK && i < 6; i++) {
        lm_tie *tie = odin3_vec_push(ties);
        *tie = (lm_tie){odin3_pin_net(lc->module, (odin3_pin_id){ypins.first.v + i}), res[i]};
    }
    return st;
}

static odin3_status run_build_add(pb_result *res) {
    lm_ctx lc;
    lm_init(&lc, &res->ctx, res->design, NULL);
    lm_set_module(&lc, res->module);
    odin3_status st = lm_transact(&lc, lm_nth(res->module, "$add", 0), build_add);
    lm_free(&lc);
    return st;
}

/* --- P-2: the types ------------------------------------------------------------------------- */

static void test_p2_missing_type(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_one_add, &res));
    lm_ctx lc;
    lm_init(&lc, &res.ctx, res.design, NULL);
    lm_set_module(&lc, res.module);
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, lm_need(&lc, LM_ADD_SUB, (odin3_node_id){0}, &id));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: cell type 'lpm_add_sub' is not registered; "
                                          "read_techlib lib/lpm.o3lib first"));
    lm_free(&lc);
    pb_free(&res);
}

static void test_p2_wrong_kind(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_one_add, &res));
    static const char text[] =
        "library L\ncell lpm_add_sub gate\n  in a 1 ; out y 1\n  fn y = a\nend\n";
    const odin3_techlib_text src = {"t.o3lib", odin3_bytes_cstr(text)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_techlib_read_text(res.design, &src));
    lm_ctx lc;
    lm_init(&lc, &res.ctx, res.design, NULL);
    lm_set_module(&lc, res.module);
    odin3_celltype_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, lm_need(&lc, LM_ADD_SUB, (odin3_node_id){0}, &id));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: cell type 'lpm_add_sub' is not a black box"));
    lm_free(&lc);
    pb_free(&res);
}

static void test_p2_lazy_lookup(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_neg, &res)); /* no lpm library read */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(res.design, "lpmmap", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$neg"));
    pb_free(&res);
}

/* --- H-2, H-3 ------------------------------------------------------------------------------- */

typedef struct pinref {
    uint32_t port;
    uint32_t bit;
} pinref;

static odin3_net_id port_net(const pb_result *res, const char *type, pinref ref) {
    odin3_pinslice pins = odin3_node_port(res->module, lm_nth(res->module, type, 0), ref.port);
    return odin3_pin_net(res->module, (odin3_pin_id){pins.first.v + ref.bit});
}

static void test_h2_ext_table(void) {
    for (int is_signed = 0; is_signed <= 1; is_signed++) {
        pb_result res;
        lm_build_ok(is_signed ? k_signed_add : k_one_add, &res);
        lm_ctx lc;
        lm_init(&lc, &res.ctx, res.design, NULL);
        lm_set_module(&lc, res.module);
        odin3_net_id out[6];
        odin3_node_id add = lm_nth(res.module, "$add", 0);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, lm_extend(&lc, odin3_node_port(res.module, add, 0), 6, is_signed, out));
        for (uint32_t i = 0; i < 3; i++) {
            TEST_ASSERT_EQUAL_UINT32(port_net(&res, "$add", (pinref){0, i}).v, out[i].v);
        }
        for (uint32_t i = 3; i < 6; i++) {
            if (is_signed) {
                TEST_ASSERT_EQUAL_UINT32(out[2].v, out[i].v);
            } else {
                TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(res.module, out[i]));
            }
        }
        /* truncation: B has 5 bits, taken at width 2 */
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, lm_extend(&lc, odin3_node_port(res.module, add, 1), 2, is_signed, out));
        TEST_ASSERT_EQUAL_UINT32(port_net(&res, "$add", (pinref){1, 1}).v, out[1].v);
        lm_free(&lc);
        pb_free(&res);
    }
}

static void test_h3_constants_are_canonical_nets(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_one_add, &res));
    lm_ctx lc;
    lm_init(&lc, &res.ctx, res.design, NULL);
    lm_set_module(&lc, res.module);
    odin3_net_id g1 = {0};
    odin3_net_id g2 = {0};
    odin3_net_id v1 = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_const(&lc, LM_GND, &g1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_const(&lc, LM_GND, &g2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_const(&lc, LM_VCC, &v1));
    TEST_ASSERT_EQUAL_UINT32(g1.v, g2.v);
    TEST_ASSERT_TRUE(g1.v != v1.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(res.module, g1));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1, odin3_net_const_value(res.module, v1));
    lm_free(&lc);
    pb_free(&res);
}

/* --- H-6, H-7, H-8: a transaction ------------------------------------------------------------ */

/* The attribute `lpmmap` of node is "lpm_add_sub:add". */
static void expect_add_attr(const pb_result *res, odin3_node_id node) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(res->design), odin3_bytes_cstr("lpmmap"), &key));
    const odin3_value *val =
        odin3_attr_get(res->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("lpm_add_sub:add",
                             odin3_strtab_get(odin3_design_strtab(res->design), val->str));
}

static void test_transaction_makes_one_named_node(void) {
    pb_result res;
    lm_build_ok(k_one_add, &res);
    odin3_prov_id source = odin3_node_prov(res.module, lm_nth(res.module, "$add", 0));
    TEST_ASSERT_TRUE(source.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_build_add(&res));
    check_full(res.design);
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$add"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_add_sub"));
    odin3_node_id node = lm_nth(res.module, "lpm_add_sub", 0);
    /* H-7: the node takes the source's name */
    TEST_ASSERT_EQUAL_STRING(
        "u0", odin3_strtab_get(odin3_design_strtab(res.design), odin3_node_name(res.module, node)));
    TEST_ASSERT_EQUAL_INT64(6, lm_param_int(res.module, node, "lpm_width"));
    TEST_ASSERT_EQUAL_STRING("ADD", lm_param_str(res.module, node, "lpm_direction"));
    TEST_ASSERT_EQUAL_STRING("SIGNED", lm_param_str(res.module, node, "lpm_representation"));
    TEST_ASSERT_TRUE(lm_port_open(res.module, node, "cin"));
    /* H-6: one DERIVED record whose parent is the source's, one operation */
    const odin3_prov_record *rec = odin3_prov_get(res.design, odin3_node_prov(res.module, node));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(source.v, rec->parents.ids[0].v);
    TEST_ASSERT_EQUAL_UINT32(1, rec->op);
    expect_add_attr(&res, node);
    pb_free(&res);
}

static void test_h8_oom(void) {
    int made = 0;
    for (int fail_at = 0; fail_at < OOM_LIMIT && made == 0; fail_at++) {
        pb_result res;
        lm_build_ok(k_one_add, &res);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = run_build_add(&res);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            made = 1;
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "lpm_add_sub"));
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            /* the IR is valid and the source cell is intact (a failure after the delete would
             * have logged the discard message) */
            TEST_ASSERT_NULL(strstr(log_text, "no longer matches"));
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$add"));
            TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "lpm_add_sub"));
            check_full(res.design);
        }
        pb_free(&res);
    }
    TEST_ASSERT_EQUAL_INT(1, made);
}

static void test_h8_determinism(void) {
    odin3_strbuf one;
    odin3_strbuf two;
    odin3_strbuf_init(&one);
    odin3_strbuf_init(&two);
    pb_result res;
    lm_build_ok(k_one_add, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_build_add(&res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &one));
    pb_free(&res);
    lm_build_ok(k_one_add, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_build_add(&res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &two));
    pb_free(&res);
    TEST_ASSERT_EQUAL_STRING(one.data, two.data);
    odin3_strbuf_free(&one);
    odin3_strbuf_free(&two);
}

/* --- the pass --------------------------------------------------------------------------------- */

static void test_h8_info_line(void) {
    pb_result res;
    lm_build_ok(k_neg, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(res.design, "lpmmap", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: 0 lpm_add_sub, 0 lpm_mult, 0 lpm_compare, "
                                          "0 altsyncram; 0 cells left soft"));
    pb_free(&res);
}

static void test_w5_options(void) {
    pb_result res;
    lm_build_ok(k_neg, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(res.design, "lpmmap",
                                                   odin3_bytes_cstr("--family \"Stratix IV\"")));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(res.design, "lpmmap", odin3_bytes_cstr("--family")));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: --family needs a name"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(res.design, "lpmmap", odin3_bytes_cstr("--bogus")));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: unknown option '--bogus'"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$neg"));
    pb_free(&res);
}

static void test_w5_family_is_stored(void) {
    pb_result res;
    lm_build_ok(k_one_add, &res);
    lm_ctx lc;
    lm_init(&lc, &res.ctx, res.design, NULL);
    TEST_ASSERT_EQUAL_STRING("Cyclone V", lc.family);
    lm_free(&lc);
    lm_init(&lc, &res.ctx, res.design, "Stratix IV");
    TEST_ASSERT_EQUAL_STRING("Stratix IV", lc.family);
    lm_free(&lc);
    pb_free(&res);
}

static void test_p3_refuse_processes(void) {
    pb_result res;
    const char *text = "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                       "  wire output 3 \\q\n  process $p\n    sync posedge \\clk\n"
                       "      update \\q \\d\n  end\nend\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &res));
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, odin3_pass_run(res.design, "lpmmap", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has processes"));
    pb_free(&res);
}

static void test_lpmmap_twice(void) {
    pb_result res;
    lm_build_ok(k_neg, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(res.design, "lpmmap", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(res.design, "lpmmap", (odin3_bytes){NULL, 0}));
    pb_free(&res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_p2_missing_type);
    RUN_TEST(test_p2_wrong_kind);
    RUN_TEST(test_p2_lazy_lookup);
    RUN_TEST(test_h2_ext_table);
    RUN_TEST(test_h3_constants_are_canonical_nets);
    RUN_TEST(test_transaction_makes_one_named_node);
    RUN_TEST(test_h8_oom);
    RUN_TEST(test_h8_determinism);
    RUN_TEST(test_h8_info_line);
    RUN_TEST(test_w5_options);
    RUN_TEST(test_w5_family_is_stored);
    RUN_TEST(test_p3_refuse_processes);
    RUN_TEST(test_lpmmap_twice);
    return UNITY_END();
}
