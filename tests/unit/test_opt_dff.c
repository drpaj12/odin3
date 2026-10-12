/*
 * test_opt_dff.c — the FF model of opt_dff (2F spec OP-9 §9.1, §9.4; plan Task 2; Tasks 11-12 add
 * the rules): ff_load over the fifteen word FF types (both polarities) and the fine ones, the
 * const-D latch, ff_emit's type table (Yosys FfData::emit, kernel/ff.cc 536-605), ff_slice, a fine
 * FF keeping its type, the init writes, misuse and out of memory at every allocation of ff_emit.
 *
 * Yosys re-derivation (0.55, the VTR build): `yosys -p "read_rtlil X.il; opt_dff; write_rtlil"`
 * on 2-bit FFs whose CLK is 1'0 (opt_dff drops the clock, then FfData::emit picks the type):
 * $dffsr -> $sr (SET/CLR kept); $adff ARST_POLARITY 0 ARST_VALUE 2'10, init 2'01 -> $dlatch
 * EN = ARST, EN_POLARITY 0, D = 2'10, init kept; $dff with init 2'01 -> `connect \q 2'01`, the
 * init attribute gone. The rest of the table is kernel/ff.cc 536-605 read directly.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "passes/ffinit.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 512, OOM_LIMIT = 4000, N_WORD = 15 };

static char last_error[LOG_TEXT];
static uint32_t n_errors;
static uint32_t n_discard; /* errors saying the design must be discarded (a half-done rewrite) */

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        size_t len = strlen(msg);
        len = len < sizeof last_error ? len : sizeof last_error - 1;
        memcpy(last_error, msg, len);
        last_error[len] = '\0';
        n_errors++;
        n_discard += strstr(msg, "discard it") != NULL ? 1U : 0U;
    }
}

typedef struct fixture {
    pb_result res;
    odin3_pass_ctx ctx;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_opt_ctx oc;
    odin3_module *module;
    bool open;
} fixture;

static fixture fx;

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void close_fixture(void) {
    if (fx.open) {
        odin3_opt_ctx_free(&fx.oc);
    }
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
}

static void build(const char *text) {
    close_fixture();
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, pb_build(text, &fx.res), last_error);
    fx.module = fx.res.module;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_ctx_init(&fx.oc, &fx.ctx, fx.module, &fx.opts));
    fx.oc.counts = &fx.counts;
    fx.open = true;
}

void setUp(void) {
    memset(&fx, 0, sizeof fx);
    last_error[0] = '\0';
    n_errors = 0;
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    close_fixture();
    odin3_log_set_sink(NULL, NULL);
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fx.res.design, opts));
}

static odin3_net_id wnet(const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(fx.module, intern(name));
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(fx.module, wire, bit);
}

static odin3_node_id node_named(const char *name) {
    return odin3_module_find_node(fx.module, intern(name));
}

static const char *type_name(odin3_node_id node) {
    return odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node))->name;
}

static odin3_net_id vnet(const odin3_vec *vec, uint32_t idx) {
    return *(const odin3_net_id *)odin3_vec_cat(vec, idx);
}

static odin3_bit vbit(const odin3_vec *vec, uint32_t idx) {
    return *(const odin3_bit *)odin3_vec_cat(vec, idx);
}

