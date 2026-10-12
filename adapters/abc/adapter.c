/*
 * adapter.c — the linked engine's table and the adapter's two exports (odin3_abc.h; 3D spec
 * §4.3, AB-7).
 */
#include "odin3_abc.h"

#include "guard.h"
#include "session.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ODIN3_ABC_COMMIT_STR
#error "ODIN3_ABC_COMMIT_STR (the pinned ABC commit) comes from CMakeLists.txt"
#endif

/*
 * The three exchange functions arrive with 3D Task 6 (session.c's load_aig, get_luts and
 * name_map). Until then each fails cleanly with RESULT, so the table is complete for the
 * registry and nothing reaches ABC through them.
 */
static odin3_status adp_not_yet(odin3_abc_fail *fail) {
    static const char text[] = "not implemented yet";
    _Static_assert(sizeof text <= ODIN3_ABC_FAIL_TEXT_BYTES, "the text fits odin3_abc_fail");
    odin3_abc_fail_reset(fail, ODIN3_ABC_FAIL_RESULT);
    if (fail != NULL) {
        memcpy(fail->text, text, sizeof text);
    }
    return ODIN3_ERR_ENGINE;
}

static odin3_status adp_load_aig(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail) {
    (void)self;
    (void)aig;
    return adp_not_yet(fail);
}

static odin3_status adp_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail) {
    (void)self;
    (void)luts;
    return adp_not_yet(fail);
}

static odin3_status adp_name_map(void *self, const odin3_abc_luts *aligned, const int32_t **lits,
                                 odin3_abc_fail *fail) {
    (void)self;
    (void)aligned;
    (void)lits;
    return adp_not_yet(fail);
}

odin3_status odin3_abc_adapter_init(void) {
    const odin3_abc_engine engine = {
        .version = ODIN3_ABC_ENGINE_VERSION,
        .name = "linked",
        .abc_commit = ODIN3_ABC_COMMIT_STR,
        .self = odin3_abc_session_self(),
        .open = odin3_abc_session_open,
        .load_aig = adp_load_aig,
        .exec = odin3_abc_session_exec,
        .get_luts = adp_get_luts,
        .name_map = adp_name_map,
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
