/*
 * sc_solve.c — the scoap fixed point (spec section 3, "Loops"): the least solution of the min-plus
 * rules, reached from INF by a worklist: a cell is evaluated again only when a net it reads
 * (forward phases) or the output net it observes through (backward phases) changed. Rounds of the
 * worklist play the role of sweeps. Controllability phases run forward, observability phases
 * backward; the combinational measures (CC, CO) and the sequential ones (SC, SO) are two domains of
 * the same rules (the combinational "+1" per cell is the domain's bonus).
 */
#include "passes/scoap/sc_int.h"
#include "util/alloc.h"

#include <string.h>

enum { SC_VALUES_COMB = 3, SC_VALUES_SEQ = 6 };

typedef struct sc_dom {
    int64_t *const *val; /* [2]: cost of value 0 and 1 per net */
    int64_t *obs;        /* observability per net */
    int bonus;           /* 1 for CC/CO (per cell), 0 for SC/SO */
    int frame;           /* 0 for CC/CO, 1 for SC/SO: a flip-flop adds this */
} sc_dom;

static int bit_of(unsigned word, uint32_t pos) {
    return (int)((word >> pos) & 1U);
}

/* a + b: unreachable if either is; finite sums saturate at SCOAP_SAT and stay finite. */
static int64_t sat_add(int64_t lhs, int64_t rhs) {
    if (lhs >= SCOAP_INF || rhs >= SCOAP_INF) {
        return SCOAP_INF;
    }
    int64_t sum = lhs + rhs;
    return sum > SCOAP_SAT ? SCOAP_SAT : sum;
}

