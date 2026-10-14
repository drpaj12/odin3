/*
 * fold.c — constant folding (2F spec OP-5; fold.h): the type table, the input loader, the 4-state
 * evaluation through 2D's cval operators and the connection of Y to the constant nets.
 */
#include "passes/opt/fold.h"

#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* How a type's inputs are read: its ports and which operands the evaluator gets. */
typedef enum fold_shape {
    SHAPE_UNARY,  /* A -> Y */
    SHAPE_BINARY, /* A, B -> Y */
    SHAPE_SHIFT,  /* A, B -> Y; B an unsigned amount ($shl $shr $sshr) */
    SHAPE_SHIFTX, /* A, B -> Y; B signed per B_SIGNED, x outside A */
    SHAPE_MUX,    /* A, B, S -> Y */
    SHAPE_PMUX    /* A, B (S_WIDTH slices), S -> Y */
} fold_shape;

/*
 * How an operator reads its operands (Yosys 0.55's CellTypes::eval on constants, probed with
 * opt_expr): AS_IS uses them whole at max(Y, A) bits (the shifts read B as an unsigned amount);
 * BITWISE extends each to Y by its own sign; NUMERIC reads each as the number its own sign gives
 * (so A signed and B unsigned mix) and truncates the exact result to Y.
 */
typedef enum fold_sign { SIGN_AS_IS, SIGN_BITWISE, SIGN_NUMERIC } fold_sign;

typedef struct fold_row {
    const char *type;
    const char *counter;
    fold_shape shape;
    fold_sign sign;
    odin3_cop op;
} fold_row;

enum {
    PORT_A = 0,
    PORT_B = 1,
    PORT_S = 2,
    UNARY_Y = 1,
    BINARY_Y = 2,
    MUX_Y = 3,
    PARAM_A_SIGNED = 0,
    PARAM_B_SIGNED = 1,
    ARENA_CHUNK = 4096
};

/* Spec §4's `const.$<type>` list, in counter order. */
static const fold_row k_rows[ODIN3_OPT_FOLD_TYPES] = {
    {"$not", "const.$not", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_NOT},
    {"$neg", "const.$neg", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_NEG},
    {"$reduce_and", "const.$reduce_and", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_RAND},
    {"$reduce_or", "const.$reduce_or", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_ROR},
    {"$reduce_xor", "const.$reduce_xor", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_RXOR},
    {"$reduce_xnor", "const.$reduce_xnor", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_RXNOR},
    {"$reduce_bool", "const.$reduce_bool", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_ROR},
    {"$logic_not", "const.$logic_not", SHAPE_UNARY, SIGN_AS_IS, ODIN3_COP_LNOT},
    {"$and", "const.$and", SHAPE_BINARY, SIGN_BITWISE, ODIN3_COP_AND},
    {"$or", "const.$or", SHAPE_BINARY, SIGN_BITWISE, ODIN3_COP_OR},
    {"$xor", "const.$xor", SHAPE_BINARY, SIGN_BITWISE, ODIN3_COP_XOR},
    {"$xnor", "const.$xnor", SHAPE_BINARY, SIGN_BITWISE, ODIN3_COP_XNOR},
    {"$lt", "const.$lt", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_LT},
    {"$le", "const.$le", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_LE},
    {"$eq", "const.$eq", SHAPE_BINARY, SIGN_AS_IS, ODIN3_COP_EQ},
    {"$ne", "const.$ne", SHAPE_BINARY, SIGN_AS_IS, ODIN3_COP_NE},
    {"$gt", "const.$gt", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_GT},
    {"$ge", "const.$ge", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_GE},
    {"$add", "const.$add", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_ADD},
    {"$sub", "const.$sub", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_SUB},
    {"$mul", "const.$mul", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_MUL},
    {"$div", "const.$div", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_DIV},
    {"$mod", "const.$mod", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_MOD},
    {"$pow", "const.$pow", SHAPE_BINARY, SIGN_NUMERIC, ODIN3_COP_POW},
    {"$logic_and", "const.$logic_and", SHAPE_BINARY, SIGN_AS_IS, ODIN3_COP_LAND},
    {"$logic_or", "const.$logic_or", SHAPE_BINARY, SIGN_AS_IS, ODIN3_COP_LOR},
    {"$shl", "const.$shl", SHAPE_SHIFT, SIGN_AS_IS, ODIN3_COP_SHL},
    {"$shr", "const.$shr", SHAPE_SHIFT, SIGN_AS_IS, ODIN3_COP_SHR},
    {"$sshr", "const.$sshr", SHAPE_SHIFT, SIGN_AS_IS, ODIN3_COP_SSHR},
    {"$shiftx", "const.$shiftx", SHAPE_SHIFTX, SIGN_AS_IS, ODIN3_COP_COUNT},
    {"$mux", "const.$mux", SHAPE_MUX, SIGN_AS_IS, ODIN3_COP_COUNT},
    {"$pmux", "const.$pmux", SHAPE_PMUX, SIGN_AS_IS, ODIN3_COP_COUNT}};

