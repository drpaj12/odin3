/* build_mem.c — expands a `$mem` node into its flat cells (mem_sim.h). */
#include "ir/module.h"
#include "sim/mem_sim.h"
#include "sim/sim4.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEM_MAX_GROUPS = 16, /* distinct (clock net, polarity) pairs of one memory */
    MEM_E_SPANS = 9,     /* spans of an edge cell */
    MEM_A_SPANS = 2      /* spans of the asynchronous read cell */
};

/* One clock net and polarity that live ports use: one edge cell. */
typedef struct mem_group {
    uint32_t clk_slot; /* root slot of the clock net */
    bool negative;     /* the falling edge */
    uint32_t n_sync;   /* synchronous read ports of the group */
} mem_group;

/* What the node needs, found by the scan of its ports. */
typedef struct mem_plan {
    odin3_sim_mem_geo geo;
    const odin3_value *params;
    mem_group groups[MEM_MAX_GROUPS];
    uint32_t n_groups;  /* edge cells: 0 for a ROM or a memory of asynchronous reads only */
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

/* One clocked port joins the plan: its group is the one on its clock net and polarity (made when
 * it is the first). Returns the group, or the group count when there is no room. */
static uint32_t note_clock(odin3_sim_builder *bld, uint32_t frame, odin3_net_id net, bool negative,
                           mem_plan *plan) {
    uint32_t slot = odin3_net_valid(net)
                        ? odin3_sim_slot_root(bld, odin3_sim_net_slot(bld, frame, net))
                        : (uint32_t)ODIN3_SIM_SLOT_ZERO;
    for (uint32_t grp = 0; grp < plan->n_groups; grp++) {
        if (plan->groups[grp].clk_slot == slot && plan->groups[grp].negative == negative) {
            return grp;
        }
    }
    if (plan->n_groups == MEM_MAX_GROUPS) {
        plan->refuse = "has ports on more than 16 different clock nets or polarities";
        return plan->n_groups;
    }
    plan->groups[plan->n_groups] = (mem_group){slot, negative, 0};
    return plan->n_groups++;
}

static void scan_write_ports(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                             mem_plan *plan) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    for (uint32_t wport = 0; wport < plan->geo.n_wr && plan->refuse == NULL; wport++) {
        if (write_dead(mod, node, plan, wport)) {
            continue;
        }
        if (!bit_flag(&plan->params[ODIN3_SIM_MEM_P_WR_CLK_ENABLE], wport)) {
            plan->refuse = "has an asynchronous write port (a write with no clock has no event to "
                           "apply it on; `lower` refuses it too)";
            return;
        }
        (void)note_clock(bld, frame,
                         port_net(mod, node, (pin_at){ODIN3_SIM_MEM_PORT_WR_CLK, wport}),
                         !bit_flag(&plan->params[ODIN3_SIM_MEM_P_WR_CLK_POLARITY], wport), plan);
    }
}

static void scan_read_ports(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                            mem_plan *plan) {
    const odin3_module *mod = odin3_sim_frame_module(bld, frame);
    for (uint32_t rport = 0; rport < plan->geo.n_rd && plan->refuse == NULL; rport++) {
        if (!bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], rport)) {
            plan->n_async++;
            continue;
        }
        uint32_t grp =
            note_clock(bld, frame, port_net(mod, node, (pin_at){ODIN3_SIM_MEM_PORT_RD_CLK, rport}),
                       !bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_POLARITY], rport), plan);
        if (grp < plan->n_groups) {
            plan->groups[grp].n_sync++;
        }
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

/* The node being built: builder, frame, node and plan, passed together. */
typedef struct mem_at {
    odin3_sim_builder *bld;
    uint32_t frame;
    odin3_node_id node;
    const mem_plan *plan;
} mem_at;

/* A clocked port: its clock port (ODIN3_SIM_MEM_PORT_WR_CLK or _RD_CLK) and index. */
typedef struct port_ref {
    uint32_t clk_port;
    uint32_t port;
} port_ref;

/* The group of a clocked port (the scan made it): the number of groups when the port is on none (a
 * dead write port, an asynchronous read port). */
