/*
 * hdl_restore.h — the record a reader makes for an object whose HDL location text it restored
 * (spec docs/specs/2026-10-20-5.4-hdl-locations-design.md D-1, D-7; docs/IR.md IR-13).
 *
 * A writer prints the HDL locations of an object (backends/common/hdl_loc.h); a reader that parses
 * them back makes, per object, one DERIVED record whose parents are the restored SOURCE record
 * first and the record the reader would have made without the locations second, so
 * odin3_hdl_locs and every "first location" consumer see the HDL, and the netlist's own position
 * stays reachable as the second leaf.
 */
#ifndef ODIN3_FRONTENDS_HDL_RESTORE_H
#define ODIN3_FRONTENDS_HDL_RESTORE_H

#include "backends/common/hdl_loc.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/* The class of the one operation that makes restored records: a rewrite (IR-12a). */
extern const odin3_opclass odin3_hdl_restore_class;

/* What to restore: the parsed locations of one object (their files are views, which the call
 * interns), the record the reader would have made (0: none), and the hierarchy path (a strtab ID
 * of the design, 0: none) that goes on both records. */
typedef struct odin3_hdl_restore_req {
    odin3_prov_id base;
    const odin3_hdl_loc *locs;
    uint32_t n;
    uint32_t hier_str;
} odin3_hdl_restore_req;

typedef struct odin3_hdl_restored {
    odin3_prov_id prov; /* the record the object takes */
    bool capped; /* n was over ODIN3_HDL_LOC_MAX: the first 8 were kept (D-7), the caller counts it
                  */
} odin3_hdl_restored;

/*
 * Makes SOURCE{locs, hier} (odin3_prov_source: kind SOURCE, the context's run, ast 0, loc 0), then
 * DERIVED{parents [source, base], hier} (odin3_prov_derive_hier) in the context's current
 * operation; out->prov is the DERIVED record. With base 0 (the reader has no record of its own) its
 * only parent is the SOURCE.
 * Records are hash-consed, so equal text on many objects is one SOURCE and one DERIVED per
 * distinct (source, base) pair. The caller begins one operation (odin3_prov_begin_op_class,
 * rewrite) per read run; this function never does. File strings are interned in the design's table.
 * More than ODIN3_HDL_LOC_MAX locations keep the first 8 and set out->capped.
 *
 * INVALID_ARG (logged by the provenance store, or "hdl_restore: no operation begun" /
 * "hdl_restore: nothing to restore") for a NULL argument, no operation begun, n 0, or a base or
 * hier that is not a record or string of the design; NO_MEMORY. Nothing changes on failure but
 * strings and the SOURCE record that may already be made.
 */
odin3_status odin3_hdl_restore(odin3_pass_ctx *ctx, const odin3_hdl_restore_req *req,
                               odin3_hdl_restored *out);

#endif
