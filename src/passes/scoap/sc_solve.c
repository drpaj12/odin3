/*
 * sc_solve.c — the scoap fixed point (spec section 3, "Loops"): the least solution of the min-plus
 * rules, reached by full sweeps in cell order from INF. Controllability sweeps run forward,
 * observability sweeps backward; the combinational measures (CC, CO) and the sequential ones (SC,
 * SO) are two domains of the same rules (the combinational "+1" per cell is the domain's bonus).
 */
#include "passes/scoap/sc_int.h"

typedef struct sc_dom {
    int32_t *const *val; /* [2]: cost of value 0 and 1 per net */
    int32_t *obs;        /* observability per net */
    int bonus;           /* 1 for CC/CO (per cell), 0 for SC/SO */
    int frame;           /* 0 for CC/CO, 1 for SC/SO: a flip-flop adds this */
} sc_dom;

static int bit_of(unsigned word, uint32_t pos) {
    return (int)((word >> pos) & 1U);
}

static int32_t clamp(int64_t value) {
    return value >= SCOAP_INF ? SCOAP_INF : (int32_t)value;
}

/* Cost of value v on a net; a pin without a net reads constant 0 (as the simulator does). */
static int32_t net_cost(const sc_dom *dom, uint32_t net, int val) {
    if (net == 0) {
        return val == 0 ? dom->bonus : SCOAP_INF;
    }
    return dom->val[val][net];
}

/* A candidate value for a net. */
typedef struct sc_cand {
    uint32_t net;
    int64_t value;
} sc_cand;

/* Lowers arr[net] to the candidate; true when it changed. Fixed (constant) nets never change. */
static bool relax(const sc_model *md, int32_t *arr, sc_cand cand) {
    int32_t val = clamp(cand.value);
    if (cand.net == 0 || md->fixed[cand.net] != 0 || val >= arr[cand.net]) {
        return false;
    }
    arr[cand.net] = val;
    return true;
}

static const uint32_t *cell_inputs(const sc_model *md, const sc_cell *cell) {
    return cell->n_in == 0 ? NULL : odin3_vec_cat(&md->in_pool, cell->in_off);
}

static int64_t cube_cost(const sc_dom *dom, const uint32_t *in, sc_cube cube) {
    int64_t sum = 0;
    for (uint32_t j = 0; j < SC_MAX_EXACT; j++) {
        if (bit_of(cube.care, j) != 0) {
            sum += net_cost(dom, in[j], bit_of(cube.val, j));
        }
    }
    return sum;
}

/* CCv(y) = bonus + min over the prime implicants of {f = v} of the cube cost. */
static bool fwd_func(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    const uint32_t *in = cell_inputs(md, cell);
    bool changed = false;
    for (int val = 0; val < 2; val++) {
        int64_t best = SCOAP_INF;
        const sc_cube *primes = odin3_vec_cat(&md->prime_pool, cell->prime_off[val]);
        for (uint32_t k = 0; k < cell->prime_n[val]; k++) {
            int64_t cost = cube_cost(dom, in, primes[k]);
            best = cost < best ? cost : best;
        }
        int64_t res = best >= SCOAP_INF ? SCOAP_INF : best + dom->bonus;
        changed = relax(md, dom->val[val], (sc_cand){cell->out, res}) || changed;
    }
    return changed;
}

/* The cheapest sensitising assignment of the other inputs for input number `input`. */
static int64_t sens_best(const sc_model *md, const sc_dom *dom, const sc_cell *cell,
                         uint32_t input) {
    const uint32_t *in = cell_inputs(md, cell);
    const uint32_t *ref = odin3_vec_cat(&md->sens_ref, 2 * (size_t)(cell->sens_off + input));
    const uint8_t *mints = ref[1] == 0 ? NULL : odin3_vec_cat(&md->sens_pool, ref[0]);
    int64_t best = SCOAP_INF;
    for (uint32_t k = 0; k < ref[1]; k++) {
        int64_t cost = 0;
        for (uint32_t j = 0; j < cell->n_in; j++) {
            cost += j == input ? 0 : net_cost(dom, in[j], bit_of(mints[k], j));
        }
        best = cost < best ? cost : best;
    }
    return best;
}

