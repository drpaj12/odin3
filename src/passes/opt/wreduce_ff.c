/*
 * wreduce_ff.c — wreduce's mux rule (3G spec 4.1, Yosys run_cell_mux) and flip-flop and latch rule
 * (spec 4.2, run_cell_dff).
 *
 * Both scan the output from the top bit down and remove a run of bits; the removed Y/Q nets are
 * connected to a replacement (wr_commit's ysrc): a common data net or x for a mux, the constant 0,
 * or, for a sign-extended FF, the net of the lowest kept bit (the pad entries of wr_commit). A Q
 * bit that nothing reads is left undriven, as Yosys leaves it. In Yosys, connecting Q[i] to Q[i-1]
 * makes the readers of Q[i] readers of Q[i-1] at once, so the next bit down is no longer unused;
 * the scan keeps that in `blocked`.
 */
#include "passes/opt/wreduce.h"

#include "passes/ffinit.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { MUX_PORT_A = 0, MUX_PORT_B = 1, MUX_PORT_S = 2, MUX_PORT_Y = 3, MUX_PRM_WIDTH = 0 };

/* --- $mux and $pmux ----------------------------------------------------------------------------
 */

typedef struct mux_view {
    uint32_t w, ns;
    const odin3_net_id *a, *b, *s, *y;
} mux_view;

/* The replacement of Y bit `bit` when every data input agrees on it (Yosys's ref loop). */
static bool mux_agree(const wr_state *ws, const mux_view *mv, uint32_t bit, odin3_net_id *out) {
    odin3_net_id ref = mv->a[bit];
    const bool strict = ws->opts->keepdc || !ws->opts->mux_undef;
    for (uint32_t word = 0; word < mv->ns; word++) {
        const odin3_net_id other = mv->b[(size_t)word * mv->w + bit];
        const bool ref_x = wr_is_const(ws, ref, ODIN3_CONST_X);
        const bool other_x = wr_is_const(ws, other, ODIN3_CONST_X);
        if ((strict || (!ref_x && !other_x)) && !wr_same(ws, ref, other)) {
            return false;
        }
        ref = other_x ? ref : other;
    }
    *out = ref;
    return true;
}

/* Scans from the top; ws->ysrc[i] = the replacement of every removed bit. */
static odin3_status mux_scan(wr_state *ws, const mux_view *mv, uint32_t *removed) {
    odin3_net_id xnet = {0};
    *removed = 0;
    for (uint32_t i = mv->w; i-- > 0;) {
        odin3_net_id repl = {0};
        if (wr_unused(ws, mv->y[i])) {
            odin3_status st =
                xnet.v != 0 ? ODIN3_OK : odin3_opt_const_net(ws->oc, ODIN3_BIT_X, &xnet);
            if (st != ODIN3_OK) {
                return st;
            }
            repl = xnet;
        } else if (!mux_agree(ws, mv, i, &repl)) {
            break;
        }
        *(odin3_net_id *)odin3_vec_at(&ws->ysrc, i) = repl;
        (*removed)++;
    }
    return ODIN3_OK;
}

/* The nets Yosys queues after a narrowing: the removed bits of A, Y and each B word. */
static odin3_status mux_mark(wr_state *ws, const mux_view *mv, uint32_t keep) {
    const uint32_t gone = mv->w - keep;
    odin3_status st = wr_mark(ws, mv->a + keep, gone);
    st = st == ODIN3_OK ? wr_mark(ws, mv->y + keep, gone) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < mv->ns; k++) {
        st = wr_mark(ws, mv->b + (size_t)k * mv->w + keep, gone);
    }
    return st;
}

