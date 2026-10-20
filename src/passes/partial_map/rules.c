/*
 * rules.c — the two constructors of `pm_rules` (4.2 spec S-2). The only file that holds the 3C
 * numbers.
 */
#include "passes/partial_map/rules.h"

#include "frontends/arch/arch.h"

enum {
    K6_MUL_WIDTH = 36U,
    K6_MIN_MUL = 3U,
    K6_MIN_ADD = 1U,
    K6_MEM_DEPTH = 15U,
    K6_MEM_WIDTH = 1U
};

pm_rules pm_rules_k6_fixed(void) {
    pm_rules rules;
    rules.has_record = false;
    rules.have_mul = true;
    rules.mul_width = K6_MUL_WIDTH;
    rules.min_mul = K6_MIN_MUL;
    rules.have_adder = true;
    rules.min_add = K6_MIN_ADD;
    rules.have_sp = true;
    rules.have_dp = true;
    rules.mem_depth = K6_MEM_DEPTH;
    rules.mem_width = K6_MEM_WIDTH;
    return rules;
}

pm_rules pm_rules_from_arch(const struct odin3_arch *arch) {
    pm_rules rules = pm_rules_k6_fixed();
    rules.has_record = arch != NULL;
    return rules;
}
