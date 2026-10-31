/*
 * gl_cal.c — signature files, the calibration file and the Bayesian posterior (spec sections 13
 * and 14). All files are read with the repository's JSON reader, so they hold integers only.
 */
#include "frontends/json/json_parse.h"
#include "passes/graphlets/gl_int.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FILE_MAX = 512 * 1024 * 1024,
    PERMILLE = 1000,
    CAL_MAX_BINS = 4096, /* bins of one calibrated metric */
    SIG_MAX_ORBITS = GLET_MAX_ORBITS
};
static const double k_ppm = 1e6;
static const double k_half = 0.5;

struct gl_cal {
    odin3_json_doc doc;
};

static odin3_status parse_file(const char *path, odin3_json_doc *doc) {
    odin3_file_bytes fb;
    odin3_status st = odin3_file_read_all(path, FILE_MAX, &fb);
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: cannot read '%s'", path);
        return ODIN3_ERR_IO;
    }
    st = odin3_json_parse(fb.data, fb.len, NULL, doc);
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: '%s': %s (line %u)", path,
                  st == ODIN3_ERR_PARSE ? doc->error.message : "out of memory",
                  (unsigned)doc->error.line);
    }
    odin3_file_bytes_free(&fb);
    return st;
}

/* A non-negative INT node. */
static bool is_count(const odin3_json_node *node) {
    return node != NULL && odin3_json_kind_of(node) == ODIN3_JSON_INT && odin3_json_int(node) >= 0;
}

/* The array of non-negative counts, `want` long (0: any length from 1 to CAL_MAX_BINS). */
static bool counts_ok(const odin3_json_node *arr, size_t want) {
    if (arr == NULL || odin3_json_kind_of(arr) != ODIN3_JSON_ARRAY) {
        return false;
    }
    size_t cnt = odin3_json_count(arr);
    if (want != 0 ? cnt != want : (cnt == 0 || cnt > CAL_MAX_BINS)) {
        return false;
    }
    for (const odin3_json_node *it = odin3_json_first(arr); it != NULL; it = odin3_json_next(it)) {
        if (!is_count(it)) {
            return false;
        }
    }
    return true;
}

/* The bin edges: 2 .. CAL_MAX_BINS + 1 integers, never decreasing. */
static bool edges_ok(const odin3_json_node *edges) {
    if (edges == NULL || odin3_json_kind_of(edges) != ODIN3_JSON_ARRAY ||
        odin3_json_count(edges) < 2 || odin3_json_count(edges) > CAL_MAX_BINS + 1) {
        return false;
    }
    int64_t prev = INT64_MIN;
    for (const odin3_json_node *it = odin3_json_first(edges); it != NULL;
         it = odin3_json_next(it)) {
        if (odin3_json_kind_of(it) != ODIN3_JSON_INT || odin3_json_int(it) < prev) {
            return false;
        }
        prev = odin3_json_int(it);
    }
    return true;
}

static bool metric_ok(const odin3_json_node *met) {
    const odin3_json_node *edges = odin3_json_member(met, "edges");
    if (!edges_ok(edges)) {
        return false;
    }
    size_t bins = odin3_json_count(edges) - 1;
    return counts_ok(odin3_json_member(met, "same"), bins) &&
           counts_ok(odin3_json_member(met, "different"), bins);
}

static bool mode_ok(const odin3_json_node *mode) {
    const odin3_json_node *prior = odin3_json_member(mode, "prior");
    if (prior != NULL) {
        const odin3_json_node *same = odin3_json_member(prior, "same");
        const odin3_json_node *diff = odin3_json_member(prior, "different");
        if ((same != NULL && !is_count(same)) || (diff != NULL && !is_count(diff))) {
            return false;
        }
    }
    const odin3_json_node *metrics = odin3_json_member(mode, "metrics");
    if (metrics == NULL) {
        return true;
    }
    for (const odin3_json_node *met = odin3_json_first(metrics); met != NULL;
         met = odin3_json_next(met)) {
        if (!metric_ok(met)) {
            return false;
        }
    }
    return true;
}

/* Counts non-negative, bin edges sorted, bin counts equal to the edges less one and at most
 * CAL_MAX_BINS, so the prior (same / (same + different)) is in [0, 1]. */
static bool cal_valid(const odin3_json_doc *doc) {
    const odin3_json_node *modes = odin3_json_member(doc->root, "modes");
    if (modes == NULL) {
        return true;
    }
    for (const odin3_json_node *md = odin3_json_first(modes); md != NULL;
         md = odin3_json_next(md)) {
        if (!mode_ok(md)) {
            return false;
        }
    }
    return true;
}

