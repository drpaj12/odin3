/*
 * expr.c — lexer and iterative shunting-yard parser for the .o3lib expression language.
 *
 * Two explicit stacks (operands and pending operators/brackets) replace recursion. The operator
 * stack holds unary and binary operators, '?' / ':' markers, and the open ( [ { brackets; a
 * replication {n{...}} turns its outer '{' entry into EK_REPL once the count is parsed.
 * Postfix [i] and [m:l] apply to the operand on top of the stack, so they bind tighter than any
 * prefix operator ("~a[2]" is ~(a[2])).
 */
#include "techlib/expr.h"

#include "ir/value.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    MAX_LIT_BITS = 1 << 16,
    DEFAULT_LIT_BITS = 32,
    DIG_X = 16,
    DIG_Z = 17,
    DIG_BAD = -1,
    DEC_BASE = 10,
    SHIFT_BIN = 1,
    SHIFT_OCT = 3,
    SHIFT_HEX = 4,
    MSG_MAX = 256,
    HEX_LETTER = 10,
    U64_BITS = 64,
    CASE_BIT = 0x20,
    ASCII_DEL = 0x7f
};

/* Binding strength, low to high; a ':' entry has PREC_TERNARY so only a closer reduces it. */
enum {
    PREC_TERNARY,
    PREC_LOR,
    PREC_LAND,
    PREC_OR,
    PREC_XOR,
    PREC_AND,
    PREC_EQ,
    PREC_REL,
    PREC_SHIFT,
    PREC_ADD,
    PREC_MUL,
    PREC_POW,
    PREC_UNARY
};

static const uint8_t k_prec[ODIN3_OP_COUNT] = {
    [ODIN3_OP_AND] = PREC_AND,   [ODIN3_OP_OR] = PREC_OR,     [ODIN3_OP_XOR] = PREC_XOR,
    [ODIN3_OP_XNOR] = PREC_XOR,  [ODIN3_OP_ADD] = PREC_ADD,   [ODIN3_OP_SUB] = PREC_ADD,
    [ODIN3_OP_MUL] = PREC_MUL,   [ODIN3_OP_DIV] = PREC_MUL,   [ODIN3_OP_MOD] = PREC_MUL,
    [ODIN3_OP_POW] = PREC_POW,   [ODIN3_OP_SHL] = PREC_SHIFT, [ODIN3_OP_SHR] = PREC_SHIFT,
    [ODIN3_OP_EQ] = PREC_EQ,     [ODIN3_OP_NE] = PREC_EQ,     [ODIN3_OP_LT] = PREC_REL,
    [ODIN3_OP_LE] = PREC_REL,    [ODIN3_OP_GT] = PREC_REL,    [ODIN3_OP_GE] = PREC_REL,
    [ODIN3_OP_LAND] = PREC_LAND, [ODIN3_OP_LOR] = PREC_LOR,
};

typedef enum tok_kind {
    TK_END,
    TK_OPERAND,
    TK_OP,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACK,
    TK_RBRACK,
    TK_LBRACE,
    TK_RBRACE,
    TK_COMMA,
    TK_QUEST,
    TK_COLON
} tok_kind;

typedef struct tok {
    tok_kind kind;
    uint32_t col;
    size_t pos, len;
    const odin3_expr *node; /* TK_OPERAND */
    odin3_expr_op bin, un;  /* TK_OP */
} tok;

typedef enum ent_kind {
    EK_UNARY,
    EK_BINARY,
    EK_QUEST,
    EK_COLON,
    EK_LPAREN,
    EK_LBRACK,
    EK_LBRACE,
    EK_REPL
} ent_kind;

typedef struct ent {
    ent_kind kind;
    odin3_expr_op op;
    uint32_t prec;
    uint32_t col;
    uint32_t base; /* LBRACE: operand count when opened */
    uint32_t aux;  /* LBRACK: colons seen; LBRACE: commas seen */
} ent;

typedef struct pstate {
    const odin3_expr_parser *cfg;
    const char *text;
    size_t len, pos;
    odin3_vec ops;  /* ent */
    odin3_vec vals; /* const odin3_expr * */
    tok cur;
    const odin3_expr *result;
} pstate;

