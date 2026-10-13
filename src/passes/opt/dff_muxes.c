/*
 * dff_muxes.c — the mux table of opt_dff's rules 10 and 11 (2F spec OP-9 §9.3): Yosys 0.55
 * passes/opt/opt_dff.cc's bit2mux and bitusers (the module's mux bits and their readers, taken
 * when the pass begins, as Yosys's are taken once per worker), the per-bit walks down single-user
 * one-select muxes, and find_muxtree_feedback_patterns on an explicit stack (no recursion).
 *
 * - Caps (agent default, spec §17 #34): a bit's pattern search gives up, finding no pattern and
 *   marking nothing, when a path holds more than ODIN3_DFF_MAX_PATH selects or the search takes
 *   more than ODIN3_DFF_MAX_FRAMES steps; a walk down muxes stops after as many steps as the
 *   module has mux bits (a mux loop cannot run forever here). The search also gives up on a mux
 *   with a constant x or z select (spec §17 #38: Yosys reads it as two independent values). A bit
 *   that gives up keeps the controls it has: a missed rewrite, never a wrong one. Rule 10's walk
 *   reads each select once (as SRST), so an x select there is a refinement and needs no guard.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/dff.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { ODIN3_DFF_MAX_PATH = 64, ODIN3_DFF_MAX_FRAMES = 4096, N_MUX_TYPES = 3 };

/* --- the snapshot ----------------------------------------------------------------------------- */

void odin3_dff_muxes_free(odin3_dff_muxes *muxes) {
    odin3_vec_free(&muxes->bits);
    odin3_u64map_destroy(muxes->index);
    muxes->index = NULL;
}

/* The bits of one mux node, into the table. */
static odin3_status add_mux_node(odin3_dff_state *ds, odin3_node_id node) {
    odin3_dff_muxes *mx = &ds->muxes;
    const odin3_celltype_def *def =
        odin3_celltype_get(ds->oc->design, odin3_node_type(ds->oc->module, node));
    odin3_dff_muxbit entry = {0,
                              node.v,
                              0,
                              0,
                              odin3_kit_port_index(def, "A"),
                              odin3_kit_port_index(def, "B"),
                              odin3_kit_port_index(def, "S")};
    odin3_pinslice ys = odin3_node_port(ds->oc->module, node, odin3_kit_port_index(def, "Y"));
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < ys.count; k++) {
        odin3_net_id net = odin3_pin_net(ds->oc->module, (odin3_pin_id){ys.first.v + k});
        if (!odin3_net_valid(net)) {
            continue;
        }
        entry.net = net.v;
        entry.bit = k;
        entry.users = odin3_kit_readers(ds->oc, net);
        odin3_dff_muxbit *slot = odin3_vec_push(&mx->bits);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (st == ODIN3_OK) {
            *slot = entry;
            st = odin3_u64map_put(mx->index, (odin3_kv){net.v, mx->bits.len - 1U});
        }
    }
    return st;
}

