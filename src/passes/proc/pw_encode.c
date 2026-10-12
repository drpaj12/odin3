/*
 * pw_encode.c — re-encoding a working form as a $proc node (2E spec §17; --stop-after, and the
 * unit tests' proc_build): 2D point (e)'s canonical layout, O and I pins in net-ID order, the
 * positions laid out as their runs lie in pw->bits (so decode then encode is the identity).
 */
#include "passes/proc/pw.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/proc/proc_internal.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Scratch marks while the pins are collected; replaced by position + 1 once they are sorted. */
#define MARK_OUT UINT32_MAX
#define MARK_IN (UINT32_MAX - 1U)

enum { ENCODE_ARENA_CHUNK = 4096, N_PROC_PARAMS = 3 };

/* A span of the new body and the run of pw->bits it was made from. */
typedef struct span_ref {
    uint32_t first, len; /* the run */
    uint32_t seq;        /* creation order: the sort's tie-break */
    odin3_pspan *span;   /* in one of the body arrays (reserved, so it stays put) */
} span_ref;

/* A run of pw->bits copied into the new positions, in order. */
typedef struct bit_range {
    uint32_t first, end;
} bit_range;

typedef struct encoder {
    odin3_proc_mctx *mctx;
    odin3_pw *pw;
    odin3_vec case_order, sw_order;         /* uint32_t: the old index of each new case, switch */
    odin3_vec case_first_sw, sw_first_case; /* uint32_t: per new case, switch */
    odin3_vec seen_case, seen_sw;           /* uint8_t: per old case, switch */
    odin3_vec out_nets, in_nets;            /* odin3_net_id: the O and I pins */
    uint32_t consts_used;                   /* one bit per constant state 0, 1, x, z */
    odin3_pbit const_pbit[ODIN3_PROC_N_CONST_NETS];
    odin3_vec pbits, compares, actions, cases, switches, syncs; /* the new body */
    odin3_vec refs, ranges;                                     /* span_ref, bit_range */
} encoder;

static void enc_init(encoder *enc, odin3_proc_mctx *mctx, odin3_pw *pw) {
    memset(enc, 0, sizeof *enc);
    enc->mctx = mctx;
    enc->pw = pw;
    odin3_vec *u32s[] = {&enc->case_order, &enc->sw_order, &enc->case_first_sw,
                         &enc->sw_first_case};
    for (size_t i = 0; i < sizeof u32s / sizeof u32s[0]; i++) {
        odin3_vec_init(u32s[i], sizeof(uint32_t));
    }
    odin3_vec_init(&enc->seen_case, sizeof(uint8_t));
    odin3_vec_init(&enc->seen_sw, sizeof(uint8_t));
    odin3_vec_init(&enc->out_nets, sizeof(odin3_net_id));
    odin3_vec_init(&enc->in_nets, sizeof(odin3_net_id));
    odin3_vec_init(&enc->pbits, sizeof(odin3_pbit));
    odin3_vec_init(&enc->compares, sizeof(odin3_pspan));
    odin3_vec_init(&enc->actions, sizeof(odin3_proc_action));
    odin3_vec_init(&enc->cases, sizeof(odin3_proc_case));
    odin3_vec_init(&enc->switches, sizeof(odin3_proc_switch));
    odin3_vec_init(&enc->syncs, sizeof(odin3_proc_sync));
    odin3_vec_init(&enc->refs, sizeof(span_ref));
    odin3_vec_init(&enc->ranges, sizeof(bit_range));
}

static void enc_free(encoder *enc) {
    odin3_vec *all[] = {&enc->case_order, &enc->sw_order, &enc->case_first_sw, &enc->sw_first_case,
                        &enc->seen_case,  &enc->seen_sw,  &enc->out_nets,      &enc->in_nets,
                        &enc->pbits,      &enc->compares, &enc->actions,       &enc->cases,
                        &enc->switches,   &enc->syncs,    &enc->refs,          &enc->ranges};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_free(all[i]);
    }
}

static uint32_t u32_at(const odin3_vec *vec, size_t idx) {
    return *(const uint32_t *)odin3_vec_cat(vec, idx);
}

