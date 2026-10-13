/*
 * expr_logic.c — opt_expr's rules on logic cells (2F spec §4.2; Yosys 0.55 opt_expr.cc, the rules
 * named const_and ... bitwise_logic_one_const, invert.double/muxsel, the gate tables and $tribuf).
 * The rules read the cell through the visit (expr.h) and rewrite it through the primitives of
 * expr_util.c.
 */
#include "passes/opt/expr.h"

#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORT_A ODIN3_EXPR_PORT_A
#define PORT_B ODIN3_EXPR_PORT_B
#define PORT_S ODIN3_EXPR_PORT_S

/* A 1-bit unsigned binary cell (Yosys: GetSize(A) == 1 && GetSize(B) == 1 && !A_SIGNED). */
static bool one_bit_unsigned(const odin3_expr_ctx *ec) {
    return odin3_expr_width(ec, PORT_A) == 1 && odin3_expr_width(ec, PORT_B) == 1 &&
           odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED) == 0;
}

/* --- const_and, const_or, and_or_buffer --------------------------------------------------------
 */

typedef enum and_or { AO_NONE, AO_AND, AO_OR } and_or;

static and_or and_or_of(const odin3_expr_ctx *ec) {
    switch (ec->kind) {
    case ODIN3_EXPR_T_REDUCE_AND:
    case ODIN3_EXPR_T_G_AND:
        return AO_AND;
    case ODIN3_EXPR_T_REDUCE_OR:
    case ODIN3_EXPR_T_REDUCE_BOOL:
    case ODIN3_EXPR_T_G_OR:
        return AO_OR;
    case ODIN3_EXPR_T_AND:
    case ODIN3_EXPR_T_LOGIC_AND:
        return one_bit_unsigned(ec) ? AO_AND : AO_NONE;
    case ODIN3_EXPR_T_OR:
    case ODIN3_EXPR_T_LOGIC_OR:
        return one_bit_unsigned(ec) ? AO_OR : AO_NONE;
    default:
        return AO_NONE;
    }
}

static bool has_b(const odin3_expr_ctx *ec) {
    return ec->kind != ODIN3_EXPR_T_REDUCE_AND && ec->kind != ODIN3_EXPR_T_REDUCE_OR &&
           ec->kind != ODIN3_EXPR_T_REDUCE_BOOL;
}

/* What the input bits are: the constants present, and how many different nets are not constant. */
typedef struct bit_scan {
    bool zero, one, undef, many, inverse;
    odin3_net_id first;
} bit_scan;

static void scan_port(odin3_expr_ctx *ec, odin3_expr_port port, bit_scan *scan) {
    uint32_t width = odin3_expr_width(ec, port);
    for (uint32_t k = 0; k < width; k++) {
        odin3_net_id net = odin3_expr_net(ec, port, k);
        switch (odin3_expr_cval(ec, net)) {
        case ODIN3_CONST_0:
            scan->zero = true;
            break;
        case ODIN3_CONST_1:
            scan->one = true;
            break;
        case ODIN3_CONST_X:
        case ODIN3_CONST_Z:
            scan->undef = true;
            break;
        case ODIN3_CONST_NONE:
        default:
            odin3_expr_mark(ec, net);
            if (!odin3_net_valid(scan->first)) {
                scan->first = net;
            } else if (scan->first.v != net.v) {
                scan->many = true;
            }
            break;
        }
    }
}

/* A bit and its inverse (the invert map) are both inputs. */
static void scan_inverse(const odin3_expr_ctx *ec, odin3_expr_port port, bit_scan *scan) {
    uint32_t width = odin3_expr_width(ec, port);
    for (uint32_t k = 0; k < width && !scan->inverse; k++) {
        odin3_net_id net = odin3_expr_net(ec, port, k);
        if (odin3_expr_cval(ec, net) == ODIN3_CONST_NONE) {
            odin3_net_id inverse = odin3_kit_inverse(ec->oc, net);
            scan->inverse = odin3_net_valid(inverse) && odin3_expr_marked(ec, inverse);
        }
    }
}

static void scan_all(odin3_expr_ctx *ec, bit_scan *scan) {
    scan_port(ec, PORT_A, scan);
    if (has_b(ec)) {
        scan_port(ec, PORT_B, scan);
    }
    scan_inverse(ec, PORT_A, scan);
    if (has_b(ec)) {
        scan_inverse(ec, PORT_B, scan);
    }
}