/* The index of node's port named name (asserted to exist). */
static uint32_t port_of(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    for (uint32_t i = 0; i < def->n_ports; i++) {
        if (strcmp(def->ports[i].name, name) == 0) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

static odin3_net_id pin_net_of(odin3_node_id node, const char *port, uint32_t bit) {
    odin3_pinslice pins = odin3_node_port(fx.module, node, port_of(node, port));
    TEST_ASSERT_TRUE(bit < pins.count);
    return odin3_pin_net(fx.module, (odin3_pin_id){pins.first.v + bit});
}

static const odin3_value *param_of(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return odin3_node_param(fx.module, node, i);
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static odin3_net_id const_net(odin3_bit bit) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_const_net(&fx.oc, bit, &net));
    return net;
}

/* --- ff_load: the fifteen word types -------------------------------------------------------- */

/* Feature bits of the tables below. */
enum {
    F_CLK = 1U << 0U,
    F_CE = 1U << 1U,
    F_SRST = 1U << 2U,
    F_ARST = 1U << 3U,
    F_ALOAD = 1U << 4U,
    F_SR = 1U << 5U,
    F_CEO = 1U << 6U
};

typedef struct word_case {
    const char *type;
    const char *ports;  /* "PORT wire" pairs, comma separated; Q is \qN */
    const char *params; /* parameter names; polarities get base ^ flip, values fixed */
    unsigned features;
} word_case;

static const word_case k_word[N_WORD] = {
    {"$dff", "CLK c,D d", "CLK_POLARITY", F_CLK},
    {"$dffe", "CLK c,EN e,D d", "CLK_POLARITY,EN_POLARITY", F_CLK | F_CE},
    {"$adff", "CLK c,ARST r,D d", "CLK_POLARITY,ARST_POLARITY,ARST_VALUE", F_CLK | F_ARST},
    {"$adffe", "CLK c,ARST r,EN e,D d", "CLK_POLARITY,EN_POLARITY,ARST_POLARITY,ARST_VALUE",
     F_CLK | F_ARST | F_CE},
    {"$aldff", "CLK c,ALOAD l,AD a,D d", "CLK_POLARITY,ALOAD_POLARITY", F_CLK | F_ALOAD},
    {"$aldffe", "CLK c,ALOAD l,AD a,EN e,D d", "CLK_POLARITY,EN_POLARITY,ALOAD_POLARITY",
     F_CLK | F_ALOAD | F_CE},
    {"$sdff", "CLK c,SRST r,D d", "CLK_POLARITY,SRST_POLARITY,SRST_VALUE", F_CLK | F_SRST},
    {"$sdffe", "CLK c,SRST r,EN e,D d", "CLK_POLARITY,EN_POLARITY,SRST_POLARITY,SRST_VALUE",
     F_CLK | F_SRST | F_CE},
    {"$sdffce", "CLK c,SRST r,EN e,D d", "CLK_POLARITY,EN_POLARITY,SRST_POLARITY,SRST_VALUE",
     F_CLK | F_SRST | F_CE | F_CEO},
    {"$dffsr", "CLK c,SET s,CLR k,D d", "CLK_POLARITY,SET_POLARITY,CLR_POLARITY", F_CLK | F_SR},
    {"$dffsre", "CLK c,SET s,CLR k,EN e,D d", "CLK_POLARITY,SET_POLARITY,CLR_POLARITY,EN_POLARITY",
     F_CLK | F_SR | F_CE},
    {"$dlatch", "EN e,D d", "EN_POLARITY", F_ALOAD},
    {"$adlatch", "EN e,ARST r,D d", "EN_POLARITY,ARST_POLARITY,ARST_VALUE", F_ALOAD | F_ARST},
    {"$dlatchsr", "EN e,SET s,CLR k,D d", "EN_POLARITY,SET_POLARITY,CLR_POLARITY", F_ALOAD | F_SR},
    {"$sr", "SET s,CLR k", "SET_POLARITY,CLR_POLARITY", F_SR},
};

/* The base polarity of each parameter (flip inverts it); SRST_POLARITY 0 as the brief's. */
static int base_polarity(const char *name) {
    static const char *const k_high[] = {"CLK_POLARITY", "EN_POLARITY", "ALOAD_POLARITY",
                                         "SET_POLARITY"};
    for (size_t i = 0; i < sizeof k_high / sizeof k_high[0]; i++) {
        if (strcmp(name, k_high[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One list item: "PORT w" (connect \PORT \w), a *_VALUE or a polarity (base ^ flip). */
static void append_item(odin3_strbuf *buf, char *item, bool ports, int flip) {
    char *space = strchr(item, ' ');
    odin3_status st = ODIN3_OK;
    if (ports) {
        TEST_ASSERT_NOT_NULL(space);
        *space = '\0';
        st = odin3_strbuf_appendf(buf, "    connect \\%s \\%s\n", item, space + 1);
    } else if (strstr(item, "_VALUE") != NULL) {
        const char *value = strcmp(item, "ARST_VALUE") == 0 ? "2'10" : "2'01";
        st = odin3_strbuf_appendf(buf, "    parameter \\%s %s\n", item, value);
    } else {
        st = odin3_strbuf_appendf(buf, "    parameter \\%s %d\n", item, base_polarity(item) ^ flip);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
}

static void append_list(odin3_strbuf *buf, const char *list, bool ports, int flip) {
    char item[64];
    const char *at = list;
    while (*at != '\0') {
        size_t len = strcspn(at, ",");
        TEST_ASSERT_TRUE(len < sizeof item);
        memcpy(item, at, len);
        item[len] = '\0';
        append_item(buf, item, ports, flip);
        at += len + (at[len] == ',' ? 1 : 0);
    }
}

/* One module with every word type as cell \fN (Q on \qN), polarities base ^ flip. */
static void build_word(int flip) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(&buf, "module \\m\n  wire input 1 \\c\n"
                                                     "  wire input 2 \\e\n  wire input 3 \\r\n"
                                                     "  wire input 4 \\l\n  wire width 2 \\d\n"
                                                     "  wire width 2 \\a\n  wire width 2 \\s\n"
                                                     "  wire width 2 \\k\n"));
    for (uint32_t i = 0; i < N_WORD; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&buf, "  wire width 2 \\q%u\n", i));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&buf,
                                                             "  cell %s \\f%u\n"
                                                             "    parameter \\WIDTH 2\n",
                                                             k_word[i].type, i));
        append_list(&buf, k_word[i].params, false, flip);
        append_list(&buf, k_word[i].ports, true, flip);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(&buf, "    connect \\Q \\q%u\n  end\n", i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&buf, "end\n"));
    build(buf.data);
    odin3_strbuf_free(&buf);
}

static void expect_flags(const odin3_ffdata *ff, unsigned features) {
    TEST_ASSERT_EQUAL((features & F_CLK) != 0, ff->has_clk);
    TEST_ASSERT_EQUAL((features & F_CE) != 0, ff->has_ce);
    TEST_ASSERT_EQUAL((features & F_SRST) != 0, ff->has_srst);
    TEST_ASSERT_EQUAL((features & F_ARST) != 0, ff->has_arst);
    TEST_ASSERT_EQUAL((features & F_ALOAD) != 0, ff->has_aload);
    TEST_ASSERT_EQUAL((features & F_SR) != 0, ff->has_sr);
    TEST_ASSERT_EQUAL((features & F_CEO) != 0, ff->ce_over_srst);
    TEST_ASSERT_FALSE(ff->is_fine);
    TEST_ASSERT_FALSE(ff->has_gclk);
}

static void expect_pols(const odin3_ffdata *ff, int flip) {
    bool high = flip == 0;
    TEST_ASSERT_EQUAL(ff->has_clk && high, ff->pol_clk);
    TEST_ASSERT_EQUAL(ff->has_ce && high, ff->pol_ce);
    TEST_ASSERT_EQUAL(ff->has_aload && high, ff->pol_aload);
    TEST_ASSERT_EQUAL(ff->has_sr && high, ff->pol_set);
    TEST_ASSERT_EQUAL(ff->has_srst && !high, ff->pol_srst);
    TEST_ASSERT_EQUAL(ff->has_arst && !high, ff->pol_arst);
    TEST_ASSERT_EQUAL(ff->has_sr && !high, ff->pol_clr);
}

/* A vector of 2 nets equal to wire name's, or empty when absent. */
static void expect_nets(const odin3_vec *vec, bool present, const char *name) {
    TEST_ASSERT_EQUAL_size_t(present ? 2 : 0, vec->len);
    for (uint32_t i = 0; present && i < 2; i++) {
        TEST_ASSERT_EQUAL_UINT32(wnet(name, i).v, vnet(vec, i).v);
    }
}

static void expect_one(odin3_net_id got, bool present, const char *name) {
    TEST_ASSERT_EQUAL_UINT32(present ? wnet(name, 0).v : 0, got.v);
}

static void expect_sigs(const odin3_ffdata *ff, uint32_t index) {
    char qname[8];
    TEST_ASSERT_TRUE(snprintf(qname, sizeof qname, "q%u", index) < (int)sizeof qname);
    bool latch = ff->has_aload && !ff->has_clk;
    expect_nets(&ff->sig_q, true, qname);
    expect_nets(&ff->sig_d, ff->has_clk, "d");
    expect_nets(&ff->sig_ad, ff->has_aload, latch ? "d" : "a");
    expect_nets(&ff->sig_set, ff->has_sr, "s");
    expect_nets(&ff->sig_clr, ff->has_sr, "k");
    expect_one(ff->sig_clk, ff->has_clk, "c");
    expect_one(ff->sig_ce, ff->has_ce, "e");
    expect_one(ff->sig_aload, ff->has_aload, latch ? "e" : "l");
    expect_one(ff->sig_arst, ff->has_arst, "r");
    expect_one(ff->sig_srst, ff->has_srst, "r");
}

static void expect_vals(const odin3_ffdata *ff) {
    TEST_ASSERT_EQUAL_size_t(ff->has_arst ? 2 : 0, ff->val_arst.len);
    TEST_ASSERT_EQUAL_size_t(ff->has_srst ? 2 : 0, ff->val_srst.len);
    if (ff->has_arst) { /* 2'10, LSB first */
        TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, vbit(&ff->val_arst, 0));
        TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&ff->val_arst, 1));
    }
    if (ff->has_srst) { /* 2'01 */
        TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&ff->val_srst, 0));
        TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, vbit(&ff->val_srst, 1));
    }
    TEST_ASSERT_EQUAL_size_t(2, ff->val_init.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, vbit(&ff->val_init, 0));
}

