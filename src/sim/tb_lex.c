/* tb_lex.c — the testbench lexer: tokens of the supported subset (spec section 2). */
#include "sim/tb_internal.h"
#include "util/log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { BASE_HEX = 16, BASE_DEC = 10, BASE_OCT = 8, BASE_BIN = 2, NUM_TEXT = 128, STR_MAX = 512 };

static const char *const MULTI_OPS[] = {"<<<", ">>>", "===", "!==", "<<", ">>", "<=", ">=", "==",
                                        "!=",  "&&",  "||",  "~&",  "~|", "~^", "^~", "**", NULL};

void *tb_alloc(tb_module *tb, size_t size) {
    void *ptr = calloc(1, size == 0 ? 1 : size);
    if (ptr == NULL) {
        return NULL;
    }
    if (tb->nallocs == tb->cap_allocs) {
        uint32_t cap = tb->cap_allocs == 0 ? 64U : tb->cap_allocs * 2U;
        void **grown = realloc(tb->allocs, cap * sizeof *grown);
        if (grown == NULL) {
            free(ptr);
            return NULL;
        }
        tb->allocs = grown;
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

/* Digits of `base` from *p (underscores skipped); false on x/z/? digits. */
static bool based_digits(const char **p, unsigned base, uint64_t *val, bool *xz) {
    *val = 0;
    *xz = false;
    bool any = false;
    for (;; (*p)++) {
        char ch = **p;
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

/* A number at *p: [size]'[s]base digits, or plain decimal. Returns false on error. */
static bool lex_number(const char **p, tb_tok *tok, const char *file) {
    uint64_t first = 0;
    bool xz = false;
    const char *q = *p;
    bool have_size = false;
    if (*q != '\'') {
        have_size = true;
        (void)based_digits(&q, BASE_DEC, &first, &xz);
    }
    tok->kind = TK_NUM;
    if (*q == '.' && q[1] >= '0' && q[1] <= '9') {
        tb_error(file, tok->line, "real number");
        return false;
    }
    if (*q != '\'') {
        tok->num = first;
        tok->width = 0;
        tok->is_signed = true;
        *p = q;
        return true;
    }
    q++;
    tok->is_signed = tolower((unsigned char)*q) == 's';
    q += tok->is_signed ? 1 : 0;
    unsigned base = base_of(*q);
    q++;
    if (base == 0 || !based_digits(&q, base, &tok->num, &xz) || xz) {
        tb_error(file, tok->line, xz ? "x/z literal (the simulator is 2-state)" : "number literal");
        return false;
    }
    tok->sized = have_size;
    tok->width = have_size ? (uint32_t)first : 0;
    *p = q;
    return true;
}

static bool lex_string(const char **p, tb_tok *tok, tb_module *tb) {
    char *buf = tb_alloc(tb, STR_MAX);
    size_t n = 0;
    const char *q = *p + 1;
    while (buf != NULL && *q != '"' && *q != '\0' && n + 1 < STR_MAX) {
        if (*q == '\\' && q[1] != '\0') {
            q++;
            buf[n++] = *q == 'n' ? '\n' : (*q == 't' ? '\t' : *q);
        } else {
            buf[n++] = *q;
        }
        q++;
    }
    if (buf == NULL || *q != '"') {
        return false;
    }
    tok->kind = TK_STR;
    tok->str = buf;
    *p = q + 1;
    return true;
}

static void lex_word(const char **p, tb_tok *tok, tb_tok_kind kind) {
    size_t n = 0;
    const char *q = *p;
    if (kind == TK_SYS) {
        tok->text[n++] = *q++;
    }
    while ((isalnum((unsigned char)*q) || *q == '_' || *q == '$') && n + 1 < TB_NAME) {
        tok->text[n++] = *q++;
    }
    tok->text[n] = '\0';
    tok->kind = kind;
    *p = q;
}

static void lex_op(const char **p, tb_tok *tok) {
    tok->kind = TK_OP;
    for (size_t i = 0; MULTI_OPS[i] != NULL; i++) {
        size_t len = strlen(MULTI_OPS[i]);
        if (strncmp(*p, MULTI_OPS[i], len) == 0) {
            memcpy(tok->text, MULTI_OPS[i], len + 1);
            *p += len;
            return;
        }
    }
    tok->text[0] = **p;
    tok->text[1] = '\0';
    (*p)++;
}

static void skip_block_comment(const char **p, uint32_t *line) {
    *p += 2;
    while (**p != '\0' && !(p[0][0] == '*' && p[0][1] == '/')) {
        *line += **p == '\n' ? 1U : 0U;
        (*p)++;
    }
    *p += **p == '\0' ? 0 : 2;
}

static void skip_to_eol(const char **p) {
    while (**p != '\n' && **p != '\0') {
        (*p)++;
    }
}

/* Skips blanks and comments; counts lines; accepts `timescale lines (ignored). */
static bool skip_trivia(const char **p, uint32_t *line, const char *file) {
    for (;;) {
        const char *q = *p;
        if (*q == '\n') {
            (*line)++;
            (*p)++;
        } else if (isspace((unsigned char)*q)) {
            (*p)++;
        } else if (q[0] == '/' && q[1] == '/') {
            skip_to_eol(p);
        } else if (q[0] == '/' && q[1] == '*') {
            skip_block_comment(p, line);
        } else if (*q == '`' && strncmp(q, "`timescale", 10) == 0) {
            skip_to_eol(p);
        } else if (*q == '`') {
            tb_error(file, *line, "compiler directive");
            return false;
        } else {
            return true;
        }
    }
}

bool tb_lex(const char *file, const char *text, tb_module *tb, tb_lexed *out) {
    uint32_t line = 1;
    const char *p = text;
    uint32_t cap = 256;
    out->toks = tb_alloc(tb, cap * sizeof *out->toks);
    out->n = 0;
    while (out->toks != NULL) {
        if (!skip_trivia(&p, &line, file)) {
            return false;
        }
        if (out->n + 1 >= cap) {
            tb_tok *grown = tb_alloc(tb, cap * 2U * sizeof *grown);
            if (grown == NULL) {
                return false;
            }
            memcpy(grown, out->toks, out->n * sizeof *grown);
            out->toks = grown;
            cap *= 2U;
        }
        tb_tok *tok = &out->toks[out->n++];
        tok->line = line;
        bool ok = true;
        if (*p == '\0') {
            tok->kind = TK_EOF;
            return true;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            lex_word(&p, tok, TK_ID);
        } else if (*p == '$') {
            lex_word(&p, tok, TK_SYS);
        } else if (isdigit((unsigned char)*p) || *p == '\'') {
            ok = lex_number(&p, tok, file);
        } else if (*p == '"') {
            ok = lex_string(&p, tok, tb);
        } else {
            lex_op(&p, tok);
        }
        if (!ok) {
            return false;
        }
    }
    return false;
}
