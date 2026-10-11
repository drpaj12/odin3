/*
 * test_verilog_parser.c — the Verilog parser's driver (2C Task 1 skeleton): an empty stream to an
 * empty UNIT, YYLLOC_DEFAULT's ranges, yyerror's messages, the error cap of odin3_vl_report, the
 * flex fatal escape, misuse, the test limits hook and an OOM sweep.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/actions.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/parser_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "verilog_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OOM_LIMIT = 64, REPORTS = 25, LOWERED_ERRORS = 3 };

/* A design with one stream (for tests that drive the run, lexer or yyerror directly). */
static odin3_design *design;
static odin3_ast *ast;
static odin3_vp_stream stream;
static odin3_vl_run run;
static vt_result captured; /* only its log and counters are used outside vt_parse */

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    vt_stream(design, "t.v", odin3_bytes_cstr("module m;\nendmodule\n"), &stream);
    odin3_vl_run_init(&run, design);
    memset(&captured, 0, sizeof captured);
    odin3_strbuf_init(&captured.log);
    odin3_log_set_sink(vt_sink, &captured);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_strbuf_free(&captured.log);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

static const char *logged(void) {
    return captured.log.data != NULL ? captured.log.data : "";
}

/* The loc of a stream offset. */
static odin3_loc at(uint32_t offset) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    return odin3_srcman_loc(sm, stream.stream, offset);
}

static size_t count_of(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *hit = strstr(text, needle); hit != NULL; hit = strstr(hit + 1, needle)) {
        count++;
    }
    return count;
}

/* --- the empty stream ---------------------------------------------------------------------- */

static void test_empty_stream_gives_empty_unit(void) {
    vt_result res;
    vt_parse("empty.v", "", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, res.status);
    TEST_ASSERT_NOT_EQUAL(0, res.unit.v);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_UNIT, odin3_ast_kind_of(res.ast, res.unit));
    TEST_ASSERT_EQUAL_STRING("empty.v", odin3_ast_name_str(res.ast, res.unit));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(res.ast, res.unit));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_root_count(res.ast));
    TEST_ASSERT_EQUAL_UINT32(res.unit.v, odin3_ast_root(res.ast, 0).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(res.ast));
    TEST_ASSERT_EQUAL_UINT32(0, res.errors);
    TEST_ASSERT_EQUAL_UINT32(0, res.invalid_args);
    TEST_ASSERT_EQUAL_STRING("(UNIT:empty.v @empty.v:1:1)", res.dump.data);
    vt_free(&res);
}

/* A byte outside the grammar is a located error, and the stream yields no UNIT. */
static void test_unexpected_byte_gives_no_unit(void) {
    vt_result res;
    vt_parse("bad.v", "\x01", &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, res.status);
    TEST_ASSERT_EQUAL_UINT32(0, res.unit.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root_count(res.ast));
    TEST_ASSERT_EQUAL_STRING("bad.v:1:1: error: unexpected byte 0x01\n", res.log.data);
    TEST_ASSERT_EQUAL_UINT32(0, res.invalid_args);
    vt_free(&res);
}

/* --- YYLLOC_DEFAULT (§2.2) -------------------------------------------------------------------- */

static void test_lloc_empty_rule(void) {
    const odin3_vp_loc rhs[1] = {{{5}, {9}, 3}};
    odin3_vp_loc cur = {{1}, {1}, 1};
    odin3_vp_lloc(&cur, rhs, 0);
    TEST_ASSERT_EQUAL_UINT32(9, cur.loc.v);
    TEST_ASSERT_EQUAL_UINT32(9, cur.end.v);
    TEST_ASSERT_EQUAL_UINT32(3, cur.off);
}

static void test_lloc_skips_empty_symbols(void) {
    /* rhs[1] is an empty attrs_opt (loc == end): the item starts at its own first token. */
    const odin3_vp_loc lead[4] = {{{1}, {5}, 0}, {{5}, {5}, 4}, {{10}, {14}, 7}, {{14}, {20}, 11}};
    odin3_vp_loc cur = {{0}, {0}, 0};
    odin3_vp_lloc(&cur, lead, 3);
    TEST_ASSERT_EQUAL_UINT32(10, cur.loc.v);
    TEST_ASSERT_EQUAL_UINT32(20, cur.end.v);
    TEST_ASSERT_EQUAL_UINT32(7, cur.off);
    /* a trailing empty symbol does not move the end */
    const odin3_vp_loc trail[3] = {{{1}, {5}, 0}, {{10}, {14}, 7}, {{30}, {30}, 20}};
    odin3_vp_lloc(&cur, trail, 2);
    TEST_ASSERT_EQUAL_UINT32(10, cur.loc.v);
    TEST_ASSERT_EQUAL_UINT32(14, cur.end.v);
    TEST_ASSERT_EQUAL_UINT32(7, cur.off);
    /* only empty symbols: the first one's position (rhs[0] is not read) */
    const odin3_vp_loc none[3] = {{{1}, {5}, 2}, {{8}, {8}, 6}, {{9}, {9}, 7}};
    odin3_vp_lloc(&cur, none, 2);
    TEST_ASSERT_EQUAL_UINT32(8, cur.loc.v);
    TEST_ASSERT_EQUAL_UINT32(8, cur.end.v);
    TEST_ASSERT_EQUAL_UINT32(6, cur.off);
}

