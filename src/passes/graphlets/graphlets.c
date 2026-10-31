/*
 * graphlets.c — the graphlets pass (spec docs/specs/2026-10-30-graphlets-design.md): a thin
 * adapter from the netlist to the generic counter in src/graph. `graphlets [--directed]
 * [--size 4|5] [--exclude-global] [--global-fanout F] [--focus F]... [--cells FILE]
 * [--partition KEY] [--halo 0|1] [--per-node] [--out FILE] [--edges-out FILE] [--max-nodes N]
 * [--force] [--max-work W]` and `graphlets --compare A[#NAME] B[#NAME] [--calibration C]
 * [--out FILE]`. Counts node orbits of each group's induced subgraph, writes module attributes
 * for whole-module groups and a versioned JSON file.
 */
#include "ir/celltype.h"
#include "ir/value.h"
#include "passes/budget.h"
#include "passes/builtin.h"
#include "passes/graphlets/gl_int.h"
#include "util/alloc.h"
#include "util/clock.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

enum { DECIMAL = 10, KIB = 1024, SCHEMA_VERSION = 1 };

static const char k_usage[] =
    "graphlets: usage: graphlets [--directed] [--size 4|5] [--exclude-global] [--global-fanout F] "
    "[--focus F] [--cells FILE] [--partition KEY] [--halo 0|1] [--per-node] [--out FILE] "
    "[--edges-out FILE] [--max-nodes N] [--force] [--max-work W] | --compare A B "
    "[--calibration C] [--out FILE]";

/* --- options --------------------------------------------------------------------------------- */

static void opts_init(gl_opts *o) {
    memset(o, 0, sizeof *o);
    o->size = 5;
    o->global_fanout = GL_DEFAULT_FANOUT;
    o->max_nodes = GL_DEFAULT_MAX_NODES;
    o->max_work = GL_DEFAULT_MAX_WORK;
    odin3_strbuf_init(&o->out);
    odin3_strbuf_init(&o->partition);
    odin3_strbuf_init(&o->edges_out);
    odin3_strbuf_init(&o->cells);
    odin3_strbuf_init(&o->calibration);
    odin3_strbuf_init(&o->cmp_a);
    odin3_strbuf_init(&o->cmp_b);
    for (unsigned i = 0; i < GL_MAX_FOCUS; i++) {
        odin3_strbuf_init(&o->focus[i]);
    }
}

static void opts_free(gl_opts *o) {
    odin3_strbuf_free(&o->out);
    odin3_strbuf_free(&o->partition);
    odin3_strbuf_free(&o->edges_out);
    odin3_strbuf_free(&o->cells);
    odin3_strbuf_free(&o->calibration);
    odin3_strbuf_free(&o->cmp_a);
    odin3_strbuf_free(&o->cmp_b);
    for (unsigned i = 0; i < GL_MAX_FOCUS; i++) {
        odin3_strbuf_free(&o->focus[i]);
    }
}

static odin3_status bad_option(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "graphlets: %s", why);
    odin3_log(ODIN3_LOG_ERROR, "%s", k_usage);
    return ODIN3_ERR_INVALID_ARG;
}

/* The next word as an unsigned number; false when it is not one. */
static bool next_number(odin3_bytes *rest, unsigned long long *num) {
    odin3_bytes word = odin3_pass_arg_next(rest);
    char text[32] = {0};
    if (word.ptr == NULL || word.len == 0 || word.len >= sizeof text) {
        return false;
    }
    memcpy(text, word.ptr, word.len);
    char *end = NULL;
    errno = 0;
    *num = strtoull(text, &end, DECIMAL);
    return text[0] >= '0' && text[0] <= '9' && *end == '\0' && errno == 0;
}

static odin3_status next_text(odin3_bytes *rest, odin3_strbuf *dst, const char *what) {
    odin3_bytes word = odin3_pass_arg_next(rest);
    if (word.ptr == NULL || word.len == 0) {
        return bad_option(what);
    }
    odin3_strbuf_clear(dst);
    return odin3_strbuf_append(dst, word);
}

