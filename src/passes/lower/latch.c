/*
 * latch.c — the latches of `lower` (3B spec §6.2, LW-8; plan Task 7): the four word latches
 * $dlatch $adlatch $dlatchsr $sr become one `$_DLATCH_P_` per bit (or `$_DLATCH_N_`). Unlike the
 * flip-flops there is no divergence: a latch is level-sensitive and transparent, so an asynchronous
 * control is absorbed into its enable and data, as Yosys's dfflegalize does. With `act(x)` = x for
 * an active-high control and `$_NOT_(x)` for an active-low one (one inverter, shared where the
 * control is one bit wide), the latch of bit b has
 *
 *   $dlatch    E = EN (the latch is $_DLATCH_N_ when EN_POLARITY = 0)        D = D[b]
 *   $adlatch   E = act(EN) | act(ARST)                                       D = ARST ? VALUE[b] :
 * D[b] $dlatchsr  E = act(EN) | act(SET[b]) | act(CLR[b])                       D = CLR[b] ? 0 :
 * (SET[b] ? 1 : D[b]) $sr        E = act(SET[b]) | act(CLR[b])                                 D =
 * !act(CLR[b])
 *
 * (clear wins over set, simlib). The data mux of an active-low control swaps its arms, so only the
 * enable needs an inverter. E is an OR of nets, which is legal (IR-8: a latch enable may come from
 * logic). INIT of each latch is the `init` attribute of its Q net, else 3 (as the flip-flops).
 */
#include "passes/ffinit.h"
#include "passes/lower/lower_int.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LT_INIT_UNKNOWN = 3 };

/* What a type has besides D. */
typedef struct lt_shape {
    bool has_en;
    bool has_arst;
    bool has_setclr;
    uint32_t max_gates; /* per bit, an upper bound: muxes, ORs, inverters and the latch */
    uint32_t fixed;     /* per cell, an upper bound: the shared inverters and OR */
} lt_shape;

enum { LT_DLATCH, LT_ADLATCH, LT_DLATCHSR, LT_SR, LT_COUNT };

static const lt_shape k_shapes[LT_COUNT] = {
    {true, false, false, 1, 0}, /* latch */
    {true, true, false, 2, 3},  /* mux, latch; 2 inverters and the OR */
    {true, false, true, 7, 1},  /* 2 muxes, 2 ORs, 2 inverters, latch; the EN inverter */
    {false, false, true, 4, 0}, /* OR, 2 inverters, latch */
};

/* The cell being lowered, by role. */
typedef struct lt_view {
    const lt_shape *shape;
    uint32_t width;
    lw_sel en;
    lw_sel arst;
    lw_sel set; /* per bit: the net is set per bit, the polarity here */
    lw_sel clr;
    lw_span data;
    lw_span set_nets;
    lw_span clr_nets;
    lw_span out; /* the cell's Q nets as they were */
    const odin3_value *value;
    odin3_net_id en_act;      /* act(EN), made once per cell */
    odin3_net_id cell_enable; /* E of an $adlatch, made once per cell */
} lt_view;

/* The names of a control: its port and its polarity parameter. */
typedef struct lt_names {
    const char *port;
    const char *polarity;
} lt_names;

static lw_sel sel_of(const lw_cell *cell, const lt_names *names) {
    lw_span span = lw_cell_span(cell, names->port);
    return (lw_sel){span.n > 0 ? span.nets[0] : (odin3_net_id){0},
                    lw_cell_polarity(cell, names->polarity)};
}

static void view_load(const lw_cell *cell, const lt_shape *shape, lt_view *view) {
    *view = (lt_view){.shape = shape};
    view->width = cell->port[cell->out_port].n;
    view->en = sel_of(cell, &(lt_names){"EN", "EN_POLARITY"});
    view->arst = sel_of(cell, &(lt_names){"ARST", "ARST_POLARITY"});
    view->set = (lw_sel){{0}, lw_cell_polarity(cell, "SET_POLARITY")};
    view->clr = (lw_sel){{0}, lw_cell_polarity(cell, "CLR_POLARITY")};
    view->data = lw_cell_span(cell, "D");
    view->set_nets = lw_cell_span(cell, "SET");
    view->clr_nets = lw_cell_span(cell, "CLR");
    view->out = cell->port[cell->out_port];
    view->value = lw_cell_param(cell, "ARST_VALUE");
}

