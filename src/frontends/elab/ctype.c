/*
 * ctype.c — the expression typer (ceval.h; spec §4.2, EL-13), Yosys 0.55's
 * detectSignWidthWorker/genRTLIL rules (frontends/ast/genrtlil.cc) in two sweeps with explicit
 * stacks:
 *  1. self (post-order): each node's self-determined (width, sign) from its operands', with the
 *     leaves typed here (NUMBER, STRING) or by the host (names, calls, memory words);
 *  2. context (pre-order): the root's final type from the context; a context-determined operand
 *     inherits its parent's final type, comparison operands get their common type, every other
 *     operand is a self-determined root (final = self).
 * Each operand has a role (ct_role): CTX (context-determined), CMP (a comparison operand), SELF
 * (self-determined) or CONST (a replication count, part-select bound or +:/-: width: typed and
 * finalised first, then evaluated through host->constant).
 *
 * Re-entrancy: a host callback may call odin3_ctype_expr on the same typer. Each sweep works on
 * the part of its stack above the length it found and restores that length, frames are copied
 * out before any callback, and nothing keeps a pointer into a stack across one.
 */
#include "frontends/elab/ceval.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    CT_MSG = 160,         /* a diagnostic's text */
    CT_UNSIZED_BITS = 32, /* an unsized literal is at least this wide (2A §4.5) */
    CT_BYTE_BITS = 8,     /* a string character */
    CT_CLOG2_BITS = 32,   /* $clog2 gives a 32-bit signed integer (Yosys mkconst_int) */
    CT_FLAG_SHIFT = 24,   /* a packed half: width in bits 0..23, flags above */
    CT_WIDTH_MASK = 0xFFFFFF,
    CT_F_SIGNED = 1,
    CT_F_UNSIZED = 2,
    CT_F_CLAIMED = 4,  /* self half only: a SELECT the host typed (a memory word) */
    CT_HALF_SHIFT = 32 /* the final half of a map value */
};

_Static_assert((uint32_t)ODIN3_ELAB_MAX_WIDTH <= (uint32_t)CT_WIDTH_MASK,
               "a packed width holds the width cap");

/* Operand roles. */
enum { CT_CTX, CT_CMP, CT_SELF, CT_CONST };

/* Operator classes: how a node's self type follows from its operands'. */
enum {
    CT_OP_SAME,  /* (L(a), s(a)): unary + - ~, and a of << >> <<< >>> ** */
    CT_OP_MAX,   /* (max(L(a), L(b)), s(a) && s(b)): + - * / % & | ^ ~^ */
    CT_OP_ONE,   /* (1, unsigned), operands self-determined: reductions, ! && || */
    CT_OP_CMP,   /* (1, unsigned), operands sized together: == != === !== < <= > >= */
    CT_OP_SHIFT, /* (L(a), s(a)), b self-determined: << >> <<< >>> ** */
};

static const uint8_t UNARY_CLASS[ODIN3_AST_UNARY_COUNT] = {
    [ODIN3_AST_UNARY_PLUS] = CT_OP_SAME, [ODIN3_AST_UNARY_MINUS] = CT_OP_SAME,
    [ODIN3_AST_UNARY_LNOT] = CT_OP_ONE,  [ODIN3_AST_UNARY_NOT] = CT_OP_SAME,
    [ODIN3_AST_UNARY_AND] = CT_OP_ONE,   [ODIN3_AST_UNARY_NAND] = CT_OP_ONE,
    [ODIN3_AST_UNARY_OR] = CT_OP_ONE,    [ODIN3_AST_UNARY_NOR] = CT_OP_ONE,
    [ODIN3_AST_UNARY_XOR] = CT_OP_ONE,   [ODIN3_AST_UNARY_XNOR] = CT_OP_ONE,
};