static odin3_status push_u32(odin3_vec *vec, uint32_t val) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = val;
    return ODIN3_OK;
}

/* A vec of count zero bytes. */
static odin3_status zero_flags(odin3_vec *vec, size_t count) {
    if (odin3_vec_reserve(vec, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (vec->len < count) {
        (void)odin3_vec_push(vec); /* reserved; zeroed */
    }
    return ODIN3_OK;
}

static const odin3_pw_case *old_case(const encoder *enc, uint32_t idx) {
    return odin3_vec_cat(&enc->pw->cases, idx);
}

static const odin3_pw_switch *old_switch(const encoder *enc, uint32_t idx) {
    return odin3_vec_cat(&enc->pw->switches, idx);
}

/* --- the breadth-first order --------------------------------------------------------------- */

/* Gives old case or switch idx the next new index; INVALID_ARG for a bad or repeated index. */
static odin3_status take_index(odin3_vec *order, uint32_t idx, odin3_vec *seen, const char *what) {
    if (idx >= seen->len) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: %s %u does not exist", what, idx);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint8_t *flag = odin3_vec_at(seen, idx);
    if (*flag != 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: %s %u is reached twice", what, idx);
        return ODIN3_ERR_INVALID_ARG;
    }
    *flag = 1;
    return push_u32(order, idx);
}

/* New case ci: its switches get the next switch indices. */
static odin3_status visit_case(encoder *enc, size_t ci) {
    const odin3_pw_case *cs = old_case(enc, u32_at(&enc->case_order, ci));
    odin3_status st = push_u32(&enc->case_first_sw, (uint32_t)enc->sw_order.len);
    for (size_t j = 0; st == ODIN3_OK && j < cs->switches.len; j++) {
        st = take_index(&enc->sw_order, u32_at(&cs->switches, j), &enc->seen_sw, "switch");
    }
    return st;
}

/* New switch si: its cases get the next case indices. */
static odin3_status visit_switch(encoder *enc, size_t si) {
    const odin3_pw_switch *sw = old_switch(enc, u32_at(&enc->sw_order, si));
    odin3_status st = push_u32(&enc->sw_first_case, (uint32_t)enc->case_order.len);
    for (size_t j = 0; st == ODIN3_OK && j < sw->cases.len; j++) {
        st = take_index(&enc->case_order, u32_at(&sw->cases, j), &enc->seen_case, "case");
    }
    return st;
}

/*
 * The canonical numbering (2D point (e)): case 0 is the root, the switches of the cases in case
 * order, the cases of the switches in switch order. Each counter depends only on its own queue, so
 * visiting the queued cases first and the queued switches when no case is left gives it.
 */
static odin3_status enc_order(encoder *enc) {
    if (enc->pw->cases.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: the working form has no root case");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = zero_flags(&enc->seen_case, enc->pw->cases.len);
    if (st == ODIN3_OK) {
        st = zero_flags(&enc->seen_sw, enc->pw->switches.len);
    }
    if (st == ODIN3_OK) {
        st = take_index(&enc->case_order, 0, &enc->seen_case, "case");
    }
    size_t ci = 0;
    size_t si = 0;
    while (st == ODIN3_OK && (ci < enc->case_order.len || si < enc->sw_order.len)) {
        st = ci < enc->case_order.len ? visit_case(enc, ci++) : visit_switch(enc, si++);
    }
    return st;
}

/* --- the pins ------------------------------------------------------------------------------ */

/* A net bit must be a live net of the module. */
static odin3_status live_net(const encoder *enc, odin3_pw_bit bit) {
    if (!odin3_net_live(enc->mctx->module, (odin3_net_id){bit})) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: bit %u is not a live net", (unsigned)bit);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* Marks a live net as a pin of the kind mark (MARK_OUT or MARK_IN) unless it is a pin already. */
static odin3_status mark_net(encoder *enc, odin3_pw_bit bit, uint32_t mark) {
    odin3_status st = live_net(enc, bit);
    odin3_net_id net = {bit};
    if (st != ODIN3_OK || odin3_proc_scratch_get(enc->mctx, net) != 0) {
        return st;
    }
    odin3_proc_scratch_set(enc->mctx, net, mark);
    odin3_net_id *slot = odin3_vec_push(mark == MARK_OUT ? &enc->out_nets : &enc->in_nets);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

/* Marks the nets of an lhs as O pins. */
static odin3_status mark_lhs(encoder *enc, odin3_pw_sig sig) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(enc->pw, sig);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < sig.len; i++) {
        if ((bits[i] & ODIN3_PW_CONST) != 0) {
            odin3_log(ODIN3_LOG_ERROR, "proc: encode: an lhs bit is a constant");
            return ODIN3_ERR_INVALID_ARG;
        }
        st = mark_net(enc, bits[i], MARK_OUT);
    }
    return st;
}

/* Marks a net read as an I pin (unless O), or records the constant used. */
static odin3_status mark_read(encoder *enc, odin3_pw_bit bit) {
    if (bit == ODIN3_PW_SM) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: a marked (Sm) bit cannot be encoded");
        return ODIN3_ERR_INVALID_ARG;
    }
    if ((bit & ODIN3_PW_CONST) == 0) {
        return mark_net(enc, bit, MARK_IN);
    }
    if (bit <= ODIN3_PW_SZ) {
        enc->consts_used |= 1U << (bit & ~ODIN3_PW_CONST);
    }
    return ODIN3_OK;
}

/* Marks the nets read by count bits. */
static odin3_status mark_reads(encoder *enc, const odin3_pw_bit *bits, uint32_t count) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = mark_read(enc, bits[i]);
    }
    return st;
}

