/*
 * test_ir_cells.c — table-driven tests of the built-in cell library (IR-8..IR-11).
 */
#include "ir/cells/cells.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/value.h"
#include "sim/sim.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ODIN3_INT_PARAMS_FILE
#error "ODIN3_INT_PARAMS_FILE must name tools/proc-check/int_params.txt (CMakeLists.txt)"
#endif

enum {
    MAX_PORTS = 8,
    MAX_PARAMS = 16,
    BAD_INIT = 4,
    SOP_WIDTH = 3,
    ROW_LEN = 4,
    N_TYPES = 68,
    STRUCTURAL_TYPES = 7,
    DFFX_TYPES = 68,  /* the 5.3 follow-up flip-flops of dffx.h; tests/unit/test_dffx_cells.c */
    SOP_INDEX = 52,   /* the last 1B type; 2D appends after it (plan point (f)) */
    NEG_INDEX = 53,   /* the first of the nine 2D word cells (IR change A, EL-31) */
    POW_INDEX = 61,   /* the last of the nine */
    PROC_INDEX = 62,  /* the process cell (IR change B, EL-33), after the nine */
    ADFFE_INDEX = 63, /* the first of the eleven 2E flip-flops and latches (IR change D, §13) */
    SR_INDEX = 73,    /* the last of the eleven */
    BUILTIN_COUNT = 143,
    FF_WIDE = 4, /* WIDTH of the pin-count and verify cases below */
    FF_WIDE_PINS = 11,
    DFFSRE_WIDTH = 3,
    DFFSRE_PINS = 14,
    SR_WIDTH = 2,
    SR_PINS = 6,
    FF_ROW_WIDTH = 3, /* the WIDTH of k_ffcells' pin counts */
    POW_A_WIDTH = 4,
    POW_B_WIDTH = 3,
    POW_Y_WIDTH = 5,
    LOG_TEXT = 512,
    MAX_INT_NAMES = 64, /* distinct INT parameter names over the built-in cells (int_params) */
    NAME_TEXT = 64
};

static odin3_design *design;
static uint32_t errors_logged;
static char last_error[LOG_TEXT];
static char first_error[LOG_TEXT]; /* the first error since it was last cleared */

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
        (void)snprintf(last_error, sizeof last_error, "%s", msg);
        if (first_error[0] == '\0') {
            (void)snprintf(first_error, sizeof first_error, "%s", msg);
        }
    }
}

/* Asserts that verify rejected params with exactly one logged error, then resets the count. */
static void assert_rejected(const odin3_celltype_def *def, const odin3_value *params) {
    errors_logged = 0;
    TEST_ASSERT_EQUAL_MESSAGE(ODIN3_ERR_INVALID_ARG, def->verify(params), def->name);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, errors_logged, def->name);
    errors_logged = 0;
}

void setUp(void) {
    errors_logged = 0;
    odin3_log_set_sink(count_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_celltype_id find_type(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str)) {
        (void)odin3_celltype_find(design, str, &id);
    }
    return id;
}

/* Expected shape of a type at its default parameters; dirs is one of I/O/B per port. */
typedef struct expect {
    const char *name;
    odin3_granularity gran;
    uint32_t flags;
    const char *dirs;
    uint32_t widths[MAX_PORTS];
} expect;

