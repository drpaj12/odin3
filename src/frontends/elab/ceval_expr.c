/*
 * ceval_expr.c — the evaluator's expressions (ceval_internal.h; spec §4.3, §4.4, §4.7; EL-14,
 * EL-15, EL-18): every node is computed at the final type the typer gave it, with Yosys 0.55's
 * RTLIL::const_* semantics (odin3_cval_op); a leaf is brought to its final type by ev_fit (an
 * unsized value repeats its top bit, EL-13).
 *
 * The empty value (width 0) of {0{…}} follows Yosys 0.55 (the Task 8 ruling): it extends to
 * zeros, a concatenation skips it, a reduction of it is its operator's identity (&: 1, |: 0,
 * ^: 0, and their inverses), ! of it is 1, && and || read it as 0, and a shift amount or
 * exponent of it is 0.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/ceval_internal.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_test.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    EX_WORD_BITS = 64,     /* a '**' word product */
    EX_CLOG2_BITS = 32,    /* $clog2 is a 32-bit signed integer (Yosys mkconst_int) */
    EX_BYTE_BITS = 8,      /* a string character */
    EX_SELECT_INDICES = 2, /* a part select's bounds, a +:/-: select's base and width */
    EX_POW_ODD_FREE = 2    /* an odd base's powers repeat with period 2^(width - 2) */
};

/* The operator of a UNARY / BINARY sub-kind. */
static const uint8_t UNARY_COP[ODIN3_AST_UNARY_COUNT] = {
    [ODIN3_AST_UNARY_PLUS] = ODIN3_COP_POS,  [ODIN3_AST_UNARY_MINUS] = ODIN3_COP_NEG,
    [ODIN3_AST_UNARY_LNOT] = ODIN3_COP_LNOT, [ODIN3_AST_UNARY_NOT] = ODIN3_COP_NOT,
    [ODIN3_AST_UNARY_AND] = ODIN3_COP_RAND,  [ODIN3_AST_UNARY_NAND] = ODIN3_COP_RNAND,
    [ODIN3_AST_UNARY_OR] = ODIN3_COP_ROR,    [ODIN3_AST_UNARY_NOR] = ODIN3_COP_RNOR,
    [ODIN3_AST_UNARY_XOR] = ODIN3_COP_RXOR,  [ODIN3_AST_UNARY_XNOR] = ODIN3_COP_RXNOR,
};

static const uint8_t BINARY_COP[ODIN3_AST_BINARY_COUNT] = {
    [ODIN3_AST_BINARY_ADD] = ODIN3_COP_ADD,     [ODIN3_AST_BINARY_SUB] = ODIN3_COP_SUB,
    [ODIN3_AST_BINARY_MUL] = ODIN3_COP_MUL,     [ODIN3_AST_BINARY_DIV] = ODIN3_COP_DIV,
    [ODIN3_AST_BINARY_MOD] = ODIN3_COP_MOD,     [ODIN3_AST_BINARY_POW] = ODIN3_COP_POW,
    [ODIN3_AST_BINARY_EQ] = ODIN3_COP_EQ,       [ODIN3_AST_BINARY_NE] = ODIN3_COP_NE,
    [ODIN3_AST_BINARY_CASE_EQ] = ODIN3_COP_EQX, [ODIN3_AST_BINARY_CASE_NE] = ODIN3_COP_NEX,
    [ODIN3_AST_BINARY_LAND] = ODIN3_COP_LAND,   [ODIN3_AST_BINARY_LOR] = ODIN3_COP_LOR,
    [ODIN3_AST_BINARY_LT] = ODIN3_COP_LT,       [ODIN3_AST_BINARY_LE] = ODIN3_COP_LE,
    [ODIN3_AST_BINARY_GT] = ODIN3_COP_GT,       [ODIN3_AST_BINARY_GE] = ODIN3_COP_GE,
    [ODIN3_AST_BINARY_AND] = ODIN3_COP_AND,     [ODIN3_AST_BINARY_OR] = ODIN3_COP_OR,
    [ODIN3_AST_BINARY_XOR] = ODIN3_COP_XOR,     [ODIN3_AST_BINARY_XNOR] = ODIN3_COP_XNOR,
    [ODIN3_AST_BINARY_SHL] = ODIN3_COP_SHL,     [ODIN3_AST_BINARY_SHR] = ODIN3_COP_SHR,
    [ODIN3_AST_BINARY_ASHL] = ODIN3_COP_SSHL,   [ODIN3_AST_BINARY_ASHR] = ODIN3_COP_SSHR,
};

