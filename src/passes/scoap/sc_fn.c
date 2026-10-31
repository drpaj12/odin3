/*
 * sc_fn.c — the function tables of the scoap pass (spec section 3): from a truth table the prime
 * implicants of {f = 0} and {f = 1} (controllability) and, per input, the assignments of the
 * other inputs that make the output depend on it (observability). No recursion: cubes are
 * enumerated as base-3 numbers, a don't-care digit is derived from smaller numbers.
 */
#include "passes/scoap/sc_int.h"

enum { TERNARY = 3, DASH = 2, MAX_CUBES = 6561 /* 3^8 */ };

/* 3^k for k = 0 .. SC_MAX_EXACT. */
static uint32_t pow3(uint32_t k) {
    uint32_t out = 1;
    for (uint32_t i = 0; i < k; i++) {
        out *= TERNARY;
    }
    return out;
}

/* Digit k of the base-3 number idx. */
static uint32_t digit_of(uint32_t idx, uint32_t k) {
    return (idx / pow3(k)) % TERNARY;
}

/* True when the cube idx lies inside {f = v}. */
static bool cube_implicant(const sc_table *tab, const uint8_t *imp, uint32_t idx, uint8_t val) {
    uint32_t minterm = 0;
    for (uint32_t k = 0; k < tab->n; k++) {
        uint32_t dig = digit_of(idx, k);
        if (dig == DASH) {
            return imp[idx - DASH * pow3(k)] != 0 && imp[idx - pow3(k)] != 0;
        }
        minterm |= dig << k;
    }
    return tab->f[minterm] == val;
}

static bool cube_prime(const sc_table *tab, const uint8_t *imp, uint32_t idx) {
    for (uint32_t k = 0; k < tab->n; k++) {
        uint32_t dig = digit_of(idx, k);
        if (dig != DASH && imp[idx + (DASH - dig) * pow3(k)] != 0) {
            return false;
        }
    }
    return true;
}

static sc_cube cube_of(const sc_table *tab, uint32_t idx) {
    sc_cube cube = {0, 0};
    for (uint32_t k = 0; k < tab->n; k++) {
        uint32_t dig = digit_of(idx, k);
        if (dig != DASH) {
            cube.care = (uint8_t)(cube.care | (1U << k));
            cube.val = (uint8_t)(cube.val | (dig << k));
        }
    }
    return cube;
}

/* The prime implicants of {f = val}, appended to the model's prime pool. */
static odin3_status add_primes(sc_model *md, sc_cell *cell, const sc_table *tab, uint8_t val) {
    uint8_t imp[MAX_CUBES];
    uint32_t total = pow3(tab->n);
    for (uint32_t idx = 0; idx < total; idx++) {
        imp[idx] = cube_implicant(tab, imp, idx, val) ? 1 : 0;
    }
    cell->prime_off[val] = (uint32_t)md->prime_pool.len;
    cell->prime_n[val] = 0;
    for (uint32_t idx = 0; idx < total; idx++) {
        if (imp[idx] == 0 || !cube_prime(tab, imp, idx)) {
            continue;
        }
        sc_cube *slot = odin3_vec_push(&md->prime_pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = cube_of(tab, idx);
        cell->prime_n[val]++;
    }
    return ODIN3_OK;
}

static bool sens_minterm(const sc_table *tab, uint32_t input, uint32_t mint) {
    return (mint >> input & 1U) == 0 && tab->f[mint] != tab->f[mint | (1U << input)];
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* Per input the minterms (bit i clear) of the other inputs that make f depend on it. */
static odin3_status add_sens(sc_model *md, sc_cell *cell, const sc_table *tab) {
    cell->sens_off = (uint32_t)(md->sens_ref.len / 2);
    for (uint32_t i = 0; i < tab->n; i++) {
        uint32_t count = 0;
        for (uint32_t mint = 0; mint < (1U << tab->n); mint++) {
            count += sens_minterm(tab, i, mint) ? 1U : 0U;
        }
        odin3_status st = push_u32(&md->sens_ref, (uint32_t)md->sens_pool.len);
        st = st == ODIN3_OK ? push_u32(&md->sens_ref, count) : st;
        for (uint32_t mint = 0; st == ODIN3_OK && mint < (1U << tab->n); mint++) {
            uint8_t *slot = sens_minterm(tab, i, mint) ? odin3_vec_push(&md->sens_pool) : NULL;
            if (sens_minterm(tab, i, mint)) {
                st = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
            }
            if (slot != NULL) {
                *slot = (uint8_t)mint;
            }
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status sc_fn_compile(sc_model *md, sc_cell *cell, const sc_table *tab) {
    odin3_status st = add_primes(md, cell, tab, 0);
    st = st == ODIN3_OK ? add_primes(md, cell, tab, 1) : st;
    return st == ODIN3_OK ? add_sens(md, cell, tab) : st;
}
