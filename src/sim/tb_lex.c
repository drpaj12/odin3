/* tb_lex.c — the testbench lexer: tokens of the supported subset (spec section 2). */
#include "sim/tb_internal.h"
#include "util/log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum {
    BASE_HEX = 16,
    BASE_DEC = 10,
    BASE_OCT = 8,
    BASE_BIN = 2,
    NUM_TEXT = 128,
    STR_MAX = 512,
    LEX_TOKS0 = 256,
    LEX_ALLOCS0 = 64,
    TIMESCALE_LEN = 10
};

static const char *const MULTI_OPS[] = {"<<<", ">>>", "===", "!==", "<<", ">>", "<=", ">=", "==",
                                        "!=",  "&&",  "||",  "~&",  "~|", "~^", "^~", "**", NULL};

void *tb_alloc(tb_module *tb, size_t size) {
    void *ptr = calloc(1, size == 0 ? 1 : size);
    if (ptr == NULL) {
        return NULL;
    }
    if (tb->nallocs == tb->cap_allocs) {
        uint32_t cap = tb->cap_allocs == 0 ? (uint32_t)LEX_ALLOCS0 : tb->cap_allocs * 2U;
        void *grown = realloc((void *)tb->allocs, (size_t)cap * sizeof(void *));
        if (grown == NULL) {
            free(ptr);
            return NULL;
        }
        tb->allocs = (void **)grown;
        tb->cap_allocs = cap;
    }
    tb->allocs[tb->nallocs++] = ptr;
    return ptr;
}

void tb_error(const char *file, uint32_t line, const char *what) {
    odin3_log(ODIN3_LOG_ERROR, "%s:%u: unsupported: %s", file, line, what);
}

static int digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + BASE_DEC;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + BASE_DEC;
    }
    return -1;
}

/* Digits of `base` from *cursor (underscores skipped); false on x/z/? digits. */
static bool based_digits(const char **cursor, unsigned base, uint64_t *val, bool *xz) {
    *val = 0;
    *xz = false;
    bool any = false;
    for (;; (*cursor)++) {
        char ch = **cursor;
        if (ch == '_') {
            continue;
        }
        if (strchr("xXzZ?", ch) != NULL && ch != '\0') {
            *xz = true;
            any = true;
            continue;
        }
        int dv = digit_value(ch);
        if (dv < 0 || (unsigned)dv >= base) {
            break;
        }
        *val = *val * base + (unsigned)dv;
        any = true;
    }
    return any;
}

static unsigned base_of(char ch) {
    switch (tolower((unsigned char)ch)) {
    case 'h':
        return BASE_HEX;
    case 'd':
        return BASE_DEC;
    case 'o':
        return BASE_OCT;
    case 'b':
        return BASE_BIN;
    default:
        return 0;
    }
}

/* A number at *cursor: [size]'[s]base digits, or plain decimal. Returns false on error. */
static bool lex_number(const char **cursor, tb_tok *tok, const char *file) {
    uint64_t first = 0;
    bool xz = false;
    const char *scan = *cursor;
    bool have_size = false;
    if (*scan != '\'') {
        have_size = true;
        (void)based_digits(&scan, BASE_DEC, &first, &xz);
    }
    tok->kind = TK_NUM;
    if (*scan == '.' && scan[1] >= '0' && scan[1] <= '9') {
        tb_error(file, tok->line, "real number");
        return false;
    }
    if (*scan != '\'') {
        tok->num = first;
        tok->width = 0;
        tok->is_signed = true;
        *cursor = scan;
        return true;
    }
    scan++;
    tok->is_signed = tolower((unsigned char)*scan) == 's';
    scan += tok->is_signed ? 1 : 0;
    unsigned base = base_of(*scan);
    scan++;
    if (base == 0 || !based_digits(&scan, base, &tok->num, &xz) || xz) {
        tb_error(file, tok->line, xz ? "x/z literal (the simulator is 2-state)" : "number literal");
        return false;
    }
    tok->sized = have_size;
    tok->width = have_size ? (uint32_t)first : 0;
    *cursor = scan;
    return true;
}

static char unescape(char ch) {
    if (ch == 'n') {
        return '\n';
    }
    if (ch == 't') {
        return '\t';
    }
    return ch;
}

static bool lex_string(const char **cursor, tb_tok *tok, tb_module *tb) {
    char *buf = tb_alloc(tb, STR_MAX);
    size_t count = 0;
    const char *scan = *cursor + 1;
    while (buf != NULL && *scan != '"' && *scan != '\0' && count + 1 < STR_MAX) {
        if (*scan == '\\' && scan[1] != '\0') {
            scan++;
            buf[count++] = unescape(*scan);
        } else {
            buf[count++] = *scan;
        }
        scan++;
    }
    if (buf == NULL || *scan != '"') {
        return false;
    }
    tok->kind = TK_STR;
    tok->str = buf;
    *cursor = scan + 1;
    return true;
}

