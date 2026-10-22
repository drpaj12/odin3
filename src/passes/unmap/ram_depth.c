/*
 * ram_depth.c — depth recovery (R-8). The write side (step 1) here; the read side and the merge
 * follow in the same file.
 */
#include "passes/unmap/ram_depth.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"

#include "passes/unmap/ram.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

enum {
    UM_MEM_RD_CLK = 0,
    UM_MEM_RD_EN = 1,
    UM_MEM_RD_ADDR = 2,
    UM_MEM_RD_DATA = 3,
    UM_MEM_WR_CLK = 4,
    UM_MEM_WR_ADDR = 6,
    UM_MEM_WR_DATA = 7,
    UM_MEM_PORTS = 8,
    UM_MEM_PARAMS = 13,
    UM_P_SIZE = 1,
    UM_P_ABITS = 3,
    UM_P_WIDTH = 4,
    UM_P_RD_PORTS = 5,
    UM_P_WR_PORTS = 6,
    UM_MUX_A = 0,
    UM_MUX_B = 1,
    UM_MUX_S = 2,
    UM_MUX_Y = 3,
    UM_DFF_CLK = 0,
    UM_DFF_D = 1,
    UM_DFF_Q = 2,
    UM_DFF_P_WIDTH = 0,
    UM_DFF_P_POLARITY = 1,
    UM_MAX_LEVELS = 16,
    UM_WR_EN_PORT = 5,
    UM_AND_A = 0,
    UM_AND_B = 1,
    UM_AND_Y = 2,
    UM_NOT_A = 0,
    UM_NOT_Y = 1
};

/* A live `$and` or `$not` driver of `net` with the given type and 1-bit ports; none otherwise. */
static odin3_node_id driver_of(const um_ctx *uc, odin3_net_id net, odin3_celltype_id type) {
    odin3_pin_id driver = odin3_net_driver(uc->module, net);
    if (!odin3_pin_valid(driver) || type.v == 0) {
        return (odin3_node_id){0};
    }
    odin3_node_id node = odin3_pin_node(uc->module, driver);
    if (!odin3_node_live(uc->module, node) || odin3_node_type(uc->module, node).v != type.v ||
        odin3_node_port(uc->module, node, 0).count != 1) {
        return (odin3_node_id){0};
    }
    return node;
}

/* One literal of the chain: the address net and the path bit it stands for. */
typedef struct um_literal {
    odin3_net_id net;
    uint32_t bit;
    odin3_node_id inverter; /* the `$not` of a path bit 0, none for a bit 1 */
} um_literal;

/* An address net, or the output of a `$not` of one. */
static um_literal literal_of(const um_ctx *uc, odin3_net_id net) {
    odin3_node_id inverter = driver_of(uc, net, uc->types.not_);
    if (inverter.v != 0) {
        return (um_literal){um_port_net(uc->module, inverter, (um_pos){UM_NOT_A, 0}), 0, inverter};
    }
    return (um_literal){net, 1, {0}};
}

static bool add_glue(um_write_tree *tree, odin3_node_id node) {
    odin3_node_id *slot = odin3_vec_push(&tree->glue);
    if (slot == NULL) {
        return false;
    }
    *slot = node;
    return true;
}

/* The chain of one leaf: from the last `$and` up, `levels` literals (the top level first) and the
 * net the first `$and` reads as the write enable. */
typedef struct um_chain_end {
    uint32_t path;
    odin3_net_id we0;
} um_chain_end;

static bool walk_leaf(const um_ctx *uc, odin3_net_id enable, uint32_t levels, um_write_tree *tree,
                      um_chain_end *end) {
    odin3_net_id net = enable;
    end->path = 0;
    for (uint32_t step = 0; step < levels; step++) {
        odin3_node_id gate = driver_of(uc, net, uc->types.and_);
        if (gate.v == 0) {
            return false;
        }
        uint32_t level = levels - 1 - step;
        um_literal lit = literal_of(uc, um_port_net(uc->module, gate, (um_pos){UM_AND_B, 0}));
        odin3_net_id *known = odin3_vec_at(&tree->ad, level);
        if (!odin3_net_valid(lit.net) || (odin3_net_valid(*known) && known->v != lit.net.v)) {
            return false;
        }
        *known = lit.net;
        if (!add_glue(tree, gate) || (lit.inverter.v != 0 && !add_glue(tree, lit.inverter))) {
            return false;
        }
        end->path |= lit.bit << level;
        net = um_port_net(uc->module, gate, (um_pos){UM_AND_A, 0});
    }
    end->we0 = net;
    return true;
}

