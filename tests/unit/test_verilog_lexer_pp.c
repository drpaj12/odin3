/*
 * test_verilog_lexer_pp.c — the Verilog lexer's macro- and include-dependent cases (2C spec
 * §2.2, §2.6, Review Focus 2; Task 3's deferral to Task 8): streams from odin3_pp_run, every
 * token's {loc, end} checked against the stream's segment map (the definition of a loc) and
 * traced through the source manager to where its bytes are spelled.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "pp_fixture.h"
#include "unity.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CHAIN_STEPS = 16 };

static odin3_ast *ast;
static odin3_vl_run run;
static odin3_vl_lexer *lx;
static odin3_vp_stream vstream;
static ODIN3_VP_STYPE val;
static odin3_vp_loc loc;

void setUp(void) {
    pp_fixture_set_up();
    ast = NULL;
    lx = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    odin3_vl_run_init(&run, design);
}

void tearDown(void) {
    odin3_vl_close(lx);
    lx = NULL;
    odin3_ast_destroy(ast);
    ast = NULL;
    pp_fixture_tear_down();
}

/* --- helpers --------------------------------------------------------------------------------- */

/* Preprocesses tmp/t.v holding text (shown as t.v) and opens the lexer over its stream. */
static void lex_file(const char *text) {
    char path[PP_PATH_BUF];
    put("t.v", odin3_bytes_cstr(text), path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_path("t.v", path), captured);
    vstream = (odin3_vp_stream){stream.text, stream.stream, id("t.v")};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_open(&run, ast, &vstream, &lx));
}

static int next(void) {
    return odin3_vl_next(lx, &val, &loc);
}

/* The stream offset of the first occurrence of needle. */
static uint32_t off_of(const char *needle) {
    size_t len = strlen(needle);
    const char *text = (const char *)stream.text.ptr;
    for (size_t i = 0; i + len <= stream.text.len; i++) {
        if (memcmp(text + i, needle, len) == 0) {
            return (uint32_t)i;
        }
    }
    TEST_FAIL_MESSAGE(needle);
    return 0;
}

/* The loc of stream offset off by the segment map's definition (0 in a loc-0 segment). */
static uint32_t seg_loc(uint32_t off) {
    uint32_t count = 0;
    const odin3_srcseg_rec *segs = segments(&count);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < count && segs[i].out_offset <= off; i++) {
        found = i;
    }
    TEST_ASSERT_TRUE(found != UINT32_MAX);
    return segs[found].loc == 0 ? 0 : segs[found].loc + (off - segs[found].out_offset);
}

/* The kind of the buffer loc lies in. */
static uint32_t buffer_kind(uint32_t at) {
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(
        odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, (odin3_loc){at}), &info));
    return info.kind;
}

/* Stream bytes [off, off + len). */
typedef struct tok_range {
    uint32_t off, len;
} tok_range;

/* The next token is kind over where: loc is the first byte's loc, end the last byte's loc plus
 * one (2A AST-2), off the offset. */
static void expect_token(int kind, tok_range where) {
    TEST_ASSERT_EQUAL_INT(kind, next());
    TEST_ASSERT_EQUAL_UINT32(seg_loc(where.off), loc.loc.v);
    TEST_ASSERT_EQUAL_UINT32(seg_loc(where.off + where.len - 1) + 1, loc.end.v);
    TEST_ASSERT_EQUAL_UINT32(where.off, loc.off);
}

/* loc chased by spelling steps to its FILE buffer is file:line:col there. */
static void expect_spelled(uint32_t at, const char *file, uint32_t line, uint32_t col) {
    odin3_loc cur = {at};
    for (int i = 0; i < CHAIN_STEPS && buffer_kind(cur.v) != ODIN3_SRCBUF_FILE; i++) {
        cur = odin3_srcman_spelling(sm, cur);
    }
    odin3_srcpos pos;
    TEST_ASSERT_TRUE(odin3_srcman_decode(sm, cur, &pos));
    odin3_srcbuf_info info;
    TEST_ASSERT_TRUE(odin3_srcman_buffer_info(sm, (odin3_srcbuf_id){pos.buffer}, &info));
    TEST_ASSERT_EQUAL_STRING(file, odin3_strtab_get(odin3_design_strtab(design), info.name));
    TEST_ASSERT_EQUAL_UINT32(line, pos.line);
    TEST_ASSERT_EQUAL_UINT32(col, pos.col);
}