#define WORD ODIN3_GRAN_WORD
#define BIT ODIN3_GRAN_BIT
static const expect k_expect[] = {
    {"$add", WORD, 0, "IIO", {1, 1, 1}},
    {"$sub", WORD, 0, "IIO", {1, 1, 1}},
    {"$mul", WORD, 0, "IIO", {1, 1, 1}},
    {"$div", WORD, 0, "IIO", {1, 1, 1}},
    {"$mod", WORD, 0, "IIO", {1, 1, 1}},
    {"$and", WORD, 0, "IIO", {1, 1, 1}},
    {"$or", WORD, 0, "IIO", {1, 1, 1}},
    {"$xor", WORD, 0, "IIO", {1, 1, 1}},
    {"$not", WORD, 0, "IO", {1, 1}},
    {"$shl", WORD, 0, "IIO", {1, 1, 1}},
    {"$shr", WORD, 0, "IIO", {1, 1, 1}},
    {"$sshr", WORD, 0, "IIO", {1, 1, 1}},
    {"$eq", WORD, 0, "IIO", {1, 1, 1}},
    {"$ne", WORD, 0, "IIO", {1, 1, 1}},
    {"$lt", WORD, 0, "IIO", {1, 1, 1}},
    {"$le", WORD, 0, "IIO", {1, 1, 1}},
    {"$gt", WORD, 0, "IIO", {1, 1, 1}},
    {"$ge", WORD, 0, "IIO", {1, 1, 1}},
    {"$reduce_and", WORD, 0, "IO", {1, 1}},
    {"$reduce_or", WORD, 0, "IO", {1, 1}},
    {"$reduce_xor", WORD, 0, "IO", {1, 1}},
    {"$mux", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$pmux", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$dff", WORD, 0, "IIO", {1, 1, 1}},
    {"$dffe", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$adff", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$sdff", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$mem", WORD, 0, "IIIOIIII", {1, 1, 1, 1, 1, 1, 1, 1}},
    {"$memrd", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$memwr", WORD, 0, "IIII", {1, 1, 1, 1}},
    {"$tribuf", WORD, ODIN3_CT_TRISTATE, "IIO", {1, 1, 1}},
    {"$_BUF_", BIT, 0, "IO", {1, 1}},
    {"$_NOT_", BIT, 0, "IO", {1, 1}},
    {"$_AND_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_OR_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_XOR_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_NAND_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_NOR_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_XNOR_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_MUX_", BIT, 0, "IIIO", {1, 1, 1, 1}},
    {"$_DFF_P_", BIT, ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0, "IIO", {1, 1, 1}},
    {"$_DFF_N_", BIT, ODIN3_CT_SEQ_EDGE | ODIN3_CT_CLOCK_PIN0, "IIO", {1, 1, 1}},
    {"$_DLATCH_P_", BIT, ODIN3_CT_SEQ_LEVEL | ODIN3_CT_CLOCK_PIN0, "IIO", {1, 1, 1}},
    {"$_DLATCH_N_", BIT, ODIN3_CT_SEQ_LEVEL | ODIN3_CT_CLOCK_PIN0, "IIO", {1, 1, 1}},
    {"$_FF_", BIT, ODIN3_CT_SEQ_EDGE, "IO", {1, 1}},
    {"$sop", BIT, 0, "IO", {0, 1}},
    {"$neg", WORD, 0, "IO", {1, 1}},
    {"$xnor", WORD, 0, "IIO", {1, 1, 1}},
    {"$reduce_xnor", WORD, 0, "IO", {1, 1}},
    {"$reduce_bool", WORD, 0, "IO", {1, 1}},
    {"$logic_not", WORD, 0, "IO", {1, 1}},
    {"$logic_and", WORD, 0, "IIO", {1, 1, 1}},
    {"$logic_or", WORD, 0, "IIO", {1, 1, 1}},
    {"$shiftx", WORD, 0, "IIO", {1, 1, 1}},
    {"$pow", WORD, 0, "IIO", {1, 1, 1}},
    {"$proc", WORD, ODIN3_CT_PROCESS, "IO", {0, 1}},
    {"$adffe", WORD, 0, "IIIIO", {1, 1, 1, 1, 1}},
    {"$sdffe", WORD, 0, "IIIIO", {1, 1, 1, 1, 1}},
    {"$sdffce", WORD, 0, "IIIIO", {1, 1, 1, 1, 1}},
    {"$aldff", WORD, 0, "IIIIO", {1, 1, 1, 1, 1}},
    {"$aldffe", WORD, 0, "IIIIIO", {1, 1, 1, 1, 1, 1}},
    {"$dffsr", WORD, 0, "IIIIO", {1, 1, 1, 1, 1}},
    {"$dffsre", WORD, 0, "IIIIIO", {1, 1, 1, 1, 1, 1}},
    {"$dlatch", WORD, 0, "IIO", {1, 1, 1}},
    {"$adlatch", WORD, 0, "IIIO", {1, 1, 1, 1}},
    {"$dlatchsr", WORD, 0, "IIIIO", {1, 1, 1, 1, 1}},
    {"$sr", WORD, 0, "IIO", {1, 1, 1}},
    {"$lut", BIT, 0, "IO", {1, 1}}, /* 3D (AB-2), after the eleven; ir/lut.h names its ID */
};
#define N_EXPECT (sizeof k_expect / sizeof k_expect[0])

static void defaults_of(const odin3_celltype_def *def, odin3_value *out) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        out[i] = def->params[i].dflt;
    }
}

static char dir_char(odin3_dir dir) {
    static const char k_chars[] = "IOB"; /* indexed by odin3_dir */
    return k_chars[dir];
}

static void check_shape(const expect *exp) {
    odin3_celltype_id id = find_type(exp->name);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, id.v, exp->name);
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    TEST_ASSERT_EQUAL_STRING(exp->name, def->name);
    TEST_ASSERT_EQUAL_MESSAGE(exp->gran, def->gran, exp->name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(exp->flags, def->flags, exp->name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)strlen(exp->dirs), def->n_ports, exp->name);
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    for (uint32_t i = 0; i < def->n_ports; i++) {
        TEST_ASSERT_EQUAL_CHAR_MESSAGE(exp->dirs[i], dir_char(def->ports[i].dir), exp->name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(
            exp->widths[i], odin3_celltype_port_width(design, id, params, i), exp->name);
    }
    if (def->verify != NULL) {
        /* $proc's empty default BODY is rejected: a default process cannot exist (IR-19). */
        odin3_status want = (def->flags & ODIN3_CT_PROCESS) != 0 ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
        TEST_ASSERT_EQUAL_MESSAGE(want, def->verify(params), exp->name);
    }
}

static void test_every_builtin_has_expected_shape(void) {
    TEST_ASSERT_EQUAL_UINT32(N_TYPES, (uint32_t)N_EXPECT);
    /* 7 structural/constant types (Task 2) + every table row = every built-in type. */
    TEST_ASSERT_EQUAL_UINT32(odin3_builtin_celltype_count,
                             STRUCTURAL_TYPES + (uint32_t)N_EXPECT + DFFX_TYPES);
    for (size_t i = 0; i < N_EXPECT; i++) {
        check_shape(&k_expect[i]);
    }
}

static const odin3_celltype_def *get_def(const char *name) {
    return odin3_celltype_get(design, find_type(name));
}

/* params[index] = v for the named INT parameter. */
static void set_int(const odin3_celltype_def *def, odin3_value *params, const char *name,
                    int64_t val) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            params[i] = odin3_value_int(val);
            return;
        }
    }
    TEST_FAIL_MESSAGE(name);
}

