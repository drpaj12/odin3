/*
 * dff.c — opt_dff, rules 1-9 and 12 and constbits (2F spec OP-9 §9.3; plan Task 11): Yosys 0.55
 * passes/opt/opt_dff.cc OptDffWorker::run and run_constbits (no -sat) over Odin III's FF cells,
 * on the FF model of ff.c. Rules 10 and 11 (SRST and EN merging) are Task 12's.
 *
 * - Work list. The module's FFs in ID order; each is loaded into an odin3_ffdata, the rules run on
 *   it in §9.3's order, and a changed FF is re-emitted (rule 12). Task 12 pushes the FFs its
 *   groups make; Yosys does not queue a re-emitted FF again, nor does this.
 * - Q ties (plan point (l)). Yosys's SigMap is built once per worker: module->connect does not
 *   change what a later rule or constbits sees. Here every Q tie (a removed FF, a given-up bit, a
 *   constbits constant) is queued in oc->ties (odin3_kit_connect and ff.c under oc->defer_ties)
 *   and made at the end of the module's pass, so a constant register pipeline collapses one stage
 *   per call. A removed bit stays in the FF model's `pend` until its FF is rebuilt without it.
 * - An FF with a pin left open, or two Q pins on one net, is no candidate (spec §17 #31).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
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

enum { LABEL_LEN = 128, TYPE_LEN = 32, DETAIL_LEN = 64, MAX_GATE_PORTS = 4, MAX_GATE_PARAMS = 8 };

/* combine_const's "no common constant" (Yosys State::Sm), beside odin3_bit's 0, 1, x, z. */
#define BIT_CONFLICT 5U

/* --- the log texts of the actions (spec §16) -------------------------------------------------- */

static const char *const k_action_text[ODIN3_OPT_DFF_N_ACTIONS] = {
    [ODIN3_OPT_DFF_NEVER_CLR] = "removing never-active CLR",
    [ODIN3_OPT_DFF_NEVER_SET] = "removing never-active SET",
    [ODIN3_OPT_DFF_ALWAYS_CLR] = "handling always-active CLR",
    [ODIN3_OPT_DFF_ALWAYS_SET] = "handling always-active SET",
    [ODIN3_OPT_DFF_CLR_SET_TO_ARST] = "converting CLR/SET to ARST",
    [ODIN3_OPT_DFF_NEVER_ALOAD] = "removing never-active ALOAD",
    [ODIN3_OPT_DFF_ALWAYS_ALOAD] = "handling always-active ALOAD",
    [ODIN3_OPT_DFF_ALOAD_TO_ARST] = "changing const-value ALOAD to ARST",
    [ODIN3_OPT_DFF_NEVER_ARST] = "removing never-active ARST",
    [ODIN3_OPT_DFF_ALWAYS_ARST] = "handling always-active ARST",
    [ODIN3_OPT_DFF_NEVER_SRST] = "removing never-active SRST",
    [ODIN3_OPT_DFF_ALWAYS_SRST] = "handling always-active SRST",
    [ODIN3_OPT_DFF_NEVER_EN] = "removing never-active EN",
    [ODIN3_OPT_DFF_ALWAYS_EN] = "handling always-active EN",
    [ODIN3_OPT_DFF_CONST_CLK] = "handling const clock",
    [ODIN3_OPT_DFF_D_EQ_Q] = "handling D = Q",
    [ODIN3_OPT_DFF_AD_EQ_Q] = "handling AD = Q",
    [ODIN3_OPT_DFF_ADD_SRST] = "adding SRST",
    [ODIN3_OPT_DFF_ADD_EN] = "adding EN",
    [ODIN3_OPT_DFF_CONST_BIT_0] = "setting constant 0",
    [ODIN3_OPT_DFF_CONST_BIT_1] = "setting constant 1",
    [ODIN3_OPT_DFF_CONST_BIT_X] = "setting constant x",
    [ODIN3_OPT_DFF_CONST_BIT_Z] = "setting constant z",
};

/* --- the state of one FF's visit -------------------------------------------------------------- */

typedef struct dff_state {
    odin3_opt_ctx *oc;
    odin3_ffdata ff;    /* the FF as the rules have left it (Yosys `ff`) */
    odin3_node_id cell; /* the node it was loaded from (Yosys `cell`), live until the commit */
    uint32_t width0;    /* the width of that node's Q */
    char type[TYPE_LEN];
    char label[LABEL_LEN];
    odin3_vec pend; /* odin3_net_pair {keep = what a given-up Q bit is tied to, drop = its Q net} */
    odin3_prov_id prov;
    bool prov_ready, changed, gone;
    uint32_t n_changed, n_bits; /* this call's totals */
} dff_state;

static void state_init(dff_state *ds, odin3_opt_ctx *oc) {
    memset(ds, 0, sizeof *ds);
    ds->oc = oc;
    odin3_ffdata_init(&ds->ff);
    odin3_vec_init(&ds->pend, sizeof(odin3_net_pair));
}

static void state_free(dff_state *ds) {
    odin3_ffdata_free(&ds->ff);
    odin3_vec_free(&ds->pend);
}

static odin3_net_id net_at(const odin3_vec *vec, uint32_t idx) {
    return *(const odin3_net_id *)odin3_vec_cat(vec, idx);
}

static odin3_bit bit_at(const odin3_vec *vec, uint32_t idx) {
    return *(const odin3_bit *)odin3_vec_cat(vec, idx);
}

/* What net carries: odin3_net_const_value (CONST_NONE for a wire or an open pin). */
static odin3_const net_const(const dff_state *ds, odin3_net_id net) {
    return odin3_net_valid(net) ? odin3_net_const_value(ds->oc->module, net) : ODIN3_CONST_NONE;
}

