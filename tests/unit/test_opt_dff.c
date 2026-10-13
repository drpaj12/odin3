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

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LOG_TEXT = 512,
    OOM_LIMIT = 4000,
    MERGE_OOM_LIMIT = 16000,
    N_WORD = 15,
    PERSISTENT_STRIDE = 1,
    SILENT_FF_C = 2
};

static char last_error[LOG_TEXT];
static uint32_t n_errors;
static uint32_t n_discard; /* errors saying the design must be discarded (a half-done rewrite) */

/* Every line logged since setUp, for the exact-text assertions of the opt_dff tests. */
enum { LINES_MAX = 64 };

typedef struct log_line {
    odin3_log_level level;
    char text[LOG_TEXT];
} log_line;

static log_line lines[LINES_MAX];
static uint32_t n_lines;

static void record_line(odin3_log_level level, const char *msg) {
    if (n_lines < LINES_MAX) {
        (void)snprintf(lines[n_lines].text, sizeof lines[n_lines].text, "%s", msg);
        lines[n_lines].level = level;
        n_lines++;
    }
}

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    record_line(level, msg);
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
    n_lines = 0;
    odin3_log_set_sink(sink, NULL);
    odin3_log_set_level(ODIN3_LOG_DEBUG);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    close_fixture();
    odin3_log_set_level(ODIN3_LOG_INFO);
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

/* ===== opt_dff: rules 1-9, constbits and the counters (2F spec §9.3; plan Task 11)
 * ================
 *
 * One odin3_opt_dff_run call per test, on a hand-built module \m. Expectations were re-derived
 * with the VTR build's Yosys 0.55: `yosys -p "read_rtlil X.il; opt_dff [-keepdc]; write_rtlil"`
 * (the data cases record their command in each .expect header).
 */

/* The standard wires: clk d r e a (inputs), q (output). */
#define W_STD                                                                                      \
    "  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\r\n  wire input 4 \\e\n"           \
    "  wire input 5 \\a\n  wire output 6 \\q\n"
#define MOD(body) "module \\m\n" body "end\n"
#define CELL(type, name, params, conns) "  cell " type " \\" name "\n" params conns "  end\n"
#define PW "    parameter \\WIDTH 1\n"
#define PCLK "    parameter \\CLK_POLARITY 1\n"
#define PEN "    parameter \\EN_POLARITY 1\n"
#define PARST "    parameter \\ARST_POLARITY 1\n    parameter \\ARST_VALUE 1'1\n"
#define PSRST "    parameter \\SRST_POLARITY 1\n    parameter \\SRST_VALUE 1'0\n"
#define PALOAD "    parameter \\ALOAD_POLARITY 1\n"
#define CCLK "    connect \\CLK \\clk\n"
#define CQ "    connect \\Q \\q\n"

static void run_dff(void) {
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, odin3_opt_dff_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts), last_error);
    check_full();
}

/* Live nodes whose type is an FF, and the first of them. */
static uint32_t count_ffs(void) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        hits += odin3_node_live(fx.module, node) &&
                        odin3_ff_is_ff(&fx.oc, odin3_node_type(fx.module, node))
                    ? 1U
                    : 0U;
    }
    return hits;
}

static odin3_node_id first_ff(void) {
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(fx.module, node) &&
            odin3_ff_is_ff(&fx.oc, odin3_node_type(fx.module, node))) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

static uint32_t count_of(const char *type) {
    uint32_t hits = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        hits += odin3_node_live(fx.module, node) && strcmp(type_name(node), type) == 0 ? 1U : 0U;
    }
    return hits;
}

static void expect_ff(const char *type) {
    TEST_ASSERT_EQUAL_UINT32(1, count_ffs());
    TEST_ASSERT_EQUAL_STRING(type, type_name(first_ff()));
}

static void expect_gone(void) {
    TEST_ASSERT_EQUAL_UINT32(0, count_ffs());
}

static void expect_const(const char *wire, uint32_t bit, odin3_const want) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, odin3_net_const_value(fx.module, wnet(wire, bit)), wire);
}

static uint32_t count_lines(odin3_log_level level, const char *text) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < n_lines; i++) {
        hits += lines[i].level == level && strcmp(lines[i].text, text) == 0 ? 1U : 0U;
    }
    return hits;
}

/* One DEBUG line "opt_dff: module 'm': <action> on <type> '<label>'" was logged. */
static void expect_action(const char *action, const char *type, const char *label) {
    char want[LOG_TEXT];
    TEST_ASSERT_TRUE(snprintf(want, sizeof want, "opt_dff: module 'm': %s on %s '%s'", action, type,
                              label) < (int)sizeof want);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_lines(ODIN3_LOG_DEBUG, want), want);
}

static void expect_info(uint32_t changed, uint32_t bits) {
    char want[LOG_TEXT];
    TEST_ASSERT_TRUE(snprintf(want, sizeof want, "opt_dff: %u flip-flops changed, %u constant bits",
                              changed, bits) < (int)sizeof want);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_lines(ODIN3_LOG_INFO, want), want);
    TEST_ASSERT_EQUAL_UINT32(changed, fx.counts.dff_changed);
    TEST_ASSERT_EQUAL_UINT32(bits, fx.counts.dff_const_bits);
}

/* Cell \f becomes unnamed and its "$c<ID>" label is written into out. */
static void unname_f(char *out, size_t size) {
    odin3_node_id node = node_named("f");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_rename(fx.module, node, 0));
    TEST_ASSERT_TRUE(snprintf(out, size, "$c%u", (unsigned)node.v) < (int)size);
}

/* --- rule 2: SET/CLR ----------------------------------------------------------------------------
 */

#define SR_PARAMS PW PCLK "    parameter \\SET_POLARITY 1\n    parameter \\CLR_POLARITY 1\n"
#define SR_CONNS(set, clr)                                                                         \
    CCLK "    connect \\SET " set "\n    connect \\CLR " clr "\n    connect \\D \\d\n" CQ

static void test_dff_sr_clr_always(void) {
    build(MOD(W_STD CELL("$dffsr", "f", SR_PARAMS, SR_CONNS("\\a", "1'1"))));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_0);
    expect_action("handling always-active CLR", "$dffsr", "f");
    expect_info(1, 0);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.dff_action[ODIN3_OPT_DFF_ALWAYS_CLR]);
}

static void test_dff_sr_set_always(void) {
    /* active-high CLR: Q = ~CLR, one $not; an active-low CLR: Q is the CLR net itself */
    build(MOD(W_STD CELL("$dffsr", "f", SR_PARAMS, SR_CONNS("1'1", "\\r"))));
    run_dff();
    expect_gone();
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$not"));
    expect_action("handling always-active SET", "$dffsr", "f");
    build(MOD(W_STD CELL("$dffsr", "f",
                         PW PCLK "    parameter \\SET_POLARITY 1\n    parameter \\CLR_POLARITY 0\n",
                         SR_CONNS("1'1", "\\r"))));
    run_dff();
    expect_gone();
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$not"));
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, wnet("q", 0).v);
}

/* A 2-bit $dffsr whose bit 1 has an always-active CLR: bit 1 of Q is 0, the FF shrinks to one
 * bit and keeps its name and attributes. */
#define SR2_TEXT                                                                                   \
    MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\r\n"                           \
        "  wire width 2 input 4 \\k\n  wire width 2 input 5 \\s\n  wire width 2 input 6 \\dd\n"    \
        "  wire width 2 output 7 \\q\n"                                                            \
        "  attribute \\keep 1\n  cell $dffsr \\f\n    parameter \\WIDTH 2\n" PCLK                  \
        "    parameter \\SET_POLARITY 1\n    parameter \\CLR_POLARITY 1\n" CCLK                    \
        "    connect \\SET \\s\n    connect \\CLR { 1'1 \\k [0] }\n    connect \\D \\dd\n"         \
        "    connect \\Q \\q\n  end\n")

static void test_dff_sr_partial_keeps_name_and_attrs(void) {
    build(SR2_TEXT);
    run_dff();
    expect_ff("$dffsr");
    odin3_node_id node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("f").v); /* the name is kept */
    TEST_ASSERT_EQUAL_UINT32(1, odin3_node_port(fx.module, node, port_of(node, "Q")).count);
    const odin3_value *keep = odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, node.v},
                                             intern("keep")); /* carried over */
    TEST_ASSERT_NOT_NULL(keep);
    TEST_ASSERT_EQUAL_INT64(1, keep->i);
    expect_const("q", 1, ODIN3_CONST_0);
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(node, "Q", 0).v);
    expect_action("handling always-active CLR", "$dffsr", "f");
}

/* OP-15: the shrunk FF, and a helper cell a rule makes, have a DERIVED record of the old FF's. */
static void expect_derived_from(odin3_node_id node, odin3_prov_id parent) {
    const odin3_prov_record *rec = odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, node));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(parent.v, rec->parents.ids[0].v);
}

static void test_dff_provenance(void) {
    build(MOD(W_STD CELL("$dffsr", "f", SR_PARAMS, SR_CONNS("1'1", "\\r"))));
    odin3_prov_id parent = odin3_node_prov(fx.module, node_named("f"));
    TEST_ASSERT_TRUE(parent.v != 0);
    run_dff();
    expect_gone();
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$not"));
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        if (strcmp(type_name((odin3_node_id){id}), "$not") == 0) {
            expect_derived_from((odin3_node_id){id}, parent);
        }
    }
    build(SR2_TEXT);
    parent = odin3_node_prov(fx.module, node_named("f"));
    run_dff();
    expect_ff("$dffsr");
    expect_derived_from(first_ff(), parent);
}

/* A 2-bit $dffsr whose bit 0 has an always-active SET (so Q[0] = ~CLR[0], a new $not) and whose
 * bit 1 stays: the FF shrinks to one bit AND a gate is made; both are DERIVED from the old FF. */