static void test_yosys_parameter_names(void) {
    const odin3_celltype_def *def = get_def("$add");
    const char *names[] = {"A_SIGNED", "B_SIGNED", "A_WIDTH", "B_WIDTH", "Y_WIDTH"};
    TEST_ASSERT_EQUAL_UINT32(5, def->n_params);
    for (uint32_t i = 0; i < def->n_params; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], def->params[i].name);
    }
    TEST_ASSERT_EQUAL_STRING("CLK", get_def("$dffe")->ports[0].name);
    TEST_ASSERT_EQUAL_STRING("EN", get_def("$dffe")->ports[1].name);
    TEST_ASSERT_EQUAL_STRING("ARST", get_def("$adff")->ports[1].name);
    TEST_ASSERT_EQUAL_STRING("SRST_VALUE", get_def("$sdff")->params[3].name);
    TEST_ASSERT_EQUAL_STRING("C", get_def("$_DFF_P_")->ports[0].name);
    TEST_ASSERT_EQUAL_STRING("E", get_def("$_DLATCH_N_")->ports[0].name);
}

static void test_sample_widths(void) {
    odin3_celltype_id id = find_type("$add");
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    set_int(def, params, "A_WIDTH", 8);
    set_int(def, params, "B_WIDTH", 4);
    set_int(def, params, "Y_WIDTH", 9);
    TEST_ASSERT_EQUAL_UINT32(8, odin3_celltype_port_width(design, id, params, 0));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_celltype_port_width(design, id, params, 1));
    TEST_ASSERT_EQUAL_UINT32(9, odin3_celltype_port_width(design, id, params, 2));
}

static void test_pmux_b_width_is_width_times_s_width(void) {
    odin3_celltype_id id = find_type("$pmux");
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    set_int(def, params, "WIDTH", 4);
    set_int(def, params, "S_WIDTH", 3);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_celltype_port_width(design, id, params, 0));
    TEST_ASSERT_EQUAL_UINT32(12, odin3_celltype_port_width(design, id, params, 1));
    TEST_ASSERT_EQUAL_UINT32(3, odin3_celltype_port_width(design, id, params, 2));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_celltype_port_width(design, id, params, 3));
    set_int(def, params, "WIDTH", INT32_MAX);
    set_int(def, params, "S_WIDTH", 4);
    assert_rejected(def, params);
}

static void test_mem_port_widths(void) {
    odin3_celltype_id id = find_type("$mem");
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    set_int(def, params, "ABITS", 5);
    set_int(def, params, "WIDTH", 8);
    set_int(def, params, "RD_PORTS", 2);
    set_int(def, params, "WR_PORTS", 3);
    const uint32_t want[MAX_PORTS] = {2, 2, 10, 16, 3, 24, 15, 24};
    for (uint32_t i = 0; i < def->n_ports; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], odin3_celltype_port_width(design, id, params, i));
    }
}

static void test_verify_rejects_bad_parameters(void) {
    const odin3_celltype_def *latch = get_def("$_DFF_P_");
    odin3_value params[MAX_PARAMS];
    defaults_of(latch, params);
    set_int(latch, params, "INIT", BAD_INIT);
    assert_rejected(latch, params);
    set_int(latch, params, "INIT", -1);
    assert_rejected(latch, params);
    set_int(latch, params, "INIT", 2);
    TEST_ASSERT_EQUAL(ODIN3_OK, latch->verify(params));

    const odin3_celltype_def *add = get_def("$add");
    defaults_of(add, params);
    set_int(add, params, "A_WIDTH", 0);
    assert_rejected(add, params);
    defaults_of(add, params);
    set_int(add, params, "A_SIGNED", 2);
    assert_rejected(add, params);

    const odin3_celltype_def *dff = get_def("$adff");
    defaults_of(dff, params);
    set_int(dff, params, "WIDTH", 2); /* ARST_VALUE still has 1 bit */
    assert_rejected(dff, params);
}

static odin3_value cover_value(const char *rows, uint32_t len, uint32_t inputs) {
    odin3_value val = {ODIN3_VAL_COVER, 0, (const uint8_t *)rows, len, 0, inputs};
    return val;
}

static void test_sop_verify(void) {
    const odin3_celltype_def *sop = get_def("$sop");
    odin3_value params[2] = {odin3_value_int(SOP_WIDTH), cover_value("01-1", ROW_LEN, SOP_WIDTH)};
    TEST_ASSERT_EQUAL(ODIN3_OK, sop->verify(params));
    params[1] = cover_value("01-11", ROW_LEN + 1, SOP_WIDTH); /* not a whole number of rows */
    assert_rejected(sop, params);
    params[1] = cover_value("01", 2, 1); /* row width right for 1 input, WIDTH says 3 */
    assert_rejected(sop, params);
    params[1] = cover_value("0x-1", ROW_LEN, SOP_WIDTH); /* bad input char */
    assert_rejected(sop, params);
    params[1] = cover_value("01-2", ROW_LEN, SOP_WIDTH); /* bad output char */
    assert_rejected(sop, params);
    params[0] = odin3_value_int(-1);
    assert_rejected(sop, params);
}

static void test_sop_const_value(void) {
    const odin3_celltype_def *sop = get_def("$sop");
    odin3_value params[2] = {odin3_value_int(0), cover_value("1", 1, 0)};
    TEST_ASSERT_EQUAL(ODIN3_OK, sop->verify(params));
    TEST_ASSERT_EQUAL(ODIN3_CONST_1, sop->const_value(params));
    params[1] = cover_value("0", 1, 0);
    TEST_ASSERT_EQUAL(ODIN3_CONST_0, sop->const_value(params));
    params[1] = cover_value("", 0, 0);
    TEST_ASSERT_EQUAL(ODIN3_OK, sop->verify(params));
    TEST_ASSERT_EQUAL(ODIN3_CONST_0, sop->const_value(params));
    params[0] = odin3_value_int(1);
    params[1] = cover_value("11", 2, 1);
    TEST_ASSERT_EQUAL(ODIN3_CONST_NONE, sop->const_value(params));
}

