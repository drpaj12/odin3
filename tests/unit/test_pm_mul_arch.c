/*
 * test_pm_mul_arch.c — partial_map 4.3 multipliers from the arch record (4.2 spec X-1 to X-3, X-5,
 * X-6; plan Tasks 13 and 15): the width W of the record, the shape of the model, `--min-mult`, the
 * explicit `multiply` rule with W, and the light signed rule. Fixtures: acc_27.xml (a 27-wide
 * square model), acc_25x18.xml, acc_renamed.xml, acc_none.xml.
 */
#include "frontends/arch/arch.h"
#include "hardmap_fixture.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hardmap.h"
#include "proc_build.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 16384,
    NAME_CAP = 1024,
    W27 = 27,
    W36 = 36,
    MIN_MULT_TEST = 8,
    SIM_CYCLES = 64,
    SIM_SEED = 11
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
    odin3_log_set_sink(NULL, NULL);
}

static void assert_logged(const char *needle) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), needle);
}

static void assert_not_logged(const char *needle) {
    TEST_ASSERT_NULL_MESSAGE(strstr(log_text, needle), needle);
}

/* --- builders ------------------------------------------------------------------------------ */

typedef struct mulcase {
    uint32_t a;
    uint32_t b;
    uint32_t y;
    bool a_signed;
    bool b_signed;
} mulcase;

/* One `$mul` cell u on wires a, b, y. */
static void mul_text(odin3_strbuf *out, mulcase mc) {
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(
            out,
            "module \\m\n  wire width %" PRIu32 " input 1 \\a\n  wire width %" PRIu32
            " input 2 \\b\n  wire width %" PRIu32 " output 3 \\y\n"
            "  cell $mul \\u\n    parameter \\A_SIGNED %d\n"
            "    parameter \\B_SIGNED %d\n    parameter \\A_WIDTH %" PRIu32
            "\n    parameter \\B_WIDTH %" PRIu32 "\n    parameter \\Y_WIDTH %" PRIu32
            "\n    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n"
            "  end\nend\n",
            mc.a, mc.b, mc.y, mc.a_signed ? 1 : 0, mc.b_signed ? 1 : 0, mc.a, mc.b, mc.y));
}

/* A design of one `$mul`, with the fixture record read (NULL: no record, the k6 library). */
static void build_mul(mulcase mc, const char *fixture, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mul_text(&text, mc);
    if (fixture == NULL) {
        hm_build_ok(text.data, res);
    } else {
        char path[NAME_CAP];
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text.data, res));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_design_set_top(res->design, odin3_module_id_of(res->module)));
        (void)snprintf(path, sizeof path, "%s/%s", ODIN3_ARCH_DATA_DIR, fixture);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_read(res->design, path));
    }
    odin3_strbuf_free(&text);
}

static odin3_net_id wire_net(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(res->module, wire, bit);
}

typedef struct pin_at {
    uint32_t port;
    uint32_t index;
} pin_at;

static odin3_net_id pin_net(const pb_result *res, odin3_node_id node, pin_at at) {
    odin3_pinslice pins = odin3_node_port(res->module, node, at.port);
    TEST_ASSERT_TRUE(at.index < pins.count);
    return odin3_pin_net(res->module, (odin3_pin_id){pins.first.v + at.index});
}

static bool is_pad(const pb_result *res, odin3_net_id net) {
    return odin3_net_valid(net) && odin3_net_const_value(res->module, net) == ODIN3_CONST_X;
}

/* --- X-1: W from the record ----------------------------------------------------------------- */

/* Pins 0 .. used-1 of operand port `port` are the bits of `wire`; the rest are the pad. */
static void check_operand(const pb_result *res, odin3_node_id mul, uint32_t port, const char *wire,
                          uint32_t used) {
    for (uint32_t i = 0; i < W27; i++) {
        odin3_net_id net = pin_net(res, mul, (pin_at){port, i});
        if (i < used) {
            TEST_ASSERT_EQUAL_UINT32(wire_net(res, wire, i).v, net.v);
        } else {
            TEST_ASSERT_TRUE(is_pad(res, net));
        }
    }
}

static void test_x1_9x9_on_27(void) {
    pb_result res;
    build_mul((mulcase){9, 9, 18, false, false}, "acc_27.xml", &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.multiply);
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    const odin3_value *width = odin3_node_param(res.module, mul, 0);
    TEST_ASSERT_NOT_NULL(width);
    TEST_ASSERT_EQUAL_INT64(W27, width->i);
    TEST_ASSERT_EQUAL_UINT32(W27, odin3_node_port(res.module, mul, 0).count);
    TEST_ASSERT_EQUAL_UINT32(2U * W27, odin3_node_port(res.module, mul, 2).count);
    check_operand(&res, mul, 0, "a", 9);
    check_operand(&res, mul, 1, "b", 9);
    for (uint32_t i = 0; i < 18; i++) {
        TEST_ASSERT_EQUAL_UINT32(wire_net(&res, "y", i).v, pin_net(&res, mul, (pin_at){2, i}).v);
    }
    pb_free(&res);
}

