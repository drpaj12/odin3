/*
 * act_prop.c — the two-frame evaluation of one function cell by mode products (spec section 3.1).
 */
#include "passes/activity/act_int.h"

#include <math.h>
#include <string.h>

static const double k_half = 0.5;
static const double k_two = 2.0;
static const act_range k_unit = {0.0, 1.0};

double act_clamp(double val, act_range range) {
    if (val < range.low) {
        return range.low;
    }
    return val > range.high ? range.high : val;
}

/* wgt[row] = probability that the inputs equal the assignment row (bit i of row = input i). */
static void weights(const act_fn_ref *fn) {
    double *wgt = fn->scratch;
    wgt[0] = 1.0;
    for (uint32_t idx = 0; idx < fn->n_in; idx++) {
        uint32_t bit = 1U << idx;
        double pp = act_clamp(fn->prob[fn->ins[idx]], k_unit);
        for (uint32_t row = 0; row < bit; row++) {
            wgt[row | bit] = wgt[row] * pp;
            wgt[row] *= 1.0 - pp;
        }
    }
}

/* Applies the pair joint of one input along its dimension of vec (mode product). */
typedef struct mode_dim {
    uint32_t n_in, idx;
} mode_dim;

static void mode_product(double *vec, mode_dim dim, act_stat stat) {
    double half = act_clamp(stat.dens, (act_range){0.0, k_two}) * k_half;
    double j01 = half;
    double j11 = fmax(stat.prob - half, 0.0);
    double j00 = fmax(1.0 - stat.prob - half, 0.0);
    uint32_t bit = 1U << dim.idx;
    for (uint32_t row = 0; row < (1U << dim.n_in); row++) {
        if ((row & bit) == 0) {
            double x0 = vec[row];
            double x1 = vec[row | bit];
            vec[row] = j00 * x0 + j01 * x1;
            vec[row | bit] = j01 * x0 + j11 * x1;
        }
    }
}

static double ones(const uint8_t *tab, const double *vec, size_t rows) {
    double sum = 0.0;
    for (size_t row = 0; row < rows; row++) {
        sum += tab[row] != 0 ? vec[row] : 0.0;
    }
    return sum;
}

double act_eval_prob(const act_fn_ref *fn) {
    weights(fn);
    return act_clamp(ones(fn->tab, fn->scratch, (size_t)1U << fn->n_in), k_unit);
}

act_stat act_eval_pair(const act_fn_ref *fn) {
    size_t rows = (size_t)1U << fn->n_in;
    double *vec = fn->scratch + rows;
    weights(fn);
    double p1 = act_clamp(ones(fn->tab, fn->scratch, rows), k_unit);
    for (size_t row = 0; row < rows; row++) {
        vec[row] = fn->tab[row];
    }
    for (uint32_t idx = 0; idx < fn->n_in; idx++) {
        act_stat in = {act_clamp(fn->prob[fn->ins[idx]], k_unit), fn->dens[fn->ins[idx]]};
        mode_product(vec, (mode_dim){fn->n_in, idx}, in);
    }
    double e11 = ones(fn->tab, vec, rows);
    return (act_stat){p1,
                      act_clamp(k_two * (p1 - e11), (act_range){0.0, k_two * fmin(p1, 1.0 - p1)})};
}