gl_cal *gl_cal_load(const char *path) {
    gl_cal *cal = odin3_util_calloc(sizeof *cal);
    if (cal == NULL) {
        return NULL;
    }
    const odin3_json_node *schema = NULL;
    const char *name = NULL;
    if (parse_file(path, &cal->doc) == ODIN3_OK) {
        schema = odin3_json_member(cal->doc.root, "schema");
        name = schema != NULL ? odin3_json_string(schema, NULL) : NULL;
    }
    if (name == NULL || strcmp(name, "odin3-graphlet-calibration") != 0) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: '%s' is not a graphlet calibration file", path);
        gl_cal_free(cal);
        return NULL;
    }
    if (!cal_valid(&cal->doc)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "graphlets: '%s': invalid calibration (negative or non-integer counts, unsorted "
                  "bin edges, bin counts not equal to the edges less one, or more than %d bins)",
                  path, CAL_MAX_BINS);
        gl_cal_free(cal);
        return NULL;
    }
    return cal;
}

void gl_cal_free(gl_cal *cal) {
    if (cal != NULL) {
        odin3_json_doc_free(&cal->doc);
        odin3_util_free(cal);
    }
}

static const odin3_json_node *path2(const odin3_json_node *obj, const char *xa, const char *xb) {
    const odin3_json_node *first = odin3_json_member(obj, xa);
    return first != NULL ? odin3_json_member(first, xb) : NULL;
}

/* Likelihood of the bin among counts (Laplace smoothing): one pass over the array. */
static double likelihood(const odin3_json_node *counts, size_t bin, size_t bins) {
    double total = 0;
    double hit = 0;
    size_t idx = 0;
    for (const odin3_json_node *it = odin3_json_first(counts); it != NULL;
         it = odin3_json_next(it), idx++) {
        total += (double)odin3_json_int(it);
        hit = idx == bin ? (double)odin3_json_int(it) : hit;
    }
    return (hit + 1.0) / (total + (double)bins);
}

/* The last bin whose lower edge is at or below vn (bin 0 when below all of them). */
static size_t bin_of(const odin3_json_node *edges, int64_t vn) {
    size_t bins = odin3_json_count(edges) - 1;
    size_t bin = 0;
    size_t idx = 0;
    for (const odin3_json_node *it = odin3_json_first(edges); it != NULL && idx < bins;
         it = odin3_json_next(it), idx++) {
        bin = vn >= odin3_json_int(it) ? idx : bin;
    }
    return bin;
}

typedef struct cal_view {
    double prior_same;
    double prior_diff;
    const odin3_json_node *metrics;
} cal_view;

static bool cal_mode(const gl_cal *cal, const char *mode, cal_view *view) {
    const odin3_json_node *node = path2(cal->doc.root, "modes", mode);
    const odin3_json_node *prior = node != NULL ? odin3_json_member(node, "prior") : NULL;
    view->metrics = node != NULL ? odin3_json_member(node, "metrics") : NULL;
    if (prior == NULL || view->metrics == NULL) {
        return false;
    }
    const odin3_json_node *same = odin3_json_member(prior, "same");
    const odin3_json_node *diff = odin3_json_member(prior, "different");
    double ps = same != NULL ? (double)odin3_json_int(same) : 0; /* counts, validated >= 0 */
    double pd = diff != NULL ? (double)odin3_json_int(diff) : 0;
    view->prior_same = ps + pd > 0 ? ps / (ps + pd) : k_half;
    view->prior_diff = 1.0 - view->prior_same;
    return true;
}

/* Likelihoods of one metric at value; false when the metric is not calibrated. */
static bool metric_likelihoods(const cal_view *view, const char *metric, double value, double *ls,
                               double *ld) {
    const odin3_json_node *met = odin3_json_member(view->metrics, metric);
    const odin3_json_node *edges = met != NULL ? odin3_json_member(met, "edges") : NULL;
    const odin3_json_node *same = met != NULL ? odin3_json_member(met, "same") : NULL;
    const odin3_json_node *diff = met != NULL ? odin3_json_member(met, "different") : NULL;
    if (edges == NULL || same == NULL || diff == NULL || odin3_json_count(edges) < 2) {
        return false;
    }
    size_t bins = odin3_json_count(edges) - 1;
    if (odin3_json_count(same) != bins || odin3_json_count(diff) != bins) {
        return false;
    }
    if (!isfinite(value)) {
        return false;
    }
    size_t bin = bin_of(edges, (int64_t)lround(value * PERMILLE));
    *ls = likelihood(same, bin, bins);
    *ld = likelihood(diff, bin, bins);
    return true;
}

