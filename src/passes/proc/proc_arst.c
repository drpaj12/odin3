/*
 * proc_arst.c — proc's arst phase (2E spec §5.4, PR-10): Yosys 0.55's proc_arst
 * (passes/proc/proc_arst.cc) without -global_arst, on a working form.
 *
 * - check_signal follows each net's driver (odin3_net_driver, O(1) a hop), where Yosys scans every
 *   cell of the module for each test, and gives up after the walk cap (§16).
 * - apply_reset's assign view is Yosys's SigMap over the process's own prune promotions (§4),
 *   then the net map; apply_const walks the tree with an explicit stack.
 * - eliminate_const removes the reset branch and cleans the root at depth 1 (§5.1).
 */
#include "passes/proc/proc_internal.h"

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "odin3/odin3.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VIEW_ROOT UINT32_MAX /* a view element without a parent */
#define NO_CASE UINT32_MAX   /* no matched case */
enum { VIEW_INITIAL_CAP = 16 };

static bool is_net(odin3_pw_bit bit) {
    return (bit & ODIN3_PW_CONST) == 0;
}

static odin3_pw_case *case_at(odin3_pw *pw, uint32_t index) {
    return odin3_vec_at(&pw->cases, index);
}

static odin3_pw_switch *switch_at(odin3_pw *pw, uint32_t index) {
    return odin3_vec_at(&pw->switches, index);
}

static odin3_pw_sync *sync_at(odin3_pw *pw, uint32_t index) {
    return odin3_vec_at(&pw->syncs, index);
}

/* Element pos of a vector of uint32_t. */
static uint32_t u32_at(const odin3_vec *vec, size_t pos) {
    return *(const uint32_t *)odin3_vec_cat(vec, pos);
}

/* --- check_signal (§5.4's table) ----------------------------------------------------------- */

/* The hops of the last walk (proc_test.h). */
static uint32_t g_hops;

uint32_t odin3_proc_test_signal_hops(void) {
    return g_hops;
}

/* How a driver passes its input on. */
typedef enum arst_link { LINK_NONE, LINK_SAME, LINK_INVERT, LINK_EQ, LINK_NE } arst_link;

static arst_link link_of(const odin3_celltype_def *def) {
    static const struct {
        const char *name;
        arst_link link;
    } k_links[] = {
        {"$reduce_or", LINK_SAME},
        {"$reduce_bool", LINK_SAME},
        {"$logic_not", LINK_INVERT},
        {"$not", LINK_INVERT},
        {"$eq", LINK_EQ},
        {"$ne", LINK_NE},
    };
    for (size_t i = 0; def != NULL && i < sizeof k_links / sizeof k_links[0]; i++) {
        if (strcmp(def->name, k_links[i].name) == 0) {
            return k_links[i].link;
        }
    }
    return LINK_NONE;
}

/* The index of def's port named name; def->n_ports when it has none. */
static uint32_t port_index(const odin3_celltype_def *def, const char *name) {
    uint32_t i = 0;
    while (i < def->n_ports && strcmp(def->ports[i].name, name) != 0) {
        i++;
    }
    return i;
}

/* The working form's bit of net (decode's reading): a constant net's constant, else the net. */
static odin3_pw_bit net_bit(const odin3_module *module, odin3_net_id net) {
    static const odin3_pw_bit k_const_bits[] = {0, ODIN3_PW_S0, ODIN3_PW_S1, ODIN3_PW_SX,
                                                ODIN3_PW_SZ};
    odin3_const value = odin3_net_const_value(module, net);
    return value != ODIN3_CONST_NONE ? k_const_bits[value] : net.v;
}

/* The bit on port `port` of node; false unless the port is one connected bit. */
static bool port_bit(const odin3_module *module, odin3_node_id node, uint32_t port,
                     odin3_pw_bit *out) {
    odin3_pinslice pins = odin3_node_port(module, node, port);
    odin3_net_id net = pins.count == 1 ? odin3_pin_net(module, pins.first) : (odin3_net_id){0};
    if (!odin3_net_valid(net)) {
        return false;
    }
    *out = net_bit(module, net);
    return true;
}