/* Cell \f<index> of build_word(flip) loads as its k_word entry wc. */
static void load_word(const word_case *wc, int flip) {
    uint32_t index = (uint32_t)(wc - k_word);
    char name[8];
    TEST_ASSERT_TRUE(snprintf(name, sizeof name, "f%u", index) < (int)sizeof name);
    odin3_node_id node = node_named(name);
    TEST_ASSERT_TRUE(odin3_ff_is_ff(&fx.oc, odin3_node_type(fx.module, node)));
    odin3_ffdata ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, node, &ff));
    TEST_ASSERT_EQUAL_UINT32(node.v, ff.node.v);
    TEST_ASSERT_EQUAL_UINT32(2, ff.width);
    TEST_ASSERT_EQUAL_UINT32(odin3_node_prov(fx.module, node).v, ff.prov.v);
    expect_flags(&ff, wc->features);
    expect_pols(&ff, flip);
    expect_sigs(&ff, index);
    expect_vals(&ff);
    odin3_ffdata_free(&ff);
}

static void test_ff_load_each_type(void) {
    for (int flip = 0; flip < 2; flip++) {
        build_word(flip);
        for (uint32_t i = 0; i < N_WORD; i++) {
            load_word(&k_word[i], flip);
        }
    }
    /* the brief's example: $sdffce with SRST_POLARITY 0 */
    build_word(0);
    odin3_ffdata ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, node_named("f8"), &ff));
    TEST_ASSERT_TRUE(ff.has_srst && ff.has_ce && ff.ce_over_srst && !ff.pol_srst);
    odin3_ffdata_free(&ff);
    check_full();
}

