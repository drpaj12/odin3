/*
 * test_opt_fold.c — constant folding (2F spec OP-5, §5; plan Task 5): the fold table
 * (tests/opt/fold_table.txt, Yosys 0.55 opt_expr on constant cells; tools/opt-check/gen-fold-table)
 * replayed through odin3_opt_fold on one-cell modules, the corner cases of division, pmux, mux,
 * 4-state logic and shiftx, a cell with a non-constant input, a constant $sop input, the
 * counters, misuse and out of memory.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "passes/opt/fold.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_OPT_DATA
#error "ODIN3_OPT_DATA (tests/opt) must be defined by the build"
#endif

enum {
    TEXT = 1 << 14,
    LINE = 1 << 12,
    FIELD = 1 << 11,
    MIN_TABLE = 2000,
    MAX_REPORTED = 20,
    OOM_LIMIT = 200
};

typedef struct fixture {
    pb_result res;
    odin3_opt_opts opts;
    odin3_opt_counts counts;
    odin3_opt_ctx oc;
    bool open;
} fixture;

static fixture fx;

void setUp(void) {
    memset(&fx, 0, sizeof fx);
}

static void close_fixture(void) {
    if (fx.open) {
        odin3_opt_ctx_free(&fx.oc);
    }
    pb_free(&fx.res);
    memset(&fx, 0, sizeof fx);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    close_fixture();
    odin3_log_set_sink(NULL, NULL);
}

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(fx.res.design, odin3_bytes_cstr(name), &str));
    return str;
}

static void build(const char *text) {
    close_fixture();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx.res));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fx.res.design, intern("opt"), &fx.res.ctx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_opt_ctx_init(&fx.oc, &fx.res.ctx, fx.res.module, &fx.opts));
    fx.oc.counts = &fx.counts;
    fx.open = true;
}

static void check_full(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(fx.res.design, opts));
}

/* One cell `\c` of `type` with its parameter and connect lines, its Y on wire `\y`. */
typedef struct cell_case {
    const char *type;
    const char *params; /* "parameter \\A_SIGNED 1\n..." */
    const char *ports;  /* "connect \\A 4'1010\n..." (Y is added) */
    const char *wires;  /* extra wire lines */
    uint32_t y_width;
} cell_case;

static void build_cell(const cell_case *cc) {
    char text[TEXT];
    int len = snprintf(text, sizeof text,
                       "module \\m\n  wire width %u output 1 \\y\n%s  cell %s \\c\n%s%s"
                       "    connect \\Y \\y\n  end\nend\n",
                       cc->y_width, cc->wires, cc->type, cc->params, cc->ports);
    TEST_ASSERT_TRUE_MESSAGE(len > 0 && (size_t)len < sizeof text, "case text fits");
    build(text);
}

static odin3_node_id cell_node(void) {
    odin3_node_id node = odin3_module_find_node(fx.res.module, intern("c"));
    TEST_ASSERT_TRUE(odin3_node_valid(node));
    return node;
}

/* The bits of wire y, MSB first in 0 1 x z, '?' for a net that is not constant. */
static void read_y(uint32_t width, char *out) {
    odin3_wire_id wire = odin3_module_find_wire(fx.res.module, intern("y"));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    for (uint32_t k = 0; k < width; k++) {
        odin3_net_id net = odin3_wire_net(fx.res.module, wire, k);
        char ch = '?';
        switch (odin3_net_const_value(fx.res.module, net)) {
        case ODIN3_CONST_0:
            ch = '0';
            break;
        case ODIN3_CONST_1:
            ch = '1';
            break;
        case ODIN3_CONST_X:
            ch = 'x';
            break;
        case ODIN3_CONST_Z:
            ch = 'z';
            break;
        case ODIN3_CONST_NONE:
        default:
            break;
        }
        out[width - 1 - k] = ch;
    }
    out[width] = '\0';
}

/* Builds the cell, folds it, and returns whether it folded; the result goes to out. */
static bool fold_cell(const cell_case *cc, char *out) {
    build_cell(cc);
    bool folded = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_fold(&fx.oc, cell_node(), &folded));
    check_full();
    read_y(cc->y_width, out);
    return folded;
}

static void expect_fold(const cell_case *cc, const char *want) {
    char got[FIELD];
    TEST_ASSERT_TRUE_MESSAGE(fold_cell(cc, got), cc->type);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, cc->type);
}

/* --- the table ---------------------------------------------------------------------------- */

