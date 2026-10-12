/*
 * proc_internal.h — one module's lowering state for the proc pass (2E spec §3, §7, §14, §15, §17):
 * the module context, its constant nets and the shared scratch index (Task 2); the net map and
 * merges, the cell helper, provenance, orphans, the pending phases and diagnostics (Task 3). Task 8
 * appends the latch database.
 */
#ifndef ODIN3_PASSES_PROC_PROC_INTERNAL_H
#define ODIN3_PASSES_PROC_PROC_INTERNAL_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/attr.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* The four shared constant nets, indexed by a constant bit's state (ODIN3_PW_S0 ... _SZ). */
enum { ODIN3_PROC_N_CONST_NETS = 4 };

/* The built-in cell types proc creates, looked up once per module (odin3_proc_type). */
enum { ODIN3_PROC_N_TYPES = 11 };

/* One recorded connection lhs <- rhs (§7), merged at M1 or M2. */
typedef struct odin3_proc_conn {
    odin3_pw_bit lhs, rhs;
} odin3_proc_conn;

/*
 * One module's lowering (pw.h typedefs it). Freed after each module, so proc's memory is bounded
 * by the largest module's bodies.
 */
struct odin3_proc_mctx {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    odin3_module *module;
    const odin3_proc_opts *opts;
    odin3_vec pws;     /* odin3_pw: the module's processes in node-ID order */
    odin3_vec scratch; /* uint32_t per net ID, 0 when unset: the signal operations' index */
    odin3_vec touched; /* uint32_t: the net IDs set in scratch, reset by each operation */
    odin3_net_id const_net[ODIN3_PROC_N_CONST_NETS]; /* $_CONST0_/1_/X_/Z_ nets, found on use */
    /* Task 3 */
    odin3_vec netmap; /* odin3_pw_bit per net ID: 0 = its own root, else its parent or a constant */
    odin3_vec pending;          /* odin3_proc_conn, in record order */
    odin3_proc_stats *stats;    /* the run's counts (may be NULL outside a run) */
    uint32_t errors;            /* errors reported in this module */
    bool stopped;               /* the run stops: the error cap (E7) or the byte budget (E6) */
    uint64_t pins_since_budget; /* pins created since the last budget check */
    uint32_t init_key;          /* strtab ID of `init`, 0 until a phase needs it (Task 8) */
    uint32_t *reported; /* the run's pending phases already reported (bit per phase), or NULL */
    uint64_t pw_bytes;  /* bytes of the working forms decoded so far: the budget's extra */
    odin3_celltype_id types[ODIN3_PROC_N_TYPES]; /* odin3_proc_type's cache, filled at init */
    /* Task 8 appends the latch database */
};

/*
 * Starts *mctx for module, whose pass run is ctx (both outlive mctx): design and module set, opts
 * and stats NULL, pws, scratch, touched, netmap and pending empty, no constant net looked up yet,
 * the cell types of odin3_proc_type cached. Allocates nothing. ODIN3_OK; ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL argument.
 */
odin3_status odin3_proc_mctx_init(odin3_proc_mctx *mctx, odin3_pass_ctx *ctx, odin3_module *module);

/* Frees every working form in pws (odin3_pw_free) and the vectors; NULL is a no-op. */
void odin3_proc_mctx_free(odin3_proc_mctx *mctx);

/*
 * The shared net of constant code (ODIN3_PW_S0, _S1, _SX or _SZ; IR-4): the net on the Y pin of
 * the first live node of type $_CONST0_, $_CONST1_, $_CONSTX_ or $_CONSTZ_ in ID order, else of a
 * new such node on a new unnamed net, both with the module's provenance. Looked up once per module
 * and kept in mctx->const_net while that net is live and still carries the constant.
 * ODIN3_ERR_INVALID_ARG (logged) for another code; ODIN3_ERR_NO_MEMORY with the IR unchanged.
 */
odin3_status odin3_proc_const_net(odin3_proc_mctx *mctx, odin3_pw_bit code, odin3_net_id *out);