/* Error recovery at end of input (Bison's YYLLOC_DEFAULT(*yylsp, yyerror_range, 2)): [0] is
 * never set, [1] and [2] are the empty end-of-input token; the error token is located there. */
static void test_lloc_error_at_end_of_input(void) {
    const odin3_vp_loc eof = {at(20), at(20), 20};
    const odin3_vp_loc range[3] = {{{0xDEADBEEF}, {0xDEADBEEF}, 0xDEADBEEF}, eof, eof};
    odin3_vp_loc cur = {{0}, {0}, 0};
    odin3_vp_lloc(&cur, range, 2);
    TEST_ASSERT_EQUAL_UINT32(at(20).v, cur.loc.v);
    TEST_ASSERT_EQUAL_UINT32(at(20).v, cur.end.v);
    TEST_ASSERT_EQUAL_UINT32(20, cur.off);
    odin3_vp_ctx ctx = {.run = &run, .ast = ast};
    odin3_vp_error(&cur, &ctx, "syntax error, unexpected end of file, expecting endmodule");
    TEST_ASSERT_EQUAL_STRING(
        "t.v:3:1: error: syntax: unexpected end of file, expecting endmodule\n", logged());
}

/* --- yyerror (§3.4, plan points (d), (e)) ------------------------------------------------------
 */

static void test_yyerror_messages(void) {
    odin3_vp_ctx ctx = {.run = &run, .ast = ast};
    odin3_vp_loc loc = {at(10), at(19), 10};
    odin3_vp_error(&loc, &ctx, "syntax error, unexpected endmodule, expecting ';' or ','");
    TEST_ASSERT_TRUE(ctx.error_mode);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, ctx.fatal);
    TEST_ASSERT_EQUAL_STRING("t.v:2:1: error: syntax: unexpected endmodule, expecting ';' or ','\n",
                             logged());
    TEST_ASSERT_EQUAL_UINT32(1, run.errors);
    odin3_strbuf_clear(&captured.log);
    ctx.fatal = ODIN3_ERR_NO_MEMORY; /* after a fatal failure yyerror is silent */
    odin3_vp_error(&loc, &ctx, "syntax error, unexpected end of file");
    TEST_ASSERT_EQUAL_STRING("", logged());
    TEST_ASSERT_EQUAL_UINT32(1, run.errors);
}

static void test_yyerror_stack_exhausted_is_fatal(void) {
    odin3_vp_ctx ctx = {.run = &run, .ast = ast};
    odin3_vp_loc loc = {at(7), at(8), 7};
    odin3_vp_error(&loc, &ctx, "memory exhausted");
    TEST_ASSERT_EQUAL_STRING("t.v:1:8: error: nesting deeper than the parser allows (262144)\n",
                             logged());
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, ctx.fatal);
    TEST_ASSERT_TRUE(run.stop);
    TEST_ASSERT_TRUE(odin3_vp_yymaxdepth() == ODIN3_VP_YYMAXDEPTH);
}

/* --- the error cap (§3.4, plan point (r)) ------------------------------------------------------
 */

/* Errors e0..e24 interleaved with warnings w0..w24 (error i located at column i % 9 + 1). */
static void report_interleaved(void) {
    for (uint32_t i = 0; i < REPORTS; i++) {
        odin3_vl_report(&run, ODIN3_LOG_WARN, at(0), "w%u", i);
        odin3_vl_report(&run, ODIN3_LOG_ERROR, at(i % 9), "e%u", i);
    }
}

static void test_report_cap_stops_after_20_errors(void) {
    report_interleaved();
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VP_MAX_ERRORS + 1, captured.errors);
    TEST_ASSERT_EQUAL_size_t(1, count_of(logged(), "too many errors; stopping"));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VP_MAX_ERRORS, run.errors);
    TEST_ASSERT_TRUE(run.stop);
    TEST_ASSERT_TRUE(odin3_vl_in_error(&run));
}

