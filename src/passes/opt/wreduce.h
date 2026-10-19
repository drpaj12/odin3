/*
 * wreduce.h — wreduce's internals (3G spec WR-1..WR-11): the rule counters, the worker state shared
 * by wreduce.c (pre-step, operators, worker) and wreduce_ff.c (muxes, flip-flops, latches), and
 * the rewrite machinery. Not part of any ABI.
 *
 * A rewrite that narrows an output (wr_commit) builds the narrower cell first (kit_new_cell, on
 * fresh output nets), then makes the one destructive step, kit_connect: the old cell goes and each
 * old output net is merged into the net ws->ysrc names for it (the new cell's net, a replacement,
 * or the old net itself for a bit that is left undriven because nothing reads it). A rewrite that
 * keeps the output width goes through kit_rebuild_node instead (wr_rebuild). Failures before the
 * destructive step leave the IR as it was; after it, the kit's half-done path.
 */
#ifndef ODIN3_PASSES_OPT_WREDUCE_H
#define ODIN3_PASSES_OPT_WREDUCE_H

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* One counter per row of spec 4 and the pre-step; the names are `--opt-stats`'s "wreduce.<name>".
 */
typedef enum odin3_wr_rule {
    WR_RULE_ONE_BIT,
    WR_RULE_DIVMOD_TRIM,
    WR_RULE_MEM_ADDR,
    WR_RULE_PORT_A,
    WR_RULE_PORT_B,
    WR_RULE_TO_SIGNED,
    WR_RULE_TO_UNSIGNED,
    WR_RULE_Y_UNUSED,
    WR_RULE_Y_CAP,
    WR_RULE_OP_REMOVED,
    WR_RULE_MUX_NARROW,
    WR_RULE_MUX_REMOVED,
    WR_RULE_FF_ZERO_EXT,
    WR_RULE_FF_SIGN_EXT,
    WR_RULE_FF_UNUSED,
    WR_RULE_FF_REMOVED,
    WR_N_RULES
} odin3_wr_rule;

_Static_assert((uint32_t)WR_N_RULES <= (uint32_t)ODIN3_WREDUCE_MAX_RULES,
               "every wreduce rule has a counter in odin3_wreduce_counts");

/* The name of rule (0 .. WR_N_RULES - 1), NULL beyond. */
const char *odin3_wr_rule_name(uint32_t rule);

/* The cell types wreduce looks at, in the order of its name table. */
typedef enum wr_type {
    WT_NOT,
    WT_NEG,
    WT_AND,
    WT_OR,
    WT_XOR,
    WT_XNOR,
    WT_SHL,
    WT_SHR,
    WT_SSHR,
    WT_SHIFTX,
    WT_LT,
    WT_LE,
    WT_EQ,
    WT_NE,
    WT_GE,
    WT_GT,
    WT_ADD,
    WT_SUB,
    WT_MUL,
    WT_MUX,
    WT_PMUX,
    WT_DFF,
    WT_DFFE,
    WT_ADFF,
    WT_ADFFE,
    WT_SDFF,
    WT_SDFFE,
    WT_SDFFCE,
    WT_DLATCH,
    WT_ADLATCH,
    WT_REDUCE_AND,
    WT_REDUCE_OR,
    WT_REDUCE_XOR,
    WT_REDUCE_XNOR,
    WT_REDUCE_BOOL,
    WT_LOGIC_NOT,
    WT_LOGIC_AND,
    WT_LOGIC_OR,
    WT_DIV,
    WT_MOD,
    WT_POW,
    WT_MEM,
    WT_COUNT,
    WT_NONE = WT_COUNT
} wr_type;

/* The worker state of one module. */
typedef struct wr_state {
    odin3_opt_ctx *oc;
    odin3_module *m;
    const odin3_opt_opts *opts;
    odin3_wreduce_counts *cnt;
    odin3_celltype_id types[WT_COUNT];
    odin3_vec marks; /* uint32_t net IDs: the nets this round changed (Yosys's work_queue_bits) */
    odin3_vec fwd;   /* uint32_t per net: the net it was merged into, 0 for none */
    odin3_vec rank;  /* uint32_t per node: its place in Yosys's cell order (wr_order_ranks) */
    odin3_vec work;  /* uint64_t: (rank << 32 | node) of this round, sorted */
    odin3_vec nets;  /* odin3_net_id scratch: the pins of the cell being looked at, in pin order */
    odin3_vec nets2; /* odin3_net_id scratch: a rebuilt port that is not a prefix (mux B) */
    odin3_vec ysrc;  /* odin3_net_id: what each old output net becomes (wr_commit) */
    odin3_vec olds;  /* odin3_net_id: the old output nets */
    odin3_vec init;  /* odin3_net_id: old Q nets whose `init` goes with the commit */
    odin3_vec bits;  /* uint8_t scratch: a truncated reset value */
    uint32_t visited_bits; /* bits the current visit removed, for the "cells narrowed" count */
} wr_state;