odin3_status odin3_expr_rule_and_or(odin3_expr_ctx *ec) {
    and_or kind = and_or_of(ec);
    if (kind == AO_NONE) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_expr_mark_begin(ec);
    if (st != ODIN3_OK) {
        return st;
    }
    bit_scan scan = {0};
    scan_all(ec, &scan);
    bool undef_x = scan.undef && ec->consume_x;
    bool dominated = kind == AO_AND ? scan.zero : scan.one;
    if (dominated || scan.inverse || undef_x) {
        return kind == AO_AND ? odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_CONST_AND, ODIN3_BIT_0)
                              : odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_CONST_OR, ODIN3_BIT_1);
    }
    if (odin3_net_valid(scan.first) && !scan.many && !scan.undef) {
        return odin3_expr_wire_net(ec, ODIN3_OPT_RULE_AND_OR_BUFFER, scan.first);
    }
    return ODIN3_OK;
}

/* --- const_xor, const_xnor, xor_buffer, xnor_buffer --------------------------------------------
 */

static bool xor_applies(const odin3_expr_ctx *ec) {
    switch (ec->kind) {
    case ODIN3_EXPR_T_G_XOR:
    case ODIN3_EXPR_T_G_XNOR:
        return true;
    case ODIN3_EXPR_T_XOR:
    case ODIN3_EXPR_T_XNOR:
        return one_bit_unsigned(ec);
    default:
        return false;
    }
}

/* The inverse of net: a new $not (or $_NOT_ in a gate netlist) on it; *out the new net. */
static odin3_status invert_net(odin3_expr_ctx *ec, bool gate, odin3_net_id net, odin3_net_id *out) {
    odin3_expr_sig in;
    odin3_expr_sig made;
    odin3_status st = odin3_expr_one(ec, net, &in);
    st = st == ODIN3_OK
             ? odin3_expr_new_un(ec, gate ? ODIN3_EXPR_T_G_NOT : ODIN3_EXPR_T_NOT, &in, 1, &made)
             : st;
    if (st == ODIN3_OK) {
        *out = made.net[0];
    }
    return st;
}

/* a ^ 0, a ^ 1 and the xnor mirror: Y (the $xnor's upper bits all 1) is the buffer or the inverter.
 */
static odin3_status xor_with_const(odin3_expr_ctx *ec, odin3_net_id var, bool one) {
    bool xnor = ec->kind == ODIN3_EXPR_T_XNOR || ec->kind == ODIN3_EXPR_T_G_XNOR;
    bool gate = ec->kind == ODIN3_EXPR_T_G_XOR || ec->kind == ODIN3_EXPR_T_G_XNOR;
    odin3_net_id value = var;
    odin3_status st = ODIN3_OK;
    if (xnor != one) { /* xor with 1 and xnor with 0 invert */
        st = invert_net(ec, gate, var, &value);
    }
    if (st != ODIN3_OK || !xnor) {
        return st != ODIN3_OK ? st : odin3_expr_wire_net(ec, ODIN3_OPT_RULE_XOR_BUFFER, value);
    }
    uint32_t width = odin3_expr_y_width(ec);
    odin3_expr_sig ones;
    odin3_expr_sig low;
    odin3_expr_sig whole;
    st = odin3_expr_fill(ec, ODIN3_BIT_1, width - 1, &ones);
    st = st == ODIN3_OK ? odin3_expr_one(ec, value, &low) : st;
    st = st == ODIN3_OK ? odin3_expr_join(ec, &low, &ones, &whole) : st;
    return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_XNOR_BUFFER, &whole) : st;
}