static void test_dff_provenance_shrink_and_gate(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 3 \\r\n  wire width 2 input 4 \\k\n"
              "  wire width 2 input 5 \\s\n  wire width 2 input 6 \\dd\n  wire width 2 \\q\n"
              "  cell $dffsr \\f\n    parameter \\WIDTH 2\n" PCLK
              "    parameter \\SET_POLARITY 1\n    parameter \\CLR_POLARITY 1\n" CCLK
              "    connect \\SET { \\s [1] 1'1 }\n    connect \\CLR { \\k [1] \\r }\n"
              "    connect \\D \\dd\n    connect \\Q \\q\n  end\n"));
    odin3_prov_id parent = odin3_node_prov(fx.module, node_named("f"));
    TEST_ASSERT_TRUE(parent.v != 0);
    run_dff();
    expect_ff("$dffsr");
    TEST_ASSERT_EQUAL_UINT32(
        1, odin3_node_port(fx.module, first_ff(), port_of(first_ff(), "Q")).count);
    expect_derived_from(first_ff(), parent);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$not"));
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        if (strcmp(type_name((odin3_node_id){id}), "$not") == 0) {
            expect_derived_from((odin3_node_id){id}, parent);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, seen);
    check_full();
}

static const char *const k_keep_text =
    MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire width 2 input 4 \\k\n"
        "  wire width 2 input 5 \\s\n  wire width 2 input 6 \\dd\n  wire width 2 \\q\n"
        "  attribute \\keep 1\n  cell $dffsr \\f\n    parameter \\WIDTH 2\n" PCLK
        "    parameter \\SET_POLARITY 1\n    parameter \\CLR_POLARITY 1\n" CCLK
        "    connect \\SET \\s\n    connect \\CLR { 1'1 \\k [0] }\n    connect \\D \\dd\n"
        "    connect \\Q \\q\n  end\n");

/* Carried item 1: a (* keep *) FF the rule shrinks survives opt_clean, with its attribute. */
static void test_dff_keep_survives(void) {
    build(k_keep_text);
    run_dff();
    expect_ff("$dffsr");
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_clean_run(&fx.ctx, fx.res.design, &fx.opts, &counts));
    expect_ff("$dffsr");
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, first_ff().v}, intern("keep")));
    check_full();
}

/* ... and the same FF without keep is removed by opt_clean (its Q is unread): the test above
 * would pass vacuously if the shrunk FF were never cleaned at all. */
static void test_dff_keep_control(void) {
    const char *text = k_keep_text;
    char plain[1024];
    const char *at = strstr(text, "  attribute \\keep 1\n");
    TEST_ASSERT_NOT_NULL(at);
    size_t head = (size_t)(at - text);
    TEST_ASSERT_TRUE(snprintf(plain, sizeof plain, "%.*s%s", (int)head, text,
                              at + strlen("  attribute \\keep 1\n")) < (int)sizeof plain);
    build(plain);
    run_dff();
    expect_ff("$dffsr");
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_clean_run(&fx.ctx, fx.res.design, &fx.opts, &counts));
    expect_gone();
}

/* Never-active CLR (all CLR constant 0, uniform SET) -> $adff with all-1 reset; never-active SET
 * -> all-0 reset; CLR and SET on complementary bits of one signal -> the derived value. */
static void test_dff_sr_to_arst(void) {
    build(MOD(W_STD CELL("$dffsr", "f", SR_PARAMS, SR_CONNS("\\a", "1'0"))));
    run_dff();
    expect_ff("$adff");
    odin3_node_id node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 0).v, pin_net_of(node, "ARST", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, param_of(node, "ARST_VALUE")->bits[0]);
    expect_action("removing never-active CLR", "$dffsr", "f");
    build(MOD(W_STD CELL("$dffsr", "f", SR_PARAMS, SR_CONNS("1'0", "\\r"))));
    run_dff();
    expect_ff("$adff");
    node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, pin_net_of(node, "ARST", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, param_of(node, "ARST_VALUE")->bits[0]);
    expect_action("removing never-active SET", "$dffsr", "f");
}

/* --- rule 3: ALOAD ------------------------------------------------------------------------------
 */

#define ALD_PARAMS PW PCLK PALOAD
#define ALD_CONNS(aload, ad)                                                                       \
    CCLK "    connect \\ALOAD " aload "\n    connect \\AD " ad "\n    connect \\D \\d\n" CQ

static void test_dff_aload_never(void) {
    build(MOD(W_STD CELL("$aldff", "f", ALD_PARAMS, ALD_CONNS("1'0", "\\a"))));
    char label[32];
    unname_f(label, sizeof label);
    run_dff();
    expect_ff("$dff");
    expect_action("removing never-active ALOAD", "$aldff", label);
    char want[LOG_TEXT];
    TEST_ASSERT_TRUE(snprintf(want, sizeof want,
                              "opt_dff: module 'm': removing never-active ALOAD on $aldff '%s'",
                              label) < (int)sizeof want);
    TEST_ASSERT_EQUAL_UINT32(1, count_lines(ODIN3_LOG_DEBUG, want));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.dff_action[ODIN3_OPT_DFF_NEVER_ALOAD]);
}

static void test_dff_aload_always(void) {
    build(MOD(W_STD CELL("$aldff", "f", ALD_PARAMS, ALD_CONNS("1'1", "\\a"))));
    run_dff();
    expect_gone();
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 0).v, wnet("q", 0).v);
    expect_action("handling always-active ALOAD", "$aldff", "f");
}

static void test_dff_aload_const_ad(void) {
    build(MOD(W_STD CELL("$aldff", "f", ALD_PARAMS, ALD_CONNS("\\r", "1'0"))));
    run_dff();
    expect_ff("$adff");
    odin3_node_id node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, pin_net_of(node, "ARST", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, param_of(node, "ARST_VALUE")->bits[0]);
    TEST_ASSERT_EQUAL_INT64(1, param_of(node, "ARST_POLARITY")->i);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.dff_action[ODIN3_OPT_DFF_ALOAD_TO_ARST]);
}

/* --- rule 4: ARST -------------------------------------------------------------------------------
 */

#define ARST_CONNS(arst) CCLK "    connect \\ARST " arst "\n    connect \\D \\d\n" CQ

static void test_dff_arst(void) {
    build(MOD(W_STD CELL("$adff", "f", PW PCLK PARST, ARST_CONNS("1'0"))));
    run_dff();
    expect_ff("$dff");
    expect_action("removing never-active ARST", "$adff", "f");
    build(MOD(W_STD CELL("$adff", "f", PW PCLK PARST, ARST_CONNS("1'1"))));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_1); /* ARST_VALUE 1'1 */
    expect_action("handling always-active ARST", "$adff", "f");
    /* an x reset is "active" unless -keepdc */
    build(MOD(W_STD CELL("$adff", "f", PW PCLK PARST, ARST_CONNS("1'x"))));
    run_dff();
    expect_gone();
    build(MOD(W_STD CELL("$adff", "f", PW PCLK PARST, ARST_CONNS("1'x"))));
    fx.opts.keepdc = true;
    run_dff();
    expect_ff("$adff");
}

/* --- rule 5: SRST -------------------------------------------------------------------------------
 */

#define SRST_CONNS(srst) CCLK "    connect \\SRST " srst "\n    connect \\D \\d\n" CQ

static void test_dff_srst(void) {
    build(MOD(W_STD CELL("$sdff", "f", PW PCLK PSRST, SRST_CONNS("1'0"))));
    run_dff();
    expect_ff("$dff");
    build(MOD(W_STD CELL("$sdff", "f", PW PCLK PSRST, SRST_CONNS("1'1"))));
    run_dff(); /* rule 5 sets D to the reset value, constbits then ties Q (init x yields) */
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_0);
    expect_action("handling always-active SRST", "$sdff", "f");
    expect_info(1, 1);
}

/* --- rule 6: EN ---------------------------------------------------------------------------------
 */

#define EN_CONNS(en) CCLK "    connect \\EN " en "\n    connect \\D \\d\n" CQ

static void test_dff_en(void) {
    build(MOD(W_STD CELL("$dffe", "f", PW PCLK PEN, EN_CONNS("1'1"))));
    run_dff();
    expect_ff("$dff");
    expect_action("handling always-active EN", "$dffe", "f");
    build(MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  attribute \\init 1'1\n"
              "  wire output 6 \\q\n" CELL("$dffe", "f", PW PCLK PEN, EN_CONNS("1'0"))));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_1);
    expect_action("removing never-active EN", "$dffe", "f");
}

static void test_dff_en_never_srst(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\r\n"
              "  attribute \\init 1'1\n  wire output 6 \\q\n"
              "  cell $sdffe \\f\n" PW PCLK PEN PSRST CCLK
              "    connect \\EN 1'0\n    connect \\SRST \\r\n    connect \\D \\d\n" CQ "  end\n"));
    run_dff();
    expect_ff("$dffe");
    odin3_node_id node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, pin_net_of(node, "EN", 0).v);
    TEST_ASSERT_EQUAL_INT64(1, param_of(node, "EN_POLARITY")->i); /* SRST_POLARITY */
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_0).v, pin_net_of(node, "D", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q", 0)));
}

/* --- rule 7: constant clock ---------------------------------------------------------------------
 */

static void test_dff_const_clk(void) {
    build(MOD("  wire input 2 \\d\n  attribute \\init 1'0\n  wire output 6 \\q\n"
              "  cell $dff \\f\n" PW PCLK "    connect \\CLK 1'0\n    connect \\D \\d\n" CQ
              "  end\n"));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_0);
    expect_action("handling const clock", "$dff", "f");
    /* -keepdc with an undefined init: D becomes Q, the FF stays */
    build(MOD("  wire input 2 \\d\n  wire output 6 \\q\n"
              "  cell $dff \\f\n" PW PCLK "    connect \\CLK 1'0\n    connect \\D \\d\n" CQ
              "  end\n"));
    fx.opts.keepdc = true;
    run_dff();
    expect_ff("$dff");
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(first_ff(), "D", 0).v);
}

/* --- rules 8 and 9: D = Q, AD = Q ---------------------------------------------------------------
 */

static void test_dff_d_eq_q(void) {
    build(MOD("  wire input 1 \\clk\n  attribute \\init 1'1\n  wire output 6 \\q\n"
              "  cell $dff \\f\n" PW PCLK CCLK "    connect \\D \\q\n" CQ "  end\n"));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_1);
    expect_action("handling D = Q", "$dff", "f");
}

