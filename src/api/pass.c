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
#include "util/vec.h"

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
 * A registered plugin pass: the pass manager's definition (its body is plugin_run for every
 * plugin pass), the plugin's body and user pointer, and the name and help strings it points to.
 */
typedef struct plugin_pass {
    odin3_pass_def def;
    odin3_pass_run_fn run;
    void *user;
    char text[]; /* the name, then the help, each NUL-terminated */
} plugin_pass;

/* Every registered plugin pass (plugin_slot), in registration order; kept for the process. */
typedef struct plugin_slot {
    plugin_pass *pass;
} plugin_slot;
static odin3_vec g_plugins;
static bool g_plugins_ready = false;

/* The registered plugin pass named name, NULL when there is none. */
static const plugin_pass *plugin_named(const char *name) {
    for (size_t i = 0; g_plugins_ready && i < g_plugins.len; i++) {
        const plugin_slot *slot = odin3_vec_cat(&g_plugins, i);
        if (strcmp(slot->pass->def.name, name) == 0) {
            return slot->pass;
        }
    }
    return NULL;
}

/*
 * The manager's body of every plugin pass: finds the plugin pass by the name of the run the
 * manager opened for it (the design's latest run: ctx's, as nothing runs between opening it and
 * this call) and calls it with NUL-terminated arguments.
 */
static odin3_status plugin_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    (void)ctx;
    odin3_passrun_id run = {odin3_passrun_end(design) - 1};
    const plugin_pass *pass = plugin_named(odin3_api_str(design, odin3_passrun_name(design, run)));
    if (pass == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "plugin pass: no plugin pass is registered for this run");
        return ODIN3_ERR_INVALID_ARG;
    }
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
    copy->def = (odin3_pass_def){copy->text, copy->text + name_len, plugin_run};
    copy->run = pass->run;
    copy->user = pass->user;
    return copy;
}

/* Lists copy in g_plugins, then registers it with the manager; on failure neither keeps it. */
static odin3_status add_plugin(plugin_pass *copy) {
    if (!g_plugins_ready) {
        odin3_vec_init(&g_plugins, sizeof(plugin_slot));
        g_plugins_ready = true;
    }
    plugin_slot *slot = odin3_vec_push(&g_plugins);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    slot->pass = copy;
    odin3_status st = odin3_pass_register_def(&copy->def);
    if (st != ODIN3_OK) {
        odin3_vec_pop(&g_plugins);
    }
    return st;
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
    odin3_status st = add_plugin(copy);
    if (st != ODIN3_OK) {
        odin3_util_free(copy);
    }
    return st;
}
