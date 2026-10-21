/*
 * names.c — provenance and hash names of the JSON writer's cell and netname keys (spec 5.4
 * section 13, H-1 to H-4). In `provenance` style an object with an HDL location has the key
 * `hier/<short>@file:line`. `hash` is two passes over the same writer: COLLECT writes those keys
 * to a sink and collects the renamed objects, resolving each module's hash names at its end;
 * APPLY writes the real file and gives each renamed object, in the same order, its hash name.
 */
#include "backends/json/jw.h"
#include "backends/prov_name.h"
#include "util/alloc.h"
#include "util/log.h"

#include <string.h>

enum { KIND_SHIFT = 32 }; /* row key: object kind << 32 | object ID */

static uint64_t row_key(const jw_keyspec *spec) {
    return ((uint64_t)spec->obj_kind << KIND_SHIFT) | spec->obj_id;
}

static odin3_status collect_row(jw *out, const jw_keyspec *spec, const char *key) {
    return odin3_hash_rows_add(
        &out->rows,
        &(odin3_hash_row_req){
            .module = odin3_module_id_of(out->module).v,
            .module_name = {odin3_strtab_get(out->strtab, odin3_module_name(out->module)),
                            odin3_strtab_len(out->strtab, odin3_module_name(out->module))},
            .kind = spec->obj_kind,
            .key = row_key(spec),
            .full = odin3_bytes_cstr(key)});
}

/* APPLY: the next collected row's hash name, which must be this object's. */
static const char *apply_row(jw *out, const jw_keyspec *spec) {
    const odin3_hash_row *row = out->row_cursor < odin3_hash_rows_count(&out->rows)
                                    ? odin3_hash_rows_at(&out->rows, out->row_cursor)
                                    : NULL;
    if (row == NULL || row->key != row_key(spec) ||
        row->module != odin3_module_id_of(out->module).v) {
        odin3_log(ODIN3_LOG_ERROR, "write_json: hash names out of step with the first pass");
        out->status = ODIN3_ERR_CHECK;
        return "";
    }
    out->row_cursor++;
    odin3_strbuf_clear(&out->key);
    odin3_status st = odin3_strbuf_append(&out->key, odin3_bytes_cstr(row->hash.text));
    if (st != ODIN3_OK) {
        out->status = st;
        return "";
    }
    jw_record_key(out, out->key.data);
    /* the first pass held the full name, not the hash name: hold it too, so a plain key is bumped
     * past it as it was there */
    uint32_t id = 0;
    st = odin3_strtab_intern(out->written, odin3_hash_rows_full(&out->rows, row), &id);
    out->status = out->status == ODIN3_OK ? st : out->status;
    return out->status == ODIN3_OK ? out->key.data : "";
}

const char *jw_prov_key(jw *out, const jw_keyspec *spec) {
    if (out->names == JW_NAMES_SHORT || spec->prov.v == 0 || out->status != ODIN3_OK) {
        return NULL;
    }
    odin3_strbuf_clear(&out->pshort);
    odin3_status st = odin3_strbuf_append(&out->pshort, odin3_bytes_cstr(spec->head));
    st = st == ODIN3_OK ? odin3_strbuf_append(&out->pshort, odin3_bytes_cstr(spec->tail)) : st;
    st = st == ODIN3_OK
             ? odin3_prov_name(out->hdl, spec->prov,
                               (odin3_bytes){out->pshort.data, out->pshort.len}, &out->pname)
             : st;
    if (st != ODIN3_OK) {
        out->status = st;
        return "";
    }
    if (out->pname.len == 0) {
        return NULL;
    }
    if (out->names == JW_NAMES_APPLY) {
        out->hide = out->pname.data[0] == '$';
        return apply_row(out, spec);
    }
    jw_keyspec use = *spec;
    use.head = out->pname.data;
    use.tail = "";
    const char *key = jw_plain_key(out, &use);
    st = out->names == JW_NAMES_COLLECT && out->status == ODIN3_OK ? collect_row(out, spec, key)
                                                                   : ODIN3_OK;
    out->status = out->status == ODIN3_OK ? st : out->status;
    return key;
}

/* The keys written in the module that stay as they are (H-3): all but the rows from `first` on. */
static odin3_status reserved_keys(jw *out, size_t first, odin3_bytes **list, uint32_t *count) {
    odin3_u64map *renamed = odin3_u64map_create(0);
    size_t names = odin3_strtab_count(out->written);
    *list = odin3_util_calloc((names + 1) * sizeof **list);
    *count = 0;
    odin3_status st = renamed != NULL && *list != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t i = first; st == ODIN3_OK && i < odin3_hash_rows_count(&out->rows); i++) {
        uint32_t id = 0;
        const odin3_hash_row *row = odin3_hash_rows_at(&out->rows, i);
        if (odin3_strtab_find(out->written, odin3_hash_rows_full(&out->rows, row), &id)) {
            st = odin3_u64map_put(renamed, (odin3_kv){id, 1});
        }
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < names; id++) {
        if (!odin3_u64map_get(renamed, id, NULL)) {
            (*list)[(*count)++] = (odin3_bytes){odin3_strtab_get(out->written, id),
                                                odin3_strtab_len(out->written, id)};
        }
    }
    odin3_u64map_destroy(renamed);
    return st;
}

void jw_names_module_end(jw *out) {
    if (out->names != JW_NAMES_COLLECT || out->status != ODIN3_OK) {
        return;
    }
    size_t first = out->module_rows;
    odin3_bytes *reserved = NULL;
    uint32_t count = 0;
    odin3_status st = reserved_keys(out, first, &reserved, &count);
    /* cells and netnames share one namespace (IR-14), so their hash names must not collide */
    st = st == ODIN3_OK ? odin3_hash_rows_resolve(
                              &out->rows, &(odin3_hash_resolve_req){first, reserved, count, true})
                        : st;
    odin3_util_free(reserved);
    out->module_rows = odin3_hash_rows_count(&out->rows);
    out->status = out->status == ODIN3_OK ? st : out->status;
}

void jw_names_done(jw *out) {
    if (out->names == JW_NAMES_APPLY && out->status == ODIN3_OK &&
        out->row_cursor != odin3_hash_rows_count(&out->rows)) {
        odin3_log(ODIN3_LOG_ERROR, "write_json: hash names out of step with the first pass");
        out->status = ODIN3_ERR_CHECK;
    }
}