static bool net_is(const dff_state *ds, odin3_net_id net, odin3_const want) {
    return net_const(ds, net) == want;
}

/* True when every net of vec carries want. */
static bool vec_is(const dff_state *ds, const odin3_vec *vec, odin3_const want) {
    for (uint32_t i = 0; i < vec->len; i++) {
        if (!net_is(ds, net_at(vec, i), want)) {
            return false;
        }
    }
    return true;
}

static bool vec_all_const(const dff_state *ds, const odin3_vec *vec) {
    for (uint32_t i = 0; i < vec->len; i++) {
        if (net_const(ds, net_at(vec, i)) == ODIN3_CONST_NONE) {
            return false;
        }
    }
    return true;
}

static bool vec_equal(const odin3_vec *lhs, const odin3_vec *rhs) {
    if (lhs->len != rhs->len) {
        return false;
    }
    for (uint32_t i = 0; i < lhs->len; i++) {
        if (net_at(lhs, i).v != net_at(rhs, i).v) {
            return false;
        }
    }
    return true;
}

static bool init_defined(const dff_state *ds) {
    for (uint32_t i = 0; i < ds->ff.width; i++) {
        odin3_bit bit = bit_at(&ds->ff.val_init, i);
        if (bit != ODIN3_BIT_0 && bit != ODIN3_BIT_1) {
            return false;
        }
    }
    return true;
}

/* A control that is constant `active` (the polarity's active value) or, without -keepdc, x. */
static bool always_active(const dff_state *ds, odin3_net_id sig, bool pol) {
    return net_is(ds, sig, pol ? ODIN3_CONST_1 : ODIN3_CONST_0) ||
           (!ds->oc->opts->keepdc && net_is(ds, sig, ODIN3_CONST_X));
}

static bool never_active(const dff_state *ds, odin3_net_id sig, bool pol) {
    return net_is(ds, sig, pol ? ODIN3_CONST_0 : ODIN3_CONST_1);
}

/* --- logging and counting --------------------------------------------------------------------- */

static void note(dff_state *ds, odin3_opt_dff_action action, const char *detail) {
    ds->oc->counts->dff_action[action]++;
    const char *module =
        odin3_strtab_get(odin3_design_strtab(ds->oc->design), odin3_module_name(ds->oc->module));
    odin3_log(ODIN3_LOG_DEBUG, "opt_dff: module '%s': %s%s on %s '%s'", module,
              k_action_text[action], detail, ds->type, ds->label);
}

/* --- loading ---------------------------------------------------------------------------------- */

/* True when the nets of one bit (Q, and D, AD, SET, CLR where the FF has them) are connected. */
static bool bit_connected(const odin3_ffdata *ff, uint32_t bit) {
    bool ok = odin3_net_valid(net_at(&ff->sig_q, bit));
    ok = ok && (!(ff->has_clk || ff->has_gclk) || odin3_net_valid(net_at(&ff->sig_d, bit)));
    ok = ok && (!ff->has_aload || odin3_net_valid(net_at(&ff->sig_ad, bit)));
    return ok && (!ff->has_sr || (odin3_net_valid(net_at(&ff->sig_set, bit)) &&
                                  odin3_net_valid(net_at(&ff->sig_clr, bit))));
}

/* True when every net the model reads is connected. */
static bool all_connected(const odin3_ffdata *ff) {
    bool ok = ff->sig_q.len == ff->width && ff->width > 0;
    for (uint32_t i = 0; ok && i < ff->width; i++) {
        ok = bit_connected(ff, i);
    }
    return ok && (!ff->has_clk || odin3_net_valid(ff->sig_clk)) &&
           (!ff->has_ce || odin3_net_valid(ff->sig_ce)) &&
           (!ff->has_srst || odin3_net_valid(ff->sig_srst)) &&
           (!ff->has_arst || odin3_net_valid(ff->sig_arst)) &&
           (!ff->has_aload || odin3_net_valid(ff->sig_aload));
}

/* True when no two Q bits are on one net. */
static odin3_status q_distinct(const odin3_ffdata *ff, bool *distinct) {
    odin3_vec keys;
    odin3_vec_init(&keys, sizeof(uint64_t));
    odin3_status st = odin3_vec_reserve(&keys, ff->width);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_kit_push_u64(&keys, net_at(&ff->sig_q, i).v);
    }
    odin3_kit_sort_unify(&keys, 0);
    *distinct = st == ODIN3_OK && keys.len == ff->width;
    odin3_vec_free(&keys);
    return st;
}

/* Loads node into ds (Yosys `FfData ff(&initvals, cell)`); *usable = false for an FF this pass
 * leaves alone. */
static odin3_status state_load(dff_state *ds, odin3_node_id node, bool *usable) {
    odin3_ffdata_free(&ds->ff);
    odin3_vec_clear(&ds->pend);
    ds->cell = node;
    ds->changed = ds->gone = ds->prov_ready = false;
    *usable = false;
    odin3_status st = odin3_ff_load(ds->oc, node, &ds->ff);
    if (st != ODIN3_OK) {
        return st;
    }
    ds->width0 = ds->ff.width;
    (void)snprintf(ds->type, sizeof ds->type, "%s",
                   odin3_celltype_get(ds->oc->design, odin3_node_type(ds->oc->module, node))->name);
    odin3_kit_node_label(ds->oc, node, ds->label, sizeof ds->label);
    odin3_kit_begin_rewrite(ds->oc);
    *usable = all_connected(&ds->ff);
    return *usable ? q_distinct(&ds->ff, usable) : ODIN3_OK;
}

/* --- editing the model ------------------------------------------------------------------------ */

