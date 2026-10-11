/*
 * project.c — the project record (PRJ-1..3): lifetime, project-file loading, builders, located
 * errors (§8) and finish.
 */
#include "frontends/project/project.h"

#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/project_test.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LIBRARY_BITS = 32, DECIMAL = 10, SIZE_TEXT = 64 };

/* --- lifetime ------------------------------------------------------------------------------ */

/* Every vec of the record with its element size, for init and free. */
typedef struct vec_slot {
    size_t offset;
    size_t elem_size;
} vec_slot;

static const vec_slot k_vecs[] = {
    {offsetof(odin3_project, files), sizeof(odin3_proj_file)},
    {offsetof(odin3_project, libfiles), sizeof(odin3_proj_file)},
    {offsetof(odin3_project, libdirs), sizeof(odin3_proj_path)},
    {offsetof(odin3_project, incdirs), sizeof(odin3_proj_path)},
    {offsetof(odin3_project, libext), sizeof(uint32_t)},
    {offsetof(odin3_project, defines), sizeof(odin3_proj_define)},
    {offsetof(odin3_project, params), sizeof(odin3_proj_param)},
    {offsetof(odin3_project, arch), sizeof(odin3_proj_arch)},
    {offsetof(odin3_project, rules), sizeof(odin3_proj_rule)},
    {offsetof(odin3_project, rule_cells), sizeof(uint32_t)},
    {offsetof(odin3_project, inventory), sizeof(odin3_proj_inventory)},
    {offsetof(odin3_project, overrides), sizeof(odin3_proj_override)},
    {offsetof(odin3_project, override_params), sizeof(odin3_proj_kv)},
    {offsetof(odin3_project, ignored), sizeof(uint32_t)},
    {offsetof(odin3_project, duplicates), sizeof(uint32_t)},
};

static odin3_vec *vec_at(odin3_project *proj, size_t idx) {
    return (odin3_vec *)((char *)proj + k_vecs[idx].offset);
}

odin3_project *odin3_project_create(odin3_design *design) {
    odin3_project *proj = odin3_util_calloc(sizeof *proj);
    if (proj == NULL) {
        return NULL;
    }
    proj->design = design;
    for (size_t i = 0; i < sizeof k_vecs / sizeof k_vecs[0]; i++) {
        odin3_vec_init(vec_at(proj, i), k_vecs[i].elem_size);
    }
    proj->limits = (odin3_proj_limits){ODIN3_PROJ_MAX_FLIST_DEPTH, ODIN3_PROJ_MAX_QIP_DEPTH,
                                       ODIN3_PROJ_MAX_WORDS, ODIN3_PROJ_MAX_XML_NODES};
    proj->arena = odin3_arena_create(0);
    proj->file_keys = odin3_u64map_create(0);
    proj->lib_keys = odin3_u64map_create(0);
    if (proj->arena == NULL || proj->file_keys == NULL || proj->lib_keys == NULL) {
        odin3_project_destroy(proj);
        return NULL;
    }
    return proj;
}

void odin3_project_destroy(odin3_project *proj) {
    if (proj == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof k_vecs / sizeof k_vecs[0]; i++) {
        odin3_vec_free(vec_at(proj, i));
    }
    odin3_u64map_destroy(proj->lib_keys);
    odin3_u64map_destroy(proj->file_keys);
    odin3_arena_destroy(proj->arena);
    odin3_util_free(proj);
}

void odin3_project_test_set_limits(odin3_project *proj, odin3_proj_limits limits) {
    proj->limits = limits;
}

/* --- errors -------------------------------------------------------------------------------- */

static const char *const k_kind_names[] = {
    "?",
    "missing_file",
    "unknown_key",
    "unknown_option",
    "unknown_file_type",
    "unsupported_input_type",
    "syntax",
    "undefined_variable",
    "f_cycle",
    "qip_cycle",
    "ambiguous_revision",
    "unknown_revision",
    "include_not_found",
    "include_cycle",
};

