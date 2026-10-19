/*
 * test_blif_lib_params.c — `.param` on the `.subckt` of a tech-library cell (1C amendment
 * 2026-10-19, PHASE5 decision 8): the PLAIN writer writes the parameters the reader would not
 * derive, the reader sets them on the node, a second write is identical; the VTR dialect still
 * refuses; a `.param` the cell does not declare, or on another cell, stays an attribute.
 */
#include "backends/blif/vtr.h"
#include "backends/blif/writer.h"
#include "frontends/blif/attrs.h"
#include "frontends/blif/param_text.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "lpmmap_fixture.h"
#include "techlib/reader.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ODIN3_LIB_DIR)
#error "ODIN3_LIB_DIR must be defined"
#endif

enum { MSG_MAX = 1024, TEXT_MAX = 1 << 16 };

static const char *const OUT_PATH = "odin3_libparam_out.blif";
static const char *const OUT2_PATH = "odin3_libparam_out2.blif";
static const char *const IN_PATH = "odin3_libparam_in.blif";

static char g_msg[MSG_MAX];

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        (void)snprintf(g_msg, sizeof g_msg, "%s", msg);
    }
}

void setUp(void) {
    g_msg[0] = '\0';
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    (void)remove(OUT_PATH);
    (void)remove(OUT2_PATH);
    (void)remove(IN_PATH);
}

static char *slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    char *text = calloc(1, TEXT_MAX);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, TEXT_MAX - 1U, fp);
    text[got] = '\0';
    (void)fclose(fp);
    return text;
}

/* Writes text to IN_PATH. */
static void write_input(const char *text) {
    FILE *fp = fopen(IN_PATH, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    (void)fputs(text, fp);
    (void)fclose(fp);
}

/* A design with one lpm_add_sub: 4-bit, direction and representation as given. */
static void build_add(pb_result *res, const char *direction, int pipeline) {
    char text[1024];
    (void)snprintf(text, sizeof text,
                   "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
                   "  wire width 4 output 3 \\y\n  cell \\lpm_add_sub \\u0\n"
                   "    parameter \\lpm_width 4\n    parameter \\lpm_direction \"%s\"\n    "
                   "parameter \\lpm_pipeline %d\n"
                   "    connect \\dataa \\a\n    connect \\datab \\b\n    connect \\result \\y\n"
                   "  end\nend\n",
                   direction, pipeline);
    lm_build_ok(text, res);
}

static odin3_design *read_back(const char *path) {
    odin3_design *design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    char lib[512];
    (void)snprintf(lib, sizeof lib, "%s/lpm.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_techlib_read(design, lib), g_msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_read(design, path), g_msg);
    return design;
}

static odin3_module *top_of(odin3_design *design) {
    uint32_t top = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_top_module(design, &top));
    return odin3_module_get(design, (odin3_module_id){top});
}

/* --- param_text ----------------------------------------------------------------------------- */

static void test_param_text_string_escapes(void) {
    odin3_design *design = odin3_design_create();
    uint32_t id = 0;
    static const char raw[] = "a \"q\" \\ b\xc3\xa9\x01";
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(raw), &id));
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, id, 0};
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_param_format(design, &value, &text));
    TEST_ASSERT_EQUAL_STRING("\"a \\\"q\\\" \\\\ b\\303\\251\\001\"", text.data);
    odin3_strbuf bits;
    odin3_strbuf_init(&bits);
    odin3_value back = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_blif_param_parse(design, ODIN3_VAL_STRING,
                                                 (odin3_bytes){text.data, text.len}, &bits, &back));
    TEST_ASSERT_EQUAL_STRING(raw, odin3_strtab_get(odin3_design_strtab(design), back.str));
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&bits);
    odin3_design_destroy(design);
}

