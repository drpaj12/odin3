/*
 * ram.c — `single_port_ram` / `dual_port_ram` to `$mem` (R-1 to R-4).
 */
#include "passes/unmap/ram.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The pin numbers of the hard cells, per port (src/passes/partial_map/ram.c k_sp_layout and
 * k_dp_layout; the cell's own port numbers). The clock is port 0 in both. */
typedef struct um_layout {
    uint32_t n_ports;
    uint32_t data[UM_RAM_PORTS];
    uint32_t addr[UM_RAM_PORTS];
    uint32_t we[UM_RAM_PORTS];
    uint32_t out[UM_RAM_PORTS];
} um_layout;

static const um_layout k_single = {1, {1, 0}, {2, 0}, {3, 0}, {4, 0}};
static const um_layout k_dual = {2, {2, 1}, {4, 3}, {6, 5}, {8, 7}};

enum {
    P_MEMID,
    P_SIZE,
    P_OFFSET,
    P_ABITS,
    P_WIDTH,
    P_RD_PORTS,
    P_WR_PORTS,
    P_RD_CLK_ENABLE,
    P_RD_CLK_POLARITY,
    P_RD_TRANSPARENT,
    P_WR_CLK_ENABLE,
    P_WR_CLK_POLARITY,
    P_INIT
};

enum {
    UM_CLK = 0,
    UM_MEM_PARAMS = 13,
    UM_MEM_PORTS = 8,
    UM_MAX_ABITS = 31,
    UM_MAX_AND_DEPTH = 64,
    UM_MEMID_CAP = 256
};

static const um_layout *layout_of(bool dual) {
    return dual ? &k_dual : &k_single;
}

/* --- the summary of one cell (R-2) --------------------------------------------------------- */

/* 1 + the highest index of port `port` whose class is not pad or open; 0 when none. */
static uint32_t highest_real(const um_ctx *uc, odin3_node_id node, uint32_t port) {
    uint32_t count = odin3_node_port(uc->module, node, port).count;
    for (uint32_t i = count; i > 0; i--) {
        um_cls cls = um_net_class(uc->module, um_port_net(uc->module, node, (um_pos){port, i - 1}));
        if (cls != UM_PAD && cls != UM_NONE) {
            return i;
        }
    }
    return 0;
}

/* 1 + the highest index of port `port` that somebody reads; 0 when none. */
static uint32_t highest_observed(const um_ctx *uc, odin3_node_id node, uint32_t port) {
    uint32_t count = odin3_node_port(uc->module, node, port).count;
    for (uint32_t i = count; i > 0; i--) {
        if (um_observed(uc->module, um_port_net(uc->module, node, (um_pos){port, i - 1}))) {
            return i;
        }
    }
    return 0;
}

/* True when the write enable is constant 0 (or open, or pad), also through the `$and` chain of a
 * depth split: an `$and` with a constant 0 input is 0. A loop up the first input, no recursion. */
static bool never_writes(const um_ctx *uc, odin3_net_id net) {
    for (uint32_t depth = 0; depth < UM_MAX_AND_DEPTH; depth++) {
        um_cls cls = um_net_class(uc->module, net);
        if (cls == UM_GND || cls == UM_NONE || cls == UM_PAD) {
            return true;
        }
        odin3_pin_id driver = odin3_net_driver(uc->module, net);
        odin3_node_id gate =
            odin3_pin_valid(driver) ? odin3_pin_node(uc->module, driver) : (odin3_node_id){0};
        if (gate.v == 0 || odin3_node_type(uc->module, gate).v != uc->types.and_.v ||
            odin3_node_port(uc->module, gate, 0).count != 1) {
            return false;
        }
        if (um_net_class(uc->module, um_port_net(uc->module, gate, (um_pos){1, 0})) == UM_GND) {
            return true;
        }
        net = um_port_net(uc->module, gate, (um_pos){0, 0});
    }
    return false;
}

static uint32_t max_of(uint32_t left, uint32_t right) {
    return left > right ? left : right;
}

