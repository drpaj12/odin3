/*
 * read_verilog.c — the read_verilog pass (2C spec §4, VP-18/VP-19): its arguments (the flags
 * --parse-only, and 2D's --nolatches, --nomem2reg and --auto-blackbox, EL-3) and file list
 * (direct paths or the project record), the reader shared with odin3-parse-check (steps 1–7:
 * preprocess and parse every file into one store, libraries through read_verilog_lib.c,
 * duplicate_module, unnamed instances, seal and check) and the hand-over of step 8 to 2D's
 * odin3_elab_run.
 */
#include "frontends/verilog/read_verilog.h"
#include "frontends/name_resolve.h"

#include "api/sv.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/elab.h"
#include "frontends/project/paths.h"
#include "frontends/project/project.h"
#include "frontends/verilog/elab.h"
#include "frontends/verilog/lexer_support.h"
#include "frontends/verilog/parser.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog_test.h"
#include "frontends/verilog/soft_blocks.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/clock.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Debug builds check the sealed store whatever the options say (§4.2 step 7). */
#ifdef NDEBUG
enum { RV_ALWAYS_CHECK = 0 };
#else
enum { RV_ALWAYS_CHECK = 1 };
#endif

/* getcwd's buffer: first try and largest (a working directory deeper than 1 MB is refused). */
enum { CWD_BUF = 4096, CWD_MAX = 1 << 20 };

/* duplicate_module keys: library << 32 | name. */
enum { RV_LIBRARY_SHIFT = 32 };

/* The library name of a file given directly to the pass (the record's default library). */
static const char k_work[] = "work";

static odin3_rv_test_hook g_hook;
static void *g_hook_user;

void odin3_rv_test_set_hook(odin3_rv_test_hook fn, void *user) {
    g_hook = fn;
    g_hook_user = user;
}

