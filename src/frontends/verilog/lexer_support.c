/*
 * lexer_support.c — the lexer object around the generated reentrant scanner (2C spec §2.1): its
 * allocator, the fatal-error escape, the one diagnostic path (§3.4), the token loop the parser
 * calls (odin3_vp_lex), the shared token helpers (lexer_internal.h) and the accessors the
 * grammar's actions use. The token rules' functions are in lexer_tokens.c and lexer_trivia.c.
 */
#include "frontends/verilog/lexer_support.h"

#include "frontends/verilog/lexer_internal.h"

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/parser.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"
#include "verilog_lexer.h" /* generated (build tree): the odin3_vp_ scanner API */

#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* yy_scan_bytes copies the text with two end-of-buffer NULs after it. */
enum { VL_SCAN_BYTES_PAD = 2 };

struct odin3_vl_lexer {
    yyscan_t scanner;
    odin3_vl_state state;
};

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

/* --- Flex's allocator (%option noyyalloc noyyrealloc): odin3_util_malloc behind the test hook.
 * Flex's own yyfree (free) releases the blocks: odin3_util_malloc is malloc. ------------------ */

/*
 * A reserved setup block when one of exactly this size is next (odin3_vl_open), else a fresh
 * block: the scanner's setup cannot fail inside Flex, where failure is fatal.
 */
void *odin3_vp_alloc(yy_size_t size, yyscan_t scanner) {
    odin3_vl_state *st = odin3_vp_get_extra(scanner);
    if (st != NULL && st->setup_next < ODIN3_VL_SETUP_BLOCKS &&
        st->setup_size[st->setup_next] == size) {
        void *block = st->setup[st->setup_next];
        st->setup[st->setup_next] = NULL;
        st->setup_next++;
        return block;
    }
    return odin3_util_malloc(size);
}

void *odin3_vp_realloc(void *ptr, yy_size_t size, yyscan_t scanner) {
    (void)scanner;
    return odin3_util_realloc(ptr, size);
}

/* Reserves the blocks yy_scan_bytes asks for, in its order (Flex 2.6.4): the text copy, the
 * buffer state and the one-entry buffer stack. */
