/*
 * number.c — NUMBER payloads (§4.5): literal text → bits as written (L4–L8), and the IEEE
 * 1364-2005 §3.5.1 sizing in odin3_ast_number_value.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    RADIX_DEC = 10,
    LETTER_DIGIT = 10, /* the value of digit 'a' */
    UNSIZED_BITS = 32, /* an unsized literal is at least this wide (§3.5.1) */
    LIMB_BITS = 32,
    DEC_LIMBS = 428, /* ≥ ODIN3_AST_MAX_DECIMAL_DIGITS · log2(10) / 32 */
    NO_DIGIT = 0xFF,
    ASCII_CASE_BIT = 0x20, /* 'A' | ASCII_CASE_BIT == 'a' */
    LOG2_10_MILLI = 3322,  /* ⌈1000 · log2(10)⌉ */
    MILLI = 1000,
};

_Static_assert((uint64_t)DEC_LIMBS *LIMB_BITS *MILLI >=
                   (uint64_t)ODIN3_AST_MAX_DECIMAL_DIGITS * LOG2_10_MILLI,
               "the decimal scratch holds the largest decimal literal");

typedef struct base_info {
    uint32_t radix;
    uint32_t bits_per_digit; /* 0 for decimal */
    const char *name;
} base_info;

static const base_info BASES[ODIN3_AST_NUMBER_COUNT] = {
    [ODIN3_AST_NUMBER_DEC] = {RADIX_DEC, 0, "decimal"},
    [ODIN3_AST_NUMBER_BIN] = {2, 1, "binary"},
    [ODIN3_AST_NUMBER_OCT] = {8, 3, "octal"},
    [ODIN3_AST_NUMBER_HEX] = {16, 4, "hex"},
};

/* A literal being converted. */
typedef struct literal {
    uint8_t sub;
    bool is_signed;
    uint32_t width;
    uint32_t ndigits; /* digits, '_' excluded */
    uint32_t nbits;   /* bits to store */
    uint8_t dec_xz;   /* a decimal x/z/? literal: its bit value; NO_DIGIT otherwise */
    bool has_xz;
    bool has_question;
    uint32_t nlimbs; /* a decimal's magnitude, little-endian 32-bit limbs */
    uint32_t limbs[DEC_LIMBS];
} literal;

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

static bool view_ok(odin3_bytes view) {
    return view.ptr != NULL || view.len == 0;
}

/* --- pieces -------------------------------------------------------------------------------- */

static bool base_letter(uint8_t letter, uint8_t *sub) {
    switch (letter | (uint32_t)ASCII_CASE_BIT) { /* ASCII lower case */
    case 'b':
        *sub = ODIN3_AST_NUMBER_BIN;
        return true;
    case 'o':
        *sub = ODIN3_AST_NUMBER_OCT;
        return true;
    case 'd':
        *sub = ODIN3_AST_NUMBER_DEC;
        return true;
    case 'h':
        *sub = ODIN3_AST_NUMBER_HEX;
        return true;
    default:
        return false;
    }
}

/* The base sub-kind and signedness of pieces; a reason string when they are malformed. */
static const char *parse_base(odin3_number_text pieces, uint8_t *sub, bool *is_signed) {
    if (!view_ok(pieces.size) || !view_ok(pieces.base) || !view_ok(pieces.digits)) {
        return "NULL argument";
    }
    const uint8_t *text = pieces.base.ptr;
    size_t len = pieces.base.len;
    if (len == 0) { /* a plain decimal: signed (§3.5.1) */
        *sub = ODIN3_AST_NUMBER_DEC;
        *is_signed = true;
        return pieces.size.len == 0 ? NULL : "a size without a base";
    }
    size_t at = 1;
    *is_signed = len > at && (text[at] == 's' || text[at] == 'S');
    at += *is_signed ? 1 : 0;
    if (text[0] != '\'' || len != at + 1 || !base_letter(text[at], sub)) {
        return "malformed base";
    }
    return NULL;
}

