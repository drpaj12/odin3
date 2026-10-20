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
#define PM_MUL_WIDTH_CAP 64U  /* the widest multiplier the rules build */
#define PM_RAM_MAX_ADDR 31U   /* an instance of more address bits is refused (R-1) */
#define PM_RAM_DEPTH_MIN 1U   /* the smallest RAM address limit */
#define PM_RAM_MODE_CAP 16U   /* the distinct (address, data) modes kept per RAM model (R-4) */
#define PM_RAM_WIDTH_CAP 128U /* the widest data slice (R-4); a wider mode is ignored */

/* One mode of a RAM model: the address pins and data pins of a leaf. */
typedef struct pm_ram_mode {
    uint32_t addr;
    uint32_t data;
} pm_ram_mode;

typedef struct pm_rules {
    bool has_record;    /* the design has an arch record: an absent model is soft (S-3) */
    bool have_mul;      /* the arch has a `multiply` model */
    uint32_t mul_width; /* the square multiplier's WIDTH (X-1) */
    uint32_t
        min_mul; /* `--min-mult`: a multiplier narrower than this, both operands >= 2, is soft */
    bool have_adder;        /* the arch has an `adder` model */
    uint32_t min_add;       /* `--min-add`: an add of fewer bits than this is soft */
    bool have_sp;           /* the arch has a single-port RAM model */
    bool have_dp;           /* the arch has a dual-port RAM model */
    uint32_t mem_depth;     /* D: the address bits of one RAM block (R-2) */
    uint32_t mem_width;     /* the data bits of one RAM slice (R-3: 1) */
    bool mem_arch;          /* `--mem-width arch` is on and the record has RAM modes (R-4) */
    uint32_t depth_kind[2]; /* D of each kind (R-2): the most address pins of its leaves; 0: none */
    uint32_t n_modes[2];    /* [0] single_port_ram, [1] dual_port_ram */
    pm_ram_mode modes[2][PM_RAM_MODE_CAP]; /* distinct modes in document order */
} pm_rules;

/* The shape of one RAM block for a memory of addr_bits address bits (R-4, R-5): `addr` is the
 * block's ADDR_WIDTH, `width` the most data bits of one slice. Default (mem_arch off): D by the
 * 1-bit slice. With mem_arch: A' = min(addr_bits, D); the mode of the model (dual: dual_port_ram)
 * with address pins >= A' and the most data pins, the fewest address pins among equals; no such
 * mode gives the default. */
typedef struct pm_ram_shape {
    uint32_t addr;
    uint32_t width;
} pm_ram_shape;

/* D of the kind (dual: dual_port_ram): the address bits of one block (R-2); mem_depth when the
 * record gives the kind none (a model that is absent is never built). */
uint32_t pm_ram_depth(const pm_rules *rules, bool dual);

pm_ram_shape pm_ram_shape_of(const pm_rules *rules, bool dual, uint32_t addr_bits);

/* The 3C values: multiplier 36, min multiplier 3, min adder 1, RAM 15 address bits by 1. */
pm_rules pm_rules_k6_fixed(void);

/* The name of the arch for messages: its library name, else its file, else "unnamed". */
const char *pm_arch_name(const struct odin3_arch *arch);

/* The values for the arch record (NULL: pm_rules_k6_fixed). */
pm_rules pm_rules_from_arch(const struct odin3_arch *arch);

#endif
