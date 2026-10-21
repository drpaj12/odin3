/*
 * dump_locs.c — the dump_locs pass (spec 5.4 section 6, T-3): every live module, wire, net and
 * node with its HDL locations, one line each, `module|kind|name|SRC`. SRC is the text the writers
 * print (backends/common/hdl_loc.h: only SOURCE leaves, Yosys's form, up to 8 joined by `|`) and is
 * empty for an object with none. In the module, kind and name fields a backslash is written as two,
 * a `|` as backslash and `|`, and a newline as backslash and `n`, so the first three unescaped
 * `|` end the fields and SRC (the rest of the line, its own `|` unescaped) is unambiguous. An
 * unnamed object is `$w<ID>`, `$n<ID>` or `$c<ID>`. With `--types` a node's kind field is
 * `node:TYPE` (the cell type name, escaped like the other fields; only the first `:` splits kind
 * from type, no field is added, so the parse rule above holds); the default output is unchanged.
 * The tool that
 * measures a write and read back (tools/loc-check) runs it before and after and compares.
 */
#include "backends/common/hdl_loc.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* One line of the dump. */
typedef struct dump_line {
    const char *kind;
    uint32_t name; /* strtab ID, 0 for unnamed */
    char prefix;   /* the generated name of an unnamed object is `$<prefix><id>` */
    uint32_t id;
    odin3_prov_id prov;
    const char *type; /* cell type name for a typed node line, else NULL */
} dump_line;

typedef struct dump {
    bool types; /* --types: node lines carry the cell type in the kind field */
    odin3_hdl_ctx *hdl;
    const odin3_design *design;
    const odin3_strtab *tab;
    const char *module_name;
    odin3_strbuf text;
    odin3_strbuf src;
} dump;

/* Appends text with backslash, `|` and newline escaped (see the file comment). */
static odin3_status append_escaped(odin3_strbuf *out, const char *text) {
    odin3_status st = ODIN3_OK;
    for (const char *at = text; st == ODIN3_OK && *at != '\0'; at++) {
        if (*at == '\\' || *at == '|') {
            st = odin3_strbuf_appendf(out, "\\%c", *at);
        } else if (*at == '\n') {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("\\n"));
        } else {
            st = odin3_strbuf_append(out, (odin3_bytes){at, 1});
        }
    }
    return st;
}

static odin3_status add_line(dump *state, const dump_line *line) {
    odin3_hdl_locs_out locs;
    uint32_t written = 0;
    odin3_status st = odin3_hdl_locs(state->hdl, line->prov, &locs);
    odin3_hdl_print_req req = {&locs, false};
    odin3_strbuf_clear(&state->src);
    st = st == ODIN3_OK && locs.n > 0 ? odin3_hdl_src_text(state->hdl, &req, &state->src, &written)
                                      : st;
    st = st == ODIN3_OK ? append_escaped(&state->text, state->module_name) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(&state->text, "|%s", line->kind) : st;
    if (st == ODIN3_OK && line->type != NULL) {
        st = odin3_strbuf_append(&state->text, odin3_bytes_cstr(":"));
        st = st == ODIN3_OK ? append_escaped(&state->text, line->type) : st;
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(&state->text, odin3_bytes_cstr("|")) : st;
    if (st == ODIN3_OK && line->name != 0) {
        st = append_escaped(&state->text, odin3_strtab_get(state->tab, line->name));
    } else if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(&state->text, "$%c%u", line->prefix, (unsigned)line->id);
    }
    st = st == ODIN3_OK ? odin3_strbuf_appendf(&state->text, "|%s\n",
                                               state->src.data != NULL ? state->src.data : "")
                        : st;
    return st;
}

static odin3_status dump_wires(dump *state, const odin3_module *module) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_wire_end(module); i++) {
        odin3_wire_id wire = {i};
        if (odin3_wire_live(module, wire)) {
            dump_line line = {"wire", odin3_wire_name(module, wire), 'w',
                              i,      odin3_wire_prov(module, wire), NULL};
            st = add_line(state, &line);
        }
    }
    return st;
}

static odin3_status dump_nets(dump *state, const odin3_module *module) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_net_end(module); i++) {
        odin3_net_id net = {i};
        if (odin3_net_live(module, net)) {
            dump_line line = {"net", odin3_net_name(module, net), 'n',
                              i,     odin3_net_prov(module, net), NULL};
            st = add_line(state, &line);
        }
    }
    return st;
}

