/*
 * tristate.h — the `tristate` pass (Peter 2026-11-04; brief odin3-tristate-pass). Tri-state drivers
 * that elaboration could not resolve inside one module become logic, and the inout ports of the
 * top module are split into inputs and outputs, a WARNING for each conversion.
 *
 *  - A net with `$tribuf` drivers that reaches no port of its module (a bus after `flatten`, or a
 *    tri-state net inside a module below the top) becomes logic. One driver: the enable is dropped
 *    and the net is the data (what Yosys `opt_expr -mux_undef`, and so Parmys, do). Several
 *    drivers: one `$pmux` per net bit, A = x (no driver enabled), B = the data, S = the enables
 *    (the lowering of elab2_trilogic.c, `tribuf -logic`). Exclusive enables are not proved: when
 *    two are high at once the result is the OR of the enabled data (a bus conflict).
 *  - A top-level `inout p` becomes input `p_i` (every read of p), output `p_o` (the data of the
 *    drivers) and output `p_oe` (their enable); an inout nothing drives inside becomes `p_i`
 *    alone, one nothing reads has no `p_i`. The IR cannot remove a port, so `p` stays as an
 *    unconnected shell with the node attribute `tristate_split`; the BLIF writers skip it.
 *  - `--strict` refuses (error) instead of converting: the old D3 behaviour.
 * Tri-states on a top-level output, on a net with a foreign driver, and on ports of modules
 * below the top are left alone (`lower` handles the first, `flatten` resolves the last).
 */
#ifndef ODIN3_PASSES_TRISTATE_TRISTATE_H
#define ODIN3_PASSES_TRISTATE_TRISTATE_H

#include "backends/common/attrs.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ts_opts {
    bool strict;
} ts_opts;

typedef struct ts_types {
    odin3_celltype_id tribuf, pmux, or_gate, const0, constx;
} ts_types;

/* The classes of a net with a $tribuf driver. */
typedef enum ts_net_class {
    TS_NET_INTERNAL, /* reaches no port: becomes logic */
    TS_NET_INOUT,    /* on a $port_inout of the top (port index in the second field) */
    TS_NET_OUT,      /* on a $port_out of the top only: a bus of tri-states there becomes logic */
    TS_NET_LEAVE     /* a port below the top or a foreign driver: left alone */
} ts_net_class;

typedef struct ts_net_info {
    ts_net_class cls;
    uint32_t port; /* the inout port's index, for TS_NET_INOUT */
    bool foreign; /* TS_NET_LEAVE because a driver is not a $tribuf (an instance, a gate, a port) */
} ts_net_info;

/* The ports of a $tribuf, in the cell type's order (a port node has one port, slot A). */
typedef enum ts_slot { TS_SLOT_A = 0, TS_SLOT_EN = 1, TS_SLOT_Y = 2 } ts_slot;

/* A bit of a slot. */
typedef struct ts_slot_at {
    ts_slot slot;
    uint32_t bit;
} ts_slot_at;

/* One bit of a $tribuf that becomes logic. */
typedef struct ts_rec {
    odin3_net_id net; /* Y */
    odin3_net_id data;
    odin3_net_id enable;
    odin3_node_id node;
} ts_rec;

typedef struct ts_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *module;
    const ts_opts *opts;
    ts_types types;
    bool is_top;
    odin3_net_id konst[2]; /* module-local constant nets: 0, x */
    uint64_t converted, split;
} ts_ctx;

/* Runs the pass over every module. */
odin3_status ts_run(odin3_pass_ctx *pass, odin3_design *design, const ts_opts *opts);

/* The net on bit `bit` of a slot of the node. */
odin3_net_id ts_slot_net(const ts_ctx *tc, odin3_node_id node, ts_slot_at at);

/* True when the node is a live $tribuf of the module. */
bool ts_is_tribuf(const ts_ctx *tc, odin3_node_id node);

/* What the net is (see ts_net_class). */
ts_net_info ts_classify(const ts_ctx *tc, odin3_net_id net);

/* The constant net of the module: bit 0 (value 0) or 1 (value x); created on first use. */
odin3_status ts_const(ts_ctx *tc, unsigned which, odin3_net_id *out);

/* A new node of the type with the given params and port nets, derived from the nodes' records. */
typedef struct ts_make {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports;
} ts_make;
odin3_status ts_make_node(ts_ctx *tc, const ts_make *mk, const odin3_node_id *from, size_t n_from);

/* One located warning about a node's net or port: "<kind>: <what> (<file:line>): <text>". */
void ts_warn(const ts_ctx *tc, odin3_node_id at, const char *kind, const char *what,
             const char *text);

/* The name of a net for messages: wire[bit] or $n<id>. */
void ts_net_label(const ts_ctx *tc, odin3_net_id net, char *out, size_t size);

/* ndrv >= 2 drivers of ynet (recs in source order) become a $pmux whose output is ynet. */
odin3_status ts_pmux(ts_ctx *tc, const ts_rec *recs, uint32_t ndrv, odin3_net_id ynet);

/* *out = the OR of the enables of the ndrv >= 1 drivers. */
odin3_status ts_or_enables(ts_ctx *tc, const ts_rec *recs, uint32_t ndrv, odin3_net_id *out);

/* Phase 1: the internal nets (tristate.c). Phase 2: the top's inout ports (split.c). */
odin3_status ts_convert_internal(ts_ctx *tc);
odin3_status ts_split_inouts(ts_ctx *tc);

#endif