/* A text being built; put() appends and checks that it fits. */
typedef struct text {
    char buf[TEXT];
    size_t len;
} text;

static void put(text *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(text *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(out->buf + out->len, sizeof out->buf - out->len, fmt, args);
    va_end(args);
    TEST_ASSERT_TRUE_MESSAGE(len >= 0 && (size_t)len < sizeof out->buf - out->len, "text fits");
    out->len += (size_t)len;
}

/* One table line: `<type> <a_signed> <b_signed> <y_width> <a> <b> <s> <result>`. */
typedef struct table_fields {
    char type[FIELD];
    char a_signed[FIELD];
    char b_signed[FIELD];
    char y_width[FIELD];
    char op_a[FIELD];
    char op_b[FIELD];
    char op_s[FIELD];
    char result[FIELD];
} table_fields;

/* The parameter and connect lines of a table line's cell. */
static void table_cell(const table_fields *tf, text *params, text *ports) {
    uint32_t width = (uint32_t)strtoul(tf->y_width, NULL, 10);
    if (strcmp(tf->type, "$mux") == 0 || strcmp(tf->type, "$pmux") == 0) {
        put(params, "    parameter \\WIDTH %u\n", width);
        if (strcmp(tf->type, "$pmux") == 0) {
            put(params, "    parameter \\S_WIDTH %zu\n", strlen(tf->op_s));
        }
        put(ports, "    connect \\A %zu'%s\n    connect \\B %zu'%s\n    connect \\S %zu'%s\n",
            strlen(tf->op_a), tf->op_a, strlen(tf->op_b), tf->op_b, strlen(tf->op_s), tf->op_s);
        return;
    }
    put(params, "    parameter \\A_SIGNED %s\n    parameter \\A_WIDTH %zu\n", tf->a_signed,
        strlen(tf->op_a));
    put(ports, "    connect \\A %zu'%s\n", strlen(tf->op_a), tf->op_a);
    if (strcmp(tf->op_b, "-") != 0) {
        put(params, "    parameter \\B_SIGNED %s\n    parameter \\B_WIDTH %zu\n", tf->b_signed,
            strlen(tf->op_b));
        put(ports, "    connect \\B %zu'%s\n", strlen(tf->op_b), tf->op_b);
    }
    put(params, "    parameter \\Y_WIDTH %u\n", width);
}

/* One table line: true on a match. */
static bool table_line(const char *line) {
    static table_fields tf;
    static text params;
    static text ports;
    int got = sscanf(line, "%1023s %1023s %1023s %1023s %1023s %1023s %1023s %1023s", tf.type,
                     tf.a_signed, tf.b_signed, tf.y_width, tf.op_a, tf.op_b, tf.op_s, tf.result);
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, got, line);
    params.len = 0;
    ports.len = 0;
    params.buf[0] = '\0';
    ports.buf[0] = '\0';
    table_cell(&tf, &params, &ports);
    cell_case cc = {tf.type, params.buf, ports.buf, "", (uint32_t)strtoul(tf.y_width, NULL, 10)};
    char got_bits[FIELD];
    bool folded = fold_cell(&cc, got_bits);
    bool same = folded && strcmp(got_bits, tf.result) == 0;
    if (!same) {
        printf("MISMATCH %s: got %s\n", line, folded ? got_bits : "(not folded)");
    }
    return same;
}

static uint32_t replay(const char *path) {
    FILE *file = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    char line[LINE];
    uint32_t cases = 0;
    uint32_t wrong = 0;
    bool header = false;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#') {
            header = header || strncmp(line, "# yosys: Yosys 0.55", 19) == 0;
            continue;
        }
        cases++;
        wrong += table_line(line) ? 0 : 1;
        TEST_ASSERT_LESS_THAN_UINT32(MAX_REPORTED, wrong);
    }
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_TRUE_MESSAGE(header, "the table names Yosys 0.55");
    TEST_ASSERT_EQUAL_UINT32(0, wrong);
    return cases;
}

static void test_fold_table(void) {
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(MIN_TABLE, replay(ODIN3_OPT_DATA "/fold_table.txt"));
}

/* --- corner cases ------------------------------------------------------------------------- */

static const char *const k_binary8 = "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
                                     "    parameter \\A_WIDTH 8\n    parameter \\B_WIDTH 8\n"
                                     "    parameter \\Y_WIDTH 8\n";

static void test_div_by_zero(void) {
    cell_case cc = {"$div", k_binary8, "    connect \\A 8'00001000\n    connect \\B 8'00000000\n",
                    "", 8};
    expect_fold(&cc, "xxxxxxxx");
    cc.type = "$mod";
    expect_fold(&cc, "xxxxxxxx");
}

