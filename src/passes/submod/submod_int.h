/*
 * submod_int.h — what register.c shares with submod.c (Phase 7 spec 3.1, plan Task 5). Internal.
 */
#ifndef ODIN3_PASSES_SUBMOD_SUBMOD_INT_H
#define ODIN3_PASSES_SUBMOD_SUBMOD_INT_H

#include "passes/submod/submod.h"

/* One group to extract: the attribute key, its value text and the new module's name. */
typedef struct odin3_submod_group {
    const char *key;
    const char *value;
    const char *name;
} odin3_submod_group;

/*
 * odin3_submod_extract_ctx with the new module named group->name instead of the value. Same
 * failures; *out (may be NULL, zeroed or filled) gets the result.
 */
odin3_status odin3_submod_extract_named(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                        const odin3_submod_group *group, odin3_submod_result *out);

/*
 * The refusals of odin3_submod_extract_named for this group (same failures and log lines), without
 * changing the IR: the pass runs it for every group before it moves any.
 */
odin3_status odin3_submod_check_named(odin3_pass_ctx *ctx, odin3_module_id module_id,
                                      const odin3_submod_group *group);

#endif
