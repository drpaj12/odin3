/*
 * project_pass.c — the read_project, dump_project and preprocess passes (2B PRJ-16, PRJ-17):
 * project input through the pass manager, the record as JSON, and the preprocessor's streams with
 * their segment maps (the debug seam before 2C's parser exists).
 */
#include "ast/srcfmt.h"
#include "ast/srcman.h"
#include "frontends/project/project.h"
#include "frontends/verilog/preproc.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

/* The mode --out creates its directory with (before the umask). */
enum { OUT_DIR_MODE = 0777 };

/* The first byte JSON writes as itself (below it: \u00XX). */
enum { JSON_FIRST_PLAIN = 0x20 };

/* --- shared helpers ------------------------------------------------------------------------ */

static odin3_status unknown_arg(const char *pass, odin3_bytes word) {
    odin3_log(ODIN3_LOG_ERROR, "%s: unknown argument '%.*s'", pass, (int)word.len,
              (const char *)word.ptr);
    return ODIN3_ERR_INVALID_ARG;
}

/* The design's finished project record; NULL (logged) when no read_project filled one (a record
 * nothing finished is dropped, as a failed read_project drops its own). */
static const odin3_project *finished_project(odin3_design *design, const char *pass) {
    const odin3_project *proj = odin3_design_project(design);
    if (proj == NULL || !proj->finished) {
        odin3_design_drop_project(design);
        odin3_log(ODIN3_LOG_ERROR, "%s: no project was read (run read_project first)", pass);
        return NULL;
    }
    return proj;
}

static const char *text_of(const odin3_design *design, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

/* Writes text to stdout; IO (logged) when it cannot. */
static odin3_status write_stdout(const char *pass, const odin3_strbuf *text) {
    if ((text->len > 0 && fwrite(text->data, 1, text->len, stdout) != text->len) ||
        fflush(stdout) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot write to stdout", pass);
        return ODIN3_ERR_IO;
    }
    return ODIN3_OK;
}

/* --- read_project -------------------------------------------------------------------------- */

/* read_project's arguments; a ptr NULL means not given. */
typedef struct read_args {
    odin3_bytes entry;
    uint8_t format; /* odin3_proj_format; 0: from the entry's suffix */
    odin3_bytes revision;
} read_args;

/* --format names, in odin3_proj_format order. */
static const char *const k_format_names[] = {"o3proj", "f", "qsf", "odin2"};

static odin3_status parse_format(odin3_bytes word, uint8_t *format) {
    for (size_t i = 0; word.ptr != NULL && i < sizeof k_format_names / sizeof k_format_names[0];
         i++) {
        if (odin3_pass_arg_is(word, k_format_names[i])) {
            *format = (uint8_t)(ODIN3_PROJ_FORMAT_O3PROJ + i);
            return ODIN3_OK;
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "read_project: --format expects o3proj, f, qsf or odin2");
    return ODIN3_ERR_INVALID_ARG;
}

/* One word of read_project's arguments (an option takes its value from *rest). */
static odin3_status read_arg(odin3_bytes word, odin3_bytes *rest, read_args *out, bool *extra) {
    if (odin3_pass_arg_is(word, "--format")) {
        return parse_format(odin3_pass_arg_next(rest), &out->format);
    }
    if (odin3_pass_arg_is(word, "--revision")) {
        out->revision = odin3_pass_arg_next(rest);
        if (out->revision.ptr == NULL) {
            odin3_log(ODIN3_LOG_ERROR, "read_project: --revision expects a revision name");
            return ODIN3_ERR_INVALID_ARG;
        }
        return ODIN3_OK;
    }
    if (word.len >= 2 && ((const char *)word.ptr)[0] == '-' && ((const char *)word.ptr)[1] == '-') {
        return unknown_arg("read_project", word);
    }
    *extra |= out->entry.ptr != NULL;
    out->entry = word;
    return ODIN3_OK;
}

static odin3_status parse_read_args(odin3_bytes args, read_args *out) {
    bool extra = false;
    odin3_status st = ODIN3_OK;
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        st = read_arg(word, &args, out, &extra);
    }
    if (st == ODIN3_OK && (extra || out->entry.len == 0)) {
        odin3_log(ODIN3_LOG_ERROR, "read_project: expects one entry file");
        st = ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

/* Fills the design's project record (odin3_project_read) with the option's top and revision; a
 * --revision argument wins over the option. Runs no front end; a flow is stored, not run. */
static odin3_status read_project_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                     void *user) {
    (void)ctx;
    (void)user;
    read_args parsed = {0};
    odin3_status st = parse_read_args(args, &parsed);
    odin3_strbuf entry;
    odin3_strbuf revision;
    odin3_strbuf_init(&entry);
    odin3_strbuf_init(&revision);
    st = st != ODIN3_OK ? st : odin3_strbuf_append(&entry, parsed.entry);
    if (st == ODIN3_OK && parsed.revision.ptr != NULL) {
        st = odin3_strbuf_append(&revision, parsed.revision);
    }
    if (st == ODIN3_OK) {
        odin3_pass_options opts = odin3_pass_get_options();
        odin3_project_request req = {entry.data, parsed.format, NULL, opts.top};
        if (parsed.revision.ptr != NULL) {
            req.revision = revision.data != NULL ? revision.data : "";
        } else if (opts.has_revision) {
            req.revision = opts.revision != NULL ? opts.revision : "";
        }
        st = odin3_project_read(design, &req);
    }
    odin3_strbuf_free(&revision);
    odin3_strbuf_free(&entry);
    return st;
}

/* --- dump_project -------------------------------------------------------------------------- */

/* Writes the record (PRJ-4) to stdout, or atomically to the one path given. */
static odin3_status dump_project_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                     void *user) {
    (void)ctx;
    (void)user;
    odin3_bytes path = odin3_pass_arg_next(&args);
    if (odin3_pass_arg_next(&args).ptr != NULL || (path.ptr != NULL && path.len == 0)) {
        odin3_log(ODIN3_LOG_ERROR, "dump_project: expects at most one path");
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_project *proj = finished_project(design, "dump_project");
    if (proj == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf text;
    odin3_strbuf name;
    odin3_strbuf_init(&text);
    odin3_strbuf_init(&name);
    odin3_status st = odin3_project_dump(proj, &text);
    if (st == ODIN3_OK && path.ptr != NULL) {
        st = odin3_strbuf_append(&name, path);
        st = st != ODIN3_OK ? st : odin3_pass_write_text(name.data, &text);
    } else if (st == ODIN3_OK) {
        st = write_stdout("dump_project", &text);
    }
    odin3_strbuf_free(&name);
    odin3_strbuf_free(&text);
    return st;
}

/* --- preprocess: arguments and configuration ----------------------------------------------- */

/* preprocess's arguments. */
typedef struct pp_args {
    odin3_bytes out; /* --out <dir>; ptr NULL: stdout */
    bool segments;   /* --segments */
} pp_args;

static odin3_status parse_pp_args(odin3_bytes args, pp_args *out) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "--segments")) {
            out->segments = true;
        } else if (!odin3_pass_arg_is(word, "--out")) {
            return unknown_arg("preprocess", word);
        } else {
            out->out = odin3_pass_arg_next(&args);
            if (out->out.len == 0) {
                odin3_log(ODIN3_LOG_ERROR, "preprocess: --out expects a directory");
                return ODIN3_ERR_INVALID_ARG;
            }
        }
    }
    if (out->segments && out->out.ptr == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "preprocess: --segments needs --out <dir>");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* The preprocessor's configuration from the record: defines in project order and the resolved
 * include directories (PP-1); the include search order itself is fixed (PP-11: the includer's
 * directory, then these; never the working directory). */
