/*
 * test_hardmap_mul.c — hardmap, PR 3C-1: the pass registration, the glue kit's transaction and the
 * `$mul` rule (3C spec H-3, H-7..H-10, M-1..M-7; plan Task 2 Step 1). Every case is flat RTLIL
 * with lib/vtr.o3lib read (pb_build_lib); hm_run checks the IR (FULL) before and after, and the
 * value-preserving cases simulate before and after (hm_sim_exhaustive).
 */
#include "hardmap_fixture.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/hardmap/hardmap.h"
#include "passes/manager.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 16384,
    MUL_PINS = 36,
    OUT_PINS = 72,
    OOM_LIMIT = 4000,
    OOM_QUIET = 4,
    MAX_CELLS = 8,
    EXPLICIT_WIDTH = 8
};

#ifndef ODIN3_SOURCE_DIR
#error "ODIN3_SOURCE_DIR must name the source tree"
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

/* One `$mul` cell: widths, signedness of both operands and an optional constant B. */
typedef struct mulcase {
    uint32_t a_width;
    uint32_t b_width;
    uint32_t y_width;
    bool a_signed;
    bool b_signed;
    const char *b_const; /* NULL: B is the input wire */
} mulcase;

/* The module `name` with cells u0 .. u<n-1>, each on its own wires a<i>, b<i>, y<i>. */
static void mul_module(odin3_strbuf *out, const char *name, const mulcase *cases, size_t count) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "module \\%s\n", name));
    uint32_t port = 1;
    for (size_t i = 0; i < count; i++) {
        const mulcase *mc = &cases[i];
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out,
                                                             "  wire width %" PRIu32
                                                             " input %" PRIu32 " \\a%zu\n",
                                                             mc->a_width, port++, i));
        if (mc->b_const == NULL) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out,
                                                                 "  wire width %" PRIu32
                                                                 " input %" PRIu32 " \\b%zu\n",
                                                                 mc->b_width, port++, i));
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out,
                                                             "  wire width %" PRIu32
                                                             " output %" PRIu32 " \\y%zu\n",
                                                             mc->y_width, port++, i));
    }
    for (size_t i = 0; i < count; i++) {
        const mulcase *mc = &cases[i];
        char bsig[64];
        (void)snprintf(bsig, sizeof bsig, "\\b%zu", i);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(
                          out,
                          "  cell $mul \\u%zu\n    parameter \\A_SIGNED %d\n"
                          "    parameter \\B_SIGNED %d\n    parameter \\A_WIDTH %" PRIu32
                          "\n    parameter \\B_WIDTH %" PRIu32 "\n    parameter \\Y_WIDTH %" PRIu32
                          "\n    connect \\A \\a%zu\n    connect \\B %s\n"
                          "    connect \\Y \\y%zu\n  end\n",
                          i, mc->a_signed ? 1 : 0, mc->b_signed ? 1 : 0, mc->a_width, mc->b_width,
                          mc->y_width, i, mc->b_const ? mc->b_const : bsig, i));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "end\n"));
}

static void mul_rtlil(odin3_strbuf *out, const mulcase *cases, size_t count) {
    mul_module(out, "m", cases, count);
}

static void build_cases(const mulcase *cases, size_t count, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_rtlil(&text, cases, count);
    hm_build_ok(text.data, res);
    odin3_strbuf_free(&text);
}

static void build_one(mulcase mc, pb_result *res) {
    build_cases(&mc, 1, res);
}

static odin3_net_id wire_net(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(res->module, wire, bit);
}

/* Pin `index` of port `port` of a node. */
typedef struct pin_at {
    uint32_t port;
    uint32_t index;
} pin_at;

static odin3_net_id pin_net(const pb_result *res, odin3_node_id node, pin_at at) {
    odin3_pinslice pins = odin3_node_port(res->module, node, at.port);
    TEST_ASSERT_TRUE(at.index < pins.count);
    return odin3_pin_net(res->module, (odin3_pin_id){pins.first.v + at.index});
}

