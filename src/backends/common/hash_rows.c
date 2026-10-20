/*
 * hash_rows.c — renamed objects, hash names and the name map file. See hash_rows.h.
 */
#include "backends/common/hash_rows.h"

#include "backends/common/hdl_loc.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

void odin3_hash_rows_init(odin3_hash_rows *rows) {
    odin3_vec_init(&rows->rows, sizeof(odin3_hash_row));
    odin3_strbuf_init(&rows->text);
}

void odin3_hash_rows_free(odin3_hash_rows *rows) {
    odin3_vec_free(&rows->rows);
    odin3_strbuf_free(&rows->text);
}

odin3_status odin3_hash_rows_add(odin3_hash_rows *rows, const odin3_hash_row_req *req) {
    size_t used = rows->text.len;
    odin3_status st = odin3_strbuf_append(&rows->text, req->module_name);
    st = st == ODIN3_OK ? odin3_strbuf_append(&rows->text, req->full) : st;
    odin3_hash_row *row = st == ODIN3_OK ? odin3_vec_push(&rows->rows) : NULL;
    if (row == NULL) {
        if (rows->text.data != NULL) {
            rows->text.len = used;
            rows->text.data[used] = '\0';
        }
        return ODIN3_ERR_NO_MEMORY;
    }
    *row = (odin3_hash_row){.module = req->module,
                            .kind = req->kind,
                            .key = req->key,
                            .module_name_off = used,
                            .module_name_len = req->module_name.len,
                            .full_off = used + req->module_name.len,
                            .full_len = req->full.len};
    return ODIN3_OK;
}

size_t odin3_hash_rows_count(const odin3_hash_rows *rows) {
    return rows->rows.len;
}

const odin3_hash_row *odin3_hash_rows_at(const odin3_hash_rows *rows, size_t index) {
    return odin3_vec_cat(&rows->rows, index);
}

odin3_bytes odin3_hash_rows_full(const odin3_hash_rows *rows, const odin3_hash_row *row) {
    return (odin3_bytes){rows->text.data + row->full_off, row->full_len};
}

/* Resolves the rows of kind `kind` (any kind when `all`) among rows[first..count). */
static odin3_status resolve_group(odin3_hash_rows *rows, const odin3_hash_resolve_req *req,
                                  bool all, odin3_name_kind kind) {
    size_t total = rows->rows.len - req->first;
    odin3_bytes *fulls = odin3_util_calloc((total + 1) * sizeof *fulls);
    odin3_hash_name *names = odin3_util_calloc((total + 1) * sizeof *names);
    size_t *index = odin3_util_calloc((total + 1) * sizeof *index);
    uint32_t count = 0;
    odin3_status st = fulls != NULL && names != NULL && index != NULL ? ODIN3_OK
                                                                      : ODIN3_ERR_NO_MEMORY;
    for (size_t i = req->first; st == ODIN3_OK && i < rows->rows.len; i++) {
        const odin3_hash_row *row = odin3_hash_rows_at(rows, i);
        if (all || row->kind == kind) {
            fulls[count] = odin3_hash_rows_full(rows, row);
            index[count++] = i;
        }
    }
    if (st == ODIN3_OK && count > 0) {
        st = odin3_hash_names_resolve(fulls, count, req->reserved, req->nreserved, names);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        odin3_hash_row *row = odin3_vec_at(&rows->rows, index[i]);
        row->hash = names[i];
    }
    odin3_util_free(fulls);
    odin3_util_free(names);
    odin3_util_free(index);
    return st;
}

odin3_status odin3_hash_rows_resolve(odin3_hash_rows *rows, const odin3_hash_resolve_req *req) {
    if (req->first >= rows->rows.len) {
        return ODIN3_OK;
    }
    if (req->merge_kinds) {
        return resolve_group(rows, req, true, ODIN3_NAME_NET);
    }
    odin3_status st = ODIN3_OK;
    for (int kind = 0; st == ODIN3_OK && kind < ODIN3_NAME_KIND_COUNT; kind++) {
        st = resolve_group(rows, req, false, (odin3_name_kind)kind);
    }
    return st;
}

odin3_status odin3_hash_rows_index(const odin3_hash_rows *rows, odin3_u64map **index) {
    *index = odin3_u64map_create(0);
    if (*index == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < rows->rows.len; i++) {
        const odin3_hash_row *row = odin3_hash_rows_at(rows, i);
        if (odin3_u64map_put(*index, (odin3_kv){row->key, i}) != ODIN3_OK) {
            odin3_u64map_destroy(*index);
            *index = NULL;
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* The map entry of a row; hier, file and line are the parts of the full name (empty and 0 when it
 * does not decode as one). */
static odin3_name_entry entry_of(const odin3_hash_rows *rows, const odin3_hash_row *row) {
    odin3_name_entry entry = {
        .module = {rows->text.data + row->module_name_off, row->module_name_len},
        .kind = row->kind,
        .short_name = {row->hash.text, strlen(row->hash.text)},
        .full = odin3_hash_rows_full(rows, row)};
    odin3_hdl_name parts;
    if (odin3_hdl_name_decode(entry.full, &parts)) {
        entry.hier = parts.hier;
        entry.file = parts.loc.file;
        entry.line = parts.loc.line;
    }
    return entry;
}

odin3_status odin3_hash_rows_map_text(const odin3_hash_rows *rows, odin3_strbuf *out) {
    size_t count = rows->rows.len;
    odin3_name_entry *entries = odin3_util_calloc((count + 1) * sizeof *entries);
    if (entries == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        entries[i] = entry_of(rows, odin3_hash_rows_at(rows, i));
    }
    odin3_status st =
        odin3_name_map_write(entries, (uint32_t)count, ODIN3_HASH_NAME_WIDTH, out);
    odin3_util_free(entries);
    return st;
}

odin3_status odin3_hash_map_path(const char *who, const char *out_path, const char *name_map,
                                 odin3_strbuf *path) {
    odin3_strbuf_clear(path);
    if (name_map != NULL) {
        return odin3_strbuf_append(path, odin3_bytes_cstr(name_map));
    }
    if (strcmp(out_path, "-") == 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: --name-style hash needs --name-map when the output is stdout",
                  who);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_strbuf_appendf(path, "%s.names", out_path);
}

odin3_status odin3_hash_map_open(odin3_hash_map_file *map, const char *path) {
    odin3_status st = odin3_atomic_file_open(&map->file, path);
    map->open = st == ODIN3_OK;
    return st;
}

odin3_status odin3_hash_map_close(odin3_hash_map_file *map, odin3_bytes text, odin3_status status) {
    if (!map->open) {
        return status;
    }
    map->open = false;
    odin3_status st = status;
    if (st == ODIN3_OK && text.len > 0 && fwrite(text.ptr, 1, text.len, map->file.fp) != text.len) {
        odin3_log(ODIN3_LOG_ERROR, "%s: write failed", map->file.path);
        st = ODIN3_ERR_IO;
    }
    return odin3_atomic_file_close(&map->file, st);
}
