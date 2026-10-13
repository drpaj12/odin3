/*
 * elab_test.h — hidden test hooks of elaboration (2D; never part of the ABI): the caps a test
 * may lower (spec §11), the forced pending kinds (plan point (l)) and the stage hooks. The
 * elaborator reads the caps and hooks in force through odin3_elab_limits_get and
 * odin3_elab_test_hooks_get, so a lowered cap is hit cheaply.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB_TEST_H
#define ODIN3_FRONTENDS_ELAB_ELAB_TEST_H

#include "ast/ast.h"

#include <stdbool.h>
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
    uint32_t max_inline_depth;     /* ODIN3_ELAB_MAX_INLINE_DEPTH */
    uint32_t max_loop_objects;     /* ODIN3_ELAB_MAX_LOOP_OBJECTS */
    uint32_t max_reentry_depth;    /* ODIN3_ELAB_MAX_REENTRY_DEPTH */
    uint32_t max_hier_depth;       /* ODIN3_ELAB_MAX_HIER_DEPTH */
    uint32_t max_specializations;  /* ODIN3_ELAB_MAX_SPECIALIZATIONS */
    uint32_t max_instances;        /* ODIN3_ELAB_MAX_INSTANCES */
    uint32_t max_elab_nodes;       /* ODIN3_ELAB_MAX_ELAB_NODES */
    uint32_t max_net_bits;         /* ODIN3_ELAB_MAX_NET_BITS */
    uint32_t max_init_bits;        /* ODIN3_ELAB_MAX_INIT_BITS */
    uint32_t max_readmem_bytes;    /* ODIN3_ELAB_MAX_READMEM_BYTES */
    uint32_t max_eval_work;        /* ODIN3_ELAB_MAX_EVAL_WORK */
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

/*
 * Forces kind_or_feature (an AST kind or an odin3_elab_pend_feature, elab.h) pending in stage
 * (ODIN3_ELAB_STAGE1 or _STAGE2) when pending is true, so plan point (l)'s mechanism stays
 * testable once the tables are empty; false removes the force (the table decides again).
 * Process-wide, until changed; out-of-range arguments are ignored.
 */
void odin3_elab_test_pend(uint8_t stage, uint32_t kind_or_feature, bool pending);

/*
 * The stage hooks (all off when zero): drop_log_entry N > 0 makes stage 1's pass W skip the Nth
 * decision-log entry (Task 11); on_body_start(user, module) runs as stage 2 starts the body of
 * each elaborated MODULE that has one (Task 17; a black box has none); refuse_port N > 0 makes
 * stage 2 treat the Nth port it creates in the run as refused by the IR (Task 17, the failure
 * path of odin3_elab2_ports: an IR refusal cannot otherwise be provoked by valid input);
 * after_prescan(user, e2, module) runs when the pre-scan of a module (step (2), Task 19) is done,
 * so a test can read what it collected (odin3_elab2_always_assigned).
 */
struct odin3_elab2; /* elab_internal.h */

typedef struct odin3_elab_test_hooks {
    uint32_t drop_log_entry;
    void (*on_body_start)(void *user, odin3_ast_id module_node);
    void *user;
    uint32_t refuse_port;
    void (*after_prescan)(void *user, struct odin3_elab2 *e2, odin3_ast_id module_node);
} odin3_elab_test_hooks;

/* Installs a copy of *hooks process-wide (NULL: every hook off). */
void odin3_elab_test_set_hooks(const odin3_elab_test_hooks *hooks);

/* The hooks in force (all zero unless a test installed some). Never NULL. */
const odin3_elab_test_hooks *odin3_elab_test_hooks_get(void);

struct odin3_elab_opts; /* elab.h */

/*
 * The options the last odin3_elab_run used, after its step 1 (the pass's flags, the top filled
 * from --top or the project), so a test sees what read_verilog passed; all zero before any run.
 * odin3_elab_test_note_opts is how odin3_elab_run records them.
 */
const struct odin3_elab_opts *odin3_elab_test_last_opts(void);
void odin3_elab_test_note_opts(const struct odin3_elab_opts *opts);

#endif
