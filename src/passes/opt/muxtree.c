/*
 * muxtree.c — opt_muxtree (2F spec OP-7, §7; plan Task 10): Yosys 0.55 opt_muxtree.cc
 * (OptMuxtreeWorker) over Odin III's $mux and $pmux nodes, with an explicit frame stack in place of
 * the recursion of eval_mux and eval_mux_port.
 *
 * The index. A mux port is one B slice with its S bit, and A last (Yosys's "port"). Nets are merged
 * aliases already, so a net ID is Yosys's bit index. A net is "seen non-mux" when any node that is
 * not a (well formed) mux reads or drives it, when a mux uses it as a select, or when a wire with
 * `keep` holds it (the $port_out nodes are such nodes). A mux is a root when a bit of its Y is seen
 * non-mux (a "pure" root) or when more than one mux reads its Y.
 *
 * The evaluation. Each root is evaluated with knowledge: per net, how many open port frames know it
 * active (the select of the port being evaluated) and how many know it inactive (the selects of the
 * mux's other ports). A frame is a mux (the loop of eval_mux over its ports) or a mux port (the
 * body of eval_mux_port, whose parents are the muxes that drive the port's data bits). Frames sit
 * on one vector and the parents of open port frames on another, so nothing recurses. The roots are
 * evaluated from the last mux to the first, then the re-queued ones, the last queued first: the
 * order Yosys's cell dictionary and pool give (a promotion to pure root prunes later evaluations,
 * so a different order removes fewer ports; observed on random layered DAGs). A budget of
 * ODIN3_OPT_MUXTREE_MAX_EVALS eval_mux entries per module stops the analysis: nothing is changed in
 * that module, not even the known-bit replacements Yosys has already made by then (§17 row 18).
 *
 * The replacements (replace_known) are kept in an overlay, one byte per A and B data bit of every
 * mux, and applied with the dead ports at the end. A bit that is the select of its own slice reads
 * 1 and any other select of the mux reads 0; Yosys compares an index into the distinct select nets
 * with the slice index instead, which is wrong when a select repeats (§17 row 17).
 * A mux that feeds more than one (user, port) pair is replaced nowhere, since each port meets it
 * under opposite knowledge and the first would win (§17 row 20).
 *
 * The rewrite. Per mux in ID order: no live port deletes the node; one live port connects Y to
 * that port's data; several rebuild a narrower $pmux or a $mux. A mux with all ports live but
 * known bits replaced is rebuilt the same way. Everything a rewrite allocates is built before its
 * first destructive step, so NO_MEMORY leaves the IR valid, every earlier rewrite done and none
 * half done (kit.h). A mux with an unconnected pin, a Y net with another driver or a width that
 * disagrees with its parameters is treated as a non-mux node (§17 row 19).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "passes/opt/opt_test.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PORT_A = 0, /* $mux, $pmux: A, B, S, Y */
    PORT_B = 1,
    PORT_S = 2,
    PORT_Y = 3,
    PRM_WIDTH = 0,
    PRM_S_WIDTH = 1,
    PRM_PMUX_N = 2,
    INPLACE_DEPTH = 3, /* Yosys's abort_count: non-pure roots evaluated in place, then re-queued */
    LABEL_MAX = 128,
    PAIR_SHIFT = 32
};

#define NO_NET UINT32_MAX

enum { NF_NONMUX = 1 };

enum { OV_NONE = 0, OV_ZERO = 1, OV_ONE = 2 };

/* --- the index -------------------------------------------------------------------------------- */

/* One mux port: its select (a net, NO_NET for A and constants) and its constant state. */
typedef struct port_info {
    uint32_t ctrl;
    uint32_t first, count; /* its input muxes: inmux[first .. first + count) */
    uint8_t on, off;       /* const_activated, const_deactivated */
    uint8_t enabled;
} port_info;

typedef struct mux_info {
    uint32_t node, n_ports, width;
    uint32_t port0;         /* its ports: ports[port0 .. port0 + n_ports) */
    uint32_t ctrl0, n_ctrl; /* its distinct non-constant select nets, sorted: ctrls[ctrl0 ..) */
    uint32_t n_repl;        /* replaced data bits */
    size_t data0;           /* overlay[data0 ..): A, then B */
    uint8_t root, enable, rerun, visited;
    uint8_t noreplace; /* reached through more than one (user, port): replace_known is unsafe */
} mux_info;

typedef enum frame_kind { F_MUX, F_PORT } frame_kind;

/* One open eval_mux (a loop over ports) or eval_mux_port. */
typedef struct frame {
    uint32_t mux, port; /* F_MUX: the next port to try; F_PORT: its port */
    uint32_t end;       /* F_MUX: one past the last port to try */
    uint32_t par0, npar, next;
    uint8_t kind, started, replace, enable, filter, abort_count;
} frame;