odin3_status odin3_expr_rule_xor(odin3_expr_ctx *ec) {
    if (!xor_applies(ec)) {
        return ODIN3_OK;
    }
    bool xnor = ec->kind == ODIN3_EXPR_T_XNOR || ec->kind == ODIN3_EXPR_T_G_XNOR;
    odin3_net_id lhs = odin3_expr_net(ec, PORT_A, 0);
    odin3_net_id rhs = odin3_expr_net(ec, PORT_B, 0);
    if (!ec->keepdc && (odin3_expr_same(ec, lhs, rhs) || odin3_expr_is_undef(ec, lhs) ||
                        odin3_expr_is_undef(ec, rhs))) {
        if (!xnor) {
            return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_CONST_XOR, ODIN3_BIT_0);
        }
        odin3_expr_sig ones; /* "for consistency since simplemap does $xnor -> $_XOR_ + $_NOT_" */
        odin3_status st = odin3_expr_fill(ec, ODIN3_BIT_1, odin3_expr_y_width(ec), &ones);
        return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_CONST_XNOR, &ones) : st;
    }
    if (odin3_expr_cval(ec, lhs) != ODIN3_CONST_NONE) { /* Yosys: if (!sig_a.wire) swap */
        odin3_net_id tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }
    odin3_const side = odin3_expr_cval(ec, rhs);
    if (side == ODIN3_CONST_0 || side == ODIN3_CONST_1) {
        return xor_with_const(ec, lhs, side == ODIN3_CONST_1);
    }
    return ODIN3_OK;
}

/* --- reduce_xnor_not, unary_buffer --------------------------------------------------------------
 */

odin3_status odin3_expr_rule_unary(odin3_expr_ctx *ec) {
    switch (ec->kind) {
    case ODIN3_EXPR_T_REDUCE_AND:
    case ODIN3_EXPR_T_REDUCE_OR:
    case ODIN3_EXPR_T_REDUCE_BOOL:
    case ODIN3_EXPR_T_REDUCE_XOR:
    case ODIN3_EXPR_T_REDUCE_XNOR:
    case ODIN3_EXPR_T_NEG:
        break;
    default:
        return ODIN3_OK;
    }
    if (odin3_expr_width(ec, PORT_A) != 1 || odin3_expr_y_width(ec) != 1) {
        return ODIN3_OK;
    }
    if (ec->kind == ODIN3_EXPR_T_REDUCE_XNOR) {
        odin3_expr_cell spec = {.type = ODIN3_EXPR_T_NOT, .n_params = 3};
        spec.params[ODIN3_EXPR_P_UN_SIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_UN_SIGNED);
        spec.params[ODIN3_EXPR_P_UN_AW] = 1;
        spec.params[ODIN3_EXPR_P_UN_YW] = 1;
        return odin3_expr_rebuild(ec, ODIN3_OPT_RULE_REDUCE_XNOR_NOT, &spec);
    }
    return odin3_expr_wire_net(ec, ODIN3_OPT_RULE_UNARY_BUFFER, odin3_expr_net(ec, PORT_A, 0));
}

/* --- bitwise_logic_one_const --------------------------------------------------------------------
 */

/* What becomes of the Y bits whose constant operand bit is 0, 1 or x. */
typedef enum bw_action { BW_ZERO, BW_ONE, BW_X, BW_PASS, BW_NOT, BW_GATE } bw_action;

enum { GROUPS = 3, GROUP_0 = 0, GROUP_1 = 1, GROUP_X = 2, NO_GROUP = 3 };

/* Per type ($and, $or, $xor, $xnor) and constant bit value (0, 1, x without -keepdc). */
static const bw_action k_bw_actions[4][GROUPS] = {{BW_ZERO, BW_PASS, BW_ZERO},
                                                  {BW_PASS, BW_ONE, BW_ONE},
                                                  {BW_PASS, BW_NOT, BW_X},
                                                  {BW_NOT, BW_PASS, BW_X}};

static uint32_t bw_row(odin3_expr_type kind) {
    switch (kind) {
    case ODIN3_EXPR_T_AND:
        return 0;
    case ODIN3_EXPR_T_OR:
        return 1;
    case ODIN3_EXPR_T_XOR:
        return 2;
    default:
        return 3;
    }
}

static bw_action bw_action_of(const odin3_expr_ctx *ec, uint32_t group) {
    return group == GROUP_X && ec->keepdc ? BW_GATE : k_bw_actions[bw_row(ec->kind)][group];
}

/* The visit's operands extended to Y's width, the Y bits, and the destination of each Y bit. */
typedef struct bw_state {
    odin3_expr_sig var, cst, dest;
    uint32_t y_width;
    uint32_t *group_of; /* per Y bit: GROUP_0/1/X or NO_GROUP (a z bit, which Yosys leaves open) */
} bw_state;

