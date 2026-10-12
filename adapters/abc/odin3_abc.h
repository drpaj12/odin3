/*
 * odin3_abc.h — the two exports of libodin3_abc.so, the linked-ABC adapter (3D spec §4.3, AB-7).
 * exports.map makes them the library's only dynamic symbols; tools/check-symbols.sh --only checks
 * it (CTest abc_adapter_exports).
 */
#ifndef ODIN3_ADAPTERS_ABC_ODIN3_ABC_H
#define ODIN3_ADAPTERS_ABC_ODIN3_ABC_H

#include "odin3/odin3.h"

#include <stdint.h>

/* The adapter compiles with hidden visibility; these two are exported. */
#define ODIN3_ABC_API __attribute__((visibility("default")))

/*
 * Registers the linked engine (name "linked", abc_commit the pinned ABC commit) as the process's
 * ABC engine through odin3_abc_engine_register. Nothing is allocated; the table's strings and
 * session are static. Returns odin3_abc_engine_register's status: ODIN3_ERR_INVALID_ARG (logged)
 * when an engine is already registered. The odin3 CLI calls it at start-up (3D Task 6).
 */
ODIN3_ABC_API odin3_status odin3_abc_adapter_init(void);

/*
 * The plugin entry point (odin3_plugin_init_fn), for odin3 --plugin and Python's
 * odin3_plugin_load: ODIN3_ERR_ABI_MISMATCH when host_abi_version is not ODIN3_ABI_VERSION, else
 * odin3_abc_adapter_init's status.
 */
ODIN3_ABC_API odin3_status odin3_plugin_init(uint32_t host_abi_version);

#endif