static const uint8_t BINARY_CLASS[ODIN3_AST_BINARY_COUNT] = {
    [ODIN3_AST_BINARY_ADD] = CT_OP_MAX,     [ODIN3_AST_BINARY_SUB] = CT_OP_MAX,
    [ODIN3_AST_BINARY_MUL] = CT_OP_MAX,     [ODIN3_AST_BINARY_DIV] = CT_OP_MAX,
    [ODIN3_AST_BINARY_MOD] = CT_OP_MAX,     [ODIN3_AST_BINARY_POW] = CT_OP_SHIFT,
    [ODIN3_AST_BINARY_EQ] = CT_OP_CMP,      [ODIN3_AST_BINARY_NE] = CT_OP_CMP,
    [ODIN3_AST_BINARY_CASE_EQ] = CT_OP_CMP, [ODIN3_AST_BINARY_CASE_NE] = CT_OP_CMP,
    [ODIN3_AST_BINARY_LAND] = CT_OP_ONE,    [ODIN3_AST_BINARY_LOR] = CT_OP_ONE,
    [ODIN3_AST_BINARY_LT] = CT_OP_CMP,      [ODIN3_AST_BINARY_LE] = CT_OP_CMP,
    [ODIN3_AST_BINARY_GT] = CT_OP_CMP,      [ODIN3_AST_BINARY_GE] = CT_OP_CMP,
    [ODIN3_AST_BINARY_AND] = CT_OP_MAX,     [ODIN3_AST_BINARY_OR] = CT_OP_MAX,
    [ODIN3_AST_BINARY_XOR] = CT_OP_MAX,     [ODIN3_AST_BINARY_XNOR] = CT_OP_MAX,
    [ODIN3_AST_BINARY_SHL] = CT_OP_SHIFT,   [ODIN3_AST_BINARY_SHR] = CT_OP_SHIFT,
    [ODIN3_AST_BINARY_ASHL] = CT_OP_SHIFT,  [ODIN3_AST_BINARY_ASHR] = CT_OP_SHIFT,
};

/* The system functions typed here; any other call is the host's. */
enum { CT_CALL_HOST, CT_CALL_SIGNED, CT_CALL_UNSIGNED, CT_CALL_CLOG2 };

struct odin3_ctype {
    odin3_ast *ast;
    odin3_ctype_host host;
    odin3_u64map *types;  /* node → self (low half) | final (high half) */
    odin3_vec self_stack; /* ct_frame */
    odin3_vec ctx_stack;  /* ct_ctx_frame */
    const char *what;     /* the role of the constant being asked for */
};

/* A self-sweep frame: the node, and whether its operands were pushed. */
typedef struct ct_frame {
    uint32_t node;
    uint32_t expanded;
} ct_frame;

/* A context-sweep frame: the node and the type it inherits (inherit false: a self root). */
typedef struct ct_ctx_frame {
    uint32_t node;
    uint32_t width;
    bool is_signed;
    bool inherit;
} ct_ctx_frame;

/* --- diagnostics --------------------------------------------------------------------------- */

static odin3_status misuse(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "odin3_ctype: %s", why);
    return ODIN3_ERR_INVALID_ARG;
}

static void ct_vreport(const odin3_ctype *ty, odin3_log_level level, odin3_ast_id node,
                       const char *fmt, va_list args) ODIN3_PRINTF(4, 0);

static void ct_vreport(const odin3_ctype *ty, odin3_log_level level, odin3_ast_id node,
                       const char *fmt, va_list args) {
    char msg[CT_MSG];
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    ty->host.report(ty->host.user, level, odin3_ast_loc(ty->ast, node), msg);
}