/*
 * The scratch index (pw_sig.c, pw_encode.c): grows mctx->scratch to odin3_module_net_end entries
 * (new ones 0) and reserves touched for as many, so odin3_proc_scratch_set never allocates.
 * ODIN3_ERR_NO_MEMORY with the index unchanged. Every operation that sets entries ends with
 * odin3_proc_scratch_reset, also on failure, so the index is all zero between operations.
 */
odin3_status odin3_proc_scratch_grow(odin3_proc_mctx *mctx);

/* Sets entry net (below the grown size) to value (nonzero), recording a net set for the first time
 * in touched. Never allocates. */
void odin3_proc_scratch_set(odin3_proc_mctx *mctx, odin3_net_id net, uint32_t value);

/* The entry of net; 0 for an unset entry or a net beyond the index. */
uint32_t odin3_proc_scratch_get(const odin3_proc_mctx *mctx, odin3_net_id net);

/* Zeroes every touched entry and empties touched (keeps the storage). */
void odin3_proc_scratch_reset(odin3_proc_mctx *mctx);

/* --- the net map, connections and merges (§7; proc_emit.c) -------------------------------- */

/*
 * The root of bit's class: a net, or the constant the class holds (a constant is its own root).
 * Iterative, with path compression (every entry on the path then names the root); never
 * allocates. A net beyond the map is its own root.
 */
odin3_pw_bit odin3_proc_find(odin3_proc_mctx *mctx, odin3_pw_bit bit);

/* Records the connection lhs <- rhs (merged by odin3_proc_apply_merges). ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_proc_connect(odin3_proc_mctx *mctx, odin3_pw_bit lhs, odin3_pw_bit rhs);

/*
 * Applies the pending connections in record order (§7, M1 and M2) and empties the list, also on
 * failure. Both sides are resolved (odin3_proc_find); equal sides are skipped. A constant side
 * merges the other net into the module's shared constant net (keep = the constant net, and the
 * other net's entry becomes the constant); otherwise keep is the net whose primary wire is a port
 * wire, else the one with a primary wire, else the lower ID, and a tie at either level keeps the
 * lower net ID (plan point (u)). Then odin3_net_merge(keep, drop), and drop's entry becomes keep.
 * ODIN3_ERR_CHECK (logged "proc: internal: merging two driven nets '<a>' and '<b>'") for two
 * different constants, and in Debug for two sides that both have a driver (every lhs is a
 * temporary or output of a deleted $proc); ODIN3_ERR_NO_MEMORY; the IR's status of a merge.
 */
odin3_status odin3_proc_apply_merges(odin3_proc_mctx *mctx);

/* --- cells (§14; proc_emit.c) --------------------------------------------------------------- */

/*
 * One cell to create. inputs: one sig per input port, in port order, in pw->bits; their bits are
 * resolved through the net map, and constants become the shared constant nets. output: the nets Q
 * drives (resolved likewise), or NULL for fresh unnamed nets, which take prov. attrs_from: a
 * working form whose attrs the cell copies (registers and latches, §14), or NULL. switch_flags:
 * ODIN3_SWITCH_FULL_CASE / _PARALLEL_CASE set the attributes full_case / parallel_case (INT 1).
 */
typedef struct odin3_proc_cell {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t n_params;
    odin3_pw *pw;
    const odin3_pw_sig *inputs;
    uint32_t n_inputs;
    const odin3_pw_sig *output;
    const odin3_pw *attrs_from;
    uint16_t switch_flags;
    odin3_prov_id prov;
} odin3_proc_cell;

/*
 * Creates the cell (one odin3_node_create_connected) and appends its output nets, in port order,
 * to pw->bits as *out. Counts its pins for the budget (a check every budget_pins pins, before the
 * cell is made: E6 at pw->prov, ODIN3_ERR_PARSE) and its kind in stats->cells.
 * ODIN3_ERR_INVALID_ARG (logged) for a sig whose length is not its port's width, a constant output
 * bit or a type the IR refuses; ODIN3_ERR_NO_MEMORY (fresh nets are deleted again; the IR stays
 * valid).
 */
