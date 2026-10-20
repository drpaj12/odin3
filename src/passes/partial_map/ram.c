/*
 * ram.c — explicit `single_port_ram` / `dual_port_ram` instances legalised to the arch's shape
 * (3C spec §6.1, R-1..R-7, H-4, H-7): depth split first (R-2, ram_split.c), then one slice per
 * data bit (R-3), then address padding (R-4), as Parmys's iterate_memories does. Every slice is a
 * RAM of ADDR_WIDTH D (rules.mem_depth) and DATA_WIDTH 1. A `we` on a constant stays wired to it
 * (R-5); two live write ports get no priority logic (R-6). An instance that already has the arch's
 * shape is left alone. Each instance is one transaction (H-9): every node is built on fresh nets,
 * then the instance is deleted and its output nets are merged into the new ones.
 */
#include "passes/partial_map/ram.h"

#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { RAM_PARAMS = 2, ROLE_SIZE = 96 };

static const ram_layout k_sp_layout = {HM_SP_RAM, 1,      5,      0,       {1, 0},
                                       {2, 0},    {3, 0}, {4, 0}, {"", ""}};
static const ram_layout k_dp_layout = {HM_DP_RAM, 2,      9,      0,         {2, 1},
                                       {4, 3},    {6, 5}, {8, 7}, {"1", "2"}};

const ram_layout *ram_layout_of(hm_hard kind) {
    return kind == HM_SP_RAM ? &k_sp_layout : &k_dp_layout;
}

/* The layout of the RAM type of node, NULL when it is neither. */
static const ram_layout *layout_of(const hm_ctx *hc, odin3_node_id node) {
    const odin3_celltype_def *def =
        odin3_celltype_get(hc->design, odin3_node_type(hc->module, node));
    if (def == NULL) {
        return NULL;
    }
    if (strcmp(def->name, "single_port_ram") == 0) {
        return &k_sp_layout;
    }
    return strcmp(def->name, "dual_port_ram") == 0 ? &k_dp_layout : NULL;
}

static uint32_t pin_count(const hm_ctx *hc, odin3_node_id node, uint32_t port) {
    return odin3_node_port(hc->module, node, port).count;
}

/* The refusal of an instance with more address bits than hardmap splits (a located error). */
static odin3_status check_addr(const hm_ctx *hc, odin3_node_id node, const ram_layout *lay) {
    uint32_t addr = pin_count(hc, node, lay->addr[0]);
    if (addr <= PM_RAM_MAX_ADDR) {
        return ODIN3_OK;
    }
    char label[ODIN3_LOG_BUF / 4];
    hm_label(hc, node, label, sizeof label);
    hm_diag(hc, ODIN3_LOG_ERROR, node,
            "partial_map: RAM instance '%s' has ADDR_WIDTH %" PRIu32
            "; partial_map splits at most %u",
            label, addr, PM_RAM_MAX_ADDR);
    return ODIN3_ERR_INVALID_ARG;
}

odin3_status hm_check_ram(hm_ctx *hc, odin3_node_id node) {
    const ram_layout *lay = layout_of(hc, node);
    if (lay == NULL) {
        return ODIN3_OK;
    }
    odin3_celltype_id type = {0};
    odin3_status st = hm_need(hc, lay->kind, node, &type);
    return st == ODIN3_OK ? check_addr(hc, node, lay) : st;
}

/* --- slices -------------------------------------------------------------------------------- */

/* R-2: the depth-split levels of a RAM of addr_bits address bits, D = rules.mem_depth. */
static uint32_t levels_of(const hm_ctx *hc, bool dual, uint32_t addr_bits) {
    uint32_t depth = pm_ram_depth(&hc->rules, dual);
    return addr_bits > depth ? addr_bits - depth : 0;
}

/* The block shape of a RAM of `kind` for addr_bits (R-4, R-5); an instance with no mode that fits
 * keeps the default shape. */
static pm_ram_shape shape_of(const hm_ctx *hc, hm_hard kind, uint32_t addr_bits) {
    pm_ram_shape shape = pm_ram_shape_of(&hc->rules, kind == HM_DP_RAM, addr_bits);
    if (shape.width == 0) {
        shape = (pm_ram_shape){pm_ram_depth(&hc->rules, kind == HM_DP_RAM), hc->rules.mem_width};
    }
    return shape;
}