static void lex_word(const char **cursor, tb_tok *tok, tb_tok_kind kind) {
    size_t count = 0;
    const char *scan = *cursor;
    if (kind == TK_SYS) {
        tok->text[count++] = *scan++;
    }
    while ((isalnum((unsigned char)*scan) || *scan == '_' || *scan == '$') && count + 1 < TB_NAME) {
        tok->text[count++] = *scan++;
    }
    tok->text[count] = '\0';
    tok->kind = kind;
    *cursor = scan;
}

static void lex_op(const char **cursor, tb_tok *tok) {
    tok->kind = TK_OP;
    for (size_t i = 0; MULTI_OPS[i] != NULL; i++) {
        size_t len = strlen(MULTI_OPS[i]);
        if (strncmp(*cursor, MULTI_OPS[i], len) == 0) {
            memcpy(tok->text, MULTI_OPS[i], len + 1);
            *cursor += len;
            return;
        }
    }
    tok->text[0] = **cursor;
    tok->text[1] = '\0';
    (*cursor)++;
}

static void skip_block_comment(const char **cursor, uint32_t *line) {
    *cursor += 2;
    while (**cursor != '\0' && !(cursor[0][0] == '*' && cursor[0][1] == '/')) {
        *line += **cursor == '\n' ? 1U : 0U;
        (*cursor)++;
    }
    *cursor += **cursor == '\0' ? 0 : 2;
}

static void skip_to_eol(const char **cursor) {
    while (**cursor != '\n' && **cursor != '\0') {
        (*cursor)++;
    }
}

/* One step over blanks, a comment or a `timescale line: 1 skipped, 0 at a token, -1 error. */
static int skip_step(const char **cursor, uint32_t *line, const char *file) {
    const char *scan = *cursor;
    if (isspace((unsigned char)*scan)) {
        if (*scan == '\n') {
            (*line)++;
        }
        (*cursor)++;
    } else if ((scan[0] == '/' && scan[1] == '/') ||
               (*scan == '`' && strncmp(scan, "`timescale", TIMESCALE_LEN) == 0)) {
        skip_to_eol(cursor);
    } else if (scan[0] == '/' && scan[1] == '*') {
        skip_block_comment(cursor, line);
    } else if (*scan == '`') {
        tb_error(file, *line, "compiler directive");
        return -1;
    } else {
        return 0;
    }
    return 1;
}

/* Skips blanks and comments; counts lines; accepts `timescale lines (ignored). */
static bool skip_trivia(const char **cursor, uint32_t *line, const char *file) {
    int step = 1;
    while (step > 0) {
        step = skip_step(cursor, line, file);
    }
    return step == 0;
}

/* Lexes one token at *cursor (not at the end) into tok. */
static bool lex_token(const char **cursor, tb_tok *tok, const tb_source *src, tb_module *tb) {
    if (isalpha((unsigned char)**cursor) || **cursor == '_') {
        lex_word(cursor, tok, TK_ID);
    } else if (**cursor == '$') {
        lex_word(cursor, tok, TK_SYS);
    } else if (isdigit((unsigned char)**cursor) || **cursor == '\'') {
        return lex_number(cursor, tok, src->file);
    } else if (**cursor == '"') {
        return lex_string(cursor, tok, tb);
    } else {
        lex_op(cursor, tok);
    }
    return true;
}

bool tb_lex(const tb_source *src, tb_module *tb, tb_lexed *out) {
    uint32_t line = 1;
    const char *cursor = src->text;
    uint32_t cap = LEX_TOKS0;
    out->toks = tb_alloc(tb, (size_t)cap * sizeof *out->toks);
    out->n = 0;
    while (out->toks != NULL) {
        if (!skip_trivia(&cursor, &line, src->file)) {
            return false;
        }
        if (out->n + 1 >= cap) {
            tb_tok *grown = tb_alloc(tb, (size_t)cap * 2U * sizeof *grown);
            if (grown == NULL) {
                return false;
            }
            memcpy(grown, out->toks, (size_t)out->n * sizeof *grown);
            out->toks = grown;
            cap *= 2U;
        }
        tb_tok *tok = &out->toks[out->n++];
        tok->line = line;
        if (*cursor == '\0') {
            tok->kind = TK_EOF;
            return true;
        }
        if (!lex_token(&cursor, tok, src, tb)) {
            return false;
        }
    }
    return false;
}
