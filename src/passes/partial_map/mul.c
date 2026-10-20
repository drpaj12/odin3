/*
 * mul.c — `$mul` to `multiply` (3C spec §4, M-1..M-7). One `multiply` of WIDTH W (rules.mul_width) per cell
 * (Parmys: fixed_hard_multiplier, no fracturing); the operand bits the cell has go to the low pins,
 * every other pin is the pad (`$_CONSTX_`, written `unconn`); out[i] drives Y[i]; the unused
 * outputs are fresh nets without a reader. A signed cell (both flags, PHASE2 #25) is sign-extended
 * to Y_WIDTH (D3). Cells M-2 leaves soft get one warning each.
 */
#include "passes/partial_map/hm_kit.h"

#include "passes/opt/kit.h"
#include "util/log.h"

#include <inttypes.h>
#include <string.h>

enum { P_A_WIDTH = 2, P_B_WIDTH = 3, P_Y_WIDTH = 4 };
enum { PORT_A, PORT_B, PORT_Y };
enum { MUL_PINS = PM_MUL_WIDTH_CAP, MUL_OUT_PINS = 2 * PM_MUL_WIDTH_CAP }; /* array capacities */

/* The shape of a `$mul` cell. */
typedef struct mul_shape {
    uint32_t a, b, y;
    bool is_signed; /* both operands signed: the sign-extending path */
} mul_shape;

typedef enum mul_verdict {
    MUL_HARD,
    MUL_SOFT_2X2,
    MUL_SOFT_WIDE,
    MUL_SOFT_SIGNED_Y,
    MUL_SOFT_Y_WIDER
} mul_verdict;

/* The pins of the multiplier: a and b operands and the product, as netvecs. */
typedef struct mul_nets {
    odin3_net_id a[MUL_PINS];
    odin3_net_id b[MUL_PINS];
    odin3_net_id out[MUL_OUT_PINS];
} mul_nets;

static uint32_t param_u32(const hm_ctx *hc, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(hc->module, node, index);
    return val != NULL && val->i > 0 ? (uint32_t)val->i : 0;
}

static mul_shape shape_of(const hm_ctx *hc, odin3_node_id node) {
    mul_shape sh;
    sh.a = param_u32(hc, node, P_A_WIDTH);
    sh.b = param_u32(hc, node, P_B_WIDTH);
    sh.y = param_u32(hc, node, P_Y_WIDTH);
    sh.is_signed = odin3_opt_both_signed(hc->module, node);
    return sh;
}

/* M-2, M-3 and M-5: hard unless too small (both operands at least 2 and the wider below
 * rules.min_mul: Parmys builds the 2x2 from gates), wider than W, signed with Y_WIDTH above W, or
 * unsigned with Y_WIDTH above A_WIDTH + B_WIDTH (run wreduce first). Parmys's own test is
 * max(a,b) >= 3 || a == 1 || b == 1, so 1xN goes hard. */
static mul_verdict verdict_of(const hm_ctx *hc, const mul_shape *sh) {
    uint32_t widest = sh->a > sh->b ? sh->a : sh->b;
    if (sh->a >= 2 && sh->b >= 2 && widest < hc->rules.min_mul) {
        return MUL_SOFT_2X2;
    }
    if (widest > hc->rules.mul_width) {
        return MUL_SOFT_WIDE;
    }
    if (sh->is_signed && sh->y > hc->rules.mul_width) {
        return MUL_SOFT_SIGNED_Y;
    }
    if (!sh->is_signed && sh->y > sh->a + sh->b) {
        return MUL_SOFT_Y_WIDER; /* the gnd merges would allocate after the delete (H-9) */
    }
    return MUL_HARD;
}

