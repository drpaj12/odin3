/*
 * test_elab_stage1_params.c — 2D Task 11: parameters (EL-8, EL-16), specializations and their
 * names (EL-9), passes E and W (§3.4), defparam and project push-down, the recursion and cap
 * errors (point (aa)), the store's shape and the decision-log replay (§16 #4). Sources are parsed
 * by 2C (elab_test.h); names and values are read from the elaborated store, and every located
 * diagnostic is asserted as the whole line.
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "frontends/verilog/elab1.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ODIN3_LIB_DIR
#error "ODIN3_LIB_DIR must name lib/"
#endif
#ifndef ODIN3_PROJECT_FIXTURES
#error "ODIN3_PROJECT_FIXTURES must name tests/golden/projects"
#endif

#define PENDING "elaboration is not available yet (2D)"

enum { OOM_LIMIT = 400000, OOM_SETTLE = 64, NAME_BUF = 512, FILE_BUF = 4096 };

/* The module every precedence case instantiates (Review Focus 1). */
#define M_W "module m #(parameter W = 1) (input [W-1:0] d); endmodule\n"

static et_result rs;
static odin3_arena *values;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
    values = odin3_arena_create(0);
    TEST_ASSERT_NOT_NULL(values);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_elab_test_set_hooks(NULL);
    odin3_elab_test_set_limits(NULL);
    odin3_pass_options opts = odin3_pass_get_options();
    opts.top = NULL;
    odin3_pass_set_options(opts);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
    odin3_arena_destroy(values);
}

/* --- running ------------------------------------------------------------------------------- */

static void set_cli_top(const char *top) {
    odin3_pass_options opts = odin3_pass_get_options();
    opts.top = top;
    odin3_pass_set_options(opts);
}

static void run_one(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
}

/* A run that must succeed: OK, no error, the store passes check FULL and binds (EL-5). */
static void run_clean(const char *text) {
    run_one(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    et_expect_binds(&rs);
}

/* A run with the project file p.o3proj (its text; read by the pre-script) over t.v. */
static void run_project(const et_src *proj, const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, "read_project p.o3proj", proj, NULL};
    et_stage1(&req, &rs);
}

static et_src project_file(const char *text) {
    return (et_src){"p.o3proj", text};
}

/* --- reading the elaborated store ---------------------------------------------------------- */

static const char *name_of(odin3_ast_id node) {
    return odin3_ast_name_str(rs.elab, node);
}

/* The first node of kind named name under root (the whole store when root is 0), else 0. */
static odin3_ast_id find_in(odin3_ast_id root, odin3_ast_kind kind, const char *name) {
    uint32_t end = odin3_ast_node_end(rs.elab).v;
    odin3_ast_parent_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_parent_index_build(rs.elab, &ix));
    odin3_ast_id found = {0};
    for (uint32_t id = 1; id < end && found.v == 0; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(rs.elab, node) != kind || strcmp(name_of(node), name) != 0) {
            continue;
        }
        odin3_ast_id up = node;
        while (root.v != 0 && up.v != 0 && up.v != root.v) {
            up = odin3_ast_parent(&ix, up);
        }
        found = root.v == 0 || up.v == root.v ? node : found;
    }
    free(ix.parents);
    return found;
}

static odin3_ast_id module_named(const char *name) {
    TEST_ASSERT_NOT_NULL_MESSAGE(rs.elab, et_log(&rs));
    return find_in((odin3_ast_id){0}, ODIN3_AST_MODULE, name);
}

/* The name of the INSTANTIATION holding instance inst (its specialization's). */
static const char *spec_of_instance(const char *inst) {
    TEST_ASSERT_NOT_NULL_MESSAGE(rs.elab, et_log(&rs));
    odin3_ast_id node = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANCE, inst);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, inst);
    odin3_ast_parent_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_parent_index_build(rs.elab, &ix));
    odin3_ast_id holder = odin3_ast_parent(&ix, node);
    free(ix.parents);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_INSTANTIATION, odin3_ast_kind_of(rs.elab, holder));
    return name_of(holder);
}

/* The UNIT's MODULE names, comma separated, in order. */
static const char *module_order(void) {
    static char buf[NAME_BUF];
    size_t len = 0;
    buf[0] = '\0';
    odin3_ast_span mods = odin3_ast_children(rs.elab, odin3_ast_root(rs.elab, 0));
    for (uint32_t i = 0; i < mods.n; i++) {
        int got =
            snprintf(buf + len, sizeof buf - len, "%s%s", i > 0 ? "," : "", name_of(mods.ids[i]));
        TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof buf - len);
        len += (size_t)got;
    }
    return buf;
}

/* The value node of the DECLARATOR named param inside module (a NUMBER or STRING). */
static odin3_ast_id value_of(const char *module, const char *param) {
    odin3_ast_id mod = module_named(module);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, mod.v, module);
    odin3_ast_id decl = find_in(mod, ODIN3_AST_DECLARATOR, param);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, decl.v, param);
    return odin3_ast_child(rs.elab, decl, 0);
}

/* A NUMBER's value as an integer (it must be known and fit). */
static int64_t number_int(odin3_ast_id node) {
    odin3_ast_literal lit;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_value(rs.elab, node, values, &lit));
    odin3_cval val = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_literal(values, &lit, &val));
    int64_t out = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&val, &out));
    return out;
}

/* A NUMBER's spelling: its sub, its SIGNED flag and its payload width (point (bb)). */
static void expect_number(odin3_ast_id node, uint32_t sub, bool is_signed, uint32_t width) {
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NUMBER, odin3_ast_kind_of(rs.elab, node));
    TEST_ASSERT_EQUAL_UINT32(sub, odin3_ast_sub(rs.elab, node));
    TEST_ASSERT_EQUAL(is_signed, (odin3_ast_flags(rs.elab, node) & ODIN3_AST_F_NUMBER_SIGNED) != 0);
    TEST_ASSERT_EQUAL_UINT32(width, odin3_ast_number_of(rs.elab, node)->width);
}

