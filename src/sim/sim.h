/*
 * sim.h — the netlist simulator (1E): a flat, levelized simulation program built from a design.
 */
#ifndef ODIN3_SIM_SIM_H
#define ODIN3_SIM_SIM_H

#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"

typedef struct odin3_sim odin3_sim; /* opaque; fields in sim/sim_internal.h */

/*
 * Builds the simulation program of module top: every module instance is expanded (iteratively)
 * into flat cells, every flat net bit gets one value slot (an instance's pins share the slots of
 * the parent nets they connect), the top module's ports become the primary inputs and outputs in
 * port order, and the cells are levelized once (edge-triggered storage cuts the graph; level
 * latches do not). The program refers to the design's parameter storage: the design must not be
 * changed (or compacted) while the simulator lives. *out gets a simulator owned by the caller
 * (odin3_sim_destroy), NULL on failure.
 *
 * ODIN3_ERR_INVALID_ARG (logged, located from provenance where it has a source location) for: an
 * invalid top; a recursive module hierarchy; a black box or a cell type without a simulate hook
 * ("cannot simulate `<type>`"); a clock pin of an edge-triggered cell whose net is not a primary
 * input of top; an inout port of top; a combinational loop (naming one net on it).
 * ODIN3_ERR_NO_MEMORY on out of memory. The design is never changed. Memory and time are linear
 * in the size of the flattened design.
 */
odin3_status odin3_sim_build(odin3_design *design, odin3_module_id top, odin3_sim **out);

/* Frees a simulator; NULL is a no-op. */
void odin3_sim_destroy(odin3_sim *sim);

#endif