static void warn_soft(const hm_ctx *hc, odin3_node_id src, const mul_shape *sh,
                      mul_verdict verdict) {
    char label[ODIN3_LOG_BUF / 4];
    hm_label(hc, src, label, sizeof label);
    if (verdict == MUL_SOFT_2X2) {
        hm_diag(hc, ODIN3_LOG_WARN, src,
                "partial_map: '$mul' cell '%s' is 2x2; left soft for lower (Parmys builds it from "
                "gates and adders)",
                label);
    } else if (verdict == MUL_SOFT_Y_WIDER) {
        hm_diag(hc, ODIN3_LOG_WARN, src,
                "partial_map: '$mul' cell '%s' has Y_WIDTH %" PRIu32
                " above A_WIDTH + B_WIDTH (%" PRIu32 "); left soft for lower (run wreduce first)",
                label, sh->y, sh->a + sh->b);
    } else if (verdict == MUL_SOFT_WIDE) {
        hm_diag(hc, ODIN3_LOG_WARN, src,
                "partial_map: '$mul' cell '%s' is %" PRIu32 "x%" PRIu32
                "; the arch's multiplier is %" PRIu32 " wide; left soft for lower",
                label, sh->a, sh->b, hc->rules.mul_width);
    } else {
        hm_diag(hc, ODIN3_LOG_WARN, src,
                "partial_map: signed '$mul' cell '%s' has Y_WIDTH %" PRIu32
                " above %" PRIu32 "; left soft for lower",
                label, sh->y, hc->rules.mul_width);
    }
}

/*
 * The W pins of one operand. Unsigned: the operand's bits, then the pad. Signed (M-5): the bits
 * below Y_WIDTH, the sign bit up to Y_WIDTH, then the pad.
 */
static void fill_operand(const hm_ctx *hc, odin3_pinslice pins, const mul_shape *sh,
                         odin3_net_id *dst) {
    uint32_t limit = sh->is_signed ? sh->y : hc->rules.mul_width;
    for (uint32_t i = 0; i < hc->rules.mul_width; i++) {
        if (i < pins.count && i < limit) {
            dst[i] = odin3_pin_net(hc->module, (odin3_pin_id){pins.first.v + i});
        } else if (sh->is_signed && i < limit && pins.count > 0) {
            dst[i] = odin3_pin_net(hc->module, (odin3_pin_id){pins.first.v + pins.count - 1});
        } else {
            dst[i] = (odin3_net_id){0};
        }
    }
}

/* Replaces the none entries that stand for the pad by the pad net: an operand bit that is really
 * unconnected stays open (it is a source bit), so the pad marks come from the fill's limit. */
static void fill_pad(const hm_ctx *hc, const mul_shape *sh, uint32_t width, odin3_net_id pad,
                     odin3_net_id *dst) {
    uint32_t limit = sh->is_signed ? sh->y : hc->rules.mul_width;
    uint32_t used = width < limit ? width : limit;
    used = sh->is_signed ? limit : used;
    for (uint32_t i = used; i < hc->rules.mul_width; i++) {
        dst[i] = pad;
    }
}

static odin3_status operands(hm_ctx *hc, odin3_node_id src, const mul_shape *sh, mul_nets *nets) {
    odin3_net_id pad = {0};
    odin3_status st = hm_const(hc, HM_PAD, &pad);
    if (st != ODIN3_OK) {
        return st;
    }
    fill_operand(hc, odin3_node_port(hc->module, src, PORT_A), sh, nets->a);
    fill_operand(hc, odin3_node_port(hc->module, src, PORT_B), sh, nets->b);
    fill_pad(hc, sh, sh->a, pad, nets->a);
    fill_pad(hc, sh, sh->b, pad, nets->b);
    return ODIN3_OK;
}

/* The ties of the commit: Y[k] <- out[k] for every Y bit (M-3: the verdict leaves a cell with
 * Y_WIDTH above the product soft, so out[] always reaches Y and no merge allocates, H-9). */