/* The paths seen are all 2^levels different ones. */
static bool all_distinct(const odin3_vec *paths, uint32_t levels) {
    uint32_t leaves = (uint32_t)1 << levels;
    if (paths->len != leaves) {
        return false;
    }
    odin3_vec seen;
    odin3_vec_init(&seen, sizeof(uint8_t));
    bool ok = odin3_vec_reserve(&seen, leaves) == ODIN3_OK;
    for (uint32_t i = 0; ok && i < leaves; i++) {
        uint8_t *slot = odin3_vec_push(&seen);
        ok = slot != NULL;
        if (ok) {
            *slot = 0;
        }
    }
    const uint32_t *items = paths->data;
    for (uint32_t i = 0; ok && i < leaves; i++) {
        uint8_t *mark = ok && items[i] < leaves ? odin3_vec_at(&seen, items[i]) : NULL;
        ok = mark != NULL && *mark == 0;
        if (mark != NULL) {
            *mark = 1;
        }
    }
    odin3_vec_free(&seen);
    return ok;
}

void um_write_tree_free(um_write_tree *tree) {
    odin3_vec_free(&tree->paths);
    odin3_vec_free(&tree->ad);
    odin3_vec_free(&tree->glue);
}

static bool tree_init(um_write_tree *tree, size_t leaves, uint32_t levels) {
    odin3_vec_init(&tree->paths, sizeof(uint32_t));
    odin3_vec_init(&tree->ad, sizeof(odin3_net_id));
    odin3_vec_init(&tree->glue, sizeof(odin3_node_id));
    tree->we0 = (odin3_net_id){0};
    bool ok = levels >= 1 && leaves == ((size_t)1 << levels);
    for (uint32_t level = 0; ok && level < levels; level++) {
        odin3_net_id *slot = odin3_vec_push(&tree->ad);
        ok = slot != NULL;
        if (ok) {
            *slot = (odin3_net_id){0};
        }
    }
    return ok;
}

/* Every data bit of the leaf has the same write enable (R-8): WR_EN[j] is WR_EN[0]. */
static bool same_enable(const um_ctx *uc, odin3_node_id leaf) {
    odin3_pinslice pins = odin3_node_port(uc->module, leaf, UM_WR_EN_PORT);
    odin3_net_id first = um_port_net(uc->module, leaf, (um_pos){UM_WR_EN_PORT, 0});
    for (uint32_t i = 1; i < pins.count; i++) {
        if (um_port_net(uc->module, leaf, (um_pos){UM_WR_EN_PORT, i}).v != first.v) {
            return false;
        }
    }
    return true;
}

/* One leaf: its chain, the agreement on we0, its path. */
static bool add_leaf(const um_ctx *uc, odin3_net_id enable, uint32_t levels, um_write_tree *tree) {
    um_chain_end end = {0, {0}};
    if (!walk_leaf(uc, enable, levels, tree, &end)) {
        return false;
    }
    if (odin3_net_valid(tree->we0) && tree->we0.v != end.we0.v) {
        return false;
    }
    tree->we0 = end.we0;
    uint32_t *slot = odin3_vec_push(&tree->paths);
    if (slot == NULL) {
        return false;
    }
    *slot = end.path;
    return true;
}

bool um_depth_write_tree(const um_ctx *uc, const odin3_vec *leaves, uint32_t levels,
                         um_write_tree *out) {
    bool ok = tree_init(out, leaves->len, levels);
    const odin3_node_id *nodes = leaves->data;
    for (size_t i = 0; ok && i < leaves->len; i++) {
        odin3_net_id enable = um_port_net(uc->module, nodes[i], (um_pos){UM_WR_EN_PORT, 0});
        ok = same_enable(uc, nodes[i]) && add_leaf(uc, enable, levels, out);
    }
    return ok && all_distinct(&out->paths, levels);
}

/* --- the read side (R-8 step 2) ------------------------------------------------------------- */