/* --- fine types, the const-D latch, misuse --------------------------------------------------- */

static const char *const k_fine = "module \\m\n"
                                  "  wire input 1 \\c\n"
                                  "  wire input 2 \\d\n"
                                  "  wire output 3 \\q\n"
                                  "  wire output 4 \\p\n"
                                  "  wire output 5 \\o\n"
                                  "  attribute \\init 1'1\n"
                                  "  wire output 6 \\n\n"
                                  "  cell $_DFF_P_ \\g\n"
                                  "    connect \\C \\c\n"
                                  "    connect \\D \\d\n"
                                  "    connect \\Q \\q\n"
                                  "  end\n"
                                  "  cell $_FF_ \\h\n"
                                  "    parameter \\INIT 0\n"
                                  "    connect \\D \\d\n"
                                  "    connect \\Q \\p\n"
                                  "  end\n"
                                  "  cell $_DLATCH_N_ \\j\n"
                                  "    parameter \\INIT 1\n"
                                  "    connect \\E \\c\n"
                                  "    connect \\D 1'0\n"
                                  "    connect \\Q \\o\n"
                                  "  end\n"
                                  "  cell $_DFF_N_ \\i\n"
                                  "    parameter \\INIT 0\n"
                                  "    connect \\C \\c\n"
                                  "    connect \\D \\d\n"
                                  "    connect \\Q \\n\n"
                                  "  end\n"
                                  "end\n";

/* Loads node, emits it unchanged; *ff keeps the model (freed by the caller). */
static odin3_node_id reemit(const char *name, odin3_ffdata *ff) {
    odin3_node_id old = node_named(name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, old, ff));
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_emit(&fx.oc, ff, &node));
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, old));
    TEST_ASSERT_EQUAL_UINT32(node.v, ff->node.v);
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named(name).v); /* the name kept */
    return node;
}

static void test_ff_fine_keeps_type(void) {
    build(k_fine);
    odin3_ffdata ff;
    odin3_node_id node = reemit("g", &ff);
    TEST_ASSERT_TRUE(ff.is_fine && ff.has_clk && ff.pol_clk && !ff.has_gclk);
    TEST_ASSERT_EQUAL_STRING("$_DFF_P_", type_name(node));
    TEST_ASSERT_EQUAL_UINT32(wnet("c", 0).v, pin_net_of(node, "C", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(node, "Q", 0).v);
    TEST_ASSERT_EQUAL_INT64(3, param_of(node, "INIT")->i); /* x keeps the unknown 3 */
    odin3_ffdata_free(&ff);
    /* $_FF_: has_gclk (not an FF without controls); INIT 0 read as its init */
    node = reemit("h", &ff);
    TEST_ASSERT_TRUE(ff.is_fine && ff.has_gclk && !ff.has_clk);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, vbit(&ff.val_init, 0));
    TEST_ASSERT_EQUAL_STRING("$_FF_", type_name(node));
    TEST_ASSERT_EQUAL_INT64(0, param_of(node, "INIT")->i);
    odin3_ffdata_free(&ff);
    /* the wire's init 1 wins over INIT 0 (first defined), and INIT follows it */
    node = reemit("i", &ff);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&ff.val_init, 0));
    TEST_ASSERT_EQUAL_STRING("$_DFF_N_", type_name(node));
    TEST_ASSERT_EQUAL_INT64(1, param_of(node, "INIT")->i);
    odin3_ffdata_free(&ff);
    check_full();
}

/* Yosys: a latch with a constant D loads as an async reset, and emits as a latch again. */
static void test_ff_const_latch(void) {
    build(k_fine);
    odin3_ffdata ff;
    odin3_node_id old = node_named("j");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, old, &ff));
    TEST_ASSERT_TRUE(ff.is_fine && ff.has_arst && !ff.has_aload && !ff.pol_arst);
    TEST_ASSERT_EQUAL_UINT32(wnet("c", 0).v, ff.sig_arst.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, vbit(&ff.val_arst, 0));
    TEST_ASSERT_EQUAL_size_t(0, ff.sig_ad.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&ff.val_init, 0)); /* from INIT 1 */
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ff_emit(&fx.oc, &ff, &node), last_error);
    TEST_ASSERT_EQUAL_STRING("$_DLATCH_N_", type_name(node));
    TEST_ASSERT_EQUAL_UINT32(wnet("c", 0).v, pin_net_of(node, "E", 0).v);
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_0).v, pin_net_of(node, "D", 0).v);
    TEST_ASSERT_EQUAL_INT64(1, param_of(node, "INIT")->i);
    /* the INIT parameter carries the init: no wire attribute is written */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, wnet("o", 0)));
    odin3_ffdata_free(&ff);
    check_full();
}

