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

enum { FILE_MAX = 512 * 1024 * 1024, PERMILLE = 1000 };
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
    return cal;
}

void gl_cal_free(gl_cal *cal) {
    if (cal != NULL) {
        odin3_json_doc_free(&cal->doc);
        odin3_util_free(cal);
    }
}

static int64_t nth_int(const odin3_json_node *arr, size_t index) {
    const odin3_json_node *item = odin3_json_first(arr);
    for (size_t i = 0; item != NULL && i < index; i++) {
        item = odin3_json_next(item);
    }
    return item != NULL ? odin3_json_int(item) : 0;
}

static const odin3_json_node *path2(const odin3_json_node *obj, const char *xa, const char *xb) {
    const odin3_json_node *first = odin3_json_member(obj, xa);
    return first != NULL ? odin3_json_member(first, xb) : NULL;
}

/* Likelihood of the bin among counts (Laplace smoothing). */
static double likelihood(const odin3_json_node *counts, size_t bin, size_t bins) {
    double total = 0;
    for (size_t i = 0; i < bins; i++) {
        total += (double)nth_int(counts, i);
    }
    return ((double)nth_int(counts, bin) + 1.0) / (total + (double)bins);
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
    double ps = same != NULL ? (double)odin3_json_int(same) : 0;
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
    int64_t vn = (int64_t)lround(value * PERMILLE);
    size_t bin = 0;
    for (size_t i = 0; i < bins; i++) {
        bin = vn >= nth_int(edges, i) ? i : bin;
    }
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
    odin3_status st =
        odin3_strbuf_appendf(out, "{\"a\": \"%s\", \"b\": \"%s\"", in->name_a, in->name_b);
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

static odin3_status sig_from_group(const odin3_json_node *grp, uint32_t norb, glet_sig *sig) {
    const odin3_json_node *totals = odin3_json_member(grp, "orbit_totals");
    const odin3_json_node *gdd = odin3_json_member(grp, "gdd");
    const odin3_json_node *rep = odin3_json_member(grp, "reported");
    if (totals == NULL || gdd == NULL || odin3_json_count(totals) != norb ||
        odin3_json_count(gdd) != norb) {
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(sig, 0, sizeof *sig);
    sig->norb = norb;
    sig->nodes = rep != NULL ? (uint64_t)odin3_json_int(rep) : 0;
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

static odin3_status take_names(const odin3_json_doc *doc, const odin3_json_node *grp,
                               odin3_strbuf *mode, odin3_strbuf *group) {
    odin3_strbuf_clear(mode);
    odin3_strbuf_clear(group);
    const odin3_json_node *md = odin3_json_member(doc->root, "mode");
    odin3_status st = odin3_strbuf_append(mode, odin3_bytes_cstr(odin3_json_string(md, NULL)));
    const odin3_json_node *gname = odin3_json_member(grp, "name");
    return gname != NULL && st == ODIN3_OK
               ? odin3_strbuf_append(group, odin3_bytes_cstr(odin3_json_string(gname, NULL)))
               : st;
}

/* The group `name` (NULL: the first) of a parsed graphlets file as a signature. */
static odin3_status sig_from_doc(const odin3_json_doc *doc, const char *name, glet_sig *sig,
                                 odin3_strbuf *mode, odin3_strbuf *group) {
    const odin3_json_node *groups = odin3_json_member(doc->root, "groups");
    const odin3_json_node *grp = groups != NULL ? find_group(groups, name) : NULL;
    const odin3_json_node *md = odin3_json_member(doc->root, "mode");
    const odin3_json_node *nm = odin3_json_member(doc->root, "n_orbits");
    if (grp == NULL || md == NULL || nm == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = sig_from_group(grp, (uint32_t)odin3_json_int(nm), sig);
    return st == ODIN3_OK ? take_names(doc, grp, mode, group) : st;
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