static odin3_status bw_load(odin3_expr_ctx *ec, bool a_const, bw_state *st) {
    odin3_expr_port y_port = odin3_expr_y_port(ec);
    st->y_width = odin3_expr_width(ec, y_port);
    odin3_expr_sig lhs;
    odin3_expr_sig rhs;
    odin3_status res = odin3_expr_read(ec, PORT_A, &lhs);
    res = res == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &rhs) : res;
    odin3_expr_sig lhs_ext;
    odin3_expr_sig rhs_ext;
    res = res == ODIN3_OK
              ? odin3_expr_extend(ec, &lhs, st->y_width,
                                  odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED) != 0, &lhs_ext)
              : res;
    res = res == ODIN3_OK
              ? odin3_expr_extend(ec, &rhs, st->y_width,
                                  odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED) != 0, &rhs_ext)
              : res;
    st->cst = a_const ? lhs_ext : rhs_ext;
    st->var = a_const ? rhs_ext : lhs_ext;
    res = res == ODIN3_OK ? odin3_expr_read(ec, y_port, &st->dest) : res; /* open bits: Y itself */
    st->group_of =
        res == ODIN3_OK ? odin3_expr_alloc(ec, sizeof(uint32_t) * (st->y_width + 1)) : NULL;
    return st->group_of != NULL ? res : ODIN3_ERR_NO_MEMORY;
}

static uint32_t group_of_const(odin3_const value) {
    switch (value) {
    case ODIN3_CONST_0:
        return GROUP_0;
    case ODIN3_CONST_1:
        return GROUP_1;
    case ODIN3_CONST_X:
        return GROUP_X;
    case ODIN3_CONST_Z:
    case ODIN3_CONST_NONE:
    default:
        return NO_GROUP;
    }
}

/* The other operand's bits for the Y bits of one group, in Y order. */
static odin3_status bw_members(odin3_expr_ctx *ec, const bw_state *st, uint32_t group,
                               odin3_expr_sig *vars) {
    uint32_t members = 0;
    for (uint32_t k = 0; k < st->y_width; k++) {
        members += st->group_of[k] == group ? 1U : 0U;
    }
    odin3_status res = odin3_expr_sig_new(ec, members, vars);
    for (uint32_t k = 0, j = 0; res == ODIN3_OK && k < st->y_width; k++) {
        if (st->group_of[k] == group) {
            vars->net[j++] = st->var.net[k];
        }
    }
    return res;
}

/* The cells an action needs on a group's operand bits: their outputs, none for the others. */
static odin3_status bw_cells(odin3_expr_ctx *ec, bw_action action, const odin3_expr_sig *vars,
                             odin3_expr_sig *made) {
    *made = (odin3_expr_sig){NULL, 0};
    if (action == BW_NOT) {
        return odin3_expr_new_un(ec, ODIN3_EXPR_T_NOT, vars, vars->n, made);
    }
    if (action != BW_GATE) {
        return ODIN3_OK;
    }
    odin3_expr_sig unknown;
    odin3_status res = odin3_expr_fill(ec, ODIN3_BIT_X, vars->n, &unknown);
    /* Yosys builds module->Xor(x, b) for both $xor and $xnor */
    odin3_expr_type gate = ec->kind == ODIN3_EXPR_T_XNOR ? ODIN3_EXPR_T_XOR : ec->kind;
    return res == ODIN3_OK ? odin3_expr_new_bin(ec, gate, &unknown, vars, made) : res;
}

static odin3_bit bw_constant(bw_action action) {
    switch (action) {
    case BW_ZERO:
        return ODIN3_BIT_0;
    case BW_ONE:
        return ODIN3_BIT_1;
    default:
        return ODIN3_BIT_X;
    }
}

/* Sets the destinations of one group's Y bits: constants, the other operand, or new cells. */
static odin3_status bw_apply(odin3_expr_ctx *ec, bw_state *st, uint32_t group) {
    odin3_expr_sig vars;
    odin3_status res = bw_members(ec, st, group, &vars);
    if (res != ODIN3_OK || vars.n == 0) {
        return res;
    }
    bw_action action = bw_action_of(ec, group);
    odin3_expr_sig made;
    res = bw_cells(ec, action, &vars, &made);
    odin3_net_id fixed = {0};
    if (res == ODIN3_OK && action != BW_PASS && action != BW_NOT && action != BW_GATE) {
        res = odin3_opt_const_net(ec->oc, bw_constant(action), &fixed);
    }
    const odin3_net_id *from = made.net; /* the nets the group's Y bits take, else `fixed` */
    if (from == NULL && action == BW_PASS) {
        from = vars.net;
    }
    for (uint32_t k = 0, j = 0; res == ODIN3_OK && k < st->y_width; k++) {
        if (st->group_of[k] == group) {
            st->dest.net[k] = from != NULL ? from[j] : fixed;
            j++;
        }
    }
    return res;
}

