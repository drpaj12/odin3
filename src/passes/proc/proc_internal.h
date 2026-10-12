/*
 * proc_internal.h — one module's lowering state for the proc pass (2E spec §3, §7, §14, §15, §17):
 * the module context, its constant nets and the shared scratch index (Task 2); the net map and
 * merges, the cell helper, provenance, orphans and diagnostics (Task 3); the latch database and
 * the latch phase (Task 8); the flip-flop phase (Task 9).
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

/* The latch database of a module (§8; proc_latch.c), built by the latch phase. */
typedef struct odin3_proc_ldb odin3_proc_ldb;

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
    uint64_t pw_bytes;          /* bytes of the working forms decoded so far: the budget's extra */
    odin3_celltype_id types[ODIN3_PROC_N_TYPES]; /* odin3_proc_type's cache, filled at init */
    odin3_proc_ldb *ldb; /* Task 8: the latch database, NULL until the latch phase builds it */
};

/*
 * Starts *mctx for module, whose pass run is ctx (both outlive mctx): design and module set, opts
 * and stats NULL, pws, scratch, touched, netmap and pending empty, no constant net looked up yet,
 * the cell types of odin3_proc_type cached. Allocates nothing. ODIN3_OK; ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL argument.
 */
odin3_status odin3_proc_mctx_init(odin3_proc_mctx *mctx, odin3_pass_ctx *ctx, odin3_module *module);

/* Frees every working form in pws (odin3_pw_free), the latch database and the vectors; NULL is a
 * no-op. */
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

/* --- step B: snippets and mux trees (§6, §7; proc_snip.c, proc_mux.c) --------------------- */

/*
 * The snippets of a process (Yosys 0.55's SigSnippets and SnippetSwCache, §6): the bits its case
 * tree assigns, partitioned into runs so that every action lhs is a concatenation of whole
 * snippets, each in its own bit order.
 */
typedef struct odin3_proc_snips odin3_proc_snips;

/*
 * Inserts every action lhs of pw's case tree (the root's actions first, then each switch's cases
 * depth first, in order), as Yosys's SigSnippets::insert: bits of no snippet yet join a new one; a
 * run that matches part of an existing snippet splits it into up to three, each a new snippet
 * (the worklist is the lhs being walked; a run matching a whole snippet leaves it in its place);
 * an lhs identical to a snippet is stored once. Snippets are numbered in creation order, the
 * pieces of a split after every older snippet. Then the switch cache (a u64map keyed
 * switch << 32 | snippet: some action below the switch assigns the snippet) and, per case and
 * snippet, the case's last action assigning it. Bits are compared as stored (the raw view). Each
 * snippet is a sub-run of an action's lhs run, so pw->bits is not changed. *out is owned by the
 * caller (odin3_proc_snips_free), NULL on failure. ODIN3_ERR_NO_MEMORY; ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL argument; ODIN3_ERR_CHECK (logged) if an lhs does not cover whole snippets
 * (a 2E bug). O(Σ lhs bits + splits' bits + Σ actions' snippets x switch depth), with u64map
 * operations.
 */
odin3_status odin3_proc_snips_build(const odin3_proc_mctx *mctx, const odin3_pw *pw,
                                    odin3_proc_snips **out);

/* The number of snippets (0 for NULL). */
uint32_t odin3_proc_snips_count(const odin3_proc_snips *snips);

/* Snippet index (below the count), a run of pw->bits. */
odin3_pw_sig odin3_proc_snip(const odin3_proc_snips *snips, uint32_t index);

/* True when an action somewhere below switch sw assigns snippet snip (SnippetSwCache::check). */
bool odin3_proc_snips_touches(const odin3_proc_snips *snips, uint32_t sw, uint32_t snip);

/* True when an action of case case_index itself assigns snippet snip; *rhs is then the slice of
 * the last such action's rhs that it assigns (a run of pw->bits, the snippet's width). */