odin3_status odin3_dff_muxes_build(odin3_dff_state *ds) {
    static const char *const k_names[N_MUX_TYPES] = {"$mux", "$pmux", "$_MUX_"};
    odin3_dff_muxes *mx = &ds->muxes;
    mx->index = odin3_u64map_create(0);
    if (mx->index == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_celltype_id types[N_MUX_TYPES];
    for (size_t i = 0; i < N_MUX_TYPES; i++) {
        types[i] = odin3_kit_type_named(ds->oc->design, k_names[i]);
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(ds->oc->module); id++) {
        odin3_node_id node = {id};
        odin3_celltype_id type = odin3_node_live(ds->oc->module, node)
                                     ? odin3_node_type(ds->oc->module, node)
                                     : (odin3_celltype_id){0};
        for (size_t i = 0; st == ODIN3_OK && type.v != 0 && i < N_MUX_TYPES; i++) {
            st = types[i].v == type.v ? add_mux_node(ds, node) : st;
        }
    }
    return st;
}

/* A mux bit with its pins: A, B and S as the pin slices of the node. */
typedef struct mux_view {
    const odin3_dff_muxbit *mb;
    odin3_pinslice a, b, s;
} mux_view;

static odin3_net_id pin_at(const odin3_dff_state *ds, odin3_pinslice slice, uint32_t at) {
    return at < slice.count ? odin3_pin_net(ds->oc->module, (odin3_pin_id){slice.first.v + at})
                            : (odin3_net_id){0};
}

/* The table entry of the mux bit that net is the output of, as a view; false when net is none. */
static bool mux_of(const odin3_dff_state *ds, odin3_net_id net, mux_view *view) {
    uint64_t at = 0;
    if (ds->muxes.index == NULL || !odin3_net_valid(net) ||
        !odin3_u64map_get(ds->muxes.index, net.v, &at)) {
        return false;
    }
    view->mb = odin3_vec_cat(&ds->muxes.bits, (size_t)at);
    odin3_node_id node = {view->mb->node};
    view->a = odin3_node_port(ds->oc->module, node, view->mb->port_a);
    view->b = odin3_node_port(ds->oc->module, node, view->mb->port_b);
    view->s = odin3_node_port(ds->oc->module, node, view->mb->port_s);
    return odin3_node_live(ds->oc->module, node) && view->mb->bit < view->a.count &&
           view->s.count > 0 && view->b.count == view->a.count * view->s.count;
}

/* The pin of input `which` ('a' or 'b') of the mux bit for select i. */
static odin3_net_id data_net(const odin3_dff_state *ds, const mux_view *view, bool take_b,
                             uint32_t sel) {
    return take_b ? pin_at(ds, view->b, sel * view->a.count + view->mb->bit)
                  : pin_at(ds, view->a, view->mb->bit);
}

/* --- the plan --------------------------------------------------------------------------------- */

void odin3_dff_plan_init(odin3_dff_plan *plan) {
    odin3_vec_init(&plan->bits, sizeof(odin3_dff_bitplan));
    odin3_vec_init(&plan->keys, sizeof(uint64_t));
    odin3_vec_init(&plan->nodes, sizeof(uint64_t));
    odin3_vec_init(&plan->marks, sizeof(odin3_dff_xmark));
}

void odin3_dff_plan_free(odin3_dff_plan *plan) {
    odin3_vec_free(&plan->bits);
    odin3_vec_free(&plan->keys);
    odin3_vec_free(&plan->nodes);
    odin3_vec_free(&plan->marks);
}

/* One bit's findings while its walks run. */
typedef struct bit_acc {
    odin3_vec ctrls, nodes, pats; /* uint64_t: (net << 1 | pol), node IDs, encoded patterns */
    odin3_vec marks;              /* odin3_dff_xmark */
    uint32_t n_pats;
    odin3_net_id dnet;
    odin3_bit reset;
    bool merged;
} bit_acc;

static void acc_init(bit_acc *acc, odin3_net_id dnet) {
    memset(acc, 0, sizeof *acc);
    odin3_vec_init(&acc->ctrls, sizeof(uint64_t));
    odin3_vec_init(&acc->nodes, sizeof(uint64_t));
    odin3_vec_init(&acc->pats, sizeof(uint64_t));
    odin3_vec_init(&acc->marks, sizeof(odin3_dff_xmark));
    acc->dnet = dnet;
    acc->reset = ODIN3_BIT_X;
}

static void acc_free(bit_acc *acc) {
    odin3_vec_free(&acc->ctrls);
    odin3_vec_free(&acc->nodes);
    odin3_vec_free(&acc->pats);
    odin3_vec_free(&acc->marks);
}

static uint64_t ctrl_word(odin3_net_id net, bool pol) {
    return ((uint64_t)net.v << 1U) | (pol ? 1U : 0U);
}

static odin3_status push_all(odin3_vec *dst, const odin3_vec *src) {
    odin3_status st = odin3_vec_reserve(dst, dst->len + src->len);
    for (size_t k = 0; st == ODIN3_OK && k < src->len; k++) {
        st = odin3_kit_push_u64(dst, *(const uint64_t *)odin3_vec_cat(src, k));
    }
    return st;
}

static odin3_status push_all_marks(odin3_dff_plan *plan, const bit_acc *acc) {
    odin3_status st = odin3_vec_reserve(&plan->marks, plan->marks.len + acc->marks.len);
    for (size_t k = 0; st == ODIN3_OK && k < acc->marks.len; k++) {
        odin3_dff_xmark *slot = odin3_vec_push(&plan->marks);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = *(const odin3_dff_xmark *)odin3_vec_cat(&acc->marks, k);
        }
    }
    return st;
}

