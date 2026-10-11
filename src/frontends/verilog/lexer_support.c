/*
 * lexer_support.c — the lexer object around the generated reentrant scanner (2C spec §2.1): its
 * allocator, the fatal-error escape, the one diagnostic path (§3.4) and the functions the
 * actions of verilog.l call.
 */
#include "frontends/verilog/lexer_support.h"

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
    odin3_strbuf_init(&st->scratch);
    odin3_vec_init(&st->meta, sizeof(odin3_vl_meta));
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
    odin3_util_free(lx);
}

void *odin3_vl_scanner(const odin3_vl_lexer *lx) {
    return lx->scanner;
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
    va_list args;
    va_start(args, fmt);
    odin3_srcman_vdiag(sm, level, loc, fmt, args);
    va_end(args);
}

bool odin3_vl_in_error(const odin3_vl_run *run) {
    return run->errors > 0 || run->stop;
}

/* --- called by the generated scanner ------------------------------------------------------- */

void odin3_vl_user_action(odin3_vl_state *st, int len) {
    assert(len >= 0);
    st->tok_start = st->offset;
    st->offset += (uint32_t)len;
}

int odin3_vl_eof(odin3_vl_state *st, odin3_vp_loc *loc) {
    *loc = (odin3_vp_loc){st->last_end, st->last_end, (uint32_t)st->stream.text.len};
    return TOK_YYEOF;
}

void odin3_vl_unexpected(odin3_vl_state *st, const char *text) {
    unsigned int byte = (unsigned char)text[0];
    odin3_loc loc = odin3_srcman_cursor_loc(&st->cur, st->tok_start);
    odin3_vl_report(st->run, ODIN3_LOG_ERROR, loc, "unexpected byte 0x%02x", byte);
}
