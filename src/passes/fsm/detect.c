/*
 * detect.c — fsm_detect (spec D-1 to D-7; Yosys 0.55 passes/fsm/fsm_detect.cc): marks the wires
 * that look like FSM state registers with fsm_encoding = "auto".
 *
 * The mux-tree check (check_state_mux_tree, :37-89) runs on an explicit frame stack, with the
 * memo of Yosys's mux_tree_cache and its recursion_monitor (a loop warns, :62-66).
 */
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/ceval.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/index.h"
#include "passes/fsm/sigcache.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LABEL_MAX = 160 };

typedef enum mt_state { MT_START, MT_DRIVERS, MT_A_BACK, MT_B_NEXT, MT_B_BACK } mt_state;

/* One signal being checked: its bits in the pool, its drivers, the mux in hand. */
typedef struct mt_frame {
    uint32_t sig_off, sig_len;
    uint32_t pool_mark, drv_mark, drv_n, drv_i;
    uint32_t cell, a_off, a_n, b_off, b_n, b_i;
    uint8_t state;
} mt_frame;

typedef struct detect {
    odin3_fsm_mod *fm;
    odin3_fsm_sigcache cache;
    odin3_vec pool, drv, frames, mtcells;
    uint8_t *in_mt, *monitor; /* per node */
} detect;

/* The facts of one candidate (Yosys's looks_like_* and friends). */
typedef struct verdict {
    bool state_reg, good_users, self_reset;
} verdict;

static void detect_free(detect *det) {
    odin3_fsm_sigcache_free(&det->cache);
    odin3_vec_free(&det->pool);
    odin3_vec_free(&det->drv);
    odin3_vec_free(&det->frames);
    odin3_vec_free(&det->mtcells);
    odin3_util_free(det->in_mt);
    odin3_util_free(det->monitor);
    memset(det, 0, sizeof *det);
}

static odin3_status detect_init(detect *det, odin3_fsm_mod *fm) {
    memset(det, 0, sizeof *det);
    det->fm = fm;
    odin3_vec_init(&det->pool, sizeof(odin3_fsm_sig));
    odin3_vec_init(&det->drv, sizeof(uint32_t));
    odin3_vec_init(&det->frames, sizeof(mt_frame));
    odin3_vec_init(&det->mtcells, sizeof(uint32_t));
    det->in_mt = odin3_util_calloc((size_t)fm->ix.node_end + 1);
    det->monitor = odin3_util_calloc((size_t)fm->ix.node_end + 1);
    odin3_status st = odin3_fsm_sigcache_init(&det->cache);
    if (st == ODIN3_OK && (det->in_mt == NULL || det->monitor == NULL)) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        detect_free(det);
    }
    return st;
}

/* --- types and signals ------------------------------------------------------------------------ */

static bool is_type(const odin3_fsm_mod *fm, uint32_t node, uint32_t type) {
    return type != 0 && odin3_fsm_ix_type(&fm->ix, node) == type;
}

static bool is_mux(const odin3_fsm_mod *fm, uint32_t node) {
    const odin3_fsm_types *types = &fm->ix.types;
    return is_type(fm, node, types->mux) || is_type(fm, node, types->pmux);
}

static bool sig_all_const(odin3_fsm_sigref sig) {
    for (uint32_t i = 0; i < sig.len; i++) {
        if (!odin3_fsm_sig_is_const(sig.sig[i])) {
            return false;
        }
    }
    return true;
}

static bool sig_equal(odin3_fsm_sigref lhs, odin3_fsm_sigref rhs) {
    return lhs.len == rhs.len &&
           (lhs.len == 0 || memcmp(lhs.sig, rhs.sig, (size_t)lhs.len * sizeof *lhs.sig) == 0);
}

static bool sig_at_port(const odin3_fsm_index *ix, odin3_fsm_sigref sig) {
    for (uint32_t i = 0; i < sig.len; i++) {
        if (!odin3_fsm_sig_is_const(sig.sig[i]) && odin3_fsm_ix_port_net(ix, sig.sig[i])) {
            return true;
        }
    }
    return false;
}