static const char *node_name_of(const pb_result *res, odin3_node_id node) {
    return odin3_strtab_get(odin3_design_strtab(res->design), odin3_node_name(res->module, node));
}

static void assert_rtlil_equal(const pb_result *res, const odin3_strbuf *expected) {
    odin3_strbuf now;
    odin3_strbuf_init(&now);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(res, &now));
    TEST_ASSERT_EQUAL_STRING(expected->data, now.data);
    odin3_strbuf_free(&now);
}

/* --- H-3: the library cells ---------------------------------------------------------------- */

static void test_h3_missing_type(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mulcase mc = {4, 4, 8, false, false, NULL};
    mul_rtlil(&text, &mc, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text.data, &res));
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hardmap_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(
        log_text,
        "hardmap: cell type 'multiply' is not registered; read_techlib lib/vtr.o3lib first"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

static void test_h3_not_hard(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mulcase mc = {4, 4, 8, false, false, NULL};
    mul_rtlil(&text, &mc, 1);
    pb_source src = {text.data, ODIN3_SOURCE_DIR "/tests/unit/data/hardmap/not_hard.o3lib"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build_lib(src, &res));
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hardmap_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "hardmap: cell type 'multiply' is not a hard block"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

static void explicit_text(odin3_strbuf *out, uint32_t width, bool with_mul);

static void test_h3_bad_shape(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mulcase mc = {4, 4, 8, false, false, NULL};
    mul_rtlil(&text, &mc, 1);
    pb_source src = {text.data, ODIN3_SOURCE_DIR "/tests/unit/data/hardmap/bad_shape.o3lib"};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build_lib(src, &res));
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hardmap_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(
        log_text, "hardmap: cell type 'multiply' does not have the expected ports and parameters"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* Every module is visited, not only the first: m1 has a 2x2 (soft), m2 a 4x4, m3 an explicit
 * WIDTH 8 instance. */
static void test_m4_two_modules(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    const mulcase soft = {2, 2, 4, false, false, NULL};
    const mulcase hard = {4, 4, 8, false, false, NULL};
    mul_module(&text, "m1", &soft, 1);
    mul_module(&text, "m2", &hard, 1);
    explicit_text(&text, 8, false);
    hm_build_ok(text.data, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(pb_module(&res, "m1"), "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(pb_module(&res, "m2"), "$mul"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(pb_module(&res, "m2"), "multiply"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(pb_module(&res, "m"), "multiply"));
    TEST_ASSERT_EQUAL_UINT64(2, stats.multiply);
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* --- M-2, M-5: which cells go hard --------------------------------------------------------- */

typedef enum verdict { HARD, SOFT_WARN } verdict;
typedef struct threshold_row {
    mulcase mc;
    verdict want;
    const char *warning; /* part of the WARN line, for SOFT_WARN */
} threshold_row;

static const threshold_row k_rows[] = {
    {{2, 2, 4, false, false, NULL}, SOFT_WARN, "is 2x2"},
    {{1, 1, 2, false, false, NULL}, HARD, NULL},
    {{1, 5, 6, false, false, NULL}, HARD, NULL},
    {{2, 32, 34, false, false, NULL}, HARD, NULL},
    {{32, 2, 34, false, false, NULL}, HARD, NULL},
    {{3, 3, 6, false, false, NULL}, HARD, NULL},
    {{36, 36, 72, false, false, NULL}, HARD, NULL},
    {{37, 4, 41, false, false, NULL}, SOFT_WARN, "is 37x4; the arch's multiplier is 36 wide"},
    {{4, 37, 41, false, false, NULL}, SOFT_WARN, "is 4x37; the arch's multiplier is 36 wide"},
    {{18, 18, 36, true, true, NULL}, HARD, NULL},
    {{18, 18, 37, true, true, NULL}, SOFT_WARN, "has Y_WIDTH 37 above 36"},
    {{4, 4, 8, true, false, NULL}, HARD, NULL}, /* mixed signs: the unsigned path (PHASE2 #25) */
    {{4, 4, 12, false, false, NULL}, SOFT_WARN, "above A_WIDTH + B_WIDTH (8)"},
    {{4, 4, 12, true, true, NULL}, HARD, NULL}, /* signed: every Y bit is driven */
};

static void check_counts(const pb_result *res, bool hard) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(hard ? 1 : 0, hm_count(res->module, "multiply"),
                                     "multiply count");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(hard ? 0 : 1, hm_count(res->module, "$mul"), "$mul count");
}

static void check_row(const threshold_row *row) {
    pb_result res;
    log_text[0] = '\0';
    build_one(row->mc, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    bool hard = row->want == HARD;
    check_counts(&res, hard);
    TEST_ASSERT_EQUAL_UINT64(hard ? 0 : 1, stats.soft);
    if (hard) {
        TEST_ASSERT_NULL(strstr(log_text, "soft for lower"));
    } else {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, row->warning), log_text);
    }
    pb_free(&res);
}

static void test_m2_threshold(void) {
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) {
        check_row(&k_rows[i]);
    }
}

/* --- M-3 .. M-5: the pins ------------------------------------------------------------------ */

static bool is_pad(const pb_result *res, odin3_net_id net) {
    return odin3_net_valid(net) && odin3_net_const_value(res->module, net) == ODIN3_CONST_X;
}

/* Pins 0 .. used-1 of an operand port are the wire's bits; the rest is the pad. */
typedef struct operand_at {
    uint32_t port;
    uint32_t used;
} operand_at;

static void check_operand(const pb_result *res, odin3_node_id mul, operand_at op) {
    uint32_t port = op.port;
    uint32_t used = op.used;
    const char *wire = port == 0 ? "a0" : "b0";
    for (uint32_t i = 0; i < MUL_PINS; i++) {
        odin3_net_id net = pin_net(res, mul, (pin_at){port, i});
        if (i < used) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(res, wire, i).v, net.v);
        } else {
            TEST_ASSERT_TRUE(is_pad(res, net));
        }
    }
}

/* out[0 .. driven-1] are the y0 bits; the other outputs are fresh nets with no reader. */
static void check_outputs(const pb_result *res, odin3_node_id mul, uint32_t driven) {
    for (uint32_t i = 0; i < OUT_PINS; i++) {
        odin3_net_id net = pin_net(res, mul, (pin_at){2, i});
        if (i < driven) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(res, "y0", i).v, net.v);
        } else {
            TEST_ASSERT_EQUAL_UINT32(1, odin3_net_pins(res->module, net).count);
        }
    }
}

