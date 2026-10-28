/*
 * expr_cmp.c — opt_expr's rules on x bits, comparisons and shifts (2F spec §4.3; Yosys 0.55
 * opt_expr.cc: xbit, trim, eqneq.*, constshift).
 */
#include "passes/opt/expr.h"

#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/opt.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORT_A ODIN3_EXPR_PORT_A
#define PORT_B ODIN3_EXPR_PORT_B

enum { INT_BITS = 32, BINARY_PORTS = 3 };

/* --- xbit ------------------------------------------------------------------------------------ */

static bool is_shift(odin3_expr_type kind) {
    return kind == ODIN3_EXPR_T_SHL || kind == ODIN3_EXPR_T_SHR || kind == ODIN3_EXPR_T_SSHR ||
           kind == ODIN3_EXPR_T_SHIFTX;
}

/* The cells whose x input makes the 1-bit result x (the others: every Y bit). */
static bool one_bit_result(odin3_expr_type kind) {
    return kind == ODIN3_EXPR_T_REDUCE_XOR || kind == ODIN3_EXPR_T_REDUCE_XNOR ||
           kind == ODIN3_EXPR_T_LT || kind == ODIN3_EXPR_T_LE || kind == ODIN3_EXPR_T_GE ||
           kind == ODIN3_EXPR_T_GT;
}

static bool xbit_applies(odin3_expr_type kind) {
    switch (kind) {
    case ODIN3_EXPR_T_NEG:
    case ODIN3_EXPR_T_ADD:
    case ODIN3_EXPR_T_SUB:
    case ODIN3_EXPR_T_MUL:
    case ODIN3_EXPR_T_DIV:
    case ODIN3_EXPR_T_MOD:
    case ODIN3_EXPR_T_POW:
        return true;
    default:
        return one_bit_result(kind) || is_shift(kind);
    }
}

/* An exactly-x input bit (Yosys tests == State::Sx; z does not count). */
odin3_status odin3_expr_rule_xbit(odin3_expr_ctx *ec) {
    if (!xbit_applies(ec->kind)) {
        return ODIN3_OK;
    }
    bool has_x = false;
    if (!is_shift(ec->kind)) { /* a shift's A may hold x: only B counts */
        has_x = odin3_expr_port_census(ec, PORT_A).xs > 0;
    }
    if (!has_x && ec->def->n_ports >= BINARY_PORTS) {
        has_x = odin3_expr_port_census(ec, PORT_B).xs > 0;
    }
    if (!has_x) {
        return ODIN3_OK;
    }
    if (one_bit_result(ec->kind)) {
        return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_XBIT, ODIN3_BIT_X);
    }
    odin3_expr_sig unknown;
    odin3_status st = odin3_expr_fill(ec, ODIN3_BIT_X, odin3_expr_y_width(ec), &unknown);
    return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_XBIT, &unknown) : st;
}

/* --- trim ------------------------------------------------------------------------------------- */

/* $shiftx: x bits above A's bit 0 reach the same x as the fill past the top, so they go. */
odin3_status odin3_expr_rule_trim(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_SHIFTX) {
        return ODIN3_OK;
    }
    uint32_t full = odin3_expr_width(ec, PORT_A);
    uint32_t width = full;
    while (width > 1 &&
           odin3_expr_cval(ec, odin3_expr_net(ec, PORT_A, width - 1)) == ODIN3_CONST_X) {
        width--;
    }
    if (width == full) {
        return ODIN3_OK;
    }
    odin3_expr_sig sig;
    odin3_status st = odin3_expr_read(ec, PORT_A, &sig);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_expr_sig narrow = odin3_expr_slice(&sig, 0, width);
    odin3_expr_cell spec = {
        .type = ODIN3_EXPR_T_SHIFTX, .n_params = ODIN3_EXPR_P_BIN_YW + 1, .in = {&narrow}};
    spec.params[ODIN3_EXPR_P_BIN_ASIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED);
    spec.params[ODIN3_EXPR_P_BIN_BSIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED);
    spec.params[ODIN3_EXPR_P_BIN_AW] = width;
    spec.params[ODIN3_EXPR_P_BIN_BW] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BW);
    spec.params[ODIN3_EXPR_P_BIN_YW] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_YW);
    return odin3_expr_rebuild(ec, ODIN3_OPT_RULE_TRIM, &spec);
}