odin3_status odin3_proc_add_cell(odin3_proc_mctx *mctx, const odin3_proc_cell *cell,
                                 odin3_pw_sig *out);

/* The built-in type named name, cached at odin3_proc_mctx_init for proc's own types; none (logged)
 * for a name the design does not have. */
odin3_celltype_id odin3_proc_type(const odin3_proc_mctx *mctx, const char *name);

/* --- provenance (§14; proc_emit.c) ---------------------------------------------------------- */

/*
 * *out = the DERIVED record of the run's current operation with parents (n of them, in order; a 0
 * parent, or none at all, stands for pw->prov). A parent that is still 0 (a process without a
 * record) is left out, and with no parent left *out is 0 and the status ODIN3_OK, as
 * odin3_proc_begin_lowering does. The IR's status (INVALID_ARG before an operation begins);
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_proc_prov(const odin3_proc_mctx *mctx, const odin3_pw *pw,
                             const odin3_prov_id *parents, uint32_t n, odin3_prov_id *out);

/*
 * Starts the process's lowering: odin3_prov_begin_op, then pw->lowered = derive([pw->prov]) (plan
 * point (i)), the record steps B-D reuse ("one operation per process"; 0 when the node has no
 * record). ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_proc_begin_lowering(odin3_proc_mctx *mctx, odin3_pw *pw);

/* --- step E, the budget (§12, §16; proc_emit.c) --------------------------------------------- */

/*
 * Step E: every net of any working form's in_nets that is live, pinless, unnamed, without a
 * primary wire and without aliases is deleted (odin3_net_delete). Wire bits are never deleted.
 */
odin3_status odin3_proc_orphans(odin3_proc_mctx *mctx);

/*
 * The design-wide byte budget at a process boundary (§16): odin3_budget_check with the working
 * forms as extra. Over it: E6 at record `at` (odin3_proc_error), stopped set, ODIN3_ERR_PARSE.
 */
odin3_status odin3_proc_budget(odin3_proc_mctx *mctx, odin3_prov_id at);

/* --- step A: the decision-tree phases (§5) ------------------------------------------------- */

/*
 * clean (§5.1, PR-7; proc_clean.c): Yosys 0.55's proc_clean_case on case case_index of pw, swept
 * again while a sweep sets Yosys's did_something (an empty action, empty switch, inline, or a
 * parallel or trailing empty case removed; a constant switch folded or a full_case switch emptied
 * sets none). Each sweep walks the subtree from case_index, as Yosys's does, so the cost is
 * O(subtree x sweeps) (§19). max_depth -1 is the whole tree; arst passes 1. With case_index 0 and
 * max_depth -1 it is proc_clean on the process: first every sync action with an empty lhs, then
 * every sync without actions, is removed. An inlined switch stays in its parent's list with no
 * cases until the parent's walk reaches it again, as in Yosys. No recursion. ODIN3_ERR_NO_MEMORY
 * (pw stays a valid tree); ODIN3_ERR_INVALID_ARG (logged) for a NULL pw or a case index past its
 * cases.
 */
odin3_status odin3_pw_clean(odin3_pw *pw, uint32_t case_index, int32_t max_depth);

/* odin3_pw_clean's max_depth for the whole tree (proc_clean's own call). */
enum { ODIN3_PW_CLEAN_WHOLE_TREE = -1 };

/*
 * rmdead (§5.2, PR-8; proc_rmdead.c): Yosys 0.55's proc_rmdead on every switch of pw's root case,
 * depth first, cases in order, each switch with its own pool (odin3_fdpool when its signal has no
 * constant bit and every compare is fully defined, else odin3_ppool): compares the pool does not
 * take are removed, a non-default case left without compares is removed, a default case takes
 * all, and a switch whose pool ends empty gains ODIN3_SWITCH_FULL_CASE. The pattern pools of the
 * switches being processed (a switch and the ones nested in its current case) share the cap: past
 * it the pool that grew stops (odin3_ppool_stop), and its switch keeps the rest of its cases and
 * compares and its flags, without full_case (§20 #7), and gets W5a once (at the switch's record,
 * else the process's); only a constant compare without 0/1 bits, or a default case, still empties
 * it (every later constant compare goes, as does a later default, and the switch gains
 * full_case), as in Yosys.
 * ODIN3_ERR_NO_MEMORY (pw stays a valid tree).
 */
