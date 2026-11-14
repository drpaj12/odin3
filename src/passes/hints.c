/*
 * hints.c — the timing hint table (hints.h) and the `read_hints` pass.
 */
#include "passes/hints.h"

#include "frontends/project/project.h"
#include "ir/prov.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_HINT_BYTES = 1U << 30,
    MAX_FIELDS = 8,
    CELL_FIELDS = 6,
    NET_FIELDS = 4,
    META_FIELDS = 3,
    PS_PER_NS = 1000
};

static char *dup_str(const char *text) {
    size_t len = strlen(text);
    char *copy = odin3_util_malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, text, len + 1);
    }
    return copy;
}

/* --- parsing --------------------------------------------------------------------------------- */

typedef struct hparse {
    const char *label;
    size_t line;
    odin3_hints *hints;
    size_t cap_cells, cap_nets;
    bool have_cpd, have_depth, have_min, have_npl;
} hparse;

static odin3_status perr(const hparse *hp, const char *what, const char *arg) {
    odin3_log(ODIN3_LOG_ERROR, "read_hints: %s:%zu: %s%s%s", hp->label, hp->line, what,
              arg != NULL ? ": " : "", arg != NULL ? arg : "");
    return ODIN3_ERR_PARSE;
}

/* A finite decimal number filling the whole field; `-` (when allow_dash) gives NAN. */
static bool num_of(const char *text, bool allow_dash, double *out) {
    if (allow_dash && strcmp(text, "-") == 0) {
        *out = NAN;
        return true;
    }
    char *end = NULL;
    errno = 0;
    double val = strtod(text, &end);
    if (text[0] == '\0' || *end != '\0' || errno != 0 || !isfinite(val)) {
        return false;
    }
    *out = val;
    return true;
}

static bool uint_of(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long val = strtoul(text, &end, 10);
    if (text[0] == '\0' || !isdigit((unsigned char)text[0]) || *end != '\0' || errno != 0 ||
        val > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)val;
    return true;
}

static odin3_status meta_line(hparse *hp, char **f, size_t n) {
    if (n != META_FIELDS) {
        return perr(hp, "a meta line has 3 fields", NULL);
    }
    odin3_hints *h = hp->hints;
    double val = 0;
    uint32_t num = 0;
    if (strcmp(f[1], "cpd_ps") == 0 || strcmp(f[1], "slack_min_ps") == 0 ||
        strcmp(f[1], "ns_per_level") == 0) {
        if (!num_of(f[2], false, &val)) {
            return perr(hp, "not a number", f[2]);
        }
        if (f[1][0] == 'c') {
            h->cpd_ps = val;
            hp->have_cpd = true;
        } else if (f[1][0] == 's') {
            h->slack_min_ps = val;
            hp->have_min = true;
        } else {
            h->ns_per_level = val;
            hp->have_npl = true;
        }
    } else if (strcmp(f[1], "tighten_levels") == 0) {
        if (!num_of(f[2], false, &val) || val < 0.0) {
            return perr(hp, "tighten_levels must be a number from 0", f[2]);
        }
        h->tighten_levels = val;
    } else if (strcmp(f[1], "depth") == 0) {
        if (!uint_of(f[2], &num)) {
            return perr(hp, "not an unsigned number", f[2]);
        }
        h->depth = num;
        hp->have_depth = true;
    } else {
        size_t used = strlen(h->source);
        (void)snprintf(h->source + used, sizeof h->source - used, "%s%s=%s", used != 0 ? " " : "",
                       f[1], f[2]);
    }
    return ODIN3_OK;
}

static odin3_status grow(void **arr, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) {
        return ODIN3_OK;
    }
    size_t ncap = *cap != 0 ? *cap * 2 : 64;
    void *mem = odin3_util_realloc(*arr, ncap * elem);
    if (mem == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *arr = mem;
    *cap = ncap;
    return ODIN3_OK;
}