/* The bits of a vec of signal bits as a signal. */
static odin3_fsm_sigref vec_sig(const odin3_vec *vec) {
    odin3_fsm_sigref ref = {vec->data, (uint32_t)vec->len};
    return ref;
}

/* --- the mux-tree check ------------------------------------------------------------------------
 */

/* The loop of check_tree: a child's answer waits in ret until its parent takes it. */
typedef struct mt_run {
    bool have_ret, ret;
    odin3_fsm_sigref old_sig;
} mt_run;

/* The tree check's request: the old signal (Q) and the signal under test (D). */
typedef struct tree_req {
    odin3_fsm_sigref old_sig;
    odin3_fsm_sigref sig;
} tree_req;

static mt_frame *top(detect *det) {
    return odin3_vec_at(&det->frames, det->frames.len - 1);
}

static odin3_fsm_sig *pool_at(detect *det, uint32_t off) {
    odin3_fsm_sig *pool = det->pool.data;
    return pool + off;
}

/* Where a frame's signal comes from: the caller's array, or bits already in the pool. */
typedef struct check_src {
    const odin3_fsm_sig *ext; /* NULL: the pool at off */
    uint32_t off, len;
} check_src;

/* Pushes a frame for src, copying its bits into the pool (the pool may move while it grows). */
static odin3_status push_check(detect *det, check_src src) {
    mt_frame *fr = odin3_vec_push(&det->frames);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    fr->pool_mark = (uint32_t)det->pool.len;
    fr->drv_mark = (uint32_t)det->drv.len;
    fr->sig_off = fr->pool_mark;
    fr->sig_len = src.len;
    for (uint32_t i = 0; i < src.len; i++) {
        odin3_fsm_sig bit = src.ext != NULL ? src.ext[i] : pool_at(det, src.off)[i];
        if (odin3_kit_push_u32(&det->pool, bit) != ODIN3_OK) {
            odin3_kit_truncate(&det->pool, fr->pool_mark);
            odin3_vec_pop(&det->frames);
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* Drops the top frame, handing `value` to its parent. */
static void pop_check(detect *det, mt_run *run, bool value) {
    const mt_frame *fr = top(det);
    odin3_kit_truncate(&det->pool, fr->pool_mark);
    odin3_kit_truncate(&det->drv, fr->drv_mark);
    odin3_vec_pop(&det->frames);
    run->have_ret = true;
    run->ret = value;
}

/* Resolves the top frame with `value`: the memo, then pop. */
static odin3_status resolve(detect *det, mt_run *run, bool value) {
    const mt_frame *fr = top(det);
    odin3_fsm_sigref key = {pool_at(det, fr->sig_off), fr->sig_len};
    odin3_status st = odin3_fsm_sigcache_put(&det->cache, key, value ? 1U : 0U);
    pop_check(det, run, value);
    return st;
}

/* The start of a signal: the memo, constants, the old signal, ports, then its drivers. */
static odin3_status mt_start(detect *det, mt_run *run) {
    mt_frame *fr = top(det);
    odin3_fsm_sigref key = {pool_at(det, fr->sig_off), fr->sig_len};
    uint32_t cached = 0;
    if (odin3_fsm_sigcache_get(&det->cache, key, &cached)) {
        pop_check(det, run, cached != 0);
        return ODIN3_OK;
    }
    if (sig_all_const(key) || sig_equal(run->old_sig, key)) {
        return resolve(det, run, true);
    }
    if (sig_at_port(&det->fm->ix, key)) {
        return resolve(det, run, false);
    }
    uint32_t count = 0;
    odin3_status st = odin3_fsm_ix_pairs(&det->fm->ix, key, ODIN3_FSM_REF_OUT, &det->drv, &count);
    fr = top(det);
    fr->drv_n = count;
    fr->state = MT_DRIVERS;
    return st;
}

/* The name for a log line about a signal bit: its wire, else "$n<ID>". */
static void net_label(const odin3_fsm_mod *fm, odin3_fsm_sig sig, char *buf, size_t size) {
    odin3_wirebit prim = {{0}, 0};
    if (!odin3_fsm_sig_is_const(sig)) {
        prim = odin3_net_primary(fm->module, (odin3_net_id){sig});
    }
    if (prim.wire.v != 0) {
        (void)snprintf(buf, size, "%s", odin3_fsm_str(fm, odin3_wire_name(fm->module, prim.wire)));
    } else {
        (void)snprintf(buf, size, "$n%u", sig);
    }
}

/* Copies the A and B ports of the mux in hand into the pool and starts on A. */
static odin3_status mt_enter_mux(detect *det) {
    mt_frame *fr = top(det);
    odin3_node_id node = {fr->cell};
    fr->a_off = (uint32_t)det->pool.len;
    odin3_status st = odin3_fsm_ix_port_sigs(&det->fm->ix, node, ODIN3_FSM_P_MUX_A, &det->pool);
    fr = top(det);
    fr->a_n = (uint32_t)det->pool.len - fr->a_off;
    fr->b_off = (uint32_t)det->pool.len;
    if (st == ODIN3_OK) {
        st = odin3_fsm_ix_port_sigs(&det->fm->ix, node, ODIN3_FSM_P_MUX_B, &det->pool);
    }
    fr = top(det);
    fr->b_n = (uint32_t)det->pool.len - fr->b_off;
    fr->state = MT_A_BACK;
    if (st != ODIN3_OK) {
        return st;
    }
    return push_check(det, (check_src){NULL, fr->a_off, fr->a_n});
}

/* The next driver: a mux driving through Y, not in a loop; then A is checked first. */
static odin3_status mt_driver(detect *det, mt_run *run) {
    mt_frame *fr = top(det);
    if (fr->drv_i >= fr->drv_n) {
        return resolve(det, run, true);
    }
    const uint32_t *pair = odin3_fsm_pair_at(&det->drv, fr->drv_mark, fr->drv_i);
    uint32_t node = pair[0];
    if (!is_mux(det->fm, node) || pair[1] != ODIN3_FSM_P_MUX_Y) {
        return resolve(det, run, false);
    }
    if (det->monitor[node] != 0) {
        char label[LABEL_MAX];
        net_label(det->fm, pool_at(det, fr->sig_off)[0], label, sizeof label);
        odin3_log(ODIN3_LOG_WARN, "fsm: module '%s': logic loop in mux tree at '%s'",
                  odin3_fsm_mod_name(det->fm), label);
        return resolve(det, run, false);
    }
    det->monitor[node] = 1;
    fr->cell = node;
    return mt_enter_mux(det);
}

/* A child answered for the slice just checked. */
static odin3_status mt_back(detect *det, mt_run *run, bool value) {
    mt_frame *fr = top(det);
    if (!value) {
        det->monitor[fr->cell] = 0;
        return resolve(det, run, false);
    }
    fr->b_i = fr->state == MT_A_BACK ? 0 : fr->b_i + 1;
    fr->state = MT_B_NEXT;
    return ODIN3_OK;
}

/* The next B slice, or the end of this mux. */
static odin3_status mt_b_next(detect *det) {
    mt_frame *fr = top(det);
    if (fr->b_i * fr->a_n >= fr->b_n) {
        det->monitor[fr->cell] = 0;
        fr->drv_i++;
        fr->state = MT_DRIVERS;
        if (det->in_mt[fr->cell] != 0) {
            return ODIN3_OK;
        }
        det->in_mt[fr->cell] = 1;
        return odin3_kit_push_u32(&det->mtcells, fr->cell);
    }
    fr->state = MT_B_BACK;
    return push_check(det, (check_src){NULL, fr->b_off + fr->b_i * fr->a_n, fr->a_n});
}

static odin3_status mt_step(detect *det, mt_run *run) {
    if (run->have_ret) {
        bool value = run->ret;
        run->have_ret = false;
        return mt_back(det, run, value);
    }
    switch (top(det)->state) {
    case MT_START:
        return mt_start(det, run);
    case MT_DRIVERS:
        return mt_driver(det, run);
    default:
        return mt_b_next(det);
    }
}

/* check_state_mux_tree(old_sig, sig): true when D is a closed tree of muxes over constants and Q.
 */
static odin3_status check_tree(detect *det, const tree_req *req, bool *result) {
    mt_run run = {false, false, req->old_sig};
    odin3_status st = push_check(det, (check_src){req->sig.sig, 0, req->sig.len});
    while (st == ODIN3_OK && det->frames.len > 0) {
        st = mt_step(det, &run);
    }
    while (det->frames.len > 0) { /* an out-of-memory exit: drop the frames and the marks */
        det->monitor[top(det)->cell] = 0;
        pop_check(det, &run, false);
    }
    *result = st == ODIN3_OK && run.ret;
    return st;
}

/* --- check_state_users -------------------------------------------------------------------------
 */

/* True when some pin of port `port` of node is connected. */
static bool port_connected(const odin3_fsm_mod *fm, uint32_t node, uint32_t port) {
    if (port == ODIN3_KIT_NONE) {
        return false;
    }
    odin3_pinslice pins = odin3_node_port(fm->module, (odin3_node_id){node}, port);
    for (uint32_t k = 0; k < pins.count; k++) {
        if (odin3_pin_net(fm->module, (odin3_pin_id){pins.first.v + k}).v != 0) {
            return true;
        }
    }
    return false;
}

/* The port indices a user is judged by: A, B, Y of its type. */
typedef struct abc_ports {
    uint32_t a, b, y;
    const odin3_celltype_def *def;
} abc_ports;

static abc_ports abc_of(const odin3_fsm_mod *fm, uint32_t node) {
    odin3_celltype_id type = {odin3_fsm_ix_type(&fm->ix, node)};
    const odin3_celltype_def *def = odin3_celltype_get(fm->design, type);
    abc_ports ports = {odin3_kit_port_index(def, "A"), odin3_kit_port_index(def, "B"),
                       odin3_kit_port_index(def, "Y"), def};
    return ports;
}

/* True when node has all of A, B and Y connected and no other port (:108-112). */
static bool only_abc_ports(const odin3_fsm_mod *fm, uint32_t node, const abc_ports *ports) {
    if (!port_connected(fm, node, ports->a) || !port_connected(fm, node, ports->b) ||
        !port_connected(fm, node, ports->y)) {
        return false;
    }
    for (uint32_t port = 0; port < ports->def->n_ports; port++) {
        bool other = port != ports->a && port != ports->b && port != ports->y;
        if (other && port_connected(fm, node, port)) {
            return false;
        }
    }
    return true;
}

/* One operand of a user, as signal bits (empty for a missing port). */
static odin3_status operand_sig(const odin3_fsm_mod *fm, uint32_t node, uint32_t port,
                                odin3_vec *out) {
    if (port == ODIN3_KIT_NONE) {
        return ODIN3_OK;
    }
    return odin3_fsm_ix_port_sigs(&fm->ix, (odin3_node_id){node}, port, out);
}

/* Is the user (node, port) of Q acceptable (fsm_detect.cc:101-117)? */
static odin3_status user_ok(detect *det, odin3_fsm_sigref sig_q, const uint32_t *pair, bool *ok) {
    const odin3_fsm_mod *fm = det->fm;
    uint32_t node = pair[0];
    *ok = true;
    if (det->in_mt[node] != 0) {
        return ODIN3_OK;
    }
    abc_ports ports = abc_of(fm, node);
    odin3_vec a_vec;
    odin3_vec b_vec;
    odin3_vec_init(&a_vec, sizeof(odin3_fsm_sig));
    odin3_vec_init(&b_vec, sizeof(odin3_fsm_sig));
    odin3_status st = operand_sig(fm, node, ports.a, &a_vec);
    st = st == ODIN3_OK ? operand_sig(fm, node, ports.b, &b_vec) : st;
    odin3_fsm_sigref a_sig = vec_sig(&a_vec);
    odin3_fsm_sigref b_sig = vec_sig(&b_vec);
    bool a_is_q = sig_equal(a_sig, sig_q);
    bool b_is_q = sig_equal(b_sig, sig_q);
    if (is_type(fm, node, fm->ix.types.logic_not) && a_is_q) {
        *ok = true; /* the compare of Q with zero */
    } else if ((pair[1] != ports.a && pair[1] != ports.b) || !only_abc_ports(fm, node, &ports)) {
        *ok = false;
    } else {
        *ok = (a_is_q && sig_all_const(b_sig)) || (b_is_q && sig_all_const(a_sig));
    }
    odin3_vec_free(&a_vec);
    odin3_vec_free(&b_vec);
    return st;
}

/* check_state_users(sig_q): false for a port, or a reader that gains nothing from recoding. */
static odin3_status check_users(detect *det, odin3_fsm_sigref sig_q, bool *good) {
    *good = false;
    if (sig_at_port(&det->fm->ix, sig_q)) {
        return ODIN3_OK;
    }
    size_t mark = det->drv.len;
    uint32_t count = 0;
    odin3_status st = odin3_fsm_ix_pairs(&det->fm->ix, sig_q, ODIN3_FSM_REF_IN, &det->drv, &count);
    bool ok = st == ODIN3_OK;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t pair[2];
        memcpy(pair, odin3_fsm_pair_at(&det->drv, mark, i), sizeof pair);
        st = user_ok(det, sig_q, pair, &ok);
        ok = ok && st == ODIN3_OK;
    }
    odin3_kit_truncate(&det->drv, mark);
    *good = ok;
    return st;
}

/* --- the self-reset test -----------------------------------------------------------------------
 */

/* 1 for the readers that Q mismatching every state forces to 1, 0 for the ones forced to 0, -1. */
static int forced_output(const odin3_fsm_mod *fm, uint32_t node) {
    const odin3_fsm_types *types = &fm->ix.types;
    if (is_type(fm, node, types->ne) || is_type(fm, node, types->reduce_or) ||
        is_type(fm, node, types->reduce_bool)) {
        return 1;
    }
    if (is_type(fm, node, types->eq) || is_type(fm, node, types->logic_not) ||
        is_type(fm, node, types->reduce_and)) {
        return 0;
    }
    return -1;
}

/* True when every non-constant input bit of node is a bit of Q. */
static bool inputs_in_q(const detect *det, uint32_t node, odin3_fsm_sigref sig_q) {
    const odin3_fsm_mod *fm = det->fm;
    odin3_pinslice pins = odin3_node_pins(fm->module, (odin3_node_id){node});
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_fsm_sig sig = odin3_fsm_ix_pin_sig(&fm->ix, pin);
        if (!odin3_pin_reads(fm->module, pin) || odin3_fsm_sig_is_const(sig)) {
            continue;
        }
        bool in_q = false;
        for (uint32_t i = 0; i < sig_q.len && !in_q; i++) {
            in_q = sig_q.sig[i] == sig;
        }
        if (!in_q) {
            return false;
        }
    }
    return true;
}

/* A reader whose outputs are forced: the cell and the value. */
typedef struct forced_cell {
    uint32_t node;
    uint8_t bit; /* odin3_fsm_bit 0 or 1 */
} forced_cell;

/* ce.set(sig_y, Const(val, size)) for every output of the cell. */
static odin3_status force_outputs(const detect *det, odin3_fsm_ceval *ce, forced_cell cell) {
    const odin3_fsm_mod *fm = det->fm;
    odin3_pinslice pins = odin3_node_pins(fm->module, (odin3_node_id){cell.node});
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        odin3_fsm_sig sig = odin3_fsm_ix_pin_sig(&fm->ix, pin);
        if (odin3_pin_dir(fm->module, pin) != ODIN3_DIR_OUT) {
            continue;
        }
        odin3_status st = odin3_fsm_ceval_set(ce, &sig, &cell.bit, 1);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Forces the readers of Q (fsm_detect.cc:172-195). */
static odin3_status force_readers(detect *det, odin3_fsm_ceval *ce, odin3_fsm_sigref sig_q) {
    const odin3_fsm_mod *fm = det->fm;
    size_t mark = det->drv.len;
    uint32_t count = 0;
    odin3_status st = odin3_fsm_ix_pairs(&fm->ix, sig_q, ODIN3_FSM_REF_IN, &det->drv, &count);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        uint32_t node = odin3_fsm_pair_at(&det->drv, mark, i)[0];
        int forced = forced_output(fm, node);
        if (forced >= 0 && inputs_in_q(det, node, sig_q)) {
            forced_cell cell = {node, forced != 0 ? ODIN3_FSM_BIT_1 : ODIN3_FSM_BIT_0};
            st = force_outputs(det, ce, cell);
        }
    }
    odin3_kit_truncate(&det->drv, mark);
    return st;
}

/*
 * Is the FSM self-resetting (fsm_detect.cc:164-204)? Every reader that decides "Q matches no
 * state" gets its output forced; if D then evaluates to a constant, a state without a matching
 * compare has a constant next state.
 */
static odin3_status self_resetting(detect *det, const odin3_vec *sigs_q, const odin3_vec *sigs_d,
                                   bool *result) {
    const odin3_fsm_mod *fm = det->fm;
    odin3_fsm_budget budget = {0, fm->opts->max_evals != 0 ? fm->opts->max_evals
                                                           : (uint64_t)ODIN3_FSM_MAX_EVALS};
    odin3_fsm_ceval *ce = NULL;
    *result = false;
    odin3_status st = odin3_fsm_ceval_create(&fm->ix, &budget, &ce);
    if (st == ODIN3_OK) {
        st = force_readers(det, ce, vec_sig(sigs_q));
    }
    odin3_vec copy;
    odin3_vec_init(&copy, sizeof(odin3_fsm_sig));
    for (size_t i = 0; st == ODIN3_OK && i < sigs_d->len; i++) {
        st = odin3_kit_push_u32(&copy, ((const odin3_fsm_sig *)sigs_d->data)[i]);
    }
    if (st == ODIN3_OK) {
        odin3_fsm_ceval_res res;
        st = odin3_fsm_ceval_eval(ce, copy.data, (uint32_t)copy.len, &res);
        *result = st == ODIN3_OK && res.ok;
    }
    odin3_vec_free(&copy);
    odin3_fsm_ceval_destroy(ce);
    return st;
}

/* --- one wire ----------------------------------------------------------------------------------
 */

static void clear_tree_state(detect *det) {
    const uint32_t *cells = det->mtcells.data;
    for (size_t i = 0; i < det->mtcells.len; i++) {
        det->in_mt[cells[i]] = 0;
    }
    odin3_vec_clear(&det->mtcells);
}

/* The ports of a $dff or $adff: Q and D. */
static void ff_ports(const odin3_fsm_mod *fm, uint32_t node, uint32_t *q_port, uint32_t *d_port) {
    bool is_adff = is_type(fm, node, fm->ix.types.adff);
    *q_port = is_adff ? ODIN3_FSM_P_ADFF_Q : ODIN3_FSM_P_DFF_Q;
    *d_port = is_adff ? ODIN3_FSM_P_ADFF_D : ODIN3_FSM_P_DFF_D;
}

/* Reads Q and D of the flip-flop `node` into q_sig and d_sig (emptied first). */
static odin3_status read_ff(const detect *det, uint32_t node, odin3_vec *q_sig, odin3_vec *d_sig) {
    const odin3_fsm_mod *fm = det->fm;
    uint32_t q_port = 0;
    uint32_t d_port = 0;
    ff_ports(fm, node, &q_port, &d_port);
    odin3_vec_clear(q_sig);
    odin3_vec_clear(d_sig);
    odin3_status st = odin3_fsm_ix_port_sigs(&fm->ix, (odin3_node_id){node}, q_port, q_sig);
    if (st == ODIN3_OK) {
        st = odin3_fsm_ix_port_sigs(&fm->ix, (odin3_node_id){node}, d_port, d_sig);
    }
    return st;
}

/*
 * The facts of one flip-flop whose Q is the wire (fsm_detect.cc:152-204); *stop is Yosys's `break`
 * when the tree check failed.
 */
static odin3_status judge_ff(detect *det, uint32_t node, verdict *vd, bool *stop) {
    odin3_vec q_sig;
    odin3_vec d_sig;
    odin3_vec_init(&q_sig, sizeof(odin3_fsm_sig));
    odin3_vec_init(&d_sig, sizeof(odin3_fsm_sig));
    odin3_status st = read_ff(det, node, &q_sig, &d_sig);
    tree_req req = {vec_sig(&q_sig), vec_sig(&d_sig)};
    if (st == ODIN3_OK) {
        clear_tree_state(det);
        st = odin3_fsm_sigcache_clear(&det->cache);
    }
    if (st == ODIN3_OK) {
        st = check_tree(det, &req, &vd->state_reg);
    }
    if (st == ODIN3_OK) {
        st = check_users(det, req.old_sig, &vd->good_users);
    }
    *stop = !vd->state_reg;
    if (st == ODIN3_OK && vd->state_reg && !det->fm->opts->ignore_self_reset) {
        bool resetting = false;
        st = self_resetting(det, &q_sig, &d_sig, &resetting);
        vd->self_reset = vd->self_reset || resetting;
    }
    odin3_vec_free(&q_sig);
    odin3_vec_free(&d_sig);
    return st;
}

/* True when `node` is a $dff or $adff whose Q is exactly the wire. */
static odin3_status whole_wire_ff(const detect *det, const uint32_t *pair, odin3_fsm_sigref wire,
                                  bool *whole) {
    const odin3_fsm_mod *fm = det->fm;
    uint32_t q_port = 0;
    uint32_t d_port = 0;
    ff_ports(fm, pair[0], &q_port, &d_port);
    *whole = false;
    bool is_ff = is_type(fm, pair[0], fm->ix.types.dff) || is_type(fm, pair[0], fm->ix.types.adff);
    if (!is_ff || pair[1] != q_port) {
        return ODIN3_OK;
    }
    odin3_vec q_sig;
    odin3_vec d_sig;
    odin3_vec_init(&q_sig, sizeof(odin3_fsm_sig));
    odin3_vec_init(&d_sig, sizeof(odin3_fsm_sig));
    odin3_status st = read_ff(det, pair[0], &q_sig, &d_sig);
    *whole = st == ODIN3_OK && sig_equal(vec_sig(&q_sig), wire);
    odin3_vec_free(&q_sig);
    odin3_vec_free(&d_sig);
    return st;
}

/* Judges, in node order, the flip-flops whose Q is the wire; vd is the last one examined. */
static odin3_status examine(detect *det, const odin3_vec *wire_sig, verdict *vd) {
    odin3_fsm_sigref wire = vec_sig(wire_sig);
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint32_t));
    uint32_t count = 0;
    odin3_status st = odin3_fsm_ix_pairs(&det->fm->ix, wire, ODIN3_FSM_REF_OUT, &pairs, &count);
    bool stop = false;
    for (uint32_t i = 0; st == ODIN3_OK && !stop && i < count; i++) {
        uint32_t pair[2];
        memcpy(pair, odin3_fsm_pair_at(&pairs, 0, i), sizeof pair);
        bool whole = false;
        st = whole_wire_ff(det, pair, wire, &whole);
        if (st == ODIN3_OK && whole) {
            st = judge_ff(det, pair[0], vd, &stop);
        }
    }
    odin3_vec_free(&pairs);
    return st;
}

