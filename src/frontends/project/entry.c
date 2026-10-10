/*
 * entry.c — read_project's core (PRJ-16): the entry and its format, the reader, --top, finish and
 * the info lines; a failed read drops the record.
 */
#include "frontends/project/project.h"

#include "frontends/project/paths.h"
#include "ir/design.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum { CWD_FIRST = 256, CWD_MAX = 1 << 20 };

static bool bytes_is(odin3_bytes bytes, const char *text) {
    size_t len = strlen(text);
    return bytes.len == len && (len == 0 || memcmp(bytes.ptr, text, len) == 0);
}

static odin3_bytes str_bytes(const odin3_project *proj, uint32_t str) {
    const odin3_strtab *tab = odin3_design_strtab(proj->design);
    return (odin3_bytes){odin3_strtab_get(tab, str), odin3_strtab_len(tab, str)};
}

static const char *str_text(const odin3_project *proj, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(proj->design), str);
}

/* An unlocated error ("odin3: error: …") naming name (none when NULL). */
static odin3_status unlocated(odin3_project *proj, odin3_proj_err_kind kind, const char *detail,
                              const odin3_bytes *name) {
    odin3_proj_err err = {kind, ODIN3_PROJ_UNLOCATED, {0}, NULL, detail, name, name != NULL};
    return odin3_project_fail(proj, &err);
}

/* --- the entry ----------------------------------------------------------------------------- */