#define DQ_SRST_TEXT(init)                                                                         \
    MOD("  wire input 1 \\clk\n  wire input 3 \\r\n" init "  wire output 6 \\q\n"                  \
        "  cell $sdff \\f\n" PW PCLK PSRST CCLK "    connect \\SRST \\r\n    connect \\D \\q\n" CQ \
        "  end\n")

static void test_dff_d_eq_q_srst(void) {
    build(DQ_SRST_TEXT("  attribute \\init 1'1\n"));
    run_dff();
    expect_ff("$dffe");
    odin3_node_id node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, pin_net_of(node, "EN", 0).v);
    TEST_ASSERT_EQUAL_INT64(1, param_of(node, "EN_POLARITY")->i);
    TEST_ASSERT_EQUAL_UINT32(const_net(ODIN3_BIT_0).v, pin_net_of(node, "D", 0).v);
    build(DQ_SRST_TEXT("")); /* init x: constbits ties Q to 0 (x yields) */
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_0);
}

static void test_dff_aload_d_eq_q(void) {
    build(MOD("  wire input 4 \\e\n  attribute \\init 1'1\n  wire output 6 \\q\n"
              "  cell $dlatch \\f\n" PW PEN "    connect \\EN \\e\n    connect \\D \\q\n" CQ
              "  end\n"));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_1);
    expect_action("handling AD = Q", "$dlatch", "f");
}

/* --- constbits ----------------------------------------------------------------------------------
 */

static const char *const k_const_d =
    MOD("  wire input 1 \\clk\n  wire output 6 \\q\n"
        "  cell $dff \\f\n" PW PCLK CCLK "    connect \\D 1'0\n" CQ "  end\n");

static void test_dff_constbits_x_init(void) {
    build(k_const_d);
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_0);
    expect_action("setting constant 0 at bit 0", "$dff", "f");
    expect_info(0, 1);
    build(k_const_d);
    fx.opts.keepdc = true;
    run_dff();
    expect_ff("$dff");
}

static void test_dff_constbits_conflict(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 3 \\r\n  wire output 6 \\q\n"
              "  cell $adff \\f\n" PW PCLK PARST CCLK "    connect \\ARST \\r\n"
              "    connect \\D 1'0\n" CQ "  end\n"));
    run_dff();
    expect_ff("$adff"); /* D = 0 against ARST_VALUE = 1 */
    expect_info(0, 0);
}

/* Point (l): a Q tied to a constant in this call does not make the D that reads it constant. */
static const char *const k_pipeline =
    MOD("  wire input 1 \\clk\n  wire output 2 \\q1\n  wire output 3 \\q2\n  wire output 4 \\q3\n"
        "  cell $dff \\f1\n" PW PCLK CCLK "    connect \\D 1'0\n    connect \\Q \\q1\n  end\n"
        "  cell $dff \\f2\n" PW PCLK CCLK "    connect \\D \\q1\n    connect \\Q \\q2\n  end\n"
        "  cell $dff \\f3\n" PW PCLK CCLK "    connect \\D \\q2\n    connect \\Q \\q3\n  end\n");

static void test_dff_constbits_one_stage(void) {
    build(k_pipeline);
    run_dff();
    TEST_ASSERT_EQUAL_UINT32(2, count_ffs());
    expect_const("q1", 0, ODIN3_CONST_0);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(fx.module, wnet("q2", 0)));
    expect_info(0, 1);
    run_dff();
    TEST_ASSERT_EQUAL_UINT32(1, count_ffs());
    expect_const("q2", 0, ODIN3_CONST_0);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_NONE, odin3_net_const_value(fx.module, wnet("q3", 0)));
    run_dff();
    expect_gone();
    expect_const("q3", 0, ODIN3_CONST_0);
}

/* --- counts, processes, other shapes ------------------------------------------------------------
 */

static void test_dff_counts(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 4 \\e\n"
              "  wire output 5 \\q1\n  wire output 6 \\q2\n"
              "  cell $adff \\f1\n" PW PCLK PARST CCLK
              "    connect \\ARST 1'0\n    connect \\D \\d\n    connect \\Q \\q1\n  end\n"
              "  cell $dffe \\f2\n" PW PCLK PEN CCLK
              "    connect \\EN 1'1\n    connect \\D \\d\n    connect \\Q \\q2\n  end\n"));
    run_dff();
    expect_info(2, 0);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.dff_action[ODIN3_OPT_DFF_NEVER_ARST]);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.dff_action[ODIN3_OPT_DFF_ALWAYS_EN]);
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$dff"));
}

static void test_dff_proc_opaque(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 2 \\a\n  wire output 3 \\p\n"
              "  attribute \\init 1'1\n  wire output 6 \\q\n"
              "  cell $dff \\f\n" PW PCLK CCLK "    connect \\D \\q\n" CQ "  end\n"
              "  process $p1\n    sync always\n      update \\p \\a\n  end\n"));
    run_dff();
    expect_gone();
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc")); /* not an FF, and not touched */
}

/* A pin left open is no rewrite: the FF is left alone, constants and all. */
static void test_dff_open_pin_untouched(void) {
    build(MOD("  wire input 1 \\clk\n  wire output 6 \\q\n"
              "  cell $dffe \\f\n" PW PCLK PEN CCLK "    connect \\D 1'0\n" CQ "  end\n"));
    run_dff();
    expect_ff("$dffe");
    expect_info(0, 0);
}

/* Fine FFs get the type-preserving rules: a $_DFF_P_ with D = Q and init 1 is its init. */
static void test_dff_fine(void) {
    build(MOD("  wire input 1 \\clk\n  attribute \\init 1'1\n  wire output 6 \\q\n"
              "  cell $_DFF_P_ \\f\n    connect \\C \\clk\n    connect \\D \\q\n" CQ "  end\n"));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_1);
}

/* --- rules 10 and 11: SRST and EN merging, the feedback patterns ------------------------------
 *
 * Yosys 0.55 (the VTR build) re-derivation, `yosys -p "read_rtlil X.il; opt_dff; write_rtlil"`:
 * - srst_merge: $dff, D = $mux(A d, B 1'0, S r) -> $sdff SRST r, SRST_POLARITY 1, SRST_VALUE 1'0,
 *   D d, the $mux left in place; A 1'0, B d -> SRST_POLARITY 0.
 * - en_merge: D = $mux(A q, B d, S e) -> $dffe EN e.
 * - srst_then_en: one call gives $sdffe; en_then_srst: one call gives $sdffce (Yosys pushes the
 *   FFs a rule makes on its work list; the plan's "$dffe first" example is not what it does).
 */
#define MUX1(name, a, b, s, y)                                                                     \
    CELL("$mux", name, PW,                                                                         \
         "    connect \\A " a "\n    connect \\B " b "\n    connect \\S " s "\n    connect \\Y " y \
         "\n")
#define W_MID "  wire \\m1\n  wire \\m2\n"
#define FF_ON(d) CELL("$dff", "f", PW PCLK, CCLK "    connect \\D " d "\n" CQ)

static void expect_pin(odin3_node_id node, const char *port, odin3_net_id want) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.v, pin_net_of(node, port, 0).v, port);
}

static void expect_bit_param(odin3_node_id node, const char *name, odin3_bit want) {
    const odin3_value *val = param_of(node, name);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, val->kind);
    TEST_ASSERT_EQUAL_UINT32(1, val->len);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, val->bits[0], name);
}

static void expect_int_param(odin3_node_id node, const char *name, int64_t want) {
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want, param_of(node, name)->i, name);
}

static void test_dff_srst_merge(void) {
    build(MOD(W_STD W_MID MUX1("mx", "\\d", "1'0", "\\r", "\\m1") FF_ON("\\m1")));
    run_dff();
    expect_ff("$sdff");
    odin3_node_id node = first_ff();
    expect_pin(node, "SRST", wnet("r", 0));
    expect_pin(node, "D", wnet("d", 0));
    expect_pin(node, "Q", wnet("q", 0));
    expect_pin(node, "CLK", wnet("clk", 0));
    expect_int_param(node, "SRST_POLARITY", 1);
    expect_bit_param(node, "SRST_VALUE", ODIN3_BIT_0);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mux")); /* opt_clean's */
    expect_action("adding SRST", "$dff", "f");
    expect_info(1, 0);
}

static void test_dff_srst_polarity(void) {
    build(MOD(W_STD W_MID MUX1("mx", "1'0", "\\d", "\\r", "\\m1") FF_ON("\\m1")));
    run_dff();
    expect_ff("$sdff");
    odin3_node_id node = first_ff();
    expect_pin(node, "SRST", wnet("r", 0));
    expect_pin(node, "D", wnet("d", 0));
    expect_int_param(node, "SRST_POLARITY", 0);
    expect_bit_param(node, "SRST_VALUE", ODIN3_BIT_0);
}

static void test_dff_srst_two_controls(void) {
    /* two resets to the same value: SRST = $reduce_or of both selects */
    build(MOD(W_STD W_MID MUX1("m_a", "\\m2", "1'1", "\\r", "\\m1")
                  MUX1("m_b", "\\d", "1'1", "\\e", "\\m2") FF_ON("\\m1")));
    run_dff();
    expect_ff("$sdff");
    odin3_node_id node = first_ff();
    expect_pin(node, "D", wnet("d", 0));
    expect_bit_param(node, "SRST_VALUE", ODIN3_BIT_1);
    expect_int_param(node, "SRST_POLARITY", 1);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$reduce_or"));
    odin3_net_id srst = pin_net_of(node, "SRST", 0);
    odin3_node_id gate = odin3_pin_node(fx.module, odin3_net_driver(fx.module, srst));
    TEST_ASSERT_EQUAL_STRING("$reduce_or", type_name(gate));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_node_port(fx.module, gate, port_of(gate, "A")).count);
    uint32_t sels = 0;
    for (uint32_t k = 0; k < 2; k++) {
        odin3_net_id in = pin_net_of(gate, "A", k);
        sels += in.v == wnet("r", 0).v || in.v == wnet("e", 0).v ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT32(2, sels);
}

