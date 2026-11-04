/*
 * split.c — the inout ports of the top module become inputs and outputs (see tristate.h): `p` is
 * read through input `p_i`, driven through output `p_o` (the data) and output `p_oe` (the enable).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/tristate/tristate.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { SP_NAME_CAP = 256, SP_TEXT_CAP = 512 };

/* What one inout port is: its nets, the cells that drive them and whether anything reads them. */
typedef struct sp_port {
    uint32_t idx, width;
    odin3_node_id node;
    odin3_vec nets;  /* odin3_net_id per bit */
    odin3_vec cells; /* odin3_node_id: the $tribuf cells */
    odin3_vec recs;  /* ts_rec per tri-state bit; .net = the port net */
    bool read;
} sp_port;

static odin3_net_id bit_net(const ts_ctx *tc, odin3_node_id node, uint32_t port, uint32_t bit) {
    odin3_pinslice s = odin3_node_port(tc->module, node, port);
    return odin3_pin_net(tc->module, (odin3_pin_id){s.first.v + bit});
}

static void sp_free(sp_port *sp) {
    odin3_vec_free(&sp->nets);
    odin3_vec_free(&sp->cells);
    odin3_vec_free(&sp->recs);
}

static int net_bit(const sp_port *sp, odin3_net_id net) {
    const odin3_net_id *nets = sp->nets.data;
    for (size_t i = 0; i < sp->nets.len; i++) {
        if (nets[i].v == net.v) {
            return (int)i;
        }
    }
    return -1;
}

/* Adds the cell's bits as records. False when a bit is on a net that is not the port's. */
static bool add_cell_recs(const ts_ctx *tc, sp_port *sp, odin3_node_id cell) {
    uint32_t width = odin3_node_port(tc->module, cell, 2).count;
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_net_id net = bit_net(tc, cell, 2, bit);
        ts_rec *rec = net_bit(sp, net) >= 0 ? odin3_vec_push(&sp->recs) : NULL;
        if (rec == NULL) {
            return false;
        }
        *rec = (ts_rec){net, bit_net(tc, cell, 0, bit), bit_net(tc, cell, 1, 0), cell};
    }
    odin3_node_id *slot = odin3_vec_push(&sp->cells);
    if (slot == NULL) {
        return false;
    }
    *slot = cell;
    return true;
}

/* Scans the pins of the port's nets; false when the port cannot be split (a foreign driver). */
static bool scan_nets(const ts_ctx *tc, sp_port *sp) {
    const odin3_net_id *nets = sp->nets.data;
    for (uint32_t j = 0; j < sp->width; j++) {
        odin3_pinlist pins = odin3_net_pins(tc->module, nets[j]);
        uint32_t drivers = odin3_net_driver_count(tc->module, nets[j]);
        for (uint32_t i = 0; i < pins.count; i++) {
            odin3_node_id node = odin3_pin_node(tc->module, pins.pins[i]);
            if (node.v == sp->node.v) {
                continue;
            }
            if (i >= drivers) {
                sp->read = true;
            } else if (!ts_is_tribuf(tc, node)) {
                return false;
            }
        }
    }
    return true;
}

/* The tri-state cells on the port's nets, each once. */
static bool scan_cells(const ts_ctx *tc, sp_port *sp) {
    const odin3_net_id *nets = sp->nets.data;
    for (uint32_t j = 0; j < sp->width; j++) {
        odin3_pinlist pins = odin3_net_pins(tc->module, nets[j]);
        uint32_t drivers = odin3_net_driver_count(tc->module, nets[j]);
        for (uint32_t i = 0; i < drivers; i++) {
            odin3_node_id node = odin3_pin_node(tc->module, pins.pins[i]);
            bool seen = false;
            const odin3_node_id *cells = sp->cells.data;
            for (size_t c = 0; c < sp->cells.len; c++) {
                seen = seen || cells[c].v == node.v;
            }
            if (ts_is_tribuf(tc, node) && !seen && !add_cell_recs(tc, sp, node)) {
                return false;
            }
        }
    }
    return true;
}

/* A new port <name>_<suffix>; *out is its node. */
static odin3_status add_port(ts_ctx *tc, const sp_port *sp, const char *suffix, odin3_dir dir,
                             odin3_node_id *out) {
    char name[SP_NAME_CAP];
    const char *base = odin3_strtab_get(odin3_design_strtab(tc->design),
                                        odin3_wire_name(tc->module, odin3_module_port_wire(
                                                                        tc->module, sp->idx)));
    (void)snprintf(name, sizeof name, "%s_%s", base, suffix);
    uint32_t str = 0;
    odin3_status st = odin3_design_intern(tc->design, odin3_bytes_cstr(name), &str);
    odin3_port_spec spec = {str,
                            dir,
                            sp->width,
                            sp->width == 1U,
                            odin3_node_prov(tc->module, sp->node),
                            false,
                            0,
                            0,
                            false};
    return st == ODIN3_OK ? odin3_module_add_port(tc->module, &spec, out) : st;
}

