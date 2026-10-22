/*
 * ram.h — hard RAM cells back to `$mem` (spec R-1 to R-7; plan Tasks 9 and 10).
 */
#ifndef ODIN3_PASSES_UNMAP_RAM_H
#define ODIN3_PASSES_UNMAP_RAM_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/unmap/unmap.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

enum { UM_RAM_PORTS = 2 };

/* The summary of one hard RAM cell (R-2). */
typedef struct um_ram {
    odin3_node_id node;
    bool dual;
    uint32_t abits;
    uint32_t width;
    uint32_t rd_ports; /* at least 1 */
    uint32_t wr_ports;
    bool read[UM_RAM_PORTS];  /* port 1, then port 2: some output bit is observed */
    bool write[UM_RAM_PORTS]; /* the write enable is not constant 0 */
} um_ram;

/* R-2: the summary of the live RAM cell `node`. */
odin3_status um_ram_summary(const um_ctx *uc, odin3_node_id node, um_ram *out);

/*
 * The cells of one `$mem` to build: `cells` (um_ram) in groups of `slices` per replica; replica 0
 * is the memory, later replicas are copies built for more read ports (R-7); the slices of a
 * replica are the data-width slices of it (R-6). One cell, one slice is the plain conversion.
 */
typedef struct um_rgroup {
    odin3_vec cells;
    uint32_t slices;
    const char *rule; /* the `unmap` attribute: ram, ram.width or ram.replica */
} um_rgroup;

/* R-2, R-5: builds the `$mem` of the group and replaces its cells. */
odin3_status um_ram_emit(um_ctx *uc, const um_rgroup *group);

/*
 * R-5 to R-7: the groups of `cells` (um_ram, node-ID order): `groups` becomes an odin3_vec of
 * um_rgroup in node-ID order of their first cell (free each `cells`, then the vec, with
 * um_rgroups_free). Candidate sets (equal control nets, no common origin) are kept apart, get
 * `unmap_cand` on their cells and one INFO line each.
 */
odin3_status um_ram_groups(um_ctx *uc, const odin3_vec *cells, odin3_vec *groups);
void um_rgroups_free(odin3_vec *groups);

/* Phase D. */
odin3_status um_ram_phase(um_ctx *uc);

#endif