odin3_status odin3_ast_number_kind(odin3_number_text pieces, uint8_t *sub, uint16_t *flags) {
    uint8_t base = 0;
    bool is_signed = false;
    const char *why =
        sub == NULL || flags == NULL ? "NULL argument" : parse_base(pieces, &base, &is_signed);
    if (why != NULL) {
        return misuse("odin3_ast_number_kind", why);
    }
    *sub = base;
    *flags = is_signed ? (uint16_t)ODIN3_AST_F_NUMBER_SIGNED : 0;
    return ODIN3_OK;
}

/* The size piece as a width in 1..cap; PARSE (located) otherwise. Empty = unsized (0). */
static odin3_status parse_size(odin3_ast *ast, odin3_loc loc, odin3_bytes size, uint32_t *width) {
    const uint8_t *text = size.ptr;
    uint64_t value = 0;
    bool digit = false;
    for (size_t i = 0; i < size.len; i++) {
        if (text[i] == '_') {
            continue;
        }
        if (text[i] < '0' || text[i] > '9') {
            odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "literal size is not a decimal number");
            return ODIN3_ERR_PARSE;
        }
        digit = true;
        value = value * RADIX_DEC + (uint64_t)(text[i] - '0');
        value = value > ODIN3_AST_MAX_NUMBER_BITS ? ODIN3_AST_MAX_NUMBER_BITS + 1ULL : value;
    }
    if (size.len > 0 && (!digit || value == 0)) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "literal of zero width");
        return ODIN3_ERR_PARSE;
    }
    if (value > ODIN3_AST_MAX_NUMBER_BITS) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "literal wider than %" PRIu32 " bits",
                   ODIN3_AST_MAX_NUMBER_BITS);
        return ODIN3_ERR_PARSE;
    }
    *width = (uint32_t)value;
    return ODIN3_OK;
}

/* --- digits -------------------------------------------------------------------------------- */

/* The value of digit c in radix, NO_DIGIT when it is not one; x/z/? give their bit value. */
static uint8_t digit_value(uint8_t chr, const base_info *base, bool *question) {
    uint8_t lower = (uint8_t)(chr | (uint32_t)ASCII_CASE_BIT);
    *question = chr == '?';
    if (lower == 'x') {
        return ODIN3_BIT_X;
    }
    if (lower == 'z' || chr == '?') {
        return ODIN3_BIT_Z;
    }
    uint32_t value = NO_DIGIT;
    if (chr >= '0' && chr <= '9') {
        value = (uint32_t)(chr - '0');
    } else if (lower >= 'a' && lower <= 'f') {
        value = (uint32_t)(lower - 'a') + LETTER_DIGIT;
    }
    return value < base->radix ? (uint8_t)value : NO_DIGIT;
}

static bool is_xz_char(uint8_t chr) {
    uint8_t lower = (uint8_t)(chr | (uint32_t)ASCII_CASE_BIT);
    return lower == 'x' || lower == 'z' || chr == '?';
}

/* Multiplies the decimal magnitude by ten and adds digit. */
static void dec_push(literal *lit, uint32_t digit) {
    uint64_t carry = digit;
    for (uint32_t i = 0; i < lit->nlimbs; i++) {
        uint64_t next = (uint64_t)lit->limbs[i] * RADIX_DEC + carry;
        lit->limbs[i] = (uint32_t)next;
        carry = next >> LIMB_BITS;
    }
    if (carry != 0 && lit->nlimbs < DEC_LIMBS) {
        lit->limbs[lit->nlimbs++] = (uint32_t)carry;
    }
}

/* The magnitude's bit length (1 for zero; dec_push never stores a zero top limb). */
static uint32_t dec_bits(const literal *lit) {
    if (lit->nlimbs == 0) {
        return 1;
    }
    uint32_t top = lit->limbs[lit->nlimbs - 1];
    uint32_t bits = (lit->nlimbs - 1) * LIMB_BITS;
    while (top != 0) {
        bits++;
        top >>= 1;
    }
    return bits;
}

