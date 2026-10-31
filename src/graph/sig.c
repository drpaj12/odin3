/*
 * sig.c — signatures of orbit counts and their similarity (spec sections 6 and 13): orbit totals,
 * the graphlet degree distribution (GDD) and the two metrics, GDD agreement (Przulj 2007) and the
 * cosine of log(1 + orbit totals).
 */
#include "graph/glet.h"
#include "util/alloc.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { SIG_GROW = 16 };
static const double k_sqrt2 = 1.4142135623730951;

static int i64_cmp(const void *lhs, const void *rhs) {
    int64_t xa = *(const int64_t *)lhs;
    int64_t xb = *(const int64_t *)rhs;
    if (xa != xb) {
        return xa < xb ? -1 : 1;
    }
    return 0;
}

void glet_sig_free(glet_sig *sig) {
    odin3_util_free(sig->totals);
    odin3_util_free(sig->off);
    odin3_util_free(sig->deg);
    odin3_util_free(sig->num);
    memset(sig, 0, sizeof *sig);
}

/* Appends the run-length pairs of the sorted values (all >= 1) of orbit j. */
static odin3_status add_runs(glet_sig *sig, const int64_t *vals, uint32_t count, size_t *used,
                             size_t *cap) {
    for (uint32_t i = 0; i < count;) {
        uint32_t jj = i;
        while (jj < count && vals[jj] == vals[i]) {
            jj++;
        }
        if (*used == *cap) {
            *cap = *cap * 2 + SIG_GROW;
            sig->deg = odin3_util_realloc(sig->deg, *cap * sizeof *sig->deg);
            sig->num = odin3_util_realloc(sig->num, *cap * sizeof *sig->num);
            if (sig->deg == NULL || sig->num == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
        sig->deg[*used] = vals[i];
        sig->num[*used] = jj - i;
        (*used)++;
        i = jj;
    }
    return ODIN3_OK;
}

/* Adds the column of orbit ob (selected rows) to the totals; returns the positive values sorted. */
static uint32_t orbit_column(glet_sig *sig, const glet_rows *rows, const uint8_t *mask,
                             int64_t *vals, uint32_t ob) {
    uint32_t cnt = 0;
    for (uint32_t vn = 0; vn < rows->n; vn++) {
        int64_t cd = rows->counts[(size_t)vn * rows->norb + ob];
        if (mask != NULL && mask[vn] == 0) {
            continue;
        }
        sig->totals[ob] += cd;
        vals[cnt] = cd;
        cnt += cd > 0 ? 1U : 0U;
    }
    qsort(vals, cnt, sizeof *vals, i64_cmp);
    return cnt;
}

odin3_status glet_sig_make(glet_sig *sig, const glet_rows *rows, const uint8_t *mask) {
    memset(sig, 0, sizeof *sig);
    sig->norb = rows->norb;
    sig->totals = odin3_util_calloc((size_t)rows->norb * sizeof *sig->totals);
    sig->off = odin3_util_calloc(((size_t)rows->norb + 1) * sizeof *sig->off);
    int64_t *vals = odin3_util_calloc(((size_t)rows->n + 1) * sizeof *vals);
    if (sig->totals == NULL || sig->off == NULL || vals == NULL) {
        odin3_util_free(vals);
        glet_sig_free(sig);
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t used = 0;
    size_t cap = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t vn = 0; vn < rows->n; vn++) {
        sig->nodes += mask == NULL || mask[vn] != 0 ? 1U : 0U;
    }
    for (uint32_t ob = 0; st == ODIN3_OK && ob < rows->norb; ob++) {
        uint32_t cnt = orbit_column(sig, rows, mask, vals, ob);
        sig->off[ob] = (uint32_t)used;
        st = add_runs(sig, vals, cnt, &used, &cap);
    }
    sig->off[rows->norb] = (uint32_t)used;
    odin3_util_free(vals);
    if (st != ODIN3_OK) {
        glet_sig_free(sig);
    }
    return st;
}

/* Scaled, normalised distribution of orbit j: S(k) = d(k) / k, divided by the sum. */
static double orbit_total_area(const glet_sig *sig, uint32_t jj) {
    double area = 0;
    for (uint32_t i = sig->off[jj]; i < sig->off[jj + 1]; i++) {
        area += (double)sig->num[i] / (double)sig->deg[i];
    }
    return area;
}

static double orbit_agreement(const glet_sig *lhs, const glet_sig *rhs, uint32_t jj) {
    double ta = orbit_total_area(lhs, jj);
    double tb = orbit_total_area(rhs, jj);
    if (ta == 0 && tb == 0) {
        return 1.0;
    }
    if (ta == 0 || tb == 0) {
        return 0.0;
    }
    uint32_t ia = lhs->off[jj];
    uint32_t ib = rhs->off[jj];
    double sum = 0;
    while (ia < lhs->off[jj + 1] || ib < rhs->off[jj + 1]) {
        bool has_a = ia < lhs->off[jj + 1];
        bool has_b = ib < rhs->off[jj + 1];
        int64_t ka = has_a ? lhs->deg[ia] : INT64_MAX;
        int64_t kb = has_b ? rhs->deg[ib] : INT64_MAX;
        double na = 0;
        double nb = 0;
        if (ka <= kb) {
            na = (double)lhs->num[ia] / (double)ka / ta;
            ia++;
        }
        if (kb <= ka) {
            nb = (double)rhs->num[ib] / (double)kb / tb;
            ib++;
        }
        sum += (na - nb) * (na - nb);
    }
    return 1.0 - sqrt(sum) / k_sqrt2;
}

double glet_gdd_agreement(const glet_sig *lhs, const glet_sig *rhs, double *per_orbit) {
    double total = 0;
    for (uint32_t jj = 0; jj < lhs->norb; jj++) {
        double xa = orbit_agreement(lhs, rhs, jj);
        if (per_orbit != NULL) {
            per_orbit[jj] = xa;
        }
        total += xa;
    }
    return lhs->norb == 0 ? 1.0 : total / (double)lhs->norb;
}

double glet_cosine_log(const glet_sig *lhs, const glet_sig *rhs) {
    double dot = 0;
    double na = 0;
    double nb = 0;
    for (uint32_t jj = 0; jj < lhs->norb; jj++) {
        double xa = log1p((double)lhs->totals[jj]);
        double xb = log1p((double)rhs->totals[jj]);
        dot += xa * xb;
        na += xa * xa;
        nb += xb * xb;
    }
    if (na == 0 && nb == 0) {
        return 1.0;
    }
    return na == 0 || nb == 0 ? 0.0 : dot / (sqrt(na) * sqrt(nb));
}