/* Appends the finished bit to the plan (the key is controls then patterns, the nodes sorted). */
static odin3_status plan_add(odin3_dff_plan *plan, bit_acc *acc) {
    odin3_kit_sort_unify(&acc->ctrls, 0);
    odin3_kit_sort_unify(&acc->nodes, 0);
    odin3_dff_bitplan bp = {
        acc->dnet,  (uint32_t)plan->keys.len, 0, (uint32_t)plan->nodes.len, 0, acc->reset,
        acc->merged};
    odin3_status st = odin3_vec_reserve(&plan->bits, plan->bits.len + 1U);
    if (acc->merged) {
        st = st == ODIN3_OK ? odin3_kit_push_u64(&plan->keys, acc->ctrls.len) : st;
        st = st == ODIN3_OK ? push_all(&plan->keys, &acc->ctrls) : st;
        st = st == ODIN3_OK ? odin3_kit_push_u64(&plan->keys, acc->n_pats) : st;
        st = st == ODIN3_OK ? push_all(&plan->keys, &acc->pats) : st;
        st = st == ODIN3_OK ? push_all(&plan->nodes, &acc->nodes) : st;
        st = st == ODIN3_OK ? push_all_marks(plan, acc) : st;
    }
    bp.key_len = (uint32_t)plan->keys.len - bp.key_off;
    bp.node_len = (uint32_t)plan->nodes.len - bp.node_off;
    odin3_dff_bitplan *slot = st == ODIN3_OK ? odin3_vec_push(&plan->bits) : NULL;
    if (slot != NULL) {
        *slot = bp;
    }
    return st;
}

/* --- the walk down one-select muxes ------------------------------------------------------------
 */

/* One single-user, one-select mux bit: its select and data nets. */
typedef struct eat_step {
    mux_view view;
    odin3_net_id s, a, b;
} eat_step;

static bool eat_next(const odin3_dff_state *ds, odin3_net_id dnet, eat_step *step) {
    if (!mux_of(ds, dnet, &step->view) || step->view.mb->users != 1 || step->view.s.count != 1) {
        return false;
    }
    step->s = pin_at(ds, step->view.s, 0);
    step->a = data_net(ds, &step->view, false, 0);
    step->b = data_net(ds, &step->view, true, 0);
    return true;
}

/* 0 or 1 when net carries that constant bit, -1 otherwise (x and z included). */
static int const01(const odin3_dff_state *ds, odin3_net_id net) {
    odin3_const val = odin3_dff_net_const(ds, net);
    if (val == ODIN3_CONST_0 || val == ODIN3_CONST_1) {
        return val == ODIN3_CONST_1 ? 1 : 0;
    }
    return -1;
}

/* What one SRST step does: stop, or reset by the select being true / false and go on to next. */
typedef struct srst_move {
    int kind; /* 0 stop, 1 reset when S, 2 reset when !S */
    odin3_net_id next;
    odin3_bit reset;
} srst_move;

static srst_move srst_move_of(const odin3_dff_state *ds, const eat_step *step, odin3_net_id qnet,
                              odin3_bit reset) {
    int a_val = const01(ds, step->a);
    int b_val = const01(ds, step->b);
    srst_move stop = {0, step->a, reset};
    if (a_val >= 0 && b_val >= 0) { /* the memory write-enable pattern: left to the EN rule */
        return stop;
    }
    bool open = reset == ODIN3_BIT_X;
    if (b_val >= 0 && (open || (int)reset == b_val)) {
        /* a mux with Q on the other side is a clock enable: the EN rule has it */
        srst_move move = {1, step->a, (odin3_bit)b_val};
        return step->a.v == qnet.v ? stop : move;
    }
    if (a_val >= 0 && (open || (int)reset == a_val)) {
        srst_move move = {2, step->b, (odin3_bit)a_val};
        return step->b.v == qnet.v ? stop : move;
    }
    return stop;
}