/* Options that take a text value. Returns -1 when word is not one of them. */
static int text_option(odin3_bytes word, odin3_bytes *rest, gl_opts *o, odin3_status *st) {
    struct {
        const char *name;
        odin3_strbuf *dst;
    } const table[] = {{"--out", &o->out},
                       {"--cells", &o->cells},
                       {"--partition", &o->partition},
                       {"--edges-out", &o->edges_out},
                       {"--calibration", &o->calibration}};
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (odin3_pass_arg_is(word, table[i].name)) {
            *st = next_text(rest, table[i].dst, "an option is missing its value");
            return 1;
        }
    }
    return -1;
}

static odin3_status focus_option(odin3_bytes *rest, gl_opts *o) {
    odin3_bytes value = odin3_pass_arg_next(rest);
    if (value.ptr == NULL || value.len == 0 || o->n_focus == GL_MAX_FOCUS) {
        return bad_option("--focus needs a value (at most 8 foci)");
    }
    unsigned i = o->n_focus++;
    return odin3_dot_focus_parse(value, &o->focus[i], &o->focus_kind[i]);
}

static odin3_status number_option(odin3_bytes word, odin3_bytes *rest, gl_opts *o, bool *used) {
    unsigned long long num = 0;
    *used = true;
    if (odin3_pass_arg_is(word, "--size")) {
        bool ok = next_number(rest, &num) && (num == 4 || num == 5);
        o->size = (unsigned)num;
        return ok ? ODIN3_OK : bad_option("--size expects 4 or 5");
    }
    if (odin3_pass_arg_is(word, "--halo")) {
        bool ok = next_number(rest, &num) && num <= 1;
        o->halo = (unsigned)num;
        return ok ? ODIN3_OK : bad_option("--halo expects 0 or 1");
    }
    if (odin3_pass_arg_is(word, "--global-fanout") || odin3_pass_arg_is(word, "--max-nodes") ||
        odin3_pass_arg_is(word, "--max-work")) {
        bool ok = next_number(rest, &num) && num > 0;
        if (odin3_pass_arg_is(word, "--global-fanout")) {
            o->global_fanout = (uint32_t)num;
        } else if (odin3_pass_arg_is(word, "--max-nodes")) {
            o->max_nodes = (size_t)num;
        } else {
            o->max_work = num;
        }
        return ok ? ODIN3_OK : bad_option("a numeric option expects a positive number");
    }
    *used = false;
    return ODIN3_OK;
}

static odin3_status flag_option(odin3_bytes word, gl_opts *o, bool *used) {
    *used = true;
    if (odin3_pass_arg_is(word, "--directed")) {
        o->directed = true;
    } else if (odin3_pass_arg_is(word, "--exclude-global")) {
        o->exclude_global = true;
    } else if (odin3_pass_arg_is(word, "--per-node")) {
        o->per_node = true;
    } else if (odin3_pass_arg_is(word, "--force")) {
        o->force = true;
    } else {
        *used = false;
    }
    return ODIN3_OK;
}

static odin3_status parse_one(odin3_bytes word, odin3_bytes *rest, gl_opts *o) {
    bool used = false;
    odin3_status st = flag_option(word, o, &used);
    st = !used && st == ODIN3_OK ? number_option(word, rest, o, &used) : st;
    if (used || st != ODIN3_OK) {
        return st;
    }
    if (odin3_pass_arg_is(word, "--focus")) {
        return focus_option(rest, o);
    }
    if (odin3_pass_arg_is(word, "--compare")) {
        st = next_text(rest, &o->cmp_a, "--compare needs two files");
        return st == ODIN3_OK ? next_text(rest, &o->cmp_b, "--compare needs two files") : st;
    }
    odin3_status text_st = ODIN3_OK;
    if (text_option(word, rest, o, &text_st) > 0) {
        return text_st;
    }
    return bad_option("unknown option");
}

static odin3_status parse_args(odin3_bytes args, gl_opts *o) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        odin3_status st = parse_one(word, &args, o);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- JSON output ------------------------------------------------------------------------------ */

static odin3_status put_escaped(odin3_strbuf *out, const char *text) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (const char *p = text; st == ODIN3_OK && *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            st = odin3_strbuf_appendf(out, "\\%c", c);
        } else if (c < 0x20) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("?"));
        } else {
            st = odin3_strbuf_appendf(out, "%c", c);
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"")) : st;
}

