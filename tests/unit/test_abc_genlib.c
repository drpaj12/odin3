/*
 * test_abc_genlib.c — the genlib text of an .o3lib library (passes/abc/genlib.h; gatemap plan
 * Task 4): the goldens of lib/cmos6.o3lib and lib/generic_gates.o3lib, delay texts, the library
 * checks and the stable SHA-256.
 */
#include "ir/design.h"
#include "passes/abc/genlib.h"
#include "techlib/libcells.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/sha256.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

#if !defined(ODIN3_LIB_DIR) || !defined(ODIN3_GENLIB_DIR)
#error "ODIN3_LIB_DIR and ODIN3_GENLIB_DIR must be defined"
#endif

enum { MSG_MAX = 1024, TEXT_MAX = 1 << 16 };

static odin3_design *g_design;
static odin3_libcells g_cells;
static odin3_strbuf g_text;
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
    g_cells = (odin3_libcells){0};
    odin3_strbuf_init(&g_text);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_strbuf_free(&g_text);
    odin3_libcells_free(&g_cells);
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(g_design);
    g_design = NULL;
}

static void load_file(const char *name) {
    char path[MSG_MAX];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_LIB_DIR, name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(g_design, path), g_msg);
}

static void load_text(const char *text) {
    odin3_techlib_text src = {"inline.o3lib", odin3_bytes_cstr(text)};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read_text(g_design, &src), g_msg);
}

static void collect(const char *library) {
    odin3_libcells_free(&g_cells);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_libcells_collect(g_design, library, &g_cells));
}

/* The genlib text of library (collected, checked, derived) into g_text. */
static void derive(const char *library) {
    collect(library);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_abc_genlib_check(g_design, &g_cells, library),
                                  g_msg);
    odin3_strbuf_clear(&g_text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_abc_genlib_text(g_design, &g_cells, &g_text));
}

static const char *slurp(const char *name, char *buf) {
    char path[MSG_MAX];
    (void)snprintf(path, sizeof path, "%s/%s", ODIN3_GENLIB_DIR, name);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t len = fread(buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    buf[len] = '\0';
    return buf;
}

static void expect_golden(const char *name) {
    static char want[TEXT_MAX];
    TEST_ASSERT_EQUAL_STRING(slurp(name, want), g_text.data);
}

static void test_genlib_cmos6_golden(void) {
    load_file("cmos6.o3lib");
    derive("cmos6");
    expect_golden("cmos6.genlib");
}

static void test_genlib_generic_golden(void) {
    load_file("generic_gates.o3lib");
    derive("generic_gates");
    expect_golden("generic_gates.genlib");
}

static void test_genlib_delay_text(void) {
    load_text("library t\n"
              "cell INV gate area 1 delay 2.5 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n"
              "cell AND2 gate area 1.5 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = A & B ; end\n");
    derive("t");
    TEST_ASSERT_EQUAL_STRING("GATE INV 1 Y=!A;\n"
                             "PIN A UNKNOWN 1 999 2.5 0 2.5 0\n"
                             "GATE AND2 1.5 Y=A*B;\n"
                             "PIN A UNKNOWN 1 999 1 0 1 0\n"
                             "PIN B UNKNOWN 1 999 1 0 1 0\n"
                             "GATE $odin3_const0 0 Y=CONST0;\n"
                             "GATE $odin3_const1 0 Y=CONST1;\n",
                             g_text.data);
}

/* A library and the one ERROR odin3_abc_genlib_check logs for it. */
typedef struct check_case {
    const char *library;
    const char *want;
} check_case;

/* check on the case's library: ODIN3_ERR_INVALID_ARG with the one ERROR want. */
static void expect_check_error(check_case want) {
    collect(want.library);
    g_errors = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_abc_genlib_check(g_design, &g_cells, want.library));
    TEST_ASSERT_EQUAL_UINT(1, g_errors);
    TEST_ASSERT_EQUAL_STRING(want.want, g_msg);
}

static void test_genlib_check_errors(void) {
    expect_check_error((check_case){"nolib",
                                    "abc: --gates: library 'nolib' is not loaded; read it with "
                                    "read_techlib first"});
    load_text("library noarea\n"
              "cell INV1 gate area 1 ; in A 1 ; out Y 1 ; fn Y = ~A ; end\n"
              "cell AND2X gate ; in A 1 ; in B 1 ; out Y 1 ; fn Y = A & B ; end\n");
    expect_check_error((check_case){"noarea",
                                    "abc: --gates: library 'noarea': cell 'AND2X' has no numeric "
                                    "area"});
    load_text("library nandonly\n"
              "cell NAND2N gate area 1 ; in A 1 ; in B 1 ; out Y 1 ; fn Y = ~(A & B) ; end\n");
    expect_check_error((check_case){"nandonly",
                                    "abc: --gates: library 'nandonly' has no inverter and 2-input "
                                    "AND, NAND, OR or NOR gate; ABC cannot map to it"});
}

static void test_genlib_sha256_stable(void) {
    load_file("generic_gates.o3lib");
    derive("generic_gates");
    char first[ODIN3_SHA256_HEX_CHARS];
    odin3_abc_genlib_sha256(&g_text, first);
    derive("generic_gates");
    char second[ODIN3_SHA256_HEX_CHARS];
    odin3_abc_genlib_sha256(&g_text, second);
    TEST_ASSERT_EQUAL_size_t(ODIN3_SHA256_HEX_CHARS - 1, strlen(first));
    TEST_ASSERT_EQUAL_STRING(first, second);
    TEST_ASSERT_EQUAL_size_t(strlen(first), strspn(first, "0123456789abcdef"));
}

static void test_genlib_text_out_of_memory(void) {
    load_file("generic_gates.o3lib");
    collect("generic_gates");
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_abc_genlib_text(g_design, &g_cells, &g_text));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_genlib_cmos6_golden);
    RUN_TEST(test_genlib_generic_golden);
    RUN_TEST(test_genlib_delay_text);
    RUN_TEST(test_genlib_check_errors);
    RUN_TEST(test_genlib_sha256_stable);
    RUN_TEST(test_genlib_text_out_of_memory);
    return UNITY_END();
}
