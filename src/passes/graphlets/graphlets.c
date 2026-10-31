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

enum {
    DECIMAL = 10,
    KIB = 1024,
    SCHEMA_VERSION = 1,
    DEFAULT_SIZE = 5,
    SMALL_SIZE = 4,
    MODE_LEN = 32,
    CTRL_LIMIT = 0x20,
    EDGE_BYTES = 24
};
static const double k_us_per_ms = 1000.0;

static const char k_usage[] =
    "graphlets: usage: graphlets [--directed] [--size 4|5] [--exclude-global] [--global-fanout F] "
    "[--focus F] [--cells FILE] [--partition KEY] [--halo 0|1] [--per-node] [--out FILE] "
    "[--edges-out FILE] [--max-nodes N] [--force] [--max-work W] | --compare A B "
    "[--calibration C] [--out FILE]";

/* --- options --------------------------------------------------------------------------------- */

static void opts_init(gl_opts *ob) {
    memset(ob, 0, sizeof *ob);
    ob->size = DEFAULT_SIZE;
    ob->global_fanout = GL_DEFAULT_FANOUT;
    ob->max_nodes = GL_DEFAULT_MAX_NODES;
    ob->max_work = GL_DEFAULT_MAX_WORK;
    odin3_strbuf_init(&ob->out);
    odin3_strbuf_init(&ob->partition);
    odin3_strbuf_init(&ob->edges_out);
    odin3_strbuf_init(&ob->cells);
    odin3_strbuf_init(&ob->calibration);
    odin3_strbuf_init(&ob->cmp_a);
    odin3_strbuf_init(&ob->cmp_b);
    for (unsigned ii = 0; ii < GL_MAX_FOCUS; ii++) {
        odin3_strbuf_init(&ob->focus[ii]);
    }
}