/* Yosys `ff = ff.slice(keep); ff.cell = cell`. */
static odin3_status state_keep(dff_state *ds, const uint32_t *keep, uint32_t n) {
    odin3_ffdata sliced;
    odin3_status st = odin3_ff_slice(&ds->ff, (odin3_ff_bits){keep, n}, &sliced);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ffdata_free(&ds->ff);
    ds->ff = sliced;
    ds->ff.node = ds->cell;
    return ODIN3_OK;
}

/* Yosys `module->connect(sig_q[i], src)`, made when the FF is rebuilt without bit i. */
static odin3_status tie_bit(dff_state *ds, uint32_t idx, odin3_net_id src) {
    odin3_net_pair *slot = odin3_vec_push(&ds->pend);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_net_pair){src, net_at(&ds->ff.sig_q, idx)};
    return ODIN3_OK;
}

static odin3_status const_net(dff_state *ds, odin3_bit bit, odin3_net_id *out) {
    return odin3_opt_const_net(ds->oc, bit, out);
}

/* Yosys `ff.remove()` with every remaining bit tied to src[i] (NULL: all are in pend already): the
 * node goes; every pending bit is tied. */
static odin3_status remove_all(dff_state *ds, const odin3_net_id *src) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && src != NULL && i < ds->ff.width; i++) {
        st = tie_bit(ds, i, src[i]);
    }
    odin3_ffdata empty;
    st = st == ODIN3_OK ? odin3_ff_slice(&ds->ff, (odin3_ff_bits){NULL, 0}, &empty) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    empty.node = ds->cell;
    odin3_node_id none = {0};
    st = odin3_ff_emit_shrunk(ds->oc, &empty, ds->pend.data, (uint32_t)ds->pend.len, &none);
    odin3_ffdata_free(&empty);
    ds->gone = ds->gone || st == ODIN3_OK;
    return st;
}

/* The constant nets of vals[0 ..) in nets (replaced). */
static odin3_status set_consts(dff_state *ds, odin3_vec *nets, const odin3_vec *vals) {
    odin3_vec_clear(nets);
    odin3_status st = odin3_vec_reserve(nets, vals->len);
    for (uint32_t i = 0; st == ODIN3_OK && i < vals->len; i++) {
        odin3_net_id net = {0};
        st = const_net(ds, bit_at(vals, i), &net);
        st = st == ODIN3_OK ? odin3_kit_push_net(nets, net) : st;
    }
    return st;
}

static odin3_status bit_push(odin3_vec *vec, odin3_bit bit) {
    odin3_bit *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = bit;
    return ODIN3_OK;
}

/* vec = n copies of bit. */
static odin3_status bits_fill(odin3_vec *vec, odin3_bit bit, uint32_t n) {
    odin3_vec_clear(vec);
    odin3_status st = odin3_vec_reserve(vec, n);
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = bit_push(vec, bit);
    }
    return st;
}

/* The odin3_bit a constant net carries (0, 1, x or z). */
static odin3_bit const_bit(odin3_const val) {
    return (odin3_bit)(val - ODIN3_CONST_0);
}

/* --- gates (the rules' helper cells) ---------------------------------------------------------- */

/* A DERIVED record of the FF's node for the helper cells (one per visit). */
static odin3_status state_prov(dff_state *ds, odin3_prov_id *out) {
    odin3_status st = ODIN3_OK;
    if (!ds->prov_ready) {
        st = odin3_kit_prov(ds->oc, &ds->cell, 1, &ds->prov);
        ds->prov_ready = st == ODIN3_OK;
    }
    *out = ds->prov;
    return st;
}

/* The value of parameter `name` of a word gate on `width` bits (A_WIDTH, B_WIDTH, Y_WIDTH, WIDTH;
 * the *_SIGNED flags 0). */
static odin3_value gate_param(const char *name, uint32_t width) {
    bool wide = strstr(name, "WIDTH") != NULL;
    return odin3_value_int(wide ? (int64_t)width : 0);
}

/*
 * A word gate type_name ($not, $and, $or, $mux) over ins (inputs in port order), width bits wide
 * (a $mux's S is one bit), on fresh output nets, which are appended to outs (cleared first).
 */