/* Yosys's is_fully_const on port `port` of node (every bit on a constant net); *one: as_bool. */
static bool port_const(const odin3_module *module, odin3_node_id node, uint32_t port, bool *one) {
    odin3_pinslice pins = odin3_node_port(module, node, port);
    *one = false;
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_net_id net = odin3_pin_net(module, (odin3_pin_id){pins.first.v + i});
        odin3_const value =
            odin3_net_valid(net) ? odin3_net_const_value(module, net) : ODIN3_CONST_NONE;
        if (value == ODIN3_CONST_NONE) {
            return false;
        }
        *one = *one || value == ODIN3_CONST_1;
    }
    return true;
}

/*
 * One hop back from net cur through its driver: *next, and whether the hop inverts. False when the
 * driver is none of the table's cells, cur is not its one-bit Y, or the port read on is not one
 * bit wide.
 */
static bool hop(const odin3_module *module, odin3_pw_bit cur, odin3_pw_bit *next, bool *flip) {
    odin3_pin_id pin = odin3_net_driver(module, (odin3_net_id){cur});
    if (!odin3_pin_valid(pin)) {
        return false;
    }
    odin3_node_id node = odin3_pin_node(module, pin);
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node));
    arst_link link = link_of(def);
    if (link == LINK_NONE || odin3_pin_port(module, pin) != port_index(def, "Y") ||
        odin3_node_port(module, node, port_index(def, "Y")).count != 1) {
        return false;
    }
    uint32_t port_a = port_index(def, "A");
    *flip = link == LINK_INVERT;
    if (link == LINK_SAME || link == LINK_INVERT) {
        return port_bit(module, node, port_a, next);
    }
    uint32_t port_b = port_index(def, "B");
    uint32_t other = port_b; /* a constant A: on to B (Yosys tests A first) */
    bool one = false;
    if (!port_const(module, node, port_a, &one)) {
        if (!port_const(module, node, port_b, &one)) {
            return false;
        }
        other = port_a;
    }
    *flip = link == LINK_EQ ? !one : one;
    return port_bit(module, node, other, next);
}

/* check_signal on two working-form bits (constants compare as RTLIL constants do). */
static bool check_bit(odin3_proc_mctx *mctx, odin3_pw_bit signal, odin3_pw_bit ref,
                      bool *polarity) {
    uint32_t cap = odin3_proc_limits_now().max_signal_walk;
    odin3_pw_bit cur = odin3_proc_find(mctx, signal);
    odin3_pw_bit want = odin3_proc_find(mctx, ref);
    bool pol = *polarity;
    uint32_t hops = 0;
    bool found = cur == want;
    while (!found && is_net(cur) && hops < cap) {
        odin3_pw_bit next = 0;
        bool flip = false;
        if (!hop(mctx->module, cur, &next, &flip)) {
            break;
        }
        pol = pol != flip;
        cur = odin3_proc_find(mctx, next);
        hops++;
        found = cur == want;
    }
    g_hops = hops;
    if (found) {
        *polarity = pol;
    }
    return found;
}

bool odin3_proc_check_signal(odin3_proc_mctx *mctx, odin3_proc_sigref sr, bool *polarity) {
    if (mctx == NULL || polarity == NULL || !odin3_net_valid(sr.signal) ||
        !odin3_net_valid(sr.ref)) {
        return false;
    }
    return check_bit(mctx, net_bit(mctx->module, sr.signal), net_bit(mctx->module, sr.ref),
                     polarity);
}

/* check_signal on a working form's signal: false unless it is one bit (Yosys's size test). */
static bool check_sig(odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig sig, odin3_pw_bit ref,
                      bool *polarity) {
    return sig.len == 1 && check_bit(mctx, odin3_pw_sig_bits(pw, sig)[0], ref, polarity);
}

/* --- the assign view: Yosys's SigMap over the process's promotions ------------------------- */

/* One bit of the view's union-find (Yosys's mfp): its parent, VIEW_ROOT for a class's root. */
typedef struct view_elem {
    odin3_pw_bit bit;
    uint32_t parent;
} view_elem;

typedef struct arst_view {
    odin3_u64map *index; /* bit -> its element; NULL until the view is built */
    odin3_vec elems;     /* view_elem */
    bool built;
} arst_view;

