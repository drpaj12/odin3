/*
 * kit.h — the opt family's rewrite kit (2F spec OP-2, OP-15; plan Task 1): the opt context, the
 * canonical constant nets, kit_connect (Yosys's remove + connect), kit_new_cell, kit_rebuild_node
 * (Yosys's setPort/setParam/type change), provenance per rewrite, the invert map, topological
 * order, `keep` and reader counts. Every sub-pass reaches the IR through it.
 *
 * Failures. ODIN3_ERR_INVALID_ARG (logged "opt: <call>: ...") is a kit misuse and changes nothing.
 * ODIN3_ERR_NO_MEMORY while a rewrite builds what it needs (provenance, fresh nets, the new node,
 * its attributes) leaves the rewrite undone: whatever the build made is removed again. A rewrite's
 * first destructive step is one atomic IR call (odin3_node_delete or odin3_node_replace); the
 * merges that follow kit_rebuild_node's are into nets that just lost a pin and add no alias, so
 * they find room already. kit_connect's merges and canon_consts' merge can still run out of
 * memory (the IR has no way to reserve them): the IR is then valid, the bits merged so far
 * connected and the others undriven, so the module may no longer compute what it did (canon: a
 * reader of the dropped constant reads an undriven net). Any failure after a rewrite's destructive
 * step logs "opt: out of memory in the middle of a rewrite in module '<m>': the design no longer
 * matches its input; discard it"; the caller must then discard the design. Undoing is best effort:
 * an undone rebuild may leave an unused DERIVED record and count its pins in pins_created, and
 * under repeated failures its new node may stay live on fresh, unread nets (the IR valid, the
 * function kept).
 */
#ifndef ODIN3_PASSES_OPT_KIT_H
#define ODIN3_PASSES_OPT_KIT_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/opt/opt.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* The number of constant values (and constant cell types): 0, 1, x, z, indexed by odin3_bit. */
enum { ODIN3_OPT_N_CONST = 4 };

/*
 * One sub-pass call on one module. counts is set by the caller after init. changed is Yosys's
 * did_something: every mutating kit call sets it. const_net[b] caches the canonical constant net
 * of odin3_bit b (0: not looked up yet). invert holds one odin3_net_id per net ID, 0 for none
 * (odin3_kit_invert_map). mod is the opt session's state of this module (dirty.h; NULL outside a
 * session): the kit notes every rewrite in it (odin3_kit_note_*), opt_expr drains it.
 * pins_created counts the pins kit_new_cell and kit_rebuild_node created (the budget check).
 * ties and defer_ties are opt_dff's (Task 11): see odin3_kit_connect and odin3_kit_flush_ties.
 */
typedef struct odin3_opt_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *module;
    const odin3_opt_opts *opts;
    odin3_opt_counts *counts;
    bool changed;
    odin3_net_id const_net[ODIN3_OPT_N_CONST];
    odin3_vec invert;
    struct odin3_opt_modstate *mod;
    uint64_t pins_created;
    odin3_vec ties;
    bool defer_ties;
} odin3_opt_ctx;

struct odin3_opt_modstate;

/* Zeroes *oc and fills it; allocates nothing. INVALID_ARG (logged) for a NULL argument. */
odin3_status odin3_opt_ctx_init(odin3_opt_ctx *oc, odin3_pass_ctx *pass, odin3_module *module,
                                const odin3_opt_opts *opts);

/* Frees the context's vectors; NULL is a no-op. */
void odin3_opt_ctx_free(odin3_opt_ctx *oc);

/*
 * OP-2's constant canonicalisation, run at the start of each sub-pass call per module: per value,
 * the lowest-ID live $_CONST0_/$_CONST1_/$_CONSTX_/$_CONSTZ_ node whose Y is on a net is kept (and
 * cached in const_net); every later one with a net is deleted and its net merged into the kept
 * one (keep = the kept net). A constant node with Y unconnected is left for opt_clean. Creates
 * nothing; constant $sop nodes are untouched. Does not set changed: Yosys's constants are canonical
 * already, so this is no rewrite of its (agent default). NO_MEMORY as kit_connect.
 */
odin3_status odin3_opt_canon_consts(odin3_opt_ctx *oc);

/*
 * *out = the canonical net of constant bit (odin3_bit 0, 1, x, z): the cached net while it is live
 * and still carries the constant, else the first live constant node of that value with a net in
 * ID order, else a new constant node on a new unnamed net (both with the module's record, as
 * proc's). Does not set changed. INVALID_ARG (logged) for a value that is not an odin3_bit;
 * NO_MEMORY with nothing created.
 */