/* Asserts the error text at level is the only error (besides any listed elsewhere). */
static void expect_only_error(const char *line) {
    et_expect(&rs, ODIN3_LOG_ERROR, line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_NULL(rs.elab);
}

/* --- precedence (Review Focus 1, EL-8) ----------------------------------------------------- */

static void test_named_beats_defparam(void) {
    run_one(M_W "module top; m #(.W(5)) u1 ();\n"
                "defparam u1.W = 7;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("m$W=5", spec_of_instance("u1"));
    et_expect(&rs, ODIN3_LOG_WARN,
              "t.v:3:10: warning: defparam of 'u1.W' is overridden by the named parameter "
              "assignment of 'u1'");
    TEST_ASSERT_EQUAL_UINT32(1, rs.warnings);
}

static void test_first_defparam_wins(void) {
    run_clean(M_W "module top; m u3 ();\ndefparam u3.W = 8; defparam u3.W = 9;\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$W=8", spec_of_instance("u3"));
    TEST_ASSERT_EQUAL_UINT32(0, rs.warnings);
}

/* Divergence 7: a defparam beats an ordered value (Yosys rejects the pair). */
static void test_defparam_beats_ordered(void) {
    run_clean(M_W "module top; m #(4) u4 ();\ndefparam u4.W = 6;\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$W=6", spec_of_instance("u4"));
}

static void test_project_beats_all(void) {
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.u1.W 3\n");
    run_project(&proj, M_W "module top; m #(.W(5)) u1 ();\ndefparam u1.W = 7;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("m$W=3", spec_of_instance("u1"));
}

/* An override equal to the default shares the base module (Yosys would make a $paramod). */
static void test_default_value_shares_base(void) {
    run_clean(M_W "module top; m u5 (); m #(.W(1)) u6 ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m", spec_of_instance("u5"));
    TEST_ASSERT_EQUAL_STRING("m", spec_of_instance("u6"));
    TEST_ASSERT_EQUAL_STRING("top,m", module_order());
}

/* --- sharing by resolved value (EL-9) ------------------------------------------------------ */

static void test_integer_shares(void) {
    run_clean("module m #(parameter integer W = 1) (); endmodule\n"
              "module top; m #(.W(8)) a (); m #(.W(32'sd8)) b ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$W=8", spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING("m$W=8", spec_of_instance("b"));
    TEST_ASSERT_EQUAL_STRING("top,m$W=8", module_order());
}

static void test_untyped_differs(void) {
    run_clean("module m #(parameter W = 1) (); endmodule\n"
              "module top; m #(.W(8)) a (); m #(.W(4'd8)) b ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$W=8", spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING("m$W=4'b1000", spec_of_instance("b"));
}

/* --- names (EL-9, point (q)) --------------------------------------------------------------- */

static void test_string_name(void) {
    run_clean("module m #(parameter S = \"ab\") (); endmodule\n"
              "module top; m #(.S(\"x\\\"y\")) u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$S=\"x\\\"y\"", spec_of_instance("u"));
    odin3_ast_id value = value_of("m$S=\"x\\\"y\"", "S");
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_STRING, odin3_ast_kind_of(rs.elab, value));
    TEST_ASSERT_EQUAL_STRING("x\"y", name_of(value));
}

static void test_signed_ranged_name(void) {
    run_clean("module m #(parameter signed [7:0] P = 0) (); endmodule\n"
              "module top; m #(.P(-16)) u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$P=8'sb11110000", spec_of_instance("u"));
}

static void test_unsized_name(void) {
    run_clean("module m #(parameter P = 0) (); endmodule\n"
              "module top; m #(.P('hFFFFFFFF)) u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$P=32'b11111111111111111111111111111111", spec_of_instance("u"));
}

/* Review I2 (Yosys 0.55): every override is sized, its sign kept (Yosys passes it as an
 * RTLIL::Const), so `'hFFFFFFFF` and `32'hFFFFFFFF` overrides share and are written BIN width 32;
 * only a default stays unsized. */
static void test_unsized_overrides_sized(void) {
    run_clean("module m #(parameter P = 'hFFFFFFFF) (); endmodule\n"
              "module top; m #(.P('hFFFFFFFF)) a (); m #(.P(32'hFFFFFFFF)) b (); m #('h7) c ();\n"
              "m d ();\ndefparam d.P = 'h3;\nm e ();\nendmodule\n");
    const char *ones = "m$P=32'b11111111111111111111111111111111";
    TEST_ASSERT_EQUAL_STRING(ones, spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING(ones, spec_of_instance("b"));
    expect_number(value_of(ones, "P"), ODIN3_AST_NUMBER_BIN, false, 32);
    expect_number(value_of("m$P=32'b00000000000000000000000000000111", "P"), ODIN3_AST_NUMBER_BIN,
                  false, 32); /* ordered */
    expect_number(value_of("m$P=32'b00000000000000000000000000000011", "P"), ODIN3_AST_NUMBER_BIN,
                  false, 32); /* defparam */
    TEST_ASSERT_EQUAL_STRING("m", spec_of_instance("e"));
    expect_number(value_of("m", "P"), ODIN3_AST_NUMBER_HEX, false, 0); /* the default */
}

/* Review I2: an x or z top bit also extends differently unsized (repeated) and sized (0), so a
 * default `'hx0` and an override `32'hxxxxxxx0` are two specializations (Yosys 0.55: 64'hx…x0
 * and 64'h00000000xxxxxxx0); a signed unsized default and a sized override of it share. */
static void test_unsized_key_boundary(void) {
    run_clean("module n #(parameter P = 'hx0) (); endmodule\n"
              "module s #(parameter W = 8) (); endmodule\n"
              "module x #(parameter X = 'sbx) (); endmodule\n"
              "module top; n a (); n #(.P(32'hxxxxxxx0)) b (); s c (); s #(.W(32'sd8)) d ();\n"
              "x e (); x #(.X(32'sbx)) f ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("n", spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING("n$P=32'bxxxxxxxxxxxxxxxxxxxxxxxxxxxx0000", spec_of_instance("b"));
    TEST_ASSERT_EQUAL_STRING("s", spec_of_instance("c"));
    TEST_ASSERT_EQUAL_STRING("s", spec_of_instance("d"));
    TEST_ASSERT_EQUAL_STRING("x", spec_of_instance("e")); /* signed: x repeats either way */
    TEST_ASSERT_EQUAL_STRING("x", spec_of_instance("f"));
}

/* Review m5 (T05): the z plane is part of the key: 4'b0z00 and 4'b0000 do not share. */
static void test_key_holds_z(void) {
    run_clean("module m #(parameter P = 0) (); endmodule\n"
              "module top; m #(.P(4'b0z00)) a (); m #(.P(4'b0000)) b ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$P=4'b0z00", spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING("m$P=4'b0000", spec_of_instance("b"));
}

/* The name of the instance of m whose string S is len zeros. */
static const char *name_with_string(int len) {
    char text[NAME_BUF * 2];
    int got = snprintf(text, sizeof text,
                       "module m #(parameter S = \"s\") (); endmodule\n"
                       "module top; m #(.S(\"%0*d\")) u ();\nendmodule\n",
                       len, 0);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof text);
    run_clean(text);
    return spec_of_instance("u");
}

/* Review m5 (T10): a name of 128 bytes is kept, one of 129 is hashed. */
static void test_name_length_boundary(void) {
    char want[NAME_BUF];
    int got = snprintf(want, sizeof want, "m$S=\"%0*d\"", 122, 0);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof want);
    const char *name = name_with_string(122);
    TEST_ASSERT_EQUAL_size_t(128, strlen(name));
    TEST_ASSERT_EQUAL_STRING(want, name);
    et_free(&rs);
    TEST_ASSERT_EQUAL_size_t(3 + 16, strlen(name_with_string(123)));
}

static void test_long_name_hashed(void) {
    run_clean("module m #(parameter P = 0) (); endmodule\n"
              "module top; m #(.P(200'h1)) u ();\nendmodule\n");
    const char *name = spec_of_instance("u");
    TEST_ASSERT_EQUAL_size_t(3 + 16, strlen(name));
    TEST_ASSERT_EQUAL_INT(0, strncmp(name, "m$h", 3));
    for (size_t i = 3; i < strlen(name); i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            isxdigit((unsigned char)name[i]) && !isupper((unsigned char)name[i]), name);
    }
}

/* A user module of the name already: the specialization takes `$2` (point (q)). */
static void test_name_taken_by_module(void) {
    run_clean(M_W "module \\m$W=5 (); endmodule\n"
                  "module top; m #(.W(5)) u1 (); \\m$W=5  u0 ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$W=5$2", spec_of_instance("u1"));
    TEST_ASSERT_EQUAL_STRING("m$W=5", spec_of_instance("u0"));
    TEST_ASSERT_EQUAL_STRING("top,m$W=5$2,m$W=5", module_order());
}

/* A registered cell type of the name (a BLIF black box): `$2` too. */
static void test_name_taken_by_cell_type(void) {
    static const et_src srcs[] = {
        {"c.blif", ".model blk\n.inputs a\n.outputs y\n.subckt m$W=6 a=a y=y\n.end\n\n"
                   ".model m$W=6\n.inputs a\n.outputs y\n.blackbox\n.end\n"},
        {"t.v", M_W "module top; m #(.W(6)) u ();\nendmodule\n"},
    };
    et_req req = {srcs, 2, "read_blif c.blif", NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("m$W=6$2", spec_of_instance("u"));
}

/* --- errors (§9.1) -------------------------------------------------------------------------- */

static void test_localparam_override(void) {
    run_one("module m #(parameter W = 1) (); localparam L = 2; endmodule\n"
            "module top; m #(.L(1)) u ();\nendmodule\n");
    expect_only_error(
        "t.v:2:17: error: 'L' is a localparam of module 'm' and cannot be overridden");
}

static void test_unknown_parameter(void) {
    run_one(M_W "module top; m #(.X(1)) u ();\nendmodule\n");
    expect_only_error("t.v:2:17: error: module 'm' has no parameter 'X'");
}

static void test_too_many_values(void) {
    run_one("module m #(parameter A = 1, B = 2) (); endmodule\n"
            "module top; m #(1, 2, 3) u ();\nendmodule\n");
    expect_only_error("t.v:2:23: error: too many parameter values for module 'm' (3 given, 2 "
                      "parameters)");
}

static void test_mixed_values(void) {
    run_one(M_W "module top; m #(1, .W(2)) u ();\nendmodule\n");
    expect_only_error(
        "t.v:2:20: error: parameter assignment of instance 'u' mixes ordered and named values");
}

/* --- dependencies (review C3 (a), EL-16) --------------------------------------------------- */

static void test_parameter_reads_later_one(void) {
    run_clean("module top #(parameter A = B + 1, B = 2) ();\nendmodule\n");
    TEST_ASSERT_EQUAL_INT64(3, number_int(value_of("top", "A")));
    TEST_ASSERT_EQUAL_INT64(2, number_int(value_of("top", "B")));
}

static void test_parameter_cycle(void) {
    run_one("module top #(parameter A = B, B = A) ();\nendmodule\n");
    expect_only_error("t.v:1:24: error: parameter 'A' depends on itself");
}

static void test_parameter_reads_net(void) {
    run_one("module top;\n  wire w;\n  parameter A = w;\nendmodule\n");
    expect_only_error("t.v:3:17: error: parameter 'A' is not constant (it reads 'w')");
}

/* --- EL-16 types and point (bb)'s spellings ------------------------------------------------ */

static void test_types_and_spellings(void) {
    run_clean("module top #(parameter [7:0] P = 9'h1FF, parameter signed S = 4'b1000,\n"
              "  parameter W = 1) ();\n"
              "  localparam [31:0] Q = 'hFFFFFFFF;\n"
              "  localparam [35:0] R = 'hFFFFFFFF;\n"
              "endmodule\n");
    odin3_ast_id pv = value_of("top", "P");
    expect_number(pv, ODIN3_AST_NUMBER_BIN, false, 8);
    TEST_ASSERT_EQUAL_INT64(0xFF, number_int(pv));
    expect_number(value_of("top", "S"), ODIN3_AST_NUMBER_BIN, true, 4);
    TEST_ASSERT_EQUAL_INT64(-8, number_int(value_of("top", "S")));
    expect_number(value_of("top", "Q"), ODIN3_AST_NUMBER_HEX, false, 0);
    expect_number(value_of("top", "W"), ODIN3_AST_NUMBER_DEC, true, 0);
    expect_number(value_of("top", "R"), ODIN3_AST_NUMBER_BIN, false, 36);
    TEST_ASSERT_EQUAL_INT64(0xFFFFFFFF, number_int(value_of("top", "R")));
    odin3_ast_id params = odin3_ast_child(rs.elab, module_named("top"), 0);
    odin3_ast_id first = odin3_ast_child(rs.elab, params, 0);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_PARAM_DECL_LOCALPARAM, odin3_ast_sub(rs.elab, first));
    TEST_ASSERT_TRUE((odin3_ast_flags(rs.elab, first) & ODIN3_AST_F_PARAM_DECL_IN_HEADER) != 0);
}

static void test_real_parameter(void) {
    run_one("module top;\n  parameter real r = 1.0;\nendmodule\n");
    expect_only_error("t.v:2:3: error: parameter type 'real' is not supported");
}

/* --- declarations (EL-23: folded ranges) --------------------------------------------------- */

static void test_declaration_range(void) {
    run_clean("module m #(parameter W = 1) (); wire [W-1:0] w; endmodule\n"
              "module top; m #(.W(5)) u ();\nendmodule\n");
    odin3_ast_id mod = module_named("m$W=5");
    odin3_ast_id net = find_in(mod, ODIN3_AST_DECLARATOR, "w");
    odin3_ast_parent_index ix;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_parent_index_build(rs.elab, &ix));
    odin3_ast_id decl = odin3_ast_parent(&ix, net);
    free(ix.parents);
    odin3_ast_id range = odin3_ast_child(rs.elab, decl, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_RANGE, odin3_ast_kind_of(rs.elab, range));
    odin3_ast_id msb = odin3_ast_child(rs.elab, range, 0);
    odin3_ast_id lsb = odin3_ast_child(rs.elab, range, 1);
    expect_number(msb, ODIN3_AST_NUMBER_DEC, true, 32);
    expect_number(lsb, ODIN3_AST_NUMBER_DEC, true, 32);
    TEST_ASSERT_EQUAL_INT64(4, number_int(msb));
    TEST_ASSERT_EQUAL_INT64(0, number_int(lsb));
}

/* --- push-down (EL-8) ---------------------------------------------------------------------- */

static void test_defparam_push_down(void) {
    run_clean("module leaf #(parameter W = 1) (); endmodule\n"
              "module mid; leaf u_leaf (); endmodule\n"
              "module top; mid u_mid ();\ndefparam u_mid.u_leaf.W = 4;\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("mid$u_leaf.W=4", spec_of_instance("u_mid"));
    TEST_ASSERT_EQUAL_STRING("leaf$W=4", spec_of_instance("u_leaf"));
}

/* Two overrides of one deeper path: the first defparam wins, and a project line beats both
 * (one pending entry per path, the highest precedence). */
static void test_push_down_precedence(void) {
    static const char text[] = "module leaf #(parameter W = 1) (); endmodule\n"
                               "module mid; leaf u_leaf (); endmodule\n"
                               "module top; mid u_mid ();\n"
                               "defparam u_mid.u_leaf.W = 4; defparam u_mid.u_leaf.W = 5;\n"
                               "endmodule\n";
    run_clean(text);
    TEST_ASSERT_EQUAL_STRING("mid$u_leaf.W=4", spec_of_instance("u_mid"));
    TEST_ASSERT_EQUAL_STRING("leaf$W=4", spec_of_instance("u_leaf"));
    et_free(&rs);
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.u_mid.u_leaf.W 6\n");
    run_project(&proj, text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("leaf$W=6", spec_of_instance("u_leaf"));
}

static void test_defparam_upward(void) {
    run_one("module leaf #(parameter W = 1) (); endmodule\n"
            "module mid; leaf u (); defparam top.u.W = 1; endmodule\n"
            "module top; mid u_mid ();\nendmodule\n");
    expect_only_error("t.v:2:33: error: defparam target 'top.u.W' does not name a parameter of an "
                      "instance below this module");
}

static void test_defparam_own_parameter(void) {
    run_one("module top #(parameter W = 1) ();\ndefparam W = 2;\nendmodule\n");
    expect_only_error("t.v:2:10: error: defparam target 'W' does not name a parameter of an "
                      "instance below this module");
    et_free(&rs);
    run_one(M_W "module top; m u ();\ndefparam u = 2;\nendmodule\n"); /* an instance alone */
    expect_only_error("t.v:3:10: error: defparam target 'u' does not name a parameter of an "
                      "instance below this module");
}

static const char k_wide[] = "module leaf #(parameter W = 1) (); endmodule\n"
                             "module wide; leaf u (); endmodule\n"
                             "module top; wide u_wide ();\nendmodule\n";

static void test_project_push_down(void) {
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.u_wide.u.W 8'hFF\n");
    run_project(&proj, k_wide);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("wide$u.W=8'b11111111", spec_of_instance("u_wide"));
    TEST_ASSERT_EQUAL_STRING("leaf$W=8'b11111111", spec_of_instance("u"));
    /* The top keeps its definition's name under overrides (v4.7, Yosys chparam). */
    TEST_ASSERT_EQUAL_STRING("top", name_of(odin3_ast_root(rs.elab, 0)));
    TEST_ASSERT_EQUAL_STRING("top,wide$u.W=8'b11111111,leaf$W=8'b11111111", module_order());
}

static void test_project_bad_parameter(void) {
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.u_wide.u.W 8'hFF\n"
                               "param top.u_wide.X 1\n");
    run_project(&proj, k_wide);
    expect_only_error("p.o3proj:4:1: error: param 'top.u_wide.X' does not name a parameter");
}

/* The corpus case param_instance: `param top.u_wide.W 4` as its ref.cmd's chparam. */
static void read_fixture(const char *rel, char *buf, size_t cap) {
    char path[FILE_BUF];
    int got = snprintf(path, sizeof path, "%s/param_instance/%s", ODIN3_PROJECT_FIXTURES, rel);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof path);
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t len = fread(buf, 1, cap - 1, file);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_TRUE(len < cap - 1);
    buf[len] = '\0';
}

static void test_corpus_param_instance(void) {
    static char inv[FILE_BUF];
    static char top[FILE_BUF];
    static char proj[FILE_BUF];
    read_fixture("src/inv.v", inv, sizeof inv);
    read_fixture("src/top.v", top, sizeof top);
    read_fixture("project.o3proj", proj, sizeof proj);
    et_src srcs[] = {{"src/inv.v", inv}, {"src/top.v", top}};
    et_src project = {"p.o3proj", proj};
    et_req req = {srcs, 2, "read_project p.o3proj", &project, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.elab, ODIN3_CHECK_FULL));
    TEST_ASSERT_EQUAL_STRING("sub$W=4", spec_of_instance("u_wide"));
    TEST_ASSERT_EQUAL_STRING("inv$W=2", spec_of_instance("u_narrow"));
    TEST_ASSERT_EQUAL_STRING("inv$W=4", spec_of_instance("u_inv"));
    TEST_ASSERT_EQUAL_INT64(4, number_int(value_of("sub$W=4", "W")));
    TEST_ASSERT_EQUAL_INT64(4, number_int(value_of("inv$W=4", "W")));
    TEST_ASSERT_EQUAL_INT64(2, number_int(value_of("inv$W=2", "W")));
}

/* --- recursion and caps (point (aa)) ------------------------------------------------------- */

static void test_recursion(void) {
    set_cli_top("ping");
    run_one("module ping; pong u (); endmodule\nmodule pong; ping v (); endmodule\n");
    expect_only_error("t.v:2:19: error: recursive instantiation of 'ping' (through 'pong')");
}

static void test_depth_cap(void) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_hier_depth = 4;
    odin3_elab_test_set_limits(&lim);
    run_one("module a0; a1 u (); endmodule\nmodule a1; a2 u (); endmodule\n"
            "module a2; a3 u (); endmodule\nmodule a3; a4 u (); endmodule\n"
            "module a4; a5 u (); endmodule\nmodule a5; endmodule\n");
    expect_only_error("t.v:5:15: error: instance hierarchy deeper than 4 at 'u'; recursive "
                      "instantiation of 'a4'?");
}

static void test_specialization_cap(void) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_specializations = 2;
    odin3_elab_test_set_limits(&lim);
    run_one(M_W "module top;\n  m u1 ();\n  m u2 ();\n  m #(1) u3 ();\n"
                "  m #(.W(2)) u4 ();\n  m #(3) u5 ();\nendmodule\n");
    expect_only_error("t.v:6:3: error: elaboration makes more than 2 specializations");
}

