/*
 * sim4.h — the 4-state models of the cell types (design note
 * docs/specs/2026-11-05-sim-4state-design.md). In 4-state mode the value bytes are codes 0, 1, x =
 * 2, z = 3 and a cell is evaluated by the hook odin3_sim4_lookup gives its type.
 */
#ifndef ODIN3_SIM_SIM4_H
#define ODIN3_SIM_SIM4_H

#include "ir/celltype.h"
#include "odin3/odin3.h"
#include "sim/cell.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*odin3_sim4_fn)(const odin3_sim_cell *cell);

/*
 * The 4-state hook of def and the data it needs (cell->type4), or NULL when the type has none.
 * Models: the bit gates, constants, flip-flops and latches (including the 68 enable/reset
 * flip-flops), $lut, and the word cells $mux, $pmux, $eq, $ne, $reduce_or, $reduce_bool and
 * $logic_not (which `lower --keep-sel` leaves in place).
 */
odin3_sim4_fn odin3_sim4_lookup(const odin3_celltype_def *def, const void **data);

/* Extra scratch bytes the hook needs for a cell whose ports have the widths of the sizing view. */
uint32_t odin3_sim4_scratch_bytes(const odin3_celltype_def *def, const odin3_sim_cell *sizing);

/* The generic model of a combinational type without a 4-state hook: any unknown input pin gives x
 * on every output pin, else the 2-state hook runs (def->simulate); the view's `type4` is the
 * definition. */
void odin3_sim4_generic(const odin3_sim_cell *cell);

#endif