odin3_status odin3_opt_const_net(odin3_opt_ctx *oc, odin3_bit bit, odin3_net_id *out);

/* kit_connect's request: output port `port` of node, connected to src[0 .. width) (LSB first). */
typedef struct odin3_kit_conn {
    odin3_node_id node;
    uint32_t port;
    const odin3_net_id *src;
    uint32_t width;
} odin3_kit_conn;

/*
 * Yosys's module->remove(cell) + module->connect(Y, src) (OP-2): deletes node, then merges, bit by
 * bit, the net of output pin k into src[k] (keep = src[k]); an unconnected pin merges nothing, and
 * a pin already on src[k] (keep == drop: the output fed back to its own input) leaves that net
 * undriven. A src net that an earlier bit merged away stands for the net it went into. The node
 * must have exactly one output port, `port`, of direction OUT, and be neither a port node nor a
 * $proc; every src[k] must be a live net and width the port's width; else INVALID_ARG (a kit
 * misuse, nothing changed). Sets changed.
 *
 * With oc->defer_ties the merges are not made: node is deleted and the pairs (keep = src[k],
 * drop = the output net) are queued in oc->ties, to be made by odin3_kit_flush_ties. A caller
 * that reads nets after a tie in the same pass then sees the net as it was before the tie, which
 * is Yosys's SigMap (built once per worker, module->connect does not update it). The queue is
 * reserved before the delete, so a NO_MEMORY leaves the IR unchanged.
 */
odin3_status odin3_kit_connect(odin3_opt_ctx *oc, const odin3_kit_conn *conn);

/*
 * The deferred merges (oc->defer_ties). reserve_ties makes room for n more (NO_MEMORY, nothing
 * queued); defer_tie queues one pair, keep = src and drop = the net that stands for it, into that
 * room (it never fails; no room is a misuse and the pair is dropped). flush_ties makes the queued
 * merges in order and empties the queue: a net an earlier merge of the flush removed stands for
 * the net it went into (as in kit_connect), a pair with a net that is gone or whose two sides
 * are one net merges nothing. NO_MEMORY before the first merge leaves the queue as it was; a
 * failing merge stops the flush with the rest queued (IR valid, partly tied: the caller treats it
 * as a half-done rewrite).
 */
odin3_status odin3_kit_reserve_ties(odin3_opt_ctx *oc, size_t extra);
void odin3_kit_defer_tie(odin3_opt_ctx *oc, odin3_net_id keep, odin3_net_id drop);
odin3_status odin3_kit_flush_ties(odin3_opt_ctx *oc);

/*
 * A cell to create: its type and parameters (as odin3_node_spec: NULL params are the defaults),
 * one netvec per port in port order, and its record. An output entry with nets NULL gets fresh
 * unnamed nets with prov (its count is ignored: the port's width for the parameters).
 */
typedef struct odin3_kit_cell {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports;
    odin3_prov_id prov;
} odin3_kit_cell;

/*
 * Creates an unnamed node for cell, connected as given (odin3_node_create_connected), *out = the
 * node. Every ODIN3_OPT_BUDGET_PINS created pins (pins_created crossing a multiple), before
 * anything is made, checks the design budget: over it, logs odin3_budget_message's text
 * ("<pass> needs more than N bytes (the design's budget is 60% of physical memory)") as an
 * unlocated error and returns ODIN3_ERR_PARSE. INVALID_ARG as create_connected (logged); NO_MEMORY
 * with nothing made. Sets changed.
 */
odin3_status odin3_kit_new_cell(odin3_opt_ctx *oc, const odin3_kit_cell *cell, odin3_node_id *out);

/*
 * kit_rebuild_node's request. cell.ports may be NULL: every input port then takes old_node's nets
 * (the port widths must agree) and every output is fresh. Output entries must be nets NULL or
 * exactly the nets of old_node's paired output: outputs pair in order, not by port index ($adff's
 * Q, port 3, pairs with $adffe's Q, port 4). cell.prov 0 asks for a DERIVED record with parent
 * [old_node's record] in the current operation; when none has been begun one is, which hides a
 * caller that forgot odin3_kit_begin_rewrite (two rewrites then share an operation, against OP-15):
 * callers begin one per rewrite. A record-less old node gives prov 0.
 */
typedef struct odin3_kit_rebuild {
    odin3_node_id old_node;
    odin3_kit_cell cell;
} odin3_kit_rebuild;