/* Scans the digits of a decimal literal: all 0-9, or one x/z/? digit. */
static odin3_status scan_decimal(odin3_ast *ast, odin3_loc loc, odin3_bytes digits, literal *lit) {
    const uint8_t *text = digits.ptr;
    for (size_t i = 0; i < digits.len; i++) {
        if (text[i] == '_') {
            continue;
        }
        bool question = false;
        uint8_t value = digit_value(text[i], &BASES[ODIN3_AST_NUMBER_DEC], &question);
        bool xz = is_xz_char(text[i]);
        if (value == NO_DIGIT || lit->has_xz || (xz && lit->ndigits > 0)) {
            odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "invalid digit '%c' in a decimal literal",
                       text[i]);
            return ODIN3_ERR_PARSE;
        }
        if (++lit->ndigits > ODIN3_AST_MAX_DECIMAL_DIGITS) {
            odin3_diag(ast->design, ODIN3_LOG_ERROR, loc,
                       "decimal literal longer than %" PRIu32 " digits",
                       ODIN3_AST_MAX_DECIMAL_DIGITS);
            return ODIN3_ERR_PARSE;
        }
        if (xz) {
            lit->has_xz = true;
            lit->has_question = question;
            lit->dec_xz = value;
        } else {
            dec_push(lit, value);
        }
    }
    lit->nbits = lit->has_xz ? 1 : dec_bits(lit);
    return ODIN3_OK;
}

/* Scans the digits of a binary, octal or hex literal. */
static odin3_status scan_based(odin3_ast *ast, odin3_loc loc, odin3_bytes digits, literal *lit) {
    const base_info *base = &BASES[lit->sub];
    const uint8_t *text = digits.ptr;
    for (size_t i = 0; i < digits.len; i++) {
        if (text[i] == '_') {
            continue;
        }
        bool question = false;
        uint8_t value = digit_value(text[i], base, &question);
        if (value == NO_DIGIT) {
            odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "invalid digit '%c' in a %s literal",
                       text[i], base->name);
            return ODIN3_ERR_PARSE;
        }
        lit->has_xz = lit->has_xz || is_xz_char(text[i]);
        lit->has_question = lit->has_question || question;
        if ((uint64_t)++lit->ndigits * base->bits_per_digit > ODIN3_AST_MAX_NUMBER_BITS) {
            odin3_diag(ast->design, ODIN3_LOG_ERROR, loc,
                       "literal digits spell more than %" PRIu32 " bits",
                       ODIN3_AST_MAX_NUMBER_BITS);
            return ODIN3_ERR_PARSE;
        }
    }
    lit->nbits = lit->ndigits * base->bits_per_digit;
    return ODIN3_OK;
}

static odin3_status scan_literal(odin3_ast *ast, odin3_loc loc, odin3_number_text pieces,
                                 literal *lit) {
    odin3_status st = parse_size(ast, loc, pieces.size, &lit->width);
    if (st == ODIN3_OK) {
        st = lit->sub == ODIN3_AST_NUMBER_DEC ? scan_decimal(ast, loc, pieces.digits, lit)
                                              : scan_based(ast, loc, pieces.digits, lit);
    }
    if (st == ODIN3_OK && lit->ndigits == 0) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "literal without digits");
        st = ODIN3_ERR_PARSE;
    }
    if (st == ODIN3_OK && lit->sub == ODIN3_AST_NUMBER_DEC && lit->width == 0 &&
        lit->nbits < UNSIZED_BITS) {
        lit->nbits = UNSIZED_BITS; /* §4.5: an unsized decimal stores at least 32 bits */
    }
    return st;
}

/* --- storing ------------------------------------------------------------------------------- */

