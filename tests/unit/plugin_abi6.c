/*
 * plugin_abi6.c — a test plugin as an ABI 6 build left it: its init accepts only host ABI 6, so
 * the ABI 7 host's odin3_plugin_load must refuse it with ODIN3_ERR_ABI_MISMATCH (test_api).
 */
#include "odin3/odin3.h"

#include <stdint.h>

enum { BUILT_FOR_ABI = 6 };

/* Exported entry point; declared here because odin3.h only declares its type. */
odin3_status odin3_plugin_init(uint32_t host_abi_version);

odin3_status odin3_plugin_init(uint32_t host_abi_version) {
    return host_abi_version == (uint32_t)BUILT_FOR_ABI ? ODIN3_OK : ODIN3_ERR_ABI_MISMATCH;
}