typedef struct name_case {
    const char *type;
    const char *const *ports;
    const char *const *names;
} name_case;

static void check_names(const name_case *item) {
    const char *type = item->type;
    const char *const *ports = item->ports;
    const char *const *names = item->names;
    const odin3_celltype_def *def = get_def(type);
    for (uint32_t i = 0; ports[i] != NULL; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ports[i], def->ports[i].name, type);
    }
    for (uint32_t i = 0; names[i] != NULL; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(names[i], def->params[i].name, type);
    }
}

static void test_names_of_other_types(void) {
    const char *const pmux_p[] = {"A", "B", "S", "Y", NULL};
    const char *const pmux_n[] = {"WIDTH", "S_WIDTH", NULL};
    check_names(&(name_case){"$pmux", pmux_p, pmux_n});
    const char *const tri_p[] = {"A", "EN", "Y", NULL};
    const char *const tri_n[] = {"WIDTH", NULL};
    check_names(&(name_case){"$tribuf", tri_p, tri_n});
    const char *const sop_p[] = {"A", "Y", NULL};
    const char *const sop_n[] = {"WIDTH", "COVER", NULL};
    check_names(&(name_case){"$sop", sop_p, sop_n});
    const char *const mem_p[] = {"RD_CLK", "RD_EN",   "RD_ADDR", "RD_DATA", "WR_CLK",
                                 "WR_EN",  "WR_ADDR", "WR_DATA", NULL};
    const char *const mem_n[] = {"MEMID",
                                 "SIZE",
                                 "OFFSET",
                                 "ABITS",
                                 "WIDTH",
                                 "RD_PORTS",
                                 "WR_PORTS",
                                 "RD_CLK_ENABLE",
                                 "RD_CLK_POLARITY",
                                 "RD_TRANSPARENT",
                                 "WR_CLK_ENABLE",
                                 "WR_CLK_POLARITY",
                                 "INIT",
                                 NULL};
    check_names(&(name_case){"$mem", mem_p, mem_n});
}

/* Sets one INT parameter of the named type to val (from defaults) and expects rejection. */
typedef struct int_case {
    const char *type;
    const char *param;
    int64_t val;
} int_case;

static void reject_int(const int_case *item) {
    const char *type = item->type;
    const char *param = item->param;
    int64_t val = item->val;
    const odin3_celltype_def *def = get_def(type);
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    set_int(def, params, param, val);
    assert_rejected(def, params);
}

static void test_verify_rejects_more_types(void) {
    reject_int(&(int_case){"$mux", "WIDTH", 0});
    reject_int(&(int_case){"$dffe", "EN_POLARITY", 2});
    reject_int(&(int_case){"$dffe", "WIDTH", 0});
    reject_int(&(int_case){"$sdff", "SRST_POLARITY", -1});
    reject_int(&(int_case){"$sdff", "WIDTH", 2}); /* SRST_VALUE still 1 bit */
    reject_int(&(int_case){"$tribuf", "WIDTH", 0});
    reject_int(&(int_case){"$memrd", "ABITS", 0});
    reject_int(&(int_case){"$memrd", "TRANSPARENT", 2});
    reject_int(&(int_case){"$memwr", "WIDTH", 0});
    reject_int(&(int_case){"$memwr", "CLK_POLARITY", 2});
    reject_int(&(int_case){"$mem", "WIDTH", 0});
    reject_int(&(int_case){"$mem", "RD_PORTS", 2}); /* bit vectors still 1 bit */
    reject_int(&(int_case){"$pmux", "S_WIDTH", 0});
}

/* wreduce narrows a one-word $mem's address to 0 bits, so $mem accepts ABITS 0 (and not less). */
static void test_mem_abits_zero_accepted(void) {
    const odin3_celltype_def *def = get_def("$mem");
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    set_int(def, params, "ABITS", 0);
    TEST_ASSERT_EQUAL(ODIN3_OK, def->verify(params));
    TEST_ASSERT_EQUAL_UINT32(
        0, odin3_celltype_port_width(design, find_type("$mem"), params, 2)); /* RD_ADDR */
    reject_int(&(int_case){"$mem", "ABITS", -1});
}

static void test_port_width_overflow_is_rejected(void) {
    const odin3_celltype_def *def = get_def("$mem");
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    set_int(def, params, "WIDTH", UINT32_MAX);
    set_int(def, params, "WR_PORTS", 2);
    assert_rejected(def, params);
    errors_logged = 0;
    TEST_ASSERT_EQUAL_UINT32(0, odin3_celltype_port_width(design, find_type("$mem"), params, 7));
    TEST_ASSERT_GREATER_THAN_UINT32(0, errors_logged);
}

/* --- the nine 2D word cells (IR change A, EL-31, spec §7.2) ------------------------------- */

static const char *const k_nine[] = {"$neg",         "$xnor",      "$reduce_xnor",
                                     "$reduce_bool", "$logic_not", "$logic_and",
                                     "$logic_or",    "$shiftx",    "$pow"};
enum { N_NINE = sizeof k_nine / sizeof k_nine[0] };