static odin3_status mark_sig(encoder *enc, odin3_pw_sig sig) {
    return mark_reads(enc, odin3_pw_sig_bits(enc->pw, sig), sig.len);
}

/* Applies mark_lhs (lhs true) or mark_sig on the rhs (false) to each action of vec. */
static odin3_status mark_actions(encoder *enc, const odin3_vec *vec, bool lhs) {
    odin3_status st = ODIN3_OK;
    for (size_t j = 0; st == ODIN3_OK && j < vec->len; j++) {
        const odin3_pw_action *act = odin3_vec_cat(vec, j);
        st = lhs ? mark_lhs(enc, act->lhs) : mark_sig(enc, act->rhs);
    }
    return st;
}

/* Every lhs net of the encoded cases and syncs. */
static odin3_status mark_outputs(encoder *enc) {
    odin3_status st = ODIN3_OK;
    for (size_t ci = 0; st == ODIN3_OK && ci < enc->case_order.len; ci++) {
        st = mark_actions(enc, &old_case(enc, u32_at(&enc->case_order, ci))->actions, true);
    }
    for (size_t k = 0; st == ODIN3_OK && k < enc->pw->syncs.len; k++) {
        st = mark_actions(enc, &((const odin3_pw_sync *)odin3_vec_cat(&enc->pw->syncs, k))->actions,
                          true);
    }
    return st;
}

/* Every read: compares and right sides of the cases, switch signals, sync signals and actions. */
static odin3_status mark_inputs(encoder *enc) {
    odin3_status st = ODIN3_OK;
    for (size_t ci = 0; st == ODIN3_OK && ci < enc->case_order.len; ci++) {
        const odin3_pw_case *cs = old_case(enc, u32_at(&enc->case_order, ci));
        for (size_t j = 0; st == ODIN3_OK && j < cs->compares.len; j++) {
            st = mark_sig(enc, *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j));
        }
        st = st == ODIN3_OK ? mark_actions(enc, &cs->actions, false) : st;
    }
    for (size_t si = 0; st == ODIN3_OK && si < enc->sw_order.len; si++) {
        st = mark_sig(enc, old_switch(enc, u32_at(&enc->sw_order, si))->signal);
    }
    for (size_t k = 0; st == ODIN3_OK && k < enc->pw->syncs.len; k++) {
        const odin3_pw_sync *sy = odin3_vec_cat(&enc->pw->syncs, k);
        if (sy->type != ODIN3_SYNC_ALWAYS) {
            st = mark_read(enc, sy->signal);
        }
        st = st == ODIN3_OK ? mark_actions(enc, &sy->actions, false) : st;
    }
    return st;
}

