/*
 * fnsim_eval.c — sizing and evaluation of compiled fn programs (fnsim.h).
 *
 * Three passes over the postorder program, all linear: (1) forward, each node's self-determined
 * width and signedness (Verilog-2005 table 5-22); (2) backward, so that a parent comes before its
 * operands, the width and signedness each node is computed at (an fn root: its own or its output's
 * width, whichever is wider; a context-determined operand: its parent's; a self-determined
 * operand: its own) and the offset of its value; (3) forward, the values. The sizing hook runs
 * (1) and (2) on a temporary record array; the simulate hook runs all three in the cell's scratch.
 */
#include "ir/value.h"
#include "sim/word.h"
#include "techlib/expr.h"
#include "techlib/fnsim.h"
#include "techlib/fnsim_internal.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { INT_BITS = 32, WIDE_INT_BITS = 64, MAX_WIDTH = 1 << 24, LIMB_BYTES = 8 };

/* Sizes and value of one node; lo/hi: a select's indices, a replication's count, an integer. */
typedef struct size_rec {
    int64_t lo;
    int64_t hi;
    uint32_t self;  /* self-determined width */
    uint32_t width; /* the width it is computed at (>= self) */
    uint32_t off;   /* its value: limbs[off ..] */
    bool self_signed;
    bool is_signed; /* the signedness it is computed with */
} size_rec;

typedef struct run {
    const odin3_fnsim_prog *prog;
    const odin3_techlib_cell *lib;
    const odin3_sim_cell *cell;
    size_rec *rec;
    uint64_t *limbs;  /* NULL while only sizing */
    uint64_t n_limbs; /* limbs of all values */
    bool too_wide;
} run;

/* --- constant indices and counts -------------------------------------------------------------- */

static bool lookup_param(const void *user, uint32_t ident, int64_t *value) {
    const run *rn = user;
    for (uint32_t i = 0; i < rn->prog->n_params; i++) {
        if (rn->prog->param_ids[i] == ident && rn->cell->params[i].kind == ODIN3_VAL_INT) {
            *value = rn->cell->params[i].i;
            return true;
        }
    }
    return false;
}

/* The value of a constant expression over the parameters; -1 when it does not evaluate. */
static int64_t const_value(const run *rn, const odin3_expr *expr) {
    const odin3_expr_env env = {lookup_param, rn, NULL};
    odin3_expr_error err;
    int64_t value = -1;
    return odin3_expr_eval_int_quiet(expr, &env, &value, &err) == ODIN3_OK ? value : -1;
}

/* --- pass 1: self-determined sizes ------------------------------------------------------------ */

static bool context_op(odin3_expr_op op) {
    switch (op) {
    case ODIN3_OP_AND:
    case ODIN3_OP_OR:
    case ODIN3_OP_XOR:
    case ODIN3_OP_XNOR:
    case ODIN3_OP_ADD:
    case ODIN3_OP_SUB:
    case ODIN3_OP_MUL:
        return true;
    default:
        return false;
    }
}

static bool shift_op(odin3_expr_op op) {
    return op == ODIN3_OP_SHL || op == ODIN3_OP_SHR;
}

static bool logic_op(odin3_expr_op op) {
    return op == ODIN3_OP_LAND || op == ODIN3_OP_LOR;
}

static void set_self(run *rn, size_rec *rec, uint64_t width, bool is_signed) {
    if (width > MAX_WIDTH) {
        rn->too_wide = true;
        width = 1;
    }
    rec->self = (uint32_t)width;
    rec->self_signed = is_signed;
}