static const char *text_of(const odin3_design *design, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

/* The design's finished project record; NULL when there is none or it was never finished. */
static const odin3_project *finished_record(const odin3_design *design) {
    const odin3_project *proj = odin3_design_project(design);
    return proj != NULL && proj->finished ? proj : NULL;
}

/* --- the run (steps 1, 2) ------------------------------------------------------------------- */

/* The preprocessor's configuration from the record (PP-1): defines in project order, resolved
 * include directories; empty without a record. */
typedef struct rv_config {
    odin3_vec defines; /* odin3_pp_define */
    odin3_vec incdirs; /* const char *, borrowed from the strtab */
} rv_config;

static odin3_status fill_config(const odin3_design *design, const odin3_project *proj,
                                rv_config *config) {
    for (size_t i = 0; proj != NULL && i < proj->defines.len; i++) {
        const odin3_proj_define *def = odin3_vec_cat(&proj->defines, i);
        odin3_pp_define *slot = odin3_vec_push(&config->defines);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (odin3_pp_define){odin3_bytes_cstr(text_of(design, def->name)),
                                  odin3_bytes_cstr(text_of(design, def->value)), def->has_value};
    }
    for (size_t i = 0; proj != NULL && i < proj->incdirs.len; i++) {
        const odin3_proj_path *dir = odin3_vec_cat(&proj->incdirs, i);
        const char **slot = (const char **)odin3_vec_push(&config->incdirs);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = text_of(design, dir->resolved);
    }
    return ODIN3_OK;
}

static odin3_status create_pp(odin3_rv *rv) {
    rv_config config;
    odin3_vec_init(&config.defines, sizeof(odin3_pp_define));
    odin3_vec_init(&config.incdirs, sizeof(const char *));
    odin3_status st = fill_config(rv->design, rv->proj, &config);
    if (st == ODIN3_OK) {
        odin3_pp_config pc = {config.defines.data, (uint32_t)config.defines.len,
                              (const char *const *)config.incdirs.data,
                              (uint32_t)config.incdirs.len};
        st = odin3_pp_create(rv->design, &pc, &rv->pp);
    }
    odin3_vec_free(&config.incdirs);
    odin3_vec_free(&config.defines);
    return st;
}

static void rv_close(odin3_rv *rv) {
    odin3_pp_destroy(rv->pp);
    odin3_u64map_destroy(rv->defined);
    odin3_u64map_destroy(rv->wanted_set);
    odin3_u64map_destroy(rv->libread);
    odin3_vec_free(&rv->wanted);
    odin3_ast_destroy(rv->ast);
    memset(rv, 0, sizeof *rv);
}

static odin3_status rv_open(odin3_rv *rv, odin3_design *design, const odin3_rv_opts *opts) {
    memset(rv, 0, sizeof *rv);
    rv->design = design;
    rv->proj = finished_record(design);
    rv->swept = 1;
    odin3_vl_run_init(&rv->vl, design);
    rv->vl.tokens = opts->tokens;
    odin3_vec_init(&rv->wanted, sizeof(odin3_rv_want));
    rv->defined = odin3_u64map_create(0);
    rv->wanted_set = odin3_u64map_create(0);
    rv->libread = odin3_u64map_create(0);
    if (rv->defined == NULL || rv->wanted_set == NULL || rv->libread == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_ast_create(design, ODIN3_AST_FORM_PARSED, opts->run, &rv->ast);
    return st != ODIN3_OK ? st : create_pp(rv);
}

/* The run's error cap before a file (plan point (r)): the preprocessor logs its own error, so a
 * run already at the cap stops here with the stop line (at the loc that named the file, else
 * unlocated) instead of preprocessing a file whose error could not be replaced by it. True when
 * the run has stopped. */
static bool at_cap(odin3_rv *rv, odin3_loc at) {
    if (rv->vl.stop || rv->vl.errors < rv->vl.limits.max_errors) {
        return rv->vl.stop;
    }
    rv->vl.stop = true;
    if (at.v != 0) {
        odin3_diag(rv->design, ODIN3_LOG_ERROR, at, "too many errors; stopping");
    } else {
        odin3_log(ODIN3_LOG_ERROR, "too many errors; stopping");
    }
    return true;
}

/* Parses one preprocessed stream into the store: a UNIT, or the stream's located errors. */
static odin3_status parse_text(odin3_rv *rv, odin3_bytes text, odin3_srcbuf_id buffer,
                               uint32_t given) {
    odin3_vp_stream vs = {text, buffer, given};
    odin3_ast_id unit = {0};
    odin3_status st = odin3_verilog_parse_stream(&rv->vl, rv->ast, &vs, &unit);
    return st == ODIN3_ERR_PARSE ? ODIN3_OK : st;
}

odin3_status odin3_rv_parse_kept(odin3_rv *rv, const odin3_ast_stream *kept) {
    if (at_cap(rv, (odin3_loc){0})) {
        return ODIN3_OK;
    }
    rv->bytes += kept->text.len;
    return parse_text(rv, kept->text, kept->buffer, kept->given);
}

odin3_status odin3_rv_read(odin3_rv *rv, const odin3_pp_source *src) {
    if (at_cap(rv, src->at)) {
        return ODIN3_OK;
    }
    odin3_pp_stream stream;
    odin3_status st = odin3_pp_run(rv->pp, src, &stream);
    if (st == ODIN3_ERR_PARSE || st == ODIN3_ERR_IO) {
        rv->vl.errors++; /* the preprocessor logged it; it counts against the cap */
        return ODIN3_OK;
    }
    if (st != ODIN3_OK) {
        rv->vl.stop = true;
        return st;
    }
    rv->bytes += stream.text.len;
    if (rv->keep) {
        odin3_ast_stream kept = {src->given, src->resolved, src->library, stream.stream,
                                 stream.text};
        st = odin3_ast_keep_stream(rv->ast, &kept);
        if (st != ODIN3_OK) {
            rv->vl.stop = true;
            return st;
        }
    }
    return parse_text(rv, stream.text, stream.stream, src->given);
}

/* --- steps 4 and 5 ------------------------------------------------------------------------- */

/* The library of a design UNIT: its FILE buffer's, "work" when it has none, as 2D's lookup reads
 * it (DESIGN §4.0 "design units are identified by library and name"). A "work" never interned
 * is 0, which then no other unit can carry either. */
static uint32_t unit_library(odin3_rv *rv, odin3_ast_id unit) {
    odin3_srcman *sm = NULL;
    odin3_srcbuf_info info;
    if (odin3_design_get_srcman(rv->design, &sm) == ODIN3_OK &&
        odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, odin3_ast_loc(rv->ast, unit)),
                                 &info) &&
        info.library != 0) {
        return info.library;
    }
    uint32_t work = 0;
    (void)odin3_strtab_find(odin3_design_strtab(rv->design), odin3_bytes_cstr(k_work), &work);
    return work;
}

/* The module and UDP names one design UNIT defines: a (library, name) already in seen is
 * duplicate_module (M11, 2B §8), located at this second definition (its MODULE or UDP_DECL node)
 * and counted against the cap. One name in two libraries is two design units (2D EL-6). */