/* Reports a located error at node; returns PARSE. */
static odin3_status ct_error(const odin3_ctype *ty, odin3_ast_id node, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

static odin3_status ct_error(const odin3_ctype *ty, odin3_ast_id node, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ct_vreport(ty, ODIN3_LOG_ERROR, node, fmt, args);
    va_end(args);
    return ODIN3_ERR_PARSE;
}

static void ct_warn(const odin3_ctype *ty, odin3_ast_id node, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

static void ct_warn(const odin3_ctype *ty, odin3_ast_id node, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ct_vreport(ty, ODIN3_LOG_WARN, node, fmt, args);
    va_end(args);
}

/* OK when width is within the cap in force, else the located width error. */
static odin3_status ct_check_width(const odin3_ctype *ty, odin3_ast_id node, uint64_t width) {
    uint32_t cap = odin3_elab_limits_get()->max_width;
    if (width <= cap) {
        return ODIN3_OK;
    }
    return ct_error(ty, node, "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits", width,
                    cap);
}

/* --- the type map -------------------------------------------------------------------------- */

static uint32_t pack(odin3_etype type, uint32_t extra) {
    uint32_t flags = (type.is_signed ? CT_F_SIGNED : 0U) | (type.is_unsized ? CT_F_UNSIZED : 0U);
    return (type.width & CT_WIDTH_MASK) | (flags | extra) << CT_FLAG_SHIFT;
}

static odin3_etype unpack(uint32_t half) {
    uint32_t flags = half >> CT_FLAG_SHIFT;
    return (odin3_etype){half & CT_WIDTH_MASK, (flags & CT_F_SIGNED) != 0,
                         (flags & CT_F_UNSIZED) != 0};
}

static uint64_t ct_entry(const odin3_ctype *ty, odin3_ast_id node) {
    uint64_t value = 0;
    (void)odin3_u64map_get(ty->types, node.v, &value);
    return value;
}

static odin3_etype ct_self_of(const odin3_ctype *ty, odin3_ast_id node) {
    return unpack((uint32_t)ct_entry(ty, node));
}

static bool ct_claimed(const odin3_ctype *ty, odin3_ast_id node) {
    return ((uint32_t)ct_entry(ty, node) >> CT_FLAG_SHIFT & CT_F_CLAIMED) != 0;
}

/* Stores node's self type (checked against the cap) and clears its final type. */
static odin3_status ct_set_self(odin3_ctype *ty, odin3_ast_id node, odin3_etype type,
                                bool claimed) {
    odin3_status st = ct_check_width(ty, node, type.width);
    if (st != ODIN3_OK) {
        return st;
    }
    return odin3_u64map_put(ty->types, (odin3_kv){node.v, pack(type, claimed ? CT_F_CLAIMED : 0U)});
}

static odin3_status ct_set_final(odin3_ctype *ty, odin3_ast_id node, odin3_etype type) {
    uint64_t low = (uint32_t)ct_entry(ty, node);
    return odin3_u64map_put(ty->types,
                            (odin3_kv){node.v, low | (uint64_t)pack(type, 0) << CT_HALF_SHIFT});
}

/* --- node shapes --------------------------------------------------------------------------- */

static uint8_t ct_call_kind(const odin3_ast *ast, odin3_ast_id node) {
    if ((odin3_ast_flags(ast, node) & ODIN3_AST_F_CALL_SYSTEM) == 0 ||
        odin3_ast_nchild(ast, node) != 1 || odin3_ast_child(ast, node, 0).v == 0) {
        return CT_CALL_HOST;
    }
    const char *name = odin3_ast_name_str(ast, node);
    if (strcmp(name, "$signed") == 0) {
        return CT_CALL_SIGNED;
    }
    if (strcmp(name, "$unsigned") == 0) {
        return CT_CALL_UNSIGNED;
    }
    return strcmp(name, "$clog2") == 0 ? CT_CALL_CLOG2 : CT_CALL_HOST;
}

/* Kinds typed at once, without operands: literals, names and the host's calls. */
static bool ct_is_leaf(const odin3_ast *ast, odin3_ast_id node) {
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_NUMBER:
    case ODIN3_AST_STRING:
    case ODIN3_AST_REAL:
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return true;
    case ODIN3_AST_CALL:
        return ct_call_kind(ast, node) == CT_CALL_HOST;
    default:
        return false;
    }
}

/* True for a SELECT whose object, through further SELECTs, is a name (maybe a memory word). */
static bool ct_select_of_name(const odin3_ast *ast, odin3_ast_id node) {
    odin3_ast_id obj = odin3_ast_child(ast, node, 0);
    while (odin3_ast_kind_of(ast, obj) == ODIN3_AST_SELECT) {
        obj = odin3_ast_child(ast, obj, 0);
    }
    odin3_ast_kind kind = odin3_ast_kind_of(ast, obj);
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME;
}

static uint8_t ct_select_role(const odin3_ast *ast, odin3_ast_id node, uint32_t idx) {
    if (idx == 0) {
        return CT_SELF; /* the object: its own bits */
    }
    if (odin3_ast_sub(ast, node) == ODIN3_AST_SELECT_PART) {
        return CT_CONST; /* [m:l] */
    }
    return idx == 1 ? CT_SELF : CT_CONST; /* the index or base; the +:/-: width */
}

/* The role of operand idx of node (§4.2's table). */
static uint8_t ct_role(const odin3_ast *ast, odin3_ast_id node, uint32_t idx) {
    uint32_t sub = odin3_ast_sub(ast, node);
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_UNARY:
        return UNARY_CLASS[sub] == CT_OP_SAME ? CT_CTX : CT_SELF;
    case ODIN3_AST_BINARY: {
        uint8_t cls = BINARY_CLASS[sub];
        if (cls == CT_OP_MAX || (cls == CT_OP_SHIFT && idx == 0)) {
            return CT_CTX;
        }
        return cls == CT_OP_CMP ? CT_CMP : CT_SELF;
    }
    case ODIN3_AST_TERNARY:
        return idx == 0 ? CT_SELF : CT_CTX;
    case ODIN3_AST_REPLICATE:
        return idx == 0 ? CT_CONST : CT_SELF;
    case ODIN3_AST_SELECT:
        return ct_select_role(ast, node, idx);
    default:
        return CT_SELF; /* CONCAT parts; $signed/$unsigned/$clog2's argument */
    }
}