static uint64_t max_u64(uint64_t lhs, uint64_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

/* An integer (a parameter or plain decimal): signed, 32 bits unless it needs 64. */
static void size_int(run *rn, size_rec *rec, int64_t value) {
    rec->lo = value;
    set_self(rn, rec, value >= INT32_MIN && value <= INT32_MAX ? INT_BITS : WIDE_INT_BITS, true);
}

static void size_op(run *rn, const odin3_fnsim_node *node, size_rec *rec) {
    const size_rec *lhs = &rn->rec[node->kid[0]];
    if (node->kind == ODIN3_FNSIM_UNARY) {
        bool lnot = node->op == ODIN3_OP_LNOT;
        set_self(rn, rec, lnot ? 1 : lhs->self, !lnot && lhs->self_signed);
        return;
    }
    const size_rec *rhs = &rn->rec[node->kid[1]];
    if (node->kind == ODIN3_FNSIM_TERNARY) {
        const size_rec *alt = &rn->rec[node->kid[2]];
        set_self(rn, rec, max_u64(rhs->self, alt->self), rhs->self_signed && alt->self_signed);
    } else if (context_op(node->op)) {
        set_self(rn, rec, max_u64(lhs->self, rhs->self), lhs->self_signed && rhs->self_signed);
    } else if (shift_op(node->op)) {
        set_self(rn, rec, lhs->self, lhs->self_signed);
    } else { /* comparisons, && and || */
        set_self(rn, rec, 1, false);
    }
}

static void size_select(run *rn, const odin3_fnsim_node *node, size_rec *rec) {
    rec->lo = const_value(rn, node->expr->b);
    rec->hi = rec->lo;
    if (node->kind == ODIN3_FNSIM_SLICE) {
        int64_t lsb = const_value(rn, node->expr->c);
        rec->lo = lsb < rec->hi ? lsb : rec->hi;
        rec->hi = lsb < rec->hi ? rec->hi : lsb;
        if (rec->lo < 0 && rec->hi >= 0) {
            rec->lo = -1; /* an index did not evaluate: one bit that reads 0 */
            rec->hi = -1;
        }
    }
    set_self(rn, rec, (uint64_t)(rec->hi - rec->lo) + 1, false);
}

static void size_concat(run *rn, const odin3_fnsim_node *node, size_rec *rec) {
    uint64_t width = 0;
    if (node->kind == ODIN3_FNSIM_REPL) {
        int64_t count = const_value(rn, node->expr->a);
        rec->lo = count > 0 ? count : 0;
        uint64_t body = rn->rec[node->kid[0]].self;
        width = body != 0 && (uint64_t)rec->lo > MAX_WIDTH ? (uint64_t)MAX_WIDTH + 1
                                                           : (uint64_t)rec->lo * body;
    } else {
        for (uint32_t i = 0; i < node->count; i++) {
            width += rn->rec[rn->prog->items[node->first + i]].self;
        }
    }
    set_self(rn, rec, width, false);
}

static void size_node(run *rn, uint32_t idx) {
    const odin3_fnsim_node *node = &rn->prog->nodes[idx];
    size_rec *rec = &rn->rec[idx];
    memset(rec, 0, sizeof *rec);
    switch (node->kind) {
    case ODIN3_FNSIM_PORT:
        set_self(rn, rec, rn->cell->ports[node->ref].width, rn->lib->ports[node->ref].is_signed);
        break;
    case ODIN3_FNSIM_PARAM:
        size_int(rn, rec, rn->cell->params[node->ref].i);
        break;
    case ODIN3_FNSIM_INT:
        size_int(rn, rec, node->expr->ival);
        break;
    case ODIN3_FNSIM_SIZED:
        set_self(rn, rec, node->expr->nbits, false);
        break;
    case ODIN3_FNSIM_BITSEL:
    case ODIN3_FNSIM_SLICE:
        size_select(rn, node, rec);
        break;
    case ODIN3_FNSIM_CONCAT:
    case ODIN3_FNSIM_REPL:
        size_concat(rn, node, rec);
        break;
    default:
        size_op(rn, node, rec);
        break;
    }
}

/* --- pass 2: the width each node is computed at, and where its value lives -------------------- */

static void give(const run *rn, uint32_t kid, uint32_t width, bool is_signed) {
    rn->rec[kid].width = width;
    rn->rec[kid].is_signed = is_signed;
}

static void own(const run *rn, uint32_t kid) {
    give(rn, kid, rn->rec[kid].self, rn->rec[kid].self_signed);
}

static void place_binary(const run *rn, const odin3_fnsim_node *node, const size_rec *rec) {
    const size_rec *lhs = &rn->rec[node->kid[0]];
    const size_rec *rhs = &rn->rec[node->kid[1]];
    if (context_op(node->op)) {
        give(rn, node->kid[0], rec->width, rec->is_signed);
        give(rn, node->kid[1], rec->width, rec->is_signed);
    } else if (shift_op(node->op)) {
        give(rn, node->kid[0], rec->width, rec->is_signed);
        own(rn, node->kid[1]); /* the shift amount is self-determined and unsigned */
    } else if (logic_op(node->op)) {
        own(rn, node->kid[0]);
        own(rn, node->kid[1]);
    } else { /* a comparison: both operands at the wider width, signed only if both are */
        uint32_t width = lhs->self > rhs->self ? lhs->self : rhs->self;
        bool is_signed = lhs->self_signed && rhs->self_signed;
        give(rn, node->kid[0], width, is_signed);
        give(rn, node->kid[1], width, is_signed);
    }
}

static void place_kids(const run *rn, const odin3_fnsim_node *node, const size_rec *rec) {
    switch (node->kind) {
    case ODIN3_FNSIM_UNARY:
        if (node->op == ODIN3_OP_LNOT) {
            own(rn, node->kid[0]);
        } else {
            give(rn, node->kid[0], rec->width, rec->is_signed);
        }
        break;
    case ODIN3_FNSIM_BINARY:
        place_binary(rn, node, rec);
        break;
    case ODIN3_FNSIM_TERNARY:
        own(rn, node->kid[0]);
        give(rn, node->kid[1], rec->width, rec->is_signed);
        give(rn, node->kid[2], rec->width, rec->is_signed);
        break;
    case ODIN3_FNSIM_BITSEL:
    case ODIN3_FNSIM_SLICE:
    case ODIN3_FNSIM_REPL:
        own(rn, node->kid[0]);
        break;
    case ODIN3_FNSIM_CONCAT:
        for (uint32_t i = 0; i < node->count; i++) {
            own(rn, rn->prog->items[node->first + i]);
        }
        break;
    default:
        break;
    }
}

static void place(run *rn) {
    rn->n_limbs = 0;
    for (uint32_t idx = rn->prog->n_nodes; idx-- > 0;) {
        const odin3_fnsim_node *node = &rn->prog->nodes[idx];
        size_rec *rec = &rn->rec[idx];
        if (node->out != ODIN3_FNSIM_NONE) {
            uint32_t out_width = rn->cell->ports[node->out].width;
            rec->width = rec->self > out_width ? rec->self : out_width;
            rec->is_signed = rec->self_signed;
        }
        rec->off = (uint32_t)rn->n_limbs;
        rn->n_limbs += odin3_word_limbs(rec->width);
        if (rn->n_limbs > UINT32_MAX) {
            rn->too_wide = true;
            rn->n_limbs = 0;
        }
        place_kids(rn, node, rec);
    }
}

static void size_all(run *rn) {
    for (uint32_t idx = 0; idx < rn->prog->n_nodes; idx++) {
        size_node(rn, idx);
    }
    place(rn);
}

static uint64_t record_bytes(const odin3_fnsim_prog *prog) {
    return (uint64_t)prog->n_nodes * sizeof(size_rec); /* a multiple of 8: limbs follow aligned */
}

odin3_status odin3_fnsim_scratch(const odin3_sim_cell *cell, uint32_t *bytes) {
    const odin3_techlib_cell *lib = cell->type_data;
    if (lib == NULL || lib->sim == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    size_rec *rec = odin3_util_malloc((size_t)record_bytes(lib->sim));
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    run rn = {lib->sim, lib, cell, rec, NULL, 0, false};
    size_all(&rn);
    odin3_util_free(rec);
    uint64_t total = record_bytes(lib->sim) + rn.n_limbs * LIMB_BYTES;
    if (rn.too_wide || total > UINT32_MAX) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *bytes = (uint32_t)total;
    return ODIN3_OK;
}

/* --- pass 3: values --------------------------------------------------------------------------- */

static uint64_t *node_value(const run *rn, uint32_t idx) {
    return rn->limbs + rn->rec[idx].off;
}

/* Copies count bits of src (from bit 0) into dst from bit pos (dst bits there are 0). */
static void copy_bits(uint64_t *dst, uint32_t pos, const uint64_t *src, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        if (odin3_word_bit(src, k)) {
            odin3_word_set_bit(dst, pos + k);
        }
    }
}

/* Widens a node's self-determined result (in its low self bits) to the width it is computed at. */
static void widen(const run *rn, uint32_t idx) {
    const size_rec *rec = &rn->rec[idx];
    const odin3_word_resize resize = {rec->self, rec->width, rec->is_signed};
    odin3_word_extend(node_value(rn, idx), &resize);
}

static void eval_leaf(const run *rn, const odin3_fnsim_node *node, uint32_t idx) {
    const size_rec *rec = &rn->rec[idx];
    uint64_t *dst = node_value(rn, idx);
    odin3_word_zero(dst, rec->width);
    if (node->kind == ODIN3_FNSIM_PORT) {
        const odin3_word_port port = {rn->cell->values, rn->cell->ports[node->ref], rec->is_signed};
        odin3_word_load(dst, rec->width, &port);
        return;
    }
    if (node->kind == ODIN3_FNSIM_SIZED) {
        for (uint32_t k = 0; k < node->expr->nbits; k++) {
            if (node->expr->bits[k] == ODIN3_BIT_1) {
                odin3_word_set_bit(dst, k);
            }
        }
    } else { /* an integer, as its self bits */
        dst[0] = (uint64_t)rec->lo;
        odin3_word_mask(dst, rec->self);
    }
    widen(rn, idx);
}

/* dst gets 0 or 1 (self width 1), widened to the node's width. */
static void eval_flag(const run *rn, uint32_t idx, bool flag) {
    uint64_t *dst = node_value(rn, idx);
    odin3_word_zero(dst, rn->rec[idx].width);
    dst[0] = flag ? 1 : 0;
    widen(rn, idx);
}

static bool compare(const run *rn, const odin3_fnsim_node *node) {
    const size_rec *lhs = &rn->rec[node->kid[0]];
    const odin3_word_bin cmp = {NULL, node_value(rn, node->kid[0]), node_value(rn, node->kid[1]),
                                lhs->width};
    int order = lhs->is_signed ? odin3_word_scmp(&cmp) : odin3_word_ucmp(&cmp);
    switch (node->op) {
    case ODIN3_OP_EQ:
        return order == 0;
    case ODIN3_OP_NE:
        return order != 0;
    case ODIN3_OP_LT:
        return order < 0;
    case ODIN3_OP_LE:
        return order <= 0;
    case ODIN3_OP_GT:
        return order > 0;
    default: /* ODIN3_OP_GE */
        return order >= 0;
    }
}

static bool nonzero(const run *rn, uint32_t idx) {
    return !odin3_word_is_zero(node_value(rn, idx), rn->rec[idx].width);
}

static odin3_word_fn word_fn(odin3_expr_op op) {
    switch (op) {
    case ODIN3_OP_AND:
        return odin3_word_and;
    case ODIN3_OP_OR:
        return odin3_word_or;
    case ODIN3_OP_ADD:
        return odin3_word_add;
    case ODIN3_OP_SUB:
        return odin3_word_sub;
    case ODIN3_OP_MUL:
        return odin3_word_mul;
    default: /* XOR, XNOR (then inverted) */
        return odin3_word_xor;
    }
}

static void eval_binary(const run *rn, const odin3_fnsim_node *node, uint32_t idx) {
    const odin3_word_bin op = {node_value(rn, idx), node_value(rn, node->kid[0]),
                               node_value(rn, node->kid[1]), rn->rec[idx].width};
    if (context_op(node->op)) {
        word_fn(node->op)(&op);
        if (node->op == ODIN3_OP_XNOR) {
            const odin3_word_bin inv = {op.dst, op.dst, NULL, op.width};
            odin3_word_not(&inv);
        }
    } else if (shift_op(node->op)) {
        const size_rec *amount = &rn->rec[node->kid[1]];
        uint64_t by = odin3_word_amount(op.rhs, amount->width);
        if (node->op == ODIN3_OP_SHL) {
            odin3_word_shl(&op, by);
        } else {
            odin3_word_shr(&op, by);
        }
    } else if (node->op == ODIN3_OP_LAND) {
        eval_flag(rn, idx, nonzero(rn, node->kid[0]) && nonzero(rn, node->kid[1]));
    } else if (node->op == ODIN3_OP_LOR) {
        eval_flag(rn, idx, nonzero(rn, node->kid[0]) || nonzero(rn, node->kid[1]));
    } else {
        eval_flag(rn, idx, compare(rn, node));
    }
}

static void eval_unary(const run *rn, const odin3_fnsim_node *node, uint32_t idx) {
    if (node->op == ODIN3_OP_LNOT) {
        eval_flag(rn, idx, !nonzero(rn, node->kid[0]));
        return;
    }
    const odin3_word_bin op = {node_value(rn, idx), node_value(rn, node->kid[0]), NULL,
                               rn->rec[idx].width};
    if (node->op == ODIN3_OP_NEG) {
        odin3_word_neg(&op);
    } else {
        odin3_word_not(&op);
    }
}

/* A bit select or slice: bits lo..hi of the operand; a bit outside it reads 0. */
static void eval_select(const run *rn, const odin3_fnsim_node *node, uint32_t idx) {
    const size_rec *rec = &rn->rec[idx];
    const size_rec *base = &rn->rec[node->kid[0]];
    const uint64_t *src = node_value(rn, node->kid[0]);
    uint64_t *dst = node_value(rn, idx);
    odin3_word_zero(dst, rec->width);
    for (uint32_t k = 0; k < rec->self; k++) {
        int64_t pos = rec->lo + (int64_t)k;
        if (pos >= 0 && pos < (int64_t)base->width && odin3_word_bit(src, (uint32_t)pos)) {
            odin3_word_set_bit(dst, k);
        }
    }
    widen(rn, idx);
}

/* Concatenation (the last item lowest) or replication of the body. */
static void eval_concat(const run *rn, const odin3_fnsim_node *node, uint32_t idx) {
    uint64_t *dst = node_value(rn, idx);
    odin3_word_zero(dst, rn->rec[idx].width);
    uint32_t pos = 0;
    if (node->kind == ODIN3_FNSIM_REPL) {
        uint32_t body = rn->rec[node->kid[0]].self;
        for (int64_t i = 0; body != 0 && i < rn->rec[idx].lo; i++, pos += body) {
            copy_bits(dst, pos, node_value(rn, node->kid[0]), body);
        }
    } else {
        for (uint32_t i = node->count; i-- > 0;) {
            uint32_t item = rn->prog->items[node->first + i];
            copy_bits(dst, pos, node_value(rn, item), rn->rec[item].self);
            pos += rn->rec[item].self;
        }
    }
    widen(rn, idx);
}

static void eval_node(const run *rn, uint32_t idx) {
    const odin3_fnsim_node *node = &rn->prog->nodes[idx];
    switch (node->kind) {
    case ODIN3_FNSIM_UNARY:
        eval_unary(rn, node, idx);
        break;
    case ODIN3_FNSIM_BINARY:
        eval_binary(rn, node, idx);
        break;
    case ODIN3_FNSIM_TERNARY: {
        uint32_t pick = nonzero(rn, node->kid[0]) ? node->kid[1] : node->kid[2];
        memcpy(node_value(rn, idx), node_value(rn, pick),
               (size_t)odin3_word_limbs(rn->rec[idx].width) * sizeof(uint64_t));
        break;
    }
    case ODIN3_FNSIM_BITSEL:
    case ODIN3_FNSIM_SLICE:
        eval_select(rn, node, idx);
        break;
    case ODIN3_FNSIM_CONCAT:
    case ODIN3_FNSIM_REPL:
        eval_concat(rn, node, idx);
        break;
    default:
        eval_leaf(rn, node, idx);
        break;
    }
}

void odin3_fnsim_simulate(const odin3_sim_cell *cell) {
    const odin3_techlib_cell *lib = cell->type_data;
    run rn = {lib->sim, lib, cell, (size_rec *)cell->scratch, NULL, 0, false};
    size_all(&rn);
    rn.limbs = (uint64_t *)(cell->scratch + record_bytes(lib->sim));
    for (uint32_t idx = 0; idx < lib->sim->n_nodes; idx++) {
        eval_node(&rn, idx);
        uint32_t out = lib->sim->nodes[idx].out;
        if (out != ODIN3_FNSIM_NONE) {
            const odin3_word_port port = {cell->values, cell->ports[out], false};
            odin3_word_store(node_value(&rn, idx), rn.rec[idx].width, &port);
        }
    }
}
