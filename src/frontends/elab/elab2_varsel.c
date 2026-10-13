/*
 * elab2_varsel.c — the positions of a non-constant select that is a procedural target (2D Task 22;
 * EL-32 writes). `y[s] = d` and `y[b +: w] <= d` are Yosys's case method (its `nowrshmsk`
 * rewrite): one case per position the select can take, each assigning that constant slice, no
 * default. This file decides which positions those are; the process generator (pg_select.c) builds
 * the case and elab2_assign.c puts the union of the bits onto the lvalue.
 *
 * A position is a declared index value the base can hold (the index expression's self-determined
 * width and sign limit it: a 2-bit unsigned `s` takes 0..3, a signed one -2..1, as Yosys's case
 * items do) at which the slice lies at least partly inside the object. A slice that straddles
 * either end of the object writes its in-range bits only (IEEE 1364-2005 4.2.1; Yosys 0.55 does
 * the same): the position records which bits of the select those are.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { VS_MAGNITUDE_LIMIT = 63 };

/* Whether the index (its width and sign) can hold the value. */
static bool varsel_fits(const odin3_elab2_varsel *vs, int64_t value) {
    uint32_t bits = vs->index_signed ? vs->index_width - 1U : vs->index_width; /* magnitude bits */
    if (bits >= VS_MAGNITUDE_LIMIT) {
        return vs->index_signed || value >= 0;
    }
    int64_t limit = INT64_C(1) << bits;
    return vs->index_signed ? (value >= -limit && value < limit) : (value >= 0 && value < limit);
}

/* The select's width, from a [b +: w] / [b -: w] width or 1 for a bit select. */
static odin3_status varsel_width(odin3_elab2 *e2, odin3_elab2_varsel *vs) {
    odin3_sigbit zero_bit = odin3_sb_of_bit(ODIN3_BIT_0);
    odin3_sig zero = {&zero_bit, 1, false};
    odin3_elab2_selrange range;
    odin3_status st = odin3_elab2_sel_range(e2, vs->node, &vs->obj, &zero, &range);
    vs->width = st == ODIN3_OK ? range.width : 1U;
    return st;
}

/* One value: a position when some bit of the slice it selects lies inside the object. */
static odin3_status varsel_try(odin3_elab2_varsel *vs, bool plus, int64_t value) {
    odin3_elab2_selrange range = {0, 0, vs->width, true, true};
    odin3_elab2_sel_orient(&vs->obj, plus, value, &range);
    /* bit j sits at wire position pos0 + j, whether or not that is inside the object */
    int64_t index0 = odin3_elab2_sel_index(&range, 0);
    int64_t pos0 = vs->obj.msb >= vs->obj.lsb ? index0 - vs->obj.lsb : vs->obj.lsb - index0;
    int64_t first = pos0 < 0 ? -pos0 : 0;
    int64_t end = pos0 + (int64_t)vs->width; /* one past the last bit's position */
    end = end > (int64_t)vs->obj.width ? (int64_t)vs->obj.width : end;
    if (first >= (int64_t)vs->width || pos0 + first >= end) {
        return ODIN3_OK;
    }
    odin3_elab2_varcand *cand = odin3_vec_push(&vs->cands);
    if (cand == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *cand = (odin3_elab2_varcand){value, (uint32_t)(pos0 + first), (uint32_t)first,
                                  (uint32_t)(end - (pos0 + first))};
    return ODIN3_OK;
}

odin3_status odin3_elab2_varsel_make(odin3_elab2 *e2, odin3_elab2_varsel *vs) {
    odin3_vec_clear(&vs->cands);
    odin3_etype type;
    odin3_status st = varsel_width(e2, vs);
    st = st != ODIN3_OK ? st : odin3_elab2_self_type(e2, vs->index, &type);
    if (st != ODIN3_OK) {
        return st;
    }
    vs->index_width = type.width > 0 ? type.width : 1U;
    vs->index_signed = type.is_signed;
    bool plus = odin3_ast_sub(e2->elab, vs->node) != ODIN3_AST_SELECT_PART_MINUS;
    int64_t low = vs->obj.msb < vs->obj.lsb ? vs->obj.msb : vs->obj.lsb;
    int64_t high = vs->obj.msb < vs->obj.lsb ? vs->obj.lsb : vs->obj.msb;
    /* a base outside the declared range still reaches the object when the slice straddles it */
    int64_t reach = (int64_t)vs->width - 1;
    for (int64_t value = low - reach; st == ODIN3_OK && value <= high + reach; value++) {
        st = varsel_fits(vs, value) ? varsel_try(vs, plus, value) : ODIN3_OK;
    }
    uint64_t bits = (uint64_t)vs->cands.len * vs->width;
    if (st == ODIN3_OK && bits > e2->limits->max_width) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, vs->node),
                          "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits", bits,
                          e2->limits->max_width);
        return ODIN3_ERR_PARSE;
    }
    return st;
}
