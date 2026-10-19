/*
 * compare.c — `$lt $le $gt $ge $eq $ne` to `lpm_compare` (5.1 spec §7, C-1..C-4). One block per
 * cell with `lpm_width` = max(A_WIDTH, B_WIDTH): both operands are extended to it by H-2
 * (sign-extended only when both are signed, so a mixed-sign comparison is unsigned). The matching
 * output (`alb aleb agb ageb aeb aneb`) drives Y[0], the other five stay open, and Y bits above
 * the first are gnd. The output meanings are those of the Intel LPM_COMPARE description (spec
 * §3.3).
 */
#include "passes/lpmmap/lm_kit.h"

#include "ir/celltype.h"
#include "passes/opt/kit.h"

#include <string.h>

enum { P_A_WIDTH = 2, P_B_WIDTH = 3, P_Y_WIDTH = 4 };
enum { PORT_A, PORT_B, PORT_Y };

/* The cell type names and the lpm_compare output each one drives (C-1). */
static const struct {
    const char *type;
    const char *out;
} k_outputs[] = {{"$lt", "alb"},  {"$le", "aleb"}, {"$gt", "agb"},
                 {"$ge", "ageb"}, {"$eq", "aeb"},  {"$ne", "aneb"}};

static uint32_t param_u32(const lm_ctx *lc, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(lc->module, node, index);
    return val != NULL && val->i > 0 ? (uint32_t)val->i : 0;
}

/* The output name for node's type; NULL for a type that is not a comparison. */
static const char *output_of(const lm_ctx *lc, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(lc->design, odin3_node_type(lc->module, node));
    for (size_t i = 0; i < sizeof k_outputs / sizeof k_outputs[0]; i++) {
        if (strcmp(def->name, k_outputs[i].type) == 0) {
            return k_outputs[i].out;
        }
    }
    return NULL;
}

/* The ties: Y[0] <- the output net, Y[k] <- gnd for k >= 1 (C-1). */
static odin3_status make_ties(lm_ctx *lc, odin3_node_id src, odin3_net_id out, odin3_vec *ties) {
    odin3_pinslice ypins = odin3_node_port(lc->module, src, PORT_Y);
    odin3_net_id gnd = {0};
    odin3_status st = ypins.count > 1 ? lm_const(lc, LM_GND, &gnd) : ODIN3_OK;
    st = st == ODIN3_OK ? odin3_vec_reserve(ties, ypins.count) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < ypins.count; k++) {
        lm_tie *tie = odin3_vec_push(ties); /* reserved: never NULL */
        *tie = (lm_tie){odin3_pin_net(lc->module, (odin3_pin_id){ypins.first.v + k}),
                        k == 0 ? out : gnd};
    }
    return st;
}

typedef struct cmp_bus {
    odin3_vec va, vb;
    odin3_net_id *a, *b;
} cmp_bus;

static odin3_status build_into(lm_ctx *lc, odin3_node_id src, uint32_t width, cmp_bus *bus,
                               odin3_vec *ties) {
    bool sgn = odin3_opt_both_signed(lc->module, src);
    odin3_net_id out = {0};
    odin3_status st = lm_begin_cell(lc, src);
    st = st == ODIN3_OK
             ? lm_extend(lc, odin3_node_port(lc->module, src, PORT_A), width, sgn, bus->a)
             : st;
    st = st == ODIN3_OK
             ? lm_extend(lc, odin3_node_port(lc->module, src, PORT_B), width, sgn, bus->b)
             : st;
    st = st == ODIN3_OK ? lm_fresh_nets(lc, 1, &out) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    const lm_pset params[] = {{"lpm_width", (int64_t)width, NULL},
                              {"lpm_representation", 0, sgn ? "SIGNED" : "UNSIGNED"}};
    const lm_pconn ports[] = {
        {"dataa", bus->a, width}, {"datab", bus->b, width}, {output_of(lc, src), &out, 1}};
    odin3_node_id node = {0};
    st = lm_new_node(lc, &(lm_node_req){LM_COMPARE, params, 2, ports, 3, "cmp"}, &node);
    return st == ODIN3_OK ? make_ties(lc, src, out, ties) : st;
}

static odin3_status build(lm_ctx *lc, odin3_node_id src, odin3_vec *ties) {
    uint32_t a = param_u32(lc, src, P_A_WIDTH);
    uint32_t b = param_u32(lc, src, P_B_WIDTH);
    uint32_t width = a > b ? a : b;
    cmp_bus bus;
    odin3_vec_init(&bus.va, sizeof(odin3_net_id));
    odin3_vec_init(&bus.vb, sizeof(odin3_net_id));
    odin3_status st = lm_net_array(&bus.va, width, &bus.a);
    st = st == ODIN3_OK ? lm_net_array(&bus.vb, width, &bus.b) : st;
    st = st == ODIN3_OK ? build_into(lc, src, width, &bus, ties) : st;
    odin3_vec_free(&bus.va);
    odin3_vec_free(&bus.vb);
    return st;
}

odin3_status lm_map_compare(lm_ctx *lc, odin3_node_id src) {
    if (output_of(lc, src) == NULL || param_u32(lc, src, P_A_WIDTH) == 0 ||
        param_u32(lc, src, P_B_WIDTH) == 0 || param_u32(lc, src, P_Y_WIDTH) == 0) {
        return ODIN3_OK; /* not a comparison, or a degenerate cell: left alone */
    }
    odin3_celltype_id type = {0};
    odin3_status st = lm_need(lc, LM_COMPARE, src, &type);
    st = st == ODIN3_OK ? lm_transact(lc, src, build) : st;
    if (st == ODIN3_OK) {
        lc->stats.compare++;
    }
    return st;
}
