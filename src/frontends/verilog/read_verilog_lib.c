/*
 * read_verilog_lib.c — library resolution for read_verilog (2C spec §4.2 step 3, DESIGN §4.0,
 * spec §10 point 3): the names the UNITs define and instantiate, every -v file read at once on
 * the first undefined name (Verilog-XL: first file wins), then <dir>/<name><ext> over the -y
 * directories and libexts per undefined name, until nothing more resolves.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/project/paths.h"
#include "frontends/project/project.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The library extension when the record names none (DESIGN §4.0). */
static const char k_default_ext[] = ".v";

static const char *text_of(const odin3_rv *rv, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(rv->design), str);
}

/* --- the name sets -------------------------------------------------------------------------- */

/* A MODULE or UDP_DECL: its name is defined. A library definition of a name a design unit or an
 * earlier library definition already has stays in the store, with an info line. */
static odin3_status define(odin3_rv *rv, odin3_ast_id node, bool library) {
    uint32_t name = odin3_ast_name(rv->ast, node);
    bool udp = odin3_ast_kind_of(rv->ast, node) == ODIN3_AST_UDP_DECL;
    uint64_t old = 0;
    bool had = odin3_u64map_get(rv->defined, name, &old);
    if (had && library) {
        odin3_vl_report(&rv->vl, ODIN3_LOG_INFO, odin3_ast_loc(rv->ast, node),
                        "library %s '%s' ignored: %s", udp ? "primitive" : "module",
                        text_of(rv, name),
                        (old & ODIN3_RV_DESIGN) != 0 ? "a design unit defines it"
                                                     : "an earlier library definition wins");
    }
    uint64_t bits = old | (library ? ODIN3_RV_LIBRARY : ODIN3_RV_DESIGN) | (udp ? ODIN3_RV_UDP : 0);
    return odin3_u64map_put(rv->defined, (odin3_kv){name, bits});
}

/* An INSTANTIATION: its module name is wanted (once, at its first instantiation). */
static odin3_status add_wanted(odin3_rv *rv, odin3_ast_id node) {
    uint32_t name = odin3_ast_name(rv->ast, node);
    if (odin3_u64map_get(rv->wanted_set, name, NULL)) {
        return ODIN3_OK;
    }
    odin3_rv_want *slot = odin3_vec_push(&rv->wanted);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_rv_want){name, odin3_ast_loc(rv->ast, node)};
    odin3_status st = odin3_u64map_put(rv->wanted_set, (odin3_kv){name, 1});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&rv->wanted);
    }
    return st;
}

/* The nodes made since the last sweep: definitions and instantiations (one ID sweep). */
static odin3_status sweep(odin3_rv *rv, bool library) {
    uint32_t end = odin3_ast_node_end(rv->ast).v;
    odin3_status st = ODIN3_OK;
    for (uint32_t id = rv->swept; st == ODIN3_OK && id < end; id++) {
        odin3_ast_id node = {id};
        odin3_ast_kind kind = odin3_ast_kind_of(rv->ast, node);
        if (kind == ODIN3_AST_MODULE || kind == ODIN3_AST_UDP_DECL) {
            st = define(rv, node, library);
        } else if (kind == ODIN3_AST_INSTANTIATION) {
            st = add_wanted(rv, node);
        }
        rv->swept = st == ODIN3_OK ? id + 1 : id;
    }
    return st;
}

static bool is_defined(const odin3_rv *rv, uint32_t name) {
    return odin3_u64map_get(rv->defined, name, NULL);
}

/* --- reading library files ------------------------------------------------------------------ */

/* One library file: read (odin3_rv_read) and swept; its resolved path remembered. */
static odin3_status read_library(odin3_rv *rv, const odin3_pp_source *src) {
    odin3_status st = odin3_u64map_put(rv->libread, (odin3_kv){src->resolved, 1});
    st = st != ODIN3_OK ? st : odin3_rv_read(rv, src);
    return st != ODIN3_OK ? st : sweep(rv, true);
}