static void opts_free(gl_opts *ob) {
    odin3_strbuf_free(&ob->out);
    odin3_strbuf_free(&ob->partition);
    odin3_strbuf_free(&ob->edges_out);
    odin3_strbuf_free(&ob->cells);
    odin3_strbuf_free(&ob->calibration);
    odin3_strbuf_free(&ob->cmp_a);
    odin3_strbuf_free(&ob->cmp_b);
    for (unsigned ii = 0; ii < GL_MAX_FOCUS; ii++) {
        odin3_strbuf_free(&ob->focus[ii]);
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
    char text[MODE_LEN] = {0};
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
static int text_option(odin3_bytes word, odin3_bytes *rest, gl_opts *ob, odin3_status *st) {
    struct {
        const char *name;
        odin3_strbuf *dst;
    } const table[] = {{"--out", &ob->out},
                       {"--cells", &ob->cells},
                       {"--partition", &ob->partition},
                       {"--edges-out", &ob->edges_out},
                       {"--calibration", &ob->calibration}};
    for (size_t ii = 0; ii < sizeof table / sizeof table[0]; ii++) {
        if (odin3_pass_arg_is(word, table[ii].name)) {
            *st = next_text(rest, table[ii].dst, "an option is missing its value");
            return 1;
        }
    }
    return -1;
}

static odin3_status focus_option(odin3_bytes *rest, gl_opts *ob) {
    odin3_bytes value = odin3_pass_arg_next(rest);
    if (value.ptr == NULL || value.len == 0 || ob->n_focus == GL_MAX_FOCUS) {
        return bad_option("--focus needs a value (at most 8 foci)");
    }
    unsigned ii = ob->n_focus++;
    return odin3_dot_focus_parse(value, &ob->focus[ii], &ob->focus_kind[ii]);
}

static odin3_status small_option(odin3_bytes word, odin3_bytes *rest, gl_opts *ob, bool *used) {
    unsigned long long num = 0;
    *used = true;
    if (odin3_pass_arg_is(word, "--size")) {
        bool ok = next_number(rest, &num) && (num == SMALL_SIZE || num == DEFAULT_SIZE);
        ob->size = (unsigned)num;
        return ok ? ODIN3_OK : bad_option("--size expects 4 or 5");
    }
    if (odin3_pass_arg_is(word, "--halo")) {
        bool ok = next_number(rest, &num) && num <= 1;
        ob->halo = (unsigned)num;
        return ok ? ODIN3_OK : bad_option("--halo expects 0 or 1");
    }
    *used = false;
    return ODIN3_OK;
}

static odin3_status number_option(odin3_bytes word, odin3_bytes *rest, gl_opts *ob, bool *used) {
    unsigned long long num = 0;
    odin3_status st = small_option(word, rest, ob, used);
    if (*used || st != ODIN3_OK) {
        return st;
    }
    *used = true;
    bool fanout = odin3_pass_arg_is(word, "--global-fanout");
    bool nodes = odin3_pass_arg_is(word, "--max-nodes");
    if (!fanout && !nodes && !odin3_pass_arg_is(word, "--max-work")) {
        *used = false;
        return ODIN3_OK;
    }
    bool ok = next_number(rest, &num) && num > 0;
    ob->global_fanout = fanout ? (uint32_t)num : ob->global_fanout;
    ob->max_nodes = nodes ? (size_t)num : ob->max_nodes;
    ob->max_work = !fanout && !nodes ? num : ob->max_work;
    return ok ? ODIN3_OK : bad_option("a numeric option expects a positive number");
}

static odin3_status flag_option(odin3_bytes word, gl_opts *ob, bool *used) {
    *used = true;
    if (odin3_pass_arg_is(word, "--directed")) {
        ob->directed = true;
    } else if (odin3_pass_arg_is(word, "--exclude-global")) {
        ob->exclude_global = true;
    } else if (odin3_pass_arg_is(word, "--per-node")) {
        ob->per_node = true;
    } else if (odin3_pass_arg_is(word, "--force")) {
        ob->force = true;
    } else {
        *used = false;
    }
    return ODIN3_OK;
}

static odin3_status parse_one(odin3_bytes word, odin3_bytes *rest, gl_opts *ob) {
    bool used = false;
    odin3_status st = flag_option(word, ob, &used);
    st = !used && st == ODIN3_OK ? number_option(word, rest, ob, &used) : st;
    if (used || st != ODIN3_OK) {
        return st;
    }
    if (odin3_pass_arg_is(word, "--focus")) {
        return focus_option(rest, ob);
    }
    if (odin3_pass_arg_is(word, "--compare")) {
        st = next_text(rest, &ob->cmp_a, "--compare needs two files");
        return st == ODIN3_OK ? next_text(rest, &ob->cmp_b, "--compare needs two files") : st;
    }
    odin3_status text_st = ODIN3_OK;
    if (text_option(word, rest, ob, &text_st) > 0) {
        return text_st;
    }
    return bad_option("unknown option");
}

static odin3_status parse_args(odin3_bytes args, gl_opts *ob) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        odin3_status st = parse_one(word, &args, ob);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- JSON output ------------------------------------------------------------------------------ */

static odin3_status put_escaped(odin3_strbuf *out, const char *text) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (const char *ps = text; st == ODIN3_OK && *ps != '\0'; ps++) {
        unsigned char cd = (unsigned char)*ps;
        if (cd == '"' || cd == '\\') {
            st = odin3_strbuf_appendf(out, "\\%c", cd);
        } else if (cd < CTRL_LIMIT) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("?"));
        } else {
            st = odin3_strbuf_appendf(out, "%c", cd);
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"")) : st;
}

static const char *mode_text(const gl_opts *ob, char *buf, size_t cap) {
    (void)snprintf(buf, cap, "%s-%u", ob->directed ? "directed" : "undirected",
                   ob->directed ? GLET_MAX_DIR_SIZE : ob->size);
    return buf;
}

static odin3_status put_totals(odin3_strbuf *out, const glet_sig *sig) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\"orbit_totals\": ["));
    for (uint32_t jj = 0; st == ODIN3_OK && jj < sig->norb; jj++) {
        st = odin3_strbuf_appendf(out, "%s%lld", jj != 0 ? ", " : "", (long long)sig->totals[jj]);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("], ")) : st;
}