typedef struct mux_state {
    odin3_opt_ctx *oc;
    odin3_module *m;
    odin3_celltype_id t_mux, t_pmux;
    uint32_t net_end;
    uint8_t *nflag;       /* per net */
    uint32_t *drv;        /* per net: the index + 1 of the mux that drives it */
    int32_t *known_inact; /* per net: open frames that know it inactive */
    int32_t *known_act;   /* per net: open frames that know it active */
    uint8_t *overlay;     /* per A and B data bit of every mux: OV_* */
    odin3_vec muxes, ports, inmux, ctrls, pairs, portpairs; /* mux_info, port_info, uint64_t ... */
    mux_info *mx;
    port_info *pt;
    odin3_vec frames, parents, rerun_q, live, na, nb, ns;
    uint32_t evals, max_evals, removed;
    bool gave_up;
} mux_state;

static void vecs_init(mux_state *ms) {
    odin3_vec_init(&ms->muxes, sizeof(mux_info));
    odin3_vec_init(&ms->ports, sizeof(port_info));
    odin3_vec *u64s[] = {&ms->inmux, &ms->ctrls, &ms->pairs, &ms->portpairs};
    for (size_t i = 0; i < sizeof u64s / sizeof u64s[0]; i++) {
        odin3_vec_init(u64s[i], sizeof(uint64_t));
    }
    odin3_vec_init(&ms->frames, sizeof(frame));
    odin3_vec_init(&ms->parents, sizeof(uint32_t));
    odin3_vec_init(&ms->rerun_q, sizeof(uint32_t));
    odin3_vec_init(&ms->live, sizeof(uint32_t));
    odin3_vec *nets[] = {&ms->na, &ms->nb, &ms->ns};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        odin3_vec_init(nets[i], sizeof(odin3_net_id));
    }
}

static void state_free(mux_state *ms) {
    odin3_vec *all[] = {&ms->muxes,     &ms->ports,  &ms->inmux,   &ms->ctrls,   &ms->pairs,
                        &ms->portpairs, &ms->frames, &ms->parents, &ms->rerun_q, &ms->live,
                        &ms->na,        &ms->nb,     &ms->ns};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        odin3_vec_free(all[i]);
    }
    odin3_util_free(ms->nflag);
    odin3_util_free(ms->drv);
    odin3_util_free(ms->known_inact);
    odin3_util_free(ms->known_act);
    odin3_util_free(ms->overlay);
}

/* Storage of a uint64_t / uint32_t vec from element off on; NULL for a vec that never allocated
 * (clang's UBSan rejects a zero offset on a null pointer). */
static const uint64_t *u64_from(const odin3_vec *vec, size_t off) {
    return vec->data == NULL ? NULL : (const uint64_t *)vec->data + off;
}

static const uint32_t *u32_from(const odin3_vec *vec, size_t off) {
    return vec->data == NULL ? NULL : (const uint32_t *)vec->data + off;
}

static odin3_net_id pin_net_at(const mux_state *ms, odin3_pinslice ps, uint32_t bit) {
    return odin3_pin_net(ms->m, (odin3_pin_id){ps.first.v + bit});
}

/* --- the index: nodes ------------------------------------------------------------------------- */

/* The sizes of a well formed mux node. */
typedef struct mux_shape {
    uint32_t width, sel;
} mux_shape;

static bool pins_connected(const mux_state *ms, odin3_pinslice ps) {
    for (uint32_t k = 0; k < ps.count; k++) {
        if (!odin3_net_valid(pin_net_at(ms, ps, k))) {
            return false;
        }
    }
    return true;
}

/* The widths of node's ports agree with shape, all its pins are connected and its Y nets have no
 * other driver. */
static bool ports_fit(const mux_state *ms, odin3_node_id node, const mux_shape *shape) {
    const odin3_pinslice pin_a = odin3_node_port(ms->m, node, PORT_A);
    const odin3_pinslice pin_b = odin3_node_port(ms->m, node, PORT_B);
    const odin3_pinslice pin_s = odin3_node_port(ms->m, node, PORT_S);
    const odin3_pinslice pin_y = odin3_node_port(ms->m, node, PORT_Y);
    if (pin_a.count != shape->width || pin_s.count != shape->sel || pin_y.count != shape->width ||
        (uint64_t)pin_b.count != (uint64_t)shape->width * shape->sel) {
        return false;
    }
    for (uint32_t bit = 0; bit < pin_y.count; bit++) {
        if (odin3_net_driver_count(ms->m, pin_net_at(ms, pin_y, bit)) != 1) {
            return false;
        }
    }
    return pins_connected(ms, pin_a) && pins_connected(ms, pin_b) && pins_connected(ms, pin_s) &&
           pins_connected(ms, pin_y);
}

/* True when node is a $mux or $pmux of sane parameters and ports that fit them (ports_fit). */
static bool mux_shape_of(const mux_state *ms, odin3_node_id node, mux_shape *out) {
    odin3_celltype_id type = odin3_node_type(ms->m, node);
    const bool is_pmux = type.v == ms->t_pmux.v;
    if (type.v != ms->t_mux.v && !is_pmux) {
        return false;
    }
    const int64_t width = odin3_node_param(ms->m, node, PRM_WIDTH)->i;
    const int64_t sel = is_pmux ? odin3_node_param(ms->m, node, PRM_S_WIDTH)->i : 1;
    if (width < 1 || sel < 1 || width > UINT32_MAX || sel > UINT32_MAX) {
        return false;
    }
    out->width = (uint32_t)width;
    out->sel = (uint32_t)sel;
    return ports_fit(ms, node, out);
}

