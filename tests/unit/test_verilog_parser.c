/*
 * test_verilog_parser.c — the Verilog parser's driver: an empty stream to an empty UNIT,
 * YYLLOC_DEFAULT's ranges, yyerror's messages, the error cap of odin3_vl_report, the flex fatal
 * escape, misuse, the test limits hook and OOM sweeps (Task 1); edge-of-file shapes, located
 * syntax errors, recovery, the error cap, the stack and AST caps, error mode and literal errors
 * through the grammar (Task 4).
 */
#include "ast/ast.h"
#include "ast/ast_test.h"
#include "ast/kinds.h"
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

enum {
    OOM_LIMIT = 64,
    REPORTS = 25,
    LOWERED_ERRORS = 3,
    MODULE_OOM_LIMIT = 4096,
    LOWERED_DEPTH = 16,
    NESTED_RANGE = 20,
    LOWERED_CHILDREN = 8,
    BAD_ITEMS = 25,
    STACK_PARENS = 300,
    DEFAULT_STACK_PARENS = 270000,
    DIGITS = 4097,
    TEXT_BYTES = 1 << 20,
};

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
    odin3_srcman *sm = NULL; /* no token: the stream's first byte */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_EQUAL_UINT32(odin3_srcman_loc(sm, empty.stream, 0).v, loc.loc.v);
    TEST_ASSERT_EQUAL_UINT32(loc.loc.v, loc.end.v);
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

/* --- Task 4: whole streams through the grammar ------------------------------------------ */

/* The caps a Task 4 test lowers (0 keeps a default). */
typedef struct hooks {
    odin3_vp_limits parser;
    uint32_t max_depth;
    uint32_t max_children;
} hooks;

/* Parses text as x.v in the setUp design, store and run with the hooks applied and the sink
 * capturing; *unit gets the UNIT (0 on failure). */
static odin3_vp_stream last_x; /* the stream parse_x parsed last */

static odin3_status parse_x(const char *text, const hooks *caps, odin3_ast_id *unit) {
    vt_stream(design, "x.v", odin3_bytes_cstr(text), &last_x);
    if (caps != NULL) {
        odin3_verilog_test_set_limits(&run, caps->parser);
        odin3_ast_test_set_limits(
            ast, (odin3_ast_limits){caps->max_children != 0 ? caps->max_children
                                                            : ODIN3_AST_MAX_CHILDREN,
                                    ODIN3_AST_MAX_NODES,
                                    caps->max_depth != 0 ? caps->max_depth : ODIN3_AST_MAX_DEPTH});
    }
    *unit = (odin3_ast_id){0};
    return odin3_verilog_parse_stream(&run, ast, &last_x, unit);
}

/* text must fail with status and exactly the log want, leaving no UNIT and the pending stack
 * where it was. */
static void expect_failure(const char *text, const hooks *caps, odin3_status status,
                           const char *want) {
    odin3_ast_id unit = {7};
    TEST_ASSERT_EQUAL_INT(status, parse_x(text, caps, &unit));
    TEST_ASSERT_EQUAL_UINT32(0, unit.v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_root_count(ast));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(ast));
    TEST_ASSERT_EQUAL_STRING(want, logged());
    TEST_ASSERT_EQUAL_UINT32(0, captured.invalid_args);
}

/* head, count copies of piece, then tail (freed by the caller with odin3_util_free). */
static char *repeat_around(const char *head, const char *piece, uint32_t count, const char *tail) {
    size_t step = strlen(piece);
    size_t len = strlen(head) + (size_t)count * step + strlen(tail);
    char *text = odin3_util_malloc(len + 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t used = strlen(head);
    memcpy(text, head, used + 1); /* each copy brings its NUL; the next one overwrites it */
    for (uint32_t i = 0; i < count; i++, used += step) {
        memcpy(text + used, piece, step + 1);
    }
    memcpy(text + used, tail, strlen(tail) + 1);
    return text;
}

/* Review Focus 3: a comment-only stream is a UNIT without children. */
static void test_comment_only_stream(void) {
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_x("// a comment\n/* and a block */", NULL, &unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_UNIT, odin3_ast_kind_of(ast, unit));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_nchild(ast, unit));
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* Review Focus 3: a stream holding only `timescale is one DIRECTIVE. */
static void test_timescale_only_stream(void) {
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_x("`timescale 1ns/1ps", NULL, &unit));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_nchild(ast, unit));
    odin3_ast_id dir = odin3_ast_child(ast, unit, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_DIRECTIVE, odin3_ast_kind_of(ast, dir));
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_DIRECTIVE_TIMESCALE, odin3_ast_sub(ast, dir));
    TEST_ASSERT_EQUAL_STRING("1ns/1ps", odin3_ast_name_str(ast, dir));
}