typedef struct punct {
    char ch;
    tok_kind kind;
} punct;

static const punct k_punct[] = {
    {'(', TK_LPAREN}, {')', TK_RPAREN}, {'[', TK_LBRACK}, {']', TK_RBRACK}, {'{', TK_LBRACE},
    {'}', TK_RBRACE}, {',', TK_COMMA},  {'?', TK_QUEST},  {':', TK_COLON},
};

typedef struct optext {
    const char *text;
    odin3_expr_op bin, un;
} optext;

/* Longest spellings first. */
static const optext k_ops[] = {
    {"~^", ODIN3_OP_XNOR, ODIN3_OP_NONE}, {"^~", ODIN3_OP_XNOR, ODIN3_OP_NONE},
    {"**", ODIN3_OP_POW, ODIN3_OP_NONE},  {"<<", ODIN3_OP_SHL, ODIN3_OP_NONE},
    {">>", ODIN3_OP_SHR, ODIN3_OP_NONE},  {"<=", ODIN3_OP_LE, ODIN3_OP_NONE},
    {">=", ODIN3_OP_GE, ODIN3_OP_NONE},   {"==", ODIN3_OP_EQ, ODIN3_OP_NONE},
    {"!=", ODIN3_OP_NE, ODIN3_OP_NONE},   {"&&", ODIN3_OP_LAND, ODIN3_OP_NONE},
    {"||", ODIN3_OP_LOR, ODIN3_OP_NONE},  {"~", ODIN3_OP_NONE, ODIN3_OP_NOT},
    {"!", ODIN3_OP_NONE, ODIN3_OP_LNOT},  {"-", ODIN3_OP_SUB, ODIN3_OP_NEG},
    {"+", ODIN3_OP_ADD, ODIN3_OP_NONE},   {"*", ODIN3_OP_MUL, ODIN3_OP_NONE},
    {"/", ODIN3_OP_DIV, ODIN3_OP_NONE},   {"%", ODIN3_OP_MOD, ODIN3_OP_NONE},
    {"<", ODIN3_OP_LT, ODIN3_OP_NONE},    {">", ODIN3_OP_GT, ODIN3_OP_NONE},
    {"&", ODIN3_OP_AND, ODIN3_OP_NONE},   {"|", ODIN3_OP_OR, ODIN3_OP_NONE},
    {"^", ODIN3_OP_XOR, ODIN3_OP_NONE},
};

/* ---- diagnostics and small helpers ---------------------------------------------------------- */

static odin3_status perr(const pstate *ps, uint32_t col, const char *fmt, ...) ODIN3_PRINTF(3, 4);

static odin3_status perr(const pstate *ps, uint32_t col, const char *fmt, ...) {
    char msg[MSG_MAX];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    const char *file = ps->cfg->file != NULL ? ps->cfg->file : "<expr>";
    odin3_log(ODIN3_LOG_ERROR, "%s:%u:%u: %s", file, ps->cfg->line, col, msg);
    return ODIN3_ERR_PARSE;
}

static bool is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static bool is_ident_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool is_ident_char(char ch) {
    return is_ident_start(ch) || is_digit(ch) || ch == '$';
}

static char peek(const pstate *ps) {
    if (ps->pos >= ps->len) {
        return '\0';
    }
    return ps->text[ps->pos];
}

static uint32_t col_at(size_t pos) {
    return (uint32_t)(pos + 1);
}

/* 0..15 for hex digits, DIG_X / DIG_Z for x X z Z ?, DIG_BAD otherwise. */
static int digit_value(char ch) {
    if (is_digit(ch)) {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + HEX_LETTER;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + HEX_LETTER;
    }
    if (ch == 'x' || ch == 'X') {
        return DIG_X;
    }
    return (ch == 'z' || ch == 'Z' || ch == '?') ? DIG_Z : DIG_BAD;
}

static odin3_expr *new_node(const pstate *ps, odin3_expr_kind kind) {
    odin3_expr *node = odin3_arena_alloc(ps->cfg->arena, sizeof *node);
    if (node != NULL) {
        node->kind = kind;
    }
    return node;
}