const char *odin3_proj_err_kind_name(odin3_proj_err_kind kind) {
    size_t count = sizeof k_kind_names / sizeof k_kind_names[0];
    return (size_t)kind < count ? k_kind_names[kind] : k_kind_names[0];
}

/* A fixed-size message under construction; text past the end is cut. */
typedef struct msg_buf {
    char text[ODIN3_LOG_BUF];
    size_t len;
} msg_buf;

static void msg_add(msg_buf *msg, const char *bytes, size_t len) {
    size_t room = sizeof msg->text - 1 - msg->len;
    size_t take = len < room ? len : room;
    if (take > 0) {
        memcpy(msg->text + msg->len, bytes, take);
    }
    msg->len += take;
    msg->text[msg->len] = '\0';
}

static void msg_str(msg_buf *msg, const char *str) {
    msg_add(msg, str, strlen(str));
}

/* A name as written, a NUL byte shown as \0 (the log line is a C string). */
static void msg_name(msg_buf *msg, odin3_bytes name) {
    if (name.len == 0) {
        return;
    }
    const char *text = name.ptr;
    size_t from = 0;
    for (size_t i = 0; i < name.len; i++) {
        if (text[i] == '\0') {
            msg_add(msg, text + from, i - from);
            msg_str(msg, "\\0");
            from = i + 1;
        }
    }
    msg_add(msg, text + from, name.len - from);
}

/* "<kind>: <detail>: 'n1', 'n2'" (point (g)). */
static void format_err(msg_buf *msg, const odin3_proj_err *err) {
    msg->len = 0;
    msg->text[0] = '\0';
    msg_str(msg, odin3_proj_err_kind_name(err->kind));
    if (err->detail != NULL || err->nnames > 0) {
        msg_str(msg, ": ");
    }
    if (err->detail != NULL) {
        msg_str(msg, err->detail);
        if (err->nnames > 0) {
            msg_str(msg, ": ");
        }
    }
    for (uint32_t i = 0; i < err->nnames; i++) {
        msg_str(msg, i > 0 ? ", '" : "'");
        msg_name(msg, err->names[i]);
        msg_str(msg, "'");
    }
}

/* A file named in the design's diagnostic style: resolved (when known) or as given. */
static const char *file_name(const odin3_project *proj, const odin3_proj_path *file) {
    const odin3_strtab *tab = odin3_design_strtab(proj->design);
    bool resolved = odin3_design_diag_paths(proj->design) == ODIN3_SRCFMT_RESOLVED;
    uint32_t str = resolved && file->resolved != 0 ? file->resolved : file->given;
    return odin3_strtab_get(tab, str);
}

odin3_status odin3_project_fail(odin3_project *proj, const odin3_proj_err *err) {
    msg_buf msg;
    format_err(&msg, err);
    if (err->scope == ODIN3_PROJ_LOCATED) {
        odin3_diag(proj->design, ODIN3_LOG_ERROR, err->at, "%s", msg.text);
    } else if (err->scope == ODIN3_PROJ_FILE && err->file != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: error: %s", file_name(proj, err->file), msg.text);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.text);
    }
    return ODIN3_ERR_PARSE;
}

/* A one-name error at a site: a located word, else a whole file, else nowhere. */
typedef struct err_site {
    odin3_loc at;
    const odin3_proj_path *file;
} err_site;

typedef struct err_what {
    odin3_proj_err_kind kind;
    const char *detail;
} err_what;

static odin3_status fail_one(odin3_project *proj, err_site site, err_what what, odin3_bytes name) {
    odin3_proj_scope scope = ODIN3_PROJ_UNLOCATED;
    if (site.at.v != 0) {
        scope = ODIN3_PROJ_LOCATED;
    } else if (site.file != NULL) {
        scope = ODIN3_PROJ_FILE;
    }
    odin3_proj_err err = {what.kind, scope, site.at, site.file, what.detail, &name, 1};
    return odin3_project_fail(proj, &err);
}