/* act(sel): the net itself when active high, else its inverter. */
static odin3_status active(lw_ctx *lw, lw_sel sel, odin3_net_id *out) {
    if (sel.high) {
        *out = sel.net;
        return ODIN3_OK;
    }
    return lw_not(lw, sel.net, out);
}

/* `net | act(sel)`; a zero net starts the chain. */
static odin3_status or_active(lw_ctx *lw, odin3_net_id net, lw_sel sel, odin3_net_id *out) {
    odin3_net_id act = {0};
    odin3_status st = active(lw, sel, &act);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_net_valid(net)) {
        *out = act;
        return ODIN3_OK;
    }
    return lw_or(lw, net, act, out);
}

/* The constant net of bit `bit` of ARST_VALUE (x and z give the x constant). */
static odin3_status value_net(lw_ctx *lw, const lt_view *view, uint32_t bit, odin3_net_id *out) {
    odin3_bit val = (odin3_bit)view->value->bits[bit];
    return lw_const(lw, val == ODIN3_BIT_0 || val == ODIN3_BIT_1 ? val : ODIN3_BIT_X, out);
}

/* The shared parts of a cell: act(EN) for the $dlatchsr, E for the $adlatch. */
static odin3_status cell_parts(lw_ctx *lw, lt_view *view) {
    odin3_status st = ODIN3_OK;
    if (view->shape->has_en && view->shape->has_setclr) {
        st = active(lw, view->en, &view->en_act);
    } else if (view->shape->has_arst) {
        st = or_active(lw, (odin3_net_id){0}, view->en, &view->cell_enable);
        st = st == ODIN3_OK ? or_active(lw, view->cell_enable, view->arst, &view->cell_enable) : st;
    }
    return st;
}

/* E and D of bit `bit`. */
typedef struct lt_pins {
    odin3_net_id enable;
    odin3_net_id data;
} lt_pins;

/* $dlatchsr: D = CLR ? 0 : (SET ? 1 : D). */
static odin3_status setclr_data(lw_ctx *lw, const lt_view *view, uint32_t bit, odin3_net_id *out) {
    odin3_net_id zero = {0};
    odin3_net_id one = {0};
    odin3_status st = lw_const(lw, ODIN3_BIT_0, &zero);
    st = st == ODIN3_OK ? lw_const(lw, ODIN3_BIT_1, &one) : st;
    odin3_net_id net = view->data.nets[bit];
    lw_sel set = {view->set_nets.nets[bit], view->set.high};
    lw_sel clr = {view->clr_nets.nets[bit], view->clr.high};
    st = st == ODIN3_OK ? lw_pick(lw, set, one, net, &net) : st;
    return st == ODIN3_OK ? lw_pick(lw, clr, zero, net, out) : st;
}

/* E = act(EN)? | act(SET[bit]) | act(CLR[bit]) for the types with SET and CLR. */
static odin3_status setclr_enable(lw_ctx *lw, const lt_view *view, uint32_t bit,
                                  odin3_net_id *out) {
    odin3_net_id net = view->shape->has_en ? view->en_act : (odin3_net_id){0};
    odin3_status st = or_active(lw, net, (lw_sel){view->set_nets.nets[bit], view->set.high}, &net);
    return st == ODIN3_OK
               ? or_active(lw, net, (lw_sel){view->clr_nets.nets[bit], view->clr.high}, out)
               : st;
}

/* $sr: D = !act(CLR[bit]); no inverter when CLR is active low. */
static odin3_status sr_data(lw_ctx *lw, const lt_view *view, uint32_t bit, odin3_net_id *out) {
    odin3_net_id clr = view->clr_nets.nets[bit];
    if (!view->clr.high) {
        *out = clr;
        return ODIN3_OK;
    }
    return lw_not(lw, clr, out);
}