static void mark_nonmux(mux_state *ms, odin3_net_id net) {
    if (odin3_net_valid(net) && net.v < ms->net_end) {
        ms->nflag[net.v] |= NF_NONMUX;
    }
}

static void mark_port(mux_state *ms, odin3_node_id node, uint32_t port) {
    odin3_pinslice ps = odin3_node_port(ms->m, node, port);
    for (uint32_t k = 0; k < ps.count; k++) {
        mark_nonmux(ms, pin_net_at(ms, ps, k));
    }
}

/* A node that is not a mux: every net it touches is seen non-mux. */
static void mark_node(mux_state *ms, odin3_node_id node) {
    odin3_pinslice ps = odin3_node_pins(ms->m, node);
    for (uint32_t k = 0; k < ps.count; k++) {
        mark_nonmux(ms, pin_net_at(ms, ps, k));
    }
}

/* Registers a mux: its record, the select nets seen non-mux, the nets its Y drives. */
static odin3_status add_mux(mux_state *ms, odin3_node_id node, const mux_shape *shape) {
    mux_info *mi = odin3_vec_push(&ms->muxes);
    if (mi == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    mi->node = node.v;
    mi->n_ports = shape->sel + 1U;
    mi->width = shape->width;
    mark_port(ms, node, PORT_S);
    const uint32_t index = (uint32_t)ms->muxes.len; /* index + 1 */
    odin3_pinslice outs = odin3_node_port(ms->m, node, PORT_Y);
    for (uint32_t bit = 0; bit < outs.count; bit++) {
        ms->drv[pin_net_at(ms, outs, bit).v] = index;
    }
    return ODIN3_OK;
}

static odin3_status collect_nodes(mux_state *ms) {
    const uint32_t node_end = odin3_module_node_end(ms->m);
    for (uint32_t id = 1; id < node_end; id++) {
        odin3_node_id node = {id};
        mux_shape shape = {0, 0};
        if (!odin3_node_live(ms->m, node)) {
            continue;
        }
        if (!mux_shape_of(ms, node, &shape)) {
            mark_node(ms, node);
            continue;
        }
        odin3_status st = add_mux(ms, node, &shape);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Wires with `keep` hold their nets seen non-mux. */
static void mark_keep_wires(mux_state *ms) {
    const uint32_t wire_end = odin3_module_wire_end(ms->m);
    for (uint32_t id = 1; id < wire_end; id++) {
        odin3_wire_id wire = {id};
        if (!odin3_wire_live(ms->m, wire) ||
            !odin3_kit_keep(ms->oc, (odin3_objref){ODIN3_OBJ_WIRE, id})) {
            continue;
        }
        const uint32_t width = odin3_wire_width(ms->m, wire);
        for (uint32_t bit = 0; bit < width; bit++) {
            mark_nonmux(ms, odin3_wire_net(ms->m, wire, bit));
        }
    }
}

/* --- the index: ports, roots ------------------------------------------------------------------ */

static bool net_is_const(const mux_state *ms, odin3_net_id net) {
    return odin3_net_const_value(ms->m, net) != ODIN3_CONST_NONE;
}

/* Appends the distinct muxes that drive the data bits of one port (sorted) to inmux. */
static odin3_status port_inputs(mux_state *ms, const mux_info *mi, uint32_t port, port_info *pi) {
    const bool is_a = port == mi->n_ports - 1U;
    const odin3_pinslice ps =
        odin3_node_port(ms->m, (odin3_node_id){mi->node}, is_a ? PORT_A : PORT_B);
    const uint32_t base = is_a ? 0 : port * mi->width;
    pi->first = (uint32_t)ms->inmux.len;
    for (uint32_t k = 0; k < mi->width; k++) {
        const uint32_t drv = ms->drv[pin_net_at(ms, ps, base + k).v];
        odin3_status st = drv != 0 ? odin3_kit_push_u64(&ms->inmux, drv - 1U) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_kit_sort_unify(&ms->inmux, pi->first);
    pi->count = (uint32_t)(ms->inmux.len - pi->first);
    return ODIN3_OK;
}

/* One port of mux mi: its select, its constant state, its input muxes. */
static odin3_status build_port(mux_state *ms, const mux_info *mi, uint32_t port) {
    port_info pi;
    memset(&pi, 0, sizeof pi);
    pi.ctrl = NO_NET;
    if (port < mi->n_ports - 1U) {
        const odin3_net_id sel =
            pin_net_at(ms, odin3_node_port(ms->m, (odin3_node_id){mi->node}, PORT_S), port);
        const odin3_const cv = odin3_net_const_value(ms->m, sel);
        pi.on = cv == ODIN3_CONST_1 ? 1U : 0U;
        pi.off = cv != ODIN3_CONST_NONE && cv != ODIN3_CONST_1 ? 1U : 0U;
        pi.ctrl = cv == ODIN3_CONST_NONE ? sel.v : NO_NET;
    }
    odin3_status st = port_inputs(ms, mi, port, &pi);
    port_info *slot = st == ODIN3_OK ? odin3_vec_push(&ms->ports) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = pi;
    }
    return st;
}

/* The distinct non-constant select nets of mux `index`, sorted, into ctrls. */
static odin3_status build_ctrls(mux_state *ms, mux_info *mi) {
    mi->ctrl0 = (uint32_t)ms->ctrls.len;
    for (uint32_t port = 0; port + 1U < mi->n_ports; port++) {
        const uint32_t ctrl = ms->pt[mi->port0 + port].ctrl;
        if (ctrl != NO_NET && odin3_kit_push_u64(&ms->ctrls, ctrl) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_kit_sort_unify(&ms->ctrls, mi->ctrl0);
    mi->n_ctrl = (uint32_t)(ms->ctrls.len - mi->ctrl0);
    return ODIN3_OK;
}

/*
 * The ports of every mux. ms->pt is the ports vector's storage, so it is re-read after each push;
 * the muxes' ports are consecutive.
 */
static odin3_status build_ports(mux_state *ms) {
    size_t data = 0;
    for (uint32_t i = 0; i < ms->muxes.len; i++) {
        ms->mx = ms->muxes.data;
        ms->mx[i].port0 = (uint32_t)ms->ports.len;
        ms->mx[i].data0 = data;
        data += (size_t)ms->mx[i].width * ms->mx[i].n_ports;
        for (uint32_t port = 0; port < ms->mx[i].n_ports; port++) {
            odin3_status st = build_port(ms, &ms->mx[i], port);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    ms->pt = ms->ports.data;
    ms->mx = ms->muxes.data;
    ms->overlay = odin3_util_calloc(data == 0 ? 1U : data);
    for (uint32_t i = 0; ms->overlay != NULL && i < ms->muxes.len; i++) {
        odin3_status st = build_ctrls(ms, &ms->mx[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ms->overlay == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
}

/* Pairs (driver, user) into ms->pairs and (driver, port) into ms->portpairs over every port's input
 * muxes, sorted and unique. */
static odin3_status collect_pairs(mux_state *ms) {
    for (uint32_t user = 0; user < ms->muxes.len; user++) {
        const mux_info *mi = &ms->mx[user];
        for (uint32_t port = 0; port < mi->n_ports; port++) {
            const port_info *pi = &ms->pt[mi->port0 + port];
            const uint64_t *in = u64_from(&ms->inmux, pi->first);
            for (uint32_t k = 0; k < pi->count; k++) {
                if (odin3_kit_push_u64(&ms->pairs, (in[k] << PAIR_SHIFT) | user) != ODIN3_OK ||
                    odin3_kit_push_u64(&ms->portpairs,
                                       (in[k] << PAIR_SHIFT) | (mi->port0 + port)) != ODIN3_OK) {
                    return ODIN3_ERR_NO_MEMORY;
                }
            }
        }
    }
    odin3_kit_sort_unify(&ms->pairs, 0);
    odin3_kit_sort_unify(&ms->portpairs, 0);
    return ODIN3_OK;
}

/* A mux that more than one mux reads is a root. */
static void mark_fanout_roots(mux_state *ms) {
    const uint64_t *pair = ms->pairs.data;
    for (size_t i = 0; i + 1 < ms->pairs.len; i++) {
        if ((pair[i + 1] >> PAIR_SHIFT) == (pair[i] >> PAIR_SHIFT)) {
            ms->mx[pair[i] >> PAIR_SHIFT].root = 1;
        }
    }
}

/*
 * A mux that feeds more than one port (of one user or of several) is evaluated once per port, each
 * time under that port's own knowledge: replacing its data bits by the first knowledge met would
 * change the function (Yosys 0.55 does; spec section 17 row 20). It, and the muxes below it that
 * are not roots, get no replacements.
 */
static void mark_multi_port(mux_state *ms) {
    const uint64_t *pair = ms->portpairs.data;
    for (size_t i = 0; i + 1 < ms->portpairs.len; i++) {
        if ((pair[i + 1] >> PAIR_SHIFT) == (pair[i] >> PAIR_SHIFT)) {
            ms->mx[pair[i] >> PAIR_SHIFT].noreplace = 1;
        }
    }
}

/* A mux with a Y bit seen non-mux is a pure root. */
static void mark_pure_roots(mux_state *ms) {
    for (uint32_t i = 0; i < ms->muxes.len; i++) {
        const odin3_pinslice outs = odin3_node_port(ms->m, (odin3_node_id){ms->mx[i].node}, PORT_Y);
        for (uint32_t bit = 0; bit < outs.count; bit++) {
            if ((ms->nflag[pin_net_at(ms, outs, bit).v] & NF_NONMUX) != 0) {
                ms->mx[i].root = 1;
                ms->mx[i].enable = 1;
            }
        }
    }
}

static odin3_status build_roots(mux_state *ms) {
    odin3_status st = collect_pairs(ms);
    if (st == ODIN3_OK) {
        mark_fanout_roots(ms);
        mark_multi_port(ms);
        mark_pure_roots(ms);
    }
    return st;
}

static odin3_status build_index(mux_state *ms) {
    ms->net_end = odin3_module_net_end(ms->m);
    ms->nflag = odin3_util_calloc(ms->net_end);
    ms->drv = odin3_util_calloc(sizeof *ms->drv * ms->net_end);
    ms->known_inact = odin3_util_calloc(sizeof *ms->known_inact * ms->net_end);
    ms->known_act = odin3_util_calloc(sizeof *ms->known_act * ms->net_end);
    if (ms->nflag == NULL || ms->drv == NULL || ms->known_inact == NULL || ms->known_act == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = collect_nodes(ms);
    if (st == ODIN3_OK) {
        mark_keep_wires(ms);
        st = build_ports(ms);
    }
    return st == ODIN3_OK ? build_roots(ms) : st;
}

/* --- the evaluation: knowledge ---------------------------------------------------------------- */

static const port_info *port_of(const mux_state *ms, const mux_info *mi, uint32_t port) {
    return &ms->pt[mi->port0 + port];
}

/*
 * The knowledge of an open port frame, added on entry and given back on exit: its own select is
 * active (not for the default port or a constant-active one), the selects of the mux's other ports
 * are inactive.
 */
static void port_knowledge(mux_state *ms, const mux_info *mi, uint32_t port, bool enter) {
    const int32_t delta = enter ? 1 : -1;
    for (uint32_t i = 0; i < mi->n_ports; i++) {
        const port_info *pi = port_of(ms, mi, i);
        if (i == port) {
            if (i + 1U < mi->n_ports && !pi->on) {
                ms->known_act[pi->ctrl] += delta;
            }
        } else if (pi->ctrl != NO_NET) {
            ms->known_inact[pi->ctrl] += delta;
        }
    }
}

/* True when net is one of the (non-constant) selects of mi. */
static bool is_select(const mux_state *ms, const mux_info *mi, uint32_t net) {
    const uint64_t *sel = u64_from(&ms->ctrls, mi->ctrl0);
    uint32_t lo = 0;
    uint32_t hi = mi->n_ctrl;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2U;
        if (sel[mid] < net) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return lo < mi->n_ctrl && sel[lo] == net;
}

/* What replace_known puts on one data bit of mi: OV_NONE, OV_ZERO or OV_ONE. `slice` is the B
 * slice the bit is in, or NO_NET for A. */
static uint8_t known_value(const mux_state *ms, const mux_info *mi, uint32_t net, uint32_t slice) {
    uint8_t value = OV_NONE;
    if (ms->known_inact[net] != 0) {
        value = OV_ZERO;
    } else if (ms->known_act[net] != 0) {
        value = OV_ONE;
    }
    if (is_select(ms, mi, net)) {
        value = slice != NO_NET && port_of(ms, mi, slice)->ctrl == net ? OV_ONE : OV_ZERO;
    }
    return value;
}

/* Yosys's replace_known on port A or B of mi: known bits become constants (the overlay). */
static void replace_known(const mux_state *ms, mux_info *mi, uint32_t port) {
    const bool is_b = port == PORT_B;
    const odin3_pinslice ps = odin3_node_port(ms->m, (odin3_node_id){mi->node}, port);
    uint8_t *ov = ms->overlay + mi->data0 + (is_b ? mi->width : 0U);
    for (uint32_t k = 0; k < ps.count; k++) {
        const odin3_net_id net = pin_net_at(ms, ps, k);
        if (ov[k] != OV_NONE || net_is_const(ms, net)) {
            continue;
        }
        const uint8_t value = known_value(ms, mi, net.v, is_b ? k / mi->width : NO_NET);
        if (value != OV_NONE) {
            ov[k] = value;
            mi->n_repl++;
        }
    }
}

/* --- the evaluation: frames ------------------------------------------------------------------- */

static odin3_status push_frame(mux_state *ms, const frame *fr) {
    frame *slot = odin3_vec_push(&ms->frames);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *fr;
    return ODIN3_OK;
}

/* Yosys's choice of ports in eval_mux: a constant-active port alone, else a port whose select is
 * known active alone, else every port that could be active (filtered as the loop reaches it). */
static void choose_ports(const mux_state *ms, const mux_info *mi, frame *fr) {
    for (uint32_t port = 0; port < mi->n_ports; port++) {
        if (port_of(ms, mi, port)->on) {
            fr->port = port;
            fr->end = port + 1U;
            return;
        }
    }
    for (uint32_t port = 0; port + 1U < mi->n_ports; port++) {
        const port_info *pi = port_of(ms, mi, port);
        if (!pi->off && ms->known_act[pi->ctrl] != 0) {
            fr->port = port;
            fr->end = port + 1U;
            return;
        }
    }
    fr->port = 0;
    fr->end = mi->n_ports;
    fr->filter = 1;
}

/* The port is evaluated: it is not constant-inactive and, but for the default, not known inactive.
 */
static bool port_wanted(const mux_state *ms, const mux_info *mi, uint32_t port) {
    const port_info *pi = port_of(ms, mi, port);
    if (pi->off) {
        return false;
    }
    return port + 1U == mi->n_ports || ms->known_inact[pi->ctrl] == 0;
}

/* One step of the top F_MUX frame: its entry (the budget, replace_known, the choice of ports), then
 * the next port as a child frame, then its end. */
static odin3_status step_mux(mux_state *ms) {
    frame *fr = odin3_vec_at(&ms->frames, ms->frames.len - 1U);
    mux_info *mi = &ms->mx[fr->mux];
    if (!fr->started) {
        fr->started = 1;
        if (++ms->evals >= ms->max_evals) {
            ms->gave_up = true;
            return ODIN3_OK;
        }
        if (fr->replace) {
            replace_known(ms, mi, PORT_A);
            replace_known(ms, mi, PORT_B);
        }
        choose_ports(ms, mi, fr);
    }
    while (fr->port < fr->end) {
        const uint32_t port = fr->port++;
        if (fr->filter && !port_wanted(ms, mi, port)) {
            continue;
        }
        frame child;
        memset(&child, 0, sizeof child);
        child.kind = F_PORT;
        child.mux = fr->mux;
        child.port = port;
        child.replace = fr->replace;
        child.enable = fr->enable;
        child.abort_count = fr->abort_count;
        return push_frame(ms, &child);
    }
    odin3_vec_pop(&ms->frames);
    return ODIN3_OK;
}

/* The entry of eval_mux_port: enable, knowledge, and the parents (the muxes driving its data). */
static odin3_status port_enter(mux_state *ms, frame *fr) {
    mux_info *mi = &ms->mx[fr->mux];
    port_info *pi = &ms->pt[mi->port0 + fr->port];
    fr->started = 1;
    if (fr->enable) {
        pi->enabled = 1;
    }
    port_knowledge(ms, mi, fr->port, true);
    fr->par0 = (uint32_t)ms->parents.len;
    fr->npar = 0;
    const uint64_t *in = u64_from(&ms->inmux, pi->first);
    for (uint32_t k = 0; k < pi->count; k++) {
        mux_info *parent = &ms->mx[in[k]];
        if (parent->visited) {
            continue;
        }
        parent->visited = 1;
        odin3_status st = odin3_kit_push_u32(&ms->parents, (uint32_t)in[k]);
        if (st != ODIN3_OK) {
            return st;
        }
        fr->npar++;
    }
    return ODIN3_OK;
}

/* The end of eval_mux_port: the parents are unmarked, the knowledge is given back. */
static void port_leave(mux_state *ms, const frame *fr) {
    const uint32_t *parent = u32_from(&ms->parents, fr->par0);
    for (uint32_t k = 0; k < fr->npar; k++) {
        ms->mx[parent[k]].visited = 0;
    }
    odin3_kit_truncate(&ms->parents, fr->par0);
    port_knowledge(ms, &ms->mx[fr->mux], fr->port, false);
}

/* A non-pure root met too deep is promoted to a pure root and queued for another evaluation. */
static odin3_status requeue(mux_state *ms, uint32_t index) {
    mux_info *mi = &ms->mx[index];
    mi->enable = 1;
    if (mi->rerun) {
        return ODIN3_OK;
    }
    mi->rerun = 1;
    return odin3_kit_push_u32(&ms->rerun_q, index);
}

/* The child frame for parent `index` of the port frame fr, as eval_mux_port calls eval_mux. */
static frame child_of(const mux_state *ms, const frame *fr, uint32_t index) {
    frame child;
    memset(&child, 0, sizeof child);
    child.kind = F_MUX;
    child.mux = index;
    child.enable = fr->enable;
    if (ms->mx[index].root) {
        child.abort_count = (uint8_t)(fr->abort_count - 1U);
    } else {
        child.replace = ms->mx[index].noreplace ? 0U : fr->replace;
        child.abort_count = fr->abort_count;
    }
    return child;
}

/* One step of the top F_PORT frame: its entry, then each parent not skipped as a child frame, then
 * its end. */
static odin3_status step_port(mux_state *ms) {
    frame *fr = odin3_vec_at(&ms->frames, ms->frames.len - 1U);
    odin3_status st = fr->started ? ODIN3_OK : port_enter(ms, fr);
    while (st == ODIN3_OK && fr->next < fr->npar) {
        const uint32_t index = ((const uint32_t *)ms->parents.data)[fr->par0 + fr->next++];
        const mux_info *parent = &ms->mx[index];
        if (parent->enable) {
            continue;
        }
        if (parent->root && fr->abort_count == 0) {
            st = requeue(ms, index);
            continue;
        }
        const frame child = child_of(ms, fr, index);
        return push_frame(ms, &child);
    }
    if (st == ODIN3_OK) {
        port_leave(ms, fr);
        odin3_vec_pop(&ms->frames);
    }
    return st;
}

/* Yosys's eval_root_mux: the knowledge starts empty, the root's own bits are replaced. */
static odin3_status eval_root(mux_state *ms, uint32_t index) {
    mux_info *mi = &ms->mx[index];
    frame fr;
    memset(&fr, 0, sizeof fr);
    fr.kind = F_MUX;
    fr.mux = index;
    fr.replace = 1;
    fr.enable = mi->enable;
    fr.abort_count = INPLACE_DEPTH;
    mi->visited = 1;
    odin3_status st = push_frame(ms, &fr);
    while (st == ODIN3_OK && !ms->gave_up && ms->frames.len > 0) {
        const frame *top = odin3_vec_cat(&ms->frames, ms->frames.len - 1U);
        st = top->kind == F_MUX ? step_mux(ms) : step_port(ms);
    }
    mi->visited = 0;
    return st;
}

/* Every root in ID order, then the re-queued ones. */
static odin3_status eval_all(mux_state *ms) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = (uint32_t)ms->muxes.len; st == ODIN3_OK && !ms->gave_up && i-- > 0;) {
        if (ms->mx[i].root) {
            ms->mx[i].rerun = 0;
            st = eval_root(ms, i);
        }
    }
    while (st == ODIN3_OK && !ms->gave_up && ms->rerun_q.len > 0) {
        const uint32_t index = ((const uint32_t *)ms->rerun_q.data)[ms->rerun_q.len - 1U];
        odin3_vec_pop(&ms->rerun_q);
        if (ms->mx[index].rerun) {
            ms->mx[index].rerun = 0;
            st = eval_root(ms, index);
        }
    }
    return st;
}

/* --- the rewrite ------------------------------------------------------------------------------ */

static const char *module_name(const mux_state *ms) {
    return odin3_strtab_get(odin3_design_strtab(ms->oc->design), odin3_module_name(ms->m));
}

static void log_dead(const mux_state *ms, const mux_info *mi, uint32_t port) {
    const odin3_node_id node = {mi->node};
    char label[LABEL_MAX];
    odin3_kit_node_label(ms->oc, node, label, sizeof label);
    odin3_log(ODIN3_LOG_DEBUG,
              "opt_muxtree: module '%s': dead port %" PRIu32 "/%" PRIu32 " on %s '%s'",
              module_name(ms), port + 1U, mi->n_ports,
              odin3_celltype_get(ms->oc->design, odin3_node_type(ms->m, node))->name, label);
}

/* Appends the nets of data port `port` of mi (a B slice, or A for the last) to out; a replaced bit
 * is the canonical constant net. */
static odin3_status append_slice(mux_state *ms, const mux_info *mi, uint32_t port, odin3_vec *out) {
    const bool is_a = port + 1U == mi->n_ports;
    const odin3_pinslice ps =
        odin3_node_port(ms->m, (odin3_node_id){mi->node}, is_a ? PORT_A : PORT_B);
    const uint32_t base = is_a ? 0 : port * mi->width;
    const uint8_t *ov = ms->overlay + mi->data0 + (is_a ? 0U : mi->width) + base;
    for (uint32_t k = 0; k < mi->width; k++) {
        odin3_net_id net = pin_net_at(ms, ps, base + k);
        odin3_status st = ODIN3_OK;
        if (ov[k] != OV_NONE) {
            st = odin3_opt_const_net(ms->oc, ov[k] == OV_ONE ? ODIN3_BIT_1 : ODIN3_BIT_0, &net);
        }
        st = st == ODIN3_OK ? odin3_kit_push_net(out, net) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* One live port: Y is connected to its data (the node goes). */
static odin3_status wire_mux(mux_state *ms, const mux_info *mi, uint32_t port) {
    odin3_vec_clear(&ms->na);
    odin3_status st = append_slice(ms, mi, port, &ms->na);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_kit_conn conn = {(odin3_node_id){mi->node}, PORT_Y, ms->na.data,
                                 (uint32_t)ms->na.len};
    return odin3_kit_connect(ms->oc, &conn);
}

/* ms->na/nb/ns = the data and selects of the live ports: the last live port becomes A. */
static odin3_status gather_live(mux_state *ms, const mux_info *mi) {
    const uint32_t *live = ms->live.data;
    const uint32_t count = (uint32_t)ms->live.len;
    const odin3_pinslice sels = odin3_node_port(ms->m, (odin3_node_id){mi->node}, PORT_S);
    odin3_vec_clear(&ms->na);
    odin3_vec_clear(&ms->nb);
    odin3_vec_clear(&ms->ns);
    odin3_status st = append_slice(ms, mi, live[count - 1U], &ms->na);
    for (uint32_t i = 0; st == ODIN3_OK && i + 1U < count; i++) {
        st = append_slice(ms, mi, live[i], &ms->nb);
        st = st == ODIN3_OK ? odin3_kit_push_net(&ms->ns, pin_net_at(ms, sels, live[i])) : st;
    }
    return st;
}

/* Several live ports (or known bits replaced): the node is rebuilt as a $mux or a narrower $pmux.
 */
static odin3_status narrow_mux(mux_state *ms, const mux_info *mi) {
    odin3_status st = gather_live(ms, mi);
    if (st != ODIN3_OK) {
        return st;
    }
    const uint32_t sel = (uint32_t)ms->ns.len;
    odin3_value params[PRM_PMUX_N] = {odin3_value_int((int64_t)mi->width),
                                      odin3_value_int((int64_t)sel)};
    const odin3_netvec ports[4] = {{ms->na.data, (uint32_t)ms->na.len},
                                   {ms->nb.data, (uint32_t)ms->nb.len},
                                   {ms->ns.data, sel},
                                   {NULL, 0}};
    odin3_kit_rebuild req;
    memset(&req, 0, sizeof req);
    req.old_node = (odin3_node_id){mi->node};
    /* No dead port (only bits replaced): the type stays, a one-select $pmux too (as Yosys). */
    const bool as_mux = sel == 1U && (ms->live.len < mi->n_ports ||
                                      odin3_node_type(ms->m, req.old_node).v == ms->t_mux.v);
    req.cell.type = as_mux ? ms->t_mux : ms->t_pmux;
    req.cell.params = params;
    req.cell.n_params = as_mux ? 1U : PRM_PMUX_N;
    req.cell.ports = ports;
    odin3_node_id fresh = {0};
    odin3_kit_begin_rewrite(ms->oc);
    return odin3_kit_rebuild_node(ms->oc, &req, &fresh);
}

/* Dead ports are logged and counted; the live ones decide the rewrite. */
static odin3_status rewrite_mux(mux_state *ms, uint32_t index) {
    const mux_info *mi = &ms->mx[index];
    uint32_t dead = 0;
    odin3_vec_clear(&ms->live);
    for (uint32_t port = 0; port < mi->n_ports; port++) {
        if (!ms->pt[mi->port0 + port].enabled) {
            log_dead(ms, mi, port);
            dead++;
        } else if (odin3_kit_push_u32(&ms->live, port) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    if (dead == 0 && mi->n_repl == 0) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    if (ms->live.len == 0) {
        st = odin3_node_delete(ms->m, (odin3_node_id){mi->node});
        ms->oc->changed = st == ODIN3_OK ? true : ms->oc->changed;
    } else if (ms->live.len == 1) {
        st = wire_mux(ms, mi, *(const uint32_t *)ms->live.data);
    } else {
        st = narrow_mux(ms, mi);
    }
    ms->removed += st == ODIN3_OK ? dead : 0U;
    return st;
}

/* --- the pass --------------------------------------------------------------------------------- */

static odin3_status muxtree_module(odin3_opt_ctx *oc, uint32_t *removed) {
    mux_state ms;
    memset(&ms, 0, sizeof ms);
    ms.oc = oc;
    ms.m = oc->module;
    ms.t_mux = odin3_kit_type_named(oc->design, "$mux");
    ms.t_pmux = odin3_kit_type_named(oc->design, "$pmux");
    ms.max_evals = odin3_opt_limits_now().muxtree_max_evals;
    vecs_init(&ms);
    odin3_status st = ms.t_mux.v != 0 && ms.t_pmux.v != 0 ? build_index(&ms) : ODIN3_OK;
    st = st == ODIN3_OK && ms.muxes.len > 0 ? eval_all(&ms) : st;
    if (st == ODIN3_OK && ms.gave_up) {
        odin3_log(ODIN3_LOG_WARN,
                  "opt_muxtree: module '%s': giving up after %" PRIu32 " evaluations",
                  module_name(&ms), ms.max_evals);
    }
    for (uint32_t i = 0; st == ODIN3_OK && !ms.gave_up && i < ms.muxes.len; i++) {
        st = rewrite_mux(&ms, i);
    }
    *removed += ms.removed;
    state_free(&ms);
    return st;
}

odin3_status odin3_opt_muxtree_run(odin3_pass_ctx *ctx, odin3_design *design,
                                   const odin3_opt_opts *opts, odin3_opt_counts *counts) {
    if (ctx == NULL || design == NULL || opts == NULL || counts == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "opt_muxtree: no pass run, design, options or counts");
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t removed = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
        if (odin3_module_proc_count(module) > 0) {
            odin3_log(ODIN3_LOG_INFO, "opt_muxtree: module '%s' has processes; skipped",
                      odin3_strtab_get(odin3_design_strtab(design), odin3_module_name(module)));
            continue;
        }
        odin3_opt_ctx oc;
        st = odin3_opt_ctx_init(&oc, ctx, module, opts);
        if (st != ODIN3_OK) {
            break;
        }
        oc.counts = counts;
        st = muxtree_module(&oc, &removed);
        odin3_opt_ctx_free(&oc);
    }
    counts->muxtree_ports += removed;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "opt_muxtree: removed %" PRIu32 " multiplexer ports", removed);
    }
    return st;
}
