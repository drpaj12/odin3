/*
 * elab2_select.c — stage 2's selects (2D Task 18; EL-32 reads): the object of a select and its
 * declared range, what a constant select covers, the bits of a constant select (out of range:
 * x with the warning) and the non-constant read through $shiftx with genrtlil's offset
 * computation (frontends/ast/genrtlil.cc, AST_IDENTIFIER with a dynamic range: the index minus
 * the declaration's low index, then for an ascending range (width - w) minus that; B is signed
 * when an offset was subtracted, the index is signed or 32 bits wide). A bit select makes
 * Yosys's cells; a +: or -: read builds its base more simply (one $sub(base, w - 1) for -:,
 * nothing for +:; spec 17 #42), equal modulo 2^32. The walk that hands it its operands is
 * elab2_expr.c; the lvalue side is elab2_assign.c.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SIG_BITS = 64, SUB_BITS = 32 };

/* Constant select indices are clamped to +-SEL_BAND (2^40): see sel_orient. */
static const int64_t SEL_BAND = INT64_C(1) << 40;

/* --- constants ------------------------------------------------------------------------------- */

/* The bit of a sig that sits at bit position pos, 0 or 1 (a constant sig only). */
static uint64_t sig_bit(const odin3_sig *sig, uint32_t pos) {
    return odin3_sb_bit(sig->bits[pos]) == ODIN3_BIT_1 ? 1U : 0U;
}

/* The value of a known constant sig, saturated to int64. */
static int64_t sig_value(const odin3_sig *sig) {
    uint64_t low = 0;
    uint32_t width = sig->width;
    for (uint32_t i = 0; i < width && i < SIG_BITS; i++) {
        low |= sig_bit(sig, i) << i;
    }
    bool negative = sig->is_signed && width > 0 && sig_bit(sig, width - 1) != 0;
    uint32_t fit = SIG_BITS - 1U; /* bits that carry the magnitude */
    for (uint32_t i = fit; i < width; i++) {
        if ((sig_bit(sig, i) != 0) != negative) {
            return negative ? INT64_MIN : INT64_MAX;
        }
    }
    if (negative && width < SIG_BITS) {
        low |= ~(uint64_t)0 << width;
    }
    return (int64_t)low;
}

int odin3_elab2_sig_i64(const odin3_sig *sig, int64_t *out) {
    bool unknown = false;
    for (uint32_t i = 0; i < sig->width; i++) {
        if (!odin3_sb_is_const(sig->bits[i])) {
            return -1;
        }
        odin3_bit bit = odin3_sb_bit(sig->bits[i]);
        unknown |= bit == ODIN3_BIT_X || bit == ODIN3_BIT_Z;
    }
    if (unknown) {
        return 0;
    }
    *out = sig_value(sig);
    return 1;
}

odin3_status odin3_elab2_number_i64(odin3_elab2 *e2, odin3_ast_id node, int64_t *out, bool *known) {
    if (odin3_ast_kind_of(e2->elab, node) != ODIN3_AST_NUMBER) {
        odin3_log(ODIN3_LOG_ERROR, "elaborated form: a select bound or width that is not a NUMBER");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_literal lit;
    odin3_cval val;
    odin3_status st = odin3_ast_number_value(e2->elab, node, e2->scratch, &lit);
    st = st != ODIN3_OK ? st : odin3_cval_from_literal(e2->scratch, &lit, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    *known = odin3_cval_to_i64(&val, out);
    return ODIN3_OK;
}

/* --- the object and its range ---------------------------------------------------------------- */

int64_t odin3_elab2_sel_pos(const odin3_elab2_selobj *obj, int64_t idx) {
    int64_t pos = 0;
    /* an index so far out that the difference does not fit is outside the object as well */
    bool overflow = obj->msb >= obj->lsb ? __builtin_sub_overflow(idx, (int64_t)obj->lsb, &pos)
                                         : __builtin_sub_overflow((int64_t)obj->lsb, idx, &pos);
    return !overflow && pos >= 0 && pos < (int64_t)obj->width ? pos : -1;
}

void odin3_elab2_sel_object(const odin3_elab2 *e2, odin3_ast_id object, uint32_t width,
                            odin3_elab2_selobj *out) {
    *out = (odin3_elab2_selobj){(int32_t)width - 1, 0, width, "expression"};
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, object);
    if (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME) {
        return;
    }
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, object));
    if (rec == NULL) {
        return;
    }
    out->name = odin3_strtab_get(odin3_design_strtab(e2->design), rec->name);
    if (rec->has_packed) {
        out->msb = rec->packed.msb;
        out->lsb = rec->packed.lsb;
    } else if (rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR) {
        out->msb = out->lsb = 0;
    }
}