static void test_ff_misuse(void) {
    build(k_fine);
    odin3_ffdata ff;
    odin3_node_id port = odin3_module_port(fx.module, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ff_load(&fx.oc, port, &ff));
    char want[LOG_TEXT];
    TEST_ASSERT_TRUE(snprintf(want, sizeof want, "opt: ff_load: node %u is not a live flip-flop",
                              port.v) < (int)sizeof want);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
    TEST_ASSERT_EQUAL_size_t(0, ff.sig_q.len);
    TEST_ASSERT_FALSE(odin3_ff_is_ff(&fx.oc, odin3_node_type(fx.module, port)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, node_named("g"), &ff));
    ff.width = 2;
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ff_emit(&fx.oc, &ff, &node));
    TEST_ASSERT_EQUAL_STRING("opt: ff_emit: a vector is not 2 bits wide", last_error);
    ff.width = 1;
    uint32_t past = 1;
    odin3_ffdata out;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ff_slice(&ff, (odin3_ff_bits){&past, 1}, &out));
    TEST_ASSERT_EQUAL_STRING("opt: ff_slice: no FF, a bad vector or a bit past its width",
                             last_error);
    ff.has_ce = true; /* a fine FF the registry has no type for */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ff_emit(&fx.oc, &ff, &node));
    TEST_ASSERT_EQUAL_STRING("opt: ff_emit: no registered cell type (fine) for this FF",
                             last_error);
    TEST_ASSERT_TRUE(odin3_node_live(fx.module, ff.node));
    odin3_ffdata_free(&ff);
    check_full();
}

/* --- ff_slice --------------------------------------------------------------------------------- */

static const char *const k_adff4 = "module \\m\n"
                                   "  wire input 1 \\c\n"
                                   "  wire input 2 \\r\n"
                                   "  wire width 4 input 3 \\d\n"
                                   "  attribute \\init 4'0x1x\n"
                                   "  wire width 4 output 4 \\q\n"
                                   "  cell $adff \\f\n"
                                   "    parameter \\WIDTH 4\n"
                                   "    parameter \\CLK_POLARITY 1\n"
                                   "    parameter \\ARST_POLARITY 1\n"
                                   "    parameter \\ARST_VALUE 4'1010\n"
                                   "    connect \\CLK \\c\n"
                                   "    connect \\ARST \\r\n"
                                   "    connect \\D \\d\n"
                                   "    connect \\Q \\q\n"
                                   "  end\n"
                                   "end\n";

static void test_ff_slice(void) {
    build(k_adff4);
    odin3_ffdata ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, node_named("f"), &ff));
    static const uint32_t k_bits[] = {1, 3};
    odin3_ffdata out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_slice(&ff, (odin3_ff_bits){k_bits, 2}, &out));
    TEST_ASSERT_EQUAL_UINT32(2, out.width);
    TEST_ASSERT_EQUAL_UINT32(0, out.node.v);
    TEST_ASSERT_EQUAL_UINT32(ff.prov.v, out.prov.v);
    TEST_ASSERT_TRUE(out.has_clk && out.has_arst && out.pol_clk && out.pol_arst);
    TEST_ASSERT_EQUAL_UINT32(ff.sig_clk.v, out.sig_clk.v);
    TEST_ASSERT_EQUAL_size_t(2, out.val_arst.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&out.val_arst, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&out.val_arst, 1));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, vbit(&out.val_init, 0)); /* init 4'0x1x: bit 1 is 1 */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, vbit(&out.val_init, 1)); /* ... bit 3 is 0 */
    for (uint32_t i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("q", k_bits[i]).v, vnet(&out.sig_q, i).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("d", k_bits[i]).v, vnet(&out.sig_d, i).v);
    }
    TEST_ASSERT_EQUAL_size_t(0, out.sig_ad.len + out.sig_set.len + out.val_srst.len);
    odin3_ffdata_free(&out);
    odin3_ffdata_free(&ff);
}

/* --- ff_emit: the type table ------------------------------------------------------------------ */

/* Undriven Q (\q), every control on its own input; fresh FFs are emitted onto \q. */
static const char *const k_emit = "module \\m\n"
                                  "  wire input 1 \\c\n"
                                  "  wire input 2 \\e\n"
                                  "  wire input 3 \\r\n"
                                  "  wire input 4 \\l\n"
                                  "  wire width 2 input 5 \\d\n"
                                  "  wire width 2 input 6 \\a\n"
                                  "  wire width 2 input 7 \\s\n"
                                  "  wire width 2 input 8 \\k\n"
                                  "  wire width 2 output 9 \\q\n"
                                  "end\n";

