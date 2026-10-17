/*
 * hardmap_fixture.h — shared helpers of the 3C unit tests (plan Task 2): flat IR built from RTLIL
 * text with lib/vtr.o3lib read, a hardmap run that checks the IR before and after (check FULL),
 * and the before/after comparison on the 1E simulator. Test code: it asserts on every call
 * instead of returning statuses, except where a status is the point of the test.
 */
#ifndef ODIN3_TESTS_UNIT_HARDMAP_FIXTURE_H
#define ODIN3_TESTS_UNIT_HARDMAP_FIXTURE_H

#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/hardmap/hardmap.h"
#include "proc_build.h"

#include <stdbool.h>
#include <stdint.h>

/* Builds a design into *out; the builder of hm_sim_equal calls it twice. */
typedef odin3_status (*hm_build_fn)(pb_result *out);

/* pb_build_lib with ODIN3_LIB_DIR/vtr.o3lib; asserts nothing, returns the status. */
odin3_status hm_build(const char *rtlil, pb_result *out);

/* hm_build, asserting ODIN3_OK, and the last module made the design's top. */
void hm_build_ok(const char *rtlil, pb_result *out);

/* odin3_hardmap_run on res, with check FULL (RTLIL view) before and after it when the run is OK;
 * returns the run's status. stats may be NULL. */
odin3_status hm_run(pb_result *res, odin3_hardmap_stats *stats);

/* The number of live nodes of module whose cell type is named type. */
uint32_t hm_count(const odin3_module *module, const char *type);

/* The nth live node (from 0) of that cell type in node-ID order, none when absent. */
odin3_node_id hm_nth(const odin3_module *module, const char *type, uint32_t nth);

/*
 * Builds the design twice with build, runs hardmap on the second, and simulates both tops for
 * `cycles` cycles on the seeded random vectors (seed): true when every output of every cycle is
 * equal. Asserts that hardmap returns OK and that the builds and simulator builds succeed.
 */
bool hm_sim_equal(hm_build_fn build, uint32_t cycles, uint64_t seed);

/*
 * As hm_sim_equal, but on every input vector: the top must have at most 20 non-clock input bits
 * (asserted); one cycle per vector.
 */
bool hm_sim_exhaustive(hm_build_fn build);

#endif