/* --- eqneq.isneq, .empty, .resize ------------------------------------------------------------- */

static bool is_eq(const odin3_expr_ctx *ec) {
    return ec->kind == ODIN3_EXPR_T_EQ;
}

/* The value of an operand bit; the invalid net is the zero pad of a narrower unsigned operand,
 * which is made only when the rule rewrites (eq_pad_fix): a constant made by a rule that ends up
 * rewriting nothing would still use up a net and a node ID (IR-6), and a Debug build revisits
 * cells a Release build does not, so the IDs would depend on the build. */
static odin3_const eq_val(const odin3_expr_ctx *ec, odin3_net_id net) {
    return odin3_net_valid(net) ? odin3_expr_cval(ec, net) : ODIN3_CONST_0;
}

/* Two constant bits that are 0 and 1 (Yosys: both non-wire, different, both <= S1). */
static bool bits_differ(const odin3_expr_ctx *ec, odin3_net_id lhs, odin3_net_id rhs) {
    odin3_const lc = eq_val(ec, lhs);
    odin3_const rc = eq_val(ec, rhs);
    return (lc == ODIN3_CONST_0 && rc == ODIN3_CONST_1) ||
           (lc == ODIN3_CONST_1 && rc == ODIN3_CONST_0);
}

/* Yosys's same-bit test: the same net, or the same constant. */
static bool bits_same(const odin3_expr_ctx *ec, odin3_net_id lhs, odin3_net_id rhs) {
    if (lhs.v == rhs.v && odin3_net_valid(lhs)) {
        return true;
    }
    odin3_const value = eq_val(ec, lhs);
    return value != ODIN3_CONST_NONE && value == eq_val(ec, rhs);
}

/* in extended to width: by its sign bit when is_signed, else by the zero pad (the invalid net). */
static odin3_status eq_extend(odin3_expr_ctx *ec, const odin3_expr_sig *in, uint32_t width,
                              bool is_signed, odin3_expr_sig *out) {
    odin3_net_id pad = {0};
    if (is_signed && in->n > 0) {
        pad = in->net[in->n - 1];
    }
    odin3_status st = odin3_expr_sig_new(ec, width, out);
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        out->net[k] = k < in->n ? in->net[k] : pad;
    }
    return st;
}

/* A and B extended to a common width (signed when both are), as Yosys does when the widths differ.
 */
static odin3_status eq_operands(odin3_expr_ctx *ec, odin3_expr_sig *lhs, odin3_expr_sig *rhs) {
    odin3_expr_sig raw_a;
    odin3_expr_sig raw_b;
    odin3_status st = odin3_expr_read(ec, PORT_A, &raw_a);
    st = st == ODIN3_OK ? odin3_expr_read(ec, PORT_B, &raw_b) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t width = raw_a.n > raw_b.n ? raw_a.n : raw_b.n;
    bool both_signed = odin3_expr_both_signed(ec);
    st = eq_extend(ec, &raw_a, width, both_signed, lhs);
    return st == ODIN3_OK ? eq_extend(ec, &raw_b, width, both_signed, rhs) : st;
}

/* The operands, and the bit pairs left once the equal ones are dropped. */
typedef struct eq_scan {
    odin3_expr_sig lhs, rhs, keep_a, keep_b;
} eq_scan;

/* Reads the operands and makes room for the pairs that stay. */
static odin3_status eq_load(odin3_expr_ctx *ec, eq_scan *scan) {
    odin3_status st = eq_operands(ec, &scan->lhs, &scan->rhs);
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, scan->lhs.n, &scan->keep_a) : st;
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, scan->lhs.n, &scan->keep_b) : st;
    scan->keep_a.n = scan->keep_b.n = 0;
    return st;
}

