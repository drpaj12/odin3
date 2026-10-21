/* reader.c — the JSON reader's entry points, errors, provenance and the two walks (M-0). */
#include "frontends/json/reader.h"

#include "frontends/json/reader_internal.h"
#include "util/file.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* --- errors -------------------------------------------------------------------------------- */

/* Appends ".part" to buf at *at, unless part is NULL or the buffer is full. */
static void path_part(char *buf, size_t size, size_t *at, const char *part) {
    if (part != NULL && part[0] != '\0' && *at < size) {
        int wrote = snprintf(buf + *at, size - *at, ".%s", part);
        *at += wrote > 0 ? (size_t)wrote : 0;
    }
}

/* modules.<module>.<section>.<key>.<member>, or "" when there is no module yet. */
static void format_path(const jr *rd, const jr_where *where, char *buf, size_t size) {
    size_t at = 0;
    buf[0] = '\0';
    if (rd->cur_name == NULL) {
        path_part(buf, size, &at, where != NULL ? where->section : NULL);
    } else {
        (void)snprintf(buf, size, "modules");
        at = strlen(buf);
        path_part(buf, size, &at, rd->cur_name);
        if (where != NULL) {
            path_part(buf, size, &at, where->section);
            path_part(buf, size, &at, where->key);
            path_part(buf, size, &at, where->member);
        }
    }
    if (at >= size) {
        buf[size - 1] = '\0';
    }
}

odin3_status jr_error(const jr *rd, const odin3_json_node *node, const jr_where *where,
                      const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    char path[JR_NAME_BUF * 3];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    format_path(rd, where, path, sizeof path);
    odin3_json_loc loc = {0, 0};
    if (node != NULL) {
        loc = odin3_json_key(node) != NULL ? odin3_json_key_pos(node) : odin3_json_pos(node);
    }
    odin3_log(ODIN3_LOG_ERROR, "%s:%u:%u: %s%s%s", rd->path, (unsigned)loc.line, (unsigned)loc.col,
              path, path[0] != '\0' ? ": " : "", msg);
    return ODIN3_ERR_PARSE;
}

odin3_status jr_fail(jr *rd, odin3_status st) {
    if (st == ODIN3_ERR_NO_MEMORY && !rd->oom_logged) {
        rd->oom_logged = true;
        odin3_log(ODIN3_LOG_ERROR, "%s: out of memory", rd->path);
    }
    return st;
}

odin3_status jr_prov(jr *rd, const odin3_json_node *node, odin3_prov_id *out) {
    odin3_json_loc pos =
        odin3_json_key(node) != NULL ? odin3_json_key_pos(node) : odin3_json_pos(node);
    odin3_srcloc loc = {.file = rd->file,
                        .line = pos.line,
                        .col = pos.col,
                        .end_line = pos.line,
                        .end_col = pos.col};
    odin3_prov_origin origin = {.locs = &loc, .n_locs = 1};
    return jr_fail(rd, odin3_prov_imported(&rd->ctx, &origin, out));
}

odin3_status jr_intern(jr *rd, const char *text, uint32_t *out) {
    return jr_fail(rd, odin3_design_intern(rd->design, odin3_bytes_cstr(text), out));
}

const char *jr_str(const jr *rd, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(rd->design), str);
}

odin3_status jr_join(const jr *rd, const jr_join_args *args, char *buf, size_t size) {
    int wrote = snprintf(buf, size, "%s%s", args->prefix, args->name);
    if (wrote < 0 || (size_t)wrote >= size) {
        buf[0] = '\0';
        return jr_error(rd, args->node, args->where, "the name '%.40s...' is longer than %zu bytes",
                        args->name, size - 1 - strlen(args->prefix));
    }
    return ODIN3_OK;
}

/* --- setup and teardown -------------------------------------------------------------------- */

