/*
 * test_fsm_ceval.c — the fsm pass's constant evaluator (4.8 plan Task 4; spec section 4): set,
 * push and pop, stop signals, the busy loop guard, the mux rules of consteval.h, evaluation
 * through the foldable cells with 4-state values (the fold table replayed), cells that are not
 * evaluable, the evaluation cap, and a mux chain 100,000 deep (no recursion).
 */
#include "ir/ids.h"
#include "ir/module.h"
#include "passes/fsm/ceval.h"
#include "passes/fsm/index.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_OPT_DATA
#error "ODIN3_OPT_DATA (tests/opt) must be defined by the build"
#endif

enum { MAX_BITS = 256, CHAIN_LEN = 100000, TEXT = 1 << 14, LINE = 1 << 12, MIN_ROWS = 1500 };

typedef struct fixture {
    pb_result res;
    odin3_fsm_index ix;
    odin3_fsm_budget budget;
    odin3_fsm_ceval *ce;
} fixture;

static fixture fx;

static void close_fixture(void) {
    odin3_fsm_ceval_destroy(fx.ce);
    odin3_fsm_index_free(&fx.ix);
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
}

void setUp(void) {
    memset(&fx, 0, sizeof fx);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    close_fixture();
}

static void open_fixture(const char *text) {
    close_fixture();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx.res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_index_build(fx.res.design, fx.res.module, &fx.ix));
    fx.budget.max = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ceval_create(&fx.ix, &fx.budget, &fx.ce));
}

/* The signal bits of wire `name`, LSB first; returns the width. */
static uint32_t wire_sig(const char *name, odin3_fsm_sig *out) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(fx.res.module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    uint32_t width = odin3_wire_width(fx.res.module, wire);
    for (uint32_t i = 0; i < width; i++) {
        out[i] = odin3_fsm_ix_sig(&fx.ix, odin3_wire_net(fx.res.module, wire, i).v);
    }
    return width;
}

/* Sets a wire from "name=value": the value MSB first in 0 1 x z. */
static void set_wire(const char *spec) {
    odin3_fsm_sig sig[MAX_BITS];
    uint8_t bits[MAX_BITS];
    char name[32];
    const char *value = strchr(spec, '=');
    TEST_ASSERT_NOT_NULL(value);
    (void)snprintf(name, sizeof name, "%.*s", (int)(value - spec), spec);
    value++;
    uint32_t width = wire_sig(name, sig);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(value), width);
    static const char k_chars[] = "01xz";
    for (uint32_t i = 0; i < width; i++) {
        bits[i] = (uint8_t)(strchr(k_chars, value[width - 1 - i]) - k_chars);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ceval_set(fx.ce, sig, bits, width));
}

/* Evaluates wire `name`; the result as a string (MSB first) with '?' for a bit not constant. */
static bool eval_wire(const char *name, char *out, odin3_fsm_ceval_res *res) {
    odin3_fsm_sig sig[MAX_BITS];
    uint32_t width = wire_sig(name, sig);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ceval_eval(fx.ce, sig, width, res));
    static const char k_chars[] = "01xz-m";
    for (uint32_t i = 0; i < width; i++) {
        char ch = '?';
        if (odin3_fsm_sig_is_const(sig[i])) {
            ch = k_chars[odin3_fsm_sig_bit(sig[i])];
        }
        out[width - 1 - i] = ch;
    }
    out[width] = '\0';
    return res->ok;
}

/* Evaluates "name=expected". */
static void expect_eval(const char *spec) {
    char name[32];
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    const char *want = strchr(spec, '=');
    TEST_ASSERT_NOT_NULL(want);
    (void)snprintf(name, sizeof name, "%.*s", (int)(want - spec), spec);
    TEST_ASSERT_TRUE_MESSAGE(eval_wire(name, got, &res), name);
    TEST_ASSERT_EQUAL_STRING(want + 1, got);
}

/* --- set, get, push, pop ------------------------------------------------------------------- */

#define HEAD "module \\m\n"