static void test_dff_srst_value_conflict(void) {
    /* a reset to 1 under a reset to 0: only the outer mux is a reset */
    build(MOD(W_STD W_MID MUX1("m_a", "\\m2", "1'0", "\\r", "\\m1")
                  MUX1("m_b", "\\d", "1'1", "\\e", "\\m2") FF_ON("\\m1")));
    run_dff();
    expect_ff("$sdff");
    odin3_node_id node = first_ff();
    expect_pin(node, "SRST", wnet("r", 0));
    expect_pin(node, "D", wnet("m2", 0));
    expect_bit_param(node, "SRST_VALUE", ODIN3_BIT_0);
}

static void test_dff_en_merge(void) {
    build(MOD(W_STD W_MID MUX1("mx", "\\q", "\\d", "\\e", "\\m1") FF_ON("\\m1")));
    run_dff();
    expect_ff("$dffe");
    odin3_node_id node = first_ff();
    expect_pin(node, "EN", wnet("e", 0));
    expect_pin(node, "D", wnet("d", 0));
    expect_pin(node, "Q", wnet("q", 0));
    expect_int_param(node, "EN_POLARITY", 1);
    expect_action("adding EN", "$dff", "f");
    /* Q on the B side: the select is the enable, active low */
    build(MOD(W_STD W_MID MUX1("mx", "\\d", "\\q", "\\e", "\\m1") FF_ON("\\m1")));
    run_dff();
    expect_ff("$dffe");
    node = first_ff();
    expect_pin(node, "EN", wnet("e", 0));
    expect_pin(node, "D", wnet("d", 0));
    expect_int_param(node, "EN_POLARITY", 0);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$not"));
}

static void test_dff_srst_then_en(void) {
    build(MOD(W_STD W_MID MUX1("m_en", "\\q", "\\d", "\\e", "\\m1")
                  MUX1("m_rs", "\\m1", "1'0", "\\r", "\\m2") FF_ON("\\m2")));
    run_dff();
    expect_ff("$sdffe");
    odin3_node_id node = first_ff();
    expect_pin(node, "SRST", wnet("r", 0));
    expect_pin(node, "EN", wnet("e", 0));
    expect_pin(node, "D", wnet("d", 0));
    expect_pin(node, "Q", wnet("q", 0));
    expect_bit_param(node, "SRST_VALUE", ODIN3_BIT_0);
    expect_info(2, 0); /* the $dff and the $sdff it made */
}

static void test_dff_en_then_srst(void) {
    build(MOD(W_STD W_MID MUX1("m_rs", "\\d", "1'0", "\\r", "\\m1")
                  MUX1("m_en", "\\q", "\\m1", "\\e", "\\m2") FF_ON("\\m2")));
    run_dff();
    expect_ff("$sdffce");
    odin3_node_id node = first_ff();
    expect_pin(node, "SRST", wnet("r", 0));
    expect_pin(node, "EN", wnet("e", 0));
    expect_pin(node, "D", wnet("d", 0));
    expect_pin(node, "Q", wnet("q", 0));
    expect_bit_param(node, "SRST_VALUE", ODIN3_BIT_0);
    run_dff(); /* a second call changes nothing */
    expect_ff("$sdffce");
    expect_pin(first_ff(), "D", wnet("d", 0));
}

static void test_dff_nodffe_nosdff(void) {
    build(MOD(W_STD W_MID MUX1("mx", "\\q", "\\d", "\\e", "\\m1") FF_ON("\\m1")));
    fx.opts.nodffe = true;
    run_dff();
    expect_ff("$dff");
    expect_pin(first_ff(), "D", wnet("m1", 0));
    build(MOD(W_STD W_MID MUX1("mx", "\\d", "1'0", "\\r", "\\m1") FF_ON("\\m1")));
    fx.opts.nosdff = true;
    run_dff();
    expect_ff("$dff");
    expect_pin(first_ff(), "D", wnet("m1", 0));
    /* -nosdff leaves the EN merge, -nodffe the SRST one */
    build(MOD(W_STD W_MID MUX1("mx", "\\d", "1'0", "\\r", "\\m1") FF_ON("\\m1")));
    fx.opts.nodffe = true;
    run_dff();
    expect_ff("$sdff");
    /* both flags at once: neither merge runs, whatever the order of the two muxes */
    build(MOD(W_STD W_MID MUX1("mx", "\\q", "\\d", "\\e", "\\m1")
                  MUX1("my", "\\m1", "1'0", "\\r", "\\m2") FF_ON("\\m2")));
    fx.opts.nodffe = true;
    fx.opts.nosdff = true;
    run_dff();
    expect_ff("$dff");
    expect_pin(first_ff(), "D", wnet("m2", 0));
    build(MOD(W_STD W_MID MUX1("mx", "\\d", "1'0", "\\r", "\\m1")
                  MUX1("my", "\\q", "\\m1", "\\e", "\\m2") FF_ON("\\m2")));
    fx.opts.nodffe = true;
    fx.opts.nosdff = true;
    run_dff();
    expect_ff("$dff");
    expect_pin(first_ff(), "D", wnet("m2", 0));
}

/* Review I1: every FF made from a (* keep *) FF keeps its attributes, so opt_clean keeps it. */
static void expect_node_attrs(odin3_node_id node) {
    odin3_objref ref = {ODIN3_OBJ_NODE, node.v};
    TEST_ASSERT_NOT_NULL(odin3_attr_get(fx.module, ref, intern("keep")));
    TEST_ASSERT_NOT_NULL(odin3_attr_get(fx.module, ref, intern("foo")));
}

static void expect_attrs_everywhere(uint32_t n_ffs) {
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        bool is_ff = odin3_node_live(fx.module, node) &&
                     odin3_ff_is_ff(&fx.oc, odin3_node_type(fx.module, node));
        if (is_ff) {
            expect_node_attrs(node);
        }
        seen += is_ff ? 1U : 0U;
    }
    TEST_ASSERT_EQUAL_UINT32(n_ffs, seen);
}

static void keep_case(const char *text, uint32_t n_ffs) {
    build(text);
    run_dff();
    TEST_ASSERT_EQUAL_UINT32(n_ffs, count_ffs());
    expect_attrs_everywhere(n_ffs);
    odin3_opt_counts counts;
    memset(&counts, 0, sizeof counts);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_clean_run(&fx.ctx, fx.res.design, &fx.opts, &counts));
    TEST_ASSERT_EQUAL_UINT32(n_ffs, count_ffs()); /* Q is unread: only keep saves them */
    expect_attrs_everywhere(n_ffs);
}

#define KEEP_ATTRS "  attribute \\keep 1\n  attribute \\foo \"bar\"\n"
#define KEEP_WIRES                                                                                 \
    "  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\r\n  wire input 4 \\e\n"           \
    "  wire width 2 input 5 \\dd\n  wire width 2 \\q\n  wire \\m1\n  wire \\m2\n"

static void test_dff_merge_keeps_attrs(void) {
    /* all merged: one FF whose D is an EN mux then an SRST mux -> one $sdffce */
    keep_case(MOD(KEEP_WIRES MUX1("mx", "\\q [0]", "\\d", "\\e", "\\m1")
                      MUX1("my", "\\m1", "1'0", "\\r", "\\m2") KEEP_ATTRS
                  "  cell $dff \\f\n" PW PCLK CCLK
                  "    connect \\D \\m2\n    connect \\Q \\q [0]\n  end\n"),
              1);
    /* partly merged: bit 0 leaves the 2-bit FF, bit 1 stays: both FFs carry the attributes */
    keep_case(MOD(KEEP_WIRES MUX1("mx", "\\q [0]", "\\d", "\\e", "\\m1") KEEP_ATTRS
                  "  cell $dff \\f\n    parameter \\WIDTH 2\n" PCLK CCLK
                  "    connect \\D { \\dd [1] \\m1 }\n    connect \\Q \\q\n  end\n"),
              2);
    /* SRST merge of a partly merged FF */
    keep_case(MOD(KEEP_WIRES MUX1("mx", "\\d", "1'0", "\\r", "\\m1") KEEP_ATTRS
                  "  cell $dff \\f\n    parameter \\WIDTH 2\n" PCLK CCLK
                  "    connect \\D { \\dd [1] \\m1 }\n    connect \\Q \\q\n  end\n"),
              2);
}

static void test_dff_single_user(void) {
    /* \m1 is also an output port: the mux is not absorbed */
    build(MOD(W_STD "  wire output 7 \\m1\n  wire \\m2\n" MUX1("mx", "\\q", "\\d", "\\e", "\\m1")
                  FF_ON("\\m1")));
    run_dff();
    expect_ff("$dff");
    expect_pin(first_ff(), "D", wnet("m1", 0));
    build(MOD(W_STD "  wire output 7 \\m1\n  wire \\m2\n" MUX1("mx", "\\d", "1'0", "\\r", "\\m1")
                  FF_ON("\\m1")));
    run_dff();
    expect_ff("$dff");
    expect_info(0, 0);
}

static void test_dff_no_aload_srst(void) {
    /* Yosys 0.55 drops the reset when it merges one into an $aldff (spec §17 #33): we do not */
    build(MOD(W_STD W_MID MUX1("mx", "\\d", "1'0", "\\r", "\\m1")
                  CELL("$aldff", "f", PW PCLK PALOAD,
                       CCLK "    connect \\ALOAD \\e\n    connect \\AD \\a\n"
                            "    connect \\D \\m1\n" CQ)));
    run_dff();
    expect_ff("$aldff");
    expect_pin(first_ff(), "D", wnet("m1", 0));
}

/* The cone of an EN evaluated without recursion: nets with their values (-1 unknown), as many
 * passes as it takes. The selects \a, \r and \e read bits 0, 1 and 2 of the mask. */
enum { CONE_MAX = 64 };

typedef struct cone {
    uint32_t net[CONE_MAX];
    int val[CONE_MAX];
    uint32_t count;
} cone;

