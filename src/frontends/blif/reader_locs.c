/* reader_locs.c — the BLIF reader's `--hdl-locs` half; see reader_locs.h. */
#include "frontends/blif/reader_locs.h"

#include "backends/common/hdl_loc.h"
#include "frontends/hdl_restore.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stddef.h>
#include <string.h>

enum { PARAM_MAX = 64, KEY_SHIFT = 32 };

struct blif_locs {
    odin3_pass_ctx *ctx;
    odin3_blif_read_stats *stats;
    const char *path;
    odin3_u64map *by_text; /* module << 32 | original text -> net of a decoded name */
    odin3_u64map *decoded; /* module << 32 | net -> 1 while the net still has its SHORT name */
    odin3_strbuf buf;      /* decoded name text */
    odin3_strbuf name;     /* candidate net names */
    bool bound;            /* version 2 map: records come from the map, none are made here */
};

blif_locs *blif_locs_create(odin3_pass_ctx *ctx, odin3_blif_read_stats *stats, const char *path) {
    blif_locs *locs = odin3_util_calloc(sizeof *locs);
    if (locs == NULL) {
        return NULL;
    }
    *stats = (odin3_blif_read_stats){0};
    locs->ctx = ctx;
    locs->stats = stats;
    locs->path = path;
    locs->by_text = odin3_u64map_create(0);
    locs->decoded = odin3_u64map_create(0);
    odin3_strbuf_init(&locs->buf);
    odin3_strbuf_init(&locs->name);
    if (locs->by_text == NULL || locs->decoded == NULL) {
        blif_locs_destroy(locs);
        return NULL;
    }
    return locs;
}

void blif_locs_bind(blif_locs *locs) {
    locs->bound = true;
}

void blif_locs_destroy(blif_locs *locs) {
    if (locs == NULL) {
        return;
    }
    odin3_u64map_destroy(locs->by_text);
    odin3_u64map_destroy(locs->decoded);
    odin3_strbuf_free(&locs->buf);
    odin3_strbuf_free(&locs->name);
    odin3_util_free(locs);
}

static uint64_t key_of(uint32_t mod_key, uint32_t id) {
    return ((uint64_t)mod_key << KEY_SHIFT) | id;
}

/* The first name base, base$1, base$2, ... no net of module has; interned, in *out. */
static odin3_status free_name(blif_locs *locs, const odin3_module *module, odin3_bytes base,
                              uint32_t *out) {
    odin3_design *design = locs->ctx->design;
    for (uint32_t attempt = 0;; attempt++) {
        odin3_strbuf_clear(&locs->name);
        odin3_status st = odin3_strbuf_append(&locs->name, base);
        if (st == ODIN3_OK && attempt > 0) {
            st = odin3_strbuf_appendf(&locs->name, "$%u", attempt);
        }
        if (st == ODIN3_OK) {
            st = odin3_design_intern(design, (odin3_bytes){locs->name.data, locs->name.len}, out);
        }
        if (st != ODIN3_OK || !odin3_net_valid(odin3_module_find_net(module, *out))) {
            return st;
        }
    }
}

odin3_status blif_locs_find_net(blif_locs *locs, const blif_scope *scope, uint32_t str,
                                odin3_net_id *out) {
    odin3_module *module = scope->module;
    uint32_t mod_key = scope->key;
    uint64_t found = 0;
    *out = (odin3_net_id){0};
    if (odin3_u64map_get(locs->by_text, key_of(mod_key, str), &found)) {
        *out = (odin3_net_id){(uint32_t)found};
        return ODIN3_OK;
    }
    odin3_net_id net = odin3_module_find_net(module, str);
    if (!odin3_net_valid(net) || !odin3_u64map_get(locs->decoded, key_of(mod_key, net.v), &found)) {
        *out = net;
        return ODIN3_OK;
    }
    /* A decoded net holds this exact text as its SHORT name: it moves aside, the raw name wins. */
    const odin3_strtab *tab = odin3_design_strtab(locs->ctx->design);
    odin3_bytes base = {odin3_strtab_get(tab, str), odin3_strtab_len(tab, str)};
    uint32_t moved = 0;
    odin3_strbuf_clear(&locs->buf);
    odin3_status st = odin3_strbuf_append(&locs->buf, base);
    if (st == ODIN3_OK) {
        st = free_name(locs, module, (odin3_bytes){locs->buf.data, locs->buf.len}, &moved);
    }
    if (st == ODIN3_OK) {
        st = odin3_net_rename(module, net, moved);
    }
    return st; /* the net stays decoded: a raw name equal to its new name moves it again */
}

