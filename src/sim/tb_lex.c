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

/* Decimal digits from *cursor (underscores skipped); *xz when an x, z or ? digit was among them. */
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

/* True when ch may stand in the digits of a number in base: a digit of the base, x, z, ? or _. */
static bool digit_char(char ch, unsigned base) {
    if (ch == '\0') {
        return false;
    }
    if (ch == '_' || strchr("xXzZ?", ch) != NULL) {
        return true;
    }
    int dv = digit_value(ch);
    return dv >= 0 && (unsigned)dv < base;
}

/* Stores the literal's planes (width bits) in the token. */
static bool keep_planes(tb_module *tb, tb_tok *tok, const tbv *val, uint32_t width) {
    size_t bytes = (size_t)odin3_word_limbs(width) * sizeof(uint64_t);
    tok->nv = tb_alloc(tb, bytes);
    tok->nu = tb_alloc(tb, bytes);
    if (tok->nv == NULL || tok->nu == NULL) {
        return false;
    }
    memcpy(tok->nv, val->v, bytes);
    memcpy(tok->nu, val->u, bytes);
    tok->num = val->v[0];
    return true;
}

/* A plain decimal number: unsized (32 bits), signed. */
static bool plain_number(tb_module *tb, tb_tok *tok, uint64_t num) {
    tbv val;
    tok->width = 0;
    tok->is_signed = true;
    tbv_set_u64(&val, TB_INT_BITS, num);
    if (!keep_planes(tb, tok, &val, TB_INT_BITS)) {
        return false;
    }
    tok->num = num;
    return true;
}

/* The digits of a based number after its base letter, sized (IEEE 1364-2005 3.5.1): truncated to
 * the size, or padded with 0, or with x or z when the leftmost digit is x or z. */
static bool based_value(const char **scan, tb_tok *tok, unsigned base, tb_module *tb) {
    const char *first = *scan;
    while (digit_char(**scan, base)) {
        (*scan)++;
    }
    tb_digits digits = {first, (size_t)(*scan - first), base};
    tbv val;
    uint32_t bits = 0;
    if (base == 0 || !tb_parse_digits(&digits, &val, &bits)) {
        return false;
    }
    uint32_t width = tok->sized ? tok->width : (uint32_t)TB_INT_BITS;
    tb_resize rs = {bits, width, false, true};
    tbv_extend(&val, &rs);
    tok->xfill = !tok->sized && (tbv_code(&val, width - 1U) & ODIN3_W4_X) != 0;
    return keep_planes(tb, tok, &val, width);
}

/* A number at *cursor: [size]'[s]base digits, or plain decimal. Returns false on error. */
static bool lex_number(const char **cursor, tb_tok *tok, const char *file, tb_module *tb) {
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
        *cursor = scan;
        return plain_number(tb, tok, first);
    }
    if (have_size && (first == 0 || first > TB_MAX_WIDTH)) {
        tb_error(file, tok->line, "literal size (1 to 1024 bits)");
        return false;
    }
    scan++;
    tok->is_signed = tolower((unsigned char)*scan) == 's';
    scan += tok->is_signed ? 1 : 0;
    unsigned base = base_of(*scan);
    scan++;
    tok->sized = have_size;
    tok->width = have_size ? (uint32_t)first : 0;
    if (!based_value(&scan, tok, base, tb)) {
        tb_error(file, tok->line, "number literal");
        return false;
    }
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

/* One `timescale term "<1|10|100> <unit>" at *text (blanks allowed): its power of ten in seconds,
 * with *text moved past it; TIME_BAD when it is not a term. */
enum { TIME_BAD = 1000, TIME_UNITS = 6, TIME_STEP = 3, TIME_MAX_DIGITS = 3, TIME_MAX_EXP = 18 };

static void skip_blanks(const char **text) {
    while (**text == ' ' || **text == '\t') {
        (*text)++;
    }
}

