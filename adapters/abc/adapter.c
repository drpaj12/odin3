/*
 * adapter.c — the linked engine's table and the adapter's two exports (odin3_abc.h; 3D spec
 * §4.3, AB-7).
 */
#include "odin3_abc.h"

#include "session.h"

#include <stddef.h>
#include <stdint.h>

#ifndef ODIN3_ABC_COMMIT_STR
#error "ODIN3_ABC_COMMIT_STR (the pinned ABC commit) comes from CMakeLists.txt"
#endif

/*
 * The table's abc_commit. A test build of the CLI (CTest cli_abc_unavailable_warning) defines
 * ODIN3_ABC_TEST_UNREGISTRABLE, so the registry refuses the table and odin3 starts without
 * linked ABC.
 */
#ifdef ODIN3_ABC_TEST_UNREGISTRABLE
#define ADP_ABC_COMMIT "unregistrable"
#else
#define ADP_ABC_COMMIT ODIN3_ABC_COMMIT_STR
#endif

odin3_status odin3_abc_adapter_init(void) {
    const odin3_abc_engine engine = {
        .version = ODIN3_ABC_ENGINE_VERSION,
        .name = "linked",
        .abc_commit = ADP_ABC_COMMIT,
        .self = odin3_abc_session_self(),
        .open = odin3_abc_session_open,
        .load_aig = odin3_abc_session_load_aig,
        .exec = odin3_abc_session_exec,
        .get_luts = odin3_abc_session_get_luts,
        .name_map = odin3_abc_session_name_map,
        .load_genlib = odin3_abc_session_load_genlib,
        .get_cells = odin3_abc_session_get_cells,
        .close = odin3_abc_session_close,
        .reserved = {NULL, NULL, NULL, NULL},
    };
    return odin3_abc_engine_register(&engine);
}

odin3_status odin3_plugin_init(uint32_t host_abi_version) {
    if (host_abi_version != ODIN3_ABI_VERSION) {
        return ODIN3_ERR_ABI_MISMATCH;
    }
    return odin3_abc_adapter_init();
}
