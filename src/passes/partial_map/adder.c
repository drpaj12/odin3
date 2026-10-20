/*
 * adder.c — `$add` and `$sub` to `adder` chains (3C spec §5, A-1..A-9). n = max(A_WIDTH, B_WIDTH);
 * the chain is a head, stages 0 .. n-1 and a top stage n (n + 2 `adder` cells, Parmys's count). A
 * `$sub` is a + ~b + 1: the head has a = b = vcc, so its carry is 1, and every b is inverted (a
 * constant folds, anything else gets one shared `$not` per distinct net). Operands are extended
 * by ext(X) of A-2 (the sign net for a signed cell, else gnd): divergence D1 from Parmys, which
 * pads with `unconn`. Y[i] is the sum of stage i. A Y wider than n + 1 (A-7) stays soft with a
 * warning, as M-3 does for `$mul`: aliasing the extra Y bits onto a net, or onto gnd, would
 * allocate after the source is deleted (H-9); 3G's wreduce caps Y at n + 1.
 */
#include "passes/partial_map/hm_kit.h"

#include "passes/opt/kit.h"
#include "util/log.h"
#include "util/u64map.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { P_A_WIDTH = 2, P_B_WIDTH = 3, P_Y_WIDTH = 4 };
enum { PORT_A, PORT_B, PORT_Y };
enum { ROLE_SIZE = 32, INV_CAP = 16, ADDER_IN = 3, ADDER_PINS = 5, FRESH_PER_ADDER = 2 };
enum { HEAD_A, HEAD_B, HEAD_CIN };

/* The shape of a `$add` / `$sub` cell. */
typedef struct add_shape {
    uint32_t n; /* max(A_WIDTH, B_WIDTH) */
    uint32_t y; /* Y_WIDTH */
    bool is_signed;
    bool sub;
} add_shape;

/* The state of one chain being built. */
typedef struct chain {
    hm_ctx *hc;
    odin3_node_id src;
    add_shape shape;
    odin3_celltype_id adder;
    odin3_u64map *inverted; /* B net -> the output net of its `$not` (A-5); NULL for `$add` */
    odin3_vec *ties;
    odin3_net_id carry; /* cout of the adder made last */
} chain;

static uint32_t param_u32(const hm_ctx *hc, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(hc->module, node, index);
    return val != NULL && val->i > 0 ? (uint32_t)val->i : 0;
}

static add_shape shape_of(const hm_ctx *hc, odin3_node_id node) {
    uint32_t awidth = param_u32(hc, node, P_A_WIDTH);
    uint32_t bwidth = param_u32(hc, node, P_B_WIDTH);
    const odin3_celltype_def *def =
        odin3_celltype_get(hc->design, odin3_node_type(hc->module, node));
    add_shape sh;
    sh.n = awidth > bwidth ? awidth : bwidth;
    sh.y = param_u32(hc, node, P_Y_WIDTH);
    sh.is_signed = odin3_opt_both_signed(hc->module, node);
    sh.sub = def != NULL && strcmp(def->name, "$sub") == 0;
    return sh;
}

/* A-2: ext(X)[bit] of operand port `port`: the bit itself, then the sign net (signed cell) or
 * gnd. */
static odin3_status ext_bit(const chain *ch, uint32_t port, odin3_net_id *out, uint32_t bit) {
    hm_ctx *hc = ch->hc;
    odin3_pinslice pins = odin3_node_port(hc->module, ch->src, port);
    if (bit < pins.count) {
        *out = odin3_pin_net(hc->module, (odin3_pin_id){pins.first.v + bit});
        return ODIN3_OK;
    }
    if (ch->shape.is_signed && pins.count > 0) {
        *out = odin3_pin_net(hc->module, (odin3_pin_id){pins.first.v + pins.count - 1});
        return ODIN3_OK;
    }
    return hm_const(hc, HM_GND, out);
}

/* A-5: ~net. A constant folds; otherwise one `$not` per distinct net. */
static odin3_status inv_bit(chain *ch, odin3_net_id net, uint32_t bit, odin3_net_id *out) {
    hm_ctx *hc = ch->hc;
    odin3_const value =
        odin3_net_valid(net) ? odin3_net_const_value(hc->module, net) : ODIN3_CONST_NONE;
    if (value == ODIN3_CONST_0) {
        return hm_const(hc, HM_VCC, out);
    }
    if (value == ODIN3_CONST_1) {
        return hm_const(hc, HM_GND, out);
    }
    uint64_t found = 0;
    if (odin3_u64map_get(ch->inverted, net.v, &found)) {
        *out = (odin3_net_id){(uint32_t)found};
        return ODIN3_OK;
    }
    char role[ROLE_SIZE];
    (void)snprintf(role, sizeof role, "nb%" PRIu32, bit);
    odin3_net_id inv = {0};
    odin3_status st = hm_not(hc, role, net, &inv);
    st = st == ODIN3_OK ? odin3_u64map_put(ch->inverted, (odin3_kv){net.v, inv.v}) : st;
    *out = inv;
    return st;
}

/* One `adder` on in = {a, b, cin} and two fresh nets; ch->carry becomes its cout, *sum its
 * sumout. */
