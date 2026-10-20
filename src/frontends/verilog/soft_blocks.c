/* soft_blocks.c — see soft_blocks.h. */
#include "frontends/verilog/soft_blocks.h"

#include "frontends/arch/arch.h"

#include <stdlib.h>

static const char *const k_names[SOFT_BLOCK_COUNT] = {"single_port_ram", "dual_port_ram",
                                                      "multiply", "adder"};

const char *odin3_soft_block_name(uint32_t index) {
    return index < SOFT_BLOCK_COUNT ? k_names[index] : "";
}

uint32_t odin3_soft_blocks_missing(const odin3_design *design) {
    const odin3_arch *arch = odin3_design_arch(design);
    uint32_t mask = 0;
    for (uint32_t i = 0; arch != NULL && i < SOFT_BLOCK_COUNT; i++) {
        mask |= odin3_arch_model_find(arch, k_names[i]) == NULL ? (1U << i) : 0U;
    }
    return mask;
}

const char *odin3_soft_blocks_dir(void) {
    const char *env = getenv("ODIN3_SOFT_PRIMITIVES_DIR");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
#ifdef ODIN3_SOFT_PRIMITIVES_DIR
    return ODIN3_SOFT_PRIMITIVES_DIR;
#else
    return NULL;
#endif
}
