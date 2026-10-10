/* write_formats.c — the write_json, write_verilog and write_dot passes over the 1F writers. */
#include "backends/dot/writer.h"
#include "backends/json/writer.h"
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
    odin3_verilog_opts verilog;
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

static odin3_status parse_name_style(odin3_bytes value, write_args *out) {
    if (odin3_pass_arg_is(value, "short")) {
        out->verilog.name_style = ODIN3_VERILOG_NAMES_SHORT;
    } else if (odin3_pass_arg_is(value, "provenance")) {
        out->verilog.name_style = ODIN3_VERILOG_NAMES_PROVENANCE;
    } else {
        odin3_log(ODIN3_LOG_ERROR, "write_verilog: --name-style expects short or provenance");
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

/* Handles option word of pass (its value read from rest); unknown options are INVALID_ARG. */
static odin3_status parse_option(const char *pass, odin3_bytes *rest, odin3_bytes word,
                                 write_args *out) {
    bool verilog = strcmp(pass, "write_verilog") == 0;
    bool dot = strcmp(pass, "write_dot") == 0;
    bool known =
        (verilog && odin3_pass_arg_is(word, "--name-style")) ||
        (dot && (odin3_pass_arg_is(word, "--focus") || odin3_pass_arg_is(word, "--max-nodes")));
    odin3_bytes value = {0};
    odin3_status st = known ? option_value(pass, rest, word, &value) : ODIN3_ERR_INVALID_ARG;
    if (!known) {
        odin3_log(ODIN3_LOG_ERROR, "%s: unknown argument '%.*s'", pass, (int)word.len,
                  (const char *)word.ptr);
    } else if (st == ODIN3_OK && verilog) {
        st = parse_name_style(value, out);
    } else if (st == ODIN3_OK && odin3_pass_arg_is(word, "--focus") && out->focus.len > 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_dot: --focus given twice");
        st = ODIN3_ERR_INVALID_ARG;
    } else if (st == ODIN3_OK && odin3_pass_arg_is(word, "--focus")) {
        st = parse_focus(value, out);
    } else if (st == ODIN3_OK) {
        st = parse_max_nodes(value, out);
    }
    return st;
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

typedef enum write_format { WRITE_JSON, WRITE_VERILOG, WRITE_DOT } write_format;

static const char *const FORMAT_PASS[] = {"write_json", "write_verilog", "write_dot"};

/* Shared body: parse, then call the format's writer (1F; each writes atomically). */
static odin3_status write_format_run(odin3_design *design, odin3_bytes args, write_format format) {
    write_args parsed = {.verilog = {ODIN3_VERILOG_NAMES_SHORT},
                         .dot = {ODIN3_DOT_FOCUS_NONE, NULL, 0}};
    odin3_strbuf_init(&parsed.path);
    odin3_strbuf_init(&parsed.focus);
    odin3_status st = parse_write_args(FORMAT_PASS[format], args, &parsed);
    odin3_module_id top = odin3_design_top(design);
    if (st == ODIN3_OK && odin3_module_valid(top)) {
        odin3_log(ODIN3_LOG_DEBUG, "%s: %s: top %s", FORMAT_PASS[format], parsed.path.data,
                  odin3_strtab_get(odin3_design_strtab(design),
                                   odin3_module_name(odin3_module_get(design, top))));
    }
    if (st == ODIN3_OK && format == WRITE_JSON) {
        st = odin3_json_write(design, parsed.path.data);
    } else if (st == ODIN3_OK && format == WRITE_VERILOG) {
        st = odin3_verilog_write(design, parsed.path.data, &parsed.verilog);
    } else if (st == ODIN3_OK) {
        st = odin3_dot_write(design, parsed.path.data, &parsed.dot);
    }
    odin3_strbuf_free(&parsed.path);
    odin3_strbuf_free(&parsed.focus);
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

const odin3_pass_def odin3_pass_write_json = {
    "write_json", "write_json <file>: write the design as Yosys-schema JSON", write_json_run, NULL,
    false};
const odin3_pass_def odin3_pass_write_verilog = {
    "write_verilog",
    "write_verilog <file> [--name-style short|provenance]: write structural Verilog",
    write_verilog_run, NULL, false};
const odin3_pass_def odin3_pass_write_dot = {
    "write_dot",
    "write_dot <file> [--focus <path>|<file:line>|cone(<net>)] [--max-nodes N]: write Graphviz "
    "dot",
    write_dot_run, NULL, false};
