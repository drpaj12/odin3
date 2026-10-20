/*
 * memory_merge.h — the `memory_merge` pass (4.2 spec section 7, G-1..G-5; plan Task 5): the R-8
 * flip-flop merge of mem_merge.c applied to the design. Internal: not part of the ABI.
 */
#ifndef ODIN3_PASSES_PARTIAL_MAP_MEMORY_MERGE_H
#define ODIN3_PASSES_PARTIAL_MAP_MEMORY_MERGE_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"

#include <stdint.h>

/* What a run did (G-5). */
typedef struct pm_merge_stats {
    uint64_t data;  /* read ports whose data flip-flops were merged */
    uint64_t addr;  /* read ports whose address flip-flops were merged */
    uint64_t async; /* read ports still asynchronous when the pass ends */
} pm_merge_stats;

/*
 * For every `$mem` of every module, in node-ID order: runs R-8 (mem_merge) on the model of the
 * cell and, when a read port merged, replaces the cell by one with RD_CLK_ENABLE, RD_CLK_POLARITY
 * and RD_TRANSPARENT set for that port, the port's pins on the flip-flops' nets, and the flip-flops
 * removed. The replacement is one transaction (hm_kit: the same name and attributes, one DERIVED
 * provenance record, the design intact on ODIN3_ERR_NO_MEMORY); the old asynchronous data nets of a
 * data merge are removed. A port whose merged enable is active low is left as it is (the pass adds
 * no inverter), and a `$mem` is left whole when a flip-flop that would go carries an initial value;
 * their asynchronous ports count in stats->async. Unlike partial_map, which keeps the flip-flops
 * of a `$mem` that ends soft, the merge is made whatever the verdict would be. Logs the INFO line
 * of G-5. *stats may be NULL. A read-port bit parameter shorter than RD_PORTS is a located error
 * (ODIN3_ERR_INVALID_ARG).
 * ODIN3_ERR_PARSE when the design budget is exceeded; ODIN3_ERR_NO_MEMORY otherwise.
 */
odin3_status pm_mem_merge(odin3_pass_ctx *ctx, odin3_design *design, pm_merge_stats *stats);

#endif