odin3_status um_ram_summary(const um_ctx *uc, odin3_node_id node, um_ram *out) {
    memset(out, 0, sizeof *out);
    out->node = node;
    out->dual = odin3_node_type(uc->module, node).v == uc->types.dp_ram.v;
    const um_layout *lay = layout_of(out->dual);
    uint32_t abits = 1;
    uint32_t width = 1;
    for (uint32_t port = 0; port < lay->n_ports; port++) {
        out->write[port] =
            !never_writes(uc, um_port_net(uc->module, node, (um_pos){lay->we[port], 0}));
        uint32_t seen = highest_observed(uc, node, lay->out[port]);
        out->read[port] = seen > 0;
        if (out->write[port] || out->read[port]) {
            abits = max_of(abits, highest_real(uc, node, lay->addr[port]));
        }
        if (out->write[port]) {
            width = max_of(width, highest_real(uc, node, lay->data[port]));
        }
        width = max_of(width, seen);
        out->rd_ports += out->read[port] ? 1 : 0;
        out->wr_ports += out->write[port] ? 1 : 0;
    }
    out->rd_ports = max_of(out->rd_ports, 1);
    out->abits = abits;
    out->width = width;
    return ODIN3_OK;
}

/* --- the new cell -------------------------------------------------------------------------- */

typedef struct um_mem {
    odin3_vec port[UM_MEM_PORTS]; /* odin3_net_id per $mem port, in port order */
    uint32_t reads;
    uint32_t writes;
} um_mem;

enum {
    MEM_RD_CLK,
    MEM_RD_EN,
    MEM_RD_ADDR,
    MEM_RD_DATA,
    MEM_WR_CLK,
    MEM_WR_EN,
    MEM_WR_ADDR,
    MEM_WR_DATA
};

static void mem_init(um_mem *mem) {
    memset(mem, 0, sizeof *mem);
    for (uint32_t i = 0; i < UM_MEM_PORTS; i++) {
        odin3_vec_init(&mem->port[i], sizeof(odin3_net_id));
    }
}

static void mem_free(um_mem *mem) {
    for (uint32_t i = 0; i < UM_MEM_PORTS; i++) {
        odin3_vec_free(&mem->port[i]);
    }
}

static odin3_status put_net(um_mem *mem, uint32_t port, odin3_net_id net) {
    odin3_net_id *item = odin3_vec_push(&mem->port[port]);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *item = net;
    return ODIN3_OK;
}

static odin3_status put_copies(um_mem *mem, uint32_t port, odin3_net_id net, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        odin3_status st = put_net(mem, port, net);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A port of a replica of the group: `replica` of its cells, `index` the source port (0 or 1). */
typedef struct um_pick {
    const um_rgroup *group;
    uint32_t replica;
    uint32_t index;
} um_pick;

static const um_ram *cell_at(const um_rgroup *group, uint32_t replica, uint32_t slice) {
    return (const um_ram *)group->cells.data + (size_t)replica * group->slices + slice;
}

static uint32_t total_width(const um_rgroup *group) {
    uint32_t width = 0;
    for (uint32_t slice = 0; slice < group->slices; slice++) {
        width += cell_at(group, 0, slice)->width;
    }
    return width;
}

/* The address nets of source port `pick.index` of the replica's first cell, abits of them: a pad
 * or open bit becomes gnd. */
static odin3_status put_addr(um_ctx *uc, um_mem *mem, uint32_t port, um_pick pick) {
    const um_ram *cell = cell_at(pick.group, pick.replica, 0);
    const um_layout *lay = layout_of(cell->dual);
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < cell->abits; bit++) {
        odin3_net_id net =
            um_port_net(uc->module, cell->node, (um_pos){lay->addr[pick.index], bit});
        um_cls cls = um_net_class(uc->module, net);
        if (cls == UM_PAD || cls == UM_NONE) {
            st = um_const_net(uc, false, &net);
        }
        st = st == ODIN3_OK ? put_net(mem, port, net) : st;
    }
    return st;
}

/* The data nets of the replica, slice after slice: the inputs `data` or the outputs `out` of the
 * source port, `width` bits of each slice. */