/* Fills the kept pairs; true when a pair can never be equal (a 0 against a 1). */
static bool eq_scan_pairs(const odin3_expr_ctx *ec, eq_scan *scan) {
    for (uint32_t bit = 0; bit < scan->lhs.n; bit++) {
        if (bits_differ(ec, scan->lhs.net[bit], scan->rhs.net[bit])) {
            return true;
        }
        if (!bits_same(ec, scan->lhs.net[bit], scan->rhs.net[bit])) {
            scan->keep_a.net[scan->keep_a.n++] = scan->lhs.net[bit];
            scan->keep_b.net[scan->keep_b.n++] = scan->rhs.net[bit];
        }
    }
    return false;
}

/* The zero pad (the invalid net) among the kept pairs becomes the canonical 0 net, made now that
 * the rule rewrites. */
static odin3_status eq_pad_fix(odin3_expr_ctx *ec, eq_scan *scan) {
    odin3_expr_sig *sides[2] = {&scan->keep_a, &scan->keep_b};
    odin3_net_id zero = {0};
    odin3_status st = ODIN3_OK;
    for (uint32_t side = 0; side < 2; side++) {
        for (uint32_t k = 0; st == ODIN3_OK && k < sides[side]->n; k++) {
            if (!odin3_net_valid(sides[side]->net[k])) {
                st = odin3_net_valid(zero) ? ODIN3_OK
                                           : odin3_opt_const_net(ec->oc, ODIN3_BIT_0, &zero);
                sides[side]->net[k] = zero;
            }
        }
    }
    return st;
}

/* The cell on the pairs that stay (Yosys goes on with it in the same visit). */
static odin3_status eq_resize(odin3_expr_ctx *ec, eq_scan *scan) {
    odin3_status fixed = eq_pad_fix(ec, scan);
    if (fixed != ODIN3_OK) {
        return fixed;
    }
    odin3_expr_cell spec = {.type = ec->kind,
                            .n_params = ODIN3_EXPR_P_BIN_YW + 1,
                            .in = {&scan->keep_a, &scan->keep_b}};
    spec.params[ODIN3_EXPR_P_BIN_ASIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED);
    spec.params[ODIN3_EXPR_P_BIN_BSIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED);
    spec.params[ODIN3_EXPR_P_BIN_AW] = scan->keep_a.n;
    spec.params[ODIN3_EXPR_P_BIN_BW] = scan->keep_b.n;
    spec.params[ODIN3_EXPR_P_BIN_YW] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_YW);
    odin3_status st = odin3_expr_rebuild(ec, ODIN3_OPT_RULE_EQNEQ_RESIZE, &spec);
    ec->resume = st == ODIN3_OK;
    return st;
}

odin3_status odin3_expr_rule_eqneq(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_EQ && ec->kind != ODIN3_EXPR_T_NE) {
        return ODIN3_OK;
    }
    eq_scan scan;
    odin3_status st = eq_load(ec, &scan);
    if (st != ODIN3_OK) {
        return st;
    }
    if (eq_scan_pairs(ec, &scan)) {
        return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_EQNEQ_ISNEQ,
                                   is_eq(ec) ? ODIN3_BIT_0 : ODIN3_BIT_1);
    }
    if (scan.keep_a.n == 0) {
        return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_EQNEQ_EMPTY,
                                   is_eq(ec) ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    return scan.keep_a.n < scan.lhs.n ? eq_resize(ec, &scan) : ODIN3_OK;
}

/* --- eqneq.swapconst and the 1-bit comparisons against a constant ----------------------------- */