static odin3_status cell_line(hparse *hp, char **f, size_t n) {
    odin3_hint_cell rec = {0};
    double slack = 0;
    double crit = 0;
    if (n != CELL_FIELDS) {
        return perr(hp, "a cell line has 6 fields", NULL);
    }
    if (!uint_of(f[1], &rec.uid) || rec.uid == 0) {
        return perr(hp, "not a record ID", f[1]);
    }
    if (!num_of(f[2], false, &slack)) {
        return perr(hp, "not a number", f[2]);
    }
    if (!num_of(f[3], false, &crit)) {
        return perr(hp, "not a number", f[3]);
    }
    if (!uint_of(f[4], &rec.depth)) {
        return perr(hp, "not an unsigned number", f[4]);
    }
    if (!uint_of(f[5], &rec.luts)) {
        return perr(hp, "not an unsigned number", f[5]);
    }
    rec.slack_ps = slack;
    rec.crit = crit;
    odin3_hints *h = hp->hints;
    if (grow((void **)&h->cells, &hp->cap_cells, h->n_cells + 1, sizeof *h->cells) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    h->cells[h->n_cells++] = rec;
    return ODIN3_OK;
}

static odin3_status net_line(hparse *hp, char **f, size_t n) {
    odin3_hint_net rec = {0};
    if (n != NET_FIELDS) {
        return perr(hp, "a net line has 4 fields", NULL);
    }
    if (strchr(f[1], '/') == NULL) {
        return perr(hp, "a net name is <module>/<net>", f[1]);
    }
    if (!num_of(f[2], true, &rec.arrival_ps)) {
        return perr(hp, "not a number", f[2]);
    }
    if (!num_of(f[3], true, &rec.required_ps)) {
        return perr(hp, "not a number", f[3]);
    }
    odin3_hints *h = hp->hints;
    if (grow((void **)&h->nets, &hp->cap_nets, h->n_nets + 1, sizeof *h->nets) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rec.name = dup_str(f[1]);
    if (rec.name == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    h->nets[h->n_nets++] = rec;
    return ODIN3_OK;
}

static size_t split_tabs(char *line, char **f) {
    size_t n = 0;
    char *at = line;
    while (n < MAX_FIELDS) {
        f[n++] = at;
        char *tab = strchr(at, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        at = tab + 1;
    }
    return n;
}

static odin3_status one_line(hparse *hp, char *line, bool *first) {
    char *f[MAX_FIELDS] = {0};
    size_t n = split_tabs(line, f);
    if (*first) {
        *first = false;
        char want[32];
        (void)snprintf(want, sizeof want, "%d", ODIN3_HINTS_VERSION);
        if (n != 2 || strcmp(f[0], ODIN3_HINTS_MAGIC) != 0 || strcmp(f[1], want) != 0) {
            return perr(hp,
                        "not a timing hint file (first line must be "
                        "`" ODIN3_HINTS_MAGIC "<TAB>1`)",
                        NULL);
        }
        return ODIN3_OK;
    }
    if (strcmp(f[0], "meta") == 0) {
        return meta_line(hp, f, n);
    }
    if (strcmp(f[0], "cell") == 0) {
        return cell_line(hp, f, n);
    }
    if (strcmp(f[0], "net") == 0) {
        return net_line(hp, f, n);
    }
    return perr(hp, "unknown record kind", f[0]);
}

static int cmp_cell(const void *lhs, const void *rhs) {
    const odin3_hint_cell *a = lhs;
    const odin3_hint_cell *b = rhs;
    return a->uid < b->uid ? -1 : (a->uid > b->uid ? 1 : 0);
}

static int cmp_net(const void *lhs, const void *rhs) {
    return strcmp(((const odin3_hint_net *)lhs)->name, ((const odin3_hint_net *)rhs)->name);
}

/* The worse of two NANs-aware values: smaller wins when want_min, else larger. */
static double pick(double a, double b, bool want_min) {
    if (isnan(a)) {
        return b;
    }
    if (isnan(b)) {
        return a;
    }
    return want_min ? fmin(a, b) : fmax(a, b);
}

/* Sorts, then merges duplicates keeping the worst. */
static void merge_dups(odin3_hints *h) {
    qsort(h->cells, h->n_cells, sizeof *h->cells, cmp_cell);
    size_t out = 0;
    for (size_t i = 0; i < h->n_cells; i++) {
        if (out > 0 && h->cells[out - 1].uid == h->cells[i].uid) {
            odin3_hint_cell *to = &h->cells[out - 1];
            to->slack_ps = fmin(to->slack_ps, h->cells[i].slack_ps);
            to->crit = fmax(to->crit, h->cells[i].crit);
            to->depth = to->depth < h->cells[i].depth ? to->depth : h->cells[i].depth;
            to->luts += h->cells[i].luts;
        } else {
            h->cells[out++] = h->cells[i];
        }
    }
    h->n_cells = out;
    qsort(h->nets, h->n_nets, sizeof *h->nets, cmp_net);
    out = 0;
    for (size_t i = 0; i < h->n_nets; i++) {
        if (out > 0 && strcmp(h->nets[out - 1].name, h->nets[i].name) == 0) {
            odin3_hint_net *to = &h->nets[out - 1];
            to->arrival_ps = pick(to->arrival_ps, h->nets[i].arrival_ps, false);
            to->required_ps = pick(to->required_ps, h->nets[i].required_ps, true);
            odin3_util_free(h->nets[i].name);
        } else {
            h->nets[out++] = h->nets[i];
        }
    }
    h->n_nets = out;
}

static odin3_status finish_parse(hparse *hp, bool first) {
    odin3_hints *h = hp->hints;
    if (first) {
        hp->line = 1;
        return perr(hp, "empty file", NULL);
    }
    if ((h->n_cells != 0 || h->n_nets != 0) && !(hp->have_cpd && hp->have_depth && hp->have_min)) {
        return perr(hp, "cell and net records need meta lines cpd_ps, depth and slack_min_ps",
                    NULL);
    }
    if (!hp->have_npl) {
        h->ns_per_level = h->depth != 0 ? h->cpd_ps / PS_PER_NS / (double)h->depth : 0.0;
    }
    if ((h->n_cells != 0 || h->n_nets != 0) && !(h->ns_per_level > 0.0)) {
        return perr(hp, "ns_per_level (cpd_ps / depth) must be positive", NULL);
    }
    merge_dups(h);
    h->cell_used = odin3_util_calloc(h->n_cells + 1);
    h->net_used = odin3_util_calloc(h->n_nets + 1);
    return h->cell_used != NULL && h->net_used != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

odin3_status odin3_hints_parse(const char *text, size_t len, const char *label, odin3_hints **out) {
    *out = NULL;
    hparse hp = {.label = label};
    hp.hints = odin3_util_calloc(sizeof *hp.hints);
    if (hp.hints != NULL) {
        hp.hints->tighten_levels = ODIN3_HINTS_TIGHTEN_DEFAULT;
    }
    char *copy = odin3_util_malloc(len + 1);
    if (hp.hints == NULL || copy == NULL) {
        odin3_util_free(hp.hints);
        odin3_util_free(copy);
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    hp.hints->label = dup_str(label);
    odin3_status st = hp.hints->label != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    bool first = true;
    char *at = copy;
    while (st == ODIN3_OK && *at != '\0') {
        char *nl = strchr(at, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        size_t ll = strlen(at);
        if (ll > 0 && at[ll - 1] == '\r') {
            at[ll - 1] = '\0';
        }
        hp.line++;
        if (at[0] != '\0' && (at[0] != '#' || first)) {
            st = one_line(&hp, at, &first);
        }
        at = nl != NULL ? nl + 1 : at + strlen(at);
    }
    st = st == ODIN3_OK ? finish_parse(&hp, first) : st;
    odin3_util_free(copy);
    if (st != ODIN3_OK) {
        odin3_hints_free(hp.hints);
        return st;
    }
    *out = hp.hints;
    return ODIN3_OK;
}

void odin3_hints_free(odin3_hints *hints) {
    if (hints == NULL) {
        return;
    }
    for (size_t i = 0; i < hints->n_nets; i++) {
        odin3_util_free(hints->nets[i].name);
    }
    odin3_util_free(hints->nets);
    odin3_util_free(hints->cells);
    odin3_util_free(hints->cell_used);
    odin3_util_free(hints->net_used);
    odin3_util_free(hints->label);
    odin3_util_free(hints);
}

/* --- lookups --------------------------------------------------------------------------------- */

const odin3_hint_cell *odin3_hints_find_cell(odin3_hints *hints, uint32_t uid) {
    const odin3_hint_cell key = {.uid = uid};
    odin3_hint_cell *hit =
        bsearch(&key, hints->cells, hints->n_cells, sizeof *hints->cells, cmp_cell);
    if (hit != NULL) {
        hints->cell_used[hit - hints->cells] = 1;
    }
    return hit;
}

const odin3_hint_net *odin3_hints_find_net(odin3_hints *hints, const char *name) {
    const odin3_hint_net key = {.name = (char *)name};
    odin3_hint_net *hit = bsearch(&key, hints->nets, hints->n_nets, sizeof *hints->nets, cmp_net);
    if (hit != NULL) {
        hints->net_used[hit - hints->nets] = 1;
    }
    return hit;
}

static size_t count_used(const uint8_t *used, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        total += used[i];
    }
    return total;
}

size_t odin3_hints_cells_used(const odin3_hints *hints) {
    return count_used(hints->cell_used, hints->n_cells);
}

size_t odin3_hints_nets_used(const odin3_hints *hints) {
    return count_used(hints->net_used, hints->n_nets);
}

/* --- the cached table of the design in hand --------------------------------------------------- */

static struct {
    uint64_t serial;
    odin3_hints *hints;
    bool project_tried; /* the project's key was tried for this serial */
} g_cache;

void odin3_hints_reset(void) {
    odin3_hints_free(g_cache.hints);
    g_cache.hints = NULL;
    g_cache.serial = 0;
    g_cache.project_tried = false;
}

static odin3_status read_file(const char *path, odin3_hints **out) {
    odin3_file_bytes bytes = {0};
    odin3_status st = odin3_file_read_all(path, MAX_HINT_BYTES, &bytes);
    if (st == ODIN3_ERR_IO) {
        odin3_log(ODIN3_LOG_ERROR, "read_hints: %s: cannot read the file: %s", path,
                  strerror(bytes.err));
    } else if (st == ODIN3_ERR_PARSE) {
        odin3_log(ODIN3_LOG_ERROR, "read_hints: %s: the file is too large", path);
    }
    if (st == ODIN3_OK) {
        st = odin3_hints_parse((const char *)bytes.data, bytes.len, path, out);
    }
    odin3_file_bytes_free(&bytes);
    return st;
}

odin3_status odin3_hints_load(const odin3_design *design, const char *path) {
    odin3_hints *hints = NULL;
    odin3_status st = read_file(path, &hints);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_hints_reset();
    g_cache.serial = odin3_design_serial(design);
    g_cache.hints = hints;
    g_cache.project_tried = true;
    return ODIN3_OK;
}

odin3_hints *odin3_hints_of(const odin3_design *design) {
    uint64_t serial = odin3_design_serial(design);
    if (g_cache.serial != serial) {
        odin3_hints_reset();
        g_cache.serial = serial;
    }
    if (g_cache.hints != NULL || g_cache.project_tried) {
        return g_cache.hints;
    }
    g_cache.project_tried = true;
    const odin3_project *proj = odin3_design_project(design);
    if (proj != NULL && proj->timing_hints.given != 0) {
        const char *path =
            odin3_strtab_get(odin3_design_strtab(design), proj->timing_hints.resolved);
        (void)read_file(path, &g_cache.hints);
    }
    return g_cache.hints;
}

/* --- the read_hints pass -----------------------------------------------------------------------
 */

static odin3_status read_hints_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)design;
    (void)user;
    odin3_prov_begin_op_lazy(ctx); /* the run is one operation; it makes no record */
    odin3_bytes file = odin3_pass_arg_next(&args);
    if (file.ptr == NULL || odin3_pass_arg_next(&args).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "read_hints: usage: read_hints <file>");
        return ODIN3_ERR_INVALID_ARG;
    }
    char *path = odin3_util_malloc(file.len + 1);
    if (path == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(path, file.ptr, file.len);
    path[file.len] = '\0';
    odin3_status st = odin3_hints_load(ctx->design, path);
    if (st == ODIN3_OK) {
        const odin3_hints *h = odin3_hints_of(ctx->design);
        odin3_log(ODIN3_LOG_INFO,
                  "read_hints: %s: %zu cell hints, %zu net hints, pass-1 cpd %.0f ps, depth %u, "
                  "%.4f ns per LUT level%s%s",
                  path, h->n_cells, h->n_nets, h->cpd_ps, (unsigned)h->depth, h->ns_per_level,
                  h->source[0] != '\0' ? "; " : "", h->source);
    }
    odin3_util_free(path);
    return st;
}

const odin3_pass_def odin3_pass_read_hints = {
    "read_hints",
    "read_hints <file>: read a timing hint file (written by odin3-flow --two-pass or "
    "tools/flow/timing_hints.py) for the passes that use hints (abc); without the pass nothing "
    "changes",
    read_hints_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