static odin3_status put_sig_json(odin3_strbuf *out, const glet_sig *sig) {
    odin3_status st = put_totals(out, sig);
    st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"gdd\": [")) : st;
    for (uint32_t jj = 0; st == ODIN3_OK && jj < sig->norb; jj++) {
        st = odin3_strbuf_appendf(out, "%s[", jj != 0 ? ", " : "");
        for (uint32_t ii = sig->off[jj]; st == ODIN3_OK && ii < sig->off[jj + 1]; ii++) {
            st = odin3_strbuf_appendf(out, "%s[%lld, %llu]", ii != sig->off[jj] ? ", " : "",
                                      (long long)sig->deg[ii], (unsigned long long)sig->num[ii]);
        }
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
}

static odin3_status put_row(odin3_strbuf *out, const int64_t *row, uint32_t norb) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(", \"v\": ["));
    for (uint32_t jj = 0; st == ODIN3_OK && jj < norb; jj++) {
        st = odin3_strbuf_appendf(out, "%s%lld", jj != 0 ? ", " : "", (long long)row[jj]);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]}")) : st;
}

/* One node vector line. */
static odin3_status put_node(odin3_strbuf *out, const odin3_module *mod, uint32_t id,
                             const int64_t *row, uint32_t norb) {
    const odin3_design *design = odin3_module_design(mod);
    const odin3_celltype_def *def =
        odin3_celltype_get(design, odin3_node_type(mod, (odin3_node_id){id}));
    uint32_t name = odin3_node_name(mod, (odin3_node_id){id});
    const char *kind = def != NULL && def->gran == ODIN3_GRAN_PORT ? "port" : "cell";
    odin3_status st = odin3_strbuf_appendf(out, "{\"id\": %u, \"name\": ", (unsigned)id);
    st =
        st == ODIN3_OK
            ? put_escaped(out, name != 0 ? odin3_strtab_get(odin3_design_strtab(design), name) : "")
            : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, ", \"kind\": \"%s\", \"type\": ", kind) : st;
    st = st == ODIN3_OK ? put_escaped(out, def != NULL ? def->name : "") : st;
    return st == ODIN3_OK ? put_row(out, row, norb) : st;
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

/* The halo: nodes one hop outside the members that an edge joins to a member. */
static void job_halo(gl_job *job) {
    const glet_edge *edges = job->gm->edges.data;
    for (size_t ii = 0; ii < job->gm->edges.len; ii++) {
        uint32_t src = edges[ii].from;
        uint32_t dst = edges[ii].to;
        bool src_in = job->dense[src] < job->n_members;
        bool dst_in = job->dense[dst] < job->n_members;
        uint32_t add = 0;
        if (src_in && job->dense[dst] == UINT32_MAX) {
            add = dst;
        } else if (dst_in && job->dense[src] == UINT32_MAX) {
            add = src;
        }
        if (add != 0) {
            job->ids[job->n] = add;
            job->dense[add] = job->n++;
        }
    }
}

static void job_nodes(gl_job *job) {
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
    if (job->opts->halo > 0) {
        job_halo(job);
    }
}

static odin3_status job_edges(const gl_job *job, odin3_vec *sub) {
    const glet_edge *edges = job->gm->edges.data;
    for (size_t ii = 0; ii < job->gm->edges.len; ii++) {
        uint32_t xa = job->dense[edges[ii].from];
        uint32_t xb = job->dense[edges[ii].to];
        if (xa == UINT32_MAX || xb == UINT32_MAX) {
            continue;
        }
        glet_edge *slot = odin3_vec_push(sub);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (glet_edge){xa, xb};
    }
    return ODIN3_OK;
}