/* A rewrite of one node (wr_commit). */
typedef struct wr_rewrite {
    odin3_node_id node;
    const odin3_kit_cell *cell; /* the replacement; NULL: the node goes */
    uint32_t out_port;          /* the old node's only output port (Y, Q) */
    uint32_t n_keep;            /* the low output bits the replacement has */
    uint32_t n_pad;             /* ysrc[n_keep .. n_keep + n_pad) become the last kept net */
    odin3_node_id made;         /* out: the replacement */
} wr_rewrite;

/* --- bits ---------------------------------------------------------------------------------- */

/* The constant net carries (ODIN3_CONST_NONE for none). */
odin3_const wr_const(const wr_state *ws, odin3_net_id net);
/* The same net, or two constants of one value (Yosys compares SigBits). */
bool wr_same(const wr_state *ws, odin3_net_id lhs, odin3_net_id rhs);
bool wr_is_const(const wr_state *ws, odin3_net_id net, odin3_const value);
/* Yosys's `!is_output && ports <= 1 && !keep`: no pin but the driver's, no `keep` wire. */
bool wr_unused(const wr_state *ws, odin3_net_id net);

/* --- the worker ---------------------------------------------------------------------------- */

wr_type wr_type_of(const wr_state *ws, odin3_node_id node);
/* Adds nets[from .. to) to the nets the round changed. NO_MEMORY with nothing added. */
odin3_status wr_mark(wr_state *ws, const odin3_net_id *nets, uint32_t count);
/* Puts the nets of all pins of node into ws->nets, port after port; *complete is false when a pin
 * is unconnected. */
odin3_status wr_gather(wr_state *ws, odin3_node_id node, bool *complete);
/* ws->ysrc and ws->olds = the nets of the node's output port (the defaults of a commit). */
odin3_status wr_ysrc_reset(wr_state *ws, odin3_node_id node, uint32_t out_port);
odin3_status wr_commit(wr_state *ws, wr_rewrite *rw);
/* kit_rebuild_node with wreduce's bookkeeping (cell order); *out is the new node. */
odin3_status wr_rebuild(wr_state *ws, const odin3_kit_rebuild *req, odin3_node_id *out);
/* DEBUG "wreduce: module 'm': removed top N bits (of W) from <what> <type> '<cell>'". */
void wr_log_removed(const wr_state *ws, odin3_node_id node, uint32_t count, uint32_t of,
                    const char *what);
/* Counts a rule hit. */
void wr_count(wr_state *ws, odin3_wr_rule rule);
/* Copies the parameters of node (at most 16) into out; returns how many there are. */
uint32_t wr_params(const wr_state *ws, odin3_node_id node, odin3_value *out);
const char *wr_module_name(const wr_state *ws);
/* The label of a node for a log line, the type's name. */
void wr_label(const wr_state *ws, odin3_node_id node, char *out, size_t size);
const char *wr_type_name(const wr_state *ws, odin3_node_id node);

/* wreduce_order.c: ws->rank[node] = the node's place in a post-order of the net graph (operand
 * before consumer, WR-7); ranks are 1..N, ws->rank must already hold one entry per node (else
 * INVALID_ARG). A node with no rank of its own (made later, never given one by wr_rebuild) keeps
 * UINT32_MAX: it sorts after every ranked node, by node ID. */
odin3_status wr_order_ranks(wr_state *ws);
/* True for a storage type (flip-flop, latch, memory): its outputs are not followed by the walk. */
bool wr_order_is_source(const char *type);
/* The index-th port (of n_ports) the walk reads for `type`: registry order, but `$mux` S, B, A. */
uint32_t wr_order_port(const char *type, uint32_t n_ports, uint32_t index);
/* Bytes per node the walk needs besides the rank table: `seen` (1) and the stack (8). */
enum { WR_ORDER_BYTES_PER_NODE = 9 };

/* wreduce_op.c: the pre-step (spec 3, step 1) and §4.3 for one node. */
odin3_status wr_prestep(wr_state *ws, odin3_node_id node);
odin3_status wr_run_op(wr_state *ws, odin3_node_id node);

/* wreduce_ff.c: §4.1 and §4.2. */
odin3_status wr_run_mux(wr_state *ws, odin3_node_id node);
odin3_status wr_run_ff(wr_state *ws, odin3_node_id node);

enum { WR_LABEL_MAX = 64, WR_MAX_PARAMS = 16, WR_MAX_PORTS = 16 };

#endif
