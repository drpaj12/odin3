/*
 * hash_rows.c — renamed objects, hash names and the name map file. See hash_rows.h.
 */
#include "backends/common/hash_rows.h"

#include "backends/common/hdl_loc.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { PATH_BUF = 4096 };

/* The test hook (odin3_hash_rows_test_start): 0 is the production width. */
static uint32_t test_start = 0;

void odin3_hash_rows_init(odin3_hash_rows *rows) {
    odin3_vec_init(&rows->rows, sizeof(odin3_hash_row));
    odin3_strbuf_init(&rows->text);
    rows->start = test_start;
}

void odin3_hash_rows_test_start(uint32_t start) {
    test_start = start;
}

static uint32_t start_of(const odin3_hash_rows *rows) {
    return rows->start != 0 ? rows->start : ODIN3_HASH_NAME_WIDTH;
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
    odin3_status st =
        fulls != NULL && names != NULL && index != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (size_t i = req->first; st == ODIN3_OK && i < rows->rows.len; i++) {
        const odin3_hash_row *row = odin3_hash_rows_at(rows, i);
        if (all || row->kind == kind) {
            fulls[count] = odin3_hash_rows_full(rows, row);
            index[count++] = i;
        }
    }
    if (st == ODIN3_OK && count > 0) {
        const odin3_hash_name_set set = {fulls, count, req->reserved, req->nreserved};
        st = odin3_hash_names_resolve_at(&set, start_of(rows), names);
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
    odin3_status st = odin3_name_map_write(entries, (uint32_t)count, start_of(rows), out);
    odin3_util_free(entries);
    return st;
}

/* The identity of path's directory (device and inode) and the last component. False when the
 * directory cannot be stat'ed. */
typedef struct dest_id {
    dev_t dev;
    ino_t ino;
    const char *base;
} dest_id;

static bool dest_id_of(const char *path, dest_id *out) {
    const char *slash = strrchr(path, '/');
    char dir_path[PATH_BUF];
    struct stat info;
    if (slash == NULL) {
        (void)snprintf(dir_path, sizeof dir_path, ".");
    } else if ((size_t)(slash - path) >= sizeof dir_path) {
        return false;
    } else {
        (void)snprintf(dir_path, sizeof dir_path, "%.*s", slash == path ? 1 : (int)(slash - path),
                       path);
    }
    if (stat(dir_path, &info) != 0) {
        return false;
    }
    *out = (dest_id){info.st_dev, info.st_ino, slash == NULL ? path : slash + 1};
    return true;
}

/* True when both paths name one file: the same directory (device and inode, so `dir/./x` and
 * `dir/x` agree) and the same name, or (both exist) the same device and inode. */
static bool same_destination(const char *left, const char *right) {
    struct stat first;
    struct stat second;
    if (stat(left, &first) == 0 && stat(right, &second) == 0 && first.st_dev == second.st_dev &&
        first.st_ino == second.st_ino) {
        return true;
    }
    dest_id lhs;
    dest_id rhs;
    return dest_id_of(left, &lhs) && dest_id_of(right, &rhs) && lhs.dev == rhs.dev &&
           lhs.ino == rhs.ino && strcmp(lhs.base, rhs.base) == 0;
}

odin3_status odin3_hash_map_path(const odin3_hash_map_req *req, odin3_strbuf *path) {
    odin3_strbuf_clear(path);
    if (req->name_map != NULL) {
        if (strcmp(req->out_path, "-") != 0 && same_destination(req->out_path, req->name_map)) {
            odin3_log(ODIN3_LOG_ERROR, "%s: --name-map is the output path %s", req->who,
                      req->out_path);
            return ODIN3_ERR_INVALID_ARG;
        }
        return odin3_strbuf_append(path, odin3_bytes_cstr(req->name_map));
    }
    if (strcmp(req->out_path, "-") == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "%s: --name-style hash needs --name-map when the output is stdout", req->who);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_strbuf_appendf(path, "%s.names", req->out_path);
}

odin3_status odin3_hash_map_open(odin3_hash_map_file *map, const char *path) {
    odin3_strbuf_init(&map->path);
    map->open = false;
    odin3_status st = odin3_strbuf_append(&map->path, odin3_bytes_cstr(path));
    st = st == ODIN3_OK ? odin3_atomic_file_open(&map->file, map->path.data) : st;
    map->open = st == ODIN3_OK;
    if (!map->open) {
        odin3_strbuf_free(&map->path);
    }
    return st;
}

odin3_status odin3_hash_map_close(odin3_hash_map_file *map, odin3_bytes text,
                                  odin3_atomic_file *netlist, odin3_status status) {
    odin3_status st = netlist != NULL ? odin3_atomic_file_finish(netlist, status) : status;
    if (map->open) {
        if (st == ODIN3_OK && text.len > 0 &&
            fwrite(text.ptr, 1, text.len, map->file.fp) != text.len) {
            odin3_log(ODIN3_LOG_ERROR, "%s: write failed", map->file.path);
            st = ODIN3_ERR_IO;
        }
        st = odin3_atomic_file_finish(&map->file, st);
        st = odin3_atomic_file_commit(&map->file, st); /* the map first: never a new netlist */
        map->open = false;                             /* beside a stale map */
    }
    if (netlist != NULL) {
        st = odin3_atomic_file_commit(netlist, st);
    }
    odin3_strbuf_free(&map->path);
    return st;
}