static odin3_status new_gate(dff_state *ds, const char *type_name, const odin3_netvec *ins,
                             uint32_t width, odin3_vec *outs) {
    odin3_celltype_id type = odin3_kit_type_named(ds->oc->design, type_name);
    const odin3_celltype_def *def = odin3_celltype_get(ds->oc->design, type);
    if (def == NULL || def->n_ports > MAX_GATE_PORTS || def->n_params > MAX_GATE_PARAMS) {
        odin3_log(ODIN3_LOG_ERROR, "opt_dff: cell type '%s' is not registered", type_name);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_value params[MAX_GATE_PARAMS];
    odin3_netvec ports[MAX_GATE_PORTS];
    for (uint32_t k = 0; k < def->n_params; k++) {
        params[k] = gate_param(def->params[k].name, width);
    }
    uint32_t input = 0;
    for (uint32_t k = 0; k < def->n_ports; k++) {
        ports[k] = def->ports[k].dir == ODIN3_DIR_IN ? ins[input++] : (odin3_netvec){NULL, 0};
    }
    odin3_kit_cell cell = {type, params, def->n_params, ports, {0}};
    odin3_status st = state_prov(ds, &cell.prov);
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_kit_new_cell(ds->oc, &cell, &node) : st;
    odin3_vec_clear(outs);
    odin3_pinslice out = {0};
    out = st == ODIN3_OK ? odin3_node_port(ds->oc->module, node, def->n_ports - 1U) : out;
    for (uint32_t k = 0; st == ODIN3_OK && k < out.count; k++) {
        st = odin3_kit_push_net(outs,
                                odin3_pin_net(ds->oc->module, (odin3_pin_id){out.first.v + k}));
    }
    return st;
}

static odin3_netvec as_netvec(const odin3_vec *vec) {
    return (odin3_netvec){vec->data, (uint32_t)vec->len};
}

/* dst = ~src (a word $not over the vector). */
static odin3_status gate_not(dff_state *ds, const odin3_vec *src, odin3_vec *dst) {
    const odin3_netvec ins[1] = {as_netvec(src)};
    return new_gate(ds, "$not", ins, (uint32_t)src->len, dst);
}

/* dst = lhs op rhs over two vectors of one width ("$and" or "$or"). */
static odin3_status gate_bin(dff_state *ds, const char *op, const odin3_vec *lhs,
                             const odin3_vec *rhs, odin3_vec *dst) {
    const odin3_netvec ins[2] = {as_netvec(lhs), as_netvec(rhs)};
    return new_gate(ds, op, ins, (uint32_t)lhs->len, dst);
}

/* dst = sel ? rhs : lhs. */
static odin3_status gate_mux(dff_state *ds, const odin3_vec *lhs, const odin3_vec *rhs,
                             odin3_net_id sel, odin3_vec *dst) {
    const odin3_netvec ins[3] = {as_netvec(lhs), as_netvec(rhs), {&sel, 1}};
    return new_gate(ds, "$mux", ins, (uint32_t)lhs->len, dst);
}

/* ~net for one net (the one-bit $not of Yosys's Not on a SigBit). */
static odin3_status not_net(dff_state *ds, odin3_net_id net, odin3_net_id *out) {
    odin3_vec one;
    odin3_vec res;
    odin3_vec_init(&one, sizeof(odin3_net_id));
    odin3_vec_init(&res, sizeof(odin3_net_id));
    odin3_status st = odin3_kit_push_net(&one, net);
    st = st == ODIN3_OK ? gate_not(ds, &one, &res) : st;
    *out = st == ODIN3_OK ? net_at(&res, 0) : (odin3_net_id){0};
    odin3_vec_free(&one);
    odin3_vec_free(&res);
    return st;
}

/* lhs & rhs for two single nets. */
static odin3_status and_nets(dff_state *ds, odin3_net_id lhs, odin3_net_id rhs, odin3_net_id *out) {
    odin3_vec va;
    odin3_vec vb;
    odin3_vec res;
    odin3_vec_init(&va, sizeof(odin3_net_id));
    odin3_vec_init(&vb, sizeof(odin3_net_id));
    odin3_vec_init(&res, sizeof(odin3_net_id));
    odin3_status st = odin3_kit_push_net(&va, lhs);
    st = st == ODIN3_OK ? odin3_kit_push_net(&vb, rhs) : st;
    st = st == ODIN3_OK ? gate_bin(ds, "$and", &va, &vb, &res) : st;
    *out = st == ODIN3_OK ? net_at(&res, 0) : (odin3_net_id){0};
    odin3_vec_free(&va);
    odin3_vec_free(&vb);
    odin3_vec_free(&res);
    return st;
}

/* --- rule 2: SET and CLR ---------------------------------------------------------------------- */

/* One bit of rule 2's first part: tied (always-active CLR or SET: *removed) or kept. */
static odin3_status sr_bit(dff_state *ds, uint32_t bit, odin3_vec *keep, bool *removed) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_net_id clr = net_at(&ff->sig_clr, bit);
    odin3_net_id set = net_at(&ff->sig_set, bit);
    odin3_net_id src = clr;
    odin3_status st = ODIN3_OK;
    if (always_active(ds, clr, ff->pol_clr)) {
        st = const_net(ds, ODIN3_BIT_0, &src);
        note(ds, ODIN3_OPT_DFF_ALWAYS_CLR, "");
    } else if (always_active(ds, set, ff->pol_set)) {
        st = ff->pol_clr ? not_net(ds, clr, &src) : ODIN3_OK;
        note(ds, ODIN3_OPT_DFF_ALWAYS_SET, "");
    } else {
        return odin3_kit_push_u32(keep, bit);
    }
    *removed = true;
    return st == ODIN3_OK ? tie_bit(ds, bit, src) : st;
}

/* The bits of the model that stay (keep) and those tied (pend), for always-active SET/CLR. */
static odin3_status sr_always_bits(dff_state *ds, odin3_vec *keep, bool *removed) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ds->ff.width; i++) {
        st = sr_bit(ds, i, keep, removed);
    }
    return st;
}

/* Never-active CLR (SET uniform) or SET (CLR uniform): the other side becomes an async reset. */
static odin3_status sr_to_arst(dff_state *ds, odin3_net_id sig, bool pol, odin3_bit val,
                               odin3_opt_dff_action action) {
    odin3_ffdata *ff = &ds->ff;
    ff->has_sr = false;
    ff->has_arst = true;
    ff->pol_arst = pol;
    ff->sig_arst = sig;
    odin3_vec_clear(&ff->sig_set);
    odin3_vec_clear(&ff->sig_clr);
    ds->changed = true;
    note(ds, action, "");
    return bits_fill(&ff->val_arst, val, ff->width);
}

static bool uniform(const odin3_vec *vec) {
    for (uint32_t i = 1; i < vec->len; i++) {
        if (net_at(vec, i).v != net_at(vec, 0).v) {
            return false;
        }
    }
    return true;
}

/* CLR and SET of one polarity on complementary bits of one signal: an async reset with the value
 * each bit selects (vals is filled; *ok = false when some bit is neither). */