static odin3_status make_ties(const hm_ctx *hc, odin3_node_id src, const mul_shape *sh,
                              const mul_nets *nets, odin3_vec *ties) {
    odin3_pinslice ypins = odin3_node_port(hc->module, src, PORT_Y);
    uint32_t bits = sh->y < ypins.count ? sh->y : ypins.count;
    odin3_status st = odin3_vec_reserve(ties, bits);
    for (uint32_t k = 0; st == ODIN3_OK && k < bits; k++) {
        odin3_net_id ynet = odin3_pin_net(hc->module, (odin3_pin_id){ypins.first.v + k});
        hm_tie *tie = odin3_vec_push(ties); /* reserved: never NULL */
        *tie = (hm_tie){ynet, nets->out[k]};
    }
    return st;
}

/* Builds the multiplier on fresh nets and the ties; nothing of the source changes. */
static odin3_status build(hm_ctx *hc, odin3_node_id src, const mul_shape *sh, odin3_vec *ties) {
    odin3_celltype_id type = {0};
    mul_nets nets;
    memset(&nets, 0, sizeof nets);
    odin3_status st = hm_need(hc, HM_MULTIPLY, src, &type);
    st = st == ODIN3_OK ? hm_begin_cell(hc, src) : st;
    st = st == ODIN3_OK ? operands(hc, src, sh, &nets) : st;
    st = st == ODIN3_OK ? hm_fresh_nets(hc, 2U * hc->rules.mul_width, nets.out) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_value width = odin3_value_int(hc->rules.mul_width);
    odin3_netvec ports[3] = {
        {nets.a, hc->rules.mul_width}, {nets.b, hc->rules.mul_width}, {nets.out, 2U * hc->rules.mul_width}};
    odin3_node_id node = {0};
    st = hm_new_node(hc, &(hm_node_req){type, &width, 1, ports, "mul"}, &node);
    return st == ODIN3_OK ? make_ties(hc, src, sh, &nets, ties) : st;
}

/* One transaction for src: build(...) on fresh nets, then commit; the undo on failure. */
typedef odin3_status (*build_fn)(hm_ctx *hc, odin3_node_id src, const mul_shape *sh,
                                 odin3_vec *ties);

static odin3_status transact(hm_ctx *hc, odin3_node_id src, const mul_shape *sh, build_fn builder) {
    odin3_vec ties;
    odin3_vec_init(&ties, sizeof(hm_tie));
    odin3_status st = builder(hc, src, sh, &ties);
    if (st != ODIN3_OK) {
        hm_abort(hc);
    } else {
        st = hm_commit(hc, odin3_vec_cat(&ties, 0), ties.len);
    }
    odin3_vec_free(&ties);
    return st;
}

odin3_status hm_map_mul(hm_ctx *hc, odin3_node_id src) {
    mul_shape sh = shape_of(hc, src);
    if (sh.a == 0 || sh.b == 0 || sh.y == 0) {
        return ODIN3_OK; /* a degenerate cell (the type's verify refuses it): left alone */
    }
    mul_verdict verdict = verdict_of(hc, &sh);
    if (verdict != MUL_HARD) {
        warn_soft(hc, src, &sh, verdict);
        hc->stats.soft++;
        return ODIN3_OK;
    }
    odin3_status st = transact(hc, src, &sh, build);
    if (st == ODIN3_OK) {
        hc->stats.multiply++;
    }
    return st;
}

/* --- explicit `multiply` instances (M-6, amended 2026-10-17) ------------------------------- */

enum { MUL_PARAMS = 5 };

static odin3_status check_width(const hm_ctx *hc, odin3_node_id node) {
    const odin3_value *width = odin3_node_param(hc->module, node, 0);
    if (width == NULL || width->i <= (int64_t)hc->rules.mul_width) {
        return ODIN3_OK;
    }
    char label[ODIN3_LOG_BUF / 4];
    hm_label(hc, node, label, sizeof label);
    hm_diag(hc, ODIN3_LOG_ERROR, node,
            "partial_map: 'multiply' instance '%s' has WIDTH %" PRId64
            "; the arch's multiplier is %" PRIu32 " wide",
            label, width->i, hc->rules.mul_width);
    return ODIN3_ERR_INVALID_ARG;
}

