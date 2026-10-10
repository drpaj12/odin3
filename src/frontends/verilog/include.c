/* include.c — `include (PP-11): the file name, the search, cycles, caps and the include stack. */
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/verilog/preproc_internal.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char k_not_quoted[] = "syntax: `include takes a double-quoted file name";

/* The "..." file name at the cursor (spaces and tabs before it), without its quotes, as a view
 * into the file's bytes; the cursor ends after the closing quote. */
static odin3_status file_name(odin3_pp *pp, odin3_bytes *name) {
    odin3_pp_skip_spaces(pp);
    odin3_loc where = odin3_pp_loc_here(pp);
    const odin3_pp_textrun *run = odin3_pp_cur_run(pp);
    if (run == NULL || odin3_pp_peek(pp, 0) != '"') {
        return odin3_pp_error(pp, where, "%s", k_not_quoted);
    }
    uint32_t start = odin3_pp_top(pp)->cur.offset + 1;
    uint32_t end = start;
    while (end < run->len && run->bytes[end] != '"' && run->bytes[end] != '\n') {
        end++;
    }
    if (end >= run->len || run->bytes[end] != '"') {
        return odin3_pp_error(pp, where, "syntax: unterminated file name of `include");
    }
    *name = (odin3_bytes){run->bytes + start, end - start};
    if (name->len == 0 || memchr(name->ptr, '\0', name->len) != NULL) {
        return odin3_pp_error(pp, where, "syntax: `include file name is empty or holds a NUL");
    }
    odin3_pp_advance(pp, end - start + 2);
    return ODIN3_OK;
}

/* ODIN3_PP_MAX_INCLUDE_DEPTH and ODIN3_PP_MAX_INCLUDES_PER_STREAM, before any search. The
 * include stack holds the stream's own file, so its depth is len - 1. */
static odin3_status include_caps(odin3_pp *pp, odin3_loc at) {
    if (pp->incstack.len - 1 >= pp->limits.include_depth) {
        odin3_pp_cap cap = {"include depth", "ODIN3_PP_MAX_INCLUDE_DEPTH",
                            pp->limits.include_depth};
        return odin3_pp_cap_error(pp, at, cap);
    }
    if (pp->nincludes >= pp->limits.includes_per_stream) {
        odin3_pp_cap cap = {"files", "ODIN3_PP_MAX_INCLUDES_PER_STREAM",
                            pp->limits.includes_per_stream};
        return odin3_pp_cap_error(pp, at, cap);
    }
    return ODIN3_OK;
}

/* Tries base_dir/name (name alone when absolute or base_dir is empty); *found when it is a
 * regular file, its normalized path then in path. */
static odin3_status try_candidate(odin3_bytes base_dir, odin3_bytes name, odin3_strbuf *path,
                                  bool *found) {
    odin3_status st = odin3_path_resolve((odin3_path_join){base_dir, name}, path);
    if (st != ODIN3_OK) {
        return st;
    }
    *found = odin3_path_stat(path->data) == ODIN3_PATH_FILE;
    return ODIN3_OK;
}

/* PP-11's search: an absolute name stands; else the includer's directory, then each incdir in
 * order; the first regular file wins. *found false when none is. */
static odin3_status search(const odin3_pp *pp, odin3_bytes name, odin3_strbuf *path, bool *found) {
    *found = false;
    if (((const char *)name.ptr)[0] == '/') {
        return try_candidate((odin3_bytes){NULL, 0}, name, path, found);
    }
    const odin3_pp_frame *frame = odin3_vec_cat(&pp->frames, pp->frames.len - 1);
    const char *includer = odin3_strtab_get(pp->strtab, frame->resolved);
    odin3_status st = try_candidate(odin3_path_dir(odin3_bytes_cstr(includer)), name, path, found);
    for (uint32_t i = 0; i < pp->nincdirs && st == ODIN3_OK && !*found; i++) {
        st = try_candidate(odin3_bytes_cstr(pp->incdirs[i]), name, path, found);
    }
    return st;
}

static bool on_include_stack(const odin3_pp *pp, uint32_t resolved) {
    for (size_t i = 0; i < pp->incstack.len; i++) {
        if (*(const uint32_t *)odin3_vec_cat(&pp->incstack, i) == resolved) {
            return true;
        }
    }
    return false;
}

/* Finds name (PP-11), refusing a miss and a cycle, and stores its resolved path's ID. */
static odin3_status resolve(odin3_pp *pp, odin3_bytes name, odin3_loc at, uint32_t *resolved) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    bool found = false;
    odin3_status st = search(pp, name, &path, &found);
    if (st == ODIN3_OK && !found) {
        st = odin3_pp_error(pp, at, "include_not_found: '%.*s'", (int)name.len,
                            (const char *)name.ptr);
    }
    if (st == ODIN3_OK) {
        st = odin3_design_intern(pp->design, (odin3_bytes){path.data, path.len}, resolved);
    }
    odin3_strbuf_free(&path);
    if (st == ODIN3_OK && on_include_stack(pp, *resolved)) {
        st = odin3_pp_error(pp, at, "include_cycle: '%.*s'", (int)name.len, (const char *)name.ptr);
    }
    return st;
}

odin3_status odin3_pp_include_directive(odin3_pp *pp, odin3_loc at) {
    odin3_bytes name = {NULL, 0};
    odin3_status st = file_name(pp, &name);
    if (st == ODIN3_OK) {
        st = include_caps(pp, at);
    }
    uint32_t resolved = 0;
    if (st == ODIN3_OK) {
        st = resolve(pp, name, at, &resolved);
    }
    uint32_t given = 0;
    if (st == ODIN3_OK) {
        st = odin3_design_intern(pp->design, name, &given);
    }
    uint32_t cache = 0;
    if (st == ODIN3_OK) {
        odin3_pp_load load = {given, resolved, at};
        st = odin3_pp_cache_load(pp, &load, &cache);
    }
    if (st == ODIN3_OK && odin3_vec_reserve(&pp->incstack, pp->incstack.len + 1) != ODIN3_OK) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t library = odin3_pp_top(pp)->library;
    odin3_pp_open open = {cache, given, library, at, at};
    odin3_srcbuf_id buf = {0};
    st = odin3_pp_open_file(pp, &open, &buf);
    if (st == ODIN3_OK) {
        *(uint32_t *)odin3_vec_push(&pp->incstack) = resolved;
        pp->gap = true;
    }
    return st;
}

odin3_status odin3_pp_include_end(odin3_pp *pp) {
    odin3_vec_pop(&pp->incstack);
    pp->gap = true;
    return ODIN3_OK;
}
