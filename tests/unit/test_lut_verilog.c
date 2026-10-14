/*
 * test_lut_verilog.c — write_verilog's $lut form (3D plan Task 9, spec §2 point (q)): an
 * `assign` over the irredundant cover of the table (3E's odin3_blif_isop), inputs A[0] .. A[k-1]
 * in pin order. The exact forms of AND, NOT, XOR, OR3 and a constant are asserted as text; every
 * written expression is also evaluated against its table in C. The same module (16 random tables
 * per width 1 to 6) is written to ODIN3_LUT_OUT/luts.v with a testbench luts_tb.v, the inputs of
 * CTest lut_verilog_icarus (iverilog + vvp print PASS).
 */
#include "backends/verilog/writer.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "ir/value.h"
#include "sim/prng.h"
#include "unity.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_LUT_OUT
#error "ODIN3_LUT_OUT must name the directory luts.v is written to"
#endif

enum {
    TEXT_MAX = 1 << 18,
    PATH_MAX_LEN = 512,
    NAME_MAX_LEN = 32,
    MAX_IN = 6,
    TABLES_PER_WIDTH = 16,
    SEED = 0x3D9
};

static odin3_design *design;
static char file_buf[TEXT_MAX];

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
}

/* --- building ------------------------------------------------------------------------------ */

static uint32_t intern(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(name), &str));
    return str;
}

static odin3_net_id add_scalar_port(odin3_module *mod, const char *name, odin3_dir dir) {
    odin3_port_spec spec = {intern(name), dir, 1, true, (odin3_prov_id){0}, false, 0, 0, false};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &spec, &node));
    return odin3_pin_net(mod, odin3_node_pins(mod, node).first);
}

/* Table bytes (LSB first) from text written MSB first, as Verilog's N'b... */
static uint32_t table_from_msb(const char *msb_first, uint8_t *bits) {
    uint32_t len = (uint32_t)strlen(msb_first);
    for (uint32_t i = 0; i < len; i++) {
        bits[i] = (uint8_t)(msb_first[len - 1 - i] == '1' ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    return len;
}

/* One $lut of the given width and table (bits, 2^width of them) from the nets in[0..width). */
static void add_lut(odin3_module *mod, uint32_t width, const uint8_t *bits,
                    const odin3_net_id *in, odin3_net_id out) {
    odin3_celltype_id type = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(design, intern("$lut"), &type));
    odin3_value params[2] = {odin3_value_int(width),
                             {.kind = ODIN3_VAL_BITS, .bits = bits, .len = 1U << width}};
    odin3_netvec ports[2] = {{in, width}, {&out, 1}};
    odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, params, 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &spec, ports, NULL));
}

static void check_full_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NETLIST};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

static void out_path(char *path, const char *name) {
    int len = snprintf(path, PATH_MAX_LEN, "%s/%s", ODIN3_LUT_OUT, name);
    TEST_ASSERT_TRUE(len > 0 && len < PATH_MAX_LEN);
}

static const char *slurp(const char *path) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t len = fread(file_buf, 1, TEXT_MAX - 1, file);
    TEST_ASSERT_TRUE(len < TEXT_MAX - 1);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    file_buf[len] = '\0';
    return file_buf;
}

/* --- reading the expression back ----------------------------------------------------------- */

typedef struct expr_env {
    const char *const *names; /* input names, input j = names[j] */
    uint32_t inputs;
    uint32_t index; /* the input assignment: input j = bit j of it */
} expr_env;