static uint32_t cone_slot(cone *cn, odin3_net_id net) {
    for (uint32_t i = 0; i < cn->count; i++) {
        if (cn->net[i] == net.v) {
            return i;
        }
    }
    TEST_ASSERT_TRUE(cn->count < CONE_MAX);
    cn->net[cn->count] = net.v;
    cn->val[cn->count] = -1;
    return cn->count++;
}

/* The value of a net that needs no cell: a constant or a select; -1 otherwise. */
static int leaf_value(odin3_net_id net, unsigned mask) {
    static const char *const k_in[] = {"a", "r", "e"};
    odin3_const val = odin3_net_const_value(fx.module, net);
    if (val == ODIN3_CONST_0 || val == ODIN3_CONST_1) {
        return val == ODIN3_CONST_1 ? 1 : 0;
    }
    for (unsigned k = 0; k < 3U; k++) {
        if (net.v == wnet(k_in[k], 0).v) {
            return (int)(mask >> k & 1U);
        }
    }
    return -1;
}

/* A cell's inputs as slots; the values of its A and B ports bit by bit; false if one is unknown. */
static bool cell_inputs(cone *cn, odin3_node_id node, const char *port, int *vals, uint32_t *n) {
    odin3_pinslice pins = odin3_node_port(fx.module, node, port_of(node, port));
    *n = pins.count;
    bool known = true;
    for (uint32_t k = 0; k < pins.count; k++) {
        vals[k] = cn->val[cone_slot(cn, pin_net_of(node, port, k))];
        known = known && vals[k] >= 0;
    }
    return known;
}

/* $ne (A against B bit by bit) and $reduce_and of the known input bits. */
static int cone_reduce(const char *name, const int *lhs, const int *rhs, uint32_t n_in) {
    bool is_ne = strcmp(name, "$ne") == 0;
    if (!is_ne) {
        TEST_ASSERT_EQUAL_STRING("$reduce_and", name);
    }
    int differ = 0;
    int all = 1;
    for (uint32_t k = 0; k < n_in; k++) {
        all = all && lhs[k];
        differ = differ || (is_ne && lhs[k] != rhs[k]);
    }
    return is_ne ? differ : all;
}

/* One try at the value of net; -1 while an input is unknown. */
static int cone_try(cone *cn, odin3_net_id net, unsigned mask) {
    int leaf = leaf_value(net, mask);
    if (leaf >= 0) {
        return leaf;
    }
    odin3_pin_id drv = odin3_net_driver(fx.module, net);
    TEST_ASSERT_TRUE(drv.v != 0);
    odin3_node_id node = odin3_pin_node(fx.module, drv);
    const char *name = type_name(node);
    int lhs[CONE_MAX];
    int rhs[CONE_MAX];
    uint32_t n_in = 0;
    uint32_t n_rhs = 0;
    bool known = cell_inputs(cn, node, "A", lhs, &n_in);
    if (strcmp(name, "$ne") == 0 || strcmp(name, "$and") == 0) {
        known = cell_inputs(cn, node, "B", rhs, &n_rhs) && known;
    }
    if (!known) {
        return -1;
    }
    if (strcmp(name, "$not") == 0) {
        return !lhs[0];
    }
    if (strcmp(name, "$and") == 0) {
        return lhs[0] && rhs[0];
    }
    return cone_reduce(name, lhs, rhs, n_in);
}

static bool eval_cone(odin3_net_id net, unsigned mask) {
    cone cn;
    memset(&cn, 0, sizeof cn);
    uint32_t target = cone_slot(&cn, net);
    for (uint32_t pass = 0; pass < CONE_MAX && cn.val[target] < 0; pass++) {
        for (uint32_t i = 0; i < cn.count; i++) {
            cn.val[i] = cn.val[i] < 0 ? cone_try(&cn, (odin3_net_id){cn.net[i]}, mask) : cn.val[i];
        }
    }
    TEST_ASSERT_TRUE(cn.val[target] >= 0);
    return cn.val[target] != 0;
}

#define FEEDBACK_TEXT                                                                              \
    MOD(W_STD W_MID MUX1("mi", "\\d", "\\q", "\\r", "\\m1")                                        \
            MUX1("mo", "\\m1", "\\e", "\\a", "\\m2") FF_ON("\\m2"))

static void test_dff_feedback_patterns(void) {
    build(FEEDBACK_TEXT);
    run_dff();
    expect_ff("$dffe");
    odin3_node_id node = first_ff();
    expect_pin(node, "D", wnet("m2", 0));
    expect_pin(node, "Q", wnet("q", 0));
    expect_int_param(node, "EN_POLARITY", 1);
    odin3_net_id en = pin_net_of(node, "EN", 0);
    for (unsigned mask = 0; mask < 4U; mask++) {
        bool val_a = (mask & 1U) != 0;
        bool val_r = (mask & 2U) != 0;
        /* the FF holds exactly when a = 0 (the outer mux takes m1) and r = 1 (m1 takes Q) */
        TEST_ASSERT_EQUAL_INT_MESSAGE(!(!val_a && val_r), eval_cone(en, mask) ? 1 : 0,
                                      "enable cone");
    }
    /* the inner mux's B input, which read Q, reads x */
    odin3_node_id inner = node_named("mi");
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X,
                          odin3_net_const_value(fx.module, pin_net_of(inner, "B", 0)));
    expect_pin(inner, "A", wnet("d", 0));
    expect_pin(inner, "S", wnet("r", 0));
    expect_pin(node_named("mo"), "A", wnet("m1", 0));
    expect_pin(node_named("mo"), "B", wnet("e", 0));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$ne"));
    expect_action("adding EN", "$dff", "f");
}

/* I1 (spec §17 #38): q <= r ? mux(S=1'x, A=1, B=q) : d. The x select would be read twice (in the
 * pattern and in the mux), each time as its own value; the search gives up instead. */
#define XSEL_TEXT                                                                                  \
    MOD(W_STD W_MID MUX1("m7", "1'1", "\\q", "1'x", "\\m1")                                        \
            MUX1("m0", "\\m1", "\\d", "\\r", "\\m2") FF_ON("\\m2"))

static void test_dff_feedback_x_select(void) {
    build(XSEL_TEXT);
    run_dff();
    expect_ff("$dff");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$ne"));
    odin3_node_id inner = node_named("m7");
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(inner, "B", 0).v); /* no x mark */
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X,
                          odin3_net_const_value(fx.module, pin_net_of(inner, "S", 0)));
}

static void test_dff_feedback_two_paths(void) {
    /* Q under two selects in different arms: both paths are patterns, ANDed by $reduce_and */
    build(MOD(W_STD W_MID "  wire \\m3\n" MUX1("m_a", "\\d", "\\q", "\\r", "\\m1")
                  MUX1("m_b", "\\q", "\\d", "\\e", "\\m3") MUX1("mo", "\\m1", "\\m3", "\\a", "\\m2")
                      FF_ON("\\m2")));
    run_dff();
    expect_ff("$dffe");
    odin3_node_id node = first_ff();
    odin3_net_id en = pin_net_of(node, "EN", 0);
    for (unsigned mask = 0; mask < 8U; mask++) {
        bool val_a = (mask & 1U) != 0;
        bool val_r = (mask & 2U) != 0;
        bool val_e = (mask & 4U) != 0;
        /* holds when a = 1 and e = 0 (m_b takes Q) or a = 0 and r = 1 (m_a takes Q) */
        bool holds = val_a ? !val_e : val_r;
        TEST_ASSERT_EQUAL_INT(holds ? 0 : 1, eval_cone(en, mask) ? 1 : 0);
    }
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$ne"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$reduce_and"));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X,
                          odin3_net_const_value(fx.module, pin_net_of(node_named("m_a"), "B", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X,
                          odin3_net_const_value(fx.module, pin_net_of(node_named("m_b"), "A", 0)));
}

#define SLICE_TEXT                                                                                 \
    MOD("  wire input 1 \\clk\n  wire width 2 input 2 \\d\n  wire input 3 \\e\n"                   \
        "  wire width 2 output 4 \\q\n  wire \\m\n"                                                \
        "  attribute \\src \"m.v:2.1\"\n  cell $mux \\mx\n" PW                                     \
        "    connect \\A \\q [0]\n    connect \\B \\d [0]\n"                                       \
        "    connect \\S \\e\n    connect \\Y \\m\n  end\n"                                        \
        "  attribute \\src \"f.v:1.1\"\n  cell $dff \\f\n    parameter \\WIDTH 2\n" PCLK CCLK      \
        "    connect \\D { \\d [1] \\m }\n"                                                        \
        "    connect \\Q \\q\n  end\n")

static void test_dff_slices(void) {
    build(SLICE_TEXT);
    odin3_prov_id ff_prov = odin3_node_prov(fx.module, node_named("f"));
    odin3_prov_id mux_prov = odin3_node_prov(fx.module, node_named("mx"));
    TEST_ASSERT_TRUE(ff_prov.v != 0 && mux_prov.v != 0 && ff_prov.v != mux_prov.v);
    run_dff();
    TEST_ASSERT_EQUAL_UINT32(2, count_ffs());
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$dffe"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$dff"));
    odin3_node_id plain = node_named("f"); /* the bit that stays keeps the old name */
    TEST_ASSERT_EQUAL_STRING("$dff", type_name(plain));
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 1).v, pin_net_of(plain, "Q", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("d", 1).v, pin_net_of(plain, "D", 0).v);
    expect_derived_from(plain, ff_prov);
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (strcmp(type_name(node), "$dffe") != 0) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(node, "Q", 0).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("d", 0).v, pin_net_of(node, "D", 0).v);
        expect_pin(node, "EN", wnet("e", 0));
        const odin3_prov_record *rec =
            odin3_prov_get(fx.res.design, odin3_node_prov(fx.module, node));
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
        TEST_ASSERT_EQUAL_UINT32(2, rec->parents.count); /* [FF, absorbed mux] */
        TEST_ASSERT_EQUAL_UINT32(ff_prov.v, rec->parents.ids[0].v);
        TEST_ASSERT_EQUAL_UINT32(mux_prov.v, rec->parents.ids[1].v);
    }
}