static const char *mode_text(const gl_opts *o, char *buf, size_t cap) {
    (void)snprintf(buf, cap, "%s-%u", o->directed ? "directed" : "undirected",
                   o->directed ? GLET_MAX_DIR_SIZE : o->size);
    return buf;
}

static odin3_status put_sig_json(odin3_strbuf *out, const glet_sig *sig) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\"orbit_totals\": ["));
    for (uint32_t j = 0; st == ODIN3_OK && j < sig->norb; j++) {
        st = odin3_strbuf_appendf(out, "%s%lld", j != 0 ? ", " : "", (long long)sig->totals[j]);
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("], \"gdd\": [")) : st;
    for (uint32_t j = 0; st == ODIN3_OK && j < sig->norb; j++) {
        st = odin3_strbuf_appendf(out, "%s[", j != 0 ? ", " : "");
        for (uint32_t i = sig->off[j]; st == ODIN3_OK && i < sig->off[j + 1]; i++) {
            st = odin3_strbuf_appendf(out, "%s[%lld, %llu]", i != sig->off[j] ? ", " : "",
                                      (long long)sig->deg[i], (unsigned long long)sig->num[i]);
        }
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
}

/* One node vector line. */
static odin3_status put_node(odin3_strbuf *out, const odin3_module *mod, uint32_t id,
                             const int64_t *row, uint32_t norb) {
    const odin3_design *design = odin3_module_design(mod);
    odin3_celltype_id type = odin3_node_type(mod, (odin3_node_id){id});
    const odin3_celltype_def *def = odin3_celltype_get(design, type);
    uint32_t name = odin3_node_name(mod, (odin3_node_id){id});
    const odin3_strtab *tab = odin3_design_strtab(design);
    odin3_status st = odin3_strbuf_appendf(out, "{\"id\": %u, \"name\": ", (unsigned)id);
    st = st == ODIN3_OK ? put_escaped(out, name != 0 ? odin3_strtab_get(tab, name) : "") : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(out, ", \"kind\": \"%s\", \"type\": ",
                                    def != NULL && def->gran == ODIN3_GRAN_PORT ? "port" : "cell")
             : st;
    st = st == ODIN3_OK ? put_escaped(out, def != NULL ? def->name : "") : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(", \"v\": [")) : st;
    for (uint32_t j = 0; st == ODIN3_OK && j < norb; j++) {
        st = odin3_strbuf_appendf(out, "%s%lld", j != 0 ? ", " : "", (long long)row[j]);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]}")) : st;
}

/* --- one group ---------------------------------------------------------------------------------
 */

typedef struct gl_job {
    const gl_opts *opts;
    odin3_pass_ctx *ctx;
    odin3_design *design;
    gl_mod *gm;
    const gl_group *grp;
    uint32_t norb;
    uint32_t *dense; /* node ID -> dense index, UINT32_MAX when out */
    uint32_t *ids;   /* dense index -> node ID */
    uint32_t n;      /* nodes in the counted subgraph */
    uint32_t n_members;
} gl_job;

static odin3_status job_nodes(gl_job *job) {
    const gl_mod *gm = job->gm;
    uint32_t end = odin3_module_node_end(gm->mod);
    for (uint32_t id = 0; id < end; id++) {
        job->dense[id] = UINT32_MAX;
    }
    for (uint32_t id = 1; id < end; id++) {
        if (job->grp->mask[id] != 0 && gm->eligible[id] != 0) {
            job->ids[job->n] = id;
            job->dense[id] = job->n++;
        }
    }
    job->n_members = job->n;
    const glet_edge *edges = gm->edges.data;
    for (size_t i = 0; job->opts->halo > 0 && i < gm->edges.len; i++) {
        uint32_t a = edges[i].from;
        uint32_t b = edges[i].to;
        bool a_in = job->dense[a] < job->n_members;
        bool b_in = job->dense[b] < job->n_members;
        uint32_t add =
            a_in && job->dense[b] == UINT32_MAX ? b : (b_in && job->dense[a] == UINT32_MAX ? a : 0);
        if (add != 0) {
            job->ids[job->n] = add;
            job->dense[add] = job->n++;
        }
    }
    return ODIN3_OK;
}

