/*
 * tcl.c — the Tcl word reader for .qsf/.qip files (PRJ-13, the oracle's TclReader byte for byte).
 */
#include "frontends/project/tcl.h"

#include "frontends/project/paths.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NO_BYTE = -1, ASCII_LIMIT = 0x80 };

/* --- Python's whitespace ------------------------------------------------------------------- */

/* The UTF-8 encodings of the non-ASCII characters str.isspace() accepts. */
static const char *const k_wide_spaces[] = {
    "\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x81",
    "\xE2\x80\x82", "\xE2\x80\x83", "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86",
    "\xE2\x80\x87", "\xE2\x80\x88", "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xA8",
    "\xE2\x80\xA9", "\xE2\x80\xAF", "\xE2\x81\x9F", "\xE3\x80\x80",
};

size_t odin3_py_space_len(odin3_bytes text, size_t pos) {
    const uint8_t *bytes = text.ptr;
    if (pos >= text.len) {
        return 0;
    }
    uint8_t byte = bytes[pos];
    if (byte == ' ' || (byte >= '\t' && byte <= '\r') || (byte >= '\x1c' && byte <= '\x1f')) {
        return 1;
    }
    if (byte < ASCII_LIMIT) {
        return 0;
    }
    for (size_t k = 0; k < sizeof k_wide_spaces / sizeof k_wide_spaces[0]; k++) {
        size_t len = strlen(k_wide_spaces[k]);
        if (text.len - pos >= len && memcmp(bytes + pos, k_wide_spaces[k], len) == 0) {
            return len;
        }
    }
    return 0;
}

/* --- the reader ---------------------------------------------------------------------------- */

/* The text with CRLF folded to LF, the cursor, and what maps offsets back to the text as given. */
typedef struct reader {
    uint8_t *text; /* owned */
    size_t len;
    size_t i;
    odin3_vec folds; /* uint32_t: folded offset of each '\n' whose '\r' was dropped */
    const odin3_tcl_opts *opts;
    odin3_tcl_err *err;
    odin3_vec words;   /* odin3_tcl_word: the command being read */
    odin3_strbuf word; /* the word being read */
} reader;

