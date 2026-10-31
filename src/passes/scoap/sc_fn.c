/*
 * sc_fn.c — the function tables of the scoap pass (spec section 3): from a truth table the prime
 * implicants of {f = 0} and {f = 1} (controllability) and, per input, the assignments of the
 * other inputs that make the output depend on it (observability). No recursion: cubes are
 * enumerated as base-3 numbers, a don't-care digit is derived from smaller numbers.
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

/* The prime implicants of {f = val}, appended to the model's prime pool. */
static odin3_status add_primes(sc_model *md, sc_cell *cell, const sc_table *tab, uint8_t val) {
    sc_scan scan = {tab, val, {0}};
    uint32_t total = pow3(tab->n);
    for (uint32_t idx = 0; idx < total; idx++) {
        scan.imp[idx] = cube_implicant(&scan, idx) ? 1 : 0;
    }
    cell->prime_off[val] = (uint32_t)md->prime_pool.len;
    cell->prime_n[val] = 0;
    for (uint32_t idx = 0; idx < total; idx++) {
        if (scan.imp[idx] == 0 || !cube_prime(&scan, idx)) {
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

static odin3_status push_u8(odin3_vec *vec, uint32_t value) {
    uint8_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (uint8_t)value;
    return ODIN3_OK;
}

/* One input's list: its offset and count in sens_ref, the minterms in sens_pool. */
static odin3_status add_sens_input(sc_model *md, const sc_table *tab, uint32_t input) {
    uint32_t count = 0;
    uint32_t rows = 1U << tab->n;
    for (uint32_t mint = 0; mint < rows; mint++) {
        count += sens_minterm(tab, input, mint) ? 1U : 0U;
    }
    odin3_status st = push_u32(&md->sens_ref, (uint32_t)md->sens_pool.len);
    st = st == ODIN3_OK ? push_u32(&md->sens_ref, count) : st;
    for (uint32_t mint = 0; st == ODIN3_OK && mint < rows; mint++) {
        st = sens_minterm(tab, input, mint) ? push_u8(&md->sens_pool, mint) : ODIN3_OK;
    }
    return st;
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