static odin3_status job_edges(const gl_job *job, odin3_vec *sub) {
    const glet_edge *edges = job->gm->edges.data;
    for (size_t i = 0; i < job->gm->edges.len; i++) {
        uint32_t a = job->dense[edges[i].from];
        uint32_t b = job->dense[edges[i].to];
        if (a == UINT32_MAX || b == UINT32_MAX) {
            continue;
        }
        glet_edge *slot = odin3_vec_push(sub);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (glet_edge){a, b};
    }
    return ODIN3_OK;
}

static odin3_status write_graph_file(const glet_graph *g, const char *path) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: cannot write '%s'", path);
        return ODIN3_ERR_IO;
    }
    uint64_t m = 0;
    for (uint64_t e = 0; e < g->off[g->n]; e++) {
        m += (g->flags[e] & GLET_DIR_OUT) != 0 ? 1U : 0U;
    }
    (void)fprintf(fp, "%u %llu\n", (unsigned)g->n, (unsigned long long)m);
    for (uint32_t u = 0; u < g->n; u++) {
        for (uint32_t e = g->off[u]; e < g->off[u + 1]; e++) {
            if ((g->flags[e] & GLET_DIR_OUT) != 0) {
                (void)fprintf(fp, "%u %u\n", (unsigned)u, (unsigned)g->adj[e]);
            }
        }
    }
    return fclose(fp) == 0 ? ODIN3_OK : ODIN3_ERR_IO;
}

static odin3_status group_refuse(const gl_job *job) {
    if (job->n_members > job->opts->max_nodes && !job->opts->force) {
        odin3_log(ODIN3_LOG_ERROR,
                  "graphlets: group '%s' has %u nodes, more than --max-nodes %zu; narrow it with "
                  "--focus or --cells, or pass --force",
                  job->grp->name.data, (unsigned)job->n_members, job->opts->max_nodes);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

typedef struct gl_counted {
    int64_t *counts;
    glet_graph g;
} gl_counted;

static odin3_status job_count(gl_job *job, gl_result *res, gl_counted *cc, odin3_strbuf *body,
                              bool first) {
    odin3_vec sub;
    odin3_vec_init(&sub, sizeof(glet_edge));
    odin3_status st = job_edges(job, &sub);
    st = st == ODIN3_OK ? glet_graph_build(&cc->g, job->n, sub.data, sub.len) : st;
    uint64_t extra = (uint64_t)job->n * job->norb * sizeof(int64_t) + sub.len * 24U;
    st = st == ODIN3_OK ? odin3_budget_check(job->ctx, job->design, extra) : st;
    cc->counts = st == ODIN3_OK
                     ? odin3_util_calloc(((size_t)job->n + 1) * job->norb * sizeof(int64_t))
                     : NULL;
    st = st == ODIN3_OK && cc->counts == NULL ? ODIN3_ERR_NO_MEMORY : st;
    glet_spec spec = {job->opts->directed, job->opts->size, job->opts->max_work};
    st = st == ODIN3_OK ? glet_count(&cc->g, &spec, cc->counts, &res->work) : st;
    if (st == ODIN3_OK && first && job->opts->edges_out.len > 0) {
        st = write_graph_file(&cc->g, job->opts->edges_out.data);
    }
    res->edges = cc->g.m;
    res->self_loops = cc->g.self_loops;
    (void)body;
    odin3_vec_free(&sub);
    return st;
}

static odin3_status job_output(const gl_job *job, const gl_result *res, const gl_counted *cc,
                               const gl_opts *o, odin3_strbuf *body) {
    const odin3_module *mod = job->gm->mod;
    const odin3_strtab *tab = odin3_design_strtab(job->design);
    odin3_status st = odin3_strbuf_append(body, odin3_bytes_cstr("    {\"name\": "));
    st = st == ODIN3_OK ? put_escaped(body, job->grp->name.data) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr(", \"module\": ")) : st;
    st = st == ODIN3_OK ? put_escaped(body, odin3_strtab_get(tab, odin3_module_name(mod))) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(
                   body,
                   ", \"source\": \"%s\", \"nodes\": %u, \"edges\": %llu, \"self_loops\": %llu, "
                   "\"global_nets\": %llu, \"halo\": %u, \"reported\": %u, \"work\": %llu, "
                   "\"us\": %lld, ",
                   job->grp->source, (unsigned)job->n, (unsigned long long)res->edges,
                   (unsigned long long)res->self_loops, (unsigned long long)job->gm->global_nets,
                   o->halo, (unsigned)job->n_members, (unsigned long long)res->work,
                   (long long)(res->ms * 1000.0))
             : st;
    st = st == ODIN3_OK ? put_sig_json(body, &res->sig) : st;
    if (st == ODIN3_OK && o->per_node) {
        st = odin3_strbuf_append(body, odin3_bytes_cstr(", \"node_vectors\": [\n"));
        for (uint32_t i = 0; st == ODIN3_OK && i < job->n_members; i++) {
            st = odin3_strbuf_appendf(body, "%s      ", i != 0 ? ",\n" : "");
            st = st == ODIN3_OK ? put_node(body, mod, job->ids[i],
                                           cc->counts + (size_t)i * job->norb, job->norb)
                                : st;
        }
        st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("\n    ]")) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("}")) : st;
}