static void push_wire(odin3_vec *vec, const char *name) {
    for (uint32_t i = 0; i < 2; i++) {
        *(odin3_net_id *)odin3_vec_push(vec) = wnet(name, i);
    }
}

static void push_bits(odin3_vec *vec, odin3_bit lsb, odin3_bit msb) {
    *(odin3_bit *)odin3_vec_push(vec) = lsb;
    *(odin3_bit *)odin3_vec_push(vec) = msb;
}

/* A fresh 2-bit FF with features f, every polarity low, ARST 2'10, SRST 2'01, init 2'01. */
static void fresh_ff(odin3_ffdata *ff, unsigned features) {
    odin3_ffdata_init(ff);
    ff->width = 2;
    ff->has_clk = (features & F_CLK) != 0;
    ff->has_ce = (features & F_CE) != 0;
    ff->has_srst = (features & F_SRST) != 0;
    ff->has_arst = (features & F_ARST) != 0;
    ff->has_aload = (features & F_ALOAD) != 0;
    ff->has_sr = (features & F_SR) != 0;
    ff->ce_over_srst = (features & F_CEO) != 0;
    push_wire(&ff->sig_q, "q");
    push_bits(&ff->val_init, ODIN3_BIT_1, ODIN3_BIT_0);
    ff->sig_clk = ff->has_clk ? wnet("c", 0) : (odin3_net_id){0};
    ff->sig_ce = ff->has_ce ? wnet("e", 0) : (odin3_net_id){0};
    ff->sig_srst = ff->has_srst ? wnet("r", 0) : (odin3_net_id){0};
    ff->sig_arst = ff->has_arst ? wnet("r", 0) : (odin3_net_id){0};
    ff->sig_aload = ff->has_aload ? wnet("l", 0) : (odin3_net_id){0};
    if (ff->has_clk) {
        push_wire(&ff->sig_d, "d");
    }
    if (ff->has_aload) {
        push_wire(&ff->sig_ad, "a");
    }
    if (ff->has_sr) {
        push_wire(&ff->sig_set, "s");
        push_wire(&ff->sig_clr, "k");
    }
    if (ff->has_arst) {
        push_bits(&ff->val_arst, ODIN3_BIT_0, ODIN3_BIT_1);
    }
    if (ff->has_srst) {
        push_bits(&ff->val_srst, ODIN3_BIT_1, ODIN3_BIT_0);
    }
}

typedef struct emit_case {
    unsigned features;
    const char *type; /* NULL: no cell */
} emit_case;

static const emit_case k_emit_table[] = {
    {F_CLK, "$dff"},
    {F_CLK | F_CE, "$dffe"},
    {F_CLK | F_ARST, "$adff"},
    {F_CLK | F_ARST | F_CE, "$adffe"},
    /* arst before aload (clocked_type's precedence, Task 2 review m-1 T1): Yosys's opt_dff never
     * makes the combination; FfData::emit picks the arst type */
    {F_CLK | F_ARST | F_ALOAD, "$adff"},
    {F_CLK | F_ALOAD, "$aldff"},
    {F_CLK | F_ALOAD | F_CE, "$aldffe"},
    {F_CLK | F_SRST, "$sdff"},
    {F_CLK | F_SRST | F_CE, "$sdffe"},
    {F_CLK | F_SRST | F_CE | F_CEO, "$sdffce"},
    {F_CLK | F_SR, "$dffsr"},
    {F_CLK | F_SR | F_CE, "$dffsre"},
    {F_ALOAD, "$dlatch"},
    {F_ALOAD | F_ARST, "$adlatch"},
    {F_ALOAD | F_SR, "$dlatchsr"},
    {F_SR, "$sr"},
    {F_ARST, "$dlatch"},
    {0, NULL},
};

/* The wire a port of the emitted cell must be on (ports by name, as fresh_ff set them). */
static const char *wire_for(const char *port, bool latch) {
    static const char *const k_map[][2] = {{"CLK", "c"},   {"SRST", "r"}, {"ARST", "r"},
                                           {"ALOAD", "l"}, {"AD", "a"},   {"SET", "s"},
                                           {"CLR", "k"},   {"Q", "q"}};
    if (strcmp(port, "EN") == 0) {
        return latch ? "l" : "e";
    }
    if (strcmp(port, "D") == 0) {
        return latch ? "a" : "d";
    }
    for (size_t i = 0; i < sizeof k_map / sizeof k_map[0]; i++) {
        if (strcmp(port, k_map[i][0]) == 0) {
            return k_map[i][1];
        }
    }
    TEST_FAIL_MESSAGE(port);
    return NULL;
}

