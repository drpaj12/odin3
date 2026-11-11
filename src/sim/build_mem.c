/* build_mem.c — expands a `$mem` node into its flat cells (mem_sim.h). */
#include "ir/module.h"
#include "sim/mem_sim.h"
#include "sim/sim4.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MEM_MAX_SPANS = 16 }; /* spans a node adds at most (7 + 2, with room) */

/* What the node needs, found by the scan of its ports. */
typedef struct mem_plan {
    odin3_sim_mem_geo geo;
    const odin3_value *params;
    bool has_edge;      /* a live write port or a synchronous read port */
    bool negative;      /* the memory's clock edge is the falling one */
    uint32_t clk_slot;  /* root slot of the common clock */
    uint32_t n_async;   /* asynchronous read ports */
    const char *refuse; /* why the memory cannot be simulated, NULL when it can */
} mem_plan;

/* Bit `bit` of port `port` of a node. */
typedef struct pin_at {
    uint32_t port;
    uint32_t bit;
} pin_at;

static odin3_net_id port_net(const odin3_module *mod, odin3_node_id node, pin_at at) {
    odin3_pinslice pins = odin3_node_port(mod, node, at.port);
    if (at.bit >= pins.count) {
        return (odin3_net_id){0};
    }
    return odin3_pin_net(mod, (odin3_pin_id){pins.first.v + at.bit});
}

/* True when every WR_EN bit of write port wport is constant 0 (or open): the port never writes. */
static bool write_dead(const odin3_module *mod, odin3_node_id node, const mem_plan *plan,
                       uint32_t wport) {
    for (uint32_t bit = 0; bit < plan->geo.width; bit++) {
        odin3_net_id net =
            port_net(mod, node, (pin_at){ODIN3_SIM_MEM_PORT_WR_EN, wport * plan->geo.width + bit});
        if (odin3_net_valid(net) && odin3_net_const_value(mod, net) != ODIN3_CONST_0) {
            return false;
        }
    }
    return true;
}

static bool bit_flag(const odin3_value *val, uint32_t idx) {
    return val->kind == ODIN3_VAL_BITS && idx < val->len && val->bits[idx] == ODIN3_BIT_1;
}

/* One clocked port joins the plan: its clock net and polarity must equal those before it. */
static void note_clock(odin3_sim_builder *bld, uint32_t frame, odin3_net_id net, bool negative,
                       mem_plan *plan) {
    uint32_t slot = odin3_net_valid(net)
                        ? odin3_sim_slot_root(bld, odin3_sim_net_slot(bld, frame, net))
                        : (uint32_t)ODIN3_SIM_SLOT_ZERO;
    if (!plan->has_edge) {
        plan->has_edge = true;
        plan->negative = negative;
        plan->clk_slot = slot;
    } else if (plan->clk_slot != slot || plan->negative != negative) {
        plan->refuse = "its ports are on several clocks or clock polarities (one clock per memory "
                       "is supported); `lower` handles write ports on one clock only too";
    }
}

static void scan_write_ports(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                             mem_plan *plan) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    for (uint32_t wport = 0; wport < plan->geo.n_wr; wport++) {
        if (write_dead(mod, node, plan, wport)) {
            continue;
        }
        if (!bit_flag(&plan->params[ODIN3_SIM_MEM_P_WR_CLK_ENABLE], wport)) {
            plan->refuse = "has an asynchronous write port";
            return;
        }
        note_clock(bld, frame, port_net(mod, node, (pin_at){ODIN3_SIM_MEM_PORT_WR_CLK, wport}),
                   !bit_flag(&plan->params[ODIN3_SIM_MEM_P_WR_CLK_POLARITY], wport), plan);
    }
}

static void scan_read_ports(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                            mem_plan *plan) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    for (uint32_t rport = 0; rport < plan->geo.n_rd; rport++) {
        if (!bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], rport)) {
            plan->n_async++;
            continue;
        }
        note_clock(bld, frame, port_net(mod, node, (pin_at){ODIN3_SIM_MEM_PORT_RD_CLK, rport}),
                   !bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_POLARITY], rport), plan);
    }
}

/* --- pins ------------------------------------------------------------------------------------- */

/* A run of the pins of one port of the node: bits [first, first + count). */
typedef struct pin_run {
    uint32_t port;
    uint32_t first;
    uint32_t count;
} pin_run;

