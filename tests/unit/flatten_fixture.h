/*
 * flatten_fixture.h — hand-building hierarchy for the 3A unit tests (plan Task 2, point 6): a few
 * lines per module, instance and gate. Test code: it asserts on every IR call instead of returning
 * statuses.
 */
#ifndef ODIN3_TESTS_UNIT_FLATTEN_FIXTURE_H
#define ODIN3_TESTS_UNIT_FLATTEN_FIXTURE_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

#include <stdint.h>

typedef struct ff {
    odin3_design *design;
    odin3_pass_ctx ctx; /* the pass run "flatten", for odin3_flatten_run */
} ff;

/* A cell's type name and (optional) instance name; a parent module and the child it instantiates.
 */
typedef struct ff_cell {
    const char *type;
    const char *name;
} ff_cell;
typedef struct ff_pair {
    odin3_module *parent;
    odin3_module *child;
} ff_pair;

/* A new design with one pass run named "flatten". */
void ff_begin(ff *fix);

/* Destroys the design. */
void ff_end(ff *fix);

uint32_t ff_str(ff *fix, const char *text);

/* A new module (no top is set). */
odin3_module *ff_module(ff *fix, const char *name);

/* A port of the given width, named name; returns the port node. */
odin3_node_id ff_port(ff *fix, odin3_module *mod, const char *name, odin3_dir dir, uint32_t width);

/* The net of bit `bit` of the wire `name` (a port or another wire); asserts it exists. */
odin3_net_id ff_wnet(ff *fix, odin3_module *mod, const char *name, uint32_t bit);

/* A new wire of the width with fresh nets (named like the wire); returns bit 0's net. */
odin3_net_id ff_wire(ff *fix, odin3_module *mod, const char *name, uint32_t width);

/* A new net with the given name (0 = none) and no wire. */
odin3_net_id ff_net(ff *fix, odin3_module *mod, const char *name);

/* A node of a built-in type with width-1 ports only ($_AND_, $_NOT_, ...); nets: one per port, in
 * port order (an id of 0 leaves it open). */
odin3_node_id ff_gate(ff *fix, odin3_module *mod, ff_cell cell, const odin3_net_id *nets);

/* An instance of child in parent; ports: one netvec per port of child, in port order. */
odin3_node_id ff_inst_bus(ff *fix, ff_pair link, const char *name, const odin3_netvec *ports);

/* An instance whose ports are all width 1; nets: one per port (0 = open). */
odin3_node_id ff_inst(ff *fix, ff_pair link, const char *name, const odin3_net_id *nets);

/* The shared constant net of the value (0 or 1) in mod, created on first use. */
odin3_net_id ff_const(ff *fix, odin3_module *mod, int value);

/* Sets an INT attribute on a node, wire or the module (obj kind/id). */
void ff_attr_int(ff *fix, odin3_module *mod, odin3_objref obj, const char *key, int64_t value);

/* Number of live nodes of mod. */
uint32_t ff_live_nodes(const odin3_module *mod);

#endif