/* Appended after $sop in point (f)'s order, then $proc; built-in IDs 0..52 do not move. */
static void test_nine_cells_appended_in_order(void) {
    TEST_ASSERT_EQUAL_UINT32(BUILTIN_COUNT, odin3_builtin_celltype_count);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_sop, odin3_builtin_celltypes[SOP_INDEX]);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_neg, odin3_builtin_celltypes[NEG_INDEX]);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_pow, odin3_builtin_celltypes[POW_INDEX]);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_proc, odin3_builtin_celltypes[PROC_INDEX]);
    for (uint32_t i = 0; i < N_NINE; i++) {
        TEST_ASSERT_EQUAL_STRING(k_nine[i], odin3_builtin_celltypes[NEG_INDEX + i]->name);
        odin3_celltype_id id = find_type(k_nine[i]); /* a fresh design has every built-in */
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, id.v, k_nine[i]);
        TEST_ASSERT_EQUAL_PTR_MESSAGE(odin3_builtin_celltypes[NEG_INDEX + i],
                                      odin3_celltype_get(design, id), k_nine[i]);
    }
}

/* The shared Yosys shapes: unary A Y / A_SIGNED A_WIDTH Y_WIDTH; binary A B Y / five params. */
static void test_nine_cells_yosys_names(void) {
    const char *const un_p[] = {"A", "Y", NULL};
    const char *const un_n[] = {"A_SIGNED", "A_WIDTH", "Y_WIDTH", NULL};
    const char *const bin_p[] = {"A", "B", "Y", NULL};
    const char *const bin_n[] = {"A_SIGNED", "B_SIGNED", "A_WIDTH", "B_WIDTH", "Y_WIDTH", NULL};
    for (uint32_t i = 0; i < N_NINE; i++) {
        const odin3_celltype_def *def = get_def(k_nine[i]);
        bool unary = def->n_ports == 2;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(unary ? 3 : 5, def->n_params, k_nine[i]);
        check_names(&(name_case){k_nine[i], unary ? un_p : bin_p, unary ? un_n : bin_n});
    }
    TEST_ASSERT_EQUAL_UINT32(2, get_def("$neg")->n_ports);
    TEST_ASSERT_EQUAL_UINT32(2, get_def("$logic_not")->n_ports);
    TEST_ASSERT_EQUAL_UINT32(3, get_def("$logic_and")->n_ports);
    TEST_ASSERT_EQUAL_UINT32(3, get_def("$shiftx")->n_ports);
}

/* Seven get 1E hooks; $shiftx and $pow have none until a pass needs them (§7.2). */
static void test_nine_cells_hooks(void) {
    for (uint32_t i = 0; i < N_NINE; i++) {
        const odin3_celltype_def *def = get_def(k_nine[i]);
        bool none = strcmp(k_nine[i], "$shiftx") == 0 || strcmp(k_nine[i], "$pow") == 0;
        TEST_ASSERT_EQUAL_MESSAGE(none, def->simulate == NULL, k_nine[i]);
        TEST_ASSERT_NULL_MESSAGE(def->const_value, k_nine[i]);
    }
}

static odin3_module *new_module(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL(ODIN3_OK, odin3_module_create(design, str, (odin3_prov_id){0}, &mid));
    return odin3_module_get(design, mid);
}

/* A binary node's params: unsigned, the three widths. */
static odin3_status binary_node(odin3_module *mod, const char *type, int64_t y_width,
                                odin3_node_id *out) {
    const odin3_value params[5] = {odin3_value_int(0), odin3_value_int(0),
                                   odin3_value_int(POW_A_WIDTH), odin3_value_int(POW_B_WIDTH),
                                   odin3_value_int(y_width)};
    odin3_node_spec spec = {find_type(type), 0, (odin3_prov_id){0}, params, 5};
    return odin3_node_create(mod, &spec, out);
}

static void test_pow_node_pins(void) {
    odin3_module *mod = new_module("m");
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL(ODIN3_OK, binary_node(mod, "$pow", POW_Y_WIDTH, &node));
    TEST_ASSERT_EQUAL_UINT32(POW_A_WIDTH + POW_B_WIDTH + POW_Y_WIDTH,
                             odin3_node_pins(mod, node).count);
    TEST_ASSERT_EQUAL_UINT32(POW_Y_WIDTH, odin3_node_port(mod, node, 2).count);
    errors_logged = 0;
    odin3_node_id bad = {0};
    TEST_ASSERT_EQUAL(ODIN3_ERR_INVALID_ARG, binary_node(mod, "$pow", -1, &bad));
    TEST_ASSERT_GREATER_THAN_UINT32(0, errors_logged);
    TEST_ASSERT_EQUAL_UINT32(0, bad.v);
}

/* No simulate hook: the build refuses the module with the 1E message. */
static void test_shiftx_is_not_simulated(void) {
    odin3_module *mod = new_module("m");
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL(ODIN3_OK, binary_node(mod, "$shiftx", POW_Y_WIDTH, &node));
    odin3_sim *sim = NULL;
    last_error[0] = '\0';
    TEST_ASSERT_EQUAL(ODIN3_ERR_INVALID_ARG,
                      odin3_sim_build(design, odin3_module_id_of(mod), &sim));
    TEST_ASSERT_NULL(sim);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(last_error, "cannot simulate `$shiftx` (node "),
                                 last_error);
}

/* --- the eleven 2E flip-flops and latches (IR change D, 2E spec §13, PR-19) ------------------- */

/* Each cell's ports and parameters, in order (Yosys 0.55 simlib); Q is the only output. */
typedef struct ff_row {
    const char *name;
    const char *ports;
    const char *params;
    const char *widths; /* per port: '1' a 1-bit scalar port, 'W' WIDTH bits */
    uint32_t pins;      /* the node's pin count at WIDTH FF_ROW_WIDTH */
} ff_row;

