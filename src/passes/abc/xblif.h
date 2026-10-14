/*
 * xblif.h — the BLIF exchange's files (3D spec §6.2, AB-15, AB-13; plan Task 16): the input BLIF
 * of a region, the command file, and the read-back of ABC's output into an abc_result. Pure IR and
 * file code; nothing here includes ABC.
 */
#ifndef ODIN3_PASSES_ABC_XBLIF_H
#define ODIN3_PASSES_ABC_XBLIF_H

#include "odin3/odin3.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/script.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Writes §6.2's input for the current region of run (run->module, run->region) to path: `.model
 * o3abc`, `.inputs`, `.outputs`, the soft cells in region order (a $sop keeps its rows; gates and
 * $lut write odin3_blif_isop's cover), then per register the AB-13 inverter, the `.latch` (always
 * init 0) and the reader inverter, then the buffers of COs that are register values. One name per
 * net: o3ci<i>, o3co<i>, o3q<j> (o3r<j> when inverted, o3d<j> its inverted input), o3c0/o3c1 for
 * constants, o3n<net ID> otherwise (o3u<node ID> for an output nothing connects). Deterministic.
 * ODIN3_ERR_IO (logged) when the file cannot be written; NO_MEMORY.
 */
odin3_status odin3_abc_xblif_write(odin3_abc_run *run, const odin3_abc_region *region,
                                   const char *path);

/*
 * The commands of the run in order (AB-22): `read "<in>"`, the script's lines, `&put` when the
 * script ends in the & space, `write_hie "<in>" "<out>"`. odin3_abc_xblif_command_count gives
 * their number; odin3_abc_xblif_command renders command index (appended to out, no line end) and
 * sets *src to the script command that produced it, or NULL for the three internal ones.
 */
uint32_t odin3_abc_xblif_command_count(const odin3_abc_run *run);
odin3_status odin3_abc_xblif_command(const odin3_abc_run *run, uint32_t index,
                                     const char *in_abs, const char *out_abs, odin3_strbuf *out,
                                     const odin3_abc_cmd **src);

/* All commands, one per line (this is abc<r>.cmd). */
odin3_status odin3_abc_xblif_commands(const odin3_abc_run *run, const char *in_abs,
                                      const char *out_abs, odin3_strbuf *out);

/*
 * Reads ABC's output at path with 1C's reader into a scratch design and fills *out (initialised
 * here; unusable on failure: free it) per §6.2: the inputs and outputs must be exactly the names
 * sent, in any order; a latch is identified by its output name o3q<k>; each $sop becomes a truth
 * table; a one-input `1 1` .names becomes a connection; Kahn's algorithm (ties by node ID) gives
 * the order; registers are numbered by input register k. Then odin3_abc_result_check runs against
 * the region. A malformed network is logged as `abc: ABC returned a malformed network: <why>` with
 * ODIN3_ERR_ENGINE (<why> includes `an unknown output '<name>'`, `a .subckt`, `a latch with init
 * <v> (all were sent at 0)`, `a LUT with N inputs (at most 16)`, `no output network` for a
 * missing file, and the texts of odin3_abc_result_check); NO_MEMORY.
 */
odin3_status odin3_abc_xblif_read(odin3_abc_run *run, const char *path, odin3_abc_result *out);

#endif