/* The width of a range of that size, checked against the width cap (the typer's text). */
static odin3_status sel_check_width(odin3_elab2 *e2, odin3_ast_id select, uint64_t width) {
    if (width <= e2->limits->max_width) {
        return ODIN3_OK;
    }
    odin3_elab2_error(e2, odin3_ast_loc(e2->elab, select),
                      "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits", width,
                      e2->limits->max_width);
    return ODIN3_ERR_PARSE;
}

/* A NUMBER bound or width; an x or z bit is the error of a range bound. */
static odin3_status sel_number(odin3_elab2 *e2, odin3_ast_id node, int64_t *out) {
    bool known = false;
    odin3_status st = odin3_elab2_number_i64(e2, node, out, &known);
    if (st == ODIN3_OK && !known) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, node), "range bound has x or z bits");
        return ODIN3_ERR_PARSE;
    }
    return st;
}

/* [m:l]: against the declaration's direction is an error (a one-bit object has none). */
static odin3_status sel_part(odin3_elab2 *e2, odin3_ast_id select, const odin3_elab2_selobj *obj,
                             odin3_elab2_selrange *range) {
    int64_t left = 0;
    int64_t right = 0;
    odin3_status st = sel_number(e2, odin3_ast_child(e2->elab, select, 1), &left);
    st = st != ODIN3_OK ? st : sel_number(e2, odin3_ast_child(e2->elab, select, 2), &right);
    if (st != ODIN3_OK) {
        return st;
    }
    if (left != right && obj->msb != obj->lsb && (left > right) != (obj->msb > obj->lsb)) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, select),
                          "part-select [%" PRId64 ":%" PRId64
                          "] of '%s' is reversed relative to its declaration [%d:%d]",
                          left, right, obj->name, (int)obj->msb, (int)obj->lsb);
        return ODIN3_ERR_PARSE;
    }
    uint64_t span =
        (left > right ? (uint64_t)left - (uint64_t)right : (uint64_t)right - (uint64_t)left);
    span += 1U;
    st = sel_check_width(e2, select, span);
    range->left = left;
    range->right = right;
    range->width = st == ODIN3_OK ? (uint32_t)span : 1U;
    return st;
}

/* The width of a [b +: w] or [b -: w]: a positive NUMBER. */
static odin3_status sel_plus_width(odin3_elab2 *e2, odin3_ast_id select, uint32_t *width) {
    odin3_ast_id node = odin3_ast_child(e2->elab, select, 2);
    int64_t value = 0;
    odin3_status st = sel_number(e2, node, &value);
    if (st == ODIN3_OK && value <= 0) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, node),
                          "part-select width %" PRId64 " is not positive", value);
        return ODIN3_ERR_PARSE;
    }
    st = st != ODIN3_OK ? st : sel_check_width(e2, select, (uint64_t)value);
    *width = st == ODIN3_OK ? (uint32_t)value : 1U;
    return st;
}

/* [b +: w] and [b -: w] with a known base: the indices lo .. hi, oriented as the declaration. */
static void sel_orient(const odin3_elab2_selobj *obj, bool plus, int64_t base_in,
                       odin3_elab2_selrange *range) {
    /* A base beyond the band is clamped to its edge: it stays far outside every object (a width
     * is at most 2^32 bits), so the select reads x with the out-of-bounds warning, and the sums
     * below cannot overflow. */
    int64_t base = base_in > SEL_BAND ? SEL_BAND : base_in;
    base = base < -SEL_BAND ? -SEL_BAND : base;
    int64_t lo = plus ? base : base - ((int64_t)range->width - 1);
    int64_t hi = lo + ((int64_t)range->width - 1);
    bool descending = obj->msb >= obj->lsb;
    range->left = descending ? hi : lo;
    range->right = descending ? lo : hi;
}