static const char *const k_pmux2 = "    parameter \\WIDTH 2\n    parameter \\S_WIDTH 2\n";

static void test_pmux(void) {
    cell_case cc = {"$pmux", k_pmux2,
                    "    connect \\A 2'00\n    connect \\B 4'1001\n    connect \\S 2'11\n", "", 2};
    expect_fold(&cc, "xx");
    cc.ports = "    connect \\A 2'00\n    connect \\B 4'1001\n    connect \\S 2'0x\n";
    expect_fold(&cc, "xx");
    cc.ports = "    connect \\A 2'00\n    connect \\B 4'1001\n    connect \\S 2'10\n";
    expect_fold(&cc, "10");
    cc.ports = "    connect \\A 2'11\n    connect \\B 4'1001\n    connect \\S 2'00\n";
    expect_fold(&cc, "11");
}

static void test_mux_undefined_select(void) {
    cell_case cc = {"$mux", "    parameter \\WIDTH 4\n",
                    "    connect \\A 4'1100\n    connect \\B 4'1010\n    connect \\S 1'x\n", "", 4};
    expect_fold(&cc, "1xx0");
    cc.ports = "    connect \\A 4'1100\n    connect \\B 4'1010\n    connect \\S 1'1\n";
    expect_fold(&cc, "1010");
}

static void test_z_reads_as_x(void) {
    cell_case cc = {"$and",
                    "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
                    "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n"
                    "    parameter \\Y_WIDTH 4\n",
                    "    connect \\A 4'10z1\n    connect \\B 4'0011\n", "", 4};
    expect_fold(&cc, "00x1");
}

static void test_shiftx(void) {
    cell_case cc = {"$shiftx",
                    "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
                    "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 3\n"
                    "    parameter \\Y_WIDTH 2\n",
                    "    connect \\A 4'1011\n    connect \\B 3'011\n", "", 2};
    expect_fold(&cc, "x1");
    cc.ports = "    connect \\A 4'1011\n    connect \\B 3'0x1\n";
    expect_fold(&cc, "xx");
}

static const char *const k_add4 = "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n"
                                  "    parameter \\A_WIDTH 4\n    parameter \\B_WIDTH 4\n"
                                  "    parameter \\Y_WIDTH 4\n";

static void test_non_constant_input(void) {
    cell_case cc = {"$add", k_add4, "    connect \\A 4'0001\n    connect \\B \\in\n",
                    "  wire width 4 input 2 \\in\n", 4};
    build_cell(&cc);
    bool folded = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_fold(&fx.oc, cell_node(), &folded));
    TEST_ASSERT_FALSE(folded);
    TEST_ASSERT_TRUE(odin3_node_live(fx.res.module, cell_node()));
    TEST_ASSERT_FALSE(fx.oc.changed);
    TEST_ASSERT_EQUAL_UINT32(0, fx.counts.expr.folded);
    check_full();
}

/* The fold counter named name (a pointer into fx.counts), NULL for none. */
static const uint32_t *fold_counter(const char *name) {
    for (uint32_t i = 0; i < ODIN3_OPT_FOLD_TYPES; i++) {
        const char *got = odin3_opt_fold_name(i);
        TEST_ASSERT_NOT_NULL(got);
        if (strcmp(got, name) == 0) {
            return &fx.counts.fold_type[i];
        }
    }
    return NULL;
}

static void test_counters(void) {
    cell_case cc = {"$add", k_add4, "    connect \\A 4'0001\n    connect \\B 4'0010\n", "", 4};
    expect_fold(&cc, "0011");
    TEST_ASSERT_NOT_NULL(fold_counter("const.$add"));
    TEST_ASSERT_EQUAL_UINT32(1, *fold_counter("const.$add"));
    TEST_ASSERT_EQUAL_UINT32(0, *fold_counter("const.$sub"));
    TEST_ASSERT_EQUAL_UINT32(1, fx.counts.expr.folded);
    TEST_ASSERT_TRUE(fx.oc.changed);
    TEST_ASSERT_NULL(odin3_opt_fold_name(ODIN3_OPT_FOLD_TYPES));
}

static bool type_foldable(const char *name) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE_MESSAGE(odin3_celltype_find(fx.res.design, intern(name), &type), name);
    return odin3_opt_foldable(&fx.oc, type);
}

