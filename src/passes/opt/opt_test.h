/*
 * opt_test.h — test hooks of the opt family (2F plan Task 1, Global Constraints): lowered caps and
 * a call hook. Never part of the ABI. The limits and the call hook live in kit.c, the key-hash
 * hooks in merge.c (opt_merge) and reduce.c (opt_reduce).
 */
#ifndef ODIN3_PASSES_OPT_OPT_TEST_H
#define ODIN3_PASSES_OPT_OPT_TEST_H

#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
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
 * on_expr_setup: called with begin = true before, and false after, opt_expr's per-round setup (the
 * invert map, the FF-polarity scan and the global topological sort; O(N) per non-empty round, spec
 * section 19), which bench_opt times apart from the rule visits (Task 15). May be NULL.
 */
typedef struct odin3_opt_test_hooks {
    void (*on_call)(void *user, const char *sub);
    void *user;
    void (*on_expr_setup)(void *user, bool begin);
    /* after_run: called with the sub-pass's name after its body ran, before the Debug check and
     * the --opt-simcheck comparison (Task 17: a test injects a wrong rewrite here). */
    void (*after_run)(void *user, const char *sub, odin3_design *design);
} odin3_opt_test_hooks;

/* Installs the hooks (copied; NULL clears them). */
void odin3_opt_test_set_hooks(const odin3_opt_test_hooks *hooks);

/* The hooks in force (all NULL when none are installed); never NULL. */
const odin3_opt_test_hooks *odin3_opt_test_hooks_now(void);

/*
 * Replaces opt_merge's key hash (finding 29): fn gets the encoded key's bytes; NULL restores the
 * real hash. Tests pass a constant to put every key on one collision chain.
 */
void odin3_opt_merge_test_set_key_hash(uint64_t (*fn)(const void *key, size_t len));

/*
 * Replaces opt_reduce's operand-vector hash: fn gets the keys' bytes; NULL restores the real hash.
 * Tests pass a constant to put every group on one collision chain.
 */
void odin3_opt_reduce_test_set_key_hash(uint64_t (*fn)(const void *key, size_t len));

#endif
