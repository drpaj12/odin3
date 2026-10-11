/*
 * lexer_internal.h — helpers shared by the lexer's sources (lexer_support.c, lexer_tokens.c,
 * lexer_trivia.c); nothing outside them includes it.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_LEXER_INTERNAL_H
#define ODIN3_FRONTENDS_VERILOG_LEXER_INTERNAL_H

#include "ast/srcman.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>

/* The scanner state of a lexer. */
const odin3_vl_state *vl_state_of(const odin3_vl_lexer *lx);

/* What a token function returns when it consumed bytes without a token (inside an opaque body
 * or after a fatal failure): odin3_vp_lex scans on. Never a token number. */
enum { VL_SKIP = -1 };

/* The loc of stream offset off (the segment cursor, §2.2). */
odin3_loc vl_loc(odin3_vl_state *st, uint32_t off);

/* The stream bytes [lo, hi). */
odin3_bytes vl_bytes(const odin3_vl_state *st, uint32_t lo, uint32_t hi);

/* The current rule's bytes [tok_start, offset). */
odin3_bytes vl_text(const odin3_vl_state *st);

/* Stream offsets [lo, hi). */
typedef struct vl_span {
    uint32_t lo, hi;
} vl_span;

/*
 * Finishes the token over span (non-empty) of kind: *loc gets {loc(lo), loc(hi - 1) + 1, lo} and
 * last_end its end; returns kind.
 */
int vl_token_at(odin3_vl_state *st, odin3_vp_loc *loc, vl_span span, int kind);

/* The current rule as one token of kind (vl_token_at over [tok_start, offset)). */
int vl_token(odin3_vl_state *st, odin3_vp_loc *loc, int kind);

/*
 * Inside an opaque body every token is part of the body's text: true (and the body has now seen
 * a token after its keyword) when the current rule is skipped that way.
 */
bool vl_in_body(odin3_vl_state *st);

/* The end of input inside an opaque body: the located error at its keyword; the body ends. */
void vl_body_unclosed(odin3_vl_state *st);

/* A located lexer error: counted, logged, and the error flag set (§3.4). */
void vl_error(odin3_vl_state *st, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* A located lexer warning (never counted, never the error flag). */
void vl_warn(odin3_vl_state *st, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* A lexer-side fatal failure (NO_MEMORY): records it, stops the run, returns VL_SKIP (the stream
 * then ends, odin3_vp_lex). */
int vl_fatal(odin3_vl_state *st, odin3_status status);

/* Interns bytes as *id; a failure is fatal (vl_fatal) and false is returned. */
bool vl_intern(odin3_vl_state *st, odin3_bytes bytes, uint32_t *id);

/* The active keyword set of the run (§2.5): the top of its `begin_keywords stack, else 2005. */
uint32_t vl_keyword_mask(const odin3_vl_run *run);

/* At the end of the stream: one warning per `begin_keywords still open (§2.5). */
void vl_keywords_at_end(odin3_vl_state *st, odin3_loc at);

#endif
