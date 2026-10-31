/*
 * gl_group.c — where the node groups of a graphlets run come from (spec section 12): the whole
 * module, `--focus` selections (shared with write_dot), a `--cells` partition file and
 * `--partition KEY` (nodes grouped by an attribute's text).
 */
#include "ir/celltype.h"
#include "ir/value.h"
#include "passes/graphlets/gl_int.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

enum {
    FILE_MAX = 64 * 1024 * 1024,
    LINE_MAX_LEN = 4096,
    DECIMAL = 10,
    GROW = 8,
    MAX_PARTITION_GROUPS = 4096 /* --partition groups of one run */
};

static void group_free(gl_group *grp) {
    odin3_strbuf_free(&grp->name);
    odin3_util_free(grp->mask);
}

void gl_groups_free(gl_groups *groups) {
    for (size_t i = 0; i < groups->count; i++) {
        group_free(&groups->items[i]);
    }
    odin3_util_free(groups->items);
    memset(groups, 0, sizeof *groups);
}

/* Adds a group taking ownership of mask; name is copied. */
static odin3_status groups_add(gl_groups *gs, const char *name, uint32_t module, const char *source,
                               uint8_t *mask) {
    if (gs->count == gs->cap) {
        size_t cap = gs->cap * 2 + GROW;
        gl_group *items = odin3_util_realloc(gs->items, cap * sizeof *items);
        if (items == NULL) {
            odin3_util_free(mask);
            return ODIN3_ERR_NO_MEMORY;
        }
        gs->items = items;
        gs->cap = cap;
    }
    gl_group *grp = &gs->items[gs->count];
    memset(grp, 0, sizeof *grp);
    odin3_strbuf_init(&grp->name);
    grp->module = module;
    grp->source = source;
    grp->mask = mask;
    gs->count++;
    return odin3_strbuf_append(&grp->name, odin3_bytes_cstr(name));
}

static const char *module_text(const odin3_module *mod) {
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(mod)), odin3_module_name(mod));
}

static uint8_t *mask_new(const odin3_module *mod) {
    return odin3_util_calloc((size_t)odin3_module_node_end(mod) + 1);
}

static bool mask_any(const uint8_t *mask, uint32_t end) {
    for (uint32_t i = 1; i < end; i++) {
        if (mask[i] != 0) {
            return true;
        }
    }
    return false;
}

static odin3_status add_whole_modules(odin3_design *design, gl_groups *gs) {
    for (uint32_t id = 1; id < odin3_design_module_end(design); id++) {
        const odin3_module *mod = odin3_module_get(design, (odin3_module_id){id});
        uint8_t *mask = mask_new(mod);
        if (mask == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        memset(mask, 1, (size_t)odin3_module_node_end(mod) + 1);
        odin3_status st = groups_add(gs, module_text(mod), id, "module", mask);
        if (st != ODIN3_OK) {
            return st;
        }
        gs->items[gs->count - 1].whole = true;
    }
    return ODIN3_OK;
}

static odin3_status add_focus(odin3_design *design, const gl_opts *opts, unsigned index,
                              gl_groups *gs) {
    odin3_dot_selection sel;
    const char *text = opts->focus[index].data;
    odin3_status st = odin3_dot_select(design, opts->focus_kind[index], text, &sel);
    for (uint32_t id = 1; st == ODIN3_OK && id < sel.mod_end; id++) {
        const odin3_module *mod = odin3_module_get(design, (odin3_module_id){id});
        if (sel.mask[id] == NULL || !mask_any(sel.mask[id], odin3_module_node_end(mod))) {
            continue;
        }
        st = groups_add(gs, text, id, "focus", sel.mask[id]);
        sel.mask[id] = NULL;
        if (st == ODIN3_OK && odin3_design_module_end(design) > 2) {
            st = odin3_strbuf_appendf(&gs->items[gs->count - 1].name, "@%s", module_text(mod));
        }
    }
    odin3_dot_selection_free(&sel);
    return st;
}

/* --- --cells -------------------------------------------------------------------------------- */

static odin3_module *find_module(odin3_design *design, const char *name) {
    uint32_t str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str)) {
        return NULL;
    }
    for (uint32_t id = 1; id < odin3_design_module_end(design); id++) {
        odin3_module *mod = odin3_module_get(design, (odin3_module_id){id});
        if (odin3_module_name(mod) == str) {
            return mod;
        }
    }
    return NULL;
}

static odin3_module *default_module(odin3_design *design) {
    odin3_module_id top = odin3_design_top(design);
    if (top.v != 0) {
        return odin3_module_get(design, top);
    }
    return odin3_design_module_end(design) == 2 ? odin3_module_get(design, (odin3_module_id){1})
                                                : NULL;
}