/* The shared constant nets of the constants used, as I pins (unless already pins). */
static odin3_status mark_consts(encoder *enc, odin3_net_id *nets) {
    odin3_status st = ODIN3_OK;
    for (uint32_t state = 0; st == ODIN3_OK && state < ODIN3_PROC_N_CONST_NETS; state++) {
        if ((enc->consts_used & (1U << state)) != 0) {
            st = odin3_proc_const_net(enc->mctx, ODIN3_PW_CONST | state, &nets[state]);
        }
    }
    st = st == ODIN3_OK ? odin3_proc_scratch_grow(enc->mctx) : st; /* a net may be new */
    for (uint32_t state = 0; st == ODIN3_OK && state < ODIN3_PROC_N_CONST_NETS; state++) {
        if ((enc->consts_used & (1U << state)) != 0) {
            st = mark_net(enc, nets[state].v, MARK_IN);
        }
    }
    return st;
}

static int net_cmp(const void *lhs, const void *rhs) {
    uint32_t net_l = ((const odin3_net_id *)lhs)->v;
    uint32_t net_r = ((const odin3_net_id *)rhs)->v;
    return (net_l > net_r) - (net_l < net_r);
}

/* Sorts the pins by net ID and gives each net its position (tag | k) + 1 in the index. */
static odin3_status number_pins(encoder *enc, odin3_vec *nets, odin3_pbit tag) {
    if (nets->len > (size_t)ODIN3_PBIT_INDEX_MASK + 1) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: more than 2^30 pins on one port");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (nets->len > 1) {
        qsort(nets->data, nets->len, sizeof(odin3_net_id), net_cmp);
    }
    for (size_t k = 0; k < nets->len; k++) {
        odin3_net_id net = *(const odin3_net_id *)odin3_vec_cat(nets, k);
        odin3_proc_scratch_set(enc->mctx, net, (tag | (uint32_t)k) + 1);
    }
    return ODIN3_OK;
}

/* O = the lhs nets, I = the other nets read and the constant nets; both numbered. */
static odin3_status enc_pins(encoder *enc) {
    odin3_net_id const_nets[ODIN3_PROC_N_CONST_NETS] = {{0}};
    odin3_status st = odin3_proc_scratch_grow(enc->mctx);
    st = st == ODIN3_OK ? mark_outputs(enc) : st;
    st = st == ODIN3_OK ? mark_inputs(enc) : st;
    st = st == ODIN3_OK ? mark_consts(enc, const_nets) : st;
    st = st == ODIN3_OK ? number_pins(enc, &enc->out_nets, ODIN3_PBIT_OUT) : st;
    st = st == ODIN3_OK ? number_pins(enc, &enc->in_nets, ODIN3_PBIT_IN) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < ODIN3_PROC_N_CONST_NETS; k++) {
        if ((enc->consts_used & (1U << k)) != 0) {
            enc->const_pbit[k] = odin3_proc_scratch_get(enc->mctx, const_nets[k]) - 1;
        }
    }
    return st;
}

/* The position of a bit (its pins numbered): IN | k, OUT | k or ANY. */
static odin3_pbit pbit_of(const encoder *enc, odin3_pw_bit bit) {
    if (bit == ODIN3_PW_SA) {
        return ODIN3_PBIT_ANY;
    }
    if ((bit & ODIN3_PW_CONST) != 0) {
        return enc->const_pbit[bit & ~ODIN3_PW_CONST]; /* 0 .. 3: Sm was refused */
    }
    return odin3_proc_scratch_get(enc->mctx, (odin3_net_id){bit}) - 1;
}

/* --- the body ------------------------------------------------------------------------------ */

/* Points span at run sig for now, recording it for the layout. */
static void add_ref(encoder *enc, odin3_pspan *span, odin3_pw_sig sig) {
    *span = (odin3_pspan){sig.first, sig.len};
    span_ref *ref = odin3_vec_push(&enc->refs); /* reserved */
    *ref = (span_ref){sig.first, sig.len, (uint32_t)(enc->refs.len - 1), span};
}