/* Appends the slots of the run (open: ZERO for an input, DISCARD for an output). */
static odin3_status push_run(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                             pin_run run, uint32_t open) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    odin3_pinslice pins = odin3_node_port(mod, node, run.port);
    if (odin3_vec_reserve(&bld->idx, bld->idx.len + run.count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t k = 0; k < run.count; k++) {
        odin3_net_id net = run.first + k < pins.count
                               ? odin3_pin_net(mod, (odin3_pin_id){pins.first.v + run.first + k})
                               : (odin3_net_id){0};
        uint32_t slot = odin3_net_valid(net) ? odin3_sim_net_slot(bld, frame, net) : open;
        if (odin3_sim_push_u32(&bld->idx, slot) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* Starts the next port span of the cell being built (its width is set when the run is known). */
static odin3_status begin_span(odin3_sim_builder *bld, uint32_t width) {
    odin3_sim_span *span = odin3_vec_push(&bld->spans);
    if (span == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    span->idx = NULL;
    span->width = width;
    return ODIN3_OK;
}

/* One span holding all ports of `port` whose read-port flag equals `sync` (sync < 0: every port),
 * `per` pins each. */
typedef struct sel_spec {
    uint32_t port;
    uint32_t per;
    int sync; /* 1: synchronous read ports, 0: asynchronous, -1: all */
    uint32_t open;
} sel_spec;

static odin3_status push_selected(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                                  const mem_plan *plan, sel_spec spec) {
    uint32_t count = 0;
    for (uint32_t rport = 0; rport < plan->geo.n_rd; rport++) {
        bool sync = bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], rport);
        count += spec.sync < 0 || sync == (spec.sync == 1) ? 1U : 0U;
    }
    odin3_status st = begin_span(bld, count * spec.per);
    for (uint32_t rport = 0; st == ODIN3_OK && rport < plan->geo.n_rd; rport++) {
        bool sync = bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], rport);
        if (spec.sync < 0 || sync == (spec.sync == 1)) {
            st = push_run(bld, frame, node, (pin_run){spec.port, rport * spec.per, spec.per},
                          spec.open);
        }
    }
    return st;
}

static odin3_status push_whole(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                               pin_run run) {
    odin3_status st = begin_span(bld, run.count);
    return st != ODIN3_OK ? st : push_run(bld, frame, node, run, ODIN3_SIM_SLOT_ZERO);
}

/* --- cells ------------------------------------------------------------------------------------ */

static odin3_sim_flat *begin_cell(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                                  const odin3_celltype_def *def) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    odin3_sim_flat *flat = odin3_vec_push(&bld->cells);
    if (flat == NULL) {
        return NULL;
    }
    *flat = (odin3_sim_flat){.def = def,
                             .module = odin3_module_id_of(mod),
                             .node = node,
                             .frame = frame,
                             .span_first = (uint32_t)bld->spans.len,
                             .idx_first = (uint32_t)bld->idx.len,
                             .state_first = bld->n_state};
    flat->view.n_ports = def->n_ports;
    flat->view.params = odin3_node_param(mod, node, 0);
    return flat;
}

/* Address of the cell just begun is stale after a vector grows: callers re-fetch by index. */
static odin3_sim_flat *last_cell(odin3_sim_builder *bld) {
    return odin3_vec_at(&bld->cells, bld->cells.len - 1U);
}

