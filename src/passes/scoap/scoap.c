/*
 * scoap.c — the scoap pass (spec docs/specs/2026-10-31-scoap-design.md): SCOAP testability
 * measures of every net (CC0, CC1, CO and, with --sequential, SC0, SC1, SO), optionally the
 * signal probability from random simulation. `scoap [--sequential] [--out FILE] [--top N]
 * [--focus F]... [--sigprob V] [--seed S] [--threshold P]`. A summary per module goes into the
 * module attributes odin3:scoap_*, the per-net values into a versioned JSON file.
 */
#include "ir/celltype.h"
#include "passes/builtin.h"
#include "passes/scoap/sc_opts.h"
#include "util/alloc.h"
#include "util/clock.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SCHEMA_VERSION = 1, DECIMAL = 10, PPM = 1000000, NUM_BUF = 48, DEFAULT_SEED = 1 };
static const double k_default_threshold = 0.01;

static const char k_usage[] =
    "scoap: usage: scoap [--sequential] [--out FILE] [--top N] [--focus F] [--sigprob V] "
    "[--seed S] [--threshold P]";

/* --- options --------------------------------------------------------------------------------- */

static odin3_status bad_option(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "scoap: %s", why);
    odin3_log(ODIN3_LOG_ERROR, "%s", k_usage);
    return ODIN3_ERR_INVALID_ARG;
}

static bool next_number(odin3_bytes *rest, unsigned long long *num) {
    odin3_bytes word = odin3_pass_arg_next(rest);
    char text[NUM_BUF] = {0};
    if (word.ptr == NULL || word.len == 0 || word.len >= sizeof text) {
        return false;
    }
    memcpy(text, word.ptr, word.len);
    char *end = NULL;
    errno = 0;
    *num = strtoull(text, &end, DECIMAL);
    return text[0] >= '0' && text[0] <= '9' && *end == '\0' && errno == 0;
}

static bool next_decimal(odin3_bytes *rest, double *val) {
    odin3_bytes word = odin3_pass_arg_next(rest);
    char text[NUM_BUF] = {0};
    if (word.ptr == NULL || word.len == 0 || word.len >= sizeof text) {
        return false;
    }
    memcpy(text, word.ptr, word.len);
    char *end = NULL;
    errno = 0;
    *val = strtod(text, &end);
    return *end == '\0' && errno == 0 && *val >= 0.0 && *val <= 1.0;
}

static odin3_status number_option(odin3_bytes word, odin3_bytes *rest, sc_opts *ob, bool *used) {
    unsigned long long num = 0;
    *used = true;
    if (odin3_pass_arg_is(word, "--top") || odin3_pass_arg_is(word, "--sigprob") ||
        odin3_pass_arg_is(word, "--seed")) {
        if (!next_number(rest, &num)) {
            return bad_option("a numeric option expects a number");
        }
        ob->top_n = odin3_pass_arg_is(word, "--top") ? (uint32_t)num : ob->top_n;
        ob->vectors = odin3_pass_arg_is(word, "--sigprob") ? num : ob->vectors;
        ob->seed = odin3_pass_arg_is(word, "--seed") ? num : ob->seed;
        return ODIN3_OK;
    }
    if (odin3_pass_arg_is(word, "--threshold")) {
        return next_decimal(rest, &ob->threshold) ? ODIN3_OK
                                                  : bad_option("--threshold expects 0 to 1");
    }
    *used = false;
    return ODIN3_OK;
}

static odin3_status focus_option(odin3_bytes *rest, sc_opts *ob) {
    odin3_bytes value = odin3_pass_arg_next(rest);
    if (value.ptr == NULL || value.len == 0 || ob->gl.n_focus == GL_MAX_FOCUS) {
        return bad_option("--focus needs a value (at most 8 foci)");
    }
    unsigned ii = ob->gl.n_focus++;
    odin3_strbuf_init(&ob->gl.focus[ii]);
    return odin3_dot_focus_parse(value, &ob->gl.focus[ii], &ob->gl.focus_kind[ii]);
}

