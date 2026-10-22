/*
 * kit.h — the unmap kit (plan Tasks 2 and 3): net classes (N-1), observed nets (N-2), the cell
 * types unmap uses found by name (E-2, E-3), the constant nets, and the replacement transaction
 * um_replace (S-5). Internal to src/passes/unmap.
 */
#ifndef ODIN3_PASSES_UNMAP_KIT_H
#define ODIN3_PASSES_UNMAP_KIT_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct um_ctx um_ctx;

/* N-1: the class of a net. */
typedef enum um_cls { UM_NONE, UM_PAD, UM_GND, UM_VCC, UM_SIG } um_cls;

um_cls um_net_class(const odin3_module *module, odin3_net_id net);

/* N-2: the net has at least one reading pin (a module output port counts). */
bool um_observed(const odin3_module *module, odin3_net_id net);

/* A pin position: port number and bit index (LSB first). */
typedef struct um_pos {
    uint32_t port;
    uint32_t index;
} um_pos;

/* The net on bit `pos.index` of port `pos.port` of `node`; invalid net when the pin is open or out
 * of range. */
odin3_net_id um_port_net(const odin3_module *module, odin3_node_id node, um_pos pos);

/* The types unmap uses; 0 when not registered (hard types) . */
typedef struct um_types {
    odin3_celltype_id adder, multiply, sp_ram, dp_ram;
    odin3_celltype_id add, sub, mul, mem, not_, and_, mux, dff, konst0, konst1;
} um_types;

/*
 * E-2 (shape of the registered hard types: a wrong shape is ODIN3_ERR_INVALID_ARG with
 * `unmap: cell type 'X' does not have the expected ports`) and E-3 (a word or constant type that
 * is missing: `unmap: built-in cell type 'X' is missing`). Hard types that are not registered stay
 * 0 and are not an error.
 */
odin3_status um_types_find(const odin3_design *design, um_types *types);

/*
 * The net of the lowest-ID live `$_CONST0_` / `$_CONST1_` node of uc->module, else a new constant
 * node on a new net (provenance of the module).
 */
odin3_status um_const_net(um_ctx *uc, bool one, odin3_net_id *out);

/* A list of old nodes replaced by one new cell. */
typedef struct um_group {
    odin3_vec nodes; /* odin3_node_id, in insertion order */
} um_group;
void um_group_init(um_group *group);
void um_group_free(um_group *group);
odin3_status um_group_add(um_group *group, odin3_node_id node);

typedef struct um_new {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports; /* one per port of the type */
    odin3_node_id namesake;    /* name and attributes come from this old node */
    const char *rule;          /* value of the `unmap` attribute */
    int64_t origin;            /* value of `unmap_origin`, or 0 for none */
    int64_t w;                 /* value of `unmap_w`, or 0 for none */
} um_new;

/* S-5 steps 1 to 5. *out is the new node. On failure the group is intact. */
odin3_status um_replace(um_ctx *uc, const um_group *group, const um_new *req, odin3_node_id *out);

/* P-3: sets the string attribute `unmap_note` (a declined group) or `unmap_cand` (a candidate set)
 * of node to `text`. */
odin3_status um_set_note(um_ctx *uc, odin3_node_id node, const char *text);
odin3_status um_set_cand(um_ctx *uc, odin3_node_id node, const char *text);

#endif