/* Every port on its wire, every polarity 0, WIDTH 2, the values as fresh_ff set them. */
static void expect_cell(odin3_node_id node, bool latch) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    for (uint32_t i = 0; i < def->n_ports; i++) {
        odin3_pinslice pins = odin3_node_port(fx.module, node, i);
        const char *wire = wire_for(def->ports[i].name, latch);
        for (uint32_t k = 0; k < pins.count; k++) {
            TEST_ASSERT_EQUAL_UINT32(wnet(wire, k).v,
                                     odin3_pin_net(fx.module, (odin3_pin_id){pins.first.v + k}).v);
        }
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_value *val = odin3_node_param(fx.module, node, i);
        const char *name = def->params[i].name;
        if (strcmp(name, "WIDTH") == 0) {
            TEST_ASSERT_EQUAL_INT64(2, val->i);
        } else if (val->kind == ODIN3_VAL_INT) {
            TEST_ASSERT_EQUAL_INT64_MESSAGE(0, val->i, name);
        } else {
            bool arst = strcmp(name, "ARST_VALUE") == 0;
            TEST_ASSERT_EQUAL_UINT32(2, val->len);
            TEST_ASSERT_EQUAL_INT(arst ? ODIN3_BIT_0 : ODIN3_BIT_1, val->bits[0]);
            TEST_ASSERT_EQUAL_INT(arst ? ODIN3_BIT_1 : ODIN3_BIT_0, val->bits[1]);
        }
    }
}

/* {arst}: a $dlatch with EN = ARST and D on the constant nets of ARST_VALUE (2'10). */
static void expect_arst_latch(odin3_node_id node) {
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, pin_net_of(node, "EN", 0).v);
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_0).v, pin_net_of(node, "D", 0).v);
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_1).v, pin_net_of(node, "D", 1).v);
    TEST_ASSERT_EQUAL_INT64(0, param_of(node, "EN_POLARITY")->i);
}

/* {}: no cell; Q on the init's constants (2'01), the init gone from \q. */
static void expect_tied(void) {
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_1).v, wnet("q", 0).v);
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_0).v, wnet("q", 1).v);
    odin3_objref ref = {ODIN3_OBJ_WIRE, odin3_module_find_wire(fx.module, intern("q")).v};
    TEST_ASSERT_NULL(odin3_attr_get(fx.module, ref, intern("init")));
}

/* One row of the table on a fresh build. */
static void emit_row(const emit_case *ec) {
    build(k_emit);
    odin3_ffdata ff;
    fresh_ff(&ff, ec->features);
    if (ec->type == NULL) { /* a defined init on \q, removed as Yosys's remove() does */
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ffinit_set(fx.module, wnet("q", 0), ODIN3_BIT_1));
    }
    odin3_node_id node = {1};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ff_emit(&fx.oc, &ff, &node), last_error);
    TEST_ASSERT_TRUE(fx.oc.changed);
    odin3_ffdata_free(&ff);
    if (ec->type == NULL) {
        TEST_ASSERT_EQUAL_UINT32(0, node.v);
        expect_tied();
        return;
    }
    TEST_ASSERT_EQUAL_STRING_MESSAGE(ec->type, type_name(node), ec->type);
    if (ec->features == F_ARST) {
        expect_arst_latch(node);
    } else {
        expect_cell(node, (ec->features & F_CLK) == 0 && (ec->features & F_ALOAD) != 0);
    }
    /* init 2'01 written on \q */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, odin3_ffinit_get(fx.module, wnet("q", 1)));
}

static void test_ff_emit_table(void) {
    for (size_t i = 0; i < sizeof k_emit_table / sizeof k_emit_table[0]; i++) {
        emit_row(&k_emit_table[i]);
        check_full();
    }
}

/* A loaded node rebuilt as another type keeps its name and Q; a changed init bit is written. */
static void test_ff_emit_rebuild(void) {
    build(k_adff4);
    odin3_node_id old = node_named("f");
    odin3_ffdata ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, old, &ff));
    ff.has_ce = true;
    ff.pol_ce = true;
    ff.sig_ce = wnet("r", 0);
    *(odin3_bit *)odin3_vec_at(&ff.val_init, 0) = ODIN3_BIT_0;
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ff_emit(&fx.oc, &ff, &node), last_error);
    TEST_ASSERT_FALSE(odin3_node_live(fx.module, old));
    TEST_ASSERT_EQUAL_STRING("$adffe", type_name(node));
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("f").v);
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 3).v, pin_net_of(node, "Q", 3).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, pin_net_of(node, "EN", 0).v);
    odin3_objref ref = {ODIN3_OBJ_WIRE, odin3_module_find_wire(fx.module, intern("q")).v};
    const odin3_value *init = odin3_attr_get(fx.module, ref, intern("init"));
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, init->bits[0]); /* 4'0x1x -> 4'0x10 */
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, init->bits[1]);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, init->bits[3]);
    odin3_ffdata_free(&ff);
    check_full();
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* After an emit of \f: undone (the old FF, its init written back), partial (the tie path only:
 * kit_connect's merges cannot be reserved, Task 1's concern 1; logged as "discard it") or done. */
