/*
 * mult.c — `$mul` to `lpm_mult` (5.1 spec §6, M-1..M-5). One block per cell: `dataa` and `datab`
 * keep their widths, `lpm_widthp` is the full product A_WIDTH + B_WIDTH (the Altera block keeps
 * only the most significant bits when `lpm_widthp` is smaller than the product, UG-01063 p. 4-3),
 * and `result[k]` drives Y[k] for every Y bit. Both-signed cells are SIGNED, every other cell
 * UNSIGNED (a mixed cell is unsigned, PHASE2 #25). A cell with Y_WIDTH above the product stays soft
 * with a warning: its extra Y bits would need a merge after the delete.
 */
#include "passes/lpmmap/lm_kit.h"

#include "passes/opt/kit.h"

#include <inttypes.h>

enum { P_A_WIDTH = 2, P_B_WIDTH = 3, P_Y_WIDTH = 4 };
enum { PORT_A, PORT_B, PORT_Y };

typedef struct mul_shape {
    uint32_t a, b, y;
    bool is_signed;
} mul_shape;

static uint32_t param_u32(const lm_ctx *lc, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(lc->module, node, index);
    return val != NULL && val->i > 0 ? (uint32_t)val->i : 0;
}

static mul_shape shape_of(const lm_ctx *lc, odin3_node_id node) {
    mul_shape sh;
    sh.a = param_u32(lc, node, P_A_WIDTH);
    sh.b = param_u32(lc, node, P_B_WIDTH);
    sh.y = param_u32(lc, node, P_Y_WIDTH);
    sh.is_signed = odin3_opt_both_signed(lc->module, node);
    return sh;
}

/* The nets of the operands as they are on the source's pins. */
static void operand_nets(const lm_ctx *lc, odin3_pinslice pins, odin3_net_id *dst) {
    for (uint32_t i = 0; i < pins.count; i++) {
        dst[i] = odin3_pin_net(lc->module, (odin3_pin_id){pins.first.v + i});
    }
}

static odin3_status make_ties(const lm_ctx *lc, odin3_node_id src, const odin3_net_id *result,
                              uint32_t bits, odin3_vec *ties) {
    odin3_pinslice ypins = odin3_node_port(lc->module, src, PORT_Y);
    uint32_t n = bits < ypins.count ? bits : ypins.count;
    odin3_status st = odin3_vec_reserve(ties, n);
    for (uint32_t k = 0; st == ODIN3_OK && k < n; k++) {
        lm_tie *tie = odin3_vec_push(ties); /* reserved: never NULL */
        *tie = (lm_tie){odin3_pin_net(lc->module, (odin3_pin_id){ypins.first.v + k}), result[k]};
    }
    return st;
}

typedef struct mul_bus {
    odin3_vec va, vb, vr;
    odin3_net_id *a, *b, *r;
} mul_bus;

static odin3_status bus_make(mul_bus *bus, const mul_shape *sh) {
    odin3_vec_init(&bus->va, sizeof(odin3_net_id));
    odin3_vec_init(&bus->vb, sizeof(odin3_net_id));
    odin3_vec_init(&bus->vr, sizeof(odin3_net_id));
    odin3_status st = lm_net_array(&bus->va, sh->a, &bus->a);
    st = st == ODIN3_OK ? lm_net_array(&bus->vb, sh->b, &bus->b) : st;
    return st == ODIN3_OK ? lm_net_array(&bus->vr, sh->a + sh->b, &bus->r) : st;
}

static void bus_free(mul_bus *bus) {
    odin3_vec_free(&bus->va);
    odin3_vec_free(&bus->vb);
    odin3_vec_free(&bus->vr);
}

static odin3_status build_into(lm_ctx *lc, odin3_node_id src, const mul_shape *sh, mul_bus *bus,
                               odin3_vec *ties) {
    uint32_t product = sh->a + sh->b;
    odin3_status st = lm_begin_cell(lc, src);
    st = st == ODIN3_OK ? lm_fresh_nets(lc, product, bus->r) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    operand_nets(lc, odin3_node_port(lc->module, src, PORT_A), bus->a);
    operand_nets(lc, odin3_node_port(lc->module, src, PORT_B), bus->b);
    const lm_pset params[] = {{"lpm_widtha", (int64_t)sh->a, NULL},
                              {"lpm_widthb", (int64_t)sh->b, NULL},
                              {"lpm_widthp", (int64_t)product, NULL},
                              {"lpm_representation", 0, sh->is_signed ? "SIGNED" : "UNSIGNED"},
                              {"lpm_pipeline", 0, NULL}};
    const lm_pconn ports[] = {
        {"dataa", bus->a, sh->a}, {"datab", bus->b, sh->b}, {"result", bus->r, product}};
    odin3_node_id node = {0};
    st = lm_new_node(lc, &(lm_node_req){LM_MULT, params, 5, ports, 3, "mul"}, &node);
    return st == ODIN3_OK ? make_ties(lc, src, bus->r, product, ties) : st;
}

static odin3_status build(lm_ctx *lc, odin3_node_id src, odin3_vec *ties) {
    mul_shape sh = shape_of(lc, src);
    mul_bus bus;
    odin3_status st = bus_make(&bus, &sh);
    st = st == ODIN3_OK ? build_into(lc, src, &sh, &bus, ties) : st;
    bus_free(&bus);
    return st;
}

odin3_status lm_map_mult(lm_ctx *lc, odin3_node_id src) {
    mul_shape sh = shape_of(lc, src);
    if (sh.a == 0 || sh.b == 0 || sh.y == 0) {
        return ODIN3_OK; /* a degenerate cell (the type's verify refuses it): left alone */
    }
    odin3_celltype_id type = {0};
    odin3_status st = lm_need(lc, LM_MULT, src, &type);
    if (st != ODIN3_OK) {
        return st;
    }
    if (sh.y > sh.a + sh.b) {
        char label[ODIN3_LOG_BUF / 4];
        lm_label(lc, src, label, sizeof label);
        lm_diag(lc, ODIN3_LOG_WARN, src,
                "lpmmap: '$mul' '%s' has Y wider than A + B; left soft (run wreduce first)", label);
        lc->stats.soft++;
        return ODIN3_OK;
    }
    st = lm_transact(lc, src, build);
    if (st == ODIN3_OK) {
        lc->stats.mult++;
    }
    return st;
}
