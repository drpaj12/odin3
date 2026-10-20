/*
 * test_hardmap_adder.c — hardmap, PR 3C-2: the `$add` / `$sub` rule (3C spec A-1..A-9, H-7, H-9;
 * plan Task 3 Step 1). Every case is flat RTLIL with lib/vtr.o3lib read (pb_build_lib); hm_run
 * checks the IR (FULL) before and after, and the value-preserving cases simulate before and after
 * on every input vector (hm_sim_exhaustive).
 */
#include "hardmap_fixture.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hardmap.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 16384,
    OOM_LIMIT = 6000,
    OOM_QUIET = 4,
    MAX_CELLS = 8,
    MAX_WIDTH = 4,
    SIGN_COMBOS = 4,
    P_A = 0,
    P_B = 1,
    P_CIN = 2,
    P_COUT = 3,
    P_SUM = 4,
    SX_VECTORS = 4096
};

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

/* One `$add` / `$sub` cell. a_sig / b_sig: NULL is the input wire a<i> / b<i> of that cell. */
typedef struct addcase {
    const char *type;
    uint32_t a_width;
    uint32_t b_width;
    uint32_t y_width;
    bool a_signed;
    bool b_signed;
    const char *a_sig;
    const char *b_sig;
} addcase;

#define ADD "$add"
#define SUB "$sub"

static void put(odin3_strbuf *out, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static void put(odin3_strbuf *out, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(buf)));
}

/* The module `name` with cells u0 .. u<n-1>, each on its own wires a<i>, b<i>, y<i>. */
static void add_module(odin3_strbuf *out, const char *name, const addcase *cases, size_t count) {
    put(out, "module \\%s\n", name);
    uint32_t port = 1;
    for (size_t i = 0; i < count; i++) {
        const addcase *ac = &cases[i];
        if (ac->a_sig == NULL) {
            put(out, "  wire width %" PRIu32 " input %" PRIu32 " \\a%zu\n", ac->a_width, port++, i);
        }
        if (ac->b_sig == NULL) {
            put(out, "  wire width %" PRIu32 " input %" PRIu32 " \\b%zu\n", ac->b_width, port++, i);
        }
        put(out, "  wire width %" PRIu32 " output %" PRIu32 " \\y%zu\n", ac->y_width, port++, i);
    }
    for (size_t i = 0; i < count; i++) {
        const addcase *ac = &cases[i];
        char asig[64];
        char bsig[64];
        (void)snprintf(asig, sizeof asig, "\\a%zu", i);
        (void)snprintf(bsig, sizeof bsig, "\\b%zu", i);
        put(out,
            "  cell %s \\u%zu\n    parameter \\A_SIGNED %d\n    parameter \\B_SIGNED %d\n"
            "    parameter \\A_WIDTH %" PRIu32 "\n    parameter \\B_WIDTH %" PRIu32
            "\n    parameter \\Y_WIDTH %" PRIu32
            "\n    connect \\A %s\n    connect \\B %s\n    connect \\Y \\y%zu\n  end\n",
            ac->type, i, ac->a_signed ? 1 : 0, ac->b_signed ? 1 : 0, ac->a_width, ac->b_width,
            ac->y_width, ac->a_sig ? ac->a_sig : asig, ac->b_sig ? ac->b_sig : bsig, i);
    }
    put(out, "end\n");
}

static void build_cases(const addcase *cases, size_t count, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    add_module(&text, "m", cases, count);
    hm_build_ok(text.data, res);
    odin3_strbuf_free(&text);
}

static void build_one(addcase ac, pb_result *res) {
    build_cases(&ac, 1, res);
}

static odin3_net_id wire_net(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(res->module, wire, bit);
}

static odin3_net_id port_net(const pb_result *res, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(res->module, node, port);
    TEST_ASSERT_EQUAL_UINT32(1, pins.count);
    return odin3_pin_net(res->module, pins.first);
}