/* How an operator reads an empty operand. */
enum { EX_CTX, EX_SELF_B, EX_ONE_BIT };

static uint8_t ex_class(odin3_cop op) {
    if (op >= ODIN3_COP_SHL && op <= ODIN3_COP_SSHR) {
        return EX_SELF_B;
    }
    if (op == ODIN3_COP_POW) {
        return EX_SELF_B;
    }
    if ((op >= ODIN3_COP_EQ && op <= ODIN3_COP_LOR) || op >= ODIN3_COP_LNOT) {
        return EX_ONE_BIT; /* comparisons, logical operators, reductions */
    }
    return EX_CTX;
}

/* A reduction or ! of the empty value (Yosys logic_reduce_wrapper's initial states). */
static odin3_bit ex_empty_reduce(odin3_cop op) {
    switch (op) {
    case ODIN3_COP_RAND:
    case ODIN3_COP_RNOR:
    case ODIN3_COP_RXNOR:
    case ODIN3_COP_LNOT:
        return ODIN3_BIT_1;
    default:
        return ODIN3_BIT_0;
    }
}

/* The task's node's final type, from the typer of its frame's level. */
static odin3_etype ex_final(const odin3_ceval *ev, const ev_task *task) {
    return odin3_ctype_final(ev_types(ev, task->frame), (odin3_ast_id){task->node});
}

/* Fits val to node's final type in frame's temporaries and pushes it. */
static odin3_status ex_push_fit(odin3_ceval *ev, const ev_task *task, odin3_cval val) {
    odin3_status st = ev_fit(ev_tmp(ev, task->frame), &val, ex_final(ev, task));
    return st == ODIN3_OK ? ev_push_value(ev, &val) : st;
}

static ev_task ex_task(uint8_t op, odin3_ast_id node, uint32_t frame) {
    return (ev_task){.op = op, .node = node.v, .frame = frame};
}

/* Pushes `then` and node's children (each an EV_EXPR), so the first child runs first. */
static odin3_status ex_push_children(odin3_ceval *ev, const ev_task *task, ev_task then) {
    odin3_status st = ev_push(ev, then);
    odin3_ast_id node = {task->node};
    for (uint32_t k = odin3_ast_nchild(ev->ast, node); k > 0 && st == ODIN3_OK; k--) {
        odin3_ast_id kid = odin3_ast_child(ev->ast, node, k - 1);
        if (kid.v != 0) {
            st = ev_push(ev, ex_task(EV_EXPR, kid, task->frame));
        }
    }
    return st;
}

/* --- leaves -------------------------------------------------------------------------------- */

static odin3_status ex_number(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_literal lit = {{0}, false, false};
    odin3_cval val = {0};
    odin3_arena *tmp = ev_tmp(ev, task->frame);
    odin3_status st = odin3_ast_number_value(ev->ast, (odin3_ast_id){task->node}, tmp, &lit);
    st = st == ODIN3_OK ? odin3_cval_from_literal(tmp, &lit, &val) : st;
    return st == ODIN3_OK ? ex_push_fit(ev, task, val) : st;
}

static odin3_status ex_string(odin3_ceval *ev, const ev_task *task) {
    odin3_cval val = {0};
    const char *text = odin3_ast_name_str(ev->ast, (odin3_ast_id){task->node}); /* "" when EMPTY */
    odin3_status st = odin3_cval_from_string(ev_tmp(ev, task->frame), odin3_bytes_cstr(text), &val);
    return st == ODIN3_OK ? ex_push_fit(ev, task, val) : st;
}

