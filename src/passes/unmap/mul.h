/*
 * mul.h — the hard `multiply` cell back to `$mul` (spec M-1 to M-3; plan Task 6).
 */
#ifndef ODIN3_PASSES_UNMAP_MUL_H
#define ODIN3_PASSES_UNMAP_MUL_H

#include "odin3/odin3.h"
#include "passes/unmap/unmap.h"

/* The ports of the `multiply` cell. */
enum { UM_MUL_A, UM_MUL_B, UM_MUL_OUT };

/* Phase B: every live `multiply` of the module becomes a `$mul` with `unmap_w` = its WIDTH. */
odin3_status um_mul_phase(um_ctx *uc);

#endif
