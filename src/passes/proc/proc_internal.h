/*
 * proc_internal.h — one module's lowering state for the proc pass (2E spec §3, §17): the module
 * context, its constant nets and the shared scratch index. Task 3 appends the net map, the latch
 * database, the statistics and the phase entry points.
 */
#ifndef ODIN3_PASSES_PROC_PROC_INTERNAL_H
#define ODIN3_PASSES_PROC_PROC_INTERNAL_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/proc/pw.h"
#include "util/vec.h"

#include <stdint.h>

/* The pass options (proc.h, 2E Task 3, repeats this typedef as C17 allows). */
typedef struct odin3_proc_opts odin3_proc_opts;

/* The four shared constant nets, indexed by a constant bit's state (ODIN3_PW_S0 ... _SZ). */
enum { ODIN3_PROC_N_CONST_NETS = 4 };

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
    /* Task 3 appends */
};

/*
 * Starts *mctx for module, whose pass run is ctx (both outlive mctx): design and module set, opts
 * NULL, pws, scratch and touched empty, no constant net looked up yet. Allocates nothing. ODIN3_OK;
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument.
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

#endif