/* B's low `keep` bits of each word, one after the other, into ws->nets2. */
static odin3_status mux_compact_b(wr_state *ws, const mux_view *mv, uint32_t keep) {
    odin3_vec_clear(&ws->nets2);
    for (uint32_t k = 0; k < mv->ns; k++) {
        for (uint32_t i = 0; i < keep; i++) {
            odin3_status st = odin3_kit_push_net(&ws->nets2, mv->b[(size_t)k * mv->w + i]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status mux_remove_cell(wr_state *ws, odin3_node_id node) {
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "wreduce: module '%s': removed %s '%s'", wr_module_name(ws),
              wr_type_name(ws, node), label);
    wr_rewrite rw = {node, NULL, MUX_PORT_Y, 0, 0, {0}};
    odin3_status st = wr_commit(ws, &rw);
    if (st == ODIN3_OK) {
        wr_count(ws, WR_RULE_MUX_REMOVED);
        ws->cnt->removed++;
    }
    return st;
}

static odin3_status mux_narrow(wr_state *ws, odin3_node_id node, const mux_view *mv,
                               uint32_t keep) {
    odin3_status st = mux_mark(ws, mv, keep);
    st = st == ODIN3_OK ? mux_compact_b(ws, mv, keep) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value params[WR_MAX_PARAMS];
    const uint32_t count = wr_params(ws, node, params);
    params[MUX_PRM_WIDTH] = odin3_value_int(keep);
    odin3_netvec ports[4] = {{mv->a, keep},
                             {ws->nets2.len > 0 ? ws->nets2.data : mv->a, (uint32_t)ws->nets2.len},
                             {mv->s, mv->ns},
                             {NULL, 0}};
    const odin3_kit_cell cell = {odin3_node_type(ws->m, node), params, count, ports, {0}};
    wr_rewrite rw = {node, &cell, MUX_PORT_Y, keep, 0, {0}};
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    st = wr_commit(ws, &rw);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_DEBUG,
                  "wreduce: module '%s': removed top %u bits (of %u) from mux cell %s '%s'",
                  wr_module_name(ws), (unsigned)(mv->w - keep), (unsigned)mv->w,
                  wr_type_name(ws, rw.made), label);
        wr_count(ws, WR_RULE_MUX_NARROW);
        ws->cnt->narrowed++;
        ws->cnt->bits += mv->w - keep;
    }
    return st;
}

odin3_status wr_run_mux(wr_state *ws, odin3_node_id node) {
    bool complete = false;
    odin3_status st = wr_gather(ws, node, &complete);
    if (st != ODIN3_OK || !complete) {
        return st;
    }
    mux_view mv;
    mv.w = odin3_node_port(ws->m, node, MUX_PORT_A).count;
    mv.ns = odin3_node_port(ws->m, node, MUX_PORT_S).count;
    mv.a = ws->nets.data;
    mv.b = mv.a + mv.w;
    mv.s = mv.b + (size_t)mv.w * mv.ns;
    mv.y = mv.s + mv.ns;
    for (uint32_t i = 0; i < mv.w; i++) {
        if (wr_const(ws, mv.y[i]) != ODIN3_CONST_NONE) {
            return ODIN3_OK;
        }
    }
    uint32_t removed = 0;
    st = wr_ysrc_reset(ws, node, MUX_PORT_Y);
    st = st == ODIN3_OK ? mux_scan(ws, &mv, &removed) : st;
    if (st != ODIN3_OK || removed == 0) {
        return st;
    }
    return removed == mv.w ? mux_remove_cell(ws, node) : mux_narrow(ws, node, &mv, mv.w - removed);
}

/* --- flip-flops and latches ---------------------------------------------------------------------
 */

typedef enum ff_kind { FF_STOP, FF_ZERO, FF_SIGN, FF_UNUSED } ff_kind;

typedef struct ff_view {
    uint32_t w, q_port, d_port;
    const odin3_net_id *d, *q;
    const uint8_t *rst; /* ARST_VALUE or SRST_VALUE bits, NULL for none */
    uint32_t rst_len, rst_param;
    uint32_t init_key;
    bool have_key, zero_ext, sign_ext, blocked;
} ff_view;

static odin3_bit init_of(const wr_state *ws, const ff_view *fv, uint32_t bit) {
    return fv->have_key ? odin3_ffinit_get_key(ws->m, fv->q[bit], fv->init_key) : ODIN3_BIT_X;
}

/* An init or reset bit that lets a 0 through: 0, or x unless -keepdc. */
static bool zero_like(const wr_state *ws, uint8_t bit) {
    return bit == ODIN3_BIT_0 || (!ws->opts->keepdc && bit == ODIN3_BIT_X);
}

static bool ff_zero_ok(const wr_state *ws, const ff_view *fv, uint32_t bit) {
    return fv->zero_ext && wr_is_const(ws, fv->d[bit], ODIN3_CONST_0) &&
           zero_like(ws, (uint8_t)init_of(ws, fv, bit)) &&
           (fv->rst == NULL || bit >= fv->rst_len || zero_like(ws, fv->rst[bit]));
}

static bool ff_sign_ok(const wr_state *ws, const ff_view *fv, uint32_t bit) {
    if (!fv->sign_ext || bit == 0 || !wr_same(ws, fv->d[bit], fv->d[bit - 1U])) {
        return false;
    }
    const odin3_bit init = init_of(ws, fv, bit);
    if (init != init_of(ws, fv, bit - 1U) || (ws->opts->keepdc && init == ODIN3_BIT_X)) {
        return false;
    }
    return fv->rst == NULL || bit >= fv->rst_len ||
           (fv->rst[bit] == fv->rst[bit - 1U] &&
            (!ws->opts->keepdc || fv->rst[bit] != ODIN3_BIT_X));
}

static ff_kind ff_classify(const wr_state *ws, const ff_view *fv, uint32_t bit) {
    if (ff_zero_ok(ws, fv, bit)) {
        return FF_ZERO;
    }
    if (ff_sign_ok(ws, fv, bit)) {
        return FF_SIGN;
    }
    return !fv->blocked && wr_unused(ws, fv->q[bit]) ? FF_UNUSED : FF_STOP;
}

typedef struct ff_scan {
    uint32_t removed, zero, sign, unused;
    bool have_zero; /* `zero` below is made on the first ZERO bit, so a no-op leaves no cell */
    odin3_net_id zero_net;
} ff_scan;

/* Bit i of Q becomes const 0 in the ysrc list; the const-0 net is made on the first such bit. */
static odin3_status ff_mark_zero(wr_state *ws, ff_scan *sc, uint32_t bit) {
    if (!sc->have_zero) {
        const odin3_status st = odin3_opt_const_net(ws->oc, ODIN3_BIT_0, &sc->zero_net);
        if (st != ODIN3_OK) {
            return st;
        }
        sc->have_zero = true;
    }
    *(odin3_net_id *)odin3_vec_at(&ws->ysrc, bit) = sc->zero_net;
    return ODIN3_OK;
}

static void ff_tally(ff_scan *sc, ff_kind kind) {
    sc->removed++;
    sc->zero += kind == FF_ZERO ? 1U : 0U;
    sc->sign += kind == FF_SIGN ? 1U : 0U;
    sc->unused += kind == FF_UNUSED ? 1U : 0U;
}

/* The state the next (lower) bit sees after this one went. */
static void ff_after_bit(ff_view *fv, ff_kind kind) {
    fv->blocked = kind == FF_SIGN;
    if (kind == FF_UNUSED) {
        fv->zero_ext = fv->sign_ext = false;
    }
}

/* Scans from the top: ysrc and the init list get one entry per removed bit. */
static odin3_status ff_scan_bits(wr_state *ws, ff_view *fv, ff_scan *sc) {
    memset(sc, 0, sizeof *sc);
    odin3_vec_clear(&ws->init);
    for (uint32_t i = fv->w; i-- > 0;) {
        const ff_kind kind = ff_classify(ws, fv, i);
        if (kind == FF_STOP) {
            break;
        }
        odin3_status st = odin3_kit_push_net(&ws->init, fv->q[i]);
        if (st != ODIN3_OK) {
            return st;
        }
        ff_tally(sc, kind);
        st = kind == FF_ZERO ? ff_mark_zero(ws, sc, i) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
        ff_after_bit(fv, kind);
    }
    return ODIN3_OK;
}

static bool ff_view_init(wr_state *ws, odin3_node_id node, ff_view *fv) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, node));
    fv->d_port = odin3_kit_port_index(def, "D");
    fv->q_port = odin3_kit_port_index(def, "Q");
    fv->w = odin3_node_port(ws->m, node, fv->q_port).count;
    if (fv->w == 0) {
        return false;
    }
    uint32_t at = 0;
    fv->d = fv->q = NULL;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        const odin3_net_id *here = (const odin3_net_id *)ws->nets.data + at;
        fv->d = i == fv->d_port ? here : fv->d;
        fv->q = i == fv->q_port ? here : fv->q;
        at += odin3_node_port(ws->m, node, i).count;
    }
    fv->rst = NULL;
    fv->rst_param = odin3_kit_param_index(def, "ARST_VALUE");
    fv->rst_param =
        fv->rst_param != ODIN3_KIT_NONE ? fv->rst_param : odin3_kit_param_index(def, "SRST_VALUE");
    if (fv->rst_param != ODIN3_KIT_NONE) {
        const odin3_value *val = odin3_node_param(ws->m, node, fv->rst_param);
        fv->rst = val->bits != NULL ? val->bits : (const uint8_t *)"";
        fv->rst_len = val->len;
    }
    fv->have_key = odin3_ffinit_key(ws->m, &fv->init_key);
    fv->zero_ext = wr_is_const(ws, fv->d[fv->w - 1U], ODIN3_CONST_0);
    fv->sign_ext = !fv->zero_ext;
    fv->blocked = false;
    return true;
}

