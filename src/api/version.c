/*
 * version.c — version, ABI and status queries from odin3.h.
 */
#include "odin3/odin3.h"
#include "util/attr.h"

#include <stddef.h>

#define ODIN3_STR_(x) #x
#define ODIN3_STR(x) ODIN3_STR_(x)

ODIN3_EXPORT const char *odin3_version_string(void) {
    return ODIN3_STR(ODIN3_VERSION_MAJOR) "." ODIN3_STR(ODIN3_VERSION_MINOR) "." ODIN3_STR(
        ODIN3_VERSION_PATCH);
}

#ifndef ODIN3_COMMIT_STR
#define ODIN3_COMMIT_STR "unknown"
#endif
#ifndef ODIN3_COMMIT_DIRTY_FLAG
#define ODIN3_COMMIT_DIRTY_FLAG 0
#endif

ODIN3_EXPORT const char *odin3_version_commit(void) {
    return ODIN3_COMMIT_STR;
}

ODIN3_EXPORT int odin3_version_commit_dirty(void) {
    return ODIN3_COMMIT_DIRTY_FLAG;
}

ODIN3_EXPORT uint32_t odin3_abi_version(void) {
    return (uint32_t)ODIN3_ABI_VERSION;
}

ODIN3_EXPORT const char *odin3_status_string(odin3_status status) {
    static const char *const names[ODIN3_STATUS_COUNT] = {
        [ODIN3_OK] = "ODIN3_OK",
        [ODIN3_ERR_INVALID_ARG] = "ODIN3_ERR_INVALID_ARG",
        [ODIN3_ERR_NO_MEMORY] = "ODIN3_ERR_NO_MEMORY",
        [ODIN3_ERR_IO] = "ODIN3_ERR_IO",
        [ODIN3_ERR_PLUGIN] = "ODIN3_ERR_PLUGIN",
        [ODIN3_ERR_ABI_MISMATCH] = "ODIN3_ERR_ABI_MISMATCH",
        [ODIN3_ERR_CHECK] = "ODIN3_ERR_CHECK",
        [ODIN3_ERR_PARSE] = "ODIN3_ERR_PARSE",
        [ODIN3_ERR_ENGINE] = "ODIN3_ERR_ENGINE",
    };
    if ((unsigned)status >= (unsigned)ODIN3_STATUS_COUNT) {
        return "ODIN3_STATUS_UNKNOWN";
    }
    return names[status];
}