/* --- operand iteration --------------------------------------------------------------------- */

typedef odin3_status (*ct_visit_fn)(const odin3_ctype *ty, odin3_ast_id kid, uint8_t role,
                                    void *arg);

typedef struct ct_visit {
    ct_visit_fn fn;
    void *arg;
} ct_visit;

/* A memory word's indices, innermost select first (source order): all self-determined. */
static odin3_status ct_visit_word(const odin3_ctype *ty, odin3_ast_id node, const ct_visit *visit) {
    uint32_t depth = 0;
    for (odin3_ast_id sel = node; odin3_ast_kind_of(ty->ast, sel) == ODIN3_AST_SELECT;
         sel = odin3_ast_child(ty->ast, sel, 0)) {
        depth++;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t level = depth; level > 0 && st == ODIN3_OK; level--) {
        odin3_ast_id sel = node;
        for (uint32_t k = 1; k < level; k++) {
            sel = odin3_ast_child(ty->ast, sel, 0);
        }
        for (uint32_t idx = 1; idx < odin3_ast_nchild(ty->ast, sel) && st == ODIN3_OK; idx++) {
            odin3_ast_id kid = odin3_ast_child(ty->ast, sel, idx);
            st = kid.v != 0 ? visit->fn(ty, kid, CT_SELF, visit->arg) : ODIN3_OK;
        }
    }
    return st;
}

/* Calls visit for each typed operand of node, in source order, with its role. */
static odin3_status ct_for_operands(const odin3_ctype *ty, odin3_ast_id node,
                                    const ct_visit *visit) {
    if (ct_is_leaf(ty->ast, node)) {
        return ODIN3_OK;
    }
    if (odin3_ast_kind_of(ty->ast, node) == ODIN3_AST_SELECT && ct_claimed(ty, node)) {
        return ct_visit_word(ty, node, visit);
    }
    odin3_status st = ODIN3_OK;
    uint32_t count = odin3_ast_nchild(ty->ast, node);
    for (uint32_t idx = 0; idx < count && st == ODIN3_OK; idx++) {
        odin3_ast_id kid = odin3_ast_child(ty->ast, node, idx);
        if (kid.v != 0) {
            st = visit->fn(ty, kid, ct_role(ty->ast, node, idx), visit->arg);
        }
    }
    return st;
}

/* --- leaves -------------------------------------------------------------------------------- */

/* Yosys's stored width of a literal (2A §4.5, number.c size_number). */
static odin3_status ct_number(odin3_ctype *ty, odin3_ast_id node) {
    const odin3_ast_number *num = odin3_ast_number_of(ty->ast, node);
    if (num == NULL || num->nbits == 0) {
        return misuse("a NUMBER node without a payload");
    }
    bool is_signed = (odin3_ast_flags(ty->ast, node) & ODIN3_AST_F_NUMBER_SIGNED) != 0;
    if (num->width != 0) {
        return ct_set_self(ty, node, (odin3_etype){num->width, is_signed, false}, false);
    }
    uint32_t width = num->nbits > CT_UNSIZED_BITS ? num->nbits : CT_UNSIZED_BITS;
    if (is_signed && num->nbits >= CT_UNSIZED_BITS && num->bits[num->nbits - 1] == ODIN3_BIT_1) {
        width++; /* a 0 sign bit above a signed top 1 */
    }
    return ct_set_self(ty, node, (odin3_etype){width, is_signed, true}, false);
}

/* A name, a memory word or a call: the host's type (a width of 0 is a host bug). */
static odin3_status ct_host_leaf(odin3_ctype *ty, odin3_ast_id node) {
    odin3_etype type = {0, false, false};
    odin3_status st = ty->host.leaf(ty->host.user, node, &type);
    if (st != ODIN3_OK) {
        return st;
    }
    if (type.width == 0) {
        return misuse("host leaf gave a width of 0");
    }
    return ct_set_self(ty, node, type, false);
}

