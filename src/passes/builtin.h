/*
 * builtin.h — the built-in pass table (private to src/passes: never part of the public ABI).
 */
#ifndef ODIN3_PASSES_BUILTIN_H
#define ODIN3_PASSES_BUILTIN_H

#include "passes/manager.h"

#include <stdint.h>

/* The built-in passes (builtin.c), in registry order; only the manager reads them. */
extern const odin3_pass_def *const odin3_builtin_passes[];
extern const uint32_t odin3_builtin_pass_count;

#endif