static odin3_status parse_args(odin3_bytes args, sc_opts *ob) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool used = false;
        odin3_status st = ODIN3_OK;
        if (odin3_pass_arg_is(word, "--sequential")) {
            ob->seq = true;
            used = true;
        } else if (odin3_pass_arg_is(word, "--out")) {
            odin3_bytes file = odin3_pass_arg_next(&args);
            st = file.ptr != NULL && file.len > 0 ? odin3_strbuf_append(&ob->out, file)
                                                  : bad_option("--out needs a file");
            used = true;
        } else if (odin3_pass_arg_is(word, "--focus")) {
            st = focus_option(&args, ob);
            used = true;
        } else {
            st = number_option(word, &args, ob, &used);
        }
        if (st != ODIN3_OK || !used) {
            return st != ODIN3_OK ? st : bad_option("unknown option");
        }
    }
    return ODIN3_OK;
}

static void opts_free(sc_opts *ob) {
    odin3_strbuf_free(&ob->out);
    for (unsigned ii = 0; ii < ob->gl.n_focus; ii++) {
        odin3_strbuf_free(&ob->gl.focus[ii]);
    }
}

/* --- per module output ----------------------------------------------------------------------- */

typedef struct sc_entry {
    uint32_t id;
    int32_t key;
} sc_entry;

static int entry_cmp(const void *pa, const void *pb) {
    const sc_entry *ea = pa;
    const sc_entry *eb = pb;
    if (ea->key != eb->key) {
        return ea->key < eb->key ? 1 : -1;
    }
    return ea->id < eb->id ? -1 : (ea->id > eb->id ? 1 : 0);
}

static const char *net_text(const sc_model *md, uint32_t id, char *fallback, size_t size) {
    odin3_net_id net = {id};
    uint32_t name = odin3_net_name(md->mod, net);
    if (name != 0) {
        return odin3_strtab_get(odin3_design_strtab(odin3_module_design(md->mod)), name);
    }
    (void)snprintf(fallback, size, "$n%u", (unsigned)id);
    return fallback;
}

static odin3_status put_net(odin3_strbuf *out, const sc_model *md, uint32_t id,
                            const int32_t *ppm) {
    char fallback[NUM_BUF];
    odin3_status st = odin3_strbuf_appendf(out, "{\"id\": %u, \"name\": ", (unsigned)id);
    st = st == ODIN3_OK ? gl_put_escaped(out, net_text(md, id, fallback, sizeof fallback)) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(out, ", \"cc0\": %d, \"cc1\": %d, \"co\": %d",
                                               md->cc[0][id], md->cc[1][id], md->co[id])
                        : st;
    if (st == ODIN3_OK && md->seq) {
        st = odin3_strbuf_appendf(out, ", \"sc0\": %d, \"sc1\": %d, \"so\": %d", md->sc[0][id],
                                  md->sc[1][id], md->so[id]);
    }
    if (st == ODIN3_OK && ppm != NULL) {
        st = odin3_strbuf_appendf(out, ", \"p1_ppm\": %d", ppm[id]);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("}")) : st;
}

/* The N first entries of the list as JSON ids. */
static odin3_status put_top(odin3_strbuf *out, const char *label, sc_entry *list, size_t cnt,
                            uint32_t top_n) {
    qsort(list, cnt, sizeof *list, entry_cmp);
    odin3_status st = odin3_strbuf_appendf(out, "\"%s\": [", label);
    for (size_t k = 0; st == ODIN3_OK && k < cnt && k < top_n; k++) {
        st = odin3_strbuf_appendf(out, "%s{\"id\": %u, \"value\": %d}", k != 0 ? ", " : "",
                                  (unsigned)list[k].id, list[k].key);
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
}

typedef struct sc_view {
    const sc_model *md;
    const sc_opts *opts;
    const uint8_t *sel; /* nets reported; NULL: every live net */
    const int32_t *ppm; /* NULL without --sigprob */
} sc_view;

static bool net_reported(const sc_view *vw, uint32_t id) {
    return vw->md->live[id] != 0 && (vw->sel == NULL || vw->sel[id] != 0);
}

static odin3_status put_nets(odin3_strbuf *out, const sc_view *vw) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\"nets\": ["));
    bool first = true;
    for (uint32_t id = 1; st == ODIN3_OK && id < vw->md->n_nets; id++) {
        if (!net_reported(vw, id)) {
            continue;
        }
        st = odin3_strbuf_append(out, odin3_bytes_cstr(first ? "\n      " : ",\n      "));
        st = st == ODIN3_OK ? put_net(out, vw->md, id, vw->ppm) : st;
        first = false;
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\n    ]")) : st;
}