static void ctx_init(ram_ctx *rc, hm_ctx *hc, odin3_node_id src, odin3_vec *ties) {
    memset(rc, 0, sizeof *rc);
    for (uint32_t rwp = 0; rwp < RAM_MAX_PORTS; rwp++) {
        odin3_vec_init(&rc->port[rwp].we, sizeof(odin3_net_id));
        odin3_vec_init(&rc->port[rwp].out, sizeof(odin3_net_id));
        odin3_vec_init(&rc->port[rwp].chunk, sizeof(odin3_net_id));
        rc->read[rwp] = true;
    }
    rc->hc = hc;
    rc->src = src;
    rc->lay = layout_of(hc, src);
    rc->ties = ties;
    if (rc->lay == NULL) {
        return;
    }
    rc->addr_bits = pin_count(hc, src, rc->lay->addr[0]);
    rc->data_bits = pin_count(hc, src, rc->lay->data[0]);
    rc->levels = levels_of(hc, rc->lay->kind == HM_DP_RAM, rc->addr_bits);
    rc->shape = shape_of(hc, rc->lay->kind, rc->addr_bits);
}

/* The context of a described RAM (no instance behind it). */
static void ctx_init_desc(ram_ctx *rc, hm_ctx *hc, const hm_ram_desc *desc, odin3_vec *ties) {
    ctx_init(rc, hc, hc->src, ties);
    rc->desc = desc;
    rc->lay = ram_layout_of(desc->kind);
    rc->addr_bits = desc->addr_bits;
    rc->data_bits = desc->data_bits;
    rc->levels = levels_of(hc, desc->kind == HM_DP_RAM, rc->addr_bits);
    rc->shape = shape_of(hc, desc->kind, rc->addr_bits);
    for (uint32_t rwp = 0; rwp < RAM_MAX_PORTS; rwp++) {
        rc->read[rwp] = desc->read[rwp];
    }
}

static void ctx_free(ram_ctx *rc) {
    for (uint32_t rwp = 0; rwp < RAM_MAX_PORTS; rwp++) {
        odin3_vec_free(&rc->port[rwp].we);
        odin3_vec_free(&rc->port[rwp].out);
        odin3_vec_free(&rc->port[rwp].chunk);
    }
}

/* R-5: the address pins of every slice of port rwp: addr[k .. A-1], then the pad up to the
 * block's ADDR_WIDTH (D by default). */
static odin3_status setup_addr(ram_ctx *rc, uint32_t rwp) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < rc->shape.addr; i++) {
        uint32_t bit = rc->levels + i;
        if (bit < rc->addr_bits) {
            rc->port[rwp].addr[i] = ram_src_net(rc, (ram_pin){rc->lay->addr[rwp], bit});
        } else {
            st = hm_const(rc->hc, HM_PAD, &rc->port[rwp].addr[i]);
        }
    }
    return st;
}

/* R-3: the RAM of the chunk `ck` of data bits in leaf `leaf`; its n outputs are appended to the
 * chunk list of each read port. */
