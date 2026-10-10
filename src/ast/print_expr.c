/*
 * print_expr.c — the printer's expression handlers and the fragments that sit inside items
 * (ranges, delays, strengths, event controls, declarators, connections, attributes, lists),
 * plus the NUMBER and REAL literal text (§11).
 */
#include "ast/print.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    LIMB_BITS = 32,
    CHUNK = 1000000000, /* 10^9: nine decimal digits per division */
    CHUNK_DIGITS = 9,
    DEC_PRINT_MAX_BITS = 13606, /* ≤ ODIN3_AST_MAX_DECIMAL_DIGITS digits: wider prints in hex */
    HEX_BITS = 4,
    DECIMAL_RADIX = 10,
    OCT_BITS = 3,
};

static odin3_ast_id kid(const odin3_printer *pr, const odin3_pr_task *task, uint32_t idx) {
    return odin3_ast_child(pr->ast, (odin3_ast_id){task->node}, idx);
}

static odin3_ast_id self(const odin3_pr_task *task) {
    return (odin3_ast_id){task->node};
}

static uint32_t flags_of(const odin3_printer *pr, const odin3_pr_task *task) {
    return odin3_ast_flags(pr->ast, self(task));
}

/* --- expressions ----------------------------------------------------------------------------- */

static void print_leaf(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_leaf(pr, self(task));
}

static void print_ident(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_name(pr, self(task));
}

static void print_hier(odin3_printer *pr, const odin3_pr_task *task) {
    uint32_t count = odin3_ast_nchild(pr->ast, self(task));
    for (uint32_t i = 0; i < count; i++) {
        odin3_pr_text(pr, i > 0 ? "." : "");
        odin3_pr_node(pr, kid(pr, task, i), ODIN3_PR_EXPR, 0);
    }
}

static void print_select(odin3_printer *pr, const odin3_pr_task *task) {
    static const char *const SEPARATOR[ODIN3_AST_SELECT_COUNT] = {
        [ODIN3_AST_SELECT_BIT] = "",
        [ODIN3_AST_SELECT_PART] = ":",
        [ODIN3_AST_SELECT_PART_PLUS] = "+:",
        [ODIN3_AST_SELECT_PART_MINUS] = "-:"};
    uint32_t sub = odin3_ast_sub(pr->ast, self(task));
    odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_OPERAND, 0);
    odin3_pr_text(pr, "[");
    odin3_pr_node(pr, kid(pr, task, 1), ODIN3_PR_EXPR, 0);
    if (sub != ODIN3_AST_SELECT_BIT && sub < ODIN3_AST_SELECT_COUNT) {
        odin3_pr_text(pr, SEPARATOR[sub]);
        odin3_pr_node(pr, kid(pr, task, 2), ODIN3_PR_EXPR, 0);
    }
    odin3_pr_text(pr, "]");
}

static void print_concat(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_text(pr, "{");
    odin3_pr_list(pr, ODIN3_PR_EXPR, self(task), 0);
    odin3_pr_text(pr, "}");
}

static void print_replicate(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_text(pr, "{");
    odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_node(pr, kid(pr, task, 1), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, "}");
}

/* An operator node: parenthesised as an operand; its attributes follow the operator. */
static void print_operator(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_id node = self(task);
    odin3_ast_kind kind = odin3_ast_kind_of(pr->ast, node);
    bool parens = task->mode == ODIN3_PR_OPERAND;
    odin3_pr_text(pr, parens ? "(" : "");
    if (kind == ODIN3_AST_UNARY) {
        odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, node));
        odin3_pr_attrs(pr, node, false);
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_OPERAND, 0);
    } else {
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_OPERAND, 0);
        odin3_pr_text(pr, " ");
        odin3_pr_text(pr, kind == ODIN3_AST_BINARY ? odin3_ast_sub_name(pr->ast, node) : "?");
        odin3_pr_text(pr, " ");
        odin3_pr_attrs(pr, node, false);
        odin3_pr_node(pr, kid(pr, task, 1), ODIN3_PR_OPERAND, 0);
        if (kind == ODIN3_AST_TERNARY) {
            odin3_pr_text(pr, " : ");
            odin3_pr_node(pr, kid(pr, task, 2), ODIN3_PR_OPERAND, 0);
        }
    }
    odin3_pr_text(pr, parens ? ")" : "");
}