odin3_status odin3_expr_rule_eq1bit(odin3_expr_ctx *ec) {
    if ((ec->kind != ODIN3_EXPR_T_EQ && ec->kind != ODIN3_EXPR_T_NE) ||
        odin3_expr_y_width(ec) != 1 || odin3_expr_width(ec, PORT_A) != 1 ||
        odin3_expr_width(ec, PORT_B) != 1) {
        return ODIN3_OK;
    }
    odin3_net_id var = odin3_expr_net(ec, PORT_A, 0);
    odin3_net_id cst = odin3_expr_net(ec, PORT_B, 0);
    if (odin3_expr_cval(ec, var) != ODIN3_CONST_NONE &&
        odin3_expr_cval(ec, cst) == ODIN3_CONST_NONE) {
        odin3_net_id tmp = var;
        var = cst;
        cst = tmp;
        odin3_expr_count(ec, ODIN3_OPT_RULE_EQNEQ_SWAPCONST);
    }
    odin3_const value = odin3_expr_cval(ec, cst);
    if (value == ODIN3_CONST_NONE) {
        return ODIN3_OK;
    }
    if (value == ODIN3_CONST_X || value == ODIN3_CONST_Z) {
        return odin3_expr_wire_bit(ec, ODIN3_OPT_RULE_EQNEQ_UNDEF, ODIN3_BIT_X);
    }
    if ((value == ODIN3_CONST_1) == is_eq(ec)) {
        return odin3_expr_wire_net(ec, ODIN3_OPT_RULE_EQNEQ_BUFFER, var);
    }
    odin3_expr_sig in;
    odin3_status st = odin3_expr_one(ec, var, &in);
    odin3_expr_cell spec = {.type = ODIN3_EXPR_T_NOT, .n_params = 3, .in = {&in}};
    spec.params[ODIN3_EXPR_P_UN_SIGNED] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED);
    spec.params[ODIN3_EXPR_P_UN_AW] = 1;
    spec.params[ODIN3_EXPR_P_UN_YW] = 1;
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_EQNEQ_ISNOT, &spec) : st;
}

/* --- eqneq.cmpzero ---------------------------------------------------------------------------- */

odin3_status odin3_expr_rule_cmpzero(odin3_expr_ctx *ec) {
    if (ec->kind != ODIN3_EXPR_T_EQ && ec->kind != ODIN3_EXPR_T_NE) {
        return ODIN3_OK;
    }
    bool a_zero = odin3_expr_fully_zero(odin3_expr_port_census(ec, PORT_A));
    bool b_zero = odin3_expr_fully_zero(odin3_expr_port_census(ec, PORT_B));
    if (!a_zero && !b_zero) {
        return ODIN3_OK;
    }
    odin3_expr_port port = a_zero ? PORT_B : PORT_A; /* the other side */
    odin3_expr_sig other;
    odin3_status st = odin3_expr_read(ec, port, &other);
    odin3_expr_cell spec = {.type = is_eq(ec) ? ODIN3_EXPR_T_LOGIC_NOT : ODIN3_EXPR_T_REDUCE_BOOL,
                            .n_params = 3,
                            .in = {&other}};
    spec.params[ODIN3_EXPR_P_UN_SIGNED] =
        odin3_expr_param(ec, a_zero ? ODIN3_EXPR_P_BIN_BSIGNED : ODIN3_EXPR_P_BIN_ASIGNED);
    spec.params[ODIN3_EXPR_P_UN_AW] = other.n;
    spec.params[ODIN3_EXPR_P_UN_YW] = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_YW);
    return st == ODIN3_OK ? odin3_expr_rebuild(ec, ODIN3_OPT_RULE_EQNEQ_CMPZERO, &spec) : st;
}

/* --- constshift ------------------------------------------------------------------------------- */

static bool bit_set(const odin3_expr_ctx *ec, uint32_t bit) {
    return odin3_expr_cval(ec, odin3_expr_net(ec, PORT_B, bit)) == ODIN3_CONST_1;
}

/* Yosys Const::get_min_size of B (leading bits are zeros, or the sign when signed; x counts as 0).
 */
static uint32_t b_min_size(const odin3_expr_ctx *ec, bool is_signed) {
    uint32_t width = odin3_expr_width(ec, PORT_B);
    odin3_const top = odin3_expr_cval(ec, odin3_expr_net(ec, PORT_B, width - 1));
    odin3_const leading = is_signed && top != ODIN3_CONST_X ? top : ODIN3_CONST_0;
    uint32_t idx = width;
    while (idx > 0 && odin3_expr_cval(ec, odin3_expr_net(ec, PORT_B, idx - 1)) == leading) {
        idx--;
    }
    if (is_signed && idx < width) {
        idx++;
    }
    return idx == 0 ? 1 : idx;
}

/* Yosys SigSpec::as_int_saturating of the constant B: its value as an int, INT_MAX / INT_MIN when
 * it does not fit; x and z bits read as 0. */