static const ff_row k_ffcells[] = {
    {"$adffe", "CLK ARST EN D Q", "WIDTH CLK_POLARITY EN_POLARITY ARST_POLARITY ARST_VALUE",
     "111WW", 9},
    {"$sdffe", "CLK SRST EN D Q", "WIDTH CLK_POLARITY EN_POLARITY SRST_POLARITY SRST_VALUE",
     "111WW", 9},
    {"$sdffce", "CLK SRST EN D Q", "WIDTH CLK_POLARITY EN_POLARITY SRST_POLARITY SRST_VALUE",
     "111WW", 9},
    {"$aldff", "CLK ALOAD AD D Q", "WIDTH CLK_POLARITY ALOAD_POLARITY", "11WWW", 11},
    {"$aldffe", "CLK ALOAD AD EN D Q", "WIDTH CLK_POLARITY EN_POLARITY ALOAD_POLARITY", "11W1WW",
     12},
    {"$dffsr", "CLK SET CLR D Q", "WIDTH CLK_POLARITY SET_POLARITY CLR_POLARITY", "1WWWW", 13},
    {"$dffsre", "CLK SET CLR EN D Q", "WIDTH CLK_POLARITY SET_POLARITY CLR_POLARITY EN_POLARITY",
     "1WW1WW", 14},
    {"$dlatch", "EN D Q", "WIDTH EN_POLARITY", "1WW", 7},
    {"$adlatch", "EN ARST D Q", "WIDTH EN_POLARITY ARST_POLARITY ARST_VALUE", "11WW", 8},
    {"$dlatchsr", "EN SET CLR D Q", "WIDTH EN_POLARITY SET_POLARITY CLR_POLARITY", "1WWWW", 13},
    {"$sr", "SET CLR Q", "WIDTH SET_POLARITY CLR_POLARITY", "WWW", 9},
};
enum { N_FFCELLS = sizeof k_ffcells / sizeof k_ffcells[0] };

/* The blank-separated words of list must be the count names got(i), in order. */
typedef const char *(*name_at)(const odin3_celltype_def *def, uint32_t idx);
static const char *port_name_at(const odin3_celltype_def *def, uint32_t idx) {
    return def->ports[idx].name;
}
static const char *param_name_at(const odin3_celltype_def *def, uint32_t idx) {
    return def->params[idx].name;
}

static void check_word_list(const odin3_celltype_def *def, const char *list, uint32_t count,
                            name_at got) {
    uint32_t idx = 0;
    for (const char *word = list; *word != '\0'; idx++) {
        size_t len = strcspn(word, " ");
        TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(count, idx, def->name);
        const char *name = got(def, idx);
        TEST_ASSERT_TRUE_MESSAGE(strlen(name) == len && strncmp(word, name, len) == 0, name);
        word += len + (word[len] == ' ' ? 1 : 0);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(count, idx, def->name);
}

/* Only Q is an output; a verify hook and no simulate or const_value hook. */
static void check_ff_dirs(const odin3_celltype_def *def) {
    TEST_ASSERT_NULL_MESSAGE(def->simulate, def->name);
    TEST_ASSERT_NULL_MESSAGE(def->const_value, def->name);
    TEST_ASSERT_NOT_NULL_MESSAGE(def->verify, def->name);
    for (uint32_t k = 0; k < def->n_ports; k++) {
        bool is_q = strcmp(def->ports[k].name, "Q") == 0;
        TEST_ASSERT_EQUAL_MESSAGE(is_q ? ODIN3_DIR_OUT : ODIN3_DIR_IN, def->ports[k].dir,
                                  def->name);
    }
}

/* WIDTH and the polarities are INT with default 1; each *_VALUE is BITS with one default bit. */
static void check_ff_param_kinds(const odin3_celltype_def *def) {
    for (uint32_t k = 0; k < def->n_params; k++) {
        const odin3_param_def *param = &def->params[k];
        bool is_value = strstr(param->name, "_VALUE") != NULL;
        TEST_ASSERT_EQUAL_MESSAGE(is_value ? ODIN3_VAL_BITS : ODIN3_VAL_INT, param->kind,
                                  param->name);
        TEST_ASSERT_EQUAL_MESSAGE(param->kind, param->dflt.kind, param->name);
        uint32_t dflt = is_value ? param->dflt.len : (uint32_t)param->dflt.i;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, dflt, param->name);
    }
}

/* Each port's width at WIDTH FF_ROW_WIDTH (1-bit ports scalar), and their sum the row's pins. */
static void check_ff_widths(odin3_celltype_id id, const ff_row *row) {
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)strlen(row->widths), def->n_ports, row->name);
    odin3_value params[MAX_PARAMS];
    defaults_of(def, params);
    params[0] = odin3_value_int(FF_ROW_WIDTH);
    uint32_t pins = 0;
    for (uint32_t k = 0; k < def->n_ports; k++) {
        bool one = row->widths[k] == '1';
        uint32_t width = odin3_celltype_port_width(design, id, params, k);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(one ? 1 : FF_ROW_WIDTH, width, def->ports[k].name);
        TEST_ASSERT_EQUAL_MESSAGE(one, def->ports[k].scalar, def->ports[k].name);
        pins += width;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(row->pins, pins, row->name);
}

/* Row i of the table: built-in ID ADFFE_INDEX + i, found by name, its ports and parameters. */
static void check_ff_row(uint32_t row_idx) {
    const ff_row *row = &k_ffcells[row_idx];
    odin3_celltype_id id = find_type(row->name);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, id.v, row->name);
    const odin3_celltype_def *def = odin3_celltype_get(design, id);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(odin3_builtin_celltypes[ADFFE_INDEX + row_idx], def, row->name);
    TEST_ASSERT_EQUAL_STRING(row->name, def->name);
    check_word_list(def, row->ports, def->n_ports, port_name_at);
    check_word_list(def, row->params, def->n_params, param_name_at);
    check_ff_dirs(def);
    check_ff_param_kinds(def);
    check_ff_widths(id, row);
}

