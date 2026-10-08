/* net.c — nets, their partitioned pin arrays, and pin connect/disconnect (IR §3 Net, IR-15). */
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/pinpool.h"
#include "util/log.h"
#include "util/u64map.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* --- pin arrays ---------------------------------------------------------------------------- */

/* Stores pin at index slot of the net's array and records the slot in the pin. */
static void put_at(odin3_module *module, odin3_net_rec *net, uint32_t slot, odin3_pin_id pin) {
    net->pins[slot] = pin;
    odin3_pin_rec_at(module, pin)->slot = slot;
}

/* Makes room for one more pin (a bigger block when full); the pins themselves do not change. */
static odin3_status net_room(odin3_module *module, odin3_net_rec *net) {
    uint32_t cls = 0;
    if (net->pins != NULL) {
        if (net->count < odin3_pinpool_capacity(net->cls)) {
            return ODIN3_OK;
        }
        cls = net->cls + 1U;
        if (cls >= ODIN3_PINPOOL_CLASSES) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_pin_id *block = odin3_pinpool_alloc(&module->pinpool, cls);
    if (block == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (net->pins != NULL) {
        memcpy(block, net->pins, sizeof *block * net->count);
        odin3_pinpool_release(&module->pinpool, net->pins, net->cls);
    }
    net->pins = block;
    net->cls = (uint8_t)cls;
    return ODIN3_OK;
}

/* Appends pin to the net (room reserved): a driver goes to the end of the driver partition. */
static void attach(odin3_module *module, odin3_net_id net_id, odin3_pin_id pin_id) {
    odin3_net_rec *net = odin3_net_rec_at(module, net_id);
    odin3_pin_rec *pin = odin3_pin_rec_at(module, pin_id);
    assert(net->pins != NULL && net->count < odin3_pinpool_capacity(net->cls));
    if (pin->dir != ODIN3_DIR_IN) {
        if (net->count > net->driver_count) { /* first sink moves to the end */
            put_at(module, net, net->count, net->pins[net->driver_count]);
        }
        put_at(module, net, net->driver_count, pin_id);
        net->driver_count++;
    } else {
        put_at(module, net, net->count, pin_id);
    }
    net->count++;
    pin->net = net_id;
}

void odin3_net_detach(odin3_module *module, odin3_pin_rec *pin) {
    odin3_net_rec *net = odin3_net_rec_at(module, pin->net);
    assert(net != NULL && net->count > 0 && pin->slot < net->count);
    uint32_t hole = pin->slot;
    if (hole < net->driver_count) { /* last driver fills the hole; the boundary moves down */
        uint32_t last_driver = net->driver_count - 1;
        if (hole != last_driver) {
            put_at(module, net, hole, net->pins[last_driver]);
        }
        hole = last_driver;
        net->driver_count--;
    }
    uint32_t last = net->count - 1;
    if (hole != last) { /* last sink fills the hole */
        put_at(module, net, hole, net->pins[last]);
    }
    net->count--;
    if (net->count == 0) {
        odin3_pinpool_release(&module->pinpool, net->pins, net->cls);
        net->pins = NULL;
        net->cls = 0;
    }
    pin->net = (odin3_net_id){0};
    pin->slot = 0;
}

odin3_status odin3_pin_connect(odin3_module *module, odin3_pin_id pin, odin3_net_id net) {
    odin3_pin_rec *pin_rec = odin3_pin_live_rec(module, pin, "pin_connect");
    odin3_net_rec *net_rec = odin3_net_live_rec(module, net, "pin_connect");
    if (pin_rec == NULL || net_rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (pin_rec->net.v == net.v) {
        return ODIN3_OK;
    }
    odin3_status st = net_room(module, net_rec);
    if (st != ODIN3_OK) {
        return st;
    }
    if (odin3_net_valid(pin_rec->net)) {
        odin3_net_detach(module, pin_rec);
    }
    attach(module, net, pin);
    return ODIN3_OK;
}

odin3_status odin3_pin_disconnect(odin3_module *module, odin3_pin_id pin) {
    odin3_pin_rec *rec = odin3_pin_live_rec(module, pin, "pin_disconnect");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_net_valid(rec->net)) {
        odin3_net_detach(module, rec);
    }
    return ODIN3_OK;
}

/* --- net lifetime and names ---------------------------------------------------------------- */

odin3_status odin3_net_create(odin3_module *module, uint32_t name_str, odin3_prov_id prov,
                              odin3_net_id *out) {
    odin3_net_id id = {module->nets.len};
    odin3_name_change name = {module->net_names, "net_create", id.v, 0, name_str};
    if (!odin3_names_available(module, &name)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_store_reserve(&module->nets, 1);
    if (st == ODIN3_OK) {
        st = odin3_names_change(module, &name); /* last fallible step */
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_rec *rec = odin3_store_take(&module->nets);
    rec->name = name_str;
    rec->prov = prov;
    if (out != NULL) {
        *out = id;
    }
    return ODIN3_OK;
}

odin3_status odin3_net_delete(odin3_module *module, odin3_net_id net) {
    odin3_net_rec *rec = odin3_net_live_rec(module, net, "net_delete");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    /* Aliases join this check when the alias table exists (module ports and wires). */
    if (rec->count > 0 || odin3_wire_valid(rec->wire)) {
        odin3_log(ODIN3_LOG_ERROR, "net_delete: net %u still has pins or a wire", net.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (rec->name != 0) {
        (void)odin3_u64map_remove(module->net_names, rec->name);
    }
    rec->dead = true;
    return ODIN3_OK;
}

odin3_status odin3_net_rename(odin3_module *module, odin3_net_id net, uint32_t name_str) {
    odin3_net_rec *rec = odin3_net_live_rec(module, net, "net_rename");
    if (rec == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_name_change name = {module->net_names, "net_rename", net.v, rec->name, name_str};
    odin3_status st = odin3_names_change(module, &name);
    if (st == ODIN3_OK) {
        rec->name = name_str;
    }
    return st;
}

/* --- net accessors ------------------------------------------------------------------------- */

bool odin3_net_live(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    return rec != NULL && !rec->dead;
}

uint32_t odin3_net_name(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    return rec != NULL ? rec->name : 0;
}

odin3_prov_id odin3_net_prov(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    return rec != NULL ? rec->prov : (odin3_prov_id){0};
}

odin3_pin_id odin3_net_driver(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    return rec != NULL && rec->driver_count > 0 ? rec->pins[0] : (odin3_pin_id){0};
}

uint32_t odin3_net_driver_count(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    return rec != NULL ? rec->driver_count : 0;
}

odin3_pinlist odin3_net_sinks(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    if (rec == NULL || rec->count == rec->driver_count) {
        return (odin3_pinlist){NULL, 0};
    }
    return (odin3_pinlist){rec->pins + rec->driver_count, rec->count - rec->driver_count};
}

odin3_pinlist odin3_net_pins(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    if (rec == NULL || rec->count == 0) {
        return (odin3_pinlist){NULL, 0};
    }
    return (odin3_pinlist){rec->pins, rec->count};
}

odin3_const odin3_net_const_value(const odin3_module *module, odin3_net_id net) {
    const odin3_net_rec *rec = odin3_net_rec_cat(module, net);
    if (rec == NULL || rec->driver_count != 1) {
        return ODIN3_CONST_NONE;
    }
    const odin3_pin_rec *pin = odin3_pin_rec_cat(module, rec->pins[0]);
    const odin3_node_rec *node = odin3_node_rec_cat(module, pin->node);
    const odin3_celltype_def *def = odin3_celltype_get(module->design, node->type);
    if (def == NULL || def->const_value == NULL) {
        return ODIN3_CONST_NONE;
    }
    return def->const_value(node->params);
}