static odin3_status cell_mark(const odin3_design *design, const odin3_module *mod, const char *tok,
                              uint8_t *mask) {
    char *end = NULL;
    unsigned long id = strtoul(tok, &end, DECIMAL);
    odin3_node_id node = {0};
    uint32_t str = 0;
    if (*tok != '\0' && *end == '\0') {
        node.v = id <= UINT32_MAX ? (uint32_t)id : 0; /* 0: no such cell, reported below */
    } else if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(tok), &str)) {
        node = odin3_module_find_node(mod, str);
    }
    if (node.v == 0 || node.v >= odin3_module_node_end(mod) || !odin3_node_live(mod, node)) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: --cells: no cell '%s' in module '%s'", tok,
                  module_text(mod));
        return ODIN3_ERR_INVALID_ARG;
    }
    mask[node.v] = 1;
    return ODIN3_OK;
}

typedef struct cells_state {
    odin3_design *design;
    gl_groups *gs;
    odin3_module *mod;
    uint8_t *mask;
    odin3_strbuf name;
} cells_state;

static odin3_status cells_flush(cells_state *cs) {
    odin3_status st = ODIN3_OK;
    if (cs->mask != NULL) {
        st = groups_add(cs->gs, cs->name.data != NULL ? cs->name.data : "default",
                        odin3_module_id_of(cs->mod).v, "cells", cs->mask);
        cs->mask = NULL;
    }
    odin3_strbuf_clear(&cs->name);
    return st;
}

static odin3_status cells_header(cells_state *cs, const char *tok) {
    const char *name = tok + 1;
    const char *module = strtok(NULL, " \t\r\n");
    odin3_status st = cells_flush(cs);
    cs->mod = module != NULL ? find_module(cs->design, module) : default_module(cs->design);
    if (st == ODIN3_OK && (name[0] == '\0' || cs->mod == NULL)) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: --cells: bad group header (use '@NAME [MODULE]')");
        return ODIN3_ERR_INVALID_ARG;
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(&cs->name, odin3_bytes_cstr(name)) : st;
    cs->mask = st == ODIN3_OK ? mask_new(cs->mod) : NULL;
    return st == ODIN3_OK && cs->mask == NULL ? ODIN3_ERR_NO_MEMORY : st;
}

static odin3_status cells_line(cells_state *cs, char *line) {
    char *hash = strchr(line, '#');
    if (hash != NULL) {
        *hash = '\0';
    }
    const char *tok = strtok(line, " \t\r\n");
    if (tok == NULL) {
        return ODIN3_OK;
    }
    if (tok[0] == '@') {
        return cells_header(cs, tok);
    }
    if (cs->mask == NULL) {
        cs->mod = default_module(cs->design);
        cs->mask = cs->mod != NULL ? mask_new(cs->mod) : NULL;
        if (cs->mask == NULL) {
            odin3_log(ODIN3_LOG_ERROR, "graphlets: --cells: no default module (name one)");
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return cell_mark(cs->design, cs->mod, tok, cs->mask);
}

static odin3_status add_cells(odin3_design *design, const char *path, gl_groups *gs) {
    odin3_file_bytes fb;
    odin3_status st = odin3_file_read_all(path, FILE_MAX, &fb);
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: cannot read --cells file '%s'", path);
        return ODIN3_ERR_IO;
    }
    cells_state cs = {design, gs, NULL, NULL, {0}};
    odin3_strbuf_init(&cs.name);
    char line[LINE_MAX_LEN];
    size_t pos = 0;
    while (st == ODIN3_OK && pos < fb.len) {
        size_t len = 0;
        while (pos + len < fb.len && fb.data[pos + len] != '\n') {
            len++;
        }
        size_t take = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, fb.data + pos, take);
        line[take] = '\0';
        pos += len + 1;
        st = cells_line(&cs, line);
    }
    st = st == ODIN3_OK ? cells_flush(&cs) : st;
    odin3_util_free(cs.mask);
    odin3_strbuf_free(&cs.name);
    odin3_file_bytes_free(&fb);
    return st;
}

/* --- --partition KEY ------------------------------------------------------------------------ */

static odin3_status value_text(const odin3_design *design, const odin3_value *val,
                               odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    if (val->kind == ODIN3_VAL_INT) {
        return odin3_strbuf_appendf(out, "%lld", (long long)val->i);
    }
    if (val->kind == ODIN3_VAL_STRING) {
        return odin3_strbuf_append(
            out, odin3_bytes_cstr(odin3_strtab_get(odin3_design_strtab(design), val->str)));
    }
    return ODIN3_OK;
}

typedef struct part_state {
    odin3_strbuf *names;
    uint8_t **masks;
    size_t count;
    size_t cap;
} part_state;

