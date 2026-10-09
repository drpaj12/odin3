/*
 * expr.h — the .o3lib expression language: AST, iterative parser and integer evaluator.
 *
 * Operators (Verilog-2005 precedence, highest first): unary ~ ! -; **; * / %; + -; << >>;
 * < <= > >=; == !=; &; ^ ~^ ^~; |; &&; ||; ?: (right associative, all others left). Primaries:
 * identifiers, plain decimals, sized literals (4'b10x1, 8'hff, 'd3), parenthesised expressions,
 * postfix bit select a[i] and slice a[msb:lsb], concatenation {a, b} and replication {n{a}}.
 * Parsing and evaluation use explicit stacks, so nesting depth is limited by memory only.
 */
#ifndef ODIN3_TECHLIB_EXPR_H
#define ODIN3_TECHLIB_EXPR_H

#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum odin3_expr_kind {
    ODIN3_EXPR_IDENT,   /* ident: strtab ID */
    ODIN3_EXPR_INT,     /* ival: plain decimal literal */
    ODIN3_EXPR_SIZED,   /* bits/nbits: sized literal, one odin3_bit per byte, LSB first */
    ODIN3_EXPR_UNARY,   /* op, a */
    ODIN3_EXPR_BINARY,  /* op, a (left), b (right) */
    ODIN3_EXPR_TERNARY, /* a ? b : c */
    ODIN3_EXPR_SLICE,   /* a[b:c]: base a, msb b, lsb c */
    ODIN3_EXPR_BITSEL,  /* a[b] */
    ODIN3_EXPR_CONCAT,  /* {items[0], ..., items[nitems-1]}, first item is most significant */
    ODIN3_EXPR_REPL     /* {a{b}}: count a, body b (always a CONCAT node) */
} odin3_expr_kind;

typedef enum odin3_expr_op {
    ODIN3_OP_NONE,
    ODIN3_OP_NOT,  /* ~ */
    ODIN3_OP_LNOT, /* ! */
    ODIN3_OP_NEG,  /* unary - */
    ODIN3_OP_AND,
    ODIN3_OP_OR,
    ODIN3_OP_XOR,
    ODIN3_OP_XNOR,
    ODIN3_OP_ADD,
    ODIN3_OP_SUB,
    ODIN3_OP_MUL,
    ODIN3_OP_DIV,
    ODIN3_OP_MOD,
    ODIN3_OP_POW,
    ODIN3_OP_SHL,
    ODIN3_OP_SHR,
    ODIN3_OP_EQ,
    ODIN3_OP_NE,
    ODIN3_OP_LT,
    ODIN3_OP_LE,
    ODIN3_OP_GT,
    ODIN3_OP_GE,
    ODIN3_OP_LAND,
    ODIN3_OP_LOR,
    ODIN3_OP_COUNT
} odin3_expr_op;

typedef struct odin3_expr odin3_expr;
struct odin3_expr {
    odin3_expr_kind kind;
    odin3_expr_op op;
    uint32_t col; /* 1-based column of the node's operator or first token */
    uint32_t ident;
    int64_t ival;
    uint32_t nbits;
    const uint8_t *bits;
    const odin3_expr *a, *b, *c;
    const odin3_expr *const *items;
    uint32_t nitems;
};

/* Caller-filled parse context; nodes live in arena and are freed with it. */
typedef struct odin3_expr_parser {
    odin3_arena *arena;
    odin3_strtab *strtab; /* identifiers are interned here */
    const char *file;     /* for diagnostics; NULL prints "<expr>" */
    uint32_t line;
} odin3_expr_parser;

/*
 * Parses text (the whole span is one expression) into *out. Errors are logged as
 * "file:line:column: message" and return ODIN3_ERR_INVALID_ARG (the status enum has no parse
 * code); out of memory is ODIN3_ERR_NO_MEMORY. *out is untouched on failure.
 */
odin3_status odin3_expr_parse(const odin3_expr_parser *parser, odin3_bytes text,
                              const odin3_expr **out);

/* Resolves an INT parameter; false when ident is unknown. */
typedef bool (*odin3_expr_lookup_fn)(const void *user, uint32_t ident, int64_t *value);

typedef struct odin3_expr_env {
    odin3_expr_lookup_fn lookup;
    const void *user;
    const odin3_strtab *strtab; /* only to name identifiers in messages; may be NULL */
} odin3_expr_env;

/*
 * Evaluates an integer expression (64-bit signed). Logical and comparison results are 0 or 1;
 * sized literals must be free of x/z and below 2^63. Overflow, division by zero, a negative
 * shift count or exponent, an unknown identifier (env may be NULL), concatenation and
 * replication yield ODIN3_ERR_INVALID_ARG with a logged message; ?: && || evaluate lazily.
 * The evaluation stacks live on the C stack up to ODIN3_EXPR_EVAL_INLINE entries (which covers
 * any expression of that many nodes): only deeper evaluations allocate, so only they can return
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_expr_eval_int(const odin3_expr *expr, const odin3_expr_env *env, int64_t *out);

enum { ODIN3_EXPR_EVAL_INLINE = 64, ODIN3_EXPR_ERR_MAX = 160 };

/* An evaluation error: the offending node's column and the message. */
typedef struct odin3_expr_error {
    uint32_t col;
    char text[ODIN3_EXPR_ERR_MAX];
} odin3_expr_error;

/*
 * odin3_expr_eval_int without logging: on ODIN3_ERR_INVALID_ARG the message goes to *err for the
 * caller to report with its own location (*err is untouched otherwise).
 */
odin3_status odin3_expr_eval_int_quiet(const odin3_expr *expr, const odin3_expr_env *env,
                                       int64_t *out, odin3_expr_error *err);

#endif
