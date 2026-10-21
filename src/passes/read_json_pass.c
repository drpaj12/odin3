/*
 * read_json_pass.c — the read_json pass over the JSON reader (spec 5.3 J-1).
 */
#include "frontends/json/reader.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <string.h>

/* read_json's arguments: the path, and the options in any order. */
typedef struct read_json_args {
    odin3_strbuf path;
    odin3_strbuf top;
    odin3_strbuf name_map;
    bool have_top;
    bool have_map;
    odin3_json_opts opts;
} read_json_args;

static odin3_status take_name_map(odin3_bytes *rest, read_json_args *args) {
    odin3_bytes file = odin3_pass_arg_next(rest);
    if (file.ptr == NULL || file.len == 0 || args->have_map) {
        odin3_log(ODIN3_LOG_ERROR, file.ptr == NULL || file.len == 0
                                       ? "read_json: --name-map expects a file"
                                       : "read_json: --name-map given twice");
        return ODIN3_ERR_INVALID_ARG;
    }
    args->have_map = true;
    return odin3_strbuf_append(&args->name_map, file);
}

static odin3_status take_option(odin3_bytes *rest, odin3_bytes word, read_json_args *args) {
    if (odin3_pass_arg_is(word, "--auto-blackbox")) {
        args->opts.auto_blackbox = true;
        return ODIN3_OK;
    }
    if (odin3_pass_arg_is(word, "--keep-src-attr")) {
        args->opts.keep_src_attr = true;
        return ODIN3_OK;
    }
    if (odin3_pass_arg_is(word, "--name-map")) {
        return take_name_map(rest, args);
    }
    if (!odin3_pass_arg_is(word, "--top")) {
        odin3_log(ODIN3_LOG_ERROR, "read_json: unknown argument '%.*s'", (int)word.len,
                  (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_bytes value = odin3_pass_arg_next(rest);
    if (value.ptr == NULL || value.len == 0 || args->have_top) {
        odin3_log(ODIN3_LOG_ERROR, value.ptr == NULL || value.len == 0
                                       ? "read_json: --top expects a module name"
                                       : "read_json: --top given twice");
        return ODIN3_ERR_INVALID_ARG;
    }
    args->have_top = true;
    return odin3_strbuf_append(&args->top, value);
}

static odin3_status parse_args(odin3_bytes rest, read_json_args *args) {
    odin3_status st = ODIN3_OK;
    for (odin3_bytes word = odin3_pass_arg_next(&rest); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&rest)) {
        bool option = word.len > 2 && word.ptr != NULL && memcmp(word.ptr, "--", 2) == 0;
        if (option) {
            st = take_option(&rest, word, args);
        } else if (args->path.len > 0) {
            odin3_log(ODIN3_LOG_ERROR, "read_json: expects one path");
            st = ODIN3_ERR_INVALID_ARG;
        } else {
            st = odin3_strbuf_append(&args->path, word);
        }
    }
    if (st == ODIN3_OK && args->path.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "read_json: expects one path");
        st = ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

static odin3_status read_json_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args_text,
                                  void *user) {
    (void)user;
    read_json_args args = {0};
    odin3_strbuf_init(&args.path);
    odin3_strbuf_init(&args.top);
    odin3_strbuf_init(&args.name_map);
    odin3_status st = parse_args(args_text, &args);
    if (st == ODIN3_OK) {
        /* the pass option wins over the global --top (J-1) */
        const char *global = odin3_pass_get_options().top;
        args.opts.top = args.have_top ? args.top.data : global;
        args.opts.name_map = args.have_map ? args.name_map.data : NULL;
        st = odin3_json_read_in(ctx, args.path.data, &args.opts);
    }
    (void)design;
    odin3_strbuf_free(&args.path);
    odin3_strbuf_free(&args.top);
    odin3_strbuf_free(&args.name_map);
    return st;
}

const odin3_pass_def odin3_pass_read_json = {
    "read_json",
    "read_json [--top NAME] [--auto-blackbox] [--keep-src-attr] [--name-map <file>] <file>: read a "
    "Yosys JSON netlist "
    "(or Odin III's own write_json output) into an empty design; --auto-blackbox makes a cell "
    "type nothing defines an implicit black box with a warning; a `src` attribute that holds HDL "
    "locations becomes a provenance record, --keep-src-attr keeps it as an attribute instead; "
    "--name-map replaces each hash key the map (written by write_json --name-style hash) holds by "
    "its full name, which gives the object its name, hierarchy and location",
    read_json_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
