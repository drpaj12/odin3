/*
 * ff.c — the flip-flop model (2F spec OP-9 §9.1, §9.4): Yosys 0.55 kernel/ff.cc FfData's
 * constructor, slice and emit over Odin III's FF cells, through the kit (OP-2) and ffinit.h.
 */
#include "passes/opt/ff.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/ffinit.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- the FF types ----------------------------------------------------------------------------- */

/* What a type has (Yosys FfData's constructor, per type). */
enum {
    FT_CLK = 1U << 0U,
    FT_CE = 1U << 1U,
    FT_SRST = 1U << 2U,
    FT_ARST = 1U << 3U,
    FT_ALOAD = 1U << 4U,
    FT_SR = 1U << 5U,
    FT_CE_OVER_SRST = 1U << 6U,
    FT_GCLK = 1U << 7U,
    FT_FINE = 1U << 8U,
    FT_POS = 1U << 9U /* a fine type's clock or enable is active high */
};

typedef struct ff_kind {
    const char *name;
    unsigned features;
} ff_kind;

static const ff_kind k_kinds[] = {
    {"$dff", FT_CLK},
    {"$dffe", FT_CLK | FT_CE},
    {"$adff", FT_CLK | FT_ARST},
    {"$adffe", FT_CLK | FT_ARST | FT_CE},
    {"$aldff", FT_CLK | FT_ALOAD},
    {"$aldffe", FT_CLK | FT_ALOAD | FT_CE},
    {"$sdff", FT_CLK | FT_SRST},
    {"$sdffe", FT_CLK | FT_SRST | FT_CE},
    {"$sdffce", FT_CLK | FT_SRST | FT_CE | FT_CE_OVER_SRST},
    {"$dffsr", FT_CLK | FT_SR},
    {"$dffsre", FT_CLK | FT_SR | FT_CE},
    {"$dlatch", FT_ALOAD},
    {"$adlatch", FT_ALOAD | FT_ARST},
    {"$dlatchsr", FT_ALOAD | FT_SR},
    {"$sr", FT_SR},
    {"$_DFF_P_", FT_FINE | FT_CLK | FT_POS},
    {"$_DFF_N_", FT_FINE | FT_CLK},
    {"$_FF_", FT_FINE | FT_GCLK},
    {"$_DLATCH_P_", FT_FINE | FT_ALOAD | FT_POS},
    {"$_DLATCH_N_", FT_FINE | FT_ALOAD},
};

enum {
    N_KINDS = sizeof k_kinds / sizeof k_kinds[0],
    FF_MAX_PARAMS = 8, /* the most any FF type has is 5 */
    FF_MAX_PORTS = 8,  /* ... and 6 ports */
    INIT_DC = 2,       /* a fine FF's INIT: BLIF .latch's don't care */
    INIT_UNKNOWN = 3   /* ... and unknown (the default) */
};

static const ff_kind *kind_of(const odin3_design *design, odin3_celltype_id type) {
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    for (uint32_t i = 0; def != NULL && i < N_KINDS; i++) {
        if (strcmp(def->name, k_kinds[i].name) == 0) {
            return &k_kinds[i];
        }
    }
    return NULL;
}

static odin3_celltype_id type_named(const odin3_design *design, const char *name) {
    uint32_t str = 0;
    odin3_celltype_id id = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) ||
        !odin3_celltype_find(design, str, &id)) {
        return (odin3_celltype_id){0};
    }
    return id;
}

static uint32_t param_named(const odin3_celltype_def *def, const char *name) {
    uint32_t i = 0;
    while (i < def->n_params && strcmp(def->params[i].name, name) != 0) {
        i++;
    }
    return i;
}

bool odin3_ff_is_ff(const odin3_opt_ctx *oc, odin3_celltype_id type) {
    return oc != NULL && kind_of(oc->design, type) != NULL;
}

/* --- the model's slots, by port and parameter name ------------------------------------------- */

static bool is_latch(const odin3_ffdata *ff) {
    return ff->has_aload && !ff->has_clk;
}