static view_elem *elem_at(arst_view *view, uint32_t index) {
    return odin3_vec_at(&view->elems, index);
}

/* mfp::ifind: the root of element i, with path compression (the root does not change). */
static uint32_t view_root(arst_view *view, uint32_t elem) {
    uint32_t root = elem;
    while (elem_at(view, root)->parent != VIEW_ROOT) {
        root = elem_at(view, root)->parent;
    }
    while (elem != root) {
        uint32_t next = elem_at(view, elem)->parent;
        elem_at(view, elem)->parent = root;
        elem = next;
    }
    return root;
}

/* mfp::ipromote: element i becomes its class's root. */
static void view_promote(arst_view *view, uint32_t elem) {
    uint32_t cur = elem;
    while (cur != VIEW_ROOT) {
        uint32_t next = elem_at(view, cur)->parent;
        elem_at(view, cur)->parent = elem;
        cur = next;
    }
    elem_at(view, elem)->parent = VIEW_ROOT;
}

/* The element of bit, made (a root) when the view has none. ODIN3_ERR_NO_MEMORY. */
static odin3_status view_elem_of(arst_view *view, odin3_pw_bit bit, uint32_t *out) {
    uint64_t found = 0;
    if (odin3_u64map_get(view->index, bit, &found)) {
        *out = (uint32_t)found;
        return ODIN3_OK;
    }
    uint32_t index = (uint32_t)view->elems.len;
    if (odin3_vec_reserve(&view->elems, view->elems.len + 1) != ODIN3_OK ||
        odin3_u64map_put(view->index, (odin3_kv){bit, index}) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *(view_elem *)odin3_vec_push(&view->elems) = (view_elem){bit, VIEW_ROOT}; /* reserved */
    *out = index;
    return ODIN3_OK;
}

/* SigMap::add(from, to) on one bit: from's class joins to's, and a constant stays its class's
 * root. ODIN3_ERR_NO_MEMORY. */
static odin3_status view_add(arst_view *view, odin3_pw_bit from, odin3_pw_bit to) {
    uint32_t from_i = 0;
    uint32_t to_i = 0;
    odin3_status st = view_elem_of(view, from, &from_i);
    st = st == ODIN3_OK ? view_elem_of(view, to, &to_i) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    from_i = view_root(view, from_i);
    to_i = view_root(view, to_i);
    bool from_net = is_net(elem_at(view, from_i)->bit);
    bool to_net = is_net(elem_at(view, to_i)->bit);
    if (!from_net && !to_net) {
        return ODIN3_OK;
    }
    if (from_i != to_i) {
        elem_at(view, from_i)->parent = to_i;
    }
    if (!from_net) {
        view_promote(view, from_i);
    } /* Yosys also promotes to's root when it is a constant: a no-op, it is the root */
    return ODIN3_OK;
}

/* Builds the view from pw->promoted, bit by bit in promotion order, as Yosys's SigMap(mod) adds
 * the module's connections (once; nothing to build without promotions). ODIN3_ERR_NO_MEMORY. */
static odin3_status view_build(arst_view *view, const odin3_pw *pw) {
    if (view->built || pw->promoted.len == 0) {
        view->built = true;
        return ODIN3_OK;
    }
    view->index = odin3_u64map_create(VIEW_INITIAL_CAP);
    odin3_status st = view->index != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t i = 0; st == ODIN3_OK && i < pw->promoted.len; i++) {
        const odin3_pw_action *act = odin3_vec_cat(&pw->promoted, i);
        const odin3_pw_bit *lhs = odin3_pw_sig_bits(pw, act->lhs);
        const odin3_pw_bit *rhs = odin3_pw_sig_bits(pw, act->rhs);
        for (uint32_t k = 0; st == ODIN3_OK && k < act->lhs.len && k < act->rhs.len; k++) {
            st = view_add(view, lhs[k], rhs[k]);
        }
    }
    view->built = st == ODIN3_OK;
    return st;
}

static void view_free(arst_view *view) {
    odin3_u64map_destroy(view->index);
    view->index = NULL;
    odin3_vec_free(&view->elems);
}

/* --- apply_reset ----------------------------------------------------------------------------- */