/* The records of port bit j, contiguous, into buf; *k their number. */
static odin3_status bit_recs(const sp_port *sp, odin3_net_id net, odin3_vec *buf, uint32_t *k) {
    const ts_rec *recs = sp->recs.data;
    odin3_vec_clear(buf);
    for (size_t i = 0; i < sp->recs.len; i++) {
        ts_rec *slot = recs[i].net.v == net.v ? odin3_vec_push(buf) : NULL;
        if (recs[i].net.v == net.v && slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (slot != NULL) {
            *slot = recs[i];
        }
    }
    *k = (uint32_t)buf->len;
    return ODIN3_OK;
}

/* The data and enable nets of port bit j, in out[0] and out[1]. */
static odin3_status bit_drive(ts_ctx *tc, const sp_port *sp, uint32_t j, odin3_net_id out[2]) {
    odin3_vec buf;
    odin3_vec_init(&buf, sizeof(ts_rec));
    uint32_t k = 0;
    odin3_net_id net = ((const odin3_net_id *)sp->nets.data)[j];
    odin3_status st = bit_recs(sp, net, &buf, &k);
    const ts_rec *recs = buf.data;
    if (st == ODIN3_OK && k == 0) {
        st = ts_const(tc, 0, &out[0]);
        out[1] = out[0];
    } else if (st == ODIN3_OK && k == 1U) {
        out[0] = recs[0].data;
        out[1] = recs[0].enable;
    } else if (st == ODIN3_OK) {
        st = odin3_net_create(tc->module, 0, odin3_module_prov(tc->module), &out[0]);
        st = st == ODIN3_OK ? ts_pmux(tc, recs, k, out[0]) : st;
        st = st == ODIN3_OK ? ts_or_enables(tc, recs, k, &out[1]) : st;
    }
    odin3_vec_free(&buf);
    return st;
}

/* The new input p_i takes over the port's net j: the shell's pin moves to a net of its own, the net of
 * p_i merges into it, so every reader (and every wire name) now sees p_i. */
static odin3_status take_over(const ts_ctx *tc, const sp_port *sp, uint32_t j, odin3_net_id from) {
    odin3_net_id net = ((const odin3_net_id *)sp->nets.data)[j];
    odin3_pinslice pins = odin3_node_port(tc->module, sp->node, 0);
    odin3_pin_id pin = {pins.first.v + j};
    odin3_net_id lone = {0};
    odin3_status st = odin3_pin_disconnect(tc->module, pin);
    st = st == ODIN3_OK ? odin3_net_create(tc->module, 0, odin3_module_prov(tc->module), &lone) : st;
    st = st == ODIN3_OK ? odin3_pin_connect(tc->module, pin, lone) : st;
    return st == ODIN3_OK ? odin3_net_merge(tc->module, (odin3_net_pair){net, from}) : st;
}

/* The nets that drive the new ports, per bit: data and enable. */
typedef struct sp_drive {
    odin3_vec data, enable; /* odin3_net_id per bit */
} sp_drive;

static odin3_status push_net(odin3_vec *vec, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

static odin3_status make_drives(ts_ctx *tc, const sp_port *sp, sp_drive *dr) {
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < sp->width; j++) {
        odin3_net_id de[2] = {{0}, {0}};
        st = bit_drive(tc, sp, j, de);
        st = st == ODIN3_OK ? push_net(&dr->data, de[0]) : st;
        st = st == ODIN3_OK ? push_net(&dr->enable, de[1]) : st;
    }
    return st;
}

/* Wires p_o / p_oe to the drives, by merging the new ports' nets into them. */
static odin3_status wire_outputs(ts_ctx *tc, const sp_drive *dr, odin3_node_id po,
                                 odin3_node_id poe) {
    uint32_t width = (uint32_t)dr->data.len;
    const odin3_net_id *data = dr->data.data;
    const odin3_net_id *enable = dr->enable.data;
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < width; j++) {
        odin3_net_pair pair = {data[j], bit_net(tc, po, 0, j)};
        st = pair.keep.v == pair.drop.v ? ODIN3_OK : odin3_net_merge(tc->module, pair);
        pair = (odin3_net_pair){enable[j], bit_net(tc, poe, 0, j)};
        st = st == ODIN3_OK && pair.keep.v != pair.drop.v ? odin3_net_merge(tc->module, pair) : st;
    }
    return st;
}

static odin3_status delete_cells(ts_ctx *tc, const sp_port *sp) {
    const odin3_node_id *cells = sp->cells.data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sp->cells.len; i++) {
        st = odin3_node_delete(tc->module, cells[i]);
    }
    return st;
}

