/* ir_test.c — test-only corruption hooks: each fault breaks one IR §9 invariant on purpose. */
#include "ir/ir_test.h"

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The live connected pin id, or NULL. */
static odin3_pin_rec *connected_pin(odin3_module *module, uint32_t id) {
    odin3_pin_rec *pin = odin3_pin_rec_at(module, (odin3_pin_id){id});
    return pin != NULL && odin3_node_live(module, pin->node) && odin3_net_valid(pin->net) ? pin
                                                                                          : NULL;
}

static odin3_net_rec *live_net(odin3_module *module, uint32_t id) {
    odin3_net_rec *net = odin3_net_rec_at(module, (odin3_net_id){id});
    return net != NULL && !net->dead ? net : NULL;
}

static odin3_node_rec *live_node(odin3_module *module, uint32_t id) {
    odin3_node_rec *node = odin3_node_rec_at(module, (odin3_node_id){id});
    return node != NULL && !node->dead ? node : NULL;
}

static const odin3_celltype_def *node_def(const odin3_module *module, const odin3_node_rec *node) {
    return odin3_celltype_get(module->design, node->type);
}

static bool pin_bad_port(odin3_module *module, uint32_t id) {
    odin3_pin_rec *pin = odin3_pin_rec_at(module, (odin3_pin_id){id});
    const odin3_node_rec *node = pin != NULL ? live_node(module, pin->node.v) : NULL;
    if (node == NULL) {
        return false;
    }
    pin->port = node_def(module, node)->n_ports;
    return true;
}

static bool pin_not_in_net(odin3_module *module, uint32_t id) {
    odin3_pin_rec *pin = connected_pin(module, id);
    if (pin == NULL) {
        return false;
    }
    odin3_net_id net = pin->net;
    odin3_net_detach(module, pin);
    pin->net = net; /* the pin still claims the net that no longer lists it */
    return true;
}

static bool net_extra_pin(odin3_module *module, uint32_t id) {
    odin3_pin_rec *pin = connected_pin(module, id);
    if (pin == NULL) {
        return false;
    }
    pin->net = (odin3_net_id){0}; /* the net still lists the pin */
    pin->slot = 0;
    return true;
}

static void put_at(odin3_module *module, odin3_net_rec *net, uint32_t slot, odin3_pin_id pin) {
    net->pins[slot] = pin;
    odin3_pin_rec_at(module, pin)->slot = slot;
}

static bool partition(odin3_module *module, uint32_t id) {
    odin3_net_rec *net = live_net(module, id);
    if (net == NULL || net->driver_count == 0 || net->driver_count == net->count) {
        return false;
    }
    odin3_pin_id driver = net->pins[0];
    odin3_pin_id sink = net->pins[net->count - 1];
    put_at(module, net, 0, sink);
    put_at(module, net, net->count - 1, driver);
    return true;
}

static bool driver_count(odin3_module *module, uint32_t id) {
    odin3_net_rec *net = live_net(module, id);
    if (net == NULL) {
        return false;
    }
    net->driver_count = net->count + 1;
    return true;
}

static odin3_status multi_driver(odin3_module *module, uint32_t id) {
    const odin3_net_rec *net = live_net(module, id);
    uint32_t name = 0;
    odin3_node_spec spec = {{0}, 0, {0}, NULL, 0};
    if (net == NULL ||
        !odin3_strtab_find(odin3_design_strtab(module->design), odin3_bytes_cstr("$_CONST0_"),
                           &name) ||
        !odin3_celltype_find(module->design, name, &spec.type)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    spec.prov = net->prov;
    odin3_netvec port = {&(odin3_net_id){id}, 1};
    return odin3_node_create_connected(module, &spec, &port, NULL);
}

static bool pin_count(odin3_module *module, uint32_t id) {
    odin3_node_rec *node = live_node(module, id);
    if (node == NULL || node->pin_count == 0) {
        return false;
    }
    node->pin_count--;
    return true;
}

static bool dup_name(odin3_module *module, uint32_t id) {
    odin3_node_rec *node = live_node(module, id);
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t i = 1; node != NULL && i < end; i++) {
        const odin3_node_rec *other = live_node(module, i);
        if (i != id && other != NULL && other->name != 0) {
            node->name = other->name; /* the map is left alone */
            return true;
        }
    }
    return false;
}

