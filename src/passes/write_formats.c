/*
 * write_formats.c — the write_json, write_verilog and write_dot passes over the 1F writers, and
 * write_rtlil (2D §13).
 */
#include "backends/dot/writer.h"
#include "backends/json/writer.h"
#include "backends/rtlil/writer.h"
#include "backends/verilog/writer.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { DECIMAL = 10, NUM_BUF = 32 };

/* A writer's arguments: one path and the options the pass knows. */
typedef struct write_args {
    odin3_strbuf path;
    odin3_strbuf focus;
    odin3_strbuf
        name_map; /* --name-map's value (write_verilog, write_json), empty when not given */
    odin3_strbuf color;
    odin3_verilog_opts verilog;
    odin3_json_write_opts json;
    odin3_dot_opts dot;
} write_args;

/* The value word after option word (INVALID_ARG, logged, when there is none). */
static odin3_status option_value(const char *pass, odin3_bytes *rest, odin3_bytes word,
                                 odin3_bytes *value) {
    *value = odin3_pass_arg_next(rest);
    if (value->ptr == NULL || value->len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: %.*s expects a value", pass, (int)word.len,
                  (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* --name-style of write_verilog and write_json: short, provenance or hash (5.4f). */
static odin3_status parse_name_style(const char *pass, odin3_bytes value, write_args *out) {
    if (odin3_pass_arg_is(value, "short")) {
        out->verilog.name_style = ODIN3_VERILOG_NAMES_SHORT;
        out->json.name_style = ODIN3_JSON_NAMES_SHORT;
    } else if (odin3_pass_arg_is(value, "provenance")) {
        out->verilog.name_style = ODIN3_VERILOG_NAMES_PROVENANCE;
        out->json.name_style = ODIN3_JSON_NAMES_PROVENANCE;
    } else if (odin3_pass_arg_is(value, "hash")) {
        out->verilog.name_style = ODIN3_VERILOG_NAMES_HASH;
        out->json.name_style = ODIN3_JSON_NAMES_HASH;
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s: --name-style expects short, provenance or hash", pass);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status parse_max_nodes(odin3_bytes value, write_args *out) {
    char text[NUM_BUF] = {0};
    bool fits = value.len < sizeof text;
    if (fits) {
        memcpy(text, value.ptr, value.len);
    }
    char *end = NULL;
    errno = 0;
    unsigned long long num =
        fits && text[0] >= '0' && text[0] <= '9' ? strtoull(text, &end, DECIMAL) : 0;
    if (!fits || end == NULL || *end != '\0' || errno != 0 || num == 0 || num > SIZE_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "write_dot: --max-nodes expects a positive number");
        return ODIN3_ERR_INVALID_ARG;
    }
    out->dot.max_nodes = (size_t)num;
    return ODIN3_OK;
}

/*
 * A focus is "cone(<net>)" (the net's fan-in cone, <net> as "net" or "module:net"), a
 * "file:line" (digits after the last ':'), or else a hierarchical path ("module/inst/...").
 */
static odin3_status parse_focus(odin3_bytes value, write_args *out) {
    const char *text = (const char *)value.ptr;
    size_t len = value.len;
    bool cone = len > strlen("cone()") && strncmp(text, "cone(", strlen("cone(")) == 0 &&
                text[len - 1] == ')';
    if (cone) {
        text += strlen("cone(");
        len -= strlen("cone()");
    }
    odin3_status st = odin3_strbuf_append(&out->focus, (odin3_bytes){text, len});
    const char *colon = strrchr(out->focus.data, ':');
    bool line = !cone && colon != NULL && colon[1] != '\0' &&
                strspn(colon + 1, "0123456789") == strlen(colon + 1);
    out->dot.focus_kind = ODIN3_DOT_FOCUS_PATH;
    if (cone) {
        out->dot.focus_kind = ODIN3_DOT_FOCUS_CONE;
    } else if (line) {
        out->dot.focus_kind = ODIN3_DOT_FOCUS_LOC;
    }
    out->dot.focus = out->focus.data;
    return st;
}

/* write_verilog's and write_json's options: --name-style and --name-map. */
static odin3_status parse_names_option(const char *pass, odin3_bytes *rest, odin3_bytes word,
                                       write_args *out) {
    odin3_bytes value = {0};
    odin3_status st = option_value(pass, rest, word, &value);
    if (st != ODIN3_OK) {
        return st;
    }
    return odin3_pass_arg_is(word, "--name-map") ? odin3_strbuf_append(&out->name_map, value)
                                                 : parse_name_style(pass, value, out);
}

/* A write_dot option word with its value read from rest (--locs has none). */
static odin3_status parse_dot_option(odin3_bytes *rest, odin3_bytes word, write_args *out) {
    if (odin3_pass_arg_is(word, "--locs")) {
        out->dot.locs = true;
        return ODIN3_OK;
    }
    odin3_bytes value = {0};
    odin3_status st = option_value("write_dot", rest, word, &value);
    if (st != ODIN3_OK) {
        return st;
    }
    if (value.len >= 2 && memcmp(value.ptr, "--", 2) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_dot: %.*s expects a value, got option '%.*s'",
                  (int)word.len, (const char *)word.ptr, (int)value.len, (const char *)value.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    bool is_focus = odin3_pass_arg_is(word, "--focus");
    bool is_color = odin3_pass_arg_is(word, "--color-by");
    if ((is_focus && out->focus.len > 0) || (is_color && out->color.len > 0)) {
        odin3_log(ODIN3_LOG_ERROR, "write_dot: %.*s given twice", (int)word.len,
                  (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (is_color) {
        st = odin3_strbuf_append(&out->color, value);
        out->dot.color_attr = out->color.data;
        return st;
    }
    return is_focus ? parse_focus(value, out) : parse_max_nodes(value, out);
}

static bool is_dot_option(odin3_bytes word) {
    return odin3_pass_arg_is(word, "--focus") || odin3_pass_arg_is(word, "--max-nodes") ||
           odin3_pass_arg_is(word, "--color-by") || odin3_pass_arg_is(word, "--locs");
}

/* Handles option word of pass (its value read from rest); unknown options are INVALID_ARG. */
static odin3_status parse_option(const char *pass, odin3_bytes *rest, odin3_bytes word,
                                 write_args *out) {
    bool names = strcmp(pass, "write_verilog") == 0 || strcmp(pass, "write_json") == 0;
    bool dot = strcmp(pass, "write_dot") == 0;
    if (strcmp(pass, "write_json") == 0 && odin3_pass_arg_is(word, "--odin3")) {
        out->json.odin3_keys = true;
        return ODIN3_OK;
    }
    if (names &&
        (odin3_pass_arg_is(word, "--name-style") || odin3_pass_arg_is(word, "--name-map"))) {
        return parse_names_option(pass, rest, word, out);
    }
    if (dot && is_dot_option(word)) {
        return parse_dot_option(rest, word, out);
    }
    odin3_log(ODIN3_LOG_ERROR, "%s: unknown argument '%.*s'", pass, (int)word.len,
              (const char *)word.ptr);
    return ODIN3_ERR_INVALID_ARG;
}

/* `<file> [options]`: exactly one path, in any position among the options. */
static odin3_status parse_write_args(const char *pass, odin3_bytes args, write_args *out) {
    odin3_status st = ODIN3_OK;
    bool extra = false;
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (word.len > 1 && ((const char *)word.ptr)[0] == '-') {
            st = parse_option(pass, &args, word, out);
        } else if (out->path.len == 0 && word.len > 0) {
            st = odin3_strbuf_append(&out->path, word);
        } else {
            extra = true;
        }
    }
    if (st == ODIN3_OK && (extra || out->path.len == 0)) {
        odin3_log(ODIN3_LOG_ERROR, "%s: expects one path (and its options)", pass);
        st = ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

typedef enum write_format { WRITE_JSON, WRITE_VERILOG, WRITE_DOT, WRITE_RTLIL } write_format;

static const char *const FORMAT_PASS[] = {"write_json", "write_verilog", "write_dot",
                                          "write_rtlil"};

/* --name-map belongs to --name-style hash; the options carry its path to the writer. */
static odin3_status check_name_map(const char *pass, write_args *parsed) {
    bool hash = parsed->json.name_style == ODIN3_JSON_NAMES_HASH;
    if (parsed->name_map.len > 0 && !hash) {
        odin3_log(ODIN3_LOG_ERROR, "%s: --name-map needs --name-style hash", pass);
        return ODIN3_ERR_INVALID_ARG;
    }
    const char *map = parsed->name_map.len > 0 ? parsed->name_map.data : NULL;
    parsed->verilog.name_map = map;
    parsed->json.name_map = map;
    return ODIN3_OK;
}

/* Shared body: parse, then call the format's writer (1F; each writes atomically). */
static odin3_status write_format_run(odin3_design *design, odin3_bytes args, write_format format) {
    write_args parsed = {.verilog = {ODIN3_VERILOG_NAMES_SHORT, NULL},
                         .json = {ODIN3_JSON_NAMES_SHORT, NULL, false},
                         .dot = {ODIN3_DOT_FOCUS_NONE, NULL, 0, NULL, false}};
    odin3_strbuf_init(&parsed.path);
    odin3_strbuf_init(&parsed.focus);
    odin3_strbuf_init(&parsed.name_map);
    odin3_strbuf_init(&parsed.color);
    odin3_status st = parse_write_args(FORMAT_PASS[format], args, &parsed);
    st = st == ODIN3_OK ? check_name_map(FORMAT_PASS[format], &parsed) : st;
    odin3_module_id top = odin3_design_top(design);
    if (st == ODIN3_OK && odin3_module_valid(top)) {
        odin3_log(ODIN3_LOG_DEBUG, "%s: %s: top %s", FORMAT_PASS[format], parsed.path.data,
                  odin3_strtab_get(odin3_design_strtab(design),
                                   odin3_module_name(odin3_module_get(design, top))));
    }
    if (st == ODIN3_OK && format == WRITE_JSON) {
        st = odin3_json_write_with(design, parsed.path.data, &parsed.json);
    } else if (st == ODIN3_OK && format == WRITE_VERILOG) {
        st = odin3_verilog_write(design, parsed.path.data, &parsed.verilog);
    } else if (st == ODIN3_OK && format == WRITE_RTLIL) {
        st = odin3_rtlil_write(design, parsed.path.data);
    } else if (st == ODIN3_OK) {
        st = odin3_dot_write(design, parsed.path.data, &parsed.dot);
    }
    odin3_strbuf_free(&parsed.path);
    odin3_strbuf_free(&parsed.focus);
    odin3_strbuf_free(&parsed.name_map);
    odin3_strbuf_free(&parsed.color);
    return st;
}

static odin3_status write_json_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)ctx;
    (void)user;
    return write_format_run(design, args, WRITE_JSON);
}

static odin3_status write_verilog_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                      void *user) {
    (void)ctx;
    (void)user;
    return write_format_run(design, args, WRITE_VERILOG);
}

static odin3_status write_dot_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)ctx;
    (void)user;
    return write_format_run(design, args, WRITE_DOT);
}

static odin3_status write_rtlil_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)ctx;
    (void)user;
    return write_format_run(design, args, WRITE_RTLIL);
}

const odin3_pass_def odin3_pass_write_json = {
    "write_json",
    "write_json <file> [--odin3] [--name-style short|provenance|hash [--name-map <file>]]: write "
    "the "
    "design as Yosys-schema JSON (file - is stdout); hash names write the name map to <file>.names "
    "or --name-map; --odin3 adds odin3_version, and per module, cell and netname odin3_prov and "
    "odin3_level",
    write_json_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
const odin3_pass_def odin3_pass_write_verilog = {
    "write_verilog",
    "write_verilog <file> [--name-style short|provenance|hash [--name-map <file>]]: write "
    "structural Verilog (file - is stdout); hash names write the name map to <file>.names or "
    "--name-map",
    write_verilog_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
const odin3_pass_def odin3_pass_write_dot = {
    "write_dot",
    "write_dot <file> [--focus <path>|<file:line>|cone(<net>)] [--max-nodes N] [--color-by "
    "<attr>|@type] [--locs]: write Graphviz dot; --color-by fills nodes by attribute value and "
    "adds a legend, --locs adds the first HDL file:line to each label (file - is stdout)",
    write_dot_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
/* 2D EL-48: RTLIL is the word-level view with processes, so the pass runs on them (EL-33). */
const odin3_pass_def odin3_pass_write_rtlil = {
    "write_rtlil",
    "write_rtlil <file>: write the design as Yosys RTLIL text (word level, processes included); "
    "file - is stdout",
    write_rtlil_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