static odin3_status put_data(const um_ctx *uc, um_mem *mem, uint32_t port, um_pick pick) {
    odin3_status st = ODIN3_OK;
    for (uint32_t slice = 0; st == ODIN3_OK && slice < pick.group->slices; slice++) {
        const um_ram *cell = cell_at(pick.group, pick.replica, slice);
        const um_layout *lay = layout_of(cell->dual);
        uint32_t source = port == MEM_RD_DATA ? lay->out[pick.index] : lay->data[pick.index];
        for (uint32_t bit = 0; st == ODIN3_OK && bit < cell->width; bit++) {
            st = put_net(mem, port, um_port_net(uc->module, cell->node, (um_pos){source, bit}));
        }
    }
    return st;
}

static odin3_net_id clock_of(const um_ctx *uc, const um_ram *cell) {
    return um_port_net(uc->module, cell->node, (um_pos){UM_CLK, 0});
}

static odin3_status build_read(um_ctx *uc, um_mem *mem, um_pick pick) {
    const um_ram *cell = cell_at(pick.group, pick.replica, 0);
    odin3_net_id vcc = {0};
    odin3_status st = um_const_net(uc, true, &vcc);
    st = st == ODIN3_OK ? put_net(mem, MEM_RD_CLK, clock_of(uc, cell)) : st;
    st = st == ODIN3_OK ? put_net(mem, MEM_RD_EN, vcc) : st;
    st = st == ODIN3_OK ? put_addr(uc, mem, MEM_RD_ADDR, pick) : st;
    st = st == ODIN3_OK ? put_data(uc, mem, MEM_RD_DATA, pick) : st;
    mem->reads++;
    return st;
}

static odin3_status build_write(um_ctx *uc, um_mem *mem, um_pick pick) {
    const um_ram *cell = cell_at(pick.group, pick.replica, 0);
    const um_layout *lay = layout_of(cell->dual);
    odin3_net_id we = um_port_net(uc->module, cell->node, (um_pos){lay->we[pick.index], 0});
    odin3_status st = put_net(mem, MEM_WR_CLK, clock_of(uc, cell));
    st = st == ODIN3_OK ? put_copies(mem, MEM_WR_EN, we, total_width(pick.group)) : st;
    st = st == ODIN3_OK ? put_addr(uc, mem, MEM_WR_ADDR, pick) : st;
    st = st == ODIN3_OK ? put_data(uc, mem, MEM_WR_DATA, pick) : st;
    mem->writes++;
    return st;
}

/* R-3: a RAM nobody reads keeps one unconnected read port. */
static odin3_status build_dummy_read(um_ctx *uc, um_mem *mem, const um_rgroup *group) {
    const um_ram *cell = cell_at(group, 0, 0);
    odin3_net_id vcc = {0};
    odin3_net_id gnd = {0};
    odin3_status st = um_const_net(uc, true, &vcc);
    st = st == ODIN3_OK ? um_const_net(uc, false, &gnd) : st;
    st = st == ODIN3_OK ? put_net(mem, MEM_RD_CLK, clock_of(uc, cell)) : st;
    st = st == ODIN3_OK ? put_net(mem, MEM_RD_EN, vcc) : st;
    st = st == ODIN3_OK ? put_copies(mem, MEM_RD_ADDR, gnd, cell->abits) : st;
    st = st == ODIN3_OK ? put_copies(mem, MEM_RD_DATA, (odin3_net_id){0}, total_width(group)) : st;
    mem->reads++;
    return st;
}

/* All the ports of the $mem: the read ports of every replica, the write ports of replica 0. */
static odin3_status build_ports(um_ctx *uc, const um_rgroup *group, um_mem *mem) {
    uint32_t replicas = (uint32_t)group->cells.len / group->slices;
    odin3_status st = ODIN3_OK;
    for (uint32_t replica = 0; st == ODIN3_OK && replica < replicas; replica++) {
        const um_ram *cell = cell_at(group, replica, 0);
        for (uint32_t index = 0; st == ODIN3_OK && index < UM_RAM_PORTS; index++) {
            st = cell->read[index] ? build_read(uc, mem, (um_pick){group, replica, index}) : st;
        }
    }
    const um_ram *first = cell_at(group, 0, 0);
    for (uint32_t index = 0; st == ODIN3_OK && index < UM_RAM_PORTS; index++) {
        st = first->write[index] ? build_write(uc, mem, (um_pick){group, 0, index}) : st;
    }
    return st == ODIN3_OK && mem->reads == 0 ? build_dummy_read(uc, mem, group) : st;
}