typedef struct pp_setup {
    odin3_vec defines; /* odin3_pp_define */
    odin3_vec incdirs; /* const char *, borrowed from the strtab */
} pp_setup;

static odin3_status fill_setup(const odin3_design *design, const odin3_project *proj,
                               pp_setup *setup) {
    for (size_t i = 0; i < proj->defines.len; i++) {
        const odin3_proj_define *def = odin3_vec_cat(&proj->defines, i);
        odin3_pp_define *slot = odin3_vec_push(&setup->defines);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (odin3_pp_define){odin3_bytes_cstr(text_of(design, def->name)),
                                  odin3_bytes_cstr(text_of(design, def->value)), def->has_value};
    }
    for (size_t i = 0; i < proj->incdirs.len; i++) {
        const odin3_proj_path *dir = odin3_vec_cat(&proj->incdirs, i);
        const char **slot = (const char **)odin3_vec_push(&setup->incdirs);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = text_of(design, dir->resolved);
    }
    return ODIN3_OK;
}

/* --- preprocess: the segment map as JSON ----------------------------------------------------- */

/* A JSON string: '"' and '\\' escaped, control bytes as \u00XX, other bytes as they are. */
static odin3_status append_json_string(odin3_strbuf *out, const char *text) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (const char *at = text; st == ODIN3_OK && *at != '\0'; at++) {
        unsigned char byte = (unsigned char)*at;
        if (byte == '"' || byte == '\\') {
            st = odin3_strbuf_appendf(out, "\\%c", *at);
        } else if (byte < JSON_FIRST_PLAIN) {
            st = odin3_strbuf_appendf(out, "\\u%04x", (unsigned)byte);
        } else {
            st = odin3_strbuf_append(out, (odin3_bytes){at, 1});
        }
    }
    return st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("\""));
}