static void test_node_cap(void) {
    odin3_elab_limits lim = *odin3_elab_limits_get();
    lim.max_elab_nodes = 50;
    odin3_elab_test_set_limits(&lim);
    run_one(M_W "module top;\n  wire [7:0] a, b, c, d, e, f, g, h;\n"
                "  assign a = b + c + d + e + f + g + h;\n  assign b = c - d - e - f - g - h;\n"
                "  assign c = d ^ e ^ f ^ g ^ h;\n  m #(2) u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_ERROR,
                                              "error: elaboration needs more than 50 elaborated "
                                              "nodes"));
}

/* --- the store's shape (EL-4) -------------------------------------------------------------- */

static void test_store_shape(void) {
    run_clean("module c; endmodule\nmodule d; endmodule\n"
              "module a #(parameter X = 1, Y = 2) (); c uc (); endmodule\n"
              "module b; d ud (); endmodule\n"
              "module top; a #(.Y(7)) ua (); b ub ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("top", name_of(odin3_ast_root(rs.elab, 0)));
    TEST_ASSERT_EQUAL_STRING("top,a$Y=7,b,c,d", module_order()); /* breadth first */
    odin3_ast_id inst = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANTIATION, "a$Y=7");
    odin3_ast_id list = odin3_ast_child(rs.elab, inst, 0);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_nchild(rs.elab, list));
    odin3_ast_id px = odin3_ast_child(rs.elab, list, 0);
    odin3_ast_id py = odin3_ast_child(rs.elab, list, 1);
    TEST_ASSERT_EQUAL_STRING("X", name_of(px));
    TEST_ASSERT_EQUAL_STRING("Y", name_of(py));
    TEST_ASSERT_EQUAL_INT64(1, number_int(odin3_ast_child(rs.elab, px, 0)));
    TEST_ASSERT_EQUAL_INT64(7, number_int(odin3_ast_child(rs.elab, py, 0)));
    TEST_ASSERT_EQUAL_UINT32(
        0, odin3_ast_child(rs.elab, find_in((odin3_ast_id){0}, ODIN3_AST_INSTANTIATION, "b"), 0)
               .v); /* no parameters: no LIST */
}