/* IDs 63..73 after $proc, in §13's order; found by name in a fresh design; the table's ports
 * (only Q an output), parameters and their kinds (WIDTH and polarities INT, *_VALUE BITS). */
static void test_ffcell_table(void) {
    TEST_ASSERT_EQUAL_UINT32(BUILTIN_COUNT, odin3_builtin_celltype_count);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_proc, odin3_builtin_celltypes[PROC_INDEX]);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_adffe, odin3_builtin_celltypes[ADFFE_INDEX]);
    TEST_ASSERT_EQUAL_PTR(&odin3_cell_sr, odin3_builtin_celltypes[SR_INDEX]);
    TEST_ASSERT_EQUAL_UINT32(SR_INDEX - ADFFE_INDEX + 1, (uint32_t)N_FFCELLS);
    for (uint32_t i = 0; i < N_FFCELLS; i++) {
        check_ff_row(i);
    }
}

/* A node of the named type with params (NULL: the defaults); its ID, or 0 when refused. */
static odin3_node_id ff_node(odin3_module *mod, const char *type, const odin3_value *params) {
    const odin3_celltype_def *def = get_def(type);
    odin3_value dflt[MAX_PARAMS];
    defaults_of(def, dflt);
    odin3_node_spec spec = {find_type(type), 0, (odin3_prov_id){0}, params != NULL ? params : dflt,
                            def->n_params};
    odin3_node_id node = {0};
    (void)odin3_node_create(mod, &spec, &node);
    return node;
}

/* type with params is refused: INVALID_ARG, an error logged, no node created. */
static void expect_ff_refused(odin3_module *mod, const char *type, const odin3_value *params) {
    const odin3_celltype_def *def = get_def(type);
    odin3_node_spec spec = {find_type(type), 0, (odin3_prov_id){0}, params, def->n_params};
    uint32_t before = odin3_module_node_end(mod);
    odin3_node_id node = {0};
    errors_logged = 0;
    first_error[0] = '\0';
    TEST_ASSERT_EQUAL_MESSAGE(ODIN3_ERR_INVALID_ARG, odin3_node_create(mod, &spec, &node), type);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, errors_logged, type);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, node.v, type);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, odin3_module_node_end(mod), type);
}

static const uint8_t k_bits4[FF_WIDE] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_0, ODIN3_BIT_0};
static const uint8_t k_bits3[FF_WIDE - 1] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_0};
static const uint8_t k_bits2[SR_WIDTH] = {ODIN3_BIT_1, ODIN3_BIT_0};

static odin3_value bits_of(const uint8_t *bits, uint32_t len) {
    odin3_value val = {ODIN3_VAL_BITS, 0, bits, len, 0, 0};
    return val;
}

/* Pin counts are the sum of the port widths. */
static void ffcell_pins(odin3_module *mod) {
    const odin3_value adffe[5] = {odin3_value_int(FF_WIDE), odin3_value_int(1), odin3_value_int(0),
                                  odin3_value_int(1), bits_of(k_bits4, FF_WIDE)};
    odin3_node_id node = ff_node(mod, "$adffe", adffe);
    TEST_ASSERT_NOT_EQUAL(0, node.v);
    TEST_ASSERT_EQUAL_UINT32(FF_WIDE_PINS, odin3_node_pins(mod, node).count);
    const odin3_value dffsre[5] = {odin3_value_int(DFFSRE_WIDTH), odin3_value_int(1),
                                   odin3_value_int(0), odin3_value_int(1), odin3_value_int(0)};
    node = ff_node(mod, "$dffsre", dffsre);
    TEST_ASSERT_NOT_EQUAL(0, node.v);
    TEST_ASSERT_EQUAL_UINT32(DFFSRE_PINS, odin3_node_pins(mod, node).count);
    const odin3_value sr[3] = {odin3_value_int(SR_WIDTH), odin3_value_int(1), odin3_value_int(0)};
    node = ff_node(mod, "$sr", sr);
    TEST_ASSERT_NOT_EQUAL(0, node.v);
    TEST_ASSERT_EQUAL_UINT32(SR_PINS, odin3_node_pins(mod, node).count);
}

/* Bad polarities, widths and values are refused with nothing created. */
static void ffcell_verify(odin3_module *mod) {
    const odin3_value aldff[3] = {odin3_value_int(1), odin3_value_int(2), odin3_value_int(1)};
    expect_ff_refused(mod, "$aldff", aldff);
    const odin3_value dlatch[2] = {odin3_value_int(0), odin3_value_int(1)};
    expect_ff_refused(mod, "$dlatch", dlatch);
    const odin3_value adlatch[4] = {odin3_value_int(FF_WIDE), odin3_value_int(1),
                                    odin3_value_int(1), bits_of(k_bits3, FF_WIDE - 1)};
    expect_ff_refused(mod, "$adlatch", adlatch);
    TEST_ASSERT_EQUAL_STRING("cell parameter ARST_VALUE must be 4 bits", first_error);
    const odin3_value sdffce[5] = {odin3_value_int(SR_WIDTH), odin3_value_int(1),
                                   odin3_value_int(1), odin3_value_int(1), odin3_value_int(0)};
    expect_ff_refused(mod, "$sdffce", sdffce);
    const odin3_value sdffce_ok[5] = {odin3_value_int(SR_WIDTH), odin3_value_int(1),
                                      odin3_value_int(1), odin3_value_int(1),
                                      bits_of(k_bits2, SR_WIDTH)};
    TEST_ASSERT_NOT_EQUAL(0, ff_node(mod, "$sdffce", sdffce_ok).v);
}