static void test_m3_shape_8x7(void) {
    pb_result res;
    build_one((mulcase){8, 7, 15, false, false, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    const odin3_value *width = odin3_node_param(res.module, mul, 0);
    TEST_ASSERT_NOT_NULL(width);
    TEST_ASSERT_EQUAL_INT64(MUL_PINS, width->i);
    check_operand(&res, mul, (operand_at){0, 8});
    check_operand(&res, mul, (operand_at){1, 7});
    check_outputs(&res, mul, 15);
    pb_free(&res);
}

/* M-3 as amended: Y_WIDTH above the product stays soft (a gnd merge after the delete would
 * allocate, H-9); the cell and its text are untouched. */
static void test_m3_y_wider(void) {
    pb_result res;
    build_one((mulcase){4, 4, 12, false, false, NULL}, &res);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    check_counts(&res, false);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "run wreduce first"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    pb_free(&res);
}

static void test_m4_pad_is_x(void) {
    pb_result res;
    build_one((mulcase){3, 5, 8, false, false, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    odin3_net_id pad = pin_net(&res, mul, (pin_at){0, 3});
    TEST_ASSERT_TRUE(is_pad(&res, pad));
    for (uint32_t i = 3; i < MUL_PINS; i++) {
        TEST_ASSERT_EQUAL_UINT32(pad.v, pin_net(&res, mul, (pin_at){0, i}).v);
    }
    for (uint32_t i = 5; i < MUL_PINS; i++) {
        TEST_ASSERT_EQUAL_UINT32(pad.v, pin_net(&res, mul, (pin_at){1, i}).v);
    }
    pb_free(&res);
}

static odin3_status build_mc(pb_result *out, mulcase mc) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_rtlil(&text, &mc, 1);
    odin3_status st = hm_build(text.data, out);
    odin3_strbuf_free(&text);
    return st == ODIN3_OK ? odin3_design_set_top(out->design, odin3_module_id_of(out->module)) : st;
}

static odin3_status build_s3x3(pb_result *out) {
    return build_mc(out, (mulcase){3, 3, 6, true, true, NULL});
}
static odin3_status build_s4x4(pb_result *out) {
    return build_mc(out, (mulcase){4, 4, 8, true, true, NULL});
}
static odin3_status build_s4x4_y6(pb_result *out) {
    return build_mc(out, (mulcase){4, 4, 6, true, true, NULL});
}
static odin3_status build_s2x5_y9(pb_result *out) {
    return build_mc(out, (mulcase){2, 5, 9, true, true, NULL});
}
static odin3_status build_mixed_su(pb_result *out) {
    return build_mc(out, (mulcase){4, 4, 8, true, false, NULL});
}
static odin3_status build_mixed_us(pb_result *out) {
    return build_mc(out, (mulcase){4, 4, 8, false, true, NULL});
}
static odin3_status build_mixed_su_y6(pb_result *out) {
    return build_mc(out, (mulcase){3, 5, 6, true, false, NULL});
}
static odin3_status build_u4x4(pb_result *out) {
    return build_mc(out, (mulcase){4, 4, 8, false, false, NULL});
}
static odin3_status build_u5x3_y12(pb_result *out) {
    return build_mc(out, (mulcase){5, 3, 12, false, false, NULL});
}
static odin3_status build_u4x4_y5(pb_result *out) {
    return build_mc(out, (mulcase){4, 4, 5, false, false, NULL});
}
static odin3_status build_u8x7(pb_result *out) {
    return build_mc(out, (mulcase){8, 7, 15, false, false, NULL});
}
static odin3_status build_u1x1(pb_result *out) {
    return build_mc(out, (mulcase){1, 1, 2, false, false, NULL});
}

static void test_m5_signed_sim(void) {
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_s3x3));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_s4x4));
}