odin3_status odin3_expr_rule_bitwise(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_AND && ec->kind != ODIN3_EXPR_T_OR &&
        ec->kind != ODIN3_EXPR_T_XOR && ec->kind != ODIN3_EXPR_T_XNOR) {
        return ODIN3_OK;
    }
    bool a_const = odin3_expr_fully_const(odin3_expr_port_census(ec, PORT_A), ec->keepdc);
    bool b_const = odin3_expr_fully_const(odin3_expr_port_census(ec, PORT_B), ec->keepdc);
    if (a_const == b_const) {
        return ODIN3_OK;
    }
    bw_state st;
    odin3_status res = bw_load(ec, a_const, &st);
    for (uint32_t k = 0; res == ODIN3_OK && k < st.y_width; k++) {
        st.group_of[k] = group_of_const(odin3_expr_cval(ec, st.cst.net[k]));
    }
    for (uint32_t group = 0; res == ODIN3_OK && group < GROUPS; group++) {
        res = bw_apply(ec, &st, group);
    }
    return res == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_BITWISE_LOGIC_ONE_CONST, &st.dest)
                           : res;
}

/* --- invert.double, invert.muxsel ----------------------------------------------------------------
 */

/* The live net that is the inverse of net, from the round's invert map. */
static odin3_net_id inverse_of(const odin3_expr_ctx *ec, odin3_net_id net) {
    odin3_net_id inverse = odin3_kit_inverse(ec->oc, net);
    return odin3_net_valid(inverse) && odin3_net_live(ec->module, inverse) ? inverse
                                                                           : (odin3_net_id){0};
}

odin3_status odin3_expr_rule_invert_double(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_NOT && ec->kind != ODIN3_EXPR_T_LOGIC_NOT &&
        ec->kind != ODIN3_EXPR_T_G_NOT) {
        return ODIN3_OK;
    }
    if (odin3_expr_width(ec, PORT_A) != 1 || odin3_expr_y_width(ec) != 1) {
        return ODIN3_OK;
    }
    odin3_net_id inverse = inverse_of(ec, odin3_expr_net(ec, PORT_A, 0));
    return odin3_net_valid(inverse) ? odin3_expr_wire_net(ec, ODIN3_OPT_RULE_INVERT_DOUBLE, inverse)
                                    : ODIN3_OK;
}

odin3_status odin3_expr_rule_invert_muxsel(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_MUX && ec->kind != ODIN3_EXPR_T_G_MUX) {
        return ODIN3_OK;
    }
    odin3_net_id inverse = inverse_of(ec, odin3_expr_net(ec, PORT_S, 0));
    if (!odin3_net_valid(inverse)) {
        return ODIN3_OK;
    }
    odin3_expr_sig lhs;
    odin3_expr_sig rhs;
    odin3_expr_sig sel;
    odin3_status st = odin3_expr_read(ec, PORT_A, &lhs);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &rhs) : st;
    st = st == ODIN3_OK ? odin3_expr_one(ec, inverse, &sel) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_expr_cell spec = {.type = ec->kind, .in = {&rhs, &lhs, &sel}};
    if (ec->kind == ODIN3_EXPR_T_MUX) {
        spec.n_params = 1;
        spec.params[ODIN3_EXPR_P_MUX_WIDTH] = odin3_expr_param(ec, ODIN3_EXPR_P_MUX_WIDTH);
    }
    return odin3_expr_rebuild(ec, ODIN3_OPT_RULE_INVERT_MUXSEL, &spec);
}

/* --- the bit-gate tables (opt_expr.cc 1100-1190) ---------------------------------------------- */

/* What a matching row does: Y = 0, 1 or x; Y = the input A, B or S; or a $_NOT_ of S. */
typedef enum gate_act { GA_Y0, GA_Y1, GA_YX, GA_A, GA_B, GA_S, GA_NOT_S } gate_act;

/* A row applies always, only in the consume_x round, or only with -mux_undef. */
typedef enum gate_cond { GC_ALWAYS, GC_CONSUME_X, GC_MUX_UNDEF } gate_cond;