/* --- attributes ----------------------------------------------------------------------------------
 */

static odin3_status set_int(odin3_design *design, odin3_module *mod, const char *key, int64_t v) {
    uint32_t k = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &k);
    odin3_value val = odin3_value_int(v);
    return st == ODIN3_OK
               ? odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, k,
                                &val)
               : st;
}

static odin3_status set_text(odin3_design *design, odin3_module *mod, const char *key,
                             const char *text) {
    uint32_t k = 0;
    uint32_t s = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &k);
    st = st == ODIN3_OK ? odin3_design_intern(design, odin3_bytes_cstr(text), &s) : st;
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, s, 0};
    return st == ODIN3_OK
               ? odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, k,
                                &val)
               : st;
}

static odin3_status write_attrs(const gl_job *job, const gl_result *res, const gl_opts *o) {
    odin3_module *mod = job->gm->mod;
    char mode[32];
    odin3_strbuf totals;
    odin3_strbuf_init(&totals);
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < res->sig.norb; j++) {
        st = odin3_strbuf_appendf(&totals, "%s%lld", j != 0 ? "," : "",
                                  (long long)res->sig.totals[j]);
    }
    st = st == ODIN3_OK
             ? set_text(job->design, mod, "odin3:glet_mode", mode_text(o, mode, sizeof mode))
             : st;
    st = st == ODIN3_OK ? set_int(job->design, mod, "odin3:glet_nodes", job->n) : st;
    st = st == ODIN3_OK ? set_int(job->design, mod, "odin3:glet_edges", (int64_t)res->edges) : st;
    st = st == ODIN3_OK
             ? set_int(job->design, mod, "odin3:glet_self_loops", (int64_t)res->self_loops)
             : st;
    st = st == ODIN3_OK
             ? set_int(job->design, mod, "odin3:glet_global_nets", (int64_t)job->gm->global_nets)
             : st;
    st = st == ODIN3_OK ? set_int(job->design, mod, "odin3:glet_version", SCHEMA_VERSION) : st;
    st = st == ODIN3_OK ? set_text(job->design, mod, "odin3:glet_orbit_totals", totals.data) : st;
    odin3_strbuf_free(&totals);
    return st;
}

/* --- the run ------------------------------------------------------------------------------------
 */

static odin3_status run_group(gl_job *job, gl_result *res, odin3_strbuf *body, bool first) {
    double t0 = odin3_now_ms();
    const gl_opts *o = job->opts;
    odin3_status st = group_refuse(job);
    gl_counted cc;
    memset(&cc, 0, sizeof cc);
    uint8_t *reported = NULL;
    if (st == ODIN3_OK) {
        st = job_count(job, res, &cc, body, first);
    }
    if (st == ODIN3_OK) {
        reported = odin3_util_calloc((size_t)job->n + 1);
        st = reported != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < job->n_members; i++) {
        reported[i] = 1;
    }
    st = st == ODIN3_OK ? glet_sig_make(&res->sig, cc.counts, job->n, job->norb, reported) : st;
    res->ms = odin3_now_ms() - t0;
    res->n_nodes = job->n;
    res->reported = job->n_members;
    st = st == ODIN3_OK ? job_output(job, res, &cc, o, body) : st;
    if (st == ODIN3_OK && job->grp->whole && o->halo == 0) {
        st = write_attrs(job, res, o);
    }
    odin3_util_free(reported);
    odin3_util_free(cc.counts);
    glet_graph_free(&cc.g);
    return st;
}

