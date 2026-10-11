/*
 * elab_limits.c — the caps in force (elab.h, elab_test.h): the §11 defaults, the byte budget
 * from physical memory, and the test hook that lowers them process-wide.
 */
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "ir/celltype.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

_Static_assert((uint32_t)ODIN3_ELAB_MAX_WIDTH == (uint32_t)ODIN3_READER_MAX_WIDTH,
               "the width cap is the readers' (PHASE2 #5)");
_Static_assert((uint32_t)ODIN3_ELAB_MAX_CELL_PINS == (uint32_t)ODIN3_READER_MAX_TOTAL_WIDTH,
               "the cell pin cap is the readers' total width");
_Static_assert((uint32_t)ODIN3_ELAB_MAX_READMEM_BYTES == (uint32_t)ODIN3_SRC_MAX_FILE_BYTES,
               "a $readmem file is capped like a source file");
_Static_assert((uint32_t)ODIN3_ELAB_MAX_WIDTH == (uint32_t)ODIN3_AST_MAX_NUMBER_BITS,
               "a literal fits a value");

enum { PERCENT = 100 };

uint64_t odin3_elab_default_max_bytes(void) {
    static uint64_t budget;
    if (budget == 0) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_bytes = sysconf(_SC_PAGESIZE);
        budget = UINT64_MAX;
        if (pages > 0 && page_bytes > 0) {
            uint64_t total = (uint64_t)pages * (uint64_t)page_bytes;
            budget = total / PERCENT * ODIN3_ELAB_MAX_BYTES_PERCENT;
        }
    }
    return budget;
}

static odin3_elab_limits defaults(void) {
    return (odin3_elab_limits){
        .max_errors = ODIN3_ELAB_MAX_ERRORS,
        .max_width = ODIN3_ELAB_MAX_WIDTH,
        .max_cell_pins = ODIN3_ELAB_MAX_CELL_PINS,
        .max_loop_iterations = ODIN3_ELAB_MAX_LOOP_ITERATIONS,
        .max_total_iterations = ODIN3_ELAB_MAX_TOTAL_ITERATIONS,
        .max_eval_steps = ODIN3_ELAB_MAX_EVAL_STEPS,
        .max_call_depth = ODIN3_ELAB_MAX_CALL_DEPTH,
        .max_hier_depth = ODIN3_ELAB_MAX_HIER_DEPTH,
        .max_specializations = ODIN3_ELAB_MAX_SPECIALIZATIONS,
        .max_instances = ODIN3_ELAB_MAX_INSTANCES,
        .max_elab_nodes = ODIN3_ELAB_MAX_ELAB_NODES,
        .max_net_bits = ODIN3_ELAB_MAX_NET_BITS,
        .max_init_bits = ODIN3_ELAB_MAX_INIT_BITS,
        .max_readmem_bytes = ODIN3_ELAB_MAX_READMEM_BYTES,
        .max_memory_bits = ODIN3_ELAB_MAX_MEMORY_BITS,
        .max_bytes = odin3_elab_default_max_bytes(),
    };
}

static odin3_elab_limits g_limits;
static bool g_ready;

static uint32_t clamp32(uint32_t wish, uint32_t cap) {
    return wish < cap ? wish : cap;
}

static uint64_t clamp64(uint64_t wish, uint64_t cap) {
    return wish < cap ? wish : cap;
}

void odin3_elab_test_set_limits(const odin3_elab_limits *limits) {
    const odin3_elab_limits dflt = defaults();
    g_ready = true;
    if (limits == NULL) {
        g_limits = dflt;
        return;
    }
    g_limits = (odin3_elab_limits){
        .max_errors = clamp32(limits->max_errors, dflt.max_errors),
        .max_width = clamp32(limits->max_width, dflt.max_width),
        .max_cell_pins = clamp32(limits->max_cell_pins, dflt.max_cell_pins),
        .max_loop_iterations = clamp32(limits->max_loop_iterations, dflt.max_loop_iterations),
        .max_total_iterations = clamp32(limits->max_total_iterations, dflt.max_total_iterations),
        .max_eval_steps = clamp32(limits->max_eval_steps, dflt.max_eval_steps),
        .max_call_depth = clamp32(limits->max_call_depth, dflt.max_call_depth),
        .max_hier_depth = clamp32(limits->max_hier_depth, dflt.max_hier_depth),
        .max_specializations = clamp32(limits->max_specializations, dflt.max_specializations),
        .max_instances = clamp32(limits->max_instances, dflt.max_instances),
        .max_elab_nodes = clamp32(limits->max_elab_nodes, dflt.max_elab_nodes),
        .max_net_bits = clamp32(limits->max_net_bits, dflt.max_net_bits),
        .max_init_bits = clamp32(limits->max_init_bits, dflt.max_init_bits),
        .max_readmem_bytes = clamp32(limits->max_readmem_bytes, dflt.max_readmem_bytes),
        .max_memory_bits = clamp64(limits->max_memory_bits, dflt.max_memory_bits),
        .max_bytes = clamp64(limits->max_bytes, dflt.max_bytes),
    };
}

const odin3_elab_limits *odin3_elab_limits_get(void) {
    if (!g_ready) {
        g_limits = defaults();
        g_ready = true;
    }
    return &g_limits;
}
