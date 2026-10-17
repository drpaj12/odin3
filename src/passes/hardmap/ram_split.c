/*
 * ram_split.c — the glue of the depth split (3C spec R-2, R-5, R-7): per read/write port the
 * inverted address bits, the registered selects (one 1-bit `$dff` per level and port, D4), the
 * write enable of every leaf (a tree of `$and`) and the output `$mux` tree of a data bit. All of
 * it is a loop over levels; nothing recurses.
 */
#include "passes/hardmap/ram.h"

#include <inttypes.h>
#include <stdio.h>

enum { ROLE_SIZE = 96, HEAD_SIZE = 48, PATH_SIZE = RAM_MAX_LEVELS + 1 };

void ram_path(char *out, size_t size, ram_choices choices) {
    size_t used = 0;
    for (uint32_t i = 0; i < choices.count && used + 1 < size; i++) {
        out[used++] = ((choices.leaf >> i) & 1U) != 0 ? 's' : 'h';
    }
    out[used] = '\0';
}

odin3_net_id ram_src_net(const ram_ctx *rc, ram_pin pin) {
    const odin3_module *mod = rc->hc->module;
    odin3_pinslice pins = odin3_node_port(mod, rc->src, pin.port);
    if (pin.bit >= pins.count) {
        return (odin3_net_id){0};
    }
    return odin3_pin_net(mod, (odin3_pin_id){pins.first.v + pin.bit});
}

static odin3_net_id net_at(const odin3_vec *vec, size_t idx) {
    return *(const odin3_net_id *)odin3_vec_cat(vec, idx);
}

/* "<head>.<path>", no dot for an empty path. */
static void path_role(char *role, size_t size, const char *head, ram_choices choices) {
    char path[PATH_SIZE];
    ram_path(path, sizeof path, choices);
    (void)snprintf(role, size, "%s%s%s", head, choices.count > 0 ? "." : "", path);
}

/* Per level: ~addr[L] and the registered select dff(clk, addr[L]) (R-7). */
static odin3_status build_select(ram_ctx *rc, uint32_t rwp) {
    ram_port *rp = &rc->port[rwp];
    const char *tag = rc->lay->tag[rwp];
    odin3_net_id clk = ram_src_net(rc, (ram_pin){rc->lay->clk, 0});
    odin3_status st = ODIN3_OK;
    for (uint32_t lvl = 0; st == ODIN3_OK && lvl < rc->levels; lvl++) {
        char role[ROLE_SIZE];
        const odin3_net_id bit = ram_src_net(rc, (ram_pin){rc->lay->addr[rwp], lvl});
        const odin3_net_id dff_in[2] = {clk, bit};
        (void)snprintf(role, sizeof role, "sel%s.l%" PRIu32, tag, lvl);
        st = hm_dff(rc->hc, role, dff_in, &rp->sel[lvl]);
        (void)snprintf(role, sizeof role, "nad%s.l%" PRIu32, tag, lvl);
        st = st == ODIN3_OK ? hm_not(rc->hc, role, bit, &rp->inv[lvl]) : st;
    }
    return st;
}

/* One `$and` of the write-enable tree: at.leaf is the prefix of at.level + 1 choices; its output
 * replaces entry leaf of the port's we (low half) or is appended (high half). */
static odin3_status and_node(ram_ctx *rc, ram_at at, const odin3_net_id in[2]) {
    char head[HEAD_SIZE];
    char role[ROLE_SIZE];
    odin3_net_id out = {0};
    (void)snprintf(head, sizeof head, "we%s", rc->lay->tag[at.rwp]);
    path_role(role, sizeof role, head, (ram_choices){at.leaf, at.level + 1});
    odin3_status st = hm_and(rc->hc, role, in, &out);
    if (st == ODIN3_OK) {
        ram_port *rp = &rc->port[at.rwp];
        if (at.leaf < ((uint32_t)1 << at.level)) {
            *(odin3_net_id *)odin3_vec_at(&rp->we, at.leaf) = out;
        } else {
            *(odin3_net_id *)odin3_vec_push(&rp->we) = out; /* reserved: never NULL */
        }
    }
    return st;
}

/* Level lvl doubles the write enables: entry v becomes v (address bit 0) and v + 2^lvl (bit 1). */
static odin3_status extend_we(ram_ctx *rc, uint32_t rwp, uint32_t lvl) {
    ram_port *rp = &rc->port[rwp];
    uint32_t half = (uint32_t)1 << lvl;
    odin3_net_id addr = ram_src_net(rc, (ram_pin){rc->lay->addr[rwp], lvl});
    odin3_status st = ODIN3_OK;
    for (uint32_t low = 0; st == ODIN3_OK && low < half; low++) {
        odin3_net_id prev = net_at(&rp->we, low);
        const odin3_net_id lo[2] = {prev, rp->inv[lvl]};
        const odin3_net_id hi[2] = {prev, addr};
        /* the high half goes first: the low one overwrites entry `low`, which `prev` came from */
        st = and_node(rc, (ram_at){rwp, 0, lvl, low + half}, hi);
        st = st == ODIN3_OK ? and_node(rc, (ram_at){rwp, 0, lvl, low}, lo) : st;
    }
    return st;
}

odin3_status ram_split_glue(ram_ctx *rc, uint32_t rwp) {
    ram_port *rp = &rc->port[rwp];
    size_t leaves = (size_t)1 << rc->levels;
    odin3_status st = odin3_vec_reserve(&rp->we, leaves);
    st = st == ODIN3_OK ? odin3_vec_reserve(&rp->out, leaves) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    *(odin3_net_id *)odin3_vec_push(&rp->we) = ram_src_net(rc, (ram_pin){rc->lay->we[rwp], 0});
    st = build_select(rc, rwp);
    for (uint32_t lvl = 0; st == ODIN3_OK && lvl < rc->levels; lvl++) {
        st = extend_we(rc, rwp, lvl);
    }
    return st;
}

/* Level at.level of the output tree: entry v is the mux of v (select 0) and v + 2^level. */
static odin3_status mux_level(ram_ctx *rc, ram_at at) {
    ram_port *rp = &rc->port[at.rwp];
    uint32_t half = (uint32_t)1 << at.level;
    odin3_status st = ODIN3_OK;
    for (uint32_t low = 0; st == ODIN3_OK && low < half; low++) {
        char head[HEAD_SIZE];
        char role[ROLE_SIZE];
        const odin3_net_id in[2] = {net_at(&rp->out, low), net_at(&rp->out, low + half)};
        odin3_net_id out = {0};
        (void)snprintf(head, sizeof head, "mux%s.b%" PRIu32, rc->lay->tag[at.rwp], at.bit);
        path_role(role, sizeof role, head, (ram_choices){low, at.level});
        st = hm_mux(rc->hc, role, rp->sel[at.level], in, &out);
        if (st == ODIN3_OK) {
            *(odin3_net_id *)odin3_vec_at(&rp->out, low) = out;
        }
    }
    return st;
}

odin3_status ram_mux_tree(ram_ctx *rc, ram_at at, odin3_net_id *root) {
    odin3_status st = ODIN3_OK;
    for (uint32_t lvl = rc->levels; st == ODIN3_OK && lvl > 0; lvl--) {
        at.level = lvl - 1;
        st = mux_level(rc, at);
    }
    if (st == ODIN3_OK) {
        *root = net_at(&rc->port[at.rwp].out, 0);
    }
    return st;
}