static odin3_status add_edge(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                             const mem_plan *plan) {
    const odin3_sim_mem_geo *geo = &plan->geo;
    if (begin_cell(bld, frame, node, &odin3_sim_mem_edge_def) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = begin_span(bld, 1);
    /* the clock: the first clocked port's pin; its root slot is what the other ports share */
    st = st != ODIN3_OK ? st : odin3_sim_push_u32(&bld->idx, plan->clk_slot);
    st = st != ODIN3_OK
             ? st
             : push_whole(bld, frame, node,
                          (pin_run){ODIN3_SIM_MEM_PORT_WR_EN, 0, geo->n_wr * geo->width});
    st = st != ODIN3_OK
             ? st
             : push_whole(bld, frame, node,
                          (pin_run){ODIN3_SIM_MEM_PORT_WR_ADDR, 0, geo->n_wr * geo->abits});
    st = st != ODIN3_OK
             ? st
             : push_whole(bld, frame, node,
                          (pin_run){ODIN3_SIM_MEM_PORT_WR_DATA, 0, geo->n_wr * geo->width});
    st = st != ODIN3_OK
             ? st
             : push_whole(bld, frame, node, (pin_run){ODIN3_SIM_MEM_PORT_RD_EN, 0, geo->n_rd});
    st = st != ODIN3_OK
             ? st
             : push_whole(bld, frame, node,
                          (pin_run){ODIN3_SIM_MEM_PORT_RD_ADDR, 0, geo->n_rd * geo->abits});
    st = st != ODIN3_OK ? st
                        : push_selected(bld, frame, node, plan,
                                        (sel_spec){ODIN3_SIM_MEM_PORT_RD_DATA, geo->width, 1,
                                                   ODIN3_SIM_SLOT_DISCARD});
    odin3_sim_flat *flat = last_cell(bld);
    flat->view.flags = ODIN3_SIM_CELL_MEM_INIT | (plan->negative ? ODIN3_SIM_CELL_MEM_NEG : 0U);
    flat->sim4 = bld->four_state ? odin3_sim_mem_edge4 : NULL;
    flat->view.n_state = (uint32_t)geo->state_size;
    return st;
}

static odin3_status add_async(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                              const mem_plan *plan, bool owner) {
    const odin3_sim_mem_geo *geo = &plan->geo;
    uint32_t first_state = bld->n_state;
    if (!owner) {
        first_state = last_cell(bld)->state_first; /* E was added just before */
    }
    if (begin_cell(bld, frame, node, &odin3_sim_mem_async_def) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st =
        push_selected(bld, frame, node, plan,
                      (sel_spec){ODIN3_SIM_MEM_PORT_RD_ADDR, geo->abits, 0, ODIN3_SIM_SLOT_ZERO});
    st = st != ODIN3_OK ? st
                        : push_selected(bld, frame, node, plan,
                                        (sel_spec){ODIN3_SIM_MEM_PORT_RD_DATA, geo->width, 0,
                                                   ODIN3_SIM_SLOT_DISCARD});
    odin3_sim_flat *flat = last_cell(bld);
    flat->state_first = first_state;
    flat->view.flags = owner ? ODIN3_SIM_CELL_MEM_INIT : 0U;
    flat->sim4 = bld->four_state ? odin3_sim_mem_async4 : NULL;
    flat->view.n_state = (uint32_t)geo->state_size;
    return st;
}

static void plan_init(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node, mem_plan *plan) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    *plan = (mem_plan){0};
    plan->params = odin3_node_param(mod, node, 0);
    odin3_sim_mem_geometry(plan->params, &plan->geo);
    scan_write_ports(bld, frame, node, plan);
    if (plan->refuse == NULL) {
        scan_read_ports(bld, frame, node, plan);
    }
    if (plan->refuse == NULL && plan->geo.state_size > UINT32_MAX - bld->n_state) {
        plan->refuse = "is too large to simulate (more than 4 GiB of state)";
    }
}

odin3_status odin3_sim_add_mem(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    mem_plan plan;
    plan_init(bld, frame, node, &plan);
    if (plan.refuse != NULL) {
        odin3_sim_err_mem(bld, frame, node, plan.refuse);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (plan.geo.n_rd == 0) {
        return ODIN3_OK; /* nothing can observe it (`lower` deletes it too) */
    }
    odin3_sim_growth grow = {2, odin3_node_pins(mod, node).count + 1U};
    if (!odin3_sim_check_budget(bld, frame, node, grow)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (bld->cells.len + 2U >= ODIN3_SIM_UNSET ||
        bld->spans.len + MEM_MAX_SPANS >= ODIN3_SIM_UNSET) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    uint32_t state = (uint32_t)plan.geo.state_size;
    if (plan.has_edge) {
        st = add_edge(bld, frame, node, &plan);
    }
    if (st == ODIN3_OK && plan.n_async > 0) {
        st = add_async(bld, frame, node, &plan, !plan.has_edge);
    }
    bld->n_state += state;
    if (plan.geo.width > bld->n_scratch) {
        bld->n_scratch = plan.geo.width;
    }
    return st;
}
