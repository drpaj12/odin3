/*
 * ff.h — the flip-flop model of the opt family (2F spec OP-9 §9.1, §9.4; plan Task 2): Yosys 0.55
 * kernel/ff.{h,cc} FfData over Odin III's FF cells. ff_load reads one of the fifteen word FFs
 * (IR change D: $dff $dffe $adff $adffe $aldff $aldffe $sdff $sdffe $sdffce $dffsr $dffsre
 * $dlatch $adlatch $dlatchsr $sr) or a fine one ($_DFF_P_ $_DFF_N_ $_FF_ $_DLATCH_P_
 * $_DLATCH_N_) into an odin3_ffdata; ff_slice keeps some of its bits; ff_emit picks the cell type
 * as FfData::emit does and rebuilds the node through the kit (OP-2). Initial values are read and
 * written per Q net through passes/ffinit.h.
 */
#ifndef ODIN3_PASSES_OPT_FF_H
#define ODIN3_PASSES_OPT_FF_H

#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Yosys FfData. The sig_* vectors hold odin3_net_id (none: an unconnected pin) and the val_*
 * vectors odin3_bit, LSB first. sig_q and val_init are always width long; sig_d is width long
 * with has_clk or has_gclk, sig_ad with has_aload, sig_set and sig_clr with has_sr, val_arst with
 * has_arst, val_srst with has_srst; a feature's vectors are empty without it (as Yosys's are).
 * A latch (has_aload without has_clk) reads EN as sig_aload and D as sig_ad.
 * - node: the node it was loaded from, 0 for a new FF (ff_slice; ff_emit sets the node it made).
 * - has_gclk: Yosys's has_gclk, set only by $_FF_ (the registry has no word $ff); agent default,
 *   beyond the briefed fields, so that a $_FF_ is not taken for an FF without control inputs.
 * - prov: the record of the node it was loaded from (kept by ff_slice, 0 for none); ff_emit gives
 *   a new cell a DERIVED record of it (agent default, beyond the briefed fields).
 * - prov_final: a new cell takes prov itself instead of a DERIVED record of it (a caller that made
 *   the record it wants, as opt_dff's SRST/EN groups do with parents [FF, absorbed muxes]).
 */
typedef struct odin3_ffdata {
    odin3_node_id node;
    uint32_t width;
    bool has_clk, has_ce, has_srst, has_arst, has_aload, has_sr, ce_over_srst, is_fine;
    bool pol_clk, pol_ce, pol_srst, pol_arst, pol_aload, pol_set, pol_clr;
    odin3_net_id sig_clk, sig_ce, sig_srst, sig_arst, sig_aload;
    odin3_vec sig_ad, sig_set, sig_clr, sig_d, sig_q;
    odin3_vec val_arst, val_srst, val_init;
    bool has_gclk;
    odin3_prov_id prov;
    bool prov_final;
} odin3_ffdata;

/* Bit indices into an FF, for ff_slice. */
typedef struct odin3_ff_bits {
    const uint32_t *bits;
    uint32_t n;
} odin3_ff_bits;

/*
 * An empty FF: width 0, every flag false (Yosys's defaults), every vector empty with its element
 * size. Allocates nothing; a fresh FF is filled by hand from here (tests, later rules).
 */
void odin3_ffdata_init(odin3_ffdata *ff);

/* Element idx of an odin3_net_id / odin3_bit vector (a net id of sig_*, a bit of val_*). */
odin3_net_id odin3_ff_net_at(const odin3_vec *vec, uint32_t idx);
odin3_bit odin3_ff_bit_at(const odin3_vec *vec, uint32_t idx);

/* Frees ff's vectors and leaves it as odin3_ffdata_init does; NULL is a no-op. */
void odin3_ffdata_free(odin3_ffdata *ff);

/* True when type is one of the twenty FF types above (Yosys RTLIL::builtin_ff_cell_types). */
bool odin3_ff_is_ff(const odin3_opt_ctx *oc, odin3_celltype_id type);

/*
 * Yosys FfData(initvals, cell): *out (initialised here) gets live FF node's features, polarities,
 * nets and *_VALUE parameters, and val_init per Q net from odin3_ffinit_get. A fine FF whose Q
 * reads x there takes its INIT parameter instead (0 or 1; 2 and 3 are x; agent default: BLIF's
 * .latch init lives there). A latch with no other control whose D nets all carry constants loads
 * as Yosys's: an async reset (sig_arst = EN, val_arst = the constants) instead of a load.
 * INVALID_ARG (logged "opt: ff_load: ...") for a node that is not a live FF; NO_MEMORY. *out is
 * left empty on failure.
 */
odin3_status odin3_ff_load(odin3_opt_ctx *oc, odin3_node_id node, odin3_ffdata *out);

/*
 * Yosys FfData::slice: *out (initialised here) is a new FF (node 0) with ff's controls,
 * polarities and record and the bits bits.bits[0 .. n) of every per-bit vector, in that order.
 * INVALID_ARG (logged) for an index past ff->width; NO_MEMORY. *out is left empty on failure.
 */
odin3_status odin3_ff_slice(const odin3_ffdata *ff, odin3_ff_bits bits, odin3_ffdata *out);

/*
 * Yosys FfData::emit (kernel/ff.cc 536-605): makes the cell ff describes and *out = it.
 * - Width 0: nothing (node must be 0), *out = 0.
 * - No clock, no load, no set/reset: an async reset becomes a load (EN = ARST, D = the constant
 *   nets of val_arst; ff is updated); with none either, the init on the Q nets is removed and Q is
 *   tied to the constant nets of val_init (kit_connect of the node, or merges of undriven Q nets
 *   for a new FF), *out = 0.
 * - Otherwise $sr, $dlatch/$adlatch/$dlatchsr, $dffsr(e), $adff(e), $aldff(e),
 *   $sdff/$sdffe/$sdffce or $dff(e), in that order of precedence. A fine FF keeps its kind
 *   ($_FF_, $_DFF_P_/N_ by pol_clk, $_DLATCH_P_/N_ by pol_aload) and its INIT parameter follows
 *   val_init (x keeps a 2 or 3); a fine FF needing another fine type is INVALID_ARG (the registry
 *   has none). A live node is rebuilt (kit_rebuild_node: its name, attributes, a DERIVED record);
 *   a new FF is created on its Q nets (kit_new_cell, a DERIVED record of ff->prov).
 * - Then every Q net whose odin3_ffinit_get differs from val_init gets odin3_ffinit_set (a fine
 *   FF's: only where the wire holds a defined bit; INIT holds the rest).
 * ff->node becomes *out. INVALID_ARG (logged "opt: ff_emit: ...") for vectors of the wrong length,
 * a node that is not live or whose Q nets are not sig_q, or a type the registry lacks.
 * NO_MEMORY before the rebuild's commit leaves the IR unchanged (removed inits written back);
 * after it (init writes, kit_connect's merges) the IR is valid and partly written. Budget errors
 * as kit_new_cell's. Sets changed.
 */
odin3_status odin3_ff_emit(odin3_opt_ctx *oc, odin3_ffdata *ff, odin3_node_id *out);

/*
 * Yosys ff.remove() + module->connect(Q, src): the live FF node goes, the init of its Q nets with
 * it, and its Q net k is tied to src[k] (kit_connect: the Q net merged into src[k]; queued
 * instead under oc->defer_ties); src[k] equal to Q net k hands the net to another driver (init
 * kept, nothing merged). n is the width of its Q port. INVALID_ARG (logged) for a node that is not
 * a live FF or another n; NO_MEMORY before the delete leaves the IR as it was (the removed inits
 * written back); kit_connect's half-done failures otherwise.
 */
odin3_status odin3_ff_remove_tie(odin3_opt_ctx *oc, odin3_node_id node, const odin3_net_id *src,
                                 uint32_t n);

/*
 * Yosys `ff = ff.slice(keep); ff.cell = cell; ff.emit()` plus the `module->connect(q_i, src)` of
 * the bits it dropped: ff->node is a live FF whose Q nets are sig_q (the bits kept) and the
 * drops of ties[0 .. n_ties) (the others, once each); ties[i].drop is a Q net given up and
 * ties[i].keep the net it is tied to (module->connect); keep == drop means the net is handed to
 * another driver: its init stays and nothing is merged (opt_dff's slices, built on the same Q net).
 * The cell ff now describes is made on the
 * kept Q nets, with the old node's attributes, then the old node goes, the given-up bits have
 * their init removed and are tied, and the new node takes the old name; *out = the new node, and
 * ff->node = it. With width 0 left the whole node goes (every Q net tied); with no control left
 * (see odin3_ff_emit) nothing is made, the kept Q nets are tied to val_init too, and *out = 0.
 * The new node is built before the old one is touched (a NO_MEMORY up to the old node's delete
 * leaves the IR as it was, inits written back); under oc->defer_ties the ties are queued. Then
 * the init writes of odin3_ff_emit. INVALID_ARG (logged "opt: ff_emit_shrunk: ...") for Q nets
 * that do not split as above.
 */
odin3_status odin3_ff_emit_shrunk(odin3_opt_ctx *oc, odin3_ffdata *ff, const odin3_net_pair *ties,
                                  uint32_t n_ties, odin3_node_id *out);

#endif