/* CO(x_i) = CO(y) + bonus + min over the sensitising assignments of the other inputs' costs. */
static bool bwd_func(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    const uint32_t *in = cell_inputs(md, cell);
    int64_t base = dom->obs[cell->out];
    bool changed = false;
    if (base >= SCOAP_INF) {
        return false;
    }
    for (uint32_t i = 0; i < cell->n_in; i++) {
        int64_t best = sens_best(md, dom, cell, i);
        int64_t res = best >= SCOAP_INF ? SCOAP_INF : base + dom->bonus + best;
        changed = relax(md, dom->obs, (sc_cand){in[i], res}) || changed;
    }
    return changed;
}

/* --- wide $sop and $lut: bounds -------------------------------------------------------------- */

/* Sum over the inputs except `skip` of the cheaper value (the bound of a wide function). */
static int64_t cheap_sum(const sc_model *md, const sc_dom *dom, const sc_cell *cell,
                         uint32_t skip) {
    const uint32_t *in = cell_inputs(md, cell);
    int64_t sum = 0;
    for (uint32_t j = 0; j < cell->n_in; j++) {
        int32_t c0 = net_cost(dom, in[j], 0);
        int32_t c1 = net_cost(dom, in[j], 1);
        if (j != skip) {
            sum += c0 < c1 ? c0 : c1;
        }
    }
    return sum;
}

/* The cheapest row of the cover that gives out_char: sum of its literals' costs. */
static int64_t row_min(const sc_model *md, const sc_dom *dom, const sc_cell *cell, char out_char) {
    const uint32_t *in = cell_inputs(md, cell);
    const uint8_t *rows = odin3_vec_cat(&md->cover_pool, cell->cover_off);
    uint32_t row_len = cell->n_in + 1;
    int64_t best = SCOAP_INF;
    for (uint32_t off = 0; off + row_len <= cell->cover_len; off += row_len) {
        int64_t cost = 0;
        for (uint32_t j = 0; rows[off + cell->n_in] == (uint8_t)out_char && j < cell->n_in; j++) {
            cost += rows[off + j] == '-' ? 0 : net_cost(dom, in[j], rows[off + j] == '1' ? 1 : 0);
        }
        best = rows[off + cell->n_in] == (uint8_t)out_char && cost < best ? cost : best;
    }
    return best;
}

static bool fwd_wide(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    bool changed = false;
    for (int val = 0; val < 2; val++) {
        int64_t best = cheap_sum(md, dom, cell, UINT32_MAX);
        if (cell->cover_val == val) {
            best = row_min(md, dom, cell, (char)('0' + val));
        } else if (cell->cover_val == -1) {
            best = val == 0 ? 0 : SCOAP_INF;
        }
        int64_t res = best >= SCOAP_INF ? SCOAP_INF : best + dom->bonus;
        changed = relax(md, dom->val[val], (sc_cand){cell->out, res}) || changed;
    }
    return changed;
}

static bool bwd_wide(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    const uint32_t *in = cell_inputs(md, cell);
    int64_t base = dom->obs[cell->out];
    bool changed = false;
    if (base >= SCOAP_INF || cell->cover_val == -1) {
        return false;
    }
    for (uint32_t i = 0; i < cell->n_in; i++) {
        int64_t res = base + dom->bonus + cheap_sum(md, dom, cell, i);
        changed = relax(md, dom->obs, (sc_cand){in[i], res}) || changed;
    }
    return changed;
}

/* --- flip-flops ------------------------------------------------------------------------------ */

/* Cost of a clock pulse: both levels; a flip-flop without a clock pin has none. */
static int64_t clock_cost(const sc_dom *dom, uint32_t clk) {
    return clk == 0 ? 0 : (int64_t)dom->val[0][clk] + dom->val[1][clk];
}