static void test_set_get_push_pop(void) {
    open_fixture(HEAD "  wire width 2 input 1 \\a\n  wire output 2 \\y\nend\n");
    odin3_fsm_sig sig[2];
    TEST_ASSERT_EQUAL_UINT32(2, wire_sig("a", sig));
    TEST_ASSERT_EQUAL_UINT32(sig[0], odin3_fsm_ceval_get(fx.ce, sig[0]));
    set_wire("a=10");
    TEST_ASSERT_EQUAL_UINT32(odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_0),
                             odin3_fsm_ceval_get(fx.ce, sig[0]));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ceval_push(fx.ce));
    set_wire("a=01");
    TEST_ASSERT_EQUAL_UINT32(odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_1),
                             odin3_fsm_ceval_get(fx.ce, sig[0]));
    odin3_fsm_ceval_pop(fx.ce);
    TEST_ASSERT_EQUAL_UINT32(odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_0),
                             odin3_fsm_ceval_get(fx.ce, sig[0]));
    TEST_ASSERT_EQUAL_UINT32(odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_1),
                             odin3_fsm_ceval_get(fx.ce, sig[1]));
}

static void test_set_skips_constants_and_marks(void) {
    open_fixture(HEAD "  wire input 1 \\a\n  wire output 2 \\y\nend\n");
    odin3_fsm_sig sig[2] = {odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_1), 0};
    wire_sig("a", &sig[1]);
    const uint8_t bits[2] = {ODIN3_FSM_BIT_0, ODIN3_FSM_BIT_DC};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ceval_set(fx.ce, sig, bits, 2));
    TEST_ASSERT_EQUAL_UINT32(sig[1], odin3_fsm_ceval_get(fx.ce, sig[1])); /* '-' sets nothing */
}

/* --- the cells ----------------------------------------------------------------------------- */

#define EQ_CELL(a_w, b_w)                                                                          \
    "  cell $eq \\e\n    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"                     \
    "    parameter \\A_WIDTH " a_w "\n    parameter \\B_WIDTH " b_w "\n"                           \
    "    parameter \\Y_WIDTH 1\n    connect \\A \\a\n    connect \\B \\b\n    connect \\Y \\y\n"   \
    "  end\n"

static const char *const k_eq = HEAD "  wire width 2 input 1 \\a\n  wire width 2 input 2 \\b\n"
                                     "  wire output 3 \\y\n" EQ_CELL("2", "2") "end\n";

static void test_eval_eq(void) {
    open_fixture(k_eq);
    set_wire("a=10");
    set_wire("b=10");
    expect_eval("y=1");
}

static void test_eval_eq_not_equal_and_x(void) {
    open_fixture(k_eq);
    set_wire("a=10");
    set_wire("b=11");
    expect_eval("y=0");
    close_fixture();
    open_fixture(k_eq);
    set_wire("a=1x");
    set_wire("b=11");
    expect_eval("y=x");
}

static void test_unset_input_has_no_driver(void) {
    open_fixture(k_eq);
    set_wire("a=10");
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    TEST_ASSERT_FALSE(eval_wire("y", got, &res));
    TEST_ASSERT_TRUE(res.has_undef);
    TEST_ASSERT_FALSE(res.capped);
    odin3_fsm_sig b_sig[2];
    wire_sig("b", b_sig);
    TEST_ASSERT_EQUAL_UINT32(b_sig[0], res.undef); /* the first bit nothing drives */
}

/* --- stop signals -------------------------------------------------------------------------- */

static const char *const k_stop =
    HEAD "  wire input 1 \\a\n  wire input 2 \\s\n  wire input 3 \\d\n  wire output 4 \\y\n"
         "  cell $mux \\m1\n    parameter \\WIDTH 1\n    connect \\A \\a\n    connect \\B \\d\n"
         "    connect \\S \\s\n    connect \\Y \\y\n  end\nend\n";

static void test_stop_signal_is_undef(void) {
    open_fixture(k_stop);
    odin3_fsm_sig sel[1];
    wire_sig("s", sel);
    odin3_fsm_ceval_stop(fx.ce, sel, 1);
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    TEST_ASSERT_FALSE(eval_wire("y", got, &res));
    TEST_ASSERT_TRUE(res.has_undef);
    TEST_ASSERT_EQUAL_UINT32(sel[0], res.undef);
    set_wire("s=0");
    set_wire("a=1");
    expect_eval("y=1"); /* a value beats a stop signal */
}

/* --- mux rules ----------------------------------------------------------------------------- */

