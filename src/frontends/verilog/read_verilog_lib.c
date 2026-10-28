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
#include "frontends/verilog/elab1.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "frontends/verilog/soft_blocks.h"
#include "ir/celltype.h"
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
 * earlier library definition already has stays in the store, with an info line, and changes
 * nothing about the name. */
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
    /* UDP describes the definition that wins (the first: design units are swept before any
     * library file), never an ignored one, so step 5 asks the right question. */
    uint64_t bits =
        old | (library ? ODIN3_RV_LIBRARY : ODIN3_RV_DESIGN) | (!had && udp ? ODIN3_RV_UDP : 0);
    return odin3_u64map_put(rv->defined, (odin3_kv){name, bits});
}

/* Whether the `#()` of INSTANTIATION node fits the registered cell type of that name; true when
 * no cell type has the name (nothing the instance could fail to fit, #262). */
static bool fits_cell(const odin3_rv *rv, odin3_ast_id node, uint32_t name) {
    odin3_celltype_id id;
    if (!odin3_celltype_find(rv->design, name, &id)) {
        return true;
    }
    return odin3_elab1_params_fit(rv->ast, rv->design, node, odin3_celltype_get(rv->design, id));
}

/* An INSTANTIATION: its module name is wanted (once, at its first instantiation), with whether
 * any instance's `#()` does not fit the registered cell type of that name. */
static odin3_status add_wanted(odin3_rv *rv, odin3_ast_id node) {
    uint32_t name = odin3_ast_name(rv->ast, node);
    uint64_t index = 0;
    if (odin3_u64map_get(rv->wanted_set, name, &index)) {
        odin3_rv_want *want = odin3_vec_at(&rv->wanted, (size_t)(index - 1));
        want->overflows_cell = want->overflows_cell || !fits_cell(rv, node, name);
        return ODIN3_OK;
    }
    odin3_rv_want *slot = odin3_vec_push(&rv->wanted);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_rv_want){name, odin3_ast_loc(rv->ast, node), !fits_cell(rv, node, name)};
    odin3_status st = odin3_u64map_put(rv->wanted_set, (odin3_kv){name, (uint64_t)rv->wanted.len});
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

/* The file name a library soft body carries (its `given` path). It starts with `<odin3>/`, a form
 * like `<o3lib>` that is no path a design names (a relative `lib/soft_primitives/<name>.v` is a
 * legal user path, and a user file there at the same line would get the same locations and hash
 * names). `<` and `>` need no escape in a name map field (only TAB, LF, `\`, `%`, `,`, `|` do),
 * in the location key (file id and line), in the JSON, Verilog comment and Python parsers. */
#define SOFT_STABLE_FMT "<odin3>/soft_primitives/%s.v"

/* The source of primitive name's body in dir, for read_library: the library "work", the file read
 * from dir (resolved) but named "<odin3>/soft_primitives/<name>.v" (given), the name every
 * location, diagnostic and hash name carries. dir depends on the build (<exe>/../../lib, the
 * install tree, $ODIN3_SOFT_PRIMITIVES_DIR); a name made from it would differ between two builds of
 * one commit, so no object of the body could be joined across builds. */
static odin3_status soft_source(odin3_rv *rv, const char *name, const char *dir, odin3_loc at,
                                odin3_pp_source *src) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    *src = (odin3_pp_source){0, 0, 0, at};
    odin3_status st = odin3_strbuf_appendf(&path, "%s/%s.v", dir, name);
    st = st != ODIN3_OK ? st
                        : odin3_design_intern(rv->design, odin3_bytes_cstr("work"), &src->library);
    st = st != ODIN3_OK
             ? st
             : odin3_design_intern(rv->design, (odin3_bytes){path.data, path.len}, &src->resolved);
    odin3_strbuf_free(&path);
    odin3_strbuf stable;
    odin3_strbuf_init(&stable);
    st = st != ODIN3_OK ? st : odin3_strbuf_appendf(&stable, SOFT_STABLE_FMT, name);
    st = st != ODIN3_OK
             ? st
             : odin3_design_intern(rv->design, (odin3_bytes){stable.data, stable.len}, &src->given);
    odin3_strbuf_free(&stable);
    return st;
}

/* The D18 warning at the first instance of a primitive the architecture has no model of (the
 * #262 path has none here: odin3_elab1_prefer_unit warns at each instance that does not fit). */
static void soft_warn(odin3_rv *rv, const odin3_rv_want *want, const char *name, bool missing) {
    if (missing) {
        odin3_vl_report(&rv->vl, ODIN3_LOG_WARN, want->at,
                        "the architecture has no '%s' model; --vtr-compat builds this instance as "
                        "soft logic, as Parmys does",
                        name);
    }
}

/* Reads a soft body from src; the #262 body (missing false) is marked a library unit so
 * odin3_elab1_prefer_unit keeps the hard cell for the instances whose `#()` fits it. */
static odin3_status read_soft_body(odin3_rv *rv, const odin3_pp_source *src, bool missing) {
    uint32_t first = odin3_ast_root_count(rv->ast);
    odin3_status st = read_library(rv, src);
    return st == ODIN3_OK && !missing
               ? odin3_ast_mark_units(rv->ast, first, odin3_ast_root_count(rv->ast),
                                      ODIN3_UNIT_LIB | ODIN3_UNIT_VTR_COMPAT)
               : st;
}

