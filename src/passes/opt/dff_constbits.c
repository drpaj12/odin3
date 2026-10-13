/*
 * dff_constbits.c — opt_dff's constant bits (2F spec OP-9 §9.3; Yosys run_constbits, no -sat): per
 * bit, the value the init, the resets, the SET/CLR constants and a constant D or AD leave it stuck
 * at.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/dff.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { DETAIL_LEN = 64 };

/* combine_const's "no common constant" (Yosys State::Sm), beside odin3_bit's 0, 1, x, z. */
#define BIT_CONFLICT 5U

/* --- constbits -------------------------------------------------------------------------------- */

/* Yosys combine_const on two states (odin3_bit values, or BIT_CONFLICT). */
static uint32_t combine_const(const odin3_dff_state *ds, uint32_t lhs, uint32_t rhs) {
    if (lhs == ODIN3_BIT_X && !ds->oc->opts->keepdc) {
        return rhs;
    }
    if (rhs == ODIN3_BIT_X && !ds->oc->opts->keepdc) {
        return lhs;
    }
    return lhs == rhs ? lhs : BIT_CONFLICT;
}

/* A bit of the FF and the value found for it so far. */
typedef struct stuck_ref {
    uint32_t bit, val;
} stuck_ref;

/* What the SET/CLR of a bit allow: its value after combining 0 for a CLR and 1 for a SET that may
 * be active. */
static uint32_t sr_stuck(const odin3_dff_state *ds, stuck_ref ref) {
    const odin3_ffdata *ff = &ds->ff;
    uint32_t val = ref.val;
    bool clr_off = odin3_dff_net_is(ds, odin3_ff_net_at(&ff->sig_clr, ref.bit),
                                    ff->pol_clr ? ODIN3_CONST_0 : ODIN3_CONST_1);
    bool set_off = odin3_dff_net_is(ds, odin3_ff_net_at(&ff->sig_set, ref.bit),
                                    ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1);
    val = clr_off ? val : combine_const(ds, val, ODIN3_BIT_0);
    return set_off ? val : combine_const(ds, val, ODIN3_BIT_1);
}

/* val combined with the constant that net carries; BIT_CONFLICT when it is no constant. */
static uint32_t with_const(const odin3_dff_state *ds, uint32_t val, odin3_net_id net) {
    odin3_const cval = odin3_dff_net_const(ds, net);
    return cval == ODIN3_CONST_NONE ? BIT_CONFLICT
                                    : combine_const(ds, val, (uint32_t)odin3_dff_const_bit(cval));
}

/* The value bit `bit` of the FF is stuck at, BIT_CONFLICT when there is none. */
static uint32_t stuck_value(const odin3_dff_state *ds, uint32_t bit) {
    const odin3_ffdata *ff = &ds->ff;
    uint32_t val = (uint32_t)odin3_ff_bit_at(&ff->val_init, bit);
    val =
        ff->has_arst ? combine_const(ds, val, (uint32_t)odin3_ff_bit_at(&ff->val_arst, bit)) : val;
    val =
        ff->has_srst ? combine_const(ds, val, (uint32_t)odin3_ff_bit_at(&ff->val_srst, bit)) : val;
    val = ff->has_sr ? sr_stuck(ds, (stuck_ref){bit, val}) : val;
    if (val != BIT_CONFLICT && (ff->has_clk || ff->has_gclk)) {
        val = with_const(ds, val, odin3_ff_net_at(&ff->sig_d, bit));
    }
    if (val != BIT_CONFLICT && ff->has_aload) {
        val = with_const(ds, val, odin3_ff_net_at(&ff->sig_ad, bit));
    }
    return val;
}

static void note_const_bit(odin3_dff_state *ds, odin3_bit val, uint32_t at_bit) {
    static const odin3_opt_dff_action k_action[] = {
        ODIN3_OPT_DFF_CONST_BIT_0, ODIN3_OPT_DFF_CONST_BIT_1, ODIN3_OPT_DFF_CONST_BIT_X,
        ODIN3_OPT_DFF_CONST_BIT_Z};
    char detail[DETAIL_LEN];
    /* "setting constant <v>" is the action's text; the bit follows it (spec §16) */
    (void)snprintf(detail, sizeof detail, " at bit %" PRIu32, at_bit);
    odin3_dff_note(ds, k_action[val], detail);
}

/* Bit i: tied to its constant (removed++), or kept. */
static odin3_status constbit(odin3_dff_state *ds, uint32_t bit, odin3_vec *keep,
                             uint32_t *removed) {
    uint32_t val = stuck_value(ds, bit);
    if (val == BIT_CONFLICT) {
        return odin3_kit_push_u32(keep, bit);
    }
    odin3_net_id net = {0};
    odin3_status st = odin3_dff_const_net(ds, (odin3_bit)val, &net);
    st = st == ODIN3_OK ? odin3_dff_tie_bit(ds, bit, net) : st;
    note_const_bit(ds, (odin3_bit)val, bit);
    *removed += 1U;
    return st;
}

/* Ties the constant bits of the loaded FF and rebuilds it without them. */
static odin3_status constbits_ff(odin3_dff_state *ds) {
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(uint32_t));
    odin3_status st = ODIN3_OK;
    uint32_t removed = 0;
    for (uint32_t i = 0; st == ODIN3_OK && i < ds->ff.width; i++) {
        st = constbit(ds, i, &keep, &removed);
    }
    if (st == ODIN3_OK && removed > 0) {
        ds->changed = true;
        ds->n_bits += removed;
        st = keep.len == 0 ? odin3_dff_remove_all(ds, NULL)
                           : odin3_dff_state_keep(ds, keep.data, (uint32_t)keep.len);
        st = st == ODIN3_OK ? odin3_dff_commit(ds) : st;
    }
    odin3_vec_free(&keep);
    return st;
}

odin3_status odin3_dff_visit_constbits(odin3_dff_state *ds, odin3_node_id node) {
    bool usable = false;
    odin3_status st = odin3_dff_state_load(ds, node, &usable);
    return st == ODIN3_OK && usable ? constbits_ff(ds) : st;
}
