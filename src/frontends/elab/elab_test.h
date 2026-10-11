/*
 * elab_test.h — hidden test hooks of elaboration (2D; never part of the ABI): the caps a test
 * may lower (spec §11), and, from Task 10, the stage hooks. The elaborator reads the caps in
 * force through odin3_elab_limits_get, so a lowered cap is hit cheaply.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_TEST_H
#define ODIN3_FRONTENDS_ELAB_ELAB_TEST_H

#include <stdint.h>

/* The caps of §11 (elab.h), as the elaborator reads them; the defaults are the named caps. */
typedef struct odin3_elab_limits {
    uint32_t max_errors;           /* ODIN3_ELAB_MAX_ERRORS */
    uint32_t max_width;            /* ODIN3_ELAB_MAX_WIDTH */
    uint32_t max_cell_pins;        /* ODIN3_ELAB_MAX_CELL_PINS */
    uint32_t max_loop_iterations;  /* ODIN3_ELAB_MAX_LOOP_ITERATIONS */
    uint32_t max_total_iterations; /* ODIN3_ELAB_MAX_TOTAL_ITERATIONS */
    uint32_t max_eval_steps;       /* ODIN3_ELAB_MAX_EVAL_STEPS */
    uint32_t max_call_depth;       /* ODIN3_ELAB_MAX_CALL_DEPTH */
    uint32_t max_hier_depth;       /* ODIN3_ELAB_MAX_HIER_DEPTH */
    uint32_t max_specializations;  /* ODIN3_ELAB_MAX_SPECIALIZATIONS */
    uint32_t max_instances;        /* ODIN3_ELAB_MAX_INSTANCES */
    uint32_t max_elab_nodes;       /* ODIN3_ELAB_MAX_ELAB_NODES */
    uint32_t max_net_bits;         /* ODIN3_ELAB_MAX_NET_BITS */
    uint32_t max_init_bits;        /* ODIN3_ELAB_MAX_INIT_BITS */
    uint32_t max_readmem_bytes;    /* ODIN3_ELAB_MAX_READMEM_BYTES */
    uint64_t max_memory_bits;      /* ODIN3_ELAB_MAX_MEMORY_BITS */
    uint64_t max_bytes;            /* odin3_elab_default_max_bytes() */
} odin3_elab_limits;

/*
 * Replaces the caps in force, process-wide, for the rest of the test run (elaboration is
 * single-threaded). Each value above its default is clamped to the default, so a test can only
 * lower a cap. NULL restores the defaults.
 */
void odin3_elab_test_set_limits(const odin3_elab_limits *limits);

/* The caps in force: the defaults unless a test lowered them. Never NULL. */
const odin3_elab_limits *odin3_elab_limits_get(void);

#endif