static odin3_status put_lists(odin3_strbuf *out, const sc_view *vw) {
    const sc_model *md = vw->md;
    size_t cnt = 0;
    sc_entry *ctl = odin3_util_calloc(((size_t)md->n_nets + 1) * sizeof *ctl);
    sc_entry *obs = odin3_util_calloc(((size_t)md->n_nets + 1) * sizeof *obs);
    odin3_status st = ctl != NULL && obs != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t id = 1; st == ODIN3_OK && id < md->n_nets; id++) {
        if (net_reported(vw, id)) {
            int32_t hard = md->cc[0][id] > md->cc[1][id] ? md->cc[0][id] : md->cc[1][id];
            ctl[cnt] = (sc_entry){id, hard};
            obs[cnt++] = (sc_entry){id, md->co[id]};
        }
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, odin3_bytes_cstr(",\n    \"top\": {"));
        st = st == ODIN3_OK ? put_top(out, "hardest_to_control", ctl, cnt, vw->opts->top_n) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(", ")) : st;
        st = st == ODIN3_OK ? put_top(out, "hardest_to_observe", obs, cnt, vw->opts->top_n) : st;
        st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("}")) : st;
    }
    odin3_util_free(ctl);
    odin3_util_free(obs);
    return st;
}

static odin3_status put_rare(odin3_strbuf *out, const sc_view *vw) {
    int64_t thr = (int64_t)(vw->opts->threshold * PPM + 0.5);
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(",\n    \"rare\": ["));
    bool first = true;
    for (uint32_t id = 1; st == ODIN3_OK && id < vw->md->n_nets; id++) {
        int32_t pp = vw->ppm[id];
        int32_t low = pp < PPM - pp ? pp : PPM - pp;
        if (net_reported(vw, id) && pp >= 0 && low < thr) {
            st = odin3_strbuf_appendf(out, "%s%u", first ? "" : ", ", (unsigned)id);
            first = false;
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("]")) : st;
}

/* --- attributes ------------------------------------------------------------------------------ */

static odin3_status set_int(odin3_module *mod, const char *key, int64_t num) {
    uint32_t k = 0;
    odin3_status st = odin3_design_intern(odin3_module_design(mod), odin3_bytes_cstr(key), &k);
    odin3_value val = odin3_value_int(num);
    return st == ODIN3_OK
               ? odin3_attr_set(mod, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(mod).v}, k,
                                &val)
               : st;
}

static int32_t max_finite(const int32_t *arr, uint32_t cnt, const uint8_t *live) {
    int32_t best = 0;
    for (uint32_t id = 1; id < cnt; id++) {
        best = live[id] != 0 && arr[id] < SCOAP_INF && arr[id] > best ? arr[id] : best;
    }
    return best;
}

static uint64_t live_nets(const sc_model *md) {
    uint64_t count = 0;
    for (uint32_t id = 1; id < md->n_nets; id++) {
        count += md->live[id];
    }
    return count;
}