static void test_dff_groups_by_control(void) {
    /* bits 0 and 1 share the enable, bit 2 has another: two $dffe, in bit order */
    build(MOD("  wire input 1 \\clk\n  wire width 3 input 2 \\d\n  wire input 3 \\e\n"
              "  wire input 4 \\r\n  wire width 3 output 5 \\q\n  wire width 3 \\m\n"
              "  cell $mux \\mx\n    parameter \\WIDTH 2\n    connect \\A \\q [1:0]\n"
              "    connect \\B \\d [1:0]\n    connect \\S \\e\n    connect \\Y \\m [1:0]\n  end\n"
              "  cell $mux \\my\n" PW "    connect \\A \\q [2]\n    connect \\B \\d [2]\n"
              "    connect \\S \\r\n    connect \\Y \\m [2]\n  end\n"
              "  cell $dff \\f\n    parameter \\WIDTH 3\n" PCLK CCLK
              "    connect \\D \\m\n    connect \\Q \\q\n  end\n"));
    run_dff();
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$dffe"));
    TEST_ASSERT_EQUAL_UINT32(2, count_ffs());
    uint32_t wide = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (strcmp(type_name(node), "$dffe") != 0) {
            continue;
        }
        uint32_t width = odin3_node_port(fx.module, node, port_of(node, "Q")).count;
        wide += width == 2 ? 1U : 0U;
        odin3_net_id en = pin_net_of(node, "EN", 0);
        TEST_ASSERT_EQUAL_UINT32(width == 2 ? wnet("e", 0).v : wnet("r", 0).v, en.v);
    }
    TEST_ASSERT_EQUAL_UINT32(1, wide);
}

static void test_dff_merge_keeps_init(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\e\n  wire \\m\n"
              "  attribute \\init 1'1\n  wire output 4 \\q\n" MUX1("mx", "\\q", "\\d", "\\e", "\\m")
                  FF_ON("\\m")));
    run_dff();
    expect_ff("$dffe");
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q", 0)));
    check_full();
}

static void test_dff_fine_no_merge(void) {
    /* no $_SDFF_ / $_DFFE_ in the registry (spec §17 #32): a fine FF keeps its mux */
    build(MOD(W_STD W_MID MUX1("mx", "\\q", "\\d", "\\e", "\\m1")
                  CELL("$_DFF_P_", "f", "", "    connect \\C \\clk\n    connect \\D \\m1\n" CQ)));
    run_dff();
    expect_ff("$_DFF_P_");
    expect_pin(first_ff(), "D", wnet("m1", 0));
}

/* A chain of n muxes, each on its own select \s[i], Q at the bottom: D = y0, y_i = s_i ? d :
 * y_{i+1}, y_n = Q. The FF holds exactly when every select is 0: one pattern of n selects. */
static void build_chain(uint32_t n) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(&text, "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                                    "  wire output 3 \\q\n  wire width 100 input 4 \\s\n"));
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&text, "  wire \\y%u\n", (unsigned)i));
    }
    for (uint32_t i = 0; i < n; i++) {
        char lower[32];
        (void)snprintf(lower, sizeof lower, i + 1U == n ? "\\q" : "\\y%u", (unsigned)(i + 1U));
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(
                          &text,
                          "  cell $mux \\x%u\n" PW "    connect \\A %s\n    connect \\B \\d\n"
                          "    connect \\S \\s [%u]\n    connect \\Y \\y%u\n  end\n",
                          (unsigned)i, lower, (unsigned)i, (unsigned)i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(&text, "  cell $dff \\f\n" PW PCLK CCLK
                                                      "    connect \\D \\y0\n" CQ "  end\nend\n"));
    build(text.data);
    odin3_strbuf_free(&text);
}

static void test_dff_pattern_depth(void) {
    build_chain(40);
    run_dff();
    expect_ff("$dffe");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$ne"));
    odin3_node_id ne =
        odin3_pin_node(fx.module, odin3_net_driver(fx.module, pin_net_of(first_ff(), "EN", 0)));
    TEST_ASSERT_EQUAL_UINT32(40, odin3_node_port(fx.module, ne, port_of(ne, "A")).count);
    /* 70 selects deep: past the cap (spec §17 #34), no pattern and no x written */
    build_chain(70);
    run_dff();
    expect_ff("$dff");
    odin3_node_id bottom = node_named("x69");
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(bottom, "A", 0).v);
}

/* --- rules 10 and 11 out of memory ---------------------------------------------------------- */

#define MERGE_OOM_TEXT                                                                             \
    MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\r\n  wire input 4 \\e\n"       \
        "  wire input 5 \\a\n  wire width 2 input 6 \\dd\n"                                        \
        "  attribute \\init 1'1\n  wire output 7 \\q1\n  attribute \\init 1'0\n"                   \
        "  wire output 8 \\q2\n  attribute \\init 2'01\n  wire width 2 output 9 \\q3\n"            \
        "  attribute \\init 1'1\n  wire output 10 \\q4\n"                                          \
        "  wire \\m1\n  wire \\m2\n  wire \\m3\n  wire \\m4\n  wire \\m5\n"                        \
        "  cell $mux \\x1\n" PW "    connect \\A \\d\n    connect \\B 1'0\n    connect \\S \\r\n"  \
        "    connect \\Y \\m1\n  end\n"                                                            \
        "  cell $dff \\f1\n" PW PCLK CCLK "    connect \\D \\m1\n    connect \\Q \\q1\n  end\n"    \
        "  cell $mux \\x2\n" PW "    connect \\A \\q2\n    connect \\B \\d\n    connect \\S \\e\n" \
        "    connect \\Y \\m2\n  end\n"                                                            \
        "  cell $dff \\f2\n" PW PCLK CCLK "    connect \\D \\m2\n    connect \\Q \\q2\n  end\n"    \
        "  cell $mux \\x3\n" PW "    connect \\A \\q3 [0]\n    connect \\B \\d\n"                  \
        "    connect \\S \\e\n    connect \\Y \\m3\n  end\n"                                       \
        "  attribute \\keep 1\n  attribute \\foo \"bar\"\n" /* the group FFs copy these (I1) */    \
        "  cell $dff \\f3\n    parameter \\WIDTH 2\n" PCLK CCLK                                    \
        "    connect \\D { \\dd [1] \\m3 }\n    connect \\Q \\q3\n  end\n"                         \
        "  cell $mux \\x4\n" PW "    connect \\A \\d\n    connect \\B \\q4\n    connect \\S \\r\n" \
        "    connect \\Y \\m4\n  end\n"                                                            \
        "  cell $mux \\x5\n" PW "    connect \\A \\m4\n    connect \\B \\e\n    connect \\S \\a\n" \
        "    connect \\Y \\m5\n  end\n"                                                            \
        "  cell $dff \\f4\n" PW PCLK CCLK "    connect \\D \\m5\n    connect \\Q \\q4\n  end\n")

static void expect_merge_inits(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q1", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, odin3_ffinit_get(fx.module, wnet("q2", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q3", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_0, odin3_ffinit_get(fx.module, wnet("q3", 1)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q4", 0)));
}

static bool merge_oom_step(long fail, uint32_t *discard) {
    close_fixture();
    memset(&fx.counts, 0, sizeof fx.counts);
    build(MERGE_OOM_TEXT);
    n_discard = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_dff_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, last_error);
    check_full(); /* no second driver, no dangling pin, whatever the failure point */
    expect_merge_inits();
    *discard += n_discard;
    return st == ODIN3_OK;
}