typedef struct um_depth {
    um_ctx *uc;
    const odin3_vec *leaves; /* odin3_node_id, node-ID order */
    uint32_t levels;
    uint32_t abits; /* of a leaf */
    uint32_t width; /* WIDTH of a leaf */
    uint32_t reads; /* RD_PORTS of a leaf */
    um_write_tree wtree;
    odin3_vec muxes;   /* odin3_node_id: every `$mux` of the trees */
    odin3_vec glue;    /* odin3_node_id: `$dff`, `$and` and `$not` cells that may be absorbed */
    odin3_vec rd_addr; /* odin3_net_id: reads * levels, the address net of each level per port */
    odin3_vec roots; /* odin3_net_id: reads * width, the root net of each data bit; none: a dummy */
} um_depth;

static bool push_flag(odin3_vec *vec) {
    uint8_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return false;
    }
    *slot = 0;
    return true;
}

static bool push_node(odin3_vec *vec, odin3_node_id node) {
    odin3_node_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return false;
    }
    *slot = node;
    return true;
}

static bool push_net_to(odin3_vec *vec, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return false;
    }
    *slot = net;
    return true;
}

/* The `$mux` whose A input is the only reader of `a_net` and whose B input is the only reader of
 * `b_net`; none when there is no such cell. */
static odin3_node_id find_mux(const um_ctx *uc, odin3_net_id a_net, odin3_net_id b_net) {
    odin3_pinlist sinks = odin3_net_sinks(uc->module, a_net);
    if (sinks.count != 1 || odin3_net_sinks(uc->module, b_net).count != 1) {
        return (odin3_node_id){0};
    }
    odin3_node_id node = odin3_pin_node(uc->module, sinks.pins[0]);
    if (!odin3_node_live(uc->module, node) ||
        odin3_node_type(uc->module, node).v != uc->types.mux.v ||
        odin3_node_port(uc->module, node, UM_MUX_A).count != 1 ||
        odin3_pin_port(uc->module, sinks.pins[0]) != UM_MUX_A ||
        um_port_net(uc->module, node, (um_pos){UM_MUX_B, 0}).v != b_net.v) {
        return (odin3_node_id){0};
    }
    return node;
}

/* A 1-bit rising-edge `$dff` whose Q bit 0 drives the net (R-8: the select register). */
static bool is_select_dff(const um_ctx *uc, odin3_node_id dff, odin3_pin_id driver) {
    return odin3_node_type(uc->module, dff).v == uc->types.dff.v &&
           odin3_pin_port(uc->module, driver) == UM_DFF_Q &&
           odin3_pin_bit(uc->module, driver) == 0 &&
           odin3_node_param(uc->module, dff, UM_DFF_P_WIDTH)->i == 1 &&
           odin3_node_param(uc->module, dff, UM_DFF_P_POLARITY)->i == 1;
}

/* The select of a mux of level `level` for read port `port`: a `$dff` clocked by the read clock;
 * the first one seen sets the port's address net of the level, later ones must agree. */
static bool check_select(um_depth *dep, odin3_node_id mux, um_pos which) {
    const um_ctx *uc = dep->uc;
    odin3_net_id select = um_port_net(uc->module, mux, (um_pos){UM_MUX_S, 0});
    odin3_pin_id driver = odin3_net_driver(uc->module, select);
    odin3_node_id dff =
        odin3_pin_valid(driver) ? odin3_pin_node(uc->module, driver) : (odin3_node_id){0};
    const odin3_node_id *leaves = dep->leaves->data;
    odin3_net_id clock = um_port_net(uc->module, leaves[0], (um_pos){UM_MEM_RD_CLK, which.port});
    if (dff.v == 0 || !is_select_dff(uc, dff, driver) ||
        um_port_net(uc->module, dff, (um_pos){UM_DFF_CLK, 0}).v != clock.v) {
        return false;
    }
    odin3_net_id *known = odin3_vec_at(&dep->rd_addr, which.port * dep->levels + which.index);
    odin3_net_id input = um_port_net(uc->module, dff, (um_pos){UM_DFF_D, 0});
    if (!odin3_net_valid(input) || (odin3_net_valid(*known) && known->v != input.v)) {
        return false;
    }
    *known = input;
    return push_node(&dep->glue, dff);
}