/* One case to visit in apply_const's walk, as known or unknown. */
typedef struct arst_item {
    uint32_t case_id;
    bool unknown;
} arst_item;

typedef struct arst_ctx {
    odin3_proc_mctx *mctx;
    odin3_pw *pw;
    arst_view view;
    odin3_pw_bit ref; /* the root switch's signal: apply_const's and eliminate_const's ref */
    bool polarity;    /* the reset's value at ref */
    odin3_vec rspec;  /* odin3_pw_bit: the bits being resolved */
    odin3_vec rval;   /* odin3_pw_bit: their values so far */
    odin3_vec last;   /* odin3_pw_bit: rval before the round */
    odin3_vec chain;  /* uint32_t per rspec position: 1 + the next position with its net, or 0 */
    odin3_vec stack;  /* arst_item */
} arst_ctx;

/* The view of bit: through the promotions, then the net map. */
static odin3_pw_bit assign_view(arst_ctx *ar, odin3_pw_bit bit) {
    uint64_t found = 0;
    if (ar->view.index != NULL && odin3_u64map_get(ar->view.index, bit, &found)) {
        bit = elem_at(&ar->view, view_root(&ar->view, (uint32_t)found))->bit;
    }
    return odin3_proc_find(ar->mctx, bit);
}

static odin3_pw_bit *bits_of(const odin3_vec *vec) {
    return vec->data;
}

/* Sets vec (of odin3_pw_bit) to n zero bits. ODIN3_ERR_NO_MEMORY. */
static odin3_status bits_resize(odin3_vec *vec, uint32_t n) {
    if (odin3_vec_reserve(vec, n) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_clear(vec);
    for (uint32_t i = 0; i < n; i++) {
        *(odin3_pw_bit *)odin3_vec_push(vec) = 0; /* reserved */
    }
    return ODIN3_OK;
}

/* Marks rspec's nets in the scratch index (1 + a position) and chains equal bits' positions. */
static odin3_status mark_rspec(arst_ctx *ar) {
    odin3_status st = odin3_proc_scratch_grow(ar->mctx);
    st = st == ODIN3_OK ? odin3_vec_reserve(&ar->chain, ar->rspec.len) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_vec_clear(&ar->chain);
    const odin3_pw_bit *rspec = bits_of(&ar->rspec);
    for (size_t i = 0; i < ar->rspec.len; i++) {
        odin3_net_id net = {rspec[i]};
        uint32_t head = is_net(rspec[i]) ? odin3_proc_scratch_get(ar->mctx, net) : 0;
        *(uint32_t *)odin3_vec_push(&ar->chain) = head; /* reserved */
        if (is_net(rspec[i])) {
            odin3_proc_scratch_set(ar->mctx, net, (uint32_t)i + 1);
        }
    }
    return ODIN3_OK;
}

/*
 * rspec.replace(lhs, rhs, &rval) for each action of case cs, in order (rhs all SM when unknown):
 * each rspec position whose net is an lhs bit takes that bit's value. Yosys's first matching lhs
 * bit wins (the lhs is walked last to first so its write is the last), which no body can show:
 * body verify refuses an lhs that repeats a bit.
 */
static void apply_actions(arst_ctx *ar, const odin3_pw_case *cs, bool unknown) {
    const odin3_pw *pw = ar->pw;
    odin3_pw_bit *rval = bits_of(&ar->rval);
    for (size_t i = 0; i < cs->actions.len; i++) {
        const odin3_pw_action *act = odin3_vec_cat(&cs->actions, i);
        const odin3_pw_bit *lhs = odin3_pw_sig_bits(pw, act->lhs);
        const odin3_pw_bit *rhs = odin3_pw_sig_bits(pw, act->rhs);
        for (uint32_t j = act->lhs.len; j-- > 0;) {
            uint32_t pos =
                is_net(lhs[j]) ? odin3_proc_scratch_get(ar->mctx, (odin3_net_id){lhs[j]}) : 0;
            for (; pos != 0; pos = u32_at(&ar->chain, pos - 1)) {
                rval[pos - 1] = unknown ? ODIN3_PW_SM : rhs[j];
            }
        }
    }
}

/* The first case of sw with a compare equal to the one-bit want, or without compares; NO_CASE. */
static uint32_t matched_case(odin3_pw *pw, const odin3_pw_switch *sw, odin3_pw_bit want) {
    for (size_t i = 0; i < sw->cases.len; i++) {
        uint32_t case_id = u32_at(&sw->cases, i);
        const odin3_pw_case *cs = case_at(pw, case_id);
        for (size_t j = 0; j < cs->compares.len; j++) {
            odin3_pw_sig cmp = *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j);
            if (cmp.len == 1 && odin3_pw_sig_bits(pw, cmp)[0] == want) {
                return case_id;
            }
        }
        if (cs->compares.len == 0) {
            return case_id;
        }
    }
    return NO_CASE;
}

