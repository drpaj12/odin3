/*
 * words.h — the word splitter shared by .o3proj and -f/-F lists (PRJ-7, the oracle's
 * split_words).
 */
#ifndef ODIN3_FRONTENDS_PROJECT_WORDS_H
#define ODIN3_FRONTENDS_PROJECT_WORDS_H

#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum odin3_words_mode {
    ODIN3_WORDS_O3PROJ = 1, /* a '"' inside a bare word is syntax; words remember quoting */
    ODIN3_WORDS_FLIST       /* "//" also starts a comment; '"' inside a bare word is ordinary;
                               quoting is forgotten */
} odin3_words_mode;

/*
 * One word: off = file offset of its first byte (the opening quote of a quoted word); text = a
 * view of the line for a bare word, or the unescaped text in opts->arena for a quoted one (the
 * caller keeps the line alive while it uses bare words); quoted = it was quoted (O3PROJ only).
 */
typedef struct odin3_word {
    uint32_t off;
    odin3_bytes text;
    bool quoted;
} odin3_word;

typedef struct odin3_words_opts {
    uint32_t offset; /* file offset of the line's first byte */
    odin3_words_mode mode;
    odin3_arena *arena; /* receives quoted words' text */
} odin3_words_opts;

/* A syntax error: off = file offset of the offending word's first byte; word = that word as
 * spelled up to the end of the line or the next blank; what = the detail (static text). The
 * oracle names nothing for these errors, so callers report names []. */
typedef struct odin3_words_err {
    uint32_t off;
    odin3_bytes word;
    const char *what;
} odin3_words_err;

/*
 * Appends the words of one line (no '\n'; a '\r' is an ordinary byte) to words (odin3_word).
 * Blanks are space and tab; '#' (and "//" in FLIST) at the start of a word ends the line; a word
 * is bare (its backslashes ordinary) or wholly inside double quotes, where \" and \\ are the only
 * escapes. Errors (ODIN3_ERR_PARSE with *err filled, nothing logged): "unterminated quote", "bad
 * escape …", "characters after a closing quote", "empty quoted word", and in O3PROJ "quote inside
 * a word". NO_MEMORY. On failure words keeps its old length.
 */
odin3_status odin3_words_split(odin3_bytes line, const odin3_words_opts *opts, odin3_vec *words,
                               odin3_words_err *err);

#endif
