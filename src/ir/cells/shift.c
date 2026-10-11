/*
 * shift.c — word-level shift cells $shl, $shr, $sshr (B is the shift amount), and (2D, EL-31)
 * $shiftx.
 */
#include "cells.h"

ODIN3_BINARY(odin3_cell_shl, "$shl");
ODIN3_BINARY(odin3_cell_shr, "$shr");
ODIN3_BINARY(odin3_cell_sshr, "$sshr");
/*
 * Y = A[B +: Y_WIDTH] (simlib; B signed when B_SIGNED, so a negative B selects below bit 0): the
 * bits outside A read x. No simulate hook until a pass needs one (2D spec §7.2).
 */
ODIN3_BINARY(odin3_cell_shiftx, "$shiftx");