/* Cost of value v on a net; a pin without a net reads constant 0 (as the simulator does). */
static int64_t net_cost(const sc_dom *dom, uint32_t net, int val) {
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
static bool relax(const sc_model *md, int64_t *arr, sc_cand cand) {
    if (cand.net == 0 || md->fixed[cand.net] != 0 || cand.value >= arr[cand.net]) {
        return false;
    }
    arr[cand.net] = cand.value;
    return true;
}

static const uint32_t *cell_inputs(const sc_model *md, const sc_cell *cell) {
    return cell->n_in == 0 ? NULL : odin3_vec_cat(&md->in_pool, cell->in_off);
}

static int64_t cube_cost(const sc_dom *dom, const uint32_t *in, sc_cube cube) {
    int64_t sum = 0;
    for (uint32_t j = 0; j < SC_MAX_EXACT; j++) {
        if (bit_of(cube.care, j) != 0) {
            sum = sat_add(sum, net_cost(dom, in[j], bit_of(cube.val, j)));
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
        /* No prime implicant: the value cannot be produced (an empty cover). */
        const sc_cube *primes =
            cell->prime_n[val] == 0 ? NULL : odin3_vec_cat(&md->prime_pool, cell->prime_off[val]);
        for (uint32_t k = 0; k < cell->prime_n[val]; k++) {
            int64_t cost = cube_cost(dom, in, primes[k]);
            best = cost < best ? cost : best;
        }
        int64_t res = sat_add(best, dom->bonus);
        changed = relax(md, dom->val[val], (sc_cand){cell->out, res}) || changed;
    }
    return changed;
}

/* The cheapest sensitising cube of the other inputs for input number `input` (don't cares cost
 * nothing). */
static int64_t sens_best(const sc_model *md, const sc_dom *dom, const sc_cell *cell,
                         uint32_t input) {
    const uint32_t *in = cell_inputs(md, cell);
    const uint32_t *ref = odin3_vec_cat(&md->sens_ref, 2 * (size_t)(cell->sens_off + input));
    const sc_cube *cubes = ref[1] == 0 ? NULL : odin3_vec_cat(&md->sens_pool, ref[0]);
    int64_t best = SCOAP_INF;
    for (uint32_t k = 0; k < ref[1]; k++) {
        int64_t cost = cube_cost(dom, in, cubes[k]);
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
        int64_t res = sat_add(sat_add(base, dom->bonus), best);
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
        int64_t c0 = net_cost(dom, in[j], 0);
        int64_t c1 = net_cost(dom, in[j], 1);
        if (j != skip) {
            sum = sat_add(sum, c0 < c1 ? c0 : c1);
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
            cost = rows[off + j] == '-'
                       ? cost
                       : sat_add(cost, net_cost(dom, in[j], rows[off + j] == '1' ? 1 : 0));
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
        int64_t res = sat_add(best, dom->bonus);
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
        int64_t res = sat_add(sat_add(base, dom->bonus), cheap_sum(md, dom, cell, i));
        changed = relax(md, dom->obs, (sc_cand){in[i], res}) || changed;
    }
    return changed;
}

/* --- flip-flops ------------------------------------------------------------------------------ */

/* Cost of a clock pulse: both levels; a flip-flop without a clock pin has none. */
static int64_t clock_cost(const sc_dom *dom, uint32_t clk) {
    return clk == 0 ? 0 : sat_add(dom->val[0][clk], dom->val[1][clk]);
}

static bool fwd_ff(const sc_model *md, const sc_dom *dom, const sc_cell *cell) {
    const uint32_t *in = cell_inputs(md, cell);
    int64_t clk = clock_cost(dom, in[1]);
    bool changed = false;
    for (int val = 0; val < 2; val++) {
        int64_t data = net_cost(dom, in[0], val);
        int64_t res = sat_add(sat_add(data, clk), dom->frame);
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
        int64_t res = sat_add(sat_add(base, clk), dom->frame);
        changed = relax(md, dom->obs, (sc_cand){in[i], res}) || changed;
    }
    int64_t via0 = sat_add(net_cost(dom, in[0], 0), dom->val[1][cell->out]);
    int64_t via1 = sat_add(net_cost(dom, in[0], 1), dom->val[0][cell->out]);
    int64_t flip = via0 < via1 ? via0 : via1;
    if (in[1] != 0 && flip < SCOAP_INF) {
        int64_t res = sat_add(sat_add(sat_add(base, clk), dom->frame), flip);
        changed = relax(md, dom->obs, (sc_cand){in[1], res}) || changed;
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

/* --- the worklist ---------------------------------------------------------------------------- */

/* A compressed table: the cells of net n are list[off[n] .. off[n + 1] - 1]. */
typedef struct sc_rows {
    uint32_t *off;
    uint32_t *list;
} sc_rows;

/* Net -> cells reading it, net -> cells driving it, and the two queues. */
typedef struct sc_work {
    sc_rows rdr, drv;
    uint32_t *cur, *next;
    uint8_t *queued;
    uint32_t n_cur, n_next;
} sc_work;

static void work_free(sc_work *wk) {
    odin3_util_free(wk->rdr.off);
    odin3_util_free(wk->rdr.list);
    odin3_util_free(wk->drv.off);
    odin3_util_free(wk->drv.list);
    odin3_util_free(wk->cur);
    odin3_util_free(wk->next);
    odin3_util_free(wk->queued);
}

static odin3_status work_alloc(const sc_model *md, sc_work *wk) {
    size_t nets = (size_t)md->n_nets + 2;
    size_t cells = md->n_cells + 1;
    wk->rdr.off = odin3_util_calloc(nets * sizeof(uint32_t));
    wk->drv.off = odin3_util_calloc(nets * sizeof(uint32_t));
    wk->rdr.list = odin3_util_calloc((md->in_pool.len + 1) * sizeof(uint32_t));
    wk->drv.list = odin3_util_calloc(cells * sizeof(uint32_t));
    wk->cur = odin3_util_calloc(cells * sizeof(uint32_t));
    wk->next = odin3_util_calloc(cells * sizeof(uint32_t));
    wk->queued = odin3_util_calloc(cells);
    bool ok = wk->rdr.off != NULL && wk->drv.off != NULL && wk->rdr.list != NULL &&
              wk->drv.list != NULL && wk->cur != NULL && wk->next != NULL && wk->queued != NULL;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* Counts (fill false: off[net + 1]) or stores (fill true: list[off[net]++]) one pair. */
static void touch(sc_rows *rows, uint32_t net, uint32_t cell, bool fill) {
    if (fill) {
        rows->list[rows->off[net]++] = cell;
    } else {
        rows->off[net + 1]++;
    }
}

/* The (net, cell) pairs: net read by the cell (readers) or net driven by it. */
static void pairs(const sc_model *md, sc_rows *rows, bool readers, bool fill) {
    for (size_t k = 0; k < md->n_cells; k++) {
        const sc_cell *cell = &md->cells[k];
        const uint32_t *in = cell_inputs(md, cell);
        if (cell->out == 0) {
            continue;
        }
        if (!readers) {
            touch(rows, cell->out, (uint32_t)k, fill);
        }
        for (uint32_t i = 0; readers && i < cell->n_in; i++) {
            if (in[i] != 0) {
                touch(rows, in[i], (uint32_t)k, fill);
            }
        }
    }
}

/* Builds one table in two passes; only cells with an output take part in the iteration. */
static void build_rows(const sc_model *md, sc_rows *rows, bool readers) {
    pairs(md, rows, readers, false);
    for (uint32_t net = 0; net < md->n_nets; net++) {
        rows->off[net + 1] += rows->off[net];
    }
    pairs(md, rows, readers, true);
    /* the fill advanced each row start to the next row's start: shift back */
    for (uint32_t net = md->n_nets; net > 0; net--) {
        rows->off[net] = rows->off[net - 1];
    }
    rows->off[0] = 0;
}

static void enqueue(sc_work *wk, uint32_t cell) {
    if (wk->queued[cell] == 0) {
        wk->queued[cell] = 1;
        wk->next[wk->n_next++] = cell;
    }
}

static void enqueue_row(sc_work *wk, const sc_rows *rows, uint32_t net) {
    for (uint32_t k = rows->off[net]; k < rows->off[net + 1]; k++) {
        enqueue(wk, rows->list[k]);
    }
}

/* After cell changed something: forward, the readers of its output; backward, the drivers of the
 * nets it observes through. */
static void wake(const sc_model *md, sc_work *wk, const sc_cell *cell, bool forward) {
    if (forward) {
        enqueue_row(wk, &wk->rdr, cell->out);
        return;
    }
    const uint32_t *in = cell_inputs(md, cell);
    for (uint32_t i = 0; i < cell->n_in; i++) {
        if (in[i] != 0) {
            enqueue_row(wk, &wk->drv, in[i]);
        }
    }
}

/* One round: evaluates the cells of cur, collects the cells to look at next in next. */
static void round_of(const sc_model *md, const sc_dom *dom, sc_work *wk, bool forward) {
    wk->n_next = 0;
    for (uint32_t k = 0; k < wk->n_cur; k++) {
        uint32_t idx = wk->cur[k];
        wk->queued[idx] = 0;
        if (cell_step(md, dom, &md->cells[idx], forward)) {
            wake(md, wk, &md->cells[idx], forward);
        }
    }
}

/* Initial queue: every cell with an output, in cell order (forward) or reverse. */
static void seed(const sc_model *md, sc_work *wk, bool forward) {
    wk->n_next = 0;
    for (size_t k = 0; k < md->n_cells; k++) {
        size_t idx = forward ? k : md->n_cells - 1 - k;
        if (md->cells[idx].out != 0) {
            enqueue(wk, (uint32_t)idx);
        }
    }
}

/* States are (net, value), so a value settles after at most 2 * nets rounds: the cap. */
static void run_phase(sc_model *md, const sc_dom *dom, bool forward, sc_work *wk) {
    uint64_t cap = 2 * (uint64_t)md->n_nets + 2;
    uint64_t rounds = 0;
    seed(md, wk, forward);
    while (wk->n_next > 0 && rounds < cap) {
        uint32_t *tmp = wk->cur;
        wk->cur = wk->next;
        wk->next = tmp;
        wk->n_cur = wk->n_next;
        md->stats.evals += wk->n_cur;
        round_of(md, dom, wk, forward);
        rounds++;
    }
    md->stats.sweeps += (uint32_t)rounds;
    md->stats.converged = md->stats.converged && wk->n_next == 0;
    for (uint32_t k = 0; k < wk->n_next; k++) {
        wk->queued[wk->next[k]] = 0;
    }
    wk->n_next = 0;
}

static void count_flags(sc_model *md) {
    uint64_t unreach = 0;
    uint64_t satur = 0;
    for (uint32_t id = 1; id < md->n_nets; id++) {
        const int64_t *vals[] = {&md->cc[0][id], &md->cc[1][id], &md->co[id],
                                 &md->sc[0][id], &md->sc[1][id], &md->so[id]};
        size_t used = md->seq ? SC_VALUES_SEQ : SC_VALUES_COMB;
        bool bad = false;
        bool sat = false;
        for (size_t k = 0; k < used; k++) {
            bad = bad || *vals[k] >= SCOAP_INF;
            sat = sat || *vals[k] == SCOAP_SAT;
        }
        unreach += md->live[id] != 0 && bad ? 1U : 0U;
        satur += md->live[id] != 0 && sat ? 1U : 0U;
    }
    md->stats.unreachable = unreach;
    md->stats.saturated = satur;
}

odin3_status sc_solve(sc_model *md) {
    const sc_dom comb = {md->cc, md->co, 1, 0};
    const sc_dom seqd = {md->sc, md->so, 0, 1};
    sc_work wk;
    memset(&wk, 0, sizeof wk);
    md->stats.converged = true;
    odin3_status st = work_alloc(md, &wk);
    if (st == ODIN3_OK) {
        build_rows(md, &wk.rdr, true);
        build_rows(md, &wk.drv, false);
        run_phase(md, &comb, true, &wk);
        run_phase(md, &comb, false, &wk);
        if (md->seq) {
            run_phase(md, &seqd, true, &wk);
            run_phase(md, &seqd, false, &wk);
        }
        count_flags(md);
    }
    work_free(&wk);
    return st;
}