static odin3_status mark_shell(ts_ctx *tc, const sp_port *sp) {
    uint32_t key = 0;
    odin3_status st =
        odin3_design_intern(tc->design, odin3_bytes_cstr(ODIN3_ATTR_TRISTATE_SPLIT), &key);
    odin3_value one = odin3_value_int(1);
    return st == ODIN3_OK
               ? odin3_attr_set(tc->module, (odin3_objref){ODIN3_OBJ_NODE, sp->node.v}, key, &one)
               : st;
}

static void say_split(const ts_ctx *tc, const sp_port *sp, bool driven) {
    char text[SP_TEXT_CAP];
    const char *base = odin3_strtab_get(
        odin3_design_strtab(tc->design),
        odin3_wire_name(tc->module, odin3_module_port_wire(tc->module, sp->idx)));
    char what[SP_NAME_CAP];
    (void)snprintf(what, sizeof what, "port %s", base);
    if (driven) {
        (void)snprintf(text, sizeof text,
                       "split into %s_i/%s_o/%s_oe (%s_o is the data, %s_oe the enable of the "
                       "tri-state drivers%s)",
                       base, base, base, base, base, sp->read ? "" : "; never read: no input");
    } else {
        (void)snprintf(text, sizeof text,
                       "split into %s_i: nothing drives it inside, so it is an input", base);
    }
    ts_warn(tc, sp->node, "inout", what, text);
}

/* Performs the split of an analysed port. */
static odin3_status do_split(ts_ctx *tc, sp_port *sp) {
    bool driven = sp->recs.len > 0;
    sp_drive dr;
    odin3_vec_init(&dr.data, sizeof(odin3_net_id));
    odin3_vec_init(&dr.enable, sizeof(odin3_net_id));
    odin3_node_id pin_node = {0}, po = {0}, poe = {0};
    odin3_status st = driven ? make_drives(tc, sp, &dr) : ODIN3_OK;
    st = st == ODIN3_OK && (sp->read || !driven) ? add_port(tc, sp, "i", ODIN3_DIR_IN, &pin_node)
                                                  : st;
    st = st == ODIN3_OK && driven ? add_port(tc, sp, "o", ODIN3_DIR_OUT, &po) : st;
    st = st == ODIN3_OK && driven ? add_port(tc, sp, "oe", ODIN3_DIR_OUT, &poe) : st;
    st = st == ODIN3_OK ? delete_cells(tc, sp) : st;
    for (uint32_t j = 0; st == ODIN3_OK && pin_node.v != 0 && j < sp->width; j++) {
        st = take_over(tc, sp, j, bit_net(tc, pin_node, 0, j));
    }
    st = st == ODIN3_OK && driven ? wire_outputs(tc, &dr, po, poe) : st;
    st = st == ODIN3_OK ? mark_shell(tc, sp) : st;
    if (st == ODIN3_OK) {
        say_split(tc, sp, driven);
        tc->split++;
    }
    odin3_vec_free(&dr.data);
    odin3_vec_free(&dr.enable);
    return st;
}

/* Analyses port `idx`; *ok is true when it can be split. */
static odin3_status analyse(const ts_ctx *tc, sp_port *sp, bool *ok) {
    odin3_vec_init(&sp->nets, sizeof(odin3_net_id));
    odin3_vec_init(&sp->cells, sizeof(odin3_node_id));
    odin3_vec_init(&sp->recs, sizeof(ts_rec));
    sp->node = odin3_module_port(tc->module, sp->idx);
    sp->width = odin3_wire_width(tc->module, odin3_module_port_wire(tc->module, sp->idx));
    sp->read = false;
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < sp->width; j++) {
        st = push_net(&sp->nets, bit_net(tc, sp->node, 0, j));
    }
    *ok = st == ODIN3_OK && scan_nets(tc, sp) && scan_cells(tc, sp) &&
          (sp->recs.len > 0 || sp->read);
    return st;
}

static odin3_status split_port(ts_ctx *tc, uint32_t idx) {
    sp_port sp = {.idx = idx};
    bool ok = false;
    odin3_status st = analyse(tc, &sp, &ok);
    if (st == ODIN3_OK && ok && tc->opts->strict) {
        odin3_log(ODIN3_LOG_ERROR, "tristate: inout port of the top is not supported with --strict");
        st = ODIN3_ERR_INVALID_ARG;
    }
    st = st == ODIN3_OK && ok ? do_split(tc, &sp) : st;
    sp_free(&sp);
    return st;
}

odin3_status ts_split_inouts(ts_ctx *tc) {
    if (!tc->is_top) {
        return ODIN3_OK;
    }
    uint32_t count = odin3_module_port_count(tc->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        odin3_node_id node = odin3_module_port(tc->module, i);
        const odin3_celltype_def *def =
            odin3_celltype_get(tc->design, odin3_node_type(tc->module, node));
        bool inout = odin3_pin_dir(tc->module, odin3_node_port(tc->module, node, 0).first) ==
                         ODIN3_DIR_INOUT &&
                     def != NULL;
        st = inout ? split_port(tc, i) : st;
    }
    return st;
}