static odin3_status sr_complement_vals(const dff_state *ds, odin3_net_id arst, odin3_vec *vals,
                                       bool *ok) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_const neutral = ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1;
    odin3_status st = odin3_vec_reserve(vals, ff->width);
    *ok = true;
    for (uint32_t i = 0; st == ODIN3_OK && *ok && i < ff->width; i++) {
        odin3_net_id clr = net_at(&ff->sig_clr, i);
        odin3_net_id set = net_at(&ff->sig_set, i);
        if (clr.v == arst.v && net_is(ds, set, neutral)) {
            st = bit_push(vals, ODIN3_BIT_0);
        } else if (set.v == arst.v && net_is(ds, clr, neutral)) {
            st = bit_push(vals, ODIN3_BIT_1);
        } else {
            *ok = false;
        }
    }
    return st;
}

static odin3_status sr_complement(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    odin3_const neutral = ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1;
    odin3_net_id arst = net_is(ds, net_at(&ff->sig_clr, 0), neutral) ? net_at(&ff->sig_set, 0)
                                                                     : net_at(&ff->sig_clr, 0);
    odin3_vec vals;
    odin3_vec_init(&vals, sizeof(odin3_bit));
    bool ok = false;
    odin3_status st = sr_complement_vals(ds, arst, &vals, &ok);
    if (st == ODIN3_OK && ok) {
        odin3_vec_free(&ff->val_arst);
        ff->val_arst = vals;
        odin3_vec_init(&vals, sizeof(odin3_bit));
        ff->has_sr = false;
        ff->has_arst = true;
        ff->sig_arst = arst;
        ff->pol_arst = ff->pol_clr;
        odin3_vec_clear(&ff->sig_set);
        odin3_vec_clear(&ff->sig_clr);
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_CLR_SET_TO_ARST, "");
    }
    odin3_vec_free(&vals);
    return st;
}

static odin3_status sr_conversions(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (vec_is(ds, &ff->sig_clr, ff->pol_clr ? ODIN3_CONST_0 : ODIN3_CONST_1)) {
        return uniform(&ff->sig_set) ? sr_to_arst(ds, net_at(&ff->sig_set, 0), ff->pol_set,
                                                  ODIN3_BIT_1, ODIN3_OPT_DFF_NEVER_CLR)
                                     : ODIN3_OK;
    }
    if (vec_is(ds, &ff->sig_set, ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1)) {
        return uniform(&ff->sig_clr) ? sr_to_arst(ds, net_at(&ff->sig_clr, 0), ff->pol_clr,
                                                  ODIN3_BIT_0, ODIN3_OPT_DFF_NEVER_SET)
                                     : ODIN3_OK;
    }
    return ff->pol_clr == ff->pol_set ? sr_complement(ds) : ODIN3_OK;
}

static odin3_status rule_sr(dff_state *ds) {
    if (!ds->ff.has_sr) {
        return ODIN3_OK;
    }
    odin3_vec keep;
    odin3_vec_init(&keep, sizeof(uint32_t));
    bool removed = false;
    odin3_status st = sr_always_bits(ds, &keep, &removed);
    if (st == ODIN3_OK && removed) {
        ds->changed = true;
        st = keep.len == 0 ? remove_all(ds, NULL) : state_keep(ds, keep.data, (uint32_t)keep.len);
    }
    odin3_vec_free(&keep);
    return st == ODIN3_OK && !ds->gone ? sr_conversions(ds) : st;
}

/* --- rule 3: ALOAD ---------------------------------------------------------------------------- */

/* Always-active ALOAD on a FF with SET/CLR: q = (AD | SET) & ~CLR, each in its polarity. */
static odin3_status aload_sr_circuit(dff_state *ds, odin3_vec *qout) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_vec inv;
    odin3_vec both;
    odin3_vec_init(&inv, sizeof(odin3_net_id));
    odin3_vec_init(&both, sizeof(odin3_net_id));
    const odin3_vec *set = &ff->sig_set;
    odin3_status st = ff->pol_set ? ODIN3_OK : gate_not(ds, set, &inv);
    set = ff->pol_set ? set : &inv;
    st = st == ODIN3_OK ? gate_bin(ds, "$or", &ff->sig_ad, set, &both) : st;
    const odin3_vec *clr = &ff->sig_clr;
    odin3_vec nclr;
    odin3_vec_init(&nclr, sizeof(odin3_net_id));
    st = st == ODIN3_OK && ff->pol_clr ? gate_not(ds, clr, &nclr) : st;
    clr = ff->pol_clr ? &nclr : clr;
    st = st == ODIN3_OK ? gate_bin(ds, "$and", &both, clr, qout) : st;
    odin3_vec_free(&inv);
    odin3_vec_free(&both);
    odin3_vec_free(&nclr);
    return st;
}

/* Always-active ALOAD on a FF with ARST: q = ARST ? val_arst : AD (in the reset's polarity). */
static odin3_status aload_arst_circuit(dff_state *ds, odin3_vec *qout) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_vec rst;
    odin3_vec_init(&rst, sizeof(odin3_net_id));
    odin3_status st = set_consts(ds, &rst, &ff->val_arst);
    if (st == ODIN3_OK && ff->pol_arst) {
        st = gate_mux(ds, &ff->sig_ad, &rst, ff->sig_arst, qout);
    } else if (st == ODIN3_OK) {
        st = gate_mux(ds, &rst, &ff->sig_ad, ff->sig_arst, qout);
    }
    odin3_vec_free(&rst);
    return st;
}

/* Always-active ALOAD: the combinational circuit of Q (Yosys builds it after ff.remove()); the
 * outputs are made first, the FF goes after, tied to them. */
static odin3_status aload_always(dff_state *ds) {
    const odin3_ffdata *ff = &ds->ff;
    note(ds, ODIN3_OPT_DFF_ALWAYS_ALOAD, "");
    odin3_vec qout;
    odin3_vec_init(&qout, sizeof(odin3_net_id));
    odin3_status st = ODIN3_OK;
    if (ff->has_sr) {
        st = aload_sr_circuit(ds, &qout);
    } else if (ff->has_arst) {
        st = aload_arst_circuit(ds, &qout);
    }
    const odin3_vec *src = ff->has_sr || ff->has_arst ? &qout : &ff->sig_ad;
    st = st == ODIN3_OK ? remove_all(ds, src->data) : st;
    odin3_vec_free(&qout);
    return st;
}