static void test_report_cap_logs_the_first_20(void) {
    report_interleaved();
    const char *text = logged();
    TEST_ASSERT_NOT_NULL(strstr(text, "t.v:1:1: error: e0\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "t.v:1:2: error: e19\n"));
    TEST_ASSERT_NULL(strstr(text, "e20\n"));
}

/* The 21st error is replaced by the stop line, located at it; warnings never count, and nothing
 * is logged after the stop. */
static void test_report_cap_ignores_warnings(void) {
    report_interleaved();
    const char *text = logged();
    TEST_ASSERT_NOT_NULL(
        strstr(text, "t.v:1:1: warning: w20\nt.v:1:3: error: too many errors; stopping\n"));
    TEST_ASSERT_EQUAL_size_t(ODIN3_VP_MAX_ERRORS + 1, count_of(text, ": warning: w"));
    TEST_ASSERT_NULL(strstr(text, "w21\n"));
}

static void test_limits_hook_lowers_and_clamps(void) {
    odin3_vl_run fresh;
    odin3_vl_run_init(&fresh, design);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VL_KW_2005, fresh.kw_mask[0]);
    TEST_ASSERT_EQUAL_UINT32(0, fresh.kw_depth);
    TEST_ASSERT_FALSE(odin3_vl_in_error(&fresh));
    TEST_ASSERT_NULL(fresh.tokens);
    odin3_vp_limits want = {ODIN3_VP_YYMAXDEPTH,        ODIN3_VP_MAX_ERRORS,
                            ODIN3_VL_MAX_KEYWORD_DEPTH, ODIN3_AST_MAX_IDENT_BYTES,
                            ODIN3_AST_MAX_STRING_BYTES, ODIN3_AST_MAX_TEXT_BYTES,
                            ODIN3_VL_MAX_COMMENT_BYTES, ODIN3_VL_MAX_DIRECTIVE_BYTES};
    TEST_ASSERT_EQUAL_MEMORY(&want, &fresh.limits, sizeof want);
    odin3_verilog_test_set_limits(&fresh, (odin3_vp_limits){.yymaxdepth = UINT32_MAX,
                                                            .max_errors = LOWERED_ERRORS,
                                                            .ident_bytes = 8});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VP_YYMAXDEPTH, fresh.limits.yymaxdepth); /* clamped */
    TEST_ASSERT_EQUAL_UINT32(LOWERED_ERRORS, fresh.limits.max_errors);
    TEST_ASSERT_EQUAL_UINT32(8, fresh.limits.ident_bytes);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_TEXT_BYTES, fresh.limits.text_bytes); /* 0: default */
    for (uint32_t i = 0; i < LOWERED_ERRORS + 2; i++) {
        odin3_vl_report(&fresh, ODIN3_LOG_ERROR, at(0), "e%u", i);
    }
    TEST_ASSERT_EQUAL_UINT32(LOWERED_ERRORS + 1, captured.errors);
    TEST_ASSERT_TRUE(fresh.stop);
    odin3_verilog_test_set_limits(NULL, want);
    odin3_vl_run_init(NULL, design);
}

/* Bison checks YYMAXDEPTH only when its stack grows past YYINITDEPTH: a lower cap is raised. */
static void test_limits_hook_stack_floor(void) {
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.yymaxdepth = 16});
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VP_YYINITDEPTH, run.limits.yymaxdepth);
    odin3_verilog_test_set_limits(&run, (odin3_vp_limits){.yymaxdepth = 1000});
    TEST_ASSERT_EQUAL_UINT32(1000, run.limits.yymaxdepth);
}

/* --- the lexer object ------------------------------------------------------------------------ */

static void test_lexer_eof_location(void) {
    odin3_vp_stream empty;
    vt_stream(design, "e.v", odin3_bytes_cstr(""), &empty);
    odin3_vl_lexer *lx = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_open(&run, ast, &empty, &lx));
    ODIN3_VP_STYPE val;
    odin3_vp_loc loc = {{7}, {7}, 7};
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, odin3_vl_next(lx, &val, &loc));
    TEST_ASSERT_EQUAL_UINT32(0, loc.loc.v);
    TEST_ASSERT_EQUAL_UINT32(0, loc.end.v);
    TEST_ASSERT_EQUAL_UINT32(0, loc.off);
    TEST_ASSERT_EQUAL_UINT32(empty.name, odin3_vl_stream_name(lx));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_fatal_status(lx));
    odin3_vl_close(lx);
    odin3_vl_close(NULL);
}

static odin3_vl_lexer *fatal_lexer;

static int fail_memory(void *arg) {
    (void)arg;
    odin3_vl_flex_fatal("out of dynamic memory in yy_scan_buffer()", odin3_vl_scanner(fatal_lexer));
}

static int fail_internal(void *arg) {
    (void)arg;
    odin3_vl_flex_fatal("flex scanner jammed", odin3_vl_scanner(fatal_lexer));
}