static void oom_expect(odin3_status st, odin3_node_id node, bool tie, uint32_t *partial) {
    odin3_node_id old = node_named("f");
    bool undone = st != ODIN3_OK && odin3_node_valid(old) && odin3_node_live(fx.module, old);
    if (undone) {
        TEST_ASSERT_EQUAL_STRING("$adff", type_name(old));
        TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q", 1)));
        TEST_ASSERT_EQUAL_UINT32(0, n_discard);
    } else if (st != ODIN3_OK) {
        TEST_ASSERT_TRUE(tie);                  /* the rebuild path is undone at every point */
        TEST_ASSERT_EQUAL_UINT32(1, n_discard); /* the half-done rewrite says so, once */
        (*partial)++;
    } else if (tie) {
        TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_1).v, wnet("q", 1).v);
    } else {
        TEST_ASSERT_EQUAL_UINT32(wnet("q", 1).v, pin_net_of(node, "Q", 1).v);
    }
}

/* One emit of the loaded \f ($adff -> $adffe, or tied) with fail injected; true when done. */
static bool emit_oom_step(long fail, bool tie, uint32_t *partial) {
    build(k_adff4);
    odin3_ffdata ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, node_named("f"), &ff));
    ff.has_clk = !tie; /* tie: no controls left, Q tied to init 4'0x1x */
    ff.has_arst = !tie;
    ff.has_ce = !tie;
    ff.sig_ce = wnet("r", 0);
    odin3_node_id node = {0};
    n_discard = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_ff_emit(&fx.oc, &ff, &node);
    odin3_util_set_alloc_fail_after(-1);
    odin3_ffdata_free(&ff);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, last_error);
    check_full();
    oom_expect(st, node, tie, partial);
    return st == ODIN3_OK;
}

static void test_ff_emit_oom(void) {
    uint32_t partial[2] = {0, 0};
    for (int tie = 0; tie < 2; tie++) {
        bool done = false;
        for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
            done = emit_oom_step(fail, tie != 0, &partial[tie]);
        }
        TEST_ASSERT_TRUE(done);
    }
    TEST_ASSERT_EQUAL_UINT32(0, partial[0]);
    TEST_ASSERT_TRUE(partial[1] <= 4); /* at most one per merged Q bit */
    printf("ff_emit OOM: %u partial points on the tie path\n", partial[1]);
}

/* Task 2 review m-1 T10: the const-D rule is for a plain latch; an $adlatch with a constant D
 * keeps its aload (D stays the AD data) and its arst. */
static void test_ff_const_adlatch(void) {
    build("module \\m\n  wire input 1 \\e\n  wire input 2 \\r\n  wire width 2 \\q\n"
          "  cell $adlatch \\j\n    parameter \\WIDTH 2\n    parameter \\EN_POLARITY 1\n"
          "    parameter \\ARST_POLARITY 1\n    parameter \\ARST_VALUE 2'10\n"
          "    connect \\EN \\e\n    connect \\ARST \\r\n    connect \\D 2'01\n"
          "    connect \\Q \\q\n  end\nend\n");
    odin3_ffdata ff;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ff_load(&fx.oc, node_named("j"), &ff));
    TEST_ASSERT_TRUE(ff.has_aload && ff.has_arst && !ff.has_clk);
    TEST_ASSERT_EQUAL_size_t(2, ff.sig_ad.len);
    TEST_ASSERT_EQUAL_UINT32(wnet("e", 0).v, ff.sig_aload.v);
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, ff.sig_arst.v);
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ff_emit(&fx.oc, &ff, &node), last_error);
    TEST_ASSERT_EQUAL_STRING("$adlatch", type_name(node));
    odin3_ffdata_free(&ff);
    check_full();
}

/* Task 2 review m-1 T13: an x init on a fine FF whose node had INIT 2 (don't care) keeps the 2. */
static void test_ff_fine_keeps_dont_care(void) {
    build("module \\m\n  wire input 1 \\c\n  wire input 2 \\d\n  wire output 3 \\q\n"
          "  cell $_DFF_P_ \\g\n    parameter \\INIT 2\n    connect \\C \\c\n"
          "    connect \\D \\d\n    connect \\Q \\q\n  end\nend\n");
    odin3_ffdata ff;
    odin3_node_id node = reemit("g", &ff);
    TEST_ASSERT_EQUAL_STRING("$_DFF_P_", type_name(node));
    TEST_ASSERT_EQUAL_INT64(2, param_of(node, "INIT")->i);
    odin3_ffdata_free(&ff);
    check_full();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ff_load_each_type);
    RUN_TEST(test_ff_fine_keeps_type);
    RUN_TEST(test_ff_const_latch);
    RUN_TEST(test_ff_const_adlatch);
    RUN_TEST(test_ff_fine_keeps_dont_care);
    RUN_TEST(test_ff_misuse);
    RUN_TEST(test_ff_slice);
    RUN_TEST(test_ff_emit_table);
    RUN_TEST(test_ff_emit_rebuild);
    RUN_TEST(test_ff_emit_oom);
    return UNITY_END();
}