/* Async load of a constant on a FF with no ARST or SR: the load is an async reset. */
static odin3_status aload_to_arst(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    odin3_vec_clear(&ff->val_arst);
    odin3_status st = odin3_vec_reserve(&ff->val_arst, ff->width);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = bit_push(&ff->val_arst, const_bit(net_const(ds, net_at(&ff->sig_ad, i))));
    }
    if (st != ODIN3_OK) {
        return st;
    }
    ff->has_arst = true;
    ff->has_aload = false;
    ff->sig_arst = ff->sig_aload;
    ff->pol_arst = ff->pol_aload;
    ff->sig_aload = (odin3_net_id){0};
    odin3_vec_clear(&ff->sig_ad);
    ds->changed = true;
    note(ds, ODIN3_OPT_DFF_ALOAD_TO_ARST, "");
    return ODIN3_OK;
}

static odin3_status rule_aload(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_aload) {
        return ODIN3_OK;
    }
    if (never_active(ds, ff->sig_aload, ff->pol_aload) ||
        (!ds->oc->opts->keepdc && net_is(ds, ff->sig_aload, ODIN3_CONST_X))) {
        ff->has_aload = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_NEVER_ALOAD, "");
        return ODIN3_OK;
    }
    if (net_is(ds, ff->sig_aload, ff->pol_aload ? ODIN3_CONST_1 : ODIN3_CONST_0)) {
        return ff->is_fine && (ff->has_sr || ff->has_arst) ? ODIN3_OK : aload_always(ds);
    }
    if (vec_all_const(ds, &ff->sig_ad) && !ff->has_arst && !ff->has_sr) {
        return aload_to_arst(ds);
    }
    return ODIN3_OK;
}

/* --- rule 4: ARST ----------------------------------------------------------------------------- */

static odin3_status rule_arst(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_arst) {
        return ODIN3_OK;
    }
    if (never_active(ds, ff->sig_arst, ff->pol_arst)) {
        ff->has_arst = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_NEVER_ARST, "");
        return ODIN3_OK;
    }
    if (!always_active(ds, ff->sig_arst, ff->pol_arst)) {
        return ODIN3_OK;
    }
    note(ds, ODIN3_OPT_DFF_ALWAYS_ARST, "");
    odin3_vec src;
    odin3_vec_init(&src, sizeof(odin3_net_id));
    odin3_status st = set_consts(ds, &src, &ff->val_arst);
    st = st == ODIN3_OK ? remove_all(ds, src.data) : st;
    odin3_vec_free(&src);
    return st;
}

/* --- rule 5: SRST ----------------------------------------------------------------------------- */

static odin3_status rule_srst(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_srst) {
        return ODIN3_OK;
    }
    if (never_active(ds, ff->sig_srst, ff->pol_srst)) {
        ff->has_srst = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_NEVER_SRST, "");
        return ODIN3_OK;
    }
    if (!always_active(ds, ff->sig_srst, ff->pol_srst)) {
        return ODIN3_OK;
    }
    note(ds, ODIN3_OPT_DFF_ALWAYS_SRST, "");
    ff->has_srst = false;
    ff->has_ce = ff->has_ce && ff->ce_over_srst;
    ds->changed = true;
    return set_consts(ds, &ff->sig_d, &ff->val_srst);
}

/* --- rule 6: EN ------------------------------------------------------------------------------- */

static odin3_status en_never_active(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    ds->changed = true;
    if (ff->has_srst && !ff->ce_over_srst) { /* the sync reset becomes the enable */
        note(ds, ODIN3_OPT_DFF_NEVER_EN, "");
        ff->pol_ce = ff->pol_srst;
        ff->sig_ce = ff->sig_srst;
        ff->has_srst = false;
        return set_consts(ds, &ff->sig_d, &ff->val_srst);
    }
    if (!ds->oc->opts->keepdc || init_defined(ds)) { /* the D path is useless */
        note(ds, ODIN3_OPT_DFF_NEVER_EN, "");
        ff->has_ce = ff->has_clk = ff->has_srst = false;
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK; /* keep the undefined init: D = Q, no line */
    odin3_vec_clear(&ff->sig_d);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_kit_push_net(&ff->sig_d, net_at(&ff->sig_q, i));
    }
    ff->has_ce = ff->has_srst = false;
    return st;
}

static odin3_status rule_en(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_ce) {
        return ODIN3_OK;
    }
    if (net_is(ds, ff->sig_ce, ff->pol_ce ? ODIN3_CONST_0 : ODIN3_CONST_1) ||
        (!ds->oc->opts->keepdc && net_is(ds, ff->sig_ce, ODIN3_CONST_X))) {
        return en_never_active(ds);
    }
    if (net_is(ds, ff->sig_ce, ff->pol_ce ? ODIN3_CONST_1 : ODIN3_CONST_0)) {
        ff->has_ce = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_ALWAYS_EN, "");
    }
    return ODIN3_OK;
}

/* --- rule 7: constant CLK --------------------------------------------------------------------- */

static odin3_status rule_clk(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!ff->has_clk || net_const(ds, ff->sig_clk) == ODIN3_CONST_NONE) {
        return ODIN3_OK;
    }
    if (!ds->oc->opts->keepdc || init_defined(ds)) { /* the D path is useless */
        ff->has_ce = ff->has_clk = ff->has_srst = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_CONST_CLK, "");
        return ODIN3_OK;
    }
    if (!ff->has_ce && !ff->has_srst && vec_equal(&ff->sig_d, &ff->sig_q)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK; /* keep the undefined init: D = Q */
    odin3_vec_clear(&ff->sig_d);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        st = odin3_kit_push_net(&ff->sig_d, net_at(&ff->sig_q, i));
    }
    ff->has_ce = ff->has_srst = false;
    ds->changed = true;
    return st;
}