/* pattern: one char per input in the order A, B[, S]: '0', '1', '*' (x or z), ' ' (anything). */
typedef struct gate_row {
    const char *pattern;
    gate_act act;
    gate_cond cond;
} gate_row;

static const gate_row k_not_rows[] = {
    {"1", GA_Y0, GC_ALWAYS}, {"0", GA_Y1, GC_ALWAYS}, {"*", GA_YX, GC_ALWAYS}};
static const gate_row k_and_rows[] = {{" 0", GA_Y0, GC_ALWAYS},    {"0 ", GA_Y0, GC_ALWAYS},
                                      {"11", GA_Y1, GC_ALWAYS},    {"**", GA_YX, GC_ALWAYS},
                                      {"1*", GA_YX, GC_ALWAYS},    {"*1", GA_YX, GC_ALWAYS},
                                      {" *", GA_Y0, GC_CONSUME_X}, {"* ", GA_Y0, GC_CONSUME_X},
                                      {" 1", GA_A, GC_ALWAYS},     {"1 ", GA_B, GC_ALWAYS}};
static const gate_row k_or_rows[] = {{" 1", GA_Y1, GC_ALWAYS},    {"1 ", GA_Y1, GC_ALWAYS},
                                     {"00", GA_Y0, GC_ALWAYS},    {"**", GA_YX, GC_ALWAYS},
                                     {"0*", GA_YX, GC_ALWAYS},    {"*0", GA_YX, GC_ALWAYS},
                                     {" *", GA_Y1, GC_CONSUME_X}, {"* ", GA_Y1, GC_CONSUME_X},
                                     {" 0", GA_A, GC_ALWAYS},     {"0 ", GA_B, GC_ALWAYS}};
static const gate_row k_xor_rows[] = {{"00", GA_Y0, GC_ALWAYS},    {"01", GA_Y1, GC_ALWAYS},
                                      {"10", GA_Y1, GC_ALWAYS},    {"11", GA_Y0, GC_ALWAYS},
                                      {" *", GA_Y0, GC_CONSUME_X}, {"* ", GA_Y0, GC_CONSUME_X}};
static const gate_row k_mux_rows[] = {
    {"  0", GA_A, GC_ALWAYS},     {"  1", GA_B, GC_ALWAYS},    {"01 ", GA_S, GC_ALWAYS},
    {"10 ", GA_NOT_S, GC_ALWAYS}, {"11 ", GA_Y1, GC_ALWAYS},   {"00 ", GA_Y0, GC_ALWAYS},
    {"** ", GA_YX, GC_ALWAYS},    {"01*", GA_YX, GC_ALWAYS},   {"10*", GA_YX, GC_ALWAYS},
    {"*  ", GA_B, GC_MUX_UNDEF},  {" * ", GA_A, GC_MUX_UNDEF}, {"  *", GA_A, GC_MUX_UNDEF}};

/* The class of a gate input: '0', '1', '*' (x or z) or 'w' (not a constant). */
static char input_class(const odin3_expr_ctx *ec, uint32_t index) {
    switch (odin3_expr_cval(ec, odin3_expr_net(ec, ODIN3_EXPR_PORT(index), 0))) {
    case ODIN3_CONST_0:
        return '0';
    case ODIN3_CONST_1:
        return '1';
    case ODIN3_CONST_X:
    case ODIN3_CONST_Z:
        return '*';
    case ODIN3_CONST_NONE:
    default:
        return 'w';
    }
}

static bool row_matches(const odin3_expr_ctx *ec, const gate_row *row, const char *inputs) {
    if ((row->cond == GC_CONSUME_X && !ec->consume_x) ||
        (row->cond == GC_MUX_UNDEF && !ec->mux_undef)) {
        return false;
    }
    for (uint32_t i = 0; row->pattern[i] != '\0'; i++) {
        if (row->pattern[i] != ' ' && row->pattern[i] != inputs[i]) {
            return false;
        }
    }
    return true;
}

