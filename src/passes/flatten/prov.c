/*
 * prov.c — provenance of flattened objects (3A spec §6; plan Task 3): instance paths, DERIVED
 * records through odin3_prov_derive_hier, and errors located at the instance's source.
 */
#include "passes/flatten/flatten_int.h"

#include "ast/srcman.h"
#include "ir/module.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

odin3_status odin3_flatten_path(odin3_flatten_ctx *fc, const flat_work *item) {
    const odin3_strtab *tab = odin3_design_strtab(fc->design);
    odin3_strbuf_clear(&fc->scratch);
    odin3_status st =
        odin3_strbuf_appendf(&fc->scratch, "%s/", odin3_strtab_get(tab, item->parent_path));
    if (st == ODIN3_OK && item->orig_name != 0) {
        st = odin3_strbuf_appendf(&fc->scratch, "%s", odin3_strtab_get(tab, item->orig_name));
    } else if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(&fc->scratch, "$c%" PRIu32, item->node);
    }
    return st == ODIN3_OK ? odin3_flatten_intern_scratch(fc, &fc->path_str) : st;
}

/* The part of an original hier to append to the instance path: everything after the child's module
 * name (which may itself hold a '/', e.g. a string parameter `sub$F="dir/x.hex"`), else after its
 * first component. Empty when nothing follows. */
static const char *hier_scope(const odin3_flatten_ctx *fc, const char *orig) {
    const char *module =
        odin3_strtab_get(odin3_design_strtab(fc->design), odin3_module_name(fc->child));
    size_t len = strlen(module);
    const char *slash = NULL;
    if (len > 0 && strncmp(orig, module, len) == 0 && (orig[len] == '/' || orig[len] == '\0')) {
        slash = orig + len;
    } else {
        slash = strchr(orig, '/');
    }
    return slash != NULL && slash[0] == '/' && slash[1] != '\0' ? slash : "";
}

/* The derived hier string for an original hier: path + the original minus the child's module name
 * (nothing appended when that is empty). */
static odin3_status hier_for(odin3_flatten_ctx *fc, uint32_t orig_hier, uint32_t *out) {
    if (orig_hier == fc->cache_from && fc->cache_to != 0) {
        *out = fc->cache_to;
        return ODIN3_OK;
    }
    const odin3_strtab *tab = odin3_design_strtab(fc->design);
    odin3_strbuf_clear(&fc->scratch);
    odin3_status st = odin3_strbuf_appendf(&fc->scratch, "%s", odin3_strtab_get(tab, fc->path_str));
    const char *orig = orig_hier != 0 ? odin3_strtab_get(tab, orig_hier) : "";
    st = st == ODIN3_OK ? odin3_strbuf_appendf(&fc->scratch, "%s", hier_scope(fc, orig)) : st;
    st = st == ODIN3_OK ? odin3_flatten_intern_scratch(fc, out) : st;
    if (st == ODIN3_OK) {
        fc->cache_from = orig_hier;
        fc->cache_to = *out;
    }
    return st;
}

odin3_status odin3_flatten_derive(odin3_flatten_ctx *fc, odin3_prov_id orig, odin3_prov_id *out) {
    odin3_prov_id parents[2];
    uint32_t count = 0;
    *out = (odin3_prov_id){0};
    if (orig.v != 0) {
        parents[count++] = orig;
    }
    if (fc->inst_prov.v != 0) {
        parents[count++] = fc->inst_prov;
    }
    if (count == 0) {
        return ODIN3_OK;
    }
    const odin3_prov_record *rec = orig.v != 0 ? odin3_prov_get(fc->design, orig) : NULL;
    uint32_t hier = 0;
    odin3_status st = hier_for(fc, rec != NULL ? rec->hier : 0, &hier);
    return st == ODIN3_OK
               ? odin3_prov_derive_hier(fc->pass, (odin3_prov_list){parents, count}, hier, out)
               : st;
}

static void first_leaf(void *user, odin3_prov_id leaf) {
    odin3_prov_id *found = user;
    if (found->v == 0) {
        *found = leaf;
    }
}

odin3_status odin3_flatten_error(const odin3_flatten_ctx *fc, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_prov_id leaf = {0};
    if (fc->inst_prov.v != 0) {
        (void)odin3_prov_sources(fc->design, fc->inst_prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(fc->design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].loc != 0) {
        odin3_diag(fc->design, ODIN3_LOG_ERROR, (odin3_loc){rec->locs[0].loc}, "%s", msg);
    } else if (rec != NULL && rec->n_locs > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%" PRIu32 ":%" PRIu32 ": error: %s",
                  odin3_strtab_get(odin3_design_strtab(fc->design), rec->locs[0].file),
                  rec->locs[0].line, rec->locs[0].col, msg);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "flatten: %s", msg);
    }
    return ODIN3_ERR_INVALID_ARG;
}