static bool bad_prov(odin3_module *module, uint32_t id) {
    odin3_node_rec *node = live_node(module, id);
    if (node == NULL) {
        return false;
    }
    node->prov.v = odin3_prov_end(module->design);
    return true;
}

static bool wire_backref(odin3_module *module, uint32_t id) {
    const odin3_wire_rec *wire = odin3_wire_rec_cat(module, (odin3_wire_id){id});
    odin3_net_rec *net = wire != NULL && !wire->dead && wire->width > 0
                             ? odin3_net_rec_at(module, wire->nets[0])
                             : NULL;
    if (net == NULL || net->wire.v != id || net->wire_bit != 0) {
        return false;
    }
    net->wire = (odin3_wire_id){0};
    return true;
}

static bool port_list(odin3_module *module, uint32_t id) {
    size_t count = module->ports.len;
    if (count < 2 || id >= count) {
        return false;
    }
    odin3_port_rec *port = odin3_vec_at(&module->ports, id);
    const odin3_port_rec *next = odin3_vec_cat(&module->ports, (id + 1) % count);
    port->node = next->node;
    return true;
}

/* A 1-bit $not has the pins of a $_NOT_ (A in, Y out): retyped, it is valid except for views. */
static bool view(odin3_module *module, uint32_t id) {
    odin3_node_rec *node = live_node(module, id);
    uint32_t name = 0;
    odin3_celltype_id gate = {0};
    if (node == NULL || strcmp(node_def(module, node)->name, "$not") != 0 || node->pin_count != 2 ||
        !odin3_strtab_find(odin3_design_strtab(module->design), odin3_bytes_cstr("$_NOT_"),
                           &name) ||
        !odin3_celltype_find(module->design, name, &gate)) {
        return false;
    }
    odin3_celltype_instances_dec(module->design, node->type);
    odin3_celltype_instances_inc(module->design, gate);
    node->type = gate;
    node->n_params = 0;
    node->params = NULL;
    return true;
}

/* Like odin3_node_delete, but the pins stay on their nets. */
static bool dead_node_live_pin(odin3_module *module, uint32_t id) {
    odin3_node_rec *node = live_node(module, id);
    if (node == NULL || node_def(module, node)->gran == ODIN3_GRAN_PORT) {
        return false;
    }
    if (node->name != 0) {
        (void)odin3_u64map_remove(module->node_names, node->name);
    }
    odin3_celltype_instances_dec(module->design, node->type);
    node->dead = true;
    return true;
}

typedef bool (*fault_fn)(odin3_module *module, uint32_t id);

odin3_status odin3_ir_test_corrupt(odin3_module *module, odin3_ir_test_target target) {
    static const fault_fn faults[ODIN3_IR_TEST_FAULT_COUNT] = {
        [ODIN3_IR_TEST_PIN_BAD_PORT] = pin_bad_port,
        [ODIN3_IR_TEST_PIN_NOT_IN_NET] = pin_not_in_net,
        [ODIN3_IR_TEST_NET_EXTRA_PIN] = net_extra_pin,
        [ODIN3_IR_TEST_PARTITION] = partition,
        [ODIN3_IR_TEST_DRIVER_COUNT] = driver_count,
        [ODIN3_IR_TEST_MULTI_DRIVER] = NULL, /* goes through the API, may fail */
        [ODIN3_IR_TEST_PIN_COUNT] = pin_count,
        [ODIN3_IR_TEST_DUP_NAME] = dup_name,
        [ODIN3_IR_TEST_BAD_PROV] = bad_prov,
        [ODIN3_IR_TEST_WIRE_BACKREF] = wire_backref,
        [ODIN3_IR_TEST_PORT_LIST] = port_list,
        [ODIN3_IR_TEST_VIEW] = view,
        [ODIN3_IR_TEST_DEAD_NODE_LIVE_PIN] = dead_node_live_pin,
    };
    if (module != NULL && target.fault == ODIN3_IR_TEST_MULTI_DRIVER) {
        odin3_status st = multi_driver(module, target.id);
        if (st != ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "ir_test_corrupt: MULTI_DRIVER on net %u failed (%s)",
                      target.id, odin3_status_string(st));
        }
        return st;
    }
    if (module == NULL || (unsigned)target.fault >= ODIN3_IR_TEST_FAULT_COUNT ||
        !faults[target.fault](module, target.id)) {
        odin3_log(ODIN3_LOG_ERROR, "ir_test_corrupt: fault %d does not apply to %u",
                  (int)target.fault, target.id);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}