/* Mixed signs are unsigned (PHASE2 #25): a sign-extending mapping would get these wrong. */
static void test_m5_mixed_sim(void) {
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_mixed_su));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_mixed_us));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_mixed_su_y6));
}

static void test_m5_signed_sim_trunc_ext(void) {
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_s4x4_y6));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_s2x5_y9));
}

/* Signed 3x3 into Y 6: operand pins 3..5 repeat the sign bit; pins 6.. are the pad. */
static void check_sign_pin(const pb_result *res, odin3_node_id mul, uint32_t pin) {
    for (uint32_t port = 0; port < 2; port++) {
        const char *wire = port == 0 ? "a0" : "b0";
        odin3_net_id net = pin_net(res, mul, (pin_at){port, pin});
        if (pin < 3) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(res, wire, pin).v, net.v);
        } else if (pin < 6) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(res, wire, 2).v, net.v);
        } else {
            TEST_ASSERT_TRUE(is_pad(res, net));
        }
    }
}

static void test_m5_signed_pins(void) {
    pb_result res;
    build_one((mulcase){3, 3, 6, true, true, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    for (uint32_t pin = 0; pin < MUL_PINS; pin++) {
        check_sign_pin(&res, mul, pin);
    }
    pb_free(&res);
}

static void test_unsigned_sim(void) {
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_u4x4));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_u5x3_y12));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_u4x4_y5));
}