const char *odin3_opt_fold_name(uint32_t index) {
    return index < ODIN3_OPT_FOLD_TYPES ? k_rows[index].counter : NULL;
}

/* The index of node's row in k_rows, ODIN3_OPT_FOLD_TYPES for none. */
static uint32_t row_of(const odin3_opt_ctx *oc, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(oc->design, type);
    uint32_t i = 0;
    while (def != NULL && i < ODIN3_OPT_FOLD_TYPES && strcmp(k_rows[i].type, def->name) != 0) {
        i++;
    }
    return def != NULL ? i : ODIN3_OPT_FOLD_TYPES;
}

bool odin3_opt_foldable(const odin3_opt_ctx *oc, odin3_celltype_id type) {
    return oc != NULL && row_of(oc, type) < ODIN3_OPT_FOLD_TYPES;
}

/* --- loading ---------------------------------------------------------------------------------- */

/* The aval/bval bits of VPI 4-state value bit: 0 = (0,0), 1 = (1,0), z = (0,1), x = (1,1). */
typedef struct bit_at {
    uint32_t pos;
    odin3_const bit;
} bit_at;

static void set_bit(odin3_cval *val, bit_at at) {
    uint64_t mask = (uint64_t)1 << (at.pos % ODIN3_CVAL_WORD_BITS);
    uint32_t word = at.pos / ODIN3_CVAL_WORD_BITS;
    if (at.bit == ODIN3_CONST_1 || at.bit == ODIN3_CONST_X) {
        val->aval[word] |= mask;
    }
    if (at.bit == ODIN3_CONST_X || at.bit == ODIN3_CONST_Z) {
        val->bval[word] |= mask;
    }
}

