/*
 * ram_depth.c — depth recovery (R-8). The write side (step 1) here; the read side and the merge
 * follow in the same file.
 */
#include "passes/unmap/ram_depth.h"

#include "ir/module.h"
#include "passes/unmap/kit.h"

#include <string.h>

enum { UM_WR_EN_PORT = 5, UM_AND_A = 0, UM_AND_B = 1, UM_AND_Y = 2, UM_NOT_A = 0, UM_NOT_Y = 1 };

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
} um_literal;

/* An address net, or the output of a `$not` of one. */
static um_literal literal_of(const um_ctx *uc, odin3_net_id net) {
    odin3_node_id inverter = driver_of(uc, net, uc->types.not_);
    if (inverter.v != 0) {
        return (um_literal){um_port_net(uc->module, inverter, (um_pos){UM_NOT_A, 0}), 0};
    }
    return (um_literal){net, 1};
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
}

static bool tree_init(um_write_tree *tree, size_t leaves, uint32_t levels) {
    odin3_vec_init(&tree->paths, sizeof(uint32_t));
    odin3_vec_init(&tree->ad, sizeof(odin3_net_id));
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
        ok = add_leaf(uc, enable, levels, out);
    }
    return ok && all_distinct(&out->paths, levels);
}

odin3_status um_ram_depth_phase(um_ctx *uc) {
    (void)uc;
    return ODIN3_OK;
}