static void test_mux_selects(void) {
    open_fixture(k_stop);
    set_wire("a=0");
    set_wire("d=1");
    set_wire("s=1");
    expect_eval("y=1");
    close_fixture();
    open_fixture(k_stop);
    set_wire("a=0");
    set_wire("d=1");
    set_wire("s=0");
    expect_eval("y=0");
}

static void test_mux_x_select_merges(void) {
    open_fixture(k_stop);
    set_wire("a=1");
    set_wire("d=1");
    set_wire("s=x");
    expect_eval("y=1"); /* equal candidates stay */
    close_fixture();
    open_fixture(k_stop);
    set_wire("a=0");
    set_wire("d=1");
    set_wire("s=x");
    expect_eval("y=x");
}

static void test_mux_z_select_takes_a(void) {
    open_fixture(k_stop);
    set_wire("a=0");
    set_wire("d=1");
    set_wire("s=z");
    expect_eval("y=0"); /* z is neither x nor 1: no select bit set, A */
}

static const char *const k_pmux =
    HEAD "  wire width 2 input 1 \\a\n  wire width 4 input 2 \\b\n  wire width 2 input 3 \\s\n"
         "  wire width 2 output 4 \\y\n"
         "  cell $pmux \\p\n    parameter \\WIDTH 2\n    parameter \\S_WIDTH 2\n"
         "    connect \\A \\a\n    connect \\B \\b\n    connect \\S \\s\n    connect \\Y \\y\n"
         "  end\nend\n";

static void test_pmux(void) {
    static const struct {
        const char *sel, *want;
    } k_rows[] = {{"00", "01"}, {"01", "11"}, {"10", "10"}, {"11", "1x"}, {"0x", "x1"}};
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) {
        open_fixture(k_pmux);
        set_wire("a=01");
        set_wire("b=1011"); /* B slice 0 = 11, slice 1 = 10 */
        char spec[16];
        (void)snprintf(spec, sizeof spec, "s=%s", k_rows[i].sel);
        set_wire(spec);
        (void)snprintf(spec, sizeof spec, "y=%s", k_rows[i].want);
        expect_eval(spec);
    }
}

/* --- sequential cells and loops -------------------------------------------------------------- */

static const char *const k_dff =
    HEAD "  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n  wire output 4 \\y\n"
         "  cell $dff \\f\n    parameter \\WIDTH 1\n    parameter \\CLK_POLARITY 1\n"
         "    connect \\CLK \\clk\n    connect \\D \\d\n    connect \\Q \\q\n  end\n"
         "  cell $logic_not \\n\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
         "    parameter \\Y_WIDTH 1\n    connect \\A \\q\n    connect \\Y \\y\n  end\nend\n";

static void test_dff_is_not_evaluable(void) {
    open_fixture(k_dff);
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    TEST_ASSERT_FALSE(eval_wire("y", got, &res));
    TEST_ASSERT_FALSE(res.has_undef); /* an evaluation error, as CellTypes::eval */
    TEST_ASSERT_FALSE(res.capped);
    set_wire("q=1"); /* a value on Q is enough: the flip-flop is never asked */
    expect_eval("y=0");
}

static const char *const k_loop =
    HEAD "  wire output 1 \\y\n  wire output 2 \\z\n"
         "  cell $logic_not \\n1\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
         "    parameter \\Y_WIDTH 1\n    connect \\A \\z\n    connect \\Y \\y\n  end\n"
         "  cell $logic_not \\n2\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
         "    parameter \\Y_WIDTH 1\n    connect \\A \\y\n    connect \\Y \\z\n  end\nend\n";

static void test_busy_loop_fails(void) {
    open_fixture(k_loop);
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    TEST_ASSERT_FALSE(eval_wire("y", got, &res));
    TEST_ASSERT_TRUE(res.has_undef); /* the busy guard reports the signal */
    TEST_ASSERT_FALSE(res.capped);
    /* the guard is released: a value breaks the loop and the same evaluator works again */
    set_wire("z=1");
    expect_eval("y=0");
}

/* --- the cap ------------------------------------------------------------------------------- */