/*
 * A walk that ran out its step cap (the module's mux bit count) went round a mux loop, and where it
 * stops depends on that count, which changes with muxes anywhere in the module: the FF journals
 * itself, so opt_dff's incremental mode (dff_dirty.c) visits it again.
 */
static void note_guard(const odin3_dff_state *ds, size_t guard) {
    if (guard > ds->muxes.bits.len && !ds->checking) {
        odin3_module_journal_add(ds->oc->module, ODIN3_JOURNAL_NODE, ds->cell.v);
    }
}

/* Rule 10's walk for one bit: the muxes with a constant on one side. */
static odin3_status srst_walk(const odin3_dff_state *ds, uint32_t idx, bit_acc *acc) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_net_id qnet = odin3_ff_net_at(&ff->sig_q, idx);
    odin3_bit reset = ff->has_srst ? odin3_ff_bit_at(&ff->val_srst, idx) : ODIN3_BIT_X;
    odin3_net_id dnet = acc->dnet;
    eat_step step;
    odin3_status st = ODIN3_OK;
    size_t guard = 0;
    for (; st == ODIN3_OK && guard <= ds->muxes.bits.len && eat_next(ds, dnet, &step); guard++) {
        srst_move move = srst_move_of(ds, &step, qnet, reset);
        if (move.kind == 0) {
            break;
        }
        reset = move.reset;
        dnet = move.next;
        st = odin3_kit_push_u64(&acc->ctrls, ctrl_word(step.s, move.kind == 1));
        st = st == ODIN3_OK ? odin3_kit_push_u64(&acc->nodes, step.view.mb->node) : st;
    }
    note_guard(ds, guard);
    acc->dnet = dnet;
    acc->reset = reset;
    return st;
}

odin3_status odin3_dff_plan_srst_bit(odin3_dff_state *ds, odin3_dff_plan *plan, uint32_t idx) {
    const odin3_ffdata *ff = &ds->ff;
    bit_acc acc;
    acc_init(&acc, odin3_ff_net_at(&ff->sig_d, idx));
    odin3_status st = srst_walk(ds, idx, &acc);
    acc.merged = st == ODIN3_OK && acc.ctrls.len > 0;
    if (acc.merged && ff->has_srst) {
        st = odin3_kit_push_u64(&acc.ctrls, ctrl_word(ff->sig_srst, ff->pol_srst));
    }
    if (!acc.merged) {
        acc.dnet = odin3_ff_net_at(&ff->sig_d, idx);
    }
    st = st == ODIN3_OK ? plan_add(plan, &acc) : st;
    acc_free(&acc);
    return st;
}

/* --- the feedback search ------------------------------------------------------------------------
 */

typedef struct frame {
    uint32_t net, off, len;
} frame;

typedef struct search {
    const odin3_dff_state *ds;
    odin3_net_id qnet;
    odin3_vec stack; /* frame */
    odin3_vec paths; /* uint64_t: sorted (net << 1 | value) words, one path after another */
    odin3_vec pats;  /* uint64_t: each pattern as its length then its words */
    odin3_vec order; /* uint32_t: the offsets in pats of the patterns, sorted */
    odin3_vec marks; /* odin3_dff_xmark */
    odin3_vec seen;  /* uint64_t: the muxes the walk went through */
    uint32_t frames;
    bool aborted;
} search;

static void search_init(search *sr, const odin3_dff_state *ds, odin3_net_id qnet) {
    memset(sr, 0, sizeof *sr);
    sr->ds = ds;
    sr->qnet = qnet;
    odin3_vec_init(&sr->stack, sizeof(frame));
    odin3_vec_init(&sr->paths, sizeof(uint64_t));
    odin3_vec_init(&sr->pats, sizeof(uint64_t));
    odin3_vec_init(&sr->order, sizeof(uint32_t));
    odin3_vec_init(&sr->marks, sizeof(odin3_dff_xmark));
    odin3_vec_init(&sr->seen, sizeof(uint64_t));
}

