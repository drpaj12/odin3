/*
 * dff.h — opt_dff's internals (2F spec OP-9 §9.3; plan Tasks 11-12): the state of one FF's visit
 * and what the files of the pass share. dff.c drives the module and the work list, dff_state.c
 * holds the model's loading and editing, dff_gates.c the helper cells, dff_rules.c rules 2-9,
 * dff_merge.c rules 10-11 (SRST and EN merging) and dff_constbits.c the constant bits.
 * odin3_opt_dff_run, the entry for 2E, stays declared in opt.h.
 */
#ifndef ODIN3_PASSES_OPT_DFF_H
#define ODIN3_PASSES_OPT_DFF_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/opt/ff.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

enum { ODIN3_DFF_LABEL_LEN = 128, ODIN3_DFF_TYPE_LEN = 32 };

/*
 * The module's mux bits as they were when the pass began (Yosys's bit2mux and bitusers, built once
 * per worker): the Y net of each bit of a $mux, $pmux or $_MUX_, the node and bit it is, its
 * readers then and the indices of the node's A, B and S ports. index maps a Y net to its entry.
 */
typedef struct odin3_dff_muxbit {
    uint32_t net, node, bit, users, port_a, port_b, port_s;
} odin3_dff_muxbit;

typedef struct odin3_dff_muxes {
    odin3_vec bits;
    odin3_u64map *index;
} odin3_dff_muxes;

/*
 * opt_dff's dirty set of one module call (dff_dirty.c). Inactive: every FF is visited. Active:
 * only the FFs marked dirty, and every node ID from first_new on (made during this call).
 */
typedef struct odin3_dff_dirty {
    uint8_t *marks; /* one byte per node ID up to n_marks */
    uint32_t n_marks, first_new;
    bool active;
} odin3_dff_dirty;

/*
 * The state of one FF's visit.
 * - ff: the FF as the rules have left it (Yosys `ff`); cell: the node it was loaded from (Yosys
 *   `cell`), live until the commit; width0: the width of that node's Q.
 * - pend: odin3_net_pair {keep = what a given-up Q bit is tied to, drop = its Q net}.
 * - changed, gone: the FF changed / its node is gone (a rule removed it).
 * - n_changed, n_bits: this call's totals.
 * - counted: a commit already made this visit's changes (rules 10 and 11 re-emit the FF at once).
 * - dirty: which FFs this call visits (dff_dirty.c).
 * - muxes, queue: the mux snapshot of the module, and the work list (node IDs) the groups of
 *   rules 10 and 11 push the FFs they make on.
 */
typedef struct odin3_dff_state {
    odin3_opt_ctx *oc;
    odin3_ffdata ff;
    odin3_node_id cell;
    uint32_t width0;
    char type[ODIN3_DFF_TYPE_LEN];
    char label[ODIN3_DFF_LABEL_LEN];
    odin3_vec pend;
    odin3_prov_id prov;
    bool prov_ready, changed, gone, counted;
    uint32_t n_changed, n_bits;
    odin3_dff_muxes muxes;
    odin3_vec *queue;
    odin3_dff_dirty dirty;
} odin3_dff_state;

typedef odin3_status (*odin3_dff_rule_fn)(odin3_dff_state *ds);

/* --- dff_dirty.c ------------------------------------------------------------------------------ */

void odin3_dff_dirty_init(odin3_dff_dirty *dd);
void odin3_dff_dirty_free(odin3_dff_dirty *dd);

/*
 * Starts a module call: the first call in an incremental session (or one after a journal overflow)
 * sweeps in full and starts the IR journal; later calls take the FFs the journal says may have
 * changed since the previous call began. Never fails for want of memory (it sweeps in full).
 */
odin3_status odin3_dff_dirty_begin(odin3_dff_state *ds);

/* True when the FF node is to be visited in this call. */
bool odin3_dff_dirty_visit(const odin3_dff_state *ds, odin3_node_id node);

/* Before the constant-bits sweep: adds what the rules sweep itself changed. */
odin3_status odin3_dff_dirty_extend(odin3_dff_state *ds);

/* --- dff_state.c ------------------------------------------------------------------------------ */

void odin3_dff_state_init(odin3_dff_state *ds, odin3_opt_ctx *oc);
void odin3_dff_state_free(odin3_dff_state *ds);

/* Loads node into ds (Yosys `FfData ff(&initvals, cell)`); *usable = false for an FF this pass
 * leaves alone. */
odin3_status odin3_dff_state_load(odin3_dff_state *ds, odin3_node_id node, bool *usable);

/* Yosys `ff = ff.slice(keep); ff.cell = cell`. */
odin3_status odin3_dff_state_keep(odin3_dff_state *ds, const uint32_t *keep, uint32_t n);

/* The log line and the counter of one action (spec §16). */
void odin3_dff_note(odin3_dff_state *ds, odin3_opt_dff_action action, const char *detail);

odin3_const odin3_dff_net_const(const odin3_dff_state *ds, odin3_net_id net);
bool odin3_dff_net_is(const odin3_dff_state *ds, odin3_net_id net, odin3_const want);
bool odin3_dff_vec_is(const odin3_dff_state *ds, const odin3_vec *vec, odin3_const want);
bool odin3_dff_vec_all_const(const odin3_dff_state *ds, const odin3_vec *vec);
bool odin3_dff_vec_equal(const odin3_vec *lhs, const odin3_vec *rhs);
bool odin3_dff_init_defined(const odin3_dff_state *ds);
bool odin3_dff_always_active(const odin3_dff_state *ds, odin3_net_id sig, bool pol);
bool odin3_dff_never_active(const odin3_dff_state *ds, odin3_net_id sig, bool pol);