bool gl_cal_posterior(const gl_cal *cal, const char *mode, const char *metric, double value,
                      double *post) {
    cal_view view;
    double ls = 0;
    double ld = 0;
    if (cal == NULL || !cal_mode(cal, mode, &view) ||
        !metric_likelihoods(&view, metric, value, &ls, &ld)) {
        return false;
    }
    double num = ls * view.prior_same;
    *post = num / (num + ld * view.prior_diff);
    return true;
}

bool gl_cal_posterior_both(const gl_cal *cal, const char *mode, const double *values,
                           double *post) {
    static const char *const k_metrics[2] = {"gdd_agreement", "cosine_log"};
    cal_view view;
    double ls = 1;
    double ld = 1;
    if (cal == NULL || !cal_mode(cal, mode, &view)) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        double xa = 0;
        double xb = 0;
        if (!metric_likelihoods(&view, k_metrics[i], values[i], &xa, &xb)) {
            return false;
        }
        ls *= xa;
        ld *= xb;
    }
    double num = ls * view.prior_same;
    *post = num / (num + ld * view.prior_diff);
    return true;
}

static odin3_status put_value(odin3_strbuf *out, const char *key, double value, bool ints) {
    if (ints) {
        return odin3_strbuf_appendf(out, ", \"%s_ppm\": %lld", key, llround(value * k_ppm));
    }
    return odin3_strbuf_appendf(out, ", \"%s\": %.6f", key, value);
}

odin3_status gl_compare_json(odin3_strbuf *out, const gl_cmp_in *in, bool ints) {
    const double vals[2] = {glet_gdd_agreement(in->sig_a, in->sig_b, NULL),
                            glet_cosine_log(in->sig_a, in->sig_b)};
    double post = 0;
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("{\"a\": "));
    st = st == ODIN3_OK ? gl_put_escaped(out, in->name_a) : st;
    st = st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr(", \"b\": ")) : st;
    st = st == ODIN3_OK ? gl_put_escaped(out, in->name_b) : st;
    st = st == ODIN3_OK ? put_value(out, "gdd_agreement", vals[0], ints) : st;
    st = st == ODIN3_OK ? put_value(out, "cosine_log", vals[1], ints) : st;
    bool have = gl_cal_posterior(in->cal, in->mode, "gdd_agreement", vals[0], &post);
    st = st == ODIN3_OK && have ? put_value(out, "posterior_gdd_agreement", post, ints) : st;
    have = gl_cal_posterior(in->cal, in->mode, "cosine_log", vals[1], &post);
    st = st == ODIN3_OK && have ? put_value(out, "posterior_cosine_log", post, ints) : st;
    have = gl_cal_posterior_both(in->cal, in->mode, vals, &post);
    st = st == ODIN3_OK && have ? put_value(out, "posterior_combined", post, ints) : st;
    return st == ODIN3_OK
               ? odin3_strbuf_appendf(out, ", \"calibrated\": %s}", have ? "true" : "false")
               : st;
}

/* --- reading a signature ------------------------------------------------------------------- */

static const odin3_json_node *find_group(const odin3_json_node *groups, const char *name) {
    for (const odin3_json_node *gr = odin3_json_first(groups); gr != NULL;
         gr = odin3_json_next(gr)) {
        const odin3_json_node *nm = odin3_json_member(gr, "name");
        const char *text = nm != NULL ? odin3_json_string(nm, NULL) : NULL;
        if (name == NULL || (text != NULL && strcmp(text, name) == 0)) {
            return gr;
        }
    }
    return NULL;
}

static int64_t nth_int(const odin3_json_node *arr, size_t index) {
    const odin3_json_node *item = odin3_json_first(arr);
    for (size_t i = 0; item != NULL && i < index; i++) {
        item = odin3_json_next(item);
    }
    return item != NULL ? odin3_json_int(item) : 0;
}

/* One GDD pair [degree, number]: both INT, degree > prev (so >= 1 and sorted), number >= 1. */
static bool pair_ok(const odin3_json_node *ps, int64_t prev) {
    if (odin3_json_kind_of(ps) != ODIN3_JSON_ARRAY || odin3_json_count(ps) != 2) {
        return false;
    }
    const odin3_json_node *deg = odin3_json_first(ps);
    const odin3_json_node *num = odin3_json_next(deg);
    return odin3_json_kind_of(deg) == ODIN3_JSON_INT && odin3_json_kind_of(num) == ODIN3_JSON_INT &&
           odin3_json_int(deg) > prev && odin3_json_int(deg) >= 1 && odin3_json_int(num) >= 1;
}