static void add_actions(encoder *enc, const odin3_vec *from) {
    for (size_t j = 0; j < from->len; j++) {
        const odin3_pw_action *act = odin3_vec_cat(from, j);
        odin3_proc_action *to = odin3_vec_push(&enc->actions); /* reserved */
        to->prov = act->prov;
        add_ref(enc, &to->lhs, act->lhs);
        add_ref(enc, &to->rhs, act->rhs);
    }
}

static void add_case(encoder *enc, size_t ci) {
    const odin3_pw_case *cs = old_case(enc, u32_at(&enc->case_order, ci));
    odin3_proc_case *to = odin3_vec_push(&enc->cases); /* reserved */
    *to = (odin3_proc_case){(uint32_t)enc->compares.len,
                            (uint32_t)cs->compares.len,
                            (uint32_t)enc->actions.len,
                            (uint32_t)cs->actions.len,
                            u32_at(&enc->case_first_sw, ci),
                            (uint32_t)cs->switches.len,
                            cs->prov};
    for (size_t j = 0; j < cs->compares.len; j++) {
        add_ref(enc, odin3_vec_push(&enc->compares),
                *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j));
    }
    add_actions(enc, &cs->actions);
}

static void add_switch(encoder *enc, size_t si) {
    const odin3_pw_switch *sw = old_switch(enc, u32_at(&enc->sw_order, si));
    odin3_proc_switch *to = odin3_vec_push(&enc->switches); /* reserved */
    to->first_case = u32_at(&enc->sw_first_case, si);
    to->n_cases = (uint32_t)sw->cases.len;
    to->flags = sw->flags;
    to->prov = sw->prov;
    add_ref(enc, &to->signal, sw->signal);
}

static void add_sync(encoder *enc, size_t sync_idx) {
    const odin3_pw_sync *sy = odin3_vec_cat(&enc->pw->syncs, sync_idx);
    odin3_proc_sync *to = odin3_vec_push(&enc->syncs); /* reserved */
    to->type = sy->type;
    to->signal = sy->type != ODIN3_SYNC_ALWAYS ? pbit_of(enc, sy->signal) : 0;
    to->first_action = (uint32_t)enc->actions.len;
    to->n_actions = (uint32_t)sy->actions.len;
    add_actions(enc, &sy->actions);
}

