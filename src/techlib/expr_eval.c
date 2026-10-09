/*
 * expr_eval.c — iterative integer evaluator for .o3lib expressions (explicit frame stack).
 *
 * A frame is a node plus a stage: stage 0 expands the node (pushing its children), stage 1
 * combines the child values left on the value stack. ?: && || push only the children they need,
 * which makes them lazy.
 */
#include "techlib/expr.h"

#include "ir/value.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdio.h>

enum { MSG_MAX = 256, MAX_KIDS = 3, I64_BITS = 64, MAX_SHIFT = 62, MAX_SLICE_WIDTH = 63 };

typedef struct frame {
    const odin3_expr *node;
    uint32_t stage;
} frame;

typedef struct estate {
    const odin3_expr_env *env;
    odin3_vec frames; /* frame */
    odin3_vec vals;   /* int64_t */
} estate;

typedef struct operands {
    int64_t lhs, rhs;
} operands;

static odin3_status eerr(const odin3_expr *node, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static odin3_status eerr(const odin3_expr *node, const char *fmt, ...) {
    char msg[MSG_MAX];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_log(ODIN3_LOG_ERROR, "expression column %u: %s", node->col, msg);
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status overflow(const odin3_expr *node) {
    return eerr(node, "integer overflow");
}

/* ---- stacks --------------------------------------------------------------------------------- */

static odin3_status push_frame(estate *es, const odin3_expr *node, uint32_t stage) {
    frame *slot = odin3_vec_push(&es->frames);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->node = node;
    slot->stage = stage;
    return ODIN3_OK;
}

static odin3_status push_int(estate *es, int64_t value) {
    int64_t *slot = odin3_vec_push(&es->vals);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

static int64_t pop_int(estate *es) {
    const int64_t value = *(const int64_t *)odin3_vec_at(&es->vals, es->vals.len - 1);
    odin3_vec_pop(&es->vals);
    return value;
}

/* ---- leaves --------------------------------------------------------------------------------- */

static odin3_status eval_ident(estate *es, const odin3_expr *node) {
    const odin3_expr_env *env = es->env;
    int64_t value = 0;
    if (env != NULL && env->lookup != NULL && env->lookup(env->user, node->ident, &value)) {
        return push_int(es, value);
    }
    const char *name =
        env != NULL && env->strtab != NULL ? odin3_strtab_get(env->strtab, node->ident) : NULL;
    if (name != NULL) {
        return eerr(node, "unknown identifier '%s'", name);
    }
    return eerr(node, "unknown identifier (id %u)", node->ident);
}

static odin3_status eval_sized(estate *es, const odin3_expr *node) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < node->nbits; i++) {
        const uint8_t bit = node->bits[i];
        if (bit > ODIN3_BIT_1) {
            return eerr(node, "sized literal contains x or z bits");
        }
        if (bit == ODIN3_BIT_1 && i >= I64_BITS - 1) {
            return overflow(node);
        }
        value |= (uint64_t)bit << (i < I64_BITS - 1 ? i : 0);
    }
    return push_int(es, (int64_t)value);
}

/* ---- operators ------------------------------------------------------------------------------ */

static odin3_status int_pow(const odin3_expr *node, operands ops, int64_t *out) {
    int64_t result = 1;
    int64_t base = ops.lhs;
    int64_t exp = ops.rhs;
    if (exp < 0) {
        return eerr(node, "negative exponent");
    }
    while (exp > 0) {
        if ((exp & 1) != 0 && __builtin_mul_overflow(result, base, &result)) {
            return overflow(node);
        }
        exp >>= 1;
        if (exp > 0 && __builtin_mul_overflow(base, base, &base)) {
            return overflow(node);
        }
    }
    *out = result;
    return ODIN3_OK;
}

static odin3_status int_shift(const odin3_expr *node, operands ops, int64_t *out) {
    const bool left = node->op == ODIN3_OP_SHL;
    if (ops.rhs < 0) {
        return eerr(node, "negative shift count");
    }
    if (left) {
        if (ops.lhs == 0) {
            *out = 0;
            return ODIN3_OK;
        }
        if (ops.rhs > MAX_SHIFT || __builtin_mul_overflow(ops.lhs, (int64_t)1 << ops.rhs, out)) {
            return overflow(node);
        }
        return ODIN3_OK;
    }
    if (ops.rhs > MAX_SHIFT) {
        *out = ops.lhs < 0 ? -1 : 0;
    } else {
        *out = ops.lhs < 0 ? ~(~ops.lhs >> ops.rhs) : ops.lhs >> ops.rhs;
    }
    return ODIN3_OK;
}

static odin3_status int_divide(const odin3_expr *node, operands ops, int64_t *out) {
    if (ops.rhs == 0) {
        return eerr(node, "division by zero");
    }
    if (ops.lhs == INT64_MIN && ops.rhs == -1) {
        return overflow(node);
    }
    *out = node->op == ODIN3_OP_DIV ? ops.lhs / ops.rhs : ops.lhs % ops.rhs;
    return ODIN3_OK;
}

static odin3_status int_arith(const odin3_expr *node, operands ops, int64_t *out) {
    bool ovf = false;
    switch (node->op) {
    case ODIN3_OP_ADD:
        ovf = __builtin_add_overflow(ops.lhs, ops.rhs, out);
        break;
    case ODIN3_OP_SUB:
        ovf = __builtin_sub_overflow(ops.lhs, ops.rhs, out);
        break;
    case ODIN3_OP_MUL:
        ovf = __builtin_mul_overflow(ops.lhs, ops.rhs, out);
        break;
    case ODIN3_OP_DIV:
    case ODIN3_OP_MOD:
        return int_divide(node, ops, out);
    case ODIN3_OP_POW:
        return int_pow(node, ops, out);
    default:
        return int_shift(node, ops, out);
    }
    return ovf ? overflow(node) : ODIN3_OK;
}

static int64_t int_compare(odin3_expr_op op, operands ops) {
    switch (op) {
    case ODIN3_OP_EQ:
        return ops.lhs == ops.rhs;
    case ODIN3_OP_NE:
        return ops.lhs != ops.rhs;
    case ODIN3_OP_LT:
        return ops.lhs < ops.rhs;
    case ODIN3_OP_LE:
        return ops.lhs <= ops.rhs;
    case ODIN3_OP_GT:
        return ops.lhs > ops.rhs;
    default:
        return ops.lhs >= ops.rhs;
    }
}

static int64_t int_bitwise(odin3_expr_op op, operands ops) {
    switch (op) {
    case ODIN3_OP_AND:
        return ops.lhs & ops.rhs;
    case ODIN3_OP_OR:
        return ops.lhs | ops.rhs;
    case ODIN3_OP_XOR:
        return ops.lhs ^ ops.rhs;
    default:
        return ~(ops.lhs ^ ops.rhs);
    }
}

static odin3_status eval_binary(estate *es, const odin3_expr *node) {
    operands ops = {0, 0};
    int64_t value = 0;
    ops.rhs = pop_int(es);
    ops.lhs = pop_int(es);
    switch (node->op) {
    case ODIN3_OP_AND:
    case ODIN3_OP_OR:
    case ODIN3_OP_XOR:
    case ODIN3_OP_XNOR:
        value = int_bitwise(node->op, ops);
        break;
    case ODIN3_OP_EQ:
    case ODIN3_OP_NE:
    case ODIN3_OP_LT:
    case ODIN3_OP_LE:
    case ODIN3_OP_GT:
    case ODIN3_OP_GE:
        value = int_compare(node->op, ops);
        break;
    default: {
        const odin3_status st = int_arith(node, ops, &value);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    }
    return push_int(es, value);
}

static odin3_status eval_unary(estate *es, const odin3_expr *node) {
    const int64_t value = pop_int(es);
    if (node->op == ODIN3_OP_NEG) {
        return value == INT64_MIN ? overflow(node) : push_int(es, -value);
    }
    return push_int(es, node->op == ODIN3_OP_NOT ? ~value : (value == 0));
}

static odin3_status eval_bitsel(estate *es, const odin3_expr *node) {
    operands ops = {0, 0};
    ops.rhs = pop_int(es);
    ops.lhs = pop_int(es);
    if (ops.rhs < 0 || ops.rhs >= I64_BITS) {
        return eerr(node, "bit select index %lld out of range", (long long)ops.rhs);
    }
    return push_int(es, (int64_t)(((uint64_t)ops.lhs >> ops.rhs) & 1U));
}

static odin3_status eval_slice(estate *es, const odin3_expr *node) {
    const int64_t lsb = pop_int(es);
    const int64_t msb = pop_int(es);
    const uint64_t value = (uint64_t)pop_int(es);
    if (lsb < 0 || msb < lsb || msb >= I64_BITS || msb - lsb >= MAX_SLICE_WIDTH) {
        return eerr(node, "slice [%lld:%lld] out of range", (long long)msb, (long long)lsb);
    }
    const uint64_t mask = ((uint64_t)1 << (msb - lsb + 1)) - 1;
    return push_int(es, (int64_t)((value >> lsb) & mask));
}

/* ---- control flow --------------------------------------------------------------------------- */

static uint32_t kids_of(const odin3_expr *node, const odin3_expr *kids[MAX_KIDS]) {
    kids[0] = node->a;
    kids[1] = node->b;
    kids[2] = node->c;
    switch (node->kind) {
    case ODIN3_EXPR_UNARY:
        return 1;
    case ODIN3_EXPR_BINARY:
    case ODIN3_EXPR_BITSEL:
        return 2;
    default:
        return MAX_KIDS;
    }
}

static odin3_status expand(estate *es, const odin3_expr *node) {
    const odin3_expr *kids[MAX_KIDS];
    const uint32_t count = kids_of(node, kids);
    odin3_status st = push_frame(es, node, 1);
    for (uint32_t i = count; st == ODIN3_OK && i-- > 0;) {
        st = push_frame(es, kids[i], 0);
    }
    return st;
}

static odin3_status step_logical(estate *es, frame fr) {
    const bool is_and = fr.node->op == ODIN3_OP_LAND;
    if (fr.stage == 0) {
        const odin3_status st = push_frame(es, fr.node, 1);
        return st != ODIN3_OK ? st : push_frame(es, fr.node->a, 0);
    }
    const int64_t value = pop_int(es);
    if (fr.stage == 2) {
        return push_int(es, value != 0);
    }
    if (is_and ? value == 0 : value != 0) {
        return push_int(es, !is_and);
    }
    const odin3_status st = push_frame(es, fr.node, 2);
    return st != ODIN3_OK ? st : push_frame(es, fr.node->b, 0);
}

static odin3_status step_ternary(estate *es, frame fr) {
    if (fr.stage == 0) {
        const odin3_status st = push_frame(es, fr.node, 1);
        return st != ODIN3_OK ? st : push_frame(es, fr.node->a, 0);
    }
    return push_frame(es, pop_int(es) != 0 ? fr.node->b : fr.node->c, 0);
}

static odin3_status step_combine(estate *es, const odin3_expr *node) {
    switch (node->kind) {
    case ODIN3_EXPR_UNARY:
        return eval_unary(es, node);
    case ODIN3_EXPR_BINARY:
        return eval_binary(es, node);
    case ODIN3_EXPR_BITSEL:
        return eval_bitsel(es, node);
    default:
        return eval_slice(es, node);
    }
}

static odin3_status step(estate *es) {
    const frame fr = *(const frame *)odin3_vec_at(&es->frames, es->frames.len - 1);
    odin3_vec_pop(&es->frames);
    switch (fr.node->kind) {
    case ODIN3_EXPR_INT:
        return push_int(es, fr.node->ival);
    case ODIN3_EXPR_IDENT:
        return eval_ident(es, fr.node);
    case ODIN3_EXPR_SIZED:
        return eval_sized(es, fr.node);
    case ODIN3_EXPR_CONCAT:
        return eerr(fr.node, "concatenation is not an integer expression");
    case ODIN3_EXPR_REPL:
        return eerr(fr.node, "replication is not an integer expression");
    case ODIN3_EXPR_TERNARY:
        return step_ternary(es, fr);
    case ODIN3_EXPR_BINARY:
        if (fr.node->op == ODIN3_OP_LAND || fr.node->op == ODIN3_OP_LOR) {
            return step_logical(es, fr);
        }
        break;
    default:
        break;
    }
    return fr.stage == 0 ? expand(es, fr.node) : step_combine(es, fr.node);
}

odin3_status odin3_expr_eval_int(const odin3_expr *expr, const odin3_expr_env *env, int64_t *out) {
    if (expr == NULL || out == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    estate es = {.env = env};
    odin3_vec_init(&es.frames, sizeof(frame));
    odin3_vec_init(&es.vals, sizeof(int64_t));
    odin3_status st = push_frame(&es, expr, 0);
    while (st == ODIN3_OK && es.frames.len > 0) {
        st = step(&es);
    }
    if (st == ODIN3_OK) {
        *out = pop_int(&es);
    }
    odin3_vec_free(&es.frames);
    odin3_vec_free(&es.vals);
    return st;
}