/* One level of the mux tree of one data bit: entries low and low + half of `cur` meet in a mux. */
static bool climb_level(um_depth *dep, odin3_vec *cur, um_pos which) {
    uint32_t half = (uint32_t)1 << which.index;
    for (uint32_t low = 0; low < half; low++) {
        odin3_net_id *left = odin3_vec_at(cur, low);
        odin3_net_id right = *(const odin3_net_id *)odin3_vec_cat(cur, low + half);
        odin3_node_id mux = find_mux(dep->uc, *left, right);
        if (mux.v == 0 || !check_select(dep, mux, which) || !push_node(&dep->muxes, mux)) {
            return false;
        }
        *left = um_port_net(dep->uc->module, mux, (um_pos){UM_MUX_Y, 0});
    }
    return true;
}

/* The tree of data bit `bit` of read port `port`: the leaf outputs in path order, up to the root.
 */
static bool climb_bit(um_depth *dep, uint32_t port, uint32_t bit) {
    odin3_vec cur;
    odin3_vec_init(&cur, sizeof(odin3_net_id));
    const odin3_node_id *leaves = dep->leaves->data;
    const uint32_t *paths = dep->wtree.paths.data;
    bool ok = true;
    for (uint32_t i = 0; ok && i < dep->leaves->len; i++) {
        ok = push_net_to(&cur, (odin3_net_id){0});
    }
    for (uint32_t i = 0; ok && i < dep->leaves->len; i++) {
        um_pos pin = {UM_MEM_RD_DATA, port * dep->width + bit};
        odin3_net_id *slot = odin3_vec_at(&cur, paths[i]);
        *slot = um_port_net(dep->uc->module, leaves[i], pin);
        ok = odin3_net_valid(*slot);
    }
    for (uint32_t level = dep->levels; ok && level > 0; level--) {
        ok = climb_level(dep, &cur, (um_pos){port, level - 1});
    }
    ok = ok && push_net_to(&dep->roots, *(odin3_net_id *)odin3_vec_at(&cur, 0));
    odin3_vec_free(&cur);
    return ok;
}

/* True when read port `port` of the first leaf has no output at all: the unconnected port of a
 * memory nobody reads. */
static bool dummy_read(const um_depth *dep, uint32_t port) {
    const odin3_node_id *leaves = dep->leaves->data;
    for (uint32_t bit = 0; bit < dep->width; bit++) {
        um_pos pin = {UM_MEM_RD_DATA, port * dep->width + bit};
        if (odin3_net_valid(um_port_net(dep->uc->module, leaves[0], pin))) {
            return false;
        }
    }
    return true;
}

static bool read_side(um_depth *dep) {
    bool ok = true;
    for (uint32_t i = 0; ok && i < dep->reads * dep->levels; i++) {
        ok = push_net_to(&dep->rd_addr, (odin3_net_id){0});
    }
    for (uint32_t port = 0; ok && port < dep->reads; port++) {
        bool dummy = dummy_read(dep, port);
        for (uint32_t bit = 0; ok && bit < dep->width; bit++) {
            ok = dummy ? push_net_to(&dep->roots, (odin3_net_id){0}) : climb_bit(dep, port, bit);
        }
    }
    return ok;
}

/* --- absorbing the glue (R-8 step 3) -------------------------------------------------------- */

/* The node is in the group set (a node-ID indexed byte vec). */
static bool in_group(const odin3_vec *marks, odin3_node_id node) {
    return node.v < marks->len && *(const uint8_t *)odin3_vec_cat(marks, node.v) != 0;
}

/* True when every sink of every net the node drives is in the group. */
static bool read_only_inside(const um_ctx *uc, const odin3_vec *marks, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(uc->module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(uc->module, pin);
        if (!odin3_pin_drives(uc->module, pin) || !odin3_net_valid(net)) {
            continue;
        }
        odin3_pinlist sinks = odin3_net_sinks(uc->module, net);
        for (uint32_t sink = 0; sink < sinks.count; sink++) {
            if (!in_group(marks, odin3_pin_node(uc->module, sinks.pins[sink]))) {
                return false;
            }
        }
    }
    return true;
}

static void set_mark(odin3_vec *marks, odin3_node_id node) {
    if (node.v < marks->len) {
        *(uint8_t *)odin3_vec_at(marks, node.v) = 1;
    }
}

