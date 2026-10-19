/*
 * opt_test.h — test hooks of the opt family (2F plan Task 1, Global Constraints): lowered caps and
 * a call hook. Never part of the ABI. The limits and the call hook live in kit.c, the key-hash
 * hooks in merge.c (opt_merge) and reduce.c (opt_reduce).
 */
#ifndef ODIN3_PASSES_OPT_OPT_TEST_H
#define ODIN3_PASSES_OPT_OPT_TEST_H

#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/opt/opt.h"

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

/* wreduce's round cap (ODIN3_WREDUCE_MAX_ROUNDS): 0 restores the default, else clamped to
 * [1, default]; odin3_opt_test_set_limits(NULL) restores it too. */
void odin3_opt_test_set_wreduce_rounds(uint32_t rounds);
uint32_t odin3_opt_wreduce_rounds_now(void);

/*
 * The initial wreduce ranks of `module` (wr_order_ranks, no rewrite): ranks[i] is the rank of live
 * node i (1..N, operand before consumer), 0 for a dead or absent node, for i < count.
 */
odin3_status odin3_opt_wreduce_probe_ranks(odin3_pass_ctx *ctx, odin3_design *design,
                                           odin3_module *module, const odin3_opt_opts *opts,
                                           uint32_t *ranks, uint32_t count);

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

#ifndef NDEBUG
/*
 * Debug builds only: opt_dff's check of the FFs it skipped (dff.c). skip, when set, is asked for
 * every FF and a true answer makes opt_dff skip that FF whatever the journal says; violations,
 * when not NULL, counts the skipped FFs the check finds would change instead of aborting. Both
 * NULL restores the defaults. Tests use it to inject a skip that is wrong.
 */
void odin3_opt_test_dff_skip(bool (*skip)(uint32_t node), uint32_t *violations);
#endif

#endif