static void test_dff_merge_oom(void) {
    uint32_t discard = 0;
    long failures = 0;
    bool done = false;
    for (long fail = 0; !done && fail < MERGE_OOM_LIMIT; fail++) {
        done = merge_oom_step(fail, &discard);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(failures > 100); /* not vacuous */
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$sdff"));
    TEST_ASSERT_EQUAL_UINT32(3, count_of("$dffe"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$dff"));
    printf("opt_dff merge OOM: %ld failing allocation points, %u half-done\n", failures, discard);
}

/* --- Yosys 0.55's own cases ----------------------------------------------------------------------
 */

enum { CASE_BUF = 65536, SUMMARY_MAX = 160, SUMMARY_LEN = 256, VALUE_LEN = 96, PARAM_LEN = 160 };

static const char *const k_cases[] = {
    "arst_always",     "arst_never",   "clk",        "const",           "qd",
    "sr_always",       "sr_never_clr", "sr_clr_na",  "sr_never_set",    "sr_set_na",
    "sr_arst_merge",   "srst_always",  "srst_never", "aload_always_sr", "aload_always_arst",
    "dq_ce_over_srst", "mux",
};

/* The mux case also runs with the merges switched off (the flags line of each .expect). */
static const char *const k_mux_flags[] = {".nodffe", ".nosdff", ".nodffe_nosdff"};

static void read_case(const char *name, const char *ext, char *buf) {
    char path[LOG_TEXT];
    TEST_ASSERT_TRUE(snprintf(path, sizeof path, "%s/%s%s", ODIN3_DFF_DATA, name, ext) <
                     (int)sizeof path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(buf, 1, CASE_BUF - 1U, file);
    (void)fclose(file);
    TEST_ASSERT_TRUE_MESSAGE(got > 0 && got < CASE_BUF - 1U, path);
    buf[got] = '\0';
}

typedef struct summary {
    char item[SUMMARY_MAX][SUMMARY_LEN];
    uint32_t n;
} summary;

static void add_item(summary *sum, const char *text) {
    TEST_ASSERT_TRUE(sum->n < SUMMARY_MAX);
    TEST_ASSERT_TRUE(strlen(text) < SUMMARY_LEN);
    (void)snprintf(sum->item[sum->n++], SUMMARY_LEN, "%s", text);
}

static int cmp_str(const void *lhs, const void *rhs) {
    return strcmp((const char *)lhs, (const char *)rhs);
}

static void value_text(const odin3_value *val, char *out, size_t size) {
    if (val->kind == ODIN3_VAL_INT) {
        (void)snprintf(out, size, "%lld", (long long)val->i);
        return;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, val->kind);
    size_t at = (size_t)snprintf(out, size, "%u'", (unsigned)val->len);
    for (uint32_t k = val->len; k > 0 && at + 1U < size; k--) {
        out[at++] = "01xz"[val->bits[k - 1U] & 3U];
    }
    out[at] = '\0';
}

/* "cell TYPE NAME=value ..." with the parameters sorted by name. */
static void add_cell(summary *sum, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
    char names[16][PARAM_LEN];
    uint32_t n_names = 0;
    TEST_ASSERT_TRUE(def->n_params <= 16);
    for (uint32_t i = 0; i < def->n_params; i++) {
        char value[VALUE_LEN];
        value_text(odin3_node_param(fx.module, node, i), value, sizeof value);
        (void)snprintf(names[n_names++], PARAM_LEN, "%s=%s", def->params[i].name, value);
    }
    qsort(names, n_names, PARAM_LEN, cmp_str);
    char line[SUMMARY_LEN];
    size_t at = (size_t)snprintf(line, sizeof line, "cell %s", def->name);
    for (uint32_t i = 0; i < n_names; i++) {
        at += (size_t)snprintf(line + at, sizeof line - at, " %s", names[i]);
    }
    add_item(sum, line);
}

static void add_wire(summary *sum, odin3_wire_id wire, const char *name) {
    for (uint32_t bit = 0; bit < odin3_wire_width(fx.module, wire); bit++) {
        static const char k_val[] = {'-', '0', '1', 'x', 'z'};
        odin3_const val = odin3_net_const_value(fx.module, odin3_wire_net(fx.module, wire, bit));
        char line[SUMMARY_LEN];
        if (val != ODIN3_CONST_NONE) {
            (void)snprintf(line, sizeof line, "const %s %u %c", name, (unsigned)bit, k_val[val]);
            add_item(sum, line);
        }
    }
    const odin3_value *init =
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, intern("init"));
    if (init != NULL) {
        char line[SUMMARY_LEN];
        size_t at = (size_t)snprintf(line, sizeof line, "init %s ", name);
        uint32_t width = odin3_wire_width(fx.module, wire); /* an init past the wire is no init */
        for (uint32_t k = width; k > 0 && at + 1U < sizeof line; k--) {
            char digit = 'x';
            if (k - 1U < init->len) {
                digit = "01xz"[init->bits[k - 1U] & 3U];
            }
            line[at++] = digit;
        }
        line[at] = '\0';
        add_item(sum, line);
    }
}

static void summarize(summary *sum) {
    memset(sum, 0, sizeof *sum);
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        const odin3_celltype_def *def =
            odin3_celltype_get(fx.res.design, odin3_node_type(fx.module, node));
        if (odin3_node_live(fx.module, node) && def->gran != ODIN3_GRAN_PORT &&
            strncmp(def->name, "$_CONST", 7) != 0) {
            add_cell(sum, node);
        }
    }
    for (uint32_t id = 1; id < odin3_module_wire_end(fx.module); id++) {
        odin3_wire_id wire = {id};
        if (odin3_wire_live(fx.module, wire)) {
            add_wire(sum, wire,
                     odin3_strtab_get(odin3_design_strtab(fx.res.design),
                                      odin3_wire_name(fx.module, wire)));
        }
    }
    qsort(sum->item, sum->n, SUMMARY_LEN, cmp_str);
}

/* The .expect file: "#" lines (the "# flags:" one sets the options), then one item per line. */
static void parse_expect(char *text, summary *want, odin3_opt_opts *opts) {
    memset(want, 0, sizeof *want);
    memset(opts, 0, sizeof *opts);
    opts->keepdc = strstr(text, "# flags: -keepdc") != NULL;
    opts->nodffe = strstr(text, "# flags: -nodffe") != NULL;
    opts->nosdff = strstr(text, "-nosdff\n") != NULL;
    for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        if (line[0] != '#') {
            add_item(want, line);
        }
    }
    qsort(want->item, want->n, SUMMARY_LEN, cmp_str);
}

static void run_case(const char *name, const char *suffix) {
    static char text[CASE_BUF];
    static char expect[CASE_BUF];
    char label[LOG_TEXT];
    TEST_ASSERT_TRUE(snprintf(label, sizeof label, "%s%s", name, suffix) < (int)sizeof label);
    char ext[32];
    TEST_ASSERT_TRUE(snprintf(ext, sizeof ext, "%s.expect", suffix) < (int)sizeof ext);
    read_case(name, ".il", text);
    read_case(name, ext, expect);
    summary want;
    odin3_opt_opts opts;
    parse_expect(expect, &want, &opts);
    build(text);
    fx.opts = opts;
    run_dff();
    summary got;
    summarize(&got);
    for (uint32_t i = 0; i < want.n && i < got.n; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.item[i], got.item[i], label);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.n, got.n, label);
}

static void test_dff_yosys_cases(void) {
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        run_case(k_cases[i], "");
        tearDown();
        setUp();
        run_case(k_cases[i], ".keepdc");
        tearDown();
        setUp();
    }
    for (size_t i = 0; i < sizeof k_mux_flags / sizeof k_mux_flags[0]; i++) {
        run_case("mux", k_mux_flags[i]);
        tearDown();
        setUp();
    }
}

/* Point (l) for the rules: a Q tied in this call is not seen by the next FF's rules. \q1 is tied
 * to its reset value by rule 4; FF f2's enable reads \q1, and only the next call sees it as 0. */
static void test_dff_tie_invisible_to_rules(void) {
    build(
        MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q1\n  wire output 4 \\q2\n"
            "  cell $adff \\f1\n" PW PCLK "    parameter \\ARST_POLARITY 1\n"
            "    parameter \\ARST_VALUE 1'0\n" CCLK "    connect \\ARST 1'1\n"
            "    connect \\D \\d\n    connect \\Q \\q1\n  end\n"
            "  cell $dffe \\f2\n" PW PCLK PEN CCLK "    connect \\EN \\q1\n    connect \\D \\d\n"
            "    connect \\Q \\q2\n  end\n"));
    run_dff();
    expect_ff("$dffe"); /* f2 is untouched in this call */
    expect_const("q1", 0, ODIN3_CONST_0);
    expect_info(1, 0);
    run_dff();
    expect_gone(); /* EN is the constant 0 now: the D path goes, Q is x */
}

/* A 2-bit $dff whose bit 1 is the constant 0: the FF shrinks to bit 0 (name and attributes
 * kept), bit 1 of Q is tied to 0 and its init is gone. */
static void test_dff_constbits_partial(void) {
    build(MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  attribute \\init 2'01\n"
              "  wire width 2 output 3 \\q\n"
              "  attribute \\keep 1\n  cell $dff \\f\n    parameter \\WIDTH 2\n" PCLK CCLK
              "    connect \\D { 1'0 \\d }\n    connect \\Q \\q\n  end\n"));
    run_dff();
    expect_ff("$dff");
    odin3_node_id node = first_ff();
    TEST_ASSERT_EQUAL_UINT32(node.v, node_named("f").v);
    TEST_ASSERT_EQUAL_INT64(1, param_of(node, "WIDTH")->i);
    TEST_ASSERT_NOT_NULL(
        odin3_attr_get(fx.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, intern("keep")));
    expect_const("q", 1, ODIN3_CONST_0);
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, pin_net_of(node, "Q", 0).v);
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_1, odin3_ffinit_get(fx.module, wnet("q", 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_BIT_X, odin3_ffinit_get(fx.module, wnet("q", 1)));
    expect_action("setting constant 0 at bit 1", "$dff", "f");
    expect_info(0, 1);
}

/* A z constant ties Q to z (Yosys sets the State it computed). */
static void test_dff_constbits_z(void) {
    build(MOD("  wire input 1 \\clk\n  wire output 6 \\q\n  cell $dff \\f\n" PW PCLK CCLK
              "    connect \\D 1'z\n" CQ "  end\n"));
    run_dff();
    expect_gone();
    expect_const("q", 0, ODIN3_CONST_Z);
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.dff_action[ODIN3_OPT_DFF_CONST_BIT_Z]);
}

static void test_dff_args(void) {
    build(k_const_d);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_dff_run(NULL, fx.res.design, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_dff_run(&fx.ctx, NULL, &fx.opts, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_dff_run(&fx.ctx, fx.res.design, NULL, &fx.counts));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_dff_run(&fx.ctx, fx.res.design, &fx.opts, NULL));
    TEST_ASSERT_EQUAL_STRING("opt_dff: no pass run, design, options or counts", last_error);
    expect_ff("$dff");
}

/* Two runs on one input give byte-identical write_rtlil text (OP-14). */
static void test_dff_deterministic(void) {
    static char text[CASE_BUF];
    odin3_strbuf first;
    odin3_strbuf second;
    odin3_strbuf_init(&first);
    odin3_strbuf_init(&second);
    read_case("qd", ".il", text);
    build(text);
    run_dff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &first));
    build(text);
    run_dff();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&fx.res, &second));
    TEST_ASSERT_EQUAL_STRING(first.data, second.data);
    odin3_strbuf_free(&first);
    odin3_strbuf_free(&second);
}

/* --- out of memory ------------------------------------------------------------------------------
 */

#define OOM_TEXT                                                                                   \
    MOD("  wire input 1 \\clk\n  wire input 2 \\d\n  wire input 3 \\r\n  wire input 4 \\e\n"       \
        "  wire width 2 input 5 \\k\n  wire width 2 input 6 \\s\n  wire width 2 input 7 \\dd\n"    \
        "  attribute \\init 2'01\n  wire width 2 output 8 \\q1\n"                                  \
        "  attribute \\init 1'1\n  wire output 9 \\q2\n  wire output 10 \\q3\n"                    \
        "  attribute \\init 1'1\n  wire output 11 \\q4\n"                                          \
        "  attribute \\keep 1\n  cell $dffsr \\f1\n    parameter \\WIDTH 2\n" PCLK                 \
        "    parameter \\SET_POLARITY 1\n    parameter \\CLR_POLARITY 1\n" CCLK                    \
        "    connect \\SET \\s\n    connect \\CLR { 1'1 \\k [0] }\n    connect \\D \\dd\n"         \
        "    connect \\Q \\q1\n  end\n"                                                            \
        "  cell $dffe \\f2\n" PW PCLK PEN CCLK "    connect \\EN 1'0\n    connect \\D \\d\n"       \
        "    connect \\Q \\q2\n  end\n"                                                            \
        "  cell $dff \\f3\n" PW PCLK CCLK "    connect \\D 1'0\n    connect \\Q \\q3\n  end\n"     \
        "  cell $sdff \\f4\n" PW PCLK PSRST CCLK "    connect \\SRST \\r\n    connect \\D \\q4\n"  \
        "    connect \\Q \\q4\n  end\n")

