/*
 * dispatch.c — the rule table of `lower` (3B plan Task 1, point (e)): one entry per word cell
 * type that has a builder, found by the type's name (resolved to type IDs once per run by the
 * driver), the types that deliberately have none and the ones whose rule a later task adds.
 */
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The rules, sorted by type name; the last entry is the sentinel. */
#define LW_RULE(type, name)                                                                        \
    { type, lw_build_##name, lw_est_##name }

static const lw_rule k_rules[] = {
    LW_RULE("$add", add),
    LW_RULE("$adff", adff),
    LW_RULE("$adffe", adffe),
    LW_RULE("$aldff", aldff),
    LW_RULE("$aldffe", aldffe),
    LW_RULE("$and", and),
    LW_RULE("$dff", dff),
    LW_RULE("$dffe", dffe),
    LW_RULE("$dffsr", dffsr),
    LW_RULE("$dffsre", dffsre),
    LW_RULE("$div", div),
    LW_RULE("$eq", eq),
    LW_RULE("$ge", ge),
    LW_RULE("$gt", gt),
    LW_RULE("$le", le),
    LW_RULE("$logic_and", logic_and),
    LW_RULE("$logic_not", logic_not),
    LW_RULE("$logic_or", logic_or),
    LW_RULE("$lt", lt),
    LW_RULE("$mod", mod),
    LW_RULE("$mul", mul),
    LW_RULE("$mux", mux),
    LW_RULE("$ne", ne),
    LW_RULE("$neg", neg),
    LW_RULE("$not", not ),
    LW_RULE("$or", or),
    LW_RULE("$pmux", pmux),
    LW_RULE("$reduce_and", reduce_and),
    LW_RULE("$reduce_bool", reduce_or),
    LW_RULE("$reduce_or", reduce_or),
    LW_RULE("$reduce_xnor", reduce_xnor),
    LW_RULE("$reduce_xor", reduce_xor),
    LW_RULE("$sdff", sdff),
    LW_RULE("$sdffce", sdffce),
    LW_RULE("$sdffe", sdffe),
    LW_RULE("$shiftx", shiftx),
    LW_RULE("$shl", shl),
    LW_RULE("$shr", shr),
    LW_RULE("$sshr", sshr),
    LW_RULE("$sub", sub),
    LW_RULE("$xnor", xnor),
    LW_RULE("$xor", xor),
    {NULL, NULL, NULL},
};

uint32_t lw_rule_count(void) {
    uint32_t count = 0;
    while (k_rules[count].type_name != NULL) {
        count++;
    }
    return count;
}

const lw_rule *lw_rule_at(uint32_t idx) {
    return idx < lw_rule_count() ? &k_rules[idx] : NULL;
}

/* Test hook (lower_test.h): a rule that shadows the table entry of its type name. */
static const lw_rule *g_override = NULL;

void odin3_lower_test_override_rule(const lw_rule *rule) {
    g_override = rule;
}

const lw_rule *lw_rule_find(const char *type_name) {
    if (g_override != NULL && strcmp(g_override->type_name, type_name) == 0) {
        return g_override;
    }
    for (uint32_t i = 0; k_rules[i].type_name != NULL; i++) {
        if (strcmp(k_rules[i].type_name, type_name) == 0) {
            return &k_rules[i];
        }
    }
    return NULL;
}

/* Word types with no bit form, by design (spec §5.7, §7, §8, Peter's ruling 4 of PHASE3 #32). */
static const char *const k_no_rule[] = {"$proc", "$tribuf", "$pow", "$memrd", "$memwr"};

/* Word types whose rule a later task adds (plan Tasks 4 to 8). */
static const char *const k_pending[] = {"$dlatch", "$adlatch", "$dlatchsr", "$sr", "$mem"};

static bool listed(const char *const *names, size_t count, const char *type_name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], type_name) == 0) {
            return true;
        }
    }
    return false;
}

bool lw_no_rule_listed(const char *type_name) {
    return listed(k_no_rule, sizeof k_no_rule / sizeof k_no_rule[0], type_name);
}

bool lw_rule_pending(const char *type_name) {
    return listed(k_pending, sizeof k_pending / sizeof k_pending[0], type_name);
}

void lw_log_no_rule(const lw_ctx *lw, odin3_node_id node, const char *type_name) {
    char label[LW_LABEL_LEN];
    lw_node_label(lw, node, label, sizeof label);
    const char *module =
        odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module));
    if (strcmp(type_name, "$pow") == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "lower: module '%s': '$pow' '%s' has no bit-level form (run opt first; Yosys "
                  "leaves it unmapped)",
                  module, label);
    } else if (strcmp(type_name, "$tribuf") == 0) {
        odin3_log(ODIN3_LOG_ERROR, "lower: '$tribuf' '%s' has no bit-level cell in the registry",
                  label);
    } else if (strcmp(type_name, "$memrd") == 0 || strcmp(type_name, "$memwr") == 0) {
        odin3_log(ODIN3_LOG_ERROR, "lower: standalone $memrd/$memwr '%s' (use $mem)", label);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "lower: no lowering rule for cell type '%s' ('%s')%s", type_name,
                  label, lw_rule_pending(type_name) ? ": not implemented yet (3B-2)" : "");
    }
}