static void test_param_text_int_and_bits(void) {
    odin3_design *design = odin3_design_create();
    odin3_strbuf text;
    odin3_strbuf bits;
    odin3_strbuf_init(&text);
    odin3_strbuf_init(&bits);
    static const int64_t nums[] = {
        0, 1, 9, -1, -2147483647 - 1, 2147483647, 2147483648LL, -3000000000LL};
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        odin3_value value = odin3_value_int(nums[i]);
        odin3_strbuf_clear(&text);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_param_format(design, &value, &text));
        TEST_ASSERT_TRUE(text.len == 32 || text.len == 64);
        odin3_value back = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_param_parse(design, ODIN3_VAL_INT,
                                                               (odin3_bytes){text.data, text.len},
                                                               &bits, &back));
        TEST_ASSERT_EQUAL_INT64(nums[i], back.i);
    }
    odin3_value nine = odin3_value_int(9);
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_param_format(design, &nine, &text));
    TEST_ASSERT_EQUAL_STRING("00000000000000000000000000001001", text.data);
    /* BITS: MSB first over 0 1 x z */
    static const uint8_t lsb_first[] = {ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_Z, ODIN3_BIT_0};
    odin3_value vec = {ODIN3_VAL_BITS, 0, lsb_first, 4, 0, 0};
    odin3_strbuf_clear(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_param_format(design, &vec, &text));
    TEST_ASSERT_EQUAL_STRING("0zx1", text.data);
    odin3_value back = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_param_parse(design, ODIN3_VAL_BITS,
                                                           odin3_bytes_cstr("0zx1"), &bits, &back));
    TEST_ASSERT_EQUAL_UINT32(4, back.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(lsb_first, back.bits, 4);
    odin3_value empty = {ODIN3_VAL_BITS, 0, NULL, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_blif_param_format(design, &empty, &text));
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&bits);
    odin3_design_destroy(design);
}

static void test_param_text_rejects_bad_text(void) {
    odin3_design *design = odin3_design_create();
    odin3_strbuf bits;
    odin3_strbuf_init(&bits);
    odin3_value out = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_PARSE,
        odin3_blif_param_parse(design, ODIN3_VAL_INT, odin3_bytes_cstr("12"), &bits, &out));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_PARSE,
        odin3_blif_param_parse(design, ODIN3_VAL_STRING, odin3_bytes_cstr("abc"), &bits, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_blif_param_parse(design, ODIN3_VAL_STRING,
                                                 odin3_bytes_cstr("\"a\"b\""), &bits, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_blif_param_parse(design, ODIN3_VAL_STRING,
                                                 odin3_bytes_cstr("\"a\\q\""), &bits, &out));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_ERR_PARSE,
        odin3_blif_param_parse(design, ODIN3_VAL_BITS, odin3_bytes_cstr("01-"), &bits, &out));
    odin3_strbuf_free(&bits);
    odin3_design_destroy(design);
}

/* --- the writer and the reader --------------------------------------------------------------- */

/* needle is in text (want) or is not (!want). */
static void expect_text(const char *text, const char *needle, bool want) {
    TEST_ASSERT_EQUAL_MESSAGE(want, strstr(text, needle) != NULL, needle);
}

static void test_writer_writes_underived_params_only(void) {
    pb_result res;
    build_add(&res, "SUB", 3);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(res.design, OUT_PATH), g_msg);
    char *text = slurp(OUT_PATH);
    expect_text(text, ".param lpm_direction \"SUB\"\n", true);
    expect_text(text, ".param lpm_pipeline 00000000000000000000000000000011\n", true);
    expect_text(text, ".param lpm_width", false); /* derived from the formals */
    expect_text(text, ".param lpm_hint", false);  /* equals the library default */
    free(text);
    pb_free(&res);
}

static void test_roundtrip_params_and_second_write_identical(void) {
    pb_result res;
    build_add(&res, "SUB", 3);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_write(res.design, OUT_PATH));
    odin3_design *back = read_back(OUT_PATH);
    odin3_module *top = top_of(back);
    odin3_node_id node = lm_nth(top, "lpm_add_sub", 0);
    TEST_ASSERT_TRUE(node.v != 0);
    TEST_ASSERT_EQUAL_INT64(4, lm_param_int(top, node, "lpm_width"));
    TEST_ASSERT_EQUAL_INT64(3, lm_param_int(top, node, "lpm_pipeline"));
    TEST_ASSERT_EQUAL_STRING("SUB", lm_param_str(top, node, "lpm_direction"));
    TEST_ASSERT_EQUAL_STRING("SIGNED", lm_param_str(top, node, "lpm_representation"));
    TEST_ASSERT_EQUAL_STRING(
        "u0", odin3_strtab_get(odin3_design_strtab(back), odin3_node_name(top, node)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(back, OUT2_PATH), g_msg);
    char *one = slurp(OUT_PATH);
    char *two = slurp(OUT2_PATH);
    TEST_ASSERT_EQUAL_STRING(one, two);
    TEST_ASSERT_NULL(strstr(one, "blif.param"));
    free(one);
    free(two);
    odin3_design_destroy(back);
    pb_free(&res);
}

static void test_default_params_write_no_param_lines(void) {
    pb_result res;
    char text[1024];
    (void)snprintf(text, sizeof text,
                   "module \\m\n  wire width 4 input 1 \\a\n  wire width 4 input 2 \\b\n"
                   "  wire width 4 output 3 \\y\n  cell \\lpm_add_sub \\u0\n"
                   "    parameter \\lpm_width 4\n    connect \\dataa \\a\n    connect \\datab "
                   "\\b\n    connect \\result \\y\n"
                   "  end\nend\n");
    lm_build_ok(text, &res);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_blif_write(res.design, OUT_PATH), g_msg);
    char *out = slurp(OUT_PATH);
    TEST_ASSERT_NULL(strstr(out, ".param"));
    free(out);
    pb_free(&res);
}