/* ANSI ports: one PORT_DECL{IN_HEADER} per declarator; an untyped net port is `wire` (the NET
 * field 1 + wire), a `reg` port keeps DT=reg and no net kind (EL-4, EL-24). */
static void test_ansi_ports(void) {
    run_clean("module top (input a, b, output reg [3:0] q, inout wire c);\nendmodule\n");
    odin3_ast_id ports = odin3_ast_child(rs.elab, module_named("top"), 1);
    TEST_ASSERT_EQUAL_UINT32(4, odin3_ast_nchild(rs.elab, ports));
    uint32_t net_one = (1U + ODIN3_AST_NET_DECL_WIRE) << ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    for (uint32_t i = 0; i < 4; i++) {
        odin3_ast_id port = odin3_ast_child(rs.elab, ports, i);
        uint32_t flags = odin3_ast_flags(rs.elab, port);
        TEST_ASSERT_TRUE((flags & ODIN3_AST_F_PORT_DECL_IN_HEADER) != 0);
        TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_nchild(rs.elab, port)); /* RANGE?, one DECLARATOR */
        uint32_t net = flags & (uint32_t)ODIN3_AST_F_PORT_DECL_NET_MASK;
        TEST_ASSERT_EQUAL_UINT32(i == 2 ? 0 : net_one, net);
    }
    uint32_t q_flags = odin3_ast_flags(rs.elab, odin3_ast_child(rs.elab, ports, 2));
    uint32_t dt = (q_flags & (uint32_t)ODIN3_AST_F_PORT_DECL_DT_MASK) >>
                  (uint32_t)ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DT_REG, dt);
    TEST_ASSERT_EQUAL_STRING(
        "b", name_of(odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, ports, 1), 1)));
}