static odin3_status bit_pins(lw_ctx *lw, const lt_view *view, uint32_t bit, lt_pins *pins) {
    odin3_status st = ODIN3_OK;
    pins->data = view->data.n > bit ? view->data.nets[bit] : (odin3_net_id){0};
    if (view->shape->has_arst) {
        pins->enable = view->cell_enable;
        odin3_net_id arm = {0};
        st = value_net(lw, view, bit, &arm);
        return st == ODIN3_OK ? lw_pick(lw, view->arst, arm, pins->data, &pins->data) : st;
    }
    if (!view->shape->has_setclr) {
        pins->enable = view->en.net;
        return ODIN3_OK;
    }
    st = setclr_enable(lw, view, bit, &pins->enable);
    if (st != ODIN3_OK) {
        return st;
    }
    return view->shape->has_en ? setclr_data(lw, view, bit, &pins->data)
                               : sr_data(lw, view, bit, &pins->data);
}

/* True when the latch is $_DLATCH_N_ on the enable itself (a $dlatch with EN_POLARITY = 0). */
static bool negative_latch(const lt_view *view) {
    return !view->shape->has_arst && !view->shape->has_setclr && !view->en.high;
}

/* One latch node on `qnet`: E, D, INIT. */
static odin3_status storage(lw_ctx *lw, const lt_view *view, const lt_pins *pins, odin3_net_id qnet,
                            odin3_bit init) {
    int64_t value = init == ODIN3_BIT_0 || init == ODIN3_BIT_1 ? (int64_t)init : LT_INIT_UNKNOWN;
    const odin3_value param = odin3_value_int(value);
    const odin3_net_id in[2] = {pins->enable, pins->data};
    const lw_gate_spec spec = {negative_latch(view) ? LW_G_DLATCH_N : LW_G_DLATCH_P, in, 2, &param};
    return lw_gate_on(lw, &spec, qnet);
}

static odin3_status build_bit(lw_ctx *lw, const lt_view *view, uint32_t bit, odin3_net_id *qnet) {
    odin3_status st = lw_net(lw, qnet);
    lt_pins pins = {{0}, {0}};
    st = st == ODIN3_OK ? bit_pins(lw, view, bit, &pins) : st;
    odin3_net_id old = bit < view->out.n ? view->out.nets[bit] : (odin3_net_id){0};
    odin3_bit init = odin3_net_valid(old) ? odin3_ffinit_get(lw->module, old) : ODIN3_BIT_X;
    return st == ODIN3_OK ? storage(lw, view, &pins, *qnet, init) : st;
}

static odin3_status build_latch(lw_ctx *lw, const lw_cell *cell, const lt_shape *shape,
                                lw_bits *out) {
    lt_view view;
    view_load(cell, shape, &view);
    odin3_status st = cell_parts(lw, &view);
    lw_bits_clear(out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < view.width; bit++) {
        odin3_net_id qnet = {0};
        st = build_bit(lw, &view, bit, &qnet);
        st = st == ODIN3_OK ? lw_bits_push(out, qnet) : st;
    }
    return st;
}

/* An upper bound of the gates: `max_gates` per bit and the shared ones of the cell. */
static uint64_t latch_estimate(const lw_cell *cell, const lt_shape *shape) {
    uint64_t width = cell->port[cell->out_port].n;
    return lw_sat_add(lw_sat_mul(width, shape->max_gates), shape->fixed);
}

/* The 8 entry points of the dispatch table. */
#define LT_RULE(fn, kind)                                                                          \
    odin3_status lw_build_##fn(lw_ctx *lw, const lw_cell *cell, lw_bits *out) {                    \
        return build_latch(lw, cell, &k_shapes[kind], out);                                        \
    }                                                                                              \
    uint64_t lw_est_##fn(const lw_cell *cell) {                                                    \
        return latch_estimate(cell, &k_shapes[kind]);                                              \
    }

LT_RULE(dlatch, LT_DLATCH)
LT_RULE(adlatch, LT_ADLATCH)
LT_RULE(dlatchsr, LT_DLATCHSR)
LT_RULE(sr, LT_SR)