/* --- the decision (fsm_detect.cc:206-255) -------------------------------------------------------
 */

typedef struct wire_facts {
    bool user_attr, has_init, is_port;
    verdict vd;
} wire_facts;

static void warn_user_attr(const odin3_fsm_mod *fm, const char *wire_name, const wire_facts *wf) {
    static const char *const k_reasons[] = {
        "forcing FSM recoding on module port might result in larger circuit",
        "users of state reg look like FSM recoding might result in larger circuit",
        ("initialization value on FSM state register is ignored; possible simulation-synthesis "
         "mismatch"),
        "doesn't look like a proper FSM; possible simulation-synthesis mismatch",
        "FSM seems to be self-resetting; possible simulation-synthesis mismatch"};
    const bool hit[] = {wf->is_port, !wf->vd.good_users, wf->has_init, !wf->vd.state_reg,
                        wf->vd.self_reset};
    uint32_t warned = 0;
    for (size_t i = 0; i < sizeof hit / sizeof hit[0]; i++) {
        if (hit[i]) {
            warned++;
            odin3_log(ODIN3_LOG_WARN, "fsm: module '%s': user-specified fsm_encoding on '%s': %s",
                      odin3_fsm_mod_name(fm), wire_name, k_reasons[i]);
        }
    }
    if (warned == 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "fsm: state register '%s.%s' already has an fsm_encoding attribute",
                  odin3_fsm_mod_name(fm), wire_name);
    }
}