static void test_unsigned_sim_edges(void) {
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_u1x1));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_u8x7));
    TEST_ASSERT_TRUE(hm_sim_equal(build_u8x7, 200, 7));
}

static void test_m1_const_operand(void) {
    pb_result res;
    build_one((mulcase){4, 4, 8, false, false, "4'0101"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    pb_free(&res);
}

/* --- M-6: explicit multiply instances ------------------------------------------------------ */

static void explicit_text(odin3_strbuf *out, uint32_t width, bool with_mul) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(out,
                                       "module \\m\n  wire width %" PRIu32 " input 1 \\a\n"
                                       "  wire width %" PRIu32 " input 2 \\b\n"
                                       "  wire width %" PRIu32 " output 3 \\o\n"
                                       "  cell \\multiply \\u\n    parameter \\WIDTH %" PRIu32
                                       "\n    connect \\a \\a\n    connect \\b \\b\n"
                                       "    connect \\out \\o\n  end\n",
                                       width, width, 2 * width, width));
    if (with_mul) {
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK,
            odin3_strbuf_appendf(out, "  wire width 4 input 4 \\x\n  wire width 4 input 5 \\z\n"
                                      "  wire width 8 output 6 \\q\n  cell $mul \\v\n"
                                      "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
                                      "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n"
                                      "    parameter \\Y_WIDTH 8\n    connect \\A \\x\n"
                                      "    connect \\B \\z\n    connect \\Y \\q\n  end\n"));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "end\n"));
}

static void run_explicit(uint32_t width, pb_result *res, odin3_hardmap_stats *stats) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, width, false);
    hm_build_ok(text.data, res);
    odin3_strbuf_free(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(res, stats));
}

/* WIDTH 36 is already the arch's multiplier: untouched, no warning. */
static void test_m6_width_36_untouched(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, 36, false);
    hm_build_ok(text.data, &res);
    odin3_node_id before = hm_nth(res.module, "multiply", 0);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(before.v, hm_nth(res.module, "multiply", 0).v);
    TEST_ASSERT_NULL(strstr(log_text, "treated as"));
    TEST_ASSERT_EQUAL_UINT64(0, stats.multiply);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* WIDTH 2 is a 2x2: the `$mul` for lower, as Parmys resolves the primitive to `$mul`. */
static void test_m6_width_2_is_mul(void) {
    pb_result res;
    odin3_hardmap_stats stats;
    run_explicit(2, &res, &stats);
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_NOT_NULL(strstr(
        log_text, "hardmap: explicit 'multiply' instance 'u' (WIDTH 2) treated as '$mul' as Parmys "
                  "does"));
    TEST_ASSERT_EQUAL_STRING("u", node_name_of(&res, hm_nth(res.module, "$mul", 0)));
    pb_free(&res);
}

/* Pin i of operand `port` of mul is wire[i] below used, else the pad. */
static void check_padded(const pb_result *res, odin3_node_id mul, uint32_t port, const char *wire) {
    for (uint32_t i = 0; i < MUL_PINS; i++) {
        odin3_net_id net = pin_net(res, mul, (pin_at){port, i});
        if (i < EXPLICIT_WIDTH) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(res, wire, i).v, net.v);
        } else {
            TEST_ASSERT_TRUE(is_pad(res, net));
        }
    }
}

/* WIDTH 8 is padded to 36 exactly as a `$mul` of that shape. */
static void test_m6_width_8_padded(void) {
    pb_result res;
    odin3_hardmap_stats stats;
    run_explicit(EXPLICIT_WIDTH, &res, &stats);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.multiply);
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    TEST_ASSERT_EQUAL_INT64(MUL_PINS, odin3_node_param(res.module, mul, 0)->i);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "instance 'u' (WIDTH 8) treated as '$mul'"));
    check_padded(&res, mul, 0, "a");
    check_padded(&res, mul, 1, "b");
    for (uint32_t i = 0; i < 2 * EXPLICIT_WIDTH; i++) {
        TEST_ASSERT_EQUAL_UINT32(wire_net(&res, "o", i).v, pin_net(&res, mul, (pin_at){2, i}).v);
    }
    pb_free(&res);
}