static bool is_const(const pb_result *res, odin3_net_id net, odin3_const value) {
    return odin3_net_valid(net) && odin3_net_const_value(res->module, net) == value;
}

static bool unread(const pb_result *res, odin3_net_id net) {
    return odin3_net_pins(res->module, net).count == 1; /* its driver only */
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

/* The simulation builders: one global case (hm_build_fn takes no argument). */
static addcase g_case;

static odin3_status build_g(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    add_module(&text, "m", &g_case, 1);
    odin3_status st = hm_build(text.data, out);
    odin3_strbuf_free(&text);
    return st == ODIN3_OK ? odin3_design_set_top(out->design, odin3_module_id_of(out->module)) : st;
}

static bool sim_exhaustive(addcase ac) {
    g_case = ac;
    return hm_sim_exhaustive(build_g);
}

static void expect_sim(addcase ac) {
    TEST_ASSERT_TRUE(sim_exhaustive(ac));
}

/* --- H-3 ----------------------------------------------------------------------------------- */

static void test_h3_missing_type(void) {
    pb_result res;
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    addcase ac = {ADD, 4, 4, 5, false, false, NULL, NULL};
    add_module(&text, "m", &ac, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text.data, &res));
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hardmap_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text,
               "partial_map: cell type 'adder' is not registered; read_techlib lib/vtr.o3lib first"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$add"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* --- A-3: the chain length ----------------------------------------------------------------- */

static void test_a3_chain_len(void) {
    static const struct {
        uint32_t a, b, adders;
    } rows[] = {{1, 1, 3}, {8, 8, 10}, {3, 6, 8}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        pb_result res;
        odin3_hardmap_stats stats;
        build_one((addcase){ADD, rows[i].a, rows[i].b,
                            1 + (rows[i].a > rows[i].b ? rows[i].a : rows[i].b), false, false, NULL,
                            NULL},
                  &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
        TEST_ASSERT_EQUAL_UINT32(rows[i].adders, hm_count(res.module, "adder"));
        TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$add"));
        TEST_ASSERT_EQUAL_UINT64(rows[i].adders, stats.adder);
        TEST_ASSERT_EQUAL_UINT64(1, stats.chains);
        pb_free(&res);
    }
}

/* Y_WIDTH 1: the count does not change; only Y[0] is connected, to stage 0's sumout. */
static void test_a3_y_width_1(void) {
    pb_result res;
    build_one((addcase){ADD, 4, 4, 1, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT32(wire_net(&res, "y0", 0).v,
                             port_net(&res, hm_nth(res.module, "adder", 1), P_SUM).v);
    for (uint32_t i = 2; i < 6; i++) {
        TEST_ASSERT_TRUE(unread(&res, port_net(&res, hm_nth(res.module, "adder", i), P_SUM)));
    }
    pb_free(&res);
}

/* --- pin helpers -------------------------------------------------------------------------- */

/* One pin of the idx-th adder (node-ID order): head 0, stage k is k + 1. */
typedef struct adder_pin_at {
    uint32_t adder;
    uint32_t port;
} adder_pin_at;

static odin3_net_id adder_pin(const pb_result *res, adder_pin_at at) {
    return port_net(res, hm_nth(res->module, "adder", at.adder), at.port);
}

static void expect_same(odin3_net_id got, odin3_net_id want) {
    TEST_ASSERT_EQUAL_UINT32(want.v, got.v);
}

static void expect_const(const pb_result *res, odin3_net_id net, odin3_const value) {
    TEST_ASSERT_TRUE(is_const(res, net, value));
}

static void expect_unread(const pb_result *res, odin3_net_id net) {
    TEST_ASSERT_TRUE(unread(res, net));
}

/* --- A-4: `$add` pins ---------------------------------------------------------------------- */

/* Stage `bit` of unsigned 4 + 4 into Y 5: wires below 4, gnd at the top; cin is the previous
 * cout; Y[bit] is its sumout. */
static void check_add_stage(const pb_result *res, uint32_t bit) {
    uint32_t idx = bit + 1;
    if (bit < 4) {
        expect_same(adder_pin(res, (adder_pin_at){idx, P_A}), wire_net(res, "a0", bit));
        expect_same(adder_pin(res, (adder_pin_at){idx, P_B}), wire_net(res, "b0", bit));
    } else { /* the top stage of an unsigned cell: a = b = gnd */
        expect_const(res, adder_pin(res, (adder_pin_at){idx, P_A}), ODIN3_CONST_0);
        expect_const(res, adder_pin(res, (adder_pin_at){idx, P_B}), ODIN3_CONST_0);
    }
    expect_same(adder_pin(res, (adder_pin_at){idx, P_CIN}),
                adder_pin(res, (adder_pin_at){idx - 1, P_COUT}));
    expect_same(adder_pin(res, (adder_pin_at){idx, P_SUM}), wire_net(res, "y0", bit));
}

static void test_a4_add_pins(void) {
    pb_result res;
    build_one((addcase){ADD, 4, 4, 5, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    expect_const(&res, adder_pin(&res, (adder_pin_at){0, P_A}), ODIN3_CONST_1);
    expect_const(&res, adder_pin(&res, (adder_pin_at){0, P_B}), ODIN3_CONST_0);
    expect_const(&res, adder_pin(&res, (adder_pin_at){0, P_CIN}), ODIN3_CONST_0);
    expect_unread(&res, adder_pin(&res, (adder_pin_at){0, P_SUM}));
    for (uint32_t bit = 0; bit < 5; bit++) {
        check_add_stage(&res, bit);
    }
    expect_unread(&res, adder_pin(&res, (adder_pin_at){5, P_COUT}));
    pb_free(&res);
}

/* --- A-2: extension ------------------------------------------------------------------------ */

static void test_a2_ext(void) {
    pb_result res;
    build_one((addcase){ADD, 3, 6, 7, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    for (uint32_t bit = 3; bit < 6; bit++) { /* stage bit is adder bit + 1 */
        expect_const(&res, adder_pin(&res, (adder_pin_at){bit + 1, P_A}), ODIN3_CONST_0);
    }
    pb_free(&res);
    build_one((addcase){ADD, 3, 6, 7, true, true, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    for (uint32_t bit = 0; bit <= 6; bit++) { /* the top stage too: ext(A)[6] is the sign */
        uint32_t src = bit < 3 ? bit : 2;
        expect_same(adder_pin(&res, (adder_pin_at){bit + 1, P_A}), wire_net(&res, "a0", src));
    }
    pb_free(&res);
}

/* --- A-5: `$sub` pins ---------------------------------------------------------------------- */

/* The `$not` whose input is net: its output net, none when there is none. */
static odin3_net_id not_of(const pb_result *res, odin3_net_id net) {
    for (uint32_t id = 1; id < odin3_module_node_end(res->module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(res->module, node) &&
            strcmp(odin3_celltype_get(res->design, odin3_node_type(res->module, node))->name,
                   "$not") == 0 &&
            odin3_pin_net(res->module, odin3_node_port(res->module, node, 0).first).v == net.v) {
            return odin3_pin_net(res->module, odin3_node_port(res->module, node, 1).first);
        }
    }
    return (odin3_net_id){0};
}

/* Stage `bit` of unsigned 8 - 5 into Y 9: b is the inverter of b0[bit] below 5, else vcc (no
 * inverter: ~gnd folds); a is a0[bit] below 8, gnd at the top. */
static void check_sub_stage(const pb_result *res, uint32_t bit) {
    odin3_net_id inb = adder_pin(res, (adder_pin_at){bit + 1, P_B});
    odin3_net_id ina = adder_pin(res, (adder_pin_at){bit + 1, P_A});
    if (bit < 5) {
        expect_same(inb, not_of(res, wire_net(res, "b0", bit)));
    } else {
        expect_const(res, inb, ODIN3_CONST_1);
    }
    if (bit < 8) {
        expect_same(ina, wire_net(res, "a0", bit));
    } else {
        expect_const(res, ina, ODIN3_CONST_0);
    }
}

static void test_a5_sub_pins(void) {
    pb_result res;
    build_one((addcase){SUB, 8, 5, 9, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(10, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT32(5, hm_count(res.module, "$not"));
    expect_const(&res, adder_pin(&res, (adder_pin_at){0, P_A}), ODIN3_CONST_1);
    expect_const(&res, adder_pin(&res, (adder_pin_at){0, P_B}), ODIN3_CONST_1);
    expect_const(&res, adder_pin(&res, (adder_pin_at){0, P_CIN}), ODIN3_CONST_X); /* the pad */
    for (uint32_t bit = 0; bit <= 8; bit++) {
        check_sub_stage(&res, bit);
    }
    pb_free(&res);
}

/* A constant B folds: no `$not`; bit i of 4'1010 is 0 at i = 0, 2, so b = vcc there. */
static void test_a5_const_b_folds(void) {
    pb_result res;
    build_one((addcase){SUB, 4, 4, 5, false, false, NULL, "4'1010"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$not"));
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "adder"));
    static const odin3_const want[4] = {ODIN3_CONST_1, ODIN3_CONST_0, ODIN3_CONST_1, ODIN3_CONST_0};
    for (uint32_t bit = 0; bit < 4; bit++) {
        expect_const(&res, adder_pin(&res, (adder_pin_at){bit + 1, P_B}), want[bit]);
    }
    pb_free(&res);
    expect_sim((addcase){SUB, 4, 4, 5, false, false, NULL, "4'1010"});
    expect_sim((addcase){ADD, 4, 4, 5, true, true, NULL, "4'1010"});
}

/* A repeated sign bit gets one `$not`: signed 5 - 2 into Y 6 inverts b[0], b[1] only. */
static void test_a5_shared_sign_not(void) {
    pb_result res;
    build_one((addcase){SUB, 5, 2, 6, true, true, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "$not"));
    odin3_net_id sign_inv = not_of(&res, wire_net(&res, "b0", 1));
    TEST_ASSERT_TRUE(odin3_net_valid(sign_inv));
    for (uint32_t bit = 1; bit <= 5; bit++) {
        expect_same(adder_pin(&res, (adder_pin_at){bit + 1, P_B}), sign_inv);
    }
    pb_free(&res);
    expect_sim((addcase){SUB, 5, 2, 6, true, true, NULL, NULL});
}

/* --- A-8: the borrow bit ------------------------------------------------------------------- */

/* Unsigned 8-bit ua - ub into 9 bits, every input pair: Y[8] is the borrow (1 when ua < ub), the
 * case Parmys loses (Task 1 probe subwide). */
static void test_a8_sub_top_stage(void) {
    expect_sim((addcase){SUB, 8, 8, 9, false, false, NULL, NULL});
    expect_sim((addcase){SUB, 8, 5, 9, false, false, NULL, NULL});
    expect_sim((addcase){SUB, 5, 8, 9, false, false, NULL, NULL});
}

/* --- A-6 (D1): sign extension -------------------------------------------------------------- */

/* The sx_add cell after 3G's wreduce: signed A 9, B 9, Y 10 (n + 1; 3G caps Y there) on 4096
 * random vectors, and a 5-bit operand against a 9-bit one exhaustively. Parmys loses the sign at
 * the bits above the narrow operand (D1). */
static void test_a6_sx_add(void) {
    g_case = (addcase){ADD, 9, 9, 10, true, true, NULL, NULL};
    TEST_ASSERT_TRUE(hm_sim_equal(build_g, SX_VECTORS, 1));
    expect_sim((addcase){ADD, 9, 5, 10, true, true, NULL, NULL});
    expect_sim((addcase){SUB, 5, 9, 10, true, true, NULL, NULL});
}

/* --- A-7: Y wider than the chain ----------------------------------------------------------- */

/* Y_WIDTH above n + 1 stays soft with a warning (aliasing the extra bits would allocate after the
 * delete, H-9): the cell and the text are untouched. */
static void check_y_wider(addcase ac, const char *type) {
    pb_result res;
    log_text[0] = '\0';
    build_one(ac, &res);
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, type));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_NOT_NULL(strstr(log_text, "above max(A_WIDTH, B_WIDTH) + 1 (5); left soft"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "run wreduce first"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    pb_free(&res);
}

static void test_a7_y_wider_soft(void) {
    check_y_wider((addcase){ADD, 4, 4, 6, false, false, NULL, NULL}, "$add");
    check_y_wider((addcase){SUB, 4, 3, 7, false, false, NULL, NULL}, "$sub");
    check_y_wider((addcase){ADD, 4, 4, 11, true, true, NULL, NULL}, "$add");
}

/* --- exhaustive matrix --------------------------------------------------------------------- */

/* One (A_WIDTH, B_WIDTH) point of the matrix: signs x Y_WIDTH in {1, n, n + 1, n + 2}. */
typedef struct matrix_point {
    const char *type;
    uint32_t awidth;
    uint32_t bwidth;
} matrix_point;

static void matrix_point_check(matrix_point pt) {
    uint32_t wide = pt.awidth > pt.bwidth ? pt.awidth : pt.bwidth;
    const uint32_t ys[4] = {1, wide, wide + 1, wide + 2};
    for (uint32_t signs = 0; signs < SIGN_COMBOS; signs++) {
        for (uint32_t yi = 0; yi < 4; yi++) {
            addcase ac = {pt.type,           pt.awidth,         pt.bwidth, ys[yi],
                          (signs & 1U) != 0, (signs & 2U) != 0, NULL,      NULL};
            TEST_ASSERT_TRUE_MESSAGE(sim_exhaustive(ac), pt.type);
        }
    }
}

static void test_adder_sim_matrix(void) {
    static const char *const types[2] = {ADD, SUB};
    for (uint32_t ti = 0; ti < 2; ti++) {
        for (uint32_t awidth = 1; awidth <= MAX_WIDTH; awidth++) {
            for (uint32_t bwidth = 1; bwidth <= MAX_WIDTH; bwidth++) {
                matrix_point_check((matrix_point){types[ti], awidth, bwidth});
            }
        }
    }
}

/* --- A-1: candidates; A-9: explicit instances --------------------------------------------- */

static void test_a1_const_operand_goes_hard(void) {
    pb_result res;
    build_one((addcase){ADD, 4, 4, 5, false, false, NULL, "4'0011"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$add"));
    pb_free(&res);
}

/* `$neg` and `$lt` are not candidates (S-1); an `$add` beside them still maps. */
static void test_a1_other_cells_untouched(void) {
    static const char *text =
        "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 5 output 3 \\n\n  wire output 4 \\l\n  wire width 5 output 5 \\s\n"
        "  cell $neg \\ng\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 4\n"
        "    parameter \\Y_WIDTH 5\n    connect \\A \\a\n    connect \\Y \\n\n  end\n"
        "  cell $lt \\lt\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 1\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\l\n  end\n"
        "  cell $add \\ad\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 5\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\s\n  end\nend\n";
    pb_result res;
    hm_build_ok(text, &res);
    odin3_node_id neg = hm_nth(res.module, "$neg", 0);
    odin3_node_id lt = hm_nth(res.module, "$lt", 0);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(neg.v, hm_nth(res.module, "$neg", 0).v);
    TEST_ASSERT_EQUAL_UINT32(lt.v, hm_nth(res.module, "$lt", 0).v);
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT64(0, stats.soft);
    pb_free(&res);
}

static void test_a9_explicit_adder_untouched(void) {
    static const char *text =
        "module \\m\n  wire input 1 \\x\n  wire input 2 \\y\n  wire input 3 \\c\n"
        "  wire output 4 \\o\n  wire output 5 \\s\n  cell \\adder \\u\n"
        "    connect \\a \\x\n    connect \\b \\y\n    connect \\cin \\c\n"
        "    connect \\cout \\o\n    connect \\sumout \\s\n  end\nend\n";
    pb_result res;
    hm_build_ok(text, &res);
    odin3_node_id before = hm_nth(res.module, "adder", 0);
    odin3_strbuf now;
    odin3_strbuf_init(&now);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &now));
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(before.v, hm_nth(res.module, "adder", 0).v);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "adder"));
    TEST_ASSERT_EQUAL_UINT64(0, stats.adder);
    assert_rtlil_equal(&res, &now);
    odin3_strbuf_free(&now);
    pb_free(&res);
}

/* --- aliased operands ---------------------------------------------------------------------- */

static void test_a_alias_operands(void) {
    expect_sim((addcase){ADD, 4, 4, 5, false, false, NULL, "\\a0"});
    expect_sim((addcase){SUB, 4, 4, 5, false, false, NULL, "\\a0"});
    expect_sim((addcase){ADD, 4, 4, 5, true, true, NULL, "\\a0"});
    expect_sim((addcase){SUB, 4, 4, 5, true, true, NULL, "\\a0"});
}

/* B is a's sign bit three times (x + x with a shared sign net), or repeats a bit of its own: one
 * `$not` serves the repeats. */
static void test_a_alias_repeated_bits(void) {
    const char *sign3 = "{ \\a0 [2] \\a0 [2] \\a0 [2] }";
    const char *rep = "{ \\a0 [0] \\a0 [0] \\a0 [1] }";
    expect_sim((addcase){ADD, 3, 3, 4, true, true, NULL, sign3});
    expect_sim((addcase){SUB, 3, 3, 4, true, true, NULL, sign3});
    expect_sim((addcase){SUB, 3, 3, 4, false, false, NULL, rep});
    pb_result res;
    build_one((addcase){SUB, 3, 3, 4, false, false, NULL, rep}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "$not"));
    pb_free(&res);
}

/* --- names, attributes, provenance, determinism, info ------------------------------------- */

static const char *attr_text(const pb_result *res, odin3_node_id node) {
    uint32_t key = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res->design, odin3_bytes_cstr("hardmap"), &key));
    const odin3_value *attr =
        odin3_attr_get(res->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key);
    TEST_ASSERT_NOT_NULL(attr);
    return odin3_strtab_get(odin3_design_strtab(res->design), attr->str);
}

static void test_names_and_attributes(void) {
    pb_result res;
    build_one((addcase){SUB, 3, 3, 4, false, false, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id head = hm_nth(res.module, "adder", 0);
    TEST_ASSERT_EQUAL_STRING("u0", node_name_of(&res, head)); /* the first node takes the name */
    TEST_ASSERT_EQUAL_STRING("adder:head", attr_text(&res, head));
    static const char *const names[4] = {"u0.s0", "u0.s1", "u0.s2", "u0.top"};
    static const char *const attrs[4] = {"adder:s0", "adder:s1", "adder:s2", "adder:top"};
    for (uint32_t i = 0; i < 4; i++) {
        odin3_node_id node = hm_nth(res.module, "adder", i + 1);
        TEST_ASSERT_EQUAL_STRING(names[i], node_name_of(&res, node));
        TEST_ASSERT_EQUAL_STRING(attrs[i], attr_text(&res, node));
    }
    odin3_node_id inv = hm_nth(res.module, "$not", 1);
    TEST_ASSERT_EQUAL_STRING("u0.nb1", node_name_of(&res, inv));
    TEST_ASSERT_EQUAL_STRING("$not:nb1", attr_text(&res, inv));
    pb_free(&res);
}

/* Order: cells map in node-ID order, so u0 comes before u1 whatever their type. */
static const addcase k_three[3] = {{ADD, 4, 4, 5, false, false, NULL, NULL},
                                   {SUB, 3, 5, 6, true, true, NULL, NULL},
                                   {SUB, 6, 2, 7, false, false, NULL, NULL}};

static bool is_type_named(const pb_result *res, odin3_node_id node, const char *type) {
    return odin3_node_live(res->module, node) &&
           strcmp(odin3_celltype_get(res->design, odin3_node_type(res->module, node))->name,
                  type) == 0;
}

/* Checks the record of a chain node and returns the (1-based) operation it belongs to. */
static uint32_t chain_node_op(const pb_result *res, odin3_node_id node,
                              const odin3_prov_id *source) {
    const odin3_prov_record *rec = odin3_prov_get(res->design, odin3_node_prov(res->module, node));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_TRUE(rec->op >= 1 && rec->op <= 3); /* one operation per source cell */
    TEST_ASSERT_EQUAL_UINT32(source[rec->op - 1].v, rec->parents.ids[0].v);
    return rec->op;
}

static void test_h7_prov_chain(void) {
    pb_result res;
    build_cases(k_three, 3, &res);
    odin3_prov_id source[3];
    source[0] = odin3_node_prov(res.module, hm_nth(res.module, "$add", 0));
    source[1] = odin3_node_prov(res.module, hm_nth(res.module, "$sub", 0));
    source[2] = odin3_node_prov(res.module, hm_nth(res.module, "$sub", 1));
    TEST_ASSERT_TRUE(source[0].v != 0 && source[1].v != 0 && source[2].v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    uint32_t seen[3] = {0, 0, 0};
    for (uint32_t id = 1; id < odin3_module_node_end(res.module); id++) {
        odin3_node_id node = {id};
        if (is_type_named(&res, node, "adder") || is_type_named(&res, node, "$not")) {
            seen[chain_node_op(&res, node, source) - 1]++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(6, seen[0]);     /* 4 + 4: 6 adders */
    TEST_ASSERT_EQUAL_UINT32(7 + 5, seen[1]); /* signed 3 - 5: 7 adders, 5 inverters */
    TEST_ASSERT_EQUAL_UINT32(8 + 2, seen[2]); /* unsigned 6 - 2: 8 adders, 2 inverters */
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
    const addcase cases[3] = {{ADD, 4, 4, 5, false, false, NULL, NULL},
                              {SUB, 2, 2, 3, false, false, NULL, NULL},
                              {ADD, 4, 4, 9, false, false, NULL, NULL}};
    build_cases(cases, 3, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_NOT_NULL(strstr(log_text,
                                "partial_map: 0 multiply, 10 adder in 2 chains, 0 single_port_ram + 0 "
                                "dual_port_ram slices; 1 cells left soft"));
    TEST_ASSERT_EQUAL_UINT64(10, stats.adder);
    TEST_ASSERT_EQUAL_UINT64(2, stats.chains);
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    pb_free(&res);
}

static void test_hardmap_twice(void) {
    pb_result res;
    build_cases(k_three, 3, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(3, stats.chains);
    odin3_strbuf once;
    odin3_strbuf_init(&once);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &once));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.adder);
    TEST_ASSERT_EQUAL_UINT64(0, stats.chains);
    assert_rtlil_equal(&res, &once);
    odin3_strbuf_free(&once);
    pb_free(&res);
}

/* `$mul` and `$add` in one module: the `$mul` phase runs first (H-8), both map. */
static void test_mul_and_add_together(void) {
    static const char *text =
        "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
        "  wire width 8 output 3 \\p\n  wire width 5 output 4 \\s\n"
        "  cell $add \\ad\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 5\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\s\n  end\n"
        "  cell $mul \\mu\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
        "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n    parameter \\Y_WIDTH 8\n"
        "    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\p\n  end\nend\n";
    pb_result res;
    hm_build_ok(text, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "multiply"));
    TEST_ASSERT_EQUAL_UINT32(6, hm_count(res.module, "adder"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "partial_map: 1 multiply, 6 adder in 1 chains"));
    pb_free(&res);
}

/* --- H-9: out of memory -------------------------------------------------------------------- */

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

typedef struct oom_case {
    addcase ac;
    uint32_t adders; /* what the finished run leaves */
} oom_case;

static bool oom_try(const oom_case *oc, long fail_at) {
    pb_result res;
    snapshot before;
    build_one(oc->ac, &res);
    snap_take(&res, &before);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
    odin3_util_set_alloc_fail_after(-1);
    bool injected = st != ODIN3_OK;
    if (injected) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, oc->ac.type)); /* the source is intact */
        TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "adder"));
        snap_expect(&res, &before); /* and nothing else changed: no fresh net, no constant */
    } else {
        TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, oc->ac.type));
        TEST_ASSERT_EQUAL_UINT32(oc->adders, hm_count(res.module, "adder"));
    }
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
    odin3_strbuf_free(&before.text);
    pb_free(&res);
    return injected;
}

/* Fails the n-th allocation for n = 0, 1 ... until OOM_QUIET runs in a row succeed; returns the
 * number of runs failed by injection. */
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
    /* `$add`; signed `$sub` with `$not`s; unsigned `$sub` (head pad, gnd/vcc); constant B (the
     * constant nodes are made inside the transaction) */
    const oom_case cases[] = {{{ADD, 4, 4, 5, false, false, NULL, NULL}, 6},
                              {{SUB, 3, 2, 4, true, true, NULL, NULL}, 5},
                              {{SUB, 5, 3, 6, false, false, NULL, NULL}, 7},
                              {{SUB, 3, 3, 4, false, false, NULL, "3'101"}, 5},
                              {{ADD, 1, 1, 2, false, false, NULL, NULL}, 3}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_TRUE(oom_sweep(&cases[i]) > 1);
    }
}

/* Y above the chain: the cell stays soft, so no allocation fails the run. */
static void test_h9_oom_y_wider(void) {
    pb_result res;
    snapshot before;
    build_one((addcase){ADD, 4, 4, 9, true, true, NULL, NULL}, &res);
    snap_take(&res, &before);
    for (long fail_at = 0; fail_at < MAX_CELLS; fail_at++) {
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$add"));
        snap_expect(&res, &before);
    }
    odin3_strbuf_free(&before.text);
    pb_free(&res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_h3_missing_type);
    RUN_TEST(test_a3_chain_len);
    RUN_TEST(test_a3_y_width_1);
    RUN_TEST(test_a4_add_pins);
    RUN_TEST(test_a2_ext);
    RUN_TEST(test_a5_sub_pins);
    RUN_TEST(test_a5_const_b_folds);
    RUN_TEST(test_a5_shared_sign_not);
    RUN_TEST(test_a8_sub_top_stage);
    RUN_TEST(test_a6_sx_add);
    RUN_TEST(test_a7_y_wider_soft);
    RUN_TEST(test_adder_sim_matrix);
    RUN_TEST(test_a1_const_operand_goes_hard);
    RUN_TEST(test_a1_other_cells_untouched);
    RUN_TEST(test_a9_explicit_adder_untouched);
    RUN_TEST(test_a_alias_operands);
    RUN_TEST(test_a_alias_repeated_bits);
    RUN_TEST(test_names_and_attributes);
    RUN_TEST(test_h7_prov_chain);
    RUN_TEST(test_h8_determinism);
    RUN_TEST(test_h10_info);
    RUN_TEST(test_hardmap_twice);
    RUN_TEST(test_mul_and_add_together);
    RUN_TEST(test_h9_oom);
    RUN_TEST(test_h9_oom_y_wider);
    return UNITY_END();
}