/* Writes a based literal's bits, LSB first (the last digit first). */
static void fill_based(const literal *lit, odin3_bytes digits, uint8_t *bits) {
    const base_info *base = &BASES[lit->sub];
    const uint8_t *text = digits.ptr;
    uint32_t pos = 0;
    for (size_t i = digits.len; i-- > 0;) {
        bool question = false;
        uint8_t value = text[i] == '_' ? NO_DIGIT : digit_value(text[i], base, &question);
        for (uint32_t k = 0; value != NO_DIGIT && k < base->bits_per_digit; k++) {
            uint32_t bit = is_xz_char(text[i]) ? value : ((uint32_t)value >> k) & 1U;
            bits[pos++] = (uint8_t)bit;
        }
    }
}

static void fill_decimal(const literal *lit, uint8_t *bits) {
    for (uint32_t i = 0; i < lit->nbits; i++) {
        uint32_t limb = i / LIMB_BITS;
        uint32_t bit = limb < lit->nlimbs ? (lit->limbs[limb] >> (i % LIMB_BITS)) & 1U : 0;
        bits[i] = lit->has_xz ? lit->dec_xz : (uint8_t)bit;
    }
}

/* Reserves the payload slot, then the arena bytes (struct + bits); NULL on out of memory. */
static odin3_ast_number *alloc_number(odin3_ast *ast, uint32_t nbits) {
    if (ast->payloads.len >= UINT32_MAX - 1 ||
        odin3_vec_reserve(&ast->payloads, ast->payloads.len + 1) != ODIN3_OK) {
        return NULL;
    }
    return odin3_arena_alloc(ast->arena, sizeof(odin3_ast_number) + nbits);
}

static odin3_status push_number(odin3_ast *ast, odin3_ast_number *num, uint32_t *payload) {
    odin3_ast_payload_rec rec = {.kind = ODIN3_AST_PAYLOAD_NUMBER, .u.number = num};
    return odin3_ast_payload_push(ast, rec, payload);
}

static const char *check_open(const odin3_ast *ast, const void *out) {
    if (ast == NULL || out == NULL) {
        return "NULL argument";
    }
    return ast->finished ? "the store is finished" : NULL;
}

odin3_status odin3_ast_number_parse(odin3_ast *ast, odin3_loc loc, odin3_number_text pieces,
                                    uint32_t *payload) {
    literal lit;
    memset(&lit, 0, sizeof lit);
    lit.dec_xz = NO_DIGIT;
    const char *why = check_open(ast, payload);
    why = why != NULL ? why : parse_base(pieces, &lit.sub, &lit.is_signed);
    if (why != NULL) {
        return misuse("odin3_ast_number_parse", why);
    }
    odin3_status st = scan_literal(ast, loc, pieces, &lit);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_number *num = alloc_number(ast, lit.nbits);
    if (num == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint8_t *bits = (uint8_t *)(num + 1);
    if (lit.sub == ODIN3_AST_NUMBER_DEC) {
        fill_decimal(&lit, bits);
    } else {
        fill_based(&lit, pieces.digits, bits);
    }
    *num = (odin3_ast_number){lit.width, lit.nbits, bits, lit.has_xz, lit.has_question, false};
    return push_number(ast, num, payload);
}

/* Why num is not a consistent payload (NULL when it is); its caps are checked before. */
static const char *check_bits(const odin3_ast_number *num) {
    bool any_xz = false;
    bool any_z = false;
    for (uint32_t i = 0; i < num->nbits; i++) {
        if (num->bits[i] > ODIN3_BIT_Z) {
            return "a bit is not 0 1 x z";
        }
        any_xz = any_xz || num->bits[i] >= ODIN3_BIT_X;
        any_z = any_z || num->bits[i] == ODIN3_BIT_Z;
    }
    if (any_xz != num->has_xz) {
        return "has_xz disagrees with the bits";
    }
    return num->has_question && !any_z ? "has_question without a z bit" : NULL;
}

odin3_status odin3_ast_number_new(odin3_ast *ast, odin3_loc loc, const odin3_ast_number *num,
                                  uint32_t *payload) {
    const char *why = check_open(ast, payload);
    if (why == NULL && (num == NULL || num->bits == NULL || num->nbits == 0)) {
        why = num == NULL ? "NULL argument" : "no bits";
    }
    if (why != NULL) {
        return misuse("odin3_ast_number_new", why);
    }
    if (num->width > ODIN3_AST_MAX_NUMBER_BITS || num->nbits > ODIN3_AST_MAX_NUMBER_BITS) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, loc, "literal wider than %" PRIu32 " bits",
                   ODIN3_AST_MAX_NUMBER_BITS);
        return ODIN3_ERR_PARSE;
    }
    why = check_bits(num);
    if (why != NULL) {
        return misuse("odin3_ast_number_new", why);
    }
    odin3_ast_number *copy = alloc_number(ast, num->nbits);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint8_t *bits = (uint8_t *)(copy + 1);
    memcpy(bits, num->bits, num->nbits);
    *copy = (odin3_ast_number){num->width, num->nbits, bits, num->has_xz, num->has_question, false};
    return push_number(ast, copy, payload);
}