/* The set of nodes that join the group: marks by node ID, the group list. */
typedef struct um_absorb {
    const um_ctx *uc;
    odin3_vec marks; /* uint8_t per node ID */
    um_group *group;
} um_absorb;

static odin3_status join_group(um_absorb *ab, odin3_node_id node) {
    if (in_group(&ab->marks, node)) {
        return ODIN3_OK;
    }
    set_mark(&ab->marks, node);
    return um_group_add(ab->group, node);
}

static odin3_status init_marks(um_absorb *ab) {
    uint32_t count = odin3_module_node_end(ab->uc->module);
    odin3_vec_init(&ab->marks, sizeof(uint8_t));
    odin3_status st = odin3_vec_reserve(&ab->marks, count);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = push_flag(&ab->marks) ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

/* Every node of `list` joins unconditionally. */
static odin3_status join_all(um_absorb *ab, const odin3_vec *list) {
    const odin3_node_id *nodes = list->data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < list->len; i++) {
        st = join_group(ab, nodes[i]);
    }
    return st;
}

/* One pass over `list`: each cell only the group reads joins; *changed says one did. */
static odin3_status join_inside(um_absorb *ab, const odin3_vec *list, bool *changed) {
    const odin3_node_id *nodes = list->data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < list->len; i++) {
        if (in_group(&ab->marks, nodes[i]) || !read_only_inside(ab->uc, &ab->marks, nodes[i])) {
            continue;
        }
        st = join_group(ab, nodes[i]);
        *changed = true;
    }
    return st;
}

/* The cells of the group, leaves first: the leaves, every mux, then each `$and`, `$not` and `$dff`
 * whose outputs only the group reads (repeated until nothing joins; a cell somebody else reads
 * stays, its net remains driven). */
static odin3_status absorb(const um_depth *dep, um_group *group) {
    um_absorb ab = {dep->uc, {0}, group};
    odin3_status st = init_marks(&ab);
    st = st == ODIN3_OK ? join_all(&ab, dep->leaves) : st;
    st = st == ODIN3_OK ? join_all(&ab, &dep->muxes) : st;
    bool changed = st == ODIN3_OK;
    while (changed) {
        changed = false;
        st = join_inside(&ab, &dep->glue, &changed);
        st = st == ODIN3_OK ? join_inside(&ab, &dep->wtree.glue, &changed) : st;
        changed = changed && st == ODIN3_OK;
    }
    odin3_vec_free(&ab.marks);
    return st;
}

/* --- the merged memory (R-8 step 4) --------------------------------------------------------- */

typedef struct um_ports {
    odin3_vec port[UM_MEM_PORTS]; /* odin3_net_id per $mem port */
} um_ports;

static void ports_free(um_ports *ports) {
    for (uint32_t i = 0; i < UM_MEM_PORTS; i++) {
        odin3_vec_free(&ports->port[i]);
    }
}

/* Copies `count` nets of port `port` of `leaf`, from pin `first`, to the end of `out`. */
static bool copy_pins(const um_ctx *uc, odin3_node_id leaf, um_pos from, odin3_vec *out,
                      uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (!push_net_to(out, um_port_net(uc->module, leaf, (um_pos){from.port, from.index + i}))) {
            return false;
        }
    }
    return true;
}

/* The read side ports: clock and enable of the leaf, the address (level nets, then the leaf's) and
 * the roots. */