/* Appends the working directory to out. IO (logged) when it cannot be read. */
static odin3_status append_cwd(odin3_strbuf *out) {
    for (size_t size = CWD_FIRST; size <= CWD_MAX; size *= 2) {
        char *buf = odin3_util_malloc(size);
        if (buf == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        bool got = getcwd(buf, size) != NULL;
        int err = errno;
        odin3_status res = got ? odin3_strbuf_append(out, odin3_bytes_cstr(buf)) : ODIN3_OK;
        odin3_util_free(buf);
        if (got || err != ERANGE) {
            if (!got) {
                odin3_log(ODIN3_LOG_ERROR, "odin3: error: cannot read the working directory: %s",
                          strerror(err));
            }
            return got ? res : ODIN3_ERR_IO;
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "odin3: error: cannot read the working directory: %s",
              strerror(ERANGE));
    return ODIN3_ERR_IO;
}

/* The entry made absolute against the working directory and normalized (point (i)). */
static odin3_status entry_path(odin3_bytes given, odin3_strbuf *out) {
    const char *text = given.ptr;
    if (given.len > 0 && text[0] == '/') {
        return odin3_path_normalize(given, out);
    }
    odin3_strbuf cwd;
    odin3_strbuf_init(&cwd);
    odin3_status res = append_cwd(&cwd);
    if (res == ODIN3_OK) {
        res = odin3_path_resolve((odin3_path_join){{cwd.data, cwd.len}, given}, out);
    }
    odin3_strbuf_free(&cwd);
    return res;
}

/* The format of an entry by its (case-sensitive) suffix; a .qip is no entry. */
static odin3_status format_of(odin3_project *proj, const odin3_project_request *req,
                              odin3_bytes resolved, uint8_t *format) {
    odin3_bytes given = odin3_bytes_cstr(req->entry);
    odin3_bytes suffix = odin3_path_suffix(resolved);
    if (bytes_is(suffix, ".qip")) {
        return unlocated(proj, ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE, "a .qip is not a project entry",
                         &given);
    }
    *format = ODIN3_PROJ_FORMAT_FLIST;
    if (bytes_is(suffix, ".o3proj")) {
        *format = ODIN3_PROJ_FORMAT_O3PROJ;
    } else if (bytes_is(suffix, ".qpf") || bytes_is(suffix, ".qsf")) {
        *format = ODIN3_PROJ_FORMAT_QUARTUS;
    } else if (bytes_is(suffix, ".xml")) {
        *format = ODIN3_PROJ_FORMAT_ODIN2;
    }
    return ODIN3_OK;
}

/* The entry must be a regular file; its format must have a reader; a revision needs Quartus. */
static odin3_status check_entry(odin3_project *proj, const odin3_project_request *req,
                                odin3_bytes resolved, uint8_t *format) {
    odin3_bytes given = odin3_bytes_cstr(req->entry);
    if (odin3_path_stat(resolved.ptr) != ODIN3_PATH_FILE) {
        return unlocated(proj, ODIN3_PROJ_ERR_MISSING_FILE, "project not found", &given);
    }
    *format = req->format;
    odin3_status res = *format == 0 ? format_of(proj, req, resolved, format) : ODIN3_OK;
    if (res == ODIN3_OK && *format == ODIN3_PROJ_FORMAT_ODIN2) {
        return unlocated(proj, ODIN3_PROJ_ERR_UNKNOWN_FILE_TYPE, "no Odin II XML reader yet",
                         &given);
    }
    if (res == ODIN3_OK && req->revision != NULL && *format != ODIN3_PROJ_FORMAT_QUARTUS) {
        return unlocated(proj, ODIN3_PROJ_ERR_SYNTAX, "--revision needs a .qpf or .qsf entry",
                         NULL);
    }
    return res;
}

/* Resolves and checks the entry, then records it, its format and the revision. */
static odin3_status set_entry(odin3_project *proj, const odin3_project_request *req) {
    odin3_bytes given = odin3_bytes_cstr(req->entry);
    odin3_strbuf resolved;
    odin3_strbuf_init(&resolved);
    uint8_t format = 0;
    odin3_status res = entry_path(given, &resolved);
    odin3_bytes path = {resolved.data, resolved.len};
    res = res != ODIN3_OK ? res : check_entry(proj, req, path, &format);
    odin3_proj_path entry = {0, 0, {0}};
    res = res != ODIN3_OK ? res : odin3_project_intern(proj, given, &entry.given);
    res = res != ODIN3_OK ? res : odin3_project_intern(proj, path, &entry.resolved);
    uint32_t revision = 0;
    if (res == ODIN3_OK && req->revision != NULL) {
        res = odin3_project_intern(proj, odin3_bytes_cstr(req->revision), &revision);
    }
    if (res == ODIN3_OK) {
        proj->entry = entry;
        proj->format = format;
        proj->revision = revision;
        proj->has_revision = req->revision != NULL;
    }
    odin3_strbuf_free(&resolved);
    return res;
}

/* --- reading ------------------------------------------------------------------------------- */

static odin3_status run_reader(odin3_project *proj, const odin3_project_request *req) {
    switch (proj->format) {
    case ODIN3_PROJ_FORMAT_O3PROJ:
        return odin3_read_o3proj(proj, &proj->entry);
    case ODIN3_PROJ_FORMAT_QUARTUS: {
        odin3_quartus_req quartus = {{"", 0}, req->revision != NULL, {0}};
        if (quartus.has_revision) {
            quartus.revision = odin3_bytes_cstr(req->revision);
        }
        return odin3_read_quartus(proj, &proj->entry, &quartus);
    }
    default:
        return odin3_read_flist(proj, &proj->entry);
    }
}

/* --top replaces the project's top (§4.4, §12 #1), with an info line when they differ. */
static odin3_status apply_top(odin3_project *proj, const odin3_project_request *req) {
    if (req->top == NULL) {
        return ODIN3_OK;
    }
    odin3_bytes name = odin3_bytes_cstr(req->top);
    uint32_t old = proj->top;
    odin3_bytes old_name = old != 0 ? str_bytes(proj, old) : (odin3_bytes){"", 0};
    if (old != 0 && old_name.len == name.len && memcmp(old_name.ptr, name.ptr, name.len) == 0) {
        return ODIN3_OK; /* the same top: it keeps its declaring line */
    }
    proj->top = 0;
    odin3_status res = odin3_project_set_top(proj, name, (odin3_loc){0});
    if (res == ODIN3_OK && old != 0) {
        odin3_log(ODIN3_LOG_INFO, "%s: --top %s replaces the project top %s",
                  str_text(proj, proj->entry.given), req->top, str_text(proj, old));
    }
    return res;
}

/* "<entry>: <what>: A, B, …" when ids is not empty. */
static odin3_status info_line(const odin3_project *proj, const odin3_vec *ids, const char *what) {
    if (ids->len == 0) {
        return ODIN3_OK;
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status res = ODIN3_OK;
    for (size_t i = 0; res == ODIN3_OK && i < ids->len; i++) {
        res = i > 0 ? odin3_strbuf_append(&text, odin3_bytes_cstr(", ")) : ODIN3_OK;
        uint32_t str = *(const uint32_t *)odin3_vec_cat(ids, i);
        res = res != ODIN3_OK ? res : odin3_strbuf_append(&text, str_bytes(proj, str));
    }
    if (res == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "%s: %s: %s", str_text(proj, proj->entry.given), what, text.data);
    }
    odin3_strbuf_free(&text);
    return res;
}

static odin3_status read_entry(odin3_project *proj, const odin3_project_request *req) {
    odin3_status res = set_entry(proj, req);
    res = res != ODIN3_OK ? res : run_reader(proj, req);
    res = res != ODIN3_OK ? res : apply_top(proj, req);
    res = res != ODIN3_OK ? res : odin3_project_finish(proj);
    res = res != ODIN3_OK ? res : info_line(proj, &proj->ignored, "ignored");
    return res != ODIN3_OK ? res
                           : info_line(proj, &proj->duplicates, "listed more than once, read once");
}

odin3_status odin3_project_read(odin3_design *design, const odin3_project_request *req) {
    if (design == NULL || req == NULL || req->entry == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_project *proj = NULL;
    odin3_status res = odin3_design_get_project(design, &proj);
    if (res != ODIN3_OK) {
        return res;
    }
    if (proj->finished) { /* point (e): misuse, the record read first stays */
        odin3_log(ODIN3_LOG_ERROR, "odin3: error: project already read from %s",
                  str_text(proj, proj->entry.given));
        return ODIN3_ERR_INVALID_ARG;
    }
    res = read_entry(proj, req);
    if (res != ODIN3_OK) {
        odin3_design_drop_project(design);
    }
    return res;
}
