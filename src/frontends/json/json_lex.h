/*
 * json_lex.h — internal: the byte cursor, errors, strings and integers of the JSON parser.
 */
#ifndef ODIN3_FRONTENDS_JSON_JSON_LEX_H
#define ODIN3_FRONTENDS_JSON_JSON_LEX_H

#include "frontends/json/json_parse.h"
#include "util/attr.h"
#include "util/vec.h"

typedef struct json_lex {
    const uint8_t *text;
    size_t len, pos, line_start;
    uint32_t line;
    odin3_json_limits lim;
    odin3_arena *arena;
    odin3_vec scratch; /* decoded string bytes (uint8_t) */
    odin3_json_error *err;
} json_lex;

/* Sets the cursor at the start of text (after a byte-order mark). Allocates nothing. */
void json_lex_init(json_lex *lex, const uint8_t *text, size_t len);

/* Skips blanks (space, tab, CR, LF), counting lines. */
void json_skip_ws(json_lex *lex);

/* The byte at the cursor or -1 at the end; json_advance moves one byte (not over a newline). */
int json_peek(const json_lex *lex);
void json_advance(json_lex *lex);

/* Line and column of the cursor. */
odin3_json_loc json_here(const json_lex *lex);

/* Records the error (printf message, cut to fit) at pos and returns code. */
odin3_status json_fail(json_lex *lex, odin3_json_loc pos, odin3_status code, const char *fmt, ...)
    ODIN3_PRINTF(4, 5);

/* The string at the cursor (which is a '"'), into the arena; *len is the decoded length. */
odin3_status json_lex_string(json_lex *lex, char **out, uint32_t *len);

/* The integer at the cursor (a '-' or digit). */
odin3_status json_lex_int(json_lex *lex, int64_t *out);

#endif