static uint32_t group_of(const mem_at *at, port_ref ref) {
    const mem_plan *plan = at->plan;
    const odin3_module *mod = odin3_sim_frame_module(at->bld, at->frame);
    bool write = ref.clk_port == ODIN3_SIM_MEM_PORT_WR_CLK;
    if (write ? write_dead(mod, at->node, plan, ref.port)
              : !bit_flag(&plan->params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], ref.port)) {
        return plan->n_groups;
    }
    odin3_net_id net = port_net(mod, at->node, (pin_at){ref.clk_port, ref.port});
    uint32_t slot = odin3_net_valid(net)
                        ? odin3_sim_slot_root(at->bld, odin3_sim_net_slot(at->bld, at->frame, net))
                        : (uint32_t)ODIN3_SIM_SLOT_ZERO;
    bool negative = !bit_flag(
        &plan->params[write ? ODIN3_SIM_MEM_P_WR_CLK_POLARITY : ODIN3_SIM_MEM_P_RD_CLK_POLARITY],
        ref.port);
    for (uint32_t grp = 0; grp < plan->n_groups; grp++) {
        if (plan->groups[grp].clk_slot == slot && plan->groups[grp].negative == negative) {
            return grp;
        }
    }
    return plan->n_groups;
}

/* One span holding all read ports of `port` that belong to group `group` (the synchronous ones on
 * that clock; group == n_groups: the asynchronous ones), `per` pins each. */
typedef struct sel_spec {
    uint32_t port;
    uint32_t per;
    uint32_t group;
    uint32_t open;
} sel_spec;

static bool selected(const mem_at *at, uint32_t group, uint32_t rport) {
    bool sync = bit_flag(&at->plan->params[ODIN3_SIM_MEM_P_RD_CLK_ENABLE], rport);
    if (group == at->plan->n_groups) {
        return !sync;
    }
    return sync && group_of(at, (port_ref){ODIN3_SIM_MEM_PORT_RD_CLK, rport}) == group;
}

static odin3_status push_selected(const mem_at *at, sel_spec spec) {
    uint32_t count = 0;
    for (uint32_t rport = 0; rport < at->plan->geo.n_rd; rport++) {
        count += selected(at, spec.group, rport) ? 1U : 0U;
    }
    odin3_status st = begin_span(at->bld, count * spec.per);
    for (uint32_t rport = 0; st == ODIN3_OK && rport < at->plan->geo.n_rd; rport++) {
        if (selected(at, spec.group, rport)) {
            st = push_run(at->bld, at->frame, at->node,
                          (pin_run){spec.port, rport * spec.per, spec.per}, spec.open);
        }
    }
    return st;
}

/* The per-port clock span of group `group`: its clock slot for a port on it, the zero slot for any
 * other (the hook tells which ports are its own by comparing with its CLK pin). */
