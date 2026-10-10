/*
 * words.c — the word splitter shared by .o3proj and -f/-F lists (PRJ-7, the oracle's
 * split_words).
 */
#include "frontends/project/words.h"

#include "util/arena.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One line being split: bytes, length, cursor, options, and where an error goes. */
typedef struct splitter {
    const uint8_t *bytes;
    size_t len;
    size_t i;
    const odin3_words_opts *opts;
    odin3_words_err *err;
} splitter;

static bool is_blank(uint8_t byte) {
    return byte == ' ' || byte == '\t';
}

/* The end of the word starting at from: the next blank or the end of the line. */
static size_t word_end(const splitter *sp, size_t from) {
    size_t j = from;
    while (j < sp->len && !is_blank(sp->bytes[j])) {
        j++;
    }
    return j;
}

static odin3_status syntax(splitter *sp, size_t start, const char *what) {
    sp->err->off = sp->opts->offset + (uint32_t)start;
    sp->err->word = (odin3_bytes){sp->bytes + start, word_end(sp, start) - start};
    sp->err->what = what;
    return ODIN3_ERR_PARSE;
}

static odin3_status push_word(odin3_vec *words, uint32_t off, odin3_bytes text, bool quoted) {
    odin3_word *word = odin3_vec_push(words);
    if (word == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *word = (odin3_word){off, text, quoted};
    return ODIN3_OK;
}

/* A checked quoted word: the index of its closing quote and its unescaped length. */
typedef struct quoted_span {
    size_t stop;
    size_t len;
} quoted_span;

/*
 * Checks the quoted word starting at the '"' at sp->i and measures its unescaped text. The
 * oracle's order: unterminated, bad escape (as met), characters after the closing quote, empty.
 */
static odin3_status scan_quoted(splitter *sp, quoted_span *span) {
    size_t start = sp->i;
    size_t j = start + 1;
    size_t len = 0;
    while (j < sp->len && sp->bytes[j] != '"') {
        if (sp->bytes[j] == '\\') {
            if (j + 1 >= sp->len || (sp->bytes[j + 1] != '"' && sp->bytes[j + 1] != '\\')) {
                return syntax(sp, start, "bad escape (only \\\" and \\\\ inside quotes)");
            }
            j++;
        }
        j++;
        len++;
    }
    if (j >= sp->len) {
        return syntax(sp, start, "unterminated quote");
    }
    if (j + 1 < sp->len && !is_blank(sp->bytes[j + 1])) {
        return syntax(sp, start, "characters after a closing quote");
    }
    if (len == 0) {
        return syntax(sp, start, "empty quoted word");
    }
    if (memchr(sp->bytes + start, '\0', j - start) != NULL) {
        return syntax(sp, start, "NUL byte in word");
    }
    *span = (quoted_span){j, len};
    return ODIN3_OK;
}

/* A quoted word: its unescaped text goes into the arena. */
static odin3_status quoted_word(splitter *sp, odin3_vec *words) {
    quoted_span span = {0, 0};
    odin3_status st = scan_quoted(sp, &span);
    if (st != ODIN3_OK) {
        return st;
    }
    char *text = odin3_arena_alloc(sp->opts->arena, span.len);
    if (text == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t k = 0;
    for (size_t j = sp->i + 1; j < span.stop; j++) {
        if (sp->bytes[j] == '\\') {
            j++;
        }
        text[k++] = (char)sp->bytes[j];
    }
    bool quoted = sp->opts->mode == ODIN3_WORDS_O3PROJ;
    st =
        push_word(words, sp->opts->offset + (uint32_t)sp->i, (odin3_bytes){text, span.len}, quoted);
    sp->i = span.stop + 1;
    return st;
}

/* A bare word: a view of the line; a '"' inside is an error in .o3proj. */
static odin3_status bare_word(splitter *sp, odin3_vec *words) {
    size_t start = sp->i;
    size_t end = word_end(sp, start);
    if (sp->opts->mode == ODIN3_WORDS_O3PROJ &&
        memchr(sp->bytes + start, '"', end - start) != NULL) {
        return syntax(sp, start, "quote inside a word");
    }
    if (memchr(sp->bytes + start, '\0', end - start) != NULL) {
        return syntax(sp, start, "NUL byte in word");
    }
    sp->i = end;
    return push_word(words, sp->opts->offset + (uint32_t)start,
                     (odin3_bytes){sp->bytes + start, end - start}, false);
}

static bool starts_comment(const splitter *sp) {
    if (sp->bytes[sp->i] == '#') {
        return true;
    }
    return sp->opts->mode == ODIN3_WORDS_FLIST && sp->bytes[sp->i] == '/' && sp->i + 1 < sp->len &&
           sp->bytes[sp->i + 1] == '/';
}

odin3_status odin3_words_split(odin3_bytes line, const odin3_words_opts *opts, odin3_vec *words,
                               odin3_words_err *err) {
    splitter sp = {line.ptr, line.len, 0, opts, err};
    size_t old_len = words->len;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && sp.i < sp.len) {
        if (is_blank(sp.bytes[sp.i])) {
            sp.i++;
        } else if (starts_comment(&sp)) {
            break;
        } else if (sp.bytes[sp.i] == '"') {
            st = quoted_word(&sp, words);
        } else {
            st = bare_word(&sp, words);
        }
    }
    while (st != ODIN3_OK && words->len > old_len) {
        odin3_vec_pop(words);
    }
    return st;
}