static void search_free(search *sr) {
    odin3_vec_free(&sr->stack);
    odin3_vec_free(&sr->paths);
    odin3_vec_free(&sr->pats);
    odin3_vec_free(&sr->order);
    odin3_vec_free(&sr->marks);
    odin3_vec_free(&sr->seen);
}

static uint64_t word_at(const odin3_vec *vec, size_t idx) {
    return *(const uint64_t *)odin3_vec_cat(vec, idx);
}

/* True when the path fr holds a select on net; *value its value. */
static bool path_find(const search *sr, frame fr, odin3_net_id net, bool *value) {
    size_t lo = 0;
    size_t hi = fr.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        uint64_t word = word_at(&sr->paths, fr.off + mid);
        if ((word >> 1U) == net.v) {
            *value = (word & 1U) != 0;
            return true;
        }
        if ((word >> 1U) < net.v) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return false;
}

/* A new path: fr's with net set to value (replacing it); *out = it. */
static odin3_status path_with(search *sr, frame fr, odin3_net_id net, bool value, frame *out) {
    out->net = fr.net;
    out->off = (uint32_t)sr->paths.len;
    odin3_status st = odin3_vec_reserve(&sr->paths, sr->paths.len + fr.len + 1U);
    bool placed = false;
    for (uint32_t k = 0; st == ODIN3_OK && k < fr.len; k++) {
        uint64_t word = word_at(&sr->paths, fr.off + k);
        if (!placed && (word >> 1U) >= net.v) {
            st = odin3_kit_push_u64(&sr->paths, ctrl_word(net, value));
            placed = true;
        }
        if ((word >> 1U) != net.v) {
            st = st == ODIN3_OK ? odin3_kit_push_u64(&sr->paths, word) : st;
        }
    }
    if (st == ODIN3_OK && !placed) {
        st = odin3_kit_push_u64(&sr->paths, ctrl_word(net, value));
    }
    out->len = (uint32_t)sr->paths.len - out->off;
    return st;
}

/* Orders the pattern at offset `at` of pats against the path fr: words first, then the length. */
static int cmp_pattern(const search *sr, uint32_t at, frame fr) {
    uint32_t len = (uint32_t)word_at(&sr->pats, at);
    uint32_t common = len < fr.len ? len : fr.len;
    for (uint32_t k = 0; k < common; k++) {
        uint64_t have = word_at(&sr->pats, (size_t)at + 1U + k);
        uint64_t want = word_at(&sr->paths, fr.off + k);
        if (have != want) {
            return have < want ? -1 : 1;
        }
    }
    if (len == fr.len) {
        return 0;
    }
    return len < fr.len ? -1 : 1;
}

/* The path fr is a pattern (a set: a repeat adds nothing). */
static odin3_status record_pattern(search *sr, frame fr) {
    size_t lo = 0;
    size_t hi = sr->order.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        int order = cmp_pattern(sr, *(const uint32_t *)odin3_vec_cat(&sr->order, mid), fr);
        if (order == 0) {
            return ODIN3_OK;
        }
        lo = order < 0 ? mid + 1U : lo;
        hi = order < 0 ? hi : mid;
    }
    uint32_t at = (uint32_t)sr->pats.len;
    odin3_status st = odin3_vec_reserve(&sr->pats, sr->pats.len + 1U + fr.len);
    st = st == ODIN3_OK ? odin3_kit_push_u64(&sr->pats, fr.len) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < fr.len; k++) {
        st = odin3_kit_push_u64(&sr->pats, word_at(&sr->paths, fr.off + k));
    }
    st = st == ODIN3_OK ? odin3_kit_push_u32(&sr->order, 0) : st;
    if (st == ODIN3_OK) {
        uint32_t *data = sr->order.data;
        memmove(data + lo + 1U, data + lo, (sr->order.len - 1U - lo) * sizeof *data);
        data[lo] = at;
    }
    return st;
}