odin3_status odin3_pw_rmdead(odin3_proc_mctx *mctx, odin3_pw *pw);

/*
 * prune (§5.3, PR-9; proc_prune.c): Yosys 0.55's proc_prune on pw in the raw view (its decoded
 * nets). From the root case, each case's switches run last to first, then its actions last to
 * first; a switch runs its cases in order, each from the state it was entered with, and adds the
 * bits newly assigned in every case when it is full_case or has a default. An action bit already
 * assigned later on the path goes, and so does a constant bit; a root action bit assigned nowhere
 * later in the process (not in `affected`) is promoted. Actions left with no bits are removed,
 * the others keep their remaining bits in order; each root action that promotes bits appends one
 * promotion (lhs <- rhs in its bit order, its record) to pw->promoted, which step B and
 * --stop-after apply. Uses mctx's scratch index (all zero again on return, also on failure) for
 * the per-net marks and counts, so it costs O(body + lhs bits). ODIN3_ERR_NO_MEMORY (pw stays a
 * valid tree); ODIN3_ERR_INVALID_ARG (logged) for a NULL argument, a pw without a root case, or an
 * action whose lhs and rhs differ in length.
 */
odin3_status odin3_pw_prune(odin3_proc_mctx *mctx, odin3_pw *pw);

/* --- step A: asynchronous resets and the clock pre-check (§5.4, §5.5, §11) ------------------ */

/* A 1-bit switch signal and the net it may stand for (proc_arst.c). */
typedef struct odin3_proc_sigref {
    odin3_net_id signal, ref;
} odin3_proc_sigref;

/*
 * Yosys 0.55's check_signal (§5.4): whether sr.signal is sr.ref seen through simple logic. Both
 * are read as the working form reads a net (a constant net as its constant, then the net map), and
 * the walk follows each net's driver (odin3_net_driver) while it is the 1-bit Y of $reduce_or or
 * $reduce_bool (on to A), $logic_not or $not (on to A, polarity flipped), or $eq / $ne with a
 * fully constant A (on to B) or else B (on to A), flipped for $eq unless that constant has a 1
 * bit, for $ne when it has one. True when it reaches ref, with *polarity flipped once per
 * inversion; false (polarity unchanged) at a constant, a net without such a driver, a port that is
 * not one bit wide, or after odin3_proc_limits_now().max_signal_walk hops (a cycle of inverters,
 * where Yosys recurses until its stack overflows). Never allocates; O(hops).
 */
bool odin3_proc_check_signal(odin3_proc_mctx *mctx, odin3_proc_sigref sr, bool *polarity);

/*
 * arst (§5.4, PR-10; proc_arst.c): Yosys 0.55's proc_arst without -global_arst. While the root
 * case has exactly one switch, each edge sync is tried in order (polarity: POSEDGE) against the
 * switch's signal (odin3_proc_check_signal). With several edge syncs the first that passes is an
 * asynchronous reset: it becomes LEVEL1 (from POSEDGE) or LEVEL0, joins the level syncs, each of
 * its actions gets rhs = the value the reset forces (apply_reset, in the assign view: a bit mapped
 * through pw->promoted, lhs to rhs as Yosys's SigMap joins them, then through the net map), and
 * the switch's cases for the reset value go, the first other one surviving without compares,
 * inlined by a clean of the root at depth 1. With one edge sync it is a VHDL-style edge test: the
 * same elimination with the polarity inverted. When anything was found the syncs are reordered:
 * level, then edge, then the rest. Errors (at pw->prov, ODIN3_ERR_PARSE, the first one only): E2
 * when a reset value keeps an ODIN3_PW_SM bit; E3 when it has not settled after
 * odin3_proc_limits_now().max_arst_rounds rounds. Uses mctx's scratch index (zero again on
 * return). ODIN3_ERR_NO_MEMORY (pw stays a valid tree); ODIN3_ERR_INVALID_ARG (logged) for a
 * NULL argument or a pw without a root case.
 */