/*
 * Rebuilds old_node as req->cell (OP-2 kit_rebuild, plan point (p)); *out = the new node.
 * - Same port signature (the same type, the same port widths, and inputs on old_node's nets):
 *   odin3_node_create, then odin3_node_replace (each new pin takes the old pin's place).
 * - Otherwise: the new node on fresh output nets, old_node deleted, then each fresh output net
 *   merged into old_node's output net at the same place (keep = the old net, which keeps its names
 *   and record); output ports are paired in order and must have equal widths, else INVALID_ARG.
 * Either way the new node takes old_node's name and attributes (first-set order). old_node must
 * be live and neither a port node nor a $proc (INVALID_ARG). Errors otherwise as kit_new_cell's
 * (the budget included). Sets changed.
 */
odin3_status odin3_kit_rebuild_node(odin3_opt_ctx *oc, const odin3_kit_rebuild *req,
                                    odin3_node_id *out);

/*
 * Copies the attributes of node `from` onto node `to`, in first-set order (what kit_rebuild_node
 * does for the node it rebuilds; a rewrite that builds its replacement with kit_new_cell calls it
 * too). NO_MEMORY (some attributes may have been copied).
 */
odin3_status odin3_kit_copy_attrs(odin3_module *module, odin3_node_id from, odin3_node_id to);

/*
 * The registered cell type named name, none ({0}) when the design has none; no allocation, no log.
 */
odin3_celltype_id odin3_kit_type_named(const odin3_design *design, const char *name);

/*
 * Passes st through; when it is not OK, logs the "out of memory in the middle of a rewrite in
 * module '<m>': the design no longer matches its input; discard it" line (the failure came after
 * the rewrite's destructive step). For callers that make such steps themselves (ff.c, dff.c).
 */
odin3_status odin3_kit_half_done(const odin3_opt_ctx *oc, odin3_status st);

/* Appends net to vec (elements odin3_net_id). NO_MEMORY with vec unchanged. */
odin3_status odin3_kit_push_net(odin3_vec *vec, odin3_net_id net);

/* qsort comparison of two uint64_t, ascending. */
int odin3_kit_cmp_u64(const void *lhs, const void *rhs);

/* "Not found" of odin3_kit_port_index and odin3_kit_param_index; odin3_node_port and
 * odin3_node_param accept it (an empty slice, NULL). */
#define ODIN3_KIT_NONE UINT32_MAX

/* The index of the port (parameter) of def called name, ODIN3_KIT_NONE when it has none. */
uint32_t odin3_kit_port_index(const odin3_celltype_def *def, const char *name);
uint32_t odin3_kit_param_index(const odin3_celltype_def *def, const char *name);

/*
 * The vector helpers below work on odin3_vec of 32- and 64-bit words and sit here, not in
 * util/vec.h, because they are specific to the opt passes' side tables (net IDs, keys, indices);
 * they allocate through odin3_vec, so NO_MEMORY leaves the vec unchanged.
 */
/* Appends value to vec (elements uint32_t / uint64_t). NO_MEMORY with vec unchanged. */
odin3_status odin3_kit_push_u32(odin3_vec *vec, uint32_t value);
odin3_status odin3_kit_push_u64(odin3_vec *vec, uint64_t value);

/* Drops elements from the end of vec until len elements are left (no-op when it has fewer). */
void odin3_kit_truncate(odin3_vec *vec, size_t len);

/*
 * Sorts vec[from ..) (elements uint64_t) and drops the repeats (Yosys's sort_and_unify).
 * Requires from <= vec->len (a larger from would wrap the count).
 */
void odin3_kit_sort_unify(odin3_vec *vec, size_t from);

/*
 * Writes the label of node for a log line into out (size bytes, truncated): its name, else
 * "$c<ID>" (spec section 16).
 */
void odin3_kit_node_label(const odin3_opt_ctx *oc, odin3_node_id node, char *out, size_t size);

/*
 * The same label from a name string read earlier (0 = unnamed), for a log line written after the
 * node is gone: name, else "$c<ID>" of node.
 */
void odin3_kit_name_label(const odin3_opt_ctx *oc, uint32_t name, odin3_node_id node, char *out,
                          size_t size);

/* Begins the next provenance operation (odin3_prov_begin_op): one per rewrite (OP-15). */
void odin3_kit_begin_rewrite(odin3_opt_ctx *oc);

/*
 * *out = the DERIVED record of the current operation whose parents are the records of
 * parents[0 .. n), in order (OP-15); nodes without a record are left out, and *out is 0 when none
 * has one. INVALID_ARG as odin3_prov_derive (no operation begun); NO_MEMORY.
 */