/* --- reading and sizing -------------------------------------------------------------------- */

const odin3_ast_number *odin3_ast_number_of(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_payload_rec *rec = odin3_ast_payload_rec_of(ast, odin3_ast_payload(ast, node));
    return rec != NULL && rec->kind == ODIN3_AST_PAYLOAD_NUMBER ? rec->u.number : NULL;
}

/* How a payload becomes a value: size bits, the first keep copied, the rest ext. */
typedef struct sizing {
    uint32_t size;
    uint32_t keep;
    uint8_t ext;
    bool truncated; /* a dropped bit was not 0 */
} sizing;

static sizing size_number(const odin3_ast_number *num, bool signed_decimal) {
    uint8_t top = num->bits[num->nbits - 1];
    uint8_t ext = top == ODIN3_BIT_X || top == ODIN3_BIT_Z ? top : (uint8_t)ODIN3_BIT_0;
    if (num->width != 0) {
        bool truncated = false;
        for (uint32_t i = num->width; i < num->nbits; i++) {
            truncated = truncated || num->bits[i] != ODIN3_BIT_0;
        }
        uint32_t keep = num->nbits < num->width ? num->nbits : num->width;
        return (sizing){num->width, keep, ext, truncated};
    }
    uint32_t sig = num->nbits;
    while (ext == ODIN3_BIT_0 && sig > 1 && num->bits[sig - 1] == ODIN3_BIT_0) {
        sig--;
    }
    uint32_t need = sig + (signed_decimal && ext == ODIN3_BIT_0 ? 1 : 0); /* a 0 sign bit */
    uint32_t size = need > UNSIZED_BITS ? need : UNSIZED_BITS;
    return (sizing){size, sig, ext, false};
}

odin3_status odin3_ast_number_value(odin3_ast *ast, odin3_ast_id node, odin3_arena *arena,
                                    odin3_value *value, bool *is_signed) {
    if (ast == NULL || arena == NULL || value == NULL || is_signed == NULL) {
        return misuse("odin3_ast_number_value", "NULL argument");
    }
    const odin3_ast_number *num = odin3_ast_number_of(ast, node);
    if (num == NULL) {
        return misuse("odin3_ast_number_value", "not a NUMBER node");
    }
    bool sgn = (odin3_ast_flags(ast, node) & ODIN3_AST_F_NUMBER_SIGNED) != 0;
    sizing how = size_number(num, sgn && odin3_ast_sub(ast, node) == ODIN3_AST_NUMBER_DEC);
    uint8_t *bits = odin3_arena_alloc(arena, how.size);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(bits, num->bits, how.keep);
    memset(bits + how.keep, how.ext, how.size - how.keep);
    if (how.truncated && !num->warned) {
        ((odin3_ast_number *)num)->warned = true; /* the arena record is writable */
        odin3_diag(ast->design, ODIN3_LOG_WARN, odin3_ast_loc(ast, node),
                   "literal truncated to its size of %" PRIu32 " bits", how.size);
    }
    *value = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = bits, .len = how.size};
    *is_signed = sgn;
    return ODIN3_OK;
}