/* JSON for a strtab name, or null. */
static odin3_status append_name(odin3_strbuf *out, const odin3_srcman *sm, uint32_t str,
                                bool present) {
    return present ? append_json_string(out, odin3_strtab_get(sm->strtab, str))
                   : odin3_strbuf_append(out, odin3_bytes_cstr("null"));
}

/* Names of odin3_srcbuf_kind values (index = kind). */
static const char *const k_kind_names[] = {"none", "file", "expansion", "macro_arg", "scratch"};

/* One segment's fields after out_offset: its file location (file, line, col, the file named in
 * style), the kind of the buffer the segment points into, and that buffer's macro; all null for
 * a loc-0 segment. */
static odin3_status append_where(odin3_strbuf *out, const odin3_srcman *sm, odin3_loc loc,
                                 odin3_srcfmt style) {
    odin3_srcpos pos;
    odin3_srcbuf_info file;
    odin3_srcbuf_info own;
    if (!odin3_srcman_decode(sm, loc, &pos) ||
        !odin3_srcman_buffer_info(sm, (odin3_srcbuf_id){pos.buffer}, &file) ||
        !odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, loc), &own) ||
        own.kind >= sizeof k_kind_names / sizeof k_kind_names[0]) {
        return odin3_strbuf_append(out, odin3_bytes_cstr("\"file\": null, \"line\": null, \"col\": "
                                                         "null, \"buffer_kind\": null, \"macro\": "
                                                         "null}"));
    }
    bool resolved = style == ODIN3_SRCFMT_RESOLVED && file.resolved != 0;
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\"file\": "));
    st = st != ODIN3_OK ? st : append_name(out, sm, resolved ? file.resolved : file.name, true);
    if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(out,
                                  ", \"line\": %" PRIu32 ", \"col\": %" PRIu32
                                  ", \"buffer_kind\": \"%s\", \"macro\": ",
                                  pos.line, pos.col, k_kind_names[own.kind]);
    }
    bool macro = own.kind == ODIN3_SRCBUF_EXPANSION || own.kind == ODIN3_SRCBUF_MACRO_ARG;
    st = st != ODIN3_OK ? st : append_name(out, sm, own.name, macro);
    return st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("}"));
}

/* The stream's segment map: [{out_offset, file, line, col, buffer_kind, macro}], one per line. */
static odin3_status segments_json(const odin3_srcman *sm, odin3_srcbuf_id stream,
                                  odin3_srcfmt style, odin3_strbuf *out) {
    odin3_srcbuf_info info;
    if (!odin3_srcman_buffer_info(sm, stream, &info)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_srcfile_rec *file =
        odin3_srcman_file_of(sm, odin3_srcman_rec_of(sm, (odin3_loc){info.start}));
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("["));
    for (size_t i = 0; st == ODIN3_OK && i < file->segs.len; i++) {
        const odin3_srcseg_rec *seg = odin3_vec_cat(&file->segs, i);
        st = odin3_strbuf_appendf(out, "%s\n  {\"out_offset\": %" PRIu32 ", ", i > 0 ? "," : "",
                                  seg->out_offset);
        st = st != ODIN3_OK ? st : append_where(out, sm, (odin3_loc){seg->loc}, style);
    }
    return st != ODIN3_OK ? st : odin3_strbuf_append(out, odin3_bytes_cstr("\n]\n"));
}

/* --- preprocess: output -------------------------------------------------------------------- */

/* Where preprocess writes: the design, its source manager and the parsed arguments. */
typedef struct pp_out {
    odin3_design *design;
    const odin3_srcman *sm;
    pp_args args;
} pp_out;

/* Writes text to <dir>/<file>. */
static odin3_status write_in_dir(const pp_out *out, const char *file, const odin3_strbuf *text) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_strbuf_appendf(&path, "%.*s/%s", (int)out->args.out.len,
                                           (const char *)out->args.out.ptr, file);
    st = st != ODIN3_OK ? st : odin3_pass_write_text(path.data, text);
    odin3_strbuf_free(&path);
    return st;
}

/* Stream n: its text to stdout or <dir>/<n>.v, and with --segments <dir>/<n>.segments.json. */
static odin3_status emit_stream(const pp_out *out, uint32_t n, const odin3_pp_stream *stream) {
    odin3_strbuf text = {(char *)stream->text.ptr, stream->text.len, stream->text.len};
    if (out->args.out.ptr == NULL) {
        return write_stdout("preprocess", &text);
    }
    char name[sizeof "4294967295.segments.json"];
    (void)snprintf(name, sizeof name, "%" PRIu32 ".v", n);
    odin3_status st = write_in_dir(out, name, &text);
    if (st == ODIN3_OK && out->args.segments) {
        odin3_strbuf json;
        odin3_strbuf_init(&json);
        (void)snprintf(name, sizeof name, "%" PRIu32 ".segments.json", n);
        st = segments_json(out->sm, stream->stream, odin3_design_diag_paths(out->design), &json);
        st = st != ODIN3_OK ? st : write_in_dir(out, name, &json);
        odin3_strbuf_free(&json);
    }
    return st;
}