static bool literal_value(const expr_env *env, const char *lit, size_t len) {
    bool negate = len > 0 && lit[0] == '~';
    if (negate) {
        lit++;
        len--;
    }
    bool value = false;
    if (len == 4 && strncmp(lit, "1'b1", len) == 0) {
        value = true;
    } else if (len == 4 && strncmp(lit, "1'b0", len) == 0) {
        value = false;
    } else {
        bool found = false;
        for (uint32_t j = 0; j < env->inputs; j++) {
            if (strlen(env->names[j]) == len && strncmp(env->names[j], lit, len) == 0) {
                value = ((env->index >> j) & 1U) != 0;
                found = true;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, lit);
    }
    return negate ? !value : value;
}

/* One cube: literals joined by " & ", optionally in parentheses. */
static bool cube_value(const expr_env *env, const char *cube, size_t len) {
    if (len >= 2 && cube[0] == '(' && cube[len - 1] == ')') {
        cube++;
        len -= 2;
    }
    bool value = true;
    size_t pos = 0;
    while (pos < len) {
        const char *amp = strstr(cube + pos, " & ");
        size_t end = (amp != NULL && (size_t)(amp - cube) < len) ? (size_t)(amp - cube) : len;
        value = literal_value(env, cube + pos, end - pos) && value;
        pos = end < len ? end + 3 : len;
    }
    return value;
}

/* The value of `<sum>` or `~(<sum>)`, the sum being cubes joined by " | ". */
static bool expr_value(const expr_env *env, const char *expr, size_t len) {
    bool negate = len > 3 && strncmp(expr, "~(", 2) == 0 && expr[len - 1] == ')' &&
                  strstr(expr + 2, "(") == NULL;
    if (negate) {
        expr += 2;
        len -= 3;
    }
    bool value = false;
    size_t pos = 0;
    while (pos < len) {
        const char *bar = strstr(expr + pos, " | ");
        size_t end = (bar != NULL && (size_t)(bar - expr) < len) ? (size_t)(bar - expr) : len;
        value = cube_value(env, expr + pos, end - pos) || value;
        pos = end < len ? end + 3 : len;
    }
    return negate ? !value : value;
}

/* The right-hand side of `  assign <out> = <rhs>;` in text, into rhs; its length. */
static size_t rhs_of(const char *text, const char *out, char *rhs, size_t cap) {
    char key[NAME_MAX_LEN + 16];
    int klen = snprintf(key, sizeof key, "\n  assign %s = ", out);
    TEST_ASSERT_TRUE(klen > 0 && (size_t)klen < sizeof key);
    const char *at = strstr(text, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, key);
    at += klen;
    const char *semi = strchr(at, ';');
    TEST_ASSERT_NOT_NULL(semi);
    size_t len = (size_t)(semi - at);
    TEST_ASSERT_TRUE(len < cap);
    memcpy(rhs, at, len);
    rhs[len] = '\0';
    return len;
}

/* Every input assignment of the written expression gives the table's bit. */
static void expect_function(const char *rhs, size_t len, const char *const *names, uint32_t width,
                            const uint8_t *bits) {
    expr_env env = {names, width, 0};
    for (uint32_t i = 0; i < (1U << width); i++) {
        env.index = i;
        bool want = bits[i] == ODIN3_BIT_1;
        TEST_ASSERT_EQUAL_MESSAGE(want, expr_value(&env, rhs, len), rhs);
    }
}

/* --- the exact forms ----------------------------------------------------------------------- */

static const char *const k_abc[3] = {"a", "b", "c"};

/* Module m with inputs a, b, c (the first `width` used) and output y, one $lut; its written text. */
static const char *write_one(uint32_t width, const char *msb_first, uint8_t *bits) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("m"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_net_id in[3];
    for (uint32_t j = 0; j < 3; j++) {
        in[j] = add_scalar_port(mod, k_abc[j], ODIN3_DIR_IN);
    }
    odin3_net_id out = add_scalar_port(mod, "y", ODIN3_DIR_OUT);
    TEST_ASSERT_EQUAL_UINT32(1U << width, table_from_msb(msb_first, bits));
    add_lut(mod, width, bits, in, out);
    check_full_ok();
    char path[PATH_MAX_LEN];
    out_path(path, "one.v");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, path, NULL));
    return slurp(path);
}

static void expect_line(const char *text, const char *line) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, line), line);
}

static void test_and(void) {
    uint8_t bits[4];
    expect_line(write_one(2, "1000", bits), "\n  assign y = a & b;\n");
}

static void test_not_on_set_wins_the_tie(void) {
    uint8_t bits[2];
    expect_line(write_one(1, "01", bits), "\n  assign y = ~a;\n");
}

static void test_xor_two_cubes(void) {
    uint8_t bits[4];
    const char *text = write_one(2, "0110", bits);
    char rhs[128];
    size_t len = rhs_of(text, "y", rhs, sizeof rhs);
    /* two cubes, each in parentheses, joined by " | " (~?a and ~?b in each) */
    TEST_ASSERT_EQUAL_CHAR('(', rhs[0]);
    TEST_ASSERT_NOT_NULL(strstr(rhs, ") | ("));
    TEST_ASSERT_EQUAL_CHAR(')', rhs[len - 1]);
    TEST_ASSERT_NOT_NULL(strstr(rhs, "a"));
    TEST_ASSERT_NOT_NULL(strstr(rhs, "b"));
    TEST_ASSERT_NULL(strstr(rhs, "c"));
    expect_function(rhs, len, k_abc, 2, bits);
}

static void test_or3_off_set(void) {
    uint8_t bits[8];
    expect_line(write_one(3, "11111110", bits), "\n  assign y = ~(~a & ~b & ~c);\n");
}

static void test_constants(void) {
    uint8_t bits[4];
    expect_line(write_one(2, "1111", bits), "\n  assign y = 1'b1;\n");
}

static void test_constant_zero(void) {
    uint8_t bits[4];
    expect_line(write_one(2, "0000", bits), "\n  assign y = 1'b0;\n");
}

/* --- random tables, evaluated here and (CTest lut_verilog_icarus) by Icarus ----------------- */

static const char *const k_names[MAX_IN] = {"i0", "i1", "i2", "i3", "i4", "i5"};