/* The ports of the narrowed FF: D cut to `keep` bits, Q fresh, the others as they are. */
static void ff_ports(wr_state *ws, odin3_node_id node, const ff_view *fv, uint32_t keep,
                     odin3_netvec *ports) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ws->oc->design, odin3_node_type(ws->m, node));
    uint32_t at = 0;
    for (uint32_t i = 0; i < def->n_ports; i++) {
        const uint32_t count = odin3_node_port(ws->m, node, i).count;
        ports[i] = (odin3_netvec){(const odin3_net_id *)ws->nets.data + at, count};
        at += count;
    }
    ports[fv->d_port].count = keep;
    ports[fv->q_port] = (odin3_netvec){NULL, 0};
}

static odin3_status ff_remove_cell(wr_state *ws, odin3_node_id node, const ff_view *fv) {
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG, "wreduce: module '%s': removed %s '%s'", wr_module_name(ws),
              wr_type_name(ws, node), label);
    wr_rewrite rw = {node, NULL, fv->q_port, 0, 0, {0}};
    odin3_status st = wr_commit(ws, &rw);
    if (st == ODIN3_OK) {
        wr_count(ws, WR_RULE_FF_REMOVED);
        ws->cnt->removed++;
    }
    return st;
}

static void ff_count(wr_state *ws, const ff_scan *sc) {
    ws->cnt->rule[WR_RULE_FF_ZERO_EXT] += sc->zero > 0 ? 1U : 0U;
    ws->cnt->rule[WR_RULE_FF_SIGN_EXT] += sc->sign > 0 ? 1U : 0U;
    ws->cnt->rule[WR_RULE_FF_UNUSED] += sc->unused > 0 ? 1U : 0U;
}