/* Flex's fatal errors return to the guarded entry point with a status instead of exiting. This
 * calls the handler directly; that YY_EXIT_FAILURE routes the generated yy_fatal_error to it is
 * proven by the symbol_rules CTest (libodin3 imports no exit). */
static void test_flex_fatal_escape(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_open(&run, ast, &stream, &fatal_lexer));
    TEST_ASSERT_EQUAL_INT(-1, odin3_vl_guard(fatal_lexer, fail_memory, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_vl_fatal_status(fatal_lexer));
    TEST_ASSERT_TRUE(run.stop);
    TEST_ASSERT_EQUAL_STRING("", logged());
    ODIN3_VP_STYPE val;
    odin3_vp_loc loc;
    TEST_ASSERT_EQUAL_INT(TOK_YYEOF, odin3_vl_next(fatal_lexer, &val, &loc));
    odin3_vl_close(fatal_lexer);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_vl_open(&run, ast, &stream, &fatal_lexer));
    TEST_ASSERT_EQUAL_INT(-1, odin3_vl_guard(fatal_lexer, fail_internal, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_vl_fatal_status(fatal_lexer));
    TEST_ASSERT_EQUAL_STRING("verilog lexer: internal error: flex scanner jammed\n", logged());
    odin3_vl_close(fatal_lexer);
    fatal_lexer = NULL;
}

/* --- misuse ----------------------------------------------------------------------------------- */

static void test_parse_stream_misuse(void) {
    odin3_ast_id unit = {7};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_verilog_parse_stream(NULL, ast, &stream, &unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_verilog_parse_stream(&run, NULL, &stream, &unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_verilog_parse_stream(&run, ast, NULL, &unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_verilog_parse_stream(&run, ast, &stream, NULL));
    TEST_ASSERT_EQUAL_size_t(4, count_of(logged(), "odin3_verilog_parse_stream: NULL argument\n"));
    run.stop = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_verilog_parse_stream(&run, ast, &stream, &unit));
    TEST_ASSERT_NOT_NULL(strstr(logged(), "odin3_verilog_parse_stream: the run has stopped\n"));
    TEST_ASSERT_EQUAL_UINT32(7, unit.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_node_end(ast).v);
    odin3_vl_lexer *lx = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_vl_open(&run, ast, NULL, &lx));
    TEST_ASSERT_NULL(lx);
}

/* --- OOM ---------------------------------------------------------------------------------------
 */

/* One OOM step: the parse of a fresh empty stream with the limit-th allocation failing. A
 * failure leaves nothing made and stops the run. */
static odin3_status oom_step(long limit, odin3_ast_id *unit) {
    odin3_vp_stream empty;
    vt_stream(design, "e.v", odin3_bytes_cstr(""), &empty);
    odin3_vl_run_init(&run, design);
    uint32_t end = odin3_ast_node_end(ast).v;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_verilog_parse_stream(&run, ast, &empty, unit);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(0, unit->v);
        TEST_ASSERT_TRUE(run.stop);
        TEST_ASSERT_EQUAL_UINT32(end, odin3_ast_node_end(ast).v);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(ast));
    }
    return st;
}

/* Every allocation of an empty stream's parse fails in turn; then the parse succeeds. */
static void test_parse_stream_oom(void) {
    odin3_ast_id unit = {0};
    long limit = 0;
    while (limit < OOM_LIMIT && oom_step(limit, &unit) != ODIN3_OK) {
        limit++;
    }
    TEST_ASSERT_TRUE(limit > 0);
    TEST_ASSERT_TRUE(limit < OOM_LIMIT);
    TEST_ASSERT_NOT_EQUAL(0, unit.v);
    TEST_ASSERT_EQUAL_UINT32(0, captured.invalid_args);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_stream_gives_empty_unit);
    RUN_TEST(test_unexpected_byte_gives_no_unit);
    RUN_TEST(test_lloc_empty_rule);
    RUN_TEST(test_lloc_skips_empty_symbols);
    RUN_TEST(test_lloc_error_at_end_of_input);
    RUN_TEST(test_yyerror_messages);
    RUN_TEST(test_yyerror_stack_exhausted_is_fatal);
    RUN_TEST(test_report_cap_stops_after_20_errors);
    RUN_TEST(test_report_cap_logs_the_first_20);
    RUN_TEST(test_report_cap_ignores_warnings);
    RUN_TEST(test_limits_hook_lowers_and_clamps);
    RUN_TEST(test_limits_hook_stack_floor);
    RUN_TEST(test_lexer_eof_location);
    RUN_TEST(test_flex_fatal_escape);
    RUN_TEST(test_parse_stream_misuse);
    RUN_TEST(test_parse_stream_oom);
    return UNITY_END();
}