/* ---- numbers -------------------------------------------------------------------------------- */

static odin3_status scan_decimal(pstate *ps, uint32_t col, uint64_t *value) {
    uint64_t acc = 0;
    while (is_digit(peek(ps)) || peek(ps) == '_') {
        if (peek(ps) != '_') {
            const uint64_t dig = (uint64_t)(peek(ps) - '0');
            if (acc > (UINT64_MAX - dig) / DEC_BASE) {
                return perr(ps, col, "integer literal too large");
            }
            acc = acc * DEC_BASE + dig;
        }
        ps->pos++;
    }
    *value = acc;
    return ODIN3_OK;
}

/* Position and width of the sized literal being lexed. */
typedef struct lit {
    uint32_t col;
    uint32_t nbits;
    uint32_t shift; /* bits per digit for b/o/h */
} lit;

static uint8_t bit_of_digit(int dig, uint32_t shift) {
    if (dig == DIG_X) {
        return ODIN3_BIT_X;
    }
    if (dig == DIG_Z) {
        return ODIN3_BIT_Z;
    }
    return (uint8_t)((dig >> shift) & 1);
}

/* Digits are validated already; writes LSB first, truncating or extending the leading digit. */
static void fill_pow2(uint8_t *bits, lit spec, odin3_bytes digs) {
    const char *text = digs.ptr;
    size_t pos = 0;
    int lead = 0;
    for (size_t i = digs.len; i-- > 0;) {
        if (text[i] == '_') {
            continue;
        }
        lead = digit_value(text[i]);
        for (uint32_t bit = 0; bit < spec.shift && pos + bit < spec.nbits; bit++) {
            bits[pos + bit] = bit_of_digit(lead, bit);
        }
        pos += spec.shift;
    }
    if (lead >= DIG_X) {
        for (; pos < spec.nbits; pos++) {
            bits[pos] = bit_of_digit(lead, 0);
        }
    }
}