static odin3_status put(odin3_u64map *map, uint64_t key, uint64_t value) {
    return odin3_u64map_put(map, (odin3_kv){key, value});
}

/* Records for a decoded name: the net's prov, with the location text of the name. */
static odin3_status name_record(blif_locs *locs, const odin3_hdl_name *nm, odin3_prov_id imported,
                                odin3_prov_id *out) {
    uint32_t hier = 0;
    odin3_status st = ODIN3_OK;
    if (nm->hier.len > 0) {
        st = odin3_design_intern(locs->ctx->design, nm->hier, &hier);
    }
    odin3_hdl_restored restored = {0};
    if (st == ODIN3_OK) {
        odin3_hdl_restore_req req = {imported, &nm->loc, 1, hier};
        st = odin3_hdl_restore(locs->ctx, &req, &restored);
    }
    *out = restored.prov;
    return st;
}

/* Registers a decoded net: its original text finds it, and a raw name equal to its current name
 * moves it aside. */
static odin3_status note_decoded(blif_locs *locs, const blif_scope *scope, odin3_bytes name,
                                 odin3_net_id net) {
    uint32_t raw = 0;
    odin3_status st = odin3_design_intern(locs->ctx->design, name, &raw);
    if (st == ODIN3_OK) {
        st = put(locs->by_text, key_of(scope->key, raw), net.v);
    }
    if (st == ODIN3_OK) {
        st = put(locs->decoded, key_of(scope->key, net.v), 1);
        locs->stats->nets++;
    }
    return st;
}

/* A decoded name with a NUL byte is no name: the raw name stays, with a located warning. */
static void warn_nul(const blif_locs *locs, odin3_bytes name, odin3_prov_id imported) {
    const odin3_prov_record *rec = odin3_prov_get(locs->ctx->design, imported);
    odin3_log(ODIN3_LOG_WARN,
              "%s:%u: net name '%.*s' decodes to a name holding a NUL byte; kept "
              "as written",
              locs->path, rec != NULL && rec->n_locs > 0 ? rec->locs[0].line : 0U, (int)name.len,
              (const char *)name.ptr);
}

odin3_status blif_locs_create_net(blif_locs *locs, const blif_scope *scope, odin3_bytes name,
                                  odin3_prov_id imported, odin3_net_id *out) {
    odin3_hdl_name decoded;
    bool matched = false;
    odin3_status st = odin3_hdl_name_vtr_decode(name, &locs->buf, &decoded, &matched);
    uint32_t str = 0;
    odin3_prov_id prov = imported;
    if (st == ODIN3_OK && matched && memchr(locs->buf.data, 0, locs->buf.len) != NULL) {
        matched = false;
        warn_nul(locs, name, imported);
    }
    if (st == ODIN3_OK && matched) {
        st = locs->bound ? ODIN3_OK : name_record(locs, &decoded, imported, &prov);
        st = st == ODIN3_OK ? free_name(locs, scope->module, decoded.short_name, &str) : st;
    } else if (st == ODIN3_OK) {
        st = odin3_design_intern(locs->ctx->design, name, &str);
    }
    st = st == ODIN3_OK ? odin3_net_create(scope->module, str, prov, out) : st;
    return st == ODIN3_OK && matched ? note_decoded(locs, scope, name, *out) : st;
}