static void random_table(odin3_prng *prng, uint32_t width, uint8_t *bits) {
    for (uint32_t i = 0; i < (1U << width); i++) {
        bits[i] = (uint8_t)(odin3_prng_bit(prng) ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
}

static void out_name(char *name, uint32_t width, uint32_t num) {
    int len = snprintf(name, NAME_MAX_LEN, "o%u_%u", (unsigned)width, (unsigned)num);
    TEST_ASSERT_TRUE(len > 0 && len < NAME_MAX_LEN);
}

/* The testbench: every input, every output against its table (hex, LSB = LUT[0]). */
static void testbench_line(odin3_strbuf *tb, uint32_t width, uint32_t num, const uint8_t *bits) {
    char name[NAME_MAX_LEN];
    out_name(name, width, num);
    uint64_t word = 0;
    for (uint32_t i = 0; i < (1U << width); i++) {
        word |= (uint64_t)(bits[i] == ODIN3_BIT_1 ? 1U : 0U) << i;
    }
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(tb,
                                       "    if (%s !== t(64'h%llx, idx[%u:0])) begin errors = errors "
                                       "+ 1; $display(\"%s differs at %%0d\", idx); end\n",
                                       name, (unsigned long long)word, (unsigned)width - 1U, name));
}

static void test_random_tables_text_and_icarus(void) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_module_create(design, intern("luts"), (odin3_prov_id){0}, &mid));
    odin3_module *mod = odin3_module_get(design, mid);
    odin3_net_id in[MAX_IN];
    for (uint32_t j = 0; j < MAX_IN; j++) {
        in[j] = add_scalar_port(mod, k_names[j], ODIN3_DIR_IN);
    }
    odin3_prng prng;
    odin3_prng_seed(&prng, SEED);
    static uint8_t tables[MAX_IN + 1][TABLES_PER_WIDTH][1U << MAX_IN];
    odin3_strbuf tb;
    odin3_strbuf_init(&tb);
    for (uint32_t width = 1; width <= MAX_IN; width++) {
        for (uint32_t num = 0; num < TABLES_PER_WIDTH; num++) {
            char name[NAME_MAX_LEN];
            out_name(name, width, num);
            odin3_net_id out = add_scalar_port(mod, name, ODIN3_DIR_OUT);
            random_table(&prng, width, tables[width][num]);
            add_lut(mod, width, tables[width][num], in, out);
            testbench_line(&tb, width, num, tables[width][num]);
        }
    }
    check_full_ok();
    char path[PATH_MAX_LEN];
    out_path(path, "luts.v");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_verilog_write(design, path, NULL));
    const char *text = slurp(path);
    for (uint32_t width = 1; width <= MAX_IN; width++) {
        for (uint32_t num = 0; num < TABLES_PER_WIDTH; num++) {
            char name[NAME_MAX_LEN];
            char rhs[2048];
            out_name(name, width, num);
            size_t len = rhs_of(text, name, rhs, sizeof rhs);
            expect_function(rhs, len, k_names, width, tables[width][num]);
        }
    }
    odin3_strbuf bench;
    odin3_strbuf_init(&bench);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(
            &bench,
            "module luts_tb;\n  reg [5:0] idx;\n  integer errors;\n  integer n;\n"
            "  wire i0 = idx[0], i1 = idx[1], i2 = idx[2], i3 = idx[3], i4 = idx[4], i5 = idx[5];\n"
            "  function t(input [63:0] table, input [5:0] at);\n    t = table[at];\n  endfunction\n"));
    for (uint32_t width = 1; width <= MAX_IN; width++) {
        for (uint32_t num = 0; num < TABLES_PER_WIDTH; num++) {
            char name[NAME_MAX_LEN];
            out_name(name, width, num);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&bench, "  wire %s;\n", name));
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&bench, "  luts dut(.i0(i0), .i1(i1), "
                                                                 ".i2(i2), .i3(i3), .i4(i4), "
                                                                 ".i5(i5)"));
    for (uint32_t width = 1; width <= MAX_IN; width++) {
        for (uint32_t num = 0; num < TABLES_PER_WIDTH; num++) {
            char name[NAME_MAX_LEN];
            out_name(name, width, num);
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_strbuf_appendf(&bench, ", .%s(%s)", name, name));
        }
    }
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(&bench, ");\n  initial begin\n    errors = 0;\n"
                                               "    for (n = 0; n < 64; n = n + 1) begin\n"
                                               "      idx = n;\n      #1;\n"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&bench, (odin3_bytes){tb.data, tb.len}));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(&bench, "    end\n    if (errors == 0) $display(\"PASS\");\n"
                                     "    else $display(\"FAIL %%0d\", errors);\n"
                                     "    $finish;\n  end\nendmodule\n"));
    out_path(path, "luts_tb.v");
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_UINT(bench.len, fwrite(bench.data, 1, bench.len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    odin3_strbuf_free(&bench);
    odin3_strbuf_free(&tb);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_and);
    RUN_TEST(test_not_on_set_wins_the_tie);
    RUN_TEST(test_xor_two_cubes);
    RUN_TEST(test_or3_off_set);
    RUN_TEST(test_constants);
    RUN_TEST(test_constant_zero);
    RUN_TEST(test_random_tables_text_and_icarus);
    return UNITY_END();
}