static void test_x1_8x8_on_k6_is_36(void) {
    pb_result res;
    build_mul((mulcase){8, 8, 16, false, false}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    const odin3_value *width = odin3_node_param(res.module, mul, 0);
    TEST_ASSERT_NOT_NULL(width);
    TEST_ASSERT_EQUAL_INT64(W36, width->i);
    pb_free(&res);
}

/* --- X-2, X-7: the model's shape ------------------------------------------------------------ */

static void test_x2_asymmetric_model_leaves_mul(void) {
    pb_result res;
    build_mul((mulcase){9, 9, 18, false, false}, "acc_25x18.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_logged("partial_map: 'multiply' model of 'acc_25x18' is 25x18, not square; '$mul' cells "
                  "are left to lower");
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "multiply"));
    pb_free(&res);
}

static void test_x2_renamed_model_leaves_mul(void) {
    pb_result res;
    build_mul((mulcase){9, 9, 18, false, false}, "acc_renamed.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_logged("'multiply' model of 'acc_renamed' does not have the ports a, b and out");
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    pb_free(&res);
}

static void test_x7_no_multiply_model_is_info(void) {
    pb_result res;
    build_mul((mulcase){9, 9, 18, false, false}, "acc_none.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_logged("partial_map: architecture 'acc_none' has no 'multiply' model; 1 '$mul' cells "
                  "left to lower");
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    pb_free(&res);
}

/* --- X-3: --min-mult ------------------------------------------------------------------------ */

typedef struct min_mult_run {
    uint32_t hard; /* multiply cells after the run */
    uint32_t soft; /* $mul cells after the run */
} min_mult_run;

/* A width x width `$mul` on the 27 fixture with --min-mult min_mult (0: not given). */
typedef struct min_mult_case {
    uint32_t min_mult;
    uint32_t width;
} min_mult_case;

static min_mult_run run_min_mult(min_mult_case mc) {
    uint32_t min_mult = mc.min_mult;
    uint32_t width = mc.width;
    pb_result res;
    build_mul((mulcase){width, width, 2U * width, false, false}, "acc_27.xml", &res);
    odin3_partial_map_opts opts = {.min_mult = min_mult};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run_opts(&res.ctx, res.design, &opts, NULL));
    min_mult_run out = {hm_count(res.module, "multiply"), hm_count(res.module, "$mul")};
    pb_free(&res);
    return out;
}

static void test_x3_min_mult_8(void) {
    min_mult_run five = run_min_mult((min_mult_case){MIN_MULT_TEST, 5});
    TEST_ASSERT_EQUAL_UINT32(0, five.hard);
    TEST_ASSERT_EQUAL_UINT32(1, five.soft);
    assert_logged("below --min-mult 8");
    TEST_ASSERT_EQUAL_UINT32(0, run_min_mult((min_mult_case){MIN_MULT_TEST, 7}).hard);
    min_mult_run eight = run_min_mult((min_mult_case){MIN_MULT_TEST, MIN_MULT_TEST});
    TEST_ASSERT_EQUAL_UINT32(1, eight.hard);
    TEST_ASSERT_EQUAL_UINT32(0, eight.soft);
}

static void test_x3_default_is_3(void) {
    TEST_ASSERT_EQUAL_UINT32(0, run_min_mult((min_mult_case){0, 2}).hard);
    assert_logged("is 2x2");
    TEST_ASSERT_EQUAL_UINT32(1, run_min_mult((min_mult_case){0, 3}).hard);
}

/* --- X-6: explicit multiply instances with W ------------------------------------------------ */

/* An explicit `multiply` of WIDTH width on the 27-wide fixture (read_arch: library and record). */
static void build_explicit(uint32_t width, pb_result *res) {
    char text[1024];
    char path[NAME_CAP];
    (void)snprintf(text, sizeof text,
                   "module \\m\n  wire width %" PRIu32 " input 1 \\a\n  wire width %" PRIu32
                   " input 2 \\b\n  wire width %" PRIu32
                   " output 3 \\y\n  cell \\multiply \\u\n    parameter \\WIDTH %" PRIu32
                   "\n    connect \\a \\a\n    connect \\b \\b\n    connect \\out \\y\n"
                   "  end\nend\n",
                   width, width, 2U * width, width);
    (void)snprintf(path, sizeof path, "%s/acc_27.xml", ODIN3_ARCH_DATA_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build_lib((pb_source){text, path}, res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_set_top(res->design, odin3_module_id_of(res->module)));
}

static void test_x6_explicit_20_padded_to_27(void) {
    pb_result res;
    build_explicit(20, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, &stats));
    assert_logged("explicit 'multiply' instance 'u' (WIDTH 20) treated as '$mul' as Parmys does");
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    const odin3_value *width = odin3_node_param(res.module, mul, 0);
    TEST_ASSERT_NOT_NULL(width);
    TEST_ASSERT_EQUAL_INT64(W27, width->i);
    TEST_ASSERT_EQUAL_UINT32(W27, odin3_node_port(res.module, mul, 0).count);
    pb_free(&res);
}

