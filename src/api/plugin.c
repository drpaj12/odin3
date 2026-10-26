/*
 * plugin.c — loading shared-object plugins (odin3_plugin_load in odin3.h).
 */
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/log.h"

#include <dlfcn.h>
#include <stddef.h>
#include <string.h>

ODIN3_EXPORT odin3_status odin3_plugin_load(const char *path) {
    if (path == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        return ODIN3_ERR_IO;
    }
    const void *sym = dlsym(handle, "odin3_plugin_init");
    if (sym == NULL) {
        dlclose(handle);
        return ODIN3_ERR_PLUGIN;
    }
    /* ISO C has no object-to-function pointer cast; POSIX guarantees the
     * representations match, so copy the bits. */
    odin3_plugin_init_fn init = NULL;
    memcpy((void *)&init, (const void *)&sym, sizeof init);
    /* Never unloaded once init has run, even when it fails: whatever it registered before failing
     * (passes, cell types and their hooks, user pointers) points into the shared object. */
    odin3_status st = init((uint32_t)ODIN3_ABI_VERSION);
    if (st == ODIN3_ERR_ABI_MISMATCH) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_plugin_load: %s was built for another ABI than the host's (%u); rebuild "
                  "it for ABI %u",
                  path, (unsigned)ODIN3_ABI_VERSION, (unsigned)ODIN3_ABI_VERSION);
    }
    return st;
}