static odin3_status add_adder(chain *ch, const char *role, const odin3_net_id in[ADDER_IN],
                              odin3_net_id *sum) {
    odin3_net_id fresh[FRESH_PER_ADDER] = {{0}, {0}}; /* cout, sumout */
    odin3_status st = hm_fresh_nets(ch->hc, FRESH_PER_ADDER, fresh);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_netvec ports[ADDER_PINS] = {
        {&in[HEAD_A], 1}, {&in[HEAD_B], 1}, {&in[HEAD_CIN], 1}, {&fresh[0], 1}, {&fresh[1], 1}};
    odin3_node_id node = {0};
    st = hm_new_node(ch->hc, &(hm_node_req){ch->adder, NULL, 0, ports, role}, &node);
    if (st == ODIN3_OK) {
        ch->carry = fresh[0];
        *sum = fresh[1];
    }
    return st;
}

/* A-4, A-5: the head. `$add`: a = vcc, b = gnd, cin = gnd. `$sub`: a = b = vcc, cin = the pad. */
static odin3_status add_head(chain *ch) {
    odin3_net_id in[ADDER_IN] = {{0}, {0}, {0}};
    odin3_net_id sum = {0};
    hm_bit cin = ch->shape.sub ? HM_PAD : HM_GND;
    hm_bit bin = ch->shape.sub ? HM_VCC : HM_GND;
    odin3_status st = hm_const(ch->hc, HM_VCC, &in[HEAD_A]);
    st = st == ODIN3_OK ? hm_const(ch->hc, bin, &in[HEAD_B]) : st;
    st = st == ODIN3_OK ? hm_const(ch->hc, cin, &in[HEAD_CIN]) : st;
    return st == ODIN3_OK ? add_adder(ch, "head", in, &sum) : st;
}

/* Stage `bit` (0 <= bit <= n): a = ext(A)[bit], b = ext(B)[bit] (inverted for `$sub`), cin = the
 * carry. */
static odin3_status add_stage(chain *ch, uint32_t bit) {
    odin3_net_id in[ADDER_IN] = {{0}, {0}, {0}};
    odin3_net_id sum = {0};
    char role[ROLE_SIZE];
    odin3_status st = ext_bit(ch, PORT_A, &in[HEAD_A], bit);
    st = st == ODIN3_OK ? ext_bit(ch, PORT_B, &in[HEAD_B], bit) : st;
    if (st == ODIN3_OK && ch->shape.sub) {
        st = inv_bit(ch, in[HEAD_B], bit, &in[HEAD_B]);
    }
    in[HEAD_CIN] = ch->carry;
    if (bit == ch->shape.n) {
        (void)snprintf(role, sizeof role, "top");
    } else {
        (void)snprintf(role, sizeof role, "s%" PRIu32, bit);
    }
    st = st == ODIN3_OK ? add_adder(ch, role, in, &sum) : st;
    odin3_pinslice ypins = odin3_node_port(ch->hc->module, ch->src, PORT_Y);
    if (st == ODIN3_OK && bit < ch->shape.y && bit < ypins.count) {
        hm_tie *tie = odin3_vec_push(ch->ties); /* reserved by build: never NULL */
        odin3_net_id ynet = odin3_pin_net(ch->hc->module, (odin3_pin_id){ypins.first.v + bit});
        *tie = (hm_tie){ynet, sum};
    }
    return st;
}

/* Builds the chain on fresh nets and the ties; nothing of the source changes. */
static odin3_status build(hm_ctx *hc, odin3_node_id src, const add_shape *sh, odin3_vec *ties) {
    chain ch = {hc, src, *sh, {0}, NULL, ties, {0}};
    odin3_status st = hm_need(hc, HM_ADDER, src, &ch.adder);
    st = st == ODIN3_OK ? hm_begin_cell(hc, src) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(ties, sh->y) : st;
    if (st == ODIN3_OK && sh->sub) {
        ch.inverted = odin3_u64map_create(INV_CAP);
        st = ch.inverted != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    st = st == ODIN3_OK ? add_head(&ch) : st;
    for (uint32_t i = 0; st == ODIN3_OK && i <= sh->n; i++) {
        st = add_stage(&ch, i);
    }
    odin3_u64map_destroy(ch.inverted);
    return st;
}

/* One transaction for src: build on fresh nets, then commit; the undo on failure. */
static odin3_status transact(hm_ctx *hc, odin3_node_id src, const add_shape *sh) {
    odin3_vec ties;
    odin3_vec_init(&ties, sizeof(hm_tie));
    odin3_status st = build(hc, src, sh, &ties);
    if (st != ODIN3_OK) {
        hm_abort(hc);
    } else {
        st = hm_commit(hc, odin3_vec_cat(&ties, 0), ties.len);
    }
    odin3_vec_free(&ties);
    return st;
}

odin3_status hm_map_adder(hm_ctx *hc, odin3_node_id src) {
    add_shape sh = shape_of(hc, src);
    if (sh.n == 0 || sh.y == 0) {
        return ODIN3_OK; /* a degenerate cell (A-1): left alone */
    }
    if (sh.y > sh.n + 1) {
        char label[ODIN3_LOG_BUF / 4];
        hm_label(hc, src, label, sizeof label);
        hm_diag(hc, ODIN3_LOG_WARN, src,
                "partial_map: '%s' cell '%s' has Y_WIDTH %" PRIu32
                " above max(A_WIDTH, B_WIDTH) + 1 (%" PRIu32
                "); left soft for lower (run wreduce first)",
                sh.sub ? "$sub" : "$add", label, sh.y, sh.n + 1);
        hc->stats.soft++;
        return ODIN3_OK;
    }
    odin3_status st = transact(hc, src, &sh);
    if (st == ODIN3_OK) {
        hc->stats.adder += (uint64_t)sh.n + 2U;
        hc->stats.chains++;
    }
    return st;
}