static void test_x6_explicit_27_untouched(void) {
    pb_result res;
    build_explicit(W27, &res);
    odin3_node_id before = hm_nth(res.module, "multiply", 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_not_logged("treated as '$mul'");
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(before.v, hm_nth(res.module, "multiply", 0).v);
    pb_free(&res);
}

/* X-6: WIDTH 40 on the 27-wide arch is the equivalent `$mul`, split by 27 (four products). */
static void test_x6_explicit_40_split_by_27(void) {
    pb_result res;
    build_explicit(40, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    assert_logged("explicit 'multiply' instance 'u' (WIDTH 40) treated as '$mul' as Parmys does");
    TEST_ASSERT_EQUAL_UINT32(4, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    odin3_node_id mul = hm_nth(res.module, "multiply", 0);
    const odin3_value *width = odin3_node_param(res.module, mul, 0);
    TEST_ASSERT_TRUE(width != NULL && width->i == W27);
    pb_free(&res);
}

/* --- X-5: signed, light (Task 15) ----------------------------------------------------------- */

static mulcase g_case;
static const char *g_fixture;

static odin3_status build_global(pb_result *out) {
    build_mul(g_case, g_fixture, out);
    return ODIN3_OK;
}

static bool sim_equal(mulcase mc, const char *fixture) {
    g_case = mc;
    g_fixture = fixture;
    return hm_sim_equal(build_global, SIM_CYCLES, SIM_SEED);
}

/* Y_WIDTH 32 is within W 36 (k6) and above W 27 (COFFE): hard on one, soft with the warning on the
 * other. */
static void test_x5_signed_16x16(void) {
    pb_result res;
    build_mul((mulcase){16, 16, 32, true, true}, NULL, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$mul"));
    assert_not_logged("left to lower");
    pb_free(&res);
    log_text[0] = '\0';
    build_mul((mulcase){16, 16, 32, true, true}, "acc_27.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mul"));
    assert_logged("partial_map: signed '$mul' 'u' needs Y_WIDTH 32 > 27; left to lower");
    pb_free(&res);
}

/* The signed product, negative operands included (random vectors), on both widths. */
static void test_x5_signed_12x12_sim(void) {
    TEST_ASSERT_TRUE(sim_equal((mulcase){12, 12, 24, true, true}, NULL));
    TEST_ASSERT_TRUE(sim_equal((mulcase){12, 12, 24, true, true}, "acc_27.xml"));
}

static void test_x5_signed_other_shapes_sim(void) {
    TEST_ASSERT_TRUE(sim_equal((mulcase){16, 16, 32, true, true}, NULL));
    TEST_ASSERT_TRUE(sim_equal((mulcase){9, 14, 23, true, true}, "acc_27.xml"));
}

static void test_x5_signed_12x12_is_hard_on_both(void) {
    pb_result res;
    build_mul((mulcase){12, 12, 24, true, true}, "acc_27.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    pb_free(&res);
}

/* PHASE2 #25: one signed operand is the unsigned path, on a record too. */
static void test_x5_mixed_is_unsigned(void) {
    TEST_ASSERT_TRUE(sim_equal((mulcase){12, 12, 24, true, false}, "acc_27.xml"));
    TEST_ASSERT_TRUE(sim_equal((mulcase){12, 12, 24, false, true}, NULL));
    pb_result res;
    build_mul((mulcase){20, 20, 40, true, false}, "acc_27.xml", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_partial_map_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply")); /* Y 40 > 27 is fine unsigned */
    pb_free(&res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_x1_9x9_on_27);
    RUN_TEST(test_x1_8x8_on_k6_is_36);
    RUN_TEST(test_x2_asymmetric_model_leaves_mul);
    RUN_TEST(test_x2_renamed_model_leaves_mul);
    RUN_TEST(test_x7_no_multiply_model_is_info);
    RUN_TEST(test_x3_min_mult_8);
    RUN_TEST(test_x3_default_is_3);
    RUN_TEST(test_x5_signed_16x16);
    RUN_TEST(test_x5_signed_12x12_sim);
    RUN_TEST(test_x5_signed_other_shapes_sim);
    RUN_TEST(test_x5_signed_12x12_is_hard_on_both);
    RUN_TEST(test_x5_mixed_is_unsigned);
    RUN_TEST(test_x6_explicit_20_padded_to_27);
    RUN_TEST(test_x6_explicit_27_untouched);
    RUN_TEST(test_x6_explicit_40_split_by_27);
    return UNITY_END();
}
