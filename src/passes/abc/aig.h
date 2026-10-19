/*
 * aig.h — the in-memory exchange's AIG builder (3D spec §6.3, AB-16; plan Task 20): a region as a
 * MiniAIG in Odin III's exchange layout (odin3_abc_aig, odin3.h). Pure IR code: nothing here
 * includes or calls ABC.
 */
#ifndef ODIN3_PASSES_ABC_AIG_H
#define ODIN3_PASSES_ABC_AIG_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/abc/region.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdint.h>

/* The literal of a MiniAIG object index, positive and complemented. */
#define ODIN3_ABC_AIG_LIT(obj) (2 * (int32_t)(obj))

/* The MiniAIG's "no fanin" marker (0x7FFFFFFF), used by PIs, object 0 and the second slot of a PO.
 */
enum { ODIN3_ABC_AIG_NULL = 0x7FFFFFFF };

/*
 * An AIG under construction or built: MiniAIG's int32_t pairs (objs, two per object), the node
 * that first made each AND (owner, one uint32_t node ID per object, 0 for the rest), the hash of
 * the ANDs (key lit0 << 32 | lit1, value the AND's literal), the literal of each soft output net
 * (net_lit: net ID to literal), and the lowest-ID soft output net of each variable (var_net:
 * literal >> 1 to net ID; the name map's reverse index). n_pis counts the real inputs and the
 * register outputs, n_regs the registers, n_pos the real outputs and the register inputs.
 */
typedef struct odin3_abc_aigb {
    odin3_vec objs, owner;
    odin3_u64map *hash;
    odin3_u64map *net_lit;
    odin3_u64map *var_net;
    uint32_t n_pis, n_regs, n_pos;
} odin3_abc_aigb;

struct odin3_abc_run;

/*
 * Builds the AIG of run's current region (run->module, run->domain, run->region) in §6.3's order:
 * object 0; one PI per CI, then one per register output; the ANDs in topological order; the POs
 * (the COs, then the register inputs). *out is initialised here and must be freed with
 * odin3_abc_aig_free, also after a failure.
 * - Literals: a register's Q and D are complemented when its INIT is 1 (AB-13); a constant pin
 *   reads 0 or 1 under AB-11's per-pin rule.
 * - A gate or `$lut` uses odin3_blif_isop's cover (an off-set cover complements the output), a
 *   `$sop` its rows; a cube is a balanced AND over its literals in input order and a cover a
 *   balanced OR over its cubes in row order (pairwise, left to right, level by level).
 * - Hashing applies a&0 = 0, a&1 = a, a&a = a and a&~a = 0; the first owner wins.
 * More objects than the cap (odin3_abc_run_limits_get()->aig_objs) is logged as `abc: module '<m>',
 * <domain>: the AIG needs more than <cap> objects` with ODIN3_ERR_PARSE; NO_MEMORY (not logged).
 * Never changes the IR.
 */
odin3_status odin3_abc_aig_build(struct odin3_abc_run *run, const odin3_abc_region *region,
                                 odin3_abc_aigb *out);

/* The engine's view of a built AIG (valid until it is freed). */
odin3_abc_aig odin3_abc_aig_view(const odin3_abc_aigb *aig);

/* The literal of the soft output net, or -1 when the AIG has none for it. */
int32_t odin3_abc_aig_lit(const odin3_abc_aigb *aig, odin3_net_id net);

/* The lowest-ID soft output net whose literal is on variable var (literal >> 1), or none. */
odin3_net_id odin3_abc_aig_net_of_var(const odin3_abc_aigb *aig, uint32_t var);

/* Frees the vecs and maps, and leaves *aig empty. NULL is a no-op. */
void odin3_abc_aig_free(odin3_abc_aigb *aig);

#endif
