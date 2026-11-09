/*
 * act_solve.c — propagation over the whole model (spec section 3): the feedback components of the
 * cell graph (iterative Tarjan), the fixpoint of the probabilities inside each of them and the
 * one-frame toggle rule of its storage cells.
 */
#include "passes/activity/act_int.h"
#include "util/alloc.h"
#include "util/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ACT_NONE UINT32_MAX

enum { CONE_CAP = 2048, ACCEL_EVERY = 10 };
static const int k_unset_byte = 0xFF;
static const double k_half = 0.5;
static const double k_two = 2.0;
static const double k_accel_max_ratio = 0.9999;
static const double k_big_change = 2.0;
static const act_range k_unit = {0.0, 1.0};

/* The reference to evaluate cell of md; the density array only when wanted. */
static act_fn_ref ref_of(const act_model *md, const act_cell *cell, double *scratch, bool dens) {
    return (act_fn_ref){md->prob,
                        dens ? md->dens : NULL,
                        odin3_vec_cat(&md->in_pool, cell->in_off),
                        odin3_vec_cat(&md->tab_pool, cell->tab_off),
                        scratch,
                        cell->n_in};
}

/* --- the cell graph --------------------------------------------------------------------------- */

typedef struct graph {
    const act_model *md;
    uint32_t *rd_start; /* per net: readers in rd_items[rd_start[n] .. rd_start[n + 1]) */
    uint32_t *rd_items;
} graph;