static void log_not_marked(const char *label, const wire_facts *wf) {
    if (wf->is_port) {
        odin3_log(ODIN3_LOG_INFO, "fsm: not marking '%s': connected to a module port", label);
    }
    if (!wf->vd.good_users) {
        odin3_log(ODIN3_LOG_INFO, "fsm: not marking '%s': users do not benefit from recoding",
                  label);
    }
    if (wf->has_init) {
        odin3_log(ODIN3_LOG_INFO, "fsm: not marking '%s': has an initialization value", label);
    }
    if (wf->vd.self_reset) {
        odin3_log(ODIN3_LOG_INFO, "fsm: not marking '%s': seems self-resetting", label);
    }
}

static odin3_status decide(odin3_fsm_mod *fm, odin3_wire_id wire, const wire_facts *wf) {
    char label[LABEL_MAX];
    odin3_fsm_wire_label(fm, wire, label, sizeof label);
    if (wf->user_attr) {
        warn_user_attr(fm, label + strlen(odin3_fsm_mod_name(fm)) + 1, wf);
        return ODIN3_OK;
    }
    const verdict *vd = &wf->vd;
    if (vd->state_reg && vd->good_users && !wf->has_init && !wf->is_port && !vd->self_reset) {
        odin3_log(ODIN3_LOG_INFO, "fsm: found state register '%s'", label);
        return odin3_fsm_wire_set_str(fm, wire, fm->key_encoding, "auto");
    }
    if (vd->state_reg) {
        log_not_marked(label, wf);
    }
    return ODIN3_OK;
}