static odin3_status ct_leaf(odin3_ctype *ty, odin3_ast_id node) {
    switch (odin3_ast_kind_of(ty->ast, node)) {
    case ODIN3_AST_NUMBER:
        return ct_number(ty, node);
    case ODIN3_AST_STRING: {
        uint64_t len = strlen(odin3_ast_name_str(ty->ast, node));
        uint64_t width = CT_BYTE_BITS * (len > 0 ? len : 1); /* "" is 8'h00 (L10) */
        odin3_status st = ct_check_width(ty, node, width);
        return st == ODIN3_OK
                   ? ct_set_self(ty, node, (odin3_etype){(uint32_t)width, false, false}, false)
                   : st;
    }
    case ODIN3_AST_REAL:
        return ct_error(ty, node, "real number in an expression is not supported");
    default:
        return ct_host_leaf(ty, node);
    }
}

/* A SELECT of a name: asks the host whether it is a memory word, and stores its type if so. */
static odin3_status ct_claim(odin3_ctype *ty, odin3_ast_id node) {
    odin3_etype type = {0, false, false};
    odin3_status st = ty->host.leaf(ty->host.user, node, &type);
    if (st != ODIN3_OK || type.width == 0) {
        return st;
    }
    return ct_set_self(ty, node, type, true);
}

/* --- the context sweep --------------------------------------------------------------------- */

/* The common type of a comparison's operands: (max(L(a), L(b)), s(a) && s(b)). */
static odin3_etype ct_compare_type(const odin3_ctype *ty, odin3_ast_id node) {
    odin3_etype lhs = ct_self_of(ty, odin3_ast_child(ty->ast, node, 0));
    odin3_etype rhs = ct_self_of(ty, odin3_ast_child(ty->ast, node, 1));
    uint32_t width = lhs.width > rhs.width ? lhs.width : rhs.width;
    return (odin3_etype){width, lhs.is_signed && rhs.is_signed, false};
}

typedef struct ct_ctx_push {
    odin3_vec *stack;  /* the context stack */
    odin3_etype final; /* the parent's */
    odin3_etype cmp;   /* its comparison operands' common type */
} ct_ctx_push;

static odin3_status ct_push_ctx(const odin3_ctype *ty, odin3_ast_id kid, uint8_t role, void *arg) {
    (void)ty;
    const ct_ctx_push *push = arg;
    if (role == CT_CONST) {
        return ODIN3_OK; /* finalised before it was evaluated */
    }
    ct_ctx_frame *fr = odin3_vec_push(push->stack);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_etype from = role == CT_CMP ? push->cmp : push->final;
    *fr = (ct_ctx_frame){kid.v, from.width, from.is_signed, role != CT_SELF};
    return ODIN3_OK;
}

/* Gives node its final type and pushes its operands with what they inherit. */
static odin3_status ct_final_node(odin3_ctype *ty, ct_ctx_frame fr) {
    odin3_ast_id node = {fr.node};
    odin3_etype self = ct_self_of(ty, node);
    odin3_etype final = self;
    if (fr.inherit) {
        final.width = self.width > fr.width ? self.width : fr.width;
        final.is_signed = fr.is_signed;
    }
    odin3_status st = ct_set_final(ty, node, final);
    if (st != ODIN3_OK) {
        return st;
    }
    ct_ctx_push push = {&ty->ctx_stack, final, {0, false, false}};
    if (odin3_ast_kind_of(ty->ast, node) == ODIN3_AST_BINARY &&
        BINARY_CLASS[odin3_ast_sub(ty->ast, node)] == CT_OP_CMP) {
        push.cmp = ct_compare_type(ty, node);
    }
    const ct_visit visit = {ct_push_ctx, &push};
    return ct_for_operands(ty, node, &visit);
}

/* The context sweep of the subtree at start.node, on the stack above its current length. */
static odin3_status ct_final_sweep(odin3_ctype *ty, ct_ctx_frame start) {
    size_t base = ty->ctx_stack.len;
    ct_ctx_frame *first = odin3_vec_push(&ty->ctx_stack);
    if (first == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *first = start;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && ty->ctx_stack.len > base) {
        ct_ctx_frame fr =
            *(const ct_ctx_frame *)odin3_vec_cat(&ty->ctx_stack, ty->ctx_stack.len - 1);
        odin3_vec_pop(&ty->ctx_stack);
        st = ct_final_node(ty, fr);
    }
    while (ty->ctx_stack.len > base) {
        odin3_vec_pop(&ty->ctx_stack);
    }
    return st;
}

/* --- constants ----------------------------------------------------------------------------- */