/* --- tokens spanning segments ---------------------------------------------------------------- */

/* `define S _y then x`S: one IDENT "x_y" whose end is the last byte's loc in the expansion
 * buffer plus one. */
static void test_ident_across_expansion(void) {
    lex_file("`define S _y\nx`S\n");
    uint32_t off = off_of("x_y");
    expect_token(TOK_IDENT, (tok_range){off, 3});
    TEST_ASSERT_EQUAL_STRING("x_y", odin3_strtab_get(odin3_design_strtab(design), val.u));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_FILE, buffer_kind(loc.loc.v));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_EXPANSION, buffer_kind(loc.end.v - 1));
    expect_spelled(loc.end.v - 1, "t.v", 1, 12);
    TEST_ASSERT_EQUAL_INT(0, next());
}

/* Hex digits from two adjacent macros: one BASED_DIGITS spanning both expansion buffers. */
static void test_based_digits_across_two_macros(void) {
    lex_file("`define H1 A\n`define H2 B\n8'h`H1`H2\n");
    TEST_ASSERT_EQUAL_INT(TOK_UNSIGNED_NUMBER, next());
    TEST_ASSERT_EQUAL_INT(TOK_BASE, next());
    uint32_t off = off_of("AB");
    expect_token(TOK_BASED_DIGITS, (tok_range){off, 2});
    TEST_ASSERT_EQUAL_size_t(2, val.text.len);
    TEST_ASSERT_EQUAL_MEMORY("AB", val.text.ptr, 2);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_EXPANSION, buffer_kind(loc.loc.v));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_EXPANSION, buffer_kind(loc.end.v - 1));
    TEST_ASSERT_NOT_EQUAL(odin3_srcman_buffer_of(sm, loc.loc).v,
                          odin3_srcman_buffer_of(sm, (odin3_loc){loc.end.v - 1}).v);
    expect_spelled(loc.loc.v, "t.v", 1, 12);
    expect_spelled(loc.end.v - 1, "t.v", 2, 12);
}

/* A comment in a macro body becomes a loc-0 blank (2B): the token after it starts on a real
 * byte, with its own loc, and nothing is recorded. */
static void test_token_after_loc0_blank(void) {
    lex_file("`define M a /* c */ b\n`M\n");
    uint32_t off_b = off_of("b\n");
    /* "a" " " <loc-0 blank for the comment> " " "b": the comment's blank is inserted */
    TEST_ASSERT_EQUAL_UINT32(0, seg_loc(off_b - 2));
    TEST_ASSERT_NOT_EQUAL(0, seg_loc(off_b - 1));
    expect_token(TOK_IDENT, (tok_range){off_of("a"), 1});
    expect_token(TOK_IDENT, (tok_range){off_b, 1});
    TEST_ASSERT_NOT_EQUAL(0, loc.loc.v);
    expect_spelled(loc.loc.v, "t.v", 1, 21);
    TEST_ASSERT_EQUAL_INT(0, next());
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comment_count(ast));
}

/* --- comments through includes and macro arguments ------------------------------------------ */

/* The comments of FILE buffer named name, after sealing the store. */
static odin3_ast_comment_span comments_of_file(odin3_srcbuf_id buf) {
    if (odin3_ast_root_count(ast) == 0) {
        (void)odin3_ast_finish(ast);
    }
    return odin3_ast_comments_of(ast, buf);
}

/* A comment inside an included file is recorded in that FILE buffer; one after the `include on
 * its line in the includer's. */