odin3_status odin3_pw_arst(odin3_proc_mctx *mctx, odin3_pw *pw);

/*
 * The clock pre-check (§5.5, PR-11; proc_clock.c), after arst: the errors Yosys's proc_dff would
 * raise, before anything changes. Every lhs bit of an edge or level sync action, in the live view
 * (odin3_proc_find) and sorted by odin3_proc_bit_key: a bit two edge syncs update is E1b when both
 * are on one net with opposite edges, else E1a naming the first two in sync order; a bit only
 * level syncs update is E5. The first bit in that order with an error is reported (at pw->prov,
 * ODIN3_ERR_PARSE), named with the run of following bits on its wire that have the same error.
 * ALWAYS syncs are ignored (the latch phase empties them first). ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pw_check_clocks(odin3_proc_mctx *mctx, const odin3_pw *pw);

/*
 * E4 (§11, plan point (n); proc_clock.c), after arst: when pw has a level sync, the first net on
 * an O pin of pw->node (pin order) that no sync action updates and that sits on a WR_EN, WR_ADDR
 * or WR_DATA pin of a $mem gives "memory '<MEMID>' is written in an always block with
 * asynchronous reset '<the first level sync's signal>'" (at pw->prov, ODIN3_ERR_PARSE). Uses
 * mctx's scratch index (zero again on return). ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_pw_check_memwr(odin3_proc_mctx *mctx, const odin3_pw *pw);

/* --- pending phases (plan point (h); proc_pending.c, deleted by Task 9) --------------------- */

/* The first pending phase in schedule order, ODIN3_PROC_N_PHASES when none is pending. */
odin3_proc_phase odin3_proc_pending_first(void);

/* True when phase is still pending. */
bool odin3_proc_pending(odin3_proc_phase phase);

/*
 * A pending phase reached: logs "proc: '<phase>' is not available yet (2E)" the first time in the
 * run (*mctx->reported), counting it as an error, and returns ODIN3_ERR_PARSE.
 */
odin3_status odin3_proc_pending_run(odin3_proc_mctx *mctx, odin3_proc_phase phase);

/* --- diagnostics (§15, plan point (k); proc_diag.c) ----------------------------------------- */

/*
 * A user error at record at (the first SOURCE or IMPORTED leaf locates it: its loc through
 * odin3_diag, else its file, line and column, else unlocated as "proc: module '<m>': <msg>").
 * Counts it in mctx->errors and stats->errors; the error that brings the run's count to the cap
 * logs "too many errors; proc stopped after <cap>" (unlocated) instead and sets stopped. Nothing is
 * logged once stopped.
 */
void odin3_proc_error(odin3_proc_mctx *mctx, odin3_prov_id at, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

/* A warning of kind at record at, located as odin3_proc_error; counts stats->warnings[kind]. */
void odin3_proc_warn(odin3_proc_mctx *mctx, odin3_proc_warning kind, odin3_prov_id at,
                     const char *fmt, ...) ODIN3_PRINTF(4, 5);

/*
 * Appends §15's name of n bits (each resolved through the net map, read only), without quotes:
 * the whole wire `q`; a contiguous part `q[7:4]` in declared indices (odin3_wire_index); else the
 * first bit `q[3]` (`q` on a one-bit wire). A net without a primary wire on a named wire is named
 * by its own name, else its first alias (a wire bit or a name), else `$n<ID>`; a constant bit as
 * RTLIL spells it (1'0, 1'1, 1'x, 1'z). Nothing for n 0. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_proc_name_bits(const odin3_proc_mctx *mctx, const odin3_pw_bit *bits, uint32_t n,
                                  odin3_strbuf *out);

#endif