static odin3_status make_sized(const pstate *ps, tok *out, lit spec, uint8_t **bits) {
    odin3_expr *node = new_node(ps, ODIN3_EXPR_SIZED);
    uint8_t *buf = odin3_arena_alloc(ps->cfg->arena, spec.nbits);
    if (node == NULL || buf == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    node->col = spec.col;
    node->nbits = spec.nbits;
    node->bits = buf;
    out->node = node;
    *bits = buf;
    return ODIN3_OK;
}

static odin3_status lex_decimal_bits(pstate *ps, tok *out, lit spec) {
    uint64_t value = 0;
    uint8_t *bits = NULL;
    if (!is_digit(peek(ps))) {
        return perr(ps, col_at(ps->pos), "expected decimal digits after base");
    }
    odin3_status st = scan_decimal(ps, spec.col, &value);
    if (st == ODIN3_OK) {
        st = make_sized(ps, out, spec, &bits);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < spec.nbits && i < U64_BITS; i++) {
        bits[i] = (uint8_t)((value >> i) & 1);
    }
    return st;
}

static odin3_status check_digits(const pstate *ps, lit spec, size_t start) {
    const int limit = 1 << spec.shift;
    for (size_t i = start; i < ps->pos; i++) {
        const int dig = digit_value(ps->text[i]);
        if (ps->text[i] != '_' && (dig == DIG_BAD || (dig < DIG_X && dig >= limit))) {
            return perr(ps, col_at(i), "invalid digit '%c' in literal", ps->text[i]);
        }
    }
    return ODIN3_OK;
}

static uint32_t pow2_shift(char base) {
    switch (base) {
    case 'b':
        return SHIFT_BIN;
    case 'o':
        return SHIFT_OCT;
    default:
        return SHIFT_HEX;
    }
}

static odin3_status lex_pow2_bits(pstate *ps, tok *out, lit spec) {
    spec.shift = pow2_shift((char)(ps->text[ps->pos - 1] | CASE_BIT));
    const size_t start = ps->pos;
    uint8_t *bits = NULL;
    while (is_ident_char(peek(ps)) || peek(ps) == '?') {
        ps->pos++;
    }
    if (ps->pos == start) {
        return perr(ps, col_at(start - 1), "expected digits after base");
    }
    odin3_status st = check_digits(ps, spec, start);
    if (st == ODIN3_OK) {
        st = make_sized(ps, out, spec, &bits);
    }
    if (st == ODIN3_OK) {
        const odin3_bytes digs = {ps->text + start, ps->pos - start};
        fill_pow2(bits, spec, digs);
    }
    return st;
}

static odin3_status lex_based(pstate *ps, tok *out, lit spec) {
    const char base = (char)(peek(ps) | CASE_BIT);
    if (base != 'b' && base != 'o' && base != 'h' && base != 'd') {
        return perr(ps, col_at(ps->pos), "invalid base '%c' in literal", peek(ps));
    }
    ps->pos++;
    return base == 'd' ? lex_decimal_bits(ps, out, spec) : lex_pow2_bits(ps, out, spec);
}

static odin3_status lex_plain(const pstate *ps, tok *out, uint64_t value) {
    if (value > (uint64_t)INT64_MAX) {
        return perr(ps, out->col, "integer literal too large");
    }
    odin3_expr *node = new_node(ps, ODIN3_EXPR_INT);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    node->col = out->col;
    node->ival = (int64_t)value;
    out->node = node;
    return ODIN3_OK;
}

static odin3_status lex_number(pstate *ps, tok *out) {
    const uint32_t col = col_at(ps->pos);
    uint64_t size = 0;
    const bool has_size = is_digit(peek(ps));
    const odin3_status st = has_size ? scan_decimal(ps, col, &size) : ODIN3_OK;
    if (st != ODIN3_OK) {
        return st;
    }
    out->kind = TK_OPERAND;
    if (peek(ps) != '\'') {
        return lex_plain(ps, out, size);
    }
    ps->pos++;
    if (has_size && (size == 0 || size > MAX_LIT_BITS)) {
        return perr(ps, col, "literal size must be between 1 and %d", MAX_LIT_BITS);
    }
    const lit spec = {col, has_size ? (uint32_t)size : (uint32_t)DEFAULT_LIT_BITS, 0};
    return lex_based(ps, out, spec);
}

/* ---- other tokens --------------------------------------------------------------------------- */

static odin3_status lex_ident(pstate *ps, tok *out) {
    const size_t start = ps->pos;
    uint32_t id = 0;
    while (is_ident_char(peek(ps))) {
        ps->pos++;
    }
    const odin3_bytes name = {ps->text + start, ps->pos - start};
    odin3_status st = odin3_strtab_intern(ps->cfg->strtab, name, &id);
    odin3_expr *node = st == ODIN3_OK ? new_node(ps, ODIN3_EXPR_IDENT) : NULL;
    if (st == ODIN3_OK && node == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st == ODIN3_OK) {
        node->ident = id;
        node->col = col_at(start);
        out->kind = TK_OPERAND;
        out->node = node;
    }
    return st;
}

static bool lex_op(pstate *ps, tok *out) {
    for (size_t i = 0; i < sizeof k_ops / sizeof k_ops[0]; i++) {
        const size_t len = strlen(k_ops[i].text);
        if (ps->len - ps->pos >= len && memcmp(ps->text + ps->pos, k_ops[i].text, len) == 0) {
            out->kind = TK_OP;
            out->bin = k_ops[i].bin;
            out->un = k_ops[i].un;
            ps->pos += len;
            return true;
        }
    }
    return false;
}

static odin3_status lex_punct(pstate *ps, tok *out) {
    for (size_t i = 0; i < sizeof k_punct / sizeof k_punct[0]; i++) {
        if (k_punct[i].ch == peek(ps)) {
            out->kind = k_punct[i].kind;
            ps->pos++;
            return ODIN3_OK;
        }
    }
    if (lex_op(ps, out)) {
        return ODIN3_OK;
    }
    const unsigned char ch = (unsigned char)peek(ps);
    if (ch >= ' ' && ch < ASCII_DEL) {
        return perr(ps, col_at(ps->pos), "unexpected character '%c'", ch);
    }
    return perr(ps, col_at(ps->pos), "unexpected character 0x%02x", (unsigned)ch);
}

static odin3_status lex_next(pstate *ps) {
    tok *out = &ps->cur;
    while (peek(ps) == ' ' || peek(ps) == '\t' || peek(ps) == '\n' || peek(ps) == '\r') {
        ps->pos++;
    }
    memset(out, 0, sizeof *out);
    out->kind = TK_END;
    out->pos = ps->pos;
    out->col = col_at(ps->pos);
    if (ps->pos >= ps->len) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    if (is_ident_start(peek(ps))) {
        st = lex_ident(ps, out);
    } else if (is_digit(peek(ps)) || peek(ps) == '\'') {
        st = lex_number(ps, out);
    } else {
        st = lex_punct(ps, out);
    }
    out->len = ps->pos - out->pos;
    return st;
}

/* ---- stacks --------------------------------------------------------------------------------- */

static ent *top_ent(pstate *ps) {
    return ps->ops.len > 0 ? odin3_vec_at(&ps->ops, ps->ops.len - 1) : NULL;
}

static odin3_status push_ent(pstate *ps, ent_kind kind) {
    ent *slot = odin3_vec_push(&ps->ops);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->kind = kind;
    slot->col = ps->cur.col;
    slot->base = (uint32_t)ps->vals.len;
    return ODIN3_OK;
}

static odin3_status push_val(pstate *ps, const odin3_expr *node) {
    const odin3_expr **slot = (const odin3_expr **)odin3_vec_push(&ps->vals);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

static const odin3_expr *pop_val(pstate *ps) {
    const odin3_expr *node = *(const odin3_expr *const *)odin3_vec_at(&ps->vals, ps->vals.len - 1);
    odin3_vec_pop(&ps->vals);
    return node;
}

/* ---- reductions ----------------------------------------------------------------------------- */

static odin3_status reduce_top(pstate *ps) {
    const ent top = *top_ent(ps);
    odin3_vec_pop(&ps->ops);
    odin3_expr *node = new_node(ps, ODIN3_EXPR_UNARY);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    node->col = top.col;
    node->op = top.op;
    if (top.kind == EK_COLON) {
        node->kind = ODIN3_EXPR_TERNARY;
        node->c = pop_val(ps);
    }
    if (top.kind != EK_UNARY) {
        if (top.kind == EK_BINARY) {
            node->kind = ODIN3_EXPR_BINARY;
        }
        node->b = pop_val(ps);
    }
    node->a = pop_val(ps);
    return push_val(ps, node);
}

static bool reducible(const ent *top, uint32_t minprec) {
    const bool op = top->kind == EK_UNARY || top->kind == EK_BINARY || top->kind == EK_COLON;
    return op && top->prec >= minprec;
}

/* Reduces pending operators whose precedence is at least minprec, stopping at any bracket. */
static odin3_status reduce(pstate *ps, uint32_t minprec) {
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && ps->ops.len > 0 && reducible(top_ent(ps), minprec)) {
        st = reduce_top(ps);
    }
    return st;
}

static odin3_status close_paren(pstate *ps) {
    odin3_status st = reduce(ps, 0);
    const ent *top = top_ent(ps);
    if (st != ODIN3_OK) {
        return st;
    }
    if (top == NULL || top->kind != EK_LPAREN) {
        return perr(ps, ps->cur.col, "unmatched ')'");
    }
    odin3_vec_pop(&ps->ops);
    return ODIN3_OK;
}

static odin3_status close_bracket(pstate *ps) {
    odin3_status st = reduce(ps, 0);
    const ent *top = top_ent(ps);
    if (st != ODIN3_OK) {
        return st;
    }
    if (top == NULL || top->kind != EK_LBRACK) {
        return perr(ps, ps->cur.col, "unmatched ']'");
    }
    const bool slice = top->aux > 0;
    odin3_vec_pop(&ps->ops);
    odin3_expr *node = new_node(ps, slice ? ODIN3_EXPR_SLICE : ODIN3_EXPR_BITSEL);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slice) {
        node->c = pop_val(ps);
    }
    node->b = pop_val(ps);
    node->a = pop_val(ps);
    node->col = node->a->col;
    return push_val(ps, node);
}

