/*
 * gate_truth.h — the truth table of a one-output gate type, computed by running its 1E simulate
 * hook over every input assignment (gatemap plan Task 3). The abc pass uses it for the soft gates
 * it sends to ABC (3D spec §5.1), the library cell view (techlib/libcells.h) for an .o3lib
 * library's gate cells.
 *
 * A table holds 2^n bits for a type of n inputs: bit i is the output for the input assignment i,
 * where bit j of i is the value of input j, the j-th input port in port order.
 */
#ifndef ODIN3_SIM_GATE_TRUTH_H
#define ODIN3_SIM_GATE_TRUTH_H

#include "ir/celltype.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

enum { ODIN3_GATE_TRUTH_MAX_INPUTS = 6 };

/*
 * True for a gate type: bit granularity, no parameters, a simulate hook, none of the flags
 * SEQ_EDGE, SEQ_LEVEL, TRISTATE and ANYVIEW, every port one bit of constant width and not inout,
 * exactly one output and 1 to ODIN3_GATE_TRUTH_MAX_INPUTS inputs.
 */
bool odin3_gate_truth_eligible(const odin3_celltype_def *def);

/* The number of input ports of def. */
uint32_t odin3_gate_truth_inputs(const odin3_celltype_def *def);

/*
 * The truth table of an eligible def into *table (the bits from 2^n up are 0). type_data is what
 * the simulator would pass the hook: the type's odin3_techlib_cell (odin3_celltype_lib) for a
 * tech-library cell, NULL for a built-in type. When the type has a sim_scratch_bytes hook, the
 * scratch is allocated for the call and freed before it returns. ODIN3_ERR_INVALID_ARG (not logged)
 * for a def that is not eligible or whose scratch hook refuses it; ODIN3_ERR_NO_MEMORY. *table is
 * untouched on failure.
 */
odin3_status odin3_gate_truth(const odin3_celltype_def *def, const void *type_data,
                              uint64_t *table);

/*
 * The value of a constant type: bit granularity, no parameters, a simulate hook, neither SEQ flag,
 * and exactly one port, a 1-bit output. *value gets its simulated output, 0 or 1. type_data and the
 * scratch as for odin3_gate_truth. ODIN3_ERR_INVALID_ARG (not logged) for any other type or a
 * refused scratch; ODIN3_ERR_NO_MEMORY. *value is untouched on failure.
 */
odin3_status odin3_gate_const_value(const odin3_celltype_def *def, const void *type_data,
                                    int *value);

#endif