/* Review Focus 3, Task 1 review: a missing endmodule is "unexpected end of file" at the last
 * token's end, through the real parser. */
static void test_end_of_input_inside_a_module(void) {
    expect_failure("module m;", NULL, ODIN3_ERR_PARSE,
                   "x.v:1:10: error: syntax: unexpected end of file\n");
}

static void test_end_of_input_with_expected_tokens(void) {
    expect_failure(
        "module m (a\n", NULL, ODIN3_ERR_PARSE,
        "x.v:1:12: error: syntax: unexpected end of file, expecting ')' or ',' or '.' or '['\n");
}

/* Three errors, one per line, each after Bison's three-token window: three diagnostics, no
 * builder misuse, no UNIT. */
static void test_recovery_reports_each_item(void) {
    expect_failure(
        "module m;\n  wire a b;\n  wire c d;\n  reg [3:0 r;\nendmodule\n", NULL, ODIN3_ERR_PARSE,
        "x.v:2:10: error: syntax: unexpected identifier, expecting ';' or ',' or '=' or '['\n"
        "x.v:3:10: error: syntax: unexpected identifier, expecting ';' or ',' or '=' or '['\n"
        "x.v:4:12: error: syntax: unexpected identifier, expecting base or ']'\n");
    TEST_ASSERT_EQUAL_UINT32(3, captured.errors);
    TEST_ASSERT_EQUAL_UINT32(3, run.errors);
}

/* description: error ENDMODULE — an error in one module still lets the next one report. */
static void test_recovery_across_modules(void) {
    expect_failure(
        "module a (x y);\nendmodule\nmodule b;\n  wire c d;\nendmodule\n", NULL, ODIN3_ERR_PARSE,
        "x.v:1:13: error: syntax: unexpected identifier, expecting ')' or ',' or '.' or '['\n"
        "x.v:4:10: error: syntax: unexpected identifier, expecting ';' or ',' or '=' or '['\n");
}

/* 25 bad items: 20 errors, then the stop line, and the run stops. */
static void test_error_cap_through_the_grammar(void) {
    char *text = repeat_around("module m;\n", "  wire a b;\n", BAD_ITEMS, "endmodule\n");
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_x(text, NULL, &unit));
    TEST_ASSERT_EQUAL_UINT32(0, unit.v);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_VP_MAX_ERRORS + 1, captured.errors);
    TEST_ASSERT_EQUAL_size_t(ODIN3_VP_MAX_ERRORS, count_of(logged(), "syntax: unexpected"));
    TEST_ASSERT_NOT_NULL(strstr(logged(), "x.v:22:10: error: too many errors; stopping\n"));
    TEST_ASSERT_TRUE(run.stop);
    TEST_ASSERT_EQUAL_UINT32(0, captured.invalid_args);
    odin3_util_free(text);
}

/* The stack cap (plan point (d)): nesting past YYMAXDEPTH is the located fatal message naming
 * the run's cap, never a NO_MEMORY. The hook's floor is YYINITDEPTH (256). */
static void test_stack_cap_is_located_and_fatal(void) {
    char *text = repeat_around("module m; wire [", "(", STACK_PARENS, "0");
    hooks caps = {.parser = {.yymaxdepth = LOWERED_DEPTH}};
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_x(text, &caps, &unit));
    TEST_ASSERT_EQUAL_UINT32(0, unit.v);
    TEST_ASSERT_EQUAL_STRING("x.v:1:259: error: nesting deeper than the parser allows (256)\n",
                             logged());
    TEST_ASSERT_TRUE(run.stop);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(ast));
    odin3_util_free(text);
}

/* The default cap, ODIN3_VP_YYMAXDEPTH, reached by nested parentheses (they make no node). */
static void test_default_stack_cap(void) {
    char *text = repeat_around("module m; wire [", "(", DEFAULT_STACK_PARENS, "0");
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_x(text, NULL, &unit));
    TEST_ASSERT_NOT_NULL(
        strstr(logged(), "error: nesting deeper than the parser allows (262144)\n"));
    TEST_ASSERT_EQUAL_UINT32(1, captured.errors);
    TEST_ASSERT_TRUE(run.stop);
    odin3_util_free(text);
}

/*
 * One step of an OOM sweep over text: a fresh run, the limit-th allocation failing. A failure is
 * NO_MEMORY with no UNIT, the pending stack and the UNIT count unchanged and the run stopped.
 */
