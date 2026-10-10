/*
 * sim.h — the netlist simulator (1E): a flat, levelized simulation program built from a design.
 */
#ifndef ODIN3_SIM_SIM_H
#define ODIN3_SIM_SIM_H

#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "sim/prng.h"

#include <stdbool.h>
#include <stdint.h>

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
 * invalid top; a recursive module hierarchy; a cell type without a simulate hook, such as a black
 * box without semantics, or a cell whose type finds it too wide to simulate ("cannot simulate
 * `<type>`"); a clock pin of an edge-triggered cell whose net is not a primary
 * input of top, or that is not connected ("has no clock connected"); an inout port of top; a net
 * with more than one driver that is neither an inout pin nor an output of a tristate type (a
 * primary input counts as a driver), naming the net; a combinational loop (naming one net on it);
 * a design that flattens past the budget (odin3_sim_options below).
 * ODIN3_ERR_NO_MEMORY on out of memory. The design is never changed. Memory and time are linear
 * in the size of the flattened design, which the budget bounds.
 *
 * On success every value and state bit is 0, every sequential cell has received the INIT event
 * (cell.h: INIT 1 starts at 1; 0, 2 and 3 at 0), every clock input is low, and the combinational
 * logic has settled once, so the outputs show the initial state.
 */
odin3_status odin3_sim_build(odin3_design *design, odin3_module_id top, odin3_sim **out);

/*
 * The flattening budget. A small hierarchical netlist can flatten to exponentially many cells
 * (two instances per level: 2^levels), so the build counts what it expands and stops, before it
 * allocates for more, at max_cells cells plus expanded instances, and at
 * ODIN3_SIM_BITS_PER_CELL x max_cells net-map entries, value slots and pin bits together; it
 * then fails with ODIN3_ERR_INVALID_ARG, logging "the design flattens to more than N …" located
 * at the instance or cell where the budget is crossed. The default, 2^24, is 4.9 times the
 * largest golden (Odin II LargeRam, 3.4M cells; the largest simulated one, LU64PEEng, has 0.38M)
 * and a design at the budget needs about 4.4 GB (measured 273 MB per 2^20 cells), inside the
 * 6 GB per-tool cap tools/sim-check applies.
 */
#define ODIN3_SIM_DEFAULT_MAX_CELLS (1U << 24)
#define ODIN3_SIM_BITS_PER_CELL 8U

typedef struct odin3_sim_options {
    uint32_t max_cells; /* 0: ODIN3_SIM_DEFAULT_MAX_CELLS */
} odin3_sim_options;

/* odin3_sim_build with options (NULL: the defaults); the budget errors are described above. */
odin3_status odin3_sim_build_opts(odin3_design *design, odin3_module_id top,
                                  const odin3_sim_options *opts, odin3_sim **out);

/* Frees a simulator; NULL is a no-op. */
void odin3_sim_destroy(odin3_sim *sim);

/*
 * Primary inputs and outputs are the top module's ports, in port order (inputs and outputs
 * numbered separately from 0); bit 0 is a port's LSB. Names are the port names (owned by the
 * design's string table); a name or width query out of range returns NULL or 0.
 */
typedef struct odin3_sim_bit {
    uint32_t port;
    uint32_t bit;
} odin3_sim_bit;

uint32_t odin3_sim_input_count(const odin3_sim *sim);
uint32_t odin3_sim_output_count(const odin3_sim *sim);
const char *odin3_sim_input_name(const odin3_sim *sim, uint32_t port);
const char *odin3_sim_output_name(const odin3_sim *sim, uint32_t port);
uint32_t odin3_sim_input_width(const odin3_sim *sim, uint32_t port);
uint32_t odin3_sim_output_width(const odin3_sim *sim, uint32_t port);

/*
 * True when input bit `at` is a clock: it drives the clock pin of an edge-triggered cell, so the
 * cycle toggles it and the caller never drives it. False for a non-clock bit or out of range.
 */
bool odin3_sim_input_is_clock(const odin3_sim *sim, odin3_sim_bit at);

/*
 * Sets non-clock input bit `at` to value for the next cycles (it keeps its value until set
 * again). ODIN3_ERR_INVALID_ARG (not logged) for a bit out of range or a clock bit; nothing
 * changes then.
 */
odin3_status odin3_sim_set_input(odin3_sim *sim, odin3_sim_bit at, bool value);

/* *value gets input bit `at` (a clock reads low between cycles); ODIN3_ERR_INVALID_ARG (not
 * logged, *value untouched) out of range. */
odin3_status odin3_sim_get_input(const odin3_sim *sim, odin3_sim_bit at, bool *value);

/*
 * Drives every non-clock input bit, in port then bit order, with odin3_prng_bit(prng): one draw
 * per bit, so a seed fixes the whole vector sequence. Clock bits draw nothing.
 */
void odin3_sim_drive_random(odin3_sim *sim, odin3_prng *prng);

/*
 * Runs one cycle with the inputs as set: settle the combinational logic; raise every clock input
 * and send the rising edge (ODIN3_SIM_POSEDGE) to every edge-triggered cell; settle; lower the
 * clocks and send the falling edge (ODIN3_SIM_NEGEDGE); settle. A settle runs each cell's COMB
 * once, in level order (edge-triggered cells first). Allocates nothing and cannot fail; time is
 * linear in the flattened design (three settles plus two edge events per edge-triggered cell).
 */
void odin3_sim_cycle(odin3_sim *sim);

/* *value gets output bit `at` as of the last settle; ODIN3_ERR_INVALID_ARG (not logged, *value
 * untouched) out of range. */
odin3_status odin3_sim_get_output(const odin3_sim *sim, odin3_sim_bit at, bool *value);

#endif
