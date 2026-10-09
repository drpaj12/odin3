/*
 * pass.c — the public ABI's pass group: the pass registry listing, process options and pass
 * scripts (forwarded to the pass manager, src/passes/manager.h).
 */
#include "api/api.h"

#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The heap copy of the --top name the pass options point at (odin3_pass_set_top_name). */
static char *g_top_copy = NULL;

ODIN3_EXPORT uint32_t odin3_pass_get_count(void) {
    return odin3_pass_count();
}

ODIN3_EXPORT odin3_status odin3_pass_get_name(uint32_t index, const char **name) {
    const odin3_pass_def *def = odin3_pass_at(index);
    if (def == NULL || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = def->name;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_pass_get_help(uint32_t index, const char **help) {
    const odin3_pass_def *def = odin3_pass_at(index);
    if (def == NULL || help == NULL) {
        return odin3_api_invalid(__func__);
    }
    *help = def->help;
    return ODIN3_OK;
}

ODIN3_EXPORT void odin3_pass_set_check(bool check) {
    odin3_pass_options opts = odin3_pass_get_options();
    opts.check = check;
    odin3_pass_set_options(opts);
}

ODIN3_EXPORT odin3_status odin3_pass_set_top_name(const char *name) {
    char *copy = NULL;
    if (name != NULL) {
        size_t len = strlen(name);
        copy = odin3_util_malloc(len + 1);
        if (copy == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memcpy(copy, name, len + 1);
    }
    odin3_pass_options opts = odin3_pass_get_options();
    opts.top = copy;
    odin3_pass_set_options(opts);
    odin3_util_free(g_top_copy);
    g_top_copy = copy;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_run_script(odin3_design *design, const char *text,
                                                  odin3_script_src src) {
    if (design == NULL || text == NULL || src.origin == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_pass_run_script(design, odin3_bytes_cstr(text), src);
}

ODIN3_EXPORT odin3_status odin3_design_run_script_file(odin3_design *design, const char *path) {
    if (design == NULL || path == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_pass_run_script_file(design, path);
}

ODIN3_EXPORT odin3_status odin3_script_resolve(const char *text, odin3_script_src src) {
    if (text == NULL || src.origin == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_pass_resolve_script(odin3_bytes_cstr(text), src);
}

ODIN3_EXPORT odin3_status odin3_script_resolve_file(const char *path) {
    if (path == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_pass_resolve_script_file(path);
}
