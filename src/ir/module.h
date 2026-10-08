/*
 * module.h — modules and their nodes, pins and nets (IR §3, IR-14..IR-16).
 */
#ifndef ODIN3_IR_MODULE_H
#define ODIN3_IR_MODULE_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_module odin3_module; /* opaque handle, owned by its design */

/*
 * Every fallible call below returns ODIN3_ERR_INVALID_ARG (logged) for misuse it can detect in
 * O(1) — an out-of-range or dead ID, a duplicate name, a name that is not a strtab ID — and
 * ODIN3_ERR_NO_MEMORY on out of memory; on any failure the IR is unchanged. Accessors take a
 * valid ID; for an out-of-range ID they return zero / none. Accessors on a dead object return
 * what it held when it died (tombstones need it). Provenance IDs are not validated (check rule 7).
 */

/* --- modules ------------------------------------------------------------------------------- */

/*
 * New empty module named name_str (a non-empty strtab ID). Registers the module's cell type
 * (granularity MODULE, same name, no ports yet; IR-7), so the name must not be a cell type of the
 * design already (that includes other modules).
 */
odin3_status odin3_module_create(odin3_design *design, uint32_t name_str, odin3_prov_id prov,
                                 odin3_module_id *out);

/* The module with ID id, NULL when id is out of range. The handle lives as long as the design. */
odin3_module *odin3_module_get(odin3_design *design, odin3_module_id id);

/* One past the last module ID (module IDs start at 1). */
uint32_t odin3_design_module_end(const odin3_design *design);

odin3_design *odin3_module_design(const odin3_module *module);
odin3_module_id odin3_module_id_of(const odin3_module *module);
uint32_t odin3_module_name(const odin3_module *module);
odin3_prov_id odin3_module_prov(const odin3_module *module);
odin3_celltype_id odin3_module_type(const odin3_module *module); /* instantiates the module */

/* One past the last ID of each store (IDs start at 1; iterate in ID order, skip dead; IR-16). */
uint32_t odin3_module_node_end(const odin3_module *module);
uint32_t odin3_module_pin_end(const odin3_module *module);
uint32_t odin3_module_net_end(const odin3_module *module);

/* Name lookups (IR-14): the live object named name_str, or none (also for name_str 0). */
odin3_node_id odin3_module_find_node(const odin3_module *module, uint32_t name_str);
odin3_net_id odin3_module_find_net(const odin3_module *module, uint32_t name_str);

/* --- nodes --------------------------------------------------------------------------------- */

/*
 * What to create. name 0 = unnamed. params: one value per parameter definition of the type, in
 * order, kinds matching (n_params must equal the definition's count); NULL means the defaults.
 * The values are copied into the module.
 */
typedef struct odin3_node_spec {
    odin3_celltype_id type;
    uint32_t name;
    odin3_prov_id prov;
    const odin3_value *params;
    uint32_t n_params;
} odin3_node_spec;

/* A run of consecutive pin IDs; count 0 means empty (first is then none). */
typedef struct odin3_pinslice {
    odin3_pin_id first;
    uint32_t count;
} odin3_pinslice;

/*
 * Creates a node and its pins: contiguous IDs in port order, then bit order (LSB first), all
 * unconnected. The pin count follows from the type's port widths for the parameters and never
 * changes. INVALID_ARG also for an unknown type, mismatched parameters, a width parameter that is
 * not an int in [0, UINT32_MAX], or parameters the type's verify hook rejects. Counts as an
 * instance of the type (odin3_celltype_instances).
 */
odin3_status odin3_node_create(odin3_module *module, const odin3_node_spec *spec,
                               odin3_node_id *out);

/* Disconnects every pin, removes the node's name from the name map and marks node and pins dead.
 */
odin3_status odin3_node_delete(odin3_module *module, odin3_node_id node);

/* Renames a live node; name_str 0 removes the name. INVALID_ARG if another node has the name. */
odin3_status odin3_node_rename(odin3_module *module, odin3_node_id node, uint32_t name_str);