static odin3_status unit_duplicates(odin3_rv *rv, odin3_ast_id unit, odin3_u64map *seen) {
    odin3_ast_span items = odin3_ast_children(rv->ast, unit);
    uint64_t library = (uint64_t)unit_library(rv, unit) << RV_LIBRARY_SHIFT;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < items.n; i++) {
        odin3_ast_kind kind = odin3_ast_kind_of(rv->ast, items.ids[i]);
        if (kind != ODIN3_AST_MODULE && kind != ODIN3_AST_UDP_DECL) {
            continue;
        }
        uint32_t name = odin3_ast_name(rv->ast, items.ids[i]);
        if (!odin3_u64map_get(seen, library | name, NULL)) {
            st = odin3_u64map_put(seen, (odin3_kv){library | name, 1});
            continue;
        }
        odin3_vl_report(&rv->vl, ODIN3_LOG_ERROR, odin3_ast_loc(rv->ast, items.ids[i]),
                        "duplicate_module: %s '%s' is already defined",
                        kind == ODIN3_AST_MODULE ? "module" : "primitive",
                        text_of(rv->design, name));
    }
    return st;
}

/* Step 4 over the design UNITs (library UNITs may repeat a name: first definition wins). */
static odin3_status check_duplicates(odin3_rv *rv) {
    odin3_u64map *seen = odin3_u64map_create(0);
    if (seen == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t idx = 0; st == ODIN3_OK && idx < rv->ndesign; idx++) {
        st = unit_duplicates(rv, odin3_ast_root(rv->ast, idx), seen);
    }
    odin3_u64map_destroy(seen);
    return st;
}

/* The unnamed INSTANCEs of one INSTANTIATION, unless it instantiates a UDP of the store. */
static void check_instantiation(odin3_rv *rv, odin3_ast_id node) {
    uint32_t module = odin3_ast_name(rv->ast, node);
    uint64_t bits = 0;
    if (odin3_u64map_get(rv->defined, module, &bits) && (bits & ODIN3_RV_UDP) != 0) {
        return;
    }
    odin3_ast_span kids = odin3_ast_children(rv->ast, node);
    for (uint32_t i = 0; i < kids.n; i++) {
        if (kids.ids[i].v != 0 && odin3_ast_kind_of(rv->ast, kids.ids[i]) == ODIN3_AST_INSTANCE &&
            odin3_ast_name(rv->ast, kids.ids[i]) == 0) {
            odin3_vl_report(&rv->vl, ODIN3_LOG_ERROR, odin3_ast_loc(rv->ast, kids.ids[i]),
                            "instance of '%s' has no name (only gate and UDP instances may be "
                            "unnamed)",
                            text_of(rv->design, module));
        }
    }
}

/* Step 5 (2A rule 4): one sweep over the node IDs. */
static void check_unnamed(odin3_rv *rv) {
    uint32_t end = odin3_ast_node_end(rv->ast).v;
    for (uint32_t id = 1; id < end && !rv->vl.stop; id++) {
        if (odin3_ast_kind_of(rv->ast, (odin3_ast_id){id}) == ODIN3_AST_INSTANTIATION) {
            check_instantiation(rv, (odin3_ast_id){id});
        }
    }
}

/* --- step 7 -------------------------------------------------------------------------------- */

/* Seals and checks the store and logs the summary line. */
static odin3_status seal(odin3_rv *rv, double start) {
    odin3_status st = odin3_ast_finish(rv->ast);
    if (st == ODIN3_OK && g_hook != NULL) {
        g_hook(rv, g_hook_user);
    }
    if (st == ODIN3_OK && (RV_ALWAYS_CHECK || odin3_pass_get_options().check)) {
        st = odin3_ast_check(rv->ast, ODIN3_CHECK_FULL);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t units = odin3_ast_root_count(rv->ast);
    uint32_t from = odin3_ast_library_units_from(rv->ast);
    odin3_log(ODIN3_LOG_INFO,
              "read_verilog: %" PRIu32 " units (%" PRIu32 " library), %" PRIu32 " nodes, %" PRIu32
              " comments, %" PRIu64 " bytes, %.3f ms",
              units, from != 0 ? units - from : 0, odin3_ast_node_end(rv->ast).v - 1,
              odin3_ast_comment_count(rv->ast), rv->bytes, odin3_now_ms() - start);
    return ODIN3_OK;
}

/* Steps 2–6 after the run is open: the files, the libraries, the two checks, the verdict. */
static odin3_status read_all(odin3_rv *rv, const odin3_rv_opts *opts) {
    odin3_status st = ODIN3_OK;
    rv->keep = opts->keep_streams;
    for (uint32_t i = 0; st == ODIN3_OK && i < opts->nfiles && !rv->vl.stop; i++) {
        st = odin3_rv_read(rv, &opts->files[i]);
    }
    rv->keep = false;
    rv->ndesign = odin3_ast_root_count(rv->ast);
    st = st != ODIN3_OK ? st : odin3_rv_resolve_libraries(rv);
    st = st != ODIN3_OK ? st : check_duplicates(rv);
    if (st == ODIN3_OK) {
        check_unnamed(rv);
    }
    if (st == ODIN3_OK && (rv->vl.errors > 0 || rv->vl.stop)) {
        st = ODIN3_ERR_PARSE; /* §4.2 step 6, an unreadable file included (logged as such) */
    }
    return st;
}

odin3_status odin3_read_verilog_files(odin3_design *design, const odin3_rv_opts *opts,
                                      odin3_ast **out) {
    return odin3_read_verilog_files_soft(design, opts, 0, false, out);
}

odin3_status odin3_read_verilog_files_soft(odin3_design *design, const odin3_rv_opts *opts,
                                           uint32_t soft_mask, bool vtr_compat, odin3_ast **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (design == NULL || opts == NULL || out == NULL || opts->files == NULL || opts->nfiles == 0) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_read_verilog_files: NULL argument or no files");
        return ODIN3_ERR_INVALID_ARG;
    }
    double start = odin3_now_ms();
    odin3_rv rv;
    odin3_status st = rv_open(&rv, design, opts);
    rv.soft_mask = soft_mask;
    rv.vtr_compat = vtr_compat;
    st = st != ODIN3_OK ? st : read_all(&rv, opts);
    st = st != ODIN3_OK ? st : seal(&rv, start);
    if (st == ODIN3_OK) {
        *out = rv.ast;
        rv.ast = NULL;
    }
    rv_close(&rv);
    return st;
}