static void test_comment_after_include(void) {
    put("inc.vh", odin3_bytes_cstr("// in the include\nwire w;\n"), NULL);
    lex_file("`include \"inc.vh\" // after the include\nwire v;\n");
    TEST_ASSERT_EQUAL_INT(TOK_WIRE, next());
    odin3_srcbuf_id inc = odin3_srcman_buffer_of(sm, loc.loc);
    while (next() != 0) {
    }
    TEST_ASSERT_EQUAL_UINT32(2, odin3_ast_comment_count(ast));
    odin3_ast_comment_span in_inc = comments_of_file(inc);
    TEST_ASSERT_EQUAL_UINT32(1, in_inc.n);
    TEST_ASSERT_EQUAL_STRING(" in the include", in_inc.items[0].text);
    odin3_ast_comment_span in_top = comments_of_file(stream.stream);
    TEST_ASSERT_EQUAL_UINT32(1, in_top.n);
    TEST_ASSERT_EQUAL_STRING(" after the include", in_top.items[0].text);
    expect_spelled(in_top.items[0].loc.v, "t.v", 1, 19);
}

/* A comment inside a macro argument is not recorded. */
static void test_comment_in_macro_argument(void) {
    lex_file("`define ID(x) x\nwire a = `ID(b /* gone */);\n");
    while (next() != 0) {
    }
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comment_count(ast));
    TEST_ASSERT_EQUAL_INT(0, errors);
}

/* --- Review Focus 2: `define F(x) (x + `W) used after an `include ----------------------------- */

static const char k_rf2_inc[] = "`define W 4\n`define F(x) (x + `W)\n";
static const char k_rf2_top[] =
    "`include \"inc.vh\"\nmodule m;\n  wire [3:0] y = `F(a);\nendmodule\n";

/* Each token of `F(a): its buffer kind and where its bytes are spelled. */
static void test_trace_macro_after_include(void) {
    put("inc.vh", odin3_bytes_cstr(k_rf2_inc), NULL);
    lex_file(k_rf2_top);
    while (next() != '=') {
    }
    uint32_t off = off_of("(a + 4)");
    expect_token('(', (tok_range){off, 1});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_EXPANSION, buffer_kind(loc.loc.v));
    expect_spelled(loc.loc.v, "inc.vh", 2, 14);
    expect_token(TOK_IDENT, (tok_range){off + 1, 1});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_MACRO_ARG, buffer_kind(loc.loc.v));
    expect_spelled(loc.loc.v, "t.v", 3, 21);
    expect_token('+', (tok_range){off + 3, 1});
    expect_spelled(loc.loc.v, "inc.vh", 2, 17);
    expect_token(TOK_UNSIGNED_NUMBER, (tok_range){off + 5, 1});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_EXPANSION, buffer_kind(loc.loc.v));
    expect_spelled(loc.loc.v, "inc.vh", 1, 11);
    expect_token(')', (tok_range){off + 6, 1});
    expect_spelled(loc.loc.v, "inc.vh", 2, 21);
    expect_token(';', (tok_range){off_of(");") + 1, 1});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SRCBUF_FILE, buffer_kind(loc.loc.v));
}

/* A syntax error on a token of a use (in an included file) prints the macro chain and the include
 * chain. (With the use in the top file the chain is the macro line alone: the source manager's
 * chain names the use's include levels, not those of the file defining the macro.) */
static void test_error_in_use_prints_chains(void) {
    put("inc.vh", odin3_bytes_cstr(k_rf2_inc), NULL);
    put("use.vh", odin3_bytes_cstr("  wire [3:0] y = `F(a) `F(b);\n"), NULL);
    char path[PP_PATH_BUF];
    put("t.v", odin3_bytes_cstr("`include \"inc.vh\"\nmodule m;\n`include \"use.vh\"\nendmodule\n"),
        path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run_path("t.v", path), captured);
    vstream = (odin3_vp_stream){stream.text, stream.stream, id("t.v")};
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_verilog_parse_stream(&run, ast, &vstream, &unit));
    TEST_ASSERT_EQUAL_INT(1, errors);
    TEST_ASSERT_EQUAL_STRING("use.vh:1:24: error: syntax: unexpected '('\n"
                             "  expanded from macro 'F' at inc.vh:2:14\n"
                             "  included from t.v:3",
                             captured);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ident_across_expansion);
    RUN_TEST(test_based_digits_across_two_macros);
    RUN_TEST(test_token_after_loc0_blank);
    RUN_TEST(test_comment_after_include);
    RUN_TEST(test_comment_in_macro_argument);
    RUN_TEST(test_trace_macro_after_include);
    RUN_TEST(test_error_in_use_prints_chains);
    return UNITY_END();
}
