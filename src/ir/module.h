/*
 * module.h — modules, ports, nodes, pins, nets, wires, aliases and attributes (IR §3, IR-2, IR-3,
 * IR-7, IR-10, IR-14..IR-16).
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

/* --- ports (IR-3, IR-7, IR-7b) ------------------------------------------------------------- */

/*
 * A module port. dir is the port's direction as an instance sees it (IN: an input of the module),
 * so IN makes a $port_in node (whose pin drives the module's net), OUT a $port_out, INOUT a
 * $port_inout. width is 1 .. 2^31; scalar records a port written without brackets and needs
 * width 1. name is a non-empty strtab ID that no wire of the module has.
 */
typedef struct odin3_port_spec {
    uint32_t name;
    odin3_dir dir;
    uint32_t width;
    bool scalar;
    odin3_prov_id prov;
} odin3_port_spec;

/*
 * Appends a port: creates the port node (WIDTH = width, unnamed), a wire named name with range
 * [width-1:0] and width new unnamed nets (each with the wire bit as primary), connects port pin k
 * to net k, appends the node to the port order and the port to the module's cell type (names,
 * directions, widths, order; IR-7). Everything takes prov. INVALID_ARG also while the module's
 * cell type has live instances (IR-7; deleting them all lifts the refusal). *port_node gets the
 * node. The cell-type definition is updated in place, so adding P ports costs O(P) memory.
 */
odin3_status odin3_module_add_port(odin3_module *module, const odin3_port_spec *spec,
                                   odin3_node_id *port_node);

/* Number of ports, and port node / port wire `index` in declaration order (none if out of range).
 */
uint32_t odin3_module_port_count(const odin3_module *module);
odin3_node_id odin3_module_port(const odin3_module *module, uint32_t index);
odin3_wire_id odin3_module_port_wire(const odin3_module *module, uint32_t index);

/* The module's cell type (IR-7): always in sync with its ports. Same as odin3_module_type. */
odin3_celltype_id odin3_module_celltype(const odin3_module *module);

/* --- creating connected nodes, replacing nodes --------------------------------------------- */

/* The nets of one port of a node to create, LSB first; an entry of none leaves that pin open. */
typedef struct odin3_netvec {
    const odin3_net_id *nets;
    uint32_t count;
} odin3_netvec;

/*
 * odin3_node_create plus connections in one call (IR-15): ports holds one netvec per port of the
 * type, in port order, whose count must equal that port's width for the parameters; every entry
 * is a live net or none. INVALID_ARG (nothing created) for a count mismatch, a dead or
 * out-of-range net, or ports NULL on a type with ports; NO_MEMORY leaves the IR unchanged too
 * (connections made before the failure are undone).
 */
odin3_status odin3_node_create_connected(odin3_module *module, const odin3_node_spec *spec,
                                         const odin3_netvec *ports, odin3_node_id *out);

typedef struct odin3_node_pair {
    odin3_node_id old_node;
    odin3_node_id new_node;
} odin3_node_pair;

/*
 * Replaces old_node by new_node (IR-15): each pin of new_node takes the place of the pin of
 * old_node with the same port and bit on its net (same slot of the net's pin array), then
 * old_node is deleted (its name leaves the map; new_node keeps its own name). The two must be
 * distinct live nodes with the same port signature (port count, and per pin the same port, bit
 * and direction), new_node's pins must all be unconnected, and neither may be a port node
 * (granularity PORT), else INVALID_ARG. Never allocates.
 */
odin3_status odin3_node_replace(odin3_module *module, odin3_node_pair pair);

/* --- wires (IR-2) -------------------------------------------------------------------------- */

/* A wire: name (0 = unnamed, else unique among the module's wires), range [msb:lsb], sign. */
typedef struct odin3_wire_spec {
    uint32_t name;
    int32_t msb, lsb;
    bool is_signed;
    odin3_prov_id prov;
} odin3_wire_spec;

/*
 * Creates a wire of width |msb - lsb| + 1 (at most UINT32_MAX). Bit k of a wire is position k of
 * its net vector, LSB first: it is the declared index lsb + k when msb >= lsb and lsb - k when
 * msb < lsb, so in [7:0] bit 0 is index 0 and in [0:7] bit 0 is index 7 and bit 7 (the MSB) is
 * index 0. nets NULL creates width new unnamed nets (prov = spec prov), each with (wire, bit) as
 * primary. Otherwise nets holds width live nets (repeats allowed): bit k becomes the primary of
 * nets[k] when that net has no primary yet, else an alias of it.
 */
odin3_status odin3_wire_create(odin3_module *module, const odin3_wire_spec *spec,
                               const odin3_net_id *nets, odin3_wire_id *out);