static odin3_status write_graph_file(const glet_graph *gr, const char *path) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: cannot write '%s'", path);
        return ODIN3_ERR_IO;
    }
    uint64_t mt = 0;
    for (uint64_t ed = 0; ed < gr->off[gr->n]; ed++) {
        mt += (gr->flags[ed] & GLET_DIR_OUT) != 0 ? 1U : 0U;
    }
    (void)fprintf(fp, "%u %llu\n", (unsigned)gr->n, (unsigned long long)mt);
    for (uint32_t un = 0; un < gr->n; un++) {
        for (uint32_t ed = gr->off[un]; ed < gr->off[un + 1]; ed++) {
            if ((gr->flags[ed] & GLET_DIR_OUT) != 0) {
                (void)fprintf(fp, "%u %u\n", (unsigned)un, (unsigned)gr->adj[ed]);
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
    glet_graph gr;
} gl_counted;

static odin3_status job_count(gl_job *job, gl_result *res, gl_counted *cc, odin3_strbuf *body,
                              bool first) {
    odin3_vec sub;
    odin3_vec_init(&sub, sizeof(glet_edge));
    odin3_status st = job_edges(job, &sub);
    st = st == ODIN3_OK ? glet_graph_build(&cc->gr, job->n, sub.data, sub.len) : st;
    uint64_t extra = (uint64_t)job->n * job->norb * sizeof(int64_t) + sub.len * EDGE_BYTES;
    st = st == ODIN3_OK ? odin3_budget_check(job->ctx, job->design, extra) : st;
    cc->counts = st == ODIN3_OK
                     ? odin3_util_calloc(((size_t)job->n + 1) * job->norb * sizeof(int64_t))
                     : NULL;
    st = st == ODIN3_OK && cc->counts == NULL ? ODIN3_ERR_NO_MEMORY : st;
    glet_spec spec = {job->opts->directed, job->opts->size, job->opts->max_work};
    st = st == ODIN3_OK ? glet_count(&cc->gr, &spec, cc->counts, &res->work) : st;
    if (st == ODIN3_OK && first && job->opts->edges_out.len > 0) {
        st = write_graph_file(&cc->gr, job->opts->edges_out.data);
    }
    res->edges = cc->gr.m;
    res->self_loops = cc->gr.self_loops;
    (void)body;
    odin3_vec_free(&sub);
    return st;
}

static odin3_status put_vectors(const gl_job *job, const gl_counted *cc, odin3_strbuf *body) {
    odin3_status st = odin3_strbuf_append(body, odin3_bytes_cstr(", \"node_vectors\": [\n"));
    for (uint32_t ii = 0; st == ODIN3_OK && ii < job->n_members; ii++) {
        st = odin3_strbuf_appendf(body, "%s      ", ii != 0 ? ",\n" : "");
        st = st == ODIN3_OK ? put_node(body, job->gm->mod, job->ids[ii],
                                       cc->counts + (size_t)ii * job->norb, job->norb)
                            : st;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("\n    ]")) : st;
}

static odin3_status put_group_head(const gl_job *job, const gl_result *res, odin3_strbuf *body) {
    const odin3_strtab *tab = odin3_design_strtab(job->design);
    odin3_status st = odin3_strbuf_append(body, odin3_bytes_cstr("    {\"name\": "));
    st = st == ODIN3_OK ? put_escaped(body, job->grp->name.data) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr(", \"module\": ")) : st;
    st = st == ODIN3_OK ? put_escaped(body, odin3_strtab_get(tab, odin3_module_name(job->gm->mod)))
                        : st;
    return st == ODIN3_OK
               ? odin3_strbuf_appendf(
                     body,
                     ", \"source\": \"%s\", \"nodes\": %u, \"edges\": %llu, \"self_loops\": %llu, "
                     "\"global_nets\": %llu, \"halo\": %u, \"reported\": %u, \"work\": %llu, "
                     "\"us\": %lld, ",
                     job->grp->source, (unsigned)job->n, (unsigned long long)res->edges,
                     (unsigned long long)res->self_loops, (unsigned long long)job->gm->global_nets,
                     job->opts->halo, (unsigned)job->n_members, (unsigned long long)res->work,
                     (long long)(res->ms * k_us_per_ms))
               : st;
}

static odin3_status job_output(const gl_job *job, const gl_result *res, const gl_counted *cc,
                               odin3_strbuf *body) {
    odin3_status st = put_group_head(job, res, body);
    st = st == ODIN3_OK ? put_sig_json(body, &res->sig) : st;
    st = st == ODIN3_OK && job->opts->per_node ? put_vectors(job, cc, body) : st;
    return st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("}")) : st;
}

/* --- attributes ----------------------------------------------------------------------------------
 */