/* A module variable's value from the store of base frame scope (non-constant without one). */
static odin3_status ex_store_read(odin3_ceval *ev, uint32_t scope, odin3_ast_id ref,
                                  const odin3_csym *sym, odin3_cval *out) {
    const odin3_ceval_store *store = ev_frame(ev, scope)->store;
    if (store == NULL || ev_frame(ev, scope)->decl.v != 0) {
        return ev_nonconst_read(ev, scope, ref, sym->name);
    }
    odin3_cval val = {0};
    bool known = false;
    odin3_status st = store->read(store->user, ref, &val, &known);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!known) {
        return ev_nonconst_read(ev, scope, ref, sym->name);
    }
    return odin3_cval_copy(ev_tmp(ev, scope), &val, out);
}

static odin3_status ex_name(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    ev_name nm;
    odin3_status st = ev_resolve(ev, task->frame, node, &nm);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_cval val = {0};
    if (nm.is_local) {
        st = odin3_cval_copy(ev_tmp(ev, task->frame), &ev_local_at(ev, nm.local)->value, &val);
    } else if (nm.sym.kind == ODIN3_CSYM_CONST) {
        val = nm.sym.value;
    } else {
        st = ex_store_read(ev, task->frame, node, &nm.sym, &val);
    }
    return st == ODIN3_OK ? ex_push_fit(ev, task, val) : st;
}

/* --- selects ------------------------------------------------------------------------------- */

/* A select's object as a vector: its declared range and whether it is a named one. */
typedef struct ex_range {
    int64_t msb, lsb;
    bool named;
    uint32_t name; /* strtab ID, for diagnostics */
} ex_range;

static odin3_status ex_object_range(odin3_ceval *ev, uint32_t scope, odin3_ast_id obj,
                                    uint32_t width, ex_range *out) {
    *out = (ex_range){(int64_t)width - 1, 0, false, 0};
    odin3_ast_kind kind = odin3_ast_kind_of(ev->ast, obj);
    odin3_csym sym = {0};
    odin3_status st = ODIN3_OK;
    if (kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME) {
        ev_name nm;
        st = ev_resolve(ev, scope, obj, &nm);
        if (st == ODIN3_OK && nm.is_local) {
            const ev_local *loc = ev_local_at(ev, nm.local);
            *out = (ex_range){loc->msb, loc->lsb, true, loc->name};
            return ODIN3_OK;
        }
        sym = nm.sym;
    } else if (kind == ODIN3_AST_SELECT) {
        st = ev_word_sym(ev, scope, obj, &sym);
    }
    if (st == ODIN3_OK && sym.kind != ODIN3_CSYM_NONE) {
        *out = (ex_range){sym.msb, sym.lsb, true, sym.name};
    }
    return st;
}

/* The select's index values (popped here) and its span in the object. */
static odin3_status ex_select_span(odin3_ceval *ev, uint32_t sub, const ex_range *range,
                                   ev_span *span, bool *known) {
    ev_sel_req req = {sub, 0, 0, range->msb, range->lsb};
    bool known2 = true;
    if (sub != ODIN3_AST_SELECT_BIT) {
        ev_pop_int(ev, &req.second, &known2);
    }
    ev_pop_int(ev, &req.first, known);
    *known = *known && known2;
    *span = ev_select_span(&req);
    return ODIN3_OK;
}

static odin3_status ex_reversed(const odin3_ceval *ev, odin3_ast_id node, const ex_range *range,
                                const ev_span *span) {
    bool down = range->msb > range->lsb;
    bool up = range->msb < range->lsb;
    if ((down && span->left < span->right) || (up && span->left > span->right)) {
        return ev_error(ev, node,
                        "part-select [%" PRId64 ":%" PRId64 "] of '%s' is reversed relative to "
                        "its declaration [%" PRId64 ":%" PRId64 "]",
                        span->left, span->right, ev_str(ev, range->name), range->msb, range->lsb);
    }
    return ODIN3_OK;
}

