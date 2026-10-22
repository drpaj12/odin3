/*
 * mul_group.h — multiplier regroup (spec M-4 to M-6; plan Tasks 7 and 8): the `$mul` and `$add`
 * cells partial_map's multiplier split made come back as the one `$mul` they were cut from.
 */
#ifndef ODIN3_PASSES_UNMAP_MUL_GROUP_H
#define ODIN3_PASSES_UNMAP_MUL_GROUP_H

#include "odin3/odin3.h"
#include "passes/unmap/unmap.h"

/* Phase C: patterns P2 and P1 on every candidate root, repeated until nothing merges (M-5). */
odin3_status um_mul_group_phase(um_ctx *uc);

#endif
