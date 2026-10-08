/* logic.c — word-level bitwise cells $and, $or, $xor, $not. */
#include "cells.h"

ODIN3_BINARY(odin3_cell_and, "$and");
ODIN3_BINARY(odin3_cell_or, "$or");
ODIN3_BINARY(odin3_cell_xor, "$xor");
ODIN3_UNARY(odin3_cell_not, "$not");