/* A replication count is folded to a NUMBER (point (bb)); a negative one is an error. */
static void test_replication_count(void) {
    run_clean("module top #(parameter N = 2) (input a, output [1:0] y);\n"
              "  assign y = {N{a}};\nendmodule\n");
    odin3_ast_id rep = find_in((odin3_ast_id){0}, ODIN3_AST_REPLICATE, "");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, rep.v);
    odin3_ast_id count = odin3_ast_child(rs.elab, rep, 0);
    expect_number(count, ODIN3_AST_NUMBER_DEC, true, 32);
    TEST_ASSERT_EQUAL_INT64(2, number_int(count));
    et_free(&rs);
    run_one("module top (input a, output [1:0] y);\n  assign y = {-1{a}};\nendmodule\n");
    expect_only_error("t.v:2:15: error: replication count -1 is negative");
}

/* Ports by name in port order, the unconnected one without an expression (EL-4, EL-10). */
static void test_named_connections(void) {
    run_clean("module m (input a, input [1:0] b, output y); endmodule\n"
              "module top; wire p, q; m u (.y(q), .a(p));\nendmodule\n");
    odin3_ast_id inst = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANCE, "u");
    TEST_ASSERT_EQUAL_UINT32(4, odin3_ast_nchild(rs.elab, inst));
    TEST_ASSERT_EQUAL_STRING("a", name_of(odin3_ast_child(rs.elab, inst, 1)));
    TEST_ASSERT_EQUAL_STRING("b", name_of(odin3_ast_child(rs.elab, inst, 2)));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, inst, 2), 0).v);
    TEST_ASSERT_EQUAL_STRING("y", name_of(odin3_ast_child(rs.elab, inst, 3)));
}

/* --- the decision log (§16 #4) ------------------------------------------------------------- */

static void test_replay_drop(void) {
    odin3_elab_test_hooks hooks = {.drop_log_entry = 1};
    odin3_elab_test_set_hooks(&hooks);
    run_one("module m; leaf a (); leaf b (); endmodule\nmodule leaf; endmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, rs.status);
    TEST_ASSERT_NULL(rs.elab);
    et_expect(&rs, ODIN3_LOG_ERROR, "stage 1: decision log of 'm' not replayed (1 of 2 entries)");
}

/* --- the C re-entry cap through stage 1's host (ceval carry) ------------------------------- */

/* "module top; localparam P0 = P1 + 1; … localparam P<links> = 0;" */
static const char *chain(int links) {
    odin3_strbuf sb;
    odin3_strbuf_init(&sb);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&sb, odin3_bytes_cstr("module top;\n")));
    for (int k = 0; k < links; k++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(&sb, "localparam P%d = P%d + 1;\n", k, k + 1));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(&sb, "localparam P%d = 0;\nendmodule\n", links));
    char *text = odin3_arena_strndup(values, sb.data, sb.len);
    odin3_strbuf_free(&sb);
    TEST_ASSERT_NOT_NULL(text);
    return text;
}

static void test_reentry_cap_through_stage1(void) {
    run_clean(chain(256));
    TEST_ASSERT_EQUAL_INT64(256, number_int(value_of("top", "P0")));
    et_free(&rs);
    run_one(chain(257));
    expect_only_error("t.v:259:19: error: constant evaluation of parameter 'P257' nested deeper "
                      "than 256");
}

/* --- more of EL-8, EL-9 and pass W -------------------------------------------------------- */

/* A defparam on a localparam has the localparam text, at its target. */
static void test_defparam_localparam(void) {
    run_one("module m (); localparam L = 1; endmodule\nmodule top; m u ();\n"
            "defparam u.L = 2;\nendmodule\n");
    expect_only_error(
        "t.v:3:10: error: 'L' is a localparam of module 'm' and cannot be overridden");
}

/* Ordered values go to the header parameters, then the body's (EL-8 item 4). */
static void test_ordered_header_then_body(void) {
    run_clean("module m #(parameter A = 1) (); parameter B = 2; endmodule\n"
              "module top; m #(5, 6) u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$A=5$B=6", spec_of_instance("u"));
    TEST_ASSERT_EQUAL_INT64(6, number_int(value_of("m$A=5$B=6", "B")));
}

/* The instances of one INSTANTIATION in different specializations: one INSTANTIATION per run of
 * instances sharing one (EL-4: its name is the specialization's). */