static odin3_status set_int(odin3_design *design, odin3_module *mod, const char *key, int64_t vn) {
    uint32_t k = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &k);
    odin3_value val = odin3_value_int(vn);
    return st == ODIN3_OK
               ? odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, k,
                                &val)
               : st;
}

static odin3_status set_text(odin3_design *design, odin3_module *mod, const char *key,
                             const char *text) {
    uint32_t k = 0;
    uint32_t sz = 0;
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &k);
    st = st == ODIN3_OK ? odin3_design_intern(design, odin3_bytes_cstr(text), &sz) : st;
    odin3_value val = {ODIN3_VAL_STRING, 0, NULL, 0, sz, 0};
    return st == ODIN3_OK
               ? odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, k,
                                &val)
               : st;
}

static odin3_status write_attrs(const gl_job *job, const gl_result *res, const gl_opts *ob) {
    odin3_module *mod = job->gm->mod;
    char mode[MODE_LEN];
    odin3_strbuf totals;
    odin3_strbuf_init(&totals);
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < res->sig.norb; j++) {
        st = odin3_strbuf_appendf(&totals, "%s%lld", j != 0 ? "," : "",
                                  (long long)res->sig.totals[j]);
    }
    st = st == ODIN3_OK
             ? set_text(job->design, mod, "odin3:glet_mode", mode_text(ob, mode, sizeof mode))
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
    const gl_opts *ob = job->opts;
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
    for (uint32_t ii = 0; st == ODIN3_OK && ii < job->n_members; ii++) {
        reported[ii] = 1;
    }
    st = st == ODIN3_OK
             ? glet_sig_make(&res->sig, &(glet_rows){cc.counts, job->n, job->norb}, reported)
             : st;
    res->ms = odin3_now_ms() - t0;
    res->n_nodes = job->n;
    res->reported = job->n_members;
    st = st == ODIN3_OK ? job_output(job, res, &cc, body) : st;
    if (st == ODIN3_OK && job->grp->whole && ob->halo == 0) {
        st = write_attrs(job, res, ob);
    }
    odin3_util_free(reported);
    odin3_util_free(cc.counts);
    glet_graph_free(&cc.gr);
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
    if (st == ODIN3_OK) {
        job_nodes(job);
    }
    return st;
}

typedef struct gl_pairs {
    gl_result *res;
    size_t count;
    const gl_cal *cal;
    const char *mode;
} gl_pairs;

static odin3_status compare_from(const gl_pairs *pairs, size_t xa, odin3_strbuf *body,
                                 bool *first) {
    odin3_status st = ODIN3_OK;
    for (size_t xb = xa + 1; st == ODIN3_OK && xb < pairs->count; xb++) {
        st = odin3_strbuf_append(body, odin3_bytes_cstr(*first ? "\n    " : ",\n    "));
        *first = false;
        gl_cmp_in in = {pairs->res[xa].name.data,
                        pairs->res[xb].name.data,
                        &pairs->res[xa].sig,
                        &pairs->res[xb].sig,
                        pairs->cal,
                        pairs->mode};
        st = st == ODIN3_OK ? gl_compare_json(body, &in, true) : st;
    }
    return st;
}

