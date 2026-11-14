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
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The smallest ns per LUT level a file may state or imply: below it the times in levels reach
 * inf in the float timing file. */
static const double NS_PER_LEVEL_FLOOR = 1e-6;

enum {
    MAX_HINT_BYTES = 1U << 30,
    MAX_FIELDS = 8,
    CELL_FIELDS = 6,
    NET_FIELDS = 4,
    META_FIELDS = 3,
    PS_PER_NS = 1000,
    DECIMAL = 10,
    FIRST_CAP = 64,
    WANT_BYTES = 32,
    CELL_LUTS_FIELD = 5 /* cell TAB uid TAB slack TAB crit TAB depth TAB luts */
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
    unsigned long val = strtoul(text, &end, DECIMAL);
    if (text[0] == '\0' || !isdigit((unsigned char)text[0]) || *end != '\0' || errno != 0 ||
        val > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)val;
    return true;
}

/* cpd_ps, slack_min_ps, ns_per_level: a number; false when fld[1] is none of them. */
static bool meta_number(hparse *hp, char **fld, odin3_status *st) {
    const char *key = fld[1];
    odin3_hints *tbl = hp->hints;
    double val = 0;
    bool is_cpd = strcmp(key, "cpd_ps") == 0;
    bool is_min = strcmp(key, "slack_min_ps") == 0;
    if (!is_cpd && !is_min && strcmp(key, "ns_per_level") != 0) {
        return false;
    }
    if (!num_of(fld[2], false, &val)) {
        *st = perr(hp, "not a number", fld[2]);
        return true;
    }
    if (is_cpd) {
        tbl->cpd_ps = val;
    } else if (is_min) {
        tbl->slack_min_ps = val;
    } else {
        tbl->ns_per_level = val;
    }
    hp->have_cpd = hp->have_cpd || is_cpd;
    hp->have_min = hp->have_min || is_min;
    hp->have_npl = hp->have_npl || (!is_cpd && !is_min);
    return true;
}

static odin3_status meta_line(hparse *hp, char **fld, size_t nfld) {
    if (nfld != META_FIELDS) {
        return perr(hp, "a meta line has 3 fields", NULL);
    }
    odin3_hints *tbl = hp->hints;
    odin3_status st = ODIN3_OK;
    if (meta_number(hp, fld, &st)) {
        return st;
    }
    if (strcmp(fld[1], "tighten_levels") == 0) {
        double val = 0;
        if (!num_of(fld[2], false, &val) || val < 0.0) {
            return perr(hp, "tighten_levels must be a number from 0", fld[2]);
        }
        tbl->tighten_levels = val;
        return ODIN3_OK;
    }
    if (strcmp(fld[1], "relax_levels") == 0) {
        double val = 0;
        if (!num_of(fld[2], false, &val) || val < 0.0) {
            return perr(hp, "relax_levels must be a number from 0", fld[2]);
        }
        tbl->relax_levels = val;
        return ODIN3_OK;
    }
    if (strcmp(fld[1], "depth") == 0) {
        if (!uint_of(fld[2], &tbl->depth)) {
            return perr(hp, "not an unsigned number", fld[2]);
        }
        hp->have_depth = true;
        return ODIN3_OK;
    }
    size_t used = strlen(tbl->source);
    (void)snprintf(tbl->source + used, sizeof tbl->source - used, "%s%s=%s", used != 0 ? " " : "",
                   fld[1], fld[2]);
    return ODIN3_OK;
}

/* A growable array: where its pointer and capacity live, and its element size. */
typedef struct vec_ref {
    void **arr;
    size_t *cap;
    size_t elem;
} vec_ref;

