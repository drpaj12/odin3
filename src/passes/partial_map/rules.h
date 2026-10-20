/*
 * rules.h — `pm_rules`, the numbers the `partial_map` rules read (4.2 spec S-2). Every constant
 * that 3C wrote into a rule file (the multiplier width, the RAM address limit, the RAM data slice,
 * the smallest multiplier and adder) is a field here, with no second copy. Internal: not part of
 * the ABI.
 *
 * Two constructors. pm_rules_k6_fixed() gives the 3C values (the design has no arch record: the
 * Phase 3 script). pm_rules_from_arch() derives them from the record: the multiplier width, the RAM
 * depth and the `have_*` flags (rules.c). The fixed capacities below size the
 * pin arrays of the rules: a width or depth above them is refused by the constructor.
 */
#ifndef ODIN3_PASSES_PARTIAL_MAP_RULES_H
#define ODIN3_PASSES_PARTIAL_MAP_RULES_H

#include <stdbool.h>
#include <stdint.h>

struct odin3_arch; /* src/frontends/arch/arch.h */

/* Capacities of the rules' pin arrays. */
#define PM_MUL_WIDTH_CAP 64U /* the widest multiplier the rules build */
#define PM_RAM_MAX_ADDR 31U  /* an instance of more address bits is refused (R-1) */
#define PM_RAM_DEPTH_MIN 1U  /* the smallest RAM address limit */

typedef struct pm_rules {
    bool has_record;    /* the design has an arch record: an absent model is soft (S-3) */
    bool have_mul;      /* the arch has a `multiply` model */
    uint32_t mul_width; /* the square multiplier's WIDTH (X-1) */
    uint32_t
        min_mul; /* `--min-mult`: a multiplier narrower than this, both operands >= 2, is soft */
    bool have_adder;    /* the arch has an `adder` model */
    uint32_t min_add;   /* `--min-add`: an add of fewer bits than this is soft */
    bool have_sp;       /* the arch has a single-port RAM model */
    bool have_dp;       /* the arch has a dual-port RAM model */
    uint32_t mem_depth; /* D: the address bits of one RAM block (R-2) */
    uint32_t mem_width; /* the data bits of one RAM slice (R-4) */
} pm_rules;

/* The 3C values: multiplier 36, min multiplier 3, min adder 1, RAM 15 address bits by 1. */
pm_rules pm_rules_k6_fixed(void);

/* The name of the arch for messages: its library name, else its file, else "unnamed". */
const char *pm_arch_name(const struct odin3_arch *arch);

/* The values for the arch record (NULL: pm_rules_k6_fixed). */
pm_rules pm_rules_from_arch(const struct odin3_arch *arch);

#endif
