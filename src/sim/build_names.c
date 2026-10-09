/* build_names.c — located, hierarchically named error messages of odin3_sim_build. */
#include "build_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Room for a location prefix, one name segment and a whole hierarchical name. */
enum { LOC_MAX = 256, SEG_MAX = 192, NAME_MAX = 512 };

static const char k_ellipsis[] = "...";

/* A name built right to left, from the leaf up the hierarchy: text + pos is the string. */
typedef struct name_buf {
    char text[NAME_MAX];
    size_t pos;
    bool full;
} name_buf;

static void name_init(name_buf *nb) {
    nb->pos = NAME_MAX - 1;
    nb->text[nb->pos] = '\0';
    nb->full = false;
}

static const char *name_str(const name_buf *nb) {
    return nb->text + nb->pos;
}

/* Prepends str; once it does not fit, prepends "..." instead and ignores everything after. */
static void prepend(name_buf *nb, const char *str) {
    size_t len = strlen(str);
    if (nb->full) {
        return;
    }
    if (len + sizeof k_ellipsis > nb->pos) {
        nb->full = true;
        str = k_ellipsis;
        len = sizeof k_ellipsis - 1;
    }
    nb->pos -= len;
    memcpy(nb->text + nb->pos, str, len);
}

static const char *strtab(const odin3_sim_builder *bld, uint32_t str) {
    const char *text = odin3_strtab_get(odin3_design_strtab(bld->design), str);
    return text != NULL ? text : "";
}

static const odin3_sim_frame *frame_at(const odin3_sim_builder *bld, uint32_t frame) {
    return odin3_vec_cat(&bld->frames, frame);
}

static const odin3_module *frame_module(const odin3_sim_builder *bld, uint32_t frame) {
    return odin3_module_get(bld->design, frame_at(bld, frame)->module);
}

/* "file:line: " of the first source location behind prov (following derivations), or "". */
static void loc_prefix(const odin3_sim_builder *bld, odin3_prov_id prov, char *buf) {
    const odin3_prov_record *rec = odin3_prov_get(bld->design, prov);
    while (rec != NULL && rec->kind == ODIN3_PROV_DERIVED && rec->parents.count > 0) {
        rec = odin3_prov_get(bld->design, rec->parents.ids[0]);
    }
    buf[0] = '\0';
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].file != 0) {
        (void)snprintf(buf, LOC_MAX, "%s:%u: ", strtab(bld, rec->locs[0].file), rec->locs[0].line);
    }
}

/* Prepends "inst/inst/.../" for frame (nothing for the top). */
static void prepend_path(const odin3_sim_builder *bld, uint32_t frame, name_buf *nb) {
    char seg[SEG_MAX];
    while (frame != 0 && !nb->full) {
        const odin3_sim_frame *fr = frame_at(bld, frame);
        const odin3_module *parent = frame_module(bld, fr->parent);
        uint32_t name = odin3_node_name(parent, fr->inst);
        if (name != 0) {
            (void)snprintf(seg, sizeof seg, "%s/", strtab(bld, name));
        } else {
            (void)snprintf(seg, sizeof seg, "#%u/", fr->inst.v);
        }
        prepend(nb, seg);
        frame = fr->parent;
    }
}

static void node_name(const odin3_sim_builder *bld, uint32_t frame, odin3_node_id node,
                      name_buf *nb) {
    char seg[SEG_MAX];
    uint32_t name = odin3_node_name(frame_module(bld, frame), node);
    if (name != 0) {
        (void)snprintf(seg, sizeof seg, "%s", strtab(bld, name));
    } else {
        (void)snprintf(seg, sizeof seg, "#%u", node.v);
    }
    name_init(nb);
    prepend(nb, seg);
    prepend_path(bld, frame, nb);
}

