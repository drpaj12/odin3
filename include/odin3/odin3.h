/*
 * odin3.h — the public C ABI of Odin III.
 *
 * This is the only header that plugins (.so), the CLI, and the Python binding
 * (plugins/python/odin3.py, via cffi) may include. Everything else in src/ is
 * private. Spec: docs/DESIGN.md §15.3.
 *
 * ABI v0 (Phase 0 skeleton): version queries, status codes, and plugin loading.
 * ABI v1 (Phase 1, 1B): adds ODIN3_ERR_CHECK.
 * ABI v2 (Phase 1, 1C): adds ODIN3_ERR_PARSE. IR handles and accessors arrive in 1D.
 *
 * The block between ODIN3_CDEF_BEGIN and ODIN3_CDEF_END is read verbatim by the
 * Python binding as a cffi cdef: keep it free of preprocessor directives and
 * attributes.
 */
#ifndef ODIN3_ODIN3_H
#define ODIN3_ODIN3_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN3_VERSION_MAJOR 0
#define ODIN3_VERSION_MINOR 0
#define ODIN3_VERSION_PATCH 0

/* ODIN3_CDEF_BEGIN */

/* Incremented on every incompatible change to this header. */
enum { ODIN3_ABI_VERSION = 2 };

/* Result of every fallible Odin III function. ODIN3_OK is always 0. */
typedef enum odin3_status {
    ODIN3_OK = 0,
    ODIN3_ERR_INVALID_ARG = 1,
    ODIN3_ERR_NO_MEMORY = 2,
    ODIN3_ERR_IO = 3,
    ODIN3_ERR_PLUGIN = 4,
    ODIN3_ERR_ABI_MISMATCH = 5,
    ODIN3_ERR_CHECK = 6, /* the IR violates an invariant (docs/IR.md section 9) */
    ODIN3_ERR_PARSE = 7, /* malformed input file (logged as file:line: message) */
    ODIN3_STATUS_COUNT = 8
} odin3_status;

/*
 * Returns the library version as "MAJOR.MINOR.PATCH".
 * The string is static; the caller must not free it. Never fails.
 */
const char *odin3_version_string(void);

/*
 * Returns ODIN3_ABI_VERSION as compiled into the library, so a plugin or
 * binding built against one header can detect a library built from another.
 * Never fails.
 */
uint32_t odin3_abi_version(void);

/*
 * Returns a short human-readable name for a status, e.g. "ODIN3_ERR_IO".
 * The string is static; the caller must not free it. Values outside the enum
 * return "ODIN3_STATUS_UNKNOWN". Never fails.
 */
const char *odin3_status_string(odin3_status status);

/*
 * Entry point every shared-object plugin must export under the name
 * "odin3_plugin_init". The host passes its ODIN3_ABI_VERSION; a plugin built
 * for a different ABI returns ODIN3_ERR_ABI_MISMATCH. In Phase 1 the plugin
 * will register passes, cell types, readers and writers from here.
 */
typedef odin3_status (*odin3_plugin_init_fn)(uint32_t host_abi_version);

/*
 * Loads the shared object at `path` and calls its odin3_plugin_init.
 * Returns ODIN3_ERR_INVALID_ARG if `path` is NULL, ODIN3_ERR_IO if the file
 * cannot be loaded, ODIN3_ERR_PLUGIN if it does not export odin3_plugin_init,
 * or whatever status the plugin's init returns. The plugin stays loaded for
 * the life of the process; nothing is returned for the caller to free.
 */
odin3_status odin3_plugin_load(const char *path);

/* ODIN3_CDEF_END */

#ifdef __cplusplus
}
#endif

#endif /* ODIN3_ODIN3_H */
