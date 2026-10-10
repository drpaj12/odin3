/*
 * net.c — the public ABI's pin, net and wire groups: pin node/port/bit/net, net names, drivers,
 * pins (drivers first) and aliases, wire liveness and names.
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- pins ---------------------------------------------------------------------------------- */

/* What a pin query returns. */
typedef enum pin_field { PIN_NODE, PIN_PORT, PIN_BIT, PIN_NET } pin_field;

/* *out gets field of pin; INVALID_ARG (logged as fn's) for bad input. */
static odin3_status pin_query(const odin3_design *design, odin3_ref pin, uint32_t *out,
                              pin_field field, const char *fn) {
    const odin3_module *mod = odin3_api_ref(design, pin, ODIN3_API_PIN);
    if (mod == NULL || out == NULL) {
        return odin3_api_invalid(fn);
    }
    odin3_pin_id id = {pin.id};
    switch (field) {
    case PIN_NODE:
        *out = odin3_pin_node(mod, id).v;
        break;
    case PIN_PORT:
        *out = odin3_pin_port(mod, id);
        break;
    case PIN_BIT:
        *out = odin3_pin_bit(mod, id);
        break;
    default:
        *out = odin3_pin_net(mod, id).v;
        break;
    }
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_pin_get_node(const odin3_design *design, odin3_ref pin,
                                             uint32_t *node) {
    return pin_query(design, pin, node, PIN_NODE, __func__);
}

ODIN3_EXPORT odin3_status odin3_pin_get_port(const odin3_design *design, odin3_ref pin,
                                             uint32_t *port) {
    return pin_query(design, pin, port, PIN_PORT, __func__);
}

ODIN3_EXPORT odin3_status odin3_pin_get_bit(const odin3_design *design, odin3_ref pin,
                                            uint32_t *bit) {
    return pin_query(design, pin, bit, PIN_BIT, __func__);
}

ODIN3_EXPORT odin3_status odin3_pin_get_net(const odin3_design *design, odin3_ref pin,
                                            uint32_t *net) {
    return pin_query(design, pin, net, PIN_NET, __func__);
}

/* --- nets ---------------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_net_is_live(const odin3_design *design, odin3_ref net, bool *live) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    if (mod == NULL || live == NULL) {
        return odin3_api_invalid(__func__);
    }
    *live = odin3_net_live(mod, (odin3_net_id){net.id});
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_net_get_name(const odin3_design *design, odin3_ref net,
                                             const char **name) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    if (mod == NULL || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_api_str(design, odin3_net_name(mod, (odin3_net_id){net.id}));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_net_get_pin_count(const odin3_design *design, odin3_ref net,
                                                  uint32_t *count) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    if (mod == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_net_pins(mod, (odin3_net_id){net.id}).count;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_net_get_driver_count(const odin3_design *design, odin3_ref net,
                                                     uint32_t *count) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    if (mod == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_net_driver_count(mod, (odin3_net_id){net.id});
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_net_get_pin_at(const odin3_design *design, odin3_ref net,
                                               uint32_t index, uint32_t *pin) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    odin3_pinlist pins = {NULL, 0};
    if (mod != NULL) {
        pins = odin3_net_pins(mod, (odin3_net_id){net.id});
    }
    if (mod == NULL || pin == NULL || index >= pins.count) {
        return odin3_api_invalid(__func__);
    }
    *pin = pins.pins[index].v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_net_get_driver(const odin3_design *design, odin3_ref net,
                                               uint32_t *pin) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    if (mod == NULL || pin == NULL) {
        return odin3_api_invalid(__func__);
    }
    *pin = odin3_net_driver(mod, (odin3_net_id){net.id}).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_net_get_alias_count(const odin3_design *design, odin3_ref net,
                                                    uint32_t *count) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    if (mod == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_net_alias_count(mod, (odin3_net_id){net.id});
    return ODIN3_OK;
}

/* Alias `index` of net (oldest first) into *alias; false when the net has fewer aliases. */
static bool alias_at(const odin3_module *mod, odin3_net_id net, uint32_t index,
                     odin3_net_alias *alias) {
    uint32_t cursor = 0;
    for (uint32_t i = 0; i <= index; i++) {
        if (!odin3_net_alias_next(mod, net, &cursor, alias)) {
            return false;
        }
    }
    return true;
}

/* The name of an alias: a bare name as is, a wire bit as "wire[i]" (interned). */
static odin3_status alias_text(const odin3_design *design, const odin3_module *mod,
                               const odin3_net_alias *alias, const char **name) {
    if (!odin3_wire_valid(alias->wb.wire)) {
        *name = odin3_api_str(design, alias->name);
        return ODIN3_OK;
    }
    const char *wire = odin3_api_str(design, odin3_wire_name(mod, alias->wb.wire));
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = odin3_strbuf_appendf(&buf, "%s[%" PRId32 "]", wire,
                                           odin3_wire_index(mod, alias->wb.wire, alias->wb.bit));
    if (st == ODIN3_OK) {
        st = odin3_api_text(design, (odin3_bytes){buf.data, buf.len}, name);
    }
    odin3_strbuf_free(&buf);
    return st;
}

ODIN3_EXPORT odin3_status odin3_net_get_alias_name(const odin3_design *design, odin3_ref net,
                                                   uint32_t index, const char **name) {
    const odin3_module *mod = odin3_api_ref(design, net, ODIN3_API_NET);
    odin3_net_alias alias = {{{0}, 0}, 0};
    if (mod == NULL || name == NULL || !alias_at(mod, (odin3_net_id){net.id}, index, &alias)) {
        return odin3_api_invalid(__func__);
    }
    return alias_text(design, mod, &alias, name);
}

/* --- wires --------------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_wire_is_live(const odin3_design *design, odin3_ref wire,
                                             bool *live) {
    const odin3_module *mod = odin3_api_ref(design, wire, ODIN3_API_WIRE);
    if (mod == NULL || live == NULL) {
        return odin3_api_invalid(__func__);
    }
    *live = odin3_wire_live(mod, (odin3_wire_id){wire.id});
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_wire_get_name(const odin3_design *design, odin3_ref wire,
                                              const char **name) {
    const odin3_module *mod = odin3_api_ref(design, wire, ODIN3_API_WIRE);
    if (mod == NULL || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_api_str(design, odin3_wire_name(mod, (odin3_wire_id){wire.id}));
    return ODIN3_OK;
}