static odin3_status build_explicit_1(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, 1, false);
    odin3_status st = hm_build(text.data, out);
    odin3_strbuf_free(&text);
    return st == ODIN3_OK ? odin3_design_set_top(out->design, odin3_module_id_of(out->module)) : st;
}
static odin3_status build_explicit_2(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, 2, false);
    odin3_status st = hm_build(text.data, out);
    odin3_strbuf_free(&text);
    return st == ODIN3_OK ? odin3_design_set_top(out->design, odin3_module_id_of(out->module)) : st;
}
static odin3_status build_explicit_5(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, 5, false);
    odin3_status st = hm_build(text.data, out);
    odin3_strbuf_free(&text);
    return st == ODIN3_OK ? odin3_design_set_top(out->design, odin3_module_id_of(out->module)) : st;
}

static void test_m6_explicit_sim(void) {
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_explicit_1));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_explicit_2));
    TEST_ASSERT_TRUE(hm_sim_exhaustive(build_explicit_5));
}

static void test_m6_over_36_error(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    explicit_text(&text, 40, true);
    hm_build_ok(text.data, &res);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hardmap_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text,
               "hardmap: 'multiply' instance 'u' has WIDTH 40; the arch's multiplier is 36 wide"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul")); /* nothing was mapped */
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* --- M-7, H-7, H-8: order, names, provenance, determinism ----------------------------------- */

static const mulcase k_three[3] = {
    {4, 4, 8, false, false, NULL}, {3, 5, 8, false, false, NULL}, {6, 2, 8, false, false, NULL}};

static void test_m7_order_naming(void) {
    pb_result res;
    build_cases(k_three, 3, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    uint32_t key = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res.design, odin3_bytes_cstr("hardmap"), &key));
    char want[8];
    uint32_t last = 0;
    for (uint32_t i = 0; i < 3; i++) {
        odin3_node_id node = hm_nth(res.module, "multiply", i);
        TEST_ASSERT_TRUE(node.v > last);
        last = node.v;
        (void)snprintf(want, sizeof want, "u%" PRIu32, i);
        TEST_ASSERT_EQUAL_STRING(want, node_name_of(&res, node));
        const odin3_value *attr =
            odin3_attr_get(res.module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key);
        TEST_ASSERT_NOT_NULL(attr);
        TEST_ASSERT_EQUAL_STRING("multiply:mul",
                                 odin3_strtab_get(odin3_design_strtab(res.design), attr->str));
    }
    pb_free(&res);
}

static void test_h7_prov(void) {
    pb_result res;
    build_cases(k_three, 3, &res);
    odin3_prov_id source[3];
    for (uint32_t i = 0; i < 3; i++) {
        source[i] = odin3_node_prov(res.module, hm_nth(res.module, "$mul", i));
        TEST_ASSERT_TRUE(source[i].v != 0);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    for (uint32_t i = 0; i < 3; i++) {
        odin3_node_id node = hm_nth(res.module, "multiply", i);
        const odin3_prov_record *rec =
            odin3_prov_get(res.design, odin3_node_prov(res.module, node));
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
        TEST_ASSERT_EQUAL_UINT32(res.ctx.run.v, rec->run.v);
        TEST_ASSERT_EQUAL_UINT32(i + 1, rec->op); /* one operation per source cell */
        TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
        TEST_ASSERT_EQUAL_UINT32(source[i].v, rec->parents.ids[0].v);
    }
    pb_free(&res);
}

static void test_h8_determinism(void) {
    pb_result one;
    pb_result two;
    odin3_strbuf text_one;
    odin3_strbuf text_two;
    odin3_strbuf_init(&text_one);
    odin3_strbuf_init(&text_two);
    build_cases(k_three, 3, &one);
    build_cases(k_three, 3, &two);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&one, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&two, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&one, &text_one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&two, &text_two));
    TEST_ASSERT_EQUAL_STRING(text_one.data, text_two.data);
    odin3_strbuf_free(&text_one);
    odin3_strbuf_free(&text_two);
    pb_free(&one);
    pb_free(&two);
}

