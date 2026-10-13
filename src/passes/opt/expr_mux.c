/*
 * expr_mux.c — opt_expr's rules on multiplexers (2F spec §4.3; Yosys 0.55 opt_expr.cc: mux_bool,
 * mux_invert, mux_and, mux_or, mux_undef, mux_empty, mux_sel01, mux_reduce, and the conservative
 * $mux rule that picks A for an x select).
 */
#include "passes/opt/expr.h"

#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/opt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORT_A ODIN3_EXPR_PORT_A
#define PORT_B ODIN3_EXPR_PORT_B
#define PORT_S ODIN3_EXPR_PORT_S

enum { N_BIN_PARAMS = ODIN3_EXPR_P_BIN_YW + 1 };

static bool is_mux(const odin3_expr_ctx *ec) {
    return ec->kind == ODIN3_EXPR_T_MUX || ec->kind == ODIN3_EXPR_T_G_MUX;
}

/* --- -mux_bool: mux_bool, mux_invert, mux_and, mux_or -------------------------------------------
 */

/* A 1-bit mux that becomes a 1-bit gate reading S in input `slot`: its type as a word cell and as a
 * bit gate, and the rule. */
typedef struct mux_gate {
    odin3_expr_type word, bit;
    uint32_t slot;
    odin3_opt_rule rule;
} mux_gate;

static const mux_gate k_to_not = {ODIN3_EXPR_T_NOT, ODIN3_EXPR_T_G_NOT, 0,
                                  ODIN3_OPT_RULE_MUX_INVERT};
static const mux_gate k_to_and = {ODIN3_EXPR_T_AND, ODIN3_EXPR_T_G_AND, 0, ODIN3_OPT_RULE_MUX_AND};
static const mux_gate k_to_or = {ODIN3_EXPR_T_OR, ODIN3_EXPR_T_G_OR, 1, ODIN3_OPT_RULE_MUX_OR};

static odin3_status mux_to_gate(odin3_expr_ctx *ec, const mux_gate *conv) {
    bool gate = ec->kind == ODIN3_EXPR_T_G_MUX;
    odin3_expr_sig sel;
    odin3_status st = odin3_expr_one(ec, odin3_expr_net(ec, PORT_S, 0), &sel);
    odin3_expr_cell spec = {.type = gate ? conv->bit : conv->word};
    spec.in[conv->slot] = &sel;
    if (!gate && conv->word != ODIN3_EXPR_T_NOT) {
        spec.n_params = N_BIN_PARAMS;
        spec.params[ODIN3_EXPR_P_BIN_AW] = spec.params[ODIN3_EXPR_P_BIN_BW] = 1;
        spec.params[ODIN3_EXPR_P_BIN_YW] = 1;
    } else if (!gate) {
        spec.n_params = ODIN3_EXPR_P_UN_YW + 1;
        spec.params[ODIN3_EXPR_P_UN_AW] = spec.params[ODIN3_EXPR_P_UN_YW] = 1;
    }
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, conv->rule, &spec) : st;
}

/* The gate a 1-bit mux with constant A and B becomes (the consume_x ones only in that round). */
static const mux_gate *gate_for(const odin3_expr_ctx *ec, odin3_const lhs, odin3_const rhs) {
    if (lhs == ODIN3_CONST_1 && rhs == ODIN3_CONST_0) {
        return &k_to_not;
    }
    if (ec->consume_x && lhs == ODIN3_CONST_0) {
        return &k_to_and;
    }
    return ec->consume_x && rhs == ODIN3_CONST_1 ? &k_to_or : NULL;
}

odin3_status odin3_expr_rule_mux_bool(odin3_expr_ctx *ec) {
    if (!ec->mux_bool || !is_mux(ec) || odin3_expr_width(ec, PORT_A) != 1) {
        return ODIN3_OK;
    }
    odin3_const lhs = odin3_expr_cval(ec, odin3_expr_net(ec, PORT_A, 0));
    odin3_const rhs = odin3_expr_cval(ec, odin3_expr_net(ec, PORT_B, 0));
    if (lhs == ODIN3_CONST_0 && rhs == ODIN3_CONST_1) {
        return odin3_expr_wire_net(ec, ODIN3_OPT_RULE_MUX_BOOL, odin3_expr_net(ec, PORT_S, 0));
    }
    const mux_gate *conv = gate_for(ec, lhs, rhs);
    return conv != NULL ? mux_to_gate(ec, conv) : ODIN3_OK;
}

/* --- -mux_undef: mux_undef, mux_empty, mux_sel01, mux_reduce -------------------------------------
 */

/* The selects of a $mux / $pmux that stay: B slice i is not all undefined and S[i] is defined. */
static uint32_t kept_selects(const odin3_expr_ctx *ec, uint32_t width, uint32_t *kept) {
    uint32_t n_sel = odin3_expr_width(ec, PORT_S);
    uint32_t n_kept = 0;
    for (uint32_t i = 0; i < n_sel; i++) {
        bool defined_bit = false;
        for (uint32_t k = 0; k < width && !defined_bit; k++) {
            odin3_net_id net = odin3_expr_net(ec, PORT_B, i * width + k);
            defined_bit = !odin3_expr_is_undef(ec, net);
        }
        if (defined_bit && !odin3_expr_is_undef(ec, odin3_expr_net(ec, PORT_S, i))) {
            kept[n_kept++] = i;
        }
    }
    return n_kept;
}

/* The operands of one $mux / $pmux: A, B, S as signals and the selects that stay. */
typedef struct mux_view {
    odin3_expr_sig a, b, s;
    uint32_t *kept;
    uint32_t n_kept, width;
} mux_view;