/* The one-net slot of port name (a latch's EN is its load), NULL for a vector port. */
static odin3_net_id *net_slot(odin3_ffdata *ff, const char *name) {
    if (strcmp(name, "CLK") == 0 || strcmp(name, "C") == 0) {
        return &ff->sig_clk;
    }
    if (strcmp(name, "EN") == 0 || strcmp(name, "E") == 0) {
        return is_latch(ff) ? &ff->sig_aload : &ff->sig_ce;
    }
    if (strcmp(name, "SRST") == 0) {
        return &ff->sig_srst;
    }
    if (strcmp(name, "ARST") == 0) {
        return &ff->sig_arst;
    }
    return strcmp(name, "ALOAD") == 0 ? &ff->sig_aload : NULL;
}

/* The vector slot of port name (a latch's D is its AD), NULL for none. */
static odin3_vec *vec_slot(odin3_ffdata *ff, const char *name) {
    if (strcmp(name, "AD") == 0 || (strcmp(name, "D") == 0 && is_latch(ff))) {
        return &ff->sig_ad;
    }
    if (strcmp(name, "D") == 0) {
        return &ff->sig_d;
    }
    if (strcmp(name, "SET") == 0) {
        return &ff->sig_set;
    }
    if (strcmp(name, "CLR") == 0) {
        return &ff->sig_clr;
    }
    return strcmp(name, "Q") == 0 ? &ff->sig_q : NULL;
}

/* The polarity of parameter name (a latch's EN_POLARITY is its load's), NULL for none. */
static bool *pol_slot(odin3_ffdata *ff, const char *name) {
    static const char *const k_names[] = {"CLK_POLARITY",   "SRST_POLARITY", "ARST_POLARITY",
                                          "ALOAD_POLARITY", "SET_POLARITY",  "CLR_POLARITY"};
    bool *const slots[] = {&ff->pol_clk,   &ff->pol_srst, &ff->pol_arst,
                           &ff->pol_aload, &ff->pol_set,  &ff->pol_clr};
    if (strcmp(name, "EN_POLARITY") == 0) {
        return is_latch(ff) ? &ff->pol_aload : &ff->pol_ce;
    }
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (strcmp(name, k_names[i]) == 0) {
            return slots[i];
        }
    }
    return NULL;
}

/* The value vector of parameter name, NULL for none. */
static odin3_vec *value_slot(odin3_ffdata *ff, const char *name) {
    if (strcmp(name, "ARST_VALUE") == 0) {
        return &ff->val_arst;
    }
    return strcmp(name, "SRST_VALUE") == 0 ? &ff->val_srst : NULL;
}

static odin3_bit bit_at(const odin3_vec *vec, uint32_t idx) {
    return *(const odin3_bit *)odin3_vec_cat(vec, idx);
}

static odin3_net_id net_at(const odin3_vec *vec, uint32_t idx) {
    return *(const odin3_net_id *)odin3_vec_cat(vec, idx);
}

static odin3_status push_net(odin3_vec *vec, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

static odin3_status push_bit(odin3_vec *vec, odin3_bit bit) {
    odin3_bit *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = bit;
    return ODIN3_OK;
}

/* --- init and free ---------------------------------------------------------------------------- */

void odin3_ffdata_init(odin3_ffdata *ff) {
    memset(ff, 0, sizeof *ff);
    odin3_vec *const nets[] = {&ff->sig_ad, &ff->sig_set, &ff->sig_clr, &ff->sig_d, &ff->sig_q};
    odin3_vec *const vals[] = {&ff->val_arst, &ff->val_srst, &ff->val_init};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        odin3_vec_init(nets[i], sizeof(odin3_net_id));
    }
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        odin3_vec_init(vals[i], sizeof(odin3_bit));
    }
}

void odin3_ffdata_free(odin3_ffdata *ff) {
    if (ff == NULL) {
        return;
    }
    odin3_vec *const all[] = {&ff->sig_ad, &ff->sig_set,  &ff->sig_clr,  &ff->sig_d,
                              &ff->sig_q,  &ff->val_arst, &ff->val_srst, &ff->val_init};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_free(all[i]);
    }
    odin3_ffdata_init(ff);
}

/* --- ff_load ---------------------------------------------------------------------------------- */