static odin3_status part_grow(part_state *ps) {
    size_t cap = ps->cap * 2 + GROW;
    odin3_strbuf *names = odin3_util_realloc(ps->names, cap * sizeof *names);
    if (names == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ps->names = names; /* kept before the masks grow: a failure below must not lose them */
    uint8_t **masks = (uint8_t **)odin3_util_realloc((void *)ps->masks, cap * sizeof *masks);
    if (masks == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ps->masks = masks;
    ps->cap = cap;
    return ODIN3_OK;
}

static odin3_status part_slot(part_state *ps, const char *text, const odin3_module *mod,
                              size_t *slot) {
    for (size_t i = 0; i < ps->count; i++) {
        if (strcmp(ps->names[i].data, text) == 0) {
            *slot = i;
            return ODIN3_OK;
        }
    }
    if (ps->count >= MAX_PARTITION_GROUPS) {
        odin3_log(ODIN3_LOG_ERROR,
                  "graphlets: --partition: more than %d groups in module '%s'; choose a coarser "
                  "attribute",
                  MAX_PARTITION_GROUPS, module_text(mod));
        return ODIN3_ERR_INVALID_ARG;
    }
    if (ps->count == ps->cap) {
        odin3_status gst = part_grow(ps);
        if (gst != ODIN3_OK) {
            return gst;
        }
    }
    uint8_t *mask = mask_new(mod);
    if (mask == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_strbuf_init(&ps->names[ps->count]);
    odin3_status st = odin3_strbuf_append(&ps->names[ps->count], odin3_bytes_cstr(text));
    if (st != ODIN3_OK) {
        odin3_strbuf_free(&ps->names[ps->count]);
        odin3_util_free(mask);
        return st;
    }
    ps->masks[ps->count] = mask;
    *slot = ps->count++;
    return ODIN3_OK;
}

static odin3_status partition_assign(const odin3_design *design, const odin3_module *mod,
                                     uint32_t key, part_state *ps) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(mod); id++) {
        const odin3_value *val = odin3_node_live(mod, (odin3_node_id){id})
                                     ? odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, id}, key)
                                     : NULL;
        st = val != NULL ? value_text(design, val, &text) : st;
        size_t slot = 0;
        if (st == ODIN3_OK && val != NULL && text.len > 0) {
            st = part_slot(ps, text.data, mod, &slot);
            ps->masks[slot][id] = st == ODIN3_OK ? 1 : 0;
        }
    }
    odin3_strbuf_free(&text);
    return st;
}

static odin3_status partition_module(const odin3_design *design, const odin3_module *mod,
                                     uint32_t key, gl_groups *gs) {
    part_state ps = {NULL, NULL, 0, 0};
    odin3_status st = partition_assign(design, mod, key, &ps);
    if (st == ODIN3_OK && gs->count + ps.count > MAX_PARTITION_GROUPS) {
        odin3_log(ODIN3_LOG_ERROR,
                  "graphlets: --partition: more than %d groups; choose a coarser "
                  "attribute",
                  MAX_PARTITION_GROUPS);
        st = ODIN3_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < ps.count; i++) {
        if (st == ODIN3_OK) {
            st = groups_add(gs, ps.names[i].data, odin3_module_id_of(mod).v, "partition",
                            ps.masks[i]);
        } else {
            odin3_util_free(ps.masks[i]);
        }
        odin3_strbuf_free(&ps.names[i]);
    }
    odin3_util_free(ps.names);
    odin3_util_free((void *)ps.masks);
    return st;
}

static odin3_status add_partition(odin3_design *design, const char *keytext, gl_groups *gs) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(keytext), &key)) {
        return ODIN3_OK; /* no object has that string: no group */
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        st = partition_module(design, odin3_module_get(design, (odin3_module_id){id}), key, gs);
    }
    return st;
}

odin3_status gl_groups_make(odin3_design *design, const gl_opts *opts, gl_groups *out) {
    memset(out, 0, sizeof *out);
    bool any = opts->n_focus > 0 || opts->cells.len > 0 || opts->partition.len > 0;
    odin3_status st = any ? ODIN3_OK : add_whole_modules(design, out);
    for (unsigned i = 0; st == ODIN3_OK && i < opts->n_focus; i++) {
        st = add_focus(design, opts, i, out);
    }
    st = st == ODIN3_OK && opts->cells.len > 0 ? add_cells(design, opts->cells.data, out) : st;
    st = st == ODIN3_OK && opts->partition.len > 0
             ? add_partition(design, opts->partition.data, out)
             : st;
    if (st != ODIN3_OK) {
        gl_groups_free(out);
    }
    return st;
}
