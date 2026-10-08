/*
 * example_plugin.c — the smallest valid Odin III plugin. Load it with
 *   odin3 --plugin build/debug/plugins/example_c/example_plugin.so
 * From Phase 1 a plugin registers passes, cell types, readers and writers here.
 */
#include "odin3/odin3.h"

#include <stdio.h>

/* Exported entry point; declared here because odin3.h only declares its type. */
odin3_status odin3_plugin_init(uint32_t host_abi_version);

odin3_status odin3_plugin_init(uint32_t host_abi_version) {
    if (host_abi_version != (uint32_t)ODIN3_ABI_VERSION) {
        return ODIN3_ERR_ABI_MISMATCH;
    }
    (void)fprintf(stderr, "example_plugin: initialised (ABI %u)\n", host_abi_version);
    return ODIN3_OK;
}