static odin3_status build_readers(graph *gr) {
    const act_model *md = gr->md;
    size_t nets = (size_t)md->n_nets + 2U;
    gr->rd_start = odin3_util_calloc(nets * sizeof *gr->rd_start);
    if (gr->rd_start == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t ci = 0; ci < md->n_cells; ci++) {
        const uint32_t *ins = odin3_vec_cat(&md->in_pool, md->cells[ci].in_off);
        for (uint32_t kk = 0; kk < md->cells[ci].n_in; kk++) {
            gr->rd_start[ins[kk] + 1U]++;
        }
    }
    for (uint32_t ne = 0; ne <= md->n_nets; ne++) {
        gr->rd_start[ne + 1U] += gr->rd_start[ne];
    }
    gr->rd_items = odin3_util_malloc((gr->rd_start[md->n_nets + 1U] + 1U) * sizeof *gr->rd_items);
    uint32_t *fill = odin3_util_malloc(nets * sizeof *fill);
    if (gr->rd_items == NULL || fill == NULL) {
        odin3_util_free(fill);
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(fill, gr->rd_start, nets * sizeof *fill);
    for (size_t ci = 0; ci < md->n_cells; ci++) {
        const uint32_t *ins = odin3_vec_cat(&md->in_pool, md->cells[ci].in_off);
        for (uint32_t kk = 0; kk < md->cells[ci].n_in; kk++) {
            gr->rd_items[fill[ins[kk]]++] = (uint32_t)ci;
        }
    }
    odin3_util_free(fill);
    return ODIN3_OK;
}

/* Tarjan's state; every array has one entry per cell. */
typedef struct tarjan {
    uint32_t *index, *low, *comp, *stack, *call, *edge;
    uint8_t *onstack;
    uint32_t next_index, sp, cp, n_comps;
    uint32_t *order;      /* cells in emission order */
    uint32_t *comp_start; /* per component: first position in order; n_comps + 1 entries */
    uint32_t emitted;
} tarjan;

static void emit_component(tarjan *tj, uint32_t root) {
    tj->comp_start[tj->n_comps] = tj->emitted;
    uint32_t top;
    do {
        top = tj->stack[--tj->sp];
        tj->onstack[top] = 0;
        tj->comp[top] = tj->n_comps;
        tj->order[tj->emitted++] = top;
    } while (top != root);
    tj->n_comps++;
    tj->comp_start[tj->n_comps] = tj->emitted;
}

/* One step of the explicit-stack search: the next cell to descend into, or ACT_NONE. */
static uint32_t descend(const graph *gr, tarjan *tj, uint32_t cell) {
    uint32_t net = gr->md->cells[cell].out;
    while (gr->rd_start[net] + tj->edge[cell] < gr->rd_start[net + 1U]) {
        uint32_t succ = gr->rd_items[gr->rd_start[net] + tj->edge[cell]++];
        if (tj->index[succ] == ACT_NONE) {
            return succ;
        }
        if (tj->onstack[succ] != 0 && tj->index[succ] < tj->low[cell]) {
            tj->low[cell] = tj->index[succ];
        }
    }
    return ACT_NONE;
}

static void visit(tarjan *tj, uint32_t cell) {
    tj->index[cell] = tj->next_index;
    tj->low[cell] = tj->next_index++;
    tj->stack[tj->sp++] = cell;
    tj->onstack[cell] = 1;
    tj->call[tj->cp++] = cell;
}

static void search_from(const graph *gr, tarjan *tj, uint32_t start) {
    visit(tj, start);
    while (tj->cp > 0) {
        uint32_t cell = tj->call[tj->cp - 1U];
        uint32_t child = descend(gr, tj, cell);
        if (child != ACT_NONE) {
            visit(tj, child);
            continue;
        }
        tj->cp--;
        if (tj->low[cell] == tj->index[cell]) {
            emit_component(tj, cell);
        }
        if (tj->cp > 0) {
            uint32_t parent = tj->call[tj->cp - 1U];
            tj->low[parent] = tj->low[cell] < tj->low[parent] ? tj->low[cell] : tj->low[parent];
        }
    }
}

static odin3_status tarjan_run(const graph *gr, tarjan *tj) {
    size_t cnt = gr->md->n_cells;
    size_t bytes = (cnt + 1U) * sizeof(uint32_t);
    uint32_t **arrs[] = {&tj->index, &tj->low,  &tj->comp,  &tj->stack,
                         &tj->call,  &tj->edge, &tj->order, &tj->comp_start};
    for (size_t kk = 0; kk < sizeof arrs / sizeof arrs[0]; kk++) {
        *arrs[kk] = odin3_util_calloc(bytes);
        if (*arrs[kk] == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    tj->onstack = odin3_util_calloc(cnt + 1U);
    if (tj->onstack == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(tj->index, k_unset_byte, bytes);
    for (uint32_t ci = 0; ci < cnt; ci++) {
        if (tj->index[ci] == ACT_NONE) {
            search_from(gr, tj, ci);
        }
    }
    return ODIN3_OK;
}

static void tarjan_free(tarjan *tj) {
    odin3_util_free(tj->index);
    odin3_util_free(tj->low);
    odin3_util_free(tj->comp);
    odin3_util_free(tj->stack);
    odin3_util_free(tj->call);
    odin3_util_free(tj->edge);
    odin3_util_free(tj->order);
    odin3_util_free(tj->comp_start);
    odin3_util_free(tj->onstack);
}

/* --- ranks ------------------------------------------------------------------------------------ */

typedef struct rank_cell {
    uint32_t rank, cell;
} rank_cell;

static int rank_cmp(const void *pa, const void *pb) {
    const rank_cell *ra = pa;
    const rank_cell *rb = pb;
    if (ra->rank == rb->rank) {
        return 0;
    }
    return ra->rank < rb->rank ? -1 : 1;
}

typedef struct solver {
    act_model *md;
    const act_opts *opts;
    const graph *gr;
    double *scratch;
    uint32_t *rank;  /* per cell: topological rank among combinational cells, ACT_NONE otherwise */
    uint32_t *mark;  /* per cell: epoch of the last cone it joined */
    uint32_t *queue; /* nets */
    rank_cell *cone;
    double *saved;
    uint32_t epoch;
} solver;

/* Per combinational cell, the number of input pins driven by combinational cells. */
static void count_comb_fanin(const act_model *md, const uint32_t *drv, uint32_t *indeg) {
    for (size_t ci = 0; ci < md->n_cells; ci++) {
        const act_cell *cell = &md->cells[ci];
        const uint32_t *ins = odin3_vec_cat(&md->in_pool, cell->in_off);
        for (uint32_t kk = 0; cell->is_ff == 0 && kk < cell->n_in; kk++) {
            uint32_t src = drv[ins[kk]];
            indeg[ci] += (src != ACT_NONE && md->cells[src].is_ff == 0) ? 1U : 0U;
        }
    }
}

/* Kahn over the combinational cells; the net queue of the solver serves as work list. */
static void kahn(solver *sv, uint32_t *indeg) {
    const act_model *md = sv->md;
    uint32_t *work = sv->queue;
    uint32_t tail = 0;
    for (size_t ci = 0; ci < md->n_cells; ci++) {
        if (md->cells[ci].is_ff == 0 && indeg[ci] == 0) {
            work[tail++] = (uint32_t)ci;
        }
    }
    for (uint32_t head = 0; head < tail; head++) {
        uint32_t net = md->cells[work[head]].out;
        sv->rank[work[head]] = head;
        for (uint32_t ei = sv->gr->rd_start[net]; ei < sv->gr->rd_start[net + 1U]; ei++) {
            uint32_t reader = sv->gr->rd_items[ei];
            if (md->cells[reader].is_ff == 0 && --indeg[reader] == 0) {
                work[tail++] = reader;
            }
        }
    }
}

/* Ranks the combinational cells by Kahn's algorithm, storage outputs being sources. */
static odin3_status rank_cells(solver *sv) {
    const act_model *md = sv->md;
    size_t cnt = md->n_cells;
    size_t nets = (size_t)md->n_nets + 1U;
    uint32_t *drv = odin3_util_malloc(nets * sizeof *drv);
    uint32_t *indeg = odin3_util_calloc((cnt + 1U) * sizeof *indeg);
    odin3_status st = (drv != NULL && indeg != NULL) ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (st == ODIN3_OK) {
        memset(drv, k_unset_byte, nets * sizeof *drv);
        for (size_t ci = 0; ci < cnt; ci++) {
            drv[md->cells[ci].out] = (uint32_t)ci;
            sv->rank[ci] = ACT_NONE;
        }
        count_comb_fanin(md, drv, indeg);
        kahn(sv, indeg);
    }
    odin3_util_free(drv);
    odin3_util_free(indeg);
    return st;
}

/* --- cones and the toggle rule ---------------------------------------------------------------- */

/* The combinational cells that read ff's state, transitively, up to the next storage cells. */
static bool collect_cone(solver *sv, const act_cell *ff, uint32_t *count) {
    const act_model *md = sv->md;
    uint32_t head = 0;
    uint32_t tail = 0;
    *count = 0;
    sv->epoch++;
    sv->queue[tail++] = ff->out;
    while (head < tail) {
        uint32_t net = sv->queue[head++];
        for (uint32_t ei = sv->gr->rd_start[net]; ei < sv->gr->rd_start[net + 1U]; ei++) {
            uint32_t reader = sv->gr->rd_items[ei];
            bool skip = sv->mark[reader] == sv->epoch || md->cells[reader].is_ff != 0 ||
                        sv->rank[reader] == ACT_NONE;
            if (skip) {
                continue;
            }
            if (*count == CONE_CAP) {
                return false;
            }
            sv->mark[reader] = sv->epoch;
            sv->cone[(*count)++] = (rank_cell){sv->rank[reader], reader};
            sv->queue[tail++] = md->cells[reader].out;
        }
    }
    qsort(sv->cone, *count, sizeof *sv->cone, rank_cmp);
    return true;
}

typedef struct cone_cond {
    uint32_t count; /* cells of the cone */
    double value;   /* the state is held at this value */
} cone_cond;

/* P(next state = 1) with the state held at value, the cone recomputed under that condition. */
static double conditional_next(solver *sv, const act_cell *ff, cone_cond cond) {
    act_model *md = sv->md;
    double saved_q = md->prob[ff->out];
    md->prob[ff->out] = cond.value;
    for (uint32_t kk = 0; kk < cond.count; kk++) {
        const act_cell *cell = &md->cells[sv->cone[kk].cell];
        sv->saved[kk] = md->prob[cell->out];
        act_fn_ref ref = ref_of(md, cell, sv->scratch, false);
        md->prob[cell->out] = act_eval_prob(&ref);
    }
    act_fn_ref self = ref_of(md, ff, sv->scratch, false);
    double next = act_eval_prob(&self);
    for (uint32_t kk = cond.count; kk > 0; kk--) {
        md->prob[md->cells[sv->cone[kk - 1U].cell].out] = sv->saved[kk - 1U];
    }
    md->prob[ff->out] = saved_q;
    return next;
}

/* Toggle probability P(next != state) of a storage cell in a loop, spec section 3.3. */
static double ff_toggle(solver *sv, const act_cell *ff) {
    act_model *md = sv->md;
    double qq = act_clamp(md->prob[ff->out], k_unit);
    uint32_t count = 0;
    double tog;
    if (collect_cone(sv, ff, &count)) {
        double next0 = conditional_next(sv, ff, (cone_cond){count, 0.0});
        double next1 = conditional_next(sv, ff, (cone_cond){count, 1.0});
        tog = (1.0 - qq) * next0 + qq * (1.0 - next1);
    } else {
        act_fn_ref self = ref_of(md, ff, sv->scratch, false);
        double next = act_eval_prob(&self);
        tog = next * (1.0 - qq) + (1.0 - next) * qq;
        md->cone_capped++;
    }
    return act_clamp(tog, (act_range){0.0, k_two * fmin(qq, 1.0 - qq)});
}

/* --- fixpoint --------------------------------------------------------------------------------- */

typedef struct comp_list {
    uint32_t *list;  /* combinational cells in rank order, then the storage cells */
    uint32_t count;  /* length of list */
    uint32_t n_comb; /* how many of them are combinational */
} comp_list;

/* One Gauss-Seidel sweep of the probabilities over cells; returns the largest change. */
static double sweep_prob(solver *sv, const uint32_t *cells, uint32_t count, bool damp) {
    act_model *md = sv->md;
    double delta = 0.0;
    for (uint32_t kk = 0; kk < count; kk++) {
        const act_cell *cell = &md->cells[cells[kk]];
        act_fn_ref ref = ref_of(md, cell, sv->scratch, false);
        double val = act_eval_prob(&ref);
        val = damp ? (val + md->prob[cell->out]) * k_half : val;
        delta = fmax(delta, fabs(val - md->prob[cell->out]));
        md->prob[cell->out] = val;
    }
    return delta;
}

/* Everything pair-evaluated in sweeps: used for a loop of combinational cells only. */
static double sweep_pair(solver *sv, const uint32_t *cells, uint32_t count, bool damp) {
    act_model *md = sv->md;
    double delta = 0.0;
    for (uint32_t kk = 0; kk < count; kk++) {
        const act_cell *cell = &md->cells[cells[kk]];
        act_fn_ref ref = ref_of(md, cell, sv->scratch, true);
        act_stat res = act_eval_pair(&ref);
        if (damp) {
            res.prob = (res.prob + md->prob[cell->out]) * k_half;
            res.dens = (res.dens + md->dens[cell->out]) * k_half;
        }
        delta = fmax(delta, fmax(fabs(res.prob - md->prob[cell->out]),
                                 fabs(res.dens - md->dens[cell->out])));
        md->prob[cell->out] = res.prob;
        md->dens[cell->out] = res.dens;
    }
    return delta;
}

static void report_iterations(solver *sv, uint32_t count, uint32_t iter, double delta) {
    sv->md->iterations += iter;
    if (delta >= sv->opts->tol) {
        sv->md->converged = false;
        odin3_log(ODIN3_LOG_WARN,
                  "activity: a feedback component of %u cells did not converge in %u sweeps "
                  "(last change %g); the values are those of the last sweep",
                  count, iter, delta);
    }
}

/* A component with a loop of combinational cells: pairs iterated together. */
static void solve_comb_loop(solver *sv, const uint32_t *cells, uint32_t count) {
    uint32_t iter = 0;
    double delta = 1.0;
    while (delta >= sv->opts->tol && iter < sv->opts->max_iter) {
        delta = sweep_pair(sv, cells, count, iter >= ACT_DAMP_FROM);
        iter++;
    }
    report_iterations(sv, count, iter, delta);
}

/* Aitken extrapolation of every cell output from the last three sweeps (prev2, prev1, now): when
 * a value is converging geometrically (same sign steps, shrinking), jump to the limit. */
static void extrapolate(act_model *md, const comp_list *cl, const double *prev1,
                        const double *prev2) {
    for (uint32_t kk = 0; kk < cl->count; kk++) {
        uint32_t out = md->cells[cl->list[kk]].out;
        double d1 = md->prob[out] - prev1[kk];
        double d0 = prev1[kk] - prev2[kk];
        if (d1 * d0 > 0.0 && fabs(d1) < fabs(d0) && d1 / d0 < k_accel_max_ratio) {
            double ratio = d1 / d0;
            md->prob[out] = act_clamp(md->prob[out] + d1 * ratio / (1.0 - ratio), k_unit);
        }
    }
}

/* Probabilities to a fixpoint, accelerated; damped when the change stops shrinking. */
static void iterate_prob(solver *sv, const comp_list *cl) {
    act_model *md = sv->md;
    size_t count = cl->count;
    double *hist = odin3_util_malloc(2U * (count + 1U) * sizeof *hist);
    double *prev1 = hist;
    double *prev2 = hist != NULL ? hist + count + 1U : NULL;
    uint32_t iter = 0;
    double delta = 1.0;
    double before = k_big_change; /* the change of the sweep before the last one */
    while (delta >= sv->opts->tol && iter < sv->opts->max_iter) {
        if (hist != NULL) {
            memcpy(prev2, prev1, count * sizeof *prev2);
            for (size_t kk = 0; kk < count; kk++) {
                prev1[kk] = md->prob[md->cells[cl->list[kk]].out];
            }
        }
        bool damp = iter >= ACT_DAMP_FROM && delta >= before * k_accel_max_ratio;
        before = delta;
        delta = sweep_prob(sv, cl->list, cl->count, damp);
        iter++;
        if (hist != NULL && iter % ACCEL_EVERY == 0 && delta >= sv->opts->tol) {
            extrapolate(md, cl, prev1, prev2);
        }
    }
    odin3_util_free(hist);
    report_iterations(sv, cl->count, iter, delta);
}

/* Fixpoint of the probabilities, then the density of every storage cell by the one-frame rule,
 * then the densities of the combinational cells in rank order. */
static void solve_feedback(solver *sv, const comp_list *cl) {
    act_model *md = sv->md;
    iterate_prob(sv, cl);
    for (uint32_t kk = cl->n_comb; kk < cl->count; kk++) {
        md->dens[md->cells[cl->list[kk]].out] = ff_toggle(sv, &md->cells[cl->list[kk]]);
        md->ff_loop++;
    }
    for (uint32_t kk = 0; kk < cl->n_comb; kk++) {
        const act_cell *cell = &md->cells[cl->list[kk]];
        act_fn_ref ref = ref_of(md, cell, sv->scratch, true);
        md->dens[cell->out] = act_eval_pair(&ref).dens;
    }
}

/* Splits the component into combinational cells in rank order and storage cells; false when a
 * combinational cell has no rank (a loop without storage). */
static bool split_component(const solver *sv, const uint32_t *cells, uint32_t count,
                            comp_list *cl) {
    rank_cell *ordered = odin3_util_malloc((count + 1U) * sizeof *ordered);
    uint32_t n_comb = 0;
    bool ok = ordered != NULL;
    for (uint32_t kk = 0; ok && kk < count; kk++) {
        if (sv->md->cells[cells[kk]].is_ff == 0) {
            ok = sv->rank[cells[kk]] != ACT_NONE;
            ordered[n_comb++] = (rank_cell){sv->rank[cells[kk]], cells[kk]};
        }
    }
    if (ok) {
        qsort(ordered, n_comb, sizeof *ordered, rank_cmp);
        uint32_t pos = 0;
        for (uint32_t kk = 0; kk < n_comb; kk++) {
            cl->list[pos++] = ordered[kk].cell;
        }
        for (uint32_t kk = 0; kk < count; kk++) {
            if (sv->md->cells[cells[kk]].is_ff != 0) {
                sv->md->cells[cells[kk]].loop = 1;
                cl->list[pos++] = cells[kk];
            }
        }
        cl->count = pos;
        cl->n_comb = n_comb;
    }
    odin3_util_free(ordered);
    return ok;
}

static odin3_status solve_cyclic(solver *sv, const uint32_t *cells, uint32_t count) {
    comp_list cl = {odin3_util_malloc((count + 1U) * sizeof(uint32_t)), 0, 0};
    if (cl.list == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (split_component(sv, cells, count, &cl)) {
        solve_feedback(sv, &cl);
    } else {
        solve_comb_loop(sv, cells, count);
    }
    odin3_util_free(cl.list);
    return ODIN3_OK;
}

static bool self_reads(const act_model *md, const act_cell *cell) {
    const uint32_t *ins = odin3_vec_cat(&md->in_pool, cell->in_off);
    for (uint32_t kk = 0; kk < cell->n_in; kk++) {
        if (ins[kk] == cell->out) {
            return true;
        }
    }
    return false;
}

static void initial_values(act_model *md, const act_opts *opts) {
    for (uint32_t id = 1; id < md->n_nets; id++) {
        if (md->kind[id] == ACT_COMB) {
            md->prob[id] = k_half;
            md->dens[id] = k_half;
        } else if (md->kind[id] == ACT_FF) {
            md->prob[id] = opts->ff_start;
            md->dens[id] = k_two * opts->ff_start * (1.0 - opts->ff_start);
        }
    }
}

static odin3_status solve_components(solver *sv, const tarjan *tj) {
    act_model *md = sv->md;
    odin3_status st = ODIN3_OK;
    for (uint32_t comp = tj->n_comps; st == ODIN3_OK && comp > 0; comp--) {
        const uint32_t *cells = tj->order + tj->comp_start[comp - 1U];
        uint32_t count = tj->comp_start[comp] - tj->comp_start[comp - 1U];
        const act_cell *first = &md->cells[cells[0]];
        if (count == 1U && !self_reads(md, first)) {
            act_fn_ref ref = ref_of(md, first, sv->scratch, true);
            act_stat res = act_eval_pair(&ref);
            md->prob[first->out] = res.prob;
            md->dens[first->out] = res.dens;
        } else {
            st = solve_cyclic(sv, cells, count);
        }
    }
    return st;
}

static odin3_status solver_alloc(solver *sv, uint32_t widest) {
    size_t cnt = sv->md->n_cells + 1U;
    sv->scratch = odin3_util_malloc(((size_t)2U << widest) * sizeof(double));
    sv->rank = odin3_util_malloc(cnt * sizeof *sv->rank);
    sv->mark = odin3_util_calloc(cnt * sizeof *sv->mark);
    sv->queue = odin3_util_malloc((cnt + sv->md->n_nets + 1U) * sizeof *sv->queue);
    sv->cone = odin3_util_malloc((CONE_CAP + 1U) * sizeof *sv->cone);
    sv->saved = odin3_util_malloc((CONE_CAP + 1U) * sizeof *sv->saved);
    bool ok = sv->scratch != NULL && sv->rank != NULL && sv->mark != NULL && sv->queue != NULL &&
              sv->cone != NULL && sv->saved != NULL;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

static void solver_free(solver *sv) {
    odin3_util_free(sv->scratch);
    odin3_util_free(sv->rank);
    odin3_util_free(sv->mark);
    odin3_util_free(sv->queue);
    odin3_util_free(sv->cone);
    odin3_util_free(sv->saved);
}

odin3_status act_solve(act_model *md, const act_opts *opts) {
    graph gr = {.md = md};
    tarjan tj;
    memset(&tj, 0, sizeof tj);
    uint32_t widest = 0;
    for (size_t ci = 0; ci < md->n_cells; ci++) {
        widest = md->cells[ci].n_in > widest ? md->cells[ci].n_in : widest;
    }
    solver sv = {.md = md, .opts = opts, .gr = &gr};
    md->converged = true;
    md->iterations = 0;
    odin3_status st = build_readers(&gr);
    st = st == ODIN3_OK ? solver_alloc(&sv, widest) : st;
    st = st == ODIN3_OK ? rank_cells(&sv) : st;
    st = st == ODIN3_OK ? tarjan_run(&gr, &tj) : st;
    if (st == ODIN3_OK) {
        initial_values(md, opts);
        st = solve_components(&sv, &tj);
    }
    tarjan_free(&tj);
    solver_free(&sv);
    odin3_util_free(gr.rd_start);
    odin3_util_free(gr.rd_items);
    return st;
}