/* Finalises the self-determined constant at node, then asks the host for its value. */
static odin3_status ct_constant(odin3_ctype *ty, odin3_ast_id node, const char *what,
                                int64_t *out) {
    odin3_status st = ct_final_sweep(ty, (ct_ctx_frame){node.v, 0, false, false});
    if (st != ODIN3_OK) {
        return st;
    }
    const char *saved = ty->what;
    ty->what = what;
    st = ty->host.constant(ty->host.user, node, out);
    ty->what = saved;
    return st;
}

/* |msb - lsb| + 1, saturated (int64_t bounds). */
static uint64_t ct_span(int64_t msb, int64_t lsb) {
    uint64_t diff = msb >= lsb ? (uint64_t)msb - (uint64_t)lsb : (uint64_t)lsb - (uint64_t)msb;
    return diff == UINT64_MAX ? UINT64_MAX : diff + 1;
}

/* An ordinary select's width: 1, |m - l| + 1 or the +:/-: width (bounds read left first). */
static odin3_status ct_select_width(odin3_ctype *ty, odin3_ast_id node, uint64_t *width) {
    uint32_t sub = odin3_ast_sub(ty->ast, node);
    if (sub == ODIN3_AST_SELECT_BIT) {
        *width = 1;
        return ODIN3_OK;
    }
    int64_t msb = 0;
    odin3_status st = ODIN3_OK;
    if (sub == ODIN3_AST_SELECT_PART) {
        st = ct_constant(ty, odin3_ast_child(ty->ast, node, 1), "range bound", &msb);
    }
    int64_t last = 0;
    odin3_ast_id last_node = odin3_ast_child(ty->ast, node, 2);
    st = st == ODIN3_OK ? ct_constant(ty, last_node, "range bound", &last) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    if (sub == ODIN3_AST_SELECT_PART) {
        *width = ct_span(msb, last);
        return ODIN3_OK;
    }
    if (last <= 0) {
        return ct_error(ty, last_node, "part-select width %" PRId64 " is not positive", last);
    }
    *width = (uint64_t)last;
    return ODIN3_OK;
}

/* count × L(body), saturated; the count is checked first. */
static odin3_status ct_replicate_width(odin3_ctype *ty, odin3_ast_id node, uint64_t *width) {
    odin3_ast_id count_node = odin3_ast_child(ty->ast, node, 0);
    int64_t count = 0;
    odin3_status st = ct_constant(ty, count_node, "replication count", &count);
    if (st != ODIN3_OK) {
        return st;
    }
    if (count < 0) {
        return ct_error(ty, count_node, "replication count %" PRId64 " is negative", count);
    }
    uint64_t body = ct_self_of(ty, odin3_ast_child(ty->ast, node, 1)).width;
    *width = body != 0 && (uint64_t)count > UINT64_MAX / body ? UINT64_MAX : (uint64_t)count * body;
    return ODIN3_OK;
}

/* --- the self sweep ------------------------------------------------------------------------ */

/* {…}: the sum of its parts; an unsized literal part warns (E12). */
static odin3_status ct_concat_width(const odin3_ctype *ty, odin3_ast_id node, uint64_t *width) {
    uint64_t sum = 0;
    odin3_ast_span parts = odin3_ast_children(ty->ast, node);
    for (uint32_t k = 0; k < parts.n; k++) {
        odin3_etype part = ct_self_of(ty, parts.ids[k]);
        sum += part.width;
        if (part.is_unsized && odin3_ast_kind_of(ty->ast, parts.ids[k]) == ODIN3_AST_NUMBER) {
            ct_warn(ty, parts.ids[k],
                    "unsized constant in a concatenation is taken as %" PRIu32 " bits", part.width);
        }
    }
    *width = sum;
    return ODIN3_OK;
}

/* A unary, binary or ternary operator's self type from its operands'. */
static odin3_etype ct_operator_type(const odin3_ctype *ty, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(ty->ast, node);
    uint32_t sub = odin3_ast_sub(ty->ast, node);
    uint32_t first = kind == ODIN3_AST_TERNARY ? 1 : 0; /* the condition does not count */
    odin3_etype lhs = ct_self_of(ty, odin3_ast_child(ty->ast, node, first));
    uint8_t cls = kind == ODIN3_AST_UNARY ? UNARY_CLASS[sub] : CT_OP_MAX;
    cls = kind == ODIN3_AST_BINARY ? BINARY_CLASS[sub] : cls;
    if (cls == CT_OP_ONE || cls == CT_OP_CMP) {
        return (odin3_etype){1, false, false};
    }
    if (cls == CT_OP_SAME || cls == CT_OP_SHIFT) {
        return (odin3_etype){lhs.width, lhs.is_signed, false};
    }
    odin3_etype rhs = ct_self_of(ty, odin3_ast_child(ty->ast, node, first + 1));
    uint32_t width = lhs.width > rhs.width ? lhs.width : rhs.width;
    return (odin3_etype){width, lhs.is_signed && rhs.is_signed, false};
}