/* <dir>/files-read.txt: every file the preprocessor read, resolved, in first-read order. */
static odin3_status emit_files_read(const pp_out *out, const odin3_pp *pp) {
    const uint32_t *paths = NULL;
    uint32_t count = odin3_pp_files_read(pp, &paths);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = odin3_strbuf_appendf(&text, "%s\n", text_of(out->design, paths[i]));
    }
    st = st != ODIN3_OK ? st : write_in_dir(out, "files-read.txt", &text);
    odin3_strbuf_free(&text);
    return st;
}

/* Creates the --out directory when it does not exist yet; IO (logged) when it cannot. */
static odin3_status make_out_dir(const pp_args *args) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = odin3_strbuf_append(&path, args->out);
    if (st == ODIN3_OK && mkdir(path.data, OUT_DIR_MODE) != 0 && errno != EEXIST) {
        odin3_log(ODIN3_LOG_ERROR, "preprocess: cannot create %s", path.data);
        st = ODIN3_ERR_IO;
    }
    odin3_strbuf_free(&path);
    return st;
}

/* --- preprocess ---------------------------------------------------------------------------- */

/* Every Verilog design file of the record, in listing order, through pp; each stream emitted. */
static odin3_status run_streams(const pp_out *out, const odin3_project *proj, odin3_pp *pp) {
    uint32_t index = 0; /* of the stream: <index>.v */
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < proj->files.len; i++) {
        const odin3_proj_file *file = odin3_vec_cat(&proj->files, i);
        if (file->lang != ODIN3_LANG_VERILOG) {
            continue;
        }
        odin3_pp_source src = {file->path.given, file->path.resolved, file->library, file->path.at};
        odin3_pp_stream stream;
        st = odin3_pp_run(pp, &src, &stream);
        st = st != ODIN3_OK ? st : emit_stream(out, index++, &stream);
    }
    if (st == ODIN3_OK && out->args.out.ptr != NULL) {
        st = emit_files_read(out, pp);
    }
    return st;
}

/* One preprocessor over the record's configuration, then run_streams. */
static odin3_status preprocess_project(const pp_out *out, const odin3_project *proj) {
    pp_setup setup;
    odin3_vec_init(&setup.defines, sizeof(odin3_pp_define));
    odin3_vec_init(&setup.incdirs, sizeof(const char *));
    odin3_pp *pp = NULL;
    odin3_status st = fill_setup(out->design, proj, &setup);
    if (st == ODIN3_OK) {
        odin3_pp_config config = {setup.defines.data, (uint32_t)setup.defines.len,
                                  (const char *const *)setup.incdirs.data,
                                  (uint32_t)setup.incdirs.len};
        st = odin3_pp_create(out->design, &config, &pp);
    }
    st = st != ODIN3_OK ? st : run_streams(out, proj, pp);
    odin3_pp_destroy(pp);
    odin3_vec_free(&setup.incdirs);
    odin3_vec_free(&setup.defines);
    return st;
}

/* Preprocesses the project's Verilog design files (PRJ-17; debug). */
static odin3_status preprocess_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)ctx;
    (void)user;
    pp_out out = {.design = design};
    odin3_status st = parse_pp_args(args, &out.args);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_project *proj = finished_project(design, "preprocess");
    if (proj == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_srcman *sm = NULL;
    st = odin3_design_get_srcman(design, &sm);
    out.sm = sm;
    if (st == ODIN3_OK && out.args.out.ptr != NULL) {
        st = make_out_dir(&out.args);
    }
    return st != ODIN3_OK ? st : preprocess_project(&out, proj);
}

/* --- the definitions ------------------------------------------------------------------------ */

const odin3_pass_def odin3_pass_read_project = {
    "read_project",
    "read_project [--format o3proj|f|qsf|odin2] [--revision <r>] <entry>: read a project "
    "description into the design's project record (no front end runs)",
    read_project_run, NULL, false};
const odin3_pass_def odin3_pass_dump_project = {
    "dump_project",
    "dump_project [<file>]: write the project record as JSON (stdout without a file)",
    dump_project_run, NULL, false};
const odin3_pass_def odin3_pass_preprocess = {
    "preprocess",
    "preprocess [--out <dir>] [--segments]: preprocess the project's Verilog files (debug): "
    "<dir>/<n>.v, <n>.segments.json and files-read.txt, or the text on stdout",
    preprocess_run, NULL, false};