/*
 * apply_const's descent from case cs, pushed last first so the walk visits it in Yosys's order: a
 * switch whose signal passes check_signal against ref descends, as known, only into its matched
 * case (the polarity at ref is the reset's); any other switch into every case as unknown. Yosys
 * first descends into an empty-signal switch's cases with the current flag, then (check_signal
 * fails on an empty signal) again as unknown; the second visit writes every position the first
 * wrote, so only it is made.
 */
static odin3_status push_children(arst_ctx *ar, const odin3_pw_case *cs) {
    odin3_pw *pw = ar->pw;
    size_t more = 0;
    for (size_t pos = 0; pos < cs->switches.len; pos++) {
        more += switch_at(pw, u32_at(&cs->switches, pos))->cases.len;
    }
    if (odin3_vec_reserve(&ar->stack, ar->stack.len + more) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t pos = cs->switches.len; pos-- > 0;) {
        const odin3_pw_switch *sw = switch_at(pw, u32_at(&cs->switches, pos));
        bool pol = ar->polarity;
        if (check_sig(ar->mctx, pw, sw->signal, ar->ref, &pol)) {
            uint32_t match = matched_case(pw, sw, pol ? ODIN3_PW_S1 : ODIN3_PW_S0);
            if (match != NO_CASE) {
                *(arst_item *)odin3_vec_push(&ar->stack) = (arst_item){match, false};
            }
            continue;
        }
        for (size_t at = sw->cases.len; at-- > 0;) {
            *(arst_item *)odin3_vec_push(&ar->stack) = (arst_item){u32_at(&sw->cases, at), true};
        }
    }
    return ODIN3_OK;
}

/* Yosys's apply_const(root, known) on rval, rspec fixed. ODIN3_ERR_NO_MEMORY. */
static odin3_status apply_const(arst_ctx *ar) {
    odin3_status st = mark_rspec(ar);
    st = st == ODIN3_OK ? odin3_vec_reserve(&ar->stack, 1) : st;
    if (st == ODIN3_OK) {
        *(arst_item *)odin3_vec_push(&ar->stack) = (arst_item){0, false}; /* reserved */
    }
    while (st == ODIN3_OK && ar->stack.len > 0) {
        arst_item item = *(const arst_item *)odin3_vec_cat(&ar->stack, ar->stack.len - 1);
        odin3_vec_pop(&ar->stack);
        const odin3_pw_case *cs = case_at(ar->pw, item.case_id);
        apply_actions(ar, cs, item.unknown);
        st = push_children(ar, cs);
    }
    odin3_vec_clear(&ar->stack);
    odin3_proc_scratch_reset(ar->mctx);
    return st;
}

/* No bit of vec is a net (Yosys's is_fully_const: x, z and m count as constants). */
static bool bits_const(const odin3_vec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        if (is_net(((const odin3_pw_bit *)vec->data)[i])) {
            return false;
        }
    }
    return true;
}

static bool bits_have(const odin3_vec *vec, odin3_pw_bit bit) {
    for (size_t i = 0; i < vec->len; i++) {
        if (((const odin3_pw_bit *)vec->data)[i] == bit) {
            return true;
        }
    }
    return false;
}