static odin3_status jr_init(jr *rd, odin3_design *design, const char *path,
                            const odin3_json_opts *opts) {
    *rd = (jr){.design = design, .path = path};
    if (opts != NULL) {
        rd->opts = *opts;
    }
    odin3_vec_init(&rd->mods, sizeof(jr_mod));
    odin3_vec_init(&rd->edges, sizeof(jr_edge));
    odin3_vec_init(&rd->implicit, sizeof(jr_implicit));
    odin3_strbuf_init(&rd->scratch);
    rd->mod_index = odin3_u64map_create(0);
    rd->implicit_index = odin3_u64map_create(0);
    rd->implicit_port = odin3_u64map_create(0);
    jr_cur_init(rd);
    rd->vals = odin3_arena_create(0);
    if (rd->mod_index == NULL || rd->implicit_index == NULL || rd->implicit_port == NULL ||
        rd->vals == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    odin3_strbuf_init(&rd->name_buf);
    odin3_status st = jr_intern(rd, path, &rd->file);
    if (st == ODIN3_OK && rd->opts.name_map != NULL) {
        st = odin3_name_resolver_open(&rd->names, "read_json", rd->opts.name_map);
        rd->have_names = st == ODIN3_OK;
    }
    return st;
}

static void jr_free(jr *rd) {
    jr_free_implicit(rd);
    odin3_vec_free(&rd->mods);
    odin3_vec_free(&rd->edges);
    odin3_vec_free(&rd->implicit);
    odin3_strbuf_free(&rd->scratch);
    odin3_strbuf_free(&rd->name_buf);
    if (rd->have_names) {
        odin3_name_resolver_close(&rd->names);
    }
    odin3_u64map_destroy(rd->mod_index);
    odin3_u64map_destroy(rd->implicit_index);
    odin3_u64map_destroy(rd->implicit_port);
    odin3_arena_destroy(rd->vals);
    odin3_json_doc_free(&rd->doc);
}

/* --- the file ------------------------------------------------------------------------------ */

/* Reads and parses the file into rd->doc. */
static odin3_status load(jr *rd) {
    odin3_file_bytes bytes = {0};
    odin3_status st = odin3_file_read_all(rd->path, ODIN3_JSON_MAX_BYTES, &bytes);
    if (st == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "%s: cannot read: %s", rd->path, strerror(bytes.err));
    } else if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_ERROR, "%s: file larger than %u bytes", rd->path,
                  (unsigned)ODIN3_JSON_MAX_BYTES);
    } else if (st == ODIN3_OK) {
        st = odin3_json_parse(bytes.data, bytes.len, NULL, &rd->doc);
        if (st == ODIN3_ERR_PARSE) {
            odin3_log(ODIN3_LOG_ERROR, "%s:%u:%u: %s", rd->path, (unsigned)rd->doc.error.line,
                      (unsigned)rd->doc.error.col, rd->doc.error.message);
        }
    }
    odin3_file_bytes_free(&bytes);
    return jr_fail(rd, st);
}

static odin3_status begin_run(jr *rd) {
    if (odin3_passrun_valid(rd->ctx.run)) {
        return ODIN3_OK;
    }
    uint32_t name = 0;
    odin3_status st = jr_intern(rd, "read_json", &name);
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_pass_run_begin(rd->design, name, &rd->ctx));
    }
    return st;
}

/* J-3: `creator` goes to the run information (once; a run that has some keeps it). */
static odin3_status note_creator(jr *rd, const odin3_json_node *root) {
    const odin3_json_node *creator = jr_member(root, "creator");
    if (creator == NULL || jr_kind(creator) != ODIN3_JSON_STRING ||
        odin3_passrun_info(rd->design, rd->ctx.run) != 0) {
        return ODIN3_OK;
    }
    size_t len = 0;
    const char *text = jr_text(creator, &len);
    odin3_strbuf info;
    odin3_strbuf_init(&info);
    uint32_t str = 0;
    odin3_status st = odin3_passrun_info_append(&info, "creator", (odin3_bytes){text, len});
    if (st == ODIN3_OK) {
        st = jr_intern(rd, info.data, &str);
    }
    if (st == ODIN3_OK) {
        st = odin3_passrun_set_info(&rd->ctx, str);
    }
    odin3_strbuf_free(&info);
    return jr_fail(rd, st);
}