/* --- cells ---------------------------------------------------------------------------------- */

typedef struct attr_item {
    uint32_t key;
    const odin3_value *value;
} attr_item;

static odin3_status gather_attr(void *ctx, uint32_t key, const odin3_value *value) {
    attr_item *item = odin3_vec_push((odin3_vec *)ctx);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *item = (attr_item){key, value};
    return ODIN3_OK;
}

odin3_status blif_copy_node_attrs(odin3_module *module, odin3_node_id from, odin3_node_id to) {
    odin3_vec items;
    odin3_vec_init(&items, sizeof(attr_item));
    odin3_status st =
        odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NODE, from.v}, gather_attr, &items);
    for (size_t i = 0; st == ODIN3_OK && i < items.len; i++) {
        const attr_item *item = odin3_vec_cat(&items, i);
        st = odin3_attr_set(module, (odin3_objref){ODIN3_OBJ_NODE, to.v}, item->key, item->value);
    }
    odin3_vec_free(&items);
    return st;
}

/* Rebuilds *node with record prov: same type, parameters, pins' nets, name and attributes. */
static odin3_status reprov_node(blif_locs *locs, odin3_module *module, odin3_node_id *node,
                                odin3_prov_id prov) {
    const odin3_celltype_def *def =
        odin3_celltype_get(locs->ctx->design, odin3_node_type(module, *node));
    odin3_value params[PARAM_MAX];
    if (def->n_params > PARAM_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cell type '%s' has too many parameters to rebuild",
                  locs->path, def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = *odin3_node_param(module, *node, i);
    }
    odin3_node_spec spec = {.type = odin3_node_type(module, *node),
                            .prov = prov,
                            .params = params,
                            .n_params = def->n_params};
    odin3_node_id fresh = {0};
    uint32_t name = odin3_node_name(module, *node);
    odin3_status st = odin3_node_create(module, &spec, &fresh);
    st = st == ODIN3_OK ? blif_copy_node_attrs(module, *node, fresh) : st;
    st = st == ODIN3_OK ? odin3_node_replace(module, (odin3_node_pair){*node, fresh}) : st;
    if (st == ODIN3_OK && name != 0) {
        st = odin3_node_rename(module, fresh, name);
    }
    *node = st == ODIN3_OK ? fresh : *node;
    return st;
}

/* The record the object had before a restore: parents[1] of a DERIVED{SOURCE, base}. */
static odin3_prov_id base_of(const odin3_design *design, odin3_prov_id prov) {
    const odin3_prov_record *rec = odin3_prov_get(design, prov);
    if (rec != NULL && rec->kind == ODIN3_PROV_DERIVED && rec->parents.count == 2) {
        const odin3_prov_record *first = odin3_prov_get(design, rec->parents.ids[0]);
        if (first != NULL && first->kind == ODIN3_PROV_SOURCE) {
            return rec->parents.ids[1];
        }
    }
    return prov;
}

/* The text without one pair of surrounding double quotes. */
static odin3_bytes unquote(odin3_bytes text) {
    const char *chars = text.ptr;
    if (text.len >= 2 && chars[0] == '"' && chars[text.len - 1] == '"') {
        return (odin3_bytes){chars + 1, text.len - 2};
    }
    return text;
}

/* The warning for a `.attr src` that is not a location list (L-6): located, never an error. */
static void bad_src(blif_locs *locs, const blif_src_text *src) {
    locs->stats->bad++;
    odin3_log(ODIN3_LOG_WARN,
              "%s:%u: `.attr src` value '%.*s' is not a location list; kept as an attribute",
              locs->path, src->line, (int)src->text.len, (const char *)src->text.ptr);
}