/* Input k of slice (a mux pin) read Q: it takes x once the groups are built. */
static odin3_status mark_q(search *sr, odin3_pinslice slice, uint32_t at) {
    odin3_dff_xmark *slot = odin3_vec_push(&sr->marks);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_dff_xmark){(odin3_pin_id){slice.first.v + at}, sr->qnet};
    return ODIN3_OK;
}

/* Pushes the walk into the data input `net` (pin k of slice) under path. */
static odin3_status push_branch(search *sr, odin3_net_id net, frame path, odin3_pinslice slice,
                                uint32_t at) {
    frame *slot = odin3_vec_push(&sr->stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (frame){net.v, path.off, path.len};
    return net.v == sr->qnet.v ? mark_q(sr, slice, at) : ODIN3_OK;
}

/* The index of a select of the mux that the path fr holds true, false when there is none. */
static bool known_true(const search *sr, frame fr, const mux_view *view, uint32_t *which) {
    for (uint32_t i = 0; i < view->s.count; i++) {
        bool value = false;
        if (path_find(sr, fr, pin_at(sr->ds, view->s, i), &value) && value) {
            *which = i;
            return true;
        }
    }
    return false;
}

/* No select is known: every select not on the path is tried true, and A under all of them false. */
static odin3_status expand_open(search *sr, frame fr, const mux_view *view) {
    frame other = fr;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && !sr->aborted && i < view->s.count; i++) {
        odin3_net_id sel = pin_at(sr->ds, view->s, i);
        bool value = false;
        if (path_find(sr, fr, sel, &value)) {
            continue;
        }
        frame here = fr;
        st = path_with(sr, fr, sel, true, &here);
        st = st == ODIN3_OK ? path_with(sr, other, sel, false, &other) : st;
        sr->aborted = here.len > ODIN3_DFF_MAX_PATH;
        st = st == ODIN3_OK && !sr->aborted
                 ? push_branch(sr, data_net(sr->ds, view, true, i), here, view->b,
                               i * view->a.count + view->mb->bit)
                 : st;
    }
    return st == ODIN3_OK && !sr->aborted
               ? push_branch(sr, data_net(sr->ds, view, false, 0), other, view->a, view->mb->bit)
               : st;
}

/* True when a select of the mux is a constant x or z: it would be read twice (in the pattern and
 * in the mux) as two independent values (spec §17 #38). 0 and 1 are one value everywhere. */
static bool undefined_select(const search *sr, const mux_view *view) {
    for (uint32_t i = 0; i < view->s.count; i++) {
        odin3_const val = odin3_dff_net_const(sr->ds, pin_at(sr->ds, view->s, i));
        if (val == ODIN3_CONST_X || val == ODIN3_CONST_Z) {
            return true;
        }
    }
    return false;
}

/* One frame of find_muxtree_feedback_patterns. */
static odin3_status expand(search *sr, frame fr) {
    mux_view view;
    if (fr.net == sr->qnet.v) {
        return record_pattern(sr, fr);
    }
    if (!mux_of(sr->ds, (odin3_net_id){fr.net}, &view) || view.mb->users > 1) {
        return ODIN3_OK;
    }
    if (undefined_select(sr, &view)) {
        sr->aborted = true; /* the bit keeps the enables it has (as past the path cap, #34) */
        return ODIN3_OK;
    }
    odin3_status st = odin3_kit_push_u64(&sr->seen, view.mb->node);
    uint32_t which = 0;
    if (st == ODIN3_OK && known_true(sr, fr, &view, &which)) {
        return push_branch(sr, data_net(sr->ds, &view, true, which), fr, view.b,
                           which * view.a.count + view.mb->bit);
    }
    return st == ODIN3_OK ? expand_open(sr, fr, &view) : st;
}