/* --- the pass: arguments and the file list ------------------------------------------------- */

/* read_verilog's arguments: the flags, the elaboration options they set (EL-3; the top stays 0
 * for odin3_elab_run's step 1) and the files (odin3_bytes views into the script). */
typedef struct rv_args {
    bool parse_only;
    bool lib; /* --lib: keep the modules as library units for later reads (2D-7b) */
    odin3_elab_opts elab;
    odin3_vec files;
    odin3_bytes name_map; /* -name-map's file (H-6), ptr NULL when not given */
} rv_args;

/* A flag word: true when it is one of the pass's flags (and sets it). */
static bool set_flag(odin3_bytes word, rv_args *out) {
    if (odin3_pass_arg_is(word, "--parse-only")) {
        out->parse_only = true;
    } else if (odin3_pass_arg_is(word, "--lib")) {
        out->lib = true;
    } else if (odin3_pass_arg_is(word, "--nolatches")) {
        out->elab.nolatches = true;
    } else if (odin3_pass_arg_is(word, "--nomem2reg")) {
        out->elab.nomem2reg = true;
    } else if (odin3_pass_arg_is(word, "--auto-blackbox")) {
        out->elab.auto_blackbox = true;
    } else if (odin3_pass_arg_is(word, "-icells") || odin3_pass_arg_is(word, "--icells")) {
        out->elab.icells = true;
    } else if (odin3_pass_arg_is(word, "-netlist") || odin3_pass_arg_is(word, "--netlist")) {
        out->elab.netlist = true;
    } else if (odin3_pass_arg_is(word, "--vtr-compat")) {
        out->elab.vtr_compat = true;
    } else {
        return false;
    }
    return true;
}

/* --lib reads and keeps; it elaborates nothing, so --auto-blackbox (a design read's option) has
 * nothing to act on and is refused rather than silently dropped. */