/* --- the parameters ------------------------------------------------------------------------ */

/* The BITS payload of the five per-port parameters, `count` bytes each, set to `bit`. */
static odin3_status push_bits(odin3_vec *bits, uint32_t count, odin3_bit bit) {
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *item = odin3_vec_push(bits);
        if (item == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *item = (uint8_t)bit;
    }
    return ODIN3_OK;
}

static odin3_value bits_value(const odin3_vec *bits, size_t first, uint32_t count) {
    const uint8_t *data = bits->data;
    return (odin3_value){ODIN3_VAL_BITS, 0, count > 0 ? data + first : NULL, count, 0, 0};
}

/* MEMID: the namesake's name, else `$mem$<node id>`. */
static odin3_status memid_value(um_ctx *uc, odin3_node_id namesake, odin3_value *out) {
    char text[UM_MEMID_CAP];
    uint32_t name = odin3_node_name(uc->module, namesake);
    if (name != 0) {
        (void)snprintf(text, sizeof text, "%s",
                       odin3_strtab_get(odin3_design_strtab(uc->design), name));
    } else {
        (void)snprintf(text, sizeof text, "$mem$%" PRIu32, namesake.v);
    }
    uint32_t str = 0;
    odin3_status st = odin3_design_intern(uc->design, odin3_bytes_cstr(text), &str);
    *out = (odin3_value){ODIN3_VAL_STRING, 0, NULL, 0, str, 0};
    return st;
}

typedef struct um_shape {
    uint32_t abits;
    uint32_t width;
    uint32_t reads;
    uint32_t writes;
} um_shape;

/* The thirteen parameters of `$mem` (src/ir/cells/mem.c order); `bits` holds the BITS payloads. */
static odin3_status mem_params(um_ctx *uc, odin3_node_id namesake, um_shape shape, odin3_vec *bits,
                               odin3_value *params) {
    odin3_status st = push_bits(bits, shape.reads * 2, ODIN3_BIT_1);
    st = st == ODIN3_OK ? push_bits(bits, shape.reads, ODIN3_BIT_0) : st;
    st = st == ODIN3_OK ? push_bits(bits, shape.writes * 2, ODIN3_BIT_1) : st;
    st = st == ODIN3_OK ? memid_value(uc, namesake, &params[0]) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    size_t rd = shape.reads;
    size_t wr = shape.writes;
    params[P_SIZE] = odin3_value_int((int64_t)1 << shape.abits);
    params[P_OFFSET] = odin3_value_int(0);
    params[P_ABITS] = odin3_value_int(shape.abits);
    params[P_WIDTH] = odin3_value_int(shape.width);
    params[P_RD_PORTS] = odin3_value_int(shape.reads);
    params[P_WR_PORTS] = odin3_value_int(shape.writes);
    params[P_RD_CLK_ENABLE] = bits_value(bits, 0, shape.reads);
    params[P_RD_CLK_POLARITY] = bits_value(bits, rd, shape.reads);
    params[P_RD_TRANSPARENT] = bits_value(bits, 2 * rd, shape.reads);
    params[P_WR_CLK_ENABLE] = bits_value(bits, 3 * rd, shape.writes);
    params[P_WR_CLK_POLARITY] = bits_value(bits, 3 * rd + wr, shape.writes);
    params[P_INIT] = bits_value(bits, 0, 0);
    return ODIN3_OK;
}

/* R-5: the record the cells share, 0 when they have none in common. */
static int64_t shared_origin(const um_ctx *uc, const um_rgroup *group) {
    const um_ram *cells = group->cells.data;
    uint32_t first = odin3_node_prov(uc->module, cells[0].node).v;
    for (size_t i = 1; i < group->cells.len; i++) {
        if (odin3_node_prov(uc->module, cells[i].node).v != first) {
            return 0;
        }
    }
    return first;
}

static odin3_status replace_cells(um_ctx *uc, const um_rgroup *group, const um_new *req) {
    um_group nodes;
    um_group_init(&nodes);
    const um_ram *cells = group->cells.data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < group->cells.len; i++) {
        st = um_group_add(&nodes, cells[i].node);
    }
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? um_replace(uc, &nodes, req, &made) : st;
    um_group_free(&nodes);
    return st;
}