/* E2 (settled) or E3 at the process, naming the reset's signal and the action's lhs. */
static odin3_status reset_error(arst_ctx *ar, const odin3_pw_sync *sync, const odin3_pw_action *act,
                                bool settled) {
    odin3_strbuf rst;
    odin3_strbuf lhs;
    odin3_strbuf_init(&rst);
    odin3_strbuf_init(&lhs);
    odin3_status st = odin3_proc_name_bits(ar->mctx, &sync->signal, 1, &rst);
    st = st == ODIN3_OK ? odin3_proc_name_bits(ar->mctx, odin3_pw_sig_bits(ar->pw, act->lhs),
                                               act->lhs.len, &lhs)
                        : st;
    if (st == ODIN3_OK && settled) {
        odin3_proc_error(ar->mctx, ar->pw->prov,
                         "asynchronous reset '%s' does not give '%s' a constant value", rst.data,
                         lhs.data);
    } else if (st == ODIN3_OK) {
        odin3_proc_error(ar->mctx, ar->pw->prov,
                         "asynchronous reset '%s' does not settle the value of '%s' after %" PRIu32
                         " substitutions",
                         rst.data, lhs.data, odin3_proc_limits_now().max_arst_rounds);
    }
    odin3_strbuf_free(&rst);
    odin3_strbuf_free(&lhs);
    return st == ODIN3_OK ? ODIN3_ERR_PARSE : st;
}

/* rspec = the view of rhs; rval = all SM but rspec's constant bits. ODIN3_ERR_NO_MEMORY. */
static odin3_status reset_start(arst_ctx *ar, odin3_pw_sig rhs) {
    odin3_status st = bits_resize(&ar->rspec, rhs.len);
    st = st == ODIN3_OK ? bits_resize(&ar->rval, rhs.len) : st;
    st = st == ODIN3_OK ? bits_resize(&ar->last, rhs.len) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pw_bit *rspec = bits_of(&ar->rspec);
    odin3_pw_bit *rval = bits_of(&ar->rval);
    for (uint32_t i = 0; i < rhs.len; i++) {
        rspec[i] = assign_view(ar, odin3_pw_sig_bits(ar->pw, rhs)[i]);
        rval[i] = is_net(rspec[i]) ? ODIN3_PW_SM : rspec[i];
    }
    return ODIN3_OK;
}

/*
 * Yosys's apply_reset for one action of the reset sync: rounds of apply_const, each followed by
 * the view of rval, until rval is fully constant or a round leaves it unchanged; a round cap
 * reached first is E3, an SM bit left is E2 (both ODIN3_ERR_PARSE). The action's rhs becomes rval.
 */
static odin3_status apply_reset(arst_ctx *ar, const odin3_pw_sync *sync, odin3_pw_action *act) {
    uint32_t width = act->rhs.len;
    odin3_status st = width > 0 ? reset_start(ar, act->rhs) : ODIN3_OK;
    uint32_t cap = odin3_proc_limits_now().max_arst_rounds;
    for (uint32_t round = 1; st == ODIN3_OK && width > 0; round++) {
        memcpy(ar->last.data, ar->rval.data, width * sizeof(odin3_pw_bit));
        st = apply_const(ar);
        odin3_pw_bit *rval = bits_of(&ar->rval);
        for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
            rval[i] = assign_view(ar, rval[i]);
        }
        if (st != ODIN3_OK || bits_const(&ar->rval) ||
            memcmp(ar->last.data, ar->rval.data, width * sizeof(odin3_pw_bit)) == 0) {
            break;
        }
        if (round >= cap) {
            return reset_error(ar, sync, act, false);
        }
        memcpy(ar->rspec.data, ar->rval.data, width * sizeof(odin3_pw_bit));
    }
    if (st != ODIN3_OK || width == 0) {
        return st;
    }
    if (bits_have(&ar->rval, ODIN3_PW_SM)) {
        return reset_error(ar, sync, act, true);
    }
    return odin3_pw_sig_new(ar->pw, bits_of(&ar->rval), width, &act->rhs);
}

/* --- eliminate_const and the loop ---------------------------------------------------------- */

/* Does case cs have a compare equal to the one-bit want? */
static bool has_compare(const odin3_pw *pw, const odin3_pw_case *cs, odin3_pw_bit want) {
    for (size_t j = 0; j < cs->compares.len; j++) {
        odin3_pw_sig cmp = *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j);
        if (cmp.len == 1 && odin3_pw_sig_bits(pw, cmp)[0] == want) {
            return true;
        }
    }
    return false;
}

