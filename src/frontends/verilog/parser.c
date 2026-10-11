/*
 * parser.c — the Verilog parser's driver (2C spec §3.4, §4.2 step 2): one stream through the
 * generated parser to one UNIT, the per-run read context, and the functions the generated
 * parser calls for locations (YYLLOC_DEFAULT), its stack cap (YYMAXDEPTH) and errors (yyerror).
 */
#include "frontends/verilog/parser.h"

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/actions.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"
#include "verilog_parser.h" /* generated (build tree): odin3_vp_parse */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The generated parser's stack cap, set from the run's limits before each parse (plan point
 * (d): under parse.lac the cap is read where the parse parameter is not visible; single-threaded,
 * IR-17).
 */
static long vp_stack_cap = ODIN3_VP_YYMAXDEPTH;

/*
 * Set when one of Bison's allocations (YYMALLOC: its stacks, LAC stack or message buffer) fails
 * during the current parse, so the "memory exhausted" that follows is a NO_MEMORY, not the cap
 * (Task 1 review M6). Bison's YYMALLOC sees no parse parameter; single-threaded, IR-17.
 */
static bool vp_stack_failed;

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

/* --- the run --------------------------------------------------------------------------------- */

static odin3_vp_limits default_limits(void) {
    return (odin3_vp_limits){.yymaxdepth = ODIN3_VP_YYMAXDEPTH,
                             .max_errors = ODIN3_VP_MAX_ERRORS,
                             .keyword_depth = ODIN3_VL_MAX_KEYWORD_DEPTH,
                             .ident_bytes = ODIN3_AST_MAX_IDENT_BYTES,
                             .string_bytes = ODIN3_AST_MAX_STRING_BYTES,
                             .text_bytes = ODIN3_AST_MAX_TEXT_BYTES,
                             .comment_bytes = ODIN3_VL_MAX_COMMENT_BYTES,
                             .directive_bytes = ODIN3_VL_MAX_DIRECTIVE_BYTES};
}

void odin3_vl_run_init(odin3_vl_run *run, odin3_design *design) {
    if (run == NULL) {
        return;
    }
    memset(run, 0, sizeof *run);
    run->design = design;
    run->limits = default_limits();
}

/* value, or the default when value is 0 or above it. */
static uint32_t clamp_limit(uint32_t value, uint32_t def) {
    return value == 0 || value > def ? def : value;
}

void odin3_verilog_test_set_limits(odin3_vl_run *run, odin3_vp_limits limits) {
    if (run == NULL) {
        return;
    }
    odin3_vp_limits def = default_limits();
    run->limits = (odin3_vp_limits){
        .yymaxdepth = limits.yymaxdepth != 0 && limits.yymaxdepth < ODIN3_VP_YYINITDEPTH
                          ? ODIN3_VP_YYINITDEPTH
                          : clamp_limit(limits.yymaxdepth, def.yymaxdepth),
        .max_errors = clamp_limit(limits.max_errors, def.max_errors),
        .keyword_depth = clamp_limit(limits.keyword_depth, def.keyword_depth),
        .ident_bytes = clamp_limit(limits.ident_bytes, def.ident_bytes),
        .string_bytes = clamp_limit(limits.string_bytes, def.string_bytes),
        .text_bytes = clamp_limit(limits.text_bytes, def.text_bytes),
        .comment_bytes = clamp_limit(limits.comment_bytes, def.comment_bytes),
        .directive_bytes = clamp_limit(limits.directive_bytes, def.directive_bytes)};
}

/* --- called by the generated parser ------------------------------------------------------- */

void odin3_vp_lloc(odin3_vp_loc *cur, const odin3_vp_loc *rhs, int n) {
    int first = 0;
    int last = 0;
    for (int i = 1; i <= n; i++) {
        if (rhs[i].loc.v != rhs[i].end.v) {
            first = first == 0 ? i : first;
            last = i;
        }
    }
    if (first == 0 && n > 0) {
        /* Only empty symbols: an empty symbol's loc is still a real position. rhs[0] is not
         * read here: on error recovery Bison passes yyerror_range, whose [0] it never sets. */
        *cur = (odin3_vp_loc){rhs[1].loc, rhs[1].loc, rhs[1].off};
        return;
    }
    if (first == 0) {
        *cur = (odin3_vp_loc){rhs[0].end, rhs[0].end, rhs[0].off};
        return;
    }
    *cur = (odin3_vp_loc){rhs[first].loc, rhs[last].end, rhs[first].off};
}

long odin3_vp_yymaxdepth(void) {
    return vp_stack_cap;
}

void *odin3_vp_stack_alloc(size_t bytes) {
    void *ptr = odin3_util_malloc(bytes);
    vp_stack_failed = vp_stack_failed || ptr == NULL;
    return ptr;
}

void odin3_vp_stack_free(void *ptr) {
    odin3_util_free(ptr);
}

void odin3_vp_enter_error(odin3_vp_ctx *ctx) {
    ctx->error_mode = true;
    odin3_ast_unwind(ctx->ast, ctx->base); /* nothing built after the first error is used */
    ctx->npieces = 0;
}

