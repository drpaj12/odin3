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

static int i64_cmp(const void *lhs, const void *rhs) {
    int64_t a = *(const int64_t *)lhs;
    int64_t b = *(const int64_t *)rhs;
    return a < b ? -1 : (a > b ? 1 : 0);
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
        uint32_t j = i;
        while (j < count && vals[j] == vals[i]) {
            j++;
        }
        if (*used == *cap) {
            *cap = *cap * 2 + 16;
            sig->deg = odin3_util_realloc(sig->deg, *cap * sizeof *sig->deg);
            sig->num = odin3_util_realloc(sig->num, *cap * sizeof *sig->num);
            if (sig->deg == NULL || sig->num == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
        sig->deg[*used] = vals[i];
        sig->num[*used] = j - i;
        (*used)++;
        i = j;
    }
    return ODIN3_OK;
}

odin3_status glet_sig_make(glet_sig *sig, const int64_t *counts, uint32_t n, uint32_t norb,
                           const uint8_t *mask) {
    memset(sig, 0, sizeof *sig);
    sig->norb = norb;
    sig->totals = odin3_util_calloc((size_t)norb * sizeof *sig->totals);
    sig->off = odin3_util_calloc(((size_t)norb + 1) * sizeof *sig->off);
    int64_t *vals = odin3_util_calloc(((size_t)n + 1) * sizeof *vals);
    if (sig->totals == NULL || sig->off == NULL || vals == NULL) {
        odin3_util_free(vals);
        glet_sig_free(sig);
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t used = 0;
    size_t cap = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t v = 0; v < n; v++) {
        sig->nodes += mask == NULL || mask[v] != 0 ? 1U : 0U;
    }
    for (uint32_t o = 0; st == ODIN3_OK && o < norb; o++) {
        uint32_t cnt = 0;
        for (uint32_t v = 0; v < n; v++) {
            int64_t c = counts[(size_t)v * norb + o];
            if (mask != NULL && mask[v] == 0) {
                continue;
            }
            sig->totals[o] += c;
            vals[cnt] = c;
            cnt += c > 0 ? 1U : 0U;
        }
        qsort(vals, cnt, sizeof *vals, i64_cmp);
        sig->off[o] = (uint32_t)used;
        st = add_runs(sig, vals, cnt, &used, &cap);
    }
    sig->off[norb] = (uint32_t)used;
    odin3_util_free(vals);
    if (st != ODIN3_OK) {
        glet_sig_free(sig);
    }
    return st;
}

/* Scaled, normalised distribution of orbit j: S(k) = d(k) / k, divided by the sum. */
static double orbit_total_area(const glet_sig *sig, uint32_t j) {
    double area = 0;
    for (uint32_t i = sig->off[j]; i < sig->off[j + 1]; i++) {
        area += (double)sig->num[i] / (double)sig->deg[i];
    }
    return area;
}

static double orbit_agreement(const glet_sig *lhs, const glet_sig *rhs, uint32_t j) {
    double ta = orbit_total_area(lhs, j);
    double tb = orbit_total_area(rhs, j);
    if (ta == 0 && tb == 0) {
        return 1.0;
    }
    if (ta == 0 || tb == 0) {
        return 0.0;
    }
    uint32_t ia = lhs->off[j];
    uint32_t ib = rhs->off[j];
    double sum = 0;
    while (ia < lhs->off[j + 1] || ib < rhs->off[j + 1]) {
        bool has_a = ia < lhs->off[j + 1];
        bool has_b = ib < rhs->off[j + 1];
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
    return 1.0 - sqrt(sum) / sqrt(2.0);
}

double glet_gdd_agreement(const glet_sig *lhs, const glet_sig *rhs, double *per_orbit) {
    double total = 0;
    for (uint32_t j = 0; j < lhs->norb; j++) {
        double a = orbit_agreement(lhs, rhs, j);
        if (per_orbit != NULL) {
            per_orbit[j] = a;
        }
        total += a;
    }
    return lhs->norb == 0 ? 1.0 : total / (double)lhs->norb;
}

double glet_cosine_log(const glet_sig *lhs, const glet_sig *rhs) {
    double dot = 0;
    double na = 0;
    double nb = 0;
    for (uint32_t j = 0; j < lhs->norb; j++) {
        double a = log1p((double)lhs->totals[j]);
        double b = log1p((double)rhs->totals[j]);
        dot += a * b;
        na += a * a;
        nb += b * b;
    }
    if (na == 0 && nb == 0) {
        return 1.0;
    }
    return na == 0 || nb == 0 ? 0.0 : dot / (sqrt(na) * sqrt(nb));
}