odin3_status blif_locs_cell_src(blif_locs *locs, odin3_module *module, odin3_node_id *node,
                                const blif_src_text *src, bool *restored) {
    odin3_hdl_loc found[ODIN3_HDL_LOC_MAX];
    uint32_t count = odin3_hdl_loc_parse(unquote(src->text), found, ODIN3_HDL_LOC_MAX);
    *restored = false;
    if (locs->bound) {
        *restored = true; /* the map holds the records: the attribute is dropped, no record made */
        return ODIN3_OK;
    }
    if (count == 0) {
        bad_src(locs, src);
        return ODIN3_OK;
    }
    uint32_t kept = count < ODIN3_HDL_LOC_MAX ? count : ODIN3_HDL_LOC_MAX;
    odin3_prov_id own = odin3_node_prov(module, *node);
    odin3_prov_id base = base_of(locs->ctx->design, own);
    bool again = base.v != own.v; /* a second `.attr src` replaces the first */
    odin3_hdl_restore_req req = {base, found, kept, 0};
    odin3_hdl_restored made = {0};
    odin3_status st = odin3_hdl_restore(locs->ctx, &req, &made);
    st = st == ODIN3_OK ? reprov_node(locs, module, node, made.prov) : st;
    if (st == ODIN3_OK) {
        *restored = true;
        locs->stats->cells += again ? 0U : 1U;
        locs->stats->capped += count > ODIN3_HDL_LOC_MAX && !again ? 1U : 0U;
    }
    return st;
}

/* --- D-5 ------------------------------------------------------------------------------------ */

/* The SOURCE record id of a restored object's record, else none (id 0). */
static odin3_prov_id restored_source(const odin3_design *design, odin3_prov_id prov) {
    const odin3_prov_record *rec = odin3_prov_get(design, prov);
    if (rec != NULL && rec->kind == ODIN3_PROV_DERIVED && rec->parents.count > 0) {
        const odin3_prov_record *first = odin3_prov_get(design, rec->parents.ids[0]);
        if (first != NULL && first->kind == ODIN3_PROV_SOURCE) {
            return rec->parents.ids[0];
        }
    }
    return (odin3_prov_id){0};
}

/* The SOURCE of the first output pin's net (pin order) that has one, else none. */
static odin3_prov_id output_source(const odin3_design *design, const odin3_module *module,
                                   odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(module, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(module, pin);
        if (!odin3_pin_drives(module, pin) || !odin3_net_valid(net)) {
            continue;
        }
        odin3_prov_id source = restored_source(design, odin3_net_prov(module, net));
        if (source.v != 0) {
            return source;
        }
    }
    return (odin3_prov_id){0};
}

static odin3_status inherit_node(blif_locs *locs, odin3_module *module, odin3_node_id node) {
    const odin3_design *design = locs->ctx->design;
    if (!odin3_node_live(module, node) ||
        odin3_celltype_get(design, odin3_node_type(module, node))->gran == ODIN3_GRAN_PORT ||
        restored_source(design, odin3_node_prov(module, node)).v != 0) {
        return ODIN3_OK;
    }
    odin3_prov_id source = output_source(design, module, node);
    if (source.v == 0) {
        return ODIN3_OK;
    }
    const odin3_prov_id parents[2] = {source, odin3_node_prov(module, node)};
    odin3_prov_id prov = {0};
    odin3_status st = odin3_prov_derive_hier(locs->ctx, (odin3_prov_list){parents, 2},
                                             odin3_prov_get(design, source)->hier, &prov);
    st = st == ODIN3_OK ? reprov_node(locs, module, &node, prov) : st;
    locs->stats->inherited += st == ODIN3_OK ? 1U : 0U;
    return st;
}

odin3_status blif_locs_inherit(blif_locs *locs, odin3_module *module) {
    if (locs->bound) {
        return ODIN3_OK;
    }
    uint32_t end = odin3_module_node_end(module); /* nodes made by the rebuilds are not revisited */
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        st = inherit_node(locs, module, (odin3_node_id){id});
    }
    return st;
}