static odin3_status open_group(gl_job *job, gl_mod *mods, const gl_group *grp) {
    gl_mod *gm = &mods[grp->module];
    odin3_module *mod = odin3_module_get(job->design, (odin3_module_id){grp->module});
    odin3_status st = gm->built ? ODIN3_OK : gl_mod_build(gm, mod, job->opts);
    uint32_t end = odin3_module_node_end(mod);
    job->gm = gm;
    job->grp = grp;
    job->n = 0;
    job->dense = odin3_util_calloc(((size_t)end + 1) * sizeof(uint32_t));
    job->ids = odin3_util_calloc(((size_t)end + 1) * sizeof(uint32_t));
    if (st == ODIN3_OK && (job->dense == NULL || job->ids == NULL)) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    return st == ODIN3_OK ? job_nodes(job) : st;
}

static odin3_status compare_all(const gl_opts *o, gl_result *res, size_t n, const char *mode,
                                odin3_strbuf *body) {
    gl_cal *cal = o->calibration.len > 0 ? gl_cal_load(o->calibration.data) : NULL;
    odin3_status st = o->calibration.len > 0 && cal == NULL ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
    bool first = true;
    for (size_t a = 0; st == ODIN3_OK && a < n; a++) {
        for (size_t b = a + 1; st == ODIN3_OK && b < n; b++) {
            st = odin3_strbuf_append(body, odin3_bytes_cstr(first ? "\n    " : ",\n    "));
            first = false;
            gl_cmp_in in = {
                res[a].name.data, res[b].name.data, &res[a].sig, &res[b].sig, cal, mode};
            st = st == ODIN3_OK ? gl_compare_json(body, &in, true) : st;
        }
    }
    if (st == ODIN3_OK && n > 1 && cal == NULL) {
        odin3_log(ODIN3_LOG_INFO, "graphlets: similarity is uncalibrated (no --calibration)");
    }
    gl_cal_free(cal);
    return st;
}

static odin3_status save_file(const char *path, const odin3_strbuf *text) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: cannot write '%s'", path);
        return ODIN3_ERR_IO;
    }
    bool ok = fwrite(text->data, 1, text->len, fp) == text->len;
    return (fclose(fp) == 0 && ok) ? ODIN3_OK : ODIN3_ERR_IO;
}

static void res_free(gl_result *res, size_t n) {
    for (size_t i = 0; i < n; i++) {
        odin3_strbuf_free(&res[i].name);
        glet_sig_free(&res[i].sig);
    }
    odin3_util_free(res);
}

/* What a run adds up over its groups. */
typedef struct gl_totals {
    uint64_t nodes;
    uint64_t edges;
    uint64_t global;
    double ms;
} gl_totals;

static void log_summary(const gl_opts *o, size_t groups, const gl_totals *tot) {
    char mode[32];
    struct rusage ru;
    (void)getrusage(RUSAGE_SELF, &ru);
    odin3_log(ODIN3_LOG_INFO,
              "graphlets: %zu groups, %llu nodes, %llu edges, mode %s, %llu global nets, "
              "%.0f ms, %ld MiB peak",
              groups, (unsigned long long)tot->nodes, (unsigned long long)tot->edges,
              mode_text(o, mode, sizeof mode), (unsigned long long)tot->global, tot->ms,
              ru.ru_maxrss / KIB);
}

typedef struct gl_run {
    odin3_pass_ctx *ctx;
    odin3_design *design;
    const gl_opts *opts;
    gl_groups *groups;
    gl_mod *mods;
    gl_result *res;
} gl_run;