static odin3_status grow(vec_ref vec, size_t need) {
    if (need <= *vec.cap) {
        return ODIN3_OK;
    }
    size_t ncap = *vec.cap != 0 ? *vec.cap * 2 : FIRST_CAP;
    void *mem = odin3_util_realloc(*vec.arr, ncap * vec.elem);
    if (mem == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *vec.arr = mem;
    *vec.cap = ncap;
    return ODIN3_OK;
}

static odin3_status cell_line(hparse *hp, char **fld, size_t nfld) {
    odin3_hint_cell rec = {0};
    double slack = 0;
    double crit = 0;
    if (nfld != CELL_FIELDS && nfld != CELL_FIELDS + 1) {
        return perr(hp, "a cell line has 6 fields (7 with `relax`)", NULL);
    }
    if (nfld == CELL_FIELDS + 1) {
        if (strcmp(fld[CELL_FIELDS], "relax") != 0) {
            return perr(hp, "the 7th field of a cell line is `relax`", fld[CELL_FIELDS]);
        }
        rec.relax = 1;
    }
    if (!uint_of(fld[1], &rec.uid) || rec.uid == 0) {
        return perr(hp, "not a record ID", fld[1]);
    }
    if (!num_of(fld[2], false, &slack)) {
        return perr(hp, "not a number", fld[2]);
    }
    if (!num_of(fld[3], false, &crit)) {
        return perr(hp, "not a number", fld[3]);
    }
    if (!uint_of(fld[4], &rec.depth)) {
        return perr(hp, "not an unsigned number", fld[4]);
    }
    if (!uint_of(fld[CELL_LUTS_FIELD], &rec.luts)) {
        return perr(hp, "not an unsigned number", fld[CELL_LUTS_FIELD]);
    }
    if (rec.depth == 0 && rec.luts == 0) {
        return perr(hp, "a cell hint with depth 0 and luts 0 together", fld[1]);
    }
    rec.slack_ps = slack;
    rec.crit = crit;
    odin3_hints *tbl = hp->hints;
    if (grow((vec_ref){(void **)&tbl->cells, &hp->cap_cells, sizeof *tbl->cells},
             tbl->n_cells + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    tbl->cells[tbl->n_cells++] = rec;
    return ODIN3_OK;
}

static odin3_status net_line(hparse *hp, char **fld, size_t nfld) {
    odin3_hint_net rec = {0};
    if (nfld != NET_FIELDS) {
        return perr(hp, "a net line has 4 fields", NULL);
    }
    if (strchr(fld[1], '/') == NULL) {
        return perr(hp, "a net name is <module>/<net>", fld[1]);
    }
    if (!num_of(fld[2], true, &rec.arrival_ps)) {
        return perr(hp, "not a number", fld[2]);
    }
    if (!num_of(fld[3], true, &rec.required_ps)) {
        return perr(hp, "not a number", fld[3]);
    }
    odin3_hints *tbl = hp->hints;
    if (grow((vec_ref){(void **)&tbl->nets, &hp->cap_nets, sizeof *tbl->nets}, tbl->n_nets + 1) !=
        ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rec.name = dup_str(fld[1]);
    if (rec.name == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    tbl->nets[tbl->n_nets++] = rec;
    return ODIN3_OK;
}

static size_t split_tabs(char *line, char **fld) {
    size_t count = 0;
    char *at = line;
    while (count < MAX_FIELDS) {
        fld[count++] = at;
        char *tab = strchr(at, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        at = tab + 1;
    }
    return count;
}

static odin3_status one_line(hparse *hp, char *line, bool *first) {
    char *fld[MAX_FIELDS] = {0};
    size_t nfld = split_tabs(line, fld);
    if (*first) {
        *first = false;
        char want[WANT_BYTES];
        (void)snprintf(want, sizeof want, "%d", ODIN3_HINTS_VERSION);
        if (nfld != 2 || strcmp(fld[0], ODIN3_HINTS_MAGIC) != 0 || strcmp(fld[1], want) != 0) {
            return perr(hp,
                        "not a timing hint file (first line must be "
                        "`" ODIN3_HINTS_MAGIC "<TAB>1`)",
                        NULL);
        }
        return ODIN3_OK;
    }
    if (strcmp(fld[0], "meta") == 0) {
        return meta_line(hp, fld, nfld);
    }
    if (strcmp(fld[0], "cell") == 0) {
        return cell_line(hp, fld, nfld);
    }
    if (strcmp(fld[0], "net") == 0) {
        return net_line(hp, fld, nfld);
    }
    return perr(hp, "unknown record kind", fld[0]);
}

static int cmp_cell(const void *lhs, const void *rhs) {
    const odin3_hint_cell *left = lhs;
    const odin3_hint_cell *right = rhs;
    if (left->uid != right->uid) {
        return left->uid < right->uid ? -1 : 1;
    }
    return 0;
}

static int cmp_net(const void *lhs, const void *rhs) {
    return strcmp(((const odin3_hint_net *)lhs)->name, ((const odin3_hint_net *)rhs)->name);
}

/* The worse of two NANs-aware values: smaller wins when want_min, else larger. */
static double pick(double first, double second, bool want_min) {
    if (isnan(first)) {
        return second;
    }
    if (isnan(second)) {
        return first;
    }
    return want_min ? fmin(first, second) : fmax(first, second);
}

/* Sorts, then merges duplicates keeping the worst. */
static void merge_dups(odin3_hints *tbl) {
    if (tbl->n_cells > 1) {
        qsort(tbl->cells, tbl->n_cells, sizeof *tbl->cells, cmp_cell);
    }
    size_t out = 0;
    for (size_t i = 0; i < tbl->n_cells; i++) {
        if (out > 0 && tbl->cells[out - 1].uid == tbl->cells[i].uid) {
            odin3_hint_cell *to = &tbl->cells[out - 1];
            to->slack_ps = fmin(to->slack_ps, tbl->cells[i].slack_ps);
            to->crit = fmax(to->crit, tbl->cells[i].crit);
            to->depth = to->depth < tbl->cells[i].depth ? to->depth : tbl->cells[i].depth;
            to->luts += tbl->cells[i].luts;
            to->relax = (uint8_t)(to->relax && tbl->cells[i].relax);
        } else {
            tbl->cells[out++] = tbl->cells[i];
        }
    }
    tbl->n_cells = out;
    if (tbl->n_nets > 1) {
        qsort(tbl->nets, tbl->n_nets, sizeof *tbl->nets, cmp_net);
    }
    out = 0;
    for (size_t i = 0; i < tbl->n_nets; i++) {
        if (out > 0 && strcmp(tbl->nets[out - 1].name, tbl->nets[i].name) == 0) {
            odin3_hint_net *to = &tbl->nets[out - 1];
            to->arrival_ps = pick(to->arrival_ps, tbl->nets[i].arrival_ps, false);
            to->required_ps = pick(to->required_ps, tbl->nets[i].required_ps, true);
            odin3_util_free(tbl->nets[i].name);
        } else {
            tbl->nets[out++] = tbl->nets[i];
        }
    }
    tbl->n_nets = out;
}

/* The meta numbers the records need, and the ns per level they give. */
static odin3_status check_units(hparse *hp) {
    odin3_hints *tbl = hp->hints;
    bool records = tbl->n_cells != 0 || tbl->n_nets != 0;
    if (records && !(hp->have_cpd && hp->have_depth && hp->have_min)) {
        return perr(hp, "cell and net records need meta lines cpd_ps, depth and slack_min_ps",
                    NULL);
    }
    if (!hp->have_npl) {
        tbl->ns_per_level = tbl->depth != 0 ? tbl->cpd_ps / PS_PER_NS / (double)tbl->depth : 0.0;
    }
    if (records && !(tbl->ns_per_level > 0.0)) {
        return perr(hp, "ns_per_level (cpd_ps / depth) must be positive", NULL);
    }
    if (records && tbl->ns_per_level < NS_PER_LEVEL_FLOOR) {
        odin3_log(ODIN3_LOG_ERROR,
                  "read_hints: %s: ns_per_level %g is below the floor %g ns (a level is the unit "
                  "the ABC timing file is written in)",
                  hp->label, tbl->ns_per_level, NS_PER_LEVEL_FLOOR);
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

static odin3_status finish_parse(hparse *hp, bool first) {
    odin3_hints *tbl = hp->hints;
    if (first) {
        hp->line = 1;
        return perr(hp, "empty file", NULL);
    }
    odin3_status st = check_units(hp);
    if (st != ODIN3_OK) {
        return st;
    }
    merge_dups(tbl);
    for (size_t i = 0; i < tbl->n_cells; i++) {
        if (tbl->cells[i].slack_ps < tbl->slack_min_ps) {
            odin3_log(ODIN3_LOG_ERROR,
                      "read_hints: %s: cell %" PRIu32 " has slack %.1f ps, below the file's "
                      "slack_min_ps %.1f",
                      hp->label, tbl->cells[i].uid, tbl->cells[i].slack_ps, tbl->slack_min_ps);
            return ODIN3_ERR_PARSE;
        }
    }
    tbl->cell_used = odin3_util_calloc(tbl->n_cells + 1);
    tbl->net_used = odin3_util_calloc(tbl->n_nets + 1);
    return tbl->cell_used != NULL && tbl->net_used != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* The records of copy (split in place), one line at a time; first says no line was read yet. */
static odin3_status scan_lines(hparse *hp, char *copy, bool *first) {
    odin3_status st = ODIN3_OK;
    char *at = copy;
    while (st == ODIN3_OK && *at != '\0') {
        char *eol = strchr(at, '\n');
        if (eol != NULL) {
            *eol = '\0';
        }
        size_t ll = strlen(at);
        if (ll > 0 && at[ll - 1] == '\r') {
            at[ll - 1] = '\0';
        }
        hp->line++;
        if (at[0] != '\0' && (at[0] != '#' || *first)) {
            st = one_line(hp, at, first);
        }
        at = eol != NULL ? eol + 1 : at + strlen(at);
    }
    return st;
}

/* The error for a NUL byte in the text: the line it is on. */
static odin3_status nul_error(hparse *hp, const char *copy) {
    hp->line = 1;
    for (const char *ch = copy; *ch != '\0'; ch++) {
        hp->line += *ch == '\n' ? 1U : 0U;
    }
    return perr(hp, "a NUL byte in the file", NULL);
}

odin3_status odin3_hints_parse(const char *text, size_t len, const char *label, odin3_hints **out) {
    *out = NULL;
    hparse hp = {.label = label};
    hp.hints = odin3_util_calloc(sizeof *hp.hints);
    if (hp.hints != NULL) {
        hp.hints->tighten_levels = ODIN3_HINTS_TIGHTEN_DEFAULT;
        hp.hints->relax_levels = 1.0;
    }
    char *copy = odin3_util_malloc(len + 1);
    if (hp.hints == NULL || copy == NULL) {
        odin3_util_free(hp.hints);
        odin3_util_free(copy);
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    odin3_status st = ODIN3_OK;
    bool first = true;
    if (strlen(copy) != len) {
        st = nul_error(&hp, copy);
    } else {
        hp.hints->label = dup_str(label);
        st = hp.hints->label != NULL ? scan_lines(&hp, copy, &first) : ODIN3_ERR_NO_MEMORY;
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
    odin3_hint_cell *hit = hints->n_cells == 0 ? NULL
                                               : bsearch(&key, hints->cells, hints->n_cells,
                                                         sizeof *hints->cells, cmp_cell);
    if (hit != NULL) {
        hints->cell_used[hit - hints->cells] = 1;
    }
    return hit;
}

const odin3_hint_net *odin3_hints_find_net(odin3_hints *hints, const char *name) {
    const odin3_hint_net key = {.name = (char *)name};
    odin3_hint_net *hit = hints->n_nets == 0 ? NULL
                                             : bsearch(&key, hints->nets, hints->n_nets,
                                                       sizeof *hints->nets, cmp_net);
    if (hit != NULL) {
        hints->net_used[hit - hints->nets] = 1;
    }
    return hit;
}

static size_t count_used(const uint8_t *used, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
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

odin3_status odin3_hints_fetch(const odin3_design *design, odin3_hints **out) {
    *out = NULL;
    uint64_t serial = odin3_design_serial(design);
    if (g_cache.serial != serial) {
        if (g_cache.hints != NULL) {
            odin3_log(ODIN3_LOG_WARN,
                      "read_hints: the hints of %s were loaded for an older design (serial "
                      "%" PRIu64 ", this design is %" PRIu64 "); they are dropped",
                      g_cache.hints->label, g_cache.serial, serial);
        }
        odin3_hints_reset();
        g_cache.serial = serial;
    }
    if (g_cache.hints != NULL || g_cache.project_tried) {
        *out = g_cache.hints;
        return ODIN3_OK;
    }
    const odin3_project *proj = odin3_design_project(design);
    if (proj != NULL && proj->timing_hints.given != 0) {
        const char *path =
            odin3_strtab_get(odin3_design_strtab(design), proj->timing_hints.resolved);
        odin3_status st = read_file(path, &g_cache.hints);
        if (st != ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR,
                      "read_hints: the project's timing_hints file %s is not "
                      "usable; fix it or remove the key",
                      path);
            return st;
        }
    }
    g_cache.project_tried = true;
    *out = g_cache.hints;
    return ODIN3_OK;
}

odin3_hints *odin3_hints_of(const odin3_design *design) {
    odin3_hints *hints = NULL;
    return odin3_hints_fetch(design, &hints) == ODIN3_OK ? hints : NULL;
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
        const odin3_hints *tbl = odin3_hints_of(ctx->design);
        odin3_log(ODIN3_LOG_INFO,
                  "read_hints: %s: %zu cell hints, %zu net hints, pass-1 cpd %.0f ps, depth %u, "
                  "%.4f ns per LUT level%s%s",
                  path, tbl->n_cells, tbl->n_nets, tbl->cpd_ps, (unsigned)tbl->depth,
                  tbl->ns_per_level, tbl->source[0] != '\0' ? "; " : "", tbl->source);
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