/* A `$`-name or a dotted HIER name as stored, any other name as an identifier. */
static void print_callee(odin3_printer *pr, const odin3_pr_task *task, uint32_t system_or_hier) {
    if ((flags_of(pr, task) & system_or_hier) != 0) {
        odin3_pr_raw(pr, self(task));
    } else {
        odin3_pr_name(pr, self(task));
    }
}

static void print_call(odin3_printer *pr, const odin3_pr_task *task) {
    print_callee(pr, task, ODIN3_AST_F_CALL_SYSTEM | ODIN3_AST_F_CALL_HIER);
    bool system = (flags_of(pr, task) & ODIN3_AST_F_CALL_SYSTEM) != 0;
    if (!system || odin3_ast_nchild(pr->ast, self(task)) > 0) {
        odin3_pr_text(pr, "(");
        odin3_pr_list(pr, ODIN3_PR_EXPR, self(task), 0);
        odin3_pr_text(pr, ")");
    }
}

static void print_mintypmax(odin3_printer *pr, const odin3_pr_task *task) {
    for (uint32_t i = 0; i < 3; i++) {
        odin3_pr_text(pr, i > 0 ? ":" : "");
        odin3_pr_node(pr, kid(pr, task, i), ODIN3_PR_EXPR, 0);
    }
}

/* --- fragments ------------------------------------------------------------------------------- */

static void print_list(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_list(pr, ODIN3_PR_LISTED, self(task), 0);
}

static void print_range(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_text(pr, "[");
    odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, ":");
    odin3_pr_node(pr, kid(pr, task, 1), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, "]");
}

static void print_declarator(odin3_printer *pr, const odin3_pr_task *task) {
    uint32_t count = odin3_ast_nchild(pr->ast, self(task));
    odin3_pr_name(pr, self(task));
    for (uint32_t i = 1; i < count; i++) {
        odin3_pr_node(pr, kid(pr, task, i), ODIN3_PR_LISTED, 0);
    }
    if (kid(pr, task, 0).v != 0) {
        odin3_pr_text(pr, " = ");
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    }
}

static void print_delay(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_text(pr, "#(");
    odin3_pr_list(pr, ODIN3_PR_EXPR, self(task), 0);
    odin3_pr_text(pr, ")");
}

static void print_strength(odin3_printer *pr, const odin3_pr_task *task) {
    static const char *const DRIVE0[] = {"", "highz0", "weak0", "pull0", "strong0", "supply0"};
    static const char *const DRIVE1[] = {"", "highz1", "weak1", "pull1", "strong1", "supply1"};
    static const char *const CHARGE[] = {"", "small", "medium", "large"};
    uint32_t flags = flags_of(pr, task);
    uint32_t s0 = (flags & ODIN3_AST_F_STRENGTH_S0_MASK) >> ODIN3_AST_F_STRENGTH_S0_SHIFT;
    uint32_t s1 = (flags & ODIN3_AST_F_STRENGTH_S1_MASK) >> ODIN3_AST_F_STRENGTH_S1_SHIFT;
    odin3_pr_text(pr, "(");
    if (odin3_ast_sub(pr->ast, self(task)) == ODIN3_AST_STRENGTH_CHARGE) {
        odin3_pr_text(pr, s0 < sizeof CHARGE / sizeof CHARGE[0] ? CHARGE[s0] : "");
    } else {
        bool both = s0 != 0 && s1 != 0;
        odin3_pr_text(pr, s0 < sizeof DRIVE0 / sizeof DRIVE0[0] ? DRIVE0[s0] : "");
        odin3_pr_text(pr, both ? ", " : "");
        odin3_pr_text(pr, s1 < sizeof DRIVE1 / sizeof DRIVE1[0] ? DRIVE1[s1] : "");
    }
    odin3_pr_text(pr, ")");
}