static void test_split_instantiation(void) {
    run_clean(M_W "module top; m #(2) a (), b (), c (), d ();\n"
                  "defparam b.W = 3; defparam c.W = 3;\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$W=2", spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING("m$W=3", spec_of_instance("b"));
    TEST_ASSERT_EQUAL_STRING("m$W=3", spec_of_instance("c"));
    TEST_ASSERT_EQUAL_STRING("m$W=2", spec_of_instance("d"));
    odin3_ast_id top = module_named("top");
    TEST_ASSERT_EQUAL_UINT32(2 + 3, odin3_ast_nchild(rs.elab, top));
    odin3_ast_id second = odin3_ast_child(rs.elab, top, 3);
    TEST_ASSERT_EQUAL_UINT32(1 + 2, odin3_ast_nchild(rs.elab, second)); /* LIST, b, c */
}

/* A specialization made twice (two values of N) warns once (point (v)). */
static void test_warn_once_across_specializations(void) {
    run_clean(M_W "module mid #(parameter N = 1) (); m #(.W(5)) u1 ();\n"
                  "defparam u1.W = 7;\nendmodule\n"
                  "module top; mid #(1) a (); mid #(2) b ();\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, rs.warnings);
    TEST_ASSERT_EQUAL_STRING("top,mid,mid$N=2,m$W=5", module_order());
}

static void test_empty_string(void) {
    run_clean("module m #(parameter S = \"ab\") (); endmodule\n"
              "module top; m #(.S(\"\")) u ();\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$S=\"\"", spec_of_instance("u"));
    odin3_ast_id value = value_of("m$S=\"\"", "S");
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_STRING, odin3_ast_kind_of(rs.elab, value));
    TEST_ASSERT_TRUE((odin3_ast_flags(rs.elab, value) & ODIN3_AST_F_STRING_EMPTY) != 0);
}

/* The project overrides the top's own parameters (its name per EL-9), with a negative number and
 * a string. */
static void test_project_top_parameters(void) {
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.W -3\nparam top.S \"q\"\n");
    run_project(&proj, "module top #(parameter W = 1, parameter S = \"s\") ();\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("top", name_of(odin3_ast_root(rs.elab, 0)));
    odin3_ast_id wv = value_of("top", "W");
    TEST_ASSERT_EQUAL_INT64(-3, number_int(wv));
    expect_number(wv, ODIN3_AST_NUMBER_BIN, true, 32); /* sized, the literal's sign kept */
    TEST_ASSERT_EQUAL_STRING("q", name_of(value_of("top", "S")));
}

/* Review m2 and v4.7: a project number keeps its literal's sign (`'s`) and is sized (an unsized
 * one included), so it shares with the sized `#()` value. */
static void test_project_number_forms(void) {
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.u.W 4'sb1111\n"
                               "param top.v.W 'hFFFFFFFF\n");
    run_project(&proj, "module p #(parameter W = 1) (); endmodule\n"
                       "module top; p u (); p v (); p #(.W(32'hFFFFFFFF)) x ();\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("p$W=4'sb1111", spec_of_instance("u"));
    TEST_ASSERT_EQUAL_STRING("p$W=32'b11111111111111111111111111111111", spec_of_instance("v"));
    TEST_ASSERT_EQUAL_STRING(spec_of_instance("v"), spec_of_instance("x"));
}

static void test_connection_errors(void) {
    run_one("module m (input a, output y); endmodule\nmodule top; wire p;\n  m u (.q(p));\n"
            "endmodule\n");
    expect_only_error("t.v:3:8: error: module 'm' has no port 'q'");
    et_free(&rs);
    run_one("module m (input a, output y); endmodule\nmodule top; wire p;\n  m u (.a(p), .a(p));\n"
            "endmodule\n");
    expect_only_error("t.v:3:15: error: port 'a' of instance 'u' is connected twice");
}

/* Forms of landed kinds that later tasks land report their kind (point (l)): a hierarchical call
 * (E21, Task 16), a specparam (P7) and a time variable
 * (N17, Task 16), a rejected net type (N15). */
static void test_task16_rows_reported(void) {
    static const char *const cases[][2] = {
        {"module top (output y);\n  assign y = u.f(1);\nendmodule\n",
         "t.v:2:14: error: hierarchical reference 'u.f' is not supported"},
        {"module top;\n  time t;\nendmodule\n",
         "t.v:2:3: error: variable type 'time' is not supported"},
        {"module top;\n  tri0 n;\nendmodule\n", "t.v:2:3: error: net type 'tri0' is not supported"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        run_one(cases[i][0]);
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
        et_expect(&rs, ODIN3_LOG_ERROR, cases[i][1]);
        et_free(&rs);
    }
}

/* Ordered connections and instance arrays landed with Task 12 (they were pending here). */
static void test_task12_forms_landed(void) {
    run_clean("module m (input a); endmodule\nmodule top; wire p;\n  m u (p);\nendmodule\n");
    odin3_ast_id inst = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANCE, "u");
    TEST_ASSERT_EQUAL_STRING("a", name_of(odin3_ast_child(rs.elab, inst, 1)));
    et_free(&rs);
    run_clean("module m (input a); endmodule\nmodule top;\n  m u [1:0] ();\nendmodule\n");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_in((odin3_ast_id){0}, ODIN3_AST_INSTANCE, "u[1]").v);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_in((odin3_ast_id){0}, ODIN3_AST_INSTANCE, "u[0]").v);
}

/* A cell-type instance keeps its parameters as given, values resolved (EL-4); a defparam on it
 * adds one (Task 12, EL-8's cell-type targets). */
static void test_cell_type_as_given(void) {
    et_src src = {"t.v", "module top; wire c;\n"
                         "  single_port_ram #(.ADDR_WIDTH(4'd10)) ram (.clk(c));\nendmodule\n"};
    et_req req = {&src, 1, "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib", NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_STRING("single_port_ram", spec_of_instance("ram"));
    odin3_ast_id inst = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANTIATION, "single_port_ram");
    odin3_ast_id conn = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, inst, 0), 0);
    TEST_ASSERT_EQUAL_STRING("ADDR_WIDTH", name_of(conn));
    TEST_ASSERT_EQUAL_INT64(10, number_int(odin3_ast_child(rs.elab, conn, 0)));
    et_free(&rs);
    src.text = "module top; wire c;\n  single_port_ram ram (.clk(c));\n"
               "defparam ram.DATA_WIDTH = 8;\nendmodule\n";
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    inst = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANTIATION, "single_port_ram");
    conn = odin3_ast_child(rs.elab, odin3_ast_child(rs.elab, inst, 0), 0);
    TEST_ASSERT_EQUAL_STRING("DATA_WIDTH", name_of(conn));
    TEST_ASSERT_EQUAL_INT64(8, number_int(odin3_ast_child(rs.elab, conn, 0)));
}

static void test_self_recursion(void) {
    set_cli_top("r");
    run_one("module r; r u (); endmodule\n");
    expect_only_error("t.v:1:13: error: recursive instantiation of 'r'");
}

/* Parameters that change at every level never close a cycle: the depth cap (1024) stops it. */
static void test_unbounded_recursion(void) {
    set_cli_top("r");
    run_one("module r #(parameter N = 0) (); r #(N + 1) u (); endmodule\n");
    expect_only_error("t.v:1:44: error: instance hierarchy deeper than 1024 at 'u'; recursive "
                      "instantiation of 'r'?");
}