/* --- rule 8: D = Q ---------------------------------------------------------------------------- */

/* D = Q on a FF with a sync reset: EN := SRST (anded with an EN that the reset is over), D :=
 * the reset value. */
static odin3_status dq_with_srst(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    odin3_status st = ODIN3_OK;
    if (ff->has_ce && ff->ce_over_srst) {
        odin3_net_id ce = ff->sig_ce;
        odin3_net_id srst = ff->sig_srst;
        st = !ff->pol_ce ? not_net(ds, ce, &ce) : st;
        st = st == ODIN3_OK && !ff->pol_srst ? not_net(ds, srst, &srst) : st;
        st = st == ODIN3_OK ? and_nets(ds, ce, srst, &ff->sig_ce) : st;
        ff->pol_ce = true;
    } else {
        ff->pol_ce = ff->pol_srst;
        ff->sig_ce = ff->sig_srst;
    }
    ff->has_ce = true;
    ff->has_srst = false;
    ds->changed = true;
    return st == ODIN3_OK ? set_consts(ds, &ff->sig_d, &ff->val_srst) : st;
}

static odin3_status rule_dq(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (!(ff->has_clk || ff->has_gclk) || !vec_equal(&ff->sig_d, &ff->sig_q)) {
        return ODIN3_OK;
    }
    if (ff->has_clk && ff->has_srst) {
        note(ds, ODIN3_OPT_DFF_D_EQ_Q, "");
        return dq_with_srst(ds);
    }
    if (!ds->oc->opts->keepdc || init_defined(ds)) {
        ff->has_gclk = ff->has_clk = ff->has_ce = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_D_EQ_Q, "");
    }
    return ODIN3_OK;
}

/* --- rule 9: AD = Q --------------------------------------------------------------------------- */

static odin3_status rule_adq(dff_state *ds) {
    odin3_ffdata *ff = &ds->ff;
    if (ff->has_aload && !ff->has_clk && vec_equal(&ff->sig_ad, &ff->sig_q)) {
        ff->has_aload = false;
        ds->changed = true;
        note(ds, ODIN3_OPT_DFF_AD_EQ_Q, "");
    }
    return ODIN3_OK;
}

/* --- one FF ----------------------------------------------------------------------------------- */

typedef odin3_status (*rule_fn)(dff_state *ds);

/* The rules of §9.3 in order (Task 12 adds 10 and 11 after the last). */
static const rule_fn k_rules[] = {rule_sr, rule_aload, rule_arst, rule_srst,
                                  rule_en, rule_clk,   rule_dq,   rule_adq};

/* Rule 12: a changed FF is re-emitted (Yosys `ff.emit()`), on the bits left of its node's Q. */
static odin3_status commit(dff_state *ds) {
    odin3_node_id out = {0};
    if (ds->gone || !ds->changed) {
        return ODIN3_OK;
    }
    ds->ff.node = ds->cell;
    return ds->ff.width == ds->width0
               ? odin3_ff_emit(ds->oc, &ds->ff, &out)
               : odin3_ff_emit_shrunk(ds->oc, &ds->ff, ds->pend.data, (uint32_t)ds->pend.len, &out);
}

static odin3_status visit_ff(dff_state *ds, odin3_node_id node) {
    bool usable = false;
    odin3_status st = state_load(ds, node, &usable);
    for (size_t i = 0;
         st == ODIN3_OK && usable && !ds->gone && i < sizeof k_rules / sizeof k_rules[0]; i++) {
        st = k_rules[i](ds);
    }
    st = st == ODIN3_OK && usable ? commit(ds) : st;
    ds->n_changed += st == ODIN3_OK && usable && (ds->gone || ds->changed) ? 1U : 0U;
    return st;
}

/* --- constbits -------------------------------------------------------------------------------- */

