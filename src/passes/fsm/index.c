/*
 * index.c — the per-module net index (index.h).
 */
#include "passes/fsm/index.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsmdata.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { CONST_PREFIX_LEN = 7 };

/* How the index treats a node. */
typedef enum node_kind {
    KIND_SKIP,  /* dead, a constant node or an unconnected one */
    KIND_PORT,  /* a module port node */
    KIND_KNOWN, /* an internal cell of Yosys */
    KIND_OTHER  /* an instance, a black box or a hard cell */
} node_kind;

static node_kind kind_of(const odin3_design *design, const odin3_module *module,
                         odin3_node_id node) {
    if (!odin3_node_live(module, node)) {
        return KIND_SKIP;
    }
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(module, node));
    if (def == NULL) {
        return KIND_SKIP;
    }
    if (def->gran == ODIN3_GRAN_PORT) {
        return KIND_PORT;
    }
    if (strncmp(def->name, "$_CONST", CONST_PREFIX_LEN) == 0) {
        return KIND_SKIP;
    }
    bool internal =
        def->name[0] == '$' && (def->gran == ODIN3_GRAN_WORD || def->gran == ODIN3_GRAN_BIT);
    return internal ? KIND_KNOWN : KIND_OTHER;
}

static uint32_t ref_flags(odin3_dir dir, node_kind kind) {
    uint32_t flags = kind == KIND_OTHER ? (uint32_t)ODIN3_FSM_REF_UNKNOWN : 0U;
    if (dir == ODIN3_DIR_IN || dir == ODIN3_DIR_INOUT) {
        flags |= ODIN3_FSM_REF_IN;
    }
    if (dir == ODIN3_DIR_OUT || dir == ODIN3_DIR_INOUT) {
        flags |= ODIN3_FSM_REF_OUT;
    }
    return flags;
}

void odin3_fsm_index_free(odin3_fsm_index *ix) {
    if (ix == NULL) {
        return;
    }
    odin3_util_free(ix->off);
    odin3_util_free(ix->refs);
    odin3_util_free(ix->net_const);
    odin3_util_free(ix->net_port);
    memset(ix, 0, sizeof *ix);
}

static void fill_types(const odin3_design *design, odin3_fsm_types *types) {
    types->dff = odin3_kit_type_named(design, "$dff").v;
    types->adff = odin3_kit_type_named(design, "$adff").v;
    types->mux = odin3_kit_type_named(design, "$mux").v;
    types->pmux = odin3_kit_type_named(design, "$pmux").v;
    types->logic_not = odin3_kit_type_named(design, "$logic_not").v;
    types->ne = odin3_kit_type_named(design, "$ne").v;
    types->eq = odin3_kit_type_named(design, "$eq").v;
    types->reduce_or = odin3_kit_type_named(design, "$reduce_or").v;
    types->reduce_bool = odin3_kit_type_named(design, "$reduce_bool").v;
    types->reduce_and = odin3_kit_type_named(design, "$reduce_and").v;
}

/* Marks the nets of the port nodes and the constant nets. */
static void classify_nets(odin3_fsm_index *ix) {
    const odin3_module *module = ix->module;
    for (uint32_t id = 1; id < ix->net_end; id++) {
        odin3_net_id net = {id};
        ix->net_const[id] = ODIN3_FSM_NO_CONST;
        if (!odin3_net_live(module, net)) {
            continue;
        }
        switch (odin3_net_const_value(module, net)) {
        case ODIN3_CONST_0:
            ix->net_const[id] = ODIN3_FSM_BIT_0;
            break;
        case ODIN3_CONST_1:
            ix->net_const[id] = ODIN3_FSM_BIT_1;
            break;
        case ODIN3_CONST_X:
            ix->net_const[id] = ODIN3_FSM_BIT_X;
            break;
        case ODIN3_CONST_Z:
            ix->net_const[id] = ODIN3_FSM_BIT_Z;
            break;
        case ODIN3_CONST_NONE:
        default:
            break;
        }
    }
    for (uint32_t id = 1; id < ix->node_end; id++) {
        odin3_node_id node = {id};
        if (kind_of(ix->design, module, node) != KIND_PORT) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(module, node);
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_net_id net = odin3_pin_net(module, (odin3_pin_id){pins.first.v + k});
            ix->net_port[net.v] = 1;
        }
    }
}