static odin3_status build_concat(pstate *ps, const ent *top) {
    const uint32_t count = (uint32_t)ps->vals.len - top->base;
    odin3_expr *node = new_node(ps, ODIN3_EXPR_CONCAT);
    const odin3_expr **items =
        (const odin3_expr **)odin3_arena_alloc(ps->cfg->arena, count * sizeof(void *));
    if (node == NULL || items == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = count; i-- > 0;) {
        items[i] = pop_val(ps);
    }
    node->col = top->col;
    node->items = items;
    node->nitems = count;
    return push_val(ps, node);
}

static odin3_status close_brace(pstate *ps) {
    odin3_status st = reduce(ps, 0);
    const ent *top = top_ent(ps);
    if (st != ODIN3_OK) {
        return st;
    }
    if (top == NULL || (top->kind != EK_LBRACE && top->kind != EK_REPL)) {
        return perr(ps, ps->cur.col, "unmatched '}'");
    }
    const ent closed = *top;
    odin3_vec_pop(&ps->ops);
    if (closed.kind == EK_LBRACE) {
        return build_concat(ps, &closed);
    }
    odin3_expr *node = new_node(ps, ODIN3_EXPR_REPL);
    if (node == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    node->col = closed.col;
    node->b = pop_val(ps);
    node->a = pop_val(ps);
    return push_val(ps, node);
}

/* ---- token handlers ------------------------------------------------------------------------- */

static odin3_status unexpected(const pstate *ps) {
    const tok *cur = &ps->cur;
    return perr(ps, cur->col, "unexpected '%.*s'", (int)cur->len, ps->text + cur->pos);
}

static odin3_status on_operand_state(pstate *ps, bool *want) {
    const tok *cur = &ps->cur;
    *want = true;
    switch (cur->kind) {
    case TK_OPERAND:
        *want = false;
        return push_val(ps, cur->node);
    case TK_OP:
        if (cur->un != ODIN3_OP_NONE) {
            const odin3_status st = push_ent(ps, EK_UNARY);
            if (st == ODIN3_OK) {
                top_ent(ps)->op = cur->un;
                top_ent(ps)->prec = PREC_UNARY;
            }
            return st;
        }
        break;
    case TK_LPAREN:
        return push_ent(ps, EK_LPAREN);
    case TK_LBRACE:
        return push_ent(ps, EK_LBRACE);
    default:
        break;
    }
    return perr(ps, cur->col, "expected expression, found '%.*s'", (int)cur->len,
                ps->text + cur->pos);
}

static odin3_status on_binary(pstate *ps) {
    const odin3_expr_op op = ps->cur.bin;
    if (op == ODIN3_OP_NONE) {
        return unexpected(ps);
    }
    odin3_status st = reduce(ps, k_prec[op]);
    if (st == ODIN3_OK) {
        st = push_ent(ps, EK_BINARY);
    }
    if (st == ODIN3_OK) {
        top_ent(ps)->op = op;
        top_ent(ps)->prec = k_prec[op];
    }
    return st;
}

static odin3_status on_colon(pstate *ps) {
    odin3_status st = reduce(ps, 0);
    ent *top = top_ent(ps);
    if (st != ODIN3_OK) {
        return st;
    }
    if (top != NULL && top->kind == EK_QUEST) {
        top->kind = EK_COLON;
        top->prec = PREC_TERNARY;
        return ODIN3_OK;
    }
    if (top != NULL && top->kind == EK_LBRACK && top->aux == 0) {
        top->aux = 1;
        return ODIN3_OK;
    }
    return unexpected(ps);
}

static odin3_status on_comma(pstate *ps) {
    odin3_status st = reduce(ps, 0);
    ent *top = top_ent(ps);
    if (st != ODIN3_OK) {
        return st;
    }
    if (top == NULL || top->kind != EK_LBRACE) {
        return unexpected(ps);
    }
    top->aux++;
    return ODIN3_OK;
}

/* "{n" followed by '{' : the outer entry becomes a replication, the inner one a concatenation. */
static odin3_status on_repl_open(pstate *ps) {
    odin3_status st = reduce(ps, 0);
    ent *top = top_ent(ps);
    if (st != ODIN3_OK) {
        return st;
    }
    if (top == NULL || top->kind != EK_LBRACE || top->aux != 0 || top->base + 1 != ps->vals.len) {
        return unexpected(ps);
    }
    top->kind = EK_REPL;
    return push_ent(ps, EK_LBRACE);
}

static odin3_status on_quest(pstate *ps) {
    const odin3_status st = reduce(ps, PREC_LOR);
    return st != ODIN3_OK ? st : push_ent(ps, EK_QUEST);
}

/* True for tokens after which an operand must follow. */
static bool wants_operand(tok_kind kind) {
    switch (kind) {
    case TK_OP:
    case TK_QUEST:
    case TK_COLON:
    case TK_LBRACK:
    case TK_LBRACE:
    case TK_COMMA:
        return true;
    default:
        return false;
    }
}

static odin3_status close_any(pstate *ps) {
    switch (ps->cur.kind) {
    case TK_RPAREN:
        return close_paren(ps);
    case TK_RBRACK:
        return close_bracket(ps);
    default:
        return close_brace(ps);
    }
}

static odin3_status on_operator_state(pstate *ps, bool *want) {
    const tok_kind kind = ps->cur.kind;
    const ent *top = top_ent(ps);
    if (top != NULL && top->kind == EK_REPL && kind != TK_RBRACE) {
        return perr(ps, ps->cur.col, "expected '}' to close the replication");
    }
    *want = wants_operand(kind);
    switch (kind) {
    case TK_OP:
        return on_binary(ps);
    case TK_QUEST:
        return on_quest(ps);
    case TK_COLON:
        return on_colon(ps);
    case TK_LBRACK:
        return push_ent(ps, EK_LBRACK);
    case TK_LBRACE:
        return on_repl_open(ps);
    case TK_COMMA:
        return on_comma(ps);
    case TK_RPAREN:
    case TK_RBRACK:
    case TK_RBRACE:
        return close_any(ps);
    default:
        return unexpected(ps);
    }
}

static const char *unclosed_text(ent_kind kind) {
    switch (kind) {
    case EK_LPAREN:
        return "unclosed '('";
    case EK_LBRACK:
        return "unclosed '['";
    case EK_QUEST:
        return "'?' without ':'";
    default:
        return "unclosed '{'";
    }
}

static odin3_status finish(pstate *ps) {
    const odin3_status st = reduce(ps, 0);
    if (st != ODIN3_OK) {
        return st;
    }
    const ent *top = top_ent(ps);
    if (top != NULL) {
        const char *what = unclosed_text(top->kind);
        return perr(ps, top->col, "%s", what);
    }
    ps->result = pop_val(ps);
    return ODIN3_OK;
}

static odin3_status parse_run(pstate *ps) {
    bool want = true;
    for (;;) {
        odin3_status st = lex_next(ps);
        if (st != ODIN3_OK) {
            return st;
        }
        if (ps->cur.kind == TK_END) {
            return want ? perr(ps, ps->cur.col, "expected expression") : finish(ps);
        }
        st = want ? on_operand_state(ps, &want) : on_operator_state(ps, &want);
        if (st != ODIN3_OK) {
            return st;
        }
    }
}

odin3_status odin3_expr_parse(const odin3_expr_parser *parser, odin3_bytes text,
                              const odin3_expr **out) {
    if (parser == NULL || parser->arena == NULL || parser->strtab == NULL || out == NULL ||
        (text.ptr == NULL && text.len > 0) || text.len >= UINT32_MAX) {
        return ODIN3_ERR_INVALID_ARG;
    }
    pstate ps = {.cfg = parser, .text = text.ptr, .len = text.len};
    odin3_vec_init(&ps.ops, sizeof(ent));
    odin3_vec_init(&ps.vals, sizeof(const odin3_expr *));
    const odin3_status st = parse_run(&ps);
    if (st == ODIN3_OK) {
        *out = ps.result;
    }
    odin3_vec_free(&ps.ops);
    odin3_vec_free(&ps.vals);
    return st;
}