static odin3_status compare_all(const gl_opts *ob, gl_result *res, size_t n, const char *mode,
                                odin3_strbuf *body) {
    gl_cal *cal = ob->calibration.len > 0 ? gl_cal_load(ob->calibration.data) : NULL;
    odin3_status st = ob->calibration.len > 0 && cal == NULL ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
    gl_pairs pairs = {res, n, cal, mode};
    bool first = true;
    for (size_t xa = 0; st == ODIN3_OK && xa < n; xa++) {
        st = compare_from(&pairs, xa, body, &first);
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
    for (size_t ii = 0; ii < n; ii++) {
        odin3_strbuf_free(&res[ii].name);
        glet_sig_free(&res[ii].sig);
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

static void log_summary(const gl_opts *ob, size_t groups, const gl_totals *tot) {
    char mode[MODE_LEN];
    struct rusage ru;
    (void)getrusage(RUSAGE_SELF, &ru);
    odin3_log(ODIN3_LOG_INFO,
              "graphlets: %zu groups, %llu nodes, %llu edges, mode %s, %llu global nets, "
              "%.0f ms, %ld MiB peak",
              groups, (unsigned long long)tot->nodes, (unsigned long long)tot->edges,
              mode_text(ob, mode, sizeof mode), (unsigned long long)tot->global, tot->ms,
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

static odin3_status run_one(const gl_run *run, size_t ii, odin3_strbuf *body, gl_totals *tot) {
    const gl_opts *ob = run->opts;
    gl_result *res = &run->res[ii];
    gl_job job = {ob, run->ctx, run->design, NULL, NULL, 0, NULL, NULL, 0, 0};
    job.norb = glet_orbit_count(ob->directed, ob->directed ? GLET_MAX_DIR_SIZE : ob->size);
    odin3_strbuf_init(&res->name);
    odin3_status st =
        odin3_strbuf_append(&res->name, odin3_bytes_cstr(run->groups->items[ii].name.data));
    st = st == ODIN3_OK ? open_group(&job, run->mods, &run->groups->items[ii]) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr(ii != 0 ? ",\n" : "")) : st;
    st = st == ODIN3_OK ? run_group(&job, res, body, ii == 0) : st;
    tot->nodes += res->n_nodes;
    tot->edges += res->edges;
    tot->global = job.gm != NULL ? job.gm->global_nets : tot->global;
    tot->ms += res->ms;
    odin3_util_free(job.dense);
    odin3_util_free(job.ids);
    return st;
}

static odin3_status run_groups(odin3_pass_ctx *ctx, odin3_design *design, const gl_opts *ob,
                               gl_groups *gs, odin3_strbuf *body) {
    uint32_t mod_end = odin3_design_module_end(design);
    gl_run run = {ctx, design, ob, gs, NULL, NULL};
    run.mods = odin3_util_calloc(((size_t)mod_end + 1) * sizeof *run.mods);
    run.res = odin3_util_calloc((gs->count + 1) * sizeof *run.res);
    odin3_status st = run.mods != NULL && run.res != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    gl_totals tot = {0, 0, 0, 0};
    for (size_t ii = 0; st == ODIN3_OK && ii < gs->count; ii++) {
        st = run_one(&run, ii, body, &tot);
    }
    char mode[MODE_LEN];
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("\n  ],\n  \"compare\": ["))
                        : st;
    st = st == ODIN3_OK && gs->count <= GL_MAX_GROUPS_COMPARED
             ? compare_all(ob, run.res, gs->count, mode_text(ob, mode, sizeof mode), body)
             : st;
    if (st == ODIN3_OK) {
        log_summary(ob, gs->count, &tot);
    }
    for (uint32_t ii = 0; run.mods != NULL && ii <= mod_end; ii++) {
        gl_mod_free(&run.mods[ii]);
    }
    odin3_util_free(run.mods);
    res_free(run.res, gs->count);
    return st;
}

static odin3_status graphlets_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)user;
    gl_opts ob;
    opts_init(&ob);
    odin3_status st = parse_args(args, &ob);
    if (st == ODIN3_OK && ob.cmp_a.len > 0) {
        st = gl_run_compare(&ob);
        opts_free(&ob);
        return st;
    }
    gl_groups gs;
    memset(&gs, 0, sizeof gs);
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    char mode[MODE_LEN];
    st = st == ODIN3_OK ? gl_groups_make(design, &ob, &gs) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(&body,
                                    "{\n  \"schema\": \"odin3-graphlets\",\n  \"version\": %d,\n  "
                                    "\"mode\": \"%s\",\n  \"n_orbits\": %u,\n  \"groups\": [\n",
                                    SCHEMA_VERSION, mode_text(&ob, mode, sizeof mode),
                                    (unsigned)glet_orbit_count(
                                        ob.directed, ob.directed ? GLET_MAX_DIR_SIZE : ob.size))
             : st;
    st = st == ODIN3_OK ? run_groups(ctx, design, &ob, &gs, &body) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(&body, odin3_bytes_cstr("]\n}\n")) : st;
    if (st == ODIN3_OK && ob.out.len > 0) {
        st = save_file(ob.out.data, &body);
    }
    odin3_strbuf_free(&body);
    gl_groups_free(&gs);
    opts_free(&ob);
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