static int scan_time_term(const char **text) {
    static const char *const UNITS[TIME_UNITS] = {"s", "ms", "us", "ns", "ps", "fs"};
    const char *cur = *text;
    skip_blanks(&cur);
    size_t ndig = 0;
    while (isdigit((unsigned char)cur[ndig])) {
        ndig++;
    }
    if (ndig == 0 || ndig > TIME_MAX_DIGITS || cur[0] != '1' || strspn(cur + 1, "0") != ndig - 1U) {
        return TIME_BAD;
    }
    cur += ndig;
    skip_blanks(&cur);
    size_t nlet = 0;
    while (isalpha((unsigned char)cur[nlet])) {
        nlet++;
    }
    for (int i = 0; i < TIME_UNITS; i++) {
        if (strlen(UNITS[i]) == nlet && strncmp(cur, UNITS[i], nlet) == 0) {
            *text = cur + nlet;
            return (int)ndig - 1 - (i == 0 ? 0 : TIME_STEP * i);
        }
    }
    return TIME_BAD;
}

/* Keeps the time unit of `timescale <n> <unit> / <precision>: digits, optional blanks, letters
 * (1ns, 1 ns, 100 ps / 10 ps all work); stored as "<n> <unit>". time_scale is unit / precision
 * (a power of ten, 1 without a valid precision): %t prints times in the precision (LRM 21.2.1.5).
 */
static void take_timescale(const char *text, tb_module *tb) {
    skip_blanks(&text);
    size_t ndig = 0;
    while (isdigit((unsigned char)text[ndig])) {
        ndig++;
    }
    const char *unit = text + ndig;
    skip_blanks(&unit);
    size_t nlet = 0;
    while (isalpha((unsigned char)unit[nlet])) {
        nlet++;
    }
    if (ndig > 0 && nlet > 0 && ndig + nlet + 2 <= sizeof tb->timescale) {
        (void)snprintf(tb->timescale, sizeof tb->timescale, "%.*s %.*s", (int)ndig, text, (int)nlet,
                       unit);
    }
    const char *cur = text;
    int unit_exp = scan_time_term(&cur);
    skip_blanks(&cur);
    tb->time_scale = 1;
    if (unit_exp == TIME_BAD || *cur != '/') {
        return;
    }
    cur++;
    int prec_exp = scan_time_term(&cur);
    if (prec_exp == TIME_BAD || prec_exp > unit_exp || unit_exp - prec_exp > TIME_MAX_EXP) {
        return;
    }
    for (int i = prec_exp; i < unit_exp; i++) {
        tb->time_scale *= BASE_DEC;
    }
}

/* The directives that have no effect on a testbench run: the preprocessor passes them through
 * and the rest of their line is dropped. */
static bool ignorable_directive(const char *scan) {
    static const char *const NAMES[] = {"`default_nettype",
                                        "`celldefine",
                                        "`endcelldefine",
                                        "`resetall",
                                        "`begin_keywords",
                                        "`end_keywords",
                                        "`unconnected_drive",
                                        "`nounconnected_drive",
                                        "`pragma",
                                        NULL};
    for (size_t i = 0; NAMES[i] != NULL; i++) {
        size_t len = strlen(NAMES[i]);
        if (strncmp(scan, NAMES[i], len) == 0 && !isalnum((unsigned char)scan[len]) &&
            scan[len] != '_') {
            return true;
        }
    }
    return false;
}

/* One step over blanks, a comment or a `timescale line: 1 skipped, 0 at a token, -1 error. */
static int skip_step(const char **cursor, uint32_t *line, const char *file, tb_module *tb) {
    const char *scan = *cursor;
    if (isspace((unsigned char)*scan)) {
        if (*scan == '\n') {
            (*line)++;
        }
        (*cursor)++;
    } else if ((scan[0] == '/' && scan[1] == '/') || (*scan == '`' && ignorable_directive(scan))) {
        skip_to_eol(cursor);
    } else if (*scan == '`' && strncmp(scan, "`timescale", TIMESCALE_LEN) == 0) {
        take_timescale(scan + TIMESCALE_LEN, tb);
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

/* Skips blanks and comments; counts lines; records `timescale lines. */
static bool skip_trivia(const char **cursor, uint32_t *line, const char *file, tb_module *tb) {
    int step = 1;
    while (step > 0) {
        step = skip_step(cursor, line, file, tb);
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
        return lex_number(cursor, tok, src->file, tb);
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
    (void)strcpy(tb->timescale, "1 ns");
    tb->time_scale = 1;
    uint32_t cap = LEX_TOKS0;
    out->toks = tb_alloc(tb, (size_t)cap * sizeof *out->toks);
    out->n = 0;
    while (out->toks != NULL) {
        if (!skip_trivia(&cursor, &line, src->file, tb)) {
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
