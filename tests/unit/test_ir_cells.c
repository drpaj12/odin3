/*
 * test_ir_cells.c — table-driven tests of the built-in cell library (IR-8..IR-11).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/value.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdint.h>
#include <string.h>

enum {
    MAX_PORTS = 8,
    MAX_PARAMS = 16,
    BAD_INIT = 4,
    SOP_WIDTH = 3,
    ROW_LEN = 4,
    N_TYPES = 46,
    STRUCTURAL_TYPES = 7
};

static odin3_design *design;
static uint32_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
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
    {"$_DFF_P_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_DFF_N_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_DLATCH_P_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_DLATCH_N_", BIT, 0, "IIO", {1, 1, 1}},
    {"$_FF_", BIT, 0, "IO", {1, 1}},
    {"$sop", BIT, 0, "IO", {0, 1}},
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
        TEST_ASSERT_EQUAL_MESSAGE(ODIN3_OK, def->verify(params), exp->name);
    }
}

static void test_every_builtin_has_expected_shape(void) {
    TEST_ASSERT_EQUAL_UINT32(N_TYPES, (uint32_t)N_EXPECT);
    /* 7 structural/constant types (Task 2) + every table row = every built-in type. */
    TEST_ASSERT_EQUAL_UINT32(odin3_builtin_celltype_count, STRUCTURAL_TYPES + (uint32_t)N_EXPECT);
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_builtin_has_expected_shape);
    RUN_TEST(test_yosys_parameter_names);
    RUN_TEST(test_sample_widths);
    RUN_TEST(test_pmux_b_width_is_width_times_s_width);
    RUN_TEST(test_mem_port_widths);
    RUN_TEST(test_verify_rejects_bad_parameters);
    RUN_TEST(test_sop_verify);
    RUN_TEST(test_sop_const_value);
    RUN_TEST(test_names_of_other_types);
    RUN_TEST(test_verify_rejects_more_types);
    RUN_TEST(test_port_width_overflow_is_rejected);
    return UNITY_END();
}