static void set_features(odin3_ffdata *ff, unsigned features) {
    ff->has_clk = (features & FT_CLK) != 0;
    ff->has_ce = (features & FT_CE) != 0;
    ff->has_srst = (features & FT_SRST) != 0;
    ff->has_arst = (features & FT_ARST) != 0;
    ff->has_aload = (features & FT_ALOAD) != 0;
    ff->has_sr = (features & FT_SR) != 0;
    ff->ce_over_srst = (features & FT_CE_OVER_SRST) != 0;
    ff->has_gclk = (features & FT_GCLK) != 0;
    ff->is_fine = (features & FT_FINE) != 0;
    bool pos = (features & FT_POS) != 0;
    ff->pol_clk = ff->is_fine && ff->has_clk && pos;
    ff->pol_aload = ff->is_fine && ff->has_aload && pos;
}

/* Port `port` (named name) of node into its slot; a port without one is ignored. */
static odin3_status read_port(const odin3_module *module, odin3_node_id node, uint32_t port,
                              const char *name, odin3_ffdata *ff) {
    odin3_pinslice pins = odin3_node_port(module, node, port);
    odin3_net_id *one = net_slot(ff, name);
    if (one != NULL) {
        *one = pins.count > 0 ? odin3_pin_net(module, pins.first) : (odin3_net_id){0};
        return ODIN3_OK;
    }
    odin3_vec *vec = vec_slot(ff, name);
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; vec != NULL && st == ODIN3_OK && k < pins.count; k++) {
        st = push_net(vec, odin3_pin_net(module, (odin3_pin_id){pins.first.v + k}));
    }
    return st;
}

/* Yosys Const::as_bool on a parameter: an INT other than 0, BITS with a 1 bit. */
static bool value_true(const odin3_value *val) {
    if (val == NULL) {
        return false;
    }
    if (val->kind == ODIN3_VAL_INT) {
        return val->i != 0;
    }
    for (uint32_t i = 0; val->kind == ODIN3_VAL_BITS && i < val->len; i++) {
        if (val->bits[i] == ODIN3_BIT_1) {
            return true;
        }
    }
    return false;
}

/* Parameter name (value val) into its slot; WIDTH, INIT and others are ignored here. */
static odin3_status read_param(odin3_ffdata *ff, const char *name, const odin3_value *val) {
    bool *pol = pol_slot(ff, name);
    if (pol != NULL) {
        *pol = value_true(val);
        return ODIN3_OK;
    }
    odin3_vec *vals = value_slot(ff, name);
    odin3_status st = ODIN3_OK;
    bool bits = val != NULL && val->kind == ODIN3_VAL_BITS;
    for (uint32_t i = 0; vals != NULL && st == ODIN3_OK && i < ff->width; i++) {
        st = push_bit(vals, bits && i < val->len ? (odin3_bit)val->bits[i] : ODIN3_BIT_X);
    }
    return st;
}

static odin3_status read_cell(const odin3_module *module, odin3_node_id node,
                              const odin3_celltype_def *def, odin3_ffdata *ff) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_ports; i++) {
        st = read_port(module, node, i, def->ports[i].name, ff);
    }
    ff->width = (uint32_t)ff->sig_q.len;
    for (uint32_t i = 0; st == ODIN3_OK && i < def->n_params; i++) {
        st = read_param(ff, def->params[i].name, odin3_node_param(module, node, i));
    }
    return st;
}

/* val_init per Q net; a fine FF reading x there takes its INIT parameter (0 or 1). */
static odin3_status read_init(const odin3_module *module, const odin3_celltype_def *def,
                              odin3_ffdata *ff) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        odin3_net_id qnet = net_at(&ff->sig_q, i);
        st = push_bit(&ff->val_init,
                      odin3_net_valid(qnet) ? odin3_ffinit_get(module, qnet) : ODIN3_BIT_X);
    }
    uint32_t init = param_named(def, "INIT");
    if (st != ODIN3_OK || !ff->is_fine || ff->width == 0 || init == def->n_params ||
        bit_at(&ff->val_init, 0) != ODIN3_BIT_X) {
        return st;
    }
    const odin3_value *val = odin3_node_param(module, ff->node, init);
    if (val != NULL && val->kind == ODIN3_VAL_INT && (val->i == 0 || val->i == 1)) {
        *(odin3_bit *)odin3_vec_at(&ff->val_init, 0) = val->i == 1 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    return st;
}