/* The reset value shrinks with Q: its low `keep` bits go into ws->bits and the parameter. */
static odin3_status ff_truncate_reset(wr_state *ws, const ff_view *fv, uint32_t keep,
                                      odin3_value *params) {
    if (fv->rst == NULL) {
        return ODIN3_OK;
    }
    odin3_vec_clear(&ws->bits);
    for (uint32_t bit = 0; bit < keep; bit++) {
        uint8_t *slot = odin3_vec_push(&ws->bits);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = bit < fv->rst_len ? fv->rst[bit] : (uint8_t)ODIN3_BIT_0;
    }
    params[fv->rst_param].bits = ws->bits.data;
    params[fv->rst_param].len = keep;
    return ODIN3_OK;
}

static odin3_status ff_narrow(wr_state *ws, odin3_node_id node, const ff_view *fv,
                              const ff_scan *sc) {
    const uint32_t keep = fv->w - sc->removed;
    odin3_status st = wr_mark(ws, fv->d, keep);
    st = st == ODIN3_OK ? wr_mark(ws, fv->q, keep) : st;
    odin3_value params[WR_MAX_PARAMS];
    const uint32_t count = wr_params(ws, node, params);
    params[0] = odin3_value_int(keep);
    st = st == ODIN3_OK ? ff_truncate_reset(ws, fv, keep, params) : st;
    odin3_netvec ports[WR_MAX_PORTS];
    ff_ports(ws, node, fv, keep, ports);
    const odin3_kit_cell cell = {odin3_node_type(ws->m, node), params, count, ports, {0}};
    wr_rewrite rw = {node, &cell, fv->q_port, keep, sc->sign, {0}};
    char label[WR_LABEL_MAX];
    wr_label(ws, node, label, sizeof label);
    st = st == ODIN3_OK ? wr_commit(ws, &rw) : st;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_DEBUG,
                  "wreduce: module '%s': removed top %u bits (of %u) from FF cell %s '%s'",
                  wr_module_name(ws), (unsigned)sc->removed, (unsigned)fv->w,
                  wr_type_name(ws, rw.made), label);
        ff_count(ws, sc);
        ws->cnt->narrowed++;
        ws->cnt->bits += sc->removed;
    }
    return st;
}

odin3_status wr_run_ff(wr_state *ws, odin3_node_id node) {
    bool complete = false;
    odin3_status st = wr_gather(ws, node, &complete);
    if (st != ODIN3_OK || !complete) {
        return st;
    }
    ff_view fv;
    if (!ff_view_init(ws, node, &fv)) {
        return ODIN3_OK;
    }
    st = wr_ysrc_reset(ws, node, fv.q_port);
    ff_scan sc;
    st = st == ODIN3_OK ? ff_scan_bits(ws, &fv, &sc) : st;
    if (st != ODIN3_OK || sc.removed == 0) {
        odin3_vec_clear(&ws->init);
        return st;
    }
    if (sc.removed == fv.w) {
        ff_count(ws, &sc);
        return ff_remove_cell(ws, node, &fv);
    }
    return ff_narrow(ws, node, &fv, &sc);
}