static odin3_status check_lib_args(const rv_args *args) {
    if (args->name_map.ptr != NULL && !args->elab.netlist) {
        odin3_log(ODIN3_LOG_ERROR, "read_verilog: -name-map is an option of -netlist");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (args->elab.icells && (args->lib || args->parse_only)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "read_verilog: -icells cannot be combined with %s (it is an option of the "
                  "design read)",
                  args->lib ? "--lib" : "--parse-only");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (args->elab.netlist && (args->lib || args->parse_only)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "read_verilog: -netlist cannot be combined with %s (it is an option of the "
                  "design read)",
                  args->lib ? "--lib" : "--parse-only");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (args->lib && args->elab.auto_blackbox) {
        odin3_log(ODIN3_LOG_ERROR,
                  "read_verilog: --lib cannot be combined with --auto-blackbox (it is an option of "
                  "the design read)");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* -name-map <file> (H-6): the one map of a -netlist read. */
static odin3_status take_name_map(odin3_bytes *rest, rv_args *out) {
    odin3_bytes file = odin3_pass_arg_next(rest);
    if (file.ptr == NULL || file.len == 0 || out->name_map.ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, file.ptr == NULL || file.len == 0
                                       ? "read_verilog: -name-map expects a file"
                                       : "read_verilog: -name-map given twice");
        return ODIN3_ERR_INVALID_ARG;
    }
    out->name_map = file;
    return ODIN3_OK;
}

/* A flag or -name-map <file>: *taken says the word was one. */
static odin3_status option_word(odin3_bytes word, odin3_bytes *rest, rv_args *out, bool *taken) {
    *taken = true;
    if (set_flag(word, out)) {
        return ODIN3_OK;
    }
    if (odin3_pass_arg_is(word, "-name-map") || odin3_pass_arg_is(word, "--name-map")) {
        return take_name_map(rest, out);
    }
    *taken = false;
    return ODIN3_OK;
}

static odin3_status parse_args(odin3_bytes args, rv_args *out) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool taken = false;
        odin3_status st = option_word(word, &args, out, &taken);
        if (st != ODIN3_OK) {
            return st;
        }
        if (taken) {
            continue;
        }
        if (word.len >= 2 && memcmp(word.ptr, "--", 2) == 0) {
            /* "option" is 2D Task 10's text; other passes say "unknown argument" (1D style). */
            odin3_log(ODIN3_LOG_ERROR, "read_verilog: unknown option '%.*s'", (int)word.len,
                      (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_bytes *slot = odin3_vec_push(&out->files);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = word;
    }
    return check_lib_args(out);
}

/* One file given directly: its path as given, resolved against the working directory. */
static odin3_status direct_source(odin3_design *design, const char *cwd, odin3_bytes word,
                                  odin3_pp_source *src) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_path_resolve((odin3_path_join){odin3_bytes_cstr(cwd), word}, &path);
    st = st != ODIN3_OK ? st : odin3_design_intern(design, word, &src->given);
    st = st != ODIN3_OK
             ? st
             : odin3_design_intern(design, (odin3_bytes){path.data, path.len}, &src->resolved);
    st = st != ODIN3_OK ? st : odin3_design_intern(design, odin3_bytes_cstr(k_work), &src->library);
    odin3_strbuf_free(&path);
    return st;
}

/* The working directory into buf (any depth: the buffer grows until getcwd fits). */
static odin3_status working_dir(odin3_strbuf *buf) {
    for (size_t cap = CWD_BUF; cap <= CWD_MAX; cap *= 2) {
        odin3_status st = odin3_strbuf_reserve(buf, cap);
        if (st != ODIN3_OK) {
            return st;
        }
        if (getcwd(buf->data, cap) != NULL) {
            buf->len = strlen(buf->data);
            return ODIN3_OK;
        }
        if (errno != ERANGE) {
            break;
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "read_verilog: cannot read the working directory");
    return ODIN3_ERR_IO;
}

/* True for a path whose extension names SystemVerilog (DESIGN §4.0: .sv and .svh). */
static bool is_sv_path(odin3_bytes word) {
    const char *text = word.ptr;
    return (word.len >= 3 && memcmp(text + word.len - 3, ".sv", 3) == 0) ||
           (word.len >= 4 && memcmp(text + word.len - 4, ".svh", 4) == 0);
}

static odin3_status direct_sources(odin3_design *design, const odin3_vec *words, odin3_vec *srcs,
                                   odin3_vec *svs) {
    odin3_strbuf cwd;
    odin3_strbuf_init(&cwd);
    odin3_status st = working_dir(&cwd);
    for (size_t i = 0; st == ODIN3_OK && i < words->len; i++) {
        odin3_bytes word = *(const odin3_bytes *)odin3_vec_cat(words, i);
        odin3_pp_source *src = odin3_vec_push(is_sv_path(word) ? svs : srcs);
        st = src == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (st == ODIN3_OK) {
            *src = (odin3_pp_source){0};
            st = direct_source(design, cwd.data, word, src);
        }
    }
    odin3_strbuf_free(&cwd);
    return st;
}

/* True for a project file a read must refuse: VHDL always, and SystemVerilog in a -v library
 * (the slang reader takes design files only for now). */
static bool unsupported(odin3_design *design, const odin3_proj_file *file, bool library) {
    bool sv_lib = library && file->lang == ODIN3_LANG_SYSTEMVERILOG;
    if (file->lang != ODIN3_LANG_VHDL && !sv_lib) {
        return false;
    }
    bool resolved = odin3_design_diag_paths(design) == ODIN3_SRCFMT_RESOLVED;
    odin3_diag(design, ODIN3_LOG_ERROR, file->path.at,
               "unsupported_language: no %s reader for a library file yet: '%s'",
               file->lang == ODIN3_LANG_VHDL ? "VHDL" : "SystemVerilog",
               text_of(design, resolved ? file->path.resolved : file->path.given));
    return true;
}

/* VHDL has no reader at all: unsupported_language at the declaration's word. */
static bool unsupported_vhdl(odin3_design *design, const odin3_proj_file *file) {
    if (file->lang != ODIN3_LANG_VHDL) {
        return false;
    }
    bool resolved = odin3_design_diag_paths(design) == ODIN3_SRCFMT_RESOLVED;
    odin3_diag(design, ODIN3_LOG_ERROR, file->path.at,
               "unsupported_language: no VHDL reader yet: '%s'",
               text_of(design, resolved ? file->path.resolved : file->path.given));
    return true;
}

/* One Verilog or SystemVerilog design file appended to srcs or svs; another language (a BLIF,
 * JSON or arch file) is not this pass's to read and adds nothing. */
static odin3_status split_source(const odin3_proj_file *file, odin3_vec *srcs, odin3_vec *svs) {
    if (file->lang != ODIN3_LANG_VERILOG && file->lang != ODIN3_LANG_SYSTEMVERILOG) {
        return ODIN3_OK;
    }
    odin3_vec *into = file->lang == ODIN3_LANG_SYSTEMVERILOG ? svs : srcs;
    odin3_pp_source *src = odin3_vec_push(into);
    if (src == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *src = (odin3_pp_source){file->path.given, file->path.resolved, file->library, file->path.at};
    return ODIN3_OK;
}

/* The record's design files in listing order (PRJ-16), split into Verilog (srcs) and
 * SystemVerilog (svs); VHDL and -v files of another language are unsupported_language. */
static odin3_status record_sources(odin3_design *design, const odin3_project *proj, odin3_vec *srcs,
                                   odin3_vec *svs) {
    uint32_t bad = 0;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < proj->files.len; i++) {
        const odin3_proj_file *file = odin3_vec_cat(&proj->files, i);
        bool vhdl = unsupported_vhdl(design, file);
        bad += vhdl ? 1U : 0U;
        if (!vhdl) {
            st = split_source(file, srcs, svs);
        }
    }
    for (size_t i = 0; i < proj->libfiles.len; i++) {
        bad += unsupported(design, odin3_vec_cat(&proj->libfiles, i), true) ? 1 : 0;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (bad > 0) {
        return ODIN3_ERR_PARSE;
    }
    if (srcs->len == 0 && svs->len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "read_verilog: no Verilog files");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status collect_sources(odin3_design *design, const rv_args *args, odin3_vec *srcs,
                                    odin3_vec *svs) {
    if (args->files.len > 0) {
        return direct_sources(design, &args->files, srcs, svs);
    }
    const odin3_project *proj = finished_record(design);
    if (proj == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "read_verilog: no project read");
        return ODIN3_ERR_INVALID_ARG;
    }
    return record_sources(design, proj, srcs, svs);
}

/* --- the pass: step 8 ------------------------------------------------------------------------ */

/* The flags of a --lib read on every root of its store (the carrier of later reads, 2D-7b). */
static odin3_status mark_library(odin3_ast *ast, const rv_args *args) {
    unsigned flags = ODIN3_UNIT_LIB;
    flags |= args->elab.nolatches ? ODIN3_UNIT_NOLATCHES : 0U;
    flags |= args->elab.nomem2reg ? ODIN3_UNIT_NOMEM2REG : 0U;
    flags |= args->elab.vtr_compat ? ODIN3_UNIT_VTR_COMPAT : 0U;
    return odin3_ast_mark_units(ast, 0, odin3_ast_root_count(ast), (uint8_t)flags);
}

/* Hands the store to the design; with --lib it stays held for later reads, with --parse-only it
 * stays held until the design is destroyed (AST-15), otherwise 2D's entry runs with the pass's
 * options (its step 3 drops the parsed store, kept under ALL, AST-16; the drop here is then a
 * no-op, plan point (a)). */
static odin3_status hand_over(odin3_pass_ctx *ctx, odin3_ast *ast, const rv_args *args) {
    odin3_status st = args->lib ? mark_library(ast, args) : ODIN3_OK;
    st = st != ODIN3_OK ? st : odin3_design_set_ast(ctx->design, ast);
    if (st != ODIN3_OK) {
        odin3_ast_destroy(ast);
        return st;
    }
    if (args->lib) {
        odin3_log(ODIN3_LOG_INFO,
                  "read_verilog --lib: %" PRIu32 " units kept as library units (elaborated where a "
                  "later read instantiates them)",
                  odin3_ast_root_count(ast));
        return ODIN3_OK;
    }
    if (args->parse_only) {
        return ODIN3_OK;
    }
    st = odin3_elab_run(ctx, ctx->design, ast, &args->elab);
    odin3_design_drop_ast(ctx->design, ctx->run, ODIN3_AST_FORM_PARSED);
    return st;
}

/* D18: the primitives the arch record has no model of, which --vtr-compat builds as soft logic
 * (soft_blocks.h); 0 outside --vtr-compat, --lib and --parse-only. */
static bool soft_read(const rv_args *args) {
    return args->elab.vtr_compat && !args->lib && !args->parse_only;
}

static uint32_t soft_blocks_of(const odin3_design *design, const rv_args *args) {
    return soft_read(args) ? odin3_soft_blocks_missing(design) : 0U;
}

/* -name-map: loads the map into names and points the elaboration options at it. */
static odin3_status open_names(rv_args *args, odin3_name_resolver *names) {
    if (args->name_map.ptr == NULL) {
        return ODIN3_OK;
    }
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_strbuf_append(&path, args->name_map);
    st = st != ODIN3_OK ? st : odin3_name_resolver_open(names, "read_verilog", path.data);
    args->elab.names = st == ODIN3_OK ? names : NULL;
    odin3_strbuf_free(&path);
    return st;
}

/* The top module name for a SystemVerilog read: the CLI's --top (it wins, DESIGN §4.0), else the
 * project's top; NULL when neither names one (the adapter then requires exactly one). */
static const char *sv_top_name(const odin3_design *design) {
    odin3_pass_options opts = odin3_pass_get_options();
    if (opts.top != NULL && opts.top[0] != '\0') {
        return opts.top;
    }
    const odin3_project *proj = finished_record(design);
    if (proj != NULL && proj->finished && proj->top != 0) {
        return text_of(design, proj->top);
    }
    return NULL;
}

/* The SystemVerilog read's options: the project's include directories and defines (so a
 * project's `include and `ifdef reach slang as they reach the Verilog preprocessor) and the
 * elaboration options. The two arrays are allocated for the call and returned through dirs/defs
 * for the caller to free. */
static odin3_status sv_options_of(const odin3_design *design, const rv_args *args,
                                  const char ***dirs, odin3_sv_define **defs,
                                  odin3_sv_options *opts) {
    *dirs = NULL;
    *defs = NULL;
    memset(opts, 0, sizeof *opts);
    opts->elab.nolatches = args->elab.nolatches;
    opts->elab.nomem2reg = args->elab.nomem2reg;
    opts->elab.auto_blackbox = args->elab.auto_blackbox;
    const odin3_project *proj = finished_record(design);
    if (proj == NULL) {
        return ODIN3_OK;
    }
    if (proj->incdirs.len > 0) {
        const char **dir_list =
            (const char **)odin3_util_malloc(proj->incdirs.len * sizeof *dir_list);
        if (dir_list == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (size_t i = 0; i < proj->incdirs.len; i++) {
            const odin3_proj_path *dir = odin3_vec_cat(&proj->incdirs, i);
            dir_list[i] = text_of(design, dir->resolved);
        }
        *dirs = dir_list;
        opts->incdirs = dir_list;
        opts->nincdirs = (uint32_t)proj->incdirs.len;
    }
    if (proj->defines.len > 0) {
        odin3_sv_define *def_list =
            (odin3_sv_define *)odin3_util_malloc(proj->defines.len * sizeof *def_list);
        if (def_list == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (size_t i = 0; i < proj->defines.len; i++) {
            const odin3_proj_define *def = odin3_vec_cat(&proj->defines, i);
            def_list[i].name = text_of(design, def->name);
            def_list[i].value = def->has_value ? text_of(design, def->value) : "";
            def_list[i].has_value = def->has_value;
            def_list[i].reserved_pad = 0;
        }
        *defs = def_list;
        opts->defines = def_list;
        opts->ndefines = (uint32_t)proj->defines.len;
    }
    return ODIN3_OK;
}

/* The SystemVerilog branch of the read: the registered reader (the slang adapter, CLI start-up)
 * builds and elaborates the store itself, in this pass's run. */
static odin3_status read_sv(odin3_design *design, odin3_pass_ctx *ctx, const rv_args *args,
                            const odin3_vec *svs) {
    if (args->parse_only || args->lib) {
        odin3_log(ODIN3_LOG_ERROR, "read_verilog: %s is not supported for SystemVerilog yet",
                  args->parse_only ? "--parse-only" : "--lib");
        return ODIN3_ERR_INVALID_ARG;
    }
    /* -icells, -netlist and --vtr-compat shape the Verilog front end; refusing them keeps the
     * read honest instead of ignoring them. */
    if (args->elab.icells || args->elab.netlist || args->elab.vtr_compat) {
        const char *flag = "--vtr-compat";
        if (args->elab.netlist) {
            flag = "-netlist";
        } else if (args->elab.icells) {
            flag = "-icells";
        }
        odin3_log(ODIN3_LOG_ERROR, "read_verilog: %s is not supported for SystemVerilog yet", flag);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_sv_read_fn read = odin3_sv_reader_get();
    if (read == NULL) {
        odin3_log(ODIN3_LOG_ERROR,
                  "read_verilog: no SystemVerilog reader registered (the slang adapter)");
        return ODIN3_ERR_INVALID_ARG;
    }
    const char **paths = (const char **)odin3_util_malloc(svs->len * sizeof *paths);
    if (paths == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < svs->len; i++) {
        const odin3_pp_source *src = odin3_vec_cat(svs, i);
        paths[i] = text_of(design, src->resolved);
    }
    const char **dirs = NULL;
    odin3_sv_define *defs = NULL;
    odin3_sv_options opts;
    odin3_status st = sv_options_of(design, args, &dirs, &defs, &opts);
    if (st == ODIN3_OK) {
        opts.paths = paths;
        opts.npaths = (uint32_t)svs->len;
        opts.top = sv_top_name(design);
        st = read(design, ctx->run.v, &opts);
    }
    odin3_util_free(defs);
    odin3_util_free((void *)dirs);
    odin3_util_free((void *)paths);
    return st;
}

static odin3_status read_verilog_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                     void *user) {
    (void)user;
    rv_args parsed = {.parse_only = false, .elab = {0}};
    odin3_vec_init(&parsed.files, sizeof(odin3_bytes));
    odin3_vec srcs;
    odin3_vec_init(&srcs, sizeof(odin3_pp_source));
    odin3_vec svs;
    odin3_vec_init(&svs, sizeof(odin3_pp_source));
    odin3_status st = parse_args(args, &parsed);
    st = st != ODIN3_OK ? st : collect_sources(design, &parsed, &srcs, &svs);
    if (st == ODIN3_OK && svs.len > 0 && srcs.len > 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "read_verilog: a mixed Verilog and SystemVerilog read is not supported yet");
        st = ODIN3_ERR_PARSE;
    }
    odin3_ast *ast = NULL;
    odin3_name_resolver names;
    if (st == ODIN3_OK && svs.len > 0) {
        st = read_sv(design, ctx, &parsed, &svs);
    } else if (st == ODIN3_OK) {
        const bool vtr = soft_read(&parsed);
        uint32_t soft = soft_blocks_of(design, &parsed);
        st = open_names(&parsed, &names);
        if (st == ODIN3_OK) {
            odin3_rv_opts opts = {srcs.data, (uint32_t)srcs.len, ctx->run, NULL, parsed.lib};
            st = odin3_read_verilog_files_soft(design, &opts, soft, vtr, &ast);
        }
        st = st != ODIN3_OK ? st : hand_over(ctx, ast, &parsed);
        if (parsed.elab.names != NULL) {
            odin3_name_resolver_close(&names);
        }
    }
    odin3_vec_free(&svs);
    odin3_vec_free(&srcs);
    odin3_vec_free(&parsed.files);
    return st;
}

const odin3_pass_def odin3_read_verilog_pass = {
    "read_verilog",
    "read_verilog [--parse-only] [--lib] [--nolatches] [--nomem2reg] [--auto-blackbox] "
    "[--vtr-compat] [-icells] [-netlist [-name-map <file>]] [<file>...]: "
    "read "
    "Verilog-2005 (the project's Verilog files, or the files given) into one parsed AST with -v/-y "
    "libraries (the project's SystemVerilog files, or files ending in .sv/.svh, go through the "
    "registered SystemVerilog reader, the slang adapter); --parse-only keeps it (write_verilog_ast "
    "prints it), otherwise elaboration (2D) "
    "follows: --nolatches makes unassigned paths of combinational blocks x, --nomem2reg keeps "
    "memories unless forced, --auto-blackbox makes an undefined module a black box, --vtr-compat "
    "reads a non-constant `initial x = e;` as `assign x = e;` as Parmys does (a warning each); "
    "-icells (or --icells) reads an instance of a `$` cell type as that registered cell, as Yosys "
    "does; -netlist (or --netlist) says the file is a netlist written by write_verilog or Yosys: "
    "its records are IMPORTED, a `(* src = \"file:line\" *)` attribute, a trailing `// file:line` "
    "comment on an assign and a `\\hier/short@file:line ` net name restore HDL locations; "
    "-name-map "
    "(with -netlist) first replaces each hash net name the map (write_verilog --name-style hash) "
    "holds by its full name; --lib "
    "keeps "
    "the modules as library units for later reads (elaborated, under this read's --nolatches, "
    "--nomem2reg and --vtr-compat, only where a later design instantiates them)",
    read_verilog_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