static odin3_status push_membership(const mem_at *at, uint32_t clk_port, uint32_t group) {
    bool write = clk_port == ODIN3_SIM_MEM_PORT_WR_CLK;
    uint32_t count = write ? at->plan->geo.n_wr : at->plan->geo.n_rd;
    odin3_status st = begin_span(at->bld, count);
    for (uint32_t port = 0; st == ODIN3_OK && port < count; port++) {
        bool mine = group_of(at, (port_ref){clk_port, port}) == group;
        st = odin3_sim_push_u32(&at->bld->idx, mine ? at->plan->groups[group].clk_slot
                                                    : (uint32_t)ODIN3_SIM_SLOT_ZERO);
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

/* The edge cell of group `grp`. Its state is the registers of its synchronous read ports. */
static odin3_status add_edge(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                             const mem_plan *plan, uint32_t grp) {
    const odin3_sim_mem_geo *geo = &plan->geo;
    const mem_group *group = &plan->groups[grp];
    if (begin_cell(bld, frame, node, &odin3_sim_mem_edge_def) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = begin_span(bld, 1);
    st = st != ODIN3_OK ? st : odin3_sim_push_u32(&bld->idx, group->clk_slot);
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
    const mem_at at = {bld, frame, node, plan};
    st = st != ODIN3_OK ? st
                        : push_selected(&at, (sel_spec){ODIN3_SIM_MEM_PORT_RD_DATA, geo->width, grp,
                                                        ODIN3_SIM_SLOT_DISCARD});
    st = st != ODIN3_OK ? st : push_membership(&at, ODIN3_SIM_MEM_PORT_WR_CLK, grp);
    st = st != ODIN3_OK ? st : push_membership(&at, ODIN3_SIM_MEM_PORT_RD_CLK, grp);
    odin3_sim_flat *flat = last_cell(bld);
    flat->view.flags = (group->negative ? ODIN3_SIM_CELL_MEM_NEG : 0U) |
                       (plan->n_groups > 1 ? ODIN3_SIM_CELL_MEM_DEFER : 0U);
    flat->sim4 = bld->four_state ? odin3_sim_mem_edge4 : NULL;
    flat->view.n_state = group->n_sync * geo->width;
    bld->n_state += flat->view.n_state;
    return st;
}

static odin3_status add_async(odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                              const mem_plan *plan) {
    const odin3_sim_mem_geo *geo = &plan->geo;
    if (begin_cell(bld, frame, node, &odin3_sim_mem_async_def) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const mem_at at = {bld, frame, node, plan};
    odin3_status st = push_selected(&at, (sel_spec){ODIN3_SIM_MEM_PORT_RD_ADDR, geo->abits,
                                                    plan->n_groups, ODIN3_SIM_SLOT_ZERO});
    st = st != ODIN3_OK ? st
                        : push_selected(&at, (sel_spec){ODIN3_SIM_MEM_PORT_RD_DATA, geo->width,
                                                        plan->n_groups, ODIN3_SIM_SLOT_DISCARD});
    last_cell(bld)->sim4 = bld->four_state ? odin3_sim_mem_async4 : NULL;
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
    uint64_t regs = 0;
    for (uint32_t grp = 0; grp < plan->n_groups; grp++) {
        regs += (uint64_t)plan->groups[grp].n_sync * plan->geo.width;
    }
    if (plan->refuse == NULL && (plan->geo.shared_size > UINT32_MAX ||
                                 plan->geo.shared_size + regs > UINT32_MAX - bld->n_state)) {
        plan->refuse = "is too large to simulate (more than 4 GiB of state)";
    }
}

/* The cells just added share the block of the memory: the next shared_size bytes of state. The
 * first cell of the node owns it (sets it from INIT). */
static void share_block(odin3_sim_builder *bld, const mem_plan *plan, size_t first_cell) {
    uint32_t block = bld->n_state;
    bld->n_state += (uint32_t)plan->geo.shared_size;
    for (size_t k = first_cell; k < bld->cells.len; k++) {
        odin3_sim_flat *flat = odin3_vec_at(&bld->cells, k);
        flat->has_aux = true;
        flat->aux_first = block;
        flat->view.flags |= k == first_cell ? (uint32_t)ODIN3_SIM_CELL_MEM_INIT : 0U;
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
    uint64_t cells = plan.n_groups + (plan.n_async > 0 ? 1U : 0U);
    odin3_sim_growth grow = {cells, (cells + 1U) * (odin3_node_pins(mod, node).count + 1U) +
                                        (uint64_t)plan.n_groups * (plan.geo.n_wr + plan.geo.n_rd)};
    if (!odin3_sim_check_budget(bld, frame, node, grow)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (bld->cells.len + cells >= ODIN3_SIM_UNSET ||
        bld->spans.len + (uint64_t)plan.n_groups * MEM_E_SPANS + MEM_A_SPANS >= ODIN3_SIM_UNSET) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t first_cell = bld->cells.len;
    odin3_status st = ODIN3_OK;
    for (uint32_t grp = 0; st == ODIN3_OK && grp < plan.n_groups; grp++) {
        st = add_edge(bld, frame, node, &plan, grp);
    }
    if (st == ODIN3_OK && plan.n_async > 0) {
        st = add_async(bld, frame, node, &plan);
    }
    share_block(bld, &plan, first_cell);
    if (plan.geo.width > bld->n_scratch) {
        bld->n_scratch = plan.geo.width;
    }
    return st;
}