bool odin3_proc_snips_assigned(const odin3_proc_snips *snips, uint32_t case_index, uint32_t snip,
                               odin3_pw_sig *rhs);

/* Frees the snippets; NULL is a no-op. */
void odin3_proc_snips_free(odin3_proc_snips *snips);

/*
 * The mux phase (§6, PR-12; proc_mux.c): Yosys 0.55's proc_mux without -ifx for pw, whose $proc
 * node step B has deleted. One tree per snippet in creation order, from the root case with
 * default x; per switch the snippet touches: its groups (one for parallel_case or simple parallel,
 * else Yosys's BitPatternPool(width) per group, odin3_ppool_init, each past the cap ending its
 * group with W5b once per switch), its full_case mask (memoised), its cases in reverse with
 * Yosys's gen_mux and append_pmux. Cells: $eq (A_SIGNED 0, B_SIGNED 0, A_WIDTH = B_WIDTH = the
 * bits left after dropping '-', Y_WIDTH 1; a 1-bit compare against 1 is the signal bit itself),
 * $reduce_or of several compares (A_SIGNED 0, A_WIDTH their count, Y_WIDTH 1), $mux (WIDTH) and
 * $pmux (WIDTH, S_WIDTH; B slices and S bits in append order), each with the switch's record
 * odin3_proc_prov(mctx, pw, [switch, its cases in case order]) and the attributes of its flags. A
 * mux is a record until no case can append to it (the next mux of its switch opens, an append
 * closes it, or its switch is done), then one node; an append never resizes a node. An append whose
 * select is empty (a compare of only '-' in a parallel_case group, where Yosys 0.55 asserts)
 * makes the case's value the result, as gen_mux does, and closes the open mux (§20 #17); gen_mux's
 * own no-cell paths (no compare, an empty select) close it too, where Yosys keeps it open and
 * loses a later append (§20 #18). Each snippet's value is recorded with odin3_proc_connect
 * (sig[i] <- value[i]), after the promotions step B records, so M1 applies them in that order.
 * ODIN3_ERR_NO_MEMORY and E6 (ODIN3_ERR_PARSE, odin3_proc_add_cell) stop at once with valid IR: the
 * cells made stay, and the nets of muxes still open are deleted. ODIN3_ERR_INVALID_ARG (logged) for
 * a NULL argument.
 */
odin3_status odin3_proc_mux(odin3_proc_mctx *mctx, odin3_pw *pw);

/* --- step C: latches (§8, §10, PR-14, PR-16; proc_latch.c) --------------------------------- */

/*
 * A hold rule of the latch walk (Yosys 0.55's rule_node_t): true when signal equals match and,
 * when it has children, one of them is true. Rules are hash-consed per module, with IDs from
 * ODIN3_PROC_RULE_FIRST in creation order; ODIN3_PROC_RULE_TRUE and _FALSE are the two constant
 * rules, which have no record. An inner rule of a mux branch is (select bit, ODIN3_PW_S0 or _S1,
 * [child]); the "or" of a mux's branches is (ODIN3_PW_S0, ODIN3_PW_S0, children sorted by ID).
 * The children are odin3_proc_ldb_child(mctx, rule, 0 .. n_children - 1).
 */
typedef struct odin3_proc_rule {
    odin3_pw_bit signal, match;
    uint32_t first_child, n_children;
} odin3_proc_rule;

enum { ODIN3_PROC_RULE_TRUE = 1, ODIN3_PROC_RULE_FALSE = 2, ODIN3_PROC_RULE_FIRST = 3 };