/* J-3: the root and its "modules" object. */
static odin3_status read_root(jr *rd) {
    static const char *const known[] = {"creator", "modules", "models"};
    const odin3_json_node *root = rd->doc.root;
    if (jr_kind(root) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, root, NULL, "the root is not an object");
    }
    jr_audit(rd, root, known, sizeof known / sizeof known[0]);
    odin3_status st = jr_check_version(rd, root);
    if (st == ODIN3_OK && jr_member(root, "models") != NULL) {
        odin3_log(ODIN3_LOG_INFO, "%s: the -aig models are ignored", rd->path);
    }
    rd->modules = jr_member(root, "modules");
    if (st == ODIN3_OK && rd->modules != NULL && jr_kind(rd->modules) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, rd->modules, NULL, "modules is not an object");
    }
    if (st == ODIN3_OK && jr_count(rd->modules) == 0) {
        return jr_error(rd, rd->modules != NULL ? rd->modules : root, NULL, "no modules");
    }
    if (st == ODIN3_OK && jr_count(rd->modules) > JR_MAX_OBJECTS) {
        return jr_error(rd, rd->modules, NULL, "more than %u modules", (unsigned)JR_MAX_OBJECTS);
    }
    return st;
}

/* Pass 2: every module that is not a black box. */
static odin3_status read_bodies(jr *rd) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < rd->mods.len; i++) {
        if (!((const jr_mod *)odin3_vec_cat(&rd->mods, i))->blackbox) {
            st = jr_read_module_body(rd, i);
        }
    }
    rd->cur_name = NULL;
    return st;
}

static odin3_status read_all(jr *rd) {
    odin3_status st = load(rd);
    if (st == ODIN3_OK) {
        st = read_root(rd);
    }
    if (st == ODIN3_OK) {
        st = begin_run(rd);
    }
    if (st == ODIN3_OK) {
        st = note_creator(rd, rd->doc.root);
    }
    if (st == ODIN3_OK) {
        st = jr_read_modules(rd);
    }
    if (st == ODIN3_OK) {
        st = jr_scan_types(rd);
    }
    if (st == ODIN3_OK) {
        st = jr_make_implicit(rd);
    }
    if (st == ODIN3_OK) {
        st = jr_check_cycles(rd);
    }
    if (st == ODIN3_OK) {
        st = jr_choose_top(rd);
    }
    if (st == ODIN3_OK) {
        st = read_bodies(rd);
    }
    if (st == ODIN3_OK) {
        jr_report_skipped(rd);
        jr_report_src(rd);
        jr_report_names(rd);
    }
    return st;
}

static odin3_status read_file(odin3_design *design, const char *path, odin3_pass_ctx *ctx,
                              const odin3_json_opts *opts) {
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "json_read: NULL design or path");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_design_module_end(design) != 1) {
        odin3_log(ODIN3_LOG_ERROR, "json_read: %s: the design already has modules", path);
        return ODIN3_ERR_INVALID_ARG;
    }
    jr rd;
    odin3_status st = jr_init(&rd, design, path, opts);
    if (ctx != NULL) {
        rd.ctx = *ctx;
    }
    if (st == ODIN3_OK) {
        st = read_all(&rd);
    }
    if (ctx != NULL) {
        ctx->op = rd.ctx.op;
    }
    jr_cur_end(&rd);
    jr_free(&rd);
    return st;
}

odin3_status odin3_json_read(odin3_design *design, const char *path, const odin3_json_opts *opts) {
    return read_file(design, path, NULL, opts);
}

odin3_status odin3_json_read_in(odin3_pass_ctx *ctx, const char *path,
                                const odin3_json_opts *opts) {
    if (ctx == NULL || ctx->design == NULL || !odin3_passrun_valid(ctx->run) ||
        ctx->run.v >= odin3_passrun_end(ctx->design)) {
        odin3_log(ODIN3_LOG_ERROR, "json_read: NULL or invalid pass context");
        return ODIN3_ERR_INVALID_ARG;
    }
    return read_file(ctx->design, path, ctx, opts);
}