static odin3_status syntax_at(odin3_project *proj, odin3_loc at, const char *detail,
                              odin3_bytes name) {
    err_site site = {at, NULL};
    return fail_one(proj, site, (err_what){ODIN3_PROJ_ERR_SYNTAX, detail}, name);
}

/* --- loading project files ----------------------------------------------------------------- */

/* A failed read, at parent (unlocated at 0), naming the file in the design's path style: IO
 * "cannot read '<file>': <reason>", or over the size cap PARSE `syntax` in the source manager's
 * words ("file '<file>' is N bytes, over the limit of M"). */
static odin3_status cannot_read(odin3_project *proj, const odin3_proj_path *file, odin3_loc parent,
                                const odin3_file_bytes *bytes) {
    msg_buf msg = {{0}, 0};
    const char *name = file_name(proj, file);
    if (bytes->too_big) {
        char size[SIZE_TEXT];
        (void)snprintf(size, sizeof size, "%zu bytes, over the limit of %" PRIu32, bytes->len,
                       ODIN3_SRC_MAX_FILE_BYTES);
        msg_str(&msg, "syntax: file '");
        msg_str(&msg, name);
        msg_str(&msg, "' is ");
        msg_str(&msg, size);
    } else {
        msg_str(&msg, "cannot read '");
        msg_str(&msg, name);
        msg_str(&msg, "': ");
        msg_str(&msg, strerror(bytes->err));
    }
    if (parent.v != 0) {
        odin3_diag(proj->design, ODIN3_LOG_ERROR, parent, "%s", msg.text);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg.text);
    }
    return bytes->too_big ? ODIN3_ERR_PARSE : ODIN3_ERR_IO;
}

/* Registers every line start with the source manager and records the text's lines (a final
 * empty line is not one). */