/* $signed / $unsigned keep the argument's width; $clog2 is a 32-bit signed integer. */
static odin3_etype ct_call_type(const odin3_ctype *ty, odin3_ast_id node) {
    uint8_t call = ct_call_kind(ty->ast, node);
    if (call == CT_CALL_CLOG2) {
        return (odin3_etype){CT_CLOG2_BITS, true, false};
    }
    odin3_etype arg = ct_self_of(ty, odin3_ast_child(ty->ast, node, 0));
    return (odin3_etype){arg.width, call == CT_CALL_SIGNED, false};
}

/* A node's self width from its operands' (wide kinds: saturated 64 bits, checked later). */
static odin3_status ct_compute(odin3_ctype *ty, odin3_ast_id node, odin3_etype *type) {
    uint64_t width = 0;
    odin3_status st = ODIN3_OK;
    switch (odin3_ast_kind_of(ty->ast, node)) {
    case ODIN3_AST_UNARY:
    case ODIN3_AST_BINARY:
    case ODIN3_AST_TERNARY:
        *type = ct_operator_type(ty, node);
        return ODIN3_OK;
    case ODIN3_AST_CALL:
        *type = ct_call_type(ty, node);
        return ODIN3_OK;
    case ODIN3_AST_CONCAT:
        st = ct_concat_width(ty, node, &width);
        break;
    case ODIN3_AST_REPLICATE:
        st = ct_replicate_width(ty, node, &width);
        break;
    case ODIN3_AST_SELECT:
        st = ct_select_width(ty, node, &width);
        break;
    default:
        return misuse("not an expression node the typer handles");
    }
    if (st == ODIN3_OK) {
        st = ct_check_width(ty, node, width);
        *type = (odin3_etype){(uint32_t)width, false, false};
    }
    return st;
}

/* A zero-width operand ({0{…}}) is legal only in a concatenation. */
static odin3_status ct_no_zero_width(const odin3_ctype *ty, odin3_ast_id kid, uint8_t role,
                                     void *arg) {
    (void)role;
    (void)arg;
    if (ct_self_of(ty, kid).width != 0) {
        return ODIN3_OK;
    }
    return ct_error(ty, kid, "zero-width concatenation");
}

