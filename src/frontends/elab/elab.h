/*
 * elab.h — elaboration (2D) shared by both stages: the caps (spec §11, EL-46), the options
 * (§2.2, EL-3) and, from Tasks 10 and 17, the stage APIs.
 *
 * Every cap is checked before the allocation it guards and reported as a located PARSE error
 * naming it (§9.1, plan point (aa)). Tests lower the caps through odin3_elab_test_set_limits
 * (elab_test.h); the elaborator reads the caps in force through odin3_elab_limits_get.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_H
#define ODIN3_FRONTENDS_ELAB_ELAB_H

#include "ast/srcman.h"

#include <stdbool.h>
#include <stdint.h>

/* The caps of §11, exact. */
enum {
    ODIN3_ELAB_MAX_ERRORS = 20,                /* errors per run; then elaboration stops */
    ODIN3_ELAB_MAX_WIDTH = 1 << 20,            /* bits of one object, expression or value */
    ODIN3_ELAB_MAX_CELL_PINS = 1 << 22,        /* pins of one cell 2D creates */
    ODIN3_ELAB_MAX_LOOP_ITERATIONS = 1 << 20,  /* iterations of one loop */
    ODIN3_ELAB_MAX_TOTAL_ITERATIONS = 1 << 24, /* unrolled loop iterations per run */
    ODIN3_ELAB_MAX_EVAL_STEPS = 1 << 26,       /* steps of one top-level constant evaluation */
    ODIN3_ELAB_MAX_CALL_DEPTH = 1024,          /* constant-function frames, inline nesting */
    ODIN3_ELAB_MAX_HIER_DEPTH = 1024,          /* instance nesting below the top */
    ODIN3_ELAB_MAX_SPECIALIZATIONS = 1 << 16,  /* modules made per run */
    ODIN3_ELAB_MAX_INSTANCES = 1 << 24,        /* instance and gate nodes per run */
    ODIN3_ELAB_MAX_ELAB_NODES = 1 << 25,       /* nodes of the elaborated store */
    ODIN3_ELAB_MAX_NET_BITS = 1 << 26,         /* nets created per run */
    ODIN3_ELAB_MAX_MEMORY_BITS = 1 << 30,      /* SIZE x WIDTH of one declared memory */
    ODIN3_ELAB_MAX_INIT_BITS = 1 << 26,        /* initial-value bits materialised per run */
    ODIN3_ELAB_MAX_READMEM_BYTES = 1 << 28,    /* one $readmem file */
    ODIN3_ELAB_MAX_MODULE_NAME = 128,          /* longer specialization names are hashed */
    ODIN3_ELAB_MAX_BYTES_PERCENT = 60          /* the byte budget: this % of physical RAM */
};

/*
 * ODIN3_ELAB_MAX_BYTES, the run-level byte budget: ODIN3_ELAB_MAX_BYTES_PERCENT of physical
 * memory (sysconf pages x page size), computed once; UINT64_MAX when the system does not say.
 */
uint64_t odin3_elab_default_max_bytes(void);

/* The options of one elaboration (§2.2, EL-3). */
typedef struct odin3_elab_opts {
    uint32_t top;       /* strtab ID of "[lib.]mod" (CLI --top, else the project top); 0 = auto */
    odin3_loc top_at;   /* where the top was given (project line); 0 = CLI or none */
    bool nolatches;     /* Yosys -nolatches: unassigned paths of combinational blocks are x */
    bool nomem2reg;     /* never replace a memory by registers except where forced (EL-27) */
    bool auto_blackbox; /* an undefined module becomes a black box, not unresolved_module */
} odin3_elab_opts;

#endif