odin3_status um_ram_emit(um_ctx *uc, const um_rgroup *group) {
    const um_ram *cells = group->cells.data;
    um_mem mem;
    mem_init(&mem);
    odin3_vec bits;
    odin3_vec_init(&bits, sizeof(uint8_t));
    odin3_status st = ODIN3_OK;
    if (cells[0].abits > UM_MAX_ABITS) {
        odin3_node_id node = cells[0].node;
        st = um_decline(uc, &node, 1, (um_why){"ram", "abits"});
        mem_free(&mem);
        return st;
    }
    st = build_ports(uc, group, &mem);
    odin3_value params[UM_MEM_PARAMS];
    um_shape shape = {cells[0].abits, total_width(group), mem.reads, mem.writes};
    st = st == ODIN3_OK ? mem_params(uc, cells[0].node, shape, &bits, params) : st;
    odin3_netvec ports[UM_MEM_PORTS];
    for (uint32_t i = 0; i < UM_MEM_PORTS; i++) {
        ports[i] = (odin3_netvec){mem.port[i].data, (uint32_t)mem.port[i].len};
        if (mem.port[i].len == 0) {
            static const odin3_net_id k_none = {0};
            ports[i].nets = &k_none;
        }
    }
    um_new req = {uc->types.mem,
                  params,
                  UM_MEM_PARAMS,
                  ports,
                  cells[0].node,
                  group->rule,
                  shared_origin(uc, group),
                  0};
    st = st == ODIN3_OK ? replace_cells(uc, group, &req) : st;
    if (st == ODIN3_OK) {
        uc->stats.mem++;
    }
    odin3_vec_free(&bits);
    mem_free(&mem);
    return st;
}

/* --- the phase ----------------------------------------------------------------------------- */

static bool is_ram(const um_ctx *uc, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(uc->module, node);
    return odin3_node_live(uc->module, node) &&
           ((uc->types.sp_ram.v != 0 && type.v == uc->types.sp_ram.v) ||
            (uc->types.dp_ram.v != 0 && type.v == uc->types.dp_ram.v));
}

static odin3_status single_group(const um_ram *cell, um_rgroup *group) {
    odin3_vec_init(&group->cells, sizeof(um_ram));
    group->slices = 1;
    group->rule = "ram";
    um_ram *slot = odin3_vec_push(&group->cells);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *cell;
    return ODIN3_OK;
}

/* The summaries of every live RAM cell, node-ID order. */
static odin3_status summarise(const um_ctx *uc, odin3_vec *cells) {
    uint32_t end = odin3_module_node_end(uc->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < end; nid++) {
        odin3_node_id node = {nid};
        if (!is_ram(uc, node)) {
            continue;
        }
        um_ram *slot = odin3_vec_push(cells);
        st = slot != NULL ? um_ram_summary(uc, node, slot) : ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

/* Every cell alone (--no-regroup). */
static odin3_status alone(const odin3_vec *cells, odin3_vec *groups) {
    odin3_vec_init(groups, sizeof(um_rgroup));
    const um_ram *items = cells->data;
    for (size_t i = 0; i < cells->len; i++) {
        um_rgroup *group = odin3_vec_push(groups);
        if (group == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        odin3_status st = single_group(&items[i], group);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status um_ram_phase(um_ctx *uc) {
    odin3_vec cells;
    odin3_vec groups;
    odin3_vec_init(&cells, sizeof(um_ram));
    odin3_vec_init(&groups, sizeof(um_rgroup));
    odin3_status st = summarise(uc, &cells);
    uc->stats.ram_cells += cells.len;
    if (st == ODIN3_OK) {
        st = uc->opts->regroup ? um_ram_groups(uc, &cells, &groups) : alone(&cells, &groups);
    }
    const um_rgroup *items = groups.data;
    for (size_t i = 0; st == ODIN3_OK && i < groups.len; i++) {
        st = um_ram_emit(uc, &items[i]);
    }
    um_rgroups_free(&groups);
    odin3_vec_free(&cells);
    return st;
}
