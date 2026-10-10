/*
 * pass.c — the public ABI's pass group: the pass registry listing, plugin passes, process options
 * and pass scripts (forwarded to the pass manager, src/passes/manager.h).
 */
#include "api/api.h"

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/log.h"

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

/* --- plugin passes ------------------------------------------------------------------------- */

/*
 * A registered plugin pass: the pass manager's definition (body plugin_run, user pointer this
 * plugin_pass), the plugin's body and user pointer, and the name and help strings it points to.
 */
typedef struct plugin_pass {
    odin3_pass_def def;
    odin3_pass_run_fn run;
    void *user;
    char text[]; /* the name, then the help, each NUL-terminated */
} plugin_pass;

/* The manager's body of every plugin pass: user is its plugin_pass (odin3_pass_def.user). Calls
 * the plugin with NUL-terminated arguments. */
static odin3_status plugin_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                               void *user) {
    (void)ctx;
    const plugin_pass *pass = user;
    char *text = odin3_util_malloc(args.len + 1);
    if (text == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (args.len > 0) {
        memcpy(text, args.ptr, args.len);
    }
    text[args.len] = '\0';
    odin3_status st = pass->run(design, text, pass->user);
    odin3_util_free(text);
    return st;
}

static bool reserved_clear(const odin3_plugin_pass *pass) {
    for (size_t i = 0; i < sizeof pass->reserved / sizeof pass->reserved[0]; i++) {
        if (pass->reserved[i] != NULL) {
            return false;
        }
    }
    return true;
}

/* A heap copy of pass for the manager (odin3_util_free); NULL on out of memory. */
static plugin_pass *copy_pass(const odin3_plugin_pass *pass) {
    size_t name_len = strlen(pass->name) + 1;
    size_t help_len = strlen(pass->help) + 1;
    plugin_pass *copy = odin3_util_malloc(sizeof *copy + name_len + help_len);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy->text, pass->name, name_len);
    memcpy(copy->text + name_len, pass->help, help_len);
    copy->def = (odin3_pass_def){copy->text, copy->text + name_len, plugin_run, copy};
    copy->run = pass->run;
    copy->user = pass->user;
    return copy;
}

ODIN3_EXPORT odin3_status odin3_pass_register(const odin3_plugin_pass *pass) {
    if (pass == NULL || pass->name == NULL || pass->help == NULL || pass->run == NULL) {
        return odin3_api_invalid(__func__);
    }
    if (!reserved_clear(pass)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: pass '%s': reserved slot is set (built for a later ABI?)",
                  __func__, pass->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    plugin_pass *copy = copy_pass(pass);
    if (copy == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    /* The registry keeps the copy for the life of the process (a pass is never unregistered). */
    odin3_status st = odin3_pass_register_def(&copy->def);
    if (st != ODIN3_OK) {
        odin3_util_free(copy);
    }
    return st;
}