static void build_chain(uint32_t length) {
    char head[256];
    (void)snprintf(head, sizeof head,
                   HEAD "  wire input 1 \\a\n  wire input 2 \\s\n  wire output 3 \\y\nend\n");
    open_fixture(head);
    odin3_celltype_id type = {0};
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr("$mux"), &name));
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, name, &type));
    odin3_net_id *nets = malloc(sizeof *nets * length);
    TEST_ASSERT_NOT_NULL(nets);
    odin3_fsm_sig tmp[1];
    wire_sig("y", tmp);
    for (uint32_t i = 0; i + 1U < length; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_net_create(fx.res.module, 0, (odin3_prov_id){0}, &nets[i]));
    }
    nets[length - 1U].v = tmp[0];
    odin3_fsm_sig a_sig[1];
    odin3_fsm_sig s_sig[1];
    wire_sig("a", a_sig);
    wire_sig("s", s_sig);
    const odin3_value param = odin3_value_int(1);
    for (uint32_t i = length; i-- > 0;) {
        const odin3_net_id data_a = i == 0 ? (odin3_net_id){a_sig[0]} : nets[i - 1U];
        const odin3_net_id data_b = {a_sig[0]};
        const odin3_net_id sel = {s_sig[0]};
        const odin3_netvec ports[4] = {{&data_a, 1}, {&data_b, 1}, {&sel, 1}, {&nets[i], 1}};
        const odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, &param, 1};
        odin3_node_id node = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_node_create_connected(fx.res.module, &spec, ports, &node));
    }
    free(nets);
    /* the index was built before the chain: rebuild it and the evaluator */
    odin3_fsm_ceval_destroy(fx.ce);
    odin3_fsm_index_free(&fx.ix);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_index_build(fx.res.design, fx.res.module, &fx.ix));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fsm_ceval_create(&fx.ix, &fx.budget, &fx.ce));
}

static void test_deep_chain_without_recursion(void) {
    build_chain(CHAIN_LEN);
    fx.budget.max = UINT64_MAX;
    set_wire("a=1");
    set_wire("s=0");
    expect_eval("y=1");
    TEST_ASSERT_EQUAL_UINT64(CHAIN_LEN, fx.budget.used);
}

static void test_cap(void) {
    build_chain(64);
    fx.budget.max = 10;
    set_wire("a=1");
    set_wire("s=0");
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    TEST_ASSERT_FALSE(eval_wire("y", got, &res));
    TEST_ASSERT_TRUE(res.capped);
    TEST_ASSERT_EQUAL_UINT64(11, fx.budget.used);
}

/* --- the fold table ------------------------------------------------------------------------ */

typedef struct row {
    char type[64], a_signed[8], b_signed[8], y_width[16];
    char op_a[LINE], op_b[LINE], op_s[LINE], result[LINE];
} row;

static void row_text(const row *rw, char *text, size_t size) {
    size_t la = strlen(rw->op_a);
    bool has_b = strcmp(rw->op_b, "-") != 0;
    int len = snprintf(text, size,
                       HEAD "  wire width %zu input 1 \\a\n  wire width %zu input 2 \\b\n"
                            "  wire width %s output 3 \\y\n  cell %s \\c\n"
                            "    parameter \\A_SIGNED %s\n    parameter \\A_WIDTH %zu\n"
                            "    parameter \\B_SIGNED %s\n    parameter \\B_WIDTH %zu\n"
                            "    parameter \\Y_WIDTH %s\n    connect \\A \\a\n    connect \\B \\b\n"
                            "    connect \\Y \\y\n  end\nend\n",
                       la, has_b ? strlen(rw->op_b) : 1U, rw->y_width, rw->type, rw->a_signed, la,
                       rw->b_signed, has_b ? strlen(rw->op_b) : 1U, rw->y_width);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < size);
}

/* The unary types have no B port: build a variant. */
static void row_text_unary(const row *rw, char *text, size_t size) {
    int len = snprintf(text, size,
                       HEAD "  wire width %zu input 1 \\a\n  wire width %s output 3 \\y\n"
                            "  cell %s \\c\n    parameter \\A_SIGNED %s\n"
                            "    parameter \\A_WIDTH %zu\n    parameter \\Y_WIDTH %s\n"
                            "    connect \\A \\a\n    connect \\Y \\y\n  end\nend\n",
                       strlen(rw->op_a), rw->y_width, rw->type, rw->a_signed, strlen(rw->op_a),
                       rw->y_width);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < size);
}