static odin3_status add_lines(odin3_srcman *sm, odin3_proj_text *out) {
    if (out->len > 0 && odin3_vec_push(&out->lines) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < out->len; i++) {
        if (out->bytes[i] != '\n') {
            continue;
        }
        odin3_status st = odin3_srcman_add_line(sm, out->buffer, i + 1);
        if (st != ODIN3_OK) {
            return st;
        }
        if (i + 1 < out->len) {
            uint32_t *start = odin3_vec_push(&out->lines);
            if (start == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *start = i + 1;
        }
    }
    return ODIN3_OK;
}

static odin3_status register_file(odin3_project *proj, const odin3_proj_path *file,
                                  odin3_loc parent, odin3_proj_text *out) {
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(proj->design, &sm);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_srcfile_spec spec = {.name = file->given,
                               .resolved = file->resolved,
                               .library = 0,
                               .parent = parent.v,
                               .len = out->len};
    st = odin3_srcman_add_file(sm, &spec, &out->buffer);
    return st != ODIN3_OK ? st : add_lines(sm, out);
}

odin3_status odin3_project_load(odin3_project *proj, const odin3_proj_path *file, odin3_loc parent,
                                odin3_proj_text *out) {
    *out = (odin3_proj_text){.bytes = NULL, .len = 0, .buffer = {0}};
    odin3_vec_init(&out->lines, sizeof(uint32_t));
    const char *path = odin3_strtab_get(odin3_design_strtab(proj->design), file->resolved);
    odin3_file_bytes bytes;
    odin3_status st = odin3_file_read_all(path, ODIN3_SRC_MAX_FILE_BYTES, &bytes);
    if (st == ODIN3_ERR_IO || st == ODIN3_ERR_PARSE) {
        st = cannot_read(proj, file, parent, &bytes);
    }
    if (st == ODIN3_OK) {
        out->bytes = bytes.data;
        out->len = (uint32_t)bytes.len;
        bytes.data = NULL;
        st = register_file(proj, file, parent, out);
    }
    odin3_file_bytes_free(&bytes);
    if (st != ODIN3_OK) {
        odin3_proj_text_free(out);
    }
    return st;
}

odin3_bytes odin3_proj_text_line(const odin3_proj_text *text, uint32_t index, uint32_t *offset) {
    uint32_t start = *(const uint32_t *)odin3_vec_cat(&text->lines, index);
    uint32_t end = text->len;
    if (index + 1 < text->lines.len) {
        end = *(const uint32_t *)odin3_vec_cat(&text->lines, index + 1) - 1; /* at the '\n' */
    } else if (end > start && text->bytes[end - 1] == '\n') {
        end--;
    }
    if (end > start && text->bytes[end - 1] == '\r' && end < text->len &&
        text->bytes[end] == '\n') {
        end--;
    }
    *offset = start;
    return (odin3_bytes){text->bytes + start, end - start};
}

void odin3_proj_text_free(odin3_proj_text *text) {
    odin3_util_free(text->bytes);
    odin3_vec_free(&text->lines);
    text->bytes = NULL;
    text->len = 0;
    text->buffer = (odin3_srcbuf_id){0};
}

/* --- paths --------------------------------------------------------------------------------- */

static bool has_nul(odin3_bytes bytes) {
    return bytes.len > 0 && memchr(bytes.ptr, '\0', bytes.len) != NULL;
}

/* A word holding a NUL byte cannot be a strtab string (PRJ-4): `syntax` at at, names [word]. */
static odin3_status reject_nul(odin3_project *proj, odin3_bytes word, odin3_loc at) {
    return has_nul(word) ? syntax_at(proj, at, "NUL byte in word", word) : ODIN3_OK;
}

odin3_status odin3_project_intern(odin3_project *proj, odin3_bytes bytes, uint32_t *id) {
    if (has_nul(bytes)) { /* callers with a location reject it first, located */
        err_what what = {ODIN3_PROJ_ERR_SYNTAX, "NUL byte in word"};
        return fail_one(proj, (err_site){{0}, NULL}, what, bytes);
    }
    return odin3_design_intern(proj->design, bytes, id);
}

static const char *const k_missing[] = {"not found", "file not found", "directory not found",
                                        "not found"};

static bool kind_ok(odin3_path_kind kind, odin3_proj_want want) {
    if (want == ODIN3_PROJ_WANT_FILE) {
        return kind == ODIN3_PATH_FILE;
    }
    if (want == ODIN3_PROJ_WANT_DIR) {
        return kind == ODIN3_PATH_DIR;
    }
    return kind != ODIN3_PATH_NONE;
}

odin3_status odin3_project_path(odin3_project *proj, const odin3_proj_path_req *req,
                                odin3_proj_path *out) {
    if (has_nul(req->given)) {
        return reject_nul(proj, req->given, req->at);
    }
    odin3_strbuf resolved;
    odin3_strbuf_init(&resolved);
    odin3_status st = odin3_path_resolve((odin3_path_join){req->base, req->given}, &resolved);
    odin3_bytes text = {resolved.data, resolved.len};
    if (st == ODIN3_OK) {
        bool nul = memchr(resolved.data, '\0', resolved.len) != NULL;
        if (nul || !kind_ok(odin3_path_stat(resolved.data), req->want)) {
            size_t nmissing = sizeof k_missing / sizeof k_missing[0];
            size_t want = (size_t)req->want < nmissing ? (size_t)req->want : 0;
            err_what what = {ODIN3_PROJ_ERR_MISSING_FILE, k_missing[want]};
            st = fail_one(proj, (err_site){req->at, NULL}, what, text);
        }
    }
    odin3_proj_path got = {0, 0, req->at};
    st = st != ODIN3_OK ? st : odin3_project_intern(proj, req->given, &got.given);
    st = st != ODIN3_OK ? st : odin3_project_intern(proj, text, &got.resolved);
    if (st == ODIN3_OK) {
        *out = got;
    }
    odin3_strbuf_free(&resolved);
    return st;
}

/* --- builders ------------------------------------------------------------------------------ */

static odin3_status push_id(odin3_vec *vec, uint32_t str) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = str;
    return ODIN3_OK;
}

