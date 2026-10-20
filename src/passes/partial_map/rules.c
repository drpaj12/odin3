/*
 * rules.c — the two constructors of `pm_rules` (4.2 spec S-2). The only file that holds the 3C
 * numbers.
 */
#include "passes/partial_map/rules.h"

#include "frontends/arch/arch.h"
#include "util/log.h"

#include <inttypes.h>

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

const char *pm_arch_name(const struct odin3_arch *arch) {
    if (arch->library != NULL) {
        return arch->library;
    }
    return arch->file != NULL ? arch->file : "unnamed";
}

/* X-1, X-2: the square multiplier's WIDTH from the record; 0 (with the X-2 warning) for a model
 * that is not square, not in range, or not the template. */
static uint32_t mul_width_of(const odin3_arch *arch) {
    if (!odin3_arch_model_is_template(arch, "multiply")) {
        if (odin3_arch_model_find(arch, "multiply") != NULL) {
            odin3_log(ODIN3_LOG_WARN,
                      "partial_map: 'multiply' model of '%s' does not have the ports a, b and out; "
                      "'$mul' cells are left to lower",
                      pm_arch_name(arch));
        }
        return 0;
    }
    const odin3_arch_model *model = odin3_arch_model_find(arch, "multiply");
    uint32_t wa = odin3_arch_model_port_width(model, "a");
    uint32_t wb = odin3_arch_model_port_width(model, "b");
    if (wa != wb) {
        odin3_log(ODIN3_LOG_WARN,
                  "partial_map: 'multiply' model of '%s' is %" PRIu32 "x%" PRIu32
                  ", not square; '$mul' cells are left to lower",
                  pm_arch_name(arch), wa, wb);
        return 0;
    }
    if (wa == 0 || wa > PM_MUL_WIDTH_CAP) {
        odin3_log(ODIN3_LOG_WARN,
                  "partial_map: 'multiply' model of '%s' is %" PRIu32
                  " wide, outside 1 to %u; '$mul' cells are left to lower",
                  pm_arch_name(arch), wa, PM_MUL_WIDTH_CAP);
        return 0;
    }
    return wa;
}

/* R-2: D, the largest address of any RAM leaf; 0 when there is none or it is out of range. */
static uint32_t mem_depth_of(const odin3_arch *arch) {
    uint32_t depth = odin3_arch_max_addr_pins(arch);
    if (depth > PM_RAM_MAX_ADDR) {
        odin3_log(
            ODIN3_LOG_WARN,
            "partial_map: the RAM models of '%s' have %" PRIu32
            " address pins, more than the %u partial_map handles; RAM cells are left to lower",
            pm_arch_name(arch), depth, PM_RAM_MAX_ADDR);
        return 0;
    }
    return depth;
}

pm_rules pm_rules_from_arch(const struct odin3_arch *arch) {
    pm_rules rules = pm_rules_k6_fixed();
    if (arch == NULL) {
        return rules;
    }
    rules.has_record = true;
    uint32_t width = mul_width_of(arch);
    rules.have_mul = width != 0;
    rules.mul_width = width != 0 ? width : rules.mul_width;
    rules.have_adder = odin3_arch_model_is_template(arch, "adder");
    uint32_t depth = mem_depth_of(arch);
    bool ram = depth >= PM_RAM_DEPTH_MIN;
    rules.have_sp = ram && odin3_arch_model_is_template(arch, "single_port_ram");
    rules.have_dp = ram && odin3_arch_model_is_template(arch, "dual_port_ram");
    rules.mem_depth = ram ? depth : rules.mem_depth;
    return rules;
}