/* The post-order step: node's self type, once its operands have theirs. */
static odin3_status ct_post(odin3_ctype *ty, odin3_ast_id node) {
    if (ct_is_leaf(ty->ast, node)) {
        return ODIN3_OK; /* typed when entered */
    }
    if (odin3_ast_kind_of(ty->ast, node) != ODIN3_AST_CONCAT) {
        const ct_visit visit = {ct_no_zero_width, NULL};
        odin3_status st = ct_for_operands(ty, node, &visit);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    if (odin3_ast_kind_of(ty->ast, node) == ODIN3_AST_SELECT && ct_claimed(ty, node)) {
        return ODIN3_OK; /* a memory word: the host's type */
    }
    odin3_etype type = {0, false, false};
    odin3_status st = ct_compute(ty, node, &type);
    return st == ODIN3_OK ? ct_set_self(ty, node, type, false) : st;
}

/* Pushes kid on the self stack (arg). */
static odin3_status ct_push_self(const odin3_ctype *ty, odin3_ast_id kid, uint8_t role, void *arg) {
    (void)ty;
    (void)role;
    ct_frame *fr = odin3_vec_push(arg);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *fr = (ct_frame){kid.v, 0};
    return ODIN3_OK;
}

/* The pre-order step: types a leaf, or pushes the operands so the first is processed first. */
static odin3_status ct_enter(odin3_ctype *ty, odin3_ast_id node) {
    if (ct_is_leaf(ty->ast, node)) {
        return ct_leaf(ty, node);
    }
    odin3_ast_kind kind = odin3_ast_kind_of(ty->ast, node);
    if (kind == ODIN3_AST_MINTYPMAX) {
        return misuse("MINTYPMAX is not an expression the typer handles");
    }
    if (kind == ODIN3_AST_SELECT) {
        (void)odin3_u64map_remove(ty->types, node.v); /* no stale claim from an earlier typing */
        if (ct_select_of_name(ty->ast, node)) {
            odin3_status st = ct_claim(ty, node);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    size_t mark = ty->self_stack.len;
    const ct_visit visit = {ct_push_self, &ty->self_stack};
    odin3_status st = ct_for_operands(ty, node, &visit);
    ct_frame *frames = ty->self_stack.data;
    for (size_t lo = mark, hi = ty->self_stack.len; st == ODIN3_OK && lo + 1 < hi; lo++, hi--) {
        ct_frame swap = frames[lo];
        frames[lo] = frames[hi - 1];
        frames[hi - 1] = swap;
    }
    return st;
}

/* The self sweep of root's subtree, on the stack above its current length. */
static odin3_status ct_self_sweep(odin3_ctype *ty, odin3_ast_id root) {
    size_t base = ty->self_stack.len;
    odin3_status st = ct_push_self(ty, root, CT_SELF, &ty->self_stack);
    while (st == ODIN3_OK && ty->self_stack.len > base) {
        size_t top = ty->self_stack.len - 1;
        ct_frame *fr = odin3_vec_at(&ty->self_stack, top);
        odin3_ast_id node = {fr->node};
        if (fr->expanded == 0) {
            fr->expanded = 1;
            st = ct_enter(ty, node);
        } else {
            odin3_vec_pop(&ty->self_stack);
            st = ct_post(ty, node);
        }
    }
    while (ty->self_stack.len > base) {
        odin3_vec_pop(&ty->self_stack);
    }
    return st;
}

/* --- the API ------------------------------------------------------------------------------- */

odin3_status odin3_ctype_create(odin3_ast *ast, const odin3_ctype_host *host, odin3_ctype **out) {
    if (ast == NULL || host == NULL || out == NULL || host->leaf == NULL ||
        host->constant == NULL || host->report == NULL) {
        return misuse("odin3_ctype_create: NULL argument or callback");
    }
    odin3_ctype *ty = odin3_util_calloc(sizeof *ty);
    if (ty == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ty->types = odin3_u64map_create(0);
    if (ty->types == NULL) {
        odin3_util_free(ty);
        return ODIN3_ERR_NO_MEMORY;
    }
    ty->ast = ast;
    ty->host = *host;
    odin3_vec_init(&ty->self_stack, sizeof(ct_frame));
    odin3_vec_init(&ty->ctx_stack, sizeof(ct_ctx_frame));
    *out = ty;
    return ODIN3_OK;
}

void odin3_ctype_destroy(odin3_ctype *ty) {
    if (ty == NULL) {
        return;
    }
    odin3_vec_free(&ty->self_stack);
    odin3_vec_free(&ty->ctx_stack);
    odin3_u64map_destroy(ty->types);
    odin3_util_free(ty);
}

static bool ct_is_expression(const odin3_ast *ast, odin3_ast_id node) {
    if (node.v == 0 || node.v >= odin3_ast_node_end(ast).v) {
        return false;
    }
    return odin3_ast_class_has(ODIN3_AST_CLS_E,
                               odin3_ast_kind_info_of((uint32_t)odin3_ast_kind_of(ast, node)));
}

odin3_status odin3_ctype_expr(odin3_ctype *ty, odin3_ast_id root, odin3_etype context) {
    if (ty == NULL || !ct_is_expression(ty->ast, root)) {
        return misuse("odin3_ctype_expr: NULL typer or a root that is not an expression");
    }
    odin3_status st = ct_check_width(ty, root, context.width);
    st = st == ODIN3_OK ? ct_self_sweep(ty, root) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_etype self = ct_self_of(ty, root);
    if (self.width == 0) {
        return ct_error(ty, root, "zero-width concatenation");
    }
    ct_ctx_frame start = {root.v, context.width, self.is_signed && context.is_signed, true};
    start.inherit = context.width != 0;
    return ct_final_sweep(ty, start);
}

odin3_etype odin3_ctype_self(const odin3_ctype *ty, odin3_ast_id node) {
    return ty != NULL ? ct_self_of(ty, node) : (odin3_etype){0, false, false};
}

odin3_etype odin3_ctype_final(const odin3_ctype *ty, odin3_ast_id node) {
    if (ty == NULL) {
        return (odin3_etype){0, false, false};
    }
    return unpack((uint32_t)(ct_entry(ty, node) >> CT_HALF_SHIFT));
}

const char *odin3_ctype_constant_what(const odin3_ctype *ty) {
    return ty != NULL ? ty->what : NULL;
}
