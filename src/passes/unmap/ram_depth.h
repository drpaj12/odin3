/*
 * ram_depth.h — depth recovery (spec R-8; plan Tasks 15 and 16): the `$mem` leaves of a depth split
 * and the word-level glue partial_map built around them become the one memory they were cut from.
 */
#ifndef ODIN3_PASSES_UNMAP_RAM_DEPTH_H
#define ODIN3_PASSES_UNMAP_RAM_DEPTH_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/unmap/unmap.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* What R-8 step 1 reads off the write enables of the leaves. */
typedef struct um_write_tree {
    odin3_vec paths;  /* uint32_t per leaf: bit L is the path bit of level L */
    odin3_vec ad;     /* odin3_net_id per level: the address net of level L */
    odin3_net_id we0; /* the write enable the chains start from */
    odin3_vec glue;   /* odin3_node_id: the `$and` and `$not` cells the chains are made of */
} um_write_tree;

void um_write_tree_free(um_write_tree *tree);

/*
 * R-8 step 1 for one candidate set of `$mem` nodes (`leaves`: odin3_node_id, 2^levels of them): the
 * write enable of each leaf is the output of a chain of `levels` `$and` cells, the last one fed by
 * the leaf's level levels-1 literal and the first by the common write enable `we0` and the level 0
 * literal. A literal is an address net `ad[L]` (path bit 1) or the output of a 1-bit `$not` of it
 * (path bit 0). Fills `out` (free it with um_write_tree_free whatever the result); false when the
 * structure does not match: a leaf whose enable is not such a chain, leaves that disagree on `we0`
 * or on an address net, or paths that are not 2^levels different ones.
 */
bool um_depth_write_tree(const um_ctx *uc, const odin3_vec *leaves, uint32_t levels,
                         um_write_tree *out);

/* Phase F: depth recovery on the `$mem` cells phase D made. */
odin3_status um_ram_depth_phase(um_ctx *uc);

#endif