/* odin3_elab1_params directly: the values of every parameter, the deeper overrides returned. */
static void test_params_api(void) {
    run_clean(M_W "module top; m u ();\nendmodule\n");
    odin3_elab1 *e1 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_create(&rs.ctx, rs.parsed, &rs.opts, &e1));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_collect_defs(e1));
    uint32_t def = 0;
    while (strcmp(odin3_elab1_str(e1, odin3_elab1_def(e1, def)->name), "m") != 0) {
        def++;
    }
    odin3_elab_override ovr[2] = {{.source = ODIN3_OVR_NAMED}, {.source = ODIN3_OVR_DEFPARAM}};
    ovr[0].path = et_intern(&rs, "W");
    ovr[1].path = et_intern(&rs, "u\x1fX"); /* the components joined by ODIN3_ELAB_PATH_SEP */
    odin3_cval_int five = {5, 32, true};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_cval_from_i64(values, &five, &ovr[0].value));
    ovr[1].value = ovr[0].value;
    odin3_elab_spec_req req = {def, ovr, 2, 1};
    odin3_cval out[1];
    odin3_elab_override pending[2];
    uint32_t n_pending = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_params(e1, &req, out, pending, &n_pending));
    int64_t got = 0;
    TEST_ASSERT_TRUE(odin3_cval_to_i64(&out[0], &got));
    TEST_ASSERT_EQUAL_INT64(5, got);
    TEST_ASSERT_EQUAL_UINT32(1, n_pending);
    TEST_ASSERT_EQUAL_STRING("u\x1fX", odin3_elab1_str(e1, pending[0].path));
    odin3_elab1_destroy(e1);
}

/* --- fix round 1: the review's survivors and minors -------------------------------------- */

/* I3 (T04): the pending overrides are part of the key: two instances of mid, one with a defparam
 * below it, are two specializations. */
static void test_pending_in_key(void) {
    run_clean("module leaf #(parameter W = 1) (); endmodule\n"
              "module mid; leaf u_leaf (); endmodule\n"
              "module top; mid a (); mid b ();\ndefparam a.u_leaf.W = 4;\nendmodule\n");
    TEST_ASSERT_EQUAL_STRING("mid$u_leaf.W=4", spec_of_instance("a"));
    TEST_ASSERT_EQUAL_STRING("mid", spec_of_instance("b"));
    TEST_ASSERT_EQUAL_STRING("top,mid$u_leaf.W=4,mid,leaf$W=4,leaf", module_order());
}

/* I3 (T16): a pushed-down path whose next component is not an instance is the located error at
 * the defparam, never dropped. */
static void test_pending_bad_component(void) {
    run_one("module leaf #(parameter W = 1) (); endmodule\n"
            "module mid; leaf u_leaf (); endmodule\n"
            "module top; mid u_mid ();\ndefparam u_mid.nope.W = 4;\nendmodule\n");
    expect_only_error("t.v:4:10: error: defparam target 'u_mid.nope.W' does not name a parameter "
                      "of an instance below this module");
}

/* I3 (T23): one CONNECTION per parameter, the body's included, with its final value (EL-4). */
static void test_body_parameter_connections(void) {
    run_clean("module m #(parameter A = 1) (); parameter B = 2; endmodule\n"
              "module top; m #(5) u ();\nendmodule\n");
    odin3_ast_id inst = find_in((odin3_ast_id){0}, ODIN3_AST_INSTANTIATION, "m$A=5");
    odin3_ast_id list = odin3_ast_child(rs.elab, inst, 0);
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_nchild(rs.elab, list));
    odin3_ast_id body = odin3_ast_child(rs.elab, list, 1);
    TEST_ASSERT_EQUAL_STRING("B", name_of(body));
    TEST_ASSERT_EQUAL_INT64(2, number_int(odin3_ast_child(rs.elab, body, 0)));
}

/* I3 (T26): a `+:`/`-:` width is folded to point (bb)'s NUMBER; the base stays as written. */
static void test_indexed_part_select(void) {
    run_clean(
        "module top #(parameter W = 2) (input [7:0] a, input [2:0] i, output [1:0] y,\n"
        "  output [1:0] z);\n  assign y = a[i +: W];\n  assign z = a[i -: W + 0];\nendmodule\n");
    uint32_t end = odin3_ast_node_end(rs.elab).v;
    int seen = 0;
    for (uint32_t id = 1; id < end; id++) {
        odin3_ast_id node = {id};
        uint32_t sub = odin3_ast_sub(rs.elab, node);
        if (odin3_ast_kind_of(rs.elab, node) != ODIN3_AST_SELECT ||
            (sub != ODIN3_AST_SELECT_PART_PLUS && sub != ODIN3_AST_SELECT_PART_MINUS)) {
            continue;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_AST_IDENT,
                              odin3_ast_kind_of(rs.elab, odin3_ast_child(rs.elab, node, 1)));
        odin3_ast_id width = odin3_ast_child(rs.elab, node, 2);
        expect_number(width, ODIN3_AST_NUMBER_DEC, true, 32);
        TEST_ASSERT_EQUAL_INT64(2, number_int(width));
        seen++;
    }
    TEST_ASSERT_EQUAL_INT(2, seen);
}

/* m1: an escaped name holding '.' is one path component (parameter and instance). */
static void test_escaped_dotted_names(void) {
    run_clean("module m #(parameter \\a.b = 1) (); endmodule\n"
              "module top; m #(.\\a.b (3)) u (); m \\v.w  ();\ndefparam \\v.w .\\a.b = 2;\n"
              "endmodule\n");
    TEST_ASSERT_EQUAL_STRING("m$a.b=3", spec_of_instance("u"));
    TEST_ASSERT_EQUAL_STRING("m$a.b=2", spec_of_instance("v.w"));
}

/* m5 (T31, T43): a construct of a module specialized twice reports its error, and its evaluator
 * warning, once (§9.3). */
static void test_reported_once(void) {
    run_one("module m #(parameter N = 1) ();\n  wire w;\n  localparam [63:0] L = {32'd0, 'h1};\n"
            "  localparam K = w;\nendmodule\n"
            "module top; m #(1) a (); m #(2) b ();\nendmodule\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, rs.status);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.errors, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:4:18: error: parameter 'K' is not constant (it reads 'w')");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, rs.warnings, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(
        1, et_count_with(&rs, ODIN3_LOG_WARN, "unsized constant in a concatenation is taken as"));
}

/* m4: the forms of landed kinds that later tasks land have a table of their own; a form not in it
 * is a 2D bug. */
static void test_pending_forms_table(void) {
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_pending_form_count(ODIN3_ELAB_STAGE1)); /* Task 16 */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_elab_pending_form_count(ODIN3_ELAB_STAGE2));
    TEST_ASSERT_FALSE(odin3_elab_pending_form(ODIN3_ELAB_STAGE1, ODIN3_AST_ALWAYS));
    TEST_ASSERT_FALSE(odin3_elab_pending_form(ODIN3_ELAB_STAGE1, ODIN3_AST_CONNECTION));
    TEST_ASSERT_FALSE(odin3_elab_pending_form(0, ODIN3_AST_ALWAYS));
    run_clean(M_W "module top; m u ();\nendmodule\n");
    odin3_elab1 *e1 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab1_create(&rs.ctx, rs.parsed, &rs.opts, &e1));
    odin3_ast_id module = odin3_ast_child(rs.parsed, odin3_ast_root(rs.parsed, 0), 0);
    odin3_elab1_pend_form(e1, module);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_CHECK, odin3_elab1_status(e1));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:1:1: error: stage 1: no elaboration of 'MODULE' (a 2D bug)");
    odin3_elab1_destroy(e1);
}