static void test_ffcell_pins_and_verify(void) {
    odin3_module *mod = new_module("m");
    ffcell_pins(mod);
    ffcell_verify(mod);
}

/* simulate is NULL for all eleven: the 1E build refuses a $dlatch with the hookless-cell text. */
static void test_ffcell_no_sim(void) {
    odin3_module *mod = new_module("m");
    odin3_node_id node = ff_node(mod, "$dlatch", NULL);
    TEST_ASSERT_NOT_EQUAL(0, node.v);
    odin3_sim *sim = NULL;
    last_error[0] = '\0';
    TEST_ASSERT_EQUAL(ODIN3_ERR_INVALID_ARG,
                      odin3_sim_build(design, odin3_module_id_of(mod), &sim));
    TEST_ASSERT_NULL(sim);
    char want[LOG_TEXT];
    (void)snprintf(want, sizeof want,
                   "cannot simulate `$dlatch` (node `#%u`): the cell type has no simulate hook",
                   node.v);
    TEST_ASSERT_EQUAL_STRING(want, last_error);
}

/* qsort comparator over an array of NAME_TEXT-byte names. */
static int name_cmp(const void *lhs, const void *rhs) {
    return strcmp((const char *)lhs, (const char *)rhs);
}

/* Adds name to the first *count entries of names unless it is already there. */
static void add_name(char (*names)[NAME_TEXT], uint32_t *count, const char *name) {
    for (uint32_t i = 0; i < *count; i++) {
        if (strcmp(names[i], name) == 0) {
            return;
        }
    }
    TEST_ASSERT_LESS_THAN_UINT32(MAX_INT_NAMES, *count);
    size_t len = strlen(name);
    TEST_ASSERT_LESS_THAN_UINT32(NAME_TEXT, (uint32_t)len);
    memcpy(names[*count], name, len + 1);
    (*count)++;
}

/* The sorted set of INT parameter names over every built-in word and bit cell. */
static uint32_t registry_int_names(char (*names)[NAME_TEXT]) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < odin3_builtin_celltype_count; i++) {
        const odin3_celltype_def *def = odin3_builtin_celltypes[i];
        if (def->gran != ODIN3_GRAN_WORD && def->gran != ODIN3_GRAN_BIT) {
            continue;
        }
        for (uint32_t j = 0; j < def->n_params; j++) {
            if (def->params[j].kind == ODIN3_VAL_INT) {
                add_name(names, &count, def->params[j].name);
            }
        }
    }
    qsort(names, count, NAME_TEXT, name_cmp);
    return count;
}

/* The lines of the file, newline stripped; fails on an over-long line or too many lines. */
static uint32_t file_int_names(char (*names)[NAME_TEXT]) {
    FILE *file = fopen(ODIN3_INT_PARAMS_FILE, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, ODIN3_INT_PARAMS_FILE);
    uint32_t count = 0;
    char line[NAME_TEXT + 2];
    while (fgets(line, (int)sizeof line, file) != NULL) {
        size_t len = strcspn(line, "\n");
        TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(NAME_TEXT, (uint32_t)len, line);
        TEST_ASSERT_LESS_THAN_UINT32(MAX_INT_NAMES, count);
        line[len] = '\0';
        memcpy(names[count], line, len + 1);
        count++;
    }
    (void)fclose(file);
    return count;
}

/* 2E plan point (o): proc-diff's INT list (tools/proc-check/int_params.txt, one name per line,
 * sorted) is exactly the set of INT parameter names of the built-in word and bit cells, so the
 * .param normalisation cannot drift from the registry. */
static void test_int_params_file(void) {
    static char want[MAX_INT_NAMES][NAME_TEXT];
    static char got[MAX_INT_NAMES][NAME_TEXT];
    uint32_t n_want = registry_int_names(want);
    uint32_t n_got = file_int_names(got);
    TEST_ASSERT_GREATER_THAN_UINT32(0, n_want);
    for (uint32_t i = 0; i < n_want && i < n_got; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want[i], got[i], "int_params.txt line");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(n_want, n_got, "int_params.txt line count");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_builtin_has_expected_shape);
    RUN_TEST(test_yosys_parameter_names);
    RUN_TEST(test_sample_widths);
    RUN_TEST(test_pmux_b_width_is_width_times_s_width);
    RUN_TEST(test_mem_port_widths);
    RUN_TEST(test_mem_abits_zero_accepted);
    RUN_TEST(test_verify_rejects_bad_parameters);
    RUN_TEST(test_sop_verify);
    RUN_TEST(test_sop_const_value);
    RUN_TEST(test_names_of_other_types);
    RUN_TEST(test_verify_rejects_more_types);
    RUN_TEST(test_port_width_overflow_is_rejected);
    RUN_TEST(test_nine_cells_appended_in_order);
    RUN_TEST(test_nine_cells_yosys_names);
    RUN_TEST(test_nine_cells_hooks);
    RUN_TEST(test_pow_node_pins);
    RUN_TEST(test_shiftx_is_not_simulated);
    RUN_TEST(test_ffcell_table);
    RUN_TEST(test_ffcell_pins_and_verify);
    RUN_TEST(test_ffcell_no_sim);
    RUN_TEST(test_int_params_file);
    return UNITY_END();
}
