/*
 * opt_test.h — test hooks of the opt family (2F plan Task 1, Global Constraints): lowered caps and
 * a call hook. Never part of the ABI. The hooks live in kit.c.
 */
#ifndef ODIN3_PASSES_OPT_OPT_TEST_H
#define ODIN3_PASSES_OPT_OPT_TEST_H

#include <stdint.h>

/* The caps of opt.h that tests lower: ODIN3_OPT_MAX_ITERATIONS, ODIN3_OPT_MUXTREE_MAX_EVALS. */
typedef struct odin3_opt_limits {
    uint32_t max_iterations, muxtree_max_evals;
} odin3_opt_limits;

/* Sets the caps (NULL restores the defaults; each value is clamped to [1, its default]). */
void odin3_opt_test_set_limits(const odin3_opt_limits *limits);

/* The caps in force (the defaults unless a test lowered them). */
odin3_opt_limits odin3_opt_limits_now(void);

/*
 * on_call: called with the sub-pass's name ("opt_expr", "opt_merge", ...) at each sub-pass call of
 * the opt driver (Task 13). May be NULL.
 */
typedef struct odin3_opt_test_hooks {
    void (*on_call)(void *user, const char *sub);
    void *user;
} odin3_opt_test_hooks;

/* Installs the hooks (copied; NULL clears them). */
void odin3_opt_test_set_hooks(const odin3_opt_test_hooks *hooks);

/* The hooks in force (all NULL when none are installed); never NULL. */
const odin3_opt_test_hooks *odin3_opt_test_hooks_now(void);

#endif