/* Reserves every body array and the refs at their final sizes, so no record moves. */
static odin3_status reserve_body(encoder *enc) {
    size_t n_compares = 0;
    size_t n_actions = 0;
    for (size_t ci = 0; ci < enc->case_order.len; ci++) {
        const odin3_pw_case *cs = old_case(enc, u32_at(&enc->case_order, ci));
        n_compares += cs->compares.len;
        n_actions += cs->actions.len;
    }
    for (size_t k = 0; k < enc->pw->syncs.len; k++) {
        n_actions += ((const odin3_pw_sync *)odin3_vec_cat(&enc->pw->syncs, k))->actions.len;
    }
    size_t n_refs = n_compares + 2 * n_actions + enc->sw_order.len;
    bool ok = odin3_vec_reserve(&enc->compares, n_compares) == ODIN3_OK &&
              odin3_vec_reserve(&enc->actions, n_actions) == ODIN3_OK &&
              odin3_vec_reserve(&enc->cases, enc->case_order.len) == ODIN3_OK &&
              odin3_vec_reserve(&enc->switches, enc->sw_order.len) == ODIN3_OK &&
              odin3_vec_reserve(&enc->syncs, enc->pw->syncs.len) == ODIN3_OK &&
              odin3_vec_reserve(&enc->refs, n_refs) == ODIN3_OK;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* The records: cases (their actions first), switches, then syncs (their actions last). */
static odin3_status enc_records(encoder *enc) {
    odin3_status st = reserve_body(enc);
    if (st != ODIN3_OK) {
        return st;
    }
    for (size_t ci = 0; ci < enc->case_order.len; ci++) {
        add_case(enc, ci);
    }
    for (size_t si = 0; si < enc->sw_order.len; si++) {
        add_switch(enc, si);
    }
    for (size_t k = 0; k < enc->pw->syncs.len; k++) {
        add_sync(enc, k);
    }
    return ODIN3_OK;
}

static int ref_cmp(const void *lhs, const void *rhs) {
    const span_ref *ref_l = lhs;
    const span_ref *ref_r = rhs;
    if (ref_l->first != ref_r->first) {
        return ref_l->first < ref_r->first ? -1 : 1;
    }
    return (ref_l->seq > ref_r->seq) - (ref_l->seq < ref_r->seq);
}

/* The ranges being cut: the open one, where it starts in the new positions, the ones closed. */
typedef struct range_cut {
    bit_range cur;
    uint64_t base; /* where cur starts in the new positions */
    uint64_t next; /* the new positions of the closed ranges */
    bool open;
} range_cut;

/* Closes the open range (appended to enc->ranges). */
static odin3_status close_range(encoder *enc, range_cut *cut) {
    if (!cut->open) {
        return ODIN3_OK;
    }
    bit_range *slot = odin3_vec_push(&enc->ranges);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = cut->cur;
    cut->next += cut->cur.end - cut->cur.first;
    cut->open = false;
    return ODIN3_OK;
}

/* Places one run (refs in pw->bits order): it extends the open range or starts the next one. */
static odin3_status place_ref(encoder *enc, range_cut *cut, span_ref *ref) {
    uint32_t end = ref->first + ref->len;
    if (cut->open && ref->first <= cut->cur.end) {
        cut->cur.end = end > cut->cur.end ? end : cut->cur.end;
    } else {
        odin3_status st = close_range(enc, cut);
        if (st != ODIN3_OK) {
            return st;
        }
        cut->cur = (bit_range){ref->first, end};
        cut->base = cut->next;
        cut->open = true;
    }
    ref->span->first = (uint32_t)(cut->base + (ref->first - cut->cur.first));
    return ODIN3_OK;
}

/*
 * Lays the referenced runs out in their pw->bits order: the union of the runs is cut into maximal
 * ranges, copied one after the other, and each span points at its run inside its range. A body
 * decoded unchanged has one range covering every position, so its positions keep their indices.
 */
static odin3_status enc_ranges(encoder *enc) {
    if (enc->refs.len > 1) {
        qsort(enc->refs.data, enc->refs.len, sizeof(span_ref), ref_cmp);
    }
    range_cut cut = {{0, 0}, 0, 0, false};
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < enc->refs.len; i++) {
        span_ref *ref = odin3_vec_at(&enc->refs, i);
        if (ref->len == 0) {
            ref->span->first = 0;
        } else {
            st = place_ref(enc, &cut, ref);
        }
    }
    return st == ODIN3_OK ? close_range(enc, &cut) : st;
}

/* The positions: each range's bits, in order. */
static odin3_status enc_pbits(encoder *enc) {
    size_t total = 0;
    for (size_t i = 0; i < enc->ranges.len; i++) {
        const bit_range *range = odin3_vec_cat(&enc->ranges, i);
        total += range->end - range->first;
    }
    if (odin3_vec_reserve(&enc->pbits, total) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < enc->ranges.len; i++) {
        const bit_range *range = odin3_vec_cat(&enc->ranges, i);
        const odin3_pw_bit *bits = odin3_pw_sig_bits(enc->pw, (odin3_pw_sig){range->first, 0});
        for (uint32_t pos = 0; pos < range->end - range->first; pos++) {
            *(odin3_pbit *)odin3_vec_push(&enc->pbits) = pbit_of(enc, bits[pos]); /* reserved */
        }
    }
    return ODIN3_OK;
}

static odin3_proc_body enc_body(const encoder *enc) {
    return (odin3_proc_body){enc->pbits.data,    (uint32_t)enc->pbits.len,
                             enc->compares.data, (uint32_t)enc->compares.len,
                             enc->actions.data,  (uint32_t)enc->actions.len,
                             enc->cases.data,    (uint32_t)enc->cases.len,
                             enc->switches.data, (uint32_t)enc->switches.len,
                             enc->syncs.data,    (uint32_t)enc->syncs.len};
}

/* --- the node ------------------------------------------------------------------------------ */

static odin3_status proc_type(const odin3_design *design, odin3_celltype_id *out) {
    uint32_t str = 0;
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr("$proc"), &str) &&
        odin3_celltype_find(design, str, out)) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR, "proc: encode: the $proc cell type is not registered");
    return ODIN3_ERR_INVALID_ARG;
}