static odin3_status dump_nodes(dump *state, const odin3_module *module) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        if (odin3_node_live(module, node)) {
            const odin3_celltype_def *def =
                state->types ? odin3_celltype_get(state->design, odin3_node_type(module, node))
                             : NULL;
            const char *type = NULL;
            if (state->types) {
                type = def != NULL ? def->name : ""; /* never mix plain and typed lines */
            }
            dump_line line = {"node", odin3_node_name(module, node), 'c',
                              i,      odin3_node_prov(module, node), type};
            st = add_line(state, &line);
        }
    }
    return st;
}

static odin3_status dump_module(dump *state, const odin3_module *module) {
    state->module_name = odin3_strtab_get(state->tab, odin3_module_name(module));
    dump_line line = {"module", odin3_module_name(module), 'm', 0, odin3_module_prov(module), NULL};
    odin3_status st = add_line(state, &line);
    st = st == ODIN3_OK ? dump_wires(state, module) : st;
    st = st == ODIN3_OK ? dump_nets(state, module) : st;
    return st == ODIN3_OK ? dump_nodes(state, module) : st;
}

/* Writes text to stdout; IO (logged) when it cannot. */
static odin3_status write_stdout(const odin3_strbuf *text) {
    if ((text->len > 0 && fwrite(text->data, 1, text->len, stdout) != text->len) ||
        fflush(stdout) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "dump_locs: cannot write to stdout");
        return ODIN3_ERR_IO;
    }
    return ODIN3_OK;
}

typedef struct dump_args {
    bool types;
    odin3_bytes path; /* ptr NULL: stdout */
} dump_args;

/* Reads `[--types] [--] [<file>]`; INVALID_ARG for another `--` word, a repeated --types, an
 * empty or a second path. A literal `--` ends the options, so a path may start with `--`. */
static odin3_status parse_args(odin3_bytes args, dump_args *out) {
    *out = (dump_args){false, {NULL, 0}};
    bool options = true;
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (options && word.len == 2 && memcmp(word.ptr, "--", 2) == 0) {
            options = false;
        } else if (options && word.len == strlen("--types") &&
                   memcmp(word.ptr, "--types", word.len) == 0) {
            if (out->types) {
                odin3_log(ODIN3_LOG_ERROR, "dump_locs: --types given twice");
                return ODIN3_ERR_INVALID_ARG;
            }
            out->types = true;
        } else if (out->path.ptr != NULL || word.len == 0 ||
                   (options && word.len >= 2 && memcmp(word.ptr, "--", 2) == 0)) {
            return ODIN3_ERR_INVALID_ARG;
        } else {
            out->path = word;
        }
    }
    return ODIN3_OK;
}

static odin3_status dump_locs_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)ctx;
    (void)user;
    dump_args parsed;
    if (parse_args(args, &parsed) != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR,
                  "dump_locs: expects [--types] [--] and at most one non-empty path");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_bytes path = parsed.path;
    dump state = {parsed.types, odin3_hdl_ctx_create(design, "dump_locs"),
                  design,       odin3_design_strtab(design),
                  NULL,         {NULL, 0, 0},
                  {NULL, 0, 0}};
    odin3_strbuf name;
    odin3_strbuf_init(&state.text);
    odin3_strbuf_init(&state.src);
    odin3_strbuf_init(&name);
    odin3_status st = state.hdl != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){i});
        st = module != NULL ? dump_module(&state, module) : ODIN3_OK;
    }
    if (st == ODIN3_OK && path.ptr != NULL) {
        st = odin3_strbuf_append(&name, path);
        st = st != ODIN3_OK ? st : odin3_pass_write_text(name.data, &state.text);
    } else if (st == ODIN3_OK) {
        st = write_stdout(&state.text);
    }
    odin3_strbuf_free(&name);
    odin3_strbuf_free(&state.src);
    odin3_strbuf_free(&state.text);
    odin3_hdl_ctx_destroy(state.hdl);
    return st;
}

const odin3_pass_def odin3_pass_dump_locs = {
    "dump_locs",
    "dump_locs [--types] [--] [<file>]: list every module, wire, net and node with its HDL "
    "locations, "
    "one "
    "`module|kind|name|SRC` line each, `node:TYPE` as the node kind with --types (stdout without a "
    "file)",
    dump_locs_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