odin3_status ev_do_sel(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    uint32_t sub = odin3_ast_sub(ev->ast, node);
    odin3_cval obj = *ev_peek_value(ev, sub == ODIN3_AST_SELECT_BIT ? 1 : EX_SELECT_INDICES);
    ex_range range;
    odin3_status st =
        ex_object_range(ev, task->frame, odin3_ast_child(ev->ast, node, 0), obj.width, &range);
    ev_span span = {0, 0, 0, 0};
    bool known = true;
    st = st == ODIN3_OK ? ex_select_span(ev, sub, &range, &span, &known) : st;
    if (st == ODIN3_OK && sub == ODIN3_AST_SELECT_PART && range.named) {
        st = ex_reversed(ev, node, &range, &span);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    (void)ev_pop_value(ev);
    odin3_etype self = odin3_ctype_self(ev_types(ev, task->frame), node);
    odin3_cval val = {0};
    if (!known || obj.width == 0 || span.width != self.width) {
        st = ev_filled(ev_tmp(ev, task->frame), (odin3_etype){self.width, false, false},
                       ODIN3_BIT_X, &val); /* x/z index, or bounds unlike the typer's */
    } else {
        const odin3_cval_part part = {&obj, span.lo, self.width};
        st = odin3_cval_select(ev_tmp(ev, task->frame), &part, &val);
    }
    return st == ODIN3_OK ? ex_push_fit(ev, task, val) : st;
}

/* The index values of a memory word (EV_WORD's operands), innermost select first. */
static odin3_status ex_push_word(odin3_ceval *ev, const ev_task *task, uint32_t depth) {
    odin3_status st =
        ev_push(ev, (ev_task){.op = EV_WORD, .node = task->node, .frame = task->frame, .a = depth});
    odin3_ast_id sel = {task->node}; /* the outermost select: its index is evaluated last */
    for (uint32_t k = 0; k < depth && st == ODIN3_OK; k++) {
        st = ev_push(ev, ex_task(EV_EXPR, odin3_ast_child(ev->ast, sel, 1), task->frame));
        sel = odin3_ast_child(ev->ast, sel, 0);
    }
    return st;
}

odin3_status ev_do_word(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    odin3_csym sym = {0};
    odin3_status st = ev_word_sym(ev, task->frame, node, &sym);
    bool all_known = true;
    while (st == ODIN3_OK && ev->word.len < task->a) {
        int64_t *slot = odin3_vec_push(&ev->word);
        st = slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (slot != NULL) {
            bool known = true;
            ev_pop_int(ev, slot, &known); /* outermost first: filled in reverse below */
            all_known = all_known && known;
        }
    }
    int64_t *idx = ev->word.data;
    for (uint32_t k = 0; st == ODIN3_OK && k < task->a / 2; k++) {
        int64_t swap = idx[k];
        idx[k] = idx[task->a - 1 - k];
        idx[task->a - 1 - k] = swap;
    }
    odin3_cval val = {0};
    if (st == ODIN3_OK && !all_known) {
        st = ev_filled(ev_tmp(ev, task->frame), sym.type, ODIN3_BIT_X, &val);
    } else if (st == ODIN3_OK) {
        st = ex_store_read(ev, task->frame, node, &sym, &val);
    }
    odin3_vec_clear(&ev->word);
    return st == ODIN3_OK ? ex_push_fit(ev, task, val) : st;
}

/* --- operators ----------------------------------------------------------------------------- */

static uint32_t ex_bit_length(const odin3_cval *val) {
    for (uint32_t pos = val->width; pos > 0; pos--) {
        if (odin3_cval_bit(val, pos - 1) == ODIN3_BIT_1) {
            return pos;
        }
    }
    return 0;
}

static uint32_t ex_log2_ceil(uint32_t width) {
    uint32_t bits = 0;
    while (bits < EX_WORD_BITS && ((uint64_t)1 << bits) < width) {
        bits++;
    }
    return bits;
}

/* True when a ** is resolved without multiplying (const_pow): an x/z operand, a negative
 * exponent, a zero base or a zero exponent. */
static bool ex_pow_cheap(const odin3_cval *base, const odin3_cval *exp) {
    if (!odin3_cval_is_known(base) || !odin3_cval_is_known(exp)) {
        return true;
    }
    bool negative =
        (exp->flags & ODIN3_CVAL_SIGNED) != 0 && odin3_cval_bit(exp, exp->width - 1) == ODIN3_BIT_1;
    return negative || ex_bit_length(base) == 0 || ex_bit_length(exp) == 0;
}

/*
 * The ** bound (2D Task 9 ruling): square-and-multiply steps (the exponent's bits; an odd base
 * repeats after width - 2, an even one is 0 after about log2(width)) times ceil(width/64)^2,
 * checked alone before it runs; *work is what it costs (0 for the cheap cases).
 */
static odin3_status ex_pow_bound(const odin3_ceval *ev, odin3_ast_id node,
                                 const odin3_cop_args *args, uint64_t *work) {
    const odin3_cval *base = args->a;
    *work = 0;
    if (ex_pow_cheap(base, args->b)) {
        return ODIN3_OK;
    }
    uint64_t steps = ex_bit_length(args->b);
    bool odd = odin3_cval_bit(base, 0) == ODIN3_BIT_1;
    uint64_t period = (uint64_t)ex_log2_ceil(base->width) + 1; /* even: 0 from here on */
    if (odd) {
        period = base->width > EX_POW_ODD_FREE ? base->width - EX_POW_ODD_FREE : 1;
    }
    steps = steps < period ? steps : period;
    uint64_t words = odin3_cval_words(base->width);
    *work = steps * words * words;
    uint32_t cap = odin3_elab_limits_get()->max_eval_work;
    if (*work <= cap) {
        return ODIN3_OK;
    }
    return ev_error(ev, node,
                    "constant '**' of %" PRIu32 " bits exceeds the limit of %" PRIu32
                    " word operations",
                    base->width, cap);
}

/* The word products * / % ** cost (finding 4: the step budget must see wide operations). */
static odin3_status ex_charge(odin3_ceval *ev, odin3_ast_id node, const odin3_cop_args *args) {
    uint64_t words = odin3_cval_words(args->width);
    uint64_t work = 0;
    odin3_status st = ODIN3_OK;
    switch (args->op) {
    case ODIN3_COP_MUL:
    case ODIN3_COP_DIV:
    case ODIN3_COP_MOD:
        work = words * words;
        break;
    case ODIN3_COP_POW:
        st = ex_pow_bound(ev, node, args, &work);
        break;
    default:
        return ODIN3_OK;
    }
    return st == ODIN3_OK ? ev_work(ev, node, work) : st;
}

/* A 1-bit 0 for an empty operand that is read as a number. */
static odin3_status ex_zero_bit(odin3_arena *arena, odin3_cval *val) {
    if (val->width != 0) {
        return ODIN3_OK;
    }
    return ev_filled(arena, (odin3_etype){1, false, false}, ODIN3_BIT_0, val);
}

/* A UNARY or BINARY operator: operands popped, the result at the final type pushed. */
static odin3_status ex_operator(odin3_ceval *ev, const ev_task *task, odin3_cop op, bool binary) {
    odin3_ast_id node = {task->node};
    odin3_etype final = ex_final(ev, task);
    odin3_arena *tmp = ev_tmp(ev, task->frame);
    odin3_cval rhs = binary ? ev_pop_value(ev) : (odin3_cval){0};
    odin3_cval lhs = ev_pop_value(ev);
    uint8_t cls = ex_class(op);
    odin3_cval out = {0};
    if (final.width == 0) { /* a context operator of empty operands: empty */
        return ev_push_value(ev, &out);
    }
    if (!binary && cls == EX_ONE_BIT && lhs.width == 0) {
        odin3_status st = ev_filled(tmp, (odin3_etype){1, false, false}, ex_empty_reduce(op), &out);
        return st == ODIN3_OK ? ex_push_fit(ev, task, out) : st;
    }
    odin3_status st = cls == EX_CTX ? ODIN3_OK : ex_zero_bit(tmp, &lhs);
    st = st == ODIN3_OK && binary ? ex_zero_bit(tmp, &rhs) : st;
    const odin3_cop_args args = {op, &lhs, binary ? &rhs : NULL, final.width, final.is_signed};
    st = st == ODIN3_OK ? ex_charge(ev, node, &args) : st;
    st = st == ODIN3_OK ? odin3_cval_op(tmp, &args, &out) : st;
    return st == ODIN3_OK ? ev_push_value(ev, &out) : st;
}

/* {…}: the parts (popped, last part on top), empty ones skipped. */
static odin3_status ex_concat(odin3_ceval *ev, const ev_task *task) {
    uint32_t count = odin3_ast_nchild(ev->ast, (odin3_ast_id){task->node});
    odin3_arena *tmp = ev_tmp(ev, task->frame);
    odin3_cval *parts = odin3_arena_alloc(tmp, (size_t)count * sizeof *parts);
    if (parts == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t used = 0;
    bool strings = true;
    for (uint32_t k = count; k > 0; k--) {
        const odin3_cval *part = ev_peek_value(ev, k - 1); /* the first part is the deepest */
        if (part->width != 0) {
            strings = strings && (part->flags & ODIN3_CVAL_STRING) != 0;
            parts[used++] = *part;
        }
    }
    for (uint32_t k = 0; k < count; k++) {
        (void)ev_pop_value(ev);
    }
    odin3_cval out = {0};
    odin3_status st = used > 0 ? odin3_cval_concat(tmp, parts, used, &out) : ODIN3_OK;
    if (st == ODIN3_OK && used > 0 && strings) {
        out.flags |= ODIN3_CVAL_STRING; /* Yosys: a concatenation of strings is a string */
    }
    return st == ODIN3_OK ? ex_push_fit(ev, task, out) : st;
}

static odin3_status ex_replicate(odin3_ceval *ev, const ev_task *task) {
    odin3_cval body = ev_pop_value(ev);
    int64_t count = 0;
    bool known = true;
    ev_pop_int(ev, &count, &known);
    odin3_status st = ODIN3_OK;
    if (!known) {
        st = ev_error(ev, odin3_ast_child(ev->ast, (odin3_ast_id){task->node}, 0),
                      "replication count has x or z bits");
    }
    odin3_cval out = {0};
    uint32_t cap = odin3_elab_limits_get()->max_width;
    if (st == ODIN3_OK && count > 0 && body.width != 0 && (uint64_t)count <= cap) {
        st = odin3_cval_replicate(ev_tmp(ev, task->frame), &body, (uint32_t)count, &out);
        out.flags |= (uint8_t)(body.flags & ODIN3_CVAL_STRING);
    }
    return st == ODIN3_OK ? ex_push_fit(ev, task, out) : st;
}

odin3_status ev_do_op(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    uint32_t sub = odin3_ast_sub(ev->ast, node);
    switch (odin3_ast_kind_of(ev->ast, node)) {
    case ODIN3_AST_UNARY:
        return ex_operator(ev, task, (odin3_cop)UNARY_COP[sub], false);
    case ODIN3_AST_BINARY:
        return ex_operator(ev, task, (odin3_cop)BINARY_COP[sub], true);
    case ODIN3_AST_CONCAT:
        return ex_concat(ev, task);
    default:
        return ex_replicate(ev, task);
    }
}

/* --- the conditional ----------------------------------------------------------------------- */

odin3_status ev_do_tern(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    odin3_cval cond = ev_pop_value(ev);
    odin3_bit truth = cond.width == 0 ? ODIN3_BIT_0 : odin3_cval_truth(&cond);
    odin3_ast_id then_e = odin3_ast_child(ev->ast, node, 1);
    odin3_ast_id else_e = odin3_ast_child(ev->ast, node, 2);
    ev_task merge = ex_task(EV_MERGE, node, task->frame);
    odin3_status st = ODIN3_OK;
    if (truth == ODIN3_BIT_1 || truth == ODIN3_BIT_0) { /* only the branch taken (Yosys) */
        merge.phase = 1;
        st = ev_push(ev, merge);
        odin3_ast_id taken = truth == ODIN3_BIT_1 ? then_e : else_e;
        return st == ODIN3_OK ? ev_push(ev, ex_task(EV_EXPR, taken, task->frame)) : st;
    }
    st = ev_push(ev, merge);
    st = st == ODIN3_OK ? ev_push(ev, ex_task(EV_EXPR, else_e, task->frame)) : st;
    return st == ODIN3_OK ? ev_push(ev, ex_task(EV_EXPR, then_e, task->frame)) : st;
}

/* Phase 1: the branch taken (fitted already) is rebuilt as Yosys's mkconst_bits does: sized,
 * and a string only when unsigned and of whole bytes. Phase 0: both branches merge bit by bit
 * (an x condition), and the result is no string. */
odin3_status ev_do_merge(odin3_ceval *ev, const ev_task *task) {
    odin3_etype final = ex_final(ev, task);
    if (task->phase == 1) {
        odin3_cval val = ev_pop_value(ev);
        val.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED;
        if (final.is_signed || val.width % EX_BYTE_BITS != 0) {
            val.flags &= (uint8_t)~ODIN3_CVAL_STRING;
        }
        return ev_push_value(ev, &val);
    }
    odin3_cval else_v = ev_pop_value(ev);
    odin3_cval then_v = ev_pop_value(ev);
    odin3_cval out = {0};
    if (then_v.width == 0 || then_v.width != else_v.width) {
        return ev_push_value(ev, &out); /* both empty (the final type is 0 bits) */
    }
    odin3_arena *tmp = ev_tmp(ev, task->frame);
    odin3_cval cond = {0};
    odin3_status st = ev_filled(tmp, (odin3_etype){1, false, false}, ODIN3_BIT_X, &cond);
    const odin3_cval_mux_args args = {&cond, &then_v, &else_v};
    st = st == ODIN3_OK ? odin3_cval_mux(tmp, &args, &out) : st;
    out.flags &= (uint8_t) ~(ODIN3_CVAL_STRING | ODIN3_CVAL_UNSIZED);
    return st == ODIN3_OK ? ex_push_fit(ev, task, out) : st;
}

/* --- system functions (§4.7) --------------------------------------------------------------- */

enum { EX_SYS_SIGNED, EX_SYS_UNSIGNED, EX_SYS_CLOG2, EX_SYS_OTHER };

static uint8_t ex_sys_kind(const odin3_ast *ast, odin3_ast_id node) {
    if ((odin3_ast_flags(ast, node) & ODIN3_AST_F_CALL_SYSTEM) == 0 ||
        odin3_ast_nchild(ast, node) != 1 || odin3_ast_child(ast, node, 0).v == 0) {
        return EX_SYS_OTHER;
    }
    const char *name = odin3_ast_name_str(ast, node);
    if (strcmp(name, "$signed") == 0) {
        return EX_SYS_SIGNED;
    }
    if (strcmp(name, "$unsigned") == 0) {
        return EX_SYS_UNSIGNED;
    }
    return strcmp(name, "$clog2") == 0 ? EX_SYS_CLOG2 : EX_SYS_OTHER;
}

/* $clog2 (Yosys 0.55): the bits of e - 1, 0 for e <= 1 and for any x or z bit. */
static int64_t ex_clog2(const odin3_cval *arg) {
    if (arg->width == 0 || !odin3_cval_is_known(arg)) {
        return 0;
    }
    uint32_t top = ex_bit_length(arg);
    bool power_of_two = top > 0;
    for (uint32_t pos = 0; top > 0 && pos + 1 < top; pos++) {
        power_of_two = power_of_two && odin3_cval_bit(arg, pos) == ODIN3_BIT_0;
    }
    return top == 0 ? 0 : (int64_t)top - (power_of_two ? 1 : 0);
}

odin3_status ev_do_sys(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    odin3_cval arg = ev_pop_value(ev);
    uint8_t kind = ex_sys_kind(ev->ast, node);
    if (kind == EX_SYS_CLOG2) {
        ev->clog2--;
        odin3_cval out = {0};
        const odin3_cval_int num = {ex_clog2(&arg), EX_CLOG2_BITS, true};
        odin3_status st = odin3_cval_from_i64(ev_tmp(ev, task->frame), &num, &out);
        return st == ODIN3_OK ? ex_push_fit(ev, task, out) : st;
    }
    /* The bits re-signed; an unsized argument extends to the context by its top bit (Yosys
     * bitsAsConst on the argument), then the result is sized (mkconst_bits). */
    odin3_cval out = arg;
    odin3_arena *tmp = ev_tmp(ev, task->frame);
    odin3_status st = arg.width != 0 ? odin3_cval_copy(tmp, &arg, &out) : ODIN3_OK;
    out.flags = (uint8_t)((arg.flags & ODIN3_CVAL_UNSIZED) |
                          (kind == EX_SYS_SIGNED ? ODIN3_CVAL_SIGNED : 0));
    st = st == ODIN3_OK ? ev_fit(tmp, &out, ex_final(ev, task)) : st;
    out.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED;
    return st == ODIN3_OK ? ev_push_value(ev, &out) : st;
}

/* --- dispatch ------------------------------------------------------------------------------ */

static odin3_status ex_select(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    odin3_csym sym = {0};
    odin3_status st = ev_word_sym(ev, task->frame, node, &sym);
    if (st != ODIN3_OK) {
        return st;
    }
    if (sym.kind != ODIN3_CSYM_NONE) {
        uint32_t depth = 0;
        (void)ev_select_base(ev->ast, node, &depth);
        return ex_push_word(ev, task, depth);
    }
    return ex_push_children(ev, task, ex_task(EV_SEL, node, task->frame));
}

static odin3_status ex_call(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    uint8_t kind = ex_sys_kind(ev->ast, node);
    if (kind == EX_SYS_OTHER) {
        return ev_push(ev, ex_task(EV_CALL, node, task->frame)); /* E20 was the typer's (ceval.c) */
    }
    if (kind == EX_SYS_CLOG2) {
        ev->clog2++; /* its argument's <what> is "$clog2 argument" (plan point (z)) */
    }
    return ex_push_children(ev, task, ex_task(EV_SYS, node, task->frame));
}

odin3_status ev_do_expr(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    odin3_status st = ev_step(ev, node);
    if (st != ODIN3_OK) {
        return st;
    }
    switch (odin3_ast_kind_of(ev->ast, node)) {
    case ODIN3_AST_NUMBER:
        return ex_number(ev, task);
    case ODIN3_AST_STRING:
        return ex_string(ev, task);
    case ODIN3_AST_IDENT:
    case ODIN3_AST_HIER_NAME:
        return ex_name(ev, task);
    case ODIN3_AST_UNARY:
    case ODIN3_AST_BINARY:
    case ODIN3_AST_CONCAT:
    case ODIN3_AST_REPLICATE:
        return ex_push_children(ev, task, ex_task(EV_OP, node, task->frame));
    case ODIN3_AST_TERNARY: {
        st = ev_push(ev, ex_task(EV_TERN, node, task->frame));
        odin3_ast_id cond = odin3_ast_child(ev->ast, node, 0);
        return st == ODIN3_OK ? ev_push(ev, ex_task(EV_EXPR, cond, task->frame)) : st;
    }
    case ODIN3_AST_SELECT:
        return ex_select(ev, task);
    case ODIN3_AST_CALL:
        return ex_call(ev, task);
    default:
        return ev_error(ev, node, "expression is not constant"); /* the typer refuses these */
    }
}

/* --- roots --------------------------------------------------------------------------------- */

odin3_status ev_do_root(odin3_ceval *ev, const ev_task *task) {
    odin3_ast_id node = {task->node};
    odin3_etype context = {task->a, task->sign != 0, false};
    odin3_status st = ev_root_open(ev, task->frame, node, context);
    st = st == ODIN3_OK ? ev_push(ev, ex_task(EV_ROOT_END, node, task->frame)) : st;
    return st == ODIN3_OK ? ev_push(ev, ex_task(EV_EXPR, node, task->frame)) : st;
}

odin3_status ev_do_root_end(odin3_ceval *ev, const ev_task *task) {
    ev_root_close(ev, (odin3_ast_id){task->node});
    return ODIN3_OK;
}