/* A 1-bit net with an fsm_encoding: the attribute goes, with a warning (:133-139). */
static odin3_status drop_one_bit_attr(odin3_fsm_mod *fm, odin3_wire_id wire) {
    char label[LABEL_MAX];
    odin3_fsm_wire_label(fm, wire, label, sizeof label);
    odin3_log(ODIN3_LOG_WARN, "fsm: removing fsm_encoding attribute from 1-bit net '%s'", label);
    return odin3_attr_remove(fm->module, (odin3_objref){ODIN3_OBJ_WIRE, wire.v}, fm->key_encoding);
}

/* detect_fsm(wire): D-2 to D-6. */
static odin3_status detect_wire(detect *det, odin3_wire_id wire) {
    odin3_fsm_mod *fm = det->fm;
    const char *enc = odin3_fsm_wire_encoding(fm, wire);
    bool is_none = enc != NULL && strcmp(enc, "none") == 0;
    wire_facts wf = {enc != NULL && !is_none, false, false, {false, false, false}};
    if (is_none) {
        return ODIN3_OK;
    }
    if (odin3_wire_width(fm->module, wire) <= 1) {
        return wf.user_attr ? drop_one_bit_attr(fm, wire) : ODIN3_OK;
    }
    odin3_vec sig;
    odin3_vec_init(&sig, sizeof(odin3_fsm_sig));
    odin3_status st = odin3_fsm_wire_sigs(fm, wire, &sig);
    odin3_objref obj = {ODIN3_OBJ_WIRE, wire.v};
    wf.has_init = odin3_attr_get(fm->module, obj, fm->key_init) != NULL;
    wf.is_port = sig_at_port(&fm->ix, vec_sig(&sig));
    if (st == ODIN3_OK) {
        st = examine(det, &sig, &wf.vd);
    }
    odin3_vec_free(&sig);
    return st == ODIN3_OK ? decide(fm, wire, &wf) : st;
}

odin3_status odin3_fsm_detect_module(odin3_fsm_mod *fm) {
    detect det;
    odin3_status st = detect_init(&det, fm);
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_wire_end(fm->module); id++) {
        odin3_wire_id wire = {id};
        if (odin3_wire_live(fm->module, wire)) {
            st = detect_wire(&det, wire);
        }
    }
    detect_free(&det);
    return st;
}