static void print_event_control(odin3_printer *pr, const odin3_pr_task *task) {
    uint32_t flags = flags_of(pr, task);
    bool bare = (flags & ODIN3_AST_F_EVENT_CONTROL_NO_PARENS) != 0;
    if ((flags & ODIN3_AST_F_EVENT_CONTROL_STAR) != 0) {
        odin3_pr_text(pr, bare ? "@*" : "@(*)");
        return;
    }
    if (kid(pr, task, 0).v != 0) {
        odin3_pr_text(pr, "repeat (");
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
        odin3_pr_text(pr, ") ");
    }
    odin3_pr_text(pr, bare ? "@" : "@(");
    uint32_t count = odin3_ast_nchild(pr->ast, self(task));
    for (uint32_t i = 1; i < count; i++) {
        bool after_or =
            (odin3_ast_flags(pr->ast, kid(pr, task, i)) & ODIN3_AST_F_EVENT_EXPR_AFTER_OR) != 0;
        if (i > 1) {
            odin3_pr_text(pr, after_or ? " or " : ", ");
        }
        odin3_pr_node(pr, kid(pr, task, i), ODIN3_PR_LISTED, 0);
    }
    odin3_pr_text(pr, bare ? "" : ")");
}

static void print_event_expr(odin3_printer *pr, const odin3_pr_task *task) {
    if (odin3_ast_sub(pr->ast, self(task)) != ODIN3_AST_EVENT_EXPR_NONE) {
        odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, self(task)));
        odin3_pr_text(pr, " ");
    }
    odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
}

/* `.name(e)` / `.name()` when named or dotted; otherwise the expression (or the bare name). */
static void print_named_value(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_id node = self(task);
    bool dotted = odin3_ast_kind_of(pr->ast, node) == ODIN3_AST_CONNECTION
                      ? odin3_ast_name(pr->ast, node) != 0
                      : (flags_of(pr, task) & ODIN3_AST_F_PORT_REF_DOTTED) != 0;
    if (dotted) {
        odin3_pr_text(pr, ".");
        odin3_pr_name(pr, node);
        odin3_pr_text(pr, "(");
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
        odin3_pr_text(pr, ")");
    } else if (odin3_ast_name(pr->ast, node) != 0) {
        odin3_pr_name(pr, node);
    } else {
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    }
}

static void print_net_assign(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, " = ");
    odin3_pr_node(pr, kid(pr, task, 1), ODIN3_PR_EXPR, 0);
}