static odin3_status build_read_ports(const um_depth *dep, um_ports *ports) {
    const um_ctx *uc = dep->uc;
    odin3_node_id leaf = *(const odin3_node_id *)dep->leaves->data;
    odin3_net_id gnd = {0};
    odin3_status st = um_const_net(dep->uc, false, &gnd);
    bool ok = st == ODIN3_OK &&
              copy_pins(uc, leaf, (um_pos){UM_MEM_RD_CLK, 0}, &ports->port[0], dep->reads) &&
              copy_pins(uc, leaf, (um_pos){UM_MEM_RD_EN, 0}, &ports->port[1], dep->reads);
    for (uint32_t port = 0; ok && port < dep->reads; port++) {
        bool dummy = dummy_read(dep, port);
        for (uint32_t level = 0; ok && level < dep->levels; level++) {
            odin3_net_id net = dummy ? gnd
                                     : *(const odin3_net_id *)odin3_vec_cat(
                                           &dep->rd_addr, port * dep->levels + level);
            ok = push_net_to(&ports->port[UM_MEM_RD_ADDR], net);
        }
        ok = ok && copy_pins(uc, leaf, (um_pos){UM_MEM_RD_ADDR, port * dep->abits},
                             &ports->port[UM_MEM_RD_ADDR], dep->abits);
    }
    const odin3_net_id *roots = dep->roots.data;
    for (size_t i = 0; ok && i < dep->roots.len; i++) {
        ok = push_net_to(&ports->port[UM_MEM_RD_DATA], roots[i]);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* The write side: clock and data of the leaf, the enable `we0` for every bit, the address (the
 * level nets, then the leaf's). */
static odin3_status build_write_ports(const um_depth *dep, um_ports *ports) {
    const um_ctx *uc = dep->uc;
    odin3_node_id leaf = *(const odin3_node_id *)dep->leaves->data;
    bool ok =
        copy_pins(uc, leaf, (um_pos){UM_MEM_WR_CLK, 0}, &ports->port[UM_MEM_WR_CLK], 1) &&
        copy_pins(uc, leaf, (um_pos){UM_MEM_WR_DATA, 0}, &ports->port[UM_MEM_WR_DATA], dep->width);
    for (uint32_t i = 0; ok && i < dep->width; i++) {
        ok = push_net_to(&ports->port[UM_WR_EN_PORT], dep->wtree.we0);
    }
    const odin3_net_id *ad = dep->wtree.ad.data;
    for (uint32_t i = 0; ok && i < dep->levels; i++) {
        ok = push_net_to(&ports->port[UM_MEM_WR_ADDR], ad[i]);
    }
    ok = ok &&
         copy_pins(uc, leaf, (um_pos){UM_MEM_WR_ADDR, 0}, &ports->port[UM_MEM_WR_ADDR], dep->abits);
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* The `$mem` that replaces the leaves: the leaf's parameters with MEMID, SIZE and ABITS of the
 * whole memory. */
static odin3_status emit_depth(um_depth *dep, const um_group *group) {
    um_ctx *uc = dep->uc;
    odin3_node_id leaf = *(const odin3_node_id *)dep->leaves->data;
    um_ports ports;
    for (uint32_t i = 0; i < UM_MEM_PORTS; i++) {
        odin3_vec_init(&ports.port[i], sizeof(odin3_net_id));
    }
    odin3_status st = build_read_ports(dep, &ports);
    st = st == ODIN3_OK ? build_write_ports(dep, &ports) : st;
    odin3_value params[UM_MEM_PARAMS];
    for (uint32_t i = 0; i < UM_MEM_PARAMS; i++) {
        params[i] = *odin3_node_param(uc->module, leaf, i);
    }
    uint32_t total = dep->abits + dep->levels;
    params[UM_P_SIZE] = odin3_value_int((int64_t)1 << total);
    params[UM_P_ABITS] = odin3_value_int(total);
    st = st == ODIN3_OK ? um_ram_memid(uc, leaf, &params[0]) : st;
    odin3_netvec nets[UM_MEM_PORTS];
    static const odin3_net_id k_none = {0};
    for (uint32_t i = 0; i < UM_MEM_PORTS; i++) {
        bool empty = ports.port[i].len == 0;
        nets[i] = (odin3_netvec){empty ? &k_none : ports.port[i].data, (uint32_t)ports.port[i].len};
    }
    int64_t origin = 0;
    (void)um_attr_int(uc, leaf, "unmap_origin", &origin);
    um_new req = {uc->types.mem, params, UM_MEM_PARAMS, nets, leaf, "ram.depth", origin, 0};
    odin3_node_id made = {0};
    st = st == ODIN3_OK ? um_replace(uc, group, &req, &made) : st;
    ports_free(&ports);
    return st;
}

/* --- the sets and the phase ---------------------------------------------------------------- */

static int64_t param_int(const um_ctx *uc, odin3_node_id node, uint32_t index) {
    return odin3_node_param(uc->module, node, index)->i;
}

static void depth_free(um_depth *dep) {
    um_write_tree_free(&dep->wtree);
    odin3_vec_free(&dep->muxes);
    odin3_vec_free(&dep->glue);
    odin3_vec_free(&dep->rd_addr);
    odin3_vec_free(&dep->roots);
}

/* A mismatch of the structure: the leaves stay (each exact), with the note ram:depth. */
static odin3_status refuse_set(um_ctx *uc, const odin3_vec *leaves) {
    return um_decline(uc, leaves->data, leaves->len, (um_why){"ram", "depth"});
}

/* R-8 for one set of 2^levels leaves. */
static odin3_status merge_set(um_ctx *uc, const odin3_vec *leaves, uint32_t levels) {
    const odin3_node_id *nodes = leaves->data;
    um_depth dep;
    memset(&dep, 0, sizeof dep);
    dep.uc = uc;
    dep.leaves = leaves;
    dep.levels = levels;
    dep.abits = (uint32_t)param_int(uc, nodes[0], UM_P_ABITS);
    dep.width = (uint32_t)param_int(uc, nodes[0], UM_P_WIDTH);
    dep.reads = (uint32_t)param_int(uc, nodes[0], UM_P_RD_PORTS);
    odin3_vec_init(&dep.muxes, sizeof(odin3_node_id));
    odin3_vec_init(&dep.glue, sizeof(odin3_node_id));
    odin3_vec_init(&dep.rd_addr, sizeof(odin3_net_id));
    odin3_vec_init(&dep.roots, sizeof(odin3_net_id));
    odin3_status st = ODIN3_OK;
    bool matched = um_depth_write_tree(uc, leaves, levels, &dep.wtree) && read_side(&dep);
    if (!matched) {
        st = refuse_set(uc, leaves);
    } else {
        um_group group;
        um_group_init(&group);
        st = absorb(&dep, &group);
        st = st == ODIN3_OK ? emit_depth(&dep, &group) : st;
        um_group_free(&group);
        if (st == ODIN3_OK) {
            uc->stats.mem -= uc->stats.mem >= leaves->len - 1 ? leaves->len - 1 : uc->stats.mem;
        }
    }
    depth_free(&dep);
    return st;
}

/* The leaves share everything but the write enable and the read data: parameters (but the name)
 * and the other ports' nets. */
static bool same_leaf(const um_ctx *uc, odin3_node_id left, odin3_node_id right) {
    for (uint32_t i = 1; i < UM_MEM_PARAMS; i++) {
        if (!odin3_value_equal(odin3_node_param(uc->module, left, i),
                               odin3_node_param(uc->module, right, i))) {
            return false;
        }
    }
    static const uint32_t k_same[] = {UM_MEM_RD_CLK, UM_MEM_RD_EN,   UM_MEM_RD_ADDR,
                                      UM_MEM_WR_CLK, UM_MEM_WR_ADDR, UM_MEM_WR_DATA};
    for (size_t port = 0; port < sizeof k_same / sizeof k_same[0]; port++) {
        odin3_pinslice lp = odin3_node_port(uc->module, left, k_same[port]);
        if (lp.count != odin3_node_port(uc->module, right, k_same[port]).count) {
            return false;
        }
        for (uint32_t i = 0; i < lp.count; i++) {
            if (um_port_net(uc->module, left, (um_pos){k_same[port], i}).v !=
                um_port_net(uc->module, right, (um_pos){k_same[port], i}).v) {
                return false;
            }
        }
    }
    return true;
}

/* A half-open range of positions. */
typedef struct um_range_t {
    size_t first;
    size_t last;
} um_range_t;

/* A `$mem` phase D made, with the origin record it carries. */
typedef struct um_cand {
    int64_t origin;
    odin3_node_id node;
} um_cand;

static int cmp_cand(const void *lhs, const void *rhs) {
    const um_cand *left = lhs;
    const um_cand *right = rhs;
    if (left->origin != right->origin) {
        return left->origin < right->origin ? -1 : 1;
    }
    if (left->node.v == right->node.v) {
        return 0;
    }
    return left->node.v < right->node.v ? -1 : 1;
}

static bool is_made_mem(const um_ctx *uc, odin3_node_id node) {
    const char *rule = um_attr_text(uc, node, "unmap");
    return odin3_node_live(uc->module, node) &&
           odin3_node_type(uc->module, node).v == uc->types.mem.v && rule != NULL &&
           strncmp(rule, "ram", 3) == 0 && strcmp(rule, "ram.depth") != 0;
}

/* The candidates, sorted by origin and node ID. */
static odin3_status collect_mems(const um_ctx *uc, odin3_vec *cands) {
    odin3_vec_init(cands, sizeof(um_cand));
    for (uint32_t nid = 1; nid < odin3_module_node_end(uc->module); nid++) {
        odin3_node_id node = {nid};
        int64_t origin = 0;
        if (!is_made_mem(uc, node) || !um_attr_int(uc, node, "unmap_origin", &origin) ||
            origin == 0) {
            continue;
        }
        um_cand *slot = odin3_vec_push(cands);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (um_cand){origin, node};
    }
    if (cands->len > 1) {
        qsort(cands->data, cands->len, sizeof(um_cand), cmp_cand);
    }
    return ODIN3_OK;
}

/* log2 of a power of two of at least 2, else 0. */
static uint32_t levels_of(size_t count) {
    uint32_t levels = 0;
    while (((size_t)1 << levels) < count && levels < UM_MAX_LEVELS) {
        levels++;
    }
    return levels > 0 && ((size_t)1 << levels) == count ? levels : 0;
}

/* One set of leaves (equal to the first): a candidate for the merge when 2^k of them. */
static odin3_status try_set(um_ctx *uc, const odin3_vec *leaves) {
    const odin3_node_id *nodes = leaves->data;
    uint32_t levels = levels_of(leaves->len);
    if (levels == 0 || param_int(uc, nodes[0], UM_P_WR_PORTS) != 1) {
        return ODIN3_OK;
    }
    /* word-level glue only: leaves whose enable is not an `$and` (after lower) are left alone */
    odin3_net_id enable = um_port_net(uc->module, nodes[0], (um_pos){UM_WR_EN_PORT, 0});
    if (driver_of(uc, enable, uc->types.and_).v == 0) {
        return ODIN3_OK;
    }
    return merge_set(uc, leaves, levels);
}

/* One origin run: the candidates, and which of them are already in a set. */
typedef struct um_runset {
    const um_ctx *uc;
    const um_cand *base;
    size_t count;
    odin3_vec used; /* uint8_t per candidate */
} um_runset;

/* The leaves equal to candidate `first`: it and every later unused one that is the same; the used
 * flags are set. */
static odin3_status gather_set(um_runset *rs, size_t first, odin3_vec *leaves) {
    odin3_vec_init(leaves, sizeof(odin3_node_id));
    odin3_status st = push_node(leaves, rs->base[first].node) ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t other = first + 1; st == ODIN3_OK && other < rs->count; other++) {
        uint8_t *flag = odin3_vec_at(&rs->used, other);
        if (*flag == 0 && same_leaf(rs->uc, rs->base[first].node, rs->base[other].node)) {
            *flag = 1;
            st = push_node(leaves, rs->base[other].node) ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        }
    }
    return st;
}

/* The sets of one origin run: each leaf joins the first set it equals. */
static odin3_status run_sets(um_ctx *uc, const um_cand *cands, um_range_t run) {
    um_runset rs = {uc, cands + run.first, run.last - run.first, {0}};
    odin3_vec_init(&rs.used, sizeof(uint8_t));
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rs.count; i++) {
        st = push_flag(&rs.used) ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; st == ODIN3_OK && i < rs.count; i++) {
        if (*(const uint8_t *)odin3_vec_cat(&rs.used, i) != 0) {
            continue;
        }
        odin3_vec leaves;
        st = gather_set(&rs, i, &leaves);
        st = st == ODIN3_OK ? try_set(uc, &leaves) : st;
        odin3_vec_free(&leaves);
    }
    odin3_vec_free(&rs.used);
    return st;
}

odin3_status um_ram_depth_phase(um_ctx *uc) {
    odin3_vec cands;
    odin3_status st = collect_mems(uc, &cands);
    const um_cand *items = cands.data;
    size_t from = 0;
    while (st == ODIN3_OK && from < cands.len) {
        size_t to = from + 1;
        while (to < cands.len && items[to].origin == items[from].origin) {
            to++;
        }
        st = run_sets(uc, items, (um_range_t){from, to});
        from = to;
    }
    odin3_vec_free(&cands);
    return st;
}