/*
 * Yosys's eliminate_const(root, ref, pol): in the root's only switch (its signal is ref, so
 * check_signal holds at once with pol, and Yosys's descent into failing switches never runs from
 * proc_arst) every case with a compare equal to pol goes, the first other case stays without
 * compares, every later one goes, and the signal is cleared; then the root is cleaned at depth 1
 * until stable, which inlines the surviving case.
 */
static odin3_status eliminate_const(arst_ctx *ar, bool pol) {
    odin3_pw *pw = ar->pw;
    const odin3_pw_case *root = case_at(pw, 0);
    assert(root->switches.len == 1);
    odin3_pw_switch *sw = switch_at(pw, u32_at(&root->switches, 0));
    odin3_pw_bit want = pol ? ODIN3_PW_S1 : ODIN3_PW_S0;
    bool found = false;
    size_t kept = 0;
    for (size_t i = 0; i < sw->cases.len; i++) {
        uint32_t case_id = u32_at(&sw->cases, i);
        odin3_pw_case *cs = case_at(pw, case_id);
        if (found || has_compare(pw, cs, want)) {
            continue; /* unlinked; its content is stale (pw.h) */
        }
        found = true;
        odin3_vec_clear(&cs->compares);
        *(uint32_t *)odin3_vec_at(&sw->cases, kept++) = case_id;
    }
    while (sw->cases.len > kept) {
        odin3_vec_pop(&sw->cases);
    }
    sw->signal = (odin3_pw_sig){0, 0};
    return odin3_pw_clean(pw, 0, 1);
}

/* The syncs by kind, as indices into pw->syncs: level, edge and the rest (ALWAYS). */
typedef struct arst_lists {
    odin3_vec level, edge, other; /* uint32_t, each reserved for every sync */
} arst_lists;

static bool is_edge(uint8_t type) {
    return type == ODIN3_SYNC_POSEDGE || type == ODIN3_SYNC_NEGEDGE;
}

