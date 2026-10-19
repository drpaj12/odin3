/*
 * aligned.h — the aligned MiniLUT of the name map (3D spec §6.3, AB-16; plan Task 21).
 */
#ifndef ODIN3_PASSES_ABC_ALIGNED_H
#define ODIN3_PASSES_ABC_ALIGNED_H

#include "odin3/odin3.h"
#include "passes/abc/run.h"
#include "util/arena.h"

/*
 * The aligned MiniLUT (§6.3, AB-16): the engine's result luts with the loaded AIG's interface, so
 * that the name map can run on it. The PIs as sent, with one register output per input register k
 * (run->region->regs); the LUTs as in luts, each fanin from result register j remapped to k*(j)
 * (luts->ci_from of its PI); the real outputs as in luts; the register inputs: j's D driver for k
 * = k*(j), the constant 0 for any other k. The arrays live in arena. ODIN3_ERR_INVALID_ARG (not
 * logged) when a register's name is not an input register's, or luts->lut_size is not 2 to 16;
 * NO_MEMORY.
 */
odin3_status odin3_abc_aligned_build(const odin3_abc_run *run, const odin3_abc_luts *res,
                                     odin3_arena *arena, odin3_abc_luts *aligned);

#endif