/* An attribute inside its `(* … *)` group: `key` or `key = value`. */
static void print_attr(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_pr_name(pr, self(task));
    if (kid(pr, task, 0).v != 0) {
        odin3_pr_text(pr, " = ");
        odin3_pr_node(pr, kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    }
}

odin3_pr_handler odin3_pr_expr_handler(uint32_t kind) {
    static const odin3_pr_handler HANDLERS[ODIN3_AST_KIND_COUNT] = {
        [ODIN3_AST_NUMBER] = print_leaf,
        [ODIN3_AST_REAL] = print_leaf,
        [ODIN3_AST_STRING] = print_leaf,
        [ODIN3_AST_IDENT] = print_ident,
        [ODIN3_AST_HIER_NAME] = print_hier,
        [ODIN3_AST_SELECT] = print_select,
        [ODIN3_AST_CONCAT] = print_concat,
        [ODIN3_AST_REPLICATE] = print_replicate,
        [ODIN3_AST_UNARY] = print_operator,
        [ODIN3_AST_BINARY] = print_operator,
        [ODIN3_AST_TERNARY] = print_operator,
        [ODIN3_AST_CALL] = print_call,
        [ODIN3_AST_MINTYPMAX] = print_mintypmax,
        [ODIN3_AST_LIST] = print_list,
        [ODIN3_AST_RANGE] = print_range,
        [ODIN3_AST_DECLARATOR] = print_declarator,
        [ODIN3_AST_DELAY] = print_delay,
        [ODIN3_AST_STRENGTH] = print_strength,
        [ODIN3_AST_EVENT_CONTROL] = print_event_control,
        [ODIN3_AST_EVENT_EXPR] = print_event_expr,
        [ODIN3_AST_PORT_REF] = print_named_value,
        [ODIN3_AST_CONNECTION] = print_named_value,
        [ODIN3_AST_NET_ASSIGN] = print_net_assign,
        [ODIN3_AST_ATTR] = print_attr,
    };
    return kind < ODIN3_AST_KIND_COUNT ? HANDLERS[kind] : NULL;
}

/* --- literals -------------------------------------------------------------------------------- */

/* A run of a literal's bits: width bits from bit at (bits past nbits read as 0). */
typedef struct bit_group {
    uint32_t at;
    uint32_t width;
} bit_group;

/* The digit the group spells: '0'-'f' (width ≤ 4), 'x', 'z' or '?', or '\0' when it mixes. */
static char group_digit(const odin3_ast_number *num, bit_group group) {
    uint32_t at = group.at;
    uint32_t width = group.width;
    uint32_t value = 0;
    uint32_t xs = 0;
    uint32_t zs = 0;
    for (uint32_t k = 0; k < width; k++) {
        uint8_t bit = at + k < num->nbits ? num->bits[at + k] : (uint8_t)ODIN3_BIT_0;
        xs += bit == ODIN3_BIT_X;
        zs += bit == ODIN3_BIT_Z;
        value |= bit == ODIN3_BIT_1 && k < HEX_BITS ? 1U << k : 0U; /* used only when width ≤ 4 */
    }
    if (xs == width) {
        return 'x';
    }
    if (zs == width) {
        return num->has_question ? '?' : 'z';
    }
    static const char HEX_DIGITS[] = "0123456789abcdef";
    if (xs + zs != 0) {
        return '\0';
    }
    return HEX_DIGITS[value];
}

/* Writes the digits of num in groups of width bits, MSB first; false when a group mixes. */
static bool fill_groups(const odin3_ast_number *num, uint32_t width, char *digits) {
    uint32_t count = (num->nbits + width - 1) / width;
    for (uint32_t i = 0; i < count; i++) {
        char digit = group_digit(num, (bit_group){(count - 1 - i) * width, width});
        if (digit == '\0') {
            return false;
        }
        digits[i] = digit;
    }
    digits[count] = '\0';
    return true;
}

/* Divides the little-endian limbs by CHUNK in place, dropping zero top limbs; the remainder. */
static uint32_t divide_chunk(uint32_t *limbs, uint32_t *nlimbs) {
    uint64_t rem = 0;
    for (uint32_t i = *nlimbs; i-- > 0;) {
        uint64_t cur = (rem << LIMB_BITS) | limbs[i];
        limbs[i] = (uint32_t)(cur / CHUNK);
        rem = cur % CHUNK;
    }
    while (*nlimbs > 0 && limbs[*nlimbs - 1] == 0) {
        (*nlimbs)--;
    }
    return (uint32_t)rem;
}

/* Writes the 0/1 bits [0, sig) of num as decimal digits (MSB first) into digits. */
static void fill_decimal(const odin3_ast_number *num, uint32_t sig, uint32_t *limbs, char *digits) {
    uint32_t nlimbs = (sig + LIMB_BITS - 1) / LIMB_BITS;
    memset(limbs, 0, (size_t)nlimbs * sizeof *limbs);
    for (uint32_t i = 0; i < sig; i++) {
        limbs[i / LIMB_BITS] |= num->bits[i] == ODIN3_BIT_1 ? 1U << (i % LIMB_BITS) : 0U;
    }
    size_t len = 0;
    do {
        uint32_t rem = divide_chunk(limbs, &nlimbs);
        for (uint32_t k = 0; k < CHUNK_DIGITS && (nlimbs > 0 || rem > 0 || k == 0); k++) {
            digits[len++] = (char)('0' + rem % DECIMAL_RADIX);
            rem /= DECIMAL_RADIX;
        }
    } while (nlimbs > 0);
    for (size_t i = 0; i < len / 2; i++) { /* least significant first → reverse */
        char tmp = digits[i];
        digits[i] = digits[len - 1 - i];
        digits[len - 1 - i] = tmp;
    }
    digits[len] = '\0';
}

/* The bits up to the highest 1. */
static uint32_t significant_bits(const odin3_ast_number *num) {
    uint32_t top = num->nbits;
    while (top > 0 && num->bits[top - 1] != ODIN3_BIT_1) {
        top--;
    }
    return top;
}

/* A decimal's digits: `x`/`z`/`?` when every bit is that one, else its value; false if mixed. */
static bool fill_decimal_digits(const odin3_ast_number *num, uint32_t *limbs, char *digits) {
    if (num->has_xz) {
        char digit = group_digit(num, (bit_group){0, num->nbits});
        digits[0] = digit;
        digits[1] = '\0';
        return digit != '\0';
    }
    fill_decimal(num, significant_bits(num), limbs, digits);
    return true;
}

typedef struct number_text {
    const odin3_ast_number *num;
    uint32_t sub;    /* the base the digits are written in */
    bool sgn;        /* `'s` */
    char *digits;    /* scratch: nbits + 2 bytes */
    uint32_t *limbs; /* scratch for a decimal */
} number_text;

/* Fills nt->digits in nt->sub, falling back to binary when a digit mixes x/z with 0/1. */
static void fill_digits(number_text *nt) {
    static const uint32_t GROUP[ODIN3_AST_NUMBER_COUNT] = {[ODIN3_AST_NUMBER_BIN] = 1,
                                                           [ODIN3_AST_NUMBER_OCT] = OCT_BITS,
                                                           [ODIN3_AST_NUMBER_HEX] = HEX_BITS};
    if (nt->sub == ODIN3_AST_NUMBER_DEC && !nt->num->has_xz &&
        significant_bits(nt->num) > DEC_PRINT_MAX_BITS) {
        nt->sub = ODIN3_AST_NUMBER_HEX; /* wider than any decimal literal can be */
    }
    bool ok = nt->sub == ODIN3_AST_NUMBER_DEC ? fill_decimal_digits(nt->num, nt->limbs, nt->digits)
                                              : fill_groups(nt->num, GROUP[nt->sub], nt->digits);
    if (!ok) {
        nt->sub = ODIN3_AST_NUMBER_BIN;
        fill_groups(nt->num, 1, nt->digits);
    }
}

static odin3_status append_literal(odin3_strbuf *out, number_text *nt) {
    static const char BASE_LETTER[ODIN3_AST_NUMBER_COUNT] = {'d', 'b', 'o', 'h'};
    fill_digits(nt);
    bool plain =
        nt->sub == ODIN3_AST_NUMBER_DEC && nt->num->width == 0 && nt->sgn && !nt->num->has_xz;
    if (plain) {
        return odin3_strbuf_append(out, odin3_bytes_cstr(nt->digits));
    }
    if (nt->num->width != 0) {
        return odin3_strbuf_appendf(out, "%u'%s%c%s", (unsigned)nt->num->width, nt->sgn ? "s" : "",
                                    BASE_LETTER[nt->sub], nt->digits);
    }
    return odin3_strbuf_appendf(out, "'%s%c%s", nt->sgn ? "s" : "", BASE_LETTER[nt->sub],
                                nt->digits);
}

odin3_status odin3_pr_append_number(odin3_strbuf *out, const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_number *num = odin3_ast_number_of(ast, node);
    uint32_t sub = odin3_ast_sub(ast, node);
    if (out == NULL || num == NULL || sub >= ODIN3_AST_NUMBER_COUNT || num->nbits == 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    number_text nt = {.num = num,
                      .sub = sub,
                      .sgn = (odin3_ast_flags(ast, node) & ODIN3_AST_F_NUMBER_SIGNED) != 0};
    nt.digits = odin3_util_malloc((size_t)num->nbits + 2);
    nt.limbs = odin3_util_malloc(((size_t)num->nbits / LIMB_BITS + 1) * sizeof *nt.limbs);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    if (nt.digits != NULL && nt.limbs != NULL) {
        st = append_literal(out, &nt);
    }
    odin3_util_free(nt.digits);
    odin3_util_free(nt.limbs);
    return st;
}

odin3_status odin3_pr_append_real(odin3_strbuf *out, double value) {
    size_t start = out != NULL ? out->len : 0;
    odin3_status st = odin3_strbuf_appendf(out, "%.17g", value);
    if (st != ODIN3_OK) {
        return st;
    }
    const char *text = out->data + start;
    if (strpbrk(text, ".eni") != NULL) { /* a fraction, an exponent, inf or nan */
        return ODIN3_OK;
    }
    return odin3_strbuf_append(out, odin3_bytes_cstr(".0"));
}