static odin3_status run_search(search *sr, odin3_net_id dnet) {
    frame *root = odin3_vec_push(&sr->stack);
    if (root == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *root = (frame){dnet.v, 0, 0};
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && sr->stack.len > 0 && !sr->aborted) {
        frame fr = *(const frame *)odin3_vec_cat(&sr->stack, sr->stack.len - 1U);
        odin3_vec_pop(&sr->stack);
        sr->frames++;
        sr->aborted = sr->frames > ODIN3_DFF_MAX_FRAMES;
        st = sr->aborted ? st : expand(sr, fr);
    }
    return st;
}

/* The patterns of a finished search (sorted) into the bit. */
static odin3_status keep_patterns(const search *sr, bit_acc *acc) {
    odin3_status st = odin3_vec_reserve(&acc->pats, sr->pats.len);
    for (size_t k = 0; st == ODIN3_OK && k < sr->order.len; k++) {
        uint32_t at = *(const uint32_t *)odin3_vec_cat(&sr->order, k);
        size_t len = (size_t)word_at(&sr->pats, at);
        for (size_t wi = 0; st == ODIN3_OK && wi <= len; wi++) {
            st = odin3_kit_push_u64(&acc->pats, word_at(&sr->pats, at + wi));
        }
        acc->n_pats++;
    }
    return st;
}

/* The finished search into the bit: its patterns, the muxes it went through, the marks. A search
 * that gave up or found nothing keeps nothing. */
static odin3_status search_keep(const search *sr, bit_acc *acc) {
    if (sr->aborted || sr->order.len == 0) {
        return ODIN3_OK;
    }
    odin3_status st = keep_patterns(sr, acc);
    st = st == ODIN3_OK ? push_all(&acc->nodes, &sr->seen) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(&acc->marks, acc->marks.len + sr->marks.len) : st;
    for (size_t k = 0; st == ODIN3_OK && k < sr->marks.len; k++) {
        odin3_dff_xmark *slot = odin3_vec_push(&acc->marks);
        st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = *(const odin3_dff_xmark *)odin3_vec_cat(&sr->marks, k);
        }
    }
    return st;
}

/* --- rule 11's bit ------------------------------------------------------------------------------
 */

/* The simple enables: muxes with Q on one side (Yosys's "eat up as many simple muxes"). */
static odin3_status en_walk(const odin3_dff_state *ds, odin3_net_id qnet, bit_acc *acc) {
    eat_step step;
    odin3_status st = ODIN3_OK;
    size_t guard = 0;
    for (; st == ODIN3_OK && guard <= ds->muxes.bits.len && eat_next(ds, acc->dnet, &step);
         guard++) {
        bool hold_a = step.a.v == qnet.v;
        if (!hold_a && step.b.v != qnet.v) {
            break;
        }
        acc->dnet = hold_a ? step.b : step.a;
        st = odin3_kit_push_u64(&acc->ctrls, ctrl_word(step.s, hold_a));
        st = st == ODIN3_OK ? odin3_kit_push_u64(&acc->nodes, step.view.mb->node) : st;
    }
    note_guard(ds, guard);
    return st;
}

odin3_status odin3_dff_plan_en_bit(odin3_dff_state *ds, odin3_dff_plan *plan, uint32_t idx) {
    const odin3_ffdata *ff = &ds->ff;
    odin3_net_id qnet = odin3_ff_net_at(&ff->sig_q, idx);
    bit_acc acc;
    acc_init(&acc, odin3_ff_net_at(&ff->sig_d, idx));
    search sr;
    search_init(&sr, ds, qnet);
    odin3_status st = en_walk(ds, qnet, &acc);
    st = st == ODIN3_OK ? run_search(&sr, acc.dnet) : st;
    st = st == ODIN3_OK ? search_keep(&sr, &acc) : st;
    acc.merged = st == ODIN3_OK && (acc.n_pats > 0 || acc.ctrls.len > 0);
    if (st == ODIN3_OK && acc.merged && ff->has_ce) {
        st = odin3_kit_push_u64(&acc.ctrls, ctrl_word(ff->sig_ce, ff->pol_ce));
    }
    st = st == ODIN3_OK ? plan_add(plan, &acc) : st;
    search_free(&sr);
    acc_free(&acc);
    return st;
}