/* D18 (soft_blocks.h): an instance of a primitive the arch has no model of, with --vtr-compat,
 * is built from lib/soft_primitives/<name>.v, read as a library unit; the instance is warned.
 * #262: the same body is read when the arch has the model but an instance's `#()` does not fit the
 * hard cell (an `adder #(W)` on the 1-bit adder): the cell cannot take the instance, and the soft
 * body is the parameterized form Parmys supplies. The body is then marked a library unit so
 * odin3_elab1_prefer_unit keeps the cell for the instances that do fit it. */
static odin3_status read_soft_block(odin3_rv *rv, const odin3_rv_want *want) {
    const char *name = text_of(rv, want->name);
    char dir[SOFT_DIR_CAP];
    char tried[SOFT_DIR_CAP];
    const bool found = odin3_soft_blocks_dir(dir, tried);
    for (uint32_t i = 0; i < SOFT_BLOCK_COUNT; i++) {
        const bool missing = ((rv->soft_mask >> i) & 1U) != 0;
        const bool overflows = rv->vtr_compat && want->overflows_cell;
        if ((!missing && !overflows) || strcmp(name, odin3_soft_block_name(i)) != 0) {
            continue;
        }
        if (!found) {
            odin3_vl_report(
                &rv->vl, ODIN3_LOG_ERROR, want->at,
                "--vtr-compat needs the soft body of '%s' (%s) but no soft_primitives directory "
                "was found; tried %s (set ODIN3_SOFT_PRIMITIVES_DIR)",
                name,
                missing ? "the architecture has no such model"
                        : "its parameters do not fit the hard cell",
                tried);
            return ODIN3_OK; /* counted as an error by the run: the read fails */
        }
        odin3_pp_source src;
        odin3_status st = soft_source(rv, name, dir, want->at, &src);
        if (st == ODIN3_OK) {
            soft_warn(rv, want, name, missing);
            st = read_soft_body(rv, &src, missing);
        }
        return st;
    }
    return ODIN3_OK;
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

/* --- the units kept by `read_verilog --lib` (2D-7b) ----------------------------------------- */

/* One kept stream: parsed again as library UNITs that carry the store's flags, from the
 * preprocessed text its own read produced (its macros and include directories; I1 of the PR
 * review: a re-preprocess under this read's macros is a different unit). A file already read is
 * skipped. */
static odin3_status parse_kept_stream(odin3_rv *rv, const odin3_ast *kept,
                                      const odin3_ast_stream *stream) {
    if (odin3_u64map_get(rv->libread, stream->resolved, NULL)) {
        return ODIN3_OK;
    }
    uint32_t before = odin3_ast_root_count(rv->ast);
    odin3_status st = odin3_u64map_put(rv->libread, (odin3_kv){stream->resolved, 1});
    st = st != ODIN3_OK ? st : odin3_rv_parse_kept(rv, stream);
    st = st != ODIN3_OK ? st : sweep(rv, true);
    return st != ODIN3_OK ? st
                          : odin3_ast_mark_units(rv->ast, before, odin3_ast_root_count(rv->ast),
                                                 odin3_ast_unit_flags(kept, 0));
}

/* One kept store: every design file of its read not parsed yet, in read order (the library UNITs
 * of a --lib read's own -v/-y files are not kept: a later read finds those itself). */
static odin3_status read_kept_store(odin3_rv *rv, const odin3_ast *kept) {
    odin3_status st = ODIN3_OK;
    const odin3_ast_stream *stream = NULL;
    for (uint32_t idx = 0;
         st == ODIN3_OK && !rv->vl.stop && (stream = odin3_ast_stream_at(kept, idx)) != NULL;
         idx++) {
        st = parse_kept_stream(rv, kept, stream);
    }
    return st;
}

/* Every store an earlier `read_verilog --lib` kept, in read order (a later --lib read sees them
 * too: harmless, nothing is elaborated, and its own kept UNITs are only its design UNITs). */
static odin3_status read_kept_libraries(odin3_rv *rv) {
    odin3_status st = ODIN3_OK;
    uint32_t count = odin3_design_ast_count(rv->design);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        const odin3_ast *kept = odin3_design_ast_at(rv->design, i);
        if (kept != NULL && (odin3_ast_unit_flags(kept, 0) & ODIN3_UNIT_LIB) != 0) {
            st = read_kept_store(rv, kept);
        }
    }
    return st;
}

/* --- step 3 ------------------------------------------------------------------------------- */

/* One wanted module no unit defines yet: the record's -v files, the -y directories, then (D18)
 * the soft body of a primitive the arch lacks. */
static odin3_status resolve_want(odin3_rv *rv, const odin3_rv_want *want) {
    odin3_status st = rv->libfiles_read ? ODIN3_OK : read_libfiles(rv);
    if (st != ODIN3_OK || is_defined(rv, want->name)) {
        return st;
    }
    st = search_dirs(rv, want);
    return st != ODIN3_OK || is_defined(rv, want->name) ? st : read_soft_block(rv, want);
}

odin3_status odin3_rv_resolve_libraries(odin3_rv *rv) {
    odin3_status st = sweep(rv, false);
    uint32_t first = odin3_ast_root_count(rv->ast);
    st = st != ODIN3_OK ? st : read_kept_libraries(rv);
    for (size_t i = 0; st == ODIN3_OK && !rv->vl.stop && i < rv->wanted.len; i++) {
        odin3_rv_want want = *(const odin3_rv_want *)odin3_vec_cat(&rv->wanted, i);
        if (is_defined(rv, want.name)) {
            continue;
        }
        st = resolve_want(rv, &want);
    }
    if (st == ODIN3_OK && first > 0 && odin3_ast_root_count(rv->ast) > first) {
        odin3_ast_set_library_units_from(rv->ast, first);
    }
    return st;
}