/* Copies text with every "\r\n" folded to "\n", recording where each fold happened. */
static odin3_status fold(reader *rd, odin3_bytes text) {
    const uint8_t *bytes = text.ptr;
    rd->text = odin3_util_malloc(text.len + 1);
    if (rd->text == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t len = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (bytes[i] == '\r' && i + 1 < text.len && bytes[i + 1] == '\n') {
            uint32_t *at = odin3_vec_push(&rd->folds);
            if (at == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *at = (uint32_t)len;
            continue;
        }
        rd->text[len++] = bytes[i];
    }
    rd->len = len;
    return ODIN3_OK;
}

/* The offset in the text as given of folded offset off: off plus the folds at or before it. */
static uint32_t given_off(const reader *rd, size_t off) {
    const uint32_t *folds = rd->folds.data;
    size_t lo = 0;
    size_t hi = rd->folds.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (folds[mid] <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (uint32_t)(off + lo);
}

static int peek(const reader *rd, size_t ahead) {
    return rd->i + ahead < rd->len ? rd->text[rd->i + ahead] : NO_BYTE;
}

static bool continuation(const reader *rd) {
    return peek(rd, 0) == '\\' && peek(rd, 1) == '\n';
}

/* Space, tab or CR (a blank inside a command). */
static bool blank(int byte) {
    return byte == ' ' || byte == '\t' || byte == '\r';
}

static odin3_status fail(reader *rd, size_t off, const char *what) {
    rd->err->off = given_off(rd, off);
    rd->err->what = what;
    return ODIN3_ERR_PARSE;
}

/* Skips blanks and continuations; with newlines also newlines, ';' and '#' comments. */
static void skip_blank(reader *rd, bool newlines) {
    for (;;) {
        int byte = peek(rd, 0);
        if (blank(byte) || (newlines && (byte == '\n' || byte == ';'))) {
            rd->i++;
        } else if (continuation(rd)) {
            rd->i += 2;
        } else if (newlines && byte == '#') {
            while (peek(rd, 0) != '\n' && peek(rd, 0) != NO_BYTE) {
                rd->i++;
            }
        } else {
            return;
        }
    }
}

/* After a closing quote, brace or bracket: a blank, ';', newline, continuation or the end. */
static odin3_status end_of_word(reader *rd, const char *what) {
    int byte = peek(rd, 0);
    if (byte != NO_BYTE && !blank(byte) && byte != '\n' && byte != ';' && !continuation(rd)) {
        return fail(rd, rd->i, what);
    }
    return ODIN3_OK;
}

static odin3_status add_byte(reader *rd, uint8_t byte) {
    return odin3_strbuf_append(&rd->word, (odin3_bytes){&byte, 1});
}

/* One character inside quotes at rd->i (the cursor moves past it): its byte in *out. */
static odin3_status quoted_char(reader *rd, size_t open, uint8_t *out) {
    int byte = peek(rd, 0);
    size_t at = rd->i++;
    if (byte == NO_BYTE || byte == '\n') {
        return fail(rd, open, "unterminated quote");
    }
    if (byte == '$') {
        return fail(rd, at, "$ substitution is not supported");
    }
    if (byte == '\\') {
        byte = peek(rd, 0);
        rd->i++;
        if (byte != '"' && byte != '\\') { /* the oracle counts a newline before it reports */
            return fail(rd, byte == '\n' ? rd->i : at, "bad escape (only \\\" and \\\\)");
        }
    }
    *out = (uint8_t)byte;
    return ODIN3_OK;
}

static odin3_status quoted(reader *rd) {
    size_t open = rd->i++;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && peek(rd, 0) != '"') {
        uint8_t byte = 0;
        st = quoted_char(rd, open, &byte);
        st = st != ODIN3_OK ? st : add_byte(rd, byte);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    rd->i++;
    return end_of_word(rd, "extra characters after close-quote");
}

static odin3_status braced(reader *rd) {
    rd->i++;
    size_t depth = 1;
    for (;;) {
        int byte = peek(rd, 0);
        if (byte == NO_BYTE) {
            return fail(rd, rd->len, "unterminated brace");
        }
        rd->i++;
        if (byte == '{') {
            depth++;
        } else if (byte == '}' && --depth == 0) {
            return end_of_word(rd, "extra characters after close-brace");
        }
        odin3_status st = add_byte(rd, (uint8_t)byte);
        if (st != ODIN3_OK) {
            return st;
        }
    }
}

/* --- the .qip idiom ------------------------------------------------------------------------ */

static const char k_idiom[] = "[file join $::quartus(qip_path) ";

static bool in_set(int byte, const char *set) {
    return byte != NO_BYTE && byte != '\0' && strchr(set, byte) != NULL;
}

/* The idiom's path at from (after the prefix): its span, and the index after the ']'. false
 * when the regex QIP_IDIOM would not match. */
typedef struct idiom_path {
    size_t start, end, stop;
} idiom_path;

static bool match_path(const reader *rd, size_t from, idiom_path *path) {
    size_t j = from;
    bool quote = j < rd->len && rd->text[j] == '"';
    j += quote;
    size_t start = j;
    odin3_bytes text = {rd->text, rd->len};
    while (j < rd->len && !in_set(rd->text[j], quote ? "\"\\$[]" : "\"\\$[]{}") &&
           (quote || odin3_py_space_len(text, j) == 0)) {
        j++;
    }
    size_t end = j;
    if (quote) {
        if (j >= rd->len || rd->text[j] != '"') {
            return false;
        }
        j++;
    }
    *path = (idiom_path){start, end, j + 1};
    return end > start && j < rd->len && rd->text[j] == ']';
}

static odin3_status bracket(reader *rd) {
    size_t prefix = sizeof k_idiom - 1;
    idiom_path path = {0, 0, 0};
    bool idiom = rd->opts->qip && rd->len - rd->i >= prefix &&
                 memcmp(rd->text + rd->i, k_idiom, prefix) == 0 &&
                 match_path(rd, rd->i + prefix, &path);
    if (!idiom) {
        return fail(rd, rd->i,
                    "command substitution [...] is not supported (only the .qip idiom "
                    "[file join $::quartus(qip_path) \"<path>\"])");
    }
    rd->i = path.stop;
    odin3_status st = end_of_word(rd, "extra characters after close-bracket");
    odin3_bytes given = {rd->text + path.start, path.end - path.start};
    return st != ODIN3_OK
               ? st
               : odin3_path_resolve((odin3_path_join){rd->opts->qip_dir, given}, &rd->word);
}

/* --- words and commands -------------------------------------------------------------------- */

static odin3_status bare(reader *rd) {
    for (int byte = peek(rd, 0);
         byte != NO_BYTE && !blank(byte) && byte != '\n' && byte != ';' && !continuation(rd);
         byte = peek(rd, 0)) {
        if (byte == '$') {
            return fail(rd, rd->i, "$ in a bare word is not supported");
        }
        if (byte == '\\') {
            return fail(rd, rd->i, "\\ in a bare word is not supported");
        }
        rd->i++;
        odin3_status st = add_byte(rd, (uint8_t)byte);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Reads one word at rd->i and appends it to rd->words (text copied into the arena). */
static odin3_status word(reader *rd) {
    size_t start = rd->i;
    int byte = peek(rd, 0);
    odin3_strbuf_clear(&rd->word);
    odin3_status st = ODIN3_OK;
    if (byte == '"') {
        st = quoted(rd);
    } else if (byte == '{') {
        st = braced(rd);
    } else if (byte == '[') {
        st = bracket(rd);
    } else {
        st = bare(rd);
    }
    if (st == ODIN3_OK && rd->word.len > 0 && memchr(rd->word.data, '\0', rd->word.len) != NULL) {
        st = fail(rd, start, "NUL byte in word");
    }
    if (st != ODIN3_OK) {
        return st;
    }
    char *text = odin3_arena_strndup(rd->opts->arena, rd->word.data, rd->word.len);
    odin3_tcl_word *slot = text != NULL ? odin3_vec_push(&rd->words) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_tcl_word){{text, rd->word.len}, given_off(rd, start)};
    return ODIN3_OK;
}

/* Moves rd->words into the arena as one command. */
static odin3_status add_command(reader *rd, odin3_vec *commands) {
    size_t bytes = rd->words.len * sizeof(odin3_tcl_word);
    odin3_tcl_word *words = odin3_arena_alloc(rd->opts->arena, bytes);
    odin3_tcl_command *slot = words != NULL ? odin3_vec_push(commands) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(words, rd->words.data, bytes);
    *slot = (odin3_tcl_command){words, (uint32_t)rd->words.len, words[0].off};
    odin3_vec_clear(&rd->words);
    return ODIN3_OK;
}

static odin3_status read_commands(reader *rd, odin3_vec *commands) {
    odin3_status st = ODIN3_OK;
    for (;;) {
        skip_blank(rd, true);
        if (peek(rd, 0) == NO_BYTE) {
            return ODIN3_OK;
        }
        while (st == ODIN3_OK && peek(rd, 0) != NO_BYTE && peek(rd, 0) != '\n' &&
               peek(rd, 0) != ';') {
            st = word(rd);
            skip_blank(rd, false);
        }
        st = st != ODIN3_OK ? st : add_command(rd, commands);
        if (st != ODIN3_OK) {
            return st;
        }
    }
}

odin3_status odin3_tcl_tokenize(odin3_bytes text, const odin3_tcl_opts *opts, odin3_vec *commands,
                                odin3_tcl_err *err) {
    reader rd = {.opts = opts, .err = err};
    odin3_vec_init(&rd.folds, sizeof(uint32_t));
    odin3_vec_init(&rd.words, sizeof(odin3_tcl_word));
    odin3_strbuf_init(&rd.word);
    size_t old_len = commands->len;
    odin3_status st = fold(&rd, text);
    st = st != ODIN3_OK ? st : read_commands(&rd, commands);
    while (st != ODIN3_OK && commands->len > old_len) {
        odin3_vec_pop(commands);
    }
    odin3_strbuf_free(&rd.word);
    odin3_vec_free(&rd.words);
    odin3_vec_free(&rd.folds);
    odin3_util_free(rd.text);
    return st;
}