static odin3_status mux_load(odin3_expr_ctx *ec, mux_view *view) {
    view->width = odin3_expr_width(ec, PORT_A);
    odin3_status st = odin3_expr_read(ec, PORT_A, &view->a);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &view->b) : st;
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_S, &view->s) : st;
    view->kept =
        st == ODIN3_OK ? odin3_expr_alloc(ec, sizeof(uint32_t) * ((size_t)view->s.n + 1)) : NULL;
    if (view->kept == NULL) {
        return st == ODIN3_OK ? ODIN3_ERR_NO_MEMORY : st;
    }
    view->n_kept = kept_selects(ec, view->width, view->kept);
    return ODIN3_OK;
}

/* The mux rebuilt on its kept selects: a $pmux for two or more, else a $mux. */
static odin3_status mux_rebuild(odin3_expr_ctx *ec, const mux_view *view,
                                const odin3_expr_sig *data, uint32_t n_sel) {
    odin3_expr_sig new_b;
    odin3_expr_sig new_s;
    odin3_status st = odin3_expr_sig_new(ec, n_sel * view->width, &new_b);
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, n_sel, &new_s) : st;
    for (uint32_t i = 0; st == ODIN3_OK && i < n_sel; i++) {
        new_s.net[i] = view->s.net[view->kept[i]];
        for (uint32_t k = 0; k < view->width; k++) {
            new_b.net[i * view->width + k] = view->b.net[view->kept[i] * view->width + k];
        }
    }
    odin3_expr_cell spec = {.type = n_sel > 1 ? ODIN3_EXPR_T_PMUX : ODIN3_EXPR_T_MUX,
                            .n_params = n_sel > 1 ? 2U : 1U,
                            .in = {data, &new_b, &new_s}};
    spec.params[ODIN3_EXPR_P_MUX_WIDTH] = view->width;
    spec.params[ODIN3_EXPR_P_PMUX_SWIDTH] = n_sel;
    st = st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_MUX_REDUCE, &spec) : st;
    ec->resume = st == ODIN3_OK; /* Yosys goes on with the reduced cell (its fold, $mux rule) */
    return st;
}

/* After the all-undefined shortcuts: drop the undefined selects and decide what is left. */
static odin3_status mux_reduce_selects(odin3_expr_ctx *ec, mux_view *view, bool a_undef) {
    odin3_expr_sig new_a = view->a;
    uint32_t n_sel = view->n_kept;
    if (a_undef && n_sel > 0) { /* an undefined A takes the last remaining B slice */
        new_a = odin3_expr_slice(&view->b, view->kept[n_sel - 1] * view->width, view->width);
        n_sel--;
    }
    if (n_sel == 0) {
        return odin3_expr_wire(ec, ODIN3_OPT_RULE_MUX_EMPTY, &new_a);
    }
    if (view->width == 1 && n_sel == 1 && odin3_expr_cval(ec, new_a.net[0]) == ODIN3_CONST_0 &&
        odin3_expr_cval(ec, view->b.net[view->kept[0]]) == ODIN3_CONST_1) {
        return odin3_expr_wire_net(ec, ODIN3_OPT_RULE_MUX_SEL01, view->s.net[view->kept[0]]);
    }
    return n_sel != view->s.n ? mux_rebuild(ec, view, &new_a, n_sel) : ODIN3_OK;
}

odin3_status odin3_expr_rule_mux_undef(odin3_expr_ctx *ec) {
    if (!ec->mux_undef || (ec->kind != ODIN3_EXPR_T_MUX && ec->kind != ODIN3_EXPR_T_PMUX)) {
        return ODIN3_OK;
    }
    mux_view view;
    odin3_status st = mux_load(ec, &view);
    if (st != ODIN3_OK) {
        return st;
    }
    bool a_undef = odin3_expr_fully_undef(odin3_expr_sig_census(ec, &view.a));
    bool b_undef = odin3_expr_fully_undef(odin3_expr_sig_census(ec, &view.b));
    if ((a_undef && b_undef) || odin3_expr_fully_undef(odin3_expr_sig_census(ec, &view.s))) {
        return odin3_expr_wire(ec, ODIN3_OPT_RULE_MUX_UNDEF, &view.a);
    }
    return mux_reduce_selects(ec, &view, a_undef);
}

/* --- the conservative $mux rule ------------------------------------------------------------------
 */

/* Be careful with $mux (it may be part of a mux tree): only a constant S, or A the same as B. */
odin3_status odin3_expr_rule_mux_const(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_MUX) {
        return ODIN3_OK;
    }
    odin3_expr_census sel = odin3_expr_port_census(ec, PORT_S);
    if (odin3_expr_fully_const(sel, false) && (!ec->keepdc || odin3_expr_fully_def(sel))) {
        odin3_expr_sig pick; /* S.as_bool(): any 1 bit, so an x select picks A */
        odin3_status st = odin3_expr_read(ec, sel.one > 0 ? PORT_B : PORT_A, &pick);
        return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_MUX_CONST_SEL, &pick) : st;
    }
    uint32_t width = odin3_expr_width(ec, PORT_A);
    for (uint32_t k = 0; k < width; k++) {
        if (!odin3_expr_same(ec, odin3_expr_net(ec, PORT_A, k), odin3_expr_net(ec, PORT_B, k))) {
            return ODIN3_OK;
        }
    }
    odin3_expr_sig lhs;
    odin3_status st = odin3_expr_read(ec, PORT_A, &lhs);
    return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_MUX_SAME, &lhs) : st;
}