/* m6: a parameter's declared range names the parameter in its diagnostics. */
static void test_parameter_range_named(void) {
    run_one("module top;\n  wire [3:0] w;\n  parameter [w:0] P = 1;\nendmodule\n");
    expect_only_error(
        "t.v:3:14: error: range bound of parameter 'P' is not constant (it reads 'w')");
}

/* --- out of memory ------------------------------------------------------------------------- */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)msg;
    (void)user;
}

/* One step of the sweep: the allocation limit, the parsed store's node count to keep and the
 * unlimited run's dump an OK run must reproduce (its parameter values and names: review I1). */
typedef struct oom_at {
    long limit;
    uint32_t nodes;
    const odin3_strbuf *want;
} oom_at;

static odin3_status oom_step(const oom_at *at) {
    odin3_ast *elab = NULL;
    odin3_util_set_alloc_fail_after(at->limit);
    odin3_status st = odin3_elab_stage1(&rs.ctx, rs.parsed, &rs.opts, &elab);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    TEST_ASSERT_TRUE(st == ODIN3_OK ? elab != NULL : elab == NULL);
    TEST_ASSERT_EQUAL_UINT32(at->nodes, odin3_ast_node_end(rs.parsed).v);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(elab, ODIN3_CHECK_FULL));
        odin3_strbuf got;
        odin3_strbuf_init(&got);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(elab, odin3_ast_root(elab, 0), &got));
        TEST_ASSERT_EQUAL_STRING(at->want->data, got.data);
        odin3_strbuf_free(&got);
    }
    odin3_ast_destroy(elab);
    return st;
}

/* Every allocation of stage 1 over the parsed store of rs fails in turn until OOM_SETTLE runs
 * in a row succeed; each OK run reproduces rs's dump. Returns the NO_MEMORY runs. */
static int oom_sweep(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    uint32_t nodes = odin3_ast_node_end(rs.parsed).v;
    odin3_log_set_sink(quiet_sink, NULL);
    int failed = 0;
    int settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        oom_at at = {limit, nodes, &rs.dump};
        bool oom = oom_step(&at) == ODIN3_ERR_NO_MEMORY;
        failed += oom ? 1 : 0;
        settled = oom ? 0 : settled + 1;
    }
    TEST_ASSERT_EQUAL_INT(OOM_SETTLE, settled);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_check(rs.parsed, ODIN3_CHECK_FULL));
    return failed;
}

/* Parameters, a defparam pushed down, a project override, the name and the writer: every
 * allocation fails in turn until OOM_SETTLE runs in a row succeed. */
static void test_oom(void) {
    et_src proj = project_file("file verilog t.v\ntop top\nparam top.u_mid.u_leaf.W 5\n");
    run_project(&proj, "module leaf #(parameter W = 1, parameter S = \"s\") (input [W-1:0] d);\n"
                       "  wire [W:0] w;\n  assign w = {2{d[0]}};\nendmodule\n"
                       "module mid #(parameter N = 2) (); leaf #(.W(N)) u_leaf (); endmodule\n"
                       "module top; mid #(3) u_mid (); mid u_two ();\n"
                       "defparam u_two.u_leaf.S = \"t\";\nendmodule\n");
    TEST_ASSERT_TRUE(oom_sweep() > 100);
}

/* The restoring evaluation of `differs` (EL-9's name) failing for want of memory fails the run,
 * not keeps the default's value (review I1): a value wider than an arena chunk makes every
 * evaluation allocate, so the sweep fails that evaluation alone at some step. */
static void test_oom_restoring_evaluation(void) {
    run_clean("module m #(parameter [599999:0] P = 0) (); endmodule\n"
              "module top; m #(.P(1)) u (); endmodule\n");
    TEST_ASSERT_TRUE(oom_sweep() > 0);
}

/* The fix round's tests (review task-11-review.md: I2, I3, m1, m2, m4, m5, m6). */
static void run_fix_round(void) {
    RUN_TEST(test_unsized_overrides_sized);
    RUN_TEST(test_unsized_key_boundary);
    RUN_TEST(test_key_holds_z);
    RUN_TEST(test_name_length_boundary);
    RUN_TEST(test_project_number_forms);
    RUN_TEST(test_pending_in_key);
    RUN_TEST(test_pending_bad_component);
    RUN_TEST(test_body_parameter_connections);
    RUN_TEST(test_indexed_part_select);
    RUN_TEST(test_escaped_dotted_names);
    RUN_TEST(test_reported_once);
    RUN_TEST(test_pending_forms_table);
    RUN_TEST(test_parameter_range_named);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_named_beats_defparam);
    RUN_TEST(test_first_defparam_wins);
    RUN_TEST(test_defparam_beats_ordered);
    RUN_TEST(test_project_beats_all);
    RUN_TEST(test_default_value_shares_base);
    RUN_TEST(test_integer_shares);
    RUN_TEST(test_untyped_differs);
    RUN_TEST(test_string_name);
    RUN_TEST(test_signed_ranged_name);
    RUN_TEST(test_unsized_name);
    RUN_TEST(test_long_name_hashed);
    RUN_TEST(test_name_taken_by_module);
    RUN_TEST(test_name_taken_by_cell_type);
    RUN_TEST(test_localparam_override);
    RUN_TEST(test_unknown_parameter);
    RUN_TEST(test_too_many_values);
    RUN_TEST(test_mixed_values);
    RUN_TEST(test_parameter_reads_later_one);
    RUN_TEST(test_parameter_cycle);
    RUN_TEST(test_parameter_reads_net);
    RUN_TEST(test_types_and_spellings);
    RUN_TEST(test_real_parameter);
    RUN_TEST(test_declaration_range);
    RUN_TEST(test_defparam_push_down);
    RUN_TEST(test_push_down_precedence);
    RUN_TEST(test_defparam_upward);
    RUN_TEST(test_defparam_own_parameter);
    RUN_TEST(test_project_push_down);
    RUN_TEST(test_project_bad_parameter);
    RUN_TEST(test_corpus_param_instance);
    RUN_TEST(test_recursion);
    RUN_TEST(test_depth_cap);
    RUN_TEST(test_specialization_cap);
    RUN_TEST(test_node_cap);
    RUN_TEST(test_store_shape);
    RUN_TEST(test_ansi_ports);
    RUN_TEST(test_replication_count);
    RUN_TEST(test_named_connections);
    RUN_TEST(test_replay_drop);
    RUN_TEST(test_reentry_cap_through_stage1);
    RUN_TEST(test_defparam_localparam);
    RUN_TEST(test_ordered_header_then_body);
    RUN_TEST(test_split_instantiation);
    RUN_TEST(test_warn_once_across_specializations);
    RUN_TEST(test_empty_string);
    RUN_TEST(test_project_top_parameters);
    RUN_TEST(test_connection_errors);
    RUN_TEST(test_task12_forms_landed);
    RUN_TEST(test_task16_rows_reported);
    RUN_TEST(test_cell_type_as_given);
    RUN_TEST(test_self_recursion);
    RUN_TEST(test_unbounded_recursion);
    RUN_TEST(test_params_api);
    run_fix_round();
    RUN_TEST(test_oom);
    RUN_TEST(test_oom_restoring_evaluation);
    return UNITY_END();
}