odin3_status odin3_dff_tie_bit(odin3_dff_state *ds, uint32_t idx, odin3_net_id src);
odin3_status odin3_dff_const_net(odin3_dff_state *ds, odin3_bit bit, odin3_net_id *out);
odin3_status odin3_dff_remove_all(odin3_dff_state *ds, const odin3_net_id *src);
odin3_status odin3_dff_set_consts(odin3_dff_state *ds, odin3_vec *nets, const odin3_vec *vals);
odin3_status odin3_dff_bit_push(odin3_vec *vec, odin3_bit bit);
odin3_status odin3_dff_bits_fill(odin3_vec *vec, odin3_bit bit, uint32_t n);
odin3_bit odin3_dff_const_bit(odin3_const val);

/* --- dff_gates.c ------------------------------------------------------------------------------ */

odin3_status odin3_dff_gate_not(odin3_dff_state *ds, const odin3_vec *src, odin3_vec *dst);
odin3_status odin3_dff_gate_bin(odin3_dff_state *ds, const char *op, const odin3_vec *lhs,
                                const odin3_vec *rhs, odin3_vec *dst);
odin3_status odin3_dff_gate_mux(odin3_dff_state *ds, const odin3_vec *lhs, const odin3_vec *rhs,
                                odin3_net_id sel, odin3_vec *dst);
odin3_status odin3_dff_gate_reduce(odin3_dff_state *ds, const char *op, const odin3_vec *vec,
                                   odin3_net_id *out);
odin3_status odin3_dff_gate_ne(odin3_dff_state *ds, const odin3_vec *lhs, const odin3_vec *rhs,
                               odin3_net_id *out);
odin3_status odin3_dff_not_net(odin3_dff_state *ds, odin3_net_id net, odin3_net_id *out);
odin3_status odin3_dff_and_nets(odin3_dff_state *ds, odin3_net_id lhs, odin3_net_id rhs,
                                odin3_net_id *out);

/* --- dff_rules.c: rules 2-9, one function each ------------------------------------------------ */

odin3_status odin3_dff_rule_sr(odin3_dff_state *ds);
odin3_status odin3_dff_rule_aload(odin3_dff_state *ds);
odin3_status odin3_dff_rule_arst(odin3_dff_state *ds);
odin3_status odin3_dff_rule_srst(odin3_dff_state *ds);
odin3_status odin3_dff_rule_en(odin3_dff_state *ds);
odin3_status odin3_dff_rule_clk(odin3_dff_state *ds);
odin3_status odin3_dff_rule_dq(odin3_dff_state *ds);
odin3_status odin3_dff_rule_adq(odin3_dff_state *ds);

/* --- dff_muxes.c: the mux snapshot and the per-bit searches of rules 10 and 11 -----------------
 */

/*
 * One FF bit's merge plan. d: the D net left once the muxes are eaten; key: the group key (words
 * of the plan's keys: the number of controls, the controls as (net << 1 | polarity), then the
 * patterns, each as its length and its (net << 1 | value) words); nodes: the muxes the bit
 * absorbed (words of the plan's nodes); reset: the reset value SRST collected; merged: the bit
 * gets a control (else it stays in the FF).
 */
typedef struct odin3_dff_bitplan {
    odin3_net_id d;
    uint32_t key_off, key_len, node_off, node_len;
    odin3_bit reset;
    bool merged;
} odin3_dff_bitplan;

/* A mux input pin that read Q and takes x (the feedback patterns); old is the net it read. */
typedef struct odin3_dff_xmark {
    odin3_pin_id pin;
    odin3_net_id old;
} odin3_dff_xmark;

typedef struct odin3_dff_plan {
    odin3_vec bits;  /* odin3_dff_bitplan, one per FF bit */
    odin3_vec keys;  /* uint64_t */
    odin3_vec nodes; /* uint64_t */
    odin3_vec marks; /* odin3_dff_xmark */
} odin3_dff_plan;

void odin3_dff_plan_init(odin3_dff_plan *plan);
void odin3_dff_plan_free(odin3_dff_plan *plan);

odin3_status odin3_dff_muxes_build(odin3_dff_state *ds);
void odin3_dff_muxes_free(odin3_dff_muxes *muxes);

/* Appends the plan of FF bit idx (Yosys rule 10's / rule 11's per-bit loop). */
odin3_status odin3_dff_plan_srst_bit(odin3_dff_state *ds, odin3_dff_plan *plan, uint32_t idx);
odin3_status odin3_dff_plan_en_bit(odin3_dff_state *ds, odin3_dff_plan *plan, uint32_t idx);

/* --- dff_merge.c: rules 10 and 11 ------------------------------------------------------------- */

odin3_status odin3_dff_rule_srst_merge(odin3_dff_state *ds);
odin3_status odin3_dff_rule_en_merge(odin3_dff_state *ds);

/* --- dff_constbits.c -------------------------------------------------------------------------- */

/* Loads node and ties its constant bits, rebuilding it without them. */
odin3_status odin3_dff_visit_constbits(odin3_dff_state *ds, odin3_node_id node);

/* --- dff.c ------------------------------------------------------------------------------------ */

/* Rule 12: a changed FF is re-emitted (Yosys `ff.emit()`), on the bits left of its node's Q. */
odin3_status odin3_dff_commit(odin3_dff_state *ds);

#endif
