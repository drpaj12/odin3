/*
 * addsub.c — `$add` / `$sub` to `lpm_add_sub` (5.1 spec §5, A-1..A-5). One block per cell with
 * `lpm_width` = Y_WIDTH: both operands are resized to Y_WIDTH by H-2 (sign-extended only when the
 * cell is both-signed), `result` drives Y, every other port stays open. Add and subtract are
 * modular, so the low Y_WIDTH bits are right whatever the operand widths and no carry-out is
 * needed. `lpm_direction` is ADD or SUB (a subtract relies on the Altera default of an omitted
 * `cin`, 1 for subtraction: UG-01063 p. 5-3), `lpm_representation` records the cell's signedness.
 */
#include "passes/lpmmap/lm_kit.h"

#include "ir/celltype.h"
#include "passes/opt/kit.h"

#include <string.h>

enum { P_A_WIDTH = 2, P_B_WIDTH = 3, P_Y_WIDTH = 4 };
enum { PORT_A, PORT_B, PORT_Y };

static uint32_t param_u32(const lm_ctx *lc, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(lc->module, node, index);
    return val != NULL && val->i > 0 ? (uint32_t)val->i : 0;
}

static bool is_sub(const lm_ctx *lc, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(lc->design, odin3_node_type(lc->module, node));
    return strcmp(def->name, "$sub") == 0;
}

/* The ties of the commit: Y[k] <- result[k] for every Y bit (the block is Y_WIDTH wide). */
static odin3_status make_ties(const lm_ctx *lc, odin3_node_id src, const odin3_net_id *result,
                              uint32_t width, odin3_vec *ties) {
    odin3_pinslice ypins = odin3_node_port(lc->module, src, PORT_Y);
    uint32_t bits = width < ypins.count ? width : ypins.count;
    odin3_status st = odin3_vec_reserve(ties, bits);
    for (uint32_t k = 0; st == ODIN3_OK && k < bits; k++) {
        lm_tie *tie = odin3_vec_push(ties); /* reserved: never NULL */
        *tie = (lm_tie){odin3_pin_net(lc->module, (odin3_pin_id){ypins.first.v + k}), result[k]};
    }
    return st;
}

/* The nets of the three buses of one block, in vectors of their own (A-2, A-3). */
typedef struct bus3 {
    odin3_vec va, vb, vr;
    odin3_net_id *a, *b, *r;
} bus3;

static odin3_status bus3_make(bus3 *bus, uint32_t width) {
    odin3_vec_init(&bus->va, sizeof(odin3_net_id));
    odin3_vec_init(&bus->vb, sizeof(odin3_net_id));
    odin3_vec_init(&bus->vr, sizeof(odin3_net_id));
    odin3_status st = lm_net_array(&bus->va, width, &bus->a);
    st = st == ODIN3_OK ? lm_net_array(&bus->vb, width, &bus->b) : st;
    return st == ODIN3_OK ? lm_net_array(&bus->vr, width, &bus->r) : st;
}

static void bus3_free(bus3 *bus) {
    odin3_vec_free(&bus->va);
    odin3_vec_free(&bus->vb);
    odin3_vec_free(&bus->vr);
}

/* Builds the block on fresh nets and the ties; nothing of the source changes. */
static odin3_status build_into(lm_ctx *lc, odin3_node_id src, bus3 *bus, uint32_t width,
                               odin3_vec *ties) {
    bool sgn = odin3_opt_both_signed(lc->module, src);
    odin3_status st = lm_begin_cell(lc, src);
    st = st == ODIN3_OK
             ? lm_extend(lc, odin3_node_port(lc->module, src, PORT_A), width, sgn, bus->a)
             : st;
    st = st == ODIN3_OK
             ? lm_extend(lc, odin3_node_port(lc->module, src, PORT_B), width, sgn, bus->b)
             : st;
    st = st == ODIN3_OK ? lm_fresh_nets(lc, width, bus->r) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    bool sub = is_sub(lc, src);
    const lm_pset params[] = {{"lpm_width", (int64_t)width, NULL},
                              {"lpm_direction", 0, sub ? "SUB" : "ADD"},
                              {"lpm_representation", 0, sgn ? "SIGNED" : "UNSIGNED"}};
    const lm_pconn ports[] = {
        {"dataa", bus->a, width}, {"datab", bus->b, width}, {"result", bus->r, width}};
    odin3_node_id node = {0};
    st = lm_new_node(lc, &(lm_node_req){LM_ADD_SUB, params, 3, ports, 3, sub ? "sub" : "add"},
                     &node);
    return st == ODIN3_OK ? make_ties(lc, src, bus->r, width, ties) : st;
}

static odin3_status build(lm_ctx *lc, odin3_node_id src, odin3_vec *ties) {
    uint32_t width = param_u32(lc, src, P_Y_WIDTH);
    bus3 bus;
    odin3_status st = bus3_make(&bus, width);
    st = st == ODIN3_OK ? build_into(lc, src, &bus, width, ties) : st;
    bus3_free(&bus);
    return st;
}

odin3_status lm_map_addsub(lm_ctx *lc, odin3_node_id src) {
    odin3_celltype_id type = {0};
    if (param_u32(lc, src, P_A_WIDTH) == 0 || param_u32(lc, src, P_B_WIDTH) == 0 ||
        param_u32(lc, src, P_Y_WIDTH) == 0) {
        return ODIN3_OK; /* a degenerate cell: left alone (A-1) */
    }
    odin3_status st = lm_need(lc, LM_ADD_SUB, src, &type);
    st = st == ODIN3_OK ? lm_transact(lc, src, build) : st;
    if (st == ODIN3_OK) {
        lc->stats.add_sub++;
    }
    return st;
}