static odin3_status oom_text_step(const char *text, long limit, odin3_ast_id *unit) {
    odin3_vl_run_init(&run, design);
    odin3_vp_stream xs;
    vt_stream(design, "x.v", odin3_bytes_cstr(text), &xs);
    uint32_t roots = odin3_ast_root_count(ast);
    uint32_t mark = odin3_ast_mark(ast);
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_verilog_parse_stream(&run, ast, &xs, unit);
    odin3_util_set_alloc_fail_after(-1);
    if (st != ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(0, unit->v);
        TEST_ASSERT_TRUE(run.stop);
        TEST_ASSERT_EQUAL_UINT32(mark, odin3_ast_mark(ast));
        TEST_ASSERT_EQUAL_UINT32(roots, odin3_ast_root_count(ast));
    }
    return st;
}

/* Fails every allocation of text's parse in turn until it succeeds; the number of steps. */
static long oom_sweep(const char *text, odin3_ast_id *unit) {
    long limit = 0;
    while (limit < MODULE_OOM_LIMIT && oom_text_step(text, limit, unit) != ODIN3_OK) {
        limit++;
    }
    TEST_ASSERT_TRUE(limit < MODULE_OOM_LIMIT);
    return limit;
}

/* Task 1 review M6: a failed allocation while Bison's stack grows is NO_MEMORY, never the depth
 * message. Every allocation of a parse that grows the stack (past YYINITDEPTH) fails in turn. */
static void test_stack_growth_oom_is_no_memory(void) {
    char *open = repeat_around("module m; wire [", "(", STACK_PARENS, "0");
    char *full = repeat_around(open, ")", STACK_PARENS, ":0] w; endmodule");
    odin3_ast_id unit = {0};
    TEST_ASSERT_TRUE(oom_sweep(full, &unit) > 0);
    TEST_ASSERT_NULL(strstr(logged(), "nesting deeper"));
    TEST_ASSERT_EQUAL_UINT32(0, captured.errors);
    odin3_util_free(full);
    odin3_util_free(open);
}

/* odin3_ast_make's depth cap on a deep range expression: located PARSE, fatal. */
static void test_ast_depth_cap(void) {
    char *head = repeat_around("module m; wire [", "x[", NESTED_RANGE, "0");
    char *text = repeat_around(head, "].y", NESTED_RANGE, ":0] w; endmodule");
    hooks caps = {.max_depth = LOWERED_DEPTH};
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_x(text, &caps, &unit));
    TEST_ASSERT_EQUAL_UINT32(0, unit.v);
    TEST_ASSERT_EQUAL_STRING("x.v:1:41: error: nesting deeper than 16\n", logged());
    TEST_ASSERT_TRUE(run.stop);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_mark(ast));
    TEST_ASSERT_EQUAL_UINT32(0, captured.invalid_args);
    odin3_util_free(text);
    odin3_util_free(head);
}

/* odin3_ast_make's child cap on a nine-port list: located at the LIST, fatal. */
static void test_child_cap(void) {
    hooks caps = {.max_children = LOWERED_CHILDREN};
    expect_failure("module m (a, b, c, d, e, f, g, h, i); endmodule", &caps, ODIN3_ERR_PARSE,
                   "x.v:1:10: error: more than 8 children\n");
    TEST_ASSERT_TRUE(run.stop);
}

/* No node is made at or after the first error (syntax or lexical): the actions are inert. */
static void assert_nothing_made_after(uint32_t offset) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_loc first_error = odin3_srcman_loc(sm, last_x.stream, offset);
    TEST_ASSERT_TRUE(odin3_ast_node_end(ast).v > 1); /* something was made before the error */
    for (uint32_t id = 1; id < odin3_ast_node_end(ast).v; id++) {
        odin3_loc loc = odin3_ast_loc(ast, (odin3_ast_id){id});
        TEST_ASSERT_TRUE_MESSAGE(loc.v < first_error.v, "a node was made after the first error");
    }
}

static void test_actions_inert_after_a_syntax_error(void) {
    expect_failure("module m; wire a; wire b c; wire d; assign d = a; endmodule", NULL,
                   ODIN3_ERR_PARSE,
                   "x.v:1:26: error: syntax: unexpected identifier, expecting ';' or ',' or '=' or "
                   "'['\n");
    assert_nothing_made_after(25);
}

static void test_actions_inert_after_a_lexer_error(void) {
    expect_failure("module m; wire a; \x01 wire c; assign c = a; endmodule", NULL, ODIN3_ERR_PARSE,
                   "x.v:1:19: error: unexpected byte 0x01\n");
    assert_nothing_made_after(18);
}