bool odin3_node_live(const odin3_module *module, odin3_node_id node);
odin3_celltype_id odin3_node_type(const odin3_module *module, odin3_node_id node);
uint32_t odin3_node_name(const odin3_module *module, odin3_node_id node);
odin3_prov_id odin3_node_prov(const odin3_module *module, odin3_node_id node);
odin3_pinslice odin3_node_pins(const odin3_module *module, odin3_node_id node);

/* Pins of port `port` (LSB first); empty for a width-0 or out-of-range port. O(log pins). */
odin3_pinslice odin3_node_port(const odin3_module *module, odin3_node_id node, uint32_t port);

/* Parameter value `index` (definition order); NULL when out of range. Valid while the module is. */
const odin3_value *odin3_node_param(const odin3_module *module, odin3_node_id node, uint32_t index);

/* --- pins ---------------------------------------------------------------------------------- */

/* A pin is live while its node is. */
bool odin3_pin_live(const odin3_module *module, odin3_pin_id pin);
odin3_node_id odin3_pin_node(const odin3_module *module, odin3_pin_id pin);
uint32_t odin3_pin_port(const odin3_module *module, odin3_pin_id pin);
uint32_t odin3_pin_bit(const odin3_module *module, odin3_pin_id pin);
odin3_net_id odin3_pin_net(const odin3_module *module, odin3_pin_id pin);   /* none: unconnected */
odin3_dir odin3_pin_dir(const odin3_module *module, odin3_pin_id pin);      /* the port's */
odin3_prov_id odin3_pin_prov(const odin3_module *module, odin3_pin_id pin); /* the node's */

/* True for a pin that drives its net: direction OUT or INOUT (IR §3, Net). */
bool odin3_pin_drives(const odin3_module *module, odin3_pin_id pin);

/* True for a pin that reads its net: direction IN or INOUT (readers = sinks + inout pins). */
bool odin3_pin_reads(const odin3_module *module, odin3_pin_id pin);

/*
 * Connects a live, unconnected pin to a live net; drivers go to the driver partition. A pin
 * already on that net is left alone (OK); a pin on another net is INVALID_ARG (disconnect first).
 */
odin3_status odin3_pin_connect(odin3_module *module, odin3_pin_id pin, odin3_net_id net);

/* Disconnects a live pin from its net; a no-op for an unconnected pin. Never allocates. */
odin3_status odin3_pin_disconnect(odin3_module *module, odin3_pin_id pin);

/* --- nets ---------------------------------------------------------------------------------- */

/* New net, no pins. name_str 0 = unnamed; INVALID_ARG if another net has the name. */
odin3_status odin3_net_create(odin3_module *module, uint32_t name_str, odin3_prov_id prov,
                              odin3_net_id *out);

/* Deletes a live net that has no pins, no wire membership and no aliases (else INVALID_ARG). */
odin3_status odin3_net_delete(odin3_module *module, odin3_net_id net);

/* Renames a live net; name_str 0 removes the name. INVALID_ARG if another net has the name. */
odin3_status odin3_net_rename(odin3_module *module, odin3_net_id net, uint32_t name_str);

bool odin3_net_live(const odin3_module *module, odin3_net_id net);
uint32_t odin3_net_name(const odin3_module *module, odin3_net_id net);
odin3_prov_id odin3_net_prov(const odin3_module *module, odin3_net_id net);

/*
 * A view of part of a net's pin array. Valid until the next connect, disconnect or delete that
 * touches the net; order within a partition is not significant.
 */
typedef struct odin3_pinlist {
    const odin3_pin_id *pins;
    uint32_t count;
} odin3_pinlist;

/* The first driver, none when the net has no driver. O(1). */
odin3_pin_id odin3_net_driver(const odin3_module *module, odin3_net_id net);
uint32_t odin3_net_driver_count(const odin3_module *module, odin3_net_id net);
odin3_pinlist odin3_net_sinks(const odin3_module *module, odin3_net_id net); /* IN pins */
odin3_pinlist odin3_net_pins(const odin3_module *module, odin3_net_id net);  /* drivers, sinks */

/*
 * IR-4: the constant the net carries, asking the const_value hook of its driver's type; NONE when
 * the net does not have exactly one driver or the driver's type has no hook.
 */
odin3_const odin3_net_const_value(const odin3_module *module, odin3_net_id net);

#endif