void odin3_vp_error(const odin3_vp_loc *at, odin3_vp_ctx *ctx, const char *msg) {
    odin3_vp_enter_error(ctx);
    if (ctx->fatal != ODIN3_OK) {
        return; /* the lexer's or an action's fatal path already ended the parse */
    }
    if (vp_stack_failed) { /* a failed YYMALLOC: the message, if any, is Bison's fallback */
        ctx->fatal = ODIN3_ERR_NO_MEMORY;
        ctx->run->stop = true;
        return;
    }
    if (strcmp(msg, "memory exhausted") == 0) {
        odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, at->loc,
                        "nesting deeper than the parser allows (%ld)", odin3_vp_yymaxdepth());
        ctx->fatal = ODIN3_ERR_PARSE;
        ctx->run->stop = true;
        return;
    }
    static const char bison_prefix[] = "syntax error, ";
    if (strncmp(msg, bison_prefix, sizeof bison_prefix - 1) == 0) {
        msg += sizeof bison_prefix - 1;
    }
    odin3_vl_report(ctx->run, ODIN3_LOG_ERROR, at->loc, "syntax: %s", msg);
}

/* --- the driver ------------------------------------------------------------------------------ */

static void ctx_init(odin3_vp_ctx *ctx, odin3_vl_run *run, odin3_ast *ast, odin3_vl_lexer *lx) {
    *ctx = (odin3_vp_ctx){.run = run,
                          .ast = ast,
                          .lexer = lx,
                          .error_mode = run->stop, /* an earlier stream's error leaves this
                                                      one live: its own errors still report */
                          .fatal = ODIN3_OK,
                          .base = odin3_ast_mark(ast)};
    odin3_vec_init(&ctx->span_scratch, sizeof(odin3_ast_id));
    odin3_vec_init(&ctx->part_scratch, sizeof(odin3_ast_id));
    odin3_vec_init(&ctx->meta_scratch, sizeof(odin3_ast_id));
    odin3_vec_init(&ctx->headers, sizeof(odin3_vp_header));
    odin3_vec_init(&ctx->windows, sizeof(odin3_vp_window));
    odin3_vl_bind_error_mode(lx, &ctx->error_mode); /* a lexer error makes the actions inert */
}

static void ctx_free(odin3_vp_ctx *ctx) {
    odin3_vec_free(&ctx->span_scratch);
    odin3_vec_free(&ctx->part_scratch);
    odin3_vec_free(&ctx->meta_scratch);
    odin3_vec_free(&ctx->headers);
    odin3_vec_free(&ctx->windows);
}

static int run_parser(void *arg) {
    return odin3_vp_parse(arg);
}

/* The UNIT's range: its project FILE buffer, first byte to one past the last. */
static odin3_vp_loc unit_range(odin3_design *design, odin3_srcbuf_id stream) {
    odin3_srcman *sm = NULL;
    odin3_srcbuf_info info;
    if (odin3_design_get_srcman(design, &sm) != ODIN3_OK ||
        !odin3_srcman_buffer_info(sm, stream, &info)) {
        return (odin3_vp_loc){{0}, {0}, 0};
    }
    return (odin3_vp_loc){odin3_srcman_loc(sm, stream, 0), odin3_srcman_loc(sm, stream, info.len),
                          0};
}

/* After the parse (rc from yyparse, or -1 from the fatal escape): the UNIT, or the failure. */
static odin3_status finish_stream(odin3_vp_ctx *ctx, int rc, uint32_t errors_before) {
    odin3_status fatal = ctx->fatal != ODIN3_OK ? ctx->fatal : odin3_vl_fatal_status(ctx->lexer);
    if (fatal != ODIN3_OK) {
        ctx->run->stop = true;
        return fatal;
    }
    if (rc != 0 || ctx->error_mode || ctx->run->errors != errors_before) {
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

odin3_status odin3_verilog_parse_stream(odin3_vl_run *run, odin3_ast *ast,
                                        const odin3_vp_stream *stream, odin3_ast_id *unit) {
    if (run == NULL || ast == NULL || stream == NULL || unit == NULL) {
        return misuse("odin3_verilog_parse_stream", "NULL argument");
    }
    if (run->stop) {
        return misuse("odin3_verilog_parse_stream", "the run has stopped");
    }
    *unit = (odin3_ast_id){0};
    odin3_vl_lexer *lx = NULL;
    odin3_status st = odin3_vl_open(run, ast, stream, &lx);
    if (st != ODIN3_OK) {
        run->stop = st == ODIN3_ERR_NO_MEMORY || run->stop;
        return st;
    }
    uint32_t errors_before = run->errors;
    uint32_t mark = odin3_ast_mark(ast);
    odin3_vp_ctx ctx;
    ctx_init(&ctx, run, ast, lx);
    vp_stack_cap = (long)run->limits.yymaxdepth;
    vp_stack_failed = false;
    int rc = odin3_vl_guard(lx, run_parser, &ctx);
    st = finish_stream(&ctx, rc, errors_before);
    if (st == ODIN3_OK) {
        odin3_vp_loc range = unit_range(run->design, stream->stream);
        st = vp_unit(&ctx, &range, mark, unit);
        run->stop = st != ODIN3_OK || run->stop;
    }
    if (st != ODIN3_OK) {
        odin3_ast_unwind(ast, mark);
        *unit = (odin3_ast_id){0};
    }
    ctx_free(&ctx);
    odin3_vl_close(lx);
    return st;
}