/* Rows the ceval does not replay: the muxes have their own rules, and a mixed-sign cell is read
 * unsigned by the registry (spec 17 #57) while the table keeps Yosys's per-operand reading. */
static bool replayed(const row *rw) {
    if (strcmp(rw->type, "$mux") == 0 || strcmp(rw->type, "$pmux") == 0) {
        return false;
    }
    bool has_b = strcmp(rw->op_b, "-") != 0;
    bool shift = strncmp(rw->type, "$sh", 3) == 0 || strcmp(rw->type, "$sshr") == 0 ||
                 strcmp(rw->type, "$pow") == 0;
    return !has_b || shift || strcmp(rw->a_signed, rw->b_signed) == 0;
}

static bool replay_row(const row *rw) {
    static char text[TEXT];
    if (strcmp(rw->op_b, "-") == 0) {
        row_text_unary(rw, text, sizeof text);
    } else {
        row_text(rw, text, sizeof text);
    }
    open_fixture(text);
    static char spec[LINE + 4];
    (void)snprintf(spec, sizeof spec, "a=%s", rw->op_a);
    set_wire(spec);
    if (strcmp(rw->op_b, "-") != 0) {
        (void)snprintf(spec, sizeof spec, "b=%s", rw->op_b);
        set_wire(spec);
    }
    char got[MAX_BITS + 1];
    odin3_fsm_ceval_res res;
    bool ok = eval_wire("y", got, &res) && strcmp(got, rw->result) == 0;
    if (!ok) {
        printf("MISMATCH %s a=%s b=%s want %s got %s\n", rw->type, rw->op_a, rw->op_b, rw->result,
               got);
    }
    return ok;
}

static void test_fold_table_replay(void) {
    FILE *file = fopen(ODIN3_OPT_DATA "/fold_table.txt", "r");
    TEST_ASSERT_NOT_NULL(file);
    char line[LINE * 4];
    static row rw;
    uint32_t cases = 0;
    uint32_t wrong = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        if (line[0] == '#') {
            continue;
        }
        int got =
            sscanf(line, "%63s %7s %7s %15s %4095s %4095s %4095s %4095s", rw.type, rw.a_signed,
                   rw.b_signed, rw.y_width, rw.op_a, rw.op_b, rw.op_s, rw.result);
        TEST_ASSERT_EQUAL_INT(8, got);
        if (replayed(&rw) && strlen(rw.op_a) < MAX_BITS && strlen(rw.op_b) < MAX_BITS &&
            strlen(rw.result) < MAX_BITS) {
            cases++;
            wrong += replay_row(&rw) ? 0 : 1;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_UINT32(0, wrong);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(MIN_ROWS, cases);
}

/* --- out of memory ------------------------------------------------------------------------- */

static void test_oom(void) {
    open_fixture(k_eq);
    set_wire("a=10");
    set_wire("b=10");
    odin3_fsm_sig sig[1];
    wire_sig("y", sig);
    long failures = 0;
    for (long after = 0; after < 60; after++) {
        odin3_fsm_sig probe = sig[0];
        odin3_fsm_ceval_res res;
        odin3_util_set_alloc_fail_after(after);
        odin3_status st = odin3_fsm_ceval_eval(fx.ce, &probe, 1, &res);
        odin3_util_set_alloc_fail_after(-1);
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
        failures += st == ODIN3_OK ? 0 : 1;
    }
    TEST_ASSERT_TRUE(failures >= 1);
    expect_eval("y=1"); /* the evaluator is still usable */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_get_push_pop);
    RUN_TEST(test_set_skips_constants_and_marks);
    RUN_TEST(test_eval_eq);
    RUN_TEST(test_eval_eq_not_equal_and_x);
    RUN_TEST(test_unset_input_has_no_driver);
    RUN_TEST(test_stop_signal_is_undef);
    RUN_TEST(test_mux_selects);
    RUN_TEST(test_mux_x_select_merges);
    RUN_TEST(test_mux_z_select_takes_a);
    RUN_TEST(test_pmux);
    RUN_TEST(test_dff_is_not_evaluable);
    RUN_TEST(test_busy_loop_fails);
    RUN_TEST(test_deep_chain_without_recursion);
    RUN_TEST(test_cap);
    RUN_TEST(test_fold_table_replay);
    RUN_TEST(test_oom);
    return UNITY_END();
}