/* A net's name: its own, else its primary wire bit, else #id. */
static void net_name(const odin3_sim_builder *bld, uint32_t frame, odin3_net_id net, name_buf *nb) {
    char seg[SEG_MAX];
    const odin3_module *mod = frame_module(bld, frame);
    odin3_wirebit wb = odin3_net_primary(mod, net);
    if (odin3_net_name(mod, net) != 0) {
        (void)snprintf(seg, sizeof seg, "%s", strtab(bld, odin3_net_name(mod, net)));
    } else if (odin3_wire_valid(wb.wire) && odin3_wire_width(mod, wb.wire) == 1) {
        (void)snprintf(seg, sizeof seg, "%s", strtab(bld, odin3_wire_name(mod, wb.wire)));
    } else if (odin3_wire_valid(wb.wire)) {
        (void)snprintf(seg, sizeof seg, "%s[%d]", strtab(bld, odin3_wire_name(mod, wb.wire)),
                       odin3_wire_index(mod, wb.wire, wb.bit));
    } else {
        (void)snprintf(seg, sizeof seg, "#%u", net.v);
    }
    name_init(nb);
    prepend(nb, seg);
    prepend_path(bld, frame, nb);
}

/* The first (frame, net) in expansion order whose final slot is slot; false when none. */
static bool slot_net(const odin3_sim_builder *bld, uint32_t slot, uint32_t *frame,
                     odin3_net_id *net) {
    const uint32_t *uf = bld->uf.data;
    const uint32_t *map = bld->netmap.data;
    for (uint32_t fi = 0; fi < bld->frames.len; fi++) {
        const odin3_sim_frame *fr = frame_at(bld, fi);
        uint32_t end = odin3_module_net_end(frame_module(bld, fi));
        for (uint32_t nid = 1; nid < end; nid++) {
            uint32_t prov = map[fr->map + nid];
            if (prov != ODIN3_SIM_UNSET && uf[prov] == slot) {
                *frame = fi;
                *net = (odin3_net_id){nid};
                return true;
            }
        }
    }
    return false;
}

/* The name of the net on final slot `slot` and the location of that net. */
static void slot_name(const odin3_sim_builder *bld, uint32_t slot, name_buf *nb, char *loc) {
    uint32_t frame = 0;
    odin3_net_id net = {0};
    loc[0] = '\0';
    if (!slot_net(bld, slot, &frame, &net)) {
        name_init(nb);
        prepend(nb, slot == ODIN3_SIM_SLOT_ZERO ? "(unconnected)" : "(internal)");
        return;
    }
    net_name(bld, frame, net, nb);
    loc_prefix(bld, odin3_net_prov(frame_module(bld, frame), net), loc);
}

void odin3_sim_err_unsupported(const odin3_sim_builder *bld, uint32_t frame, odin3_node_id node) {
    const odin3_module *mod = frame_module(bld, frame);
    const odin3_celltype_def *def = odin3_celltype_get(bld->design, odin3_node_type(mod, node));
    char loc[LOC_MAX];
    name_buf nb;
    loc_prefix(bld, odin3_node_prov(mod, node), loc);
    node_name(bld, frame, node, &nb);
    odin3_log(ODIN3_LOG_ERROR, "%scannot simulate `%s` (node `%s`): %s", loc, def->name,
              name_str(&nb),
              def->gran == ODIN3_GRAN_BLACKBOX ? "a black box has no semantics"
                                               : "the cell type has no simulate hook");
}

void odin3_sim_err_clock(const odin3_sim_builder *bld, const odin3_sim_flat *cell, uint32_t slot) {
    char loc[LOC_MAX];
    char net_loc[LOC_MAX];
    name_buf node;
    name_buf net;
    loc_prefix(bld, odin3_node_prov(frame_module(bld, cell->frame), cell->node), loc);
    node_name(bld, cell->frame, cell->node, &node);
    slot_name(bld, slot, &net, net_loc);
    odin3_log(ODIN3_LOG_ERROR,
              "%scannot simulate: the clock of `%s` node `%s` is net `%s`, which is not a "
              "primary input (clocks must come from top-level input ports)",
              loc, cell->def->name, name_str(&node), name_str(&net));
}

void odin3_sim_err_loop(const odin3_sim_builder *bld, uint32_t slot) {
    char loc[LOC_MAX];
    name_buf net;
    slot_name(bld, slot, &net, loc);
    odin3_log(ODIN3_LOG_ERROR, "%scannot simulate: combinational loop through net `%s`", loc,
              name_str(&net));
}

void odin3_sim_err_inout(const odin3_sim_builder *bld, uint32_t port_name) {
    odin3_log(ODIN3_LOG_ERROR, "cannot simulate inout port `%s` of the top module",
              strtab(bld, port_name));
}