/* The new node from the verified body; nothing else changes on failure. */
static odin3_status enc_create(const encoder *enc, const odin3_proc_body *body,
                               odin3_node_id *out) {
    odin3_celltype_id type = {0};
    odin3_status st = proc_type(enc->mctx->design, &type);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_arena *arena = odin3_arena_create(ENCODE_ARENA_CHUNK);
    if (arena == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_value params[N_PROC_PARAMS] = {odin3_value_int((int64_t)enc->in_nets.len),
                                         odin3_value_int((int64_t)enc->out_nets.len),
                                         {0}};
    st = odin3_proc_body_pack(arena, body, &params[ODIN3_PROC_P_BODY]);
    if (st == ODIN3_OK) {
        odin3_node_spec spec = {type, 0, enc->pw->prov, params, N_PROC_PARAMS};
        odin3_netvec ports[2] = {{enc->in_nets.data, (uint32_t)enc->in_nets.len},
                                 {enc->out_nets.data, (uint32_t)enc->out_nets.len}};
        st = odin3_node_create_connected(enc->mctx->module, &spec, ports, out);
    }
    odin3_arena_destroy(arena); /* node_create copied the parameters */
    return st;
}

/* Takes a new node out again: disconnected (never allocates), then deleted when it can be. */
static void enc_undo(odin3_module *module, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        (void)odin3_pin_disconnect(module, (odin3_pin_id){pins.first.v + i});
    }
    (void)odin3_node_delete(module, node);
}

/* The attributes onto the new node, then the old node deleted; on failure the new one goes. */
static odin3_status enc_commit(encoder *enc, odin3_node_id node) {
    odin3_module *module = enc->mctx->module;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < enc->pw->attrs.len; i++) {
        const odin3_pw_attr *attr = odin3_vec_cat(&enc->pw->attrs, i);
        st = odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, attr->key, attr->value);
    }
    if (st == ODIN3_OK && odin3_node_valid(enc->pw->node) &&
        odin3_node_live(module, enc->pw->node)) {
        st = odin3_node_delete(module, enc->pw->node);
    }
    if (st != ODIN3_OK) {
        enc_undo(module, node);
        return st;
    }
    enc->pw->node = node;
    return ODIN3_OK;
}

/* A live pw->node must be a process: encode deletes it. */
static odin3_status check_old_node(const encoder *enc) {
    const odin3_module *module = enc->mctx->module;
    odin3_node_id node = enc->pw->node;
    if (!odin3_node_valid(node) || !odin3_node_live(module, node)) {
        return ODIN3_OK;
    }
    const odin3_celltype_def *def =
        odin3_celltype_get(enc->mctx->design, odin3_node_type(module, node));
    if (def == NULL || (def->flags & ODIN3_CT_PROCESS) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: node %u is not a process", node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The body checked as the node's verify will (so the reason is logged here). */
static odin3_status enc_verify(const encoder *enc, const odin3_proc_body *body) {
    odin3_proc_widths widths = {(uint32_t)enc->in_nets.len, (uint32_t)enc->out_nets.len};
    odin3_proc_why why = {""};
    if (odin3_proc_body_verify(body, widths, &why) != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "proc: encode: %s", why.text);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

odin3_status odin3_pw_encode(odin3_proc_mctx *mctx, odin3_pw *pw) {
    encoder enc;
    enc_init(&enc, mctx, pw);
    odin3_status st = check_old_node(&enc);
    st = st == ODIN3_OK ? enc_order(&enc) : st;
    st = st == ODIN3_OK ? enc_pins(&enc) : st;
    st = st == ODIN3_OK ? enc_records(&enc) : st;
    st = st == ODIN3_OK ? enc_ranges(&enc) : st;
    st = st == ODIN3_OK ? enc_pbits(&enc) : st;
    odin3_proc_scratch_reset(mctx);
    odin3_proc_body body = enc_body(&enc);
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? enc_verify(&enc, &body) : st;
    st = st == ODIN3_OK ? enc_create(&enc, &body, &node) : st;
    st = st == ODIN3_OK ? enc_commit(&enc, node) : st;
    enc_free(&enc);
    return st;
}
