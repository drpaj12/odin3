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
 * parent, or none at all, stands for pw->prov). The IR's status (INVALID_ARG before an
 * operation begins); ODIN3_ERR_NO_MEMORY.
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
