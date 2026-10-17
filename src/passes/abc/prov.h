/*
 * prov.h — provenance across ABC (3D spec §8, AB-25; plan Task 18): anchors, the records of a
 * run's new objects, the LUT depth of the top module. The rules are IR-12's amendment (§8.5).
 */
#ifndef ODIN3_PASSES_ABC_PROV_H
#define ODIN3_PASSES_ABC_PROV_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * The design's `$lut` cell type (a design's slot 0 is reserved, so this is not the built-in
 * index). A design always has it; the result is the zero ID only if the registry is damaged.
 */
odin3_celltype_id odin3_abc_lut_type(const odin3_design *design);

/* §8.3's order: COs, register inputs, registers, the rest. */
typedef enum odin3_abc_anchor_kind {
    ODIN3_ABC_ANCHOR_CO = 1,
    ODIN3_ABC_ANCHOR_REG_IN,
    ODIN3_ABC_ANCHOR_REG,
    ODIN3_ABC_ANCHOR_CI,
    ODIN3_ABC_ANCHOR_NAMEMAP
} odin3_abc_anchor_kind;

/*
 * An anchor pairs a result object with an old net that computes the same function. complemented
 * counts for provenance but never moves names (name-map anchors only; Task 21).
 */
typedef struct odin3_abc_anchor {
    uint32_t obj;
    odin3_net_id old_net;
    uint8_t kind; /* an odin3_abc_anchor_kind */
    bool complemented;
} odin3_abc_anchor;

/*
 * The identity anchors of the run's region and result (every CO, every register input, every CI;
 * C3) and the register anchors (AB-13), appended to out (a vec of odin3_abc_anchor, cleared
 * first) in §8.3's order: COs, register inputs, registers, CIs. A register input whose old D pin
 * is unconnected has no anchor. res is the restored result. NO_MEMORY (not logged).
 */
odin3_status odin3_abc_anchors(odin3_abc_run *run, const odin3_abc_result *res, odin3_vec *out);

/*
 * The record of each new object (§8.5), into obj_prov (a vec of uint32_t cleared first, one entry
 * per result object; 0: the object has none: constants, CIs and logic no anchor reaches). Each
 * anchor is one operation whose DERIVED record has the records of its old region's cells as
 * parents, in node-ID order; a new LUT in one new region carries that record, one in several
 * carries the record of one more operation whose parents are those regions' records in anchor
 * order. Walks are iterative with per-anchor visit stamps. The parent edges are summed first:
 * over the budget (run limits prov_edges, else 16 x (old cells + new objects) + 65,536) the run
 * makes one record whose parents are all its old cells in node-ID order, every new object carries
 * it, and it warns `abc: module '<m>', <domain>: provenance needs N parent edges (budget M); the
 * run's LUTs share one record`. NO_MEMORY, with records made so far kept (records are immutable).
 */
odin3_status odin3_abc_records(odin3_abc_run *run, const odin3_abc_result *res,
                               const odin3_vec *anchors, odin3_vec *obj_prov);

/*
 * One DERIVED record, in a new operation of the run, whose parents are the records of every old
 * soft cell and register of the run's region in node-ID order (the over-budget form; also the
 * record of a new object that no anchor reaches). *out stays 0 when no old cell has a record.
 * NO_MEMORY.
 */
odin3_status odin3_abc_coarse_record(odin3_abc_run *run, odin3_prov_id *out);

/*
 * The longest path of $lut cells in the top module between its CIs and COs (ports, flip-flops and
 * cut cells bound it), by Kahn's order; 0 without a top. A combinational loop contributes the
 * part before it. NO_MEMORY (not logged).
 */
odin3_status odin3_abc_depth(const odin3_design *design, uint32_t *depth);

#endif