static odin3_status run_one(const gl_run *run, size_t i, odin3_strbuf *body, gl_totals *tot) {
    const gl_opts *o = run->opts;
    gl_result *res = &run->res[i];
    gl_job job = {o, run->ctx, run->design, NULL, NULL, 0, NULL, NULL, 0, 0};
    job.norb = glet_orbit_count(o->directed, o->directed ? GLET_MAX_DIR_SIZE : o->size);
    odin3_strbuf_init(&res->name);
    odin3_status st =
        odin3_strbuf_append(&res->name, odin3_bytes_cstr(run->groups->items[i].name.data));
    st = st == ODIN3_OK ? open_group(&job, run->mods, &run->groups->items[i]) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr(i != 0 ? ",\n" : "")) : st;
    st = st == ODIN3_OK ? run_group(&job, res, body, i == 0) : st;
    tot->nodes += res->n_nodes;
    tot->edges += res->edges;
    tot->global = job.gm != NULL ? job.gm->global_nets : tot->global;
    tot->ms += res->ms;
    odin3_util_free(job.dense);
    odin3_util_free(job.ids);
    return st;
}

static odin3_status run_groups(odin3_pass_ctx *ctx, odin3_design *design, const gl_opts *o,
                               gl_groups *gs, odin3_strbuf *body) {
    uint32_t mod_end = odin3_design_module_end(design);
    gl_run run = {ctx, design, o, gs, NULL, NULL};
    run.mods = odin3_util_calloc(((size_t)mod_end + 1) * sizeof *run.mods);
    run.res = odin3_util_calloc((gs->count + 1) * sizeof *run.res);
    odin3_status st = run.mods != NULL && run.res != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    gl_totals tot = {0, 0, 0, 0};
    for (size_t i = 0; st == ODIN3_OK && i < gs->count; i++) {
        st = run_one(&run, i, body, &tot);
    }
    char mode[32];
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("\n  ],\n  \"compare\": ["))
                        : st;
    st = st == ODIN3_OK && gs->count <= GL_MAX_GROUPS_COMPARED
             ? compare_all(o, run.res, gs->count, mode_text(o, mode, sizeof mode), body)
             : st;
    if (st == ODIN3_OK) {
        log_summary(o, gs->count, &tot);
    }
    for (uint32_t i = 0; run.mods != NULL && i <= mod_end; i++) {
        gl_mod_free(&run.mods[i]);
    }
    odin3_util_free(run.mods);
    res_free(run.res, gs->count);
    return st;
}

static odin3_status graphlets_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)user;
    gl_opts o;
    opts_init(&o);
    odin3_status st = parse_args(args, &o);
    if (st == ODIN3_OK && o.cmp_a.len > 0) {
        st = gl_run_compare(&o);
        opts_free(&o);
        return st;
    }
    gl_groups gs;
    memset(&gs, 0, sizeof gs);
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    char mode[32];
    st = st == ODIN3_OK ? gl_groups_make(design, &o, &gs) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(
                   &body,
                   "{\n  \"schema\": \"odin3-graphlets\",\n  \"version\": %d,\n  "
                   "\"mode\": \"%s\",\n  \"n_orbits\": %u,\n  \"groups\": [\n",
                   SCHEMA_VERSION, mode_text(&o, mode, sizeof mode),
                   (unsigned)glet_orbit_count(o.directed, o.directed ? GLET_MAX_DIR_SIZE : o.size))
             : st;
    st = st == ODIN3_OK ? run_groups(ctx, design, &o, &gs, &body) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(&body, odin3_bytes_cstr("]\n}\n")) : st;
    if (st == ODIN3_OK && o.out.len > 0) {
        st = save_file(o.out.data, &body);
    }
    odin3_strbuf_free(&body);
    gl_groups_free(&gs);
    opts_free(&o);
    return st;
}

const odin3_pass_def odin3_pass_graphlets = {
    "graphlets",
    "graphlets [--directed] [--size 4|5] [--exclude-global] [--focus F] [--cells FILE] "
    "[--partition KEY] [--halo 0|1] [--per-node] [--out FILE] | --compare A B: node graphlet "
    "orbit counts (undirected 73 orbits, directed 129) of a module or a subgraph as module "
    "attributes odin3:glet_* and a JSON file",
    graphlets_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