/*
 * The latch phase (§8, PR-14), Yosys 0.55's proc_dlatch for the whole module after step B. It
 * builds the database once (odin3_proc_ldb_build, unless one is already built; nothing at all
 * happens when no process has an ALWAYS action), then for each process in node-ID order (the
 * byte budget checked at each): every ALWAYS sync action (live view, odin3_proc_find) is
 * quickchecked (odin3_proc_quickcheck); a false one leaves all its bits no-latch, a true one makes
 * each bit a candidate out <- in; the sync's actions are then cleared. Candidates are sorted by
 * odin3_proc_bit_key of out (a repeated out keeps its last in) and each is walked from in
 * (find_mux_feedback: A first, then the B slices; memoised per (net, set_undef) within one out, so
 * each mux bit of a cone is expanded at most once per flag; set_undef false below a net with more
 * than one reader in the snapshot; an A or B pin on out moved to the x constant net when it is
 * true; a combinational cycle through the walk reads as no feedback, where Yosys's recursion does
 * not end). The no-latch bits (the false actions' bits, then the candidates whose walk is false)
 * become connections out <- in, merged at M2, and lose their initial value
 * (odin3_ffinit_remove; W2 once per wire per process for a defined bit). The latch bits, in key
 * order, are grouped while the next bit has the same primary wire and rule; each group is one
 * $dlatch (WIDTH, EN_POLARITY 1; EN = $not(hold(rule)), D = the in bits, Q = the out bits) with
 * W1. hold(n) is built once per rule: TRUE is 1'1; a rule whose signal differs from its match
 * gives the signal (match 1) or $not(signal) (match 0), made first; children give the $reduce_or
 * of their holds in order; two terms their $and. Every cell of the step has A_SIGNED/B_SIGNED 0
 * and Y_WIDTH 1 where it applies, pw->lowered as its record and pw's attributes. Then the fix-up:
 * breadth-first upstream from every $dlatch through the drivers of input nets, visiting only §8's
 * internal combinational types, each $mux/$pmux loses its all-x/z B slices with their select bits
 * (none left: B = A, S = 1'0), takes its last valid slice as A when A is all x/z, and is $mux
 * with one select, else $pmux; a changed one is deleted and re-created on the same nets with its
 * old record and attributes (not counted again in the statistics). Then M2
 * (odin3_proc_apply_merges). The database's per-net snapshot is released at the end; its rules
 * and latches stay until mctx is freed or the database is rebuilt. ODIN3_ERR_CHECK (logged "proc:
 * internal: latch candidate on a constant net") when an ALWAYS action's lhs bit resolves to a
 * constant (2D never merges an always output into one, so it is a 2E bug); ODIN3_ERR_NO_MEMORY and
 * E6 (ODIN3_ERR_PARSE, at a process boundary or in odin3_proc_add_cell) stop at once with valid
 * IR; ODIN3_ERR_INVALID_ARG (logged) for a NULL mctx.
 */
odin3_status odin3_proc_latches(odin3_proc_mctx *mctx);

/*
 * (Re)builds mctx's latch database from the module as it stands (§8): for every live $mux and
 * $pmux, the (node, bit) of each net on Y and a snapshot of the distinct nets on A and B that are
 * not constant nets; per net the number of reading pins (sinks and inouts) of nodes other than
 * ports and processes, plus one per $port_in or $port_inout pin that drives it (Yosys's sigusers);
 * an empty rule table, hold memo and latch list. An earlier database is freed first.
 * ODIN3_ERR_NO_MEMORY (the database is then left unbuilt); ODIN3_ERR_INVALID_ARG (logged) for a
 * NULL mctx. O(module pins).
 */
odin3_status odin3_proc_ldb_build(odin3_proc_mctx *mctx);

/* Frees mctx's latch database (mctx->ldb NULL afterwards); a NULL mctx or database is a no-op. */
void odin3_proc_ldb_free(odin3_proc_mctx *mctx);

/* The bytes the latch database holds, for the byte budget (0 without one). */
uint64_t odin3_proc_ldb_bytes(const odin3_proc_mctx *mctx);

/* A quickcheck: rhs and lhs are runs of pw->bits. */
typedef struct odin3_proc_qc {
    const odin3_pw *pw;
    odin3_pw_sig rhs, lhs;
} odin3_proc_qc;