/* The gdd and totals arrays are well formed for norb orbits (see pair_ok); totals never negative.
 */
static bool sig_shape_ok(const odin3_json_node *totals, const odin3_json_node *gdd) {
    for (const odin3_json_node *tt = odin3_json_first(totals); tt != NULL;
         tt = odin3_json_next(tt)) {
        if (!is_count(tt)) {
            return false;
        }
    }
    for (const odin3_json_node *ob = odin3_json_first(gdd); ob != NULL; ob = odin3_json_next(ob)) {
        if (odin3_json_kind_of(ob) != ODIN3_JSON_ARRAY) {
            return false;
        }
        int64_t prev = 0;
        for (const odin3_json_node *ps = odin3_json_first(ob); ps != NULL;
             ps = odin3_json_next(ps)) {
            if (!pair_ok(ps, prev)) {
                return false;
            }
            prev = nth_int(ps, 0);
        }
    }
    return true;
}

static odin3_status sig_alloc(glet_sig *sig, const odin3_json_node *gdd) {
    uint32_t norb = sig->norb;
    size_t pairs = 0;
    for (const odin3_json_node *ob = odin3_json_first(gdd); ob != NULL; ob = odin3_json_next(ob)) {
        pairs += odin3_json_count(ob);
    }
    sig->totals = odin3_util_calloc((size_t)norb * sizeof *sig->totals);
    sig->off = odin3_util_calloc(((size_t)norb + 1) * sizeof *sig->off);
    sig->deg = odin3_util_calloc((pairs + 1) * sizeof *sig->deg);
    sig->num = odin3_util_calloc((pairs + 1) * sizeof *sig->num);
    if (sig->totals == NULL || sig->off == NULL || sig->deg == NULL || sig->num == NULL) {
        glet_sig_free(sig);
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

static odin3_status sig_from_group(const odin3_json_node *grp, uint32_t norb, glet_sig *sig) {
    const odin3_json_node *totals = odin3_json_member(grp, "orbit_totals");
    const odin3_json_node *gdd = odin3_json_member(grp, "gdd");
    const odin3_json_node *rep = odin3_json_member(grp, "reported");
    if (totals == NULL || gdd == NULL || odin3_json_kind_of(totals) != ODIN3_JSON_ARRAY ||
        odin3_json_kind_of(gdd) != ODIN3_JSON_ARRAY || odin3_json_count(totals) != norb ||
        odin3_json_count(gdd) != norb || (rep != NULL && !is_count(rep)) ||
        !sig_shape_ok(totals, gdd)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(sig, 0, sizeof *sig);
    sig->norb = norb;
    sig->nodes = rep != NULL ? (uint64_t)odin3_json_int(rep) : 0;
    odin3_status st = sig_alloc(sig, gdd);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t j = 0;
    uint32_t used = 0;
    const odin3_json_node *tt = odin3_json_first(totals);
    for (const odin3_json_node *ob = odin3_json_first(gdd); ob != NULL;
         ob = odin3_json_next(ob), j++) {
        sig->totals[j] = odin3_json_int(tt);
        tt = odin3_json_next(tt);
        sig->off[j] = used;
        for (const odin3_json_node *ps = odin3_json_first(ob); ps != NULL;
             ps = odin3_json_next(ps)) {
            sig->deg[used] = nth_int(ps, 0);
            sig->num[used++] = (uint64_t)nth_int(ps, 1);
        }
    }
    sig->off[norb] = used;
    return ODIN3_OK;
}

/* The text of a non-empty STRING node, else NULL. */
static const char *req_string(const odin3_json_node *node) {
    const char *text = node != NULL ? odin3_json_string(node, NULL) : NULL;
    return text != NULL && text[0] != '\0' ? text : NULL;
}

static odin3_status take_names(const odin3_json_doc *doc, const odin3_json_node *grp,
                               odin3_strbuf *mode, odin3_strbuf *group) {
    const char *md = req_string(odin3_json_member(doc->root, "mode"));
    const char *gname = req_string(odin3_json_member(grp, "name"));
    if (md == NULL || gname == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf_clear(mode);
    odin3_strbuf_clear(group);
    odin3_status st = odin3_strbuf_append(mode, odin3_bytes_cstr(md));
    return st == ODIN3_OK ? odin3_strbuf_append(group, odin3_bytes_cstr(gname)) : st;
}

/* The group `name` (NULL: the first) of a parsed graphlets file as a signature. The mode and the
 * group name must be non-empty strings (checked before the signature is built). */
static odin3_status sig_from_doc(const odin3_json_doc *doc, const char *name, glet_sig *sig,
                                 odin3_strbuf *mode, odin3_strbuf *group) {
    const odin3_json_node *groups = odin3_json_member(doc->root, "groups");
    const odin3_json_node *grp = groups != NULL ? find_group(groups, name) : NULL;
    const odin3_json_node *nm = odin3_json_member(doc->root, "n_orbits");
    if (grp == NULL || nm == NULL || odin3_json_kind_of(nm) != ODIN3_JSON_INT ||
        odin3_json_int(nm) < 1 || odin3_json_int(nm) > SIG_MAX_ORBITS ||
        req_string(odin3_json_member(doc->root, "mode")) == NULL ||
        req_string(odin3_json_member(grp, "name")) == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = sig_from_group(grp, (uint32_t)odin3_json_int(nm), sig);
    if (st != ODIN3_OK) {
        return st;
    }
    st = take_names(doc, grp, mode, group);
    if (st != ODIN3_OK) {
        glet_sig_free(sig);
    }
    return st;
}

odin3_status gl_sig_load(const char *path_hash_name, glet_sig *sig, odin3_strbuf *mode,
                         odin3_strbuf *group) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    const char *hash = strrchr(path_hash_name, '#');
    size_t plen = hash != NULL ? (size_t)(hash - path_hash_name) : strlen(path_hash_name);
    odin3_status st = odin3_strbuf_append(&path, (odin3_bytes){path_hash_name, plen});
    odin3_json_doc doc;
    memset(&doc, 0, sizeof doc);
    st = st == ODIN3_OK ? parse_file(path.data, &doc) : st;
    if (st == ODIN3_OK) {
        st = sig_from_doc(&doc, hash != NULL ? hash + 1 : NULL, sig, mode, group);
        if (st == ODIN3_ERR_INVALID_ARG) {
            odin3_log(ODIN3_LOG_ERROR, "graphlets: '%s': no such group or not a graphlets file",
                      path_hash_name);
        }
    }
    odin3_json_doc_free(&doc);
    odin3_strbuf_free(&path);
    return st;
}

static odin3_status save_text(const char *path, const odin3_strbuf *text) {
    FILE *fp = fopen(path, "w");
    odin3_status st = fp != NULL && fprintf(fp, "%s\n", text->data) > 0 ? ODIN3_OK : ODIN3_ERR_IO;
    if (fp != NULL) {
        (void)fclose(fp);
    }
    return st;
}

odin3_status gl_run_compare(const gl_opts *opts) {
    glet_sig sa;
    glet_sig sb;
    odin3_strbuf mode_a;
    odin3_strbuf mode_b;
    odin3_strbuf name_a;
    odin3_strbuf name_b;
    odin3_strbuf out;
    odin3_strbuf_init(&mode_a);
    odin3_strbuf_init(&mode_b);
    odin3_strbuf_init(&name_a);
    odin3_strbuf_init(&name_b);
    odin3_strbuf_init(&out);
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    gl_cal *cal = opts->calibration.len > 0 ? gl_cal_load(opts->calibration.data) : NULL;
    odin3_status st = opts->calibration.len > 0 && cal == NULL ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
    st = st == ODIN3_OK ? gl_sig_load(opts->cmp_a.data, &sa, &mode_a, &name_a) : st;
    st = st == ODIN3_OK ? gl_sig_load(opts->cmp_b.data, &sb, &mode_b, &name_b) : st;
    if (st == ODIN3_OK && (strcmp(mode_a.data, mode_b.data) != 0 || sa.norb != sb.norb)) {
        odin3_log(ODIN3_LOG_ERROR, "graphlets: --compare: the two files use different modes");
        st = ODIN3_ERR_INVALID_ARG;
    }
    gl_cmp_in in = {name_a.data, name_b.data, &sa, &sb, cal, mode_a.data};
    st = st == ODIN3_OK ? gl_compare_json(&out, &in, false) : st;
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "graphlets: compare %s", out.data);
        if (cal == NULL) {
            odin3_log(ODIN3_LOG_INFO,
                      "graphlets: uncalibrated (no --calibration): similarity only");
        }
    }
    st = st == ODIN3_OK && opts->out.len > 0 ? save_text(opts->out.data, &out) : st;
    glet_sig_free(&sa);
    glet_sig_free(&sb);
    gl_cal_free(cal);
    odin3_strbuf_free(&mode_a);
    odin3_strbuf_free(&mode_b);
    odin3_strbuf_free(&name_a);
    odin3_strbuf_free(&name_b);
    odin3_strbuf_free(&out);
    return st;
}