odin3_status odin3_kit_prov(const odin3_opt_ctx *oc, const odin3_node_id *parents, uint32_t n,
                            odin3_prov_id *out);

/*
 * Snapshot of Yosys opt_expr's invert map into oc->invert (sized to the net end, 0 = none): Y -> A
 * for every live $not, $logic_not and $_NOT_ with a 1-bit A and Y, and Y -> S for every live
 * 1-bit $mux or $_MUX_ whose A reads constant 1 and B constant 0 (odin3_net_const_value). Later
 * rewrites do not update it. NO_MEMORY.
 */
odin3_status odin3_kit_invert_map(odin3_opt_ctx *oc);

/* The net whose inverse net is (from the last odin3_kit_invert_map), none when it has none. */
odin3_net_id odin3_kit_inverse(const odin3_opt_ctx *oc, odin3_net_id net);

/*
 * Kahn's topological order over the live evaluable nodes (Yosys's cell_evaluable set, restricted
 * to the registry: the combinational word cells, $lut, $sop and the gates), an edge from the driver
 * of each input net to its reader, ties by lowest node ID (a min-heap). order (elements
 * odin3_node_id) is cleared and receives the nodes. On a loop *sorted is false and order holds the
 * evaluable nodes in ID order. NO_MEMORY (order then empty).
 */
odin3_status odin3_kit_topo(const odin3_opt_ctx *oc, odin3_vec *order, bool *sorted);

/*
 * True when obj has attribute `keep` with a non-zero value (Yosys get_bool_attribute: an INT other
 * than 0, BITS with a 1 bit, a non-empty STRING).
 */
bool odin3_kit_keep(const odin3_opt_ctx *oc, odin3_objref obj);

/* The pins that read net: its sinks and its inout pins. */
uint32_t odin3_kit_readers(const odin3_opt_ctx *oc, odin3_net_id net);

/* --- the dirty set (Task 13; dirty.c) ---------------------------------------------------- */

/*
 * Notes for opt_expr's incremental mode. They never fail and do nothing outside an opt session
 * (oc->mod NULL). Every rewrite of the kit makes its own; a sub-pass that changes the IR by its
 * own calls (a direct node delete, a pin moved, a net merge) calls them itself.
 * - note_change: the module changed (the iteration's per-module flag; no marks).
 * - note_node: node was built, or had its pins moved; it and the readers of its outputs get
 *   marked (call it when the node's output nets are in place).
 * - note_net: net is about to be merged into another or re-driven; its readers, and the readers
 *   of theirs, get marked (call it before the merge, while the readers are on net).
 */
void odin3_kit_note_change(odin3_opt_ctx *oc);
void odin3_kit_note_node(odin3_opt_ctx *oc, odin3_node_id node);
void odin3_kit_note_net(odin3_opt_ctx *oc, odin3_net_id net);
/* note_net on every output net of node (call it before the node is deleted). */
void odin3_kit_note_outputs(odin3_opt_ctx *oc, odin3_node_id node);

/* The session state of module (NULL outside a session); odin3_opt_ctx_init calls it. */
struct odin3_opt_modstate *odin3_opt_modstate_of(const odin3_design *design,
                                                 const odin3_module *module);

/*
 * opt_expr's reads, per round kind (x_round: the round that consumes x bits). incremental: the
 * session keeps marks and the module has been swept once, so only marked nodes need a visit.
 * pending: some node is marked for the round (always true when not incremental). take: true when
 * node is to be visited in the round, clearing its mark for it (always true when not
 * incremental); peek is take without clearing. settle: opt_expr reached its fixed point on the
 * module, so no node is dirty any more.
 */
bool odin3_opt_dirty_incremental(const odin3_opt_ctx *oc);
bool odin3_opt_dirty_pending(const odin3_opt_ctx *oc, bool x_round);
bool odin3_opt_dirty_take(odin3_opt_ctx *oc, odin3_node_id node, bool x_round);
bool odin3_opt_dirty_peek(const odin3_opt_ctx *oc, odin3_node_id node, bool x_round);
void odin3_opt_dirty_settle(odin3_opt_ctx *oc);
/* Marks module as needing a full sweep again (the verification pass of Debug builds). */
void odin3_opt_dirty_sweep_all(odin3_opt_ctx *oc);
/* The bytes the module's marks hold (the budget check's side arrays). */
uint64_t odin3_opt_dirty_bytes(const odin3_opt_ctx *oc);

#endif
