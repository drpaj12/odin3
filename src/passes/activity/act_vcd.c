/*
 * act_vcd.c — primary input statistics from a file of `name P1 D` lines (--pi-file) or from a VCD
 * waveform (--vcd): each input is sampled just before every rising edge of the clock, P1 = ones /
 * samples, D = changes / (samples - 1), as `sim_tb --vcd` writes end-of-step values.
 */
#include "passes/activity/act_int.h"
#include "util/alloc.h"
#include "util/log.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NAME_CAP = 1024,
    ID_MAX = 24,
    TOKENS_MAX = 16,
    MAX_VAR_WIDTH = 4096,
    FIRST_CAP = 64,
    MIN_HASH = 16,
    HASH_SEED = 5381,
    HASH_MUL = 33,
    VAR_TOKENS = 5,
    TOK_WIDTH = 2,
    TOK_ID = 3,
    TOK_NAME = 4,
    MIN_SAMPLES = 2,
    DECIMAL = 10,
    PI_WORDS = 3
};
static const double k_two = 2.0;
static const double k_slack = 1e-9;
static const int k_unset_byte = 0xFF;

/* --- primary input names ---------------------------------------------------------------------- */

typedef struct pi_name {
    const char *name;
    char *owned;
    uint32_t net;
} pi_name;

static int pi_cmp(const void *pa, const void *pb) {
    return strcmp(((const pi_name *)pa)->name, ((const pi_name *)pb)->name);
}

typedef struct pi_set {
    pi_name *items;
    size_t count;
} pi_set;

static void pi_set_free(pi_set *set) {
    for (size_t kk = 0; kk < set->count; kk++) {
        odin3_util_free(set->items[kk].owned);
    }
    odin3_util_free(set->items);
    memset(set, 0, sizeof *set);
}

static char *dup_text(const char *text) {
    size_t len = strlen(text);
    char *copy = odin3_util_malloc(len + 1U);
    if (copy != NULL) {
        memcpy(copy, text, len + 1U);
    }
    return copy;
}

/* Copies text into dst of size bytes, cut to fit. */
static void copy_text(char *dst, size_t size, const char *text) {
    size_t len = strlen(text);
    len = len < size ? len : size - 1U;
    memcpy(dst, text, len);
    dst[len] = '\0';
}

/* The non-clock primary inputs by name, sorted. */
static odin3_status pi_set_build(const act_model *md, act_name_fn name_of, void *ctx, pi_set *set) {
    size_t cap = 0;
    for (uint32_t id = 1; id < md->n_nets; id++) {
        cap += md->kind[id] == ACT_PI ? 1U : 0U;
    }
    set->items = odin3_util_calloc((cap + 1U) * sizeof *set->items);
    if (set->items == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t id = 1; id < md->n_nets; id++) {
        odin3_strbuf nm;
        odin3_strbuf_init(&nm);
        if (md->kind[id] == ACT_PI && name_of(ctx, id, &nm) && nm.len > 0) {
            set->items[set->count].owned = dup_text(nm.data);
            set->items[set->count].name = set->items[set->count].owned;
            set->items[set->count++].net = id;
        }
        odin3_strbuf_free(&nm);
    }
    qsort(set->items, set->count, sizeof *set->items, pi_cmp);
    return ODIN3_OK;
}

static const pi_name *pi_find(const pi_set *set, const char *name) {
    pi_name key = {name, NULL, 0};
    return bsearch(&key, set->items, set->count, sizeof *set->items, pi_cmp);
}

/* The largest density a stationary signal of probability pp can have. */
static double max_density(double pp) {
    return k_two * fmin(pp, 1.0 - pp);
}

static bool valid_pair(double pp, double dd) {
    return pp >= 0.0 && pp <= 1.0 && dd >= 0.0 && dd <= max_density(pp) + k_slack;
}

/* --- --pi-file -------------------------------------------------------------------------------- */

typedef struct pi_line {
    const char *name;
    double prob, dens;
} pi_line;