/* Yosys: a plain latch whose D is all constant loads as an async reset (EN = ARST). */
static odin3_status const_latch(const odin3_module *module, odin3_ffdata *ff) {
    if (!ff->has_aload || ff->has_clk || ff->has_sr || ff->has_arst) {
        return ODIN3_OK;
    }
    for (uint32_t i = 0; i < ff->width; i++) {
        odin3_net_id net = net_at(&ff->sig_ad, i);
        if (!odin3_net_valid(net) || odin3_net_const_value(module, net) == ODIN3_CONST_NONE) {
            return ODIN3_OK;
        }
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        odin3_const val = odin3_net_const_value(module, net_at(&ff->sig_ad, i));
        st = push_bit(&ff->val_arst, (odin3_bit)(val - ODIN3_CONST_0));
    }
    if (st != ODIN3_OK) {
        return st;
    }
    ff->has_aload = false;
    ff->has_arst = true;
    ff->sig_arst = ff->sig_aload;
    ff->pol_arst = ff->pol_aload;
    ff->sig_aload = (odin3_net_id){0};
    ff->pol_aload = false;
    odin3_vec_clear(&ff->sig_ad);
    return ODIN3_OK;
}

odin3_status odin3_ff_load(odin3_opt_ctx *oc, odin3_node_id node, odin3_ffdata *out) {
    if (out != NULL) {
        odin3_ffdata_init(out);
    }
    if (oc == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_load: no context or output");
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_module *module = oc->module;
    const ff_kind *kind =
        odin3_node_live(module, node) ? kind_of(oc->design, odin3_node_type(module, node)) : NULL;
    if (kind == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_load: node %u is not a live flip-flop", node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_celltype_def *def = odin3_celltype_get(oc->design, odin3_node_type(module, node));
    out->node = node;
    out->prov = odin3_node_prov(module, node);
    set_features(out, kind->features);
    odin3_status st = read_cell(module, node, def, out);
    st = st == ODIN3_OK ? read_init(module, def, out) : st;
    st = st == ODIN3_OK ? const_latch(module, out) : st;
    if (st != ODIN3_OK) {
        odin3_ffdata_free(out);
    }
    return st;
}

/* --- ff_slice --------------------------------------------------------------------------------- */

/* True when every vector ff's features use is width long. */
static bool vectors_ok(const odin3_ffdata *ff) {
    uint32_t width = ff->width;
    bool data = (!(ff->has_clk || ff->has_gclk) || ff->sig_d.len == width) &&
                (!ff->has_aload || ff->sig_ad.len == width);
    bool resets = (!ff->has_sr || (ff->sig_set.len == width && ff->sig_clr.len == width)) &&
                  (!ff->has_arst || ff->val_arst.len == width) &&
                  (!ff->has_srst || ff->val_srst.len == width);
    return ff->sig_q.len == width && ff->val_init.len == width && data && resets;
}

enum { N_PER_BIT = 8 };

/* The per-bit vectors of ff (src) and out (dst), and whether ff's features use each. */
typedef struct per_bit {
    const odin3_vec *src[N_PER_BIT];
    odin3_vec *dst[N_PER_BIT];
    bool used[N_PER_BIT];
} per_bit;

static void per_bit_of(const odin3_ffdata *ff, odin3_ffdata *out, per_bit *pb) {
    const per_bit all = {{&ff->sig_q, &ff->val_init, &ff->sig_d, &ff->sig_ad, &ff->sig_set,
                          &ff->sig_clr, &ff->val_arst, &ff->val_srst},
                         {&out->sig_q, &out->val_init, &out->sig_d, &out->sig_ad, &out->sig_set,
                          &out->sig_clr, &out->val_arst, &out->val_srst},
                         {true, true, ff->has_clk || ff->has_gclk, ff->has_aload, ff->has_sr,
                          ff->has_sr, ff->has_arst, ff->has_srst}};
    *pb = all;
}

static odin3_status slice_bits(const odin3_ffdata *ff, odin3_ff_bits bits, odin3_ffdata *out) {
    per_bit pb;
    per_bit_of(ff, out, &pb);
    for (uint32_t slot_i = 0; slot_i < N_PER_BIT; slot_i++) {
        odin3_status st = pb.used[slot_i] ? odin3_vec_reserve(pb.dst[slot_i], bits.n) : ODIN3_OK;
        for (uint32_t k = 0; st == ODIN3_OK && pb.used[slot_i] && k < bits.n; k++) {
            void *slot = odin3_vec_push(pb.dst[slot_i]);
            st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
            if (slot != NULL) {
                memcpy(slot, odin3_vec_cat(pb.src[slot_i], bits.bits[k]),
                       pb.src[slot_i]->elem_size);
            }
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_ff_slice(const odin3_ffdata *ff, odin3_ff_bits bits, odin3_ffdata *out) {
    if (out != NULL) {
        odin3_ffdata_init(out);
    }
    bool ok = ff != NULL && out != NULL && (bits.n == 0 || bits.bits != NULL) && vectors_ok(ff);
    for (uint32_t k = 0; ok && k < bits.n; k++) {
        ok = bits.bits[k] < ff->width;
    }
    if (!ok) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_slice: no FF, a bad vector or a bit past its width");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ffdata vectors = *out; /* out's empty vectors, kept across the copy below */
    *out = *ff;
    per_bit pb;
    per_bit_of(&vectors, out, &pb);
    for (uint32_t slot_i = 0; slot_i < N_PER_BIT; slot_i++) {
        *pb.dst[slot_i] = *pb.src[slot_i];
    }
    out->node = (odin3_node_id){0};
    out->width = bits.n;
    odin3_status st = slice_bits(ff, bits, out);
    if (st != ODIN3_OK) {
        odin3_ffdata_free(out);
    }
    return st;
}

/* --- ff_emit: the type ------------------------------------------------------------------------ */

static const char *clocked_type(const odin3_ffdata *ff) {
    bool ce = ff->has_ce;
    if (ff->has_sr) {
        return ce ? "$dffsre" : "$dffsr";
    }
    if (ff->has_arst) {
        return ce ? "$adffe" : "$adff";
    }
    if (ff->has_aload) {
        return ce ? "$aldffe" : "$aldff";
    }
    if (ff->has_srst) {
        if (!ce) {
            return "$sdff";
        }
        return ff->ce_over_srst ? "$sdffce" : "$sdffe";
    }
    return ce ? "$dffe" : "$dff";
}

/* FfData::emit's word branch; "$ff" (has_gclk) is not in the registry. */
static const char *word_type(const odin3_ffdata *ff) {
    if (ff->has_gclk) {
        return "$ff";
    }
    if (!ff->has_aload && !ff->has_clk) {
        return "$sr";
    }
    if (!ff->has_clk) {
        if (ff->has_sr) {
            return "$dlatchsr";
        }
        return ff->has_arst ? "$adlatch" : "$dlatch";
    }
    return clocked_type(ff);
}

/* The fine types the registry has; NULL for a fine FF needing another. */
static const char *fine_type(const odin3_ffdata *ff) {
    bool more = ff->has_ce || ff->has_srst || ff->has_arst || ff->has_sr;
    if (ff->has_gclk) {
        return more || ff->has_clk || ff->has_aload ? NULL : "$_FF_";
    }
    if (more || (ff->has_clk && ff->has_aload)) {
        return NULL;
    }
    if (ff->has_clk) {
        return ff->pol_clk ? "$_DFF_P_" : "$_DFF_N_";
    }
    return ff->pol_aload ? "$_DLATCH_P_" : "$_DLATCH_N_";
}

static odin3_status emit_type(const odin3_opt_ctx *oc, const odin3_ffdata *ff,
                              odin3_celltype_id *out) {
    const char *name = ff->is_fine ? fine_type(ff) : word_type(ff);
    *out = name != NULL ? type_named(oc->design, name) : (odin3_celltype_id){0};
    if (!odin3_celltype_valid(*out)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_emit: no registered cell type %s for this FF",
                  name != NULL ? name : "(fine)");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* --- ff_emit: the checks ---------------------------------------------------------------------- */

/* The index of node's port Q (every FF type has it; it is the only output). */
static uint32_t q_port(const odin3_opt_ctx *oc, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(oc->design, odin3_node_type(oc->module, node));
    uint32_t port = 0;
    while (port < def->n_ports && strcmp(def->ports[port].name, "Q") != 0) {
        port++;
    }
    return port;
}

/* True when node's Q nets are exactly ff's sig_q. */
static bool q_matches(const odin3_opt_ctx *oc, const odin3_ffdata *ff) {
    const odin3_celltype_def *def =
        odin3_celltype_get(oc->design, odin3_node_type(oc->module, ff->node));
    uint32_t port = q_port(oc, ff->node);
    odin3_pinslice pins = odin3_node_port(oc->module, ff->node, port);
    bool same = port < def->n_ports && pins.count == ff->width;
    for (uint32_t k = 0; same && k < pins.count; k++) {
        same = odin3_pin_net(oc->module, (odin3_pin_id){pins.first.v + k}).v ==
               net_at(&ff->sig_q, k).v;
    }
    return same;
}

static odin3_status emit_check(const odin3_opt_ctx *oc, const odin3_ffdata *ff) {
    if (!vectors_ok(ff)) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_emit: a vector is not %u bits wide", ff->width);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (ff->node.v != 0 && (!odin3_node_live(oc->module, ff->node) ||
                            kind_of(oc->design, odin3_node_type(oc->module, ff->node)) == NULL ||
                            !q_matches(oc, ff))) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_emit: node %u is not a live FF on the model's Q",
                  ff->node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* --- ff_emit: Q tied to init ------------------------------------------------------------------ */

/* Writes saved init bits back on the Q nets still live (after a failed tie). */
static void restore_inits(odin3_opt_ctx *oc, const odin3_ffdata *ff, const odin3_bit *saved) {
    for (uint32_t i = 0; i < ff->width; i++) {
        odin3_net_id qnet = net_at(&ff->sig_q, i);
        if (saved[i] != ODIN3_BIT_X && odin3_net_live(oc->module, qnet)) {
            (void)odin3_ffinit_set(oc->module, qnet, saved[i]);
        }
    }
}

/* A new FF's Q nets merged into the constant nets (Yosys module->connect(sig_q, val_init)). */
static odin3_status tie_new(odin3_opt_ctx *oc, const odin3_ffdata *ff, const odin3_net_id *src) {
    for (uint32_t i = 0; i < ff->width; i++) {
        odin3_net_id qnet = net_at(&ff->sig_q, i);
        if (!odin3_net_valid(qnet) || !odin3_net_live(oc->module, qnet) || qnet.v == src[i].v) {
            continue;
        }
        odin3_status st = odin3_net_merge(oc->module, (odin3_net_pair){src[i], qnet});
        if (st != ODIN3_OK) {
            return st;
        }
        oc->changed = true;
    }
    return ODIN3_OK;
}

/* The constant nets of val_init, the inits saved, then removed (Yosys remove()). */
static odin3_status tie_prepare(odin3_opt_ctx *oc, const odin3_ffdata *ff, odin3_net_id *src,
                                odin3_bit *saved) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        odin3_net_id qnet = net_at(&ff->sig_q, i);
        saved[i] = odin3_net_valid(qnet) ? odin3_ffinit_get(oc->module, qnet) : ODIN3_BIT_X;
        st = odin3_opt_const_net(oc, bit_at(&ff->val_init, i), &src[i]);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        odin3_net_id qnet = net_at(&ff->sig_q, i);
        st = odin3_net_valid(qnet) ? odin3_ffinit_remove(oc->module, qnet, NULL) : ODIN3_OK;
    }
    return st;
}

static odin3_status emit_tie(odin3_opt_ctx *oc, odin3_ffdata *ff) {
    odin3_net_id *src = odin3_util_calloc(sizeof *src * ((size_t)ff->width + 1));
    odin3_bit *saved = odin3_util_malloc(sizeof *saved * ((size_t)ff->width + 1));
    odin3_status st = src != NULL && saved != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? tie_prepare(oc, ff, src, saved) : st;
    if (st == ODIN3_OK && ff->node.v != 0) {
        odin3_kit_conn conn = {ff->node, q_port(oc, ff->node), src, ff->width};
        st = odin3_kit_connect(oc, &conn);
    } else if (st == ODIN3_OK) {
        st = tie_new(oc, ff, src);
    }
    if (st != ODIN3_OK && saved != NULL && src != NULL) {
        restore_inits(oc, ff, saved);
    }
    if (st == ODIN3_OK) {
        ff->node = (odin3_node_id){0};
    }
    odin3_util_free(src);
    odin3_util_free(saved);
    return st;
}

/* --- ff_emit: the cell ------------------------------------------------------------------------ */

/* Yosys arst_to_aload: EN = ARST, AD = the constant nets of val_arst. */
static odin3_status arst_to_aload(odin3_opt_ctx *oc, odin3_ffdata *ff) {
    odin3_vec_clear(&ff->sig_ad);
    odin3_status st = odin3_vec_reserve(&ff->sig_ad, ff->width);
    for (uint32_t i = 0; st == ODIN3_OK && i < ff->width; i++) {
        odin3_net_id net = {0};
        st = odin3_opt_const_net(oc, bit_at(&ff->val_arst, i), &net);
        st = st == ODIN3_OK ? push_net(&ff->sig_ad, net) : st;
    }
    if (st != ODIN3_OK) {
        odin3_vec_clear(&ff->sig_ad);
        return st;
    }
    ff->has_aload = true;
    ff->pol_aload = ff->pol_arst;
    ff->sig_aload = ff->sig_arst;
    ff->has_arst = false;
    ff->sig_arst = (odin3_net_id){0};
    odin3_vec_clear(&ff->val_arst);
    return ODIN3_OK;
}

/* The cell being made: its type, parameters, ports and the *_VALUE bits. */
typedef struct emit_plan {
    odin3_celltype_id type;
    const odin3_celltype_def *def;
    odin3_value params[FF_MAX_PARAMS];
    odin3_netvec ports[FF_MAX_PORTS];
    uint8_t *bits[FF_MAX_PARAMS];
} emit_plan;

/* A fine FF's INIT: val_init 0 or 1, else the node's 2 or 3, else 3. */
static int64_t fine_init(const odin3_opt_ctx *oc, const odin3_ffdata *ff, uint32_t index) {
    odin3_bit bit = bit_at(&ff->val_init, 0);
    if (bit == ODIN3_BIT_0 || bit == ODIN3_BIT_1) {
        return bit == ODIN3_BIT_1 ? 1 : 0;
    }
    const odin3_value *old = ff->node.v != 0 ? odin3_node_param(oc->module, ff->node, index) : NULL;
    if (old != NULL && old->kind == ODIN3_VAL_INT &&
        (old->i == INIT_DC || old->i == INIT_UNKNOWN)) {
        return old->i;
    }
    return INIT_UNKNOWN;
}

static odin3_status plan_param(const odin3_opt_ctx *oc, odin3_ffdata *ff, emit_plan *plan,
                               uint32_t index) {
    const char *name = plan->def->params[index].name;
    odin3_value *val = &plan->params[index];
    *val = plan->def->params[index].dflt;
    bool *pol = pol_slot(ff, name);
    const odin3_vec *vals = value_slot(ff, name);
    if (strcmp(name, "WIDTH") == 0) {
        val->i = ff->width;
    } else if (strcmp(name, "INIT") == 0) {
        val->i = fine_init(oc, ff, index);
    } else if (pol != NULL) {
        val->i = *pol ? 1 : 0;
    } else if (vals != NULL) {
        plan->bits[index] = odin3_util_malloc(ff->width);
        if (plan->bits[index] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (uint32_t k = 0; k < ff->width; k++) {
            plan->bits[index][k] = (uint8_t)bit_at(vals, k);
        }
        val->bits = plan->bits[index];
        val->len = ff->width;
    }
    return ODIN3_OK;
}

static odin3_status plan_open(const odin3_opt_ctx *oc, odin3_ffdata *ff, emit_plan *plan) {
    plan->def = odin3_celltype_get(oc->design, plan->type);
    if (plan->def->n_params > FF_MAX_PARAMS || plan->def->n_ports > FF_MAX_PORTS) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_emit: %s has too many ports", plan->def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < plan->def->n_ports; i++) {
        const char *name = plan->def->ports[i].name;
        odin3_net_id *one = net_slot(ff, name);
        odin3_vec *vec = vec_slot(ff, name);
        if (one != NULL) {
            plan->ports[i] = (odin3_netvec){one, 1};
        } else if (vec != NULL) {
            plan->ports[i] = (odin3_netvec){vec->data, (uint32_t)vec->len};
        }
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < plan->def->n_params; i++) {
        st = plan_param(oc, ff, plan, i);
    }
    return st;
}

/* A DERIVED record of ff->prov in the current operation (one is begun if none), 0 without. */
static odin3_status new_prov(odin3_opt_ctx *oc, const odin3_ffdata *ff, odin3_prov_id *out) {
    *out = (odin3_prov_id){0};
    if (ff->prov.v == 0) {
        return ODIN3_OK;
    }
    if (oc->pass->op == 0) {
        odin3_prov_begin_op(oc->pass);
    }
    return odin3_prov_derive(oc->pass, (odin3_prov_list){&ff->prov, 1}, out);
}

static odin3_status emit_cell(odin3_opt_ctx *oc, odin3_ffdata *ff, odin3_celltype_id type,
                              odin3_node_id *out) {
    emit_plan plan;
    memset(&plan, 0, sizeof plan);
    plan.type = type;
    odin3_status st = plan_open(oc, ff, &plan);
    odin3_kit_cell cell = {type, plan.params, plan.def->n_params, plan.ports, {0}};
    if (st == ODIN3_OK && ff->node.v != 0) {
        /* Q is sig_q, the old node's Q nets: the kit pairs it with the old Q, wherever that sits */
        odin3_kit_rebuild req = {ff->node, cell};
        st = odin3_kit_rebuild_node(oc, &req, out);
    } else if (st == ODIN3_OK) {
        st = new_prov(oc, ff, &cell.prov);
        st = st == ODIN3_OK ? odin3_kit_new_cell(oc, &cell, out) : st;
    }
    for (uint32_t i = 0; i < FF_MAX_PARAMS; i++) {
        odin3_util_free(plan.bits[i]);
    }
    return st;
}

/* Every Q net whose init differs from val_init gets it (a fine FF: where the wire has one). */
static odin3_status write_inits(odin3_opt_ctx *oc, const odin3_ffdata *ff) {
    for (uint32_t i = 0; i < ff->width; i++) {
        odin3_net_id qnet = net_at(&ff->sig_q, i);
        if (!odin3_net_valid(qnet)) {
            continue;
        }
        odin3_bit cur = odin3_ffinit_get(oc->module, qnet);
        odin3_bit want = bit_at(&ff->val_init, i);
        if (cur == want || (ff->is_fine && cur == ODIN3_BIT_X)) {
            continue;
        }
        odin3_status st = odin3_ffinit_set(oc->module, qnet, want);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_ff_emit(odin3_opt_ctx *oc, odin3_ffdata *ff, odin3_node_id *out) {
    if (oc == NULL || ff == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt: ff_emit: no context, FF or output");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = (odin3_node_id){0};
    odin3_status st = emit_check(oc, ff);
    if (st != ODIN3_OK || ff->width == 0) {
        return st;
    }
    if (!ff->has_aload && !ff->has_clk && !ff->has_gclk && !ff->has_sr) {
        if (!ff->has_arst) {
            return emit_tie(oc, ff);
        }
        st = arst_to_aload(oc, ff);
    }
    odin3_celltype_id type = {0};
    st = st == ODIN3_OK ? emit_type(oc, ff, &type) : st;
    st = st == ODIN3_OK ? emit_cell(oc, ff, type, out) : st;
    if (st == ODIN3_OK) {
        ff->node = *out;
        st = write_inits(oc, ff);
    }
    return st;
}