odin3_status hm_check_multiply(hm_ctx *hc, odin3_node_id node) {
    odin3_celltype_id type = {0};
    odin3_status st = hm_need(hc, HM_MULTIPLY, node, &type);
    return st == ODIN3_OK ? check_width(hc, node) : st;
}

/* The shape of the `$mul` an explicit `multiply` stands for: unsigned, the pin counts of a, b, out.
 */
static mul_shape shape_of_multiply(const hm_ctx *hc, odin3_node_id node) {
    mul_shape sh;
    sh.a = odin3_node_port(hc->module, node, PORT_A).count;
    sh.b = odin3_node_port(hc->module, node, PORT_B).count;
    sh.y = odin3_node_port(hc->module, node, PORT_Y).count;
    sh.is_signed = false;
    return sh;
}

/* The soft case: the same operands on a `$mul` cell (Y on fresh nets, tied at the commit). */
static odin3_status build_soft(hm_ctx *hc, odin3_node_id src, const mul_shape *sh,
                               odin3_vec *ties) {
    odin3_celltype_id type = {0};
    mul_nets nets;
    memset(&nets, 0, sizeof nets);
    if (!hm_find_type(hc->design, "$mul", &type)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = hm_begin_cell(hc, src);
    st = st == ODIN3_OK ? hm_fresh_nets(hc, sh->y, nets.out) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t i = 0; i < sh->a; i++) {
        nets.a[i] = odin3_pin_net(
            hc->module, (odin3_pin_id){odin3_node_port(hc->module, src, PORT_A).first.v + i});
    }
    for (uint32_t i = 0; i < sh->b; i++) {
        nets.b[i] = odin3_pin_net(
            hc->module, (odin3_pin_id){odin3_node_port(hc->module, src, PORT_B).first.v + i});
    }
    odin3_value params[MUL_PARAMS] = {odin3_value_int(0), odin3_value_int(0),
                                      odin3_value_int(sh->a), odin3_value_int(sh->b),
                                      odin3_value_int(sh->y)};
    odin3_netvec ports[3] = {{nets.a, sh->a}, {nets.b, sh->b}, {nets.out, sh->y}};
    odin3_node_id node = {0};
    st = hm_new_node(hc, &(hm_node_req){type, params, MUL_PARAMS, ports, "mul"}, &node);
    return st == ODIN3_OK ? make_ties(hc, src, sh, &nets, ties) : st;
}

/*
 * An explicit `multiply` is treated like the equivalent `$mul` (Parmys resolves the primitive to
 * `$mul`): WIDTH 2 becomes a `$mul` for lower, other WIDTH below W are padded to the arch's
 * multiplier exactly as a `$mul` of that shape, WIDTH W is already that. A warning names it.
 */
odin3_status hm_map_explicit(hm_ctx *hc, odin3_node_id src) {
    const odin3_value *width = odin3_node_param(hc->module, src, 0);
    mul_shape sh = shape_of_multiply(hc, src);
    if (width == NULL || width->i == (int64_t)hc->rules.mul_width || sh.a == 0 || sh.b == 0 ||
        sh.y == 0) {
        return ODIN3_OK;
    }
    mul_verdict verdict = verdict_of(hc, &sh);
    if (verdict != MUL_HARD && verdict != MUL_SOFT_2X2) {
        return ODIN3_OK;
    }
    char label[ODIN3_LOG_BUF / 4];
    hm_label(hc, src, label, sizeof label);
    hm_diag(hc, ODIN3_LOG_WARN, src,
            "partial_map: explicit 'multiply' instance '%s' (WIDTH %" PRId64
            ") treated as '$mul' as Parmys does",
            label, width->i);
    odin3_status st = transact(hc, src, &sh, verdict == MUL_HARD ? build : build_soft);
    if (st == ODIN3_OK) {
        hc->stats.multiply += verdict == MUL_HARD ? 1U : 0U;
        hc->stats.soft += verdict == MUL_HARD ? 0U : 1U;
    }
    return st;
}