/* One pass over the pins of the reference-bearing nodes: counts (fill false) or stores them. */
static void scan_refs(odin3_fsm_index *ix, bool fill, uint32_t *cursor) {
    const odin3_module *module = ix->module;
    for (uint32_t id = 1; id < ix->node_end; id++) {
        odin3_node_id node = {id};
        node_kind kind = kind_of(ix->design, module, node);
        if (kind != KIND_KNOWN && kind != KIND_OTHER) {
            continue;
        }
        odin3_pinslice pins = odin3_node_pins(module, node);
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_pin_id pin = {pins.first.v + k};
            odin3_net_id net = odin3_pin_net(module, pin);
            if (net.v == 0 || ix->net_const[net.v] != ODIN3_FSM_NO_CONST) {
                continue;
            }
            if (!fill) {
                ix->off[net.v + 1]++;
                continue;
            }
            odin3_fsm_ref *ref = &ix->refs[cursor[net.v]++];
            ref->node = id;
            ref->port = odin3_pin_port(module, pin);
            ref->flags = ref_flags(odin3_pin_dir(module, pin), kind);
        }
    }
}

odin3_status odin3_fsm_index_build(odin3_design *design, odin3_module *module,
                                   odin3_fsm_index *ix) {
    memset(ix, 0, sizeof *ix);
    ix->design = design;
    ix->module = module;
    ix->net_end = odin3_module_net_end(module);
    ix->node_end = odin3_module_node_end(module);
    fill_types(design, &ix->types);
    ix->off = odin3_util_calloc(((size_t)ix->net_end + 1) * sizeof *ix->off);
    ix->net_const = odin3_util_calloc((size_t)ix->net_end + 1);
    ix->net_port = odin3_util_calloc((size_t)ix->net_end + 1);
    uint32_t *cursor = odin3_util_calloc(((size_t)ix->net_end + 1) * sizeof *cursor);
    if (ix->off == NULL || ix->net_const == NULL || ix->net_port == NULL || cursor == NULL) {
        odin3_util_free(cursor);
        odin3_fsm_index_free(ix);
        return ODIN3_ERR_NO_MEMORY;
    }
    classify_nets(ix);
    scan_refs(ix, false, NULL);
    for (uint32_t net = 1; net <= ix->net_end; net++) {
        ix->off[net] += ix->off[net - 1];
    }
    for (uint32_t net = 0; net < ix->net_end; net++) {
        cursor[net] = ix->off[net];
    }
    ix->refs = odin3_util_calloc(((size_t)ix->off[ix->net_end] + 1) * sizeof *ix->refs);
    if (ix->refs == NULL) {
        odin3_util_free(cursor);
        odin3_fsm_index_free(ix);
        return ODIN3_ERR_NO_MEMORY;
    }
    scan_refs(ix, true, cursor);
    odin3_util_free(cursor);
    return ODIN3_OK;
}

odin3_fsm_refs odin3_fsm_ix_refs(const odin3_fsm_index *ix, uint32_t net) {
    odin3_fsm_refs span = {NULL, 0};
    if (net != 0 && net < ix->net_end) {
        span.refs = ix->refs + ix->off[net];
        span.count = ix->off[net + 1] - ix->off[net];
    }
    return span;
}

bool odin3_fsm_ix_port_net(const odin3_fsm_index *ix, uint32_t net) {
    return net != 0 && net < ix->net_end && ix->net_port[net] != 0;
}

odin3_fsm_sig odin3_fsm_ix_sig(const odin3_fsm_index *ix, uint32_t net) {
    if (net == 0) {
        return odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_X);
    }
    if (net < ix->net_end && ix->net_const[net] != ODIN3_FSM_NO_CONST) {
        return odin3_fsm_sig_of_bit(ix->net_const[net]);
    }
    return net;
}

odin3_fsm_sig odin3_fsm_ix_pin_sig(const odin3_fsm_index *ix, odin3_pin_id pin) {
    return odin3_fsm_ix_sig(ix, odin3_pin_net(ix->module, pin).v);
}

odin3_status odin3_fsm_ix_port_sigs(const odin3_fsm_index *ix, odin3_node_id node, uint32_t port,
                                    odin3_vec *out) {
    odin3_pinslice pins = odin3_node_port(ix->module, node, port);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_fsm_sig *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = odin3_fsm_ix_pin_sig(ix, (odin3_pin_id){pins.first.v + k});
    }
    return ODIN3_OK;
}

uint32_t odin3_fsm_ix_type(const odin3_fsm_index *ix, uint32_t node) {
    if (node == 0 || node >= ix->node_end || !odin3_node_live(ix->module, (odin3_node_id){node})) {
        return 0;
    }
    return odin3_node_type(ix->module, (odin3_node_id){node}).v;
}
