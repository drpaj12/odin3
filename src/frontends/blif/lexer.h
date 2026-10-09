/*
 * lexer.h — BLIF lexer: buffered reading into logical lines of tokens.
 */
#ifndef ODIN3_FRONTENDS_BLIF_LEXER_H
#define ODIN3_FRONTENDS_BLIF_LEXER_H

#include "odin3/odin3.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_blif_lexer odin3_blif_lexer;

/*
 * One logical line: continuations joined, comments stripped. Token i has 1-based column index
 * i + 1 within the logical line. Every token is NUL-terminated (tokens[i].len excludes the NUL).
 * `line` is the first physical line (1-based) holding a token of the logical line.
 */
typedef struct odin3_blif_line {
    const odin3_bytes *tokens;
    uint32_t count;
    uint32_t line;
} odin3_blif_line;

/* Opens `path`; NULL (logged as an error) on an I/O error or out of memory. */
odin3_blif_lexer *odin3_blif_lexer_open(const char *path);

/*
 * Reads the next non-blank logical line: `\` at the end of a physical line (optionally followed by
 * blanks) continues it, `#` starts a comment to the end of the physical line, tokens split on
 * blanks (space, tab), a CR before LF is ignored. Returns false at end of input or on failure
 * (see odin3_blif_lexer_status); on an I/O error, out of memory or a NUL byte (ODIN3_ERR_PARSE,
 * logged as file:line: message) it returns false and no partial line is published. A `\` not at
 * the end of a physical line is an ordinary character (`a\ b` gives `a\` and `b`). The tokens stay
 * valid until the next call. Reads the file in 64 KiB chunks; time is linear in the input.
 */
bool odin3_blif_lexer_next(odin3_blif_lexer *lx, odin3_blif_line *out);

/* ODIN3_OK, ODIN3_ERR_IO, ODIN3_ERR_NO_MEMORY or ODIN3_ERR_PARSE (sticky; once set, next returns
 * false). */
odin3_status odin3_blif_lexer_status(const odin3_blif_lexer *lx);

void odin3_blif_lexer_close(odin3_blif_lexer *lx);

#endif
