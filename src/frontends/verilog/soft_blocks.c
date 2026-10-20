/* soft_blocks.c — see soft_blocks.h. */
#include "frontends/verilog/soft_blocks.h"

#include "frontends/arch/arch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* Appends candidate to tried (", " separated) and true when it is an existing directory, copied
 * to dir. */
static bool try_dir(const char *candidate, char *dir, char *tried) {
    struct stat info;
    size_t used = strlen(tried);
    (void)snprintf(tried + used, SOFT_DIR_CAP - used, "%s'%s'", used > 0 ? ", " : "", candidate);
    if (stat(candidate, &info) != 0 || !S_ISDIR(info.st_mode)) {
        return false;
    }
    (void)snprintf(dir, SOFT_DIR_CAP, "%s", candidate);
    return true;
}

/* The directory of the running executable, "" when it is not known. */
static void exe_dir(char *out) {
    ssize_t len = readlink("/proc/self/exe", out, SOFT_DIR_CAP - 1);
    out[0] = len > 0 ? out[0] : '\0';
    out[len > 0 ? len : 0] = '\0';
    char *slash = strrchr(out, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
}

bool odin3_soft_blocks_dir(char *dir, char *tried) {
    dir[0] = '\0';
    tried[0] = '\0';
    const char *env = getenv("ODIN3_SOFT_PRIMITIVES_DIR");
    if (env != NULL && env[0] != '\0') {
        return try_dir(env, dir, tried); /* given: no fallback */
    }
    char base[SOFT_DIR_CAP];
    char path[SOFT_DIR_CAP];
    exe_dir(base);
    static const char *const k_rel[] = {"%s/../share/odin3/soft_primitives",
                                        "%s/../../lib/soft_primitives"};
    for (size_t i = 0; base[0] != '\0' && i < sizeof k_rel / sizeof k_rel[0]; i++) {
        (void)snprintf(path, sizeof path, k_rel[i], base);
        if (try_dir(path, dir, tried)) {
            return true;
        }
    }
#ifdef ODIN3_SOFT_PRIMITIVES_DIR
    return try_dir(ODIN3_SOFT_PRIMITIVES_DIR, dir, tried);
#else
    return false;
#endif
}