static odin3_status gate_apply(odin3_expr_ctx *ec, const gate_row *row, odin3_opt_rule rule) {
    switch (row->act) {
    case GA_Y0:
        return odin3_expr_wire_bit(ec, rule, ODIN3_BIT_0);
    case GA_Y1:
        return odin3_expr_wire_bit(ec, rule, ODIN3_BIT_1);
    case GA_YX:
        return odin3_expr_wire_bit(ec, rule, ODIN3_BIT_X);
    case GA_A:
        return odin3_expr_wire_net(ec, rule, odin3_expr_net(ec, PORT_A, 0));
    case GA_B:
        return odin3_expr_wire_net(ec, rule, odin3_expr_net(ec, PORT_B, 0));
    case GA_S:
        return odin3_expr_wire_net(ec, rule, odin3_expr_net(ec, PORT_S, 0));
    case GA_NOT_S:
    default: {
        odin3_expr_sig sel;
        odin3_status st = odin3_expr_one(ec, odin3_expr_net(ec, PORT_S, 0), &sel);
        odin3_expr_cell spec = {.type = ODIN3_EXPR_T_G_NOT, .in = {&sel}};
        return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_MUX_TO_INV, &spec) : st;
    }
    }
}

/* The gates that have a table, their rows and the rule that counts them. */
typedef struct gate_table {
    odin3_expr_type kind;
    const gate_row *rows;
    size_t n_rows;
    odin3_opt_rule rule;
} gate_table;

#define ROWS(rows) rows, sizeof(rows) / sizeof((rows)[0])

static const gate_table k_gate_tables[] = {
    {ODIN3_EXPR_T_G_NOT, ROWS(k_not_rows), ODIN3_OPT_RULE_GATE_NOT},
    {ODIN3_EXPR_T_G_AND, ROWS(k_and_rows), ODIN3_OPT_RULE_GATE_AND},
    {ODIN3_EXPR_T_G_OR, ROWS(k_or_rows), ODIN3_OPT_RULE_GATE_OR},
    {ODIN3_EXPR_T_G_XOR, ROWS(k_xor_rows), ODIN3_OPT_RULE_GATE_XOR},
    {ODIN3_EXPR_T_G_MUX, ROWS(k_mux_rows), ODIN3_OPT_RULE_GATE_MUX}};

odin3_status odin3_expr_rule_gate(odin3_expr_ctx *ec) {
    const gate_table *table = NULL;
    for (size_t i = 0; i < sizeof k_gate_tables / sizeof k_gate_tables[0]; i++) {
        table = k_gate_tables[i].kind == ec->kind ? &k_gate_tables[i] : table;
    }
    if (table == NULL) {
        return ODIN3_OK;
    }
    char inputs[4] = {0};
    for (uint32_t index = 0; index + 1 < ec->def->n_ports; index++) {
        inputs[index] = input_class(ec, index);
    }
    if (ec->kind == ODIN3_EXPR_T_G_MUX &&
        odin3_expr_same(ec, odin3_expr_net(ec, PORT_A, 0), odin3_expr_net(ec, PORT_B, 0))) {
        return odin3_expr_wire_net(ec, table->rule, odin3_expr_net(ec, PORT_A, 0));
    }
    for (size_t i = 0; i < table->n_rows; i++) {
        if (row_matches(ec, &table->rows[i], inputs)) {
            return gate_apply(ec, &table->rows[i], table->rule);
        }
    }
    return ODIN3_OK;
}

/* --- $tribuf ---------------------------------------------------------------------------------- */

odin3_status odin3_expr_rule_tribuf(odin3_expr_ctx *ec) {
    odin3_const enable = odin3_expr_cval(ec, odin3_expr_net(ec, ODIN3_EXPR_PORT_EN, 0));
    if (ec->kind != ODIN3_EXPR_T_TRIBUF || (enable != ODIN3_CONST_1 && enable != ODIN3_CONST_0)) {
        return ODIN3_OK;
    }
    odin3_expr_sig data;
    odin3_status st = odin3_expr_read(ec, PORT_A, &data);
    if (st == ODIN3_OK && enable == ODIN3_CONST_1) {
        return odin3_expr_wire(ec, ODIN3_OPT_RULE_TRIBUF_EN1, &data);
    }
    if (st != ODIN3_OK || odin3_expr_fully_undef(odin3_expr_sig_census(ec, &data))) {
        return st;
    }
    odin3_expr_sig unknown;
    st = odin3_expr_fill(ec, ODIN3_BIT_X, data.n, &unknown);
    odin3_expr_cell spec = {.type = ODIN3_EXPR_T_TRIBUF, .n_params = 1, .in = {&unknown}};
    spec.params[ODIN3_EXPR_P_MUX_WIDTH] = odin3_expr_param(ec, ODIN3_EXPR_P_MUX_WIDTH);
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_TRIBUF_EN0, &spec) : st;
}