static void test_vtr_dialect_still_refuses(void) {
    pb_result res;
    build_add(&res, "SUB", 3);
    odin3_blif_write_opts opts = {.dialect = ODIN3_BLIF_DIALECT_VTR};
    TEST_ASSERT_NOT_EQUAL_INT(ODIN3_OK, odin3_blif_write_with(res.design, OUT_PATH, &opts));
    pb_free(&res);
}

static const char k_sub_in[] =
    ".model m\n.inputs a[0] a[1] b[0] b[1]\n.outputs y[0] y[1]\n"
    ".subckt lpm_add_sub dataa[0]=a[0] dataa[1]=a[1] datab[0]=b[0] datab[1]=b[1] result[0]=y[0] "
    "result[1]=y[1]\n";

static void read_text(const char *extra, odin3_status want) {
    char text[2048];
    (void)snprintf(text, sizeof text, "%s%s.end\n", k_sub_in, extra);
    write_input(text);
    odin3_design *design = odin3_design_create();
    char lib[512];
    (void)snprintf(lib, sizeof lib, "%s/lpm.o3lib", ODIN3_LIB_DIR);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_techlib_read(design, lib));
    TEST_ASSERT_EQUAL_INT_MESSAGE(want, odin3_blif_read(design, IN_PATH), g_msg);
    odin3_design_destroy(design);
}

/* Node has the attribute `key_text` (a full key such as blif.param:x). */
static void expect_attr(odin3_design *design, odin3_module *top, odin3_node_id node,
                        const char *key_text) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE_MESSAGE(
        odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(key_text), &key), key_text);
    TEST_ASSERT_NOT_NULL_MESSAGE(odin3_attr_get(top, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key),
                                 key_text);
}

static void test_reader_sets_known_param_and_keeps_unknown_as_attribute(void) {
    write_input(".model m\n.inputs a[0] a[1] b[0] b[1]\n.outputs y[0] y[1]\n"
                ".subckt lpm_add_sub dataa[0]=a[0] dataa[1]=a[1] datab[0]=b[0] datab[1]=b[1] "
                "result[0]=y[0] result[1]=y[1]\n.cname adder\n.param lpm_direction \"SUB\"\n"
                ".param not_a_param 12\n.attr src \"x.v:3\"\n.end\n");
    odin3_design *design = read_back(IN_PATH);
    odin3_module *top = top_of(design);
    odin3_node_id node = lm_nth(top, "lpm_add_sub", 0);
    TEST_ASSERT_EQUAL_STRING("SUB", lm_param_str(top, node, "lpm_direction"));
    const char *name = odin3_strtab_get(odin3_design_strtab(design), odin3_node_name(top, node));
    TEST_ASSERT_EQUAL_STRING("adder", name);
    expect_attr(design, top, node, ODIN3_BLIF_PARAM_PREFIX "not_a_param");
    expect_attr(design, top, node, ODIN3_BLIF_ATTR_PREFIX "src");
    odin3_design_destroy(design);
}

static void test_reader_errors(void) {
    read_text(".param lpm_direction SUB\n", ODIN3_ERR_PARSE);
    TEST_ASSERT_NOT_NULL(strstr(g_msg, ".param lpm_direction of 'lpm_add_sub': cannot read 'SUB'"));
    read_text(".param lpm_pipeline 12\n", ODIN3_ERR_PARSE);
    TEST_ASSERT_NOT_NULL(strstr(g_msg, "binary integer"));
    read_text(".param lpm_width 00000000000000000000000000000111\n", ODIN3_ERR_PARSE);
    TEST_ASSERT_NOT_NULL(
        strstr(g_msg, ".param lpm_width changes the port widths of 'lpm_add_sub'"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_param_text_string_escapes);
    RUN_TEST(test_param_text_int_and_bits);
    RUN_TEST(test_param_text_rejects_bad_text);
    RUN_TEST(test_writer_writes_underived_params_only);
    RUN_TEST(test_roundtrip_params_and_second_write_identical);
    RUN_TEST(test_default_params_write_no_param_lines);
    RUN_TEST(test_vtr_dialect_still_refuses);
    RUN_TEST(test_reader_sets_known_param_and_keeps_unknown_as_attribute);
    RUN_TEST(test_reader_errors);
    return UNITY_END();
}