/* Literals: number_parse's own located errors are recoverable, counted errors. */
static void test_literal_digit_error(void) {
    expect_failure("module m(input['b12:0] a); endmodule", NULL, ODIN3_ERR_PARSE,
                   "x.v:1:16: error: invalid digit '2' in a binary literal\n");
    TEST_ASSERT_EQUAL_UINT32(1, run.errors);
    TEST_ASSERT_FALSE(run.stop);
}

/* A base without digits is reported at the base, as written, and nothing after it. */
static void test_literal_without_digits(void) {
    expect_failure("module m; assign x = 4'h; wire 3 4; endmodule", NULL, ODIN3_ERR_PARSE,
                   "x.v:1:23: error: expected digits after 'h\n");
    TEST_ASSERT_EQUAL_UINT32(1, run.errors);
}

static void test_literal_too_many_digits(void) {
    char *text = repeat_around("module m; assign x = ", "9", DIGITS, "; endmodule");
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, parse_x(text, NULL, &unit));
    TEST_ASSERT_EQUAL_STRING("x.v:1:22: error: decimal literal longer than 4096 digits\n",
                             logged());
    TEST_ASSERT_EQUAL_UINT32(1, run.errors);
    odin3_util_free(text);
}

/* The empty string interns as strtab ID 0, which a STRING (name required, 2A §4.3) cannot hold:
 * a located, counted error rather than a builder misuse (agent default, see the Task 4 report). */
static void test_empty_string_is_a_located_error(void) {
    expect_failure("module m; parameter p = \"\"; endmodule", NULL, ODIN3_ERR_PARSE,
                   "x.v:1:25: error: empty string literal \"\" is not supported\n");
    TEST_ASSERT_EQUAL_UINT32(1, run.errors);
}

/* Review Focus 3: a last line without a newline (endmodule, a comment) parses. */
static void test_last_line_without_newline(void) {
    odin3_ast_id unit = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_x("module m; endmodule", NULL, &unit));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_nchild(ast, unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, parse_x("module m;\nendmodule\n// c", NULL, &unit));
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_nchild(ast, unit));
    TEST_ASSERT_EQUAL_STRING("", logged());
}

/* Every allocation of a module's parse fails in turn: NO_MEMORY with no UNIT, the pending stack
 * where it was, the run stopped, no misuse and (under ASan) no leak; then the parse succeeds. */
static void test_module_parse_oom(void) {
    static const char text[] =
        "`timescale 1ns/1ps\n(* a *) module m #(parameter P = 1) (input [3:0] a, output y);\n"
        "  wire (strong0, pull1) #(1:2:3) w = a, v [0:1];\n  assign {y, v[0]} = 4'hF;\n"
        "  defparam u.W = 2;\n  specify (a => y) = 1; endspecify\nendmodule\n"
        "primitive p (y, a); endprimitive\n";
    odin3_ast_id unit = {0};
    TEST_ASSERT_TRUE(oom_sweep(text, &unit) > 1);
    TEST_ASSERT_EQUAL_UINT32(3, odin3_ast_nchild(ast, unit));
    TEST_ASSERT_EQUAL_UINT32(0, captured.invalid_args);
    TEST_ASSERT_EQUAL_UINT32(0, captured.errors);
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
    RUN_TEST(test_comment_only_stream);
    RUN_TEST(test_timescale_only_stream);
    RUN_TEST(test_end_of_input_inside_a_module);
    RUN_TEST(test_end_of_input_with_expected_tokens);
    RUN_TEST(test_recovery_reports_each_item);
    RUN_TEST(test_recovery_across_modules);
    RUN_TEST(test_error_cap_through_the_grammar);
    RUN_TEST(test_stack_cap_is_located_and_fatal);
    RUN_TEST(test_default_stack_cap);
    RUN_TEST(test_stack_growth_oom_is_no_memory);
    RUN_TEST(test_ast_depth_cap);
    RUN_TEST(test_child_cap);
    RUN_TEST(test_actions_inert_after_a_syntax_error);
    RUN_TEST(test_actions_inert_after_a_lexer_error);
    RUN_TEST(test_literal_digit_error);
    RUN_TEST(test_literal_without_digits);
    RUN_TEST(test_literal_too_many_digits);
    RUN_TEST(test_empty_string_is_a_located_error);
    RUN_TEST(test_last_line_without_newline);
    RUN_TEST(test_module_parse_oom);
    return UNITY_END();
}