static odin3_status make_slice(ram_ctx *rc, uint32_t leaf, ram_chunk ck) {
    const ram_layout *lay = rc->lay;
    odin3_net_id clk = ram_src_net(rc, (ram_pin){lay->clk, 0});
    odin3_net_id data[RAM_MAX_PORTS][PM_RAM_WIDTH_CAP];
    odin3_net_id fresh[RAM_MAX_PORTS][PM_RAM_WIDTH_CAP];
    odin3_netvec ports[RAM_MAX_PINS];
    memset(fresh, 0, sizeof fresh); /* a port nobody reads keeps its output open */
    odin3_net_id pad = {0};
    odin3_status st = rc->shape.width > ck.n ? hm_const(rc->hc, HM_PAD, &pad) : ODIN3_OK;
    for (uint32_t rwp = 0; st == ODIN3_OK && rwp < lay->n_ports; rwp++) {
        st = rc->read[rwp] ? hm_fresh_nets(rc->hc, ck.n, fresh[rwp]) : st;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    ports[lay->clk] = (odin3_netvec){&clk, 1};
    for (uint32_t rwp = 0; rwp < lay->n_ports; rwp++) {
        for (uint32_t j = 0; j < rc->shape.width; j++) {
            data[rwp][j] =
                j < ck.n ? ram_src_net(rc, (ram_pin){lay->data[rwp], ck.first + j}) : pad;
        }
        ports[lay->data[rwp]] = (odin3_netvec){data[rwp], rc->shape.width};
        ports[lay->addr[rwp]] = (odin3_netvec){rc->port[rwp].addr, rc->shape.addr};
        ports[lay->we[rwp]] = (odin3_netvec){odin3_vec_cat(&rc->port[rwp].we, leaf), 1};
        ports[lay->out[rwp]] = (odin3_netvec){fresh[rwp], rc->shape.width};
    }
    char role[ROLE_SIZE];
    char path[RAM_MAX_LEVELS + 1];
    ram_path(path, sizeof path, (ram_choices){leaf, rc->levels});
    (void)snprintf(role, sizeof role, "b%" PRIu32 "%s%s", ck.first, rc->levels > 0 ? "." : "",
                   path);
    const odin3_value params[RAM_PARAMS] = {odin3_value_int(rc->shape.addr),
                                            odin3_value_int(rc->shape.width)};
    odin3_node_id node = {0};
    st = hm_new_node(rc->hc, &(hm_node_req){rc->type, params, RAM_PARAMS, ports, role}, &node);
    for (uint32_t rwp = 0; st == ODIN3_OK && rwp < lay->n_ports; rwp++) {
        for (uint32_t j = 0; rc->read[rwp] && j < ck.n; j++) {
            *(odin3_net_id *)odin3_vec_push(&rc->port[rwp].chunk) = fresh[rwp][j]; /* reserved */
        }
    }
    return st;
}

/* The output tree of data bit ck.first + idx of read/write port rwp, and its tie. */
static odin3_status tie_bit(ram_ctx *rc, uint32_t rwp, ram_chunk ck, uint32_t idx) {
    ram_port *rp = &rc->port[rwp];
    odin3_vec_clear(&rp->out);
    for (uint32_t leaf = 0; leaf < ((uint32_t)1 << rc->levels); leaf++) {
        const odin3_net_id *from = odin3_vec_cat(&rp->chunk, (size_t)leaf * ck.n + idx);
        *(odin3_net_id *)odin3_vec_push(&rp->out) = *from; /* reserved by ram_split_glue */
    }
    odin3_net_id root = {0};
    odin3_status st = ram_mux_tree(rc, (ram_at){rwp, ck.first + idx, 0, 0}, &root);
    if (st == ODIN3_OK) {
        hm_tie *tie = odin3_vec_push(rc->ties); /* reserved by build_body: never NULL */
        *tie = (hm_tie){ram_src_net(rc, (ram_pin){rc->lay->out[rwp], ck.first + idx}), root};
    }
    return st;
}

/* All the RAMs of the chunk (every leaf), then each read port's output trees and ties. */
static odin3_status build_chunk(ram_ctx *rc, ram_chunk ck) {
    const size_t leaves = (size_t)1 << rc->levels;
    odin3_status st = ODIN3_OK;
    for (uint32_t rwp = 0; st == ODIN3_OK && rwp < rc->lay->n_ports; rwp++) {
        odin3_vec_clear(&rc->port[rwp].chunk);
        st = odin3_vec_reserve(&rc->port[rwp].chunk, leaves * ck.n);
    }
    for (uint32_t leaf = 0; st == ODIN3_OK && leaf < leaves; leaf++) {
        st = make_slice(rc, leaf, ck);
    }
    for (uint32_t rwp = 0; st == ODIN3_OK && rwp < rc->lay->n_ports; rwp++) {
        for (uint32_t j = 0; rc->read[rwp] && st == ODIN3_OK && j < ck.n; j++) {
            st = tie_bit(rc, rwp, ck, j);
        }
    }
    return st;
}

/* The slices of everything in rc on fresh nets, and the ties; nothing of the source changes. */
static odin3_status build_body(ram_ctx *rc) {
    odin3_status st = hm_need(rc->hc, rc->lay->kind, rc->src, &rc->type);
    st = st == ODIN3_OK
             ? odin3_vec_reserve(rc->ties, rc->ties->len + (size_t)rc->data_bits * rc->lay->n_ports)
             : st;
    for (uint32_t rwp = 0; st == ODIN3_OK && rwp < rc->lay->n_ports; rwp++) {
        st = setup_addr(rc, rwp);
        st = st == ODIN3_OK ? ram_split_glue(rc, rwp) : st;
    }
    for (uint32_t first = 0; st == ODIN3_OK && first < rc->data_bits; first += rc->shape.width) {
        uint32_t left = rc->data_bits - first;
        st = build_chunk(rc, (ram_chunk){first, left < rc->shape.width ? left : rc->shape.width});
    }
    return st;
}

/* S-5: one WARNING per design when `--mem-width arch` gave a block other than the default one. */
static void warn_width(ram_ctx *rc) {
    hm_ctx *hc = rc->hc;
    if (hc->rules.mem_arch && !hc->width_warned &&
        (rc->shape.addr != pm_ram_depth(&hc->rules, rc->lay->kind == HM_DP_RAM) ||
         rc->shape.width != hc->rules.mem_width)) {
        hc->width_warned = true;
        odin3_log(ODIN3_LOG_WARN,
                  "partial_map: --mem-width arch: memories are sliced to the arch's widest mode, "
                  "not 1 bit as Parmys does");
    }
}

/* The number of RAMs rc builds: 2^levels leaves per chunk of data bits. */
static uint64_t slice_count(const ram_ctx *rc) {
    uint64_t chunks = ((uint64_t)rc->data_bits + rc->shape.width - 1U) / rc->shape.width;
    return ((uint64_t)1 << rc->levels) * chunks;
}

odin3_status hm_build_ram(hm_ctx *hc, const hm_ram_desc *desc, odin3_vec *ties, uint64_t *slices) {
    ram_ctx rc;
    ctx_init_desc(&rc, hc, desc, ties);
    odin3_status st = build_body(&rc);
    if (st == ODIN3_OK) {
        warn_width(&rc);
    }
    *slices += st == ODIN3_OK ? slice_count(&rc) : 0;
    ctx_free(&rc);
    return st;
}

/* Builds everything on fresh nets and the ties; nothing of the source changes. */
static odin3_status build(ram_ctx *rc) {
    odin3_status st = hm_begin_cell(rc->hc, rc->src);
    return st == ODIN3_OK ? build_body(rc) : st;
}

/* One transaction for src: check, build on fresh nets, then commit; the undo on failure. */
static odin3_status transact(ram_ctx *rc) {
    hm_ctx *hc = rc->hc;
    odin3_status st = check_addr(hc, rc->src, rc->lay);
    st = st == ODIN3_OK ? build(rc) : st;
    if (st != ODIN3_OK) {
        hm_abort(hc);
        return st;
    }
    st = hm_commit(hc, odin3_vec_cat(rc->ties, 0), rc->ties->len);
    if (st == ODIN3_OK) {
        warn_width(rc);
        uint64_t slices = slice_count(rc);
        uint64_t *count = rc->lay->kind == HM_SP_RAM ? &hc->stats.sp_slices : &hc->stats.dp_slices;
        *count += slices;
    }
    return st;
}

/* Not a RAM, a degenerate one (no data bit), or one that already has the arch's shape: left. */
static bool needs_mapping(const ram_ctx *rc) {
    return rc->lay != NULL && rc->data_bits > 0 &&
           (rc->levels > 0 || rc->addr_bits != rc->shape.addr || rc->data_bits != rc->shape.width);
}

odin3_status hm_map_ram(hm_ctx *hc, odin3_node_id src) {
    odin3_vec ties;
    odin3_vec_init(&ties, sizeof(hm_tie));
    ram_ctx rc;
    pm_memattr_warn(hc, src, "partial_map");
    ctx_init(&rc, hc, src, &ties);
    odin3_status st = needs_mapping(&rc) ? transact(&rc) : ODIN3_OK;
    ctx_free(&rc);
    odin3_vec_free(&ties);
    return st;
}