static odin3_status reserve_setup(odin3_vl_state *st, size_t text_len) {
    const size_t sizes[ODIN3_VL_SETUP_BLOCKS] = {
        text_len + VL_SCAN_BYTES_PAD, sizeof(struct yy_buffer_state), sizeof(YY_BUFFER_STATE)};
    for (uint32_t i = 0; i < ODIN3_VL_SETUP_BLOCKS; i++) {
        st->setup[i] = odin3_util_malloc(sizes[i]);
        if (st->setup[i] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        st->setup_size[i] = sizes[i];
    }
    st->setup_next = 0;
    return ODIN3_OK;
}

/* Frees the reserved blocks Flex did not take and disarms the reserve. */
static void release_setup(odin3_vl_state *st) {
    for (uint32_t i = 0; i < ODIN3_VL_SETUP_BLOCKS; i++) {
        odin3_util_free(st->setup[i]);
        st->setup[i] = NULL;
    }
    st->setup_next = ODIN3_VL_SETUP_BLOCKS;
}

/* --- the fatal escape ------------------------------------------------------------------------ */

int odin3_vl_guard(odin3_vl_lexer *lx, int (*fn)(void *arg), void *arg) {
    jmp_buf escape;
    jmp_buf *outer = lx->state.escape;
    volatile int result = -1;
    lx->state.escape = &escape;
    if (setjmp(escape) == 0) {
        result = fn(arg);
    }
    lx->state.escape = outer;
    return result;
}

_Noreturn void odin3_vl_flex_fatal(const char *msg, void *scanner) {
    odin3_vl_state *st = odin3_vp_get_extra(scanner);
    bool memory = strstr(msg, "out of dynamic memory") != NULL;
    st->fatal = memory ? ODIN3_ERR_NO_MEMORY : ODIN3_ERR_PARSE;
    if (!memory) {
        odin3_log(ODIN3_LOG_ERROR, "verilog lexer: internal error: %s", msg);
    }
    st->run->stop = true;
    /* Every call into the scanner runs under odin3_vl_guard, so the escape is armed. */
    assert(st->escape != NULL);
    longjmp(*st->escape, 1);
}

/* --- open and close -------------------------------------------------------------------------- */

static void state_init(odin3_vl_state *st, odin3_vl_run *run, odin3_ast *ast,
                       const odin3_vp_stream *stream) {
    st->run = run;
    st->ast = ast;
    st->stream = *stream;
    st->error_mode = &st->error_local;
    odin3_strbuf_init(&st->scratch);
    odin3_vec_init(&st->meta, sizeof(odin3_vl_meta));
    odin3_vec_init(&st->names, sizeof(odin3_vl_name));
    st->fatal = ODIN3_OK;
    st->setup_next = ODIN3_VL_SETUP_BLOCKS;
}

static int scan_text(void *arg) {
    odin3_vl_lexer *lx = arg;
    const odin3_bytes text = lx->state.stream.text;
    const char *bytes = text.len > 0 ? text.ptr : "";
    return odin3_vp__scan_bytes(bytes, (int)text.len, lx->scanner) != NULL ? 0 : -1;
}

static odin3_status open_scanner(odin3_vl_lexer *lx) {
    if (odin3_vp_lex_init_extra(&lx->state, &lx->scanner) != 0) {
        lx->scanner = NULL;
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = reserve_setup(&lx->state, lx->state.stream.text.len);
    if (st == ODIN3_OK && odin3_vl_guard(lx, scan_text, lx) != 0) {
        st = lx->state.fatal != ODIN3_OK ? lx->state.fatal : ODIN3_ERR_NO_MEMORY;
    }
    release_setup(&lx->state);
    return st;
}

odin3_status odin3_vl_open(odin3_vl_run *run, odin3_ast *ast, const odin3_vp_stream *stream,
                           odin3_vl_lexer **out) {
    if (run == NULL || ast == NULL || stream == NULL || out == NULL ||
        (stream->text.ptr == NULL && stream->text.len > 0)) {
        return misuse("odin3_vl_open", "NULL argument");
    }
    if (stream->text.len > ODIN3_SRC_MAX_FILE_BYTES) {
        return misuse("odin3_vl_open", "stream over ODIN3_SRC_MAX_FILE_BYTES");
    }
    *out = NULL;
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(run->design, &sm);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_vl_lexer *lx = odin3_util_calloc(sizeof *lx);
    if (lx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    state_init(&lx->state, run, ast, stream);
    odin3_srcman_cursor_init(&lx->state.cur, sm, stream->stream.v);
    lx->state.last_end = odin3_srcman_cursor_loc(&lx->state.cur, 0);
    st = open_scanner(lx);
    if (st != ODIN3_OK) {
        odin3_vl_close(lx);
        return st;
    }
    *out = lx;
    return ODIN3_OK;
}

void odin3_vl_close(odin3_vl_lexer *lx) {
    if (lx == NULL) {
        return;
    }
    if (lx->scanner != NULL) {
        odin3_vp_lex_destroy(lx->scanner);
    }
    release_setup(&lx->state);
    odin3_strbuf_free(&lx->state.scratch);
    odin3_vec_free(&lx->state.meta);
    odin3_vec_free(&lx->state.names);
    odin3_util_free(lx);
}

void *odin3_vl_scanner(const odin3_vl_lexer *lx) {
    return lx->scanner;
}

const odin3_vl_state *vl_state_of(const odin3_vl_lexer *lx) {
    return &lx->state;
}

uint32_t odin3_vl_stream_name(const odin3_vl_lexer *lx) {
    return lx->state.stream.name;
}

odin3_status odin3_vl_fatal_status(const odin3_vl_lexer *lx) {
    return lx->state.fatal;
}

/* --- one token (tests) ------------------------------------------------------------------------ */

typedef struct next_args {
    odin3_vl_lexer *lx;
    ODIN3_VP_STYPE *val;
    odin3_vp_loc *loc;
} next_args;

static int scan_next(void *arg) {
    next_args *args = arg;
    return odin3_vp_lex(args->val, args->loc, args->lx->scanner);
}

/* The end of input after a fatal failure or the run's stop: no diagnostic. */
static int end_quietly(odin3_vl_state *st, odin3_vp_loc *loc) {
    *loc = (odin3_vp_loc){st->last_end, st->last_end, (uint32_t)st->stream.text.len};
    return TOK_YYEOF;
}

/* Appends the token's location to the run's token map (plan: the location oracle's map). */
static bool map_token(odin3_vl_state *st, const odin3_vp_loc *loc) {
    if (st->run->tokens == NULL) {
        return true;
    }
    odin3_vp_loc *slot = odin3_vec_push(st->run->tokens);
    if (slot == NULL) {
        (void)vl_fatal(st, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    *slot = *loc;
    return true;
}

/*
 * The generated parser's yylex: scans until a rule returns a token (an opaque body's inner rules
 * return none), maps it, and returns the end of input once the stream has failed fatally or the
 * run has stopped.
 */
int odin3_vp_lex(ODIN3_VP_STYPE *yylval_param, odin3_vp_loc *yylloc_param, void *yyscanner) {
    odin3_vl_state *st = odin3_vp_get_extra(yyscanner);
    int kind = VL_SKIP;
    while (kind == VL_SKIP && st->fatal == ODIN3_OK && !st->run->stop) {
        kind = odin3_vl_scan(yylval_param, yylloc_param, yyscanner);
    }
    if (st->fatal != ODIN3_OK || st->run->stop) {
        return end_quietly(st, yylloc_param);
    }
    return map_token(st, yylloc_param) ? kind : end_quietly(st, yylloc_param);
}

int odin3_vl_next(odin3_vl_lexer *lx, ODIN3_VP_STYPE *val, odin3_vp_loc *loc) {
    if (lx->state.fatal != ODIN3_OK) {
        return TOK_YYEOF;
    }
    next_args args = {lx, val, loc};
    int kind = odin3_vl_guard(lx, scan_next, &args);
    return kind < 0 ? TOK_YYEOF : kind;
}

/* --- diagnostics ------------------------------------------------------------------------------ */

void odin3_vl_report(odin3_vl_run *run, odin3_log_level level, odin3_loc loc, const char *fmt,
                     ...) {
    va_list args;
    va_start(args, fmt);
    odin3_vl_vreport(run, level, loc, fmt, args);
    va_end(args);
}

void odin3_vl_vreport(odin3_vl_run *run, odin3_log_level level, odin3_loc loc, const char *fmt,
                      va_list args) {
    if (run->stop) {
        return;
    }
    if (level == ODIN3_LOG_ERROR) {
        if (run->errors >= run->limits.max_errors) {
            run->stop = true;
            odin3_diag(run->design, ODIN3_LOG_ERROR, loc, "too many errors; stopping");
            return;
        }
        run->errors++;
    }
    odin3_srcman *sm = NULL;
    if (odin3_design_get_srcman(run->design, &sm) != ODIN3_OK) {
        sm = NULL;
    }
    odin3_srcman_vdiag(sm, level, loc, fmt, args);
}

bool odin3_vl_in_error(const odin3_vl_run *run) {
    return run->errors > 0 || run->stop;
}

/* --- the shared token helpers (lexer_internal.h) ------------------------------------------- */

odin3_loc vl_loc(odin3_vl_state *st, uint32_t off) {
    return odin3_srcman_cursor_loc(&st->cur, off);
}

odin3_bytes vl_bytes(const odin3_vl_state *st, uint32_t lo, uint32_t hi) {
    return (odin3_bytes){(const char *)st->stream.text.ptr + lo, hi - lo};
}

odin3_bytes vl_text(const odin3_vl_state *st) {
    return vl_bytes(st, st->tok_start, st->offset);
}

int vl_token_at(odin3_vl_state *st, odin3_vp_loc *loc, vl_span span, int kind) {
    assert(span.hi > span.lo);
    odin3_loc first = vl_loc(st, span.lo);
    odin3_loc last = vl_loc(st, span.hi - 1);
    *loc = (odin3_vp_loc){first, {last.v + 1}, span.lo};
    st->last_end = loc->end;
    return kind;
}

int vl_token(odin3_vl_state *st, odin3_vp_loc *loc, int kind) {
    return vl_token_at(st, loc, (vl_span){st->tok_start, st->offset}, kind);
}

bool vl_in_body(odin3_vl_state *st) {
    if (st->body.kind == 0) {
        return false;
    }
    st->body.first = false;
    return true;
}

static void vl_vdiag(odin3_vl_state *st, odin3_log_level level, odin3_loc loc, const char *fmt,
                     va_list args) {
    if (level == ODIN3_LOG_ERROR) {
        *st->error_mode = true;
    }
    odin3_vl_vreport(st->run, level, loc, fmt, args);
}

void vl_error(odin3_vl_state *st, odin3_loc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vl_vdiag(st, ODIN3_LOG_ERROR, loc, fmt, args);
    va_end(args);
}

void vl_warn(odin3_vl_state *st, odin3_loc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vl_vdiag(st, ODIN3_LOG_WARN, loc, fmt, args);
    va_end(args);
}

int vl_fatal(odin3_vl_state *st, odin3_status status) {
    if (st->fatal == ODIN3_OK) {
        st->fatal = status;
    }
    st->run->stop = true;
    return VL_SKIP;
}

bool vl_intern(odin3_vl_state *st, odin3_bytes bytes, uint32_t *id) {
    odin3_status status = odin3_ast_intern(st->ast, bytes, id);
    if (status != ODIN3_OK) {
        *id = 0;
        (void)vl_fatal(st, status);
        return false;
    }
    return true;
}

/* --- the accessors the grammar's actions use --------------------------------------------- */

void odin3_vl_bind_error_mode(odin3_vl_lexer *lx, bool *flag) {
    lx->state.error_mode = flag != NULL ? flag : &lx->state.error_local;
}

/* The first index of a pending metacomment at or after offset off. */
static size_t meta_lower_bound(const odin3_vec *meta, uint32_t off) {
    const odin3_vl_meta *items = meta->data;
    size_t lo = 0;
    size_t hi = meta->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (items[mid].off < off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

odin3_status odin3_vl_meta_take(odin3_vl_lexer *lx, uint32_t lo, uint32_t hi, odin3_vec *out) {
    odin3_vec *meta = &lx->state.meta;
    size_t first = meta_lower_bound(meta, lo);
    size_t last = hi > lo ? meta_lower_bound(meta, hi) : first;
    size_t count = last - first;
    if (count == 0) {
        return ODIN3_OK;
    }
    if (odin3_vec_reserve(out, out->len + count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = first; i < last; i++) {
        odin3_vl_meta *slot = odin3_vec_push(out); /* reserved: cannot fail */
        assert(slot != NULL);
        *slot = *(const odin3_vl_meta *)odin3_vec_cat(meta, i);
    }
    size_t tail = meta->len - last;
    if (tail > 0) {
        memmove(odin3_vec_at(meta, first), odin3_vec_at(meta, last), tail * sizeof(odin3_vl_meta));
    }
    for (size_t i = 0; i < count; i++) {
        odin3_vec_pop(meta);
    }
    return ODIN3_OK;
}

void odin3_vl_meta_clear(odin3_vl_lexer *lx) {
    odin3_vec_clear(&lx->state.meta);
}

uint32_t odin3_vl_opaque_name(const odin3_vl_lexer *lx, uint32_t off) {
    const odin3_vec *names = &lx->state.names;
    const odin3_vl_name *items = names->data;
    size_t lo = 0;
    size_t hi = names->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (items[mid].off == off) {
            return items[mid].name;
        }
        if (items[mid].off < off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

/* --- called by the generated scanner ------------------------------------------------------- */

void odin3_vl_user_action(odin3_vl_state *st, int len) {
    assert(len >= 0);
    st->tok_start = st->offset;
    st->offset += (uint32_t)len;
}

void odin3_vl_skip(odin3_vl_state *st) {
    (void)st; /* blanks and comment text: the offsets carry everything */
}

/* The end of input inside a block comment or an opaque body. */
static void report_unclosed(odin3_vl_state *st) {
    if (st->body.kind != 0) {
        vl_body_unclosed(st);
    } else if (st->token_loc.v != 0) {
        vl_error(st, st->token_loc, "unterminated comment");
    }
    st->token_loc = (odin3_loc){0};
}

int odin3_vl_eof(odin3_vl_state *st, odin3_vp_loc *loc) {
    if (!st->eof_seen) {
        st->eof_seen = true;
        report_unclosed(st);
        vl_keywords_at_end(st, st->last_end);
    }
    *loc = (odin3_vp_loc){st->last_end, st->last_end, (uint32_t)st->stream.text.len};
    return TOK_YYEOF;
}

void odin3_vl_unexpected(odin3_vl_state *st) {
    if (vl_in_body(st)) {
        return;
    }
    unsigned int byte = ((const unsigned char *)st->stream.text.ptr)[st->tok_start];
    vl_error(st, vl_loc(st, st->tok_start), "unexpected byte 0x%02x", byte);
}