static odin3_status split_syncs(const odin3_pw *pw, arst_lists *lists) {
    size_t count = pw->syncs.len;
    if (odin3_vec_reserve(&lists->level, count) != ODIN3_OK ||
        odin3_vec_reserve(&lists->edge, count) != ODIN3_OK ||
        odin3_vec_reserve(&lists->other, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t type = ((const odin3_pw_sync *)odin3_vec_cat(&pw->syncs, i))->type;
        odin3_vec *list = &lists->other;
        if (type == ODIN3_SYNC_LEVEL0 || type == ODIN3_SYNC_LEVEL1) {
            list = &lists->level;
        } else if (is_edge(type)) {
            list = &lists->edge;
        }
        *(uint32_t *)odin3_vec_push(list) = i; /* reserved */
    }
    return ODIN3_OK;
}

/* Edge sync pos of the lists becomes a level sync (LEVEL1 from POSEDGE), appended to the level
 * list; the other edge syncs keep their order. */
static void make_level(odin3_pw *pw, arst_lists *lists, size_t pos) {
    uint32_t index = u32_at(&lists->edge, pos);
    odin3_pw_sync *sync = sync_at(pw, index);
    sync->type = sync->type == ODIN3_SYNC_POSEDGE ? ODIN3_SYNC_LEVEL1 : ODIN3_SYNC_LEVEL0;
    uint32_t *edge = lists->edge.data;
    memmove(edge + pos, edge + pos + 1, (lists->edge.len - pos - 1) * sizeof *edge);
    odin3_vec_pop(&lists->edge);
    *(uint32_t *)odin3_vec_push(&lists->level) = index; /* reserved */
}

/* The reset sync's actions: rhs = apply_reset(rhs), in order, in the assign view. */
static odin3_status reset_actions(arst_ctx *ar, odin3_pw_sync *sync) {
    odin3_status st = view_build(&ar->view, ar->pw);
    for (size_t i = 0; st == ODIN3_OK && i < sync->actions.len; i++) {
        st = apply_reset(ar, sync, odin3_vec_at(&sync->actions, i));
    }
    return st;
}

/*
 * One turn of the loop: the first edge sync whose signal the root's only switch tests. With other
 * edge syncs it is a reset (made a level sync, its values applied, its branch eliminated); alone it
 * is a VHDL-style edge test (the other branch eliminated). *found false when none is tested.
 */
static odin3_status arst_turn(arst_ctx *ar, arst_lists *lists, bool *found) {
    odin3_pw *pw = ar->pw;
    odin3_pw_sig root_sig = switch_at(pw, u32_at(&case_at(pw, 0)->switches, 0))->signal;
    *found = false;
    for (size_t k = 0; k < lists->edge.len; k++) {
        odin3_pw_sync *sync = sync_at(pw, u32_at(&lists->edge, k));
        bool pol = sync->type == ODIN3_SYNC_POSEDGE;
        if (!check_sig(ar->mctx, pw, root_sig, sync->signal, &pol)) {
            continue;
        }
        *found = true;
        ar->ref = odin3_pw_sig_bits(pw, root_sig)[0];
        ar->polarity = pol;
        if (lists->edge.len == 1) {
            return eliminate_const(ar, !pol);
        }
        make_level(pw, lists, k);
        odin3_status st = reset_actions(ar, sync);
        return st == ODIN3_OK ? eliminate_const(ar, pol) : st;
    }
    return ODIN3_OK;
}

/* The syncs reordered: level, then edge, then the rest. ODIN3_ERR_NO_MEMORY with pw unchanged. */
static odin3_status reorder(odin3_pw *pw, const arst_lists *lists) {
    odin3_vec fresh;
    odin3_vec_init(&fresh, sizeof(odin3_pw_sync));
    if (odin3_vec_reserve(&fresh, pw->syncs.len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_vec *order[] = {&lists->level, &lists->edge, &lists->other};
    for (size_t k = 0; k < sizeof order / sizeof order[0]; k++) {
        for (size_t i = 0; i < order[k]->len; i++) {
            *(odin3_pw_sync *)odin3_vec_push(&fresh) = *sync_at(pw, u32_at(order[k], i));
        }
    }
    odin3_vec_free(&pw->syncs); /* the actions moved to fresh */
    pw->syncs = fresh;
    return ODIN3_OK;
}

static void ctx_init(arst_ctx *ar, odin3_proc_mctx *mctx, odin3_pw *pw) {
    memset(ar, 0, sizeof *ar);
    ar->mctx = mctx;
    ar->pw = pw;
    odin3_vec_init(&ar->view.elems, sizeof(view_elem));
    odin3_vec_init(&ar->rspec, sizeof(odin3_pw_bit));
    odin3_vec_init(&ar->rval, sizeof(odin3_pw_bit));
    odin3_vec_init(&ar->last, sizeof(odin3_pw_bit));
    odin3_vec_init(&ar->chain, sizeof(uint32_t));
    odin3_vec_init(&ar->stack, sizeof(arst_item));
}

static void ctx_free(arst_ctx *ar) {
    view_free(&ar->view);
    odin3_vec_free(&ar->rspec);
    odin3_vec_free(&ar->rval);
    odin3_vec_free(&ar->last);
    odin3_vec_free(&ar->chain);
    odin3_vec_free(&ar->stack);
}

odin3_status odin3_pw_arst(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (mctx == NULL || pw == NULL || pw->cases.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: arst without a process");
        return ODIN3_ERR_INVALID_ARG;
    }
    arst_ctx ar;
    ctx_init(&ar, mctx, pw);
    arst_lists lists;
    odin3_vec_init(&lists.level, sizeof(uint32_t));
    odin3_vec_init(&lists.edge, sizeof(uint32_t));
    odin3_vec_init(&lists.other, sizeof(uint32_t));
    odin3_status st = split_syncs(pw, &lists);
    bool did = false;
    bool found = true;
    while (st == ODIN3_OK && found && case_at(pw, 0)->switches.len == 1) {
        st = arst_turn(&ar, &lists, &found);
        did = did || found;
    }
    if (st == ODIN3_OK && did) {
        st = reorder(pw, &lists);
    }
    odin3_vec_free(&lists.level);
    odin3_vec_free(&lists.edge);
    odin3_vec_free(&lists.other);
    ctx_free(&ar);
    return st;
}