/* One `name P1 D` line; false for a blank or malformed line. */
static bool parse_pi_line(char *line, pi_line *out) {
    char *save = NULL;
    char *words[PI_WORDS];
    for (size_t kk = 0; kk < PI_WORDS; kk++) {
        words[kk] = strtok_r(kk == 0 ? line : NULL, " \t\r\n", &save);
        if (words[kk] == NULL) {
            return false;
        }
    }
    char *end1 = NULL;
    char *end2 = NULL;
    out->name = words[0];
    out->prob = strtod(words[1], &end1);
    out->dens = strtod(words[2], &end2);
    return *end1 == '\0' && *end2 == '\0';
}

static odin3_status apply_pi_line(act_model *md, const pi_set *set, char *line) {
    pi_line pl;
    if (!parse_pi_line(line, &pl)) {
        return ODIN3_OK;
    }
    const pi_name *hit = pi_find(set, pl.name);
    if (hit == NULL) {
        odin3_log(ODIN3_LOG_WARN, "activity: --pi-file: '%s' is not a primary input", pl.name);
    } else if (!valid_pair(pl.prob, pl.dens)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "activity: --pi-file: '%s' has P1 %g and density %g (need 0 <= D <= 2 min(P1, "
                  "1 - P1))",
                  pl.name, pl.prob, pl.dens);
        return ODIN3_ERR_INVALID_ARG;
    } else {
        md->prob[hit->net] = pl.prob;
        md->dens[hit->net] = pl.dens;
    }
    return ODIN3_OK;
}

odin3_status act_read_pi_file(act_model *md, const char *path, act_name_fn name_of, void *ctx) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "activity: cannot read '%s'", path);
        return ODIN3_ERR_IO;
    }
    pi_set set = {0};
    odin3_status st = pi_set_build(md, name_of, ctx, &set);
    char *line = NULL;
    size_t cap = 0;
    while (st == ODIN3_OK && getline(&line, &cap, fp) >= 0) {
        st = apply_pi_line(md, &set, line);
    }
    free(line);
    (void)fclose(fp);
    pi_set_free(&set);
    return st;
}

/* --- --vcd ------------------------------------------------------------------------------------ */

typedef struct vcd_var {
    char id[ID_MAX];
    uint32_t width;
    char *cur, *pre; /* width characters each: value now, value before this timestamp */
    uint64_t dirty;  /* timestamp counter at which pre was saved */
} vcd_var;

typedef struct vcd_bit {
    uint32_t var, bit, net;
    int prev;
    uint64_t ones, toggles;
} vcd_bit;

typedef struct vcd_state {
    vcd_var *vars;
    size_t n_vars, cap_vars;
    vcd_bit *bits;
    size_t n_bits, cap_bits;
    int clk_var;
    int32_t *htab; /* open-addressing table of variable indices by id code */
    size_t hmask;
    uint64_t samples, stamp;
    const pi_set *set;
    const char *scope; /* wanted scope path or "" */
    const char *clock;
} vcd_state;

static void vcd_free(vcd_state *vs) {
    for (size_t kk = 0; kk < vs->n_vars; kk++) {
        odin3_util_free(vs->vars[kk].cur);
        odin3_util_free(vs->vars[kk].pre);
    }
    odin3_util_free(vs->vars);
    odin3_util_free(vs->bits);
    odin3_util_free(vs->htab);
}

