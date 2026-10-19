/*
 * splice.h — re-stitching a mapped region into the IR (3D spec §6.5, AB-18; plan Task 17): the
 * init-1 restore on the result, the canonical constant net, and the splice itself (phases A to D).
 */
#ifndef ODIN3_PASSES_ABC_SPLICE_H
#define ODIN3_PASSES_ABC_SPLICE_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"

#include <stdint.h>

/* What one splice did (the per-run INFO line and the tests read it). */
typedef struct odin3_abc_splice_stats {
    uint32_t luts, ffs, buffers, merges, deleted_cells, deleted_nets, deleted_wires;
} odin3_abc_splice_stats;

struct odin3_abc_run;

/*
 * The lowest-ID net of run->module driven by a `$_CONST<value>_` node or by a constant `$sop`
 * that is not the undefined `unconn` generator (value 0 or 1); when there is none, a new
 * `$_CONST<value>_` node and its net. ODIN3_ERR_INVALID_ARG (logged) for a value other than 0
 * and 1; NO_MEMORY (nothing created).
 */
odin3_status odin3_abc_canonical_const(struct odin3_abc_run *run, int value, odin3_net_id *out);

/*
 * The init-1 restore (AB-13, both exchanges), done on the result before any IR object exists:
 * every result register j whose representative has INIT 1 holds the inverse of the IR value, so
 * res is edited until object j means the IR value. The table of each LUT that reads j is flipped
 * in that variable; a real CO that reads j directly gets a NOT LUT (one new LUT object appended
 * to res); a CO that read j through an inverter LUT (now a buffer) is redirected to j; a register
 * input whose source LUT feeds only j has the table complemented, a constant source flips, any
 * other source gets a NOT LUT, and a source that is itself a restored register object j2 is j2
 * directly. run->region names the registers. NO_MEMORY leaves res valid but possibly part way.
 */
odin3_status odin3_abc_restore(struct odin3_abc_run *run, odin3_abc_result *res);

/*
 * Applies res (constants folded, checked) to run->module for run->domain: records, the restore,
 * phases A to D (§6.5). region is the region res was made from. Any failure leaves the IR valid
 * and equivalent: a failure in phase A removes what it made (best effort: a node that cannot be
 * deleted stays, fully disconnected); in phase B the old drivers of the unvisited outputs stay; in
 * phase C the function is complete and only names may not have moved. stats (may be NULL) is filled
 * on success. NO_MEMORY (not logged) and the IR statuses.
 */
odin3_status odin3_abc_splice(struct odin3_abc_run *run, const odin3_abc_region *region,
                              odin3_abc_result *res, odin3_abc_splice_stats *stats);

#endif
