/*
 * sc_fn.c — the function tables of the scoap pass (spec section 3): from a truth table the prime
 * implicants of {f = 0} and {f = 1} (controllability) and, per input, the prime implicants of
 * "the output follows this input with a known polarity" over the other inputs (observability). No
 * recursion: cubes are enumerated as base-3 numbers, a don't-care digit is derived from smaller
 * numbers.
 */
#include "passes/scoap/sc_int.h"

enum { TERNARY = 3, DASH = 2, MAX_CUBES = 6561 /* 3^8 */ };

/* The table, the value looked for and the implicant flags found so far. */
typedef struct sc_scan {
    const sc_table *tab;
    uint8_t val;
    uint8_t imp[MAX_CUBES];
} sc_scan;

/* 3^pos for pos = 0 .. SC_MAX_EXACT. */
static uint32_t pow3(uint32_t pos) {
    uint32_t out = 1;
    for (uint32_t step = 0; step < pos; step++) {
        out *= TERNARY;
    }
    return out;
}

/* Digit pos of the base-3 number idx. */
static uint32_t digit_of(uint32_t idx, uint32_t pos) {
    return (idx / pow3(pos)) % TERNARY;
}

/* True when the cube idx lies inside {f = val}. */
static bool cube_implicant(const sc_scan *scan, uint32_t idx) {
    uint32_t minterm = 0;
    for (uint32_t pos = 0; pos < scan->tab->n; pos++) {
        uint32_t dig = digit_of(idx, pos);
        if (dig == DASH) {
            return scan->imp[idx - DASH * pow3(pos)] != 0 && scan->imp[idx - pow3(pos)] != 0;
        }
        minterm |= dig << pos;
    }
    return scan->tab->f[minterm] == scan->val;
}

static bool cube_prime(const sc_scan *scan, uint32_t idx) {
    for (uint32_t pos = 0; pos < scan->tab->n; pos++) {
        uint32_t dig = digit_of(idx, pos);
        if (dig != DASH && scan->imp[idx + (DASH - dig) * pow3(pos)] != 0) {
            return false;
        }
    }
    return true;
}

static sc_cube cube_of(const sc_table *tab, uint32_t idx) {
    sc_cube cube = {0, 0};
    for (uint32_t pos = 0; pos < tab->n; pos++) {
        uint32_t dig = digit_of(idx, pos);
        if (dig != DASH) {
            cube.care = (uint8_t)(cube.care | (1U << pos));
            cube.val = (uint8_t)(cube.val | (dig << pos));
        }
    }
    return cube;
}

/* Moves a cube over the inputs without `skip` back to the full input numbering. */
static sc_cube lift_cube(sc_cube cube, uint32_t skip) {
    uint32_t low = (1U << skip) - 1U;
    cube.care = (uint8_t)((cube.care & low) | ((cube.care & ~low) << 1));
    cube.val = (uint8_t)((cube.val & low) | ((cube.val & ~low) << 1));
    return cube;
}

/* What to emit: the prime implicants of {f = val}; skip >= 0: tab leaves input `skip` out and the
 * cubes are lifted to the numbering with it. */
typedef struct sc_emit {
    const sc_table *tab;
    uint8_t val;
    int skip;
} sc_emit;

static odin3_status emit_primes(odin3_vec *pool, const sc_emit *what, uint32_t *count) {
    const sc_table *tab = what->tab;
    int skip = what->skip;
    sc_scan scan = {tab, what->val, {0}};
    uint32_t total = pow3(tab->n);
    for (uint32_t idx = 0; idx < total; idx++) {
        scan.imp[idx] = cube_implicant(&scan, idx) ? 1 : 0;
    }
    for (uint32_t idx = 0; idx < total; idx++) {
        if (scan.imp[idx] == 0 || !cube_prime(&scan, idx)) {
            continue;
        }
        sc_cube *slot = odin3_vec_push(pool);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = cube_of(tab, idx);
        *slot = skip >= 0 ? lift_cube(*slot, (uint32_t)skip) : *slot;
        (*count)++;
    }
    return ODIN3_OK;
}

static odin3_status add_primes(sc_model *md, sc_cell *cell, const sc_table *tab, uint8_t val) {
    cell->prime_off[val] = (uint32_t)md->prime_pool.len;
    cell->prime_n[val] = 0;
    sc_emit what = {tab, val, -1};
    return emit_primes(&md->prime_pool, &what, &cell->prime_n[val]);
}

/* h(r) = 1 when, for the other inputs r, f(x=0, r) = pol and f(x=1, r) = 1 - pol: the output
 * follows input x with polarity pol whatever the don't-care inputs of a cube over r are. */
typedef struct sc_sel {
    uint32_t input;
    uint8_t pol;
} sc_sel;

static sc_table polarity_table(const sc_table *tab, sc_sel sel) {
    sc_table sub = {.n = tab->n - 1};
    uint32_t input = sel.input;
    uint8_t pol = sel.pol;
    uint32_t low_mask = (1U << input) - 1U;
    for (uint32_t row = 0; row < (1U << sub.n); row++) {
        uint32_t m0 = (row & low_mask) | ((row & ~low_mask) << 1);
        uint32_t m1 = m0 | (1U << input);
        sub.f[row] = (tab->f[m0] == pol && tab->f[m1] == 1 - pol) ? 1 : 0;
    }
    return sub;
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* One input's list: its offset and count in sens_ref, the cubes (both polarities) in sens_pool. */
static odin3_status add_sens_input(sc_model *md, const sc_table *tab, uint32_t input) {
    uint32_t count = 0;
    uint32_t first = (uint32_t)md->sens_pool.len;
    odin3_status st = ODIN3_OK;
    for (uint8_t pol = 0; st == ODIN3_OK && pol < 2; pol++) {
        sc_table sub = polarity_table(tab, (sc_sel){input, pol});
        sc_emit what = {&sub, 1, (int)input};
        st = emit_primes(&md->sens_pool, &what, &count);
    }
    st = st == ODIN3_OK ? push_u32(&md->sens_ref, first) : st;
    return st == ODIN3_OK ? push_u32(&md->sens_ref, count) : st;
}

static odin3_status add_sens(sc_model *md, sc_cell *cell, const sc_table *tab) {
    cell->sens_off = (uint32_t)(md->sens_ref.len / 2);
    odin3_status st = ODIN3_OK;
    for (uint32_t input = 0; st == ODIN3_OK && input < tab->n; input++) {
        st = add_sens_input(md, tab, input);
    }
    return st;
}

odin3_status sc_fn_compile(sc_model *md, sc_cell *cell, const sc_table *tab) {
    odin3_status st = add_primes(md, cell, tab, 0);
    st = st == ODIN3_OK ? add_primes(md, cell, tab, 1) : st;
    return st == ODIN3_OK ? add_sens(md, cell, tab) : st;
}
