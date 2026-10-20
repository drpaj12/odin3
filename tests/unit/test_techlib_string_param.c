/*
 * test_techlib_string_param.c — `param NAME string "text"` in the .o3lib reader (5.1 L-3).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/value.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

enum { MSG_MAX = 1024 };

static odin3_design *g_design;
static char g_msg[MSG_MAX];
static unsigned g_errors;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
        g_errors++;
    }
}

void setUp(void) {
    g_msg[0] = '\0';
    g_errors = 0;
    odin3_log_set_sink(sink, NULL);
    g_design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(g_design);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(g_design);
    g_design = NULL;
}

static odin3_status read_text(const char *text) {
    const odin3_techlib_text src = {"t.o3lib", odin3_bytes_cstr(text)};
    return odin3_techlib_read_text(g_design, &src);
}

static const odin3_celltype_def *def_of(const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(g_design), odin3_bytes_cstr(name), &str));
    TEST_ASSERT_TRUE(odin3_celltype_find(g_design, str, &id));
    const odin3_celltype_def *def = odin3_celltype_get(g_design, id);
    TEST_ASSERT_NOT_NULL(def);
    return def;
}

static const char *str_default(const odin3_param_def *param) {
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, param->kind);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, param->dflt.kind);
    return odin3_strtab_get(odin3_design_strtab(g_design), param->dflt.str);
}

#define CELL "library L\ncell C blackbox\n"

static void test_string_param_default(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, read_text(CELL "  param operation_mode string \"SINGLE_PORT\"\nend\n"), g_msg);
    const odin3_celltype_def *def = def_of("C");
    TEST_ASSERT_EQUAL_UINT32(1, def->n_params);
    TEST_ASSERT_EQUAL_STRING("operation_mode", def->params[0].name);
    TEST_ASSERT_EQUAL_STRING("SINGLE_PORT", str_default(&def->params[0]));
}

static void test_string_param_with_space(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        read_text(CELL "  param fam string \"Cyclone V\" ; param f2 string \"MAX 10 FPGA\"\nend\n"),
        g_msg);
    const odin3_celltype_def *def = def_of("C");
    TEST_ASSERT_EQUAL_STRING("Cyclone V", str_default(&def->params[0]));
    TEST_ASSERT_EQUAL_STRING("MAX 10 FPGA", str_default(&def->params[1]));
}

static void test_string_param_empty(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, read_text(CELL "  param s string \"\"\nend\n"), g_msg);
    TEST_ASSERT_EQUAL_STRING("", str_default(&def_of("C")->params[0]));
}

static void test_string_param_order_mixed(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, read_text(CELL "  param W int 8\n  param S string \"x\"\n  param D int 3\nend\n"),
        g_msg);
    const odin3_celltype_def *def = def_of("C");
    TEST_ASSERT_EQUAL_UINT32(3, def->n_params);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, def->params[0].kind);
    TEST_ASSERT_EQUAL_INT64(8, def->params[0].dflt.i);
    TEST_ASSERT_EQUAL_STRING("S", def->params[1].name);
    TEST_ASSERT_EQUAL_STRING("x", str_default(&def->params[1]));
    TEST_ASSERT_EQUAL_INT64(3, def->params[2].dflt.i);
}

static void test_string_param_in_width_rejected(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          read_text(CELL "  param S string \"x\"\n  in a S\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "string parameter"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "t.o3lib:4:"));
}

static void test_string_param_unterminated(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param S string \"abc\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "t.o3lib:3:"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "string"));
}

static void test_string_param_not_quoted(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param S string abc\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "t.o3lib:3:"));
}

static void test_string_param_trailing_text(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param S string \"a\" b\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "t.o3lib:3:"));
}

static void test_unknown_kind_message(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param S bits 4\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "expected the parameter kind int or string"));
}

static void test_duplicate_name_with_string(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          read_text(CELL "  param S string \"a\" ; param S int 1\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "duplicate"));
}

static void test_int_param_unchanged(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        read_text("library L\ncell multiply hard\n  param WIDTH int 1\n  in a WIDTH\n"
                  "  in b WIDTH\n  out out 2 * WIDTH\n  fn out = a * b\nend\n"),
        g_msg);
    const odin3_celltype_def *def = def_of("multiply");
    TEST_ASSERT_EQUAL_UINT32(1, def->n_params);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, def->params[0].kind);
    TEST_ASSERT_EQUAL_INT64(1, def->params[0].dflt.i);
}

static void test_string_param_blackbox_cell(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK,
        read_text("library L\ncell BB blackbox\n  param LPM_WIDTH int 1\n"
                  "  param REP string \"UNSIGNED\"\n  in dataa LPM_WIDTH\n  in clock 1 clock\n"
                  "  out result LPM_WIDTH\nend\n"),
        g_msg);
    const odin3_celltype_def *def = def_of("BB");
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def->gran);
    TEST_ASSERT_EQUAL_UINT32(2, def->n_params);
    TEST_ASSERT_EQUAL_STRING("LPM_WIDTH", def->ports[0].width_param);
    TEST_ASSERT_EQUAL_STRING("UNSIGNED", str_default(&def->params[1]));
}

static void test_string_param_oom_sweep(void) {
    static const char text[] = "library L\ncell C blackbox\n  param S string \"Cyclone V\"\nend\n";
    for (int fail_at = 0; fail_at < 200; fail_at++) {
        tearDown();
        setUp();
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = read_text(text);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            TEST_ASSERT_EQUAL_STRING("Cyclone V", str_default(&def_of("C")->params[0]));
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
    }
}

static void test_clock_may_name_a_port(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        ODIN3_OK, read_text(CELL "  in clock 1 clock ; in clock2 1 clock ; out q 1\nend\n"), g_msg);
    const odin3_celltype_def *def = def_of("C");
    TEST_ASSERT_EQUAL_STRING("clock", def->ports[0].name);
    TEST_ASSERT_EQUAL_STRING("clock2", def->ports[1].name);
}

static void test_signed_and_x_stay_reserved(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  in signed 1\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "reserved word"));
}

static void test_clock_is_reserved_for_parameters(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param clock int 4\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "'clock' is a reserved word"));
}

static void test_string_default_with_separator_explains(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param S string \"a;b\"\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "may not hold ';' or '#'"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, read_text(CELL "  param T string \"a#b\"\nend\n"));
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "may not hold ';' or '#'"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_string_param_default);
    RUN_TEST(test_string_param_with_space);
    RUN_TEST(test_string_param_empty);
    RUN_TEST(test_string_param_order_mixed);
    RUN_TEST(test_string_param_in_width_rejected);
    RUN_TEST(test_string_param_unterminated);
    RUN_TEST(test_string_param_not_quoted);
    RUN_TEST(test_string_param_trailing_text);
    RUN_TEST(test_unknown_kind_message);
    RUN_TEST(test_duplicate_name_with_string);
    RUN_TEST(test_int_param_unchanged);
    RUN_TEST(test_string_param_blackbox_cell);
    RUN_TEST(test_string_param_oom_sweep);
    RUN_TEST(test_clock_may_name_a_port);
    RUN_TEST(test_signed_and_x_stay_reserved);
    RUN_TEST(test_clock_is_reserved_for_parameters);
    RUN_TEST(test_string_default_with_separator_explains);
    return UNITY_END();
}
