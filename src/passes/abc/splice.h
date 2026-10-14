/*
 * splice.h — re-stitching a mapped region into the IR (3D spec §6.5, AB-18; plan Task 17).
 */
#ifndef ODIN3_PASSES_ABC_SPLICE_H
#define ODIN3_PASSES_ABC_SPLICE_H

#include "odin3/odin3.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"

/*
 * Applies res (constants folded, checked) to run->module for run->domain: records, phases A to D
 * (§6.5), the init-1 restore. region is the region res was made from. Any failure leaves the IR
 * valid and equivalent. PLACEHOLDER until Task 17: it logs `abc: the splice is not implemented
 * yet` and returns ODIN3_ERR_INVALID_ARG without touching the IR.
 */
struct odin3_abc_run;
odin3_status odin3_abc_splice(struct odin3_abc_run *run, const odin3_abc_region *region,
                              odin3_abc_result *res);

#endif