static int64_t b_saturating(const odin3_expr_ctx *ec, bool is_signed) {
    uint32_t width = odin3_expr_width(ec, PORT_B);
    uint32_t size = b_min_size(ec, is_signed);
    bool convertible =
        size <= INT_BITS - 1 || (size == INT_BITS && (is_signed || !bit_set(ec, INT_BITS - 1)));
    if (!convertible) {
        bool negative =
            is_signed && odin3_expr_cval(ec, odin3_expr_net(ec, PORT_B, size - 1)) != ODIN3_CONST_0;
        return negative ? INT_MIN : INT_MAX;
    }
    uint32_t bits = 0;
    for (uint32_t k = 0; k < width && k < INT_BITS; k++) {
        bits |= bit_set(ec, k) ? UINT32_C(1) << k : 0U;
    }
    if (is_signed && bit_set(ec, width - 1)) {
        for (uint32_t k = width; k < INT_BITS; k++) {
            bits |= UINT32_C(1) << k;
        }
    }
    return (int32_t)bits;
}

/* What a shift reads: A as the operand wide enough for Y, the fill, the shift, the sign rule. */
typedef struct shift_job {
    odin3_expr_sig data;
    odin3_net_id fill;
    int64_t shift;
    bool sign;
} shift_job;

/* Y bit `bit`: A[bit + shift], else the fill (0, or x for $shiftx; the sign bit for $sshr). */
static odin3_net_id shift_pick(const shift_job *job, uint32_t bit) {
    int64_t idx = (int64_t)bit + job->shift;
    int64_t len = (int64_t)job->data.n;
    if (idx >= 0 && idx < len) {
        return job->data.net[idx];
    }
    return idx >= len && job->sign ? job->data.net[len - 1] : job->fill;
}

static odin3_status shifted(odin3_expr_ctx *ec, int64_t shift, odin3_expr_sig *out) {
    uint32_t y_width = odin3_expr_y_width(ec);
    bool is_x = ec->kind == ODIN3_EXPR_T_SHIFTX;
    bool a_signed = odin3_expr_param(ec, ODIN3_EXPR_P_BIN_ASIGNED) != 0;
    odin3_expr_sig raw;
    odin3_status st = odin3_expr_read(ec, PORT_A, &raw);
    shift_job job = {.sign = ec->kind == ODIN3_EXPR_T_SSHR && a_signed};
    uint32_t a_len = raw.n < y_width && !is_x ? y_width : raw.n;
    st = st == ODIN3_OK ? odin3_expr_extend(ec, &raw, a_len, a_signed, &job.data) : st;
    st = st == ODIN3_OK ? odin3_opt_const_net(ec->oc, is_x ? ODIN3_BIT_X : ODIN3_BIT_0, &job.fill)
                        : st;
    st = st == ODIN3_OK ? odin3_expr_sig_new(ec, y_width, out) : st;
    job.shift = shift > (int64_t)a_len ? (int64_t)a_len : shift; /* saturated at A's width */
    for (uint32_t bit = 0; st == ODIN3_OK && bit < y_width; bit++) {
        out->net[bit] = shift_pick(&job, bit);
    }
    return st;
}

odin3_status odin3_expr_rule_constshift(odin3_expr_ctx *ec) {
    if (!is_shift(ec->kind)) {
        return ODIN3_OK;
    }
    odin3_expr_census census = odin3_expr_port_census(ec, PORT_B);
    if (ec->keepdc ? !odin3_expr_fully_def(census) : !odin3_expr_fully_const(census, false)) {
        return ODIN3_OK;
    }
    bool b_signed =
        ec->kind == ODIN3_EXPR_T_SHIFTX && odin3_expr_param(ec, ODIN3_EXPR_P_BIN_BSIGNED) != 0;
    int64_t shift = b_saturating(ec, b_signed);
    shift = ec->kind == ODIN3_EXPR_T_SHL ? -shift : shift;
    odin3_expr_sig result;
    odin3_status st = shifted(ec, shift, &result);
    return st == ODIN3_OK ? odin3_expr_wire(ec, ODIN3_OPT_RULE_CONSTSHIFT, &result) : st;
}