static odin3_status write_attrs(const sc_model *md) {
    odin3_module *mod = md->mod;
    const sc_stats *sts = &md->stats;
    int32_t max_cc = max_finite(md->cc[0], md->n_nets, md->live);
    int32_t max_cc1 = max_finite(md->cc[1], md->n_nets, md->live);
    const struct {
        const char *key;
        int64_t val;
    } table[] = {
        {"odin3:scoap_version", SCHEMA_VERSION},
        {"odin3:scoap_inf", SCOAP_INF},
        {"odin3:scoap_sequential", md->seq ? 1 : 0},
        {"odin3:scoap_nets", (int64_t)live_nets(md)},
        {"odin3:scoap_cells",
         (int64_t)(sts->cells[SC_FUNC] + sts->cells[SC_WIDE] + sts->cells[SC_FF] +
                   sts->cells[SC_SCAN] + sts->cells[SC_OPAQUE])},
        {"odin3:scoap_scan_cells", (int64_t)sts->cells[SC_SCAN]},
        {"odin3:scoap_opaque_cells", (int64_t)sts->cells[SC_OPAQUE]},
        {"odin3:scoap_wide_cells", (int64_t)sts->cells[SC_WIDE]},
        {"odin3:scoap_sweeps", sts->sweeps},
        {"odin3:scoap_converged", sts->converged ? 1 : 0},
        {"odin3:scoap_unreachable", (int64_t)sts->unreachable},
        {"odin3:scoap_max_cc", max_cc > max_cc1 ? max_cc : max_cc1},
        {"odin3:scoap_max_co", max_finite(md->co, md->n_nets, md->live)},
    };
    odin3_status st = ODIN3_OK;
    for (size_t k = 0; st == ODIN3_OK && k < sizeof table / sizeof table[0]; k++) {
        st = set_int(mod, table[k].key, table[k].val);
    }
    return st;
}

/* --- the run --------------------------------------------------------------------------------- */

static void log_opaque(const sc_model *md, const char *mname) {
    const odin3_design *design = odin3_module_design(md->mod);
    size_t cnt = md->opaque_types.len;
    for (size_t k = 0; k < cnt; k++) {
        const uint32_t *cur = odin3_vec_cat(&md->opaque_types, k);
        bool seen = false;
        for (size_t j = 0; j < k; j++) {
            seen = seen || *(const uint32_t *)odin3_vec_cat(&md->opaque_types, j) == *cur;
        }
        const odin3_celltype_def *def = odin3_celltype_get(design, (odin3_celltype_id){*cur});
        if (!seen && def != NULL) {
            odin3_log(ODIN3_LOG_INFO, "scoap: module %s: opaque cell type %s (outputs as inputs)",
                      mname, def->name);
        }
    }
}

typedef struct sc_run {
    odin3_design *design;
    const sc_opts *opts;
    gl_groups groups;
    bool use_groups;
    odin3_module_id top;
    int32_t *ppm;
    uint64_t nets, cells, unreachable;
    uint32_t sweeps, modules;
} sc_run;

/* Nets touched by the focus groups of module id; NULL (not an error) without --focus. */
static uint8_t *focus_nets(const sc_run *run, uint32_t id, const odin3_module *mod) {
    if (!run->use_groups) {
        return NULL;
    }
    uint8_t *sel = odin3_util_calloc((size_t)odin3_module_net_end(mod) + 1);
    for (size_t g = 0; sel != NULL && g < run->groups.count; g++) {
        const gl_group *grp = &run->groups.items[g];
        for (uint32_t node = 1; grp->module == id && node < odin3_module_node_end(mod); node++) {
            odin3_pinslice pins = odin3_node_pins(mod, (odin3_node_id){node});
            for (uint32_t k = 0; grp->mask[node] != 0 && k < pins.count; k++) {
                odin3_net_id net = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + k});
                sel[net.v] = 1;
            }
        }
    }
    return sel;
}

static odin3_status put_module(odin3_strbuf *body, sc_run *run, uint32_t id, bool first) {
    odin3_module *mod = odin3_module_get(run->design, (odin3_module_id){id});
    const char *mname = odin3_strtab_get(odin3_design_strtab(run->design), odin3_module_name(mod));
    sc_model md;
    odin3_status st = sc_model_build(&md, mod, run->opts->seq);
    if (st == ODIN3_OK) {
        sc_solve(&md);
        log_opaque(&md, mname);
        st = write_attrs(&md);
    }
    uint8_t *sel = st == ODIN3_OK ? focus_nets(run, id, mod) : NULL;
    sc_view view = {&md, run->opts, sel, id == run->top.v ? run->ppm : NULL};
    st = st == ODIN3_OK ? odin3_strbuf_appendf(body, "%s    {\"name\": ", first ? "" : ",\n") : st;
    st = st == ODIN3_OK ? gl_put_escaped(body, mname) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(body, ", \"sweeps\": %u, \"converged\": %d,\n    ",
                                               md.stats.sweeps, md.stats.converged ? 1 : 0)
                        : st;
    st = st == ODIN3_OK ? put_nets(body, &view) : st;
    st = st == ODIN3_OK ? put_lists(body, &view) : st;
    st = st == ODIN3_OK && view.ppm != NULL ? put_rare(body, &view) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(body, odin3_bytes_cstr("}")) : st;
    run->nets += live_nets(&md);
    run->unreachable += md.stats.unreachable;
    run->sweeps += md.stats.sweeps;
    run->cells += md.n_cells;
    run->modules++;
    odin3_util_free(sel);
    sc_model_free(&md);
    return st;
}