/* The Q nets of the live FFs keep the init they had (OP-2): rules never write one. */
static void expect_inits_kept(void) {
    static const struct {
        const char *wire;
        uint32_t bit;
        odin3_bit want;
    } k_init[] = {{"q1", 0, ODIN3_BIT_1}, {"q2", 0, ODIN3_BIT_1}, {"q4", 0, ODIN3_BIT_1}};
    for (uint32_t id = 1; id < odin3_module_node_end(fx.module); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(fx.module, node) ||
            !odin3_ff_is_ff(&fx.oc, odin3_node_type(fx.module, node))) {
            continue;
        }
        for (size_t i = 0; i < sizeof k_init / sizeof k_init[0]; i++) {
            odin3_net_id net = wnet(k_init[i].wire, k_init[i].bit);
            odin3_pinslice pins = odin3_node_port(fx.module, node, port_of(node, "Q"));
            for (uint32_t k = 0; k < pins.count; k++) {
                if (odin3_pin_net(fx.module, (odin3_pin_id){pins.first.v + k}).v == net.v) {
                    TEST_ASSERT_EQUAL_INT_MESSAGE(k_init[i].want, odin3_ffinit_get(fx.module, net),
                                                  "init");
                }
            }
        }
    }
}

static bool oom_step(long fail, uint32_t *discard) {
    close_fixture();
    memset(&fx.counts, 0, sizeof fx.counts);
    build(OOM_TEXT);
    n_discard = 0;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_opt_dff_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, last_error);
    check_full();
    expect_inits_kept();
    *discard += n_discard;
    return st == ODIN3_OK;
}

/* The finished run of OOM_TEXT: f1 shrunk to one bit, f2 and f3 tied, f4 an enable on the reset. */
static void expect_oom_result(void) {
    TEST_ASSERT_EQUAL_UINT32(2, count_ffs());
    expect_const("q2", 0, ODIN3_CONST_1);
    expect_const("q3", 0, ODIN3_CONST_0);
    expect_const("q1", 1, ODIN3_CONST_0);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$dffsr"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$dffe"));
}

/* Sweeps the allocation failure point over OOM_TEXT; returns the failing points. */
static long oom_sweep(uint32_t *discard) {
    long failures = 0;
    bool done = false;
    for (long fail = 0; !done && fail < OOM_LIMIT; fail++) {
        done = oom_step(fail, discard);
        failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    return failures;
}

static void test_dff_oom(void) {
    uint32_t discard = 0;
    long failures = oom_sweep(&discard);
    TEST_ASSERT_TRUE(failures > 20); /* not vacuous: many allocation points were failed */
    TEST_ASSERT_TRUE(discard <= 3);  /* only a tie's merge cannot be reserved: one per tied bit */
    expect_oom_result();
    printf("opt_dff OOM: %ld failing allocation points, %u half-done\n", failures, discard);
}

/* Persistent out-of-memory (I2): from allocation `fail` on, `len` allocations fail (all later ones
 * for LONG_MAX, as real OOM does). The IR is valid afterwards, or the run logged the discard line.
 */
typedef struct oom_sweep_cfg {
    const char *text;
    long limit, len;
} oom_sweep_cfg;

typedef struct oom_tally {
    long failures;
    uint32_t invalid; /* the points that left an invalid IR */
    uint32_t silent;  /* of a burst, those without the discard line */
} oom_tally;

/* One run; true when it finished (no failure point reached). */
static bool persistent_step(const oom_sweep_cfg *cfg, long fail, oom_tally *tally) {
    close_fixture();
    memset(&fx.counts, 0, sizeof fx.counts);
    build(cfg->text);
    n_discard = 0;
    odin3_util_set_alloc_fail_window((odin3_alloc_window){fail, cfg->len});
    odin3_status st = odin3_opt_dff_run(&fx.ctx, fx.res.design, &fx.opts, &fx.counts);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, last_error);
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    bool valid = odin3_check_design(fx.res.design, opts) == ODIN3_OK;
    tally->invalid += valid ? 0U : 1U;
    if (!valid && n_discard == 0 && cfg->len != LONG_MAX) {
        tally->silent++;
    } else if (!valid && n_discard == 0) {
        char msg[LOG_TEXT];
        (void)snprintf(msg, sizeof msg, "invalid IR without the discard line (fail %ld) %.300s",
                       fail, last_error);
        TEST_FAIL_MESSAGE(msg);
    }
    return st == ODIN3_OK;
}

/* Sweeps every PERSISTENT_STRIDE-th failure point. */
static oom_tally persistent_sweep(const oom_sweep_cfg *cfg) {
    oom_tally tally = {0, 0, 0};
    bool done = false;
    for (long fail = 0; !done && fail < cfg->limit; fail += PERSISTENT_STRIDE) {
        done = persistent_step(cfg, fail, &tally);
        tally.failures += done ? 0 : 1;
    }
    TEST_ASSERT_TRUE(done);
    return tally;
}

static void test_dff_persistent_oom(void) {
    oom_sweep_cfg cfg_a = {OOM_TEXT, OOM_LIMIT, LONG_MAX};
    oom_sweep_cfg cfg_b = {MERGE_OOM_TEXT, MERGE_OOM_LIMIT, LONG_MAX};
    oom_sweep_cfg cfg_w = {MERGE_OOM_TEXT, MERGE_OOM_LIMIT, 2};
    oom_tally tal_a = persistent_sweep(&cfg_a);
    oom_tally tal_b = persistent_sweep(&cfg_b);
    TEST_ASSERT_TRUE(tal_a.failures > 20 && tal_b.failures > 100); /* not vacuous */
    /* a short burst: the undo's own delete fails, later allocations succeed */
    oom_tally tal_w = persistent_sweep(&cfg_w);
    TEST_ASSERT_TRUE(tal_w.failures > 100);
    /* The silent points are ff.c's shrink_build/shrink_commit, whose cleanup deletes are still
     * (void) (merged code, outside rules 10-11: left for the owner of ff.c). dff_merge.c's undo
     * had 27 of them before it reported its own failure. */
    TEST_ASSERT_TRUE_MESSAGE(tal_w.silent <= SILENT_FF_C,
                             "a rollback failed without the discard line");
    printf("opt_dff persistent OOM: %ld+%ld failing points (stride %ld), %u+%u invalid IR, all "
           "with the discard line\n",
           tal_a.failures, tal_b.failures, (long)PERSISTENT_STRIDE, tal_a.invalid, tal_b.invalid);
    printf("opt_dff burst OOM (2 allocations): %ld failing points, %u invalid IR, %u silent\n",
           tal_w.failures, tal_w.invalid, tal_w.silent);
}

static void run_merge_tests(void) {
    RUN_TEST(test_dff_fine_no_merge);
    RUN_TEST(test_dff_pattern_depth);
    RUN_TEST(test_dff_srst_merge);
    RUN_TEST(test_dff_srst_polarity);
    RUN_TEST(test_dff_srst_two_controls);
    RUN_TEST(test_dff_srst_value_conflict);
    RUN_TEST(test_dff_en_merge);
    RUN_TEST(test_dff_srst_then_en);
    RUN_TEST(test_dff_en_then_srst);
    RUN_TEST(test_dff_nodffe_nosdff);
    RUN_TEST(test_dff_merge_keeps_attrs);
    RUN_TEST(test_dff_single_user);
    RUN_TEST(test_dff_no_aload_srst);
    RUN_TEST(test_dff_feedback_patterns);
    RUN_TEST(test_dff_feedback_x_select);
    RUN_TEST(test_dff_feedback_two_paths);
    RUN_TEST(test_dff_slices);
    RUN_TEST(test_dff_groups_by_control);
    RUN_TEST(test_dff_merge_keeps_init);
    RUN_TEST(test_dff_merge_oom);
    RUN_TEST(test_dff_persistent_oom);
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
    RUN_TEST(test_dff_sr_clr_always);
    RUN_TEST(test_dff_sr_set_always);
    RUN_TEST(test_dff_sr_partial_keeps_name_and_attrs);
    RUN_TEST(test_dff_keep_survives);
    RUN_TEST(test_dff_provenance);
    RUN_TEST(test_dff_provenance_shrink_and_gate);
    RUN_TEST(test_dff_keep_control);
    RUN_TEST(test_dff_sr_to_arst);
    RUN_TEST(test_dff_aload_never);
    RUN_TEST(test_dff_aload_always);
    RUN_TEST(test_dff_aload_const_ad);
    RUN_TEST(test_dff_arst);
    RUN_TEST(test_dff_srst);
    RUN_TEST(test_dff_en);
    RUN_TEST(test_dff_en_never_srst);
    RUN_TEST(test_dff_const_clk);
    RUN_TEST(test_dff_d_eq_q);
    RUN_TEST(test_dff_d_eq_q_srst);
    RUN_TEST(test_dff_aload_d_eq_q);
    RUN_TEST(test_dff_constbits_x_init);
    RUN_TEST(test_dff_constbits_conflict);
    RUN_TEST(test_dff_constbits_one_stage);
    RUN_TEST(test_dff_counts);
    RUN_TEST(test_dff_proc_opaque);
    RUN_TEST(test_dff_open_pin_untouched);
    RUN_TEST(test_dff_fine);
    RUN_TEST(test_dff_yosys_cases);
    RUN_TEST(test_dff_tie_invisible_to_rules);
    RUN_TEST(test_dff_constbits_partial);
    RUN_TEST(test_dff_constbits_z);
    RUN_TEST(test_dff_args);
    RUN_TEST(test_dff_deterministic);
    RUN_TEST(test_dff_oom);
    run_merge_tests();
    return UNITY_END();
}