/* Every Verilog -v file of the record, in order (all its modules become available at once). */
static odin3_status read_libfiles(odin3_rv *rv) {
    rv->libfiles_read = true;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && rv->proj != NULL && i < rv->proj->libfiles.len; i++) {
        const odin3_proj_file *file = odin3_vec_cat(&rv->proj->libfiles, i);
        if (file->lang == ODIN3_LANG_VERILOG &&
            !odin3_u64map_get(rv->libread, file->path.resolved, NULL)) {
            odin3_pp_source src = {file->path.given, file->path.resolved, file->library,
                                   file->path.at};
            st = read_library(rv, &src);
        }
    }
    return st;
}

/* A -y candidate: a directory, an extension and the name looked for. */
typedef struct lib_probe {
    const odin3_proj_path *dir;
    const char *ext;
    const odin3_rv_want *want;
} lib_probe;

/* Interns <base>/<name><ext> into *out. */
static odin3_status probe_path(odin3_rv *rv, const lib_probe *probe, uint32_t base, uint32_t *out) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_strbuf_appendf(&path, "%s/%s%s", text_of(rv, base),
                                           text_of(rv, probe->want->name), probe->ext);
    st = st != ODIN3_OK ? st
                        : odin3_design_intern(rv->design, (odin3_bytes){path.data, path.len}, out);
    odin3_strbuf_free(&path);
    return st;
}

/* Reads the candidate when it is a regular file not read yet; *found when it is a file. */
static odin3_status try_probe(odin3_rv *rv, const lib_probe *probe, bool *found) {
    odin3_pp_source src = {0, 0, 0, probe->want->at};
    odin3_status st = probe_path(rv, probe, probe->dir->resolved, &src.resolved);
    if (st != ODIN3_OK || odin3_path_stat(text_of(rv, src.resolved)) != ODIN3_PATH_FILE) {
        return st;
    }
    *found = true;
    if (odin3_u64map_get(rv->libread, src.resolved, NULL)) {
        return ODIN3_OK;
    }
    st = probe_path(rv, probe, probe->dir->given, &src.given);
    return st != ODIN3_OK ? st : read_library(rv, &src);
}

/* The libext list of the record, or the default ".v": entry idx (NULL past the end). */
static const char *ext_at(const odin3_rv *rv, size_t idx) {
    size_t count = rv->proj->libext.len;
    if (count == 0) {
        return idx == 0 ? k_default_ext : NULL;
    }
    return idx < count ? text_of(rv, *(const uint32_t *)odin3_vec_cat(&rv->proj->libext, idx))
                       : NULL;
}

/* <dir>/<name><ext> over the libdirs in order and the libexts in order: the first regular file
 * is read. A name holding '/' is never looked up (it would name another directory). */
static odin3_status search_dirs(odin3_rv *rv, const odin3_rv_want *want) {
    if (rv->proj == NULL || strchr(text_of(rv, want->name), '/') != NULL) {
        return ODIN3_OK;
    }
    bool found = false;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && !found && i < rv->proj->libdirs.len; i++) {
        lib_probe probe = {odin3_vec_cat(&rv->proj->libdirs, i), NULL, want};
        for (size_t j = 0; st == ODIN3_OK && !found && (probe.ext = ext_at(rv, j)) != NULL; j++) {
            st = try_probe(rv, &probe, &found);
        }
    }
    return st;
}

/* --- step 3 ------------------------------------------------------------------------------- */

odin3_status odin3_rv_resolve_libraries(odin3_rv *rv) {
    odin3_status st = sweep(rv, false);
    uint32_t first = odin3_ast_root_count(rv->ast);
    for (size_t i = 0; st == ODIN3_OK && !rv->vl.stop && i < rv->wanted.len; i++) {
        odin3_rv_want want = *(const odin3_rv_want *)odin3_vec_cat(&rv->wanted, i);
        if (is_defined(rv, want.name)) {
            continue;
        }
        if (!rv->libfiles_read) {
            st = read_libfiles(rv);
            if (st != ODIN3_OK || is_defined(rv, want.name)) {
                continue;
            }
        }
        st = search_dirs(rv, &want);
    }
    if (st == ODIN3_OK && first > 0 && odin3_ast_root_count(rv->ast) > first) {
        odin3_ast_set_library_units_from(rv->ast, first);
    }
    return st;
}