static odin3_status save_file(const char *path, const odin3_strbuf *text) {
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "scoap: cannot write '%s'", path);
        return ODIN3_ERR_IO;
    }
    bool ok = fwrite(text->data, 1, text->len, fp) == text->len;
    return (fclose(fp) == 0 && ok) ? ODIN3_OK : ODIN3_ERR_IO;
}

static odin3_status prepare(sc_run *run, const sc_opts *ob, odin3_design *design) {
    run->design = design;
    run->opts = ob;
    run->top = odin3_design_top(design);
    odin3_status st = ODIN3_OK;
    if (ob->gl.n_focus > 0) {
        st = gl_groups_make(design, &ob->gl, &run->groups);
        run->use_groups = st == ODIN3_OK;
    }
    if (st == ODIN3_OK && ob->vectors > 0) {
        if (run->top.v == 0) {
            odin3_log(ODIN3_LOG_ERROR, "scoap: --sigprob needs a top module");
            return ODIN3_ERR_INVALID_ARG;
        }
        st = sc_sigprob(design, run->top, ob, &run->ppm);
    }
    return st;
}

static odin3_status scoap_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                              void *user) {
    (void)ctx;
    (void)user;
    sc_opts ob;
    memset(&ob, 0, sizeof ob);
    ob.seed = DEFAULT_SEED;
    ob.threshold = k_default_threshold;
    odin3_strbuf_init(&ob.out);
    odin3_status st = parse_args(args, &ob);
    sc_run run;
    memset(&run, 0, sizeof run);
    odin3_strbuf body;
    odin3_strbuf_init(&body);
    double start = odin3_now_ms();
    st = st == ODIN3_OK ? prepare(&run, &ob, design) : st;
    st = st == ODIN3_OK
             ? odin3_strbuf_appendf(&body,
                                    "{\n  \"schema\": \"odin3-scoap\",\n  \"version\": %d,\n  "
                                    "\"inf\": %d,\n  \"mode\": \"%s\",\n  \"sigprob_vectors\": "
                                    "%llu,\n  \"seed\": %llu,\n  \"modules\": [\n",
                                    SCHEMA_VERSION, SCOAP_INF, ob.seq ? "seq" : "comb",
                                    (unsigned long long)ob.vectors, (unsigned long long)ob.seed)
             : st;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_design_module_end(design); id++) {
        st = put_module(&body, &run, id, id == 1);
    }
    st = st == ODIN3_OK ? odin3_strbuf_append(&body, odin3_bytes_cstr("\n  ]\n}\n")) : st;
    st = st == ODIN3_OK && ob.out.len > 0 ? save_file(ob.out.data, &body) : st;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "scoap: %u modules, %llu nets, %llu cells, mode %s, %u sweeps, %llu "
                  "unreachable, %.0f ms",
                  run.modules, (unsigned long long)run.nets, (unsigned long long)run.cells,
                  ob.seq ? "seq" : "comb", run.sweeps, (unsigned long long)run.unreachable,
                  (odin3_now_ms() - start));
    }
    odin3_util_free(run.ppm);
    gl_groups_free(&run.groups);
    odin3_strbuf_free(&body);
    opts_free(&ob);
    return st;
}

const odin3_pass_def odin3_pass_scoap = {
    "scoap",
    "scoap [--sequential] [--out FILE] [--top N] [--focus F] [--sigprob V] [--seed S] "
    "[--threshold P]: SCOAP testability measures (CC0, CC1, CO; SC0, SC1, SO) of every net as "
    "module attributes odin3:scoap_* and a JSON file, optional signal probability",
    scoap_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