static void test_h10_info(void) {
    pb_result res;
    const mulcase cases[5] = {{4, 4, 8, false, false, NULL},
                              {2, 2, 4, false, false, NULL},
                              {3, 5, 8, false, false, NULL},
                              {2, 2, 4, false, false, NULL},
                              {6, 2, 8, false, false, NULL}};
    build_cases(cases, 5, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_NOT_NULL(strstr(log_text,
                                "hardmap: 3 multiply, 0 adder in 0 chains, 0 single_port_ram + 0 "
                                "dual_port_ram slices; 2 cells left soft"));
    TEST_ASSERT_EQUAL_UINT64(3, stats.multiply);
    TEST_ASSERT_EQUAL_UINT64(2, stats.soft);
    pb_free(&res);
}

static void test_hardmap_twice(void) {
    pb_result res;
    build_cases(k_three, 3, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(3, stats.multiply);
    odin3_strbuf once;
    odin3_strbuf_init(&once);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &once));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.multiply);
    assert_rtlil_equal(&res, &once);
    odin3_strbuf_free(&once);
    pb_free(&res);
}

/* --- H-9: out of memory -------------------------------------------------------------------- */

/* What a failed run must not change: the text, and the live nodes and nets (the text does not show
 * unnamed nets or constant nodes). */
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

static void snap_expect(const pb_result *res, const snapshot *want) {
    snapshot now;
    snap_take(res, &now);
    TEST_ASSERT_EQUAL_STRING(want->text.data, now.text.data);
    TEST_ASSERT_EQUAL_UINT32(want->nodes, now.nodes);
    TEST_ASSERT_EQUAL_UINT32(want->nets, now.nets);
    odin3_strbuf_free(&now.text);
}

/* One mapping with the allocation failing after fail_at; true when it was injected. `mapped` is
 * the cell type the finished run leaves; `source` the one a failed run leaves. */
typedef struct oom_case {
    const char *rtlil;
    const char *source;
    const char *mapped;
} oom_case;

static bool oom_try(const oom_case *oc, long fail_at) {
    pb_result res;
    snapshot before;
    hm_build_ok(oc->rtlil, &res);
    snap_take(&res, &before);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
    odin3_util_set_alloc_fail_after(-1);
    bool injected = st != ODIN3_OK;
    if (injected) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, oc->source)); /* the source is intact */
        if (strcmp(oc->source, oc->mapped) != 0) {
            TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, oc->mapped));
        }
        snap_expect(&res, &before); /* and nothing else changed: no fresh net, no constant */
    } else {
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, oc->mapped));
        const odin3_value *width =
            odin3_node_param(res.module, hm_nth(res.module, "multiply", 0), 0);
        TEST_ASSERT_TRUE(width != NULL && width->i == MUL_PINS); /* mapped, not just left alone */
    }
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
    odin3_strbuf_free(&before.text);
    pb_free(&res);
    return injected;
}

/* Fails the n-th allocation for n = 0, 1 ... until OOM_QUIET runs in a row succeed (a failure
 * inside a diagnostic is swallowed on purpose, so a success can come early); returns the number of
 * runs that were failed by injection. */
static long oom_sweep(const oom_case *oc) {
    long failed = 0;
    long quiet = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT && quiet < OOM_QUIET; fail_at++) {
        bool injected = oom_try(oc, fail_at);
        failed += injected ? 1 : 0;
        quiet = injected ? 0 : quiet + 1;
    }
    TEST_ASSERT_TRUE(quiet == OOM_QUIET);
    return failed;
}