/* Appends file to list unless key is in keys (then its path goes to duplicates). */
static odin3_status add_keyed(odin3_project *proj, odin3_vec *list, odin3_u64map *keys,
                              uint64_t key, const odin3_proj_file *file) {
    uint64_t seen = 0;
    if (odin3_u64map_get(keys, key, &seen)) {
        return push_id(&proj->duplicates, file->path.resolved);
    }
    odin3_proj_file *slot = odin3_vec_push(list);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (odin3_u64map_put(keys, (odin3_kv){key, 1}) != ODIN3_OK) {
        odin3_vec_pop(list);
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *file;
    return ODIN3_OK;
}

odin3_status odin3_project_add_file(odin3_project *proj, const odin3_proj_file *file) {
    uint64_t key = (uint64_t)file->path.resolved << LIBRARY_BITS | file->library;
    return add_keyed(proj, &proj->files, proj->file_keys, key, file);
}

odin3_status odin3_project_add_libfile(odin3_project *proj, const odin3_proj_file *file) {
    odin3_proj_file lib = *file;
    lib.library = 0;
    return add_keyed(proj, &proj->libfiles, proj->lib_keys, file->path.resolved, &lib);
}

odin3_status odin3_project_add_dir(odin3_project *proj, odin3_proj_dir_list list,
                                   const odin3_proj_path *path) {
    odin3_vec *vec = list == ODIN3_PROJ_INCDIRS ? &proj->incdirs : &proj->libdirs;
    odin3_proj_path *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *path;
    return ODIN3_OK;
}

odin3_status odin3_project_add_libext(odin3_project *proj, odin3_bytes ext, odin3_loc at) {
    odin3_lang lang = ODIN3_LANG_VERILOG;
    odin3_status st = reject_nul(proj, ext, at);
    if (st == ODIN3_OK && !odin3_lang_of_ext(ext, &lang)) {
        err_what what = {ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE, "no language for extension"};
        st = fail_one(proj, (err_site){at, NULL}, what, ext);
    }
    uint32_t str = 0;
    st = st != ODIN3_OK ? st : odin3_project_intern(proj, ext, &str);
    return st != ODIN3_OK ? st : push_id(&proj->libext, str);
}

static bool ident_start(uint8_t byte) {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool ident_char(uint8_t byte) {
    return ident_start(byte) || (byte >= '0' && byte <= '9') || byte == '$';
}

/* Whether text[0, len) is an IDENT ([A-Za-z_][A-Za-z0-9_$]*). */
static bool is_ident(const uint8_t *text, size_t len) {
    if (len == 0 || !ident_start(text[0])) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (!ident_char(text[i])) {
            return false;
        }
    }
    return true;
}

enum { PY_ASCII_LIMIT = 0x80 };

/* --- Python's whitespace ------------------------------------------------------------------- */

/* The UTF-8 encodings of the non-ASCII characters str.isspace() accepts. */
static const char *const k_wide_spaces[] = {
    "\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x81",
    "\xE2\x80\x82", "\xE2\x80\x83", "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86",
    "\xE2\x80\x87", "\xE2\x80\x88", "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xA8",
    "\xE2\x80\xA9", "\xE2\x80\xAF", "\xE2\x81\x9F", "\xE3\x80\x80",
};

size_t odin3_py_space_len(odin3_bytes text, size_t pos) {
    const uint8_t *bytes = text.ptr;
    if (pos >= text.len) {
        return 0;
    }
    uint8_t byte = bytes[pos];
    if (byte == ' ' || (byte >= '\t' && byte <= '\r') || (byte >= '\x1c' && byte <= '\x1f')) {
        return 1;
    }
    if (byte < PY_ASCII_LIMIT) {
        return 0;
    }
    for (size_t k = 0; k < sizeof k_wide_spaces / sizeof k_wide_spaces[0]; k++) {
        size_t len = strlen(k_wide_spaces[k]);
        if (text.len - pos >= len && memcmp(bytes + pos, k_wide_spaces[k], len) == 0) {
            return len;
        }
    }
    return 0;
}

odin3_bytes odin3_proj_py_strip(odin3_bytes text) {
    size_t start = 0;
    for (size_t len = odin3_py_space_len(text, 0); len > 0; len = odin3_py_space_len(text, start)) {
        start += len;
    }
    size_t end = start;
    for (size_t k = start; k < text.len;) {
        size_t len = odin3_py_space_len(text, k);
        k += len > 0 ? len : 1;
        end = len > 0 ? end : k;
    }
    return (odin3_bytes){(const char *)text.ptr + start, end - start};
}

bool odin3_proj_bytes_is(odin3_bytes bytes, const char *text) {
    size_t len = strlen(text);
    return bytes.len == len && (len == 0 || memcmp(bytes.ptr, text, len) == 0);
}

bool odin3_proj_bytes_eq(odin3_bytes left, odin3_bytes right) {
    return left.len == right.len && (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

odin3_bytes odin3_proj_str_bytes(const odin3_project *proj, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(proj->design);
    return (odin3_bytes){odin3_strtab_get(tab, str), odin3_strtab_len(tab, str)};
}

size_t odin3_proj_ident_len(odin3_bytes text, size_t from) {
    const uint8_t *bytes = text.ptr;
    if (from >= text.len || !ident_start(bytes[from])) {
        return 0;
    }
    size_t j = from + 1;
    while (j < text.len && ident_char(bytes[j])) {
        j++;
    }
    return j - from;
}

odin3_status odin3_project_add_define(odin3_project *proj, odin3_bytes word, odin3_loc at) {
    const uint8_t *text = word.ptr;
    const uint8_t *eq = word.len > 0 ? memchr(text, '=', word.len) : NULL;
    size_t name_len = eq != NULL ? (size_t)(eq - text) : word.len;
    if (!is_ident(text, name_len)) {
        return syntax_at(proj, at, "bad macro name", word);
    }
    if (has_nul(word)) {
        return reject_nul(proj, word, at);
    }
    odin3_proj_define def = {0, 0, eq != NULL, at};
    odin3_status st = odin3_project_intern(proj, (odin3_bytes){text, name_len}, &def.name);
    if (st == ODIN3_OK && eq != NULL) {
        st = odin3_project_intern(proj, (odin3_bytes){eq + 1, word.len - name_len - 1}, &def.value);
    }
    odin3_proj_define *slot = st == ODIN3_OK ? odin3_vec_push(&proj->defines) : NULL;
    if (slot != NULL) {
        *slot = def;
    }
    return st != ODIN3_OK || slot != NULL ? st : ODIN3_ERR_NO_MEMORY;
}

/* IDENT or IDENT.IDENT. */
static bool top_name_ok(odin3_bytes name) {
    const uint8_t *text = name.ptr;
    const uint8_t *dot = name.len > 0 ? memchr(text, '.', name.len) : NULL;
    if (dot == NULL) {
        return is_ident(text, name.len);
    }
    size_t lib = (size_t)(dot - text);
    return is_ident(text, lib) && is_ident(dot + 1, name.len - lib - 1);
}

static odin3_status set_top(odin3_project *proj, odin3_bytes name, err_site site) {
    if (!top_name_ok(name)) {
        return fail_one(proj, site, (err_what){ODIN3_PROJ_ERR_SYNTAX, "bad top name"}, name);
    }
    if (proj->top != 0) {
        const char *old = odin3_strtab_get(odin3_design_strtab(proj->design), proj->top);
        msg_buf detail = {{0}, 0};
        msg_str(&detail, "top given twice (first ");
        msg_str(&detail, old);
        msg_str(&detail, ")");
        /* names [the second top] only, as the oracle's set_top */
        return fail_one(proj, site, (err_what){ODIN3_PROJ_ERR_SYNTAX, detail.text}, name);
    }
    return odin3_project_intern(proj, name, &proj->top);
}

odin3_status odin3_project_set_top(odin3_project *proj, odin3_bytes name, odin3_loc at) {
    odin3_status st = set_top(proj, name, (err_site){at, NULL});
    if (st == ODIN3_OK) {
        proj->top_at = at;
        proj->top_file = (odin3_proj_path){0, 0, {0}};
    }
    return st;
}

odin3_status odin3_project_set_top_file(odin3_project *proj, odin3_bytes name,
                                        const odin3_proj_path *file) {
    odin3_status st = set_top(proj, name, (err_site){{0}, file});
    if (st == ODIN3_OK) {
        proj->top_at = (odin3_loc){0};
        proj->top_file = *file;
    }
    return st;
}

static bool is_digit(uint8_t byte) {
    return byte >= '0' && byte <= '9';
}

static bool in_set(uint8_t byte, const char *set) {
    return byte != '\0' && strchr(set, byte) != NULL;
}

/* The length of the run of bytes from set starting at word[from]. */
static size_t run_of(odin3_bytes word, size_t from, const char *set) {
    const uint8_t *text = word.ptr;
    size_t j = from;
    while (j < word.len && in_set(text[j], set)) {
        j++;
    }
    return j - from;
}

bool odin3_proj_is_number(odin3_bytes word) {
    const uint8_t *text = word.ptr;
    size_t len = word.len;
    size_t j = len > 0 && text[0] == '-';
    if (j < len && is_digit(text[j]) && j + run_of(word, j, "0123456789_") == len) {
        return true; /* -?[0-9][0-9_]* */
    }
    j = len > 0 && is_digit(text[0]) ? run_of(word, 0, "0123456789_") : 0;
    if (j >= len || text[j] != '\'') {
        return false;
    }
    j++;
    j += run_of(word, j, "sS") > 0;
    if (j >= len || !in_set(text[j], "bBoOdDhH")) {
        return false;
    }
    size_t digits = run_of(word, j + 1, "0123456789abcdefABCDEFxXzZ_?");
    return digits > 0 && j + 1 + digits == len;
}

odin3_status odin3_project_count(odin3_project *proj, odin3_bytes text, odin3_loc at,
                                 uint32_t *out) {
    const uint8_t *digit = text.ptr;
    uint64_t value = 0;
    bool digits = text.len > 0;
    bool large = false;
    for (size_t i = 0; i < text.len && digits; i++) {
        digits = digit[i] >= '0' && digit[i] <= '9';
        if (!digits) {
            break; /* no arithmetic on a non-digit */
        }
        value = value * DECIMAL + (uint64_t)(digit[i] - '0');
        large = large || value > UINT32_MAX;
        value = large ? UINT32_MAX : value;
    }
    if (!digits) {
        return syntax_at(proj, at, "expected a count", text);
    }
    if (large) {
        return syntax_at(proj, at, "count too large", text);
    }
    *out = (uint32_t)value;
    return ODIN3_OK;
}

/* Appends one element of elem_size bytes copied from elem to vec. */
static odin3_status push_copy(odin3_vec *vec, const void *elem) {
    void *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(slot, elem, vec->elem_size);
    return ODIN3_OK;
}

odin3_status odin3_project_add_param(odin3_project *proj, const odin3_proj_param *param) {
    return push_copy(&proj->params, param);
}

odin3_status odin3_project_add_arch(odin3_project *proj, const odin3_proj_arch *arch) {
    return push_copy(&proj->arch, arch);
}

odin3_status odin3_project_add_inventory(odin3_project *proj, const odin3_proj_inventory *inv) {
    return push_copy(&proj->inventory, inv);
}

/* Reserves room for n more elements of vec (so the appends after it cannot fail). */
static odin3_status reserve_more(odin3_vec *vec, size_t more) {
    return more == 0 ? ODIN3_OK : odin3_vec_reserve(vec, vec->len + more);
}

odin3_status odin3_project_add_rule(odin3_project *proj, const odin3_proj_rule *rule,
                                    odin3_proj_ids cells) {
    odin3_proj_rule copy = *rule;
    bool map = rule->kind == ODIN3_PROJ_RULE_MAP;
    uint32_t count = map ? cells.n : 0;
    odin3_status st = reserve_more(&proj->rule_cells, count);
    st = st != ODIN3_OK ? st : reserve_more(&proj->rules, 1);
    if (st != ODIN3_OK) {
        return st;
    }
    if (map) {
        copy.u.map.cells = (uint32_t)proj->rule_cells.len;
        copy.u.map.ncells = count;
    }
    for (uint32_t i = 0; i < count; i++) {
        (void)push_id(&proj->rule_cells, cells.ids[i]);
    }
    return push_copy(&proj->rules, &copy);
}

odin3_status odin3_project_add_override(odin3_project *proj, const odin3_proj_override *ov,
                                        odin3_proj_kvs params) {
    odin3_proj_override copy = *ov;
    uint32_t count = ov->kind == ODIN3_PROJ_OVERRIDE_CELL ? params.n : 0;
    odin3_status st = reserve_more(&proj->override_params, count);
    st = st != ODIN3_OK ? st : reserve_more(&proj->overrides, 1);
    if (st != ODIN3_OK) {
        return st;
    }
    copy.params = (uint32_t)proj->override_params.len;
    copy.nparams = count;
    for (uint32_t i = 0; i < count; i++) {
        (void)push_copy(&proj->override_params, &params.kvs[i]);
    }
    return push_copy(&proj->overrides, &copy);
}

odin3_status odin3_project_add_ignored(odin3_project *proj, odin3_bytes name) {
    uint32_t str = 0;
    odin3_status st = odin3_project_intern(proj, name, &str);
    return st != ODIN3_OK ? st : push_id(&proj->ignored, str);
}

/* --- finish -------------------------------------------------------------------------------- */

typedef struct sort_item {
    const char *bytes;
    size_t len;
    uint32_t str;
} sort_item;

/* Byte order, a prefix first (Python's order on str for UTF-8 text). */
static int compare_items(const void *lhs, const void *rhs) {
    const sort_item *left = lhs;
    const sort_item *right = rhs;
    size_t common = left->len < right->len ? left->len : right->len;
    int order = common > 0 ? memcmp(left->bytes, right->bytes, common) : 0;
    if (order != 0) {
        return order;
    }
    return (left->len > right->len) - (left->len < right->len);
}

/* Sorts the strtab IDs of vec by their bytes and drops repeats (in place). */
static odin3_status sort_unique(const odin3_project *proj, odin3_vec *vec) {
    if (vec->len < 2) {
        return ODIN3_OK;
    }
    sort_item *items = odin3_util_malloc(vec->len * sizeof *items);
    if (items == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_strtab *tab = odin3_design_strtab(proj->design);
    uint32_t *ids = vec->data;
    for (size_t i = 0; i < vec->len; i++) {
        items[i] =
            (sort_item){odin3_strtab_get(tab, ids[i]), odin3_strtab_len(tab, ids[i]), ids[i]};
    }
    qsort(items, vec->len, sizeof *items, compare_items);
    size_t count = vec->len;
    odin3_vec_clear(vec);
    for (size_t i = 0; i < count; i++) {
        if (i == 0 || items[i].str != items[i - 1].str) {
            (void)push_id(vec, items[i].str); /* within the capacity it had */
        }
    }
    odin3_util_free(items);
    return ODIN3_OK;
}

odin3_status odin3_project_finish(odin3_project *proj) {
    odin3_status st = sort_unique(proj, &proj->ignored);
    st = st != ODIN3_OK ? st : sort_unique(proj, &proj->duplicates);
    if (st == ODIN3_OK) {
        proj->finished = true;
    }
    return st;
}
