/*
 * cells.h — the gate-mapped result of abc --gates (gatemap spec 4.3; plan Task 11): the engine's
 * odin3_abc_cells read as an abc_result whose LUTs are library gates, each tagged with its cell.
 */
#ifndef ODIN3_PASSES_ABC_CELLS_H
#define ODIN3_PASSES_ABC_CELLS_H

#include "odin3/odin3.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "util/arena.h"

/*
 * Reads cells->net with odin3_abc_result_from_luts against run->region (its CIs, COs and
 * registers; no --lut bound), then tags each LUT: LUT k is the k-th MiniLUT object that is neither
 * a PI (slot 0 0x7FFFFFFF) nor a PO (slot 1 0x7FFFFFFE), and out->gate[k] is the index into
 * run->lib.gates of the library cell named cells->names[cells->gate[object]]. Each failure is
 * logged `abc: ABC returned a malformed network: <why>` and returns ODIN3_ERR_ENGINE: `a gate
 * object without a gate name`; `gate '<g>' is not a cell of library '<L>'`; `gate '<g>' has <n>
 * inputs; library cell '<g>' has <m>`; `gate '<g>' does not compute library cell '<g>''s function`
 * (the first 2^n bits of its truth differ from the cell's table); and every failure of
 * odin3_abc_result_from_luts. NO_MEMORY (not logged). *out is initialised here (free it on
 * failure too); arena holds scratch.
 */
odin3_status odin3_abc_result_from_cells(const odin3_abc_run *run, const odin3_abc_cells *cells,
                                         odin3_arena *arena, odin3_abc_result *out);

#endif