/* Every name of names[0 .. n) is (want) or is not foldable. */
static void expect_foldable(const char *const *names, size_t count, bool want) {
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, type_foldable(names[i]), names[i]);
    }
}

static void test_foldable_types(void) {
    build("module \\m\nend\n");
    static const char *const yes[] = {"$not",  "$and", "$reduce_bool", "$logic_or", "$shiftx",
                                      "$pmux", "$mux", "$pow",         "$neg",      "$ge"};
    static const char *const no[] = {"$dff", "$lut", "$sop", "$_AND_", "$dlatch", "$proc"};
    expect_foldable(yes, sizeof yes / sizeof yes[0], true);
    expect_foldable(no, sizeof no / sizeof no[0], false);
    TEST_ASSERT_FALSE(odin3_opt_foldable(&fx.oc, (odin3_celltype_id){0}));
}

static void test_misuse(void) {
    cell_case cc = {"$add", k_add4, "    connect \\A 4'0001\n    connect \\B 4'0010\n", "", 4};
    build_cell(&cc);
    bool folded = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_fold(NULL, cell_node(), &folded));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_fold(&fx.oc, cell_node(), NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_opt_fold(&fx.oc, (odin3_node_id){0}, &folded));
    odin3_cval val;
    bool all = true;
    odin3_opt_load_req req = {cell_node(), 0, false, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_opt_load(&fx.oc, &req, &val, &all));
}

/* A constant $sop (no inputs, one row saying 1) on the input net reads as ONE (IR-4). */
static void test_constant_sop_input(void) {
    cell_case cc = {"$not",
                    "    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
                    "    parameter \\Y_WIDTH 1\n",
                    "    connect \\A \\k\n", "  wire width 1 \\k\n", 1};
    build_cell(&cc);
    odin3_celltype_id sop_type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fx.res.design, intern("$sop"), &sop_type));
    odin3_wire_id wire = odin3_module_find_wire(fx.res.module, intern("k"));
    odin3_net_id knet = odin3_wire_net(fx.res.module, wire, 0);
    odin3_value params[2];
    params[0] = odin3_value_int(0);
    memset(&params[1], 0, sizeof params[1]);
    params[1].kind = ODIN3_VAL_COVER;
    params[1].bits = (const uint8_t *)"1";
    params[1].len = 1;
    odin3_node_spec spec = {.type = sop_type, .params = params, .n_params = 2};
    odin3_net_id none = {0};
    odin3_netvec ports[2] = {{&none, 0}, {&knet, 1}};
    odin3_node_id sop = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(fx.res.module, &spec, ports, &sop));
    bool folded = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_opt_fold(&fx.oc, cell_node(), &folded));
    TEST_ASSERT_TRUE(folded);
    char got[FIELD];
    read_y(1, got);
    TEST_ASSERT_EQUAL_STRING("0", got);
    check_full();
}

/* One fold with the allocator failing after `fail` calls: true when it succeeded. */
static bool oom_step(const cell_case *cc, long fail) {
    build_cell(cc);
    odin3_util_set_alloc_fail_after(fail);
    bool folded = false;
    odin3_status st = odin3_opt_fold(&fx.oc, cell_node(), &folded);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        char got[FIELD];
        read_y(cc->y_width, got);
        TEST_ASSERT_TRUE(folded);
        TEST_ASSERT_EQUAL_STRING("0011", got);
        return true;
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    TEST_ASSERT_FALSE(folded);
    return false;
}

/* Out of memory at every allocation: the IR stays valid, and a success is the right fold. */
static void test_oom(void) {
    cell_case cc = {"$add", k_add4, "    connect \\A 4'0001\n    connect \\B 4'0010\n", "", 4};
    long fail = 0;
    while (fail < OOM_LIMIT && !oom_step(&cc, fail)) {
        check_full();
        fail++;
    }
    TEST_ASSERT_GREATER_THAN(0, fail);
    TEST_ASSERT_LESS_THAN(OOM_LIMIT, fail);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fold_table);
    RUN_TEST(test_div_by_zero);
    RUN_TEST(test_pmux);
    RUN_TEST(test_mux_undefined_select);
    RUN_TEST(test_z_reads_as_x);
    RUN_TEST(test_shiftx);
    RUN_TEST(test_non_constant_input);
    RUN_TEST(test_counters);
    RUN_TEST(test_foldable_types);
    RUN_TEST(test_misuse);
    RUN_TEST(test_constant_sop_input);
    RUN_TEST(test_oom);
    return UNITY_END();
}