static bool grow_vars(vcd_state *vs) {
    if (vs->n_vars < vs->cap_vars) {
        return true;
    }
    size_t cap = vs->cap_vars == 0 ? FIRST_CAP : vs->cap_vars * 2U;
    vcd_var *grown = odin3_util_realloc(vs->vars, cap * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    vs->vars = grown;
    vs->cap_vars = cap;
    return true;
}

/* The variable of this id code (VCD aliases share one), else a new one. */
static vcd_var *add_var(vcd_state *vs, const char *id, uint32_t width) {
    for (size_t kk = vs->n_vars; kk > 0; kk--) {
        if (strcmp(vs->vars[kk - 1U].id, id) == 0) {
            return &vs->vars[kk - 1U];
        }
    }
    if (!grow_vars(vs)) {
        return NULL;
    }
    vcd_var *var = &vs->vars[vs->n_vars++];
    memset(var, 0, sizeof *var);
    copy_text(var->id, sizeof var->id, id);
    var->width = width;
    var->cur = odin3_util_malloc(width);
    var->pre = odin3_util_malloc(width);
    if (var->cur == NULL || var->pre == NULL) {
        return NULL;
    }
    memset(var->cur, '0', width);
    memset(var->pre, '0', width);
    return var;
}

static bool add_bit(vcd_state *vs, vcd_bit bit) {
    if (vs->n_bits == vs->cap_bits) {
        size_t cap = vs->cap_bits == 0 ? FIRST_CAP : vs->cap_bits * 2U;
        vcd_bit *grown = odin3_util_realloc(vs->bits, cap * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        vs->bits = grown;
        vs->cap_bits = cap;
    }
    vs->bits[vs->n_bits++] = bit;
    return true;
}

/* Whitespace-separated tokens of a header line. */
static size_t split_tokens(char *line, char *tok[TOKENS_MAX]) {
    size_t cnt = 0;
    char *save = NULL;
    for (char *cur = strtok_r(line, " \t\r\n", &save); cur != NULL && cnt < TOKENS_MAX;
         cur = strtok_r(NULL, " \t\r\n", &save)) {
        tok[cnt++] = cur;
    }
    return cnt;
}

static bool scope_ok(const vcd_state *vs, const char *path) {
    size_t len = strlen(vs->scope);
    return len == 0 ||
           (strncmp(path, vs->scope, len) == 0 && (path[len] == '.' || path[len] == '\0'));
}

/* The key a bit of a variable is looked up under: `name` for a scalar, `name[bit]` for a vector. */
static void bit_key(char *key, size_t size, const char *name, int bit) {
    if (bit >= 0) {
        (void)snprintf(key, size, "%s[%u]", name, (unsigned)bit);
    } else {
        copy_text(key, size, name);
    }
}

/* Tracks every bit of the variable that is a primary input; the variable or NULL. */
static odin3_status track_bits(vcd_state *vs, char *tok[], uint32_t width, vcd_var **out) {
    char key[NAME_CAP];
    *out = NULL;
    for (uint32_t bit = 0; bit < width; bit++) {
        bit_key(key, sizeof key, tok[TOK_NAME], width > 1U ? (int)bit : -1);
        const pi_name *hit = pi_find(vs->set, key);
        if (hit == NULL) {
            continue;
        }
        *out = *out != NULL ? *out : add_var(vs, tok[TOK_ID], width);
        if (*out == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        vcd_bit tracked = {
            .var = (uint32_t)(*out - vs->vars), .bit = bit, .net = hit->net, .prev = -1};
        if (!add_bit(vs, tracked)) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* $var type width id name [range] $end */
static odin3_status header_var(vcd_state *vs, char *tok[], size_t cnt, const char *path) {
    uint32_t width = cnt >= VAR_TOKENS ? (uint32_t)strtoul(tok[TOK_WIDTH], NULL, DECIMAL) : 0U;
    if (width == 0 || width > MAX_VAR_WIDTH || !scope_ok(vs, path)) {
        return ODIN3_OK;
    }
    vcd_var *var = NULL;
    odin3_status st = track_bits(vs, tok, width, &var);
    if (st == ODIN3_OK && vs->clk_var < 0 && strcmp(tok[TOK_NAME], vs->clock) == 0) {
        var = var != NULL ? var : add_var(vs, tok[TOK_ID], width);
        vs->clk_var = var != NULL ? (int)(var - vs->vars) : -1;
        st = var != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    }
    return st;
}

static size_t id_hash(const char *id) {
    size_t hash = HASH_SEED;
    for (; *id != '\0'; id++) {
        hash = hash * HASH_MUL + (unsigned char)*id;
    }
    return hash;
}

/* Hash table over the variable id codes (built after the header). */
static odin3_status build_hash(vcd_state *vs) {
    size_t size = MIN_HASH;
    while (size < vs->n_vars * 2U) {
        size *= 2U;
    }
    vs->htab = odin3_util_malloc(size * sizeof *vs->htab);
    if (vs->htab == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(vs->htab, k_unset_byte, size * sizeof *vs->htab);
    vs->hmask = size - 1U;
    for (size_t kk = 0; kk < vs->n_vars; kk++) {
        size_t pos = id_hash(vs->vars[kk].id) & vs->hmask;
        while (vs->htab[pos] >= 0) {
            pos = (pos + 1U) & vs->hmask;
        }
        vs->htab[pos] = (int32_t)kk;
    }
    return ODIN3_OK;
}

/* Keeps the scope path in step with a $scope / $upscope line. */
static void track_scope(char *path, size_t size, char *tok[], size_t cnt) {
    if (cnt >= TOK_ID && strcmp(tok[0], "$scope") == 0) {
        size_t len = strlen(path);
        if (len > 0 && len + 1U < size) {
            path[len++] = '.';
            path[len] = '\0';
        }
        copy_text(path + len, size - len, tok[TOK_WIDTH]);
    } else if (cnt >= 1U && strcmp(tok[0], "$upscope") == 0) {
        const char *dot = strrchr(path, '.');
        path[dot != NULL ? (size_t)(dot - path) : 0U] = '\0';
    }
}

/* Reads the declarations; leaves fp after $enddefinitions. */
static odin3_status read_header(FILE *fp, vcd_state *vs) {
    char *line = NULL;
    size_t cap = 0;
    char path[NAME_CAP] = "";
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && getline(&line, &cap, fp) >= 0) {
        char *tok[TOKENS_MAX];
        size_t cnt = split_tokens(line, tok);
        if (cnt >= 1U && strcmp(tok[0], "$enddefinitions") == 0) {
            break;
        }
        track_scope(path, sizeof path, tok, cnt);
        st = (cnt >= 1U && strcmp(tok[0], "$var") == 0) ? header_var(vs, tok, cnt, path) : st;
    }
    free(line);
    return st;
}

static vcd_var *find_var(vcd_state *vs, const char *id) {
    size_t pos = id_hash(id) & vs->hmask;
    while (vs->htab[pos] >= 0) {
        vcd_var *var = &vs->vars[vs->htab[pos]];
        if (strcmp(var->id, id) == 0) {
            return var;
        }
        pos = (pos + 1U) & vs->hmask;
    }
    return NULL;
}

static void set_value(const vcd_state *vs, vcd_var *var, const char *text) {
    if (var->dirty != vs->stamp) {
        memcpy(var->pre, var->cur, var->width);
        var->dirty = vs->stamp;
    }
    size_t len = strlen(text);
    for (uint32_t kk = 0; kk < var->width; kk++) {
        bool one = kk < len && text[len - 1U - kk] == '1';
        var->cur[var->width - 1U - kk] = one ? '1' : '0';
    }
}

/* Applies one value-change line: `<v><id>` or `b<bits> <id>`. */
static void apply_change(vcd_state *vs, char *line) {
    char one[2] = {line[0], '\0'};
    char *text = one;
    char *id = line + 1;
    if (line[0] == 'b' || line[0] == 'B') {
        text = line + 1;
        id = text + strcspn(text, " \t");
        if (*id != '\0') {
            *id++ = '\0';
        }
        id += strspn(id, " \t");
    }
    id[strcspn(id, " \t\r\n")] = '\0';
    vcd_var *var = find_var(vs, id);
    if (var != NULL) {
        set_value(vs, var, text);
    }
}

static int bit_of(const vcd_var *var, uint32_t bit, bool pre) {
    const char *txt = pre ? var->pre : var->cur;
    return txt[var->width - 1U - bit] == '1' ? 1 : 0;
}

/* One sample of every tracked input: the value before the changes of the current timestamp. */
static void take_sample(vcd_state *vs) {
    for (size_t kk = 0; kk < vs->n_bits; kk++) {
        vcd_bit *bit = &vs->bits[kk];
        const vcd_var *var = &vs->vars[bit->var];
        int val = bit_of(var, bit->bit, var->dirty == vs->stamp);
        bit->ones += (uint64_t)val;
        bit->toggles += (bit->prev >= 0 && bit->prev != val) ? 1U : 0U;
        bit->prev = val;
    }
    vs->samples++;
}

/* Closes the timestamp: a sample when the clock rose in it (clock 0 before, 1 after). */
static void close_stamp(vcd_state *vs) {
    if (vs->clk_var < 0) {
        return;
    }
    const vcd_var *clk = &vs->vars[vs->clk_var];
    if (clk->dirty == vs->stamp && clk->pre[0] == '0' && clk->cur[0] == '1') {
        take_sample(vs);
    }
}

static bool is_change_line(char lead) {
    return lead != '\0' && strchr("01xXzZbB", lead) != NULL;
}

static void read_changes(FILE *fp, vcd_state *vs) {
    char *line = NULL;
    size_t cap = 0;
    vs->stamp = 1;
    while (getline(&line, &cap, fp) >= 0) {
        if (line[0] == '#') {
            close_stamp(vs);
            vs->stamp++;
        } else if (is_change_line(line[0])) {
            apply_change(vs, line);
        }
    }
    close_stamp(vs);
    free(line);
}

static odin3_status store_bits(act_model *md, const vcd_state *vs) {
    if (vs->samples < MIN_SAMPLES) {
        odin3_log(ODIN3_LOG_ERROR, "activity: --vcd: %llu rising clock edges, need at least 2",
                  (unsigned long long)vs->samples);
        return ODIN3_ERR_INVALID_ARG;
    }
    double num = (double)vs->samples;
    for (size_t kk = 0; kk < vs->n_bits; kk++) {
        const vcd_bit *bit = &vs->bits[kk];
        double pp = (double)bit->ones / num;
        double dd = (double)bit->toggles / (num - 1.0);
        md->prob[bit->net] = pp;
        md->dens[bit->net] = fmin(dd, max_density(pp));
    }
    odin3_log(ODIN3_LOG_INFO, "activity: --vcd: %llu clock edges, %zu primary-input bits measured",
              (unsigned long long)vs->samples, vs->n_bits);
    return ODIN3_OK;
}

odin3_status act_read_vcd(act_model *md, const act_opts *opts, act_name_fn name_of, void *ctx) {
    FILE *fp = fopen(opts->vcd.data, "r");
    if (fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "activity: cannot read '%s'", opts->vcd.data);
        return ODIN3_ERR_IO;
    }
    pi_set set = {0};
    vcd_state vs = {.clk_var = -1, .set = &set};
    vs.scope = opts->vcd_scope.len > 0 ? opts->vcd_scope.data : "";
    vs.clock = opts->vcd_clock.len > 0 ? opts->vcd_clock.data : "";
    odin3_status st = pi_set_build(md, name_of, ctx, &set);
    st = st == ODIN3_OK ? read_header(fp, &vs) : st;
    if (st == ODIN3_OK && vs.clk_var < 0) {
        odin3_log(ODIN3_LOG_ERROR, "activity: --vcd: clock variable '%s' not found (--vcd-clock)",
                  vs.clock);
        st = ODIN3_ERR_INVALID_ARG;
    }
    st = st == ODIN3_OK ? build_hash(&vs) : st;
    if (st == ODIN3_OK) {
        read_changes(fp, &vs);
        st = store_bits(md, &vs);
    }
    (void)fclose(fp);
    vcd_free(&vs);
    pi_set_free(&set);
    return st;
}