/* Yosys combine_const on two states (odin3_bit values, or BIT_CONFLICT). */
static uint32_t combine_const(const dff_state *ds, uint32_t lhs, uint32_t rhs) {
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
static uint32_t sr_stuck(const dff_state *ds, stuck_ref ref) {
    const odin3_ffdata *ff = &ds->ff;
    uint32_t val = ref.val;
    bool clr_off =
        net_is(ds, net_at(&ff->sig_clr, ref.bit), ff->pol_clr ? ODIN3_CONST_0 : ODIN3_CONST_1);
    bool set_off =
        net_is(ds, net_at(&ff->sig_set, ref.bit), ff->pol_set ? ODIN3_CONST_0 : ODIN3_CONST_1);
    val = clr_off ? val : combine_const(ds, val, ODIN3_BIT_0);
    return set_off ? val : combine_const(ds, val, ODIN3_BIT_1);
}

/* val combined with the constant that net carries; BIT_CONFLICT when it is no constant. */
static uint32_t with_const(const dff_state *ds, uint32_t val, odin3_net_id net) {
    odin3_const cval = net_const(ds, net);
    return cval == ODIN3_CONST_NONE ? BIT_CONFLICT
                                    : combine_const(ds, val, (uint32_t)const_bit(cval));
}

/* The value bit `bit` of the FF is stuck at, BIT_CONFLICT when there is none. */
static uint32_t stuck_value(const dff_state *ds, uint32_t bit) {
    const odin3_ffdata *ff = &ds->ff;
    uint32_t val = (uint32_t)bit_at(&ff->val_init, bit);
    val = ff->has_arst ? combine_const(ds, val, (uint32_t)bit_at(&ff->val_arst, bit)) : val;
    val = ff->has_srst ? combine_const(ds, val, (uint32_t)bit_at(&ff->val_srst, bit)) : val;
    val = ff->has_sr ? sr_stuck(ds, (stuck_ref){bit, val}) : val;
    if (val != BIT_CONFLICT && (ff->has_clk || ff->has_gclk)) {
        val = with_const(ds, val, net_at(&ff->sig_d, bit));
    }
    if (val != BIT_CONFLICT && ff->has_aload) {
        val = with_const(ds, val, net_at(&ff->sig_ad, bit));
    }
    return val;
}

static void note_const_bit(dff_state *ds, odin3_bit val, uint32_t at_bit) {
    static const odin3_opt_dff_action k_action[] = {
        ODIN3_OPT_DFF_CONST_BIT_0, ODIN3_OPT_DFF_CONST_BIT_1, ODIN3_OPT_DFF_CONST_BIT_X,
        ODIN3_OPT_DFF_CONST_BIT_Z};
    char detail[DETAIL_LEN];
    (void)snprintf(detail, sizeof detail, " at bit %" PRIu32, at_bit);
    /* "setting constant <v>" is the action's text; the bit follows it (spec §16) */
    ds->oc->counts->dff_action[k_action[val]]++;
    const char *module =
        odin3_strtab_get(odin3_design_strtab(ds->oc->design), odin3_module_name(ds->oc->module));
    odin3_log(ODIN3_LOG_DEBUG, "opt_dff: module '%s': %s%s on %s '%s'", module,
              k_action_text[k_action[val]], detail, ds->type, ds->label);
}

/* Bit i: tied to its constant (removed++), or kept. */
static odin3_status constbit(dff_state *ds, uint32_t bit, odin3_vec *keep, uint32_t *removed) {
    uint32_t val = stuck_value(ds, bit);
    if (val == BIT_CONFLICT) {
        return odin3_kit_push_u32(keep, bit);
    }
    odin3_net_id net = {0};
    odin3_status st = const_net(ds, (odin3_bit)val, &net);
    st = st == ODIN3_OK ? tie_bit(ds, bit, net) : st;
    note_const_bit(ds, (odin3_bit)val, bit);
    *removed += 1U;
    return st;
}

/* Ties the constant bits of the loaded FF and rebuilds it without them. */
static odin3_status constbits_ff(dff_state *ds) {
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
        st = keep.len == 0 ? remove_all(ds, NULL) : state_keep(ds, keep.data, (uint32_t)keep.len);
        st = st == ODIN3_OK ? commit(ds) : st;
    }
    odin3_vec_free(&keep);
    return st;
}

static odin3_status visit_constbits(dff_state *ds, odin3_node_id node) {
    bool usable = false;
    odin3_status st = state_load(ds, node, &usable);
    return st == ODIN3_OK && usable ? constbits_ff(ds) : st;
}

/* --- the module and the pass ------------------------------------------------------------------ */

/* The FFs of the module in ID order (elements uint32_t node IDs). */
static odin3_status collect_ffs(const odin3_opt_ctx *oc, odin3_vec *list) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(oc->module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(oc->module, node) &&
            odin3_ff_is_ff(oc, odin3_node_type(oc->module, node))) {
            st = odin3_kit_push_u32(list, id);
        }
    }
    return st;
}

static odin3_status module_rules(dff_state *ds) {
    odin3_vec list;
    odin3_vec_init(&list, sizeof(uint32_t));
    odin3_status st = collect_ffs(ds->oc, &list);
    /* the list grows when Task 12's groups push the FFs they make */
    for (size_t i = 0; st == ODIN3_OK && i < list.len; i++) {
        st = visit_ff(ds, (odin3_node_id){*(const uint32_t *)odin3_vec_cat(&list, i)});
    }
    odin3_vec_free(&list);
    return st;
}

static odin3_status module_constbits(dff_state *ds) {
    odin3_status st = ODIN3_OK;
    const uint32_t end = odin3_module_node_end(ds->oc->module);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(ds->oc->module, node) &&
            odin3_ff_is_ff(ds->oc, odin3_node_type(ds->oc->module, node))) {
            st = visit_constbits(ds, node);
        }
    }
    return st;
}

typedef struct dff_totals {
    uint32_t changed, bits;
} dff_totals;

static odin3_status dff_module(odin3_opt_ctx *oc, dff_totals *totals) {
    dff_state ds;
    state_init(&ds, oc);
    odin3_status st = odin3_opt_canon_consts(oc);
    oc->defer_ties = true;
    st = st == ODIN3_OK ? module_rules(&ds) : st;
    st = st == ODIN3_OK ? module_constbits(&ds) : st;
    /* the ties are made even after a failure: the FFs that went are tied as far as they can be; a
     * tie that cannot be made leaves its Q net undriven (the IR valid, the design to be dropped) */
    odin3_status flushed = odin3_kit_half_done(oc, odin3_kit_flush_ties(oc));
    oc->defer_ties = false;
    totals->changed += ds.n_changed;
    totals->bits += ds.n_bits;
    state_free(&ds);
    return st != ODIN3_OK ? st : flushed;
}

odin3_status odin3_opt_dff_run(odin3_pass_ctx *ctx, odin3_design *design,
                               const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_dff: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    dff_totals totals = {0, 0};
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, odin3_module_get(design, (odin3_module_id){id}), opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        st = dff_module(&oc, &totals);
        odin3_opt_ctx_free(&oc);
    }
    counts->dff_changed += totals.changed;
    counts->dff_const_bits += totals.bits;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "opt_dff: %" PRIu32 " flip-flops changed, %" PRIu32 " constant bits",
                  totals.changed, totals.bits);
    }
    return st;
}