odin3_status odin3_elab2_sel_range(odin3_elab2 *e2, odin3_ast_id select,
                                   const odin3_elab2_selobj *obj, const odin3_sig *index,
                                   odin3_elab2_selrange *range) {
    uint32_t sub = odin3_ast_sub(e2->elab, select);
    *range = (odin3_elab2_selrange){0, 0, 1, true, true};
    if (sub == ODIN3_AST_SELECT_PART) {
        return sel_part(e2, select, obj, range);
    }
    if (index == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_sel_range: a bit or +:/-: select without an index");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = ODIN3_OK;
    if (sub != ODIN3_AST_SELECT_BIT) {
        st = sel_plus_width(e2, select, &range->width);
    }
    int64_t base = 0;
    int rc = st == ODIN3_OK ? odin3_elab2_sig_i64(index, &base) : -1;
    range->constant = rc >= 0;
    range->known = rc != 0;
    if (st != ODIN3_OK || rc <= 0) {
        return st;
    }
    sel_orient(obj, sub == ODIN3_AST_SELECT_PART_PLUS, base, range);
    return ODIN3_OK;
}

/* --- constant selects ------------------------------------------------------------------------ */

static void sel_warn_outside(odin3_elab2 *e2, odin3_ast_id select, const odin3_elab2_selobj *obj,
                             const odin3_elab2_selrange *range) {
    odin3_loc loc = odin3_ast_loc(e2->elab, select);
    if (odin3_ast_sub(e2->elab, select) == ODIN3_AST_SELECT_BIT) {
        odin3_elab2_warn(e2, loc, "bit select [%" PRId64 "] of '%s' is out of bounds; the bit is x",
                         range->left, obj->name);
        return;
    }
    odin3_elab2_warn(e2, loc,
                     "range select [%" PRId64 ":%" PRId64
                     "] of '%s' is out of bounds; bits outside [%d:%d] are x",
                     range->left, range->right, obj->name, (int)obj->msb, (int)obj->lsb);
}

/* The bits at the declared indices; the ones outside the object read x, with the warning. */
static odin3_status sel_constant(odin3_elab2 *e2, const odin3_elab2_selargs *args,
                                 const odin3_elab2_selobj *obj, const odin3_elab2_selrange *range,
                                 odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, range->width, out);
    bool outside = false;
    for (uint32_t j = 0; st == ODIN3_OK && j < range->width; j++) {
        int64_t pos = range->known ? odin3_elab2_sel_pos(obj, odin3_elab2_sel_index(range, j)) : -1;
        outside |= range->known && pos < 0;
        out->bits[j] = pos >= 0 && pos < (int64_t)args->object->width
                           ? args->object->bits[pos]
                           : odin3_sb_of_bit(ODIN3_BIT_X);
    }
    if (st == ODIN3_OK && outside) {
        sel_warn_outside(e2, args->node, obj, range);
    }
    return st;
}

/* --- non-constant reads ($shiftx) ------------------------------------------------------------ */