odin3_status odin3_opt_load(const odin3_opt_ctx *oc, const odin3_opt_load_req *req, odin3_cval *out,
                            bool *all_const) {
    if (oc == NULL || req == NULL || out == NULL || all_const == NULL || req->arena == NULL ||
        !odin3_node_live(oc->module, req->node)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: odin3_opt_load: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_pinslice port = odin3_node_port(oc->module, req->node, req->port);
    *all_const = false;
    if (port.count == 0 || port.count >= ODIN3_ELAB_MAX_WIDTH) {
        return ODIN3_OK;
    }
    odin3_cval_spec spec = {port.count, req->is_signed ? ODIN3_CVAL_SIGNED : 0, ODIN3_BIT_0};
    odin3_cval val;
    odin3_status st = odin3_cval_new(req->arena, &spec, &val);
    for (uint32_t k = 0; st == ODIN3_OK && k < port.count; k++) {
        odin3_pin_id pin = {port.first.v + k};
        odin3_net_id net = odin3_pin_net(oc->module, pin);
        odin3_const bit = net.v != 0 ? odin3_net_const_value(oc->module, net) : ODIN3_CONST_NONE;
        if (bit == ODIN3_CONST_NONE) {
            return ODIN3_OK;
        }
        set_bit(&val, (bit_at){k, bit});
    }
    if (st == ODIN3_OK) {
        *out = val;
        *all_const = true;
    }
    return st;
}

/* --- evaluation ------------------------------------------------------------------------------- */

/* One cell's operands: loaded values, their signs and Y's width. */
typedef struct fold_job {
    odin3_opt_ctx *oc;
    odin3_node_id node;
    const fold_row *row;
    odin3_arena *arena;
    odin3_cval a, b, s;
    bool a_signed, b_signed, all_const;
    uint32_t y_width, y_port;
} fold_job;

static bool param_set(const odin3_opt_ctx *oc, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(oc->module, node, index);
    return val != NULL && val->i != 0;
}

/*
 * A z bit of a shift amount: Yosys's constshift (plain opt_expr, which takes any constant amount
 * before the fold, and the fold table with it) reads it as 0 (as_int_saturating), while
 * const_shift_worker (the fold under -keepdc, where constshift wants a fully defined amount) takes
 * any bit that is not 0 or 1 as undefined and gives all x. So z reads as 0 unless keepdc.
 */
static void z_to_zero(odin3_cval *val) {
    for (uint32_t i = 0; i < odin3_cval_words(val->width); i++) {
        val->bval[i] &= val->aval[i];
    }
}

/* Loads port `port` into *out; clears job->all_const when it is not constant. */
static odin3_status load_port(fold_job *job, uint32_t port, bool is_signed, odin3_cval *out) {
    odin3_opt_load_req req = {job->node, port, is_signed, job->arena};
    bool all = false;
    odin3_status st = odin3_opt_load(job->oc, &req, out, &all);
    job->all_const = job->all_const && all;
    return st;
}

/* The operands of the job's shape; job->all_const tells whether every one is constant. */
static odin3_status load_operands(fold_job *job) {
    const fold_shape shape = job->row->shape;
    job->all_const = true;
    odin3_status st = load_port(job, PORT_A, job->a_signed, &job->a);
    if (st == ODIN3_OK && shape != SHAPE_UNARY && job->all_const) {
        st = load_port(job, PORT_B, shape == SHAPE_SHIFT ? false : job->b_signed, &job->b);
        if (st == ODIN3_OK && job->all_const && !job->oc->opts->keepdc &&
            (shape == SHAPE_SHIFT || shape == SHAPE_SHIFTX)) {
            z_to_zero(&job->b);
        }
    }
    if (st == ODIN3_OK && (shape == SHAPE_MUX || shape == SHAPE_PMUX) && job->all_const) {
        st = load_port(job, PORT_S, false, &job->s);
    }
    return st;
}

static odin3_status all_x(fold_job *job, odin3_cval *out) {
    odin3_cval_spec spec = {job->y_width, 0, ODIN3_BIT_X};
    return odin3_cval_new(job->arena, &spec, out);
}

/* $shiftx: A[B +: Y_WIDTH], x outside A; an undefined B, or one no int64 holds, selects nothing. */
static odin3_status eval_shiftx(fold_job *job, odin3_cval *out) {
    int64_t offset = 0;
    if (!odin3_cval_to_i64(&job->b, &offset)) {
        return all_x(job, out);
    }
    odin3_cval_part part = {&job->a, offset, job->y_width};
    return odin3_cval_select(job->arena, &part, out);
}

/* $mux: S = 0 -> A, S = 1 -> B, S undefined -> A and B merged (Yosys const_mux). */
static odin3_status eval_mux(fold_job *job, odin3_cval *out) {
    odin3_cval_mux_args args = {&job->s, &job->b, &job->a};
    return odin3_cval_mux(job->arena, &args, out);
}

/* $pmux (Yosys const_pmux): S all 0 -> A; one bit set -> that slice of B; undefined S or several
 * set -> all x. */
static odin3_status eval_pmux(fold_job *job, odin3_cval *out) {
    uint32_t hot = 0;
    uint32_t set = 0;
    for (uint32_t i = 0; i < job->s.width; i++) {
        odin3_bit bit = odin3_cval_bit(&job->s, i);
        if (bit == ODIN3_BIT_X || bit == ODIN3_BIT_Z) {
            return all_x(job, out);
        }
        if (bit == ODIN3_BIT_1) {
            hot = i;
            set++;
        }
    }
    if (set == 0) {
        return odin3_cval_copy(job->arena, &job->a, out);
    }
    if (set > 1) {
        return all_x(job, out);
    }
    odin3_cval_part part = {&job->b, (int64_t)hot * job->y_width, job->y_width};
    return odin3_cval_select(job->arena, &part, out);
}

/* max(Y, A, B) bits when with_b, else max(Y, A). */
static uint32_t wide_width(const fold_job *job, bool with_b) {
    uint32_t width = job->y_width > job->a.width ? job->y_width : job->a.width;
    return with_b && job->b.width > width ? job->b.width : width;
}

/* BITWISE and NUMERIC: both operands extended by their own sign to width, then op's context. */
static odin3_status eval_extended(fold_job *job, uint32_t width, odin3_cval *out) {
    odin3_cval ext_a;
    odin3_cval ext_b;
    odin3_status st = odin3_cval_resize(job->arena, &job->a, width, job->a_signed, &ext_a);
    if (st == ODIN3_OK) {
        st = odin3_cval_resize(job->arena, &job->b, width, job->b_signed, &ext_b);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    bool numeric = job->row->sign == SIGN_NUMERIC;
    if (numeric) { /* one bit wider than every operand, so an unsigned value stays positive */
        ext_a.flags |= ODIN3_CVAL_SIGNED;
        ext_b.flags |= ODIN3_CVAL_SIGNED;
    }
    odin3_cop_args args = {job->row->op, &ext_a, &ext_b, width, numeric};
    return odin3_cval_op(job->arena, &args, out);
}

/* const_eq's verdict on two extended values: 0 on a definite mismatch, else x when a differing
 * pair is undefined (x against z too, but z against z or x against x not), else 1. */
static odin3_const eq_verdict(const odin3_cval *lhs, const odin3_cval *rhs) {
    bool undef = false;
    for (uint32_t i = 0; i < lhs->width; i++) {
        odin3_bit bit_l = odin3_cval_bit(lhs, i);
        odin3_bit bit_r = odin3_cval_bit(rhs, i);
        if (bit_l == bit_r) {
            continue;
        }
        if (bit_l < ODIN3_BIT_X && bit_r < ODIN3_BIT_X) {
            return ODIN3_CONST_0;
        }
        undef = true;
    }
    return undef ? ODIN3_CONST_X : ODIN3_CONST_1;
}

/* $eq and $ne: both extended to their common width, by sign when both are signed. */
static odin3_status eval_eq(fold_job *job, odin3_cval *out) {
    uint32_t width = job->a.width > job->b.width ? job->a.width : job->b.width;
    bool both = job->a_signed && job->b_signed;
    odin3_cval ext_a;
    odin3_cval ext_b;
    odin3_status st = odin3_cval_resize(job->arena, &job->a, width, both, &ext_a);
    if (st == ODIN3_OK) {
        st = odin3_cval_resize(job->arena, &job->b, width, both, &ext_b);
    }
    odin3_cval_spec spec = {job->y_width, 0, ODIN3_BIT_0};
    if (st == ODIN3_OK) {
        st = odin3_cval_new(job->arena, &spec, out);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_const verdict = eq_verdict(&ext_a, &ext_b);
    if (job->row->op == ODIN3_COP_NE && verdict != ODIN3_CONST_X) {
        verdict = verdict == ODIN3_CONST_1 ? ODIN3_CONST_0 : ODIN3_CONST_1;
    }
    set_bit(out, (bit_at){0, verdict});
    return ODIN3_OK;
}

/* The operator cells ($not ... $pow, the shifts): cval_op in the cell's context. */
static odin3_status eval_op(fold_job *job, odin3_cval *out) {
    if (job->row->op == ODIN3_COP_POW && !odin3_cval_is_known(&job->b)) {
        return all_x(job, out); /* const_pow: an undefined exponent gives all x */
    }
    if (job->row->op == ODIN3_COP_EQ || job->row->op == ODIN3_COP_NE) {
        return eval_eq(job, out);
    }
    if (job->row->sign == SIGN_NUMERIC) {
        return eval_extended(job, wide_width(job, true) + 1, out);
    }
    if (job->row->sign == SIGN_BITWISE) {
        return eval_extended(job, job->y_width, out);
    }
    odin3_cop_args args = {job->row->op, &job->a, job->row->shape == SHAPE_UNARY ? NULL : &job->b,
                           wide_width(job, false), job->a_signed};
    return odin3_cval_op(job->arena, &args, out);
}

static odin3_status evaluate(fold_job *job, odin3_cval *out) {
    switch (job->row->shape) {
    case SHAPE_SHIFTX:
        return eval_shiftx(job, out);
    case SHAPE_MUX:
        return eval_mux(job, out);
    case SHAPE_PMUX:
        return eval_pmux(job, out);
    case SHAPE_UNARY:
    case SHAPE_BINARY:
    case SHAPE_SHIFT:
    default:
        return eval_op(job, out);
    }
}

/* --- connecting Y ----------------------------------------------------------------------------- */

/* Deletes the node and connects Y bit k to the canonical constant net of result bit k. */
static odin3_status connect_result(fold_job *job, const odin3_cval *res) {
    odin3_opt_ctx *oc = job->oc;
    odin3_net_id *nets = odin3_arena_alloc(job->arena, (size_t)job->y_width * sizeof *nets);
    if (nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < job->y_width; k++) {
        odin3_status st = odin3_opt_const_net(oc, odin3_cval_bit(res, k), &nets[k]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_kit_conn conn = {job->node, job->y_port, nets, job->y_width};
    return odin3_kit_connect(oc, &conn);
}

static uint32_t y_port_of(fold_shape shape) {
    switch (shape) {
    case SHAPE_UNARY:
        return UNARY_Y;
    case SHAPE_MUX:
    case SHAPE_PMUX:
        return MUX_Y;
    case SHAPE_BINARY:
    case SHAPE_SHIFT:
    case SHAPE_SHIFTX:
    default:
        return BINARY_Y;
    }
}

/* Fills the job for node: false when Y's width is outside what the evaluator holds. */
static bool job_init(fold_job *job, uint32_t row) {
    const odin3_opt_ctx *oc = job->oc;
    job->row = &k_rows[row];
    job->y_port = y_port_of(job->row->shape);
    job->y_width = odin3_node_port(oc->module, job->node, job->y_port).count;
    bool is_mux = job->row->shape == SHAPE_MUX || job->row->shape == SHAPE_PMUX;
    job->a_signed = !is_mux && param_set(oc, job->node, PARAM_A_SIGNED);
    job->b_signed =
        job->row->shape != SHAPE_UNARY && !is_mux && param_set(oc, job->node, PARAM_B_SIGNED);
    /* A binary operator reads both operands signed only when both flags are set (the mixed-sign
     * ruling, spec 17 #57: simlib and the registry evaluator); the shifts and $pow keep B's flag
     * as the amount's own */
    if (job->row->shape == SHAPE_BINARY && job->row->op != ODIN3_COP_POW) {
        job->a_signed = job->b_signed = odin3_opt_both_signed(oc->module, job->node);
    }
    /* NUMERIC adds a bit: keep every width below the evaluator's cap */
    return job->y_width >= 1 && job->y_width < ODIN3_ELAB_MAX_WIDTH;
}

/* Evaluates the job's cell at Y's width into *out. */
static odin3_status evaluate_y(fold_job *job, odin3_cval *out) {
    odin3_cval res;
    odin3_status st = evaluate(job, &res);
    if (st == ODIN3_OK && res.width != job->y_width) {
        return odin3_cval_resize(job->arena, &res, job->y_width, false, out);
    }
    if (st == ODIN3_OK) {
        *out = res;
    }
    return st;
}

/* Evaluates and connects with the arena set up; *folded as odin3_opt_fold. */
static odin3_status fold_in_arena(fold_job *job, uint32_t row, bool *folded) {
    if (!job_init(job, row)) {
        return ODIN3_OK;
    }
    odin3_status st = load_operands(job);
    if (st != ODIN3_OK || !job->all_const) {
        return st;
    }
    odin3_cval res;
    st = evaluate_y(job, &res);
    if (st == ODIN3_OK) {
        st = connect_result(job, &res);
    }
    *folded = st == ODIN3_OK;
    return st;
}

odin3_status odin3_opt_fold(odin3_opt_ctx *oc, odin3_node_id node, bool *folded) {
    if (oc == NULL || folded == NULL || !odin3_node_live(oc->module, node)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: odin3_opt_fold: bad argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    *folded = false;
    uint32_t row = row_of(oc, odin3_node_type(oc->module, node));
    if (row >= ODIN3_OPT_FOLD_TYPES) {
        return ODIN3_OK;
    }
    fold_job job = {.oc = oc, .node = node};
    job.arena = odin3_arena_create(ARENA_CHUNK);
    if (job.arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = fold_in_arena(&job, row, folded);
    odin3_arena_destroy(job.arena);
    if (st == ODIN3_OK && *folded && oc->counts != NULL) {
        oc->counts->expr.folded++;
        oc->counts->fold_type[row]++;
    }
    return st;
}