static void test_h9_oom(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_rtlil(&text, &(mulcase){8, 7, 15, false, false, NULL}, 1);
    TEST_ASSERT_TRUE(oom_sweep(&(oom_case){text.data, "$mul", "multiply"}) > 1);
    odin3_strbuf_free(&text);
}

/* 4x4 into Y 12: the cell stays soft, so no allocation fails the run (M-2 of the review). */
static void test_h9_oom_y_wider(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_rtlil(&text, &(mulcase){4, 4, 12, false, false, NULL}, 1);
    pb_result res;
    snapshot before;
    hm_build_ok(text.data, &res);
    snap_take(&res, &before);
    for (long fail_at = 0; fail_at < MAX_CELLS; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
        snap_expect(&res, &before);
    }
    odin3_strbuf_free(&before.text);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* The signed cell and the explicit instances go through the same transaction. */
static void test_h9_oom_variants(void) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_rtlil(&text, &(mulcase){3, 3, 6, true, true, NULL}, 1);
    TEST_ASSERT_TRUE(oom_sweep(&(oom_case){text.data, "$mul", "multiply"}) > 1);
    odin3_strbuf_free(&text);
    odin3_strbuf_init(&text);
    explicit_text(&text, 8, false);
    TEST_ASSERT_TRUE(oom_sweep(&(oom_case){text.data, "multiply", "multiply"}) > 1);
    odin3_strbuf_free(&text);
}

/* --- the pass ------------------------------------------------------------------------------- */

static void test_pass_runs_and_takes_no_arguments(void) {
    pb_result res;
    build_cases(k_three, 3, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run(res.design, "hardmap", odin3_bytes_cstr("-x")));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "hardmap: takes no arguments"));
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(res.design, "hardmap", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    pb_free(&res);
}

static void test_refuse_processes(void) {
    pb_result res;
    const char *text = "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                       "  wire output 3 \\q\n  process $p\n    sync posedge \\clk\n"
                       "      update \\q \\d\n  end\nend\n";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_build(text, &res));
    TEST_ASSERT_NOT_EQUAL(ODIN3_OK, odin3_pass_run(res.design, "hardmap", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "has processes"));
    pb_free(&res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_h3_missing_type);
    RUN_TEST(test_h3_not_hard);
    RUN_TEST(test_m2_threshold);
    RUN_TEST(test_m3_shape_8x7);
    RUN_TEST(test_m3_y_wider);
    RUN_TEST(test_m4_pad_is_x);
    RUN_TEST(test_m5_signed_sim);
    RUN_TEST(test_m5_signed_sim_trunc_ext);
    RUN_TEST(test_m5_mixed_sim);
    RUN_TEST(test_m5_signed_pins);
    RUN_TEST(test_unsigned_sim);
    RUN_TEST(test_unsigned_sim_edges);
    RUN_TEST(test_m1_const_operand);
    RUN_TEST(test_m6_width_36_untouched);
    RUN_TEST(test_m6_width_2_is_mul);
    RUN_TEST(test_m6_width_8_padded);
    RUN_TEST(test_m6_explicit_sim);
    RUN_TEST(test_m6_over_36_error);
    RUN_TEST(test_m4_two_modules);
    RUN_TEST(test_h3_bad_shape);
    RUN_TEST(test_m7_order_naming);
    RUN_TEST(test_h7_prov);
    RUN_TEST(test_h8_determinism);
    RUN_TEST(test_h10_info);
    RUN_TEST(test_hardmap_twice);
    RUN_TEST(test_h9_oom);
    RUN_TEST(test_h9_oom_y_wider);
    RUN_TEST(test_h9_oom_variants);
    RUN_TEST(test_pass_runs_and_takes_no_arguments);
    RUN_TEST(test_refuse_processes);
    return UNITY_END();
}