/* One past the last wire ID; the live wire named name_str, or none. */
uint32_t odin3_module_wire_end(const odin3_module *module);
odin3_wire_id odin3_module_find_wire(const odin3_module *module, uint32_t name_str);

bool odin3_wire_live(const odin3_module *module, odin3_wire_id wire);
uint32_t odin3_wire_name(const odin3_module *module, odin3_wire_id wire);
odin3_prov_id odin3_wire_prov(const odin3_module *module, odin3_wire_id wire);
int32_t odin3_wire_msb(const odin3_module *module, odin3_wire_id wire);
int32_t odin3_wire_lsb(const odin3_module *module, odin3_wire_id wire);
bool odin3_wire_signed(const odin3_module *module, odin3_wire_id wire);
uint32_t odin3_wire_width(const odin3_module *module, odin3_wire_id wire);
odin3_node_id odin3_wire_port_node(const odin3_module *module, odin3_wire_id wire); /* or none */

/* The net at bit `bit` (vector position, LSB first); none when out of range. */
odin3_net_id odin3_wire_net(const odin3_module *module, odin3_wire_id wire, uint32_t bit);

/* The declared index of bit `bit` (see odin3_wire_create); 0 when out of range. */
int32_t odin3_wire_index(const odin3_module *module, odin3_wire_id wire, uint32_t bit);

/* --- aliases and merge (IR-2, IR-14, IR-15) ------------------------------------------------- */

/* A bit of a wire. */
typedef struct odin3_wirebit {
    odin3_wire_id wire;
    uint32_t bit;
} odin3_wirebit;

/* A net's primary (wire, bit); wire none when it has none. */
odin3_wirebit odin3_net_primary(const odin3_module *module, odin3_net_id net);

/*
 * Makes wire bit wb an alias of net: the wire's vector entry at wb.bit becomes net, and the net
 * that held it loses that membership (its primary is cleared, or the alias leaves it). A no-op
 * when wb already holds net. INVALID_ARG for a bit of a port wire (it follows the port node).
 */
odin3_status odin3_wire_add_alias(odin3_module *module, odin3_wirebit wb, odin3_net_id net);

/* One alias of a net: a (wire, bit) membership (wire set, name 0) or a bare name (wire none). */
typedef struct odin3_net_alias {
    odin3_wirebit wb;
    uint32_t name;
} odin3_net_alias;

/*
 * Iterates a net's aliases, most recently added first: start with *cursor = 0 and call until
 * false. The net's own name and primary are not aliases. Do not mutate the module in between.
 */
bool odin3_net_alias_next(const odin3_module *module, odin3_net_id net, uint32_t *cursor,
                          odin3_net_alias *alias);

/* Number of aliases of a net (O(aliases)). */
uint32_t odin3_net_alias_count(const odin3_module *module, odin3_net_id net);

typedef struct odin3_net_pair {
    odin3_net_id keep;
    odin3_net_id drop;
} odin3_net_pair;

/*
 * Merges drop into keep (IR-15): every pin of drop moves to keep (partition kept); every wire
 * vector entry that held drop now holds keep; drop's name, primary (wire, bit) and aliases become
 * aliases of keep, and the name map sends each of those names to keep; drop dies (its name field
 * stays for history, its primary is cleared). Attributes of drop stay with the dead net.
 * INVALID_ARG unless keep and drop are distinct live nets.
 */
odin3_status odin3_net_merge(odin3_module *module, odin3_net_pair pair);

/* --- attributes (IR-10) -------------------------------------------------------------------- */

typedef enum odin3_objkind {
    ODIN3_OBJ_NODE,
    ODIN3_OBJ_NET,
    ODIN3_OBJ_WIRE,
    ODIN3_OBJ_MODULE
} odin3_objkind;

/* An object of a module: kind and ID (for ODIN3_OBJ_MODULE, the module's own ID). */
typedef struct odin3_objref {
    odin3_objkind kind;
    uint32_t id;
} odin3_objref;

/*
 * Sets attribute key_str (a non-empty strtab ID) of a live object to a copy of value (payload
 * copied into the module), replacing any earlier value. INVALID_ARG for a dead or unknown object,
 * a bad key, or a value of unknown kind or with a NULL payload of nonzero length.
 */
odin3_status odin3_attr_set(odin3_module *module, odin3_objref obj, uint32_t key_str,
                            const odin3_value *value);

/*
 * The value of attribute key_str of obj, NULL when it has none (or obj is invalid). The pointer
 * stays readable while the module lives; a later set of the same key does not change it.
 */
const odin3_value *odin3_attr_get(const odin3_module *module, odin3_objref obj, uint32_t key_str);

#endif