static bool fwd_ff(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    const uint32_t *in = cell_inputs(md, cell);
    int64_t clk = clock_cost(dom, in[1]);
    bool changed = false;
    for (int val = 0; val < 2; val++) {
        int64_t data = net_cost(dom, in[0], val);
        int64_t res = data >= SCOAP_INF ? SCOAP_INF : data + clk + dom->frame;
        changed = relax(md, dom->val[val], (sc_cand){cell->out, res}) || changed;
    }
    return changed;
}

static bool bwd_ff(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    const uint32_t *in = cell_inputs(md, cell);
    int64_t base = dom->obs[cell->out];
    int64_t clk = clock_cost(dom, in[1]);
    bool changed = false;
    if (base >= SCOAP_INF) {
        return false;
    }
    for (uint32_t i = 0; i < cell->n_in; i++) {
        if (i == 1) {
            continue;
        }
        changed = relax(md, dom->obs, (sc_cand){in[i], base + clk + dom->frame}) || changed;
    }
    int64_t via0 = (int64_t)net_cost(dom, in[0], 0) + dom->val[1][cell->out];
    int64_t via1 = (int64_t)net_cost(dom, in[0], 1) + dom->val[0][cell->out];
    int64_t flip = via0 < via1 ? via0 : via1;
    if (in[1] != 0 && flip < SCOAP_INF) {
        changed = relax(md, dom->obs, (sc_cand){in[1], base + clk + dom->frame + flip}) || changed;
    }
    return changed;
}

/* --- sweeps ---------------------------------------------------------------------------------- */

static bool cell_step(const sc_model *md, const sc_dom *dom, const sc_cell *cell, bool forward) {
    if (cell->out == 0) {
        return false;
    }
    switch ((sc_kind)cell->kind) {
    case SC_FUNC:
        return forward ? fwd_func(md, dom, cell) : bwd_func(md, dom, cell);
    case SC_WIDE:
        return forward ? fwd_wide(md, dom, cell) : bwd_wide(md, dom, cell);
    case SC_FF:
        return forward ? fwd_ff(md, dom, cell) : bwd_ff(md, dom, cell);
    default:
        return false;
    }
}

static bool sweep(const sc_model *md, const sc_dom *dom, bool forward) {
    bool changed = false;
    for (size_t k = 0; k < md->n_cells; k++) {
        const sc_cell *cell = &md->cells[forward ? k : md->n_cells - 1 - k];
        changed = cell_step(md, dom, cell, forward) || changed;
    }
    return changed;
}

static void run_phase(sc_model *md, const sc_dom *dom, bool forward) {
    uint32_t cap = md->n_nets + 2;
    bool changed = true;
    uint32_t done = 0;
    while (changed && done < cap) {
        changed = sweep(md, dom, forward);
        done++;
    }
    md->stats.sweeps += done;
    md->stats.converged = md->stats.converged && !changed;
}

static uint64_t count_unreachable(const sc_model *md) {
    uint64_t count = 0;
    for (uint32_t id = 1; id < md->n_nets; id++) {
        bool bad =
            md->cc[0][id] >= SCOAP_INF || md->cc[1][id] >= SCOAP_INF || md->co[id] >= SCOAP_INF;
        bad = bad || (md->seq && (md->sc[0][id] >= SCOAP_INF || md->sc[1][id] >= SCOAP_INF ||
                                  md->so[id] >= SCOAP_INF));
        count += md->live[id] != 0 && bad ? 1U : 0U;
    }
    return count;
}

void sc_solve(sc_model *md) {
    const sc_dom comb = {md->cc, md->co, 1, 0};
    const sc_dom seqd = {md->sc, md->so, 0, 1};
    md->stats.converged = true;
    run_phase(md, &comb, true);
    run_phase(md, &comb, false);
    if (md->seq) {
        run_phase(md, &seqd, true);
        run_phase(md, &seqd, false);
    }
    md->stats.unreachable = count_unreachable(md);
}