/*
 * Yosys 0.55's quickcheck(rhs, lhs) on the database's snapshot (§8 step 1): breadth-first from the
 * nets of rhs (live view) through the $mux/$pmux drivers recorded at the build and their recorded
 * A/B nets; true when a recorded net is one of lhs's nets. Reads only the snapshot, so a pin moved
 * after the build is not seen until the next odin3_proc_ldb_build. False without a built database.
 * Never allocates; O(the cone's nets and recorded inputs).
 */
bool odin3_proc_quickcheck(odin3_proc_mctx *mctx, const odin3_proc_qc *qc);

/* The rule with ID id (from ODIN3_PROC_RULE_FIRST), NULL for another ID or without a database. */
const odin3_proc_rule *odin3_proc_ldb_rule(const odin3_proc_mctx *mctx, uint32_t id);

/* Child index (below rule->n_children) of a rule of mctx's database. */
uint32_t odin3_proc_ldb_child(const odin3_proc_mctx *mctx, const odin3_proc_rule *rule,
                              uint32_t index);

/* The index-th $dlatch the latch phase made (creation order, from 0) and its hold rule; false past
 * the last. */
bool odin3_proc_ldb_latch(const odin3_proc_mctx *mctx, uint32_t index, odin3_node_id *node,
                          uint32_t *rule);

/* Calls the test hook on_expand (proc_test.h), if one is installed (proc_pass.c owns the hooks). */
void odin3_proc_hook_expand(odin3_net_id net, bool set_undef);

/* --- step D: flip-flops (§9, PR-15; proc_ff.c) --------------------------------------------- */

/*
 * The flip-flop phase (§9, PR-15), Yosys 0.55's proc_dff for pw after the latch step, in the live
 * view: every edge and level sync's signal and actions are first read through the net map. Then,
 * while a sync assigns anything, find_any_lvalue gives a group (the first non-empty action lhs of
 * the last sync that has one, as Yosys's loop leaves it, sorted by odin3_proc_bit_key and unified,
 * then narrowed to its common part with each sync's bits where that part is not empty), one
 * register: each level sync action meeting it gives a rule (its value, z where the action does not
 * assign; its signal), the edge sync's actions give insig (z where none assigns). Rules whose
 * values are all equal collapse into one LEVEL1 rule on $reduce_or of their triggers (the signal,
 * or a $not of it for LEVEL0, made first); one rule whose value is the group itself becomes insig =
 * $mux(insig, group, trigger). Then: several rules give W4 and $dffsr (SET and CLR from $mux chains
 * built from the last rule to the first, starting from 0; CLK_POLARITY from the edge, SET_POLARITY
 * = CLR_POLARITY = 1); one rule whose value is not made of constant nets gives W3 and $aldff
 * (ALOAD_POLARITY 1); else $adff (ARST_POLARITY 1, ARST_VALUE the constant) or $dff. WIDTH is the
 * group's width, CLK_POLARITY 1 for POSEDGE and 0 for NEGEDGE; the register's Q is the group's own
 * nets and it copies the $proc node's attributes; every cell has pw->lowered as its record. W3
 * names the first reset as written, W4 the first three and then `…`, both at pw->prov. The group's
 * bits then leave every action, so the syncs end empty. ODIN3_ERR_CHECK (logged "proc: internal:
 * ...") for what step A or the latch step rules out: an ALWAYS sync with an action, a group without
 * an edge sync or with two, a register output on a constant net; ODIN3_ERR_NO_MEMORY and E6
 * (ODIN3_ERR_PARSE, odin3_proc_add_cell) stop at once with valid IR; ODIN3_ERR_INVALID_ARG (logged)
 * for a NULL argument. O(groups x sync actions x width), Yosys's bound.
 */
odin3_status odin3_proc_ffs(odin3_proc_mctx *mctx, odin3_pw *pw);

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
