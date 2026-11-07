/*
 * gv_pass.c — the write_graph pass (spec docs/specs/2026-11-05-graph-views-design.md section 1):
 * `write_graph KIND FILE`, KIND one of stg, cfg, ddg. One JSON file, schema odin3-graph version 1.
 */
#include "passes/builtin.h"
#include "passes/graphviews/gv.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

enum { SCHEMA_VERSION = 1, PATH_BUF = 4096 };

typedef struct gv_kind {
    const char *name;
    odin3_status (*fn)(gv_run *run);
} gv_kind;

static const gv_kind k_kinds[] = {
    {"stg", gv_write_stg}, {"cfg", gv_write_cfg}, {"ddg", gv_write_ddg}};

static const char k_usage[] = "write_graph: usage: write_graph stg|cfg|ddg FILE";

static const gv_kind *find_kind(odin3_bytes word) {
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        if (odin3_pass_arg_is(word, k_kinds[i].name)) {
            return &k_kinds[i];
        }
    }
    return NULL;
}

static odin3_status save_file(const char *path, const odin3_strbuf *text) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "write_graph: cannot write '%s'", path);
        return ODIN3_ERR_IO;
    }
    bool ok = fwrite(text->data, 1, text->len, fp) == text->len;
    return (fclose(fp) == 0 && ok) ? ODIN3_OK : ODIN3_ERR_IO;
}

static odin3_status assemble(gv_run *run, const char *kind, odin3_strbuf *body) {
    odin3_status st = odin3_strbuf_appendf(body,
                                           "{\n  \"schema\": \"odin3-graph\",\n  \"version\": "
                                           "%d,\n  \"kind\": \"%s\",\n  \"modules\": [",
                                           SCHEMA_VERSION, kind);
    for (uint32_t id = 1; st == ODIN3_OK && id < run->n_mods; id++) {
        const odin3_module *mod = odin3_module_get(run->design, (odin3_module_id){id});
        st = odin3_strbuf_appendf(body, "%s\n    {\"id\": %u, \"name\": ", id == 1 ? "" : ",", id);
        st = st == ODIN3_OK ? gv_put_str(body, gv_str(run, odin3_module_name(mod))) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr(", \"graphs\": [\n      "))
                            : st;
        if (st == ODIN3_OK && run->mods[id].len > 0) {
            st = odin3_strbuf_append(body, (odin3_bytes){run->mods[id].data, run->mods[id].len});
        }
        st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("\n    ]}")) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("\n  ]\n}\n")) : st;
}

static odin3_status run_kind(gv_run *run, const gv_kind *kind, const char *path) {
    run->n_mods = odin3_design_module_end(run->design);
    run->mods = odin3_util_calloc((size_t)run->n_mods * sizeof *run->mods);
    run->hdl = odin3_hdl_ctx_create(run->design, "write_graph");
    if (run->mods == NULL || run->hdl == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t id = 0; id < run->n_mods; id++) {
        odin3_strbuf_init(&run->mods[id]);
    }
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    odin3_status st = kind->fn(run);
    st = st == ODIN3_OK ? assemble(run, kind->name, &body) : st;
    st = st == ODIN3_OK ? save_file(path, &body) : st;
    odin3_strbuf_free(&body);
    return st;
}

static void run_free(gv_run *run) {
    for (uint32_t id = 0; run->mods != NULL && id < run->n_mods; id++) {
        odin3_strbuf_free(&run->mods[id]);
    }
    odin3_util_free(run->mods);
    odin3_hdl_ctx_destroy(run->hdl);
}

static odin3_status write_graph_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)ctx;
    (void)user;
    odin3_bytes word = odin3_pass_arg_next(&args);
    const gv_kind *kind = word.ptr != NULL ? find_kind(word) : NULL;
    odin3_bytes file = odin3_pass_arg_next(&args);
    odin3_bytes extra = odin3_pass_arg_next(&args);
    if (kind == NULL || file.ptr == NULL || file.len == 0 || extra.ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s", k_usage);
        return ODIN3_ERR_INVALID_ARG;
    }
    char path[PATH_BUF];
    if (file.len >= sizeof path) {
        odin3_log(ODIN3_LOG_ERROR, "write_graph: file name too long");
        return ODIN3_ERR_INVALID_ARG;
    }
    memcpy(path, file.ptr, file.len);
    path[file.len] = '\0';
    gv_run run;
    memset(&run, 0, sizeof run);
    run.design = design;
    odin3_status st = run_kind(&run, kind, path);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "write_graph: %s written to %s", kind->name, path);
    }
    run_free(&run);
    return st;
}

const odin3_pass_def odin3_pass_write_graph = {
    "write_graph",
    "write_graph stg|cfg|ddg FILE: graph views as JSON (schema odin3-graph): FSM state "
    "transition graph, process control flow graph, signal data dependency graph",
    write_graph_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