/* A 32-bit constant signal of value (genrtlil's SigSpec(int)). */
static odin3_status sel_const32(odin3_elab2 *e2, int64_t value, bool is_signed, odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, SUB_BITS, out);
    uint32_t pattern = (uint32_t)(int32_t)value;
    for (uint32_t i = 0; st == ODIN3_OK && i < SUB_BITS; i++) {
        out->bits[i] = odin3_sb_of_bit(((pattern >> i) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    out->is_signed = is_signed;
    return st;
}

/* lhs - rhs as a $sub: both operands signed as the index is (genrtlil's Module::Sub), Y as wide as
 * the wider. */
static odin3_status sel_sub(odin3_elab2 *e2, odin3_ast_id node, odin3_sig lhs, odin3_sig rhs,
                            odin3_sig *out) {
    uint32_t width = lhs.width > rhs.width ? lhs.width : rhs.width;
    odin3_elab2_cell_req req = {ODIN3_E2T_SUB, node, &lhs, &rhs, width, NULL, NULL};
    return odin3_elab2_cell(e2, &req, out);
}

/* One step of the offset: shift - value (a constant on the right) or value - shift (on the left).
 */
typedef struct sel_step {
    odin3_ast_id node;
    int64_t value;
    bool value_first;
    bool is_signed;
} sel_step;

static odin3_status sel_offset(odin3_elab2 *e2, const sel_step *step, odin3_sig *shift) {
    odin3_sig value;
    odin3_status st = sel_const32(e2, step->value, step->is_signed, &value);
    if (st != ODIN3_OK) {
        return st;
    }
    shift->is_signed = step->is_signed;
    odin3_sig result;
    st = step->value_first ? sel_sub(e2, step->node, value, *shift, &result)
                           : sel_sub(e2, step->node, *shift, value, &result);
    if (st == ODIN3_OK) {
        *shift = result;
    }
    return st;
}

/* What a non-constant read is made of: the operands, the object and the select's width. */
typedef struct sel_target {
    const odin3_elab2_selargs *args;
    const odin3_elab2_selobj *obj;
    uint32_t width;
} sel_target;

/* The position of the select's right end in the object: index - low index, then flipped for an
 * ascending object; true in *offset when a $sub was made. */
static odin3_status sel_position(odin3_elab2 *e2, const sel_target *tg, odin3_sig *shift,
                                 bool *offset) {
    const odin3_elab2_selargs *args = tg->args;
    const odin3_elab2_selobj *obj = tg->obj;
    bool plain = args->index->is_signed;
    uint32_t sub = odin3_ast_sub(e2->elab, args->node);
    *shift = *args->index;
    odin3_status st = ODIN3_OK;
    if (sub == ODIN3_AST_SELECT_PART_MINUS &&
        tg->width > 1) { /* the lowest index: base - (w - 1) */
        st = sel_offset(e2, &(sel_step){args->node, (int64_t)tg->width - 1, false, plain}, shift);
        *offset = true;
    }
    int32_t low = obj->msb < obj->lsb ? obj->msb : obj->lsb;
    if (st == ODIN3_OK && low != 0) {
        st = sel_offset(e2, &(sel_step){args->node, low, false, plain}, shift);
        *offset = true;
    }
    if (st == ODIN3_OK && obj->msb < obj->lsb) {
        st = sel_offset(e2, &(sel_step){args->node, (int64_t)obj->width - tg->width, true, plain},
                        shift);
        *offset = true;
    }
    return st;
}

static odin3_status sel_dynamic(odin3_elab2 *e2, const sel_target *tg, odin3_sig *out) {
    const odin3_elab2_selargs *args = tg->args;
    odin3_sig shift;
    bool offset = false;
    odin3_status st = sel_position(e2, tg, &shift, &offset);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_sig object = *args->object;
    object.is_signed = false;
    shift.is_signed = offset || shift.width >= SUB_BITS || args->index->is_signed;
    odin3_elab2_cell_req req = {ODIN3_E2T_SHIFTX, args->node, &object, &shift,
                                tg->width,        NULL,       NULL};
    if (args->dest != NULL && args->dest_width == tg->width) {
        req.dest = args->dest;
    }
    return odin3_elab2_cell(e2, &req, out);
}

odin3_status odin3_elab2_select_emit(odin3_elab2 *e2, const odin3_elab2_selargs *args,
                                     odin3_sig *out) {
    odin3_ast_id object = odin3_ast_child(e2->elab, args->node, 0);
    odin3_elab2_selobj obj;
    odin3_elab2_selrange range;
    odin3_elab2_sel_object(e2, object, args->object->width, &obj);
    odin3_status st = odin3_elab2_sel_range(e2, args->node, &obj, args->index, &range);
    if (st != ODIN3_OK) {
        return st;
    }
    const sel_target target = {args, &obj, range.width};
    st = range.constant ? sel_constant(e2, args, &obj, &range, out) : sel_dynamic(e2, &target, out);
    out->is_signed = false;
    return st;
}

odin3_status odin3_elab2_select(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out) {
    if (odin3_ast_kind_of(e2->elab, req->expr) != ODIN3_AST_SELECT) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_select: not a SELECT node");
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_elab2_expr(e2, req, out);
}
