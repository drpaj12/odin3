/*
 * timing.h — timing hints for the abc pass (spec docs/specs/2026-11-14-timing-hints.md, section
 * on the abc consumer). For one region it turns the loaded hints into arrival times of the
 * region's inputs and required times of its outputs and register inputs, in LUT levels, and
 * writes them to a file that the ABC command `o3_timing` (adapters/abc/bridge.c) reads just
 * before the mapper `if`. Without hints nothing here runs and the run is as before.
 */
#ifndef ODIN3_PASSES_ABC_TIMING_H
#define ODIN3_PASSES_ABC_TIMING_H

#include "odin3/odin3.h"
#include "passes/abc/run.h"

#include <stdbool.h>
#include <stdint.h>

/* The first script command that is the mapper `if` (its index), or UINT32_MAX when there is none.
 */
uint32_t odin3_abc_timing_slot(const odin3_abc_script *script);

/*
 * Start-up: finds the hints of the design and decides whether this run can use them. They need the
 * BLIF exchange, the linked engine, no --gates and a script with an `if` command; otherwise one
 * WARNING says why and run->hints stays NULL. Sets run->hints and run->timing_slot.
 */
void odin3_abc_timing_start(odin3_abc_run *run);

/*
 * For the current region (run->module, run->region): marks the hints that match a gate, computes
 * the times and, when there is at least one, writes them to path and sets run->timing_on. Always
 * clears run->timing_on first; no hints or no match leaves it false. ODIN3_ERR_IO (logged),
 * NO_MEMORY.
 */
odin3_status odin3_abc_timing_region(odin3_abc_run *run, const char *path);

/* At the end of the pass: one INFO line with the counts and a WARNING for unmatched hints. */
void odin3_abc_timing_report(const odin3_abc_run *run);

#endif
